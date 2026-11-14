/* ics_client_fetch(): the real iCalendar parser, fed canned text by fake_http.c. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "event_store.h"
#include "fake_http.h"
#include "ics_client.h"
#include "test.h"

#define MAXEV 8

static app_calendar_cfg_t cal;
static gcal_event_t ev[MAXEV];
static int count;

/* Wall-clock UTC -> epoch. */
static time_t utc(int y, int mo, int d, int h, int mi, int s)
{
    struct tm t = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s};
    return timegm(&t);
}

/* The whole of a year 2000..2100 unless a test narrows it. */
static time_t T_MIN, T_MAX;

static int fetch_max(const char *ics, int max)
{
    fake_http_serve(ics, 200, 0);
    memset(ev, 0, sizeof(ev));
    count = 0;
    return ics_client_fetch(fake_http_client(), &cal, 3, T_MIN, T_MAX, ev, &count, max);
}

static int fetch(const char *ics)
{
    return fetch_max(ics, MAXEV);
}

#define WRAP(body) "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" body "END:VCALENDAR\r\n"
#define EVENT(props) "BEGIN:VEVENT\r\nUID:1\r\n" props "END:VEVENT\r\n"

static void utc_event(void)
{
    CHECK(fetch(WRAP(EVENT("SUMMARY:Team sync\r\nDTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n"))));
    CHECK_INT(count, 1);
    CHECK_STR(ev[0].summary, "Team sync");
    CHECK_INT(ev[0].start, utc(2026, 10, 14, 1, 0, 0));
    CHECK_INT(ev[0].end, utc(2026, 10, 14, 2, 0, 0));
    CHECK(!ev[0].all_day);
    CHECK_INT(ev[0].color, 0x3C8D2F);
    CHECK_STR(ev[0].calendar_label, "Family");
    CHECK_INT(ev[0].calendar_index, 3);
}

static void all_day_event(void)
{
    /* The display's zone (set in test_ics) is AEST/AEDT; DST began on 4 Oct 2026, so +11. */
    CHECK(fetch(WRAP(EVENT("SUMMARY:Holiday\r\nDTSTART;VALUE=DATE:20261015\r\nDTEND;VALUE=DATE:20261016\r\n"))));
    CHECK_INT(count, 1);
    CHECK(ev[0].all_day);
    CHECK_INT(ev[0].start, utc(2026, 10, 14, 13, 0, 0));
    CHECK_INT(ev[0].end, utc(2026, 10, 15, 13, 0, 0));
}

static void local_time_is_the_displays_zone(void)
{
    /* With or without a TZID, a bare local time is read in the display's own zone. */
    CHECK(fetch(WRAP(EVENT("SUMMARY:A\r\nDTSTART;TZID=Australia/Melbourne:20261014T090000\r\n"
                           "DTEND;TZID=Australia/Melbourne:20261014T100000\r\n"))));
    CHECK_INT(count, 1);
    CHECK_INT(ev[0].start, utc(2026, 10, 13, 22, 0, 0));
    CHECK_INT(ev[0].end, utc(2026, 10, 13, 23, 0, 0));
    CHECK(!ev[0].all_day);

    CHECK(fetch(WRAP(EVENT("SUMMARY:B\r\nDTSTART:20261014T090000\r\nDTEND:20261014T100000\r\n"))));
    CHECK_INT(count, 1);
    CHECK_INT(ev[0].start, utc(2026, 10, 13, 22, 0, 0));
}

static void explicit_date_time_value_type(void)
{
    /* "VALUE=DATE-TIME" is the default type spelled out - still a timed event. */
    CHECK(fetch(WRAP(EVENT("SUMMARY:Spelled out\r\nDTSTART;VALUE=DATE-TIME:20261014T010000Z\r\n"
                           "DTEND;VALUE=DATE-TIME:20261014T020000Z\r\n"))));
    CHECK_INT(count, 1);
    CHECK(!ev[0].all_day);
    CHECK_INT(ev[0].start, utc(2026, 10, 14, 1, 0, 0));
    CHECK_INT(ev[0].end, utc(2026, 10, 14, 2, 0, 0));
}

