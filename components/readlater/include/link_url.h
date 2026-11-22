#pragma once
/*
 * The one check every link in this project goes through: the RSS reader and the Wikipedia reader before they keep a
 * link, the read-later store before it saves one, and the config page before it prints one into an href. A link
 * comes from outside (a news feed, Wikipedia), so only a plain web address is accepted - never a javascript: or
 * data: URL, nothing with quotes or angle brackets that could break out of an HTML attribute, no spaces or control
 * characters, and no user-info section (http://trusted.example@evil.example/ is a classic disguise).
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_URL_MAX 200          /* buffer size, NUL included */

/* True for "http://" or "https://" (any case) followed by a host of letters, digits, '.' and '-' (an optional
 * ":port"), then optionally a path, query or fragment of printable ASCII without ", ', <, >, \ or `. The URL
 * must fit LINK_URL_MAX with its NUL. */
bool link_url_ok(const char *url);

#ifdef __cplusplus
}
#endif
