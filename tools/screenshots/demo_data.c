/* Fictional data for the screenshot renderer - see demo.h. */
#include "demo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "event_store.h"
#include "gcal.h"
#include "ha_weather.h"
#include "onthisday.h"
#include "rss_feed.h"

static time_t s_now;
void demo_set_now(time_t now) { s_now = now; }
time_t demo_now(void) { return s_now; }

time_t demo_local(int y, int mo, int d, int h, int mi)
{
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_isdst = -1};
    return mktime(&tm);
}

/* ---------------- settings ---------------- */

static void cal(app_settings_t *c, int i, const char *label, uint32_t color)
{
    c->calendars[i].source = APP_CAL_SOURCE_GOOGLE;
    snprintf(c->calendars[i].id, sizeof(c->calendars[i].id), "demo-%d@example.invalid", i);
    snprintf(c->calendars[i].label, sizeof(c->calendars[i].label), "%s", label);
    c->calendars[i].color = color;
    c->calendars[i].enabled = true;
}

void demo_fill_settings(app_settings_t *c)
{
    memset(c, 0, sizeof(*c));
    cal(c, 0, "Family", 0x4285F4);
    cal(c, 1, "Bins", 0x3C8D2F);
    cal(c, 2, "Tasks", 0xD93025);
    cal(c, 3, "School", 0x8E63CE);
    cal(c, 4, "Holidays", 0xE8820C);
    c->calendar_count = 5;
    snprintf(c->posix_tz, sizeof(c->posix_tz), "AEST-10AEDT,M10.1.0,M4.1.0/3");
    c->refresh_interval_s = 300;
    c->screen_timeout_s = 300;
    c->view_start_hour = 7;
    c->view_end_hour = 20;
    c->fetch_past_days = 14;
    c->fetch_future_days = 60;
    snprintf(c->weather_place, sizeof(c->weather_place), "Port Hartley");
    snprintf(c->ha_base_url, sizeof(c->ha_base_url), "http://homeassistant.invalid:8123");
    snprintf(c->feed_label[0], sizeof(c->feed_label[0]), "Gazette");
    snprintf(c->feed_url[0], sizeof(c->feed_url[0]), "https://example.invalid/gazette.xml");
    snprintf(c->feed_label[1], sizeof(c->feed_label[1]), "Surf");
    snprintf(c->feed_url[1], sizeof(c->feed_url[1]), "https://example.invalid/surf.xml");
    snprintf(c->ha_graph_label, sizeof(c->ha_graph_label), "Backyard temperature");
    snprintf(c->ha_graph2_label, sizeof(c->ha_graph2_label), "Harbour (official)");
}

/* ---------------- events ---------------- */

static gcal_event_t *s_ev;
static int s_n;

static void ev(int idx, const char *summary, time_t a, time_t b, bool all_day)
{
    if (s_n >= 1500) {
        return;
    }
    gcal_event_t *e = &s_ev[s_n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->summary, sizeof(e->summary), "%s", summary);
    e->start = a;
    e->end = b;
    e->all_day = all_day;
    static const uint32_t col[5] = {0x4285F4, 0x3C8D2F, 0xD93025, 0x8E63CE, 0xE8820C};
    static const char *lab[5] = {"Family", "Bins", "Tasks", "School", "Holidays"};
    e->color = col[idx];
    snprintf(e->calendar_label, sizeof(e->calendar_label), "%s", lab[idx]);
    e->calendar_index = (uint8_t)idx;
}

/* y/m/d are local dates; h/mi the start; len minutes. */
static void timed(int idx, const char *s, int y, int mo, int d, int h, int mi, int len)
{
    time_t a = demo_local(y, mo, d, h, mi);
    ev(idx, s, a, a + len * 60, false);
}
static void allday(int idx, const char *s, int y, int mo, int d, int ndays)
{
    time_t a = demo_local(y, mo, d, 0, 0);
    time_t b = demo_local(y, mo, d + ndays, 0, 0);
    ev(idx, s, a, b, true);
}

