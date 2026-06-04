#include "sensor.h"
#include "app_config.h"
#include "calibration.h"
#include "sample.h"
#include "queues.h"
#include "power.h"
#include "rtc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"  // TODO: migrate to driver/i2c_master.h before ESP-IDF v7.0
#include "driver/gpio.h"
#include "esp_log.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "sensor";

// Voltage measured at AIN0 in clean air — used as the zero reference for ppm.
// Loaded from NVS on boot; updated at the end of every calibration sequence.
static float g_v_zero = 3.3f * 0.20f;  // safe default until first calibration

static void nvs_load_v_zero(void) {
    nvs_handle_t h;
    if (nvs_open("sensor_cal", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t raw = 0;
    if (nvs_get_u32(h, "v_zero", &raw) == ESP_OK) {
        memcpy(&g_v_zero, &raw, sizeof(float));
        ESP_LOGI(TAG, "NVS v_zero loaded: %.4fV", g_v_zero);
    }
    nvs_close(h);
}

static void nvs_save_v_zero(float v) {
    nvs_handle_t h;
    if (nvs_open("sensor_cal", NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t raw;
    memcpy(&raw, &v, sizeof(float));
    nvs_set_u32(h, "v_zero", raw);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS v_zero saved: %.4fV", v);
}

// ── I2C bus ──────────────────────────────────────────────────────────────────
#define I2C_PORT     I2C_NUM_0
#define I2C_SDA      GPIO_NUM_22
#define I2C_SCL      GPIO_NUM_23
#define I2C_FREQ_HZ  100000
#define ACK_EN       true

// ── LMP91002 ─────────────────────────────────────────────────────────────────
#define LMP_MENB_GPIO  GPIO_NUM_16   // active-low enable; shared with UART0 TX
#define LMP_ADDR       0x48
#define LMP_REG_LOCK   0x01
#define LMP_REG_TIACN  0x10
#define LMP_REG_REFCN  0x11
#define LMP_REG_MODECN 0x12

// TIACN: TIA gain 35 kΩ = bits [4:2] = 0b101
#define LMP_TIACN_RLOAD_10 0x00   // RLOAD = 10 Ω (bits [1:0])
#define LMP_GAIN_35K       (5 << 2)
// REFCN: INT_Z=20% → INTZCAL bits[6:5]=00, BIAS=0, internal ref → value 0x00
#define LMP_REFCN_INT_20   0x00
// MODECN: 3-lead amperometric
#define LMP_MODE_3LEAD     0x03

static esp_err_t lmp_read_reg(uint8_t reg, uint8_t *val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LMP_ADDR << 1) | I2C_MASTER_WRITE, ACK_EN);
    i2c_master_write_byte(cmd, reg, ACK_EN);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return ret;

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LMP_ADDR << 1) | I2C_MASTER_READ, ACK_EN);
    i2c_master_read_byte(cmd, val, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t lmp_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LMP_ADDR << 1) | I2C_MASTER_WRITE, ACK_EN);
    i2c_master_write(cmd, buf, 2, ACK_EN);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t lmp_init(void) {
    // Unlock registers
    esp_err_t r = lmp_write_reg(LMP_REG_LOCK, 0x00);
    if (r != ESP_OK) return r;

    r = lmp_write_reg(LMP_REG_TIACN, LMP_GAIN_35K | LMP_TIACN_RLOAD_10);
    if (r != ESP_OK) return r;

    r = lmp_write_reg(LMP_REG_REFCN, LMP_REFCN_INT_20);
    if (r != ESP_OK) return r;

    r = lmp_write_reg(LMP_REG_MODECN, LMP_MODE_3LEAD);
    return r;
}

// ── ADS1115 ──────────────────────────────────────────────────────────────────
#define ADS_ADDR     0x49
#define ADS_REG_CONV   0x00
#define ADS_REG_CFG    0x01

// Config register high byte: OS=1(start), MUX=AIN0..3 vs GND, PGA, MODE=single
#define ADS_OS_START   0x80
#define ADS_MUX_AIN0   0x40   // 100 → AIN0/GND
#define ADS_MUX_AIN1   0x50   // 101 → AIN1/GND
#define ADS_MUX_AIN2   0x60   // 110 → AIN2/GND
#define ADS_PGA_2V048  0x04   // bits[3:1] = 010 → ±2.048 V, 62.5 µV/LSB
#define ADS_PGA_4V096  0x02   // bits[3:1] = 001 → ±4.096 V, 125 µV/LSB
#define ADS_MODE_SINGLE 0x01  // bit0 of high byte
// Config register low byte: 128 SPS (bits[7:5]=100), comparator disabled
#define ADS_DR_128SPS  0x80
#define ADS_COMP_OFF   0x03

