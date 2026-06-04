/*
 * test_hardware.cpp — on-device integration tests for the H2S sensor board.
 *
 * Run with: platformio test -e seeed_xiao_esp32c6
 *
 * The board must be powered, all peripherals connected, and an SD card inserted.
 * Tests are grouped by subsystem and run sequentially from app_main.
 * Output is printed over USB-serial and captured by the PlatformIO test runner.
 *
 * Initialisation order in app_main:
 *   1. NVS flash
 *   2. I2C master (GPIO6/GPIO7) — required by groups 1–4
 *   3. LMP91002 register init  — required by group 2
 *   4. SPI bus + SD mount      — required by group 6
 *   5. Pump GPIO               — required by group 7
 */

#include <unity.h>
#include "app_config.h"
#include "calibration.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ── Pin / address constants ───────────────────────────────────────────────────

#define I2C_PORT      I2C_NUM_0
#define I2C_SDA       GPIO_NUM_6
#define I2C_SCL       GPIO_NUM_7

#define LMP_ADDR      0x48
#define ADS_ADDR      0x49
#define OLED_ADDR     0x3C

#define SD_MISO       GPIO_NUM_4
#define SD_CS         GPIO_NUM_9
#define SD_SCK        GPIO_NUM_10
#define SD_MOSI       GPIO_NUM_11
#define PUMP_GPIO     GPIO_NUM_17

// ── LMP91002 register addresses and expected values after init ────────────────

#define LMP_REG_LOCK    0x01
#define LMP_REG_TIACN   0x10
#define LMP_REG_REFCN   0x11
#define LMP_REG_MODECN  0x12

// TIACN: gain=35kΩ (bits[4:2]=100) + RLOAD=10Ω (bits[1:0]=00) → 0x10
#define LMP_TIACN_EXPECTED  0x10
// REFCN: INT_Z=20% (bit5=1), BIAS=0% (bits[3:0]=0) → 0x20
#define LMP_REFCN_EXPECTED  0x20
// MODECN: 3-lead amperometric → 0x03
#define LMP_MODECN_EXPECTED 0x03

// ── ADS1115 register / config constants ───────────────────────────────────────

#define ADS_REG_CONV    0x00
#define ADS_REG_CFG     0x01
#define ADS_OS_START    0x80
#define ADS_MUX_AIN0    0x40
#define ADS_MUX_AIN1    0x50
#define ADS_MUX_AIN2    0x60
#define ADS_MUX_AIN3    0x70
#define ADS_PGA_2V048   0x04   // ±2.048 V → 62.5 µV/LSB
#define ADS_MODE_SINGLE 0x01
#define ADS_DR_128SPS   0x80
#define ADS_COMP_OFF    0x03

// ── Module-level SD state (shared across SD tests) ────────────────────────────

static sdmmc_card_t *s_card = NULL;
static sdmmc_host_t  s_host;

// ─────────────────────────────────────────────────────────────────────────────
// I2C / ADS helpers
// ─────────────────────────────────────────────────────────────────────────────

/*
 * Send a zero-byte write to addr and return true if the device ACKs.
 * An ACK means a device is alive at that address; a NACK means nothing is there.
 */
static bool i2c_device_present(uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return r == ESP_OK;
}

/* Write one byte to an LMP91002 register. */
static esp_err_t lmp_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LMP_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, 2, true);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return r;
}

/* Read one byte from an LMP91002 register. */
static esp_err_t lmp_read_reg(uint8_t reg, uint8_t *out) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LMP_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    if (r != ESP_OK) return r;

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LMP_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, out, 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    r = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return r;
}

/*
 * Trigger a single-shot ADS1115 conversion on the given mux channel and
 * return the result in Volts. Waits 15 ms (safe margin for 128 SPS).
 */
