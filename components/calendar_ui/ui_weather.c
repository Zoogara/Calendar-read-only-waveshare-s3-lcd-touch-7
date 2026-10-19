/* Weather details panel - opened by tapping the weather on the ambient clock
 * (ui_clock.c), filled from components/weather (Open-Meteo: current
 * readings plus a 7-day forecast).
 *
 * A full-screen scrolling overlay on
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
#include "weather.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
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
    /* See ui_lock.c: a full-screen lv_layer_top() object
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
static void min_max(const weather_day_t *d, char *out, size_t sz)
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

/* "40%, 3.0 mm" (the amount always shown with the chance), "40%" if
 * there's no amount, or "". prefix goes in front when non-empty. */
static void rain_text(const weather_day_t *d, const char *prefix, char *out, size_t sz)
{
    out[0] = '\0';
    if (d->rain_chance[0]) {
        int n = snprintf(out, sz, "%s%s%%", prefix, d->rain_chance);
        if (d->rain_amount[0] && n > 0 && (size_t)n < sz) {
            snprintf(out + n, sz - n, ", %s mm", d->rain_amount);
        }
    }
}

static void add_now(lv_obj_t *list, const weather_details_t *d)
{
    char line[160];
    int n = 0;
    /* From the details, not weather_get(): that takes the same lock this
     * panel is already holding (see weather_details_acquire()). */
    if (d->temp.value[0]) {
        n = snprintf(line, sizeof(line), "%s%s", d->temp.value, d->temp.unit);
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
static void add_big_day(lv_obj_t *list, const weather_day_t *d, const char *name)
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
    snprintf(line, sizeof(line), "%s%s%s%s%s", name, mm[0] ? "   " : "", mm, rain[0] ? "   " : "", rain);
    add_label(col, &gcal_font_20, HEAD_COLOR, line);
    add_wrapping_label(col, &gcal_font_14, TEXT_COLOR, d->extended[0] ? d->extended : d->short_text);
}

/* Days 2-6: one line each. */
static void add_small_day(lv_obj_t *list, const weather_day_t *d, const char *name)
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

#define GRAPH_H        220            /* plot height - the same as BAR_H below */
/* The blank space above each section's heading: about half a line of the 20 px heading text. */
#define SECTION_GAP    22
#define GRAPH_AXIS_W   31            /* the Y numbers: up to "27.5" in 14px text */

static const uint32_t SERIES_COLOR[WEATHER_HISTORY_SERIES] = { 0x8AB4F8 };   /* blue */

static const char *wind_compass16(float deg)
{
    static const char *N16[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                  "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    float d = fmodf(deg, 360.0f);
    if (d < 0) {
        d += 360.0f;
    }
    return N16[((int)floorf((d + 11.25f) / 22.5f)) % 16];
}

static void hhmm(time_t t, char *out, size_t sz)
{
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(out, sz, "%H:%M", &lt);
}

/* ---- Chart text drawn inside the chart, not as label objects ----
 * Each label object costs a few hundred bytes of INTERNAL RAM, and the graphs
 * below had ~70 of them (axis numbers, times, a label per day). With the panel
 * open that was ~30KB of the ~40KB free. Instead each chart leaves room in its
 * own padding (left for the Y numbers, bottom for times / day labels, top for
 * the wind arrows) and one draw callback paints all the text from a small
 * PSRAM table - same look, a fraction of the RAM. */
#define CHART_PAD_B     20            /* under a line chart: the time labels */
#define WIND_ARROWS_H   34            /* above the wind chart: the hourly arrows */
#define WIND_GUST_COLOR 0xF6AD55
#define WIND_STRONG     12.0f         /* an hour at or above this (km/h) is drawn in the gust colour */

typedef struct {
    int ny;
    char ylab[12][16];                 /* Y numbers, top to bottom, on the gridlines */
    int nx;
    char xlab[3][8];                  /* under the plot: left, middle, right */
    int ndays, day_lines;             /* per-day text under a bar chart */
    char day[WEATHER_WEEK_DAYS][4][14];
    uint32_t day_col[4];              /* colour of each line of the day text */
    bool arrows;                      /* draw the wind arrows in the top padding */
} chart_decor_t;

static struct {
    float dir;                        /* degrees the wind comes FROM */
    bool calm;
    uint32_t color;
} s_arrows[WEATHER_WIND_HOURS];

/* One arrow per hour, evenly spread over `a` (the same width as the plot below,
 * so each sits over its hour). It points the way the wind is blowing, i.e.
 * opposite to the bearing it comes from; a calm hour is a small dot. */
static void draw_wind_arrows(lv_draw_ctx_t *ctx, const lv_area_t *a)
{
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    float len = (float)(h / 2 - 3);
    for (int i = 0; i < WEATHER_WIND_HOURS; i++) {
        float cx = (float)a->x1 + (float)w * (2 * i + 1) / (2.0f * WEATHER_WIND_HOURS);
        float cy = (float)a->y1 + (float)h / 2.0f;
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

static void chart_decor_cb(lv_event_t *e)
{
    lv_obj_t *ch = lv_event_get_target(e);
    const chart_decor_t *d = lv_event_get_user_data(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t obj, plot;
    lv_obj_get_coords(ch, &obj);
    lv_obj_get_content_coords(ch, &plot);          /* the plot area inside the padding */
    int pw = lv_area_get_width(&plot), ph = lv_area_get_height(&plot);

    /* LVGL runs its gridlines right across the chart's padding, where the text
     * goes: paint the padding strips black first, then frame the plot ourselves. */
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_hex(BG_COLOR);
    bg.bg_opa = LV_OPA_COVER;
    bg.border_width = 0;
    lv_area_t m = { obj.x1, obj.y1, plot.x1 - 1, obj.y2 };
    lv_draw_rect(ctx, &bg, &m);
    if (plot.y2 < obj.y2) {
        m = (lv_area_t){ plot.x1, plot.y2 + 1, obj.x2, obj.y2 };
        lv_draw_rect(ctx, &bg, &m);
    }
    if (plot.y1 > obj.y1) {
        m = (lv_area_t){ plot.x1, obj.y1, obj.x2, plot.y1 - 1 };
        lv_draw_rect(ctx, &bg, &m);
    }
    lv_draw_rect_dsc_t fr;
    lv_draw_rect_dsc_init(&fr);
    fr.bg_opa = LV_OPA_TRANSP;
    fr.border_color = lv_color_hex(0x3C4043);
    fr.border_width = 1;
    fr.border_opa = LV_OPA_COVER;
    lv_draw_rect(ctx, &fr, &plot);

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &gcal_font_14;
    ld.color = lv_color_hex(DIM_COLOR);
    lv_area_t r;

    ld.align = LV_TEXT_ALIGN_RIGHT;                 /* Y numbers, centred on their gridlines */
    for (int i = 0; i < d->ny; i++) {
        lv_coord_t yc = (lv_coord_t)(plot.y1 + (d->ny > 1 ? ph * i / (d->ny - 1) : 0));
        r = (lv_area_t){ obj.x1, yc - 9, plot.x1 - 6, yc + 9 };
        lv_draw_label(ctx, &ld, &r, d->ylab[i], NULL);
    }
    for (int i = 0; i < d->nx; i++) {              /* times under the plot */
        ld.align = i == 0 ? LV_TEXT_ALIGN_LEFT : i == d->nx - 1 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
        r = (lv_area_t){ plot.x1, plot.y2 + 3, plot.x2, plot.y2 + 21 };
        lv_draw_label(ctx, &ld, &r, d->xlab[i], NULL);
    }
    if (d->ndays > 0) {                             /* a few lines of text under each bar */
        ld.align = LV_TEXT_ALIGN_CENTER;
        int cw = pw / d->ndays;
        for (int i = 0; i < d->ndays; i++) {
            for (int l = 0; l < d->day_lines; l++) {
                if (d->day[i][l][0] == '\0') {
                    continue;
                }
                ld.color = lv_color_hex(d->day_col[l]);
                r = (lv_area_t){ plot.x1 + i * cw, plot.y2 + 4 + l * 15, plot.x1 + (i + 1) * cw - 1, plot.y2 + 4 + l * 15 + 16 };
                lv_draw_label(ctx, &ld, &r, d->day[i][l], NULL);
            }
        }
    }
    if (d->arrows) {
        lv_area_t top = { plot.x1, obj.y1, plot.x2, plot.y1 - 1 };
        draw_wind_arrows(ctx, &top);
    }
}

static void chart_decor_free_cb(lv_event_t *e)
{
    heap_caps_free(lv_event_get_user_data(e));
}

/* A round tick step (0.5, 1, 2, 5, 10 ...) giving no more intervals than leave
 * the Y labels at least 24 px apart on a plot `plot_h` tall, and the range
 * widened to whole steps so every gridline has a label and every label a line. */
static void nice_scale(float *lo, float *hi, int plot_h, float *step, int *intervals)
{
    int max_intervals = plot_h / 24;
    if (max_intervals < 2) {
        max_intervals = 2;
    }
    static const float steps[] = { 0.5f, 1, 2, 5, 10, 20, 50, 100 };
    float st = steps[0];
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        st = steps[i];
        if ((*hi - *lo) / st <= (float)max_intervals) {
            break;
        }
    }
    *lo = floorf(*lo / st) * st;
    *hi = ceilf(*hi / st) * st;
    if (*hi <= *lo) {
        *hi = *lo + st;
    }
    *step = st;
    *intervals = (int)lroundf((*hi - *lo) / st);
}

/* A chart the width of the list with room in its padding for the text above. */
static lv_obj_t *new_chart(lv_obj_t *list, lv_coord_t total_h, lv_coord_t pad_top, lv_coord_t pad_bottom,
                           chart_decor_t **decor)
{
    /* The top Y label is centred on the plot's top edge, so half of it sits above the
     * plot: leave it room (found by the host renderer, tools/screenshots - it was being
     * clipped at the chart's top edge). The wind chart's arrow band already provides it. */
    if (pad_top < 10) {
        total_h += 10 - pad_top;
        pad_top = 10;
    }
    lv_obj_t *c = lv_chart_create(list);
    lv_obj_set_size(c, LV_HOR_RES - 64, total_h);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_pad_left(c, GRAPH_AXIS_W + 6, 0);
    lv_obj_set_style_pad_top(c, pad_top, 0);
    lv_obj_set_style_pad_bottom(c, pad_bottom, 0);
    chart_decor_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_SPIRAM);
    if (d != NULL) {
        lv_obj_add_event_cb(c, chart_decor_cb, LV_EVENT_DRAW_MAIN_END, d);
        lv_obj_add_event_cb(c, chart_decor_free_cb, LV_EVENT_DELETE, d);
    }
    *decor = d;
    return c;
}

static void add_history_graph(lv_obj_t *list)
{
    const weather_history_t *h = weather_history_acquire();
    if (h == NULL || h->count < 1) {
        weather_history_release();
        return;
    }

    /* One Y range for every line: whole units around all the data, never flatter than 2. */
    float mn = h->s[0].min, mx = h->s[0].max;
    for (int k = 1; k < h->count; k++) {
        mn = fminf(mn, h->s[k].min);
        mx = fmaxf(mx, h->s[k].max);
    }
    float lo = mn, hi = mx, step;
    int intervals;
    nice_scale(&lo, &hi, GRAPH_H, &step, &intervals);

    lv_obj_t *gap = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(gap, SECTION_GAP);
    lv_obj_t *heading = add_label(list, &gcal_font_20, HEAD_COLOR, "Last 24 hours");
    lv_obj_set_width(heading, LV_PCT(100));

    /* Legend: a coloured bar and the sensor's name, one per line on the graph. */
    lv_obj_t *legend = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(legend, 24, 0);
    for (int k = 0; k < h->count; k++) {
        const weather_series_t *sr = &h->s[k];
        char key[80];
        snprintf(key, sizeof(key), "\xE2\x80\x94\xE2\x80\x94 %s", sr->name);
        add_label(legend, &gcal_font_14, SERIES_COLOR[sr->slot], key);
    }

    chart_decor_t *dec;
    lv_obj_t *chart = new_chart(list, GRAPH_H + CHART_PAD_B, 0, CHART_PAD_B, &dec);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, WEATHER_HISTORY_POINTS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(lo * 10), (lv_coord_t)(hi * 10));
    lv_chart_set_div_line_count(chart, intervals - 1, 7);    /* a line per labelled step; one every 4 hours */
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x3C4043), 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2C2E), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);   /* plain lines, no dots */
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);      /* taps and drags go to the list */
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int k = 0; k < h->count; k++) {
        lv_chart_series_t *ser = lv_chart_add_series(chart, lv_color_hex(SERIES_COLOR[h->s[k].slot]),
                                                     LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < WEATHER_HISTORY_POINTS; i++) {
            lv_chart_set_value_by_id(chart, ser, i, isnan(h->s[k].v[i]) ? LV_CHART_POINT_NONE
                                                  : (lv_coord_t)lroundf(h->s[k].v[i] * 10.0f));
        }
    }
    lv_chart_refresh(chart);

    time_t t_end = h->end;
    time_t t_start = t_end - (time_t)(WEATHER_HISTORY_POINTS - 1) * h->step_s;
    if (dec != NULL) {
        for (int t = 0; t <= intervals && t < 12; t++) {
            snprintf(dec->ylab[t], sizeof(dec->ylab[t]), "%g", (double)(hi - t * step));
        }
        dec->ny = intervals + 1 > 12 ? 12 : intervals + 1;
        hhmm(t_start, dec->xlab[0], sizeof(dec->xlab[0]));
        hhmm((t_start + t_end) / 2, dec->xlab[1], sizeof(dec->xlab[1]));
        hhmm(t_end, dec->xlab[2], sizeof(dec->xlab[2]));
        dec->nx = 3;
    }

    /* "Now 16.1 °C    Low 10.5 at 04:43    High 23.6 at 13:47" per line, in its colour. */
    for (int k = 0; k < h->count; k++) {
        const weather_series_t *sr = &h->s[k];
        char lo_t[8], hi_t[8], line[160];
        hhmm(t_end - (time_t)(WEATHER_HISTORY_POINTS - 1 - sr->min_i) * h->step_s, lo_t, sizeof(lo_t));
        hhmm(t_end - (time_t)(WEATHER_HISTORY_POINTS - 1 - sr->max_i) * h->step_s, hi_t, sizeof(hi_t));
        snprintf(line, sizeof(line), "Now %.1f%s%s    Low %.1f at %s    High %.1f at %s",
                 (double)sr->v[WEATHER_HISTORY_POINTS - 1], sr->unit[0] ? " " : "", sr->unit,
                 (double)sr->min, lo_t, (double)sr->max, hi_t);
        lv_obj_t *sum = add_label(list, &gcal_font_14, SERIES_COLOR[sr->slot], line);
        lv_obj_set_width(sum, LV_PCT(100));
    }
    weather_history_release();
}

