#include "url_decode.h"

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool url_percent_decode(const char *in, char *out, size_t out_sz)
{
    if (out_sz == 0) {
        return false;
    }
    size_t n = 0;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == '%') {
            int hi = p[1] ? hex_val(p[1]) : -1;
            int lo = (hi >= 0 && p[2]) ? hex_val(p[2]) : -1;
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
                out[0] = '\0';
                return false;
            }
            c = (char)((hi << 4) | lo);
            p += 2;
        }
        if (n + 1 >= out_sz) {          /* no room for this byte and the terminator */
            out[0] = '\0';
            return false;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return true;
}
