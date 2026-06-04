#include "display.h"
#include "sensor.h"
#include "storage.h"
#include "pump.h"
#include "power.h"
#include "rtc.h"
#include "sample.h"
#include "app_config.h"
#include "queues.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include <stdio.h>

static const char *TAG = "main";

static void pump_delayed_start(void *pv) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    pump_run_continuous();
    vTaskDelete(NULL);
}

// FreeRTOS queues
QueueHandle_t q_display = NULL;
QueueHandle_t q_storage = NULL;
SemaphoreHandle_t i2c_mutex = NULL;
SemaphoreHandle_t sd_mutex = NULL;

static void app_fatal(const char *message) {
	ESP_LOGE(TAG, "FATAL: %s", message ? message : "unknown");
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

extern "C" void app_main(void) {
	ESP_LOGI(TAG, "app_main started");

	// NVS must be initialised before any driver that reads persistent config.
	esp_err_t nvs_ret = nvs_flash_init();
	if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		nvs_ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(nvs_ret);

	// Mutexes and queues must exist before any init that touches I2C or SD.
	q_display = xQueueCreate(APP_DISPLAY_QUEUE_LENGTH, sizeof(sample_t));
	q_storage = xQueueCreate(APP_STORAGE_QUEUE_LENGTH, sizeof(sample_t));
	i2c_mutex = xSemaphoreCreateMutex();
	sd_mutex  = xSemaphoreCreateMutex();
	if (!q_display || !q_storage || !i2c_mutex || !sd_mutex) {
		app_fatal("failed to create queues or mutexes");
	}
	ESP_LOGI(TAG, "queues and mutexes OK");

	// sensor_init installs the I2C driver; display_init must come after.
	ESP_LOGI(TAG, "sensor_init...");
	sensor_init();
	ESP_LOGI(TAG, "rtc_init...");
	rtc_init();
	ESP_LOGI(TAG, "display_init...");
	display_init();
	ESP_LOGI(TAG, "storage_init...");
	storage_init();
	ESP_LOGI(TAG, "pump_init...");
	pump_init();
	ESP_LOGI(TAG, "power_init...");
	power_init();

	ESP_LOGI(TAG, "all init done, starting tasks");

	// Create tasks
	xTaskCreate(sensor_task,         "SensorTask",  APP_SENSOR_TASK_STACK,  NULL, APP_SENSOR_TASK_PRIO,  NULL);
	xTaskCreate(display_task,        "DisplayTask", APP_DISPLAY_TASK_STACK, NULL, APP_DISPLAY_TASK_PRIO, NULL);
	xTaskCreate(storage_task,        "StorageTask", APP_STORAGE_TASK_STACK, NULL, APP_STORAGE_TASK_PRIO, NULL);
	xTaskCreate(power_task,          "PowerTask",   APP_POWER_TASK_STACK,   NULL, APP_POWER_TASK_PRIO,   NULL);
	xTaskCreate(pump_delayed_start,  "PumpStart",   1024,                   NULL, 1,                     NULL);

	// Keep main alive (tasks run in FreeRTOS)
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}
