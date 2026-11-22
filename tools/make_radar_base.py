#!/usr/bin/env python3
"""Makes components/calendar_ui/radar_base.rgb565 from the radar base map picture: raw RGB565, little endian
(LVGL's native colour with LV_COLOR_16_SWAP off), 380x330, 250,800 bytes, embedded in flash by the build and
drawn straight from there (no RAM). The map is the one the radar frames are drawn over - the same scale and
centre as RainViewer's 384 px, zoom 6 frames shown scaled 4/3.

  python3 tools/make_radar_base.py picture.png [output.rgb565]      (needs Pillow)

The default output is components/calendar_ui/radar_base.rgb565 - the fictional map the repository ships (made by
tools/make_fictional_map.py). To use a real map of your own area, write it to components/calendar_ui/radar_base.local.rgb565
instead: the build prefers that file when it exists (re-run cmake after adding it) and git ignores it.
"""
import struct
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
assert im.size == (380, 330), f"expected 380x330, got {im.size}"
out = bytearray()
for r, g, b in im.getdata():
    out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
dest = sys.argv[2] if len(sys.argv) > 2 else "components/calendar_ui/radar_base.rgb565"
open(dest, "wb").write(out)
print(len(out), "bytes ->", dest)
