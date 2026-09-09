/*******************************************************************************
 * Generated (via `npx lv_font_conv`), not hand-written - see the "Opts" line
 * below to regenerate. One glyph only: FontAwesome 5 Solid's "clock" icon
 * (U+F017), from the same FontAwesome5-Solid+Brands+Regular.woff already
 * used to build gcal_font_20.c's LV_SYMBOL_* set - but "clock" isn't one of
 * LVGL's own built-in LV_SYMBOL_* glyphs (see lv_symbol_def.h - there's no
 * LV_SYMBOL_CLOCK), so it was never pulled into gcal_font_20.c and needs
 * its own tiny standalone font instead. Used only for the ambient-clock
 * toggle icon in calendar_ui.c's top bar (see build_top_bar()) - reference
 * the glyph directly by its raw UTF-8 bytes ("\xEF\x80\x97", the 3-byte
 * UTF-8 encoding of U+F017) rather than via an LV_SYMBOL_* define, since
 * this codepoint has no such define of its own.
 *
 * Size: 20 px (matches gcal_font_20, so it drops into the same top-bar
 * icon row at the same visual size as the neighbouring settings gear)
 * Bpp: 4
 * Opts: --no-compress --no-prefilter --bpp 4 --size 20 --font FontAwesome5-Solid+Brands+Regular.woff -r 0xf017 --format lvgl -o gcal_font_icon_clock.c --lv-font-name gcal_font_icon_clock --force-fast-kern-format
 ******************************************************************************/

#include "lvgl.h"

#ifndef GCAL_FONT_ICON_CLOCK
#define GCAL_FONT_ICON_CLOCK 1
#endif

#if GCAL_FONT_ICON_CLOCK

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F017 "" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x1, 0x7c, 0xef, 0xfd,
    0x94, 0x0, 0x0, 0x0, 0x0, 0x0, 0x7f, 0xff,
    0xff, 0xff, 0xff, 0xc2, 0x0, 0x0, 0x0, 0xb,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0x40, 0x0,
    0x0, 0xaf, 0xff, 0xff, 0xdb, 0xbf, 0xff, 0xff,
    0xf2, 0x0, 0x6, 0xff, 0xff, 0xff, 0x60, 0xe,
    0xff, 0xff, 0xfd, 0x0, 0xe, 0xff, 0xff, 0xff,
    0x60, 0xe, 0xff, 0xff, 0xff, 0x60, 0x6f, 0xff,
    0xff, 0xff, 0x60, 0xe, 0xff, 0xff, 0xff, 0xd0,
    0xaf, 0xff, 0xff, 0xff, 0x60, 0xe, 0xff, 0xff,
    0xff, 0xf1, 0xcf, 0xff, 0xff, 0xff, 0x60, 0xe,
    0xff, 0xff, 0xff, 0xf4, 0xef, 0xff, 0xff, 0xff,
    0x60, 0xc, 0xff, 0xff, 0xff, 0xf5, 0xdf, 0xff,
    0xff, 0xff, 0x60, 0x0, 0x9f, 0xff, 0xff, 0xf4,
    0xbf, 0xff, 0xff, 0xff, 0xd3, 0x0, 0x6, 0xff,
    0xff, 0xf2, 0x7f, 0xff, 0xff, 0xff, 0xff, 0x60,
    0xa, 0xff, 0xff, 0xe0, 0x2f, 0xff, 0xff, 0xff,
    0xff, 0xfa, 0x8f, 0xff, 0xff, 0x90, 0x9, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x10,
    0x1, 0xdf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xf5, 0x0, 0x0, 0x2e, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x80, 0x0, 0x0, 0x1, 0xcf, 0xff,
    0xff, 0xff, 0xff, 0xf5, 0x0, 0x0, 0x0, 0x0,
    0x5, 0xcf, 0xff, 0xff, 0xe9, 0x10, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x1, 0x34, 0x42, 0x0, 0x0,
    0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 320, .box_w = 20, .box_h = 21, .ofs_x = 0, .ofs_y = -3}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61463, .range_length = 1, .glyph_id_start = 1,
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
const lv_font_t gcal_font_icon_clock = {
#else
lv_font_t gcal_font_icon_clock = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 21,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -7,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if GCAL_FONT_ICON_CLOCK*/

