/* Renders the real calendar UI with fictional data to PPM files (see Makefile / topng.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lvgl.h"
#include "app_settings.h"
#include "calendar_ui.h"
#include "calendar_ui_internal.h"
#include "demo.h"
#include "rss_feed.h"

#define W 800
#define H 480

void host_select_view(int v);
void host_set_cursor(time_t t);
void host_go_ambient(void);
void host_open_settings_menu(void);
bool host_detail_show(const char *summary_part);
void host_close_top(void);
void host_go_calendar(void);

static lv_color_t s_draw[W * H];
static lv_color_t s_fb[W * H];
static app_settings_t s_cfg;

/* A simulated touch screen, so scenes can be driven through LVGL's own hit-testing - a direct call into a card
 * can't catch "a touch on this label goes to the list instead" (which is how a tap on a feed story once closed the list). */
static bool s_ptr_down;
static int s_ptr_x, s_ptr_y;
static int s_failures;

static void ptr_read(lv_indev_drv_t *d, lv_indev_data_t *data)
{
    (void)d;
    data->point.x = s_ptr_x;
    data->point.y = s_ptr_y;
    data->state = s_ptr_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        s_failures++;
    }
}
static const char *s_out = "out";

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    int w = lv_area_get_width(a);
    for (int y = a->y1; y <= a->y2; y++) {
        memcpy(&s_fb[y * W + a->x1], &px[(y - a->y1) * w], (size_t)w * sizeof(lv_color_t));
    }
    lv_disp_flush_ready(drv);
}

