#!/usr/bin/env python3
"""Generate every graphic and audio asset of Cockroaches Cathisteria.

All art is procedural pixel art drawn with a handful of primitives, so the
repository contains no binary image sources and every asset is reproducible:

  src/assets.h          textures, sprites, palettes, props, font, sine table,
                        skyline -- pointer-free C tables (see docs/design.md)
  audio/samples/*.u8    four single-cycle PCM waveforms
  audio/cc_audio.json   the PRG32 AUDIO block description
  assets/preview/*.png  enlarged contact sheets for reviewing the art

Colours are chosen on the PRG32 6x6x6 system cube: a colour is written as
C(r, g, b) with components 0..5 and stored as the cube index r*36 + g*6 + b.
The cartridge builds its RGB565 palette from the same cube (render.c), so the
ESP32-C6 (which quantises to the cube) and QEMU show identical colours.

Textures are 16x16 and sprites up to 32x32, both 4 bits per pixel with a
16-entry palette of cube indices. Palette slot 0 of a sprite is transparent.
"""

from __future__ import annotations

import json
import math
import random
import re
import sys
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PRG32 = ROOT.parent / "PRG32"
OUT_H = ROOT / "src" / "assets.h"
PREVIEW = ROOT / "assets" / "preview"
AUDIO_DIR = ROOT / "audio"


def C(r: int, g: int, b: int) -> int:
    """Cube index of the colour with 0..5 components."""
    return r * 36 + g * 6 + b


def cube_rgb(index: int) -> tuple[int, int, int]:
    r, g, b = index // 36, (index // 6) % 6, index % 6
    return (r * 51, g * 51, b * 51)


# --------------------------------------------------------------------------
# A tiny indexed canvas with the primitives used by every asset
# --------------------------------------------------------------------------

