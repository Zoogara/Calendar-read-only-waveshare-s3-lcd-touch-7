#pragma once
#include <stdlib.h>
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_DMA 8
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t c, size_t n, int caps) { (void)caps; return calloc(c, n); }
static inline void *heap_caps_realloc(void *p, size_t n, int caps) { (void)caps; return realloc(p, n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(int caps) { (void)caps; return 0; }
static inline size_t heap_caps_get_largest_free_block(int caps) { (void)caps; return 0; }
