/*
 * game.c -- the top-level state machine and the three cartridge entry
 * points. All progression lives here: the other modules only report
 * events (boss defeated, exit reached, Fed down) through game_set_state(),
 * on_boss_defeated() and level_complete().
 *
 *   TITLE -> INTRO -> LEVEL_START -> PLAYING -> ROOM_CLEARED -> BOSS
 *        ^                ^           ^  |                        |
 *        |                |           |  `--(exit)--> LEVEL_COMPLETE
 *        |                |           `----(boss down: exit opens)-'
 *        |          PLAYER_DEAD (A: retry the level, B: GAME_OVER)
 *        |   last level: BOSS -> FINAL_PORTAL -> ENDING -> TITLE
 *
 * FINAL_PORTAL is the finale (spoilers: see docs/design.md, section 6):
 *   REVEAL -> NODES -> PROTECT -> ANCHORS -> STABILIZER -> COLLAPSE
 */

static uint32_t last_ms, acc_ms, prev_in, pending;
static int sel_down, sel_used, screen_dirty, world_static_drawn;
static int32_t portal_x, portal_y;
static int combat_heat; /* ticks of combat music left after the last close call */

static int world_state(int s) {
    return s == ST_LEVEL_START || s == ST_PLAYING || s == ST_ROOM_CLEARED || s == ST_BOSS ||
           s == ST_FINAL_PORTAL || s == ST_LEVEL_COMPLETE || s == ST_PLAYER_DEAD;
}

static void game_set_state(int s) {
    state = s;
    state_timer = 0;
    screen_dirty = 1;
    hud_force = 1;
    world_static_drawn = 0;
    switch (s) {
    case ST_TITLE:
        page = 0;
        ambient(0);
        music_play(MUS_EXPLORE);
        break;
    case ST_INTRO:
        page = 0;
        break;
    case ST_LEVEL_START:
        paused = 0;
        combat_heat = 0;
        ambient(lv->hazard == HAZ_PORTAL);
        music_play(lv->music);
        break;
    case ST_PLAYING:
        break;
    case ST_ROOM_CLEARED:
        sfx(SFX_WARN);
        message("ROOM CLEARED! SOMETHING BIG IS COMING...", 75);
        break;
    case ST_BOSS:
        break;
    case ST_PLAYER_DEAD:
        music_play(MUS_NONE);
        sfx(SFX_FALL);
        break;
    case ST_LEVEL_COMPLETE:
        music_play(MUS_EXPLORE);
        sfx(SFX_MEOW);
        break;
    case ST_ENDING:
        page = 0;
        ambient(0);
        ev_ending++;
        music_play(MUS_NONE); /* silence as the portal closes; blues after */
        break;
    case ST_GAME_OVER:
        music_play(MUS_SAD);
        break;
    default:
        break;
    }
}

static void new_game(void) {
    memset(&pl, 0, sizeof(pl));
    pl.health = 100;
    cats_max = 4;
    checkpoint.health = 100;
    checkpoint.cats_max = cats_max;
    level_load(CC_START_LEVEL);
}

static void find_portal(void) {
    portal_x = boss_x;
    portal_y = boss_y;
    for (int i = 0; i < nprops; ++i) {
        if (props[i].kind == PROP_PORTAL) {
            portal_x = props[i].x;
            portal_y = props[i].y;
        }
    }
}

/* Called exactly once per level, when the last boss falls. */
static void on_boss_defeated(void) {
    if (boss_defeated) return;
    boss_defeated = 1;
    ev_boss_defeated++;
    cats_recover_all();
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        enemy_t *e = &enemies[i];
        if (e->active && e->state != AI_DEAD && !(e->flags & (EF_BOSS | EF_OBJECTIVE))) {
            e->state = AI_DEAD; /* the leaderless minions keel over */
            e->timer = 40;
        }
    }
    if (lv->hazard == HAZ_PORTAL) {
        /* No victory yet: the portal destabilises and reveals its prisoner. */
        find_portal();
        finale = FIN_REVEAL;
        finale_timer = 0;
        ralph_x = boss_x;
        ralph_y = boss_y + ONE;
        ralph_visible = 1;
        quake = 20;
        music_play(MUS_SAD);
        game_set_state(ST_FINAL_PORTAL);
    } else {
        exit_open = 1;
        message("THE WAY FORWARD IS OPEN! FIND THE EXIT.", 90);
        game_set_state(ST_PLAYING);
    }
}

