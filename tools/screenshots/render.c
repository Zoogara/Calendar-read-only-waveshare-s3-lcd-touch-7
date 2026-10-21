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

#define W 800
#define H 480

void host_select_view(int v);
void host_set_cursor(time_t t);
void host_go_ambient(void);
void host_go_calendar(void);

static lv_color_t s_draw[W * H];
static lv_color_t s_fb[W * H];
static app_settings_t s_cfg;
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

static void snap(const char *name)
{
    settle(300);
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.ppm", s_out, name);
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

    demo_fill_events();
    demo_build_data(0);
    /* A (fictional) PIN, so the top bar has its padlock - and the weather icon its real position. */
    snprintf(s_cfg.lock_pin, sizeof(s_cfg.lock_pin), "1234");
    calendar_ui_init(&s_cfg);
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

    weather_shots();

    ui_history_open();
    snap("history");
    ui_history_close();
    settle(300);

    ui_feed_open(0);
    snap("feed");
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
    return 0;
}
