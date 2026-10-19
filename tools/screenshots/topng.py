#!/usr/bin/env python3
"""Convert the renderer's PPM files in a directory to PNG (needs Pillow)."""
import glob, sys
from PIL import Image
d = sys.argv[1] if len(sys.argv) > 1 else "out"
for p in sorted(glob.glob(d + "/*.ppm")):
    Image.open(p).save(p[:-4] + ".png", optimize=True)
    print("png", p[:-4] + ".png")
