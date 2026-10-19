/* gcal_url_encode(): the query-string encoder gcal_client.c uses for calendar ids, times and tokens. */
#include <string.h>

#include "test.h"
#include "url_encode.h"

static char out[128];

static void check_enc(const char *in, const char *want)
{
    CHECK(gcal_url_encode(in, out, sizeof(out)));
    CHECK_STR(out, want);
}

static void unreserved_pass_through(void)
{
    check_enc("AZaz09-_.~", "AZaz09-_.~");
    check_enc("", "");
}

static void reserved_characters(void)
{
    check_enc(" ", "%20");
    check_enc("/", "%2F");
    check_enc("?&=#", "%3F%26%3D%23");
    check_enc("+:@,;", "%2B%3A%40%2C%3B");
    check_enc("%", "%25");                               /* encoded once, never "decoded" */
    check_enc("%41", "%2541");
    check_enc("\"<>\\{}|^`[]", "%22%3C%3E%5C%7B%7D%7C%5E%60%5B%5D");
    check_enc("\t\n", "%09%0A");
}

static void utf8_and_high_bytes(void)
{
    check_enc("caf\xC3\xA9", "caf%C3%A9");
    check_enc("\xE2\x82\xAC", "%E2%82%AC");
    check_enc("\xFF\x80", "%FF%80");                    /* not sign-extended */
}

static void the_firmwares_own_values(void)
{
    check_enc("family@group.calendar.google.com", "family%40group.calendar.google.com");
    check_enc("2026-10-14T00:00:00Z", "2026-10-14T00%3A00%3A00Z");
    check_enc("nextPageToken,items(status,summary)", "nextPageToken%2Citems%28status%2Csummary%29");
}

/* Whatever the buffer size: always NUL-terminated, never past the end, and a result that
 * doesn't fit is reported as such (and is a proper prefix, never a half escape). */
static void small_buffers_never_overrun(void)
{
    const char *in = "a b/c\xC3\xA9?d";
    char full[64];
    CHECK(gcal_url_encode(in, full, sizeof(full)));
    size_t full_len = strlen(full);

    for (size_t sz = 1; sz <= full_len + 4; sz++) {
        char mem[80];
        memset(mem, 0x5A, sizeof(mem));
        char *buf = mem + 8;
        bool ok = gcal_url_encode(in, buf, sz);
        CHECK(mem[8 + sz] == 0x5A);                      /* nothing written past out_sz */
        CHECK(memchr(buf, '\0', sz) != NULL);            /* terminated within the buffer */
        size_t got = strlen(buf);
        CHECK(got <= full_len && memcmp(buf, full, got) == 0);
        if (ok) {
            CHECK_INT(got, full_len);
        } else {
            CHECK(got < full_len);
            /* never ends in the middle of a %XX */
            CHECK(!(got >= 1 && buf[got - 1] == '%'));
            CHECK(!(got >= 2 && buf[got - 2] == '%'));
        }
    }
}

void test_urlenc(void)
{
    RUN(unreserved_pass_through);
    RUN(reserved_characters);
    RUN(utf8_and_high_bytes);
    RUN(the_firmwares_own_values);
    RUN(small_buffers_never_overrun);
}
