/* url_percent_decode(): what the /ota handler does with the url= query value. */
#include <string.h>

#include "test.h"
#include "url_decode.h"

static char out[300];

static void plain_and_empty(void)
{
    CHECK(url_percent_decode("https://example.invalid/fw.bin", out, sizeof(out)));
    CHECK_STR(out, "https://example.invalid/fw.bin");
    CHECK(url_percent_decode("", out, sizeof(out)));
    CHECK_STR(out, "");
}

static void percent_sequences(void)
{
    CHECK(url_percent_decode("http%3A%2F%2F192.168.0.5%3A8070%2Fgcal_display.bin", out, sizeof(out)));
    CHECK_STR(out, "http://192.168.0.5:8070/gcal_display.bin");
    CHECK(url_percent_decode("a%20b", out, sizeof(out)));
    CHECK_STR(out, "a b");
    CHECK(url_percent_decode("%25", out, sizeof(out)));
    CHECK_STR(out, "%");
    CHECK(url_percent_decode("%2541", out, sizeof(out)));          /* decoded once, not twice */
    CHECK_STR(out, "%41");
    CHECK(url_percent_decode("caf%C3%A9", out, sizeof(out)));
    CHECK_STR(out, "caf\xC3\xA9");
}

static void hex_case(void)
{
    CHECK(url_percent_decode("%2f%2F%aB%Ab", out, sizeof(out)));
    CHECK_STR(out, "//\xAB\xAB");
}

static void plus_is_left_alone(void)
{
    CHECK(url_percent_decode("a+b%2Bc", out, sizeof(out)));
    CHECK_STR(out, "a+b+c");
}

static void invalid_sequences(void)
{
    const char *bad[] = {"%", "%4", "abc%", "abc%4", "%zz", "%4z", "%z4", "%g0", "%-1", "% 4", "100%", "%%41"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(out, 'x', sizeof(out));
        CHECK(!url_percent_decode(bad[i], out, sizeof(out)));
        CHECK_STR(out, "");                                         /* nothing half-decoded is left behind */
    }
}

static void nul_is_rejected(void)
{
    CHECK(!url_percent_decode("%00", out, sizeof(out)));
    CHECK(!url_percent_decode("http://a/%00.bin", out, sizeof(out)));
    CHECK(!url_percent_decode("a%00b", out, sizeof(out)));
    CHECK(url_percent_decode("%01%10", out, sizeof(out)));          /* other control bytes are the caller's business */
    CHECK_INT(strlen(out), 2);
}

static void buffer_limits(void)
{
    /* decoded text plus its NUL must fit */
    CHECK(url_percent_decode("abcde", out, 6));
    CHECK_STR(out, "abcde");
    CHECK(!url_percent_decode("abcdef", out, 6));                   /* one byte over */
    CHECK_STR(out, "");
    CHECK(url_percent_decode("%41%42", out, 3));                    /* six input bytes, two output */
    CHECK_STR(out, "AB");
    CHECK(!url_percent_decode("%41%42", out, 2));
    CHECK(!url_percent_decode("", out, 0));                         /* no room even for the NUL */
    CHECK(url_percent_decode("", out, 1));
    CHECK_STR(out, "");
    CHECK(!url_percent_decode("a", out, 1));
}

static void the_handlers_256_byte_buffer(void)
{
    char in[300], dst[256];
    memset(in, 'u', 255);
    in[255] = '\0';
    CHECK(url_percent_decode(in, dst, sizeof(dst)));                /* 255 characters: the most that fits */
    CHECK_INT(strlen(dst), 255);
    memset(in, 'u', 256);
    in[256] = '\0';
    CHECK(!url_percent_decode(in, dst, sizeof(dst)));               /* 256: one too many */
    CHECK_STR(dst, "");

    /* encoded input that decodes to fit: 255 x "%41" is 765 characters in, 255 out */
    static char big[800];
    for (int i = 0; i < 255; i++) {
        memcpy(big + 3 * i, "%41", 3);
    }
    big[765] = '\0';
    CHECK(url_percent_decode(big, dst, sizeof(dst)));
    CHECK_INT(strlen(dst), 255);
    CHECK(dst[0] == 'A' && dst[254] == 'A');
}

void test_urldecode(void)
{
    RUN(plain_and_empty);
    RUN(percent_sequences);
    RUN(hex_case);
    RUN(plus_is_left_alone);
    RUN(invalid_sequences);
    RUN(nul_is_rejected);
    RUN(buffer_limits);
    RUN(the_handlers_256_byte_buffer);
}