void demo_fill_events(void)
{
    event_store_init();
    s_ev = event_store_begin_update();
    s_n = 0;

    /* weekly patterns from 28 Sep to 15 Nov 2026 */
    for (int off = -17; off <= 32; off++) {
        time_t t = demo_local(2026, 10, 1, 12, 0) + (time_t)off * 86400;
        struct tm lt;
        localtime_r(&t, &lt);
        int y = lt.tm_year + 1900, m = lt.tm_mon + 1, d = lt.tm_mday, wd = lt.tm_wday;   /* 0 = Sunday */
        int wk = (off + 40) / 7;                                                       /* alternating weeks */
        if (wd == 2) timed(0, "Netball training", y, m, d, 17, 30, 60);
        if (wd == 4) timed(0, "Piano lesson", y, m, d, 16, 0, 45);
        if (wd == 6) timed(0, "Market run", y, m, d, 8, 0, 90);
        if (wd == 0) timed(1, (wk & 1) ? "Recycling" : "Rubbish", y, m, d, 19, 30, 60);
        if (wd == 5 && (wk & 1)) timed(0, "Pasta night", y, m, d, 18, 30, 90);
    }
    /* one-offs - Family */
    timed(0, "Dentist", 2026, 10, 14, 9, 30, 45);
    timed(0, "Cafe catch-up with Jo", 2026, 10, 14, 12, 30, 60);
    timed(0, "Physio", 2026, 10, 14, 15, 30, 45);
    timed(0, "Dinner at Maya's", 2026, 10, 14, 18, 30, 150);
    allday(2, "\xE2\x98\x90 Return library books", 2026, 10, 14, 1);
    timed(0, "Parent-teacher chats", 2026, 10, 15, 15, 30, 60);
    allday(0, "Sam's birthday", 2026, 10, 17, 1);
    timed(0, "Vet - Biscuit", 2026, 10, 20, 11, 0, 30);
    allday(0, "Weekend at the coast", 2026, 10, 23, 3);
    timed(0, "Book club", 2026, 10, 30, 19, 30, 120);
    timed(0, "Halloween party", 2026, 10, 31, 17, 0, 240);
    timed(0, "Car service", 2026, 11, 2, 8, 0, 120);
    timed(0, "Cup Day barbecue", 2026, 11, 3, 12, 0, 180);
    timed(0, "Council meeting", 2026, 11, 10, 18, 30, 90);
    /* Tasks (the checkbox glyphs are in the font) */
    allday(2, "\xE2\x98\x91 Order school shoes", 2026, 10, 12, 1);
    allday(2, "\xE2\x98\x90 Renew car rego", 2026, 10, 16, 1);
    allday(2, "\xE2\x98\x90 Book dentist for kids", 2026, 10, 20, 1);
    allday(2, "\xE2\x98\x90 Call the plumber", 2026, 10, 22, 1);
    allday(2, "\xE2\x98\x90 Post parcel", 2026, 10, 27, 1);
    /* School */
    allday(3, "Term 4 starts", 2026, 10, 5, 1);
    timed(3, "School photo day", 2026, 10, 21, 9, 0, 120);
    allday(3, "Year 6 excursion", 2026, 10, 29, 1);
    allday(3, "Pupil-free day", 2026, 11, 2, 1);
    /* Holidays */
    allday(4, "Daylight saving starts", 2026, 10, 4, 1);
    allday(4, "Halloween", 2026, 10, 31, 1);
    allday(4, "Melbourne Cup", 2026, 11, 3, 1);
    allday(4, "Remembrance Day", 2026, 11, 11, 1);
    event_store_publish(s_n);
    event_store_mark_refreshed();
}

/* ---------------- weather and graphs ---------------- */

static ha_weather_t s_wx;
static ha_weather_details_t s_details;
static ha_history_t s_hist;
static ha_week_t s_week;
static ha_wind_t s_wind;
static ha_hourly_t s_hourly;
static onthisday_t s_otd;
static rss_feed_t s_feed[2];