static float ads_single_read(uint8_t mux_hi) {
    uint16_t cfg = ((uint16_t)(ADS_OS_START | mux_hi | ADS_PGA_2V048 | ADS_MODE_SINGLE) << 8)
                 | (uint16_t)(ADS_DR_128SPS | ADS_COMP_OFF);
    uint8_t buf[3] = {ADS_REG_CFG, (uint8_t)(cfg >> 8), (uint8_t)(cfg & 0xFF)};

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADS_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, 3, true);
    i2c_master_stop(cmd);
    if (i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100)) != ESP_OK) {
        i2c_cmd_link_delete(cmd);
        return 0.0f;
    }
    i2c_cmd_link_delete(cmd);

    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t ptr = ADS_REG_CONV;
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADS_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, ptr, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);

    uint8_t rbuf[2] = {0, 0};
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADS_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, rbuf, 2, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);

    int16_t raw = (int16_t)(((uint16_t)rbuf[0] << 8) | rbuf[1]);
    return (float)raw * 62.5e-6f;
}

/* Mount the SD card and store the handle in s_card. Returns true on success. */
static bool sd_mount(void) {
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS;
    slot_cfg.host_id = (spi_host_device_t)s_host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed   = false,
        .max_files                = 4,
        .allocation_unit_size     = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat              = false,
    };

    return esp_vfs_fat_sdspi_mount("/sdcard", &s_host, &slot_cfg,
                                   &mount_cfg, &s_card) == ESP_OK;
}

/* Unmount the SD card and clear the handle. */
static void sd_unmount(void) {
    if (s_card) {
        esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        s_card = NULL;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Unity boilerplate
// ─────────────────────────────────────────────────────────────────────────────

void setUp(void)    {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════════════════════
// GROUP 1 — I2C device presence
// Run these first. If any fail, everything else in that subsystem will also
// fail, so there is no point continuing until wiring is fixed.
// ═════════════════════════════════════════════════════════════════════════════

/*
 * Checks that the LMP91002 AFE responds on I2C address 0x48.
 * FAIL → check SDA/SCL wiring, LMP91002 power rail, and solder joints.
 */
void test_lmp91002_present(void) {
    TEST_ASSERT_TRUE_MESSAGE(i2c_device_present(LMP_ADDR),
        "LMP91002 not found at 0x48 — check SDA/SCL wiring and power");
}

/*
 * Checks that the ADS1115 ADC responds on I2C address 0x49.
 * Address 0x49 requires ADDR pin tied to VDD on the PCB.
 * FAIL → verify ADDR pin connection, check for conflict with LMP at 0x48.
 */
void test_ads1115_present(void) {
    TEST_ASSERT_TRUE_MESSAGE(i2c_device_present(ADS_ADDR),
        "ADS1115 not found at 0x49 — check ADDR pin tied to VDD");
}

/*
 * Checks that the SSD1327 OLED responds on I2C address 0x3C.
 * FAIL → check display power, I2C pull-up resistors, and address solder
 * jumper on the Waveshare module (some variants default to 0x3D).
 */
void test_ssd1327_present(void) {
    TEST_ASSERT_TRUE_MESSAGE(i2c_device_present(OLED_ADDR),
        "SSD1327 not found at 0x3C — check display power and address jumper");
}

// ═════════════════════════════════════════════════════════════════════════════
// GROUP 2 — LMP91002 register write-back
// Verifies that the init sequence actually wrote the correct values.
// A silent I2C write failure (e.g. LOCK register not unlocked) would cause
// the sensor to read nothing useful but give no error at runtime.
// ═════════════════════════════════════════════════════════════════════════════

/*
 * Reads back TIACN after init and asserts it matches the expected value.
 * Expected 0x10: TIA gain = 35 kΩ (bits[4:2]=100), RLOAD = 10 Ω (bits[1:0]=00).
 * FAIL → LOCK register was not unlocked before writing, or I2C write silently failed.
 */
void test_lmp91002_tiacn_readback(void) {
    uint8_t val = 0xFF;
    esp_err_t r = lmp_read_reg(LMP_REG_TIACN, &val);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, r, "I2C read of TIACN failed");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(LMP_TIACN_EXPECTED, val,
        "TIACN mismatch — LMP91002 may not have been unlocked before writing");
}

/*
 * Reads back REFCN and asserts expected value 0x20.
 * Expected 0x20: internal zero = 20% of Vref (bit5=1), BIAS = 0% (bits[3:0]=0).
 * FAIL → wrong internal reference; H2S zero point will be off.
 */
void test_lmp91002_refcn_readback(void) {
    uint8_t val = 0xFF;
    esp_err_t r = lmp_read_reg(LMP_REG_REFCN, &val);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, r, "I2C read of REFCN failed");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(LMP_REFCN_EXPECTED, val,
        "REFCN mismatch — internal Vref percentage is wrong");
}

/*
 * Reads back MODECN and asserts expected value 0x03.
 * Expected 0x03: 3-lead amperometric mode.
 * FAIL → sensor is in a different mode (e.g. deep sleep or 2-lead); no valid output.
 */
void test_lmp91002_modecn_readback(void) {
    uint8_t val = 0xFF;
    esp_err_t r = lmp_read_reg(LMP_REG_MODECN, &val);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, r, "I2C read of MODECN failed");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(LMP_MODECN_EXPECTED, val,
        "MODECN mismatch — sensor is not in 3-lead amperometric mode");
}

