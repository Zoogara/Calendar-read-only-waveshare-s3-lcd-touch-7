#pragma once
#include "task.h"
static inline BaseType_t xTaskCreateWithCaps(void (*f)(void *), const char *n, uint32_t s, void *a, UBaseType_t p, TaskHandle_t *h, uint32_t caps)
{ (void)f; (void)n; (void)s; (void)a; (void)p; (void)h; (void)caps; return pdPASS; }
