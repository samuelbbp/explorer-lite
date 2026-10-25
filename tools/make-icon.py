#!/usr/bin/env python3
"""res/explorer-lite.ico: a folder in the yellow of Windows' own folders, with a pulse line (the task manager inside),
on a dark rounded square. Drawn at 4x and scaled down per size. Needs Pillow.
    python3 tools/make-icon.py res/explorer-lite.ico [preview.png]"""
import sys
from PIL import Image, ImageDraw

S = 4  # supersampling
BG = (27, 31, 39, 255)
BACK = (232, 163, 23, 255)    # the folder's back and tab, darker (Windows 11's folder)
FRONT = (255, 200, 61, 255)   # its front
FRONT_TOP = (255, 215, 100, 255)
LINE = (27, 31, 39, 255)


def draw(size=256):
    im = Image.new("RGBA", (256 * S, 256 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    s = lambda *v: [x * S for x in v]
    d.rounded_rectangle(s(8, 8, 248, 248), radius=44 * S, fill=BG)
    d.rounded_rectangle(s(44, 68, 116, 104), radius=10 * S, fill=BACK)   # the tab
    d.rounded_rectangle(s(44, 84, 212, 196), radius=12 * S, fill=BACK)   # the back
    d.rounded_rectangle(s(44, 92, 212, 196), radius=12 * S, fill=FRONT)  # the front
    d.rectangle(s(56, 92, 200, 96), fill=FRONT_TOP)                       # its lit top edge
    pts = [(60, 142), (100, 142), (118, 112), (138, 172), (156, 142), (196, 142)]
    d.line([(x * S, y * S) for x, y in pts], fill=LINE, width=12 * S, joint="curve")
    for x, y in (pts[0], pts[-1]):
        d.ellipse(s(x - 6, y - 6, x + 6, y + 6), fill=LINE)
    return im.resize((size, size), Image.LANCZOS)


big = draw(256)
sizes = [16, 20, 24, 32, 40, 48, 64, 256]
big.save(sys.argv[1], sizes=[(n, n) for n in sizes], append_images=[draw(n) for n in sizes if n != 256])
if len(sys.argv) > 2:
    big.save(sys.argv[2])