// ═════════════════════════════════════════════════════════════════════════════
// GROUP 3 — ADS1115 voltage range sanity
// Reads all three active channels and checks the voltages fall in physically
// plausible ranges for the connected hardware with no gas present.
// A value outside the range points to a wiring problem on that specific channel
// rather than a firmware bug.
// ═════════════════════════════════════════════════════════════════════════════

/*
 * AIN0: LMP91002 Vout (H2S signal).
 * With no H2S present, current through the cell is ~0 so Vout should sit
 * at the internal Vref = 20% × 3.3 V = 0.66 V.
 * Allowed range: 0.40 V – 0.90 V (generous margin for cell offset and noise).
 * FAIL → LMP91002 not biased correctly, or AIN0 wiring open/shorted.
 */
void test_ain0_h2s_near_vref(void) {
    float v = ads_single_read(ADS_MUX_AIN0);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.25f, 0.65f, v,
        "AIN0 (H2S Vout) outside 0.40-0.90 V — check LMP91002 bias or AIN0 wiring");
}

/*
 * AIN1: battery resistor divider.
 * A charged LiPo is 3.6–4.2 V; through the 100k/51k divider that becomes
 * 1.22–1.42 V at the ADC pin.  Extended range 1.0–1.5 V allows for a
 * slightly discharged or slightly over-charged cell.
 * FAIL → battery not connected, divider resistors wrong, or AIN1 wiring open.
 */
void test_ain1_battery_in_range(void) {
    float v = ads_single_read(ADS_MUX_AIN1);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.25f, 1.25f, v,
        "AIN1 (battery divider) outside 1.0-1.5 V — check battery and divider wiring");
}

/*
 * AIN2: NTC thermistor.
 * At room temperature (5–50 °C) the 10 kΩ NTC with a 10 kΩ pullup to 3.3 V
 * produces 0.87–2.38 V.  Extended range 0.70–2.60 V leaves extra margin.
 * FAIL → thermistor open (reads ~3.3 V) or shorted (reads ~0 V), or AIN2 wiring.
 */
void test_ain2_thermistor_in_range(void) {
    float v = ads_single_read(ADS_MUX_AIN2);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.95f, 1.65f, v,
        "AIN2 (thermistor) outside 0.70-2.60 V — check thermistor and pullup wiring");
}

// ═════════════════════════════════════════════════════════════════════════════
// GROUP 4 — Temperature plausibility
// Runs the AIN2 voltage through the full calibration formula and checks that
// the resulting temperature is a sensible room-temperature value.
// This catches a wrong B-coefficient or pullup value in calibration.h without
// needing a calibrated reference thermometer.
// ═════════════════════════════════════════════════════════════════════════════

/*
 * Converts the live AIN2 reading to °C using voltage_to_celsius() and asserts
 * the result is between 5 °C and 50 °C.
 * FAIL → formula constants wrong (B-coefficient, R0, pullup value), or AIN2
 * hardware fault (caught more specifically by test_ain2_thermistor_in_range).
 */
void test_temperature_in_room_range(void) {
    float v   = ads_single_read(ADS_MUX_AIN2);
    float deg = voltage_to_celsius(v);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(22.5f, 27.5f, deg,
        "Temperature outside 5-50 °C — check calibration.h constants or thermistor");
}

// ═════════════════════════════════════════════════════════════════════════════
// GROUP 5 — SD card
// Three tests covering mount, large writes, and data persistence across
// an unmount/remount cycle.  The last test is the most important for
// validating the fclose-per-sample flush strategy used in storage.cpp.
// ═════════════════════════════════════════════════════════════════════════════

