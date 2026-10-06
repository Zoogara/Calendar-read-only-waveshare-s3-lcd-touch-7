#include "wifi_sta.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "ping/ping_sock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi_sta";

static EventGroupHandle_t s_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          10

static int s_retry_count;
static bool s_wifi_started;

/* Pinned to one mesh node by wifi_sta_check_link() (see below): reconnects
 * go to that BSSID only. Released after UNPIN_AFTER_FAILS failed reconnects
 * in a row, so a pinned node that disappears can't strand the device. */
static bool s_pinned;
#define UNPIN_AFTER_FAILS 3

static void unpin(void)
{
    if (!s_pinned) {
        return;
    }
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK) {
        c.sta.bssid_set = false;
        esp_wifi_set_config(WIFI_IF_STA, &c);
    }
    s_pinned = false;
    ESP_LOGI(TAG, "no longer pinned to a mesh node");
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        if (s_pinned && s_retry_count + 1 >= UNPIN_AFTER_FAILS) {
            unpin();   /* the pinned node isn't coming back - any node will do */
        }
        /* Keep retrying forever - this is a 24/7 wall display, not a
         * one-shot connect. We still raise WIFI_FAIL_BIT once after
         * MAX_RETRY *consecutive* attempts purely so wifi_sta_connect()'s
         * blocking wait gives up and lets main.c's outer retry loop
         * log/back off instead of blocking forever on e.g. a wrong
         * password - reconnection attempts continue regardless. */
        esp_wifi_connect();
        if (s_retry_count < MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi dropped, reconnecting (%d/%d)", s_retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_connect(const app_settings_t *cfg, uint32_t timeout_ms)
{
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
    }
    /* Clear any stale bits from a previous failed attempt before we wait
     * again below. */
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    if (!s_wifi_started) {
        /* One-time bring-up. Deliberately NOT repeated on retries -
         * calling esp_wifi_init()/esp_wifi_start() again on an already
         * running driver would fail; reconnection after this point is
         * handled entirely by event_handler(), forever. */
        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.sta.ssid, cfg->wifi_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, cfg->wifi_password, sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = strlen(cfg->wifi_password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        /* Scan every channel and join the strongest access point for this
         * SSID, not the first one heard (ESP-IDF's default is
         * WIFI_FAST_SCAN, where sort_method is ignored). The home network
         * is a multi-node mesh, and which node the device lands on has
         * turned out to matter: on real hardware (2026-09-25) one node
         * silently stopped passing this device's traffic for ~10 minutes
         * at a time - DNS timeouts, "no route to host", and the device
         * unpingable from the LAN - while the association stayed up with
         * no disconnect or beacon loss, so nothing on the device side
         * noticed. Other nodes on other days were fine. */
        wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        /* Turn OFF modem-sleep power save (ESP-IDF defaults STA to
         * WIFI_PS_MIN_MODEM). This board is mains-powered - a wall
         * display - so there's no battery-life reason to have the radio
         * dozing between DTIM beacons, and on real hardware (2026-09-10)
         * leaving it on correlated with persistently flaky calendar
         * syncs: intermittent TLS handshake failures
         * (FETCH_HEADER/CONNECT/PK-verify) and the occasional 35-second
         * fetch, all with healthy RAM and a strong-signal AP. The serial
         * log was full of "wifi:m f null" (failed power-save NULL-data
         * frames - the station's "going to sleep" / "awake now"
         * signalling to the AP) and "bcn_timeout,ap_probe_send_start"
         * (missed beacons entirely), i.e. the AP's view of this station's
         * power state kept drifting out of sync with reality, which drops
         * or delays packets mid-transfer. A neighbouring device doing
         * near-constant traffic (so its radio never actually sleeps)
         * never had the problem - the tell that this was the cause.
         * WIFI_PS_NONE keeps the radio always-on and always-listening. */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        s_wifi_started = true;
        ESP_LOGI(TAG, "connecting to \"%s\"...", cfg->wifi_ssid);
    } else {
        /* Already running (this is a retry after an earlier timeout) -
         * event_handler is already reconnecting on its own; just wait
         * again. */
        s_retry_count = 0;
        esp_wifi_connect();
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "failed to connect to \"%s\" within %lu ms", cfg->wifi_ssid, (unsigned long)timeout_ms);
    return ESP_FAIL;
}

void wifi_sta_force_reconnect(void)
{
    if (!s_wifi_started) {
        return;
    }
    /* event_handler()'s WIFI_EVENT_STA_DISCONNECTED branch already calls
     * esp_wifi_connect() on every disconnect, so dropping the association
     * is all that's needed - the reconnect re-scans (all channels, see
     * wifi_sta_connect()) and joins the strongest node, which may well be
     * a different one from the node that stopped passing traffic. */
    ESP_LOGW(TAG, "forcing Wi-Fi reconnect");
    unpin();   /* a fresh choice of node */
    esp_wifi_disconnect();
}

/* ---------- link quality: find a mesh node that actually works ----------
 *
 * The station joins the strongest node for the SSID, but on this mesh the
 * strongest node has repeatedly been the worst one: associated, full
 * signal, yet carrying local traffic at 300-2000ms a ping, or not at all -
 * Home Assistant requests timing out while internet requests (with longer
 * timeouts) limped through. Nothing at the Wi-Fi layer notices. So the
 * link is measured directly: pings to the gateway. A node that fails is
 * avoided for AVOID_US, and the device pins itself to the strongest other
 * node with the SSID. */

#define PING_COUNT        5
#define BAD_AVG_MS        150
#define BAD_LOST          2          /* this many of PING_COUNT lost = bad */
#define AVOID_US          (60LL * 60 * 1000000)
#define AVOID_SLOTS       4
#define REJOIN_WAIT_MS    20000

typedef struct {
    uint8_t bssid[6];
    int64_t until_us;
} avoid_t;
static avoid_t s_avoid[AVOID_SLOTS];

typedef struct {
    SemaphoreHandle_t done;
    uint32_t replies;
    uint32_t total_ms;
} ping_result_t;

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    ping_result_t *r = args;
    uint32_t gap = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &gap, sizeof(gap));
    r->replies++;
    r->total_ms += gap;
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    xSemaphoreGive(((ping_result_t *)args)->done);
}