static void skipped_events(void)
{
    CHECK(fetch(WRAP(
        EVENT("SUMMARY:Weekly\r\nRRULE:FREQ=WEEKLY\r\nDTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n")
        EVENT("SUMMARY:Cancelled\r\nSTATUS:CANCELLED\r\nDTSTART:20261014T030000Z\r\nDTEND:20261014T040000Z\r\n")
        EVENT("SUMMARY:No end\r\nDTSTART:20261014T050000Z\r\n")
        EVENT("SUMMARY:Keeper\r\nDTSTART:20261014T060000Z\r\nDTEND:20261014T070000Z\r\n"))));
    CHECK_INT(count, 1);
    CHECK_STR(ev[0].summary, "Keeper");
}

static void folded_lines(void)
{
    CHECK(fetch(WRAP(EVENT("SUMMARY:Quarterly planning mee\r\n ting with the\r\n\t team\r\n"
                           "DTSTART:20261014T01\r\n 0000Z\r\nDTEND:20261014T020000Z\r\n"))));
    CHECK_INT(count, 1);
    CHECK_STR(ev[0].summary, "Quarterly planning meeting with the team");
    CHECK_INT(ev[0].start, utc(2026, 10, 14, 1, 0, 0));
}

static void text_escapes(void)
{
    CHECK(fetch(WRAP(EVENT("SUMMARY:Lunch\\, then walk\\; bring\\nsnacks \\\\ done\r\n"
                           "DTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n"))));
    CHECK_INT(count, 1);
    CHECK_STR(ev[0].summary, "Lunch, then walk; bring snacks \\ done");
}

static void property_parameters_and_case(void)
{
    CHECK(fetch(WRAP(EVENT("SUMMARY;LANGUAGE=en:Hello\r\nDTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n"))));
    CHECK_INT(count, 1);
    CHECK_STR(ev[0].summary, "Hello");

    CHECK(fetch("begin:vcalendar\r\nbegin:vevent\r\nsummary:lower\r\ndtstart:20261014T010000Z\r\n"
                "dtend:20261014T020000Z\r\nend:vevent\r\nend:vcalendar\r\n"));
    CHECK_INT(count, 1);
    CHECK_STR(ev[0].summary, "lower");
}

static void missing_title(void)
{
    CHECK(fetch(WRAP(EVENT("DTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n"))));
    CHECK_INT(count, 1);
    CHECK_STR(ev[0].summary, "(No title)");
}

static void line_endings(void)
{
    CHECK(fetch("BEGIN:VCALENDAR\nBEGIN:VEVENT\nSUMMARY:LF only\nDTSTART:20261014T010000Z\nDTEND:20261014T020000Z\n"
                "END:VEVENT\nEND:VCALENDAR\n"));
    CHECK_INT(count, 1);
    CHECK_STR(ev[0].summary, "LF only");
}

static void alarm_inside_event(void)
{
    CHECK(fetch(WRAP(EVENT("SUMMARY:With alarm\r\nDTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n"
                           "BEGIN:VALARM\r\nTRIGGER:-PT15M\r\nACTION:DISPLAY\r\nDESCRIPTION:Reminder\r\nEND:VALARM\r\n"))));
    CHECK_INT(count, 1);
    CHECK_STR(ev[0].summary, "With alarm");
}

static void time_window(void)
{
    T_MIN = utc(2026, 10, 14, 0, 0, 0);
    T_MAX = utc(2026, 10, 15, 0, 0, 0);
    CHECK(fetch(WRAP(
        EVENT("SUMMARY:Before\r\nDTSTART:20261013T100000Z\r\nDTEND:20261013T110000Z\r\n")
        EVENT("SUMMARY:Overlaps start\r\nDTSTART:20261013T230000Z\r\nDTEND:20261014T010000Z\r\n")
        EVENT("SUMMARY:Inside\r\nDTSTART:20261014T050000Z\r\nDTEND:20261014T060000Z\r\n")
        EVENT("SUMMARY:Starts at end\r\nDTSTART:20261015T000000Z\r\nDTEND:20261015T010000Z\r\n")
        EVENT("SUMMARY:After\r\nDTSTART:20261016T000000Z\r\nDTEND:20261016T010000Z\r\n"))));
    CHECK_INT(count, 2);
    CHECK_STR(ev[0].summary, "Overlaps start");
    CHECK_STR(ev[1].summary, "Inside");
    T_MIN = utc(2000, 1, 1, 0, 0, 0);
    T_MAX = utc(2100, 1, 1, 0, 0, 0);
}

