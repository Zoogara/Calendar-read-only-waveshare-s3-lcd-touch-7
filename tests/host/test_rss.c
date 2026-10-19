/* rss_parse(): RSS 2.0 and Atom reading, entity decoding, tag stripping, truncation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rss_parse.h"
#include "test.h"

static char scratch[RSS_SCRATCH_LEN];
static rss_feed_t feed;

static int parse(const char *xml)
{
    return rss_parse(xml, strlen(xml), &feed, scratch);
}

/* Structurally valid UTF-8 (no stray continuation or cut-off sequences). */
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

static void rss2_basic(void)
{
    CHECK_INT(parse("<?xml version=\"1.0\"?><rss version=\"2.0\"><channel><title>Port Hartley Gazette</title>"
                    "<link>https://example.invalid/</link>"
                    "<item><title>First story</title><description>Body one.</description></item>"
                    "<item><title>Second story</title><description>Body two.</description></item>"
                    "</channel></rss>"), 2);
    CHECK_STR(feed.name, "Port Hartley Gazette");
    CHECK_STR(feed.items[0].title, "First story");
    CHECK_STR(feed.items[0].text, "Body one.");
    CHECK_STR(feed.items[1].title, "Second story");
    CHECK_STR(feed.items[1].text, "Body two.");
}

static void no_channel_title(void)
{
    CHECK_INT(parse("<rss><channel><item><title>A</title></item></channel></rss>"), 1);
    CHECK_STR(feed.name, "");
    CHECK_STR(feed.items[0].text, "");
}

static void item_with_attributes(void)
{
    CHECK_INT(parse("<rdf><item rdf:about=\"x\"><title>A</title></item></rdf>"), 1);
    CHECK_STR(feed.items[0].title, "A");
}

static void cdata_and_markup(void)
{
    CHECK_INT(parse("<rss><channel><item><title><![CDATA[Plain heading]]></title>"
                    "<description><![CDATA[<p>Hello <b>big</b>   world</p><p>Second</p>]]></description>"
                    "</item></channel></rss>"), 1);
    CHECK_STR(feed.items[0].title, "Plain heading");
    CHECK_STR(feed.items[0].text, "Hello big world Second");
}

static void escaped_markup_is_dropped(void)
{
    /* Feeds often carry HTML as escaped text; it is stripped just the same. */
    CHECK_INT(parse("<rss><item><title>Fish &amp; chips</title>"
                    "<description>&lt;p&gt;Fresh &lt;i&gt;today&lt;/i&gt;&lt;/p&gt;</description></item></rss>"), 1);
    CHECK_STR(feed.items[0].title, "Fish & chips");
    CHECK_STR(feed.items[0].text, "Fresh today");
}

static void entities(void)
{
    CHECK_INT(parse("<rss><item><title>Caf&#233; &amp; Bar &#x2019; &mdash; &foo; end</title></item></rss>"), 1);
    CHECK_STR(feed.items[0].title, "Caf\xC3\xA9 & Bar \xE2\x80\x99 \xE2\x80\x94 &foo; end");
}

static void literal_angle_brackets_stay(void)
{
    CHECK_INT(parse("<rss><item><title>1 &lt; 2 and 3 &gt; 2</title><description>I &lt;3 you</description></item></rss>"), 1);
    CHECK_STR(feed.items[0].title, "1 < 2 and 3 > 2");
    CHECK_STR(feed.items[0].text, "I <3 you");
}

static void whitespace_is_collapsed(void)
{
    CHECK_INT(parse("<rss><item><title>  Lots   of\n\t space\xC2\xA0here  </title></item></rss>"), 1);
    CHECK_STR(feed.items[0].title, "Lots of space here");
}

static void atom_feed(void)
{
    CHECK_INT(parse("<feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Surf Report</title>"
                    "<entry><title>Swell building</title><summary>Two to three metres.</summary></entry>"
                    "<entry><title type=\"html\">Water &amp; wind</title>"
                    "<content type=\"html\">&lt;p&gt;Light offshore&lt;/p&gt;</content></entry></feed>"), 2);
    CHECK_STR(feed.name, "Surf Report");
    CHECK_STR(feed.items[0].title, "Swell building");
    CHECK_STR(feed.items[0].text, "Two to three metres.");
    CHECK_STR(feed.items[1].title, "Water & wind");
    CHECK_STR(feed.items[1].text, "Light offshore");
}

