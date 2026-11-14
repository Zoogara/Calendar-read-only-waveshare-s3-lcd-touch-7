#include "event_text.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ELLIPSIS "\xE2\x80\xA6"

typedef struct {
    char *out;
    size_t cap;        /* bytes usable, leaving room for the ellipsis and NUL */
    size_t n;
    int pending;       /* 0 none, 1 a space, 2 a line break */
    int truncated;
} sink_t;

static void put_bytes(sink_t *s, const char *b, size_t len)
{
    if (s->truncated) {
        return;
    }
    size_t sep = (s->pending && s->n > 0) ? 1 : 0;
    if (s->n + sep + len > s->cap) {
        s->truncated = 1;
        return;
    }
    if (sep) {
        s->out[s->n++] = (s->pending == 2) ? '\n' : ' ';
    }
    s->pending = 0;
    memcpy(s->out + s->n, b, len);
    s->n += len;
}

static void put_cp(sink_t *s, uint32_t cp)
{
    char b[4];
    size_t len;
    switch (cp) {
    case 0xA0:   cp = ' '; break;                       /* no-break space */
    case 0x2018: case 0x2019: cp = '\''; break;
    case 0x201C: case 0x201D: cp = '"'; break;
    case 0x2013: case 0x2014: cp = '-'; break;
    default: break;
    }
    if (cp == ' ' || cp == '\t') {
        if (s->pending < 1) {
            s->pending = 1;
        }
        return;
    }
    if (cp == '\n') {
        s->pending = 2;
        return;
    }
    if (cp < 0x20 || cp == 0x7F) {
        return;                                          /* other control characters */
    }
    if (cp < 0x80) {
        b[0] = (char)cp;
        len = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        len = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        len = 4;
    }
    put_bytes(s, b, len);
}

/* A named or numeric entity at p ("&amp;" ...): returns its length and sets *cp, or 0. */
static size_t entity_at(const char *p, uint32_t *cp)
{
    static const struct { const char *name; uint32_t cp; } NAMED[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
        {"&nbsp;", ' '}, {"&ndash;", '-'}, {"&mdash;", '-'}, {"&lsquo;", '\''}, {"&rsquo;", '\''},
        {"&ldquo;", '"'}, {"&rdquo;", '"'}, {"&hellip;", 0x2026},
    };
    if (p[0] != '&') {
        return 0;
    }
    const char *semi = memchr(p, ';', strnlen(p, 12));      /* never read past the string's end */
    if (semi == NULL) {
        return 0;
    }
    size_t n = (size_t)(semi - p) + 1;
    if (p[1] == '#') {
        char *endp;
        unsigned long v = (p[2] == 'x' || p[2] == 'X') ? strtoul(p + 3, &endp, 16) : strtoul(p + 2, &endp, 10);
        if (endp != semi || v == 0 || v > 0x10FFFF) {
            return 0;
        }
        *cp = (uint32_t)v;
        return n;
    }
    for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
        if (strlen(NAMED[i].name) == n && memcmp(p, NAMED[i].name, n) == 0) {
            *cp = NAMED[i].cp;
            return n;
        }
    }
    return 0;
}

/* A tag at p ("<br>", "</p>", "<a href=...>"): returns its length (to the '>') and sets *newline when
 * it should read as a line break; 0 if p isn't the start of a tag. */
static size_t tag_at(const char *p, int *newline)
{
    if (p[0] != '<' || !(isalpha((unsigned char)p[1]) || p[1] == '/' || p[1] == '!')) {
        return 0;
    }
    const char *gt = memchr(p, '>', strnlen(p, 400));       /* never read past the string's end */
    if (gt == NULL) {
        return 0;
    }
    const char *name = p + 1 + (p[1] == '/' ? 1 : 0);
    static const char *BREAKS[] = {"br", "p", "div", "li", "tr", "ul", "ol", "table", "h1", "h2", "h3", "h4", "h5", "h6"};
    *newline = 0;
    for (size_t i = 0; i < sizeof(BREAKS) / sizeof(BREAKS[0]); i++) {
        size_t l = strlen(BREAKS[i]);
        if (strncasecmp(name, BREAKS[i], l) == 0 && !isalnum((unsigned char)name[l])) {
            *newline = 1;
            break;
        }
    }
    return (size_t)(gt - p) + 1;
}

/* One pass over `in` into out, using at most `cap` bytes of it. Returns true if it all fit. */
static bool clean_into(const char *in, char *out, size_t cap)
{
    sink_t s = {.out = out, .cap = cap, .n = 0, .pending = 0, .truncated = 0};
    for (const char *p = in; *p && !s.truncated;) {
        unsigned char c = (unsigned char)*p;
        int nl;
        size_t tl = tag_at(p, &nl);
        if (tl) {
            if (nl) {
                s.pending = 2;
            } else if (s.pending < 1) {
                s.pending = 1;
            }
            p += tl;
            continue;
        }
        uint32_t cp;
        size_t el = entity_at(p, &cp);
        if (el) {
            put_cp(&s, cp);
            p += el;
            continue;
        }
        if (c == '\r') {
            p++;
            continue;
        }
        if (c >= 0x80) {                                  /* one whole UTF-8 character */
            int clen = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            if (c == 0xC2 && (unsigned char)p[1] == 0xA0) {   /* U+00A0 */
                put_cp(&s, 0xA0);
                p += 2;
                continue;
            }
            for (int i = 1; i < clen; i++) {
                if (p[i] == '\0') {
                    clen = i;
                    break;
                }
            }
            put_bytes(&s, p, (size_t)clen);
            p += clen;
            continue;
        }
        put_cp(&s, c);
        p++;
    }
    s.out[s.n] = '\0';
    return !s.truncated;
}

void gcal_clean_text(const char *in, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (in == NULL) {
        return;
    }
    if (clean_into(in, out, out_sz - 1)) {
        return;
    }
    /* Too long: go again leaving room for the ellipsis. */
    clean_into(in, out, out_sz > 4 ? out_sz - 4 : 0);
    size_t n = strlen(out);
    if (out_sz >= n + 4) {
        memcpy(out + n, ELLIPSIS, 3);
        out[n + 3] = '\0';
    }
}
