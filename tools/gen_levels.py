#!/usr/bin/env python3
"""Compile the ASCII level files in levels/*.txt into src/level_data.h.

Every level is a small text file: a header of `key: value` lines followed by
a `map:` block drawn with one character per tile. The compiler validates the
map (rectangular, closed border, one start, reachable objectives) and emits
pointer-free C tables, because portable PRG32 cartridges have no relocations:

  level_maps[]    4-bit tile codes, two tiles per byte
  level_spawns[]  enemy spawn triples (x, y, type | wave << 4)
  level_defs[]    one level_def_t per level (see src/game.h)

Map legend (tile code in brackets):

  .  floor [0]           #  wall A [1]        %  wall B [9]     &  wall C [10]
  D  door [2]            o  prop A [3]        q  prop B [14]    X  exit [4]
  *  boss spawn [5]      c  cat rescue [6]    h  pizza alla Nerano (health) [7]
  P  portal anchor [8]   m  cat food [11]     y  yarn ball (shield boost) [12]
  v  roof hole [13]      @  player start
  enemies, wave 1 / wave 2:
     r/R roach   f/F runner   s/S spitter   b/B beetle   a/A armored beetle
     e/E elite   w/W winged roach

Doors open when wave 1 is cleared, so wave-1 enemies must be reachable with
the doors closed. Waves beyond 2 re-use the wave-1 spawn points with tougher
enemy types (see level.c).
"""

from __future__ import annotations

import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEVEL_DIR = ROOT / "levels"
OUT = ROOT / "src" / "level_data.h"

TILES = {
    ".": 0, "#": 1, "D": 2, "o": 3, "X": 4, "*": 5, "c": 6, "h": 7, "P": 8,
    "%": 9, "&": 10, "m": 11, "y": 12, "v": 13, "q": 14,
}
ENEMIES = {"r": "E_ROACH", "f": "E_RUNNER", "s": "E_SPITTER", "b": "E_BEETLE",
           "a": "E_ARMORED", "e": "E_ELITE", "w": "E_FLYER"}
SOLID = {"#", "%", "&", "o", "q"}
DIRS = {"E": 0, "S": 256, "W": 512, "N": 768}
TEXT_LIMIT = 36


def fail(path: Path, message: str) -> None:
    raise SystemExit(f"{path.name}: {message}")


def parse(path: Path) -> dict:
    header: dict[str, list[str]] = {}
    rows: list[str] = []
    in_map = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        if in_map:
            if raw.strip():
                rows.append(raw.rstrip())
            continue
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line == "map:":
            in_map = True
            continue
        key, _, value = line.partition(":")
        header.setdefault(key.strip(), []).append(value.strip())
    return {"header": header, "rows": rows}


def one(path: Path, header: dict, key: str, default: str | None = None) -> str:
    values = header.get(key)
    if not values:
        if default is None:
            fail(path, f"missing '{key}:'")
        return default
    return values[0]


def text(path: Path, value: str, limit: int = TEXT_LIMIT) -> str:
    value = value.upper()
    if len(value) > limit:
        fail(path, f"text longer than {limit} characters: {value!r}")
    bad = [ch for ch in value if not (32 <= ord(ch) <= 95)]
    if bad:
        fail(path, f"characters {bad} are not in the in-game font")
    return value


def c_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def reachable(rows: list[str], start: tuple[int, int], doors_open: bool) -> set:
    h, w = len(rows), len(rows[0])
    seen = {start}
    queue = deque([start])
    while queue:
        x, y = queue.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if not (0 <= nx < w and 0 <= ny < h) or (nx, ny) in seen:
                continue
            ch = rows[ny][nx]
            if ch in SOLID or ch == "v" or (ch == "D" and not doors_open):
                continue
            seen.add((nx, ny))
            if ch != "X":  # the exit is a destination, not a corridor
                queue.append((nx, ny))
    return seen


