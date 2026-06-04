#include "storage.h"
#include "app_config.h"
#include "sample.h"
#include "queues.h"
#include "rtc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define SD_MISO  GPIO_NUM_20
#define SD_CS    GPIO_NUM_18
#define SD_SCK   GPIO_NUM_19
#define SD_MOSI  GPIO_NUM_21
#define MOUNT_PT "/sdcard"

static const char *TAG = "storage";

static char   s_filename[64];
static bool   s_mounted = false;

// Returns a monotonic boot count stored in NVS.
static uint32_t get_boot_count(void) {
    nvs_handle_t h;
    uint32_t count = 0;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boot_cnt", &count);
        nvs_set_u32(h, "boot_cnt", count + 1);
        nvs_commit(h);
        nvs_close(h);
    }
    return count;
}

static bool mount_sd(void) {
    esp_log_level_set("sdmmc", ESP_LOG_VERBOSE);
    esp_log_level_set("sdspi_transaction", ESP_LOG_VERBOSE);
    esp_log_level_set("sdspi_host", ESP_LOG_VERBOSE);

    // GPIO sanity check — log pin levels before handing off to SPI driver.
    gpio_set_direction(SD_CS,   GPIO_MODE_OUTPUT);
    gpio_set_direction(SD_MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SD_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_CS,   GPIO_PULLUP_ONLY);
    gpio_set_level(SD_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "GPIO pre-SPI: CS(%d)=%d  MISO(%d)=%d  SCK(%d)=input  MOSI(%d)=input",
             SD_CS,   gpio_get_level(SD_CS),
             SD_MISO, gpio_get_level(SD_MISO),
             SD_SCK,  SD_MOSI);
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed   = false,
        .max_files                = 4,
        .allocation_unit_size     = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat              = false,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 400;  // 400 kHz — SD spec minimum for init

    spi_bus_config_t bus_cfg = {};
    // ESP-IDF/C++ compatibility: use data0/data1 aliases instead of mosi/miso designated init.
    bus_cfg.data0_io_num   = SD_MOSI; // MOSI
    bus_cfg.data1_io_num   = SD_MISO; // MISO
    bus_cfg.sclk_io_num    = SD_SCK;
    bus_cfg.quadwp_io_num  = -1;
    bus_cfg.quadhd_io_num  = -1;
    bus_cfg.max_transfer_sz = 4096;
    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS;
    slot_cfg.host_id = (spi_host_device_t)host.slot;

    sdmmc_card_t *card;
    ret = esp_vfs_fat_sdspi_mount(MOUNT_PT, &host, &slot_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        spi_bus_free((spi_host_device_t)host.slot);
        return false;
    }
    return true;
}

void storage_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_mounted = mount_sd();
    if (!s_mounted) {
        ESP_LOGE(TAG, "SD card not available");
        return;
    }

    char stem[32];
    rtc_format_filename(stem, sizeof(stem));
    if (strcmp(stem, "log_UNKNOWN") == 0) {
        // RTC not available — fall back to boot counter
        uint32_t boot = get_boot_count();
        snprintf(stem, sizeof(stem), "log_%04lu", (unsigned long)boot);
    }
    snprintf(s_filename, sizeof(s_filename), MOUNT_PT "/%s.csv", stem);

    FILE *f = fopen(s_filename, "w");
    if (f) {
        fprintf(f, "datetime,timestamp_ms,ppm,temperature_c,battery_mv,status\n");
        fclose(f);
    } else {
        ESP_LOGE(TAG, "Could not create log file");
        s_mounted = false;
    }
}

static void write_sample(const sample_t *s) {
    FILE *f = fopen(s_filename, "a");
    if (!f) {
        ESP_LOGE(TAG, "fopen failed");
        return;
    }
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%llu,%.3f,%.2f,%lu,%lu\n",
            s->datetime.tm_year + 1900, s->datetime.tm_mon + 1, s->datetime.tm_mday,
            s->datetime.tm_hour, s->datetime.tm_min, s->datetime.tm_sec,
            (unsigned long long)s->timestamp,
            s->ppb,
            s->temperature,
            (unsigned long)s->battery_mv,
            (unsigned long)s->status);
    fclose(f);  // flush by close; reopen next sample
}

void storage_task(void *pvParameter) {
    (void)pvParameter;
    sample_t s = {};

    for (;;) {
        if (q_storage && xQueueReceive(q_storage, &s, portMAX_DELAY) == pdTRUE) {
            printf("DATA,%04d-%02d-%02d %02d:%02d:%02d,%llu,%.3f,%.2f,%lu,%lu\n",
                   s.datetime.tm_year + 1900, s.datetime.tm_mon + 1, s.datetime.tm_mday,
                   s.datetime.tm_hour, s.datetime.tm_min, s.datetime.tm_sec,
                   (unsigned long long)s.timestamp,
                   s.ppb, s.temperature,
                   (unsigned long)s.battery_mv,
                   (unsigned long)s.status);

            if (!s_mounted) {
                ESP_LOGE(TAG, "SD not mounted, skipping sample");
                continue;
            }
            if (sd_mutex && xSemaphoreTake(sd_mutex, portMAX_DELAY) == pdTRUE) {
                write_sample(&s);
                xSemaphoreGive(sd_mutex);
            }
            ESP_LOGI(TAG, "#%lu ts=%llums ppm=%.3f temp=%.1fC batt=%lumV status=0x%lx",
                     (unsigned long)s.seq,
                     (unsigned long long)s.timestamp,
                     s.ppb,
                     s.temperature,
                     (unsigned long)s.battery_mv,
                     (unsigned long)s.status);
        }
    }
}