/* ---- Hourly forecast: a horizontally scrolling strip, one cell per hour ----
 * Each cell: the time, the condition icon (as wide as the cell), the forecast
 * temperature and the chance and amount of rain. Drawn by ONE object - a wide
 * child of a horizontally scrolling row - rather than 24 x 5 labels: with the
 * panel open internal RAM is tight and ~120 label objects would take most of
 * what is left. The strings are formatted once when the panel opens. */
#define HOURLY_CELL_W 64
#define HOURLY_H      138
#define RAIN_COLOR_HR 0x5BC0EB

static struct {
    char time[16], temp[16], pct[12], mm[16];
    const char *glyph;
    bool now, day_start;
} s_hr[WEATHER_HOURLY_MAX];
static int s_hr_n;

/* weather_hourly_t already carries the icon conditions ui_weather_glyph() knows. */
static const char *hourly_descriptor(const char *c)
{
    return c;
}

static void hourly_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    for (int i = 0; i < s_hr_n; i++) {
        lv_area_t cell = { a.x1 + i * HOURLY_CELL_W, a.y1, a.x1 + (i + 1) * HOURLY_CELL_W - 2, a.y2 };
        lv_area_t vis;
        if (!_lv_area_intersect(&vis, &cell, ctx->clip_area)) {
            continue;                             /* off screen in the scrolled row */
        }
        if (s_hr[i].now) {
            lv_draw_rect_dsc_t bg;
            lv_draw_rect_dsc_init(&bg);
            bg.bg_color = lv_color_hex(0x1B2A3F);
            bg.bg_opa = LV_OPA_COVER;
            bg.radius = 8;
            lv_draw_rect(ctx, &bg, &cell);
        }
        if (s_hr[i].day_start) {                  /* a faint line where a new day begins */
            lv_draw_line_dsc_t ln;
            lv_draw_line_dsc_init(&ln);
            ln.color = lv_color_hex(0x3C4043);
            ln.width = 1;
            lv_point_t p1 = { cell.x1 - 1, cell.y1 + 6 }, p2 = { cell.x1 - 1, cell.y2 - 6 };
            lv_draw_line(ctx, &ln, &p1, &p2);
        }
        lv_draw_label_dsc_t d;
        lv_area_t r;
        /* time */
        lv_draw_label_dsc_init(&d);
        d.font = &gcal_font_14;
        d.color = lv_color_hex(s_hr[i].now ? HEAD_COLOR : DIM_COLOR);
        d.align = LV_TEXT_ALIGN_CENTER;
        r = (lv_area_t){ cell.x1, cell.y1 + 4, cell.x2, cell.y1 + 22 };
        lv_draw_label(ctx, &d, &r, s_hr[i].time, NULL);
        /* icon, filling the cell width */
        d.font = &gcal_font_weather_48;
        d.color = lv_color_hex(ICON_COLOR);
        r = (lv_area_t){ cell.x1, cell.y1 + 26, cell.x2, cell.y1 + 78 };
        lv_draw_label(ctx, &d, &r, s_hr[i].glyph, NULL);
        /* temperature */
        d.font = &gcal_font_20;
        d.color = lv_color_hex(HEAD_COLOR);
        r = (lv_area_t){ cell.x1, cell.y1 + 82, cell.x2, cell.y1 + 104 };
        lv_draw_label(ctx, &d, &r, s_hr[i].temp, NULL);
        /* chance of rain, then the amount when there is some */
        d.font = &gcal_font_14;
        d.color = lv_color_hex(RAIN_COLOR_HR);
        r = (lv_area_t){ cell.x1, cell.y1 + 106, cell.x2, cell.y1 + 122 };
        lv_draw_label(ctx, &d, &r, s_hr[i].pct, NULL);
        if (s_hr[i].mm[0]) {
            d.color = lv_color_hex(TEXT_COLOR);
            r = (lv_area_t){ cell.x1, cell.y1 + 121, cell.x2, cell.y1 + 137 };
            lv_draw_label(ctx, &d, &r, s_hr[i].mm, NULL);
        }
    }
}