static void output_cap_and_existing_events(void)
{
    const char *three = WRAP(
        EVENT("SUMMARY:One\r\nDTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n")
        EVENT("SUMMARY:Two\r\nDTSTART:20261014T030000Z\r\nDTEND:20261014T040000Z\r\n")
        EVENT("SUMMARY:Three\r\nDTSTART:20261014T050000Z\r\nDTEND:20261014T060000Z\r\n"));
    CHECK(fetch_max(three, 2));
    CHECK_INT(count, 2);
    CHECK_STR(ev[1].summary, "Two");

    /* Events from earlier calendars are kept and counted. */
    fake_http_serve(three, 200, 0);
    memset(ev, 0, sizeof(ev));
    strcpy(ev[0].summary, "Earlier");
    count = 1;
    CHECK(ics_client_fetch(fake_http_client(), &cal, 3, T_MIN, T_MAX, ev, &count, 3));
    CHECK_INT(count, 3);
    CHECK_STR(ev[0].summary, "Earlier");
    CHECK_STR(ev[1].summary, "One");
    CHECK_STR(ev[2].summary, "Two");
}

static void failures_leave_events_alone(void)
{
    memset(ev, 0, sizeof(ev));
    count = 0;
    fake_http_serve("", 404, 0);
    CHECK(!ics_client_fetch(fake_http_client(), &cal, 0, T_MIN, T_MAX, ev, &count, MAXEV));
    CHECK_INT(count, 0);
    fake_http_serve("", 200, -1);
    CHECK(!ics_client_fetch(fake_http_client(), &cal, 0, T_MIN, T_MAX, ev, &count, MAXEV));
    CHECK_INT(count, 0);
    /* a 200 with no calendar in it is just an empty calendar */
    CHECK(fetch("<html>not a calendar</html>"));
    CHECK_INT(count, 0);
}

static void very_long_summary_is_cut(void)
{
    char ics[1024], long_title[300];
    memset(long_title, 'L', sizeof(long_title) - 1);
    long_title[sizeof(long_title) - 1] = '\0';
    snprintf(ics, sizeof(ics), WRAP(EVENT("SUMMARY:%s\r\nDTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n")),
             long_title);
    CHECK(fetch(ics));
    CHECK_INT(count, 1);
    CHECK_INT(strlen(ev[0].summary), GCAL_MAX_SUMMARY - 1);
}

void fake_store_reset(void);

static char loc_out[GCAL_MAX_LOCATION], desc_out[GCAL_MAX_DESCRIPTION];

static bool details_of(int i)
{
    return event_store_detail_get(ev[i].detail, ev[i].detail_gen, loc_out, sizeof(loc_out), desc_out,
                                  sizeof(desc_out));
}

static void location_and_description(void)
{
    fake_store_reset();
    CHECK(fetch(WRAP(EVENT("SUMMARY:Picnic\r\nLOCATION:Town Hall\\, 1 Main St\r\n"
                           "DESCRIPTION:Bring a plate.\\nSee you there\\; rain or shine.\r\n"
                           "DTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n"))));
    CHECK_INT(count, 1);
    CHECK(details_of(0));
    CHECK_STR(loc_out, "Town Hall, 1 Main St");
    CHECK_STR(desc_out, "Bring a plate.\nSee you there; rain or shine.");     /* \n kept as a line break */
}

static void html_in_a_description_is_stripped(void)
{
    fake_store_reset();
    CHECK(fetch(WRAP(EVENT("SUMMARY:Call\r\nDESCRIPTION:<p>Agenda</p><p>Item &amp; notes</p>\r\n"
                           "DTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n"))));
    CHECK(details_of(0));
    CHECK_STR(desc_out, "Agenda\nItem & notes");
}

