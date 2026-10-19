#pragma once
/* Percent-encoding for query-string values. Pure C, unit-tested on a computer (tests/host). */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes `in` to `out` with everything but letters, digits and "-_.~" as "%XX" (upper-case
 * hex), NUL-terminated. Returns false if out_sz was too small and the result was cut short. */
bool gcal_url_encode(const char *in, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
