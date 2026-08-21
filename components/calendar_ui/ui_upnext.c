#include "calendar_ui_internal.h"
#include "ui_theme.h"
#include "event_store.h"

#include <stdio.h>
#include <string.h>

#define MAX_UPNEXT_ROWS 14
#define ITEM_H          36
/* Wide enough for "Tomorrow" (the longest string format_day() ever
 * produces - longer than "Today" or a "Wed 20"-style date) to fit on one
 * line at montserrat_14 without wrapping; 64 was just narrow enough that
 * only the final "w" wrapped to its own line. */
#define DATE_COL_W      78

static lv_obj_t *s_list;

lv_obj_t *ui_upnext_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, UI_CONTENT_X, UI_CONTENT_Y);
    lv_obj_set_size(root, UI_CONTENT_W, UI_CONTENT_H);
    lv_obj_set_style_bg_color(root, ui_color(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_pos(s_list, 0, 0);
    lv_obj_set_size(s_list, UI_CONTENT_W, UI_CONTENT_H);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 10, 0);

    return root;
}

/* Weekday + day only (no month) - one column to the left of that day's
 * items, shown once per day group rather than repeated on every row. */
static void format_day(time_t start, time_t now, char *out, size_t out_sz)
{
    struct tm ev_tm;
    localtime_r(&start, &ev_tm);
    static const char *WD[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

    if (ui_same_local_day(start, now)) {
        snprintf(out, out_sz, "Today");
    } else if (ui_same_local_day(start, ui_add_days(now, 1))) {
        snprintf(out, out_sz, "Tomorrow");
    } else {
        snprintf(out, out_sz, "%s %d", WD[ev_tm.tm_wday], ev_tm.tm_mday);
    }
}

static void format_time12(time_t t, char *out, size_t out_sz)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    int h12 = tmv.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    snprintf(out, out_sz, "%d:%02d %s", h12, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
}

/* One item box per event, filled with that calendar's color (the legend
 * above already names the calendar, so the fill is enough - no need to
 * repeat the calendar name in the text). All-day events just show the
 * summary; timed events append "start - end" after the summary so it all
 * still fits on the box's single line. */
static void add_item(lv_obj_t *items_col, const gcal_event_t *ev)
{
    lv_obj_t *item = lv_obj_create(items_col);
    lv_obj_remove_style_all(item);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, ITEM_H);
    lv_obj_set_style_radius(item, 6, 0);
    lv_obj_set_style_bg_color(item, ui_color(ev->color), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    char text[160];
    if (ev->all_day) {
        snprintf(text, sizeof(text), "%s", ev->summary);
    } else {
        char start_s[16], end_s[16];
        format_time12(ev->start, start_s, sizeof(start_s));
        format_time12(ev->end, end_s, sizeof(end_s));
        snprintf(text, sizeof(text), "%s    %s - %s", ev->summary, start_s, end_s);
    }

    lv_obj_t *label = lv_label_create(item);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(label, LV_PCT(100), ITEM_H - 8);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_text_font(label, &gcal_font_14, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, text);
}

/* True if ev falls on local calendar day `day` (a value from
 * ui_start_of_day()) - a multi-day all-day event (e.g. a 3-day trip) is
 * active on every one of those days, not just the day it starts, so it
 * should be listed under each one. end is exclusive, matching how
 * event_store_copy_range() and parse_date_only() already treat all-day
 * event boundaries elsewhere in this app. */
static bool event_active_on_day(const gcal_event_t *ev, time_t day)
{
    if (ev->all_day) {
        return ev->start <= day && day < ev->end;
    }
    return ui_same_local_day(ev->start, day);
}

void ui_upnext_release(void)
{
    lv_obj_clean(s_list);
}

void ui_upnext_populate(lv_obj_t *root)
{
    (void)root;
    lv_obj_clean(s_list);

    time_t now;
    time(&now);

    /* static, not a stack local - see the matching comment in ui_month.c's
     * populate function for why. */
    static gcal_event_t events[64];
    int n = event_store_copy_upcoming(now, events, 64);

    /* Sweep forward day by day (rather than event by event) so a
     * multi-day all-day event gets re-listed under every day it spans.
     * Bounded by the furthest event end actually in the data, not an
     * arbitrary day count. */
    time_t day = ui_start_of_day(now);
    time_t last_day = day;
    for (int i = 0; i < n; i++) {
        if (events[i].end > last_day) {
            last_day = events[i].end;
        }
    }

    int shown = 0;
    while (day < last_day && shown < MAX_UPNEXT_ROWS) {
        lv_obj_t *group = NULL;
        lv_obj_t *items_col = NULL;

        for (int i = 0; i < n && shown < MAX_UPNEXT_ROWS; i++) {
            if (!ui_calendar_enabled(events[i].calendar_index)) {
                continue;
            }
            if (!event_active_on_day(&events[i], day)) {
                continue;
            }

            if (group == NULL) {
                group = lv_obj_create(s_list);
                lv_obj_remove_style_all(group);
                lv_obj_set_width(group, LV_PCT(100));
                lv_obj_set_height(group, LV_SIZE_CONTENT);
                lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
                lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t *date_lbl = lv_label_create(group);
                lv_obj_set_width(date_lbl, DATE_COL_W);
                lv_label_set_long_mode(date_lbl, LV_LABEL_LONG_CLIP);
                lv_obj_set_style_text_font(date_lbl, &gcal_font_14, 0);
                lv_obj_set_style_text_color(date_lbl, ui_color(UI_COLOR_TEXT_MUTED), 0);
                char day_buf[24];
                format_day(day, now, day_buf, sizeof(day_buf));
                lv_label_set_text(date_lbl, day_buf);

                items_col = lv_obj_create(group);
                lv_obj_remove_style_all(items_col);
                lv_obj_set_flex_grow(items_col, 1);
                lv_obj_set_height(items_col, LV_SIZE_CONTENT);
                lv_obj_set_flex_flow(items_col, LV_FLEX_FLOW_COLUMN);
                lv_obj_set_style_pad_row(items_col, 6, 0);
                lv_obj_clear_flag(items_col, LV_OBJ_FLAG_SCROLLABLE);
            }

            add_item(items_col, &events[i]);
            shown++;
        }

        day = ui_add_days(day, 1);
    }

    if (shown == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "Nothing coming up.");
        lv_obj_set_style_text_color(empty, ui_color(UI_COLOR_TEXT_MUTED), 0);
    }
}

void ui_upnext_title(char *out, size_t out_sz)
{
    snprintf(out, out_sz, "Up next");
}
