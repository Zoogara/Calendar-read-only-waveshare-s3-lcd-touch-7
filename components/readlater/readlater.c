#include "readlater.h"

#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "readlater";
#define NVS_NS  "gcalrl"
#define NVS_KEY "links"

static SemaphoreHandle_t s_lock;
static rl_list_t *s_list;               /* PSRAM */
static bool s_dirty;                    /* guarded by s_lock */
static bool s_saving;                   /* a save task exists; guarded by s_lock */

static uint32_t now_s(void)
{
    time_t t = time(NULL);
    return t > 0 ? (uint32_t)t : 0;
}

static int unstamped(const rl_list_t *l)
{
    int n = 0;
    for (int i = 0; i < l->n; i++) {
        n += l->items[i].saved == 0;
    }
    return n;
}

static void save_task(void *arg)
{
    (void)arg;
    uint8_t *buf = heap_caps_malloc(RL_ENCODED_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        size_t n = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_dirty || buf == NULL) {
            s_saving = false;               /* decided under the lock, so a save requested now starts a new task */
            xSemaphoreGive(s_lock);
            break;
        }
        s_dirty = false;
        n = rl_encode(s_list, buf, RL_ENCODED_MAX);
        xSemaphoreGive(s_lock);

        nvs_handle_t h;
        esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = n > 0 ? nvs_set_blob(h, NVS_KEY, buf, n) : ESP_FAIL;
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "saving the list failed: %s (will retry)", esp_err_to_name(err));
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_dirty = true;                 /* housekeeping tries again */
            s_saving = false;
            xSemaphoreGive(s_lock);
            break;
        }
    }
    heap_caps_free(buf);
    vTaskDelete(NULL);
}

/* Starts the save task if none is running. Call without s_lock held. */
static void request_save(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool start = s_dirty && !s_saving;
    if (start) {
        s_saving = true;
    }
    xSemaphoreGive(s_lock);
    if (start && xTaskCreate(save_task, "rl_save", 4096, NULL, 4, NULL) != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_saving = false;                   /* dirty stays set: housekeeping retries */
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "no memory for the save task - will retry");
    }
}

void readlater_init(void)
{
    if (s_lock != NULL) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_list = heap_caps_calloc(1, sizeof(*s_list), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_lock == NULL || s_list == NULL) {
        ESP_LOGE(TAG, "no memory for the read-later list");
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;                              /* nothing saved yet */
    }
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len > 0 && len <= RL_ENCODED_MAX) {
        uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf != NULL && nvs_get_blob(h, NVS_KEY, buf, &len) == ESP_OK) {
            if (rl_decode(buf, len, s_list)) {
                ESP_LOGI(TAG, "%d saved link(s) loaded", s_list->n);
            } else {
                ESP_LOGW(TAG, "the saved list is damaged - starting empty");
                memset(s_list, 0, sizeof(*s_list));
            }
        }
        heap_caps_free(buf);
    }
    nvs_close(h);
}

rl_add_result_t readlater_add(const char *title, const char *url, uint16_t keep_days)
{
    if (s_list == NULL) {
        return RL_BAD_URL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t now = now_s();
    rl_prune(s_list, now, keep_days);
    rl_add_result_t r = rl_add(s_list, now, title, url);
    if (r != RL_BAD_URL) {
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
    request_save();
    return r;
}

int readlater_copy(rl_list_t *out, uint16_t keep_days)
{
    if (s_list == NULL) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int before = s_list->n, stamps = unstamped(s_list);
    rl_prune(s_list, now_s(), keep_days);
    if (s_list->n != before || unstamped(s_list) != stamps) {
        s_dirty = true;
    }
    memcpy(out, s_list, sizeof(*out));
    xSemaphoreGive(s_lock);
    request_save();
    return out->n;
}

bool readlater_remove(int index, uint32_t saved, uint16_t keep_days)
{
    (void)keep_days;
    if (s_list == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = rl_remove(s_list, index, saved);
    if (ok) {
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
    request_save();
    return ok;
}

void readlater_clear(void)
{
    if (s_list == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_list->n > 0) {
        rl_clear(s_list);
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
    request_save();
}

void readlater_housekeeping(uint16_t keep_days)
{
    if (s_list == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int before = s_list->n, stamps = unstamped(s_list);
    rl_prune(s_list, now_s(), keep_days);
    if (s_list->n != before || unstamped(s_list) != stamps) {
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
    request_save();                          /* also retries a write that failed earlier */
}
