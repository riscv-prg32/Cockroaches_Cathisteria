/*
 * cats.c -- the feline armada, Fed's only weapon.
 *
 *   Fed commands -> a cat leaps -> it mauls the target -> it trots back
 *
 * cats_max is the size of the armada, cats_out the cats currently in the
 * field; cats_ready() = cats_max - cats_out can be sent (A) or hold the
 * shield together (B, player.c). A cat that bites into an enemy mid-attack
 * may be knocked out: it stays behind as a dazed pickup that Fed can
 * collect, and every dazed cat wanders home when the room boss falls.
 * The armada never drops below one loyal cat, and no count goes negative.
 */

#define CAT_SPEED 110
#define CAT_RETURN_SPEED 120
#define CAT_DAMAGE 12
#define CAT_KNOCKOUT_PCT 25

static int cat_launches;

static int cats_ready(void) { return cats_max - cats_out; }

static void cats_reset(void) {
    memset(cats, 0, sizeof(cats));
    cats_out = 0;
    cats_max = ICLAMP(cats_max, 1, MAX_CATS);
}

/* Put a dazed cat near (x, y) on free floor. */
static void drop_dazed(int32_t x, int32_t y, int variant) {
    static const int8_t off[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int k = 0; k < 5; ++k) {
        int32_t px = x + off[k][0] * 90, py = y + off[k][1] * 90;
        if (can_stand(px, py, 40) && tile_at(px, py) != T_HOLE) {
            pickup_spawn(PK_DAZED, px, py, variant);
            return;
        }
    }
    pickup_spawn(PK_DAZED, pl.x, pl.y, variant);
}

/* Poison or a nasty bite scares one ready cat away. */
static void cats_lose_one(int32_t x, int32_t y) {
    if (cats_max <= 1 || cats_ready() <= 0) return;
    cats_max--;
    drop_dazed(x - (icos(pl.ang) >> 7), y - (isin(pl.ang) >> 7), cats_max % 5);
    cat_message(cats_max, " PANICS AND HIDES! FIND HER.");
}

/* Is enemy i a valid target? Sealed anchors are not. */
static int cat_can_target(int i) {
    const enemy_t *e = &enemies[i];
    if (!e->active || e->state == AI_DEAD || e->z > 150) return 0;
    if (e->type == E_ANCHOR && (e->flags & EF_INVULN)) return 0;
    return 1;
}

/* The nearest enemy inside a narrow cone around the crosshair. */
static int cat_find_target(void) {
    int best = -1;
    int32_t best_depth = 9 * ONE;
    int c = icos(pl.ang), s = isin(pl.ang);
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        if (!cat_can_target(i)) continue;
        int32_t rx = enemies[i].x - pl.x, ry = enemies[i].y - pl.y;
        int32_t depth = (rx * c + ry * s) >> 14;
        int32_t lat = (ry * c - rx * s) >> 14;
        if (depth <= 16 || depth >= best_depth) continue;
        if (IABS(lat) > depth / 4 + 90) continue;
        if (!line_of_sight(pl.x, pl.y, enemies[i].x, enemies[i].y)) continue;
        best = i;
        best_depth = depth;
    }
    return best;
}

/* The finale: every cat at Fed's side leaps at the stabilizer at once. */
static void cats_volley(void) {
    int t = -1, n = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (enemies[i].active && enemies[i].state != AI_DEAD && (enemies[i].flags & EF_STAB)) t = i;
    if (t < 0 || cats_ready() <= 0) return;
    for (int i = 0; i < MAX_CATS && cats_ready() > 0; ++i) {
        cat_t *k = &cats[i];
        if (k->state != CAT_IDLE) continue;
        k->state = CAT_OUT;
        k->target = (uint8_t)t;
        k->timer = 0;
        k->variant = (uint8_t)(i % 5);
        k->x = pl.x + (i - 4) * 24;
        k->y = pl.y + (4 - i) * 12;
        cats_out++;
        n++;
    }
    enemies[t].flags &= (uint8_t)~EF_INVULN;
    enemies[t].hp = enemies[t].maxhp = (int16_t)IMAX(1, n * 6); /* it takes them all */
    pl.cat_cd = 30;
    pl.leap = 8;
    sfx(SFX_LAUNCH);
    message(n > 1 ? "THE WHOLE ARMADA POUNCES!" : "ONE BRAVE CAT POUNCES!", 60);
}

