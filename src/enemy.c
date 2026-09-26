/*
 * enemy.c -- giant alien cockroaches and beetles.
 *
 * Every enemy runs the same small state machine:
 *
 *   IDLE --sees Fed--> CHASE --close--> ATTACK (bite after a wind-up)
 *                        |---in range--> SPIT (poison projectile)
 *                        `---beetle----> CHARGE (wind-up, then a rush)
 *   any --hit--> HURT (stagger)      hp <= 0 --> DEAD (corpse, then freed)
 *
 * Steering is deliberately simple: walk straight at Fed, slide along
 * walls, and sidestep for a moment when stuck. Rooms are small enough that
 * no path search is needed. Bosses share the pool and the damage rules but
 * think in boss.c.
 */

#define ETF_SPIT 1
#define ETF_CHARGE 2
#define ETF_FLY 4

typedef struct {
    uint8_t spr, pal, hp, speed, dmg, radius;
    uint16_t ww, wh;
    uint8_t armor;   /* damage taken, in quarters: 4 = full, 1 = a quarter */
    uint8_t spit_cd;
    uint8_t flags;
} etype_t;

static const etype_t etypes[E_TYPE_COUNT] = {
    /* ROACH   */ {SPR_ROACH_A, PAL_ROACH, 20, 12, 4, 60, 150, 110, 4, 0, 0},
    /* RUNNER  */ {SPR_ROACH_A, PAL_RUNNER, 16, 22, 3, 56, 130, 100, 4, 0, 0},
    /* SPITTER */ {SPR_ROACH_A, PAL_SPITTER, 22, 10, 4, 60, 150, 110, 4, 70, ETF_SPIT},
    /* BEETLE  */ {SPR_BEETLE_A, PAL_BEETLE, 45, 8, 12, 80, 190, 150, 2, 0, ETF_CHARGE},
    /* ARMORED */ {SPR_BEETLE_A, PAL_ARMORED, 60, 9, 14, 84, 200, 160, 1, 0, ETF_CHARGE},
    /* ELITE   */ {SPR_ROACH_A, PAL_ELITE, 34, 20, 7, 64, 170, 130, 4, 50, ETF_SPIT},
    /* FLYER   */ {SPR_ROACH_A, PAL_FLYER, 18, 18, 4, 56, 150, 110, 4, 0, ETF_FLY},
    /* BOSS    */ {SPR_ROACH_A, PAL_ROACH, 100, 12, 12, 120, 300, 220, 4, 0, 0},
    /* ANCHOR  */ {SPR_ANCHOR, PAL_ANCHOR, 60, 0, 0, 70, 120, 220, 4, 0, 0},
};

static int enemy_radius(const enemy_t *e) {
    return e->type == E_BOSS ? 120 : etypes[e->type].radius;
}

static int enemy_spawn(int type, int32_t x, int32_t y, int drop) {
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        enemy_t *e = &enemies[i];
        if (e->active) continue;
        memset(e, 0, sizeof(*e));
        e->active = 1;
        e->type = (uint8_t)type;
        e->x = x;
        e->y = y;
        e->hp = e->maxhp = etypes[type].hp;
        e->state = AI_IDLE;
        e->cd = (uint8_t)(20 + (rnd() & 31));
        e->z = drop ? 420 : 0;
        e->anim = (uint8_t)i;
        return i;
    }
    return -1;
}

static int enemies_alive(int flags_any, int flags_none) {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &enemies[i];
        if (!e->active || e->state == AI_DEAD) continue;
        if (flags_any && !(e->flags & flags_any)) continue;
        if (e->flags & flags_none) continue;
        n++;
    }
    return n;
}

/* Would a body of radius r at (x, y) overlap a living enemy? */
static int enemy_blocks(int32_t x, int32_t y, int r) {
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &enemies[i];
        if (!e->active || e->state == AI_DEAD || e->z > 120) continue;
        int d = enemy_radius(e) + r - 16;
        if (IABS(e->x - x) < d && IABS(e->y - y) < d) return 1;
    }
    return 0;
}

static void enemy_kill(int i) {
    enemy_t *e = &enemies[i];
    e->state = AI_DEAD;
    e->timer = 60;
    e->z = 0;
    e->flags &= (uint8_t)~(EF_INVULN | EF_EXPOSED);
    squashed++;
    sfx(SFX_DIE);
    if (e->type == E_ANCHOR) {
        if (anchors_left > 0) anchors_left--;
        quake = 10;
    }
    if ((e->flags & EF_BOSS) && enemies_alive(EF_BOSS, 0) == 0) on_boss_defeated();
}

