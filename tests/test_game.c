/*
 * Host tests for Cockroaches Cathisteria.
 *
 * The whole cartridge (src/main.c) is compiled natively together with
 * stubbed PRG32 calls, then driven tick by tick. The stubs keep a 320x240
 * RGB565 framebuffer, so the same program can also save screenshots, and
 * they count ABI calls and blitted pixels for the performance model in
 * docs/design.md.
 *
 *   tests/run_tests.sh                 all logic tests + soak
 *   build/test_game shots DIR          screenshots (PPM) of every screen
 *   build/test_game cost               per-level draw cost estimate
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_HOST 1
static long cost_ops;
#define CC_COUNT(n) (cost_ops += (n))
#include "main.c"

/* ---- PRG32 stubs --------------------------------------------------------- */

static uint32_t sim_ms, sim_input;
static uint16_t fb[240][320];
static long st_blits, st_blit_px, st_presents, st_calls;
static char band_txt[2][48];

uint32_t prg32_ticks_ms(void) { return sim_ms; }
uint32_t prg32_input_read(void) { return sim_input; }
void prg32_audio_note_on(uint8_t c, uint8_t i, uint8_t n, uint8_t v) { (void)c; (void)i; (void)n; (void)v; st_calls++; }
void prg32_audio_note_off(uint8_t c) { (void)c; st_calls++; }
void prg32_band_set_mode(uint8_t b, prg32_band_mode_t m) { (void)b; (void)m; }
void prg32_band_set_colors(uint8_t b, uint16_t f, uint16_t g) { (void)b; (void)f; (void)g; }
void prg32_band_set_text(uint8_t b, const char *t) {
    st_calls++;
    strncpy(band_txt[b & 1], t, 47);
}
void prg32_gfx_present(void) { st_presents++; }
void prg32_sprite_draw_indexed(int x, int y, const prg32_indexed_sprite_t *s, uint32_t frame) {
    st_calls++;
    st_blits++;
    if (!s || s->bits_per_pixel != 8 || frame >= s->frame_count) {
        fprintf(stderr, "bad blit\n");
        abort();
    }
    for (int r = 0; r < s->height; ++r)
        for (int c = 0; c < s->width; ++c) {
            int px = x + c, py = y + r;
            if (px < 0 || px >= 320 || py < 0 || py >= 200) continue;
            uint8_t idx = s->pixels[r * s->width + c];
            if (idx < s->palette_count) fb[py + 20][px] = s->palette[idx];
            st_blit_px++;
        }
}

/* ---- helpers -------------------------------------------------------------- */

static int failures, checks;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        checks++;                                                               \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

static uint32_t prev_input;

static void tick(uint32_t in) {
    uint32_t pressed = in & ~prev_input;
    prev_input = in;
    game_tick(in, pressed);
}

static void ticks_n(uint32_t in, int n) {
    for (int i = 0; i < n; ++i) tick(in);
}

static void press(uint32_t button) {
    tick(0);
    tick(button);
    tick(0);
}

static void boot(void) {
    memset(fb, 0, sizeof(fb));
    rng_state = getenv("CC_SEED") ? (uint32_t)atoi(getenv("CC_SEED")) : 12345; /* tests/run_tests.sh */
    prev_input = 0;
    cockroaches_init();
}

static void start_level(int n) {
    boot();
    new_game();
    level_enter(n);
    game_set_state(ST_LEVEL_START);
    game_set_state(ST_PLAYING);
}

static void kill_all(int flags_any, int flags_none) {
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        enemy_t *e = &enemies[i];
        if (!e->active || e->state == AI_DEAD) continue;
        if (flags_any && !(e->flags & flags_any)) continue;
        if (e->flags & flags_none) continue;
        e->flags &= (uint8_t)~EF_INVULN;
        e->z = 0;
        enemy_damage(i, 30000, 0);
    }
}

/* An empty room with walls around it and nothing in it. */
static void empty_room(void) {
    start_level(0);
    level_clear_objects();
    map_w = map_h = 10;
    memset(map, 0, sizeof(map));
    for (int i = 0; i < 10; ++i) {
        map[i] = map[90 + i] = map[i * 10] = map[i * 10 + 9] = T_WALL_A;
    }
    pl.x = 2 * ONE + 128;
    pl.y = 5 * ONE + 128;
    pl.ang = 0;
    pl.health = 100;
    cats_max = 5;
    cats_reset();
}

static int count_state(int st) {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) n += enemies[i].active && enemies[i].state == st;
    return n;
}

