#include "presence_sensor.h"

/* This build has no presence sensor fitted, so someone is always "there":
 * the idle display is the ambient clock rather than the screensaver. */

void presence_sensor_init(void)
{
}

bool presence_sensor_is_detected(void)
{
    return true;
}
