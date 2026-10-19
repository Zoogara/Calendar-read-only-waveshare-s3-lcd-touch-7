#pragma once
/* Percent-decoding of a URL query value. Pure C, unit-tested on a computer (tests/host). */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes "%XX" (either case of hex digits) in `in` into `out`, NUL-terminated. '+' is
 * left as it is. Returns false - with `out` holding an empty string - if a '%' isn't
 * followed by two hex digits, if the result would contain a NUL (%00), or if the decoded
 * text plus its terminator doesn't fit in out_sz. */
bool url_percent_decode(const char *in, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
