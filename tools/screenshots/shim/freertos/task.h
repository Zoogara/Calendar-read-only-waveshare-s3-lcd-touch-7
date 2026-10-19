#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline BaseType_t xTaskCreate(void (*f)(void *), const char *n, uint32_t s, void *a, UBaseType_t p, TaskHandle_t *h) { (void)f; (void)n; (void)s; (void)a; (void)p; (void)h; return pdPASS; }
static inline void vTaskDelete(TaskHandle_t t) { (void)t; }
