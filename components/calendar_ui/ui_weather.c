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
#include "ha_wind_calc.h"
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

/* The last 24 hours of up to two sensors (components/ha_weather/ha_history.c)
 * as line graphs on shared axes at the end of the list: a legend, a Y-axis
 * label column and the chart side by side, a row of times under it, and a
 * Now / Low / High line per sensor in that sensor's colour. Left out entirely
 * when there is no history yet. */
#define GRAPH_H        150
#define GRAPH_AXIS_W   56

static const uint32_t SERIES_COLOR[HA_HISTORY_SERIES] = { 0x8AB4F8, 0xF6AD55 };   /* blue, orange */

static void hhmm(time_t t, char *out, size_t sz)
{
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(out, sz, "%H:%M", &lt);
}

static void add_history_graph(lv_obj_t *list)
{
    const ha_history_t *h = ha_history_acquire();
    if (h == NULL || h->count < 1) {
        ha_history_release();
        return;
    }

    /* One Y range for every line: whole units around all the data, never flatter than 2. */
    float mn = h->s[0].min, mx = h->s[0].max;
    for (int k = 1; k < h->count; k++) {
        mn = fminf(mn, h->s[k].min);
        mx = fmaxf(mx, h->s[k].max);
    }
    float lo = floorf(mn), hi = ceilf(mx);
    if (hi - lo < 2.0f) {
        lo -= 1.0f;
        hi += 1.0f;
    }

    lv_obj_t *gap = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(gap, 10);
    lv_obj_t *heading = add_label(list, &gcal_font_20, HEAD_COLOR, "Last 24 hours");
    lv_obj_set_width(heading, LV_PCT(100));

    /* Legend: a coloured bar and the sensor's name, one per line on the graph. */
    lv_obj_t *legend = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(legend, 24, 0);
    for (int k = 0; k < h->count; k++) {
        const ha_series_t *sr = &h->s[k];
        char key[80];
        /* The label set on the config page wins over Home Assistant's name. */
        const app_settings_t *cfg = ui_get_cfg();
        const char *label = sr->slot == 0 ? cfg->ha_graph_label : cfg->ha_graph2_label;
        snprintf(key, sizeof(key), "\xE2\x80\x94\xE2\x80\x94 %s",
                 label[0] ? label : sr->name[0] ? sr->name : sr->entity);
        add_label(legend, &gcal_font_14, SERIES_COLOR[sr->slot], key);
    }

    lv_obj_t *row = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 6, 0);

    /* Y-axis labels: the range top, middle and bottom, spread over the chart's height. */
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
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);   /* plain lines, no dots */
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);      /* taps and drags go to the list */
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int k = 0; k < h->count; k++) {
        lv_chart_series_t *ser = lv_chart_add_series(chart, lv_color_hex(SERIES_COLOR[h->s[k].slot]),
                                                     LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < HA_HISTORY_POINTS; i++) {
            lv_chart_set_value_by_id(chart, ser, i, isnan(h->s[k].v[i]) ? LV_CHART_POINT_NONE
                                                  : (lv_coord_t)lroundf(h->s[k].v[i] * 10.0f));
        }
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

    /* "Now 16.1 °C    Low 10.5 at 04:43    High 23.6 at 13:47" per line, in its colour. */
    for (int k = 0; k < h->count; k++) {
        const ha_series_t *sr = &h->s[k];
        char lo_t[8], hi_t[8], line[160];
        hhmm(t_end - (time_t)(HA_HISTORY_POINTS - 1 - sr->min_i) * h->step_s, lo_t, sizeof(lo_t));
        hhmm(t_end - (time_t)(HA_HISTORY_POINTS - 1 - sr->max_i) * h->step_s, hi_t, sizeof(hi_t));
        snprintf(line, sizeof(line), "Now %.1f%s%s    Low %.1f at %s    High %.1f at %s",
                 (double)sr->v[HA_HISTORY_POINTS - 1], sr->unit[0] ? " " : "", sr->unit,
                 (double)sr->min, lo_t, (double)sr->max, hi_t);
        lv_obj_t *sum = add_label(list, &gcal_font_14, SERIES_COLOR[sr->slot], line);
        lv_obj_set_width(sum, LV_PCT(100));
    }
    ha_history_release();
}

