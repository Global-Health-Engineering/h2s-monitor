#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define APP_SENSOR_PERIOD_MS    3000
#define APP_DISPLAY_REFRESH_MS  1000
#define APP_POWER_MONITOR_MS    10000

#define APP_DISPLAY_QUEUE_LENGTH 8
#define APP_STORAGE_QUEUE_LENGTH 32

#define APP_DISPLAY_TASK_STACK  4096
#define APP_SENSOR_TASK_STACK   4096
#define APP_STORAGE_TASK_STACK  8192
#define APP_POWER_TASK_STACK    4096

#define APP_DISPLAY_TASK_PRIO   1
#define APP_SENSOR_TASK_PRIO    2
#define APP_STORAGE_TASK_PRIO   1
#define APP_POWER_TASK_PRIO     3

#define APP_LOW_BATTERY_MV      3200  // ~17% for Li-Ion (3.0V cutoff, 4.2V full)
#define APP_PUMP_DEFAULT_MS     2000
#define APP_OLED_I2C_ADDR       0x3C

// LMP91002 TIA feedback resistor (Ohms) — adjust after PCB testing
#define SENSOR_TIA_GAIN_OHMS    35000

// Samples before zero-calibration starts (at APP_SENSOR_PERIOD_MS each).
// Electrochemical sensors need ~60 s to stabilise — 20 × 3 s = 60 s.
#define SENSOR_WARMUP_SAMPLES   20

// Samples averaged to compute the zero-reference voltage.
// More samples → more stable baseline estimate.
#define SENSOR_CAL_SAMPLES      30

// ADS1115 supply voltage (used for Vref calculation)
#define ADS1115_VDD             3.3f

#endif // APP_CONFIG_H
