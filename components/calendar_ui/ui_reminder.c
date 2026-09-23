/* Event-reminder pop-over: shows an event's details starting 15 minutes
 * before it's due, staying up until it's dismissed (tap anywhere on the
 * card, or its small close glyph) or the event's own start time passes -
 * whichever comes first, at which point it's gone for good (no re-showing
 * the same event later in its own window).
 *
 * Only timed (non-all-day) events on a currently-enabled calendar
 * qualify. All-day events - which includes every task synced in from the
 * "Task Sync" calendar (see README's Google Tasks bonus section) - are
 * deliberately excluded for now: Google Tasks only ever exposes a due
 * *date*, not a time, so "15 min before" has no real meaning for one yet.
 *
 * The pop-over itself is only ever shown while the calendar is actually
 * on screen (ui_reminder_tick()'s `calendar_visible` argument, driven by
 * ui_screensaver.c's own display-state machine) - while the ambient clock
 * or the sleep screen is up instead, this stays hidden and ui_clock.c
 * shows a small bell indicator (see ui_reminder_has_pending()) rather
 * than interrupting either of those. */
#include "calendar_ui_internal.h"
#include "ui_theme.h"
#include "event_store.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ui_reminder";

#define REMIND_BEFORE_S (15 * 60)
#define SCAN_MAX_EVENTS 16
#define MAX_DISMISSED   8

/* No persistent event ID exists in gcal_event_t, but (calendar_index,
 * start) is a fine enough stand-in in practice - two events on the same
 * calendar starting at the exact same second never happens for anything
 * this reminder cares about (timed events on a real calendar). */
typedef struct {
    uint8_t calendar_index;
    time_t start;
} reminder_key_t;

static lv_obj_t *s_card;
static lv_obj_t *s_close_label;
static lv_obj_t *s_title_label;
static lv_obj_t *s_meta_label;

static reminder_key_t s_dismissed[MAX_DISMISSED];
static int s_dismissed_count;

static reminder_key_t s_shown_key; /* identity of whatever the card is currently populated with */
static bool s_shown_valid;
static bool s_pending; /* true whenever *any* undismissed candidate exists right now,
                           independent of whether the card itself is allowed to be
                           visible - see ui_reminder_has_pending() */

static bool key_equal(const reminder_key_t *a, const reminder_key_t *b)
{
    return a->calendar_index == b->calendar_index && a->start == b->start;
}

static bool is_dismissed(const reminder_key_t *key)
{
    for (int i = 0; i < s_dismissed_count; i++) {
        if (key_equal(&s_dismissed[i], key)) {
            return true;
        }
    }
    return false;
}

static void remember_dismissed(const reminder_key_t *key)
{
    /* Drop the oldest entry to make room rather than refusing to track a
     * new one - a stale "dismissed" record for an event whose 15-minute
     * window has long since closed is harmless either way (nothing will
     * ever match it again), so simply cycling the array is enough. */
    if (s_dismissed_count >= MAX_DISMISSED) {
        memmove(&s_dismissed[0], &s_dismissed[1], sizeof(s_dismissed[0]) * (MAX_DISMISSED - 1));
        s_dismissed_count = MAX_DISMISSED - 1;
    }
    s_dismissed[s_dismissed_count++] = *key;
}

/* Clears s_shown_valid, logging only if something actually was showing -
 * called both when the event's own start time has passed (it ages out of
 * find_candidate()'s results naturally, since event_store_copy_upcoming()
 * only ever returns events with start >= now) and when the display
 * leaves the calendar view entirely. Logging only on that true->false
 * transition, not every tick, is what keeps this from spamming a line a
 * second during the routine "nothing to show right now" steady state. */
static void clear_shown(const char *reason)
{
    if (!s_shown_valid) {
        return;
    }
    ESP_LOGI(TAG, "auto-cleared, %s (calendar_index=%u start=%lld)", reason,
             s_shown_key.calendar_index, (long long)s_shown_key.start);
    s_shown_valid = false;
}

static void card_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (!s_shown_valid) {
        return;
    }
    ESP_LOGI(TAG, "dismissed (calendar_index=%u start=%lld)",
             s_shown_key.calendar_index, (long long)s_shown_key.start);
    remember_dismissed(&s_shown_key);
    s_shown_valid = false;
    s_pending = false;
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
}