static void enemy_damage(int i, int amount, int from_cat) {
    enemy_t *e = &enemies[i];
    (void)from_cat;
    if (!e->active || e->state == AI_DEAD) return;
    if (e->flags & EF_INVULN) {
        sfx(SFX_BOING);
        return;
    }
    int armor = (e->flags & EF_EXPOSED) ? 8 : etypes[e->type].armor;
    int dmg = IMAX(1, amount * armor / 4);
    e->hp -= (int16_t)dmg;
    e->flash = 4;
    if (e->state == AI_IDLE) e->state = AI_CHASE;
    if (e->hp <= 0) {
        e->hp = 0;
        enemy_kill(i);
        return;
    }
    if (e->type <= E_RUNNER || e->type == E_SPITTER || e->type == E_ELITE || e->type == E_FLYER) {
        /* light bugs stagger backwards */
        if (e->state != AI_ATTACK || (rnd() & 1)) {
            e->state = AI_HURT;
            e->timer = 5;
            aim(pl.x, pl.y, e->x, e->y, 10, &e->vx, &e->vy);
        }
    }
}

/* Walk towards (tx, ty); sidestep for a while when stuck. */
static void enemy_steer(enemy_t *e, int32_t tx, int32_t ty, int speed) {
    int16_t vx, vy;
    int32_t ox = e->x, oy = e->y;
    int r = enemy_radius(e) * 3 / 4;
    aim(e->x, e->y, tx, ty, speed, &vx, &vy);
    if (e->stuck) {
        int16_t t = vx;
        vx = (int16_t)(-vy * e->side);
        vy = (int16_t)(t * e->side);
        e->stuck--;
    }
    move_actor(&e->x, &e->y, vx, vy, r);
    if (!e->stuck && IABS(e->x - ox) + IABS(e->y - oy) < speed / 3) {
        e->stuck = (uint8_t)(10 + (rnd() & 7));
        e->side = (rnd() & 1) ? 1 : -1;
    }
}

static void enemies_separate(void) {
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        enemy_t *a = &enemies[i];
        if (!a->active || a->state == AI_DEAD || a->type == E_ANCHOR) continue;
        for (int j = i + 1; j < MAX_ENEMIES; ++j) {
            enemy_t *b = &enemies[j];
            if (!b->active || b->state == AI_DEAD || b->type == E_ANCHOR) continue;
            int d = (enemy_radius(a) + enemy_radius(b)) * 3 / 4;
            int32_t dx = b->x - a->x, dy = b->y - a->y;
            if (IABS(dx) >= d || IABS(dy) >= d) continue;
            int px = dx >= 0 ? 4 : -4, py = dy >= 0 ? 4 : -4;
            if (IABS(dx) > IABS(dy)) py = 0;
            else px = 0;
            move_actor(&b->x, &b->y, px, py, enemy_radius(b) * 3 / 4);
            move_actor(&a->x, &a->y, -px, -py, enemy_radius(a) * 3 / 4);
        }
    }
}

