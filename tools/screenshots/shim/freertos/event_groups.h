#pragma once
#include "FreeRTOS.h"
typedef void *EventGroupHandle_t;
typedef uint32_t EventBits_t;
static inline EventGroupHandle_t xEventGroupCreate(void) { return (void *)1; }
static inline EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b) { (void)g; return b; }
static inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t b, BaseType_t c, BaseType_t a, TickType_t t) { (void)g; (void)b; (void)c; (void)a; (void)t; return 0; }
