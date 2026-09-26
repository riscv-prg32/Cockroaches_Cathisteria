/*
 * config.h -- constants, object limits and compile-time switches.
 *
 * Cockroaches Cathisteria is a "unity build": src/main.c #includes every
 * module in dependency order, because the PRG32 cartridge builder compiles a
 * single source file. Portable cartridges are freestanding RV32IMC code with
 * no relocations, so the rules are: no libc, no floats, no 64-bit division,
 * and no pointers inside initialised static tables.
 */
#ifndef CC_CONFIG_H
#define CC_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "prg32.h"

/* Cartridge Store id; also the local score key (at most 23 characters). */
#define GAME_ID "prg32.cockroaches"

/* Build switches. All must stay 0 in the sources: scripts/build.sh and the
 * host tests set them in wrapper files. */
#ifndef CC_TARGET_QEMU
#define CC_TARGET_QEMU 0 /* QEMU: one present per frame (see game.c) */
#endif
#ifndef CC_START_LEVEL
#define CC_START_LEVEL 0 /* first level of a new game */
#endif
#ifndef CC_TEST_GOD
#define CC_TEST_GOD 0 /* Fed cannot lose health */
#endif
#ifndef CC_HOST
#define CC_HOST 0 /* native host build for tests (uses the host libc) */
#endif
#ifndef CC_COUNT
#define CC_COUNT(n) ((void)0) /* host cost-model hook, see tests/ */
#endif

/* Screen layout: the 3D view on top, the status bar below. */
#define SW PRG32_GAME_W /* 320 */
#define SH PRG32_GAME_H /* 200 */
#define VIEW_H 136
#define HORIZON (VIEW_H / 2)
#define NRAYS 160           /* one ray per 2-pixel column */
#define STRIP_H 4           /* rows composed per blit */
#define HUD_Y VIEW_H

/* Camera: plane length 0.66 (a 67 degree field of view). A wall one tile
 * away is WALL_S pixels tall; one tile across at distance 1 is FOCAL pixels. */
#define PLANE_Q12 2703
#define FOCAL 242
#define WALL_S 200
#define FOV_UNITS 190 /* the field of view in 1/1024 turn */

/* Fixed-step simulation: 30 logic ticks per second, whatever the frame rate. */
#define TICK_MS 33
#define MAX_TICKS_PER_FRAME 4

/* World units: positions are Q8, 256 = one map tile. */
#define ONE 256
#define MAP_MAX 32
#define MAP_CELLS 640 /* w * h limit, checked by tools/gen_levels.py */

/* Object pools. Sized from the budget in docs/design.md: every pool is a
 * fixed array in .bss, nothing is allocated at run time. */
#define MAX_ENEMIES 20
#define MAX_PROJECTILES 16
#define MAX_CATS 9
#define MAX_PICKUPS 20
#define MAX_PUDDLES 6
#define MAX_PROPS 28
#define MAX_DEBRIS 3
#define MAX_VIS 40
#define MAX_OPS 36

#define IABS(v) ((v) < 0 ? -(v) : (v))
#define IMIN(a, b) ((a) < (b) ? (a) : (b))
#define IMAX(a, b) ((a) > (b) ? (a) : (b))
#define ICLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

/* A cube colour from 0..5 components (the PRG32 6x6x6 system cube). */
#define CUBE(r, g, b) ((uint8_t)((r) * 36 + (g) * 6 + (b)))

#endif