/* Pings the gateway, filling in the average round trip and how many
 * pings were lost. False if it couldn't run at all. */
static bool ping_gateway(uint32_t *avg_ms, uint32_t *lost)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.gw.addr == 0) {
        return false;
    }
    ping_result_t r = { .done = xSemaphoreCreateBinary() };
    if (r.done == NULL) {
        return false;
    }
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.count = PING_COUNT;
    pc.interval_ms = 200;
    pc.timeout_ms = 1000;
    pc.data_size = 32;
    pc.target_addr.type = IPADDR_TYPE_V4;
    pc.target_addr.u_addr.ip4.addr = ip.gw.addr;
    esp_ping_callbacks_t cbs = {
        .cb_args = &r,
        .on_ping_success = on_ping_success,
        .on_ping_end = on_ping_end,
    };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&pc, &cbs, &ping) != ESP_OK) {
        vSemaphoreDelete(r.done);
        return false;
    }
    esp_ping_start(ping);
    xSemaphoreTake(r.done, pdMS_TO_TICKS(PING_COUNT * 1500 + 2000));
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    vSemaphoreDelete(r.done);
    *lost = PING_COUNT - r.replies;
    *avg_ms = r.replies ? r.total_ms / r.replies : 0;
    return true;
}

static bool avoided(const uint8_t *bssid, int64_t now)
{
    for (int i = 0; i < AVOID_SLOTS; i++) {
        if (s_avoid[i].until_us > now && memcmp(s_avoid[i].bssid, bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void avoid(const uint8_t *bssid, int64_t now)
{
    int slot = 0;
    for (int i = 0; i < AVOID_SLOTS; i++) {
        if (s_avoid[i].until_us <= now || memcmp(s_avoid[i].bssid, bssid, 6) == 0) {
            slot = i;
            break;
        }
        if (s_avoid[i].until_us < s_avoid[slot].until_us) {
            slot = i;   /* else reuse the one expiring soonest */
        }
    }
    memcpy(s_avoid[slot].bssid, bssid, 6);
    s_avoid[slot].until_us = now + AVOID_US;
}

/* Scans for the SSID and pins to the strongest node that isn't avoided.
 * True if it switched. */
static bool switch_node(void)
{
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) {
        return false;
    }
    wifi_scan_config_t sc = { .ssid = c.sta.ssid, .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return false;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        return false;
    }
    if (n > 16) {
        n = 16;
    }
    wifi_ap_record_t *aps = heap_caps_malloc(n * sizeof(*aps), MALLOC_CAP_SPIRAM);
    if (aps == NULL) {
        esp_wifi_clear_ap_list();
        return false;
    }
    esp_wifi_scan_get_ap_records(&n, aps);
    int64_t now = esp_timer_get_time();
    int best = -1;
    for (int i = 0; i < n; i++) {
        bool bad = avoided(aps[i].bssid, now);
        ESP_LOGI(TAG, "  node " MACSTR " ch %d rssi %d%s", MAC2STR(aps[i].bssid),
                 aps[i].primary, aps[i].rssi, bad ? " (avoided)" : "");
        if (!bad && (best < 0 || aps[i].rssi > aps[best].rssi)) {
            best = i;
        }
    }
    bool switched = false;
    if (best >= 0) {
        ESP_LOGW(TAG, "switching to node " MACSTR " (rssi %d)", MAC2STR(aps[best].bssid), aps[best].rssi);
        memcpy(c.sta.bssid, aps[best].bssid, 6);
        c.sta.bssid_set = true;
        esp_wifi_set_config(WIFI_IF_STA, &c);
        s_pinned = true;
        s_retry_count = 0;
        esp_wifi_disconnect();   /* event_handler reconnects - to that node now */
        switched = true;
    } else {
        ESP_LOGW(TAG, "no other node for the SSID - staying put");
    }
    free(aps);
    return switched;
}

bool wifi_sta_check_link(void)
{
    if (!s_wifi_started || !(xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT)) {
        return true;   /* not connected - the normal reconnect logic handles that */
    }
    uint32_t avg, lost;
    if (!ping_gateway(&avg, &lost)) {
        return true;
    }
    wifi_ap_record_t ap;
    bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    bool bad = lost >= BAD_LOST || avg > BAD_AVG_MS;
    ESP_LOGI(TAG, "link check: node " MACSTR " rssi %d - %lu/%d replies, avg %lu ms%s",
             MAC2STR(have_ap ? ap.bssid : (uint8_t[6]){0}), have_ap ? ap.rssi : 0,
             (unsigned long)(PING_COUNT - lost), PING_COUNT, (unsigned long)avg, bad ? " - POOR" : "");
    if (!bad || !have_ap) {
        return true;
    }

    avoid(ap.bssid, esp_timer_get_time());
    if (!switch_node()) {
        return false;
    }
    /* Wait for the new association before the caller carries on (it's
     * about to sync), then measure the new node once for the log. */
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(REJOIN_WAIT_MS));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "not reconnected after %d s", REJOIN_WAIT_MS / 1000);
        return false;
    }
    if (ping_gateway(&avg, &lost)) {
        ESP_LOGI(TAG, "new node: %lu/%d replies, avg %lu ms", (unsigned long)(PING_COUNT - lost),
                 PING_COUNT, (unsigned long)avg);
    }
    return true;
}