/*
 * Verifies the SD card mounts and basic file I/O works.
 * This is a prerequisite for the two tests below.
 * FAIL → card not inserted, SPI wiring wrong, or card formatted with an
 * incompatible filesystem (re-format as FAT32).
 */
void test_sd_card_mounts_and_basic_io(void) {
    TEST_ASSERT_TRUE_MESSAGE(s_card != NULL,
        "SD card was not mounted during app_main init — check SPI wiring and card");

    FILE *f = fopen("/sdcard/test_basic.tmp", "w");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Could not create file on SD card");
    fprintf(f, "basic_ok");
    fclose(f);

    f = fopen("/sdcard/test_basic.tmp", "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Could not re-open file after write");
    char buf[16] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    TEST_ASSERT_EQUAL_STRING("basic_ok", buf);

    remove("/sdcard/test_basic.tmp");
}

/*
 * Writes a 4 KB buffer with a known byte pattern across multiple SD sectors,
 * reads it back, and compares byte-for-byte.
 * Catches SPI clock issues and marginal cards that ACK but corrupt data at
 * larger transfer sizes — something a 2-byte test would never reveal.
 * FAIL → SPI signal integrity problem, marginal card, or wrong DMA settings.
 */
void test_sd_multisector_write(void) {
    const size_t SIZE = 4096;
    uint8_t *wbuf = (uint8_t *)malloc(SIZE);
    uint8_t *rbuf = (uint8_t *)malloc(SIZE);
    TEST_ASSERT_NOT_NULL_MESSAGE(wbuf, "malloc failed for write buffer — not enough heap");
    TEST_ASSERT_NOT_NULL_MESSAGE(rbuf, "malloc failed for read buffer — not enough heap");

    // Fill with a rolling pattern that makes any byte-swap or offset easy to spot.
    for (size_t i = 0; i < SIZE; i++) wbuf[i] = (uint8_t)(i & 0xFF);

    FILE *f = fopen("/sdcard/test_multi.tmp", "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Could not create multi-sector file");
    size_t written = fwrite(wbuf, 1, SIZE, f);
    fclose(f);
    TEST_ASSERT_EQUAL_MESSAGE(SIZE, written, "Short write — card full or SPI error");

    f = fopen("/sdcard/test_multi.tmp", "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Could not re-open multi-sector file");
    size_t nread = fread(rbuf, 1, SIZE, f);
    fclose(f);
    TEST_ASSERT_EQUAL_MESSAGE(SIZE, nread, "Short read on multi-sector file");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(wbuf, rbuf, SIZE, "Data mismatch after multi-sector write — possible SPI corruption");

    remove("/sdcard/test_multi.tmp");
    free(wbuf);
    free(rbuf);
}

/*
 * Writes a sentinel file, unmounts the card, remounts it, then reads the file
 * back and verifies the content is intact.
 * This directly validates the fclose-per-sample strategy in storage.cpp:
 * if fclose does not actually flush to the card, data written before an
 * unmount will be lost and this test will fail.
 * FAIL → FAT flush not working, or card goes into a bad state after unmount.
 */
void test_sd_survives_remount(void) {
    FILE *f = fopen("/sdcard/test_remount.tmp", "w");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Could not create sentinel file");
    fprintf(f, "remount_ok");
    fclose(f);  // this is the flush we rely on in production

    sd_unmount();
    TEST_ASSERT_TRUE_MESSAGE(sd_mount(),
        "SD card failed to remount — SPI bus or card issue");

    f = fopen("/sdcard/test_remount.tmp", "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Sentinel file missing after remount — data was lost on unmount");
    char buf[16] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("remount_ok", buf,
        "Sentinel data corrupted after remount — fclose flush may not be working");

    remove("/sdcard/test_remount.tmp");
}

// ═════════════════════════════════════════════════════════════════════════════
// GROUP 6 — Pump GPIO
// Verifies GPIO17 can be driven high and low, and that gpio_get_level()
// confirms the state.  Does not verify the MOSFET fired (needs oscilloscope)
// but confirms the GPIO driver and pin are functional.
// ═════════════════════════════════════════════════════════════════════════════

