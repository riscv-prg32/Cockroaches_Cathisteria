# Cockroaches Cathisteria: design and technical notes

This document explains how the cartridge is built and why. The README covers how to play.

## 1. Constraints

The PRG32 runtime shapes every decision:

| Constraint | Value | Consequence |
|---|---|---|
| Cartridge RAM (code + data + bss) | 65,536 bytes | every table is budgeted; no framebuffer of our own |
| Store image (code + audio + metadata) | 65,536 bytes | small palettised store art, a tiny AUDIO block |
| Load image (code + data + audio, one heap block) | about 54.5 KB in QEMU (measured) | tighter than the RAM limit: bss does not count, tables do |
| Portable ABI cartridges | no relocations | no pointers in initialised static tables |
| Freestanding RV32IMC, no libgcc | | integer and fixed-point maths, own `memset`/`memcpy` |
| One source file per build | | unity build: `src/main.c` includes every module |
| ILI9341 over 32 MHz SPI | about 0.62 µs per pixel sent | the 3D view is 320x136, the status bar is redrawn only on change |

## 2. Architecture

The design brief suggested one `.c`/`.h` pair per system. The PRG32 builder compiles a single file, so the systems are kept as separate modules that `main.c` includes in dependency order. Types and cross-module declarations live in one `game.h`:

| Module | Responsibility |
|---|---|
| `config.h` | screen layout, camera constants, pool sizes, build switches |
| `game.h` | enums, records, global state, forward declarations |
| `assets.h` | generated: textures, sprites, palettes, props, font, sine, skyline |
| `level_data.h` | generated: maps, spawns, level definitions |
| `fixmath.c` | `memset`/`memcpy`, `isin`/`icos` (Q14), `isqrt`, xorshift RNG, string pool |
| `audio.c` | sound-effect sequencer, three-voice music, portal ambience |
| `raycast.c` | palette and shading, DDA raycaster, floor/ceiling casting, billboards, 2D ops, strip compositor |
| `collision.c` | tile solidity, sliding movement, line of sight |
| `level.c` | level loading, props/pickups/anchors from tiles, waves, doors, checkpoints |
| `player.c` | movement, strafing, feline shield, damage rules, pickups |
| `cats.c` | the armada: targeting, launch, maul, return, knockouts, recovery |
| `enemy.c` | enemy types and the IDLE/CHASE/ATTACK/SPIT/CHARGE/HURT/DEAD machine |
| `boss.c` | the boss table and the shared boss pattern machine |
| `projectile.c` | poison spit, puddles, falling bricks, level hazards |
| `hud.c` | status bar, status bands, view overlays, full-screen pages |
| `game.c` | the top-level state machine and the three entry points |

Progression logic lives only in `game.c`. Other modules report events through `game_set_state()`, `on_boss_defeated()` and `level_complete()`, and both of the last two are idempotent.

```text
TITLE -> INTRO -> LEVEL_START -> PLAYING -> ROOM_CLEARED -> BOSS
                     ^             ^  |                      |
                     |             |  `--(exit reached)--> LEVEL_COMPLETE -> next LEVEL_START
                     |             `----(boss down: exit opens)
               PLAYER_DEAD --A--> LEVEL_START (restart from the checkpoint)
                    `-----B--> GAME_OVER -> TITLE