/* ---- Wind: 24-hour speed and gust lines with an arrow for each hour ---- */
#define WIND_CHART_H   130
#define WIND_ARROWS_H  34
#define WIND_GUST_COLOR 0xF6AD55
#define WIND_STRONG     12.0f     /* an hour at or above this (km/h) is drawn in the gust colour */

static struct {
    float dir;        /* degrees the wind comes FROM */
    bool calm;
    uint32_t color;
} s_arrows[HA_WIND_HOURS];

/* One arrow per hour, evenly spread over the object's width (the same width
 * as the chart below it, so each sits over its hour). The arrow points the
 * way the wind is blowing, i.e. opposite to the bearing it comes from; a calm
 * hour is a small dot. */
static void arrows_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    float len = (float)(h / 2 - 3);
    for (int i = 0; i < HA_WIND_HOURS; i++) {
        float cx = (float)a.x1 + (float)w * (2 * i + 1) / (2.0f * HA_WIND_HOURS);
        float cy = (float)a.y1 + (float)h / 2.0f;
        lv_draw_line_dsc_t ln;
        lv_draw_line_dsc_init(&ln);
        ln.color = lv_color_hex(s_arrows[i].color);
        ln.opa = LV_OPA_COVER;
        ln.round_start = ln.round_end = 1;
        if (s_arrows[i].calm) {
            ln.width = 3;
            lv_point_t p = { (lv_coord_t)cx, (lv_coord_t)cy };
            lv_draw_line(ctx, &ln, &p, &p);
            continue;
        }
        ln.width = 2;
        float to = (s_arrows[i].dir + 180.0f) * (float)M_PI / 180.0f;
        float dx = sinf(to) * len, dy = -cosf(to) * len;
        lv_point_t tail = { (lv_coord_t)lroundf(cx - dx), (lv_coord_t)lroundf(cy - dy) };
        lv_point_t head = { (lv_coord_t)lroundf(cx + dx), (lv_coord_t)lroundf(cy + dy) };
        lv_draw_line(ctx, &ln, &tail, &head);
        for (int side = -1; side <= 1; side += 2) {
            float ang = to + (float)side * 150.0f * (float)M_PI / 180.0f;
            lv_point_t barb = { (lv_coord_t)lroundf((float)head.x + sinf(ang) * 6.0f),
                                (lv_coord_t)lroundf((float)head.y - cosf(ang) * 6.0f) };
            lv_draw_line(ctx, &ln, &head, &barb);
        }
    }
}

