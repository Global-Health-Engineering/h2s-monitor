#pragma once
#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// DS3232M RTC — I2C address 0x68, shares the I2C bus with other peripherals.
// Call rtc_init() after sensor_init() (I2C driver must be installed first).
// If the oscillator-stop flag is set (fresh battery or first boot), the RTC
// is written with the firmware's compile-time timestamp.

void rtc_init(void);

// Fill *dt with the current date/time.  Returns true on success.
bool rtc_get_datetime(struct tm *dt);

// Write a filename string of the form "logYYYYMMDD_HHMMSS" into buf.
// Falls back to "log_UNKNOWN" if the RTC cannot be read.
void rtc_format_filename(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
