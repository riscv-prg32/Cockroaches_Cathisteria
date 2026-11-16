# Cockroaches Cathisteria: design and technical notes

This document explains how the cartridge is built and why. The README covers how to play.

## 1. Constraints

The PRG32 runtime shapes every decision:

| Constraint | Value | Consequence |
|---|---|---|
| Cartridge RAM (code + data + bss) | 131,072 bytes (PRG32 128 KiB profile) | room for 32x32 textures and a 48x48 portrait; still no 64,000-byte framebuffer of our own |
| Store image (code + audio + metadata) | 131,072 bytes (one cartridge slot) | palettised store art, a 168-byte AUDIO block |
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
| `audio.c` | SID-like sound effects with stereo position, the eight-voice blues band, portal ambience |
| `raycast.c` | palette and shading, DDA raycaster, floor/ceiling casting, billboards, 2D ops, strip compositor |
| `collision.c` | tile solidity, sliding movement, line of sight |
| `level.c` | level loading, props/pickups/anchors from tiles, waves, doors, checkpoints |
| `player.c` | movement, strafing (B + LEFT/RIGHT), feline shield, damage rules, pickups |
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

### Controls

Play uses only the digital joystick, A and B: in the world states `game_tick()` masks the input with `PLAY_BUTTONS` (`src/config.h`), so START, SELECT and the C/D/L/R buttons of newer controllers do nothing, and there is no pause. Holding B raises the shield and turns LEFT/RIGHT into a sidestep, so strafing is a defensive move. START is read only on the title, game-over and ending screens; the story pages, level cards, death screen and level results answer to A and B. `test_controls` checks all of this.

## 3. Rendering

### Pipeline

Each frame in a world state does the following:

1. **Cast** 160 rays (one per 2-pixel column) through the map with an integer DDA. Positions are Q8 (256 = one tile) and directions Q12. For each ray it stores the perpendicular distance (the z-buffer), the wall's top and bottom, the texel column (32 per tile), the palette, a shade, and the skyline height for that heading.
2. **Project** every enemy, cat, projectile, pickup, puddle and prop into camera space (depth and lateral offset against the view direction), scale it by `FOCAL` horizontally and `WALL_S` vertically, and sort far to near. At most 40 billboards are drawn.
   A billboard closer than `NEAR_DEPTH` (1.2 tiles) is projected as if it stood at that distance, with its lateral offset scaled by the same factor so it stays on its side of the view, and no billboard is taller than `MAX_SPRITE_H` (168 px, about the height of the view). An enemy biting Fed therefore fills most of the view but stays a readable sprite, instead of a few texels blown up into screen-sized blocks as in version 1. The z-buffer test still uses the true depth (`test_point_blank`).
3. **Compose** the view in 8-row strips. For every row the floor or ceiling is floor-cast: the row distance is precomputed, and the world position steps linearly across the 160 columns. Wall texels come from the per-ray data. Sky rows use a gradient, stars and a building silhouette. Sprites are then drawn column by column, hidden behind nearer walls, followed by the 2D overlays (text, rectangles, dimming, scaled sprites) from a small retained op list.
4. **Blit** each finished strip with `prg32_sprite_draw_indexed()` as an 8-bit indexed sprite.

The status bar (rows 136–199) goes through the same compositor, but only when a value it shows has changed. On the ESP32-C6 it is presented on its own, so the following frame's dirty rectangle covers only the 3D view. Static pages (title, story, ending) are redrawn only when they change.

### Colour

The ESP32-C6 display keeps an 8-bit framebuffer with a 6x6x6 colour cube, and quantises every RGB565 colour it receives with `component * 5 / max`. The cartridge's palette has one entry per cube colour. Each component is the smallest value that quantises to the intended level: 0, 7, 13, 19, 25, 31 for 5-bit channels and 0, 13, 26, 38, 51, 63 for green. Hardware therefore shows exactly the intended cube colour, and QEMU (which keeps RGB565) shows almost the same colour. All art is authored as cube indices (`C(r, g, b)` in `tools/gen_assets.py`).

`shade_tab[6][216]` holds five darkening levels (fog, flicker, y-side walls, dimmed banners) and one flash level (hit feedback). A pixel costs one table lookup.

### Why strips

A 320x200 buffer would take 64,000 of the 131,072 bytes of cartridge RAM. An 8-row strip is 2,560 bytes. A frame is 17 strips for the view plus 8 for the status bar when it changes, so each frame makes at most 25 blit calls (34 + 16 with the 4-row strips of version 1).

## 4. Memory budget (measured)

