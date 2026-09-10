#include "calendar_ui_internal.h"
#include "ui_theme.h"
#include "event_store.h"

#include <stdio.h>
#include <string.h>

#define GRID_COLS 7
#define GRID_ROWS 6
#define HEADER_H  22
#define MAX_BARS_PER_CELL 2

static const char *WEEKDAY_ABBR[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
static const char *MONTH_NAMES[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

typedef struct {
    lv_obj_t *cell;
    lv_obj_t *day_label;
    lv_obj_t *events_box;
    lv_obj_t *overflow_badge; /* NULL when this cell has no overflow badge
                                  right now - see ui_month_populate(),
                                  which must delete this itself since it
                                  lives directly on `cell`, not inside
                                  events_box (the only child that gets
                                  lv_obj_clean()'d every populate). */
    time_t day;
} month_cell_t;

static month_cell_t s_cells[GRID_COLS * GRID_ROWS];

/* Shared styles for the event bars/labels and the overflow badge that
 * ui_month_populate() creates and destroys fresh on every single sync
 * cycle and month/view navigation (unlike the 42 persistent day_label
 * cells, which are created once and just have individual properties
 * mutated in place afterward). Every lv_obj_set_style_*() call on an
 * object with no matching style yet allocates that object its own
 * dynamically-sized "local style" out of internal RAM (confirmed:
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 keeps allocations this small
 * off PSRAM entirely) - for properties that are actually constant across
 * every bar/label/badge (radius, opacity, font, and even text colour
 * where there are only ever two possible values), defining ONE static
 * lv_style_t up front and attaching it via lv_obj_add_style() costs
 * nothing per object beyond a pointer, instead of paying for a fresh
 * local style on every single one, every single populate(). Found while
 * chasing a real, reproducible correlation (2026-09-10) between this
 * view being actively rendered (tighter internal-RAM headroom) and the
 * ICS calendar's TLS certificate verification becoming intermittently
 * flaky - see gcal_client.c/ics_client.c's own comments. Only bg_color
 * (which takes one of many per-calendar colours, not a small fixed set)
 * stays a genuine per-object property. */
static lv_style_t s_bar_style;         /* event bar: radius + bg_opa */
static lv_style_t s_lbl_style;         /* event label, not past: font + pad + white text */
static lv_style_t s_lbl_past_style;    /* event label, past: font + pad + muted text */
static lv_style_t s_badge_style;       /* overflow badge: bg_color + bg_opa + radius (all constant) */
static lv_style_t s_badge_ind_style;   /* overflow badge's "+" glyph: font + white text (all constant) */
static bool s_styles_ready;

static void ensure_shared_styles(void)
{
    if (s_styles_ready) {
        return;
    }
    s_styles_ready = true;

    lv_style_init(&s_bar_style);
    lv_style_set_radius(&s_bar_style, 3);
    lv_style_set_bg_opa(&s_bar_style, LV_OPA_COVER);

    lv_style_init(&s_lbl_style);
    lv_style_set_text_font(&s_lbl_style, &gcal_font_14);
    lv_style_set_pad_left(&s_lbl_style, 3);
    lv_style_set_text_color(&s_lbl_style, lv_color_white());

    lv_style_init(&s_lbl_past_style);
    lv_style_set_text_font(&s_lbl_past_style, &gcal_font_14);
    lv_style_set_pad_left(&s_lbl_past_style, 3);
    lv_style_set_text_color(&s_lbl_past_style, ui_color(UI_COLOR_TEXT_PAST));

    lv_style_init(&s_badge_style);
    lv_style_set_bg_color(&s_badge_style, ui_color(UI_COLOR_TEXT_MUTED));
    lv_style_set_bg_opa(&s_badge_style, LV_OPA_COVER);
    lv_style_set_radius(&s_badge_style, 3);

    lv_style_init(&s_badge_ind_style);
    lv_style_set_text_font(&s_badge_ind_style, &gcal_font_14);
    lv_style_set_text_color(&s_badge_ind_style, lv_color_white());
}

static void cell_click_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    ui_switch_to_day(s_cells[idx].day);
}

lv_obj_t *ui_month_create(lv_obj_t *parent)
{
    ensure_shared_styles();

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, UI_CONTENT_X, UI_CONTENT_Y);
    lv_obj_set_size(root, UI_CONTENT_W, UI_CONTENT_H);
    lv_obj_set_style_bg_color(root, ui_color(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    int cell_w = UI_CONTENT_W / GRID_COLS;
    int cell_h = (UI_CONTENT_H - HEADER_H) / GRID_ROWS;

    for (int c = 0; c < GRID_COLS; c++) {
        lv_obj_t *hdr = lv_label_create(root);
        lv_label_set_text(hdr, WEEKDAY_ABBR[c]);
        lv_obj_set_style_text_font(hdr, &gcal_font_14, 0);
        lv_obj_set_style_text_color(hdr, ui_color(UI_COLOR_TEXT_MUTED), 0);
        lv_obj_set_pos(hdr, c * cell_w + 6, 2);
    }

    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) {
        int row = i / GRID_COLS;
        int col = i % GRID_COLS;

        lv_obj_t *cell = lv_obj_create(root);
        lv_obj_remove_style_all(cell);
        lv_obj_set_pos(cell, col * cell_w, HEADER_H + row * cell_h);
        lv_obj_set_size(cell, cell_w, cell_h);
        lv_obj_set_style_border_color(cell, ui_color(UI_COLOR_GRID_LINE), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(cell, cell_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *day_label = lv_label_create(cell);
        lv_obj_set_style_text_font(day_label, &gcal_font_14, 0);
        lv_obj_set_style_pad_all(day_label, 3, 0);
        lv_obj_set_style_radius(day_label, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_pos(day_label, 3, 2);

        lv_obj_t *events_box = lv_obj_create(cell);
        lv_obj_remove_style_all(events_box);
        lv_obj_set_pos(events_box, 2, 24);
        lv_obj_set_size(events_box, cell_w - 4, cell_h - 26);
        lv_obj_clear_flag(events_box, LV_OBJ_FLAG_SCROLLABLE);
        /* lv_obj_create() defaults to CLICKABLE=true (unlike labels, which
         * clear it by default) - this box covers most of the cell's area,
         * has no click handler of its own, and doesn't bubble events to
         * its parent, so without this almost every tap on a day cell was
         * silently swallowed here instead of reaching cell_click_cb. */
        lv_obj_clear_flag(events_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(events_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(events_box, 2, 0);

        s_cells[i].cell = cell;
        s_cells[i].day_label = day_label;
        s_cells[i].events_box = events_box;
    }

    return root;
}

/* Small solid badge pinned to the day cell's top-right corner, shown when
 * the cell's event bars are full (MAX_BARS_PER_CELL already shown) and at
 * least one more event exists that day but has no room to render - same
 * solid-badge treatment as the day/week views' boundary-indicator
 * chevron (see ui_day.c's add_boundary_indicator()), so "there's more
 * here, tap in" reads consistently across views. This replaced an inline
 * "+N more" text row that lived inside the cell's already-full events
 * flex box - with MAX_BARS_PER_CELL=2 event bars already consuming
 * essentially all of a month cell's height, that text row had no space
 * left to actually render in and was invisible in practice; a
 * fixed-position corner badge doesn't need any of that flex space. */
static lv_obj_t *add_overflow_badge(lv_obj_t *cell)
{
    lv_obj_t *badge = lv_obj_create(cell);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 14, 14);
    lv_obj_add_style(badge, &s_badge_style, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -2, 4);

    /* montserrat_14 is the smallest font compiled into this build (see
     * sdkconfig's CONFIG_LV_FONT_MONTSERRAT_* - only 14 and 20 are
     * enabled) - LVGL's built-in symbol glyphs are sized to match their
     * font's point size, so PLUS at 14px centers cleanly in this 14x14
     * badge with no extra padding needed. */
    lv_obj_t *ind = lv_label_create(badge);
    lv_label_set_text(ind, LV_SYMBOL_PLUS);
    lv_obj_add_style(ind, &s_badge_ind_style, 0);
    lv_obj_center(ind);

    return badge;
}

void ui_month_release(void)
{
    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) {
        lv_obj_clean(s_cells[i].events_box);
        if (s_cells[i].overflow_badge != NULL) {
            lv_obj_del(s_cells[i].overflow_badge);
            s_cells[i].overflow_badge = NULL;
        }
    }
}

void ui_month_populate(lv_obj_t *root, time_t cursor)
{
    (void)root;
    time_t month_start = ui_start_of_month(cursor);
    time_t grid_start = ui_start_of_week(month_start);
    time_t now;
    time(&now);
    time_t today = ui_start_of_day(now);

    struct tm cursor_tm;
    localtime_r(&cursor, &cursor_tm);

    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) {
        time_t day = ui_add_days(grid_start, i);
        s_cells[i].day = day;

        struct tm day_tm;
        localtime_r(&day, &day_tm);
        bool in_month = (day_tm.tm_mon == cursor_tm.tm_mon && day_tm.tm_year == cursor_tm.tm_year);
        bool is_today = ui_same_local_day(day, today);

        char num[4];
        snprintf(num, sizeof(num), "%d", day_tm.tm_mday);
        lv_label_set_text(s_cells[i].day_label, num);

        if (is_today) {
            lv_obj_set_style_bg_opa(s_cells[i].day_label, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_cells[i].day_label, ui_color(UI_COLOR_TODAY_BG), 0);
            lv_obj_set_style_text_color(s_cells[i].day_label, lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_opa(s_cells[i].day_label, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(s_cells[i].day_label,
                                         in_month ? ui_color(UI_COLOR_TEXT) : ui_color(UI_COLOR_TEXT_MUTED), 0);
        }

        lv_obj_clean(s_cells[i].events_box);
        /* Not inside events_box, so lv_obj_clean() above doesn't reach it -
         * without deleting it explicitly here, every populate() that finds
         * this cell still overflowing added *another* badge on top of the
         * last one instead of replacing it, leaking an LVGL object (and
         * its internal-RAM-backed label) on every sync cycle or month/
         * view navigation. That's a real, observed cause of the internal-
         * RAM/TLS certificate-verification failures documented in
         * gcal_client.c's log_heap_state() comment - not just a cosmetic
         * bug. */
        if (s_cells[i].overflow_badge != NULL) {
            lv_obj_del(s_cells[i].overflow_badge);
            s_cells[i].overflow_badge = NULL;
        }

        /* static, not a stack local: at ~168 bytes/event this is ~4KB,
         * over half of esp_lvgl_port's default 7KB LVGL task stack, and
         * this function runs deep in that task's call chain (button click
         * -> event callback -> render_current_view -> here). Not
         * reentrant, so static is safe: this only ever runs either inside
         * the LVGL task's own serialized event loop, or from net_task
         * while holding bsp_lvgl_lock. */
        static gcal_event_t events[24];
        int n = event_store_copy_range(day, ui_add_days(day, 1), events, 24);
        int shown = 0, hidden = 0;
        for (int e = 0; e < n; e++) {
            if (!ui_calendar_enabled(events[e].calendar_index)) {
                continue;
            }
            if (shown >= MAX_BARS_PER_CELL) {
                hidden++;
                continue;
            }
            /* Past if either this whole event has ended, or - for a
             * multi-day event still in progress - this specific day cell
             * is before today, even though the event as a whole hasn't
             * ended yet (e.g. a 5-day trip: days 1-2 should read as
             * already happened once day 3 arrives, not stay full-colour
             * until the whole trip is over on day 5). */
            bool is_past = (day < today) || (events[e].end <= now);
            lv_obj_t *bar = lv_obj_create(s_cells[i].events_box);
            lv_obj_remove_style_all(bar);
            lv_obj_set_width(bar, LV_PCT(100));
            lv_obj_set_height(bar, 16);
            lv_obj_add_style(bar, &s_bar_style, 0);
            lv_obj_set_style_bg_color(bar, ui_color(is_past ? ui_lighten(events[e].color) : events[e].color), 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            /* Same reasoning as events_box above - let taps on an event
             * chip fall through to the day cell instead of being
             * swallowed by this handler-less child. */
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *lbl = lv_label_create(bar);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(lbl, LV_PCT(100));
            lv_obj_add_style(lbl, is_past ? &s_lbl_past_style : &s_lbl_style, 0);
            lv_label_set_text(lbl, events[e].summary);
            shown++;
        }
        if (hidden > 0) {
            s_cells[i].overflow_badge = add_overflow_badge(s_cells[i].cell);
        }
    }
}

void ui_month_title(time_t cursor, char *out, size_t out_sz)
{
    struct tm tm;
    localtime_r(&cursor, &tm);
    snprintf(out, out_sz, "%s %d", MONTH_NAMES[tm.tm_mon], tm.tm_year + 1900);
}
