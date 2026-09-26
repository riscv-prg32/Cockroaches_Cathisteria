/*
 * Cockroaches Cathisteria -- a Doom-style PRG32 cartridge.
 *
 * Fed lives in a three-story house on South Harper Street, Hyde Park,
 * Chicago, with her feline armada. When an interdimensional portal opens in
 * her basement, she fights her way from the front yard, up the chimney,
 * across the roof and down through the whole house to seal it -- with cats
 * as her only weapons.
 *
 * The PRG32 builder compiles a single source file, so this file #includes
 * every module in dependency order (a "unity build"):
 *
 *   config.h       constants, pool sizes, build switches
 *   assets.h       generated art tables (tools/gen_assets.py)
 *   game.h         shared types, global state, declarations
 *   level_data.h   generated level tables (tools/gen_levels.py)
 *   fixmath.c      memset/memcpy, trigonometry, random numbers, strings
 *   audio.c        sound effects and the music player
 *   raycast.c      raycaster, billboards, strip compositor
 *   collision.c    map queries, sliding movement, line of sight
 *   level.c        level loading and enemy waves
 *   player.c       Fed: movement, shield, damage, pickups
 *   cats.c         the feline armada
 *   enemy.c        cockroaches and beetles
 *   boss.c         the reusable boss framework
 *   projectile.c   poison, puddles, falling bricks
 *   hud.c          status bar, overlays, pages
 *   game.c         state machine and cartridge entry points
 *
 * The runtime calls cockroaches_init() once, then cockroaches_update() and
 * cockroaches_draw() every frame. docs/design.md describes the design.
 */

#include "config.h"
#include "assets.h"
#include "game.h"
#include "level_data.h"
#include "fixmath.c"
#include "audio.c"
#include "raycast.c"
#include "collision.c"
#include "level.c"
#include "player.c"
#include "cats.c"
#include "enemy.c"
#include "boss.c"
#include "projectile.c"
#include "hud.c"
#include "game.c"
