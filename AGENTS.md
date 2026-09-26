# AGENTS.md

Guidance for automated coding agents and maintainers of Cockroaches Cathisteria.

## Project intent

An original, Doom-inspired PRG32 cartridge: Fed defends her Hyde Park house
from alien cockroaches with her cats. Keep it copyright-safe: no names, art,
sounds or level layouts from commercial games. All art comes from
`tools/gen_assets.py`, all levels from `levels/*.txt`.

The cats are the core mechanic (attack with A, shield with B). Do not turn
them into a generic gun; see `docs/design.md`.

## Source layout

`src/main.c` `#include`s the modules in dependency order (a unity build):
a module may use what earlier modules define, or what `src/game.h` declares.
`src/assets.h` and `src/level_data.h` are generated: edit the generators or
`levels/*.txt`, never the headers.

## Rules that are easy to break

- **No pointers in initialised static data.** Portable cartridges have no
  relocations. Use offsets (see `level_text[]`, `spr_info[]`) and 2-D arrays.
- **No libc, no floats, no 64-bit division or shifts.** Freestanding RV32IMC,
  Q8 positions, Q12 directions, Q14 trigonometry.
- **Never draw with `prg32_gfx_*` directly.** Everything goes through the strip
  compositor in `src/raycast.c` (`render_band` with ops).
- **Colours are cube indices** (`CUBE(r, g, b)`, components 0..5).
- **Budget:** `scripts/build.sh` prints `mem=` (must stay <= 65,536; about
  2.3 KB free) and the store image size (<= 65,536). Measure before adding
  tables; `docs/design.md` has the breakdown.
- **Progression only in `src/game.c`.** Other modules report events through
  `game_set_state()`, `on_boss_defeated()` and `level_complete()`.
- **Build switches** (`CC_TARGET_QEMU`, `CC_START_LEVEL`, `CC_TEST_GOD`,
  `CC_HOST`) stay 0 in the sources; set them in wrapper files.
- **Synth instruments** use the decoder's real bit layout with resonance <= 3
  (`docs/design.md`, section 8).

## Workflow

```bash
python3 tools/gen_levels.py && python3 tools/gen_assets.py   # if levels/art changed
tests/run_tests.sh                                           # must report 0 failures
tests/run_tests.sh shots                                     # look at build/shots/
scripts/build.sh && scripts/pack-store-bundle.sh
python3 scripts/qemu_preview.py --script play --out build/preview   # optional
```

Update `README.md` and `docs/design.md` when gameplay, rendering, audio or the
memory budget change.
