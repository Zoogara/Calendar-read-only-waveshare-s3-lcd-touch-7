/* event_text.c: cleaning an event's location/description for the details box. */
#include <string.h>

#include "event_text.h"
#include "test.h"

static char out[256];

static void clean(const char *in)
{
    gcal_clean_text(in, out, sizeof(out));
}

static int valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
        if (n == 0) {
            return 0;
        }
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                return 0;
            }
        }
        p += n;
    }
    return 1;
}

static void plain_text_is_unchanged(void)
{
    clean("Bring a plate");        CHECK_STR(out, "Bring a plate");
    clean("");                     CHECK_STR(out, "");
    gcal_clean_text(NULL, out, sizeof(out));
    CHECK_STR(out, "");
    clean("Caf\xC3\xA9 Port Hartley"); CHECK_STR(out, "Caf\xC3\xA9 Port Hartley");
}

static void tags_are_dropped(void)
{
    clean("<b>Bring</b> a <i>plate</i>");                  CHECK_STR(out, "Bring a plate");
    clean("<a href=\"http://x.invalid/y?a=1&b=2\">Join</a> now"); CHECK_STR(out, "Join now");
    clean("one<span>two</span>three");                     CHECK_STR(out, "one two three");
}

static void block_tags_become_line_breaks(void)
{
    clean("Line1<br>Line2<br/>Line3");            CHECK_STR(out, "Line1\nLine2\nLine3");
    clean("<p>One</p><p>Two</p>");                CHECK_STR(out, "One\nTwo");
    clean("<ul><li>a</li><li>b</li></ul>");       CHECK_STR(out, "a\nb");
    clean("<div>Agenda</div><div>Notes</div>");   CHECK_STR(out, "Agenda\nNotes");
    clean("<br>Hello<br><br>");                   CHECK_STR(out, "Hello");        /* edges trimmed */
    clean("<BR>Upper<P>Case");                    CHECK_STR(out, "Upper\nCase");
}

static void entities(void)
{
    clean("Tom &amp; Jerry &lt;3 &quot;hi&quot; &apos;x&apos; a&nbsp;b");
    CHECK_STR(out, "Tom & Jerry <3 \"hi\" 'x' a b");
    clean("&rsquo;s &ldquo;q&rdquo; a &ndash; b");   CHECK_STR(out, "'s \"q\" a - b");
    clean("&#233;&#x41;");                          CHECK_STR(out, "\xC3\xA9" "A");
    clean("&#x1F600;");                             CHECK_STR(out, "\xF0\x9F\x98\x80");
    clean("&foo; &amp");                            CHECK_STR(out, "&foo; &amp");   /* unknown / unterminated: left as text */
    clean("&#0; &#99999999;");                      CHECK_STR(out, "&#0; &#99999999;");
}

static void decoded_angle_brackets_are_text_not_tags(void)
{
    clean("&lt;b&gt;bold&lt;/b&gt;");               CHECK_STR(out, "<b>bold</b>");
}

static void whitespace(void)
{
    clean("  a \t\t b \n\n\n c  ");                 CHECK_STR(out, "a b\nc");
    clean("a\r\nb\r\n\r\nc");                       CHECK_STR(out, "a\nb\nc");
    clean("a\xC2\xA0\xC2\xA0 b");                   CHECK_STR(out, "a b");
    clean("\x01" "a\x7F" "b");                      CHECK_STR(out, "ab");               /* control characters dropped */
}

static void literal_angle_brackets_stay(void)
{
    clean("5 < 6 and 7 > 3");                       CHECK_STR(out, "5 < 6 and 7 > 3");
    clean("I <3 you");                              CHECK_STR(out, "I <3 you");
    clean("<a href=");                              CHECK_STR(out, "<a href=");         /* never closed: just text */
}

static void truncation(void)
{
    char small[20];
    gcal_clean_text("0123456789abcdefghijklmnopqrstuvwxyz", small, sizeof(small));
    CHECK_INT(strlen(small), 19);                               /* 16 characters and the ellipsis */
    CHECK(memcmp(small + 16, "\xE2\x80\xA6", 3) == 0);

    gcal_clean_text("0123456789abcdefghi", small, sizeof(small));  /* exactly 19: fits, no ellipsis */
    CHECK_STR(small, "0123456789abcdefghi");
    gcal_clean_text("0123456789abcdefghij", small, sizeof(small)); /* 20: one too many */
    CHECK(memcmp(small + strlen(small) - 3, "\xE2\x80\xA6", 3) == 0);

    char tiny[1] = {'x'};
    gcal_clean_text("abc", tiny, sizeof(tiny));
    CHECK_INT(tiny[0], 0);
    char two[2];
    gcal_clean_text("abc", two, sizeof(two));
    CHECK(strlen(two) <= 1);
}

static void truncation_never_splits_a_character(void)
{
    char in[200];
    in[0] = '\0';
    for (int i = 0; i < 60; i++) {
        strcat(in, "\xC3\xA9");                                  /* two-byte e-acute */
    }
    for (size_t sz = 2; sz < 130; sz += 3) {
        char buf[130];
        memset(buf, 0x5A, sizeof(buf));
        gcal_clean_text(in, buf, sz);
        CHECK(valid_utf8(buf));
        CHECK(strlen(buf) < sz);
        CHECK(buf[sz] == 0x5A || sz >= sizeof(buf));              /* nothing written past the buffer */
    }
    char em[256];
    em[0] = '\0';
    for (int i = 0; i < 50; i++) {
        strcat(em, "\xF0\x9F\x98\x80");                          /* four-byte emoji */
    }
    char buf[41];
    gcal_clean_text(em, buf, sizeof(buf));
    CHECK(valid_utf8(buf));
}

static void a_realistic_description(void)
{
    clean("<p>Join us for the <b>annual</b> picnic!</p><p>Bring: <ul><li>a chair</li><li>a hat</li></ul></p>"
          "<a href=\"https://example.invalid/rsvp\">RSVP here</a> &mdash; thanks");
    CHECK_STR(out, "Join us for the annual picnic!\nBring:\na chair\na hat\nRSVP here - thanks");
}

void test_eventtext(void)
{
    RUN(plain_text_is_unchanged);
    RUN(tags_are_dropped);
    RUN(block_tags_become_line_breaks);
    RUN(entities);
    RUN(decoded_angle_brackets_are_text_not_tags);
    RUN(whitespace);
    RUN(literal_angle_brackets_stay);
    RUN(truncation);
    RUN(truncation_never_splits_a_character);
    RUN(a_realistic_description);
}
