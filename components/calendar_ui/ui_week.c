#include "calendar_ui_internal.h"
#include "ui_theme.h"
#include "event_store.h"

#include <stdio.h>
#include <string.h>

#define WEEK_DAYS     7
#define ROW_H         40
#define TIME_COL_W    40
#define HEADER_H      56
#define MAX_DAY_EVENTS 24

/* Hour range shown in the timed-event grid - see the matching comment in
 * ui_day.c, same reasoning applies here. */
static int s_hour_start = APP_SETTINGS_DEFAULT_VIEW_START_HOUR;
static int s_hour_end = APP_SETTINGS_DEFAULT_VIEW_END_HOUR;

static const char *WEEKDAY_ABBR[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

static lv_obj_t *s_day_header[WEEK_DAYS];
static lv_obj_t *s_day_date_label[WEEK_DAYS];
static lv_obj_t *s_day_allday_box[WEEK_DAYS];
static lv_obj_t *s_day_col[WEEK_DAYS];
static time_t s_week_start;

static int day_col_w(void)
{
    return (UI_CONTENT_W - TIME_COL_W) / WEEK_DAYS;
}

static void day_header_click_cb(lv_event_t *e)
{
    int d = (int)(uintptr_t)lv_event_get_user_data(e);
    ui_switch_to_day(ui_add_days(s_week_start, d));
}

lv_obj_t *ui_week_create(lv_obj_t *parent)
{
    const app_settings_t *cfg = ui_get_cfg();
    if (cfg->view_start_hour < cfg->view_end_hour) {
        s_hour_start = cfg->view_start_hour;
        s_hour_end = cfg->view_end_hour;
    }
    int hours_shown = s_hour_end - s_hour_start;
    int body_content_h = hours_shown * ROW_H;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, UI_CONTENT_X, UI_CONTENT_Y);
    lv_obj_set_size(root, UI_CONTENT_W, UI_CONTENT_H);
    lv_obj_set_style_bg_color(root, ui_color(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    int colw = day_col_w();

    lv_obj_t *header = lv_obj_create(root);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, UI_CONTENT_W, HEADER_H);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, ui_color(UI_COLOR_GRID_LINE), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    for (int d = 0; d < WEEK_DAYS; d++) {
        lv_obj_t *cell = lv_obj_create(header);
        lv_obj_remove_style_all(cell);
        lv_obj_set_pos(cell, TIME_COL_W + d * colw, 0);
        lv_obj_set_size(cell, colw, HEADER_H);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, day_header_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)d);
        s_day_header[d] = cell;

        lv_obj_t *date_lbl = lv_label_create(cell);
        lv_obj_set_style_text_font(date_lbl, &gcal_font_14, 0);
        lv_obj_set_pos(date_lbl, 4, 2);
        s_day_date_label[d] = date_lbl;

        lv_obj_t *allday = lv_obj_create(cell);
        lv_obj_remove_style_all(allday);
        lv_obj_set_pos(allday, 2, 22);
        lv_obj_set_size(allday, colw - 4, HEADER_H - 24);
        lv_obj_clear_flag(allday, LV_OBJ_FLAG_SCROLLABLE);
        /* lv_obj_create() defaults to CLICKABLE=true - this box (and the
         * per-event chips populated into it below) sits on top of most of
         * the header cell now that it's tappable, and would otherwise
         * silently swallow the tap instead of it reaching
         * day_header_click_cb (same gotcha noted in ui_month.c's
         * events_box). */
        lv_obj_clear_flag(allday, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(allday, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(allday, 1, 0);
        s_day_allday_box[d] = allday;
    }

    lv_obj_t *body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, HEADER_H);
    lv_obj_set_size(body, UI_CONTENT_W, UI_CONTENT_H - HEADER_H);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* Time labels + hour gridlines, static content. */
    for (int h = 0; h < hours_shown; h++) {
        lv_obj_t *line = lv_obj_create(body);
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, 0, h * ROW_H);
        lv_obj_set_size(line, UI_CONTENT_W, ROW_H);
        lv_obj_set_style_border_side(line, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_color(line, ui_color(UI_COLOR_GRID_LINE), 0);
        lv_obj_set_style_border_width(line, 1, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(body);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d:00", s_hour_start + h);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &gcal_font_14, 0);
        lv_obj_set_style_text_color(lbl, ui_color(UI_COLOR_TEXT_MUTED), 0);
        /* Explicit width + clip, not left to auto-size - "10:00" at this
         * font can render wider than TIME_COL_W leaves room for, and
         * without a hard clip boundary it overflows into the first day
         * column instead of just being cut off cleanly. */
        lv_obj_set_width(lbl, TIME_COL_W - 4);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(lbl, 2, h * ROW_H + 2);
    }

    int colw2 = day_col_w();
    for (int d = 0; d < WEEK_DAYS; d++) {
        lv_obj_t *col = lv_obj_create(body);
        lv_obj_remove_style_all(col);
        lv_obj_set_pos(col, TIME_COL_W + d * colw2, 0);
        lv_obj_set_size(col, colw2, body_content_h);
        lv_obj_set_style_border_side(col, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(col, ui_color(UI_COLOR_GRID_LINE), 0);
        lv_obj_set_style_border_width(col, 1, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        s_day_col[d] = col;
    }

    return root;
}

/* Small solid badge pinned to a day column's top/bottom edge, shown when
 * at least one event that day falls (even partially) outside the
 * configured hour range - see the matching (longer) comment on ui_day.c's
 * version of this for why it's a solid badge rather than a bare glyph,
 * and why it sits a little clear of the exact edge. */
static void add_boundary_indicator(lv_obj_t *parent, bool at_top)
{
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 18, 15);
    lv_obj_set_style_bg_color(badge, ui_color(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 3, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(badge, at_top ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_BOTTOM_RIGHT,
                 -4, at_top ? 4 : -4);

    lv_obj_t *ind = lv_label_create(badge);
    lv_label_set_text(ind, at_top ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(ind, &gcal_font_14, 0);
    lv_obj_set_style_text_color(ind, lv_color_white(), 0);
    lv_obj_center(ind);
}

/* day is the local midnight of the day column being drawn - see the
 * matching (much longer) comment on ui_day.c's version of add_block() for
 * why this is needed to render an overnight event correctly instead of
 * collapsing it into a fake 30-minute block. Shared between the column-
 * packing pre-pass and add_block() itself - see ui_day.c's copy of this
 * same helper for why. */
static void day_relative_hours(const gcal_event_t *ev, time_t day, double *out_start_h, double *out_end_h)
{
    struct tm start_tm, end_tm;
    localtime_r(&ev->start, &start_tm);
    localtime_r(&ev->end, &end_tm);
    bool starts_before_this_day = ev->start < day;
    bool ends_after_this_day = ev->end > ui_add_days(day, 1);
    *out_start_h = starts_before_this_day ? 0.0 : (start_tm.tm_hour + start_tm.tm_min / 60.0);
    *out_end_h = ends_after_this_day ? 24.0 : (end_tm.tm_hour + end_tm.tm_min / 60.0);
    if (*out_end_h <= *out_start_h) {
        *out_end_h = *out_start_h + 0.5;
    }
}

/* Greedy interval-column packing - see ui_day.c's copy of this same
 * helper for the full rationale. Run once per day column here, so an
 * overlap in Monday's column never affects Tuesday's layout. */
static void assign_columns(const double *start_h, const double *end_h, int n, int *out_col, int *out_ncols)
{
    double col_end[MAX_DAY_EVENTS];
    int ncols = 0;
    int cluster_begin = 0;
    double cluster_max_end = -1.0;

    for (int i = 0; i < n; i++) {
        if (ncols > 0 && start_h[i] >= cluster_max_end) {
            for (int j = cluster_begin; j < i; j++) {
                out_ncols[j] = ncols;
            }
            cluster_begin = i;
            ncols = 0;
            cluster_max_end = -1.0;
        }
        int assigned = -1;
        for (int j = 0; j < ncols; j++) {
            if (col_end[j] <= start_h[i]) {
                assigned = j;
                break;
            }
        }
        if (assigned < 0) {
            assigned = ncols++;
        }
        col_end[assigned] = end_h[i];
        out_col[i] = assigned;
        if (end_h[i] > cluster_max_end) {
            cluster_max_end = end_h[i];
        }
    }
    for (int j = cluster_begin; j < n; j++) {
        out_ncols[j] = ncols;
    }
}

static void add_block(lv_obj_t *parent, int colw, const gcal_event_t *ev, time_t day, time_t now,
                       bool *out_before, bool *out_after, int col, int ncols)
{
    double start_h, end_h;
    day_relative_hours(ev, day, &start_h, &end_h);
    if (start_h < s_hour_start) {
        *out_before = true;
    }
    if (end_h > s_hour_end) {
        *out_after = true;
    }
    start_h = start_h < s_hour_start ? s_hour_start : start_h;
    end_h = end_h > s_hour_end ? s_hour_end : end_h;
    if (end_h <= start_h) {
        return; /* fully outside the visible window */
    }

    int y = (int)((start_h - s_hour_start) * ROW_H);
    int h = (int)((end_h - start_h) * ROW_H);
    if (h < 18) {
        h = 18;
    }

    /* colw - 2 leaves a 1px margin each side (matching the single-column
     * layout below); ncols slots share that space with a 2px gap between
     * each - day columns here are much narrower than day view's, so a
     * smaller gap keeps more room for the label. */
    int avail = colw - 2;
    int gap = ncols > 1 ? 2 : 0;
    int slot_w = (avail - gap * (ncols - 1)) / ncols;
    int x = 1 + col * (slot_w + gap);

    bool is_past = ev->end <= now;
    lv_obj_t *blk = lv_obj_create(parent);
    lv_obj_remove_style_all(blk);
    lv_obj_set_pos(blk, x, y);
    lv_obj_set_size(blk, slot_w, h - 1);
    lv_obj_set_style_radius(blk, 3, 0);
    lv_obj_set_style_bg_color(blk, ui_color(is_past ? ui_lighten(ev->color) : ev->color), 0);
    lv_obj_set_style_bg_opa(blk, LV_OPA_COVER, 0);
    lv_obj_clear_flag(blk, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(blk);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(lbl, slot_w - 4, h - 3);
    lv_obj_set_pos(lbl, 3, 1);
    lv_obj_set_style_text_font(lbl, &gcal_font_14, 0);
    lv_obj_set_style_text_color(lbl, is_past ? ui_color(UI_COLOR_TEXT_PAST) : lv_color_white(), 0);
    lv_label_set_text(lbl, ev->summary);
}

void ui_week_release(void)
{
    for (int d = 0; d < WEEK_DAYS; d++) {
        lv_obj_clean(s_day_allday_box[d]);
        lv_obj_clean(s_day_col[d]);
    }
}

void ui_week_populate(lv_obj_t *root, time_t cursor)
{
    (void)root;
    s_week_start = ui_start_of_week(cursor);
    time_t now;
    time(&now);
    time_t today = ui_start_of_day(now);
    int colw = day_col_w();

    for (int d = 0; d < WEEK_DAYS; d++) {
        time_t day = ui_add_days(s_week_start, d);
        struct tm day_tm;
        localtime_r(&day, &day_tm);
        bool is_today = ui_same_local_day(day, today);

        char buf[16];
        snprintf(buf, sizeof(buf), "%s %d", WEEKDAY_ABBR[d], day_tm.tm_mday);
        lv_label_set_text(s_day_date_label[d], buf);
        lv_obj_set_style_text_color(s_day_date_label[d],
                                     is_today ? ui_color(UI_COLOR_ACCENT) : ui_color(UI_COLOR_TEXT), 0);

        lv_obj_clean(s_day_allday_box[d]);
        lv_obj_clean(s_day_col[d]);

        /* static, not a stack local - see the matching comment in
         * ui_month.c's populate function for why. */
        static gcal_event_t events[MAX_DAY_EVENTS];
        int n = event_store_copy_range(day, ui_add_days(day, 1), events, MAX_DAY_EVENTS);

        /* Pre-pass: work out each timed event's column assignment before
         * creating any of them - see ui_day.c's populate function for why. */
        static double timed_start_h[MAX_DAY_EVENTS];
        static double timed_end_h[MAX_DAY_EVENTS];
        static int timed_col[MAX_DAY_EVENTS];
        static int timed_ncols[MAX_DAY_EVENTS];
        int tn = 0;
        for (int e = 0; e < n; e++) {
            if (!ui_calendar_enabled(events[e].calendar_index) || events[e].all_day) {
                continue;
            }
            day_relative_hours(&events[e], day, &timed_start_h[tn], &timed_end_h[tn]);
            tn++;
        }
        assign_columns(timed_start_h, timed_end_h, tn, timed_col, timed_ncols);

        int allday_shown = 0;
        bool has_before = false, has_after = false;
        int ti = 0;
        for (int e = 0; e < n; e++) {
            if (!ui_calendar_enabled(events[e].calendar_index)) {
                continue;
            }
            if (events[e].all_day) {
                if (allday_shown >= 2) {
                    continue;
                }
                /* Past if either this whole event has ended, or - for a
                 * multi-day event still in progress - this day column is
                 * before today, same reasoning as ui_month.c's populate. */
                bool is_past = (day < today) || (events[e].end <= now);
                lv_obj_t *chip = lv_obj_create(s_day_allday_box[d]);
                lv_obj_remove_style_all(chip);
                lv_obj_set_width(chip, LV_PCT(100));
                lv_obj_set_height(chip, 14);
                lv_obj_set_style_radius(chip, 3, 0);
                lv_obj_set_style_bg_color(chip, ui_color(is_past ? ui_lighten(events[e].color) : events[e].color), 0);
                lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
                lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE); /* let the tap reach day_header_click_cb */
                lv_obj_t *lbl = lv_label_create(chip);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
                lv_obj_set_width(lbl, LV_PCT(100));
                lv_obj_set_style_text_font(lbl, &gcal_font_14, 0);
                lv_obj_set_style_text_color(lbl, is_past ? ui_color(UI_COLOR_TEXT_PAST) : lv_color_white(), 0);
                lv_label_set_text(lbl, events[e].summary);
                allday_shown++;
            } else {
                add_block(s_day_col[d], colw, &events[e], day, now, &has_before, &has_after,
                          timed_col[ti], timed_ncols[ti]);
                ti++;
            }
        }

        if (has_before) {
            add_boundary_indicator(s_day_col[d], true);
        }
        if (has_after) {
            add_boundary_indicator(s_day_col[d], false);
        }
    }
}

void ui_week_title(time_t cursor, char *out, size_t out_sz)
{
    time_t start = ui_start_of_week(cursor);
    time_t end = ui_add_days(start, 6);
    struct tm ts, te;
    localtime_r(&start, &ts);
    localtime_r(&end, &te);
    static const char *MON3[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    if (ts.tm_mon == te.tm_mon) {
        snprintf(out, out_sz, "%s %d - %d, %d", MON3[ts.tm_mon], ts.tm_mday, te.tm_mday, ts.tm_year + 1900);
    } else {
        snprintf(out, out_sz, "%s %d - %s %d", MON3[ts.tm_mon], ts.tm_mday, MON3[te.tm_mon], te.tm_mday);
    }
}
