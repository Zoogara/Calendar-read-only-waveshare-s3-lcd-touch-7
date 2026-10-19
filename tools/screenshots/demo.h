#pragma once
/* Fictional demo data for the screenshot renderer. Nothing here is real: the town "Port
 * Hartley", the household, the events and the news items are invented (the weather curves are
 * made up; the "on this day" entries are well-known public history). */
#include <time.h>
#include "app_settings.h"

void demo_set_now(time_t now);
time_t demo_now(void);
time_t demo_local(int y, int mo, int d, int h, int mi);          /* local wall time -> epoch */
void demo_fill_settings(app_settings_t *cfg);
void demo_fill_events(void);                                      /* publishes into event_store */
void demo_build_data(int night);                                  /* weather, graphs, feeds, history */