static void body_element_priority(void)
{
    CHECK_INT(parse("<rss><item><title>A</title><summary>from summary</summary>"
                    "<description>from description</description></item></rss>"), 1);
    CHECK_STR(feed.items[0].text, "from description");
}

static void namespaced_title_is_not_a_title(void)
{
    CHECK_INT(parse("<rss><item><media:title>Nope</media:title><description>x</description></item>"
                    "<item><title>Real</title></item></rss>"), 1);
    CHECK_STR(feed.items[0].title, "Real");
}

static void same_title_and_text(void)
{
    CHECK_INT(parse("<rss><item><title>Same</title><description>Same</description></item></rss>"), 1);
    CHECK_STR(feed.items[0].text, "");
}

static void truncated_document(void)
{
    /* Cut off in the second item's body: it keeps its heading. */
    CHECK_INT(parse("<rss><channel><title>T</title>"
                    "<item><title>One</title><description>First</description></item>"
                    "<item><title>Two</title><description>Cut off in the mid"), 2);
    CHECK_STR(feed.items[0].text, "First");
    CHECK_STR(feed.items[1].title, "Two");
    CHECK_STR(feed.items[1].text, "");
    /* Cut off inside the second item's heading: that item is dropped. */
    CHECK_INT(parse("<rss><item><title>One</title></item><item><title>Thr"), 1);
    CHECK_STR(feed.items[0].title, "One");
}

static void empty_and_junk_input(void)
{
    CHECK_INT(rss_parse("", 0, &feed, scratch), 0);
    CHECK_INT(parse("<rss><channel></channel></rss>"), 0);
    CHECK_INT(parse("not xml at all"), 0);
    CHECK_INT(parse("<item><title></title></item>"), 0);
}

static void item_cap(void)
{
    char *xml = malloc(8192);
    size_t n = 0;
    n += (size_t)snprintf(xml + n, 8192 - n, "<rss><channel><title>Big</title>");
    for (int i = 0; i < 20; i++) {
        n += (size_t)snprintf(xml + n, 8192 - n, "<item><title>Story %d</title><description>Body %d</description></item>", i, i);
    }
    CHECK_INT(rss_parse(xml, n, &feed, scratch), RSS_MAX_ITEMS);
    CHECK_STR(feed.items[RSS_MAX_ITEMS - 1].title, "Story 14");
    free(xml);
}

static void long_text_is_cut_cleanly(void)
{
    char xml[2048];
    char body[700];
    size_t ell = strlen("\xE2\x80\xA6");

    /* plain ASCII, no spaces */
    memset(body, 'x', 500);
    body[500] = '\0';
    snprintf(xml, sizeof(xml), "<rss><item><title>T</title><description>%s</description></item></rss>", body);
    CHECK_INT(parse(xml), 1);
    size_t len = strlen(feed.items[0].text);
    CHECK(len <= RSS_TEXT_LEN - 1);
    CHECK(len > ell && memcmp(feed.items[0].text + len - ell, "\xE2\x80\xA6", ell) == 0);

    /* two-byte characters: the cut must not split one */
    body[0] = '\0';
    for (int i = 0; i < 300; i++) {
        strcat(body, "\xC3\xA9");
    }
    snprintf(xml, sizeof(xml), "<rss><item><title>T</title><description>%s</description></item></rss>", body);
    CHECK_INT(parse(xml), 1);
    len = strlen(feed.items[0].text);
    CHECK(len <= RSS_TEXT_LEN - 1);
    CHECK(valid_utf8(feed.items[0].text));
    CHECK(memcmp(feed.items[0].text + len - ell, "\xE2\x80\xA6", ell) == 0);

    /* a long heading too */
    char big[400];
    memset(big, 'h', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    snprintf(xml, sizeof(xml), "<rss><item><title>%s</title></item></rss>", big);
    CHECK_INT(parse(xml), 1);
    CHECK(strlen(feed.items[0].title) <= RSS_TITLE_LEN - 1);
}

void test_rss(void)
{
    RUN(rss2_basic);
    RUN(no_channel_title);
    RUN(item_with_attributes);
    RUN(cdata_and_markup);
    RUN(escaped_markup_is_dropped);
    RUN(entities);
    RUN(literal_angle_brackets_stay);
    RUN(whitespace_is_collapsed);
    RUN(atom_feed);
    RUN(body_element_priority);
    RUN(namespaced_title_is_not_a_title);
    RUN(same_title_and_text);
    RUN(truncated_document);
    RUN(empty_and_junk_input);
    RUN(item_cap);
    RUN(long_text_is_cut_cleanly);
}
