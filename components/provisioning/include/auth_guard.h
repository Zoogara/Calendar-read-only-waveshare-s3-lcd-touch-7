#pragma once
/*
 * Password check and brute-force throttle for the config web page. Pure C with no
 * ESP-IDF calls, so it is unit-tested on a computer (tests/host): the caller passes the
 * current time in, from a monotonic source (esp_timer_get_time()) - not wall-clock
 * time, which isn't set until SNTP has run.
 *
 * Five failures in a row lock the device out for 30 s; each further failure while the
 * count is still at or above five locks it out again for twice as long (30 s, 60 s,
 * 2 min ... capped at 15 min). While locked out even the correct password is refused,
 * without being compared. A correct password resets everything. The state lives in RAM
 * only, and is per device, not per client.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_GUARD_MAX_FAILURES   5
#define AUTH_GUARD_LOCKOUT_FIRST_S 30
#define AUTH_GUARD_LOCKOUT_MAX_S   (15 * 60)

typedef struct {
    uint32_t failures;          /* consecutive wrong passwords */
    uint32_t lockouts;          /* lockouts so far since the last success */
    int64_t locked_until_us;    /* 0 = not locked */
} auth_guard_t;

typedef enum {
    AUTH_OK,
    AUTH_BAD,       /* wrong (or empty) password */
    AUTH_LOCKED,    /* refused without checking; *retry_after_s is set */
} auth_result_t;

void auth_guard_init(auth_guard_t *g);

/* True if the guard is locked out at `now_us`; sets *retry_after_s (whole seconds, at
 * least 1) when it is. Does not change any state. */
bool auth_guard_locked(const auth_guard_t *g, int64_t now_us, uint32_t *retry_after_s);

/* Checks `supplied` against `expected`. An empty `expected` never matches: a device
 * with no password set accepts nobody. */
auth_result_t auth_guard_check(auth_guard_t *g, int64_t now_us, const char *supplied,
                               const char *expected, uint32_t *retry_after_s);

/* Records a failed attempt that never got as far as a comparison (e.g. a malformed
 * Authorization header that did carry credentials). */
void auth_guard_record_failure(auth_guard_t *g, int64_t now_us);

/* Compares two strings in time that depends only on the longer one's length, not on
 * where they first differ or on how long the shorter is. */
bool auth_ct_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