/* Clear the current level the direct way; returns 1 on reaching the end state. */
static int clear_level(void) {
    for (int guard = 0; guard < 40 && (state == ST_PLAYING) && !exit_open; ++guard) {
        kill_all(0, EF_BOSS | EF_OBJECTIVE);
        ticks_n(0, 50);
    }
    if (state == ST_ROOM_CLEARED) ticks_n(0, 80);
    if (state != ST_BOSS) return 0;
    for (int guard = 0; guard < 20 && state == ST_BOSS; ++guard) {
        kill_all(EF_BOSS, 0);
        tick(0);
    }
    return 1;
}

/* ---- the tests ------------------------------------------------------------- */

static void test_walls(void) {
    printf("player cannot walk through walls\n");
    for (int l = 0; l < LEVEL_COUNT; ++l) {
        start_level(l);
        level_clear_objects();
        for (int d = 0; d < 4; ++d) {
            pl.ang = d * 256 + 40;
            ticks_n(PRG32_BTN_UP, 120);
            ticks_n(PRG32_BTN_UP | PRG32_BTN_SELECT | PRG32_BTN_LEFT, 60);
            CHECK(can_stand(pl.x, pl.y, PLAYER_R - 1), "level %d dir %d: Fed inside a wall at %d,%d",
                  l, d, (int)pl.x, (int)pl.y);
            CHECK(pl.x > 0 && pl.y > 0 && pl.x < map_w * ONE && pl.y < map_h * ONE, "level %d outside map", l);
        }
    }
}

static void test_no_damage_through_walls(void) {
    printf("enemies cannot damage Fed through walls\n");
    empty_room();
    for (int y = 0; y < 10; ++y) map[y * 10 + 4] = T_WALL_A; /* a full wall at x = 4 */
    int a = enemy_spawn(E_ROACH, 5 * ONE + 128, 5 * ONE + 128, 0);
    int b = enemy_spawn(E_SPITTER, 6 * ONE + 128, 4 * ONE + 128, 0);
    int c = enemy_spawn(E_BEETLE, 5 * ONE + 128, 6 * ONE + 128, 0);
    enemies[a].state = enemies[b].state = enemies[c].state = AI_CHASE;
    for (int i = 0; i < 900; ++i) {
        cats_update();
        enemies_update();
        projectiles_update();
    }
    CHECK(pl.health == 100, "health %d after 30 s behind a wall", pl.health);
    CHECK(enemies[a].x > 4 * ONE + 64, "roach walked through the wall (x=%d)", (int)enemies[a].x);
}

static void test_poison(void) {
    printf("poison collides with walls and with Fed\n");
    empty_room();
    map[5 * 10 + 4] = T_WALL_A;
    proj_spawn(PJ_SPIT, 7 * ONE, 5 * ONE + 128, pl.x, pl.y, 34, 8);
    for (int i = 0; i < 120; ++i) projectiles_update();
    CHECK(pl.health == 100, "spit passed a wall: health %d", pl.health);
    CHECK(projs[0].active == 0, "spit still alive after hitting a wall");
    map[5 * 10 + 4] = T_EMPTY;
    proj_spawn(PJ_SPIT, 7 * ONE, 5 * ONE + 128, pl.x, pl.y, 34, 8);
    for (int i = 0; i < 120; ++i) projectiles_update();
    CHECK(pl.health == 92, "spit should hit Fed for 8: health %d", pl.health);
    CHECK(pl.poison > 0, "Fed should be poisoned");
    /* the shield blocks spit */
    pl.health = 100;
    pl.iframes = 0;
    pl.shield = 100;
    pl.shield_on = 1;
    proj_spawn(PJ_SPIT, 7 * ONE, 5 * ONE + 128, pl.x, pl.y, 34, 8);
    for (int i = 0; i < 120; ++i) projectiles_update();
    CHECK(pl.health == 100, "shielded Fed took poison damage: %d", pl.health);
    CHECK(pl.shield < 100, "blocked spit should cost shield power");
}

