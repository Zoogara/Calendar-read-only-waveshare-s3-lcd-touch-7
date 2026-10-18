/* Weather details panel - opened by tapping the temperature on the ambient
 * clock (ui_clock.c), filled from components/ha_weather's details
 * (ha_weather_details.c: current readings plus the BOM 7-day forecast, from
 * Home Assistant).
 *
 * Same pattern as ui_history.c: a full-screen scrolling overlay on
 * lv_layer_top() above the clock, built when opened and deleted when closed,
 * so its ~45 objects only cost internal RAM while it's showing. A tap closes
 * it (a drag scrolls); ui_screensaver.c closes it after a minute untouched.
 *
 *   Weather for <place> - <date>   (a fixed heading; the rest scrolls)
 *   Now        16.0°C, feels like 14.4°C / wind / rain and pressure
 *   Today      [48px icon]  min / max, rain chance and amount, long forecast
 *   Tomorrow   [48px icon]  the same
 *   Thu..Mon   [32px icon]  short forecast, min / max, rain chance */
#include "calendar_ui_internal.h"
#include "ui_theme.h"
#include "ha_weather.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"

#define BG_COLOR     0x000000
#define HEAD_COLOR   0xE8EAED
#define TEXT_COLOR   0xBDC1C6
#define DIM_COLOR    0x9AA0A6
#define ICON_COLOR   0x8AB4F8

#define DEG "\xC2\xB0"

static const char *TAG = "ui_weather";

static lv_obj_t *s_overlay;

static void delete_overlay(void)
{
    if (s_overlay == NULL) {
        return;
    }
    lv_obj_del(s_overlay);
    s_overlay = NULL;
    /* See ui_history.c / ui_lock.c: a full-screen lv_layer_top() object
     * needs both framebuffers redrawn after it goes. */
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}

static void close_async(void *arg)
{
    (void)arg;
    delete_overlay();
    ui_screensaver_keypad_dismissed();   /* the closing tap isn't a new touch on the clock */
    ESP_LOGI(TAG, "closed");
}

static void tap_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
    lv_async_call(close_async, NULL);
}

static lv_obj_t *add_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    return l;
}

/* A label that takes the rest of its row's width and wraps. */
static lv_obj_t *add_wrapping_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = add_label(parent, font, color, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static lv_obj_t *add_container(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, flow);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(c, 14, 0);
    lv_obj_set_style_pad_row(c, 2, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);   /* taps reach the list -> close */
    return c;
}

