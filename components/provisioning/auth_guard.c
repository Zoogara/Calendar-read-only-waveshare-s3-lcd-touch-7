#include "auth_guard.h"

#include <string.h>

#define US_PER_S 1000000LL

void auth_guard_init(auth_guard_t *g)
{
    memset(g, 0, sizeof(*g));
}

bool auth_ct_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la != lb);
    for (size_t i = 0; i < n; i++) {
        /* Past the end of the shorter string, compare against 0 without reading it. */
        unsigned char ca = (unsigned char)(i < la ? a[i] : 0);
        unsigned char cb = (unsigned char)(i < lb ? b[i] : 0);
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

bool auth_guard_locked(const auth_guard_t *g, int64_t now_us, uint32_t *retry_after_s)
{
    if (g->locked_until_us == 0 || now_us >= g->locked_until_us) {
        return false;
    }
    if (retry_after_s != NULL) {
        int64_t left = g->locked_until_us - now_us;
        *retry_after_s = (uint32_t)((left + US_PER_S - 1) / US_PER_S);   /* round up */
    }
    return true;
}

static void lock_out(auth_guard_t *g, int64_t now_us)
{
    int64_t secs = AUTH_GUARD_LOCKOUT_FIRST_S;
    for (uint32_t i = 0; i < g->lockouts && secs < AUTH_GUARD_LOCKOUT_MAX_S; i++) {
        secs *= 2;
    }
    if (secs > AUTH_GUARD_LOCKOUT_MAX_S) {
        secs = AUTH_GUARD_LOCKOUT_MAX_S;
    }
    g->locked_until_us = now_us + secs * US_PER_S;
    if (g->lockouts < 32) {
        g->lockouts++;
    }
}

void auth_guard_record_failure(auth_guard_t *g, int64_t now_us)
{
    if (g->failures < UINT32_MAX) {
        g->failures++;
    }
    /* The count isn't cleared by a lockout: once five failures have piled up, every
     * further wrong password locks out again, for longer. */
    if (g->failures >= AUTH_GUARD_MAX_FAILURES) {
        lock_out(g, now_us);
    }
}

auth_result_t auth_guard_check(auth_guard_t *g, int64_t now_us, const char *supplied,
                               const char *expected, uint32_t *retry_after_s)
{
    if (auth_guard_locked(g, now_us, retry_after_s)) {
        return AUTH_LOCKED;
    }
    /* Always do the comparison, even for an empty expected password, so the work done
     * doesn't depend on whether one is set; an empty expected never matches. */
    bool same = auth_ct_equal(supplied, expected);
    if (same && expected[0] != '\0') {
        g->failures = 0;
        g->lockouts = 0;
        g->locked_until_us = 0;
        return AUTH_OK;
    }
    auth_guard_record_failure(g, now_us);
    return AUTH_BAD;
}