/*
 * Sets GPIO17 HIGH, reads it back, then sets it LOW and reads it back.
 * FAIL → GPIO17 shorted to GND, wrong pin number in firmware, or GPIO driver
 * not initialised (pump_init() not called before UNITY_BEGIN).
 */
void test_pump_gpio_toggles(void) {
    gpio_set_level(PUMP_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    TEST_ASSERT_EQUAL_MESSAGE(1, gpio_get_level(PUMP_GPIO),
        "GPIO17 stuck LOW after set HIGH — check for short to GND");

    gpio_set_level(PUMP_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    TEST_ASSERT_EQUAL_MESSAGE(0, gpio_get_level(PUMP_GPIO),
        "GPIO17 stuck HIGH after set LOW — check for short to VCC");
}

// ═════════════════════════════════════════════════════════════════════════════
// GROUP 7 — System health
// Checks that the MCU has enough free heap for normal operation and that
// all four task stacks can be allocated simultaneously.
// Run this after all hardware inits so the measurement reflects real usage.
// ═════════════════════════════════════════════════════════════════════════════

/*
 * Asserts at least 40 KB of free heap remains after all hardware initialisations.
 * The four FreeRTOS task stacks together need ~21 KB; leaving 40 KB free gives
 * comfortable room for I2C DMA buffers, FAT cache, and runtime allocations.
 * FAIL → reduce static allocations, check for memory leaks in init code, or
 * increase psram usage if available.
 */
void test_heap_headroom_adequate(void) {
    size_t free_heap = esp_get_free_heap_size();
    char msg[64];
    snprintf(msg, sizeof(msg), "Only %u bytes free — need at least 40 KB", (unsigned)free_heap);
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(40 * 1024, free_heap, msg);
}

/* Dummy task used only for stack allocation testing. */
static void dummy_task(void *pv) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(100));
}

/*
 * Creates four tasks using the same stack sizes as the real firmware tasks and
 * lets them run briefly, then checks the high-water mark on each.
 * uxTaskGetStackHighWaterMark() returns the minimum remaining stack in 32-bit
 * words; asserting > 128 words (512 bytes) gives a safety margin against
 * deeper call chains in the real tasks.
 * FAIL → a stack size in app_config.h is too small, or not enough heap for
 * all four stacks simultaneously.
 */