static void cats_launch(void) {
    if (finale == FIN_STABILIZER) {
        if (!pl.cat_cd) cats_volley();
        return;
    }
    if (finale == FIN_COLLAPSE) return; /* everyone is running home */
    if (cats_ready() <= 0 || pl.cat_cd || pl.shield_on) return;
    for (int i = 0; i < MAX_CATS; ++i) {
        cat_t *k = &cats[i];
        if (k->state != CAT_IDLE) continue;
        int t = cat_find_target();
        k->state = CAT_OUT;
        k->target = (uint8_t)(t < 0 ? 255 : t);
        k->timer = 0;
        k->variant = (uint8_t)(cat_launches++ % 5);
        k->dx = (int16_t)((icos(pl.ang) * CAT_SPEED) >> 14);
        k->dy = (int16_t)((isin(pl.ang) * CAT_SPEED) >> 14);
        k->x = pl.x + k->dx / 2;
        k->y = pl.y + k->dy / 2;
        cats_out++;
        pl.cat_cd = pl.poison ? 14 : 7;
        pl.leap = 8;
        sfx(SFX_LAUNCH);
        return;
    }
}

/* A cat reaches its enemy: damage, and maybe a knockout. */
static void cat_strike(cat_t *k, int i) {
    enemy_t *e = &enemies[i];
    /* Only the heavy hitters can knock a cat out: beetles, elites and
     * bosses caught mid-attack. Plain roaches just get mauled. */
    int heavy = e->type == E_BEETLE || e->type == E_ARMORED || e->type == E_ELITE;
    int risky = (heavy && (e->state == AI_ATTACK || e->state == AI_CHARGE)) ||
                (e->type == E_BOSS && boss_is_risky(e));
    if (e->flags & EF_INVULN) {
        sfx(SFX_BOING);
        k->state = CAT_RETURN;
        return;
    }
    enemy_damage(i, pl.poison ? CAT_DAMAGE * 2 / 3 : CAT_DAMAGE, 1);
    sfx(SFX_HIT);
    k->state = CAT_MAUL;
    k->target = (uint8_t)i;
    k->timer = 8;
    if (risky && cats_max > 1 && (int)(rnd() % 100) < CAT_KNOCKOUT_PCT) {
        k->state = CAT_IDLE;
        cats_out--;
        cats_max--;
        drop_dazed(e->x, e->y, k->variant);
        cat_message(cats_max, " GOT KNOCKED OUT!");
    }
}

static void cats_update(void) {
    for (int n = 0; n < MAX_CATS; ++n) {
        cat_t *k = &cats[n];
        if (k->state == CAT_IDLE) continue;
        if (k->state != CAT_MAUL) k->timer++;
        if (k->state == CAT_OUT) {
            int t = k->target;
            if (t != 255 && !cat_can_target(t)) t = 255;
            if (t != 255) aim(k->x, k->y, enemies[t].x, enemies[t].y, CAT_SPEED, &k->dx, &k->dy);
            int32_t nx = k->x + k->dx, ny = k->y + k->dy;
            if (solid_at(nx, ny) && tile_at(nx, ny) != T_PROP_A && tile_at(nx, ny) != T_PROP_B) {
                k->state = CAT_RETURN; /* bonk: cats leap over furniture, not walls */
                continue;
            }
            k->x = nx;
            k->y = ny;
            /* A homing cat only strikes its target; a cat sent at nothing
             * pounces on whatever it meets. */
            for (int i = 0; i < MAX_ENEMIES; ++i) {
                if (!cat_can_target(i) || (t != 255 && i != t)) continue;
                int reach = (enemies[i].type == E_BOSS ? 150 : 90);
                if (IABS(enemies[i].x - k->x) < reach && IABS(enemies[i].y - k->y) < reach) {
                    cat_strike(k, i);
                    break;
                }
            }
            if (k->state == CAT_OUT && k->timer > 40) k->state = CAT_RETURN;
        } else if (k->state == CAT_MAUL) {
            const enemy_t *e = &enemies[k->target];
            if (!e->active || e->state == AI_DEAD || --k->timer == 0) {
                k->state = CAT_RETURN;
                k->timer = 0;
            } else {
                k->x = e->x + ((int)(ticks & 3) - 1) * 12;
                k->y = e->y;
            }
        } else { /* CAT_RETURN */
            aim(k->x, k->y, pl.x, pl.y, CAT_RETURN_SPEED, &k->dx, &k->dy);
            k->x += k->dx;
            k->y += k->dy;
            if ((IABS(k->x - pl.x) < 80 && IABS(k->y - pl.y) < 80) || k->timer > 90) {
                k->state = CAT_IDLE;
                if (cats_out > 0) cats_out--;
            }
        }
    }
}

/* Every dazed cat comes home (after a boss fight). */
static void cats_recover_all(void) {
    int back = 0;
    for (int i = 0; i < MAX_PICKUPS; ++i) {
        if (pickups[i].active && pickups[i].type == PK_DAZED) {
            pickups[i].active = 0;
            if (cats_max < MAX_CATS) cats_max++;
            back++;
        }
    }
    if (back) message("THE DAZED CATS WANDER BACK HOME.", 60);
}
