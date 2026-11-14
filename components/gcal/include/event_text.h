#pragma once
/* Turns the text of an event's location or description - which Google and ICS feeds may carry as HTML,
 * with entities, odd whitespace and arbitrary length - into plain text for the details box. Pure C,
 * unit-tested on a computer (tests/host). */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes a cleaned copy of `in` to `out` (NUL-terminated, at most out_sz - 1 bytes):
 *  - tags are dropped; <br>, <p>, <div>, <li>, <tr> and headings become a line break, other tags a space;
 *  - &amp; &lt; &gt; &quot; &apos; &nbsp; and numeric entities are decoded (typographic quotes and
 *    dashes become plain ASCII ones, which the display's fonts have);
 *  - runs of spaces and tabs collapse to one space, runs of line breaks to one, and leading and
 *    trailing whitespace is trimmed;
 *  - if it doesn't fit it is cut at a whole UTF-8 character and ends in an ellipsis (U+2026).
 * NULL `in` gives "". */
void gcal_clean_text(const char *in, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
