/* Force-included in every file of the host build (see Makefile). */
#pragma once
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
/* The UI reads the wall clock with time(); the renderer pins it to a fixed, fictional moment. */
time_t host_time(time_t *t);
#define time(t) host_time(t)
