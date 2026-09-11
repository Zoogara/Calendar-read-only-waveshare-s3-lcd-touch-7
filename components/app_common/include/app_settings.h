#pragma once
/*
 * Shared configuration model.
 *
 * This struct is populated once at boot (from NVS, written there by the
 * provisioning web form - see components/provisioning) and then read by
 * the Wi-Fi connect step, the Google Calendar client and the UI. Nothing
 * else needs to know how it got there.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SETTINGS_MAX_CALENDARS   8
#define APP_SETTINGS_MAX_STR         96
#define APP_SETTINGS_MAX_KEY         2200   /* PEM RSA private keys are ~1.7KB */
#define APP_SETTINGS_MAX_TZ          64
#define APP_SETTINGS_MAX_CAL_ID      220    /* fits a Google calendar ID (short)
                                                or an ICS feed URL (some hosts'
                                                secret/token URLs run long) */

typedef enum {
    APP_CAL_SOURCE_GOOGLE = 0, /* fetched via the service account + Calendar API,
                                   id is a Google calendar ID ("primary" or
                                   "abc123@group.calendar.google.com") */
    APP_CAL_SOURCE_ICS = 1,    /* fetched as a plain HTTPS GET, id is the feed's
                                   URL (e.g. a calendar's "Secret address in
                                   iCal format") - for calendars that can't be
                                   shared with the service account at all, even
                                   ones already imported into a Google
                                   calendar. Recurring (RRULE) events aren't
                                   expanded yet - see ics_client.c. */
} app_cal_source_t;

typedef struct {
    app_cal_source_t source;
    char id[APP_SETTINGS_MAX_CAL_ID];   /* meaning depends on source - see
                                            app_cal_source_t above */
    char label[40];                     /* Short name shown in the UI */
    uint32_t color;                     /* 0xRRGGBB, shown as the calendar's colour
                                            everywhere (month dots, week/day blocks,
                                            up-next strip, legend chip) */
    bool enabled;                       /* Toggleable from the on-screen legend */
    bool daily_only;                    /* When set, this calendar is only fetched
                                            once per local calendar day (on the first
                                            refresh cycle on or after local midnight,
                                            and once at startup) rather than every
                                            refresh_interval_s like normal - for feeds
                                            that barely ever change (a public holidays
                                            ICS, say) or a flaky third-party host not
                                            worth hammering. Its events are carried
                                            forward untouched on the cycles in
                                            between; see gcal_client.c's
                                            gcal_refresh_all(). A failed daily fetch
                                            still retries on the next regular cycle -
                                            the once-a-day quiet only kicks in after
                                            it has actually succeeded that day. */
} app_calendar_cfg_t;

typedef struct {
    /* Wi-Fi */
    char wifi_ssid[APP_SETTINGS_MAX_STR];
    char wifi_password[APP_SETTINGS_MAX_STR];

    /* Google service account (see README "Google Cloud setup").
     * private_key is the PEM block exactly as it appears in the
     * downloaded JSON key file's "private_key" field (with real
     * newlines, not "\n" escapes - the provisioning form does that
     * conversion for you). */
    char sa_client_email[APP_SETTINGS_MAX_STR];
    char sa_private_key_pem[APP_SETTINGS_MAX_KEY];

    /* Calendars to display, each with its own colour - mirrors how the
     * Google Calendar app lets you assign a colour per calendar. */
    app_calendar_cfg_t calendars[APP_SETTINGS_MAX_CALENDARS];
    uint8_t calendar_count;

    /* POSIX TZ string, e.g. "AEST-10AEDT,M10.1.0,M4.1.0/3" for
     * Australia/Melbourne. Used directly with setenv("TZ", ...); tzset(). */
    char posix_tz[APP_SETTINGS_MAX_TZ];

    /* How often to re-poll the Calendar API, in seconds. */
    uint32_t refresh_interval_s;

    /* HTTPS URL of a firmware .bin to fetch on a manual "Check for
     * update" (see the gear-icon settings dialog / ota_update.c).
     * Optional - left blank, that option just reports there's nothing
     * configured. */
    char ota_url[192];

    /* Idle time before the backlight turns off (see ui_screensaver.c).
     * 0 means never sleep. */
    uint32_t screen_timeout_s;

    /* Hour range (24h clock, start inclusive/end exclusive) shown in the
     * week/day views' timed-event grid - see HOUR_START/HOUR_END in
     * ui_day.c and ui_week.c. */
    uint8_t view_start_hour;
    uint8_t view_end_hour;

    /* How many days around "now" to keep fetched/cached (see
     * gcal_refresh_all()'s window_past_days/window_future_days) - wide
     * enough that paging around in month/week view doesn't need a fresh
     * fetch, but each extra day is more events to pull, parse and hold in
     * RAM every cycle. Configurable (config_web.c only, not the initial
     * setup portal - same as screen_timeout_s/view_*_hour below) so a
     * wide window can be dialed back if it turns out to cost more than
     * it's worth, without needing a firmware change either way. */
    uint16_t fetch_past_days;
    uint16_t fetch_future_days;

    /* Password for the runtime config web server (see config_web.c) -
     * everything configurable except Wi-Fi, reachable over the LAN while
     * the device is running normally (unlike the AP-mode setup portal,
     * which only runs before Wi-Fi is configured at all). Set via the
     * gear-icon Display Settings dialog. Left blank, the web server
     * doesn't start at all - there's no "no password" mode, since that
     * would mean anyone on the LAN could rewrite the calendar list. HTTP
     * Basic Auth checks only the password half of the challenge; the
     * username is ignored, so any (or no) username works. */
    char config_web_password[64];

    bool valid; /* true once loaded/saved successfully at least once */
} app_settings_t;

#define APP_SETTINGS_DEFAULT_REFRESH_S        300   /* 5 minutes */
#define APP_SETTINGS_DEFAULT_SCREEN_TIMEOUT_S  300   /* 5 minutes */
#define APP_SETTINGS_DEFAULT_VIEW_START_HOUR   6
#define APP_SETTINGS_DEFAULT_VIEW_END_HOUR     22
#define APP_SETTINGS_DEFAULT_FETCH_PAST_DAYS   14
#define APP_SETTINGS_DEFAULT_FETCH_FUTURE_DAYS 60

#ifdef __cplusplus
}
#endif