static esp_err_t ads_write_reg(uint8_t reg, uint16_t val) {
    uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADS_ADDR << 1) | I2C_MASTER_WRITE, ACK_EN);
    i2c_master_write(cmd, buf, 3, ACK_EN);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t ads_read_reg(uint8_t reg, uint16_t *out) {
    // Set pointer
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADS_ADDR << 1) | I2C_MASTER_WRITE, ACK_EN);
    i2c_master_write_byte(cmd, reg, ACK_EN);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return ret;

    // Read 2 bytes
    uint8_t buf[2];
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADS_ADDR << 1) | I2C_MASTER_READ, ACK_EN);
    i2c_master_read(cmd, buf, 2, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret == ESP_OK) {
        *out = ((uint16_t)buf[0] << 8) | buf[1];
    }
    return ret;
}

// Trigger single-shot conversion on given mux and PGA, return voltage in Volts.
// Caller must hold i2c_mutex.
static float ads_read_voltage(uint8_t mux_hi, uint8_t pga_hi) {
    uint16_t cfg = ((uint16_t)(ADS_OS_START | mux_hi | pga_hi | ADS_MODE_SINGLE) << 8)
                 | ((uint16_t)(ADS_DR_128SPS | ADS_COMP_OFF));
    if (ads_write_reg(ADS_REG_CFG, cfg) != ESP_OK) return 0.0f;

    // Wait for conversion (~8 ms @ 128 SPS; poll OS bit)
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint16_t status;
        if (ads_read_reg(ADS_REG_CFG, &status) == ESP_OK) {
            if (status & 0x8000) break;  // OS=1 → conversion done
        }
    }

    uint16_t raw;
    if (ads_read_reg(ADS_REG_CONV, &raw) != ESP_OK) return 0.0f;

    int16_t signed_raw = (int16_t)raw;
    float lsb = (pga_hi == ADS_PGA_4V096) ? 125e-6f : 62.5e-6f;
    return (float)signed_raw * lsb;
}

// ── Public API ────────────────────────────────────────────────────────────────

