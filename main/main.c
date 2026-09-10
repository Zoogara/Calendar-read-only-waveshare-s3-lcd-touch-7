/*
 * Google Calendar display for the Waveshare ESP32-S3-Touch-LCD-7.
 *
 * Boot flow:
 *   1. Bring up the display/touch/LVGL.
 *   2. Load saved config from NVS. If there isn't any yet, show on-screen
 *      setup instructions and start the Wi-Fi-AP + web-form setup portal
 *      (provisioning_run_portal never returns - it reboots once the form
 *      is submitted). A gear icon in the top bar lets you re-enter this
 *      mode later too (see calendar_ui.c's settings button).
 *   3. Set the timezone, show the calendar UI immediately (empty, "not
 *      yet synced").
 *   4. In the background: connect Wi-Fi, sync time over SNTP, then
 *      loop forever: fetch all configured calendars, push into the
 *      event store, ask the UI to redraw, sleep, repeat.
 *
 * See README.md for the one-time Google Cloud / service-account setup
 * and for the first-boot Wi-Fi portal walkthrough.
 */

#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#include "lvgl.h"

#include "app_settings.h"
#include "provisioning.h"
#include "config_web.h"
#include "wifi_sta.h"
#include "board_bsp.h"
#include "sd_card.h"
#include "presence_sensor.h"
#include "calendar_ui.h"
#include "gcal_client.h"
#include "event_store.h"

static const char *TAG = "main";
static app_settings_t s_cfg;

/* provisioning_run_portal() never returns once it's decided to run, so the
 * normal boot path's bsp_display_init() call is never reached in that
 * case - without this, the screen just stays dark (no backlight, no LVGL)
 * for the device's entire time in setup mode, with zero on-device
 * indication it's even alive, let alone what Wi-Fi network/URL to use. */
static void show_provisioning_screen(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(label,
        "Setup required\n\n"
        "1. Connect to Wi-Fi:\n\"%s\"\n\n"
        "2. Browse to:\n192.168.4.1",
        PROVISIONING_AP_SSID);
    lv_obj_set_width(label, 600);
    lv_obj_center(label);
}

#define WIFI_CONNECT_TIMEOUT_MS 20000

/* A failed cycle waits this long before trying again, instead of the full
 * (much longer) refresh_interval_s - a refresh right after waking from a
 * long sleep can easily lose the race against Wi-Fi still re-associating
 * after being dropped by the AP, and without a short backoff that single
 * lost race meant both the wake-triggered sync and a manual force-sync tap
 * would silently do nothing until the next scheduled cycle, minutes later. */
#define RETRY_BACKOFF_S 20

/* How often keepalive_probe() runs during an otherwise idle gap between
 * sync cycles - see that function's own comment. */
#define KEEPALIVE_INTERVAL_MS (60U * 1000U)

/* Lightweight network keep-alive: resolves and opens (then immediately
 * closes) a plain TCP connection to the same host calendar sync talks
 * to, with no TLS handshake or data exchanged - just enough to exercise
 * the exact DNS + routing + TCP-connect path a real sync needs, roughly
 * once a minute during whatever of net_task's wait between sync cycles
 * is still otherwise completely silent (up to refresh_interval_s, 5
 * minutes by default - see net_task() below).
 *
 * Added 2026-09-09 chasing intermittent ESP_ERR_HTTP_CONNECT /
 * FETCH_HEADER / EAGAIN sync failures that recurred across a device
 * reset and a Wi-Fi channel change, at inconsistent points relative to
 * boot (once within 25 seconds, twice around 40 minutes in) - ruling out
 * both an accumulating on-device resource issue (internal RAM was
 * confirmed flat and healthy, ~46KB free, every single time, including
 * at the moment of failure) and a fixed uptime-based trigger. The
 * working theory: a connection sitting completely silent for minutes at
 * a stretch is more likely to hit router-side connection-tracking/ARP
 * staleness or a Wi-Fi power-save edge case than one with some regular
 * traffic on it - matching the observation that a neighbouring device
 * doing frequent updates never showed the same problem, and that entire
 * multi-hour stretches with the ambient clock or sleep screen showing
 * used to have literally zero network activity at all (see
 * calendar_ui_is_asleep()'s use in net_task() below, removed for the
 * same reason).
 *
 * Deliberately only logs on failure, not every success - a steady
 * stream of these failing right before a real sync failure would be a
 * strong confirming signal for the theory above; if they never fail even
 * when a real sync does, that's useful evidence against it too. Not
 * itself proven to fix anything yet - this is instrumented well enough
 * to tell either way from the log. */
