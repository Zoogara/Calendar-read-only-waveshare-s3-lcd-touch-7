#include "link_url.h"
#include <ctype.h>
#include <string.h>

bool link_url_ok(const char *url)
{
    if (url == NULL) {
        return false;
    }
    size_t n = strlen(url);
    if (n == 0 || n >= LINK_URL_MAX) {
        return false;
    }
    const char *p;
    if (strncasecmp(url, "https://", 8) == 0) {
        p = url + 8;
    } else if (strncasecmp(url, "http://", 7) == 0) {
        p = url + 7;
    } else {
        return false;
    }
    /* host */
    const char *host = p;
    while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':') {
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-')) {
            return false;
        }
        p++;
    }
    if (p == host) {
        return false;
    }
    if (*p == ':') {                        /* an optional port */
        p++;
        const char *port = p;
        while (isdigit((unsigned char)*p)) {
            p++;
        }
        if (p == port || p - port > 5) {
            return false;
        }
    }
    if (*p != '\0' && *p != '/' && *p != '?' && *p != '#') {
        return false;                       /* "@" and anything else in the authority */
    }
    for (; *p; p++) {                       /* path, query, fragment */
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\' || c == '`') {
            return false;
        }
    }
    return true;
}