void test_task_stacks_allocatable(void) {
    TaskHandle_t handles[4] = {NULL};

    // Mirror the real task stack sizes from app_config.h
    const uint32_t stacks[4] = {
        APP_SENSOR_TASK_STACK,
        APP_DISPLAY_TASK_STACK,
        APP_STORAGE_TASK_STACK,
        APP_POWER_TASK_STACK,
    };
    const char *names[4] = {"t_sens", "t_disp", "t_stor", "t_pwr"};

    for (int i = 0; i < 4; i++) {
        BaseType_t r = xTaskCreate(dummy_task, names[i], stacks[i], NULL, 1, &handles[i]);
        char msg[64];
        snprintf(msg, sizeof(msg), "Failed to create task %s — not enough heap", names[i]);
        TEST_ASSERT_EQUAL_MESSAGE(pdPASS, r, msg);
    }

    vTaskDelay(pdMS_TO_TICKS(300));  // let scheduler run them at least once

    for (int i = 0; i < 4; i++) {
        if (!handles[i]) continue;
        UBaseType_t wm = uxTaskGetStackHighWaterMark(handles[i]);
        char msg[64];
        snprintf(msg, sizeof(msg), "Task %s has < 512 bytes stack headroom (%u words free)",
                 names[i], (unsigned)wm);
        TEST_ASSERT_GREATER_THAN_MESSAGE(128, wm, msg);
        vTaskDelete(handles[i]);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// GROUP 8 — I2C bus recovery
// Verifies that a failed transaction (address with no device) does not lock
// up the bus.  The ESP-IDF legacy I2C driver can stall if a STOP condition
// is not generated after a NACK; this test confirms recovery works so that a
// brief hardware glitch does not take down all three I2C peripherals.
// ═════════════════════════════════════════════════════════════════════════════

/*
 * Sends a write to address 0x00 (nothing should ACK), then immediately checks
 * that the ADS1115 at 0x49 still responds normally.
 * FAIL → I2C driver left the bus in a hung state after the failed transaction;
 * consider calling i2c_reset_tx_fifo / i2c_reset_rx_fifo or re-installing the
 * driver in your error handling paths.
 */
void test_i2c_recovers_after_failed_transaction(void) {
    // Deliberately address a non-existent device; expect a NACK / timeout.
    i2c_device_present(0x00);  // return value intentionally ignored

    // The ADS1115 must still ACK immediately after.
    TEST_ASSERT_TRUE_MESSAGE(i2c_device_present(ADS_ADDR),
        "ADS1115 stopped responding after a failed I2C transaction — bus is hung");
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────

extern "C" void app_main(void) {
    // NVS required by some ESP-IDF components.
    nvs_flash_init();

    // I2C master — shared by LMP91002, ADS1115, SSD1327.
    i2c_config_t i2c_cfg = {
        .mode          = I2C_MODE_MASTER,
        .sda_io_num    = I2C_SDA,
        .scl_io_num    = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master        = {.clk_speed = 100000},
        .clk_flags     = 0,
    };
    i2c_param_config(I2C_PORT, &i2c_cfg);
    i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);

    // Initialise LMP91002 so the register write-back tests have something to read.
    lmp_write_reg(LMP_REG_LOCK,   0x00);                                  // unlock
    lmp_write_reg(LMP_REG_TIACN,  LMP_TIACN_EXPECTED);                   // TIA gain
    lmp_write_reg(LMP_REG_REFCN,  LMP_REFCN_EXPECTED);                   // internal Vref
    lmp_write_reg(LMP_REG_MODECN, LMP_MODECN_EXPECTED);                  // 3-lead mode

    // SPI bus + SD card mount — shared by all SD tests.
    s_host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    spi_bus_config_t spi_cfg = {};
    spi_cfg.data0_io_num    = SD_MOSI;
    spi_cfg.data1_io_num    = SD_MISO;
    spi_cfg.sclk_io_num     = SD_SCK;
    spi_cfg.quadwp_io_num   = -1;
    spi_cfg.quadhd_io_num   = -1;
    spi_cfg.max_transfer_sz = 4096;
    spi_bus_initialize((spi_host_device_t)s_host.slot, &spi_cfg, SDSPI_DEFAULT_DMA);
    sd_mount();  // s_card == NULL if this fails; test_sd_card_mounts_and_basic_io will catch it

    // Pump GPIO — needed by test_pump_gpio_toggles.
    gpio_config_t pump_cfg = {
        .pin_bit_mask = (1ULL << PUMP_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pump_cfg);
    gpio_set_level(PUMP_GPIO, 0);

    // ── Run all tests ─────────────────────────────────────────────────────────
    UNITY_BEGIN();

    // Group 1: I2C device presence
    RUN_TEST(test_lmp91002_present);
    RUN_TEST(test_ads1115_present);
    RUN_TEST(test_ssd1327_present);

    // Group 2: LMP91002 register write-back
    RUN_TEST(test_lmp91002_tiacn_readback);
    RUN_TEST(test_lmp91002_refcn_readback);
    RUN_TEST(test_lmp91002_modecn_readback);

    // Group 3: ADS1115 channel voltage ranges
    RUN_TEST(test_ain0_h2s_near_vref);
    RUN_TEST(test_ain1_battery_in_range);
    RUN_TEST(test_ain2_thermistor_in_range);

    // Group 4: Temperature plausibility
    RUN_TEST(test_temperature_in_room_range);

    // Group 5: SD card
    RUN_TEST(test_sd_card_mounts_and_basic_io);
    RUN_TEST(test_sd_multisector_write);
    RUN_TEST(test_sd_survives_remount);

    // Group 6: Pump GPIO
    RUN_TEST(test_pump_gpio_toggles);

    // Group 7: System health
    RUN_TEST(test_heap_headroom_adequate);
    RUN_TEST(test_task_stacks_allocatable);

    // Group 8: I2C bus recovery
    RUN_TEST(test_i2c_recovers_after_failed_transaction);

    UNITY_END();

    // ── Cleanup ───────────────────────────────────────────────────────────────
    gpio_set_level(PUMP_GPIO, 0);
    sd_unmount();
    spi_bus_free((spi_host_device_t)s_host.slot);
    i2c_driver_delete(I2C_PORT);
}
