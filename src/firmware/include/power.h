#ifndef POWER_H
#define POWER_H

#include <stdint.h>

void power_init(void);
void power_task(void *pvParameter);

// Called by sensor_task after each battery measurement.
void power_update_battery_mv(uint32_t mv);

// Returns true if battery voltage has been rising steadily (~2 min detection lag).
bool power_is_charging(void);

#endif // POWER_H