static void add_hourly_strip(lv_obj_t *list)
{
    const weather_hourly_t *h = weather_hourly_acquire();
    if (h == NULL || h->count == 0) {
        weather_hourly_release();
        return;
    }
    s_hr_n = h->count;
    int prev_day = -1;
    for (int i = 0; i < h->count; i++) {
        struct tm lt;
        localtime_r(&h->t[i], &lt);
        int h12 = lt.tm_hour % 12 == 0 ? 12 : lt.tm_hour % 12;
        if (i == 0) {
            snprintf(s_hr[i].time, sizeof(s_hr[i].time), "Now");
        } else {
            snprintf(s_hr[i].time, sizeof(s_hr[i].time), "%d %s", h12, lt.tm_hour < 12 ? "AM" : "PM");
        }
        if (isnan(h->temp[i])) {
            snprintf(s_hr[i].temp, sizeof(s_hr[i].temp), "-");
        } else {
            snprintf(s_hr[i].temp, sizeof(s_hr[i].temp), "%.0f" DEG, (double)h->temp[i]);
        }
        if (h->rain_pct[i] >= 0) {
            snprintf(s_hr[i].pct, sizeof(s_hr[i].pct), "%d%%", h->rain_pct[i]);
        } else {
            s_hr[i].pct[0] = '\0';
        }
        if (h->rain_mm[i] >= 0.1f) {
            snprintf(s_hr[i].mm, sizeof(s_hr[i].mm), "%.1f mm", (double)h->rain_mm[i]);
        } else {
            s_hr[i].mm[0] = '\0';
        }
        bool night = h->night[i];
        s_hr[i].glyph = ui_weather_glyph(hourly_descriptor(h->cond[i]), night);
        s_hr[i].now = (i == 0);
        s_hr[i].day_start = (i > 0 && lt.tm_yday != prev_day);
        prev_day = lt.tm_yday;
    }
    weather_hourly_release();

    lv_obj_t *gap = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(gap, SECTION_GAP);
    lv_obj_t *heading = add_label(list, &gcal_font_20, HEAD_COLOR, "Next 24 hours");
    lv_obj_set_width(heading, LV_PCT(100));

    /* The scrolling row; its one wide child is the drawn strip. Horizontal
     * drags scroll it, vertical ones fall through to the panel's list. */
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), HOURLY_H);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_t *strip = lv_obj_create(row);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, HOURLY_CELL_W * s_hr_n, HOURLY_H);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(strip, hourly_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
}