static void a_long_folded_description_is_joined_and_cut(void)
{
    fake_store_reset();
    char ics[4096];
    int n = snprintf(ics, sizeof(ics), "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nSUMMARY:Long\r\nDESCRIPTION:start");
    for (int i = 0; i < 30; i++) {                          /* 30 folded lines of 70 characters */
        n += snprintf(ics + n, sizeof(ics) - (size_t)n, "\r\n %s", "0123456789012345678901234567890123456789012345678901234567890123456789");
    }
    snprintf(ics + n, sizeof(ics) - (size_t)n,
             "\r\nDTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    CHECK(fetch(ics));
    CHECK_INT(count, 1);
    CHECK(details_of(0));
    CHECK(strncmp(desc_out, "start0123456789", 15) == 0);       /* the folds were joined */
    CHECK(strlen(desc_out) < GCAL_MAX_DESCRIPTION);
    CHECK(memcmp(desc_out + strlen(desc_out) - 3, "\xE2\x80\xA6", 3) == 0);   /* cut with an ellipsis */
}

static void no_details_means_no_offset(void)
{
    fake_store_reset();
    CHECK(fetch(WRAP(EVENT("SUMMARY:Plain\r\nDTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n"))));
    CHECK_INT(count, 1);
    CHECK_INT(ev[0].detail, 0);
    CHECK(!details_of(0));
    CHECK_STR(loc_out, "");
}

static void only_a_location(void)
{
    fake_store_reset();
    CHECK(fetch(WRAP(EVENT("SUMMARY:Where\r\nLOCATION:Room 4\r\nDTSTART:20261014T010000Z\r\n"
                           "DTEND:20261014T020000Z\r\n"))));
    CHECK(details_of(0));
    CHECK_STR(loc_out, "Room 4");
    CHECK_STR(desc_out, "");
}

static void each_event_keeps_its_own_details(void)
{
    fake_store_reset();
    CHECK(fetch(WRAP(
        EVENT("SUMMARY:One\r\nLOCATION:Alpha\r\nDESCRIPTION:first\r\nDTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n")
        EVENT("SUMMARY:Two\r\nDTSTART:20261014T030000Z\r\nDTEND:20261014T040000Z\r\n")
        EVENT("SUMMARY:Three\r\nLOCATION:Gamma\r\nDESCRIPTION:third\r\nDTSTART:20261014T050000Z\r\nDTEND:20261014T060000Z\r\n"))));
    CHECK_INT(count, 3);
    CHECK(details_of(0));  CHECK_STR(loc_out, "Alpha");  CHECK_STR(desc_out, "first");
    CHECK(!details_of(1));                                                      /* nothing leaked from the first */
    CHECK(details_of(2));  CHECK_STR(loc_out, "Gamma");  CHECK_STR(desc_out, "third");
}

static void a_reminder_block_does_not_overwrite_the_event(void)
{
    fake_store_reset();
    CHECK(fetch(WRAP(EVENT("SUMMARY:Real title\r\nDESCRIPTION:Real description\r\n"
                           "DTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n"
                           "BEGIN:VALARM\r\nACTION:EMAIL\r\nSUMMARY:Alarm subject\r\n"
                           "DESCRIPTION:Alarm body\r\nTRIGGER:-PT15M\r\nEND:VALARM\r\n"))));
    CHECK_INT(count, 1);
    CHECK_STR(ev[0].summary, "Real title");
    CHECK(details_of(0));
    CHECK_STR(desc_out, "Real description");
}

static void a_description_before_the_reminder_block_survives_it(void)
{
    fake_store_reset();
    CHECK(fetch(WRAP(EVENT("BEGIN:VALARM\r\nDESCRIPTION:Alarm body\r\nEND:VALARM\r\n"
                           "SUMMARY:Title\r\nDESCRIPTION:Event body\r\n"
                           "DTSTART:20261014T010000Z\r\nDTEND:20261014T020000Z\r\n"))));
    CHECK(details_of(0));
    CHECK_STR(desc_out, "Event body");
}

void test_ics(void)
{
    setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1);
    tzset();
    cal.source = APP_CAL_SOURCE_ICS;
    snprintf(cal.id, sizeof(cal.id), "https://example.invalid/family.ics");
    snprintf(cal.label, sizeof(cal.label), "Family");
    cal.color = 0x3C8D2F;
    cal.enabled = true;
    T_MIN = utc(2000, 1, 1, 0, 0, 0);
    T_MAX = utc(2100, 1, 1, 0, 0, 0);

    RUN(utc_event);
    RUN(all_day_event);
    RUN(local_time_is_the_displays_zone);
    RUN(explicit_date_time_value_type);
    RUN(skipped_events);
    RUN(folded_lines);
    RUN(text_escapes);
    RUN(property_parameters_and_case);
    RUN(missing_title);
    RUN(line_endings);
    RUN(alarm_inside_event);
    RUN(time_window);
    RUN(output_cap_and_existing_events);
    RUN(failures_leave_events_alone);
    RUN(very_long_summary_is_cut);
    RUN(location_and_description);
    RUN(html_in_a_description_is_stripped);
    RUN(a_long_folded_description_is_joined_and_cut);
    RUN(no_details_means_no_offset);
    RUN(only_a_location);
    RUN(each_event_keeps_its_own_details);
    RUN(a_reminder_block_does_not_overwrite_the_event);
    RUN(a_description_before_the_reminder_block_survives_it);
}