static void level_complete(void) {
    if (state != ST_PLAYING || !exit_open) return; /* never before the boss falls */
    ev_level_complete++;
    game_set_state(ST_LEVEL_COMPLETE);
}

static void finale_set(int f, const char *text) {
    finale = f;
    finale_timer = 0;
    if (text) message(text, 90);
}

/* Portal surges, reinforcements and energy bolts; `hot` makes them faster. */
static void portal_attacks(int hot) {
    int period = hot ? 110 : 150;
    int phase = ++portal_timer % period;
    if (phase == period - 45) {
        sfx(SFX_WARN);
        message("PORTAL SURGE! RAISE THE FELINE SHIELD!", 45);
    } else if (phase == 0) {
        sfx(SFX_PULSE);
        quake = 10;
        player_damage(15, DMG_PORTAL);
    }
    if (portal_timer % (hot ? 150 : 200) == 100 && enemies_alive(0, EF_OBJECTIVE) < 4) {
        int32_t y = portal_y + 2 * ONE;
        if (can_stand(portal_x, y, 64)) enemy_spawn(rnd() & 1 ? E_ELITE : E_RUNNER, portal_x, y, 1);
    }
    if (hot && portal_timer % 60 == 30) proj_spawn(PJ_ZAP, portal_x, portal_y + 200, pl.x, pl.y, 40, 8);
}

static int spawn_objective(int32_t x, int32_t y, int flags) {
    int e = enemy_spawn(E_ANCHOR, x, y, 0);
    if (e >= 0) enemies[e].flags |= (uint8_t)(EF_OBJECTIVE | flags);
    return e;
}

/* The finale: discover the prisoner, free him, get him clear, seal it. */
static void finale_update(void) {
    finale_timer++;
    switch (finale) {
    case FIN_REVEAL: {
        /* Fed turns towards what the portal machinery was hiding. */
        int best = pl.ang, bl = 1 << 30;
        for (int a = 0; a < 1024; a += 16) {
            int32_t rx = ralph_x - pl.x, ry = ralph_y - pl.y;
            int32_t lat = IABS((ry * icos(a) - rx * isin(a)) >> 14);
            if (((rx * icos(a) + ry * isin(a)) >> 14) > 0 && lat < bl) {
                bl = lat;
                best = a;
            }
        }
        int d = ((best - pl.ang + 512) & 1023) - 512;
        pl.ang = (pl.ang + ICLAMP(d, -12, 12)) & 1023;
        if (finale_timer == 70) sfx(SFX_MEOW);
        if (finale_timer >= 200) {
            nodes_left = 0;
            static const int8_t off[3][2] = {{-5, 0}, {5, 0}, {0, -4}};
            for (int k = 0; k < 3; ++k)
                if (spawn_objective(ralph_x + off[k][0] * 64, ralph_y + off[k][1] * 64, EF_NODE) >= 0)
                    nodes_left++;
            finale_set(FIN_NODES, "BREAK THE 3 CONTAINMENT NODES!");
        }
        break;
    }
    case FIN_NODES:
        portal_attacks(0);
        if (nodes_left == 0) {
            ralph_free = 1;
            sfx(SFX_DOOR);
            finale_set(FIN_PROTECT, "RALPH IS FREE! GET HIM AWAY FROM THE PORTAL!");
        }
        break;
    case FIN_PROTECT: {
        portal_attacks(0);
        if (finale_timer % 45 == 20)
            proj_spawn(PJ_ZAP, portal_x, portal_y, ralph_x, ralph_y, 34, 8);
        /* Scripted: run to Fed, then head for the entrance with her. He
         * clambers over the portal machinery; only walls stop him. */
        int32_t sx = (lv->start_x << 8) + 128, sy = (lv->start_y << 8) + 128;
        if (!ralph_follow && dist_q8(pl.x - ralph_x, pl.y - ralph_y) < 320) ralph_follow = 1;
        if (ralph_stun) {
            ralph_stun--;
        } else {
            int16_t vx, vy;
            aim(ralph_x, ralph_y, ralph_follow ? sx : pl.x, ralph_follow ? sy : pl.y,
                ralph_follow ? 14 : 24, &vx, &vy);
            if (!view_solid(tile_at(ralph_x + vx, ralph_y))) ralph_x += vx;
            if (!view_solid(tile_at(ralph_x, ralph_y + vy))) ralph_y += vy;
        }
        if (ralph_follow && dist_q8(sx - ralph_x, sy - ralph_y) < 160) {
            for (int i = 0; i < MAX_ENEMIES; ++i)
                if (enemies[i].active && enemies[i].type == E_ANCHOR && !(enemies[i].flags & (EF_NODE | EF_STAB)))
                    enemies[i].flags &= (uint8_t)~EF_INVULN;
            finale_set(FIN_ANCHORS, "RALPH IS SAFE. NOW BREAK THE 4 ANCHORS!");
        }
        break;
    }
    case FIN_ANCHORS:
        portal_attacks(1);
        mus_layers = 1 + (4 - anchors_left);
        if (anchors_left == 0) {
            spawn_objective(portal_x, portal_y, EF_STAB | EF_INVULN);
            sfx(SFX_WARN);
            finale_set(FIN_STABILIZER, "A STABILIZER HOLDS IT OPEN! A: ALL CATS!");
        }
        break;
    case FIN_STABILIZER:
        portal_attacks(1);
        mus_layers = 4;
        if (enemies_alive(EF_STAB, 0) == 0) {
            collapse = 1;
            sfx(SFX_COLLAPSE);
            for (int i = 0; i < MAX_ENEMIES; ++i)
                if (enemies[i].active && enemies[i].state != AI_DEAD) {
                    enemies[i].state = AI_DEAD;
                    enemies[i].timer = 60;
                }
            finale_set(FIN_COLLAPSE, "IT COLLAPSES! CATS, COME BACK!");
        }
        break;
    default: /* FIN_COLLAPSE: the cats make it back just in time */
        quake = 4;
        if (collapse < 100) {
            collapse++;
            if (collapse == 100 && cats_out > 0) collapse = 99;  /* wait for the last cat */
            if (collapse == 100) {
                music_play(MUS_NONE);     /* the instant it closes: silence */
                sfx(SFX_THUD);
            }
        } else if (++collapse >= 145) {
            game_set_state(ST_ENDING);
        }
        break;
    }
}