def compile_level(path: Path, index: int) -> dict:
    data = parse(path)
    hd, rows = data["header"], data["rows"]
    if not rows:
        fail(path, "empty map")
    w, h = len(rows[0]), len(rows)
    if any(len(r) != w for r in rows):
        fail(path, "map rows have different lengths: " +
             ", ".join(f"{i}:{len(r)}" for i, r in enumerate(rows) if len(r) != w))
    if not (8 <= w <= 32 and 8 <= h <= 32) or w * h > 640:
        fail(path, f"map is {w}x{h}; sides must be 8..32 and w*h at most 640 (MAP_CELLS)")
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch not in TILES and ch.lower() not in ENEMIES and ch != "@":
                fail(path, f"unknown map character {ch!r} at {x},{y}")
            if (x in (0, w - 1) or y in (0, h - 1)) and ch not in "#%&XD":
                fail(path, f"border tile {ch!r} at {x},{y} must be a wall or exit")

    def find(ch: str) -> list[tuple[int, int]]:
        return [(x, y) for y, r in enumerate(rows) for x, c in enumerate(r) if c == ch]

    starts, bosses, exits = find("@"), find("*"), find("X")
    if len(starts) != 1:
        fail(path, "exactly one '@' start is required")
    if len(bosses) != 1:
        fail(path, "exactly one '*' boss spawn is required")
    has_exit = one(path, hd, "exit", "yes") != "none"
    if has_exit and len(exits) != 1:
        fail(path, "exactly one 'X' exit is required")
    if not has_exit and exits:
        fail(path, "'exit: none' levels must not contain 'X'")
    anchors = find("P")
    if one(path, hd, "hazard") == "HAZ_PORTAL" and len(anchors) != 4:
        fail(path, "the portal level needs exactly four 'P' anchors")

    closed = reachable(rows, starts[0], doors_open=False)
    opened = reachable(rows, starts[0], doors_open=True)
    targets = bosses + exits + anchors
    for y, r in enumerate(rows):
        for x, ch in enumerate(r):
            if ch in "chmy" or ch.lower() in ENEMIES:
                targets.append((x, y))
            if ch in ENEMIES and (x, y) not in closed:
                fail(path, f"wave-1 enemy {ch!r} at {x},{y} is behind a closed door")
    for t in targets:
        if t not in opened and rows[t[1]][t[0]] != "P":
            fail(path, f"{rows[t[1]][t[0]]!r} at {t[0]},{t[1]} is unreachable")

    codes = []
    spawns = []
    for y, r in enumerate(rows):
        for x, ch in enumerate(r):
            low = ch.lower() if ch.isalpha() else ch
            if ch != "@" and low in ENEMIES and ch not in TILES:
                wave = 1 if ch.islower() else 2
                spawns.append((x, y, f"{ENEMIES[low]} | (({wave}) << 4)"))
                codes.append(0)
            else:
                codes.append(TILES.get(ch, 0))
    if len(spawns) > 24:
        fail(path, "at most 24 enemy spawns per level")

    intro = [text(path, v) for v in hd.get("intro", [])][:2]
    outro = [text(path, v) for v in hd.get("outro", [])][:2]
    while len(intro) < 2:
        intro.append("")
    while len(outro) < 2:
        outro.append("")
    walls = one(path, hd, "walls").split()
    props = one(path, hd, "props").split()
    if len(walls) != 3 or len(props) != 2:
        fail(path, "'walls:' needs three textures and 'props:' two props")
    return {
        "index": index,
        "key": one(path, hd, "key"),
        "name": text(path, one(path, hd, "name"), 15),
        "place": text(path, one(path, hd, "place"), 27),
        "intro": intro, "outro": outro,
        "quip": text(path, one(path, hd, "quip")),
        "w": w, "h": h, "codes": codes, "spawns": spawns,
        "start": starts[0], "dir": DIRS[one(path, hd, "facing")],
        "boss_xy": bosses[0],
        "sky": one(path, hd, "sky"),
        "floor": one(path, hd, "floor"), "ceiling": one(path, hd, "ceiling"),
        "fog": int(one(path, hd, "fog")),
        "walls": walls, "door": one(path, hd, "door"), "props": props,
        "music": one(path, hd, "music"), "hazard": one(path, hd, "hazard"),
        "waves": int(one(path, hd, "waves")), "boss": one(path, hd, "boss"),
        "flags": one(path, hd, "flags", "0"),
    }