static void test_cat_targeting(void) {
    printf("cat attack hits the intended target\n");
    empty_room();
    int a = enemy_spawn(E_ROACH, 6 * ONE + 128, 5 * ONE + 128, 0); /* straight ahead */
    int b = enemy_spawn(E_ROACH, 5 * ONE + 128, 2 * ONE + 128, 0); /* off to the left */
    CHECK(cat_find_target() == a, "crosshair target %d, expected %d", cat_find_target(), a);
    cats_launch();
    CHECK(cats_out == 1 && cats_ready() == 4, "launch: out %d ready %d", cats_out, cats_ready());
    for (int i = 0; i < 60; ++i) cats_update();
    CHECK(enemies[a].hp < enemies[a].maxhp, "target not hit (hp %d)", enemies[a].hp);
    CHECK(enemies[b].hp == enemies[b].maxhp, "bystander was hit (hp %d)", enemies[b].hp);
    for (int i = 0; i < 120; ++i) cats_update();
    CHECK(cats_out == 0 && cats_ready() == 5, "cat did not come back: out %d", cats_out);
    /* shield up: no cats can be sent */
    pl.shield_on = 1;
    pl.cat_cd = 0;
    cats_launch();
    CHECK(cats_out == 0, "cats launched while shielding");
}

static void test_shield(void) {
    printf("shield blocks damage and is not permanent\n");
    empty_room();
    int a = enemy_spawn(E_ROACH, 3 * ONE + 128 + 40, 5 * ONE + 128, 0);
    enemies[a].state = AI_CHASE;
    int blocked_ticks = 0;
    for (int i = 0; i < 300; ++i) {
        ticks++;
        player_update(PRG32_BTN_B);
        enemies_update();
        if (pl.shield_on) blocked_ticks++;
        if (pl.shield_on) CHECK(pl.health == 100, "damage through the shield at tick %d", i);
        if (pl.health < 100) break;
    }
    CHECK(blocked_ticks > 20, "shield held only %d ticks", blocked_ticks);
    CHECK(pl.shield_broken || pl.health < 100, "shield never ran out: power %d", pl.shield);
    /* it regrows when released */
    enemies[a].active = 0;
    int before = pl.shield;
    for (int i = 0; i < 120; ++i) {
        ticks++;
        player_update(0);
    }
    CHECK(pl.shield > before, "shield did not regrow (%d -> %d)", before, pl.shield);
    CHECK(pl.shield <= shield_max(), "shield above its maximum");
}

static void test_cat_count(void) {
    printf("cat count never becomes negative\n");
    empty_room();
    cats_max = 2;
    for (int i = 0; i < 2000; ++i) {
        int r = (int)(rnd() % 6);
        if (r == 0) cats_lose_one(pl.x, pl.y);
        else if (r == 1) { pl.cat_cd = 0; pl.shield_on = 0; cats_launch(); }
        else if (r == 2) {
            int e = enemy_spawn(E_ROACH, 4 * ONE, 5 * ONE, 0);
            if (e >= 0) enemies[e].state = AI_ATTACK;
        } else if (r == 3) pickups_update();
        cats_update();
        CHECK(cats_max >= 1 && cats_max <= MAX_CATS, "cats_max %d", cats_max);
        CHECK(cats_out >= 0 && cats_out <= cats_max, "cats_out %d of %d", cats_out, cats_max);
        CHECK(cats_ready() >= 0, "cats_ready %d", cats_ready());
        if (failures > 20) return;
    }
}

static void test_boss_once(void) {
    printf("boss death opens the exit exactly once\n");
    start_level(0);
    ev_boss_defeated = ev_level_complete = 0;
    CHECK(clear_level(), "could not reach the boss");
    CHECK(ev_boss_defeated == 1, "boss events %d", ev_boss_defeated);
    CHECK(exit_open && state == ST_PLAYING, "exit %d state %d", exit_open, state);
    for (int i = 0; i < MAX_ENEMIES; ++i) enemy_damage(i, 999, 1); /* hit the corpse */
    on_boss_defeated();
    ticks_n(0, 30);
    CHECK(ev_boss_defeated == 1, "boss events after extra hits %d", ev_boss_defeated);
    pl.x = exit_x;
    pl.y = exit_y;
    ticks_n(0, 5);
    CHECK(state == ST_LEVEL_COMPLETE && ev_level_complete == 1, "state %d completes %d", state, ev_level_complete);
    level_complete();
    ticks_n(0, 20);
    CHECK(ev_level_complete == 1, "level completed twice");
}

static void test_exit_locked(void) {
    printf("level exits cannot activate prematurely\n");
    start_level(0);
    ev_level_complete = 0;
    pl.x = exit_x;
    pl.y = exit_y;
    ticks_n(0, 5);
    level_complete();
    CHECK(state != ST_LEVEL_COMPLETE && ev_level_complete == 0, "exit worked before the boss");
    /* ... and not during the boss fight either */
    start_level(0);
    kill_all(0, EF_BOSS | EF_OBJECTIVE);
    ticks_n(0, 60);
    kill_all(0, EF_BOSS | EF_OBJECTIVE);
    ticks_n(0, 200);
    CHECK(state == ST_BOSS, "expected the boss fight, state %d", state);
    level_complete();
    CHECK(ev_level_complete == 0 && state == ST_BOSS, "exit worked during the boss fight");
}