static void keepalive_probe(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int gai_err = getaddrinfo("www.googleapis.com", "443", &hints, &res);
    if (gai_err != 0 || res == NULL) {
        ESP_LOGW(TAG, "keepalive: DNS resolve failed: %d", gai_err);
        return;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGW(TAG, "keepalive: socket() failed: errno %d", errno);
        freeaddrinfo(res);
        return;
    }

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "keepalive: connect() failed: errno %d", errno);
    }
    close(sock);
    freeaddrinfo(res);
}

static bool sync_time(void)
{
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync timed out on first try - date/times shown may "
                      "be wrong until it catches up in the background");
        return false;
    }
    ESP_LOGI(TAG, "time synced");
    return true;
}

static SemaphoreHandle_t s_wifi_up;

/* esp_wifi_init() - called once, inside wifi_sta_connect()'s first-ever
 * bring-up path - does a raw NVS flash write for Wi-Fi's own internal
 * calibration/misc data (misc_nvs_init() -> nvs_open() -> ... ->
 * esp_flash_write()). That needs a task with an internal-RAM stack, not
 * PSRAM, since PSRAM itself is unreachable while the flash cache is
 * briefly disabled for the write - same constraint as reconfigure_task in
 * calendar_ui.c and config_web.c's save_task. net_task's own stack is
 * deliberately in PSRAM (see its creation below), so this first bring-up
 * has to happen on this small dedicated task instead - confirmed
 * on-device 2026-09-05 as a hard abort (assert inside
 * spi_flash_disable_interrupts_caches_and_other_cpu(), reached via
 * esp_wifi_init() -> misc_nvs_init()) that boot-looped every time,
 * whereas the (already-known, self-healing) SD-mount race crash it was
 * first mistaken for never hard-loops. Reconnects after this first
 * bring-up don't re-run esp_wifi_init() - they're handled entirely by
 * wifi_sta.c's own event handler on the default event loop task, which
 * already has a normal internal-RAM stack - so this task's job is done
 * once Wi-Fi comes up the first time. */
static void wifi_bringup_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (wifi_sta_connect(&s_cfg, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "Wi-Fi connect failed, retrying in 15s");
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
    xSemaphoreGive(s_wifi_up);
    vTaskDelete(NULL);
}

