#include "readlater_store.h"
#include <string.h>

static void clean_title(const char *title, const char *url, char out[RL_TITLE_LEN])
{
    size_t w = 0;
    bool pending_space = false;
    for (const unsigned char *p = (const unsigned char *)(title ? title : ""); *p; p++) {
        if (*p < 0x20 || *p == 0x7F || *p == ' ') {
            pending_space = w > 0;                       /* collapse runs, no leading space */
            continue;
        }
        size_t need = (pending_space ? 1 : 0) + 1;
        if (w + need >= RL_TITLE_LEN) {
            break;
        }
        if (pending_space) {
            out[w++] = ' ';
            pending_space = false;
        }
        out[w++] = (char)*p;
    }
    out[w] = '\0';
    /* the byte-wise cut above may have split a multi-byte character: back up to its lead byte if it is incomplete */
    size_t k = w;
    while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) {
        k--;
    }
    if (k > 0) {
        unsigned char lead = (unsigned char)out[k - 1];
        size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (want > 1 && w - (k - 1) < want) {
            w = k - 1;
            out[w] = '\0';
        }
    }
    if (w == 0 && url != NULL) {                         /* nothing usable: the address stands in */
        size_t n = strlen(url);
        if (n >= RL_TITLE_LEN) {
            n = RL_TITLE_LEN - 1;
        }
        memcpy(out, url, n);
        out[n] = '\0';
    }
}

rl_add_result_t rl_add(rl_list_t *l, uint32_t now, const char *title, const char *url)
{
    if (!link_url_ok(url)) {
        return RL_BAD_URL;
    }
    rl_link_t k;
    memset(&k, 0, sizeof(k));
    k.saved = now >= RL_MIN_VALID_TIME ? now : 0;
    clean_title(title, url, k.title);
    strcpy(k.url, url);

    rl_add_result_t result = RL_ADDED;
    for (int i = 0; i < l->n; i++) {
        if (strcmp(l->items[i].url, url) == 0) {
            memmove(&l->items[i], &l->items[i + 1], (size_t)(l->n - i - 1) * sizeof(rl_link_t));
            l->n--;
            result = RL_ALREADY;
            break;
        }
    }
    if (l->n >= RL_MAX) {
        l->n = RL_MAX - 1;                               /* the oldest (last) goes */
        result = RL_ADDED_DROPPED_OLDEST;
    }
    memmove(&l->items[1], &l->items[0], (size_t)l->n * sizeof(rl_link_t));
    l->items[0] = k;
    l->n++;
    return result;
}

int rl_prune(rl_list_t *l, uint32_t now, int max_days)
{
    if (now < RL_MIN_VALID_TIME) {
        return 0;
    }
    if (max_days < 1) {
        max_days = 1;
    }
    const uint32_t life = (uint32_t)max_days * 86400u;
    int removed = 0, w = 0;
    for (int i = 0; i < l->n; i++) {
        rl_link_t *k = &l->items[i];
        if (k->saved == 0) {
            k->saved = now;                              /* its life starts when the clock first allows */
        } else if (now > k->saved && now - k->saved > life) {
            removed++;
            continue;
        }
        if (w != i) {
            l->items[w] = *k;
        }
        w++;
    }
    l->n = w;
    return removed;
}

bool rl_remove(rl_list_t *l, int index, uint32_t saved)
{
    if (index < 0 || index >= l->n || l->items[index].saved != saved) {
        return false;
    }
    memmove(&l->items[index], &l->items[index + 1], (size_t)(l->n - index - 1) * sizeof(rl_link_t));
    l->n--;
    return true;
}

void rl_clear(rl_list_t *l)
{
    l->n = 0;
}

int rl_days_left(const rl_link_t *k, uint32_t now, int max_days)
{
    if (k->saved == 0 || now < RL_MIN_VALID_TIME) {
        return -1;
    }
    if (max_days < 1) {
        max_days = 1;
    }
    const uint32_t life = (uint32_t)max_days * 86400u;
    uint32_t age = now > k->saved ? now - k->saved : 0;
    if (age >= life) {
        return 0;
    }
    return (int)((life - age + 86399u) / 86400u);        /* whole days, rounded up: "1 day left" until it goes */
}

/* ---- flash form ---- */

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
        }
    }
    return ~c;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t rl_encode(const rl_list_t *l, uint8_t *out, size_t cap)
{
    size_t need = 4 + 1 + 1 + 4;
    for (int i = 0; i < l->n; i++) {
        need += 4 + 1 + strlen(l->items[i].title) + 1 + strlen(l->items[i].url);
    }
    if (cap < need || l->n < 0 || l->n > RL_MAX) {
        return 0;
    }
    size_t o = 0;
    memcpy(out, "GCRL", 4);
    o = 4;
    out[o++] = 1;                                        /* version */
    out[o++] = (uint8_t)l->n;
    for (int i = 0; i < l->n; i++) {
        const rl_link_t *k = &l->items[i];
        put32(out + o, k->saved);
        o += 4;
        size_t tl = strlen(k->title), ul = strlen(k->url);
        out[o++] = (uint8_t)tl;
        memcpy(out + o, k->title, tl);
        o += tl;
        out[o++] = (uint8_t)ul;
        memcpy(out + o, k->url, ul);
        o += ul;
    }
    put32(out + o, crc32(out, o));
    return o + 4;
}

bool rl_decode(const uint8_t *in, size_t len, rl_list_t *out)
{
    if (in == NULL || len < 4 + 1 + 1 + 4 || memcmp(in, "GCRL", 4) != 0 || in[4] != 1) {
        return false;
    }
    if (crc32(in, len - 4) != get32(in + len - 4)) {
        return false;
    }
    int n = in[5];
    if (n > RL_MAX) {
        return false;
    }
    rl_list_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    size_t o = 6, end = len - 4;
    for (int i = 0; i < n; i++) {
        if (o + 4 + 1 > end) return false;
        rl_link_t *k = &tmp.items[i];
        k->saved = get32(in + o);
        o += 4;
        size_t tl = in[o++];
        if (tl == 0 || tl >= RL_TITLE_LEN || o + tl + 1 > end) return false;
        memcpy(k->title, in + o, tl);
        k->title[tl] = '\0';
        if (strlen(k->title) != tl) return false;       /* an embedded NUL */
        for (size_t j = 0; j < tl; j++) {
            if ((unsigned char)k->title[j] < 0x20 || (unsigned char)k->title[j] == 0x7F) return false;
        }
        o += tl;
        size_t ul = in[o++];
        if (ul == 0 || ul >= LINK_URL_MAX || o + ul > end) return false;
        memcpy(k->url, in + o, ul);
        k->url[ul] = '\0';
        if (strlen(k->url) != ul || !link_url_ok(k->url)) return false;
        o += ul;
        tmp.n++;
    }
    if (o != end) {
        return false;                                    /* trailing bytes */
    }
    *out = tmp;
    return true;
}