static void test_restart(void) {
    printf("death and restart reset the level\n");
    start_level(2);
    int alive0 = enemies_alive(0, EF_OBJECTIVE);
    int32_t sx = pl.x, sy = pl.y;
    int cm = cats_max;
    kill_all(0, 0);
    pl.x += 3 * ONE;
    cats_max = 1;
    player_damage(500, DMG_BITE);
    CHECK(state == ST_PLAYER_DEAD, "Fed should be down, state %d", state);
    ticks_n(0, 40);
    press(PRG32_BTN_A);
    CHECK(state == ST_LEVEL_START, "restart state %d", state);
    CHECK(level == 2, "restart changed the level to %d", level);
    CHECK(pl.x == sx && pl.y == sy, "player not back at the start");
    CHECK(pl.health == checkpoint.health && pl.health >= 60, "health %d", pl.health);
    CHECK(cats_max == cm, "cats %d, expected %d", cats_max, cm);
    CHECK(enemies_alive(0, EF_OBJECTIVE) == alive0, "enemies %d, expected %d",
          enemies_alive(0, EF_OBJECTIVE), alive0);
    CHECK(!exit_open && !boss_defeated && wave == 1, "level flags not reset");
    press(PRG32_BTN_B);
    player_damage(500, DMG_BITE);
    ticks_n(0, 40);
    press(PRG32_BTN_B);
    CHECK(state == ST_GAME_OVER, "B on the death screen should give up, state %d", state);
}

/* The finale, from the leaders' fall to the ending (called on the last level). */
static void test_finale(void) {
    CHECK(state == ST_FINAL_PORTAL && finale == FIN_REVEAL, "no reveal (state %d finale %d)", state, finale);
    CHECK(anchors_left == 4 && ralph_visible && !ralph_free, "reveal setup");
    ticks_n(0, 210);
    CHECK(finale == FIN_NODES && nodes_left == 3, "nodes phase %d, nodes %d", finale, nodes_left);
    /* the portal cannot be sealed before Ralph is out */
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (enemies[i].active && enemies[i].type == E_ANCHOR && !(enemies[i].flags & EF_NODE))
            enemy_damage(i, 999, 1);
    CHECK(anchors_left == 4, "anchors broke while Ralph was trapped (%d)", anchors_left);
    kill_all(EF_NODE, 0);
    tick(0);
    CHECK(finale == FIN_PROTECT && ralph_free, "Ralph not released (finale %d)", finale);
    for (int t = 0; t < 3000 && finale == FIN_PROTECT; ++t) {
        pl.health = 100;
        tick(0);
    }
    CHECK(finale == FIN_ANCHORS, "Ralph never reached safety (finale %d, at %d,%d)", finale,
          (int)ralph_x, (int)ralph_y);
    kill_all(EF_OBJECTIVE, EF_NODE | EF_STAB);
    tick(0);
    CHECK(anchors_left == 0 && finale == FIN_STABILIZER, "anchors %d finale %d", anchors_left, finale);
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (enemies[i].active && (enemies[i].flags & EF_STAB)) enemy_damage(i, 999, 1);
    CHECK(enemies_alive(EF_STAB, 0) == 1, "the stabilizer fell to a single hit");
    int ready = cats_ready();
    press(PRG32_BTN_A);
    CHECK(cats_out == ready && ready > 0, "volley sent %d of %d cats", cats_out, ready);
    for (int t = 0; t < 600 && state == ST_FINAL_PORTAL; ++t) {
        pl.health = 100;
        tick(0);
    }
    CHECK(state == ST_ENDING && ev_ending == 1, "no ending (state %d, finale %d)", state, finale);
    CHECK(cats_out == 0, "cats left behind in the portal: %d", cats_out);
    CHECK(mus_song == MUS_NONE, "the portal closes in silence (mood %d)", mus_song);
    ticks_n(0, 50);
    CHECK(mus_song == MUS_VICTORY, "no victory blues (mood %d)", mus_song);
}

