#!/usr/bin/env python3
"""Make the Cartridge Store icon and screenshot.

  assets/icon.png        64x64, drawn from the game's own sprites
  assets/screenshot.png  320x200 gameplay frame rendered by the host build

The screenshot comes from `tests/run_tests.sh shots` (the host framebuffer
is pixel-identical to the cartridge's strips), so run that first. Both
images are stored with small palettes because the store trailer counts
towards the 64 KiB cartridge limit.
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import gen_assets as ga  # noqa: E402

SHOT = "39_boss.png"


def icon() -> Image.Image:
    img = Image.new("RGB", (64, 64), ga.cube_rgb(ga.C(1, 0, 1)))
    roach = ga.render(ga.roach("atk"), ga.PALETTES["elite"], True, 1).resize((24, 24))
    cat = ga.render(ga.cat_pounce(), ga.PALETTES["cat_orange"], True, 2)
    img.paste(roach, (38, 38), roach)
    img.paste(roach.transpose(Image.FLIP_LEFT_RIGHT), (2, 38), roach.transpose(Image.FLIP_LEFT_RIGHT))
    img.paste(cat, (8, 2), cat)
    return img


def main() -> int:
    out = ROOT / "assets"
    out.mkdir(exist_ok=True)
    icon().quantize(colors=32).save(out / "icon.png", optimize=True)
    shot = ROOT / "build" / "shots" / SHOT
    if not shot.exists():
        raise SystemExit(f"{shot} is missing: run tests/run_tests.sh shots first")
    frame = Image.open(shot).convert("RGB").crop((0, 20, 320, 220))
    frame.quantize(colors=48, dither=Image.Dither.NONE).save(out / "screenshot.png", optimize=True)
    for name in ("icon.png", "screenshot.png"):
        print(f"assets/{name}: {(out / name).stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