`scripts/build.sh` reports `code=64664 mem=76140 audio=168` for version 2.0.1 (`--cart-ram-kib 128`). The largest items:

| Item | Bytes | Notes |
|---|---:|---|
| code (.text) | ~29,500 | all modules, `-Os` |
| texture patterns | 10,496 | 27 patterns, 32x32: 2 or 4 bpp |
| sprite pixels | 10,764 | 33 frames, 4 bpp, up to 48x48 (Fed's portrait) |
| level and page text | ~3,200 | one NUL-separated blob, strings shared |
| level maps | 1,036 | run-length coded, 12 levels |
| page items | ~830 | drawing items (`pages/pages.txt`) |
| level definitions + spawns | 786 | |
| palettes | 1,184 | 74 palettes |
| font, sine, skyline, blues patterns, sfx, boss table | ~2,200 | |
| string literals | ~1,800 | in-game messages |
| shade tables + RGB565 palette | 1,728 | bss, built at init |
| strip buffer | 2,560 | bss, 8 rows |
| per-ray arrays | ~2,100 | bss |
| object pools | ~1,900 | 20 enemies, 16 projectiles, 9 cats, 14 pickups, 6 puddles, 16 props |
| map | 640 | bss; `tools/gen_levels.py` enforces w*h <= 640 |
| **cartridge RAM** | **76,140** | of 131,072 (54,932 free) |
| **store image** | **74,719** | of 131,072, with icon, screenshot and colophon |

Version 1 had to fit 64 KiB and a QEMU load image of about 54.5 KB (the firmware then copied the whole image into one heap block). Current PRG32 firmware copies the payload straight from flash into the cartridge window, so that limit is gone; the remaining limits are the two in the table. The 2.0 growth is mostly art: textures went from 2.6 KB to 10.5 KB and Fed's portrait from three 24x24 to three 48x48 frames, while the AUDIO block shrank from 588 to 168 bytes (no PCM waves).

## 5. Performance

`tests/run_tests.sh cost` replays 60 frames per level and counts blits, blitted pixels and inner-loop steps (the `CC_COUNT` hook). With the SpaceBeltMadness cost figures for the board (0.62 µs per pixel sent, about 12 µs per blit call), plus rough composition estimates:

| Part | Per frame |
|---|---:|
| blitted pixels (view) | ~43,900 |
| blit calls | ~17 |
| SPI transfer of the view | ~27 ms |
| composition in cartridge code | ~5–7 ms (estimate) |
| firmware indexed blit (216-colour palette mapping + pixel copy) | ~5 ms (estimate) |

That is about 35–40 ms, or 25–30 fps. The frame is SPI-bound: the view size is what matters. The figure is an estimate from the host model.

**Measuring on a board.** `scripts/measure_hw.sh --slot cartN [--url http://board]` builds a `CC_MEASURE` cartridge (with `CC_TEST_GOD`), uploads it to that slot (overwriting it), starts it, waits, and reads the result from the PRG32 performance broker (`prg32 esp32c6 performance`), printing frames per second and mean, p95 and max frame time per level, with the draw and present shares. The JSON is kept in `build/measure/`. In this build the game plays each of the 12 levels by itself for 240 frames (walking, turning, sending cats, so enemies, cats and the band are all active). It records one broker case per level (`L01`..`L12`), timing update, draw, present and the whole frame with `prg32_perf_now_us()`, and shows the average and the worst level in the bands when it is done. In QEMU (`scripts/qemu_preview.py --script measure`) the run completes and reports 30.2 fps on every level, which only shows that the path works, because QEMU paces frames. Hardware numbers have not been recorded yet. On a `pcb1` board use `--url`: its joystick DOWN is wired to GPIO13, the ESP32-C6's native USB D+, so once the firmware starts, USB uploads (and flashing over that port) fail with `PRG32 device did not answer protocol request 1`. If needed, the first knob is `VIEW_H` (fewer rows sent). `STRIP_H` is already 8 (fewer, larger blits, at 320 bytes of RAM per row). The 32x32 textures cost no extra time: each texel is still one lookup.

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

**Art** is procedural. A texture is a 32x32 pattern (2 bpp when it uses at most four colours, otherwise 4 bpp) plus an 8-colour palette. Each pattern is designed at 16x16, enlarged with Scale2x (EPX), which keeps edges crisp and rounds diagonals, then a per-pattern pass adds full-resolution detail: lit and shaded brick and board edges, wood grain, re-grained noise, a smooth portal swirl. One pattern serves several textures (brick, soot and parapet share one). Sprites are 4 bpp with colour 0 transparent. Palette swaps make the enemy variants and bosses: roach slots 10–11 are the wings, which turn translucent blue for the winged roach. Props map a sprite and palette to a world size.

## 8. Audio

### What PRG32 offers

The PRG32 audio runtime mixes eight voices at 22,050 Hz. Each voice plays either a looped 8-bit PCM sample or a firmware synth voice: a SID-like oscillator (triangle, saw, pulse with 16 widths, or LFSR noise whose rate follows the note), an ADSR envelope and a resonant state-variable low-pass filter (16 cutoffs, 4 resonances). Per-note volume and pan are set with `prg32_audio_note_on_pan()`. The firmware tracker cannot change a note's pitch, so the cartridge sequences everything itself at the 30 Hz logic rate from short reusable patterns.

The AUDIO block holds sixteen synth instruments and no samples (168 bytes). They are written by `tools/gen_assets.py` with the documented bit layout that current firmware decodes (`PRG32_AUDIO_SYNTH_ID`): resonance 11:10, cutoff 9:6, pulse width 5:2, waveform 1:0. Version 1 encoded its three synth instruments for the old firmware decoder (resonance 6:4, cutoff 9:7, pulse width 13:10); after PRG32 fixed its decoder those ids decoded as different, mostly wrong instruments, so 2.0 re-encodes everything.

| Instrument | Oscillator | Use |
|---|---|---|
| BASS | pulse 6/17, cutoff 9, resonance 2 | plucky bass |
| KICK | triangle, fast decay | kick, thuds |
| SNARE, HAT | noise, bright filter, short decays | snare (note 100), hi-hat (note 124) |
| HARP | narrow pulse, soft attack | harmonica lead, meows |
| GUITAR | saw, cutoff 12, resonance 2 | guitar lead |
| ORGAN | square | organ stabs |
| BELL, CHIME | triangle / narrow pulse | victory lead, pickups, shield |
| BOOM, GROWL, HISS | low noise / resonant saw / noise | explosions, hurt and death, spit and skitter |
| ZAP, SWEEP | resonant pulse / sustained triangle | UI, alarms, pitch slides |
| HUM, ECHO | resonant low saw / soft pulse | portal drone, the lead's echo |

### The blues band

The soundtrack is one original 12-bar blues in A (I-I-I-I, IV-IV, I-I, V-IV, I-V), played in swung eighths: each beat is a long and a short step (2/3 and 1/3 of the beat).

| Voice | Part |
|---|---|
| 0 | bass: sparse root-fifth, walking quarters, boogie-woogie eighths, heavy low register, or "doom" (flat second and tritone) |
| 1 | lead: harmonica, guitar or bell riffs from the A blues scale. Calls sound on even bars, answers on odd bars, and every riff follows the chord root. |
| 2 | kick |
| 3 | organ stabs on the third and the flat seventh of each chord, on the off-beats, panned left |
| 4 | snare and hi-hat (panned slightly apart); the last beats of bar 12 are a fill into the next chorus |
| 5 | the portal hum on portal levels; elsewhere the lead's echo, one swung step late, quieter and panned right |

Voices 6–7 carry the sound effects (and the portal's groans, as a low-priority effect).

A mood is one row of `moods[]` in `src/audio.c`: a tempo plus a choice of bass, drum, call and answer patterns, organ on or off, and the lead instrument:

| Mood | Used for | Character |
|---|---|---|
| EXPLORE | calm rooms, title, level cards | 100 bpm, root-fifth bass, brushes, an occasional harmonica phrase |
| DANGER | enemies have noticed Fed | 120 bpm, walking bass, full shuffle, a repeating guitar tension riff |
| COMBAT | enemies close in, or Fed was just hit (lasts 4 s) | boogie bass, shuffle, fast guitar runs answered by the harmonica, organ |
| BOSS | every boss fight | 150 bpm, heavy low bass, driving drums, the boss riff |
| PORTAL | calm moments in the basement and the portal chamber | doom bass, displaced drums, alien intervals on the bell |
| SAD | the reveal, game over | 75 bpm, no bass, slow guitar and a harmonica answer |
| RESCUE | freeing Ralph | walking bass, call and answer: the theme returning |
| CRESCENDO | the anchors and the stabilizer | layered up as anchors fall |
| VICTORY | the ending and credits | medium shuffle, boogie bass, a playful major-sixth melody on the bell, a meow every four bars |

The game sets the mood every tick (`music_mood()` in `src/game.c`). The sequencer applies a change only at the next bar line and keeps its position in the 12-bar form, so the band never restarts. The exceptions are silence, the sad phrase and the first bars after a silence, which start at once. `mus_warp` corrupts the blues near the portal: at level 1 some lead notes slip a semitone or a tritone; at 2 drum hits are pushed a step late and the portal groans every other bar; at 3 the tempo also rises. The basement plays at warp 1 and the portal chamber at 2. The leaders' fight starts clean and becomes more distorted as their combined health drops (above 66%: 0; 33–66%: 2; below 33%: 3). `mus_layers` builds the finale crescendo.

### Effects

Effects are 4-byte steps (instrument, note, volume, ticks) on voices 6–7. One-tick steps on the sustained SWEEP instrument give SID-style pitch slides (the cat's launch, the shield, the fall, boings). `sfx_at(id, x, y)` places an effect in the world: the pan follows the source's lateral offset in Fed's view and the volume falls with distance (`test_sfx_pan`); enemies, bosses and cats use it, while Fed's own sounds stay centred. Roach skittering and portal groans are the lowest priority and never interrupt another effect.

### Level

The firmware mixes eight voices without a limiter, so the cartridge sets the level. In 2.0 the band sat at −25.6 LUFS (integrated, QEMU stereo capture of the Store preview), below the other PRG32 games measured the same way (−18.8 to −28.5 LUFS), and the effects masked it. Since 2.0.1 every music note goes through `mvol()` in `src/audio.c` (+40%, about +3 dB, capped at 255), while effects keep their volumes. The new preview measures −21.3 LUFS with a −10.2 dBFS true peak, so there is headroom left and no clipping. These are digital levels. How loud it sounds from the board's speaker depends on the amplifier and the firmware's master volume, and has to be checked by ear.

## 9. Testing

`tests/run_tests.sh` compiles `src/main.c` natively with AddressSanitizer and UndefinedBehaviorSanitizer against stub PRG32 calls, then runs:

| Requirement | Test |
|---|---|
| Fed cannot walk through walls | `test_walls`: every level, four headings, walking and strafing |
| controls | `test_controls`: START and the extra buttons do nothing in play, B + LEFT strafes with the shield up, START is ignored on level cards and the death screen, and works on the title and game over |
| positional sound | `test_sfx_pan`: effects pan towards their source, fade with distance, and Fed's own stay centred |
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
| point-blank enemies stay readable | `test_point_blank`: a roach and a beetle 0.47 tiles away, left, centre and right: height capped, width bounded, still on their side |
| rendering | `test_render_smoke`, plus the soak's frequent draws under the sanitizers |
| robustness | `test_soak`: random input on every level |

The tests are deterministic: `CC_SEED=n build/test_game` replays them with another random seed. Seeds 1, 777, 9001, 12345 and 424242 all pass.

`tests/run_tests.sh shots` saves every screen as PNG from the host framebuffer, which is pixel-identical to the cartridge's strips. `scripts/qemu_preview.py` runs the real cartridge on the PRG32 QEMU firmware with scripted input (stereo audio switched on in the firmware's setup menu first) and saves framebuffer dumps, the audio stream and, with `--video`, an MP4.

The Store preview (`assets/preview.mp4`, 60 s, 640x400) is cut from two QEMU recordings: 34 s of the release cartridge (title, story, the front yard) with the `preview` script, then 26 s of the portal level, enemies at point blank included, with the `portal` script, recorded from a build whose wrapper sets `CC_START_LEVEL 11` and `CC_TEST_GOD 1`. Every frame and sample comes from QEMU.

## 10. Platform notes

1. **Profiles.** The cartridge is built with `--cart-ram-kib 128` for PRG32's large profile, the default on ESP32-C6 and QEMU firmware. Its 76 KB of RAM do not fit firmware built with the 32 or 64 KiB profiles, which reject it at upload. (Version 1 needed a CLI adapter to raise the host builder's 32 KiB check; current PRG32 defaults to 128 KiB, so the adapter is gone.)
2. **Synth layout.** Current firmware decodes synth ids exactly as `PRG32_AUDIO_SYNTH_ID` encodes them (see section 8); version 1's hand-encoded ids targeted the old decoder.
3. The QEMU firmware needs an SDL display (headless via `SDL_VIDEODRIVER=offscreen`) for presents to complete. In current firmware `g_fb` is a pointer to the panel framebuffer; `scripts/qemu_preview.py` follows it.
4. **QEMU keys.** Since PRG32 added the C/D/L/R buttons, the QEMU UART keyboard maps A to `h` and B to `j` (it was `j`/`k`); the preview script uses the new map.
5. The firmware's fast `memcpy` path for 8-bit sprites requires a palette that maps onto itself. A 216-colour cube palette does not, so strips take the per-pixel path, which is still cheap next to the SPI transfer.