static void test_progression(void) {
    printf("progression reaches every level, the portal and the ending\n");
    boot();
    ev_ending = ev_boss_defeated = ev_level_complete = 0;
    press(PRG32_BTN_A);
    CHECK(state == ST_INTRO, "title -> intro, state %d", state);
    for (int i = 0; i < 3; ++i) { ticks_n(0, 2); press(PRG32_BTN_A); }
    CHECK(state == ST_LEVEL_START && level == 0, "intro -> level 1, state %d level %d", state, level);
    for (int l = 0; l < LEVEL_COUNT; ++l) {
        CHECK(level == l, "expected level %d, got %d", l, level);
        ticks_n(0, 20);
        press(PRG32_BTN_A);
        CHECK(state == ST_PLAYING, "level %d did not start (state %d)", l, state);
        CHECK(can_stand(pl.x, pl.y, PLAYER_R), "level %d: start is blocked", l);
        CHECK(can_stand(boss_x, boss_y, 90), "level %d: boss spawn is blocked", l);
        CHECK(clear_level(), "level %d: no boss fight", l);
        if (l < LEVEL_COUNT - 1) {
            CHECK(exit_open && state == ST_PLAYING, "level %d: exit not open", l);
            pl.x = exit_x;
            pl.y = exit_y;
            ticks_n(0, 3);
            CHECK(state == ST_LEVEL_COMPLETE, "level %d: not complete (state %d)", l, state);
            ticks_n(0, 40);
            press(PRG32_BTN_A);
        } else {
            test_finale();
        }
        if (failures > 10) return;
    }
    CHECK(ev_level_complete == LEVEL_COUNT - 1, "level completions %d", ev_level_complete);
    CHECK(ev_boss_defeated == LEVEL_COUNT, "boss defeats %d", ev_boss_defeated);
    for (int p = 0; p < 4; ++p) { ticks_n(0, 45); press(PRG32_BTN_A); }
    CHECK(state == ST_TITLE, "ending -> title, state %d", state);
    CHECK(ev_ending == 1, "ending triggered %d times", ev_ending);
}

static void test_portal_needs_leaders(void) {
    printf("portal anchors are sealed until the leaders fall\n");
    start_level(LEVEL_COUNT - 1);
    int anchors = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (enemies[i].active && enemies[i].type == E_ANCHOR) {
            anchors++;
            enemy_damage(i, 999, 1);
        }
    CHECK(anchors == 4 && anchors_left == 4, "sealed anchors were destroyed (%d/%d)", anchors, anchors_left);
    CHECK(clear_level(), "no leader fight");
    CHECK(bosses_spawned == 2, "expected two leaders, got %d", bosses_spawned);
    CHECK(state == ST_FINAL_PORTAL, "state %d", state);
}

static void test_music(void) {
    printf("the blues band follows the action\n");
    start_level(0);
    level_clear_objects();
    ticks_n(0, 40);
    CHECK(mus_song == MUS_EXPLORE, "calm: mood %d", mus_song);
    /* a mood change waits for the bar line and never restarts the song */
    int bar = mus_bar;
    music_play(MUS_COMBAT);
    CHECK(mus_song == MUS_EXPLORE || mus_step == 0, "changed mid-bar");
    for (int i = 0; i < 400 && mus_song != MUS_COMBAT; ++i) {
        music_tick();
        if (mus_song == MUS_COMBAT) CHECK(mus_step == 1, "changed off the bar line (step %d)", mus_step);
    }
    CHECK(mus_song == MUS_COMBAT && mus_bar != bar, "no change at the next bar");
    /* alerted roaches: danger, then combat when they close in */
    start_level(0);
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (enemies[i].active) enemies[i].state = AI_CHASE, enemies[i].x = pl.x + 6 * ONE;
    tick(0);
    CHECK(mus_next == MUS_DANGER, "alert: next mood %d", mus_next);
    pl.hurt = 5;
    tick(0);
    CHECK(mus_next == MUS_COMBAT, "hurt: next mood %d", mus_next);
    /* the leaders: heavy blues, then dissonance, then fast and unstable */
    start_level(LEVEL_COUNT - 1);
    kill_all(0, EF_BOSS | EF_OBJECTIVE);
    ticks_n(0, 60);
    kill_all(0, EF_BOSS | EF_OBJECTIVE);
    ticks_n(0, 200);
    CHECK(state == ST_BOSS && mus_next == MUS_BOSS && mus_warp == 0, "boss: %d warp %d", mus_next, mus_warp);
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (enemies[i].active && (enemies[i].flags & EF_BOSS)) enemies[i].hp = enemies[i].maxhp / 2;
    tick(0);
    CHECK(mus_warp == 2, "half health warp %d", mus_warp);
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (enemies[i].active && (enemies[i].flags & EF_BOSS)) enemies[i].hp = enemies[i].maxhp / 5;
    tick(0);
    CHECK(mus_warp == 3, "low health warp %d", mus_warp);
    kill_all(EF_BOSS, 0);
    tick(0);
    CHECK(mus_song == MUS_SAD, "the reveal drops to the sad phrase at once (%d)", mus_song);
}