/* ---- Wind: 24-hour speed and gust lines with an arrow for each hour ---- */
#define WIND_CHART_H   220            /* plot height - the same as BAR_H */

static void add_wind_section(lv_obj_t *list)
{
    const weather_wind_t *w = weather_wind_acquire();
    if (w == NULL) {
        weather_wind_release();
        return;
    }
    const char *unit = w->unit[0] ? w->unit : "km/h";

    /* Y range: from 0, to the highest gust (or speed) in whole nice steps. */
    float top = 5.0f;
    for (int i = 0; i < WEATHER_WIND_STEPS; i++) {
        float v = (w->gust_ok && !isnan(w->gust[i])) ? w->gust[i] : w->speed[i];
        if (!isnan(v) && v > top) {
            top = v;
        }
    }
    float lo = 0.0f, hi = top, step;
    int intervals;
    nice_scale(&lo, &hi, WIND_CHART_H, &step, &intervals);

    lv_obj_t *gap = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(gap, SECTION_GAP);
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
        add_label(legend, &gcal_font_14, DIM_COLOR, "Wind direction, hourly");
    }

    const lv_coord_t arrows_h = w->dir_ok ? WIND_ARROWS_H : 0;
    chart_decor_t *dec;
    lv_obj_t *chart = new_chart(list, arrows_h + WIND_CHART_H + CHART_PAD_B, arrows_h, CHART_PAD_B, &dec);
    if (w->dir_ok) {
        for (int i = 0; i < WEATHER_WIND_HOURS; i++) {
            s_arrows[i].dir = w->hr_dir[i];
            s_arrows[i].calm = w->hr_calm[i];
            s_arrows[i].color = (!isnan(w->hr_speed[i]) && w->hr_speed[i] >= WIND_STRONG) ? WIND_GUST_COLOR : ICON_COLOR;
        }
    }
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, WEATHER_WIND_STEPS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)(hi * 10));
    lv_chart_set_div_line_count(chart, intervals - 1, 5);       /* a line per label; one every 4 hours */
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x3C4043), 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2C2E), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (w->gust_ok) {
        lv_chart_series_t *g = lv_chart_add_series(chart, lv_color_hex(WIND_GUST_COLOR), LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < WEATHER_WIND_STEPS; i++) {
            lv_chart_set_value_by_id(chart, g, i, isnan(w->gust[i]) ? LV_CHART_POINT_NONE
                                                    : (lv_coord_t)lroundf(w->gust[i] * 10.0f));
        }
    }
    lv_chart_series_t *sp = lv_chart_add_series(chart, lv_color_hex(ICON_COLOR), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < WEATHER_WIND_STEPS; i++) {
        lv_chart_set_value_by_id(chart, sp, i, isnan(w->speed[i]) ? LV_CHART_POINT_NONE
                                                : (lv_coord_t)lroundf(w->speed[i] * 10.0f));
    }
    lv_chart_refresh(chart);

    time_t t_end = w->end, t_start = t_end - (time_t)WEATHER_WIND_STEPS * 900;
    if (dec != NULL) {
        for (int t = 0; t <= intervals && t < 12; t++) {
            snprintf(dec->ylab[t], sizeof(dec->ylab[t]), "%g", (double)(hi - t * step));
        }
        dec->ny = intervals + 1 > 12 ? 12 : intervals + 1;
        hhmm(t_start, dec->xlab[0], sizeof(dec->xlab[0]));
        hhmm((t_start + t_end) / 2, dec->xlab[1], sizeof(dec->xlab[1]));
        hhmm(t_end, dec->xlab[2], sizeof(dec->xlab[2]));
        dec->nx = 3;
        dec->arrows = w->dir_ok;
    }

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
    weather_wind_release();
}

