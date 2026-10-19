#pragma once
#include <stdint.h>
#include <stdlib.h>
static inline uint32_t esp_random(void) { return (uint32_t)rand(); }
static inline void esp_fill_random(void *b, size_t n) { for (size_t i = 0; i < n; i++) ((unsigned char *)b)[i] = (unsigned char)rand(); }
