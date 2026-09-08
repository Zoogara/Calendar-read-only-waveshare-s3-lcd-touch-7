#pragma once
/*
 * Presence sensor on GPIO6 - high while presence is detected, low
 * otherwise. Wired directly to the ESP32-S3, no expander/level-shifter
 * involved (unlike most of this board's other peripherals, which go
 * through the CH422G).
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configures GPIO6 as an input with the internal pull-down enabled.
 * The pull-down isn't optional: with no pull at all, the line floated and
 * showed frequent false transitions (flickering between detected/clear
 * every few seconds with nobody around) - confirmed on real hardware that
 * enabling this dropped false transitions dramatically (observed gaps of
 * 80+ seconds between real state changes, vs. seconds before). Consistent
 * with the sensor's output being open-drain active-high - it actively
 * pulls the line low itself, needing something else to hold it low the
 * rest of the time. */
void presence_sensor_init(void);

/* True if presence is currently detected. A plain synchronous GPIO read -
 * cheap enough to call from a periodic timer (e.g. ui_screensaver.c's
 * existing 1-second tick) without needing a dedicated polling task. Logs
 * on state change (INFO level) purely as an on-device diagnostic. */
bool presence_sensor_is_detected(void);

#ifdef __cplusplus
}
#endif