static lv_obj_t *add_icon(lv_obj_t *parent, const lv_font_t *font, const char *cond, int width)
{
    lv_obj_t *l = add_label(parent, font, ICON_COLOR, ui_weather_glyph(cond, false));
    lv_obj_set_width(l, width);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* "6° / 21°", "21°" or "" - today's minimum is often gone by the afternoon. */
static void min_max(const ha_forecast_day_t *d, char *out, size_t sz)
{
    if (d->lo[0] && d->hi[0]) {
        snprintf(out, sz, "%s" DEG " / %s" DEG, d->lo, d->hi);
    } else if (d->hi[0]) {
        snprintf(out, sz, "max %s" DEG, d->hi);
    } else if (d->lo[0]) {
        snprintf(out, sz, "min %s" DEG, d->lo);
    } else {
        out[0] = '\0';
    }
}

/* "40%, 0–1 mm" (the range always shown with the chance, "0" included),
 * "40%" if there's no range, or "". prefix goes in front when non-empty. */
static void rain_text(const ha_forecast_day_t *d, const char *prefix, char *out, size_t sz)
{
    out[0] = '\0';
    if (d->rain_chance[0]) {
        int n = snprintf(out, sz, "%s%s%%", prefix, d->rain_chance);
        if (d->rain_range[0] && n > 0 && (size_t)n < sz) {
            snprintf(out + n, sz - n, ", %s mm", d->rain_range);
        }
    }
}

static void add_now(lv_obj_t *list, const ha_weather_details_t *d)
{
    char line[160];
    int n = 0;
    ha_weather_t wx;
    if (ha_weather_get(&wx)) {
        n = snprintf(line, sizeof(line), "%.1f" DEG "C", wx.temp_c);
    }
    if (d->feels.value[0]) {
        n += snprintf(line + n, sizeof(line) - n, "%sfeels like %s%s", n ? ", " : "",
                      d->feels.value, d->feels.unit);
    }
    if (n > 0) {
        add_label(list, &gcal_font_20, HEAD_COLOR, line);
    }

    n = 0;
    if (d->wdir.value[0] || d->wind.value[0]) {
        n = snprintf(line, sizeof(line), "Wind %s %s %s", d->wdir.value, d->wind.value, d->wind.unit);
    }
    if (d->gust.value[0]) {
        n += snprintf(line + n, sizeof(line) - n, "%sgusting %s %s", n ? ", " : "Wind ",
                      d->gust.value, d->gust.unit);
    }
    if (n > 0) {
        add_label(list, &gcal_font_20, TEXT_COLOR, line);
    }

    n = 0;
    if (d->rain.value[0]) {
        n = snprintf(line, sizeof(line), "Rain %s %s", d->rain.value, d->rain.unit);
    }
    if (d->pressure.value[0]) {
        n += snprintf(line + n, sizeof(line) - n, "%sPressure %s %s", n ? "     " : "",
                      d->pressure.value, d->pressure.unit);
    }
    if (n > 0) {
        add_label(list, &gcal_font_20, TEXT_COLOR, line);
    }
}

/* Today / Tomorrow: big icon, then a summary line and the long forecast. */
static void add_big_day(lv_obj_t *list, const ha_forecast_day_t *d, const char *name)
{
    lv_obj_t *row = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_top(row, 14, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    add_icon(row, &gcal_font_weather_48, d->icon, 72);

    lv_obj_t *col = add_container(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(col, 1);
    lv_obj_set_flex_grow(col, 1);

    char mm[32], rain[48], line[128];
    min_max(d, mm, sizeof(mm));
    rain_text(d, "rain ", rain, sizeof(rain));
    snprintf(line, sizeof(line), "%s:%s%s%s%s", name, mm[0] ? "   " : "", mm, rain[0] ? "   " : "", rain);
    add_label(col, &gcal_font_20, HEAD_COLOR, line);
    add_wrapping_label(col, &gcal_font_14, TEXT_COLOR, d->extended[0] ? d->extended : d->short_text);
}

/* Days 2-6: one line each. */
static void add_small_day(lv_obj_t *list, const ha_forecast_day_t *d, const char *name)
{
    lv_obj_t *row = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_t *day = add_label(row, &gcal_font_20, HEAD_COLOR, name);
    lv_obj_set_width(day, 52);
    add_icon(row, &gcal_font_weather, d->icon, 72);

    lv_obj_t *text = add_label(row, &gcal_font_20, TEXT_COLOR, d->short_text);
    lv_obj_set_width(text, 1);
    lv_obj_set_flex_grow(text, 1);
    lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);

    char mm[32];
    min_max(d, mm, sizeof(mm));
    lv_obj_t *t = add_label(row, &gcal_font_20, HEAD_COLOR, mm);
    lv_obj_set_width(t, 110);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_RIGHT, 0);

    char rc[40];
    rain_text(d, "", rc, sizeof(rc));
    lv_obj_t *r = add_label(row, &gcal_font_14, DIM_COLOR, rc);
    lv_obj_set_width(r, 110);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
}

/* The last 24 hours of one sensor (components/ha_weather/ha_history.c) as a
 * line graph at the end of the list: heading, a Y-axis label column and the
 * chart side by side, a row of times under it, and a summary line. Left out
 * entirely when there is no history yet. */
#define GRAPH_H        150
#define GRAPH_AXIS_W   56

static void hhmm(time_t t, char *out, size_t sz)
{
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(out, sz, "%H:%M", &lt);
}

static void add_history_graph(lv_obj_t *list)
{
    const ha_history_t *h = ha_history_acquire();
    if (h == NULL || h->have < 2) {
        ha_history_release();
        return;
    }

    /* Y range: whole degrees (or units) around the data, never flatter than 2. */
    float lo = floorf(h->min), hi = ceilf(h->max);
    if (hi - lo < 2.0f) {
        lo -= 1.0f;
        hi += 1.0f;
    }

    char title[120], unit[16];
    snprintf(unit, sizeof(unit), "%s", h->unit);
    snprintf(title, sizeof(title), "%s \xE2\x80\x93 last 24 hours", h->name[0] ? h->name : h->entity);

    lv_obj_t *gap = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(gap, 10);
    lv_obj_t *heading = add_label(list, &gcal_font_20, HEAD_COLOR, title);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);

    lv_obj_t *row = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 6, 0);

    /* Y-axis labels: the range top and bottom, spread over the chart's height. */
    lv_obj_t *axis = add_container(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(axis, GRAPH_AXIS_W, GRAPH_H);
    lv_obj_set_flex_align(axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    char buf[40];
    snprintf(buf, sizeof(buf), "%.0f", (double)hi);
    add_label(axis, &gcal_font_14, DIM_COLOR, buf);
    snprintf(buf, sizeof(buf), "%.0f", (double)((lo + hi) / 2.0f));
    add_label(axis, &gcal_font_14, DIM_COLOR, buf);
    snprintf(buf, sizeof(buf), "%.0f", (double)lo);
    add_label(axis, &gcal_font_14, DIM_COLOR, buf);

    lv_obj_t *chart = lv_chart_create(row);
    lv_obj_set_size(chart, LV_HOR_RES - 64 - GRAPH_AXIS_W - 6, GRAPH_H);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, HA_HISTORY_POINTS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(lo * 10), (lv_coord_t)(hi * 10));
    lv_chart_set_div_line_count(chart, 4, 7);    /* a line every 4 hours, 3 horizontal */
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x3C4043), 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2C2E), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);   /* a plain line, no dots */
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);      /* taps and drags go to the list */
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_chart_series_t *ser = lv_chart_add_series(chart, lv_color_hex(ICON_COLOR), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < HA_HISTORY_POINTS; i++) {
        lv_chart_set_value_by_id(chart, ser, i,
                                 isnan(h->v[i]) ? LV_CHART_POINT_NONE : (lv_coord_t)lroundf(h->v[i] * 10.0f));
    }
    lv_chart_refresh(chart);

    /* Times under the chart: start, middle, end. */
    time_t t_end = h->end;
    time_t t_start = t_end - (time_t)(HA_HISTORY_POINTS - 1) * h->step_s;
    lv_obj_t *times = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(times, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(times, GRAPH_AXIS_W + 6, 0);
    char a[8], b[8], c[8];
    hhmm(t_start, a, sizeof(a));
    hhmm((t_start + t_end) / 2, b, sizeof(b));
    hhmm(t_end, c, sizeof(c));
    add_label(times, &gcal_font_14, DIM_COLOR, a);
    add_label(times, &gcal_font_14, DIM_COLOR, b);
    add_label(times, &gcal_font_14, DIM_COLOR, c);

    /* "Now 16.1 °C   Low 10.5 at 04:43   High 23.6 at 13:47" */
    float now_v = h->v[HA_HISTORY_POINTS - 1];
    char lo_t[8], hi_t[8];
    hhmm(t_end - (time_t)(HA_HISTORY_POINTS - 1 - h->min_i) * h->step_s, lo_t, sizeof(lo_t));
    hhmm(t_end - (time_t)(HA_HISTORY_POINTS - 1 - h->max_i) * h->step_s, hi_t, sizeof(hi_t));
    snprintf(buf, sizeof(buf), "%s", unit[0] ? unit : "");
    char line[160];
    snprintf(line, sizeof(line), "Now %.1f%s%s    Low %.1f at %s    High %.1f at %s", (double)now_v,
             unit[0] ? " " : "", unit, (double)h->min, lo_t, (double)h->max, hi_t);
    lv_obj_t *sum = add_label(list, &gcal_font_14, TEXT_COLOR, line);
    lv_obj_set_width(sum, LV_PCT(100));
    ha_history_release();
}