/* ---- 14-day bar graphs: daily low / high temperature, daily rainfall ---- */
#define BAR_H 220
#define TEMP_HI_COLOR 0xF6AD55
#define TEMP_LO_COLOR 0x8AB4F8
#define RAIN_COLOR    0x5BC0EB

/* One section: heading, Y labels + bar chart, and under it a label per day
 * (weekday, then the value(s) in the bars' colours - recolor syntax). */
static void bar_section(lv_obj_t *list, const char *title, const weather_week_t *w, int nser,
                        const float *const vals[], const uint32_t colors[], float lo, float hi,
                        bool temp)
{
    lv_obj_t *gap = add_container(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(gap, SECTION_GAP);
    lv_obj_t *heading = add_label(list, &gcal_font_20, HEAD_COLOR, title);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);

    float step;
    int intervals;
    nice_scale(&lo, &hi, BAR_H, &step, &intervals);

    const int lines = temp ? 4 : 3;
    chart_decor_t *dec;
    lv_obj_t *chart = new_chart(list, BAR_H + lines * 15 + 8, 0, lines * 15 + 8, &dec);
    lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(chart, WEATHER_WEEK_DAYS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(lo * 10), (lv_coord_t)(hi * 10));
    lv_chart_set_div_line_count(chart, intervals - 1, 0);      /* a line at every labelled step */
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x3C4043), 0);
    lv_obj_set_style_pad_column(chart, 5, LV_PART_MAIN);      /* gap between days */
    lv_obj_set_style_pad_column(chart, 2, LV_PART_ITEMS);     /* gap between a day's bars */
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2C2E), LV_PART_MAIN);
    lv_obj_set_style_radius(chart, 0, LV_PART_ITEMS);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int k = 0; k < nser; k++) {
        lv_chart_series_t *ser = lv_chart_add_series(chart, lv_color_hex(colors[k]), LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < WEATHER_WEEK_DAYS; i++) {
            bool have = temp ? w->t_ok[i] : w->r_day_ok[i];
            lv_chart_set_value_by_id(chart, ser, i,
                                     have ? (lv_coord_t)lroundf(vals[k][i] * 10.0f) : LV_CHART_POINT_NONE);
        }
    }
    lv_chart_refresh(chart);

    /* The Y numbers and the text under each day, painted by chart_decor_cb(). */
    if (dec != NULL) {
        for (int t = 0; t <= intervals && t < 12; t++) {
            snprintf(dec->ylab[t], sizeof(dec->ylab[t]), "%g", (double)(hi - t * step));
        }
        dec->ny = intervals + 1 > 12 ? 12 : intervals + 1;
        dec->ndays = WEATHER_WEEK_DAYS;
        dec->day_lines = lines;
        dec->day_col[0] = DIM_COLOR;
        dec->day_col[1] = DIM_COLOR;
        dec->day_col[2] = temp ? TEMP_HI_COLOR : RAIN_COLOR;
        dec->day_col[3] = TEMP_LO_COLOR;
        for (int i = 0; i < WEATHER_WEEK_DAYS; i++) {
            if (i == WEATHER_WEEK_DAYS - 1) {
                snprintf(dec->day[i][0], sizeof(dec->day[i][0]), "Now");
            } else {
                struct tm lt;
                localtime_r(&w->day[i], &lt);
                char wd[8];
                strftime(wd, sizeof(wd), "%a", &lt);
                /* 14 days repeat the weekday names, so the date goes under them. */
                snprintf(dec->day[i][0], sizeof(dec->day[i][0]), "%.1s", wd);
                snprintf(dec->day[i][1], sizeof(dec->day[i][1]), "%d", lt.tm_mday);
            }
            if (temp) {
                if (w->t_ok[i]) {
                    snprintf(dec->day[i][2], sizeof(dec->day[i][2]), "%.1f", (double)w->tmax[i]);
                    snprintf(dec->day[i][3], sizeof(dec->day[i][3]), "%.1f", (double)w->tmin[i]);
                } else {
                    snprintf(dec->day[i][2], sizeof(dec->day[i][2]), "-");
                }
            } else if (w->r_day_ok[i]) {
                snprintf(dec->day[i][2], sizeof(dec->day[i][2]), "%.1f", (double)w->rain[i]);
            } else {
                snprintf(dec->day[i][2], sizeof(dec->day[i][2]), "-");
            }
        }
    }
}