/* The band follows the action: calm, alert, fighting, bosses, the portal. */

static void music_mood(void) {
    int mood = lv->music, warp = lv->hazard == HAZ_PORTAL ? 2 : lv->hazard == HAZ_FLICKER ? 1 : 0;
    int alerted = 0, near = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &enemies[i];
        if (!e->active || e->state == AI_DEAD || e->state == AI_IDLE || e->type == E_ANCHOR) continue;
        alerted++;
        if (IABS(e->x - pl.x) < 4 * ONE && IABS(e->y - pl.y) < 4 * ONE) near++;
    }
    if (near >= 2 || pl.hurt) combat_heat = 120;
    else if (combat_heat) combat_heat--;
    if (state == ST_BOSS) {
        mood = MUS_BOSS;
        int hp = 0, max = 1;
        for (int i = 0; i < MAX_ENEMIES; ++i)
            if (enemies[i].active && (enemies[i].flags & EF_BOSS)) {
                hp += enemies[i].hp;
                max += enemies[i].maxhp;
            }
        /* The leaders: heavy blues, then dissonance, then fast and unstable. */
        warp = lv->hazard != HAZ_PORTAL ? warp : hp * 3 > max * 2 ? 0 : hp * 3 > max ? 2 : 3;
    } else if (state == ST_FINAL_PORTAL) {
        static const uint8_t fin_mood[7] = {MUS_PORTAL, MUS_SAD, MUS_RESCUE, MUS_RESCUE,
                                            MUS_CRESCENDO, MUS_CRESCENDO, MUS_CRESCENDO};
        mood = fin_mood[finale];
        warp = finale == FIN_REVEAL ? 0 : finale <= FIN_PROTECT ? 1 : finale == FIN_ANCHORS ? 2 : 3;
        if (finale == FIN_COLLAPSE && collapse >= 100) mood = MUS_NONE;
    } else if (state == ST_ROOM_CLEARED) {
        mood = MUS_DANGER;
    } else if (!exit_open) {
        mood = combat_heat ? MUS_COMBAT : alerted ? MUS_DANGER : lv->music;
    }
    music_play(mood);
    mus_warp = warp;
}

/* One 1/30 s step of the world. */
static void world_tick(uint32_t in) {
    int s = state;
    if (state == ST_FINAL_PORTAL && finale == FIN_REVEAL) { /* a moment of silence */
        cats_update();
        finale_update();
        return;
    }
    player_update(in);
    if (state != s) return; /* Fed died or reached the exit */
    cats_update();
    enemies_update();
    if (state != s && state != ST_PLAYING && state != ST_FINAL_PORTAL) return;
    projectiles_update();
    hazards_update();
    waves_update();
    if (state == ST_ROOM_CLEARED) {
        if (state_timer % 20 == 0) sfx(SFX_WARN);
        if (state_timer >= 75) {
            boss_spawn_level();
            sp_begin();
            sp_str("BOSS: ");
            sp_str(boss_defs[lv->boss].name);
            message(sp_end(), 90);
            game_set_state(ST_BOSS);
        }
    } else if (state == ST_FINAL_PORTAL) {
        finale_update();
    }
}

