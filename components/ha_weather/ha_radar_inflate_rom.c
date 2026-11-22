/* radar_zlib_inflate() for the display, on the miniz that is in the ESP32-S3's ROM (so no flash for the code): the core
 * tinfl_decompress() coroutine, driven with a 32 KB wrapping window and flushed through put() - what miniz's own
 * tinfl_decompress_mem_to_callback() does, but with the window and the decompressor state (about 8 KB) put in PSRAM by
 * us rather than allocated by ROM code. */
#include "ha_radar_png.h"
#include "esp_heap_caps.h"
#include "miniz.h"

int radar_zlib_inflate(const uint8_t *in, size_t in_len, radar_put_fn put, void *user)
{
    tinfl_decompressor *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *dict = heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int ok = 0;
    if (d != NULL && dict != NULL) {
        tinfl_init(d);
        size_t in_ofs = 0, dict_ofs = 0;
        for (;;) {
            size_t in_sz = in_len - in_ofs, out_sz = TINFL_LZ_DICT_SIZE - dict_ofs;
            tinfl_status st = tinfl_decompress(d, in + in_ofs, &in_sz, dict, dict + dict_ofs, &out_sz,
                                               TINFL_FLAG_PARSE_ZLIB_HEADER);
            in_ofs += in_sz;
            if (out_sz > 0 && !put(dict + dict_ofs, (int)out_sz, user)) {
                break;
            }
            if (st != TINFL_STATUS_HAS_MORE_OUTPUT) {
                ok = st == TINFL_STATUS_DONE;
                break;
            }
            dict_ofs = (dict_ofs + out_sz) & (TINFL_LZ_DICT_SIZE - 1);
        }
    }
    heap_caps_free(d);
    heap_caps_free(dict);
    return ok;
}
