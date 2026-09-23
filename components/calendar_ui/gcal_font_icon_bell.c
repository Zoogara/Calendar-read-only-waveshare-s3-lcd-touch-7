/*******************************************************************************
 * Generated (via `npx lv_font_conv`), not hand-written - see the "Opts" line
 * below to regenerate. One glyph only: FontAwesome 5 Solid's "bell" icon
 * (U+F0F3), from the same FontAwesome5-Solid+Brands+Regular.woff the other
 * gcal_font_icon_*.c files use. Used only by ui_clock.c's ambient clock, as
 * a small "a reminder's pending" indicator in the bottom-left corner -
 * reference the glyph directly by its raw UTF-8 bytes ("\xEF\x83\xB3", the
 * 3-byte UTF-8 encoding of U+F0F3) rather than via an LV_SYMBOL_* define,
 * since this codepoint has no such define of its own.
 *
 * Size: 16 px (deliberately much smaller than gcal_font_icon_clock's 20px -
 * "same size as a full stop" against the ambient clock's 180px digits,
 * i.e. a small, easy-to-miss-unless-you're-looking-for-it dot, not a
 * regular top-bar-sized icon)
 * Bpp: 4
 * Opts: --no-compress --no-prefilter --bpp 4 --size 16 --font FontAwesome5-Solid+Brands+Regular.woff -r 0xf0f3 --format lvgl -o gcal_font_icon_bell.c --lv-font-name gcal_font_icon_bell --force-fast-kern-format
 ******************************************************************************/

#include "lvgl.h"

#ifndef GCAL_FONT_ICON_BELL
#define GCAL_FONT_ICON_BELL 1
#endif

#if GCAL_FONT_ICON_BELL

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F0F3 "" */
    0x0, 0x0, 0x0, 0xcc, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x2, 0xff, 0x30, 0x0, 0x0, 0x0, 0x1,
    0xbf, 0xff, 0xfc, 0x20, 0x0, 0x0, 0x1e, 0xff,
    0xff, 0xff, 0xe1, 0x0, 0x0, 0x9f, 0xff, 0xff,
    0xff, 0xf8, 0x0, 0x0, 0xef, 0xff, 0xff, 0xff,
    0xfd, 0x0, 0x0, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x0, 0x1, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0,
    0x3, 0xff, 0xff, 0xff, 0xff, 0xff, 0x30, 0x8,
    0xff, 0xff, 0xff, 0xff, 0xff, 0x80, 0x1e, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xe1, 0xcf, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xfc, 0xcf, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xfc, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0xe, 0xff, 0xe0, 0x0,
    0x0, 0x0, 0x0, 0x4, 0xee, 0x40, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 224, .box_w = 14, .box_h = 16, .ofs_x = 0, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61683, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t gcal_font_icon_bell = {
#else
lv_font_t gcal_font_icon_bell = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -6,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if GCAL_FONT_ICON_BELL*/