/* A bot that fights only with cats: it aims at the nearest enemy, holds A,
 * and hops next to targets it cannot see (so no path finding is needed).
 * Health is refilled, so this checks that every wave and every boss --
 * shells, jumps and all -- can really be beaten by cats. */
static void test_cats_can_win(void) {
    printf("the armada alone can clear every level\n");
    for (int l = 0; l < LEVEL_COUNT; ++l) {
        start_level(l);
        int t;
        for (t = 0; t < 30 * 60 * 6; ++t) {
            pl.health = 100;
            if ((state == ST_PLAYING && exit_open) || state == ST_ENDING) break;
            int best = -1;
            int32_t bd = 1 << 30;
            for (int i = 0; i < MAX_ENEMIES; ++i) {
                const enemy_t *e = &enemies[i];
                if (!cat_can_target(i)) continue;
                int32_t d = dist_q8(e->x - pl.x, e->y - pl.y);
                if (d < bd) {
                    bd = d;
                    best = i;
                }
            }
            uint32_t in = PRG32_BTN_A;
            for (int i = 0; i < MAX_PICKUPS; ++i) /* go back for dazed cats */
                if (pickups[i].active && pickups[i].type == PK_DAZED && !pickups[i].wait &&
                    can_stand(pickups[i].x, pickups[i].y, PLAYER_R) && !enemy_blocks(pickups[i].x, pickups[i].y, PLAYER_R)) {
                    pl.x = pickups[i].x;
                    pl.y = pickups[i].y;
                    break;
                }
            if (best >= 0) {
                const enemy_t *e = &enemies[best];
                if (!line_of_sight(pl.x, pl.y, e->x, e->y) || bd > 6 * ONE) {
                    /* hop to a free tile near the target */
                    for (int k = 0; k < 8; ++k) {
                        int32_t nx = e->x + icos(k * 128) * 2 / 64, ny = e->y + isin(k * 128) * 2 / 64;
                        if (can_stand(nx, ny, PLAYER_R) && tile_at(nx, ny) != T_HOLE &&
                            !enemy_blocks(nx, ny, PLAYER_R) &&
                            line_of_sight(nx, ny, e->x, e->y)) {
                            pl.x = nx;
                            pl.y = ny;
                            break;
                        }
                    }
                }
                int bestang = pl.ang;
                int32_t bestlat = 1 << 30;
                for (int a = 0; a < 1024; a += 8) {
                    int32_t rx = e->x - pl.x, ry = e->y - pl.y;
                    int32_t depth = (rx * icos(a) + ry * isin(a)) >> 14;
                    int32_t lat = IABS((ry * icos(a) - rx * isin(a)) >> 14);
                    if (depth > 0 && lat < bestlat) {
                        bestlat = lat;
                        bestang = a;
                    }
                }
                pl.ang = bestang;
            }
            if (state == ST_LEVEL_START) in = (t & 1) ? PRG32_BTN_A : 0;
            tick((t & 1) ? in : 0);
            if (state == ST_PLAYER_DEAD) break;
        }
        CHECK(boss_defeated, "level %d: the cats did not beat the boss in %d ticks (state %d)", l + 1, t, state);
        if (l == LEVEL_COUNT - 1) CHECK(state == ST_ENDING, "portal not sealed by the cats (state %d, anchors %d)",
                                        state, anchors_left);
        printf("  level %2d cleared by cats in %3d s, armada %d\n", l + 1, t / 30, cats_max);
    }
}

/* Random buttons on every level, drawing as we go (run under ASan/UBSan). */
static void test_soak(int ticks_per_level) {
    printf("soak: random play on every level (%d ticks each)\n", ticks_per_level);
    for (int l = 0; l < LEVEL_COUNT; ++l) {
        start_level(l);
        uint32_t in = 0;
        for (int t = 0; t < ticks_per_level; ++t) {
            if ((t & 7) == 0) in = rnd() & 0x3f;
            if ((t & 511) == 0 && state == ST_PLAYING) kill_all(0, EF_BOSS | EF_OBJECTIVE);
            if (state == ST_PLAYER_DEAD || state == ST_LEVEL_COMPLETE || state == ST_LEVEL_START ||
                state == ST_GAME_OVER || state == ST_TITLE || state == ST_INTRO || state == ST_ENDING) {
                pl.health = 100;
                start_level(l);
            }
            tick(in);
            if ((t % 3) == 0) cockroaches_draw();
            CHECK(pl.health >= 0 && pl.health <= 100, "health %d", pl.health);
            CHECK(cats_out >= 0 && cats_out <= cats_max && cats_max >= 1, "cats %d/%d", cats_out, cats_max);
            if (failures > 20) return;
        }
    }
}