static void add_week_graphs(lv_obj_t *list)
{
    const weather_week_t *w = weather_week_acquire();
    if (w == NULL) {
        weather_week_release();
        return;
    }
    if (w->t_have > 0) {
        float lo = 0.0f, hi = -1000.0f, mn = 1000.0f;
        for (int i = 0; i < WEATHER_WEEK_DAYS; i++) {
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
        for (int i = 0; i < WEATHER_WEEK_DAYS; i++) {
            hi = fmaxf(hi, ceilf(w->rain[i]));
        }
        const float *vals[1] = { w->rain };
        const uint32_t colors[1] = { RAIN_COLOR };
        char title[64];
        snprintf(title, sizeof(title), "Last 14 days \xE2\x80\x93 daily rainfall %s", w->r_unit[0] ? w->r_unit : "mm");
        bar_section(list, title, w, 1, vals, colors, 0.0f, hi, false);
    }
    weather_week_release();
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

    /* The graphs below couldn't be fully loaded last time (a failed or cut-off
     * reply) - what's shown is older data or missing a graph. */
    if (weather_graphs_incomplete()) {
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

    const weather_details_t *d = weather_details_acquire();
    if (d == NULL) {
        add_wrapping_label(list, &gcal_font_20, TEXT_COLOR,
                           "No weather details yet - they're fetched from Open-Meteo every "
                           "15 minutes once a location is set on the config page.");
    } else {
        add_now(list, d);
        add_big_day(list, &d->days[0], "Today");
        add_big_day(list, &d->days[1], "Tomorrow");
        lv_obj_t *gap = add_container(list, LV_FLEX_FLOW_ROW);
        lv_obj_set_height(gap, 8);
        time_t now = time(NULL);
        for (int i = 2; i < WEATHER_FORECAST_DAYS; i++) {
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
    weather_details_release();
    add_hourly_strip(list);
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