last level:  BOSS -> FINAL_PORTAL -> ENDING -> TITLE
```

### Timing

`cockroaches_update()` accumulates the elapsed `prg32_ticks_ms()` and runs the simulation in fixed 33 ms ticks (at most four per frame, and at most 200 ms of backlog). Gameplay speed is therefore the same on the ESP32-C6, in QEMU and in the host tests, and the tests drive `game_tick()` directly. Button presses are latched between ticks, so a press shorter than a tick is never lost.

## 3. Rendering

### Pipeline

Each frame in a world state does the following:

1. **Cast** 160 rays (one per 2-pixel column) through the map with an integer DDA. Positions are Q8 (256 = one tile) and directions Q12. For each ray it stores the perpendicular distance (the z-buffer), the wall's top and bottom, the texel column, the palette, a shade, and the skyline height for that heading.
2. **Project** every enemy, cat, projectile, pickup, puddle and prop into camera space (depth and lateral offset against the view direction), scale it by `FOCAL` horizontally and `WALL_S` vertically, and sort far to near. At most 40 billboards are drawn.
3. **Compose** the view in 4-row strips. For every row the floor or ceiling is floor-cast: the row distance is precomputed, and the world position steps linearly across the 160 columns. Wall texels come from the per-ray data. Sky rows use a gradient, stars and a building silhouette. Sprites are then drawn column by column, hidden behind nearer walls, followed by the 2D overlays (text, rectangles, dimming, scaled sprites) from a small retained op list.
4. **Blit** each finished strip with `prg32_sprite_draw_indexed()` as an 8-bit indexed sprite.

The status bar (rows 136–199) goes through the same compositor, but only when a value it shows has changed. On the ESP32-C6 it is presented on its own, so the following frame's dirty rectangle covers only the 3D view. Static pages (title, story, ending) are redrawn only when they change.

### Colour

The ESP32-C6 display keeps an 8-bit framebuffer with a 6x6x6 colour cube, and quantises every RGB565 colour it receives with `component * 5 / max`. The cartridge's palette has one entry per cube colour. Each component is the smallest value that quantises to the intended level: 0, 7, 13, 19, 25, 31 for 5-bit channels and 0, 13, 26, 38, 51, 63 for green. Hardware therefore shows exactly the intended cube colour, and QEMU (which keeps RGB565) shows almost the same colour. All art is authored as cube indices (`C(r, g, b)` in `tools/gen_assets.py`).

`shade_tab[6][216]` holds five darkening levels (fog, flicker, y-side walls, dimmed banners) and one flash level (hit feedback). A pixel costs one table lookup.

### Why strips

The cartridge has no room for a 320x200 buffer (64,000 bytes). A 4-row strip is 1,280 bytes. A frame is 34 strips for the view plus 16 for the status bar when it changes, so each frame makes at most 50 ABI calls.

## 4. Memory budget (measured)

`scripts/build.sh` reports `code=53404 mem=63596 audio=588` for version 1.1. The largest items:

| Item | Bytes | Notes |
|---|---:|---|
| code (.text) | ~29,000 | all modules, `-Os` |
| sprite pixels | 8,172 | 33 frames, 4 bpp, up to 32x32 |
| texture patterns | 2,624 | 27 patterns, 16x16: 13 at 2 bpp, 14 at 4 bpp |
| level and page text | 3,170 | one NUL-separated blob, strings shared |
| level maps | 1,036 | run-length coded, 12 levels |
| page items | 830 | 83 drawing items (`pages/pages.txt`) |
| level definitions + spawns | 786 | |
| palettes | 888 | 37 sprite palettes x 16, 37 texture palettes x 8 |
| font, sine, skyline, blues patterns, sfx, boss table | ~2,000 | |
| string literals | ~1,800 | in-game messages |
| shade tables + RGB565 palette | 1,728 | bss, built at init |
| strip buffer | 1,280 | bss |
| per-ray arrays | ~2,100 | bss |
| object pools | ~1,900 | 20 enemies, 16 projectiles, 9 cats, 14 pickups, 6 puddles, 16 props |
| map | 640 | bss; `tools/gen_levels.py` enforces w*h <= 640 |
| **cartridge RAM** | **63,596** | 1,940 bytes free |
| **load image** | **~54,090** | header + code/data + AUDIO block, about 460 bytes under the QEMU limit |

The Chicago blues soundtrack, the finale and Ralph cost about 4.4 KB of code and data. The space came from these savings: run-length coded maps, the full-screen pages turned from code into data, 2-bpp patterns where four colours suffice, 8-entry texture palettes, a 65-step sine table with interpolation, no "dead" sprite frames (a dead bug is its attack frame, flattened and darkened), a shorter noise loop, a redundant per-ray skyline cache removed, and pools sized to what the levels need. Earlier savings: per-ray texture offsets instead of pointers, packed level strings, 4-row strips, 16-bit texture steps, one queen frame, and a compact op record.

## 5. Performance

`tests/run_tests.sh cost` replays 60 frames per level and counts blits, blitted pixels and inner-loop steps (the `CC_COUNT` hook). With the SpaceBeltMadness cost figures for the board (0.62 µs per pixel sent, about 12 µs per blit call), plus rough composition estimates:

| Part | Per frame |
|---|---:|
| blitted pixels (view) | ~43,900 |
| blit calls | ~34 |
| SPI transfer of the view | ~27 ms |
| composition in cartridge code | ~5–7 ms (estimate) |
| firmware indexed blit (216-colour palette mapping + pixel copy) | ~5 ms (estimate) |

That is about 35–40 ms, or 25–30 fps. The frame is SPI-bound: the view size is what matters. The figure is an estimate from the host model and has **not yet been measured on hardware**. PRG32's performance-metrics firmware (`CONFIG_PRG32_METRICS_ENABLE`) is the tool to confirm it. If needed, the first knobs are `VIEW_H` (fewer rows sent) and `STRIP_H` (fewer, larger blits, at 320 bytes of RAM per extra row).

QEMU presents only when the emulated panel refreshes, so the QEMU build (`CC_TARGET_QEMU`) sends one present per frame instead of presenting the status bar separately.

## 6. Game systems

### Fed

Fed has health 100 and a collision radius of a quarter tile. She walks at 0.12 tiles per tick (0.6x while shielding) and turns at 20/1024 of a turn per tick. Hits grant 8 ticks of immunity to bites, spit and shocks, so a volley or a crowd hurts but cannot kill her in a single frame. Roof holes send her back to the last safe spot for 12 damage.

### Feline shield

`pl.shield` ranges from 0 to `shield_max()` = 40 + 10 x cats (at most 100). Holding B drains 1 per tick (half with yarn). A blocked hit costs 8 (spit), 10 (brick), 15 (portal surge) or the damage + 6 (bites). When the cost exceeds the remaining power the shield breaks and half the damage gets through. After release it regrows by 1 every 2 ticks following a 15-tick delay, and it must reach 30 to work again once broken. It never blocks floor puddles or falls.

### Cats

`cats_max` is the armada size (1..9, starting at 4) and `cats_out` counts cats in the field. `cats_ready()` = `cats_max - cats_out`. Sending a cat costs a 7-tick cooldown (14 while poisoned). The cat homes on the nearest enemy inside a cone of roughly ±14° around the crosshair that is in line of sight, or flies straight for 40 ticks. It deals 12 damage (8 while poisoned), times the target's armour, doubled on exposed targets. It mauls for 8 ticks and then returns. Knockout: 25% when the target is a beetle or elite mid-attack, or a boss mid-pattern. Plain roaches never knock cats out, so level 1 cannot cost you cats. Poison hits spook one cat in four. Dazed cats are pickups, and all of them come home when the boss falls. The armada never drops below one cat, and the tests check that no count goes negative.

### Enemies

| Type | HP | Speed | Bite | Armour | Special |
|---|---:|---:|---:|---|---|
| roach | 20 | 12 | 4 | full | |
| runner | 16 | 22 | 3 | full | |
| spitter | 22 | 10 | 4 | full | spit (8), cooldown 70+ |
| beetle | 45 | 8 | 12 | half | charge; bouncing off the shield exposes its belly |
| armored beetle | 60 | 9 | 14 | quarter | charge |
| elite | 34 | 20 | 7 | full | fast spit, cooldown 50+ |
| winged roach | 18 | 18 | 4 | full | hovers |

Speeds are Q8 tiles per tick. AI checks line of sight on waking and before biting, and steers straight at Fed. When blocked it slides along walls, and when stuck it sidesteps at a right angle for 10–17 ticks. Enemies push each other apart. The design brief allowed eight sprite directions if space permitted. To stay inside the budget, enemies have front-facing frames only, mirrored on alternate walk frames. This is the reduction the brief anticipated.

### Waves, doors and bosses

Wave-1 enemies are lowercase letters in the map and wave-2 enemies uppercase. A third wave reuses the wave-1 points with upgraded types (roach→runner, runner/spitter→elite, beetle→armored). Doors open when wave 1 is cleared, and `tools/gen_levels.py` rejects maps where a wave-1 enemy sits behind a door. After the last wave comes ROOM_CLEARED (a 2.5 s siren), then the boss drops in from above. The boss framework is described in `src/boss.c`. Each boss combines patterns from CHARGE, RAPID, BURST, SUMMON, SHELL and JUMP, and adds more below 50% health:

| Boss | Level | Patterns (phase 2 adds) |
|---|---|---|
| Lawn Roach | 1 | charge (summon) |
| Grill Beetle | 2 | shell, charge (burst) |
| Crumb Baron | 3 | summon, rapid (burst) |
| Soot Sweeper | 4 | jump, rapid (summon) |
| Gutter Gargoyle | 5 | burst, charge (rapid) |
| Dust Matriarch | 6 | summon, shell (burst) |
| Bathtub Behemoth | 7 | charge, rapid (jump) |
| Librarian Beetle | 8 | shell, burst (summon) |
| Butler Roach | 9 | jump, summon (rapid) |
| Couch Colossus | 10 | charge, summon, burst (shell, jump, rapid) |
| Furnace Warden | 11 | burst, charge (shell, rapid) |
| Cockroach Queen + General Chitin | 12 | summon, burst, shell / charge, rapid, jump (rapid / burst) |

When the last boss falls, `on_boss_defeated()` runs exactly once. It recovers the dazed cats, drops the leaderless minions and opens the exit. On the portal level it starts the finale instead.

### The finale (spoilers)

Nothing before the last fight mentions Ralph: not the title, the story, the level texts, the HUD or the store description. After the invasion leaders fall, `ST_FINAL_PORTAL` runs a short sequence of phases (`finale` in `src/game.c`):

| Phase | What happens | Music |
|---|---|---|
| REVEAL | The portal destabilises. Fed turns towards what its machinery was hiding: Ralph, her husband, held in a containment field. "Ralph?!" "Fed!" "...I knew this basement was trouble." | the band stops; a slow guitar phrase and a harmonica answer |
| NODES | Three containment nodes around Ralph become vulnerable. Portal surges and reinforcements keep coming. The field flickers faster as nodes break. | rescue blues: the theme returns |
| PROTECT | Ralph runs to Fed, then walks back to the entrance with her (scripted and straight-line; he clambers over the machinery). The portal fires bolts at him. A hit stuns him, unless Fed holds the shield within about 1.5 tiles of him. | rescue blues |
| ANCHORS | Only now do the four anchors unseal. Surges and reinforcements come faster, and bolts target Fed. | finale crescendo: one layer per broken anchor (drums, bass, organ, lead) |
| STABILIZER | The portal is collapsing but a stabilizer inside holds it open. A sends the whole armada at once. It takes all of them. | full crescendo, most distorted |
| COLLAPSE | The cats race home. The portal closes only when the last cat is back, and that instant is silence. Pressing A no longer sends cats. | silence, held, then the victory blues on the ending |

The anchors cannot be broken while Ralph is trapped: sealing the portal first would leave him on the other side. The ending shows the three of them in the ruined basement ("What happened?" "Cockroaches." "...right." Meow.), then the final screen with Fed, Ralph, the armada and the sealed remains of the portal: HUMANITY SAVED, RALPH RESCUED, THE PORTAL IS SEALED ...FOR GOOD, and after a pause THE CATS WANT DINNER. Ralph's pixel art follows a reference photograph: short dark hair, thin black rectangular glasses, a round face with a wry smile, a white polo shirt and a smartwatch.

### Checkpoints

Entering a level saves Fed's health (at least 60) and the armada size. Retrying restores them and reloads the level from its data.

## 7. Data formats

**Levels** (`levels/NN_name.txt`) are a header of `key: value` lines plus an ASCII map. The legend is at the top of `tools/gen_levels.py`. The compiler checks the map is rectangular and closed, has exactly one start, one boss spawn and one exit, and that every item, enemy and objective is reachable. It emits pointer-free tables, with strings stored as offsets into one `level_text[]` blob. New levels need no engine code.

**Pages** (`pages/pages.txt`) describe the title, help, story, ending and game-over screens as a short drawing script (text, sprites, rows of cats, dialogue boxes, prompts, each optionally visible only in a tick window). The level compiler turns them into `page_items[]` (10 bytes each) plus strings in the shared text blob, and `page_ops()` in `hud.c` draws them. This takes far less space than a call per item.

**Maps** are run-length coded: one byte per run, `(length - 1) << 4 | tile`.

**Art** is procedural. A texture is a 16x16 pattern (2 bpp when it uses at most four colours, otherwise 4 bpp) plus an 8-colour palette, so one pattern serves several textures (brick, soot and parapet share one). Sprites are 4 bpp with colour 0 transparent. Palette swaps make the enemy variants and bosses: roach slots 10–11 are the wings, which turn translucent blue for the winged roach. Props map a sprite and palette to a world size.

## 8. Audio

### What PRG32 offers

The PRG32 audio runtime mixes eight voices at 22,050 Hz. Each voice plays either a looped 8-bit PCM sample (pitched per note) or a firmware synth voice (triangle, saw, pulse or noise, with an ADSR envelope and a resonant low-pass filter). Per-channel volume and pan can change at run time, and effects and music share the mixer. The firmware tracker spends one tick per event and cannot play simultaneous notes, so the cartridge sequences everything itself at the 30 Hz logic rate from short reusable patterns. No PCM tracks are stored.

The AUDIO block holds four PCM waves tuned to C4 (84-sample pulse, saw and triangle, and a 160-sample noise loop) and three synth instruments (bass, kick, boom). Synth instruments are encoded for the bit layout `prg32_audio_synth_start()` actually decodes: wave 1:0, resonance 6:4 (kept at 3 or below), cutoff 9:7, pulse width 13:10.

### The blues band

The soundtrack is one original 12-bar blues in A (I-I-I-I, IV-IV, I-I, V-IV, I-V), played in swung eighths: each beat is a long and a short step (2/3 and 1/3 of the beat). There are five parts:

| Voice | Part |
|---|---|
| 0 | bass: sparse root-fifth, walking quarters, boogie-woogie eighths, heavy low register, or "doom" (flat second and tritone) |
| 1 | lead: harmonica (pulse) or guitar (saw) riffs from the A blues scale. Calls sound on even bars, answers on odd bars, and every riff follows the chord root. |
| 2 | drums: kick, snare and hat from the synth kick and the noise wave, in light, shuffle, driving or displaced patterns |
| 3 | organ stabs on the third and the flat seventh of each chord, on the off-beats |
| 4 | portal noises (distorted moods only) |

Voice 5 carries the portal hum, and voices 6–7 the sound effects.

A mood is one row of `moods[]` in `src/audio.c`: a tempo plus a choice of bass, drum, call and answer patterns, organ on or off, and the lead instrument:

| Mood | Used for | Character |
|---|---|---|
| EXPLORE | calm rooms, title, level cards | 100 bpm, root-fifth bass, brushes, an occasional harmonica phrase |
| DANGER | enemies have noticed Fed | 120 bpm, walking bass, full shuffle, a repeating tension riff |
| COMBAT | enemies close in, or Fed was just hit (lasts 4 s) | boogie bass, shuffle, fast runs answered by the harmonica, organ |
| BOSS | every boss fight | 150 bpm, heavy low bass, driving drums, the boss riff |
| PORTAL | calm moments in the basement and the portal chamber | doom bass, displaced drums, alien intervals |
| SAD | the reveal, game over | 75 bpm, no bass, slow guitar and a harmonica answer |
| RESCUE | freeing Ralph | walking bass, call and answer: the theme returning |
| CRESCENDO | the anchors and the stabilizer | layered up as anchors fall |
| VICTORY | the ending and credits | medium shuffle, boogie bass, a playful major-sixth melody, a meow every four bars |

The game sets the mood every tick (`music_mood()` in `src/game.c`). The sequencer applies a change only at the next bar line and keeps its position in the 12-bar form, so the band never restarts. The exceptions are silence, the sad phrase and the first bars after a silence, which start at once. `mus_warp` corrupts the blues near the portal: at level 1 some lead notes slip a semitone or a tritone; at 2 drum hits are pushed a step late and the portal groans every other bar; at 3 the tempo also rises. The basement plays at warp 1 and the portal chamber at 2. The leaders' fight starts clean and becomes more distorted as their combined health drops (above 66%: 0; 33–66%: 2; below 33%: 3). `mus_layers` builds the finale crescendo.

Effects are 4-byte steps (instrument, note, volume, ticks) on voices 6–7. Roach skittering is the lowest priority and never interrupts another effect.

## 9. Testing

`tests/run_tests.sh` compiles `src/main.c` natively with AddressSanitizer and UndefinedBehaviorSanitizer against stub PRG32 calls, then runs:

| Requirement | Test |
|---|---|
| Fed cannot walk through walls | `test_walls`: every level, four headings, walking and strafing |
| enemies cannot damage Fed through walls | `test_no_damage_through_walls`: 30 s of roach, spitter and beetle behind a wall |
| poison collides correctly | `test_poison`: stops at walls, hits Fed, poisons, is blocked by the shield |
| the cat attack hits the intended target | `test_cat_targeting`: crosshair target hit, bystander untouched, cat returns |
| the shield blocks and is not permanent | `test_shield`: no damage while up, it runs out, it regrows |
| cat counts never go negative | `test_cat_count`: 2,000 random losses, launches and knockouts |
| boss death opens the exit exactly once | `test_boss_once` |
| exits cannot activate prematurely | `test_exit_locked`: before and during the boss fight |
| death and restart reset the level | `test_restart` |
| progression reaches every level, the portal and the ending | `test_progression` |
| portal sequence | `test_portal_needs_leaders`; `test_finale` (inside `test_progression`) plays the whole finale: reveal, nodes, Ralph's escape, anchors sealed until he is safe, the stabilizer surviving a single hit, the full volley, the cats' return, silence, then the victory blues |
| adaptive music | `test_music`: moods for calm, alert and combat; changes only on bar lines; the leaders' health phases; the sad drop at the reveal |
| cats alone can win | `test_cats_can_win`: a bot clears all 12 levels, finale included, using only A (it goes back for dazed cats, as a player would) |
| rendering | `test_render_smoke`, plus the soak's frequent draws under the sanitizers |
| robustness | `test_soak`: random input on every level |

The tests are deterministic: `CC_SEED=n build/test_game` replays them with another random seed. Seeds 1, 777, 9001, 12345 and 424242 all pass.

`tests/run_tests.sh shots` saves every screen as PNG from the host framebuffer, which is pixel-identical to the cartridge's strips. `scripts/qemu_preview.py` runs the real cartridge on the PRG32 QEMU firmware with scripted input and saves framebuffer dumps and the audio stream.

## 10. Platform notes

These notes come from SpaceBeltMadness and were confirmed here:

1. The host builder still checks portable cartridges against 32 KiB, while the firmware defaults to 64 KiB. `tools/prg32_cli_64.py` raises only the host-side check.
2. The synth bit layout differs from the `PRG32_AUDIO_SYNTH_*` macros (see section 8).
3. The QEMU firmware needs an SDL display (headless via `SDL_VIDEODRIVER=offscreen`) for presents to complete.
4. **Load size.** Before starting a cartridge, the firmware copies its header, code/data and AUDIO block into one temporary heap block. In the QEMU firmware a 55.5 KB image failed with "out of memory reading cartridge", while 54.5 KB loaded. That limit is tighter than the 64 KiB RAM window, and bss does not count towards it.
5. The firmware's fast `memcpy` path for 8-bit sprites requires a palette that maps onto itself. A 216-colour cube palette does not, so strips take the per-pixel path, which is still cheap next to the SPI transfer.