static unsigned s_seed = 12345;
static float jit(float amp)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return ((float)((s_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 2.0f * amp;
}

static void rd(ha_reading_t *r, const char *v, const char *u)
{
    snprintf(r->value, sizeof(r->value), "%s", v);
    snprintf(r->unit, sizeof(r->unit), "%s", u);
}

static void day(int i, const char *txt, const char *icon, const char *lo, const char *hi, const char *rc, const char *rr)
{
    ha_forecast_day_t *d = &s_details.days[i];
    snprintf(d->short_text, sizeof(d->short_text), "%s", txt);
    snprintf(d->icon, sizeof(d->icon), "%s", icon);
    snprintf(d->lo, sizeof(d->lo), "%s", lo);
    snprintf(d->hi, sizeof(d->hi), "%s", hi);
    snprintf(d->rain_chance, sizeof(d->rain_chance), "%s", rc);
    snprintf(d->rain_range, sizeof(d->rain_range), "%s", rr);
}

static void item(int f, int i, const char *t, const char *x)
{
    rss_item_t *it = &s_feed[f].items[i];
    snprintf(it->title, sizeof(it->title), "%s", t);
    snprintf(it->text, sizeof(it->text), "%s", x);
    s_feed[f].count = i + 1;
}

static void otd(int i, int year, const char *title, const char *text)
{
    onthisday_entry_t *e = &s_otd.entries[i];
    e->year = (int16_t)year;
    snprintf(e->title, sizeof(e->title), "%s", title);
    snprintf(e->text, sizeof(e->text), "%s", text);
    s_otd.count = i + 1;
}

void demo_build_data(int night)
{
    time_t now = demo_now();
    struct tm lt;
    localtime_r(&now, &lt);

    /* ---- the clock's weather line ---- */
    memset(&s_wx, 0, sizeof(s_wx));
    s_wx.temp_c = night ? 13.8f : 22.6f;
    snprintf(s_wx.desc, sizeof(s_wx.desc), night ? "Clear" : "Mostly sunny");
    snprintf(s_wx.cond, sizeof(s_wx.cond), night ? "clear" : "mostly_sunny");
    s_wx.night = night ? 1 : 0;

    /* ---- details panel ---- */
    memset(&s_details, 0, sizeof(s_details));
    rd(&s_details.feels, "21.4", "\xC2\xB0" "C");
    rd(&s_details.wind, "14", "km/h");
    rd(&s_details.gust, "26", "km/h");
    rd(&s_details.wdir, "NE", "");
    rd(&s_details.rain, "0.0", "mm");
    rd(&s_details.pressure, "1018.2", "hPa");
    day(0, "Mostly sunny.", "mostly_sunny", "11", "24", "5", "0");
    day(1, "Partly cloudy. Slight chance of a shower in the afternoon.", "partly_cloudy", "12", "22", "30", "0\xE2\x80\x93" "1");
    day(2, "Showers developing.", "shower", "11", "18", "70", "2\xE2\x80\x93" "6");
    day(3, "Rain, heavy at times.", "rain", "10", "16", "90", "5\xE2\x80\x93" "12");
    day(4, "Mostly cloudy.", "cloudy", "9", "15", "20", "0");
    day(5, "Mostly sunny.", "mostly_sunny", "8", "17", "5", "0");
    day(6, "Sunny.", "sunny", "9", "20", "0", "0");
    snprintf(s_details.days[0].extended, sizeof(s_details.days[0].extended),
             "Mostly sunny. Light winds becoming northeasterly 15 to 20 km/h in the afternoon, "
             "then easing in the evening. Daytime maximum temperatures around 24.");
    snprintf(s_details.days[1].extended, sizeof(s_details.days[1].extended),
             "Partly cloudy. A slight chance of a shower in the afternoon, mainly near the "
             "ranges. Northeasterly winds 15 to 25 km/h tending light later. Overnight "
             "temperatures falling to around 12 with daytime temperatures reaching 22.");

    /* ---- 24-hour history: two temperature curves ---- */
    memset(&s_hist, 0, sizeof(s_hist));
    s_hist.end = now - now % 900;
    s_hist.step_s = 900;
    s_hist.count = 2;
    for (int k = 0; k < 2; k++) {
        ha_series_t *s = &s_hist.s[k];
        s->slot = k;
        s->min = 1e9f;
        s->max = -1e9f;
        for (int i = 0; i < HA_HISTORY_POINTS; i++) {
            time_t t = s_hist.end - (time_t)(HA_HISTORY_POINTS - 1 - i) * 900;
            struct tm tt;
            localtime_r(&t, &tt);
            float hr = tt.tm_hour + tt.tm_min / 60.0f;
            float v = 15.0f + 6.5f * sinf((hr - 9.5f) / 24.0f * 2.0f * (float)M_PI) + jit(k ? 0.1f : 0.25f) + (k ? 0.8f : 0.0f);
            s->v[i] = v;
            s->have++;
            if (v < s->min) { s->min = v; s->min_i = i; }
            if (v > s->max) { s->max = v; s->max_i = i; }
        }
        snprintf(s->entity, sizeof(s->entity), k ? "sensor.demo_harbour_temp" : "sensor.demo_backyard_temp");
        snprintf(s->name, sizeof(s->name), k ? "Harbour (official)" : "Backyard temperature");
        snprintf(s->unit, sizeof(s->unit), "\xC2\xB0" "C");
    }

    /* ---- 14-day bars ---- */
    memset(&s_week, 0, sizeof(s_week));
    static const float hi[14] = {21.3f, 17.8f, 20.4f, 24.4f, 21.7f, 25.0f, 27.0f, 16.8f, 18.2f, 19.8f, 22.3f, 22.0f, 23.6f, 24.0f};
    static const float lo[14] = {9.8f, 8.1f, 7.5f, 11.2f, 12.0f, 10.4f, 13.1f, 11.7f, 6.9f, 7.3f, 9.9f, 12.4f, 10.6f, 11.0f};
    static const float rn[14] = {0, 0, 0, 0, 0, 0, 2.4f, 6.8f, 1.2f, 0, 0, 0, 0, 0};
    s_week.day[13] = demo_local(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, 0, 0);
    for (int i = 12; i >= 0; i--) {
        s_week.day[i] = s_week.day[i + 1] - 86400 + (i == 12 ? 0 : 0);
    }
    for (int i = 0; i < HA_WEEK_DAYS; i++) {
        s_week.tmax[i] = hi[i];
        s_week.tmin[i] = lo[i];
        s_week.t_ok[i] = true;
        s_week.rain[i] = rn[i];
        s_week.r_day_ok[i] = true;
    }
    s_week.t_have = HA_WEEK_DAYS;
    s_week.r_ok = true;
    snprintf(s_week.t_unit, sizeof(s_week.t_unit), "\xC2\xB0" "C");
    snprintf(s_week.r_unit, sizeof(s_week.r_unit), "mm");

    /* ---- wind ---- */
    memset(&s_wind, 0, sizeof(s_wind));
    s_wind.end = s_hist.end;
    s_wind.gust_ok = s_wind.dir_ok = true;
    float mg = 0;
    for (int i = 0; i < HA_WIND_STEPS; i++) {
        float h = i / 4.0f;                                    /* hours since 24 h ago */
        float sp = 9.0f + 7.0f * expf(-powf((h - 20.0f) / 4.5f, 2)) + 3.0f * expf(-powf((h - 6.0f) / 3.0f, 2)) + jit(1.0f);
        if (sp < 1) sp = 1;
        s_wind.speed[i] = sp;
        s_wind.gust[i] = sp * (1.45f + 0.2f * fabsf(jit(1.0f)));
        if (s_wind.gust[i] > mg) { mg = s_wind.gust[i]; s_wind.max_gust_i = i; }
    }
    s_wind.max_gust = mg;
    for (int h = 0; h < HA_WIND_HOURS; h++) {
        s_wind.hr_dir[h] = fmodf(60.0f - h * 4.0f + (h > 14 ? (h - 14) * 9.0f : 0.0f) + jit(8.0f) + 360.0f, 360.0f);
        s_wind.hr_speed[h] = s_wind.speed[h * 4 + 2];
        s_wind.hr_calm[h] = (h >= 3 && h <= 5);
    }
    s_wind.now_speed = s_wind.speed[HA_WIND_STEPS - 1];
    s_wind.now_dir = 52.0f;
    s_wind.trend = 48.0f;
    snprintf(s_wind.unit, sizeof(s_wind.unit), "km/h");

    /* ---- hourly strip ---- */
    memset(&s_hourly, 0, sizeof(s_hourly));
    static const char *conds[24] = {"sunny", "sunny", "partlycloudy", "partlycloudy", "cloudy", "cloudy", "rainy", "rainy",
                                    "clear-night", "clear-night", "clear-night", "cloudy", "cloudy", "clear-night", "clear-night", "clear-night",
                                    "clear-night", "clear-night", "sunny", "sunny", "sunny", "sunny", "partlycloudy", "partlycloudy"};
    time_t h0 = now - now % 3600;
    for (int i = 0; i < HA_HOURLY_MAX; i++) {
        s_hourly.t[i] = h0 + i * 3600;
        struct tm tt;
        localtime_r(&s_hourly.t[i], &tt);
        float hr = (float)tt.tm_hour;
        s_hourly.temp[i] = 16.0f + 7.5f * sinf((hr - 9.5f) / 24.0f * 2.0f * (float)M_PI);
        snprintf(s_hourly.cond[i], sizeof(s_hourly.cond[i]), "%s", conds[i]);
        s_hourly.rain_pct[i] = (i == 6 || i == 7) ? 60 + i : (i >= 4 && i <= 5) ? 20 : 5;
        s_hourly.rain_mm[i] = (i == 6) ? 0.6f : (i == 7) ? 1.2f : 0.0f;
        s_hourly.night[i] = (tt.tm_hour >= 19 || tt.tm_hour < 6);
    }
    s_hourly.count = HA_HOURLY_MAX;

    /* ---- "on this day" (14 October: well-known history) ---- */
    memset(&s_otd, 0, sizeof(s_otd));
    s_otd.month = 10;
    s_otd.day = 14;
    otd(0, 1066, "Battle of Hastings", "William, Duke of Normandy, defeats Harold Godwinson, the last Anglo-Saxon king of England, near Hastings.");
    otd(1, 1884, "George Eastman", "George Eastman receives a US patent for his paper-strip photographic film.");
    otd(2, 1912, "Theodore Roosevelt", "Former US president Theodore Roosevelt is shot in Milwaukee while campaigning, but gives his speech anyway.");
    otd(3, 1926, "Winnie-the-Pooh", "A. A. Milne's Winnie-the-Pooh is first published.");
    otd(4, 1947, "Chuck Yeager", "Chuck Yeager becomes the first person to break the sound barrier, flying the Bell X-1.");
    otd(5, 1962, "Cuban Missile Crisis", "A U-2 reconnaissance flight photographs Soviet missile sites under construction in Cuba.");
    otd(6, 1964, "Martin Luther King Jr.", "Martin Luther King Jr. is awarded the Nobel Peace Prize.");
    otd(7, 2012, "Felix Baumgartner", "Felix Baumgartner jumps from a balloon almost 39 kilometres up and becomes the first person to break the sound barrier in freefall.");

    /* ---- two fictional feeds ---- */
    memset(s_feed, 0, sizeof(s_feed));
    snprintf(s_feed[0].name, sizeof(s_feed[0].name), "Port Hartley Gazette");
    item(0, 0, "Council backs new ferry timetable", "Weekend sailings will start an hour earlier from next month after a unanimous vote, with a trial of a late service on Fridays.");
    item(0, 1, "Fish market reopens after renovation", "The harbourside market is back with twice the stalls and a covered seating area, just in time for the long weekend.");
    item(0, 2, "Dune restoration volunteers wanted", "The beach care group is planting native grasses on Saturday morning and asks locals to bring gloves and a hat.");
    item(0, 3, "Library extends opening hours", "From Monday the library will open until 8 pm on weeknights, after a survey found most borrowers could not make it by five.");
    item(0, 4, "Lighthouse tours return for summer", "Tours of the old lighthouse resume on the first of November, with bookings limited to twelve people per climb.");
    item(0, 5, "Junior sailing club turns fifty", "Past and present members will gather at the clubhouse this weekend to mark half a century of Sunday morning races.");
    snprintf(s_feed[1].name, sizeof(s_feed[1].name), "Coast Surf Report");
    item(1, 0, "Swell building through the weekend", "A long-period southerly swell arrives Saturday afternoon, peaking Sunday at two to three metres with light offshore winds.");
    item(1, 1, "Water temperature 17 degrees", "Wetsuits still recommended, though the first warm days of spring have lifted the surface temperature a degree on last week.");
    item(1, 2, "Tide times for the week", "Low tide just after dawn midweek, with the biggest high of the month on Friday evening.");
}

/* ---------------- the acquire/release API the UI calls ---------------- */

bool ha_weather_get(ha_weather_t *out) { *out = s_wx; return true; }
const ha_weather_details_t *ha_weather_details_acquire(void) { return &s_details; }
void ha_weather_details_release(void) {}
const ha_history_t *ha_history_acquire(void) { return &s_hist; }
void ha_history_release(void) {}
const ha_week_t *ha_week_acquire(void) { return &s_week; }
void ha_week_release(void) {}
const ha_wind_t *ha_wind_acquire(void) { return &s_wind; }
void ha_wind_release(void) {}
const ha_hourly_t *ha_hourly_acquire(void) { return &s_hourly; }
void ha_hourly_release(void) {}
bool ha_graphs_incomplete(void) { return false; }
const onthisday_t *onthisday_acquire(void) { return &s_otd; }
void onthisday_release(void) {}
const rss_feed_t *rss_feed_acquire(int idx) { return (idx >= 0 && idx < 2) ? &s_feed[idx] : NULL; }
void rss_feed_release(void) {}
