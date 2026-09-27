/*
 * level.c -- turning a level_def_t into a playable room, and enemy waves.
 *
 * Levels are pure data (the files in levels/, compiled by tools/gen_levels.py into
 * level_data.h). Loading decodes the 4-bit map into `map[]` and converts
 * the item tiles into pickups, anchors and props, so new levels need no new
 * engine code. Only walls, doors, props, the exit and roof holes remain as
 * tile codes at run time.
 */

static void level_clear_objects(void) {
    memset(enemies, 0, sizeof(enemies));
    memset(pickups, 0, sizeof(pickups));
    memset(projs, 0, sizeof(projs));
    memset(puddles, 0, sizeof(puddles));
    nprops = 0;
}

static void prop_add(int32_t x, int32_t y, int kind) {
    if (nprops >= MAX_PROPS) return;
    props[nprops].x = x;
    props[nprops].y = y;
    props[nprops].kind = (uint8_t)kind;
    nprops++;
}

static int upgrade_type(int type) {
    switch (type) {
    case E_ROACH: return E_RUNNER;
    case E_RUNNER:
    case E_SPITTER: return E_ELITE;
    case E_BEETLE: return E_ARMORED;
    default: return type;
    }
}

/* Spawn wave n. Waves 1 and 2 use the map's spawn points; later waves
 * reuse the wave-1 points with tougher enemies. */
static void wave_spawn(int n) {
    const uint8_t *s = &level_spawns[lv->spawn_off * 3];
    int drop = (lv->flags & LF_DROP) != 0;
    for (int i = 0; i < lv->spawn_count; ++i, s += 3) {
        int type = s[2] & 15, w = s[2] >> 4;
        int32_t x = (s[0] << 8) + 128, y = (s[1] << 8) + 128;
        if (n <= 2 && w == n) enemy_spawn(type, x, y, drop || n > 1);
        else if (n > 2 && w == 1) enemy_spawn(upgrade_type(type), x, y, 1);
    }
}

static void open_doors(void) {
    int any = 0;
    for (int i = 0; i < map_w * map_h; ++i) {
        if (map[i] == T_DOOR) {
            map[i] = T_EMPTY;
            any = 1;
        }
    }
    doors_open = 1;
    if (any) {
        sfx(SFX_DOOR);
        message("THE DOORS SWING OPEN!", 60);
    }
}

static void level_load(int index) {
    level = ICLAMP(index, 0, LEVEL_COUNT - 1);
    lv = &level_defs[level];
    map_w = lv->w;
    map_h = lv->h;
    level_clear_objects();
    memset(map, 0, sizeof(map));
    const uint8_t *rle = &level_maps[lv->map_off];
    int run = 0, code = 0;
    for (int i = 0; i < map_w * map_h; ++i) {
        if (run == 0) { /* run-length coded: (count - 1) << 4 | tile */
            run = (*rle >> 4) + 1;
            code = *rle++ & 15;
        }
        run--;
        int tile = code;
        int32_t x = ((i % map_w) << 8) + 128, y = ((i / map_w) << 8) + 128;
        switch (tile) {
        case T_CAT: pickup_spawn(PK_CAT, x, y, 0); tile = T_EMPTY; break;
        case T_HEALTH: pickup_spawn(PK_HEALTH, x, y, 0); tile = T_EMPTY; break;
        case T_FOOD: pickup_spawn(PK_FOOD, x, y, 0); tile = T_EMPTY; break;
        case T_YARN: pickup_spawn(PK_YARN, x, y, 0); tile = T_EMPTY; break;
        case T_ANCHOR: {
            int e = enemy_spawn(E_ANCHOR, x, y, 0);
            if (e >= 0) enemies[e].flags |= EF_OBJECTIVE | EF_INVULN;
            tile = T_EMPTY;
            break;
        }
        case T_BOSS: boss_x = x; boss_y = y; tile = T_EMPTY; break;
        case T_EXIT: exit_x = x; exit_y = y; break;
        case T_PROP_A: prop_add(x, y, lv->props[0]); break;
        case T_PROP_B: prop_add(x, y, lv->props[1]); break;
        default: break;
        }
        map[i] = (uint8_t)tile;
    }
    pl.x = (lv->start_x << 8) + 128;
    pl.y = (lv->start_y << 8) + 128;
    pl.ang = lv->start_dir * 4;
    pl.safe_x = pl.x;
    pl.safe_y = pl.y;
    pl.shield = shield_max();
    pl.shield_on = pl.shield_broken = pl.shield_delay = 0;
    pl.yarn = pl.poison = pl.cat_cd = pl.leap = pl.hurt = pl.shield_hit = pl.iframes = 0;
    cats_reset();
    wave = 1;
    waves_total = lv->waves;
    wave_delay = 0;
    doors_open = exit_open = 0;
    boss_defeated = bosses_spawned = 0;
    anchors_left = lv->hazard == HAZ_PORTAL ? 4 : 0;
    portal_timer = collapse = 0;
    finale = finale_timer = nodes_left = ralph_visible = ralph_stun = ralph_free = ralph_follow = 0;
    flicker = quake = 0;
    squashed = 0;
    wave_spawn(1);
    render_level_setup();
}

/* Start (or restart) the current level from its checkpoint. */
static void level_restart(void) {
    pl.health = checkpoint.health;
    cats_max = checkpoint.cats_max;
    level_load(level);
}

/* Enter a level from the previous one: save the checkpoint first. */
static void level_enter(int index) {
    checkpoint.health = IMAX(pl.health, 60);
    checkpoint.cats_max = cats_max;
    pl.health = checkpoint.health;
    level_load(index);
}

/* Enemy waves; called every tick while the room is being fought. */
static void waves_update(void) {
    if (state != ST_PLAYING || exit_open || bosses_spawned) return;
    if (enemies_alive(0, EF_BOSS | EF_OBJECTIVE) > 0) return;
    if (wave_delay) {
        if (--wave_delay == 0) {
            wave++;
            wave_spawn(wave);
            sfx(SFX_SKITTER);
        }
        return;
    }
    if (wave == 1 && !doors_open) open_doors();
    if (wave < waves_total) {
        wave_delay = 45;
        message(wave + 1 == waves_total ? "LAST WAVE! THEY ARE ANGRY." : "MORE ROACHES INCOMING!", 60);
    } else {
        game_set_state(ST_ROOM_CLEARED);
    }
}
