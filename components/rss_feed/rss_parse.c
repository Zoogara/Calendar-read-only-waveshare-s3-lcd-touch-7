#include "rss_parse.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small string helpers ---- */

/* Appends the UTF-8 encoding of `cp` at *w, never past `end`. */
static void put_utf8(char **w, char *end, uint32_t cp)
{
    char buf[4];
    int n;
    if (cp < 0x80) {
        buf[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*w + n > end) {
        return;
    }
    memcpy(*w, buf, n);
    *w += n;
}

/* Named entity at s ("&amp;" ...): returns its length and sets *cp, or 0. */
static size_t entity_at(const char *s, const char *e, uint32_t *cp)
{
    if (*s != '&') {
        return 0;
    }
    const char *semi = memchr(s, ';', (size_t)(e - s) < 12 ? (size_t)(e - s) : 12);
    if (semi == NULL) {
        return 0;
    }
    size_t n = (size_t)(semi - s) + 1;
    if (s[1] == '#') {
        char *endp;
        unsigned long v = (s[2] == 'x' || s[2] == 'X') ? strtoul(s + 3, &endp, 16)
                                                         : strtoul(s + 2, &endp, 10);
        if (endp != semi || v == 0 || v > 0x10FFFF) {
            return 0;
        }
        *cp = (uint32_t)v;
        return n;
    }
    static const struct { const char *name; uint32_t cp; } NAMED[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
        {"&nbsp;", ' '}, {"&ndash;", 0x2013}, {"&mdash;", 0x2014}, {"&lsquo;", 0x2018},
        {"&rsquo;", 0x2019}, {"&ldquo;", 0x201C}, {"&rdquo;", 0x201D}, {"&hellip;", 0x2026},
    };
    for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
        size_t l = strlen(NAMED[i].name);
        if (l == n && memcmp(s, NAMED[i].name, l) == 0) {
            *cp = NAMED[i].cp;
            return n;
        }
    }
    return 0;
}

/* Raw element content [s, e) -> plain text in out[out_sz]: unwraps CDATA,
 * decodes entities, drops any markup (escaped or not), collapses runs of
 * whitespace, cuts at a UTF-8 boundary with an ellipsis if it doesn't fit. */
static void clean_text(const char *s, const char *e, char *out, size_t out_sz, char *scratch)
{
    out[0] = '\0';
    /* 1. unwrap CDATA, decode entities into scratch */
    char *w = scratch, *wend = scratch + RSS_SCRATCH_LEN - 1;
    while (s < e && w < wend) {
        if (e - s >= 9 && memcmp(s, "<![CDATA[", 9) == 0) {
            s += 9;
            while (s < e && w < wend && !(e - s >= 3 && memcmp(s, "]]>", 3) == 0)) {
                *w++ = *s++;
            }
            if (e - s >= 3) {
                s += 3;
            }
            continue;
        }
        uint32_t cp;
        size_t n = entity_at(s, e, &cp);
        if (n) {
            put_utf8(&w, wend, cp);
            s += n;
        } else {
            *w++ = *s++;
        }
    }
    *w = '\0';

    /* 2. strip tags, collapse whitespace, truncate */
    char *o = out, *oend = out + out_sz - 1;
    int pending_space = 0, truncated = 0;
    for (const char *p = scratch; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (c == '<') {
            const char *gt = strchr(p, '>');
            if (gt != NULL && gt - p < 400 && (isalpha((unsigned char)p[1]) || p[1] == '/' || p[1] == '!')) {
                pending_space = 1;     /* a tag reads as a break */
                p = gt + 1;
                continue;
            }
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || (c == 0xC2 && (unsigned char)p[1] == 0xA0)) {
            pending_space = 1;
            p += (c == 0xC2) ? 2 : 1;
            continue;
        }
        /* copy one whole UTF-8 character */
        int clen = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        size_t need = (size_t)clen + (pending_space && o != out ? 1 : 0);
        if (o + need > oend - 3) {          /* keep room for the ellipsis */
            truncated = 1;
            break;
        }
        if (pending_space && o != out) {
            *o++ = ' ';
        }
        pending_space = 0;
        for (int i = 0; i < clen && p[i]; i++) {
            *o++ = p[i];
        }
        p += clen;
    }
    if (truncated) {
        memcpy(o, "\xE2\x80\xA6", 3);        /* U+2026 */
        o += 3;
    }
    *o = '\0';
}

/* ---- element lookup ---- */

