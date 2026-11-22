#!/usr/bin/env python3
"""Makes the radar test fixtures from real RainViewer frames (384x384 RGBA PNGs):

  radarN.png      a copy of the frame as it came from Home Assistant (the streaming PNG decoder tests)
  radarN.rgba.z   the frame as raw RGBA bytes, zlib-compressed (the tests read it with zlib's uncompress())
  goldenN.rgba.z  what Pillow makes of it for the window: resize to 512x512 (bilinear), crop (66, 91, 446, 421),
                  as raw straight-alpha RGBA, zlib-compressed - an independent implementation to compare
                  radar_expand() against

  python3 make_radar_fixtures.py frame1.png frame2.png ...        (needs Pillow)
"""
import sys
import zlib
from PIL import Image

import shutil
for n, path in enumerate(sys.argv[1:], 1):
    shutil.copy(path, f"radar{n}.png")                    # the PNG itself, for the streaming decoder tests
    im = Image.open(path).convert("RGBA")
    assert im.size == (384, 384), im.size
    open(f"radar{n}.rgba.z", "wb").write(zlib.compress(im.tobytes(), 9))
    gold = im.resize((512, 512), Image.BILINEAR).crop((66, 91, 66 + 380, 91 + 330))
    open(f"golden{n}.rgba.z", "wb").write(zlib.compress(gold.tobytes(), 9))
    print(n, path)
