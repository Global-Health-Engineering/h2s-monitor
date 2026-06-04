#ifndef QUEUES_H
#define QUEUES_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

extern QueueHandle_t q_display;
extern QueueHandle_t q_storage;
extern SemaphoreHandle_t i2c_mutex;
extern SemaphoreHandle_t sd_mutex;

#endif // QUEUES_H
