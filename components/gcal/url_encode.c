#include "url_encode.h"

#include <ctype.h>
#include <stdio.h>

bool gcal_url_encode(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    for (; *p && o + 4 < out_sz; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            o += snprintf(out + o, out_sz - o, "%%%02X", *p);
        }
    }
    out[o] = '\0';
    return *p == '\0';
}
