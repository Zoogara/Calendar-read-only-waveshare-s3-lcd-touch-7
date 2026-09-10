#include "calendar_ui_internal.h"
#include "ui_theme.h"
#include "event_store.h"

#include <stdio.h>
#include <string.h>

#define ROW_H         44
#define TIME_COL_W    44
#define HEADER_H      50
#define MAX_DAY_EVENTS 32

/* Hour range shown in the timed-event grid (24h clock, start inclusive /
 * end exclusive) - configurable via the gear-icon settings dialog, read
 * from cfg once at ui_day_create() time (applying a change needs a
 * restart, same as the other settings in that dialog, since the grid's
 * geometry is built once here rather than rebuilt on every populate). */
static int s_hour_start = APP_SETTINGS_DEFAULT_VIEW_START_HOUR;
static int s_hour_end = APP_SETTINGS_DEFAULT_VIEW_END_HOUR;

static lv_obj_t *s_allday_box;
static lv_obj_t *s_event_col;

static int col_w(void)
{
    return UI_CONTENT_W - TIME_COL_W;
}

/* Shared styles for the event blocks/chips and boundary-indicator badges
 * that ui_day_populate() creates and destroys fresh every sync cycle and
 * day/view navigation - see ui_month.c's ensure_shared_styles() for the
 * full reasoning (constant properties shared via one static lv_style_t
 * cost nothing per object beyond a pointer, instead of every object
 * paying for its own dynamically-sized local style out of internal RAM).
 * Only bg_color (one of many per-calendar colours) stays a genuine
 * per-object property. */
static lv_style_t s_bar_style;         /* timed event block: radius + bg_opa */
static lv_style_t s_chip_style;        /* all-day chip: radius + bg_opa + pad_hor */
static lv_style_t s_lbl_style;         /* event label, not past: font + white text */
static lv_style_t s_lbl_past_style;    /* event label, past: font + muted text */
static lv_style_t s_badge_style;       /* boundary indicator: bg_color + bg_opa + radius */
static lv_style_t s_badge_ind_style;   /* boundary indicator's arrow glyph: font + white text */
static bool s_styles_ready;

static void ensure_shared_styles(void)
{
    if (s_styles_ready) {
        return;
    }
    s_styles_ready = true;

    lv_style_init(&s_bar_style);
    lv_style_set_radius(&s_bar_style, 4);
    lv_style_set_bg_opa(&s_bar_style, LV_OPA_COVER);

    lv_style_init(&s_chip_style);
    lv_style_set_radius(&s_chip_style, 4);
    lv_style_set_bg_opa(&s_chip_style, LV_OPA_COVER);
    lv_style_set_pad_hor(&s_chip_style, 8);

    lv_style_init(&s_lbl_style);
    lv_style_set_text_font(&s_lbl_style, &gcal_font_14);
    lv_style_set_text_color(&s_lbl_style, lv_color_white());

    lv_style_init(&s_lbl_past_style);
    lv_style_set_text_font(&s_lbl_past_style, &gcal_font_14);
    lv_style_set_text_color(&s_lbl_past_style, ui_color(UI_COLOR_TEXT_PAST));

    lv_style_init(&s_badge_style);
    lv_style_set_bg_color(&s_badge_style, ui_color(UI_COLOR_TEXT_MUTED));
    lv_style_set_bg_opa(&s_badge_style, LV_OPA_COVER);
    lv_style_set_radius(&s_badge_style, 4);

    lv_style_init(&s_badge_ind_style);
    lv_style_set_text_font(&s_badge_ind_style, &gcal_font_14);
    lv_style_set_text_color(&s_badge_ind_style, lv_color_white());
}