def emit(levels: list[dict]) -> str:
    out = [
        "/* GENERATED by tools/gen_levels.py from the levels/ directory -- do not edit. */",
        "#ifndef CC_LEVEL_DATA_H",
        "#define CC_LEVEL_DATA_H",
        "",
        f"#define LEVEL_COUNT {len(levels)}",
        "",
    ]
    for lv in levels:
        out.append(f"#define LV_{lv['key']} {lv['index']}")
    maps: list[int] = []
    spawns: list[str] = []
    defs: list[str] = []
    blob: list[str] = []
    offsets: dict[str, int] = {}

    def txt(value: str) -> int:
        """Offset of a string in level_text[], sharing duplicates."""
        if value not in offsets:
            offsets[value] = sum(len(s) + 1 for s in blob)
            blob.append(value)
        return offsets[value]

    for lv in levels:
        map_off = len(maps)
        codes = lv["codes"] + ([0] if len(lv["codes"]) & 1 else [])
        maps.extend((codes[i] << 4) | codes[i + 1] for i in range(0, len(codes), 2))
        spawn_off = len(spawns) // 3
        for x, y, t in lv["spawns"]:
            spawns.extend([str(x), str(y), t])
        sx, sy = lv["start"]
        bx, by = lv["boss_xy"]
        defs.append("\n".join([
            "    {",
            f"        .name = {txt(lv['name'])}, .place = {txt(lv['place'])},",
            f"        .intro = {{{txt(lv['intro'][0])}, {txt(lv['intro'][1])}}},",
            f"        .quip = {txt(lv['quip'])},",
            f"        .outro = {{{txt(lv['outro'][0])}, {txt(lv['outro'][1])}}},",
            f"        .map_off = {map_off}, .w = {lv['w']}, .h = {lv['h']},",
            f"        .spawn_off = {spawn_off}, .spawn_count = {len(lv['spawns'])},",
            f"        .start_x = {sx}, .start_y = {sy}, .start_dir = {lv['dir'] // 4},",
            f"        .boss_x = {bx}, .boss_y = {by},",
            f"        .sky = {lv['sky']}, .floor_tex = {lv['floor']}, .ceil_tex = {lv['ceiling']},",
            f"        .fog = {lv['fog']},",
            f"        .wall_tex = {{{', '.join(lv['walls'])}}}, .door_tex = {lv['door']},",
            f"        .props = {{{', '.join(lv['props'])}}},",
            f"        .music = {lv['music']}, .hazard = {lv['hazard']}, .waves = {lv['waves']},",
            f"        .boss = {lv['boss']}, .flags = {lv['flags']},",
            "    },",
        ]))
    size = sum(len(s) + 1 for s in blob)
    out += ["", "/* Every level string, NUL-separated; level_def_t holds offsets. */",
            f"static const char level_text[{size}] ="]
    out += [f"    {c_string(s)} \"\\0\"" for s in blob[:-1]]
    out += [f"    {c_string(blob[-1])};"]
    out += ["", f"static const uint8_t level_maps[{len(maps)}] = {{"]
    for i in range(0, len(maps), 16):
        out.append("    " + ", ".join(f"0x{b:02x}" for b in maps[i:i + 16]) + ",")
    out += ["};", "", f"static const uint8_t level_spawns[{len(spawns)}] = {{"]
    for i in range(0, len(spawns), 3):
        out.append("    " + ", ".join(spawns[i:i + 3]) + ",")
    out += ["};", "", "static const level_def_t level_defs[LEVEL_COUNT] = {"]
    out += defs
    out += ["};", "", "#endif", ""]
    return "\n".join(out)


def main() -> int:
    files = sorted(LEVEL_DIR.glob("*.txt"))
    if not files:
        raise SystemExit("no levels/*.txt files")
    levels = [compile_level(p, i) for i, p in enumerate(files)]
    OUT.write_text(emit(levels), encoding="utf-8")
    total = sum((lv["w"] * lv["h"] + 1) // 2 for lv in levels)
    print(f"wrote {OUT.relative_to(ROOT)}: {len(levels)} levels, {total} map bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
