#include "remote_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

/* Everything here is deliberately cheap on internal RAM: the ring buffer and
 * the packet buffer live in PSRAM, and so does the sender task's stack (it
 * never writes flash, so a PSRAM stack is safe). This file must not log
 * itself - the hook would feed its own output back into the ring. */
#define RING_BYTES 24576
#define LINE_MAX   256
#define PKT_MAX    1200
#define SEND_STACK 4096

static RingbufHandle_t s_rb;
static vprintf_like_t s_orig;
static char s_host[64];
static int s_port;

static int log_hook(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int n = s_orig ? s_orig(fmt, args) : vprintf(fmt, args);
    if (s_rb != NULL && !xPortInIsrContext()) {
        char line[LINE_MAX];
        int len = vsnprintf(line, sizeof(line), fmt, copy);
        if (len > 0) {
            if (len >= (int)sizeof(line)) {
                len = sizeof(line) - 1;
            }
            xRingbufferSend(s_rb, line, len, 0); /* full: drop, never block a logger */
        }
    }
    va_end(copy);
    return n;
}

static void sender_task(void *arg)
{
    (void)arg;
    char *pkt = heap_caps_malloc(PKT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pkt == NULL) {
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest = {0};
    int sock = -1;
    for (;;) {
        /* Resolve (or parse) the host, retrying until Wi-Fi/DNS are up; log
         * lines keep collecting in the ring meanwhile. */
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
        struct addrinfo *res = NULL;
        if (getaddrinfo(s_host, NULL, &hints, &res) == 0 && res != NULL) {
            dest = *(struct sockaddr_in *)res->ai_addr;
            dest.sin_port = htons(s_port);
            freeaddrinfo(res);
            sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock >= 0) {
                break;
            }
        } else if (res != NULL) {
            freeaddrinfo(res);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    for (;;) {
        size_t used = 0, sz = 0;
        char *item = xRingbufferReceive(s_rb, &sz, pdMS_TO_TICKS(500));
        while (item != NULL) {
            if (used + sz > PKT_MAX) {
                sendto(sock, pkt, used, 0, (struct sockaddr *)&dest, sizeof(dest));
                used = 0;
            }
            memcpy(pkt + used, item, sz);
            used += sz;
            vRingbufferReturnItem(s_rb, item);
            item = xRingbufferReceive(s_rb, &sz, 0);
        }
        if (used > 0) {
            sendto(sock, pkt, used, 0, (struct sockaddr *)&dest, sizeof(dest));
        }
    }
}

void remote_log_start(const char *host_port)
{
    if (host_port == NULL || host_port[0] == '\0' || s_rb != NULL) {
        return;
    }
    const char *colon = strrchr(host_port, ':');
    size_t hlen = colon ? (size_t)(colon - host_port) : strlen(host_port);
    if (hlen == 0 || hlen >= sizeof(s_host)) {
        return;
    }
    memcpy(s_host, host_port, hlen);
    s_host[hlen] = '\0';
    s_port = colon ? atoi(colon + 1) : 0;
    if (s_port <= 0 || s_port > 65535) {
        s_port = 9999;
    }

    s_rb = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    if (s_rb == NULL) {
        return;
    }
    if (xTaskCreateWithCaps(sender_task, "remote_log", SEND_STACK, NULL, 1, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vRingbufferDeleteWithCaps(s_rb);
        s_rb = NULL;
        return;
    }
    s_orig = esp_log_set_vprintf(log_hook);
}