lv_obj_t *ui_day_create(lv_obj_t *parent)
{
    ensure_shared_styles();

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

    s_allday_box = lv_obj_create(root);
    lv_obj_remove_style_all(s_allday_box);
    lv_obj_set_pos(s_allday_box, TIME_COL_W, 4);
    lv_obj_set_size(s_allday_box, col_w(), HEADER_H - 6);
    lv_obj_set_flex_flow(s_allday_box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_allday_box, 4, 0);
    lv_obj_set_style_pad_row(s_allday_box, 2, 0);
    lv_obj_clear_flag(s_allday_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header_line = lv_obj_create(root);
    lv_obj_remove_style_all(header_line);
    lv_obj_set_pos(header_line, 0, HEADER_H);
    lv_obj_set_size(header_line, UI_CONTENT_W, 1);
    lv_obj_set_style_bg_color(header_line, ui_color(UI_COLOR_GRID_LINE), 0);
    lv_obj_set_style_bg_opa(header_line, LV_OPA_COVER, 0);

    lv_obj_t *body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, HEADER_H + 1);
    lv_obj_set_size(body, UI_CONTENT_W, UI_CONTENT_H - HEADER_H - 1);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);

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
        lv_obj_set_width(lbl, TIME_COL_W - 4);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(lbl, 2, h * ROW_H + 2);
    }

    s_event_col = lv_obj_create(body);
    lv_obj_remove_style_all(s_event_col);
    lv_obj_set_pos(s_event_col, TIME_COL_W, 0);
    lv_obj_set_size(s_event_col, col_w(), body_content_h);
    lv_obj_clear_flag(s_event_col, LV_OBJ_FLAG_SCROLLABLE);

    return root;
}

/* A small solid badge pinned to the top/bottom edge of the scrollable
 * event area, shown when at least one event on the day falls (even
 * partially) outside the configured hour range - without it, an event
 * before/after the visible window is just silently missing with no
 * indication it exists at all. A solid dark badge (rather than a bare
 * coloured glyph) keeps it readable whether it lands over the plain
 * background or a colour-block event, and the extra margin from the edge
 * keeps it from overhanging an event box that happens to end right at
 * the visible window's boundary. */
static void add_boundary_indicator(lv_obj_t *parent, bool at_top)
{
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 22, 18);
    lv_obj_add_style(badge, &s_badge_style, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    /* Event blocks end at x = col_w()-8 (4px inset + width col_w()-12) -
     * an 8px right margin put this badge's left edge right up against
     * that same line, reading as if it were overhanging the block. */
    lv_obj_align(badge, at_top ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_BOTTOM_RIGHT,
                 -18, at_top ? 8 : -8);

    lv_obj_t *ind = lv_label_create(badge);
    lv_label_set_text(ind, at_top ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    lv_obj_add_style(ind, &s_badge_ind_style, 0);
    lv_obj_center(ind);
}

/* day is the local midnight of the day currently being viewed (from
 * ui_day_populate()'s own ui_start_of_day() call) - needed to tell "this
 * event's end time is a small hour-of-day number because it genuinely
 * runs past midnight into tomorrow" apart from "this event is malformed/
 * zero-duration", which look identical if you only look at tm_hour.
 * Without this, an overnight event (e.g. 10pm-1am) had its end computed
 * from end_tm.tm_hour directly (1, i.e. numerically *before* its own
 * start hour of 22), tripped the "invalid range" fallback below, and
 * collapsed into a fake 30-minute block at its start time instead of
 * rendering as an hour-and-a-bit running to the bottom of the grid.
 * Shared between the column-packing pre-pass (which only needs the
 * hours, not a rendered block) and add_block() itself, so the day-
 * boundary special-casing lives in exactly one place. */
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

/* Greedy interval-column packing, same idea Google Calendar's day/week
 * grid uses: events (already sorted by start time - event_store keeps
 * everything sorted by ev->start) get packed into as few side-by-side
 * columns as needed, and split evenly within just their own cluster of
 * mutually/transitively overlapping events - so two events overlapping
 * only each other never get squeezed by some unrelated clash elsewhere
 * in the day. Without this, a second event starting during a still-
 * running first one just got drawn on top of it, obscuring whichever one
 * happened to be added second. */
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

static void add_block(const gcal_event_t *ev, time_t day, time_t now, bool *out_before, bool *out_after,
                       int col, int ncols)
{
    struct tm start_tm, end_tm;
    localtime_r(&ev->start, &start_tm);
    localtime_r(&ev->end, &end_tm);

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
        return;
    }

    int y = (int)((start_h - s_hour_start) * ROW_H);
    int h = (int)((end_h - start_h) * ROW_H);
    if (h < 24) {
        h = 24;
    }

    /* col_w() - 8 leaves a 4px margin each side (matching the single-
     * column layout below); ncols slots share that space with a 3px gap
     * between each. */
    int avail = col_w() - 8;
    int gap = ncols > 1 ? 3 : 0;
    int slot_w = (avail - gap * (ncols - 1)) / ncols;
    int x = 4 + col * (slot_w + gap);

    bool is_past = ev->end <= now;
    lv_obj_t *blk = lv_obj_create(s_event_col);
    lv_obj_remove_style_all(blk);
    lv_obj_set_pos(blk, x, y);
    lv_obj_set_size(blk, slot_w, h - 2);
    lv_obj_add_style(blk, &s_bar_style, 0);
    lv_obj_set_style_bg_color(blk, ui_color(is_past ? ui_lighten(ev->color) : ev->color), 0);
    lv_obj_clear_flag(blk, LV_OBJ_FLAG_SCROLLABLE);

    char time_buf[24];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d - %02d:%02d",
             start_tm.tm_hour, start_tm.tm_min, end_tm.tm_hour, end_tm.tm_min);

    lv_obj_t *title = lv_label_create(blk);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(title, slot_w - 8, h - 6);
    lv_obj_set_pos(title, 6, 3);
    lv_obj_add_style(title, is_past ? &s_lbl_past_style : &s_lbl_style, 0);
    lv_label_set_text_fmt(title, "%s\n%s", ev->summary, time_buf);
}