static void add_wind_section(lv_obj_t *list)
{
    const ha_wind_t *w = ha_wind_acquire();
    if (w == NULL) {
        ha_wind_release();
        return;
    }
    const char *unit = w->unit[0] ? w->unit : "km/h";

    /* Y range: from 0, to the highest gust (or speed) in whole nice steps. */
    float top = 5.0f;
    for (int i = 0; i < HA_WIND_STEPS; i++) {
        float v = (w->gust_ok && !isnan(w->gust[i])) ? w->gust[i] : w->speed[i];
        if (!isnan(v) && v > top) {
            top = v;
        }
    }
    float step = 1.0f;
    static const float steps[] = { 1, 2, 5, 10, 20, 50 };
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        step = steps[s];
        if (top / step <= 8.0f) {
            break;
        }
    }
    float hi = ceilf(top / step) * step;
    int intervals = (int)lroundf(hi / step);

    lv_obj_t *gap = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(gap, 10);
    char title[64];
    snprintf(title, sizeof(title), "Wind \xE2\x80\x93 last 24 hours (%s)", unit);
    lv_obj_t *heading = add_label(list, &gcal_font_20, HEAD_COLOR, title);
    lv_obj_set_width(heading, LV_PCT(100));

    lv_obj_t *legend = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(legend, 24, 0);
    add_label(legend, &gcal_font_14, ICON_COLOR, "\xE2\x80\x94\xE2\x80\x94 speed");
    if (w->gust_ok) {
        add_label(legend, &gcal_font_14, WIND_GUST_COLOR, "\xE2\x80\x94\xE2\x80\x94 gust");
    }
    if (w->dir_ok) {
        add_label(legend, &gcal_font_14, DIM_COLOR, "arrows: the way the wind blows, hourly");
    }

    const lv_coord_t chart_w = LV_HOR_RES - 64 - GRAPH_AXIS_W - 6;
    const lv_coord_t arrows_h = w->dir_ok ? WIND_ARROWS_H : 0;
    lv_obj_t *row = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 6, 0);

    /* Y labels, one per gridline, centred on the chart's lines (below the arrows). */
    lv_obj_t *axis = lv_obj_create(row);
    lv_obj_remove_style_all(axis);
    lv_obj_set_size(axis, GRAPH_AXIS_W, arrows_h + WIND_CHART_H);
    lv_obj_clear_flag(axis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(axis, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int t = 0; t <= intervals; t++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%g", (double)(hi - t * step));
        lv_obj_t *l = add_label(axis, &gcal_font_14, DIM_COLOR, buf);
        lv_obj_set_width(l, GRAPH_AXIS_W);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_coord_t y = arrows_h + (lv_coord_t)((WIND_CHART_H - 1) * t / intervals) - 9;
        if (y < arrows_h) {
            y = arrows_h;
        }
        if (y > arrows_h + WIND_CHART_H - 18) {
            y = arrows_h + WIND_CHART_H - 18;
        }
        lv_obj_set_pos(l, 0, y);
    }

    lv_obj_t *col = add_container(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(col, chart_w);
    lv_obj_set_style_pad_row(col, 0, 0);
    if (w->dir_ok) {
        for (int i = 0; i < HA_WIND_HOURS; i++) {
            s_arrows[i].dir = w->hr_dir[i];
            s_arrows[i].calm = w->hr_calm[i];
            s_arrows[i].color = (!isnan(w->hr_speed[i]) && w->hr_speed[i] >= WIND_STRONG) ? WIND_GUST_COLOR : ICON_COLOR;
        }
        lv_obj_t *arrows = lv_obj_create(col);
        lv_obj_remove_style_all(arrows);
        lv_obj_set_size(arrows, chart_w, WIND_ARROWS_H);
        lv_obj_clear_flag(arrows, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(arrows, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(arrows, arrows_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    }
    lv_obj_t *chart = lv_chart_create(col);
    lv_obj_set_size(chart, chart_w, WIND_CHART_H);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, HA_WIND_STEPS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)(hi * 10));
    lv_chart_set_div_line_count(chart, intervals - 1, 5);       /* a line per label; one every 4 hours */
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x3C4043), 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2C2E), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (w->gust_ok) {
        lv_chart_series_t *g = lv_chart_add_series(chart, lv_color_hex(WIND_GUST_COLOR), LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < HA_WIND_STEPS; i++) {
            lv_chart_set_value_by_id(chart, g, i, isnan(w->gust[i]) ? LV_CHART_POINT_NONE
                                                    : (lv_coord_t)lroundf(w->gust[i] * 10.0f));
        }
    }
    lv_chart_series_t *sp = lv_chart_add_series(chart, lv_color_hex(ICON_COLOR), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < HA_WIND_STEPS; i++) {
        lv_chart_set_value_by_id(chart, sp, i, isnan(w->speed[i]) ? LV_CHART_POINT_NONE
                                                : (lv_coord_t)lroundf(w->speed[i] * 10.0f));
    }
    lv_chart_refresh(chart);

    /* Times under the chart: start, middle, end. */
    time_t t_end = w->end, t_start = t_end - (time_t)HA_WIND_STEPS * 900;
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

    /* "Now 9.7 km/h from NNE    Max gust 16.6 at 14:26    Mostly from NE" */
    char gt[8], line[160];
    hhmm(t_start + (time_t)w->max_gust_i * 900 + 450, gt, sizeof(gt));
    int n = snprintf(line, sizeof(line), "Now %.1f %s", (double)w->now_speed, unit);
    if (w->now_dir >= 0 && n < (int)sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - n, " from %s", wind_compass16(w->now_dir));
    }
    if (w->max_gust > 0 && n < (int)sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - n, "    Max %s %.1f at %s", w->gust_ok ? "gust" : "speed",
                      (double)w->max_gust, gt);
    }
    if (w->prevailing >= 0 && n < (int)sizeof(line)) {
        snprintf(line + n, sizeof(line) - n, "    Mostly from %s", wind_compass16(w->prevailing));
    }
    lv_obj_t *sum = add_label(list, &gcal_font_14, TEXT_COLOR, line);
    lv_obj_set_width(sum, LV_PCT(100));
    ha_wind_release();
}