void ui_reminder_init(lv_obj_t *parent)
{
    s_card = lv_obj_create(parent);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, 420, 110);
    /* Under the top bar + legend, hugging the right edge - clear of the
     * nav rail on the left and of whatever the current view is doing
     * underneath, without covering the whole screen (this is meant to
     * read as a non-blocking notification, not a modal dialog). */
    lv_obj_align(s_card, LV_ALIGN_TOP_RIGHT, -16, UI_TOP_BAR_H + UI_LEGEND_H + 12);
    lv_obj_set_style_bg_color(s_card, ui_color(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 10, 0);
    /* A solid stripe in the event's own calendar colour along the left
     * edge - the same "which calendar is this" cue the legend's dot and
     * every event chip/bar elsewhere already give, readable before even
     * getting to the text. Colour itself is set per-event in
     * ui_reminder_tick(). */
    lv_obj_set_style_border_side(s_card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(s_card, 6, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    /* Explicitly clickable, with the whole card (not just the close
     * glyph) dismissing it - a wall display has no cursor to hover a
     * small "x" precisely, so the big, forgiving hit area matters more
     * here than it would with a mouse. The glyph stays as a visible
     * affordance, it just isn't the only way to dismiss. */
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_card, card_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_close_label = lv_label_create(s_card);
    lv_label_set_text(s_close_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(s_close_label, &gcal_font_14, 0);
    lv_obj_set_style_text_color(s_close_label, ui_color(UI_COLOR_TEXT_FAINT), 0);
    lv_obj_align(s_close_label, LV_ALIGN_TOP_RIGHT, -8, 8);

    s_title_label = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_title_label, &gcal_font_20, 0);
    lv_obj_set_style_text_color(s_title_label, ui_color(UI_COLOR_TEXT), 0);
    lv_obj_set_width(s_title_label, 380);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_title_label, 18, 14);

    s_meta_label = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_meta_label, &gcal_font_14, 0);
    lv_obj_set_style_text_color(s_meta_label, ui_color(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_width(s_meta_label, 380);
    lv_label_set_long_mode(s_meta_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_meta_label, 18, 48);
}

/* Finds the soonest-starting timed event, on a currently-enabled
 * calendar, that's due within REMIND_BEFORE_S and hasn't been dismissed.
 * event_store_copy_upcoming() returns events sorted by start time
 * ascending, so the scan can stop the instant it reaches one whose start
 * is already further out than the reminder window - nothing later in the
 * list could possibly be closer. */
static bool find_candidate(time_t now, gcal_event_t *out)
{
    static gcal_event_t buf[SCAN_MAX_EVENTS];
    int n = event_store_copy_upcoming(now, buf, SCAN_MAX_EVENTS);

    for (int i = 0; i < n; i++) {
        gcal_event_t *ev = &buf[i];
        if (ev->start - now > REMIND_BEFORE_S) {
            break;
        }
        if (ev->all_day || !ui_calendar_enabled(ev->calendar_index)) {
            continue;
        }
        reminder_key_t key = {ev->calendar_index, ev->start};
        if (is_dismissed(&key)) {
            continue;
        }
        *out = *ev;
        return true;
    }
    return false;
}

void ui_reminder_tick(bool calendar_visible)
{
    if (s_card == NULL) {
        return; /* ui_reminder_init() hasn't run yet */
    }

    time_t now = time(NULL);

    /* Prune dismissed entries whose event has already started - keeps
     * the array small and self-cleaning without needing any explicit
     * "forget this" call from anywhere else. */
    int kept = 0;
    for (int i = 0; i < s_dismissed_count; i++) {
        if (s_dismissed[i].start > now) {
            s_dismissed[kept++] = s_dismissed[i];
        }
    }
    s_dismissed_count = kept;

    gcal_event_t candidate;
    bool found = find_candidate(now, &candidate);
    s_pending = found;

    if (!calendar_visible) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        if (!found) {
            clear_shown("start time passed (while calendar wasn't on screen)");
        }
        return;
    }

    if (!found) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        clear_shown("start time passed");
        return;
    }

    reminder_key_t key = {candidate.calendar_index, candidate.start};
    if (!s_shown_valid || !key_equal(&s_shown_key, &key)) {
        ESP_LOGI(TAG, "showing \"%s\" (calendar_index=%u start=%lld, in %lds)",
                 candidate.summary, candidate.calendar_index, (long long)candidate.start,
                 (long)(candidate.start - now));
        lv_label_set_text(s_title_label, candidate.summary);
        lv_obj_set_style_border_color(s_card, ui_color(candidate.color), 0);
        s_shown_key = key;
        s_shown_valid = true;
    }

    /* Re-formatted every tick (unlike the title/border above) so "in N
     * min" stays accurate for as long as the card is up. */
    struct tm start_tm, end_tm;
    localtime_r(&candidate.start, &start_tm);
    localtime_r(&candidate.end, &end_tm);
    int mins_left = (int)((candidate.start - now) / 60);
    if (mins_left < 0) {
        mins_left = 0;
    }
    /* U+2022 (bullet) as a separator, not U+00B7 (middle dot) - gcal_font_14
     * was generated with 0x2022 in its glyph range (see its own header
     * comment's Opts line) but not 0xB7, so a middle dot would render as a
     * missing/blank glyph on real hardware. */
    char meta[96];
    snprintf(meta, sizeof(meta), "in %d min \xE2\x80\xA2 %02d:%02d-%02d:%02d \xE2\x80\xA2 %s",
             mins_left, start_tm.tm_hour, start_tm.tm_min, end_tm.tm_hour, end_tm.tm_min,
             candidate.calendar_label);
    lv_label_set_text(s_meta_label, meta);

    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
}

bool ui_reminder_has_pending(void)
{
    return s_pending;
}