static void net_task(void *arg)
{
    (void)arg;

    s_wifi_up = xSemaphoreCreateBinary();
    xTaskCreate(wifi_bringup_task, "wifi_bringup", 4096, NULL, 5, NULL);
    xSemaphoreTake(s_wifi_up, portMAX_DELAY);
    vSemaphoreDelete(s_wifi_up);

    /* No-ops if config_web_password isn't set - see config_web.c. */
    config_web_start(&s_cfg);

    if (sync_time()) {
        /* calendar_ui_init() ran before Wi-Fi/SNTP, so the "today" cursor
         * it computed was anchored to whatever bogus pre-sync clock value
         * time(NULL) returned then - re-anchor it now that the clock is
         * actually correct. */
        calendar_ui_sync_today();
    }

    for (;;) {
        uint32_t wait_ms = s_cfg.refresh_interval_s * 1000;
        /* Syncs every cycle regardless of display state - this used to
         * skip entirely while asleep ("nobody's looking, don't bother"),
         * but that meant a display that's been asleep for hours (an
         * overnight gap, not just the few-minute screensaver windows)
         * went that whole time with zero network activity at all, which
         * turned out to matter: see keepalive_probe()'s own comment for
         * why a connection sitting completely silent for minutes at a
         * time is suspected of being more prone to
         * ESP_ERR_HTTP_CONNECT/FETCH_HEADER/EAGAIN sync failures than one
         * with some regular traffic on it. Syncing while asleep also
         * means the calendar is already fresh the instant the display
         * wakes, rather than showing however-stale data until the next
         * scheduled cycle happens to land. calendar_ui_refresh() and
         * calendar_ui_notify_sync_failed() touch LVGL objects that just
         * aren't currently visible while asleep, not wasted work in any
         * way that matters next to the network fetch itself. */
        bool all_ok = false;
        /* Cache window is user-configurable (config_web.c - see
         * app_settings.h's fetch_past_days/fetch_future_days comment
         * for why), not a fixed constant. */
        esp_err_t err = gcal_refresh_all(&s_cfg, s_cfg.fetch_past_days, s_cfg.fetch_future_days, &all_ok);
        if (err == ESP_OK) {
            calendar_ui_refresh();
            /* refresh() above just cleared the warning icon - a
             * cycle where at least one calendar came back is still
             * an ESP_OK, so a calendar that's been failing every
             * cycle would otherwise never show any warning at all. */
            if (!all_ok) {
                calendar_ui_notify_sync_failed();
                /* A calendar that failed here almost always failed for
                 * the same reason a *total* failure below would have -
                 * transient low internal RAM during the fetch (see
                 * gcal_client.c's log_heap_state() comment) - not
                 * "this one calendar is broken." That condition
                 * reliably clears within seconds (screen redraws
                 * finish, TLS buffers get released), so there's no
                 * reason to leave 1-3 stale calendars on screen for
                 * up to a full refresh_interval_s when a short retry
                 * would very likely just work. */
                ESP_LOGW(TAG, "calendar refresh partially failed (retrying in %ds)", RETRY_BACKOFF_S);
                wait_ms = RETRY_BACKOFF_S * 1000;
            }
        } else {
            ESP_LOGW(TAG, "calendar refresh failed: %s (retrying in %ds)",
                     esp_err_to_name(err), RETRY_BACKOFF_S);
            calendar_ui_notify_sync_failed();
            wait_ms = RETRY_BACKOFF_S * 1000;
        }
        /* Waits for either the refresh/backoff interval above or an early
         * wake - which, since 2026-09-10, means only a manual force-sync
         * tap (the "updated HH:MM" label), not a plain touch waking the
         * display from the screensaver too. A touch-wake used to set the
         * same bit and land here as well, but that's ui_screensaver.c's
         * go_calendar()'s own job now (calendar_ui_restore_active_view(),
         * straight from event_store's already-cached data, no network) -
         * see its own comment for why forcing an *extra* fetch right at
         * that exact moment was actively harmful, not just redundant.
         * Broken into KEEPALIVE_INTERVAL_MS-sized chunks (instead of one
         * long wait) so keepalive_probe() can run periodically during
         * whatever of that interval is still otherwise completely idle -
         * see that function's own comment. */
        uint32_t remaining_ms = wait_ms;
        while (remaining_ms > 0) {
            uint32_t chunk_ms = remaining_ms < KEEPALIVE_INTERVAL_MS ? remaining_ms : KEEPALIVE_INTERVAL_MS;
            if (calendar_ui_wait_wake(chunk_ms)) {
                break; /* early wake - loop back around to a fresh sync now */
            }
            remaining_ms -= chunk_ms;
            if (remaining_ms > 0) {
                keepalive_probe();
            }
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Independent of display/Wi-Fi/config, so started as early as
     * possible. ui_screensaver.c's own idle timer polls
     * presence_sensor_is_detected() directly once it's up; nothing else
     * needs a dedicated task for this. */
    presence_sensor_init();

    /* Load config from NVS before anything else - specifically, before
     * bsp_display_init() below creates the LVGL task, whose stack
     * deliberately lives in PSRAM (see board_bsp.c's lvgl_init()) and is
     * therefore briefly unreachable whenever a raw NVS read disables the
     * flash/PSRAM cache. Doing the read here means main_task is provably
     * the only task alive yet - nothing to race against - which is good
     * practice regardless, but on its own this did NOT fix the
     * intermittent post-SD-mount crash investigated below (a stress test
     * with this reorder in place still hit it on ~4-6 of 15 reboots), so
     * the NVS/LVGL-task race theory was wrong or at best incomplete. Kept
     * anyway since it's a real (if apparently not dominant) risk it rules
     * out for free. */
    bool have_cfg = (provisioning_load(&s_cfg) == ESP_OK);

    /* Bring the display up next regardless of whether setup is needed, so
     * show_provisioning_screen() below has something to draw on - see the
     * boot flow comment at the top of this file. */
    ESP_ERROR_CHECK(bsp_display_init());

    /* Settling delay before touching the SD SPI bus/CH422G CS: with no
     * delay here at all, mounting the card immediately after
     * bsp_display_init() panics deterministically (confirmed 2026-09-04),
     * so some delay here is required. Beyond that, though, this remains
     * an unresolved, intermittent (~25-40% of boots in repeated 15-reboot
     * stress tests on 2026-09-05) crash landing inside the LVGL task's
     * own background redraw timer, immediately after "TF card mounted" -
     * always self-recovering via the panic handler's own reboot within
     * about a second, never a hard loop in any test run. Investigated and
     * ruled out: main_task/LVGL-task stack size, heap corruption
     * (CONFIG_HEAP_POISONING_COMPREHENSIVE caught nothing), the NVS/LVGL-
     * task race above, and SD SPI clock speed (400kHz vs. the 20MHz
     * default made no meaningful difference). Leading remaining theory,
     * untested: a GDMA channel-sharing interaction between the SD SPI
     * bus's spi_bus_initialize() (auto-selected DMA channel) and the RGB
     * panel's own continuous-refresh GDMA channel (esp_lcd_panel_rgb.c) -
     * investigating that properly needs real driver-level work with no
     * guaranteed payoff, so as of 2026-09-05 the decision (made
     * knowingly, not by default) is to accept the self-healing crash
     * rather than keep chasing it. Revisit if it ever starts hard-looping
     * instead of recovering, or if you have a concrete new lead. */
    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_err_t sd_err = sd_card_init(bsp_get_expander());
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "no TF card mounted (%s) - continuing without one",
                 esp_err_to_name(sd_err));
    } else if (provisioning_sd_ensure_dir() != ESP_OK) {
        ESP_LOGW(TAG, "could not create %s on the TF card - config backup disabled",
                 PROVISIONING_SD_DIR);
    }

    /* Config load, part 2: NVS is authoritative whenever it holds a valid
     * config - it's the only thing the on-device settings dialog and the
     * LAN config web server (config_web.c) ever write to, so anything
     * changed there needs to actually take effect on the next boot
     * instead of being silently reverted. The TF card backup is only a
     * FALLBACK, consulted when NVS comes back empty/invalid - e.g. after
     * flashing other/test firmware that wiped or reused NVS - so this
     * firmware can pick its calendar config back up on its own instead of
     * re-running the setup portal. (Earlier, this had it backwards -
     * preferring the TF card unconditionally - which meant
     * fetch_past_days/fetch_future_days and other config_web.c-only
     * settings appeared to stop saving: they saved to NVS fine, but the
     * next boot loaded the TF card's now-stale snapshot over them. Fixed
     * 2026-09-05.) provisioning_load_sd() reads a FATFS file, not NVS, so
     * unlike provisioning_load() above it doesn't need to run before the
     * LVGL task exists - it was never implicated in the cache-disable
     * race either way. */
    if (!have_cfg && sd_err == ESP_OK && provisioning_sd_config_exists() &&
        provisioning_load_sd(&s_cfg) == ESP_OK) {
        ESP_LOGI(TAG, "NVS config missing/invalid - loaded from TF card backup (%s)",
                 PROVISIONING_SD_DIR);
        have_cfg = true;

        /* Heal NVS from the recovered config, not just this boot's
         * in-memory s_cfg - without this, NVS stayed invalid forever
         * (every future boot would keep falling back to the card instead
         * of NVS becoming authoritative again), which is fragile: if the
         * card were ever removed or failed a mount after this point,
         * the device would incorrectly fall through to the setup portal
         * despite having had a perfectly good config a boot ago.
         * provisioning_save() only touches NVS (see its own comment for
         * why it no longer also writes the card), so this can't interfere
         * with the boot-time SD-backup refresh below either way. */
        if (provisioning_save(&s_cfg) != ESP_OK) {
            ESP_LOGW(TAG, "could not restore NVS from the TF card backup - "
                          "will keep falling back to the card each boot until this succeeds");
        }
    }

    if (!have_cfg) {
        ESP_LOGW(TAG, "no valid configuration in flash - starting setup portal "
                      "(connect to Wi-Fi \"%s\" and browse to 192.168.4.1)",
                 PROVISIONING_AP_SSID);
        show_provisioning_screen();
        provisioning_run_portal(); /* reboots once the form is submitted; never returns */
    }

    /* Refresh the TF card backup on every boot, not just once when the
     * file was missing. provisioning_save() itself can no longer do this
     * at the moment a setting actually changes - the card is deinited
     * below and stays that way for the rest of this boot's uptime (SD is
     * only ever mounted in this narrow boot-time window), so
     * provisioning_save_sd() would just fail silently the next time the
     * on-device dialog or the LAN config page saved something,
     * confirmed on-device 2026-09-08 as exactly why an added calendar
     * "disappeared": it never made it past NVS. Every existing save path
     * (the setup portal, the settings dialog, config_web.c) already calls
     * esp_restart() right after provisioning_save() succeeds, so
     * reconciling here - once per boot, while the card is mounted anyway
     * - keeps the two in sync within seconds of any real change, with no
     * extra runtime SD-mount risk (see the settling-delay comment above
     * for why that risk is worth minimizing) beyond the one already
     * accepted at this exact point in boot. */
    if (sd_err == ESP_OK && provisioning_save_sd(&s_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "failed to write TF card config backup");
    }

    /* Nothing else touches the TF card for the rest of this boot (or ever
     * again this session - there's no runtime feature using /sdcard yet),
     * so free its SPI bus/DMA buffers/GDMA channel now rather than hold
     * them for the device's entire uptime. Small in isolation (~2.4KB of
     * internal RAM measured directly via heap_caps_get_free_size() around
     * sd_card_init() on 2026-09-04), but every internal-RAM KB matters
     * here - see the CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH comment in
     * sdkconfig.defaults and the gcal_event_t buffer comment in
     * gcal_client.c's gcal_refresh_all() for why calendar refresh's TLS
     * handshakes are already this budget's biggest pressure point. */
    if (sd_err == ESP_OK) {
        sd_card_deinit();
    }

    setenv("TZ", s_cfg.posix_tz[0] ? s_cfg.posix_tz : "UTC0", 1);
    tzset();

    event_store_init();
    calendar_ui_init(&s_cfg);

    /* 8192 was already tight for TLS (mbedtls handshake state is large) +
     * JSON parsing + JWT signing on the stack; it overflowed once the
     * jwt_auth.c buffers were sized up to safely fit a 4096-bit RSA key
     * (see sig_b64 in build_signed_jwt), crashing net_task the moment the
     * first calendar refresh ran.
     *
     * Stack itself lives in PSRAM (xTaskCreateWithCaps, not a plain
     * xTaskCreate) - a plain internal-RAM stack this size was permanently
     * eating ~16KB out of the ~237KB internal-SRAM pool that mbedtls also
     * needs for TLS handshake/certificate-verification allocations, which
     * we tracked down as the cause of a "PK verify failed" cert-check
     * failure that got the device stuck failing every refresh until
     * rebooted. Same fix already applied to the LVGL task's stack earlier
     * for the same class of internal-RAM starvation. This does mean the
     * very first wifi_sta_connect() call below runs with a PSRAM stack;
     * if that turns out to trigger the flash-write-needs-internal-RAM-
     * stack assert (see reconfigure_task in calendar_ui.c for that
     * failure mode), split net_task's startup Wi-Fi connect into its own
     * short-lived internal-RAM-stack task instead. */
    xTaskCreateWithCaps(net_task, "net_task", 16384, NULL, 5, NULL, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "app_main done, UI + net_task running");
}