/* ---- 14-day bar graphs: daily low / high temperature, daily rainfall ---- */
#define BAR_H 220
#define TEMP_HI_COLOR 0xF6AD55
#define TEMP_LO_COLOR 0x8AB4F8
#define RAIN_COLOR    0x5BC0EB

/* One section: heading, Y labels + bar chart, and under it a label per day
 * (weekday, then the value(s) in the bars' colours - recolor syntax). */
static void bar_section(lv_obj_t *list, const char *title, const ha_week_t *w, int nser,
                        const float *const vals[], const uint32_t colors[], float lo, float hi,
                        bool temp)
{
    lv_obj_t *gap = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(gap, 10);
    lv_obj_t *heading = add_label(list, &gcal_font_20, HEAD_COLOR, title);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);

    /* A "nice" tick step (1, 2, 5, 10 ...) that gives about 5-10 intervals, then
     * the range widened to whole steps so every gridline has a label. */
    float step = 1.0f;
    {
        static const float steps[] = { 1, 2, 5, 10, 20, 50, 100 };
        for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
            step = steps[s];
            if ((hi - lo) / step <= 10.0f) {
                break;
            }
        }
        lo = floorf(lo / step) * step;
        hi = ceilf(hi / step) * step;
        if (hi <= lo) {
            hi = lo + step;
        }
    }
    int intervals = (int)lroundf((hi - lo) / step);

    const lv_coord_t chart_w = LV_HOR_RES - 64 - GRAPH_AXIS_W - 6;
    lv_obj_t *row = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 6, 0);

    /* Y scale: a label at every gridline, each centred on its line. */
    lv_obj_t *axis = lv_obj_create(row);
    lv_obj_remove_style_all(axis);
    lv_obj_set_size(axis, GRAPH_AXIS_W, BAR_H);
    lv_obj_clear_flag(axis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(axis, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int t = 0; t <= intervals; t++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%g", (double)(hi - t * step));
        lv_obj_t *l = add_label(axis, &gcal_font_14, DIM_COLOR, buf);
        lv_obj_set_width(l, GRAPH_AXIS_W);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_coord_t y = (lv_coord_t)((BAR_H - 1) * t / intervals) - 9;     /* 18 px label, centred */
        if (y < 0) {
            y = 0;
        }
        if (y > BAR_H - 18) {
            y = BAR_H - 18;
        }
        lv_obj_set_pos(l, 0, y);
    }

    lv_obj_t *chart = lv_chart_create(row);
    lv_obj_set_size(chart, chart_w, BAR_H);
    lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(chart, HA_WEEK_DAYS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(lo * 10), (lv_coord_t)(hi * 10));
    lv_chart_set_div_line_count(chart, intervals - 1, 0);      /* a line at every labelled step */
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x3C4043), 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_pad_column(chart, 5, LV_PART_MAIN);      /* gap between days */
    lv_obj_set_style_pad_column(chart, 2, LV_PART_ITEMS);     /* gap between a day's bars */
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2C2E), LV_PART_MAIN);
    lv_obj_set_style_radius(chart, 0, LV_PART_ITEMS);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int k = 0; k < nser; k++) {
        lv_chart_series_t *ser = lv_chart_add_series(chart, lv_color_hex(colors[k]), LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < HA_WEEK_DAYS; i++) {
            bool have = temp ? w->t_ok[i] : w->r_day_ok[i];
            lv_chart_set_value_by_id(chart, ser, i,
                                     have ? (lv_coord_t)lroundf(vals[k][i] * 10.0f) : LV_CHART_POINT_NONE);
        }
    }
    lv_chart_refresh(chart);

    /* Weekday and value labels, one slot per day, lined up under the bars. */
    lv_obj_t *days = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_left(days, GRAPH_AXIS_W + 6, 0);
    lv_obj_set_style_pad_column(days, 0, 0);
    lv_obj_set_flex_align(days, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (int i = 0; i < HA_WEEK_DAYS; i++) {
        char name[16], text[112];
        if (i == HA_WEEK_DAYS - 1) {
            snprintf(name, sizeof(name), "Now");
        } else {
            struct tm lt;
            localtime_r(&w->day[i], &lt);
            char wd[8];
            strftime(wd, sizeof(wd), "%a", &lt);
            /* 14 days repeat the weekday names, so the date goes under them. */
            snprintf(name, sizeof(name), "%.1s\n%d", wd, lt.tm_mday);
        }
        if (temp) {
            if (w->t_ok[i]) {
                snprintf(text, sizeof(text), "%s\n#%06X %.1f#\n#%06X %.1f#", name, TEMP_HI_COLOR,
                         (double)w->tmax[i], TEMP_LO_COLOR, (double)w->tmin[i]);
            } else {
                snprintf(text, sizeof(text), "%s\n-", name);
            }
        } else if (w->r_day_ok[i]) {
            snprintf(text, sizeof(text), "%s\n#%06X %.1f#", name, RAIN_COLOR, (double)w->rain[i]);
        } else {
            snprintf(text, sizeof(text), "%s\n-", name);
        }
        lv_obj_t *l = add_label(days, &gcal_font_14, DIM_COLOR, text);
        lv_label_set_recolor(l, true);
        lv_obj_set_width(l, chart_w / HA_WEEK_DAYS);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void add_week_graphs(lv_obj_t *list)
{
    const ha_week_t *w = ha_week_acquire();
    if (w == NULL) {
        ha_week_release();
        return;
    }
    if (w->t_have > 0) {
        float lo = 0.0f, hi = -1000.0f, mn = 1000.0f;
        for (int i = 0; i < HA_WEEK_DAYS; i++) {
            if (w->t_ok[i]) {
                hi = fmaxf(hi, w->tmax[i]);
                mn = fminf(mn, w->tmin[i]);
            }
        }
        lo = fminf(0.0f, floorf(mn));
        hi = ceilf(hi);
        if (hi - lo < 2.0f) {
            hi = lo + 2.0f;
        }
        const float *vals[2] = { w->tmax, w->tmin };
        const uint32_t colors[2] = { TEMP_HI_COLOR, TEMP_LO_COLOR };
        char title[64];
        snprintf(title, sizeof(title), "Last 14 days \xE2\x80\x93 daily high and low%s%s",
                 w->t_unit[0] ? " " : "", w->t_unit);
        bar_section(list, title, w, 2, vals, colors, lo, hi, true);
    }
    if (w->r_ok) {
        float hi = 2.0f;
        for (int i = 0; i < HA_WEEK_DAYS; i++) {
            hi = fmaxf(hi, ceilf(w->rain[i]));
        }
        const float *vals[1] = { w->rain };
        const uint32_t colors[1] = { RAIN_COLOR };
        char title[64];
        snprintf(title, sizeof(title), "Last 14 days \xE2\x80\x93 daily rainfall %s", w->r_unit[0] ? w->r_unit : "mm");
        bar_section(list, title, w, 1, vals, colors, 0.0f, hi, false);
    }
    ha_week_release();
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

    /* The history graphs below couldn't be fully loaded last time (a failed or
     * cut-off reply) - what's shown is older data or missing a graph. */
    if (ha_graphs_incomplete()) {
        lv_obj_t *warn = add_label(s_overlay, &gcal_font_20, 0xF9AB00, LV_SYMBOL_WARNING);
        lv_obj_align(warn, LV_ALIGN_TOP_RIGHT, -32, 20);
    }

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
    add_wind_section(list);
    add_week_graphs(list);

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