static void test_render_smoke(void) {
    printf("every screen renders the full viewport\n");
    boot();
    int states[] = {ST_TITLE, ST_INTRO, ST_ENDING, ST_GAME_OVER};
    for (unsigned k = 0; k < sizeof(states) / sizeof(states[0]); ++k) {
        game_set_state(states[k]);
        st_blits = 0;
        cockroaches_draw();
        CHECK(st_blits == SH / STRIP_H, "state %d: %ld blits", states[k], st_blits);
    }
    start_level(4);
    st_blits = 0;
    cockroaches_draw();
    CHECK(st_blits == SH / STRIP_H, "play: %ld blits (with the status bar)", st_blits);
}

/* ---- screenshots and cost ----------------------------------------------- */

static void band_draw(int band) {
    int y0 = band ? 220 : 0;
    for (int y = y0; y < y0 + 20; ++y)
        for (int x = 0; x < 320; ++x) fb[y][x] = cube565[CUBE(1, 0, 1)];
    const char *t = band_txt[band];
    int x = (320 - (int)strlen(t) * 8) / 2;
    for (; *t; ++t, x += 8) {
        int ch = (unsigned char)*t;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) ch = '?';
        for (int gy = 0; gy < 8; ++gy)
            for (int gx = 0; gx < 8; ++gx)
                if (font8[ch - 32][gy] & (0x80 >> gx) && x + gx >= 0 && x + gx < 320)
                    fb[y0 + 6 + gy][x + gx] = band ? cube565[CUBE(4, 5, 5)] : cube565[CUBE(5, 4, 1)];
    }
}

