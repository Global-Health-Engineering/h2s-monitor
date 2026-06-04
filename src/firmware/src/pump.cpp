#include "pump.h"
#include "sample.h"
#include "app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "pump";

#define PUMP_GPIO GPIO_NUM_17

static TimerHandle_t s_pump_timer = NULL;

static void pump_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    gpio_set_level(PUMP_GPIO, 0);
    ESP_LOGI(TAG, "pump OFF (timer expired)");
}

void pump_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PUMP_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PUMP_GPIO, 0);

    s_pump_timer = xTimerCreate("pump", pdMS_TO_TICKS(APP_PUMP_DEFAULT_MS),
                                pdFALSE, NULL, pump_timer_cb);
}

void pump_start(uint32_t duration_ms) {
    if (!s_pump_timer) return;
    ESP_LOGI(TAG, "pump ON for %lu ms", (unsigned long)duration_ms);
    gpio_set_level(PUMP_GPIO, 1);
    xTimerChangePeriod(s_pump_timer, pdMS_TO_TICKS(duration_ms), 0);
    xTimerStart(s_pump_timer, 0);
}

void pump_run_continuous(void) {
    if (s_pump_timer) xTimerStop(s_pump_timer, 0);
    gpio_set_level(PUMP_GPIO, 1);
    ESP_LOGI(TAG, "pump ON (continuous)");
}

void pump_stop(void) {
    if (s_pump_timer) xTimerStop(s_pump_timer, 0);
    gpio_set_level(PUMP_GPIO, 0);
    ESP_LOGI(TAG, "pump OFF (manual stop)");
}
