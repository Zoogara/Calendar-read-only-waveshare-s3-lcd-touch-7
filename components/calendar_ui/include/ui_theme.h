#pragma once
/* Shared look-and-feel constants for the calendar screens - kept in one
 * place so the four views (month/week/day/up-next) read as one app,
 * loosely modelled on the Google Calendar Android app's light theme. */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_COLOR_BG           0xF6F6F6
#define UI_COLOR_SURFACE      0xFFFFFF
#define UI_COLOR_TEXT         0x202124
#define UI_COLOR_TEXT_MUTED   0x5F6368
#define UI_COLOR_ACCENT       0x1A73E8
#define UI_COLOR_GRID_LINE    0xE3E3E3
#define UI_COLOR_NAV_BG       0xFFFFFF
#define UI_COLOR_NAV_SELECTED 0xE8F0FE
#define UI_COLOR_TODAY_BG     0x1A73E8
#define UI_COLOR_WARNING      0xD93025
/* Text colour for past events on their now-pale (ui_lighten()'d)
 * background - matches how the Google Calendar app renders them: light
 * grey text, not the white used on a still-saturated current/future
 * event's full-strength colour. */
#define UI_COLOR_TEXT_PAST    0x9AA0A6

/* Fixed screen geometry (800x480 panel) - see calendar_ui.c. */
#define UI_NAV_RAIL_W   90
#define UI_TOP_BAR_H    56
#define UI_LEGEND_H     34
#define UI_CONTENT_X    UI_NAV_RAIL_W
#define UI_CONTENT_Y    (UI_TOP_BAR_H + UI_LEGEND_H)
#define UI_CONTENT_W    (800 - UI_NAV_RAIL_W)
#define UI_CONTENT_H    (480 - UI_CONTENT_Y)

static inline lv_color_t ui_color(uint32_t rgb888)
{
    return lv_color_hex(rgb888);
}

/* A visibly lighter, pale/washed-out shade of a calendar/event colour
 * (blended 65% of the way to white) - used to fade out events that have
 * already finished, so a day that's mostly past reads at a glance as
 * mostly faded without losing which calendar each event belongs to.
 * Matches the Google Calendar app's own past-event styling (a pale tint
 * of the calendar colour, not just a slightly darker one) - paired with
 * UI_COLOR_TEXT_PAST for the label instead of white, since white doesn't
 * read on a background this pale. */
static inline uint32_t ui_lighten(uint32_t rgb888)
{
    uint8_t r = (uint8_t)((rgb888 >> 16) & 0xFF);
    uint8_t g = (uint8_t)((rgb888 >> 8) & 0xFF);
    uint8_t b = (uint8_t)(rgb888 & 0xFF);
    r = (uint8_t)(r + (255 - r) * 65 / 100);
    g = (uint8_t)(g + (255 - g) * 65 / 100);
    b = (uint8_t)(b + (255 - b) * 65 / 100);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

#ifdef __cplusplus
}
#endif