void ui_day_release(void)
{
    lv_obj_clean(s_allday_box);
    lv_obj_clean(s_event_col);
}

void ui_day_populate(lv_obj_t *root, time_t cursor)
{
    (void)root;
    time_t day = ui_start_of_day(cursor);
    time_t now;
    time(&now);
    time_t today = ui_start_of_day(now);

    lv_obj_clean(s_allday_box);
    lv_obj_clean(s_event_col);

    /* static, not a stack local - see the matching comment in ui_month.c's
     * populate function for why. */
    static gcal_event_t events[MAX_DAY_EVENTS];
    int n = event_store_copy_range(day, ui_add_days(day, 1), events, MAX_DAY_EVENTS);

    /* Pre-pass: work out how many side-by-side columns each timed event
     * needs before creating any of them - add_block() needs its column
     * assignment up front to size/position the block, not after the fact. */
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

    bool has_before = false, has_after = false;
    int ti = 0;
    for (int e = 0; e < n; e++) {
        if (!ui_calendar_enabled(events[e].calendar_index)) {
            continue;
        }
        if (events[e].all_day) {
            /* Past if either this whole event has ended, or - for a
             * multi-day event still in progress - the day being viewed is
             * before today, same reasoning as ui_month.c's populate. */
            bool is_past = (day < today) || (events[e].end <= now);
            lv_obj_t *chip = lv_obj_create(s_allday_box);
            lv_obj_remove_style_all(chip);
            lv_obj_set_height(chip, 22);
            lv_obj_set_width(chip, LV_SIZE_CONTENT);
            lv_obj_add_style(chip, &s_chip_style, 0);
            lv_obj_set_style_bg_color(chip, ui_color(is_past ? ui_lighten(events[e].color) : events[e].color), 0);
            lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *lbl = lv_label_create(chip);
            lv_obj_add_style(lbl, is_past ? &s_lbl_past_style : &s_lbl_style, 0);
            lv_label_set_text(lbl, events[e].summary);
            lv_obj_center(lbl);
        } else {
            add_block(&events[e], day, now, &has_before, &has_after, timed_col[ti], timed_ncols[ti]);
            ti++;
        }
    }

    if (has_before) {
        add_boundary_indicator(s_event_col, true);
    }
    if (has_after) {
        add_boundary_indicator(s_event_col, false);
    }
}

void ui_day_title(time_t cursor, char *out, size_t out_sz)
{
    struct tm tm;
    localtime_r(&cursor, &tm);
    static const char *WD[7] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    static const char *MON3[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(out, out_sz, "%s, %s %d", WD[tm.tm_wday], MON3[tm.tm_mon], tm.tm_mday);
}