static void settle(int ms)
{
    for (int t = 0; t < ms; t += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
    lv_refr_now(NULL);
}

static const char *s_suffix = "";     /* "_24h" on the second (24-hour clock style) run */

static void snap(const char *name)
{
    settle(300);
    char path[256];
    snprintf(path, sizeof(path), "%s/%s%s.ppm", s_out, name, s_suffix);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        lv_color32_t c;
        c.full = lv_color_to32(s_fb[i]);
        fputc(c.ch.red, f);
        fputc(c.ch.green, f);
        fputc(c.ch.blue, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

/* the topmost overlay on lv_layer_top(), and its scrolling list (the last child) */
static lv_obj_t *top_overlay(void)
{
    lv_obj_t *top = lv_layer_top();
    uint32_t n = lv_obj_get_child_cnt(top);
    return n ? lv_obj_get_child(top, n - 1) : NULL;
}

void host_radar_show(const char *base_map_path);

static void host_tap(int x, int y)
{
    s_ptr_x = x;
    s_ptr_y = y;
    s_ptr_down = true;
    settle(80);
    s_ptr_down = false;
    settle(250);
}

/* The taps that matter on the news-feed and On-this-day pages, as real touches. */
static void tap_tests(void)
{
    /* feed: a story opens its card; Close returns to the list; a tap outside the card does too; the heading closes the list */
    ui_feed_open(0);
    settle(300);
    host_tap(120, 74);
    check(ui_link_is_open() && ui_feed_is_open(), "feed: touching a story's heading opens its link card (list stays)");
    host_tap(628, 398);
    check(!ui_link_is_open() && ui_feed_is_open(), "feed: Close returns to the list");
    host_tap(300, 112);
    check(ui_link_is_open() && ui_feed_is_open(), "feed: touching a story's text opens its link card too");
    host_tap(30, 240);
    check(!ui_link_is_open() && ui_feed_is_open(), "feed: a tap outside the card closes only the card");
    host_tap(300, 34);
    check(!ui_feed_is_open(), "feed: a tap on the heading closes the list");
    ui_feed_open(0);
    settle(300);
    host_tap(765, 34);
    check(!ui_feed_is_open(), "feed: the close button closes the list");

    /* on this day: the same */
    ui_history_open();
    settle(300);
    host_tap(120, 74);
    check(ui_link_is_open() && ui_history_is_open(), "history: touching an entry opens its link card (list stays)");
    host_tap(628, 398);
    check(!ui_link_is_open() && ui_history_is_open(), "history: Close returns to the list");
    host_tap(300, 34);
    check(!ui_history_is_open(), "history: a tap on the heading closes the list");
    ui_history_open();
    settle(300);
    host_tap(765, 34);
    check(!ui_history_is_open(), "history: the close button closes the list");
    settle(300);
}

static void weather_shots(void)
{
    ui_weather_open();
    settle(400);
    lv_obj_t *ov = top_overlay();
    lv_obj_t *list = lv_obj_get_child(ov, lv_obj_get_child_cnt(ov) - 1);
    char name[64];
    /* page down in 380 px steps until the end of the list */
    for (int i = 0; i < 20; i++) {
        snprintf(name, sizeof(name), "weather_%d", i);
        snap(name);
        if (lv_obj_get_scroll_bottom(list) <= 0) {
            break;
        }
        lv_obj_scroll_to_y(list, lv_obj_get_scroll_top(list) + 380, LV_ANIM_OFF);
    }
    ui_weather_close();
    settle(300);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        s_out = argv[1];
    }
    bool clock_24h = argc > 2 && strcmp(argv[2], "24h") == 0;   /* ./render out 24h */
    if (clock_24h) {
        s_suffix = "_24h";
    }
    mkdir(s_out, 0755);
    setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1);
    tzset();

    /* Wed 14 October 2026, 14:35 - the fictional "now" (afternoon, so the week and day
     * views scroll to show the evening events) */
    demo_set_now(demo_local(2026, 10, 14, 14, 35));
    demo_fill_settings(&s_cfg);

    lv_init();
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, s_draw, NULL, W * H);
    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res = W;
    drv.ver_res = H;
    drv.flush_cb = flush_cb;
    drv.draw_buf = &draw_buf;
    drv.full_refresh = 1;
    lv_disp_drv_register(&drv);
    static lv_indev_drv_t idrv;
    lv_indev_drv_init(&idrv);
    idrv.type = LV_INDEV_TYPE_POINTER;
    idrv.read_cb = ptr_read;
    lv_indev_drv_register(&idrv);

    demo_fill_events();
    demo_build_data(0);
    /* A (fictional) PIN, so the top bar has its padlock - and the weather icon its real position. */
    snprintf(s_cfg.lock_pin, sizeof(s_cfg.lock_pin), "1234");
    s_cfg.clock_24h = clock_24h;      /* set before the UI is built, as on the device (a change restarts it) */
    calendar_ui_init(&s_cfg);
    settle(500);
    /* With a PIN set the screen starts locked, which shows only the clock: unlock it so the
     * calendar views are what gets rendered. */
    ui_screensaver_unlock();
    settle(500);

    host_select_view(UI_VIEW_MONTH);
    snap("month");
    host_select_view(UI_VIEW_WEEK);
    snap("week");
    host_set_cursor(demo_now());
    host_select_view(UI_VIEW_DAY);
    snap("day");
    host_select_view(UI_VIEW_UPNEXT);
    snap("upnext");

    /* The event details card, in each view that has it (Month is unchanged: a tap opens that day). */
    host_select_view(UI_VIEW_WEEK);
    host_detail_show("Physio");
    snap("detail_week");
    ui_detail_close();
    host_set_cursor(demo_now());
    host_select_view(UI_VIEW_DAY);
    host_detail_show("Dinner at Maya");
    snap("detail_day");
    ui_detail_close();
    host_detail_show("Return library");
    snap("detail_allday");
    ui_detail_close();
    host_select_view(UI_VIEW_UPNEXT);
    host_detail_show("Parent-teacher");
    snap("detail_upnext");
    ui_detail_close();
    settle(300);

    host_select_view(UI_VIEW_MONTH);
    host_open_settings_menu();
    snap("settings_menu");
    host_close_top();
    settle(300);

    weather_shots();
    tap_tests();

    /* The radar and satellite page: the real page code with a fictional rain field, satellite picture and map. */
    host_radar_show("../../components/calendar_ui/radar_base.rgb565");
    settle(400);
    snap("radar");
    ui_radar_close();
    settle(300);

    ui_history_open();
    snap("history");
    ui_history_close();
    settle(300);

    ui_feed_open(0);
    snap("feed");
    /* Touching the first item: its link card, with the QR code. */
    {
        const rss_feed_t *f = rss_feed_acquire(0);
        if (f != NULL && f->count > 0) {
            ui_link_show(f->items[0].title, f->items[0].url);
        }
        rss_feed_release();
    }
    settle(300);
    snap("link_card");
    ui_link_close();
    /* The longest link a feed can give (199 characters): the biggest code, at the smallest module size - not copied to docs/,
     * but decoded by a QR reader to check it scans. */
    {
        char url[200] = "https://news.example.org/port-hartley/";
        size_t n = strlen(url);
        for (int i = 0; n < 199; i++) {
            url[n++] = "abcdefghijklmnopqrstuvwxyz0123456789-"[(i * 7) % 37];
        }
        url[n] = '\0';
        ui_link_show("The longest link the card has to show: a story with an extremely long headline that wraps over several lines of the card", url);
        settle(300);
        snap("link_card_long");
        ui_link_close();
    }
    ui_feed_close();
    settle(300);

    ui_lock_keypad_open();
    snap("lock");
    ui_lock_keypad_close();
    settle(300);

    host_go_ambient();
    snap("clock_day");
    demo_set_now(demo_local(2026, 10, 14, 22, 41));
    demo_build_data(1);
    ui_clock_update();
    snap("clock_night");

    /* The weather panel after dark: Today's icon must be the night variant (not copied to docs/). */
    ui_weather_open();
    snap("weather_night");
    ui_weather_close();
    return s_failures ? 1 : 0;
}