static void enemy_think(enemy_t *e) {
    const etype_t *t = &etypes[e->type];
    int32_t dx = pl.x - e->x, dy = pl.y - e->y;
    int32_t d = dist_q8(dx, dy);
    int reach = t->radius + 64 + 24;
    switch (e->state) {
    case AI_IDLE:
        if (((ticks + e->anim) & 7) == 0 && d < 10 * ONE && line_of_sight(e->x, e->y, pl.x, pl.y)) {
            e->state = AI_CHASE;
            if ((rnd() & 3) == 0) sfx(SFX_SKITTER);
        }
        break;
    case AI_CHASE:
        if (t->flags & ETF_FLY) e->z = (int16_t)(70 + (isin((int)ticks * 12 + e->anim * 90) >> 9));
        if (e->cd) e->cd--;
        if ((t->flags & ETF_SPIT) && !e->cd && d > 380 && d < 7 * ONE &&
            line_of_sight(e->x, e->y, pl.x, pl.y)) {
            e->state = AI_SPIT;
            e->timer = 12;
        } else if ((t->flags & ETF_CHARGE) && !e->cd && d > 300 && d < 4 * ONE &&
                   line_of_sight(e->x, e->y, pl.x, pl.y)) {
            e->state = AI_CHARGE;
            e->timer = 18;
            e->vx = e->vy = 0;
        } else if (d < reach) {
            if (!e->cd) {
                e->state = AI_ATTACK; /* rear up: a readable warning before the bite */
                e->timer = 12;
            }
        } else {
            enemy_steer(e, pl.x, pl.y, t->speed);
            if (((ticks + e->anim) & 63) == 0 && d < 5 * ONE) sfx(SFX_SKITTER);
        }
        break;
    case AI_ATTACK:
        if (--e->timer == 0) {
            if (d < reach + 48 && line_of_sight(e->x, e->y, pl.x, pl.y))
                player_damage(t->dmg, DMG_BITE);
            e->state = AI_CHASE;
            e->cd = 30;
        }
        break;
    case AI_SPIT:
        if (--e->timer == 0) {
            proj_spawn(PJ_SPIT, e->x, e->y, pl.x, pl.y, e->type == E_ELITE ? 44 : 34, 8);
            sfx(SFX_SPIT);
            e->state = AI_CHASE;
            e->cd = (uint8_t)(t->spit_cd + (rnd() & 31));
        }
        break;
    case AI_CHARGE:
        if (e->vx == 0 && e->vy == 0) {        /* wind-up: shell opens, it glares */
            if (--e->timer == 0) {
                aim(e->x, e->y, pl.x, pl.y, 64, &e->vx, &e->vy);
                if (e->vx == 0 && e->vy == 0) e->vx = 1;
                e->timer = 24;
            }
        } else {
            int blocked = move_actor(&e->x, &e->y, e->vx, e->vy, t->radius * 3 / 4);
            if (d < t->radius + 80) {
                if (pl.shield_on) {           /* bounces off the feline barrier */
                    sfx(SFX_BOING);
                    player_damage(t->dmg, DMG_BITE);
                    e->state = AI_HURT;
                    e->timer = 45;
                    e->flags |= EF_EXPOSED;
                    aim(pl.x, pl.y, e->x, e->y, 12, &e->vx, &e->vy);
                    message("BOING! ITS BELLY IS EXPOSED. POUNCE!", 45);
                } else {
                    player_damage(t->dmg, DMG_BITE);
                    e->state = AI_CHASE;
                    e->cd = 60;
                }
            } else if (blocked) {             /* into a wall: dazed */
                sfx(SFX_THUD);
                e->state = AI_HURT;
                e->timer = 30;
                e->flags |= EF_EXPOSED;
                e->vx = e->vy = 0;
            } else if (--e->timer == 0) {
                e->state = AI_CHASE;
                e->cd = 60;
            }
        }
        break;
    case AI_HURT:
        move_actor(&e->x, &e->y, e->vx, e->vy, t->radius * 3 / 4);
        e->vx = (int16_t)(e->vx * 3 / 4);
        e->vy = (int16_t)(e->vy * 3 / 4);
        if (--e->timer == 0) {
            e->state = AI_CHASE;
            e->flags &= (uint8_t)~EF_EXPOSED;
            if (t->flags & ETF_CHARGE) e->cd = 40;
        }
        break;
    default:
        break;
    }
}

static void enemies_update(void) {
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        enemy_t *e = &enemies[i];
        if (!e->active) continue;
        if (e->flash) e->flash--;
        if (e->state == AI_DEAD) {
            if (e->timer && --e->timer == 0) e->active = 0;
            continue;
        }
        if (e->type == E_ANCHOR) continue;
        if (e->type == E_BOSS) {
            boss_think(e, i);
            continue;
        }
        if (e->state == AI_IDLE && e->z > 0) {
            e->z = (int16_t)IMAX(0, e->z - 24); /* dropping in from above */
            if (e->z == 0) {
                sfx(SFX_THUD);
                e->state = AI_CHASE;
            }
            continue;
        }
        enemy_think(e);
    }
    enemies_separate();
}

/* Animation frame and flags for the renderer. */
static void enemy_sprite(const enemy_t *e, int i, int *spr, int *pal, int *flags) {
    const etype_t *t = &etypes[e->type];
    int walk = (int)((ticks >> 2) + (uint32_t)i) & 1;
    int beetle = t->spr == SPR_BEETLE_A;
    *pal = t->pal;
    *flags = e->flash ? VF_FLASH : 0;
    if (e->type == E_ANCHOR) {
        *spr = SPR_ANCHOR;
        if (e->flags & EF_INVULN) *flags |= VF_DARK;
        return;
    }
    if (e->state == AI_DEAD) {
        *spr = beetle ? SPR_BEETLE_DEAD : SPR_ROACH_DEAD;
        return;
    }
    if (beetle) {
        *spr = (e->state == AI_CHARGE && e->vx == 0 && e->vy == 0) || (e->flags & EF_EXPOSED)
                   ? SPR_BEETLE_OPEN : (walk ? SPR_BEETLE_B : SPR_BEETLE_A);
        if (e->state == AI_CHARGE && e->vx == 0 && (ticks & 2)) *flags |= VF_FLASH;
    } else {
        *spr = (e->state == AI_ATTACK || e->state == AI_SPIT) ? SPR_ROACH_ATK
                                                              : (walk ? SPR_ROACH_B : SPR_ROACH_A);
    }
    if (walk && e->state == AI_CHASE) *flags |= VF_FLIP;
}