static void game_tick(uint32_t in, uint32_t pressed) {
    ticks++;
    state_timer++;
    if (msg_ticks) msg_ticks--;
    sp_reset();
    switch (state) {
    case ST_TITLE:
        if ((state_timer & 15) == 0) screen_dirty = 1;
        if (pressed & (PRG32_BTN_A | PRG32_BTN_START)) {
            sfx(SFX_UI);
            if (page == 1) {
                new_game();
                game_set_state(ST_INTRO);
            } else {
                new_game();
                game_set_state(ST_INTRO);
            }
        } else if (pressed & PRG32_BTN_B) {
            page ^= 1;
            screen_dirty = 1;
            sfx(SFX_UI);
        }
        break;
    case ST_INTRO:
        if ((state_timer & 15) == 0) screen_dirty = 1;
        if (pressed & (PRG32_BTN_A | PRG32_BTN_START | PRG32_BTN_B)) {
            sfx(SFX_UI);
            if (++page > 2 || (pressed & PRG32_BTN_B)) {
                level_restart();
                game_set_state(ST_LEVEL_START);
            } else {
                screen_dirty = 1;
            }
        }
        break;
    case ST_LEVEL_START:
        if ((state_timer & 15) == 0) screen_dirty = 1;
        if ((state_timer > 15 && (pressed & (PRG32_BTN_A | PRG32_BTN_START))) || state_timer > 240) {
            game_set_state(ST_PLAYING);
            message(LV_TEXT(quip), 60);
        }
        break;
    case ST_PLAYING:
    case ST_ROOM_CLEARED:
    case ST_BOSS:
    case ST_FINAL_PORTAL:
        /* SELECT alone toggles pause; SELECT with a direction strafes. */
        if (pressed & PRG32_BTN_SELECT) {
            sel_down = 1;
            sel_used = 0;
        }
        if (sel_down && (in & (PRG32_BTN_LEFT | PRG32_BTN_RIGHT))) sel_used = 1;
        if (sel_down && !(in & PRG32_BTN_SELECT)) {
            sel_down = 0;
            if (!sel_used) {
                paused = !paused;
                screen_dirty = 1;
                world_static_drawn = 0;
            }
        }
        if (paused) {
            state_timer--;
            break;
        }
        world_tick(in);
        if (world_state(state) && state != ST_PLAYER_DEAD && state != ST_LEVEL_COMPLETE) music_mood();
        break;
    case ST_PLAYER_DEAD:
        if ((state_timer & 15) == 0) screen_dirty = 1;
        if (state_timer > 30 && (pressed & (PRG32_BTN_A | PRG32_BTN_START))) {
            level_restart();
            game_set_state(ST_LEVEL_START);
        } else if (state_timer > 30 && (pressed & PRG32_BTN_B)) {
            game_set_state(ST_GAME_OVER);
        }
        break;
    case ST_LEVEL_COMPLETE:
        if ((state_timer & 15) == 0) screen_dirty = 1;
        if ((state_timer > 30 && (pressed & (PRG32_BTN_A | PRG32_BTN_START))) || state_timer > 600) {
            level_enter(level + 1);
            game_set_state(ST_LEVEL_START);
        }
        break;
    case ST_ENDING:
        if (page == 0 && state_timer == 45) music_play(MUS_VICTORY); /* after the silence */
        if (page == 0 || (state_timer & 7) == 0) screen_dirty = 1;
        if (page == 1 && state_timer == 150) sfx(SFX_MEOW);
        if (state_timer > 40 && (pressed & (PRG32_BTN_A | PRG32_BTN_START))) {
            state_timer = 0;
            screen_dirty = 1;
            if (++page > 3) game_set_state(ST_TITLE);
        }
        break;
    case ST_GAME_OVER:
        if ((state_timer & 15) == 0) screen_dirty = 1;
        if (state_timer > 30 && (pressed & (PRG32_BTN_A | PRG32_BTN_START))) game_set_state(ST_TITLE);
        break;
    default:
        game_set_state(ST_TITLE);
        break;
    }
    audio_tick();
}

