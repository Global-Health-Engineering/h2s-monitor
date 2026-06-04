#ifndef SAMPLE_H
#define SAMPLE_H

#include <stdint.h>
#include <time.h>

typedef struct sample_t {
    uint64_t  timestamp;   // ms since boot
    struct tm datetime;    // wall-clock time from RTC
    uint32_t  seq;         // reading number, 1-based
    int32_t   raw;         // raw ADS1115 counts (AIN0)
    float     ppb;         // calibrated H2S reading in ppm
    float     temperature; // degrees Celsius
    uint32_t  battery_mv;  // battery voltage in mV
    uint32_t  status;      // bitflags (see below)
} sample_t;

#define STATUS_LOW_BATTERY   (1u << 0)
#define STATUS_SD_ERROR      (1u << 1)
#define STATUS_SENSOR_WARMUP (1u << 2)
#define STATUS_PUMP_RUNNING  (1u << 3)
#define STATUS_CHARGING      (1u << 4)
#define STATUS_CALIBRATING   (1u << 5)

#endif // SAMPLE_H
