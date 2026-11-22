#!/usr/bin/env python3
"""Makes a radar base map, components/calendar_ui/radar_base_zN.rgb565, from a picture of it: raw RGB565, little endian
(LVGL's native colour with LV_COLOR_16_SWAP off), 380x330, 250,800 bytes, embedded in flash by the build and drawn
straight from there (no RAM). The picture is the map the radar frames are drawn over - the same scale and centre as
RainViewer's 384 px frames at that zoom, shown scaled 4/3. The radar page has three zooms, each with its own map:

    zoom 5 = 1/2x (four times the area)    zoom 6 = 1x (the normal view)    zoom 7 = 2x

  python3 tools/make_radar_base.py picture.png --zoom 6 [--local]          (needs Pillow)

Without --local the output is components/calendar_ui/radar_base_zN.rgb565 - the fictional map the repository ships (made
by tools/make_fictional_map.py). To use a real map of your own area, add --local: the output is then
components/calendar_ui/radar_base_zN.local.rgb565, which the build prefers when it exists (re-run cmake after adding
one) and git ignores. Each zoom is chosen separately, so any you don't make a local map for keeps the fictional one.
"""
import argparse
import struct
from PIL import Image

ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("picture")
ap.add_argument("--zoom", type=int, choices=(5, 6, 7), default=6)
ap.add_argument("--local", action="store_true", help="write the git-ignored radar_base_zN.local.rgb565")
ap.add_argument("--out", help="output file (overrides --zoom/--local naming)")
a = ap.parse_args()

im = Image.open(a.picture).convert("RGB")
assert im.size == (380, 330), f"expected 380x330, got {im.size}"
out = bytearray()
for r, g, b in im.getdata():
    out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
dest = a.out or f"components/calendar_ui/radar_base_z{a.zoom}{'.local' if a.local else ''}.rgb565"
open(dest, "wb").write(out)
print(len(out), "bytes ->", dest)
