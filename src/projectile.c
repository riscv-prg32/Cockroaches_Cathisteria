/*
 * projectile.c -- poison spit, poison puddles and falling chimney bricks.
 *
 * Projectiles are points moving in a straight line. They die on the first
 * wall they touch (so nothing hits Fed through a wall), and hit Fed when
 * they come within a third of a tile. The feline shield blocks spit and
 * bricks but not the puddles on the floor: those must be walked around.
 */

static void proj_spawn(int type, int32_t x, int32_t y, int32_t tx, int32_t ty, int speed, int dmg) {
    for (int i = 0; i < MAX_PROJECTILES; ++i) {
        proj_t *p = &projs[i];
        if (p->active) continue;
        p->active = 1;
        p->type = (uint8_t)type;
        p->x = x;
        p->y = y;
        p->dmg = (uint8_t)dmg;
        p->ttl = 120;
        p->z = 70;
        aim(x, y, tx, ty, speed, &p->vx, &p->vy);
        return;
    }
}

static void puddle_spawn(int32_t x, int32_t y) {
    int oldest = 0;
    for (int i = 0; i < MAX_PUDDLES; ++i) {
        if (puddles[i].ttl == 0) {
            oldest = i;
            break;
        }
        if (puddles[i].ttl < puddles[oldest].ttl) oldest = i;
    }
    puddles[oldest].x = x;
    puddles[oldest].y = y;
    puddles[oldest].ttl = 300;
}

static void debris_spawn(void) {
    int32_t x = pl.x + rnd_range(-200, 200), y = pl.y + rnd_range(-200, 200);
    if (!can_stand(x, y, 40)) return;
    for (int i = 0; i < MAX_PROJECTILES; ++i) {
        proj_t *p = &projs[i];
        if (p->active) continue;
        p->active = 1;
        p->type = PJ_DEBRIS;
        p->x = x;
        p->y = y;
        p->vx = p->vy = 0;
        p->z = 460;
        p->dmg = 8;
        p->ttl = 60;
        return;
    }
}

static void projectiles_update(void) {
    for (int i = 0; i < MAX_PROJECTILES; ++i) {
        proj_t *p = &projs[i];
        if (!p->active) continue;
        if (p->type == PJ_DEBRIS) {
            p->z = (int16_t)(p->z - 14);
            if (p->z <= 0) {
                p->active = 0;
                sfx(SFX_THUD);
                if (IABS(p->x - pl.x) < 100 && IABS(p->y - pl.y) < 100)
                    player_damage(p->dmg, DMG_DEBRIS);
            }
            continue;
        }
        int32_t nx = p->x + p->vx, ny = p->y + p->vy;
        if (solid_at(nx, ny) && tile_at(nx, ny) != T_PROP_A && tile_at(nx, ny) != T_PROP_B) {
            p->active = 0;
            if (p->type == PJ_BOSS_SPIT && (rnd() & 3) == 0) puddle_spawn(p->x, p->y);
            continue;
        }
        p->x = nx;
        p->y = ny;
        if (p->type == PJ_ZAP && ralph_free && IABS(p->x - ralph_x) < 90 && IABS(p->y - ralph_y) < 90) {
            p->active = 0;
            if (pl.shield_on && pl.shield > 8 && dist_q8(pl.x - ralph_x, pl.y - ralph_y) < 380) {
                pl.shield -= 8; /* the feline barrier covers him too */
                pl.shield_hit = 6;
                sfx(SFX_SHIELD_HIT);
            } else {
                ralph_stun = 40;
                sfx(SFX_HURT);
                message("RALPH IS HIT! SHIELD HIM!", 45);
            }
            continue;
        }
        if (IABS(p->x - pl.x) < 80 && IABS(p->y - pl.y) < 80) {
            p->active = 0;
            player_damage(p->dmg, DMG_POISON);
            continue;
        }
        if (--p->ttl == 0) p->active = 0;
    }
    for (int i = 0; i < MAX_PUDDLES; ++i) {
        puddle_t *u = &puddles[i];
        if (!u->ttl) continue;
        u->ttl--;
        if ((u->ttl % 12) == 0 && IABS(u->x - pl.x) < 90 && IABS(u->y - pl.y) < 90)
            player_damage(2, DMG_PUDDLE);
    }
}

/* Level hazards that are not tied to an enemy. */
static void hazards_update(void) {
    switch (lv->hazard) {
    case HAZ_DEBRIS:
        if (state == ST_PLAYING || state == ST_BOSS)
            if (rnd() % 90 == 0) debris_spawn();
        break;
    case HAZ_FLICKER:
        if (flicker) flicker--;
        else if (rnd() % 50 == 0) flicker = rnd_range(3, 10);
        break;
    default:
        break;
    }
    if (quake) quake--;
}
