#!/usr/bin/env python3
"""The rows' static covers: tools/tiles.py [DIR], DIR by default assets/ beside this folder.

Three 128 x 128 pictures, which Q2 Pod shows 64 px square beside a row: a folder's, Settings' and
the one a video gets when no picture of it can be made. Folder and no-picture are JPEGs, a white
glyph on a mid-grey tile; Settings' is a bare mid-grey gear on transparent (white would vanish on a light list; covers get
no Light Mode ink), a PNG (Q2 Pod reads a
cover by what it holds, not its name; a JPEG has no transparency). Flat: no gradient or gloss.
Each is drawn eight times the size and scaled down, for smooth edges, and JPEGs are
saved without chroma subsampling, which would blur a glyph's edge at this size. Needs Pillow.
"""
import math
import os
import sys

from PIL import Image, ImageDraw

PX, UP = 128, 8
TILE, GLYPH = "#7C7C80", "#FFFFFF"
GEAR = "#8E8E93"  # Settings' gear: a mid-grey, which reads on a light list and a dark one


def canvas():
    image = Image.new("RGB", (PX * UP, PX * UP), TILE)
    return image, ImageDraw.Draw(image)


def box(draw, x0, y0, x1, y1, radius, fill):
    """A rounded rectangle, in the tile's own 128 px units."""
    draw.rounded_rectangle([x0 * UP, y0 * UP, x1 * UP, y1 * UP], radius * UP, fill=fill)


def folder():
    image, draw = canvas()
    box(draw, 24, 32, 64, 56, 7, GLYPH)  # the tab
    box(draw, 24, 43, 104, 98, 8, GLYPH)  # the folder
    return image


def settings():
    """A mid-grey gear on transparent: PNG data, which Q2 Pod's cover reader takes by what the file
    holds, not its name (a JPEG has no transparency)."""
    mask = Image.new("L", (PX * UP, PX * UP), 0)
    draw = ImageDraw.Draw(mask)
    cx = cy = PX / 2 * UP
    for tooth in range(8):  # each a wedge from the hub's rim out to the tip
        a = tooth * math.pi / 4
        points = [(27, -0.25), (40, -0.16), (40, 0.16), (27, 0.25)]
        draw.polygon([(cx + r * UP * math.cos(a + da), cy + r * UP * math.sin(a + da))
                      for r, da in points], fill=255)
    for radius, fill in ((30, 255), (12, 0)):  # the wheel, and its hole
        draw.ellipse([cx - radius * UP, cy - radius * UP, cx + radius * UP, cy + radius * UP],
                     fill=fill)
    image = Image.new("RGBA", mask.size, GEAR)
    image.putalpha(mask)
    return image


def nopicture():
    image, draw = canvas()
    box(draw, 20, 30, 108, 98, 8, GLYPH)  # a frame of film
    box(draw, 40, 40, 88, 88, 4, TILE)  # its picture, empty
    for side in (25, 93):  # the holes down each edge
        for hole in range(4):
            box(draw, side, 37 + hole * 15, side + 10, 46 + hole * 15, 2, TILE)
    return image


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "assets")
    os.makedirs(out, exist_ok=True)
    for name, glyph in (("folder", folder), ("settings", settings), ("nopicture", nopicture)):
        tile = glyph().resize((PX, PX), Image.LANCZOS)
        path = os.path.join(out, "tile-%s.jpg" % name)
        if tile.mode == "RGBA":
            tile.save(path, format="PNG", optimize=True)
        else:
            tile.save(path, quality=92, subsampling=0, optimize=True)


if __name__ == "__main__":
    main()
