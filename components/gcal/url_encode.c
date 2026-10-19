#include "url_encode.h"

#include <ctype.h>

bool gcal_url_encode(const char *in, char *out, size_t out_sz)
{
    static const char HEX[] = "0123456789ABCDEF";
    if (out_sz == 0) {
        return false;
    }
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    for (; *p; p++) {
        bool plain = isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        size_t need = plain ? 1 : 3;
        if (o + need >= out_sz) {          /* keep room for the NUL */
            break;
        }
        if (plain) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = HEX[*p >> 4];
            out[o++] = HEX[*p & 15];
        }
    }
    out[o] = '\0';
    return *p == '\0';
}