void sensor_init(void) {
    // Drive MENB low to enable the LMP91002.
    // GPIO16 doubles as UART0 TX (idles high), which keeps MENB high and the
    // chip disabled. Reconfigure as plain output so the LMP91002 powers up.
    // USB-JTAG console is unaffected.
    gpio_config_t menb_cfg = {
        .pin_bit_mask = (1ULL << LMP_MENB_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&menb_cfg);
    gpio_set_level(LMP_MENB_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(5));   // LMP91002 wake-up

    // Configure I2C master
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA,
        .scl_io_num       = I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master = {
            .clk_speed    = I2C_FREQ_HZ,
        },
        .clk_flags        = 0,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    // LMP91002 init (no mutex needed here — tasks not running yet)
    if (lmp_init() != ESP_OK) {
        ESP_LOGE(TAG, "LMP91002 init failed");
    } else {
        // Read back MODECN to confirm writes were accepted (LOCK cleared OK)
        uint8_t modecn = 0xFF;
        lmp_read_reg(LMP_REG_MODECN, &modecn);
        if (modecn == LMP_MODE_3LEAD) {
            ESP_LOGI(TAG, "LMP91002 OK — 3-lead amperometric mode confirmed");
        } else {
            ESP_LOGE(TAG, "LMP91002 MODECN readback=0x%02X (expected 0x03) — registers not written, chip may be locked", modecn);
        }
    }

    // Verify ADS1115 is present — read its default config register
    uint16_t ads_cfg = 0;
    if (ads_read_reg(ADS_REG_CFG, &ads_cfg) == ESP_OK) {
        ESP_LOGI(TAG, "ADS1115 OK, cfg=0x%04X", ads_cfg);
    } else {
        ESP_LOGE(TAG, "ADS1115 not found on I2C");
    }

    nvs_load_v_zero();
}

void sensor_task(void *pvParameter) {
    (void)pvParameter;
    const TickType_t interval = pdMS_TO_TICKS(APP_SENSOR_PERIOD_MS);

    typedef enum { PHASE_WARMUP, PHASE_CALIBRATING, PHASE_RUNNING } phase_t;
    phase_t  phase       = PHASE_WARMUP;
    uint32_t phase_count = 0;
    float    cal_acc     = 0.0f;
    uint32_t sample_n    = 0;

    for (;;) {
        sample_t s = {};
        s.timestamp = (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        rtc_get_datetime(&s.datetime);

        float v_h2s = 0.0f, v_batt = 0.0f, v_therm = 0.0f;
        if (xSemaphoreTake(i2c_mutex, portMAX_DELAY) == pdTRUE) {
            v_h2s   = ads_read_voltage(ADS_MUX_AIN0, ADS_PGA_2V048);
            v_batt  = ads_read_voltage(ADS_MUX_AIN1, ADS_PGA_2V048);
            v_therm = ads_read_voltage(ADS_MUX_AIN2, ADS_PGA_4V096);
            xSemaphoreGive(i2c_mutex);

            s.raw         = (int32_t)(v_h2s / 62.5e-6f);
            s.temperature = voltage_to_celsius(v_therm);
            s.battery_mv  = voltage_to_battery_mv(v_batt);
        }

        power_update_battery_mv(s.battery_mv);
        if (s.battery_mv < APP_LOW_BATTERY_MV) s.status |= STATUS_LOW_BATTERY;
        if (power_is_charging())               s.status |= STATUS_CHARGING;

        switch (phase) {
        case PHASE_WARMUP:
            s.status |= STATUS_SENSOR_WARMUP;
            phase_count++;
            ESP_LOGI(TAG, "[warmup %lu/%d] AIN0=%.4fV  %.1fC  %lumV",
                     (unsigned long)phase_count, SENSOR_WARMUP_SAMPLES,
                     v_h2s, s.temperature, (unsigned long)s.battery_mv);
            if (phase_count >= SENSOR_WARMUP_SAMPLES) {
                phase = PHASE_CALIBRATING;
                phase_count = 0;
                cal_acc = 0.0f;
                ESP_LOGI(TAG, "Warmup complete — starting zero calibration (%d samples)",
                         SENSOR_CAL_SAMPLES);
            }
            if (q_display) xQueueSend(q_display, &s, 0);
            break;

        case PHASE_CALIBRATING:
            s.status |= STATUS_CALIBRATING;
            cal_acc += v_h2s;
            phase_count++;
            s.seq = phase_count;
            ESP_LOGI(TAG, "[cal %lu/%d] AIN0=%.4fV  running mean=%.4fV",
                     (unsigned long)phase_count, SENSOR_CAL_SAMPLES,
                     v_h2s, cal_acc / (float)phase_count);
            if (phase_count >= SENSOR_CAL_SAMPLES) {
                g_v_zero = cal_acc / (float)SENSOR_CAL_SAMPLES;
                nvs_save_v_zero(g_v_zero);
                ESP_LOGI(TAG, "Zero calibration complete — v_zero=%.4fV", g_v_zero);
                phase = PHASE_RUNNING;
                phase_count = 0;
            }
            if (q_display) xQueueSend(q_display, &s, 0);
            break;

        case PHASE_RUNNING:
            s.seq = ++sample_n;
            s.ppb = voltage_to_ppm(v_h2s, g_v_zero);
            ESP_LOGI(TAG, "#%lu  %04d-%02d-%02d %02d:%02d:%02d | AIN0=%.4fV AIN1=%.4fV AIN2=%.4fV | %.2f ppm  %.1f C  %lu mV",
                     (unsigned long)s.seq,
                     s.datetime.tm_year + 1900, s.datetime.tm_mon + 1, s.datetime.tm_mday,
                     s.datetime.tm_hour, s.datetime.tm_min, s.datetime.tm_sec,
                     v_h2s, v_batt, v_therm,
                     s.ppb, s.temperature, (unsigned long)s.battery_mv);
            if (q_display) xQueueSend(q_display, &s, 0);
            if (q_storage) xQueueSend(q_storage, &s, 0);
            break;
        }

        vTaskDelay(interval);
    }
}
