/* auth_guard.c: constant-time compare, failure counting, lockout and its doubling. */
#include <stdint.h>

#include "auth_guard.h"
#include "test.h"

#define S 1000000LL
#define PW "correct horse"

static auth_guard_t g;
static uint32_t retry;

static auth_result_t try_pw(int64_t now_s, const char *pw)
{
    retry = 0;
    return auth_guard_check(&g, now_s * S, pw, PW, &retry);
}

static void compare(void)
{
    CHECK(auth_ct_equal("", ""));
    CHECK(auth_ct_equal("abc", "abc"));
    CHECK(!auth_ct_equal("abc", "abd"));
    CHECK(!auth_ct_equal("abc", "xbc"));
    CHECK(!auth_ct_equal("abc", "ab"));          /* prefix of the right one */
    CHECK(!auth_ct_equal("ab", "abc"));
    CHECK(!auth_ct_equal("abc", ""));
    CHECK(!auth_ct_equal("", "abc"));
    CHECK(!auth_ct_equal("abc", "abc\x01"));     /* same up to the extra byte */
    CHECK(auth_ct_equal("p\xC3\xA9ss", "p\xC3\xA9ss"));
    CHECK(!auth_ct_equal("p\xC3\xA9ss", "p\xC3\xA8ss"));
}

static void correct_wrong_empty_prefix(void)
{
    auth_guard_init(&g);
    CHECK_INT(try_pw(0, PW), AUTH_OK);
    CHECK_INT(try_pw(0, "wrong"), AUTH_BAD);
    CHECK_INT(try_pw(0, ""), AUTH_BAD);
    CHECK_INT(try_pw(0, "correct hors"), AUTH_BAD);          /* a prefix of the right one */
    CHECK_INT(try_pw(0, "correct horse!"), AUTH_BAD);        /* the right one plus more */
    /* those were four failures: still one short of a lockout */
    CHECK(!auth_guard_locked(&g, 0, NULL));
    CHECK_INT(try_pw(0, PW), AUTH_OK);
    CHECK_INT(try_pw(0, "Correct horse"), AUTH_BAD);         /* case matters */
}

static void no_password_set_accepts_nobody(void)
{
    auth_guard_init(&g);
    CHECK_INT(auth_guard_check(&g, 0, "", "", &retry), AUTH_BAD);
    CHECK_INT(auth_guard_check(&g, 0, "anything", "", &retry), AUTH_BAD);
}

static void five_failures_then_lockout(void)
{
    auth_guard_init(&g);
    for (int i = 0; i < 4; i++) {
        CHECK_INT(try_pw(10, "wrong"), AUTH_BAD);
        CHECK(!auth_guard_locked(&g, 10 * S, NULL));
    }
    CHECK_INT(try_pw(10, "wrong"), AUTH_BAD);                /* the 5th is still just "bad" */
    CHECK(auth_guard_locked(&g, 10 * S, &retry));
    CHECK_INT(retry, 30);

    /* locked: refused without being compared - even with the right password */
    CHECK_INT(try_pw(11, PW), AUTH_LOCKED);
    CHECK_INT(retry, 29);
    CHECK_INT(try_pw(11, "wrong"), AUTH_LOCKED);
    CHECK_INT(try_pw(39, PW), AUTH_LOCKED);                  /* one second to go */
    CHECK_INT(retry, 1);
    CHECK(auth_guard_locked(&g, 39 * S + 999999, &retry));
    CHECK_INT(retry, 1);                                     /* rounded up, never 0 */
}

static void lockout_expires(void)
{
    auth_guard_init(&g);
    for (int i = 0; i < 5; i++) {
        try_pw(100, "wrong");
    }
    CHECK(auth_guard_locked(&g, 129 * S, NULL));
    CHECK(!auth_guard_locked(&g, 130 * S, NULL));            /* 30 s after it started */
    CHECK_INT(try_pw(130, PW), AUTH_OK);
    CHECK(!auth_guard_locked(&g, 130 * S, NULL));
}

static void lockout_doubles_up_to_the_cap(void)
{
    auth_guard_init(&g);
    int64_t t = 0;
    for (int i = 0; i < 5; i++) {
        try_pw(t, "wrong");
    }
    const uint32_t want[] = {30, 60, 120, 240, 480, 900, 900, 900};
    for (int i = 0; i < 8; i++) {
        CHECK(auth_guard_locked(&g, t * S, &retry));
        CHECK_INT(retry, want[i]);
        t += want[i];                                        /* wait it out, then one more wrong guess */
        CHECK(!auth_guard_locked(&g, t * S, NULL));
        CHECK_INT(try_pw(t, "wrong"), AUTH_BAD);
    }
}

static void success_resets_everything(void)
{
    auth_guard_init(&g);
    for (int i = 0; i < 5; i++) {
        try_pw(0, "wrong");
    }
    try_pw(30, "wrong");                                     /* a second, 60 s lockout */
    CHECK(auth_guard_locked(&g, 30 * S, &retry));
    CHECK_INT(retry, 60);
    CHECK_INT(try_pw(90, PW), AUTH_OK);

    /* back to the beginning: four wrong guesses are fine, the fifth locks for 30 s again */
    for (int i = 0; i < 4; i++) {
        try_pw(100, "wrong");
    }
    CHECK(!auth_guard_locked(&g, 100 * S, NULL));
    try_pw(100, "wrong");
    CHECK(auth_guard_locked(&g, 100 * S, &retry));
    CHECK_INT(retry, 30);
}

static void successes_between_failures_never_lock(void)
{
    auth_guard_init(&g);
    for (int i = 0; i < 50; i++) {
        CHECK_INT(try_pw(i, "wrong"), AUTH_BAD);
        CHECK_INT(try_pw(i, PW), AUTH_OK);
    }
    CHECK(!auth_guard_locked(&g, 50 * S, NULL));
}

static void malformed_credentials_count(void)
{
    auth_guard_init(&g);
    for (int i = 0; i < 5; i++) {
        auth_guard_record_failure(&g, 0);
    }
    CHECK(auth_guard_locked(&g, 0, &retry));
    CHECK_INT(retry, 30);
}

static void works_near_time_zero_and_huge_times(void)
{
    auth_guard_init(&g);
    CHECK(!auth_guard_locked(&g, 0, NULL));                  /* nothing locked at boot */
    for (int i = 0; i < 5; i++) {
        try_pw(0, "wrong");
    }
    CHECK(auth_guard_locked(&g, 0, NULL));

    auth_guard_init(&g);                                     /* ~292 years of uptime */
    int64_t big = (int64_t)9000000000LL;
    for (int i = 0; i < 5; i++) {
        try_pw(big, "wrong");
    }
    CHECK(auth_guard_locked(&g, big * S, &retry));
    CHECK_INT(retry, 30);
}

void test_auth(void)
{
    RUN(compare);
    RUN(correct_wrong_empty_prefix);
    RUN(no_password_set_accepts_nobody);
    RUN(five_failures_then_lockout);
    RUN(lockout_expires);
    RUN(lockout_doubles_up_to_the_cap);
    RUN(success_resets_everything);
    RUN(successes_between_failures_never_lock);
    RUN(malformed_credentials_count);
    RUN(works_near_time_zero_and_huge_times);
}