bool ui_weather_is_open(void)
{
    return s_overlay != NULL;
}

void ui_weather_open(void)
{
    if (s_overlay != NULL) {
        return;
    }

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, tap_cb, LV_EVENT_CLICKED, NULL);

    /* Fixed heading - outside the scrolling list below. */
    time_t now_t = time(NULL);
    struct tm today;
    localtime_r(&now_t, &today);
    char wday[12], month[12], heading[96];
    strftime(wday, sizeof(wday), "%A", &today);
    strftime(month, sizeof(month), "%B", &today);
    const char *place = ui_get_cfg()->weather_place;
    if (place[0] != '\0') {
        snprintf(heading, sizeof(heading), "Weather for %s \xE2\x80\x93 %s, %d %s",
                 place, wday, today.tm_mday, month);
    } else {
        snprintf(heading, sizeof(heading), "Weather \xE2\x80\x93 %s, %d %s", wday, today.tm_mday, month);
    }
    lv_obj_t *head = add_label(s_overlay, &gcal_font_20, HEAD_COLOR, heading);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 32, 20);

    lv_obj_t *list = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_HOR_RES - 64, LV_VER_RES - 72);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 32, 60);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(list, tap_cb, LV_EVENT_CLICKED, NULL);

    const ha_weather_details_t *d = ha_weather_details_acquire();
    if (d == NULL) {
        add_wrapping_label(list, &gcal_font_20, TEXT_COLOR,
                           "No weather details yet - they're fetched from Home Assistant "
                           "with the temperature every few minutes.");
    } else {
        add_now(list, d);
        /* Once today's minimum has passed (usually by the afternoon) the BOM
         * stops publishing it - the forecast is then only for what's left of
         * the day. */
        add_big_day(list, &d->days[0], d->days[0].lo[0] ? "Today" : "Remainder of today");
        add_big_day(list, &d->days[1], "Tomorrow");
        lv_obj_t *gap = add_container(list, LV_FLEX_FLOW_ROW);
        lv_obj_set_height(gap, 8);
        time_t now = time(NULL);
        for (int i = 2; i < HA_FORECAST_DAYS; i++) {
            if (d->days[i].short_text[0] == '\0' && d->days[i].icon[0] == '\0') {
                continue;
            }
            time_t t = now + (time_t)i * 86400;
            struct tm lt;
            localtime_r(&t, &lt);
            char name[8];
            strftime(name, sizeof(name), "%a", &lt);
            add_small_day(list, &d->days[i], name);
        }
    }
    ha_weather_details_release();
    add_history_graph(list);

    /* The opening tap's release mustn't land on the panel and close it. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
    ESP_LOGI(TAG, "open");
}

void ui_weather_close(void)
{
    if (s_overlay != NULL) {
        delete_overlay();
        ESP_LOGI(TAG, "closed (idle)");
    }
}
