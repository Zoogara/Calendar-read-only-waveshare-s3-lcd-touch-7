/* A stand-in for the event store's text pool (event_store.c needs FreeRTOS and PSRAM): the same
 * add/get contract, kept in a plain array so tests can read back what the parsers stored. */
#include <stdio.h>
#include <string.h>

#include "event_store.h"

static char s_pool[16384];
static size_t s_len = 1;
static const uint16_t GEN = 7;

void fake_store_reset(void)
{
    s_len = 1;
}

uint32_t event_store_detail_add(const char *location, const char *description, uint16_t *gen_out)
{
    if (location == NULL) {
        location = "";
    }
    if (description == NULL) {
        description = "";
    }
    if (location[0] == '\0' && description[0] == '\0') {
        return 0;
    }
    size_t ll = strlen(location), dl = strlen(description);
    if (s_len + ll + dl + 2 > sizeof(s_pool)) {
        return 0;
    }
    uint32_t off = (uint32_t)s_len;
    memcpy(s_pool + off, location, ll + 1);
    memcpy(s_pool + off + ll + 1, description, dl + 1);
    s_len += ll + dl + 2;
    if (gen_out != NULL) {
        *gen_out = GEN;
    }
    return off;
}

bool event_store_detail_get(uint32_t offset, uint16_t gen, char *location, size_t loc_sz,
                            char *description, size_t desc_sz)
{
    location[0] = '\0';
    description[0] = '\0';
    if (offset == 0 || gen != GEN || offset >= s_len) {
        return false;
    }
    const char *l = s_pool + offset;
    snprintf(location, loc_sz, "%s", l);
    snprintf(description, desc_sz, "%s", l + strlen(l) + 1);
    return true;
}