class Canvas:
    def __init__(self, w: int, h: int, fill: int = 0):
        self.w, self.h = w, h
        self.px = [[fill] * w for _ in range(h)]

    def set(self, x: float, y: float, s: int) -> None:
        x, y = int(round(x)), int(round(y))
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = s

    def get(self, x: int, y: int) -> int:
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.px[y][x]
        return 0

    def rect(self, x0: int, y0: int, x1: int, y1: int, s: int) -> None:
        """Filled rectangle, inclusive corners."""
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, s)

    def ellipse(self, cx: float, cy: float, rx: float, ry: float, s: int) -> None:
        for y in range(self.h):
            for x in range(self.w):
                dx, dy = (x - cx) / max(rx, 0.01), (y - cy) / max(ry, 0.01)
                if dx * dx + dy * dy <= 1.0:
                    self.px[y][x] = s

    def line(self, x0: float, y0: float, x1: float, y1: float, s: int) -> None:
        n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(n + 1):
            t = i / n
            self.set(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, s)

    def poly(self, pts: list[tuple[float, float]], s: int) -> None:
        """Filled polygon (even-odd scanline)."""
        for y in range(self.h):
            yc = y + 0.5
            xs = []
            for i in range(len(pts)):
                (ax, ay), (bx, by) = pts[i], pts[(i + 1) % len(pts)]
                if (ay <= yc < by) or (by <= yc < ay):
                    xs.append(ax + (yc - ay) * (bx - ax) / (by - ay))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(math.ceil(xs[i] - 0.5)), int(math.floor(xs[i + 1] - 0.5)) + 1):
                    self.set(x, y, s)

    def replace(self, old: int, new: int) -> None:
        for row in self.px:
            for i, v in enumerate(row):
                if v == old:
                    row[i] = new

    def outline(self, s: int) -> None:
        """Paint slot s around every opaque pixel (4-neighbourhood)."""
        add = []
        for y in range(self.h):
            for x in range(self.w):
                if self.px[y][x] == 0 and any(
                        self.get(x + dx, y + dy) not in (0, s)
                        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                    add.append((x, y))
        for x, y in add:
            self.px[y][x] = s

    def flip(self) -> "Canvas":
        c = Canvas(self.w, self.h)
        c.px = [list(reversed(r)) for r in self.px]
        return c

    def mask_ellipse(self, cx, cy, rx, ry, inside: int, outside: int | None = None):
        """Recolour opaque pixels inside (and optionally outside) an ellipse."""
        for y in range(self.h):
            for x in range(self.w):
                if self.px[y][x] == 0:
                    continue
                dx, dy = (x - cx) / rx, (y - cy) / ry
                if dx * dx + dy * dy <= 1.0:
                    self.px[y][x] = inside
                elif outside is not None:
                    self.px[y][x] = outside


# --------------------------------------------------------------------------
# Palettes (16 cube indices each)
# --------------------------------------------------------------------------

PALETTES: dict[str, list[int]] = {}


def palette(name: str, *colors: int) -> None:
    cols = list(colors) + [0] * (16 - len(colors))
    PALETTES[name] = cols[:16]


# Texture palettes: slot 0 is a real colour.
palette("grass", C(0, 2, 0), C(0, 3, 0), C(1, 3, 0), C(1, 4, 1), C(0, 1, 0), C(5, 2, 4), C(5, 5, 1), C(2, 4, 1))
palette("plaster", C(4, 4, 3), C(4, 4, 4), C(3, 3, 3), C(5, 5, 4))
palette("concrete", C(2, 2, 2), C(2, 2, 3), C(1, 1, 2), C(3, 3, 3))
palette("sootfloor", C(1, 1, 1), C(1, 0, 0), C(0, 0, 0), C(2, 1, 1))
palette("hedge", C(0, 2, 0), C(0, 3, 1), C(1, 4, 1), C(0, 1, 0), C(2, 4, 1), C(5, 3, 4))
palette("growth", C(2, 0, 2), C(3, 0, 3), C(1, 3, 1), C(2, 5, 1), C(1, 0, 1), C(4, 1, 4))
palette("goo", C(1, 2, 0), C(1, 3, 1), C(0, 1, 0), C(2, 4, 1), C(1, 0, 1), C(2, 1, 2))
palette("fence", C(4, 3, 2), C(3, 2, 1), C(2, 1, 1), C(5, 4, 3))
palette("attic", C(3, 2, 1), C(2, 1, 0), C(1, 1, 0), C(4, 3, 2))
palette("woodfloor", C(3, 1, 0), C(2, 1, 0), C(1, 0, 0), C(4, 2, 1))
palette("atticfloor", C(2, 1, 1), C(1, 1, 0), C(1, 0, 0), C(3, 2, 1))
palette("siding", C(3, 4, 4), C(2, 3, 3), C(1, 2, 2), C(4, 5, 5))
palette("house", C(3, 4, 4), C(2, 3, 3), C(1, 2, 2), C(4, 5, 5), C(5, 5, 5), C(0, 1, 2), C(1, 2, 4), C(4, 0, 0))
palette("dormer", C(2, 2, 3), C(1, 1, 2), C(1, 1, 1), C(3, 3, 3), C(4, 4, 4), C(0, 0, 1), C(4, 4, 1), C(3, 0, 0))
palette("brick", C(3, 1, 0), C(4, 1, 1), C(2, 0, 0), C(3, 3, 2))
palette("soot", C(1, 1, 1), C(2, 1, 1), C(0, 0, 0), C(1, 0, 0))
palette("parapet", C(3, 3, 3), C(3, 2, 2), C(2, 2, 2), C(2, 1, 1))
palette("stone", C(2, 2, 2), C(3, 3, 2), C(1, 1, 1), C(2, 2, 1))
palette("patio", C(3, 3, 2), C(4, 3, 3), C(2, 2, 1), C(3, 2, 2))
palette("checker", C(5, 5, 5), C(1, 1, 1), C(4, 4, 4), C(0, 0, 0))
palette("ktile", C(5, 5, 5), C(3, 4, 5), C(4, 4, 4), C(1, 3, 5))
palette("bathtile", C(2, 4, 4), C(1, 3, 3), C(4, 5, 5), C(0, 2, 2))
palette("cabinet", C(4, 2, 1), C(3, 1, 0), C(2, 1, 0), C(4, 4, 4), C(5, 5, 3), C(3, 3, 3))
palette("fridge", C(5, 5, 5), C(4, 4, 4), C(3, 3, 3), C(5, 0, 0), C(0, 3, 5), C(5, 4, 0))
palette("shingles", C(1, 1, 2), C(2, 2, 2), C(0, 0, 1), C(2, 1, 1))
palette("beams", C(2, 1, 1), C(1, 1, 0), C(3, 2, 1), C(1, 0, 0))
palette("carpet", C(3, 0, 1), C(4, 1, 1), C(2, 0, 1), C(5, 4, 2))
palette("rose", C(5, 4, 4), C(5, 3, 3), C(4, 1, 2), C(2, 3, 1))
palette("stripes", C(1, 3, 2), C(1, 2, 1), C(4, 4, 2), C(2, 4, 3))
palette("painting", C(1, 3, 2), C(1, 2, 1), C(5, 4, 1), C(3, 2, 0), C(5, 3, 0), C(0, 0, 0), C(1, 4, 2), C(5, 5, 4))
palette("books", C(1, 0, 0), C(4, 0, 0), C(0, 2, 1), C(0, 1, 3), C(4, 3, 1), C(3, 1, 0), C(4, 4, 3), C(2, 1, 3))
palette("wainscot", C(4, 4, 3), C(3, 3, 2), C(3, 1, 0), C(2, 1, 0), C(4, 2, 1))
palette("pipes", C(1, 1, 1), C(0, 0, 0), C(4, 2, 1), C(5, 3, 2), C(2, 1, 0), C(3, 3, 3), C(4, 4, 4), C(2, 2, 2))
palette("swirl", C(1, 0, 2), C(3, 0, 4), C(5, 1, 5), C(1, 5, 3), C(4, 5, 4), C(0, 0, 1))
palette("door", C(3, 1, 0), C(2, 1, 0), C(4, 2, 1), C(5, 4, 1), C(1, 0, 0))
palette("exitlock", C(3, 1, 0), C(2, 1, 0), C(1, 4, 1), C(2, 5, 1), C(3, 3, 3), C(1, 1, 1))
palette("exitopen", C(5, 5, 3), C(5, 5, 4), C(5, 4, 2), C(5, 5, 5), C(3, 1, 0), C(2, 1, 0))

# Sprite palettes: slot 0 is transparent.
# roach slots: 1 outline 2-5 body ramp 6 legs 7 antennae 8 eyes 9 mouth
#              10-11 wings 12 belly 13 accent 14 accent2 15 glint
ROACH_BASE = [0, C(0, 0, 0), C(1, 0, 0), C(2, 1, 0), C(3, 1, 0), C(4, 3, 2), C(1, 0, 0),
              C(2, 1, 0), C(5, 1, 0), C(4, 0, 0), C(2, 1, 0), C(3, 2, 1), C(4, 3, 1),
              C(2, 1, 0), C(3, 1, 0), C(5, 5, 5)]


def roach_palette(name: str, **slots: int) -> None:
    cols = list(ROACH_BASE)
    names = {"outline": 1, "d": 2, "m": 3, "l": 4, "hi": 5, "legs": 6, "ant": 7,
             "eye": 8, "mouth": 9, "w1": 10, "w2": 11, "belly": 12, "acc": 13, "acc2": 14}
    for k, v in slots.items():
        cols[names[k]] = v
    PALETTES[name] = cols


roach_palette("roach")
roach_palette("runner", d=C(2, 1, 0), m=C(3, 2, 0), l=C(4, 3, 1), hi=C(5, 4, 2), w1=C(3, 2, 0), w2=C(4, 3, 1))
roach_palette("spitter", d=C(1, 1, 0), m=C(2, 2, 0), l=C(2, 3, 0), hi=C(3, 5, 1), acc=C(1, 5, 0), acc2=C(3, 5, 1), eye=C(2, 5, 0), mouth=C(1, 5, 0), w1=C(2, 2, 0), w2=C(2, 3, 0))
roach_palette("elite", d=C(1, 0, 0), m=C(3, 0, 0), l=C(4, 0, 0), hi=C(5, 2, 1), eye=C(5, 5, 0), w1=C(2, 0, 0), w2=C(3, 0, 0), acc=C(5, 5, 0), legs=C(0, 0, 0))
roach_palette("flyer", w1=C(3, 4, 5), w2=C(4, 5, 5), eye=C(5, 0, 5))
roach_palette("lawn", d=C(1, 1, 0), m=C(2, 2, 0), l=C(3, 3, 1), hi=C(4, 5, 2), w1=C(1, 2, 0), w2=C(2, 3, 0), eye=C(5, 5, 0))
roach_palette("crumb", d=C(2, 1, 0), m=C(4, 2, 0), l=C(5, 3, 1), hi=C(5, 5, 3), w1=C(4, 2, 0), w2=C(5, 3, 1), eye=C(5, 0, 0))
roach_palette("sootboss", d=C(0, 0, 0), m=C(1, 1, 1), l=C(2, 2, 2), hi=C(3, 3, 3), w1=C(1, 1, 1), w2=C(2, 2, 2), eye=C(5, 3, 0), legs=C(0, 0, 0), ant=C(1, 1, 1))
roach_palette("gutter", d=C(1, 1, 1), m=C(2, 2, 2), l=C(3, 3, 3), hi=C(4, 4, 5), w1=C(2, 3, 4), w2=C(3, 4, 5), eye=C(0, 5, 5))
roach_palette("butler", d=C(0, 0, 0), m=C(0, 0, 1), l=C(1, 1, 1), hi=C(2, 2, 3), belly=C(5, 5, 5), acc=C(5, 5, 5), w1=C(0, 0, 0), w2=C(1, 1, 1), eye=C(5, 5, 0))

BEETLE_BASE = [0, C(0, 0, 0), C(0, 1, 0), C(0, 2, 1), C(1, 3, 1), C(3, 5, 3), C(0, 1, 0),
               C(1, 1, 0), C(5, 2, 0), C(4, 0, 0), C(3, 4, 3), C(4, 5, 4), C(4, 4, 1),
               C(1, 2, 1), C(2, 3, 2), C(5, 5, 5)]


def beetle_palette(name: str, **slots: int) -> None:
    cols = list(BEETLE_BASE)
    names = {"d": 2, "m": 3, "l": 4, "hi": 5, "legs": 6, "horn": 7, "eye": 8, "mouth": 9,
             "w1": 10, "w2": 11, "belly": 12, "acc": 13, "acc2": 14}
    for k, v in slots.items():
        cols[names[k]] = v
    PALETTES[name] = cols


beetle_palette("beetle")
beetle_palette("armored", d=C(1, 1, 2), m=C(2, 2, 3), l=C(3, 3, 4), hi=C(5, 5, 5), acc=C(2, 2, 2), acc2=C(3, 3, 3), horn=C(3, 3, 3))
beetle_palette("grill", d=C(0, 0, 0), m=C(1, 1, 1), l=C(2, 2, 2), hi=C(5, 2, 0), acc=C(5, 1, 0), acc2=C(5, 3, 0), eye=C(5, 5, 0))
beetle_palette("dust", d=C(2, 2, 2), m=C(3, 3, 3), l=C(4, 4, 4), hi=C(5, 5, 5), acc=C(3, 3, 2), acc2=C(4, 4, 3), eye=C(5, 0, 0))
beetle_palette("bathtub", d=C(0, 2, 2), m=C(1, 3, 3), l=C(2, 4, 4), hi=C(4, 5, 5), acc=C(5, 5, 5), acc2=C(3, 5, 5))
beetle_palette("librarian", d=C(1, 0, 2), m=C(2, 0, 3), l=C(3, 1, 4), hi=C(4, 3, 5), acc=C(5, 4, 1), acc2=C(4, 3, 0), eye=C(5, 5, 5))
beetle_palette("couch", d=C(2, 0, 0), m=C(3, 0, 1), l=C(4, 1, 1), hi=C(5, 3, 3), acc=C(5, 4, 2), acc2=C(4, 3, 1), horn=C(4, 3, 1), eye=C(5, 5, 0))
beetle_palette("furnace", d=C(2, 0, 0), m=C(3, 1, 0), l=C(4, 2, 0), hi=C(5, 4, 1), acc=C(5, 2, 0), acc2=C(5, 5, 0), eye=C(5, 5, 5))
beetle_palette("general", d=C(1, 1, 0), m=C(2, 2, 0), l=C(3, 3, 1), hi=C(4, 4, 2), acc=C(5, 4, 0), acc2=C(4, 3, 0), eye=C(5, 0, 0), horn=C(4, 4, 3))

palette("queen", 0, C(0, 0, 0), C(1, 0, 1), C(2, 0, 2), C(3, 1, 3), C(4, 3, 4), C(1, 0, 1),
        C(2, 1, 2), C(5, 0, 0), C(5, 1, 3), C(2, 1, 3), C(3, 2, 4), C(4, 3, 3), C(5, 4, 0),
        C(5, 5, 2), C(5, 5, 5))
# cat slots: 1 outline 2 fur dark 3 fur 4 fur light 5 stripes 6 eye 7 nose/mouth
#            8 claws/teeth 9 inner ear 10 belly 11 stars
CAT_SLOTS = ["outline", "dark", "fur", "light", "stripe", "eye", "nose", "claw", "ear", "belly", "star"]


def cat_palette(name: str, dark, fur, light, stripe, eye=C(1, 5, 1), belly=C(5, 5, 4)):
    palette(name, 0, C(0, 0, 0), dark, fur, light, stripe, eye, C(4, 1, 2), C(5, 5, 5),
            C(5, 3, 3), belly, C(5, 5, 0))


cat_palette("cat_orange", C(3, 1, 0), C(5, 3, 0), C(5, 4, 2), C(4, 2, 0))
cat_palette("cat_black", C(0, 0, 0), C(1, 1, 1), C(2, 2, 2), C(0, 0, 0), eye=C(5, 5, 0), belly=C(2, 2, 2))
cat_palette("cat_gray", C(2, 2, 2), C(3, 3, 3), C(4, 4, 4), C(2, 2, 2), eye=C(1, 4, 5))
cat_palette("cat_calico", C(1, 1, 1), C(5, 5, 5), C(5, 4, 3), C(5, 2, 0))
cat_palette("cat_siamese", C(2, 1, 0), C(5, 4, 3), C(5, 5, 4), C(3, 2, 1), eye=C(0, 2, 5))
palette("fed", 0, C(0, 0, 0), C(1, 0, 2), C(2, 0, 3), C(3, 1, 4), C(5, 4, 3), C(4, 3, 2),
        C(0, 3, 3), C(5, 0, 1), C(5, 4, 0), C(5, 3, 3), C(5, 5, 5), C(1, 3, 5), C(3, 0, 0),
        C(4, 1, 5))
palette("items", 0, C(0, 0, 0), C(4, 2, 0), C(5, 3, 1), C(5, 0, 0), C(3, 0, 0), C(5, 5, 1),
        C(5, 5, 4), C(0, 2, 4), C(1, 4, 5), C(4, 4, 4), C(2, 2, 2), C(5, 2, 4), C(4, 0, 2),
        C(5, 4, 5), C(5, 5, 5))
palette("poison", 0, C(0, 1, 0), C(0, 3, 0), C(1, 5, 0), C(4, 5, 2), C(5, 5, 5), C(0, 2, 0),
        C(2, 4, 0))
palette("props", 0, C(0, 0, 0), C(0, 2, 0), C(1, 3, 0), C(2, 4, 1), C(5, 2, 4), C(4, 0, 2),
        C(2, 2, 2), C(3, 3, 3), C(4, 4, 4), C(1, 1, 1), C(5, 1, 0), C(3, 1, 0), C(4, 2, 1),
        C(2, 1, 0), C(5, 5, 3))
palette("props_wood", 0, C(0, 0, 0), C(2, 1, 0), C(3, 1, 0), C(4, 2, 1), C(5, 5, 5), C(4, 0, 0),
        C(2, 2, 2), C(3, 3, 3), C(4, 4, 4), C(1, 1, 1), C(5, 4, 2), C(3, 2, 1), C(4, 3, 2),
        C(1, 0, 0), C(5, 5, 3))
palette("props_trunk", 0, C(0, 0, 0), C(1, 0, 0), C(2, 1, 0), C(3, 1, 1), C(5, 5, 5), C(4, 0, 0),
        C(2, 2, 2), C(3, 3, 3), C(4, 4, 4), C(1, 1, 1), C(5, 4, 1), C(2, 1, 1), C(3, 2, 1),
        C(1, 0, 0), C(5, 5, 2))
palette("portal", 0, C(0, 0, 0), C(1, 0, 2), C(3, 0, 4), C(5, 1, 5), C(1, 5, 3), C(4, 5, 4),
        C(5, 5, 5), C(2, 0, 3), C(0, 3, 2), C(5, 3, 5), C(0, 5, 5))
palette("anchor", 0, C(0, 0, 0), C(1, 0, 2), C(2, 0, 4), C(4, 1, 5), C(5, 4, 5), C(0, 4, 3),
        C(3, 5, 4), C(5, 5, 5), C(1, 1, 1), C(2, 2, 2))
palette("debris", 0, C(0, 0, 0), C(2, 0, 0), C(3, 1, 0), C(4, 1, 1), C(3, 3, 2), C(1, 1, 1))
palette("fx", 0, C(5, 5, 5), C(5, 0, 0), C(5, 3, 0), C(5, 5, 0))

# --------------------------------------------------------------------------
# Textures: 16x16 patterns
# --------------------------------------------------------------------------

T = 16
PATTERNS: dict[str, Canvas] = {}


def pattern(name: str):
    def deco(fn):
        c = Canvas(T, T)
        rng = random.Random(zlib.crc32(name.encode()))
        fn(c, rng)
        PATTERNS[name] = c
        return fn
    return deco


def noise(c: Canvas, rng, slots: list[int], weights: list[int]) -> None:
    for y in range(T):
        for x in range(T):
            c.px[y][x] = rng.choices(slots, weights)[0]


@pattern("grass")
def _(c, rng):
    noise(c, rng, [0, 1, 2, 4], [5, 6, 3, 2])
    for _ in range(10):
        x, y = rng.randrange(T), rng.randrange(1, T)
        c.set(x, y, 3)
        c.set(x, y - 1, 2)
    for x, y in ((3, 4), (11, 12)):
        c.set(x, y, 5)
        c.set(x + 1, y, 6)


@pattern("plaster")
def _(c, rng):
    noise(c, rng, [0, 1, 2], [14, 3, 1])
    c.rect(0, 0, 15, 0, 2)                  # faint seams between ceiling boards
    c.rect(0, 0, 0, 15, 1)


@pattern("leaves")
def _(c, rng):
    noise(c, rng, [0, 1, 3], [5, 4, 2])
    for _ in range(14):
        x, y = rng.randrange(T), rng.randrange(T)
        c.ellipse(x, y, 1.3, 1.0, 1)
        c.set(x - 1, y - 1, 2)
    for _ in range(6):
        c.set(rng.randrange(T), rng.randrange(T), 4)
    c.set(5, 6, 5)
    c.set(12, 3, 5)


@pattern("organic")
def _(c, rng):
    noise(c, rng, [0, 1, 4], [6, 3, 2])
    for k in range(3):
        x, y = rng.randrange(T), 0
        while y < T:
            c.set(x, y, 2)
            c.set(x + 1, y, 3 if k == 1 else 2)
            x += rng.choice((-1, 0, 1))
            y += 1
    for _ in range(4):
        x, y = rng.randrange(2, 14), rng.randrange(2, 14)
        c.ellipse(x, y, 1.4, 1.4, 5)
        c.set(x - 0.4, y - 0.6, 3)


@pattern("picket")
def _(c, rng):
    for x in range(T):
        col = x % 4
        for y in range(T):
            c.px[y][x] = 2 if col == 3 else (3 if col == 0 else 0)
    for x in range(0, T, 4):
        for y in (4, 11):
            c.set(x + 1, y, 1)
    for y in (3, 12):
        for x in range(T):
            if x % 4 != 3:
                c.set(x, y, 1)


@pattern("siding")
def _(c, rng):
    for y in range(T):
        band = y % 4
        for x in range(T):
            c.px[y][x] = 2 if band == 3 else (3 if band == 0 else 0)
    for y in (1, 9):
        c.set(rng.randrange(T), y, 1)


@pattern("house")
def _(c, rng):
    PATTERNS_SIDING = PATTERNS["siding"]
    for y in range(T):
        c.px[y] = list(PATTERNS_SIDING.px[y])
    c.rect(3, 2, 12, 12, 4)          # white frame
    c.rect(4, 3, 11, 11, 5)          # glass
    c.rect(4, 3, 5, 11, 7)           # curtains
    c.rect(10, 3, 11, 11, 7)
    c.rect(4, 7, 11, 7, 4)           # muntins
    c.rect(7, 3, 8, 11, 4)
    c.set(6, 4, 6)
    c.set(9, 8, 6)
    c.rect(2, 13, 13, 13, 4)         # sill


@pattern("stone")
def _(c, rng):
    pts = [(rng.uniform(0, T), rng.uniform(0, T), rng.choice((0, 1, 3))) for _ in range(9)]
    for y in range(T):
        for x in range(T):
            ds = []
            for px, py, s in pts:
                for ox in (-T, 0, T):
                    for oy in (-T, 0, T):
                        ds.append(((x - px - ox) ** 2 + (y - py - oy) ** 2, s))
            ds.sort()
            c.px[y][x] = 2 if ds[1][0] - ds[0][0] < 5 else ds[0][1]


@pattern("checker")
def _(c, rng):
    for y in range(T):
        for x in range(T):
            c.px[y][x] = 0 if ((x // 8) + (y // 8)) & 1 == 0 else 1
    for i in range(0, T, 8):
        c.set(i + 1, i + 1, 2)
        c.set(i + 9 - 8, i + 9 - 8, 2)


@pattern("tile")
def _(c, rng):
    for y in range(T):
        for x in range(T):
            gx, gy = x % 8, y % 8
            c.px[y][x] = 2 if gx == 7 or gy == 7 else 0
    for oy in (0, 8):
        for ox in (0, 8):
            c.set(ox + 1, oy + 1, 1)
            c.set(ox + 2, oy + 1, 1)
            c.set(ox + 1, oy + 2, 1)
    c.rect(0, 11, 15, 12, 3)         # decorative band


@pattern("cabinet")
def _(c, rng):
    c.rect(0, 0, 15, 15, 0)
    c.rect(0, 0, 15, 2, 5)           # counter top
    c.rect(0, 3, 15, 3, 2)
    for ox in (0, 8):
        c.rect(ox + 1, 5, ox + 6, 14, 1)
        c.rect(ox + 2, 6, ox + 5, 13, 0)
        c.set(ox + 5 if ox == 0 else ox + 2, 9, 4)
    c.rect(0, 15, 15, 15, 2)
    c.set(3, 1, 3)
    c.set(11, 1, 3)


@pattern("fridge")
def _(c, rng):
    c.rect(0, 0, 15, 15, 0)
    c.rect(0, 0, 0, 15, 1)
    c.rect(15, 0, 15, 15, 2)
    c.rect(1, 5, 14, 5, 2)
    c.rect(12, 1, 13, 3, 2)
    c.rect(12, 7, 13, 12, 2)
    c.set(3, 2, 3)
    c.set(6, 8, 4)
    c.set(4, 11, 5)
    c.rect(7, 10, 9, 12, 1)          # a kid's drawing of a cat
    c.set(7, 9, 1)
    c.set(9, 9, 1)


@pattern("brick")
def _(c, rng):
    for y in range(T):
        row = y // 4
        for x in range(T):
            xs = (x + (4 if row & 1 else 0)) % 8
            if y % 4 == 3 or xs == 7:
                c.px[y][x] = 3
            else:
                c.px[y][x] = rng.choice((0, 0, 1)) if xs != 0 else 2
    for _ in range(4):
        c.set(rng.randrange(T), rng.randrange(T), 2)


@pattern("shingle")
def _(c, rng):
    for y in range(T):
        row = y // 4
        for x in range(T):
            xs = (x + (3 if row & 1 else 0)) % 6
            c.px[y][x] = 2 if y % 4 == 3 else (2 if xs == 5 else rng.choice((0, 0, 1, 3)))


@pattern("planks")
def _(c, rng):
    for x in range(T):
        col = x % 5
        for y in range(T):
            c.px[y][x] = 2 if col == 4 else rng.choice((0, 0, 0, 1)) if col else 3
    for x in range(0, T, 5):
        c.set(x + 2, rng.randrange(T), 2)
        c.set(x + 1, rng.randrange(T), 1)


@pattern("beams")
def _(c, rng):
    noise(c, rng, [0, 1], [5, 1])
    c.rect(0, 0, 15, 3, 2)
    c.rect(0, 4, 15, 4, 3)
    c.rect(0, 11, 15, 11, 3)


@pattern("carpet")
def _(c, rng):
    for y in range(T):
        for x in range(T):
            d = abs((x % 8) - 4) + abs((y % 8) - 4)
            c.px[y][x] = 3 if d == 3 else (1 if d < 2 else (2 if (x + y) % 5 == 0 else 0))


@pattern("floral")
def _(c, rng):
    c.rect(0, 0, 15, 15, 0)
    for oy, ox in ((2, 2), (10, 10), (2, 10), (10, 2)):
        if (ox + oy) % 16 == 4 or (ox, oy) in ((2, 2), (10, 10)):
            c.ellipse(ox + 1.5, oy + 1.5, 1.6, 1.6, 2)
            c.set(ox + 1.5, oy + 1.5, 1)
            c.set(ox + 3, oy + 3.5, 3)
            c.set(ox + 4, oy + 4, 3)
        else:
            c.set(ox + 1, oy + 1, 1)
            c.set(ox + 2, oy + 2, 3)


@pattern("stripes")
def _(c, rng):
    for x in range(T):
        s = [0, 0, 0, 3, 1, 1, 1, 2][x % 8]
        for y in range(T):
            c.px[y][x] = s


@pattern("painting")
def _(c, rng):
    for y in range(T):
        for x in range(T):
            c.px[y][x] = 0 if x % 4 else 1
    c.rect(2, 2, 13, 13, 2)          # gold frame
    c.rect(3, 3, 12, 12, 3)
    c.rect(4, 4, 11, 11, 6)          # green backdrop
    c.ellipse(7.5, 8.5, 3.2, 2.8, 4) # orange cat face
    c.poly([(4.5, 7), (5, 3.5), (7, 6)], 4)
    c.poly([(10.5, 7), (10, 3.5), (8, 6)], 4)
    c.set(6, 8, 5)
    c.set(9, 8, 5)
    c.set(7.5, 10, 5)
    c.rect(4, 14, 11, 14, 7)          # name plate


@pattern("books")
def _(c, rng):
    for shelf in (0, 8):
        c.rect(0, shelf + 7, 15, shelf + 7, 5)
        x = 0
        while x < T:
            w = rng.choice((1, 2, 2, 3))
            col = rng.choice((1, 2, 3, 4, 7))
            top = shelf + rng.choice((1, 2, 2, 3))
            c.rect(x, top, min(T - 1, x + w - 1), shelf + 6, col)
            if w > 1:
                c.set(x, top + 2, 6)
            c.rect(x, shelf, min(T - 1, x + w - 1), top - 1, 0)
            x += w


@pattern("wainscot")
def _(c, rng):
    c.rect(0, 0, 15, 7, 0)
    for x in range(0, T, 4):
        c.rect(x, 0, x, 7, 1)
    c.rect(0, 8, 15, 8, 4)
    c.rect(0, 9, 15, 15, 2)
    c.rect(2, 10, 13, 14, 3)
    c.rect(3, 11, 12, 13, 2)


@pattern("pipes")
def _(c, rng):
    noise(c, rng, [0, 1, 7], [6, 1, 2])
    for y0, a, b in ((2, 2, 3), (9, 5, 6)):
        c.rect(0, y0, 15, y0 + 3, a)
        c.rect(0, y0, 15, y0, b)
        c.rect(0, y0 + 3, 15, y0 + 3, 4 if a == 2 else 7)
        c.rect(6, y0 - 1, 7, y0 + 4, b)
    c.rect(11, 13, 13, 15, 2)
    c.set(12, 12, 3)


@pattern("swirl")
def _(c, rng):
    for y in range(T):
        for x in range(T):
            dx, dy = x - 7.5, y - 7.5
            a = math.atan2(dy, dx)
            r = math.hypot(dx, dy)
            v = (a / (2 * math.pi) * 3 + r / 4.0) % 1.0
            c.px[y][x] = [0, 1, 2, 3, 4, 3, 2, 1][int(v * 8) % 8]


@pattern("door")
def _(c, rng):
    c.rect(0, 0, 15, 15, 1)
    c.rect(2, 1, 13, 15, 0)
    c.rect(4, 3, 7, 7, 2)
    c.rect(8, 3, 11, 7, 2)
    c.rect(4, 9, 7, 14, 2)
    c.rect(8, 9, 11, 14, 2)
    c.set(12, 9, 3)
    c.set(12, 10, 4)


@pattern("doorlock")
def _(c, rng):
    for y in range(T):
        c.px[y] = list(PATTERNS["door"].px[y])
    c.line(1, 2, 14, 13, 4)
    c.line(1, 13, 14, 2, 4)
    c.line(1, 3, 14, 14, 5)
    for _ in range(18):
        x, y = rng.randrange(T), rng.randrange(T)
        c.set(x, y, 2)
        c.set(x, y + 1, 3)


@pattern("dooropen")
def _(c, rng):
    c.rect(0, 0, 15, 15, 4)
    c.rect(2, 1, 13, 15, 0)
    for y in range(1, 16):
        c.rect(3, y, 12, y, 1 if y < 6 else (0 if y < 12 else 2))
    c.rect(6, 4, 9, 13, 3)


TEXTURES = [
    ("GRASS", "grass", "grass"), ("PLASTER", "plaster", "plaster"),
    ("CONCRETE", "plaster", "concrete"), ("SOOTFLOOR", "plaster", "sootfloor"),
    ("HEDGE", "leaves", "hedge"), ("GROWTH", "organic", "growth"), ("GOO", "organic", "goo"),
    ("FENCE", "picket", "fence"), ("SIDING", "siding", "siding"), ("HOUSE", "house", "house"),
    ("DORMER", "house", "dormer"), ("PATIO", "stone", "patio"), ("STONE", "stone", "stone"),
    ("CHECKER", "checker", "checker"), ("KTILE", "tile", "ktile"),
    ("BATHTILE", "tile", "bathtile"), ("CABINET", "cabinet", "cabinet"),
    ("FRIDGE", "fridge", "fridge"), ("BRICK", "brick", "brick"), ("SOOT", "brick", "soot"),
    ("PARAPET", "brick", "parapet"), ("SHINGLES", "shingle", "shingles"),
    ("ATTIC", "planks", "attic"), ("WOODFLOOR", "planks", "woodfloor"),
    ("ATTICFLOOR", "planks", "atticfloor"), ("BEAMS", "beams", "beams"),
    ("CARPET", "carpet", "carpet"), ("ROSE", "floral", "rose"), ("STRIPES", "stripes", "stripes"),
    ("PAINTING", "painting", "painting"), ("BOOKS", "books", "books"),
    ("WAINSCOT", "wainscot", "wainscot"), ("PIPES", "pipes", "pipes"),
    ("SWIRL", "swirl", "swirl"), ("DOOR", "door", "door"),
    ("EXIT_LOCK", "doorlock", "exitlock"), ("EXIT_OPEN", "dooropen", "exitopen"),
]

# --------------------------------------------------------------------------
# Sprites
# --------------------------------------------------------------------------

SPRITES: list[tuple[str, Canvas, str]] = []


def sprite(name: str, canvas: Canvas, pal: str) -> None:
    SPRITES.append((name, canvas, pal))


def roach(frame: str) -> Canvas:
    c = Canvas(24, 24)
    legs = {
        "a": [((7, 9), (2, 7), (1, 10)), ((6, 12), (1, 13), (1, 16)), ((7, 15), (3, 18), (2, 21)),
              ((16, 9), (21, 8), (22, 11)), ((17, 12), (22, 12), (22, 15)), ((16, 15), (20, 19), (21, 22))],
        "b": [((7, 9), (2, 8), (2, 5)), ((6, 12), (1, 12), (0, 15)), ((7, 15), (3, 17), (3, 21)),
              ((16, 9), (21, 9), (21, 6)), ((17, 12), (22, 13), (23, 16)), ((16, 15), (20, 18), (20, 22))],
    }
    if frame in ("a", "b"):
        for (x0, y0), (x1, y1), (x2, y2) in legs[frame]:
            c.line(x0, y0, x1, y1, 6)
            c.line(x1, y1, x2, y2, 6)
        c.ellipse(11.5, 9, 6.5, 8, 3)           # wings / abdomen
        c.ellipse(11.5, 8, 5.5, 6.5, 10)
        c.ellipse(10, 6.5, 2.5, 4, 11)          # wing sheen
        c.line(11.5, 2, 11.5, 14, 2)            # wing seam
        c.ellipse(11.5, 15.5, 5.5, 3.2, 4)      # pronotum
        c.ellipse(11.5, 15, 3.5, 1.6, 5)
        c.ellipse(11.5, 19.5, 3, 2.2, 2)        # head
        c.set(10, 19, 8)
        c.set(13, 19, 8)
        c.set(10, 21, 9)
        c.set(13, 21, 9)
        c.line(10, 18, 5, 10, 7)                # antennae
        c.line(5, 10, 2, 1, 7)
        c.line(13, 18, 18, 10, 7)
        c.line(18, 10, 21, 1, 7)
        c.set(8, 11, 13)
        c.set(15, 11, 13)
    elif frame == "atk":
        for (x0, y0), (x1, y1) in (((7, 13), (1, 9)), ((16, 13), (22, 9)), ((7, 16), (2, 20)),
                                   ((16, 16), (21, 20)), ((8, 18), (4, 23)), ((15, 18), (19, 23))):
            c.line(x0, y0, x1, y1, 6)
        c.line(1, 9, 3, 5, 6)
        c.line(22, 9, 20, 5, 6)
        c.ellipse(11.5, 11, 6.5, 7, 3)
        c.ellipse(11.5, 10, 5, 5.5, 10)
        c.line(11.5, 5, 11.5, 15, 2)
        c.ellipse(11.5, 16, 6, 3.4, 4)
        c.ellipse(11.5, 15.5, 4, 1.8, 5)
        c.ellipse(11.5, 20, 4, 3, 2)
        c.ellipse(11.5, 21.3, 2.2, 1.4, 9)       # open mouth
        c.set(9, 19, 8)
        c.set(14, 19, 8)
        c.set(9, 18, 15)
        c.line(8, 22, 10, 21, 12)               # mandibles
        c.line(15, 22, 13, 21, 12)
        c.line(10, 18, 3, 3, 7)
        c.line(13, 18, 20, 3, 7)
        c.set(8, 12, 13)
        c.set(15, 12, 13)
    else:  # dead, on its back
        for x in (5, 9, 14, 18):
            c.line(x + 1, 12, x, 5, 6)
            c.line(x, 5, x + (1 if x < 12 else -1), 3, 6)
        c.ellipse(11.5, 15, 8, 5, 12)
        for x in (7, 10, 13, 16):
            c.line(x, 12, x, 18, 3)
        c.ellipse(11.5, 20, 3, 2, 2)
        c.line(8, 21, 1, 23, 7)
        c.line(15, 21, 22, 23, 7)
        c.set(10, 20, 1)
        c.set(13, 20, 1)
    c.outline(1)
    return c


def beetle(frame: str) -> Canvas:
    c = Canvas(24, 24)
    if frame in ("a", "b"):
        off = 1 if frame == "b" else 0
        for sx in (-1, 1):
            for i, y in enumerate((9, 13, 17)):
                x0 = 12 + sx * 8
                c.line(x0, y, x0 + sx * 3, y + 2 + (off if i != 1 else -off), 6)
                c.line(x0 + sx * 3, y + 2 + (off if i != 1 else -off), x0 + sx * 3, y + 5, 6)
        c.ellipse(11.5, 11, 9, 8.5, 3)          # dome shell
        c.ellipse(11.5, 10, 8, 7, 4)
        c.ellipse(8, 6.5, 3, 2.2, 5)            # sheen
        c.set(7, 6, 15)
        c.line(11.5, 3, 11.5, 18, 2)            # elytra split
        c.line(12.5, 3, 12.5, 18, 13)
        c.ellipse(11.5, 18, 5, 2.4, 2)          # pronotum
        c.ellipse(11.5, 20.5, 3, 2, 2)          # head
        c.set(9.5, 20, 8)
        c.set(13.5, 20, 8)
        c.line(11.5, 22, 11.5, 15, 7)           # horn
        c.line(12.5, 21, 12.5, 16, 7)
        c.set(11.5, 14, 14)
        for x, y in ((5, 12), (18, 12), (7, 15), (16, 15)):
            c.set(x, y, 14)
    elif frame == "open":
        for sx in (-1, 1):
            c.poly([(12 + sx * 1, 4), (12 + sx * 11, 1), (12 + sx * 11, 12), (12 + sx * 3, 14)], 3)
            c.line(12 + sx * 11, 1, 12 + sx * 11, 12, 5)
            c.poly([(12 + sx * 2, 8), (12 + sx * 9, 13), (12 + sx * 5, 21)], 10)
            c.line(12 + sx * 2, 8, 12 + sx * 7, 18, 11)
            for y in (15, 19):
                c.line(12 + sx * 6, y, 12 + sx * 10, y + 3, 6)
        c.ellipse(11.5, 12, 4.5, 7, 12)         # soft belly
        for y in (8, 11, 14):
            c.line(9, y, 14, y, 13)
        c.ellipse(11.5, 20.5, 3, 2.2, 2)
        c.ellipse(11.5, 22, 1.8, 1.2, 9)
        c.set(9.5, 20, 8)
        c.set(13.5, 20, 8)
        c.line(11.5, 18, 11.5, 13, 7)
    else:
        for x in (5, 9, 14, 18):
            c.line(x, 11, x - 1, 5, 6)
        c.ellipse(11.5, 14, 9, 6, 12)
        c.line(11.5, 9, 11.5, 19, 13)
        c.ellipse(11.5, 20, 3, 2, 2)
        c.set(10, 20, 1)
        c.set(13, 20, 1)
    c.outline(1)
    return c


def queen(frame: str) -> Canvas:
    c = Canvas(32, 32)
    c.ellipse(15.5, 8, 11, 8, 12)               # egg-laden abdomen
    for x, y in ((9, 6), (13, 4), (18, 5), (22, 8), (12, 10), (17, 11), (8, 10)):
        c.ellipse(x, y, 1.3, 1.1, 14)
        c.set(x - 0.4, y - 0.5, 15)
    for i in range(6, 28, 4):
        c.line(i, 13, i + 2, 3, 4)
    arms_up = frame == "atk"
    for sx in (-1, 1):
        c.line(16 + sx * 6, 17, 16 + sx * 13, 13 if arms_up else 21, 6)
        c.line(16 + sx * 13, 13 if arms_up else 21, 16 + sx * 14, 6 if arms_up else 27, 6)
        c.line(16 + sx * 5, 21, 16 + sx * 12, 26, 6)
    c.ellipse(15.5, 18, 7, 5, 3)                # thorax
    c.ellipse(15.5, 17, 5, 3, 4)
    c.ellipse(15.5, 25, 5, 4, 2)                # head
    c.set(13, 24, 8)
    c.set(18, 24, 8)
    c.set(13, 23, 15)
    if arms_up:
        c.ellipse(15.5, 27.5, 3, 2, 9)
        c.line(11, 29, 13, 27, 12)
        c.line(20, 29, 18, 27, 12)
    else:
        c.line(14, 28, 17, 28, 9)
    c.poly([(10, 21), (11, 16), (13, 19), (15.5, 14), (18, 19), (20, 16), (21, 21)], 13)  # crown
    c.line(10, 21, 21, 21, 14)
    c.set(15.5, 17, 8)
    c.line(13, 22, 5, 12, 7)
    c.line(18, 22, 26, 12, 7)
    c.outline(1)
    return c


def cat_leap() -> Canvas:
    """A cat seen from behind, leaping away from Fed towards the enemy."""
    c = Canvas(24, 24)
    c.poly([(7, 22), (9, 15), (11, 17)], 2)      # hind legs
    c.poly([(17, 22), (15, 15), (13, 17)], 2)
    c.ellipse(12, 14, 5, 6, 3)                   # body
    c.ellipse(12, 7, 5, 4.2, 3)                  # back of head
    c.poly([(7.5, 6), (8, 0.5), (11, 4)], 3)
    c.poly([(16.5, 6), (16, 0.5), (13, 4)], 3)
    c.set(8.5, 3, 9)
    c.set(15.5, 3, 9)
    for y in (10, 13, 16):
        c.line(9, y, 15, y, 5)
    c.line(12, 4, 12, 9, 5)
    c.line(16, 18, 21, 12, 3)                    # tail up
    c.line(21, 12, 22, 7, 4)
    c.line(17, 18, 22, 12, 5)
    c.rect(4, 8, 6, 9, 3)                        # front paws reaching
    c.rect(18, 8, 20, 9, 3)
    c.outline(1)
    return c


def cat_pounce() -> Canvas:
    c = Canvas(24, 24)
    c.ellipse(12, 18, 5, 5, 3)                   # body behind
    for sx in (-1, 1):
        c.poly([(12 + sx * 4, 12), (12 + sx * 11, 9), (12 + sx * 11, 13), (12 + sx * 5, 16)], 3)
        for k in range(3):
            c.set(12 + sx * 11.5, 9.5 + k * 1.5, 8)      # claws
    c.ellipse(12, 10, 6.5, 5.5, 3)               # head
    c.poly([(6, 8), (6.5, 1), (10, 5.5)], 3)
    c.poly([(18, 8), (17.5, 1), (14, 5.5)], 3)
    c.set(7, 4, 9)
    c.set(17, 4, 9)
    c.line(12, 5, 12, 7, 5)
    c.line(10, 5, 10.5, 7, 5)
    c.line(14, 5, 13.5, 7, 5)
    c.ellipse(9, 9, 1.4, 1.4, 6)
    c.ellipse(15, 9, 1.4, 1.4, 6)
    c.set(9, 9, 1)
    c.set(15, 9, 1)
    c.set(12, 11, 7)
    c.ellipse(12, 13.3, 2, 1.3, 7)               # hissing mouth
    c.set(11, 13, 8)
    c.set(13, 13, 8)
    c.ellipse(12, 20, 2.5, 3, 10)
    c.outline(1)
    return c


def cat_sit() -> Canvas:
    c = Canvas(16, 16)
    c.poly([(3, 15), (4, 9), (8, 7), (12, 9), (13, 15)], 3)
    c.ellipse(8, 12.5, 2, 2.5, 10)
    c.line(13, 14, 15, 11, 3)
    c.line(15, 11, 15, 8, 4)
    c.ellipse(8, 5.5, 4, 3.5, 3)
    c.poly([(4.5, 4.5), (4.5, 0.5), (7, 3)], 3)
    c.poly([(11.5, 4.5), (11.5, 0.5), (9, 3)], 3)
    c.set(5, 2, 9)
    c.set(11, 2, 9)
    c.set(6, 5, 6)
    c.set(10, 5, 6)
    c.set(8, 6.5, 7)
    c.line(6, 2.5, 6, 3.5, 5)
    c.line(10, 2.5, 10, 3.5, 5)
    c.outline(1)
    return c


def cat_dazed() -> Canvas:
    c = Canvas(16, 16)
    c.ellipse(9, 12, 6, 3, 3)
    c.line(3, 13, 0, 11, 3)
    c.ellipse(4, 10, 3.5, 3, 3)
    c.poly([(1.5, 9), (1, 5.5), (3.5, 7.5)], 3)
    c.poly([(6, 8), (6.5, 5), (4, 7)], 3)
    c.line(2, 9, 3, 10, 1)
    c.line(3, 9, 2, 10, 1)
    c.line(5, 9, 6, 10, 1)
    c.line(6, 9, 5, 10, 1)
    for x, y in ((3, 2), (8, 1), (12, 3)):
        c.set(x, y, 11)
        c.set(x - 1, y, 11)
        c.set(x + 1, y, 11)
        c.set(x, y - 1, 11)
        c.set(x, y + 1, 11)
    c.outline(1)
    return c


def fed(expr: str) -> Canvas:
    c = Canvas(24, 24)
    rng = random.Random(7)
    c.ellipse(12, 10, 11, 10, 3)                  # big curly hair
    for _ in range(26):
        a = rng.uniform(0, 2 * math.pi)
        c.ellipse(12 + math.cos(a) * 10, 10 + math.sin(a) * 9, 2.2, 2.2, rng.choice((2, 3, 4)))
    for _ in range(14):
        c.set(rng.randrange(3, 21), rng.randrange(1, 12), 4)
    c.ellipse(12, 14, 6.5, 7.5, 5)               # face
    c.ellipse(9, 18, 1.3, 1, 10)                 # blush
    c.ellipse(15, 18, 1.3, 1, 10)
    c.poly([(5, 8), (18, 8), (17, 5), (9, 4)], 3)  # fringe
    c.line(6, 21, 6, 20, 9)                      # earrings
    c.line(18, 21, 18, 20, 9)
    # cat-eye glasses
    for cx in (8.5, 15.5):
        c.ellipse(cx, 13, 2.8, 2.2, 12)
        c.set(cx - 1, 12, 11)
    c.line(5, 11.5, 6, 11, 7)
    c.line(19, 11.5, 18, 11, 7)
    c.line(5.5, 12, 11, 11, 7)
    c.line(12.5, 11, 18.5, 12, 7)
    c.line(11, 13, 13, 13, 7)
    if expr == "ok":
        c.set(8.5, 13, 1)
        c.set(15.5, 13, 1)
        c.line(9, 19, 15, 19, 8)
        c.set(8.5, 18, 8)
        c.set(15.5, 18, 8)
    elif expr == "ouch":
        c.line(7.5, 13, 9.5, 12.5, 1)
        c.line(14.5, 12.5, 16.5, 13, 1)
        c.ellipse(12, 19.5, 1.6, 1.8, 13)
        c.set(20, 13, 12)
        c.set(20, 14, 12)
        c.set(21, 12, 11)
    else:  # fierce
        c.line(6.5, 9.5, 10, 10.5, 1)
        c.line(17.5, 9.5, 14, 10.5, 1)
        c.set(8.5, 13, 1)
        c.set(15.5, 13, 1)
        c.rect(9, 18, 15, 19, 8)
        c.line(10, 18.5, 14, 18.5, 11)
    c.outline(1)
    return c


def item(kind: str) -> Canvas:
    if kind == "pizza":
        c = Canvas(16, 16)
        c.poly([(1, 3), (15, 3), (8, 15)], 3)          # crust
        c.poly([(2.5, 5), (13.5, 5), (8, 13.5)], 4)     # sauce
        for x, y in ((5, 6), (9, 7), (7, 9), (11, 6)):
            c.ellipse(x, y, 1.2, 1, 6)                  # cheese
        c.rect(1, 2, 15, 3, 2)
        c.outline(1)
        return c
    if kind == "food":
        c = Canvas(14, 14)
        c.rect(1, 3, 12, 12, 10)
        c.rect(1, 3, 12, 4, 15)
        c.rect(1, 11, 12, 12, 15)
        c.rect(2, 6, 11, 9, 8)
        c.poly([(3, 7.5), (7, 6), (9, 7.5), (7, 9)], 9)  # fish
        c.poly([(9, 7.5), (11, 6), (11, 9)], 9)
        c.set(4.5, 7, 1)
        c.outline(1)
        return c
    if kind == "yarn":
        c = Canvas(14, 14)
        c.ellipse(7, 7, 5.5, 5.5, 12)
        for k in range(-4, 6, 3):
            c.line(2 + k, 3, 8 + k, 12, 13)
        c.line(3, 5, 11, 4, 14)
        c.line(12, 10, 13, 13, 12)
        c.outline(1)
        return c
    if kind == "poison":
        c = Canvas(8, 8)
        c.ellipse(3.5, 4, 3.3, 3, 2)
        c.ellipse(3, 3.4, 2, 1.7, 3)
        c.set(2, 2, 5)
        c.set(6, 1, 3)
        return c
    if kind == "splat":
        c = Canvas(16, 8)
        c.ellipse(8, 5, 7.5, 2.8, 6)
        c.ellipse(7, 4.5, 5, 1.8, 2)
        c.ellipse(9, 4.7, 3, 1.1, 3)
        c.set(3, 2, 3)
        c.set(13, 7, 2)
        return c
    if kind == "debris":
        c = Canvas(12, 12)
        c.poly([(1, 4), (9, 1), (11, 7), (3, 10)], 3)
        c.line(1, 4, 9, 1, 4)
        c.line(4, 5, 9, 4, 2)
        c.set(6, 8, 5)
        c.outline(1)
        return c
    if kind == "claw":
        c = Canvas(16, 16)
        for k in range(3):
            c.line(3 + k * 4, 2, 7 + k * 4, 13, 2)
            c.line(4 + k * 4, 2, 8 + k * 4, 13, 1)
        c.set(1, 7, 4)
        c.set(14, 4, 4)
        return c
    raise ValueError(kind)


def prop(kind: str) -> Canvas:
    c = Canvas(24, 24)
    if kind == "bush":
        for x, y, r in ((7, 14, 6), (16, 14, 6.5), (12, 9, 6.5), (12, 17, 6)):
            c.ellipse(x, y, r, r * 0.85, 3)
        rng = random.Random(3)
        for _ in range(30):
            c.set(rng.randrange(2, 22), rng.randrange(4, 22), rng.choice((2, 4)))
        for x, y in ((6, 11), (14, 7), (18, 15), (10, 17)):
            c.set(x, y, 5)
            c.set(x + 1, y, 6)
        c.rect(10, 22, 13, 23, 13)
    elif kind == "bin":
        c.rect(5, 6, 18, 23, 8)
        c.rect(4, 4, 19, 6, 7)
        c.rect(10, 2, 13, 3, 7)
        for x in (7, 11, 15):
            c.line(x, 8, x, 22, 7)
        c.ellipse(15, 6, 4, 1.5, 3)               # goo on the lid
        c.set(16, 8, 3)
    elif kind == "grill":
        c.ellipse(12, 11, 9, 7, 9)
        c.rect(3, 11, 21, 12, 9)
        c.ellipse(9, 8, 3, 2, 7)
        c.line(6, 14, 3, 23, 9)
        c.line(18, 14, 21, 23, 9)
        c.line(12, 16, 12, 23, 9)
        c.rect(3, 9, 21, 9, 10)
        c.set(12, 3, 7)
    elif kind == "table":
        c.rect(1, 8, 22, 11, 3)
        c.rect(1, 8, 22, 8, 12)
        c.rect(2, 12, 21, 13, 5)              # tablecloth hem
        for x in (3, 20):
            c.rect(x, 12, x + 1, 23, 2)
        c.ellipse(8, 7, 2.5, 1, 8)            # plate
        c.rect(15, 4, 16, 7, 7)               # bottle
    elif kind == "chair":
        c.rect(6, 2, 17, 4, 3)
        for x in (6, 10, 14, 17):
            c.rect(x, 5, x, 13, 3)
        c.rect(5, 13, 18, 15, 4)
        for x in (6, 17):
            c.rect(x, 16, x, 23, 2)
    elif kind == "antenna":
        c.rect(11, 2, 12, 23, 7)
        for y, w in ((3, 10), (7, 8), (11, 6)):
            c.rect(12 - w, y, 12 + w - 1, y, 8)
            c.set(12 - w, y + 1, 7)
            c.set(12 + w - 1, y + 1, 7)
        c.line(11, 14, 5, 23, 9)
        c.line(12, 14, 18, 23, 9)
    elif kind == "vent":
        c.ellipse(12, 10, 7, 7, 8)
        for k in range(-6, 7, 3):
            c.line(12 + k, 4, 12 + k // 2, 16, 7)
        c.rect(8, 16, 15, 23, 7)
        c.rect(6, 22, 17, 23, 9)
    elif kind == "box":
        c.rect(2, 7, 21, 23, 3)
        c.rect(2, 7, 21, 9, 12)
        c.rect(10, 7, 13, 23, 11)
        c.rect(2, 7, 2, 23, 2)
        c.rect(3, 16, 8, 17, 10)              # label
    elif kind == "lamp":
        c.poly([(6, 2), (17, 2), (20, 9), (3, 9)], 11)
        c.line(3, 9, 20, 9, 12)
        c.ellipse(11.5, 5, 3, 2, 14)
        c.rect(11, 10, 12, 21, 13)
        c.rect(7, 22, 16, 23, 13)
    elif kind == "heater":
        c.rect(5, 3, 18, 22, 8)
        c.ellipse(11.5, 3, 6.5, 1.5, 9)
        c.rect(5, 3, 7, 22, 9)
        c.rect(15, 3, 18, 22, 7)
        c.rect(9, 15, 14, 18, 10)             # glowing burner window
        c.rect(11, 0, 12, 2, 7)
        c.rect(4, 22, 19, 23, 9)
        c.rect(10, 8, 13, 10, 12)
    elif kind == "portal":
        c = Canvas(32, 32)
        for y in range(32):
            for x in range(32):
                dx, dy = x - 15.5, y - 15.5
                r = math.hypot(dx, dy * 0.8)
                if r > 15.5:
                    continue
                a = math.atan2(dy, dx)
                v = (a / (2 * math.pi) * 4 + r / 3.5) % 1.0
                c.px[y][x] = [2, 3, 4, 10, 5, 6, 9, 7][int(v * 8) % 8]
        c.ellipse(15.5, 15.5, 3, 2.5, 6)
    elif kind == "anchor":
        c = Canvas(16, 24)
        c.poly([(8, 0), (13, 8), (11, 20), (5, 20), (3, 8)], 3)
        c.poly([(8, 1), (11, 8), (8, 18)], 4)
        c.line(8, 2, 5, 10, 7)
        c.rect(3, 20, 12, 23, 9)
        c.rect(2, 22, 13, 23, 8)
        c.set(6, 12, 6)
        c.set(10, 6, 5)
    c.outline(1)
    return c


sprite("ROACH_A", roach("a"), "roach")
sprite("ROACH_B", roach("b"), "roach")
sprite("ROACH_ATK", roach("atk"), "roach")
sprite("ROACH_DEAD", roach("dead"), "roach")
sprite("BEETLE_A", beetle("a"), "beetle")
sprite("BEETLE_B", beetle("b"), "beetle")
sprite("BEETLE_OPEN", beetle("open"), "beetle")
sprite("BEETLE_DEAD", beetle("dead"), "beetle")
sprite("QUEEN_A", queen("a"), "queen")
sprite("CAT_LEAP", cat_leap(), "cat_orange")
sprite("CAT_POUNCE", cat_pounce(), "cat_orange")
sprite("CAT_SIT", cat_sit(), "cat_orange")
sprite("CAT_DAZED", cat_dazed(), "cat_orange")
sprite("FED_OK", fed("ok"), "fed")
sprite("FED_OUCH", fed("ouch"), "fed")
sprite("FED_FIERCE", fed("fierce"), "fed")
sprite("PIZZA", item("pizza"), "items")
sprite("FOOD", item("food"), "items")
sprite("YARN", item("yarn"), "items")
sprite("POISON", item("poison"), "poison")
sprite("SPLAT", item("splat"), "poison")
sprite("DEBRIS", item("debris"), "debris")
sprite("CLAW", item("claw"), "fx")
for name in ("bush", "bin", "grill", "table", "chair", "antenna", "box", "lamp", "heater",
             "portal", "anchor"):
    sprite(name.upper(), prop(name), "portal" if name == "portal" else
           "anchor" if name == "anchor" else
           "props_wood" if name in ("table", "chair", "box", "lamp") else "props")

# Props: (sprite, palette, world width, world height) in 1/256 tile units.
PROPS = [
    ("BUSH", "BUSH", "props", 200, 180), ("BIN", "BIN", "props", 150, 170),
    ("GRILL", "GRILL", "props", 170, 170), ("TABLE", "TABLE", "props_wood", 220, 150),
    ("CHAIR", "CHAIR", "props_wood", 150, 170), ("ANTENNA", "ANTENNA", "props", 220, 300),
("BOX", "BOX", "props_wood", 190, 170),
    ("TRUNK", "BOX", "props_trunk", 200, 160), ("LAMP", "LAMP", "props_wood", 140, 270),
    ("HEATER", "HEATER", "props", 180, 260), ("PORTAL", "PORTAL", "portal", 420, 420),
]

# --------------------------------------------------------------------------
# Font, sine table, skyline
# --------------------------------------------------------------------------


def load_font() -> list[list[int]]:
    """Take characters 32..95 from the PRG32 firmware font (MIT, same author)."""
    src = (PRG32 / "components/prg32/prg32_display_qemu_rgb.c").read_text(encoding="utf-8")
    glyphs: dict[int, list[int]] = {}
    for m in re.finditer(r"\['(\\?.)' - 32\] = \{([^}]*)\}", src):
        ch = m.group(1)[-1]
        glyphs[ord(ch)] = [int(v, 16) for v in m.group(2).split(",") if v.strip()]
    missing = [chr(c) for c in range(32, 96) if c not in glyphs]
    if missing:
        raise SystemExit(f"font glyphs missing from PRG32 source: {missing}")
    return [glyphs[c] for c in range(32, 96)]


def skyline() -> list[int]:
    """Horizon silhouette heights for 128 compass steps (0 = east, y down).

    Hyde Park sits south of downtown Chicago: the tall skyline rises to the
    north, Lake Michigan is flat to the east, bungalows and trees elsewhere.
    """
    rng = random.Random(1893)
    h = []
    for i in range(128):
        a = i * 360.0 / 128.0          # 0 east, 90 south, 180 west, 270 north
        base = 3 + rng.randrange(4)
        if 250 <= a <= 300:            # downtown
            base = 8 + rng.randrange(14)
            if abs(a - 268) < 2.9:
                base = 34              # the tallest tower and its antennas
            elif abs(a - 280) < 2.9:
                base = 28
        elif a < 30 or a > 340:
            base = 1                   # the lake
        elif 100 < a < 160 and rng.random() < 0.3:
            base = 9                   # the park's big trees
        h.append(base)
    return h


# --------------------------------------------------------------------------
# Emit C
# --------------------------------------------------------------------------

def pack4(c: Canvas) -> list[int]:
    flat = [v for row in c.px for v in row]
    if len(flat) & 1:
        flat.append(0)
    return [(flat[i] << 4) | flat[i + 1] for i in range(0, len(flat), 2)]


def hexrows(data: list[int], per: int = 16, indent: str = "    ") -> list[str]:
    return [indent + ", ".join(f"0x{v:02x}" for v in data[i:i + per]) + ","
            for i in range(0, len(data), per)]


def emit_header() -> str:
    pal_names = list(PALETTES)
    pal_id = {n: i for i, n in enumerate(pal_names)}
    pat_names = list(PATTERNS)
    pat_id = {n: i for i, n in enumerate(pat_names)}
    out = [
        "/* GENERATED by tools/gen_assets.py -- do not edit. All art is procedural. */",
        "#ifndef CC_ASSETS_H",
        "#define CC_ASSETS_H",
        "",
        "/* Palettes: 16 cube indices (r*36 + g*6 + b) each. */",
    ]
    out.append("enum {")
    out += [f"    PAL_{n.upper()}," for n in pal_names]
    out += ["    PAL_COUNT", "};", ""]
    out.append("static const uint8_t pal_sets[PAL_COUNT][16] = {")
    for n in pal_names:
        out.append("    {" + ", ".join(str(v) for v in PALETTES[n]) + "},")
    out += ["};", ""]

    out.append(f"/* 16x16 4-bpp wall/floor patterns, row-major, left pixel in the high nibble. */")
    out.append(f"#define PATTERN_COUNT {len(pat_names)}")
    out.append("static const uint8_t tex_patterns[PATTERN_COUNT][128] = {")
    for n in pat_names:
        out.append(f"    {{ /* {n} */")
        out += hexrows(pack4(PATTERNS[n]), 16, "        ")
        out.append("    },")
    out += ["};", ""]
    out.append("enum {")
    out += [f"    TEX_{n}," for n, _, _ in TEXTURES]
    out += ["    TEX_COUNT", "};", ""]
    out.append("/* Texture = pattern + palette. */")
    out.append("static const uint8_t tex_defs[TEX_COUNT][2] = {")
    for n, p, pal in TEXTURES:
        out.append(f"    {{{pat_id[p]}, PAL_{pal.upper()}}}, /* {n} */")
    out += ["};", ""]

    offsets, blob = [], []
    for name, c, pal in SPRITES:
        offsets.append(len(blob))
        blob += pack4(c)
    out.append("enum {")
    out += [f"    SPR_{name}," for name, _, _ in SPRITES]
    out += ["    SPR_COUNT", "};", ""]
    out.append("/* Sprite frames: byte offset into spr_pixels, width, height, default palette. */")
    out.append("typedef struct { uint16_t off; uint8_t w, h, pal; } spr_info_t;")
    out.append("static const spr_info_t spr_info[SPR_COUNT] = {")
    for (name, c, pal), off in zip(SPRITES, offsets):
        out.append(f"    {{{off}, {c.w}, {c.h}, PAL_{pal.upper()}}}, /* {name} */")
    out += ["};", "", f"static const uint8_t spr_pixels[{len(blob)}] = {{"]
    out += hexrows(blob)
    out += ["};", ""]

    out.append("enum {")
    out += [f"    PROP_{n}," for n, _, _, _, _ in PROPS]
    out += ["    PROP_COUNT", "};", ""]
    out.append("/* Props: sprite, palette, world width and height (1/256 tile). */")
    out.append("typedef struct { uint8_t spr, pal; uint16_t ww, wh; } prop_info_t;")
    out.append("static const prop_info_t prop_info[PROP_COUNT] = {")
    for n, spr, pal, ww, wh in PROPS:
        out.append(f"    {{SPR_{spr}, PAL_{pal.upper()}, {ww}, {wh}}}, /* {n} */")
    out += ["};", ""]

    font = load_font()
    out.append("/* 8x8 glyphs for ASCII 32..95, taken from the PRG32 firmware font. */")
    out.append("static const uint8_t font8[64][8] = {")
    for g in font:
        out.append("    {" + ", ".join(f"0x{v:02x}" for v in g) + "},")
    out += ["};", ""]

    sin = [round(math.sin(i * math.pi / 512.0) * 16384) for i in range(257)]
    out.append("/* Quarter-wave sine, angle units of 1/1024 turn, Q14. */")
    out.append("static const int16_t sin_q14[257] = {")
    for i in range(0, 257, 12):
        out.append("    " + ", ".join(str(v) for v in sin[i:i + 12]) + ",")
    out += ["};", ""]

    sky = skyline()
    out.append("/* Horizon silhouette (pixels above the horizon) for 128 headings. */")
    out.append("static const uint8_t skyline[128] = {")
    for i in range(0, 128, 16):
        out.append("    " + ", ".join(str(v) for v in sky[i:i + 16]) + ",")
    out += ["};", "", "#endif", ""]
    return "\n".join(out), len(blob), len(pat_names) * 128


# --------------------------------------------------------------------------
# Previews and audio
# --------------------------------------------------------------------------

def render(c: Canvas, pal: list[int], transparent: bool, scale: int):
    from PIL import Image
    img = Image.new("RGBA", (c.w * scale, c.h * scale), (40, 40, 48, 255))
    for y in range(c.h):
        for x in range(c.w):
            v = c.px[y][x]
            if transparent and v == 0:
                continue
            rgb = cube_rgb(pal[v])
            for yy in range(scale):
                for xx in range(scale):
                    img.putpixel((x * scale + xx, y * scale + yy), rgb + (255,))
    return img


def previews() -> None:
    try:
        from PIL import Image
    except ImportError:  # e.g. inside the ESP-IDF Python environment
        print("PIL not available: skipping assets/preview/*.png")
        return
    PREVIEW.mkdir(parents=True, exist_ok=True)
    s = 4
    cols = 8
    rows = (len(TEXTURES) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * (T * s + 4), rows * (T * s + 4)), (0, 0, 0, 255))
    for i, (n, p, pal) in enumerate(TEXTURES):
        tile = render(PATTERNS[p], PALETTES[pal], False, s)
        sheet.paste(tile, ((i % cols) * (T * s + 4), (i // cols) * (T * s + 4)))
    sheet.save(PREVIEW / "textures.png")
    cols = 8
    cell = 32 * s + 4
    extra = [("roach", "runner"), ("roach", "spitter"), ("roach", "elite"), ("roach", "flyer"),
             ("beetle", "armored"), ("roach", "butler"), ("beetle", "couch"), ("beetle", "general")]
    items = [(c, PALETTES[p]) for _, c, p in SPRITES]
    for base, pal in extra:
        items.append((roach("a") if base == "roach" else beetle("a"), PALETTES[pal]))
    rows = (len(items) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell, rows * cell), (60, 60, 70, 255))
    for i, (c, pal) in enumerate(items):
        sheet.paste(render(c, pal, True, s), ((i % cols) * cell, (i // cols) * cell))
    sheet.save(PREVIEW / "sprites.png")


def audio() -> None:
    """Four single-cycle 84-sample waves: at 22050 Hz a loop plays C4."""
    samples = AUDIO_DIR / "samples"
    samples.mkdir(parents=True, exist_ok=True)
    n = 84
    waves = {
        "pulse": [200 if i < n // 4 else 56 for i in range(n)],
        "saw": [int(40 + 176 * i / (n - 1)) for i in range(n)],
        "tri": [int(128 + 90 * (1 - 4 * abs(i / n - 0.5))) for i in range(n)],
    }
    rng = random.Random(99)
    waves["noise"] = [rng.randrange(40, 216) for _ in range(512)]
    for name, data in waves.items():
        (samples / f"{name}.u8").write_bytes(bytes(data))

    def synth(wave: int, pulse: int, cutoff: int, res: int) -> int:
        # Bit layout decoded by prg32_audio_synth_start(): wave 1:0,
        # resonance 6:4, cutoff 9:7, pulse width 13:10 (see docs/design.md).
        assert res <= 3
        return 0x8000 | wave | (res << 4) | (cutoff << 7) | (pulse << 10)

    def ins(name, sid, a=0, d=0, s=255, r=0, vol=220):
        return {"name": name, "sample_id": sid, "default_volume": vol, "default_pan": 0,
                "attack": a, "decay": d, "sustain": s, "release": r}

    cfg = {
        "comment": "Cockroaches Cathisteria AUDIO block: four PCM waves and seven instruments. "
                   "Music and effects are sequenced by the cartridge (src/audio.c).",
        "samples": [{"file": f"samples/{n}.u8", "base_note": 60, "loop": True}
                    for n in ("pulse", "saw", "tri", "noise")],
        "instruments": [
            ins("PULSE", 0), ins("SAW", 1), ins("TRI", 2), ins("NOISE", 3),
            ins("BASS", synth(0, 7, 6, 1), 1, 60, 150, 30),
            ins("KICK", synth(0, 7, 7, 0), 0, 30, 0, 10),
            ins("BOOM", synth(3, 7, 4, 1), 0, 110, 0, 40),
        ],
    }
    (AUDIO_DIR / "cc_audio.json").write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    text, spr_bytes, tex_bytes = emit_header()
    OUT_H.write_text(text, encoding="utf-8")
    previews()
    audio()
    print(f"wrote {OUT_H.relative_to(ROOT)}: {len(SPRITES)} sprites ({spr_bytes} bytes), "
          f"{len(PATTERNS)} patterns ({tex_bytes} bytes), {len(TEXTURES)} textures, "
          f"{len(PALETTES)} palettes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
