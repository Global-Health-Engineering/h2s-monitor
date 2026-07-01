#ifndef PUMP_H
#define PUMP_H

#include <stdint.h>

void pump_init(void);
void pump_start(uint32_t duration_ms);
void pump_run_continuous(void);
void pump_stop(void);

#endif // PUMP_H
