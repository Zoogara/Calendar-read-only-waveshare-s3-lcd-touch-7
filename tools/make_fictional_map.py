#!/usr/bin/env python3
"""Draws the FICTIONAL radar base map that ships with the repository (components/calendar_ui/radar_base.rgb565):
made-up terrain, rivers, roads and towns in the style of a street map, with a ring marking "you are here" at the centre
(the radar frames are centred on your location). Deterministic - the same output every run - so it can be regenerated.

To use a real map of your own area instead, make a 380x330 picture of it (the same scale and centre as the radar frames:
RainViewer's 384 px zoom 6 tile, shown 4/3 scaled) and run  tools/make_radar_base.py yourmap.png
components/calendar_ui/radar_base.local.rgb565  - the build uses that file in preference when it exists, and git
ignores it, so a private map never gets committed.

  python3 tools/make_fictional_map.py        (needs Pillow; writes the .rgb565 and tools/screenshots/out/radar_base_fictional.png)
"""
import os
import random
import struct
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 380, 330
rnd = random.Random(20261122)

# ---- terrain: a blurred field of random blobs, banded into tints (flat plains, green farmland, hills) ----
field = Image.new("L", (W, H), 0)
d = ImageDraw.Draw(field)
for _ in range(70):
    x, y = rnd.randrange(-40, W + 40), rnd.randrange(-40, H + 40)
    r = rnd.randrange(20, 70)
    d.ellipse((x - r, y - r, x + r, y + r), fill=rnd.randrange(40, 120))
# a range of hills in the south-east
for _ in range(60):
    x, y = rnd.randrange(170, W + 30), rnd.randrange(150, H + 30)
    r = rnd.randrange(14, 46)
    d.ellipse((x - r, y - r, x + r, y + r), fill=rnd.randrange(120, 230))
field = field.filter(ImageFilter.GaussianBlur(14))

PLAIN, FARM, HILL, PEAK = (244, 239, 223), (230, 234, 205), (216, 224, 190), (201, 211, 176)
img = Image.new("RGB", (W, H), PLAIN)
px, fp = img.load(), field.load()
for y in range(H):
    for x in range(W):
        v = fp[x, y]
        px[x, y] = PLAIN if v < 52 else FARM if v < 95 else HILL if v < 150 else PEAK
img = img.filter(ImageFilter.GaussianBlur(1.4))
d = ImageDraw.Draw(img)

# forest patches
for _ in range(26):
    x, y = rnd.randrange(0, W), rnd.randrange(0, H)
    r = rnd.randrange(5, 17)
    d.ellipse((x - r, y - r * 0.7, x + r, y + r * 0.7), fill=(205, 222, 186))
img = img.filter(ImageFilter.GaussianBlur(0.8))
d = ImageDraw.Draw(img)

# a lake
d.ellipse((268, 40, 316, 66), fill=(176, 207, 224), outline=(150, 190, 214))

def smooth(points, steps=14):
    """Catmull-Rom through the points, for rivers and roads that bend."""
    out = []
    p = [points[0]] + points + [points[-1]]
    for i in range(1, len(p) - 2):
        for s in range(steps):
            t = s / steps
            def c(a, b, cc, dd):
                return 0.5 * ((2 * b) + (-a + cc) * t + (2 * a - 5 * b + 4 * cc - dd) * t * t + (-a + 3 * b - 3 * cc + dd) * t ** 3)
            out.append((c(p[i - 1][0], p[i][0], p[i + 1][0], p[i + 2][0]), c(p[i - 1][1], p[i][1], p[i + 1][1], p[i + 2][1])))
    out.append(points[-1])
    return out

# a river and a tributary
river = smooth([(-10, 128), (60, 140), (120, 122), (178, 150), (236, 146), (300, 168), (390, 150)])
d.line(river, fill=(160, 196, 218), width=4)
d.line(smooth([(150, -10), (158, 40), (146, 90), (178, 150)]), fill=(160, 196, 218), width=2)
d.line(smooth([(240, 340), (232, 290), (252, 236), (236, 146)]), fill=(160, 196, 218), width=2)

# a border (dashed, grey) running east-west north of the river
border = smooth([(-10, 96), (80, 104), (150, 88), (230, 104), (300, 90), (390, 100)], 10)
for i in range(0, len(border) - 1, 3):
    d.line(border[i:i + 2], fill=(150, 150, 150), width=1)

# roads: two highways and a few minor roads
def road(points, w, col, edge=(214, 150, 80)):
    pts = smooth(points)
    d.line(pts, fill=edge, width=w + 2)
    d.line(pts, fill=col, width=w)
road([(-10, 250), (70, 236), (130, 200), (190, 166), (260, 120), (330, 70), (390, 56)], 3, (252, 214, 142))
road([(40, -10), (80, 60), (96, 150), (130, 215), (150, 340)], 3, (252, 214, 142))
for pts in ([(190, 166), (196, 230), (186, 300)], [(190, 166), (250, 190), (330, 188), (390, 210)],
            [(130, 215), (60, 270), (20, 340)], [(260, 120), (270, 60), (250, 0)], [(96, 150), (30, 160), (-10, 170)]):
    road(pts, 1, (255, 255, 255), (222, 214, 196))

# ---- labels ----
font = ImageFont.load_default(size=13)
small = ImageFont.load_default(size=11)

def label(xy, text, f=font, dot=True, anchor="lm"):
    x, y = xy
    if dot:
        d.ellipse((x - 3, y - 3, x + 3, y + 3), fill=(255, 255, 255), outline=(40, 40, 40))
        x += 7
    d.text((x, y), text, font=f, fill=(25, 25, 25), anchor=anchor, stroke_width=2, stroke_fill=(255, 255, 255))

label((124, 82), "Fernhollow")
label((76, 124), "Quillmere")
label((50, 177), "Brackenby")
label((18, 208), "Oddwick")
label((106, 238), "Thistledown")
label((200, 218), "Marrowgate")
label((226, 102), "Pennyfold")
label((252, 86), "Larkspur Bend")
label((318, 168), "Wickerby")
label((222, 252), "Little Crake")
label((276, 286), "Hobnail Cross")
label((140, 296), "Ashgrove")
label((282, 128), "Dunmere", small)
label((60, 20), "Merrow Water", small, dot=False)
label((290, 236), "Highfell Range", small, dot=False)

# highway shields
def shield(xy, text):
    x, y = xy
    d.rounded_rectangle((x - 13, y - 8, x + 13, y + 8), radius=3, fill=(72, 168, 52), outline=(255, 255, 255))
    d.text((x, y), text, font=small, fill=(255, 255, 255), anchor="mm")
shield((138, 214), "A7")
shield((358, 60), "A12")
shield((48, 256), "B31")

# "you are here": ring and dot at the centre of the window, where the radar is centred
cx, cy = W // 2, H // 2
d.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), fill=(255, 255, 255), outline=(30, 30, 30), width=2)
d.ellipse((cx - 4, cy - 4, cx + 4, cy + 4), fill=(30, 30, 30))

os.makedirs("tools/screenshots/out", exist_ok=True)
img.save("tools/screenshots/out/radar_base_fictional.png")
data = bytearray()
for r, g, b in img.convert("RGB").getdata():
    data += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
open("components/calendar_ui/radar_base.rgb565", "wb").write(data)
print(len(data), "bytes -> components/calendar_ui/radar_base.rgb565;  preview tools/screenshots/out/radar_base_fictional.png")
