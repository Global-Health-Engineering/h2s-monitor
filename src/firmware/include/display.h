// Display API (lightweight stubs)
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include "sample.h"

#ifdef __cplusplus
extern "C" {
#endif

void display_init(void);
void display_clear(void);
void display_show_status(const char *msg);
void display_show_reading(const sample_t *s);
void display_backlight_on(void);
void display_backlight_off(void);
void display_sleep(void);
void display_wake(void);

// FreeRTOS task entry (can be used with xTaskCreate)
void display_task(void *pvParameter);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_H