/* ---- drawing -------------------------------------------------------- */

static void world_vis(void) {
    nvis = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &enemies[i];
        if (!e->active) continue;
        int spr, pal, flags, ww, wh;
        if (e->type == E_BOSS) {
            boss_sprite(e, &spr, &pal, &flags, &ww, &wh);
            if (e->z > 0) vis_add(e->x, e->y, 0, ww, ww / 3, SPR_SPLAT, PAL_DEBRIS, VF_DARK);
        } else {
            enemy_sprite(e, i, &spr, &pal, &flags);
            ww = etypes[e->type].ww;
            wh = etypes[e->type].wh;
            if (e->flags & EF_STAB) ww = 200, wh = 330;
            if (e->state == AI_DEAD) wh = wh * 2 / 3;
        }
        vis_add(e->x, e->y, e->z, ww, wh, spr, pal, flags);
    }
    for (int i = 0; i < MAX_CATS; ++i) {
        const cat_t *k = &cats[i];
        if (k->state == CAT_IDLE) continue;
        /* right next to Fed the leap overlay (hud.c) shows the cat instead */
        if (IABS(k->x - pl.x) < 150 && IABS(k->y - pl.y) < 150) continue;
        int pal = PAL_CAT_ORANGE + k->variant;
        if (k->state == CAT_OUT) vis_add(k->x, k->y, 50, 110, 110, SPR_CAT_LEAP, pal, 0);
        else if (k->state == CAT_RETURN) vis_add(k->x, k->y, 20, 100, 100, SPR_CAT_POUNCE, pal, 0);
        else {
            vis_add(k->x, k->y, 60, 120, 120, SPR_CAT_POUNCE, pal, 0);
            if (ticks & 2) vis_add(k->x, k->y, 70, 120, 120, SPR_CLAW, PAL_FX, 0);
        }
    }
    for (int i = 0; i < MAX_PROJECTILES; ++i) {
        const proj_t *p = &projs[i];
        if (!p->active) continue;
        if (p->type == PJ_DEBRIS) {
            vis_add(p->x, p->y, 0, 130, 40, SPR_SPLAT, PAL_DEBRIS, VF_DARK);
            vis_add(p->x, p->y, p->z, 90, 90, SPR_DEBRIS, PAL_DEBRIS, 0);
        } else {
            vis_add(p->x, p->y, p->z, 60, 60, SPR_POISON, PAL_POISON, (ticks & 2) ? VF_FLIP : 0);
        }
    }
    for (int i = 0; i < MAX_PICKUPS; ++i) {
        const pickup_t *p = &pickups[i];
        if (!p->active || (IABS(p->x - pl.x) < 72 && IABS(p->y - pl.y) < 72)) continue;
        int z = 16 + (isin((int)ticks * 24 + i * 100) >> 10);
        switch (p->type) {
        case PK_CAT: vis_add(p->x, p->y, 0, 110, 110, SPR_CAT_SIT, PAL_CAT_ORANGE + i % 5, (ticks >> 4) & 1 ? VF_FLIP : 0); break;
        case PK_DAZED: vis_add(p->x, p->y, 0, 100, 100, SPR_CAT_DAZED, PAL_CAT_ORANGE + p->variant % 5, 0); break;
        case PK_HEALTH: vis_add(p->x, p->y, z, 90, 90, SPR_PIZZA, PAL_ITEMS, 0); break;
        case PK_FOOD: vis_add(p->x, p->y, z, 80, 80, SPR_FOOD, PAL_ITEMS, 0); break;
        default: vis_add(p->x, p->y, z, 80, 80, SPR_YARN, PAL_ITEMS, 0); break;
        }
    }
    for (int i = 0; i < MAX_PUDDLES; ++i)
        if (puddles[i].ttl) vis_add(puddles[i].x, puddles[i].y, 0, 220, 70, SPR_SPLAT, PAL_POISON, 0);
    if (ralph_visible) {
        int trapped = !ralph_free;
        int flick = trapped && (ticks & (uint32_t)(nodes_left + 1)) == 0;
        vis_add(ralph_x, ralph_y, trapped ? 60 + (isin((int)ticks * 20) >> 10) : 0, 140, 200,
                SPR_RALPH, PAL_RALPH, flick ? VF_FLASH : (ralph_stun & 4) ? VF_DARK : 0);
    }
    for (int i = 0; i < nprops; ++i) {
        const prop_t *p = &props[i];
        const prop_info_t *pi = &prop_info[p->kind];
        int ww = pi->ww, wh = pi->wh, flags = 0;
        if (p->kind == PROP_PORTAL) {
            int s = collapse ? IMAX(10, 100 - collapse) : 100;
            ww = ww * s / 100;
            wh = wh * s / 100;
            flags = (ticks >> 2) & 1 ? VF_FLIP : 0;
        }
        vis_add(p->x, p->y, 0, ww, wh, pi->spr, pi->pal, flags);
    }
    vis_sort();
}

