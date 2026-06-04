#include "rtc.h"
#include "queues.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"  // TODO: migrate to driver/i2c_master.h before ESP-IDF v7.0
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "rtc";

#define DS3232_ADDR  0x68
#define I2C_PORT     I2C_NUM_0
#define ACK_EN       true

// Register map
#define REG_SECONDS  0x00
#define REG_STATUS   0x0F
#define STATUS_OSF   (1 << 7)   // oscillator stop flag — set on fresh/dead battery

static inline uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static inline uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

static esp_err_t ds_write(uint8_t reg, const uint8_t *data, size_t len) {
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (DS3232_ADDR << 1) | I2C_MASTER_WRITE, ACK_EN);
    i2c_master_write_byte(h, reg, ACK_EN);
    i2c_master_write(h, (uint8_t *)data, len, ACK_EN);
    i2c_master_stop(h);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
    return ret;
}

static esp_err_t ds_read(uint8_t reg, uint8_t *data, size_t len) {
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (DS3232_ADDR << 1) | I2C_MASTER_WRITE, ACK_EN);
    i2c_master_write_byte(h, reg, ACK_EN);
    i2c_master_stop(h);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
    if (ret != ESP_OK) return ret;

    h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (DS3232_ADDR << 1) | I2C_MASTER_READ, ACK_EN);
    i2c_master_read(h, data, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(h);
    ret = i2c_master_cmd_begin(I2C_PORT, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
    return ret;
}

// Parse __DATE__ ("Mon DD YYYY") and __TIME__ ("HH:MM:SS") into struct tm.
static void parse_compile_time(struct tm *dt) {
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {};
    int day = 0, year = 0, hour = 0, min = 0, sec = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);

    const char *p = strstr(months, mon);
    dt->tm_mon  = p ? (int)(p - months) / 3 : 0;
    dt->tm_mday = day;
    dt->tm_year = year - 1900;
    dt->tm_hour = hour;
    dt->tm_min  = min;
    dt->tm_sec  = sec;
}

static void set_rtc(const struct tm *dt) {
    uint8_t regs[7] = {
        dec2bcd(dt->tm_sec),
        dec2bcd(dt->tm_min),
        dec2bcd(dt->tm_hour),
        1,                          // day-of-week (unused; set to 1)
        dec2bcd(dt->tm_mday),
        dec2bcd(dt->tm_mon + 1),
        dec2bcd(dt->tm_year % 100),
    };
    ds_write(REG_SECONDS, regs, 7);

    // Clear the OSF bit in the status register
    uint8_t status = 0;
    ds_read(REG_STATUS, &status, 1);
    status &= ~STATUS_OSF;
    ds_write(REG_STATUS, &status, 1);
}

void rtc_init(void) {
    uint8_t status = 0;
    if (i2c_mutex && xSemaphoreTake(i2c_mutex, portMAX_DELAY) == pdTRUE) {
        esp_err_t ret = ds_read(REG_STATUS, &status, 1);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "DS3232M not found on I2C (err %s)", esp_err_to_name(ret));
            xSemaphoreGive(i2c_mutex);
            return;
        }

        if (status & STATUS_OSF) {
            // Oscillator was stopped — set from compile time
            struct tm dt = {};
            parse_compile_time(&dt);
            set_rtc(&dt);
            ESP_LOGI(TAG, "RTC set from compile time: %04d-%02d-%02d %02d:%02d:%02d",
                     dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday,
                     dt.tm_hour, dt.tm_min, dt.tm_sec);
        } else {
            ESP_LOGI(TAG, "RTC already running (OSF=0), time preserved");
        }
        xSemaphoreGive(i2c_mutex);
    }
}

bool rtc_get_datetime(struct tm *dt) {
    uint8_t regs[7];
    bool ok = false;
    if (i2c_mutex && xSemaphoreTake(i2c_mutex, portMAX_DELAY) == pdTRUE) {
        ok = (ds_read(REG_SECONDS, regs, 7) == ESP_OK);
        xSemaphoreGive(i2c_mutex);
    }
    if (!ok) return false;

    dt->tm_sec  = bcd2dec(regs[0] & 0x7F);
    dt->tm_min  = bcd2dec(regs[1] & 0x7F);
    dt->tm_hour = bcd2dec(regs[2] & 0x3F);
    dt->tm_mday = bcd2dec(regs[4] & 0x3F);
    dt->tm_mon  = bcd2dec(regs[5] & 0x1F) - 1;
    dt->tm_year = bcd2dec(regs[6]) + 100;  // DS3232 stores 00-99; offset from 1900
    return true;
}

void rtc_format_filename(char *buf, size_t len) {
    struct tm dt = {};
    if (rtc_get_datetime(&dt)) {
        snprintf(buf, len, "log%04d%02d%02d_%02d%02d%02d",
                 dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday,
                 dt.tm_hour, dt.tm_min, dt.tm_sec);
    } else {
        snprintf(buf, len, "log_UNKNOWN");
    }
}