#include "power.h"
#include "app_config.h"
#include "sample.h"
#include "queues.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static volatile uint32_t s_battery_mv = 4200;

// Rolling buffer: compare newest reading to the one ~2 min ago.
// 40 readings × 3 s = 120 s window. If voltage rose > 6 mV → charging.
#define CHG_BUF_SIZE     40
#define CHG_THRESHOLD_MV  6

static uint32_t s_chg_buf[CHG_BUF_SIZE];
static uint8_t  s_chg_head = 0;
static uint8_t  s_chg_fill = 0;
static volatile bool s_charging = false;

void power_update_battery_mv(uint32_t mv) {
    s_battery_mv = mv;

    s_chg_buf[s_chg_head] = mv;
    s_chg_head = (s_chg_head + 1) % CHG_BUF_SIZE;
    if (s_chg_fill < CHG_BUF_SIZE) s_chg_fill++;

    if (s_chg_fill == CHG_BUF_SIZE) {
        // oldest entry sits at s_chg_head (next slot to be overwritten)
        uint32_t oldest = s_chg_buf[s_chg_head];
        s_charging = (mv > oldest + CHG_THRESHOLD_MV);
    }
}

bool power_is_charging(void) {
    return s_charging;
}

void power_init(void) {
    // Nothing to configure — battery is read by ADS1115 in sensor_task.
}

void power_task(void *pvParameter) {
    (void)pvParameter;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(APP_POWER_MONITOR_MS));
        // Low-battery visual is handled by display_task (blinking border on battery %).
        // This task is kept for future actions (e.g. graceful shutdown).
    }
}