static void shot(const char *dir, const char *name) {
    char path[512];
    band_draw(0);
    band_draw(1);
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 240\n255\n");
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) {
            uint16_t c = fb[y][x];
            uint8_t rgb[3] = {(uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                              (uint8_t)((c & 31) * 255 / 31)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void draw_now(void) {
    screen_dirty = 1;
    hud_force = 1;
    world_static_drawn = 0;
    cockroaches_draw();
}

/* Face the nearest living enemy so the shot shows the action. */
static void face_enemy(void) {
    int best = -1;
    int32_t bd = 1 << 30;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &enemies[i];
        if (!e->active || e->state == AI_DEAD) continue;
        int32_t d = dist_q8(e->x - pl.x, e->y - pl.y);
        if (d < bd && line_of_sight(pl.x, pl.y, e->x, e->y)) {
            bd = d;
            best = i;
        }
    }
    if (best < 0) return;
    int bestang = 0;
    int32_t bestlat = 1 << 30;
    for (int a = 0; a < 1024; a += 4) {
        int32_t rx = enemies[best].x - pl.x, ry = enemies[best].y - pl.y;
        int32_t depth = (rx * icos(a) + ry * isin(a)) >> 14;
        int32_t lat = IABS((ry * icos(a) - rx * isin(a)) >> 14);
        if (depth > 0 && lat < bestlat) {
            bestlat = lat;
            bestang = a;
        }
    }
    pl.ang = bestang;
}

static void shots(const char *dir) {
    char name[64];
    boot();
    draw_now();
    shot(dir, "00_title");
    press(PRG32_BTN_B);
    draw_now();
    shot(dir, "01_help");
    press(PRG32_BTN_B);
    press(PRG32_BTN_A);
    for (int p = 0; p < 3; ++p) {
        draw_now();
        snprintf(name, sizeof(name), "02_intro%d", p);
        shot(dir, name);
        ticks_n(0, 2);
        press(PRG32_BTN_A);
    }
    for (int l = 0; l < LEVEL_COUNT; ++l) {
        start_level(l);
        game_set_state(ST_LEVEL_START);
        draw_now();
        snprintf(name, sizeof(name), "1%X_start", l);
        shot(dir, name);
        game_set_state(ST_PLAYING);
        ticks_n(0, 30);
        face_enemy();
        ticks_n(PRG32_BTN_UP, 12);
        face_enemy();
        tick(PRG32_BTN_A);
        tick(0);
        tick(0);
        draw_now();
        snprintf(name, sizeof(name), "1%X_play", l);
        shot(dir, name);
        if (l == 0 || l == 9 || l == 11) {
            clear_level();
            if (state == ST_PLAYING || state == ST_FINAL_PORTAL) {
                /* show the boss fight instead: respawn and look at it */
                start_level(l);
                kill_all(0, EF_BOSS | EF_OBJECTIVE);
                game_set_state(ST_BOSS);
                boss_spawn_level();
                for (int i = 0; i < MAX_ENEMIES; ++i) enemies[i].z = 0, enemies[i].state = enemies[i].active ? AI_CHASE : 0;
                pl.x = boss_x;
                pl.y = boss_y + 3 * ONE;
                if (!can_stand(pl.x, pl.y, PLAYER_R)) pl.y = boss_y - 3 * ONE;
                face_enemy();
                ticks_n(PRG32_BTN_B, 8);
                draw_now();
                snprintf(name, sizeof(name), "3%X_boss", l);
                shot(dir, name);
            }
        }
    }
    start_level(9);
    kill_all(0, 0);
    player_damage(500, DMG_BITE);
    draw_now();
    shot(dir, "40_dead");
    start_level(0);
    clear_level();
    pl.x = exit_x;
    pl.y = exit_y;
    ticks_n(0, 3);
    draw_now();
    shot(dir, "41_complete");
    /* the finale */
    start_level(LEVEL_COUNT - 1);
    clear_level();
    pl.x = portal_x + ONE;
    pl.y = portal_y + 4 * ONE;
    ticks_n(0, 100);
    draw_now();
    shot(dir, "42_reveal");
    ticks_n(0, 115);
    face_enemy();
    draw_now();
    shot(dir, "43_nodes");
    kill_all(EF_NODE, 0);
    ticks_n(0, 70);
    draw_now();
    shot(dir, "44_protect");
    for (int t = 0; t < 3000 && finale == FIN_PROTECT; ++t) {
        pl.health = 100;
        tick(0);
    }
    kill_all(EF_OBJECTIVE, EF_NODE | EF_STAB);
    ticks_n(0, 2);
    pl.x = portal_x;
    pl.y = portal_y + 4 * ONE;
    pl.ang = 768;
    press(PRG32_BTN_A);
    ticks_n(0, 3);
    draw_now();
    shot(dir, "45_volley");
    game_set_state(ST_ENDING);
    for (int p = 0; p < 4; ++p) {
        page = p;
        state_timer = p == 1 ? 70 : p == 2 ? 100 : 30;
        draw_now();
        snprintf(name, sizeof(name), "5%d_ending", p);
        shot(dir, name);
    }
    game_set_state(ST_GAME_OVER);
    draw_now();
    shot(dir, "60_gameover");
}

/* Estimated ESP32-C6 frame cost while playing, per level. */
static void cost(void) {
    /* microseconds: see docs/design.md, "Performance" */
    const double us_blit_call = 12.0, us_blit_px = 0.08, us_send_px = 0.62, us_op = 0.012;
    printf("level  blits  px_blit  ops/frame  est_ms (compose+blit+SPI)\n");
    for (int l = 0; l < LEVEL_COUNT; ++l) {
        start_level(l);
        ticks_n(0, 10);
        face_enemy();
        long frames = 60;
        st_blits = st_blit_px = 0;
        cost_ops = 0;
        for (int f = 0; f < frames; ++f) {
            tick(f & 8 ? PRG32_BTN_LEFT : PRG32_BTN_UP);
            cockroaches_draw();
        }
        double blits = (double)st_blits / frames, px = (double)st_blit_px / frames;
        double ops = (double)cost_ops / frames;
        double ms = (blits * us_blit_call + px * us_blit_px + px * us_send_px + ops * us_op) / 1000.0;
        printf("%5d  %5.1f  %7.0f  %9.0f  %6.1f\n", l + 1, blits, px, ops, ms);
    }
}

int main(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[1], "shots") == 0) {
        shots(argv[2]);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "cost") == 0) {
        cost();
        return 0;
    }
    int soak = argc > 1 ? atoi(argv[1]) : 3000;
    test_walls();
    test_no_damage_through_walls();
    test_poison();
    test_cat_targeting();
    test_shield();
    test_cat_count();
    test_boss_once();
    test_exit_locked();
    test_restart();
    test_portal_needs_leaders();
    test_progression();
    test_cats_can_win();
    test_music();
    test_render_smoke();
    test_soak(soak);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
