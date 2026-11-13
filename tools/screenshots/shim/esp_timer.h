#pragma once
#include <stdint.h>
/* Fictional uptime for the settings-menu screenshot: 2 d 5 h 12 min. */
static inline int64_t esp_timer_get_time(void) { return 191520LL * 1000000; }