static void draw_world(void) {
    int moving = !(state == ST_LEVEL_START || state == ST_LEVEL_COMPLETE ||
                   state == ST_PLAYER_DEAD || paused);
    if (!moving && world_static_drawn && !screen_dirty) return;
    world_static_drawn = 1;
    screen_dirty = 0;
    dark_extra = (lv->hazard == HAZ_FLICKER && flicker) ? 2 : 0;
    camera_from_player();
    cast_rays();
    world_vis();
    ops_clear();
    switch (state) {
    case ST_LEVEL_START: level_start_ops(); break;
    case ST_LEVEL_COMPLETE: level_complete_ops(); break;
    case ST_PLAYER_DEAD: dead_ops(); break;
    default:
        view_ops();
        if (state == ST_ROOM_CLEARED) room_cleared_ops();
        if (state == ST_FINAL_PORTAL) portal_ops();
        if (paused) {
            op_dim(0, 0, SW, VIEW_H);
            op_text(0, 56, 3, COL_TEXT, OPF_CENTER | OPF_SHADOW, "PAUSED");
        }
        break;
    }
    render_band(0, VIEW_H, 1, 0);
}

static void draw_screen(void) {
    if (!screen_dirty) return;
    screen_dirty = 0;
    ops_clear();
    {
        int pg = state == ST_TITLE ? PG_TITLE + page : state == ST_INTRO ? PG_INTRO0 + page :
                 state == ST_ENDING ? PG_END0 + page : PG_GAMEOVER;
        if (state == ST_ENDING && page == 0) { /* the portal shrinks to nothing */
            int s = IMAX(8, 160 - state_timer * 2);
            op_sprite(SPR_PORTAL, PAL_PORTAL, SW / 2 - s / 2, 90 - s / 2, s, s, (ticks >> 2) & 1 ? VF_FLIP : 0);
        }
        page_ops(pg, state_timer);
    }
    render_band(0, SH, 0, state == ST_ENDING && page == 0 ? CUBE(0, 0, 0) : HUD_BG);
}

/* ---- cartridge entry points ------------------------------------------- */

void cockroaches_init(void) {
    rng_state ^= prg32_ticks_ms() * 2654435761u;
    if (!rng_state) rng_state = 1;
    render_init();
    audio_init();
    prg32_band_set_mode(PRG32_BAND_TOP, PRG32_BAND_MODE_CUSTOM);
    prg32_band_set_mode(PRG32_BAND_BOTTOM, PRG32_BAND_MODE_CUSTOM);
    prg32_band_set_colors(PRG32_BAND_TOP, cube565[CUBE(5, 4, 1)], cube565[CUBE(1, 0, 1)]);
    prg32_band_set_colors(PRG32_BAND_BOTTOM, cube565[CUBE(4, 5, 5)], cube565[CUBE(1, 0, 1)]);
    mus_song = mus_next = MUS_NONE;
    last_ms = prg32_ticks_ms();
    new_game();
    game_set_state(ST_TITLE);
}

void cockroaches_update(void) {
    uint32_t now = prg32_ticks_ms();
    uint32_t in = prg32_input_read();
    acc_ms += now - last_ms;
    last_ms = now;
    if (acc_ms > 200) acc_ms = 200; /* after a long stall, do not fast-forward */
    pending |= in & ~prev_in;
    prev_in = in;
    for (int n = 0; acc_ms >= TICK_MS && n < MAX_TICKS_PER_FRAME; ++n) {
        acc_ms -= TICK_MS;
        game_tick(in, pending);
        pending = 0;
    }
}

void cockroaches_draw(void) {
    sp_reset();
    if (world_state(state)) {
        bands_update();
        if (hud_needs_redraw()) {
            ops_clear();
            hud_ops();
            render_band(HUD_Y, SH, 0, HUD_BG);
            /* On the ESP32-C6 send the status bar on its own, so the next
             * present only covers the rows of the 3D view. */
            if (!CC_TARGET_QEMU) prg32_gfx_present();
        }
        draw_world();
    } else {
        bands_update();
        draw_screen();
    }
}