static int is_name_end(char c)
{
    return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* First "<name ...>" in [s, e); returns the '<' or NULL. `name` must be the
 * whole element name (so "title" doesn't match "titles" or "media:title"). */
static const char *find_open(const char *s, const char *e, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = s; p + nl + 1 < e; p++) {
        p = memchr(p, '<', (size_t)(e - p));
        if (p == NULL || p + nl + 1 >= e) {
            return NULL;
        }
        if (memcmp(p + 1, name, nl) == 0 && is_name_end(p[1 + nl])) {
            return p;
        }
    }
    return NULL;
}

/* Content of the first <name>...</name> in [s, e). */
static int element(const char *s, const char *e, const char *name, const char **cs, const char **ce)
{
    const char *open = find_open(s, e, name);
    if (open == NULL) {
        return 0;
    }
    const char *gt = memchr(open, '>', (size_t)(e - open));
    if (gt == NULL) {
        return 0;
    }
    if (gt[-1] == '/') {            /* <name/> - empty */
        *cs = *ce = gt;
        return 1;
    }
    char close[40];
    snprintf(close, sizeof(close), "</%s>", name);
    size_t cl = strlen(close);
    for (const char *p = gt + 1; p + cl <= e; p++) {
        p = memchr(p, '<', (size_t)(e - p));
        if (p == NULL || p + cl > e) {
            return 0;
        }
        if (memcmp(p, close, cl) == 0) {
            *cs = gt + 1;
            *ce = p;
            return 1;
        }
    }
    return 0;
}

/* The value of attribute `name` inside the tag text [s, e): sets *vs / *ve to the value (without its quotes). */
static int attr_value(const char *s, const char *e, const char *name, const char **vs, const char **ve)
{
    size_t nl = strlen(name);
    for (const char *p = s; p + nl + 2 < e; p++) {
        if ((p[0] == ' ' || p[0] == '\t' || p[0] == '\r' || p[0] == '\n') && strncmp(p + 1, name, nl) == 0) {
            const char *q = p + 1 + nl;
            while (q < e && (*q == ' ' || *q == '\t')) {
                q++;
            }
            if (q >= e || *q != '=') {
                continue;
            }
            q++;
            while (q < e && (*q == ' ' || *q == '\t')) {
                q++;
            }
            if (q >= e || (*q != '"' && *q != '\'')) {
                continue;
            }
            char quote = *q++;
            const char *close = memchr(q, quote, (size_t)(e - q));
            if (close == NULL) {
                return 0;
            }
            *vs = q;
            *ve = close;
            return 1;
        }
    }
    return 0;
}

/* The item's own address. RSS 2.0 has <link>https://...</link> (possibly in CDATA); Atom has
 * <link href="..." rel="alternate"/>, often next to a rel="self" or "enclosure" one that must not be taken. If there
 * is no usable <link>, an RSS <guid> that is itself an address will do. Anything that isn't a plain web address, or
 * that doesn't fit, is dropped and the item simply has no link. */
static void item_link(const char *start, const char *end, char *out, size_t out_sz, char *scratch)
{
    out[0] = '\0';
    const char *p = start;
    while ((p = find_open(p, end, "link")) != NULL) {
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (gt == NULL) {
            return;
        }
        const char *vs, *ve, *cs, *ce;
        bool have = false;
        if (attr_value(p, gt, "href", &vs, &ve)) {
            const char *rs, *re;
            bool alternate = true;
            if (attr_value(p, gt, "rel", &rs, &re)) {
                alternate = (size_t)(re - rs) == 9 && memcmp(rs, "alternate", 9) == 0;
            }
            if (alternate) {
                clean_text(vs, ve, out, out_sz, scratch);
                have = true;
            }
        } else if (gt[-1] != '/' && element(p, end, "link", &cs, &ce)) {
            clean_text(cs, ce, out, out_sz, scratch);
            have = true;
        }
        if (have && link_url_ok(out)) {
            return;
        }
        out[0] = '\0';
        p = gt + 1;
    }
    const char *cs, *ce;
    if (element(start, end, "guid", &cs, &ce)) {
        clean_text(cs, ce, out, out_sz, scratch);
        if (!link_url_ok(out)) {
            out[0] = '\0';
        }
    }
}

int rss_parse(const char *xml, size_t len, rss_feed_t *out, char *scratch)
{
    const char *end = xml + len;
    memset(out, 0, sizeof(*out));

    const char *pos = xml;
    int first = 1;
    while (out->count < RSS_MAX_ITEMS) {
        const char *item = find_open(pos, end, "item");
        const char *entry = find_open(pos, end, "entry");
        const char *start = item;
        const char *close_tag = "</item>";
        if (entry != NULL && (item == NULL || entry < item)) {
            start = entry;
            close_tag = "</entry>";
        }
        if (start == NULL) {
            break;
        }
        const char *cs, *ce;
        if (first) {
            /* The feed's own title sits before the first item. */
            if (element(xml, start, "title", &cs, &ce)) {
                clean_text(cs, ce, out->name, sizeof(out->name), scratch);
            }
            first = 0;
        }
        const char *stop = strstr(start, close_tag);
        const char *item_end = (stop != NULL && stop < end) ? stop : end;

        rss_item_t *it = &out->items[out->count];
        if (element(start, item_end, "title", &cs, &ce)) {
            clean_text(cs, ce, it->title, sizeof(it->title), scratch);
        }
        if (element(start, item_end, "description", &cs, &ce) ||
            element(start, item_end, "summary", &cs, &ce) ||
            element(start, item_end, "content", &cs, &ce)) {
            clean_text(cs, ce, it->text, sizeof(it->text), scratch);
        }
        item_link(start, item_end, it->url, sizeof(it->url), scratch);
        /* An item with no heading is of no use; one cut off mid-way by the
         * size cap has no closing tag - keep it only if it has a heading. */
        if (it->title[0] != '\0') {
            if (strcmp(it->title, it->text) == 0) {
                it->text[0] = '\0';
            }
            out->count++;
        }
        if (stop == NULL || stop >= end) {
            break;
        }
        pos = stop + strlen(close_tag);
    }
    return out->count;
}
