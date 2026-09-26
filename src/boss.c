/*
 * boss.c -- one reusable boss framework for every room boss and the two
 * invasion leaders.
 *
 * A boss is an ordinary enemy slot (type E_BOSS, flag EF_BOSS) with a row in
 * boss_defs[]: sprite, palette, size, hit points and a bitmask of attack
 * patterns. Bosses are not just bigger roaches: each combines a few of
 *
 *   CHARGE  flashing wind-up, then a straight rush that stuns it on a wall
 *   RAPID   a quick volley of poison spit
 *   BURST   a ring of poison and a puddle underneath
 *   SUMMON  calls minions of its own type
 *   SHELL   invulnerable for a while (cats bounce off), then exposed
 *   JUMP    leaps out of view and lands on Fed's last position
 *
 * Below half health a boss enters phase two: faster, with extra patterns.
 */

#define BP_CHARGE 1
#define BP_RAPID 2
#define BP_BURST 4
#define BP_SUMMON 8
#define BP_SHELL 16
#define BP_JUMP 32

enum { B_CHASE, B_WINDUP, B_CHARGE, B_RAPID, B_BURST, B_SUMMON, B_SHELL, B_JUMP, B_STUN };

typedef struct {
    char name[18];
    uint8_t spr, pal;
    uint16_t hp;
    uint8_t speed, dmg;
    uint16_t ww, wh;
    uint8_t patterns, patterns2, minion;
} boss_def_t;

static const boss_def_t boss_defs[BOSS_COUNT] = {
    {"LAWN ROACH", SPR_ROACH_A, PAL_LAWN, 90, 14, 10, 300, 220, BP_CHARGE, BP_SUMMON, E_ROACH},
    {"GRILL BEETLE", SPR_BEETLE_A, PAL_GRILL, 140, 10, 14, 320, 250, BP_SHELL | BP_CHARGE, BP_BURST, E_ROACH},
    {"CRUMB BARON", SPR_ROACH_A, PAL_CRUMB, 150, 14, 10, 300, 220, BP_SUMMON | BP_RAPID, BP_BURST, E_RUNNER},
    {"SOOT SWEEPER", SPR_ROACH_A, PAL_SOOTBOSS, 160, 16, 12, 300, 230, BP_JUMP | BP_RAPID, BP_SUMMON, E_ROACH},
    {"GUTTER GARGOYLE", SPR_ROACH_A, PAL_GUTTER, 170, 18, 12, 320, 230, BP_BURST | BP_CHARGE, BP_RAPID, E_FLYER},
    {"DUST MATRIARCH", SPR_BEETLE_A, PAL_DUST, 220, 10, 14, 340, 260, BP_SUMMON | BP_SHELL, BP_BURST, E_SPITTER},
    {"BATHTUB BEHEMOTH", SPR_BEETLE_A, PAL_BATHTUB, 240, 12, 16, 360, 270, BP_CHARGE | BP_RAPID, BP_JUMP, E_RUNNER},
    {"LIBRARIAN BEETLE", SPR_BEETLE_A, PAL_LIBRARIAN, 260, 11, 16, 340, 260, BP_SHELL | BP_BURST, BP_SUMMON, E_SPITTER},
    {"BUTLER ROACH", SPR_ROACH_A, PAL_BUTLER, 260, 18, 14, 320, 240, BP_JUMP | BP_SUMMON, BP_RAPID, E_ELITE},
    {"COUCH COLOSSUS", SPR_BEETLE_A, PAL_COUCH, 520, 12, 20, 520, 380, BP_CHARGE | BP_SUMMON | BP_BURST,
     BP_SHELL | BP_JUMP | BP_RAPID, E_BEETLE},
    {"FURNACE WARDEN", SPR_BEETLE_A, PAL_FURNACE, 380, 12, 18, 400, 300, BP_BURST | BP_CHARGE, BP_SHELL | BP_RAPID, E_ELITE},
    {"COCKROACH QUEEN", SPR_QUEEN_A, PAL_QUEEN, 480, 9, 18, 460, 460, BP_SUMMON | BP_BURST | BP_SHELL, BP_RAPID, E_ELITE},
    {"GENERAL CHITIN", SPR_BEETLE_A, PAL_GENERAL, 360, 14, 18, 400, 300, BP_CHARGE | BP_RAPID | BP_JUMP, BP_BURST, E_ARMORED},
};

#define BOSS_PHASE2 0x80 /* kept in bcount's top bit */

static int boss_spawn_one(int id, int32_t x, int32_t y) {
    int i = enemy_spawn(E_BOSS, x, y, 1);
    if (i < 0) {
        /* Never let a full pool block the boss: recycle a minion slot. */
        for (i = 0; i < MAX_ENEMIES; ++i)
            if (!(enemies[i].flags & (EF_BOSS | EF_OBJECTIVE))) break;
        if (i >= MAX_ENEMIES) return -1;
        enemies[i].active = 0;
        i = enemy_spawn(E_BOSS, x, y, 1);
        if (i < 0) return -1;
    }
    enemy_t *e = &enemies[i];
    e->boss = (uint8_t)id;
    e->hp = e->maxhp = (int16_t)boss_defs[id].hp;
    e->flags = EF_BOSS;
    e->bstate = B_CHASE; /* still AI_IDLE: it drops in from above first */
    e->btimer = 60;
    return i;
}

static void boss_spawn_level(void) {
    int n = 0;
    if (boss_spawn_one(lv->boss, boss_x, boss_y) >= 0) n++;
    if (lv->boss == BOSS_QUEEN) {
        int32_t gx = boss_x + (can_stand(boss_x + 2 * ONE, boss_y, 120) ? 2 * ONE : -2 * ONE);
        if (boss_spawn_one(BOSS_GENERAL, gx, boss_y) >= 0) n++;
    }
    bosses_spawned = n;
}

static int boss_next_pattern(enemy_t *e) {
    const boss_def_t *b = &boss_defs[e->boss];
    int mask = b->patterns | ((e->bcount & BOSS_PHASE2) ? b->patterns2 : 0);
    for (int k = 1; k <= 6; ++k) {
        int bit = ((e->bcount & 7) + k) % 6;
        if (mask & (1 << bit)) {
            e->bcount = (uint8_t)((e->bcount & BOSS_PHASE2) | bit);
            return 1 << bit;
        }
    }
    return BP_CHARGE;
}

static void boss_think(enemy_t *e, int i) {
    const boss_def_t *b = &boss_defs[e->boss];
    int phase2 = (e->bcount & BOSS_PHASE2) != 0;
    int speed = phase2 ? b->speed * 13 / 10 : b->speed;
    int32_t d = dist_q8(pl.x - e->x, pl.y - e->y);
    (void)i;

    if (e->state == AI_IDLE && e->z > 0) { /* drop-in entrance */
        e->z = (int16_t)IMAX(0, e->z - 20);
        if (!e->z) {
            sfx(SFX_THUD);
            quake = 8;
            e->state = AI_CHASE;
        }
        return;
    }
    if (!phase2 && e->hp * 2 < e->maxhp) {
        e->bcount |= BOSS_PHASE2;
        sp_begin();
        sp_str(b->name);
        sp_str(" IS ENRAGED!");
        message(sp_end(), 60);
        sfx(SFX_WARN);
    }
    switch (e->bstate) {
    case B_CHASE:
        if (d > 200) enemy_steer(e, pl.x, pl.y, speed);
        if (e->cd) e->cd--;
        if (d < 260 && !e->cd && line_of_sight(e->x, e->y, pl.x, pl.y)) {
            player_damage(b->dmg, DMG_BITE);
            e->cd = 30;
        }
        if (e->btimer) {
            e->btimer--;
            break;
        }
        switch (boss_next_pattern(e)) {
        case BP_CHARGE: e->bstate = B_WINDUP; e->btimer = 20; break;
        case BP_RAPID: e->bstate = B_RAPID; e->btimer = phase2 ? 5 : 3; break;
        case BP_BURST: e->bstate = B_BURST; e->btimer = 16; break;
        case BP_SUMMON: e->bstate = B_SUMMON; e->btimer = 20; break;
        case BP_SHELL:
            e->bstate = B_SHELL;
            e->btimer = 70;
            e->flags |= EF_INVULN;
            message("ITS SHELL IS UP! SHIELD AND WAIT.", 45);
            break;
        default:
            e->bstate = B_JUMP;
            e->btimer = 48;
            break;
        }
        break;
    case B_WINDUP:
        if (--e->btimer == 0) {
            aim(e->x, e->y, pl.x, pl.y, phase2 ? 80 : 66, &e->vx, &e->vy);
            e->bstate = B_CHARGE;
            e->btimer = 32;
        }
        break;
    case B_CHARGE: {
        int blocked = move_actor(&e->x, &e->y, e->vx, e->vy, 90);
        if (d < 230) {
            player_damage(b->dmg + 4, DMG_BITE);
            if (pl.shield_on) {
                e->bstate = B_STUN;
                e->btimer = 40;
                e->flags |= EF_EXPOSED;
                sfx(SFX_BOING);
                break;
            }
            e->bstate = B_CHASE;
            e->btimer = 70;
        } else if (blocked) {
            sfx(SFX_THUD);
            quake = 6;
            e->bstate = B_STUN;
            e->btimer = 40;
            e->flags |= EF_EXPOSED;
        } else if (--e->btimer == 0) {
            e->bstate = B_CHASE;
            e->btimer = 60;
        }
        break;
    }
    case B_RAPID:
        if ((ticks & 7) == 0) {
            proj_spawn(PJ_BOSS_SPIT, e->x, e->y, pl.x, pl.y, 46, 10);
            sfx(SFX_SPIT);
            if (--e->btimer == 0) {
                e->bstate = B_CHASE;
                e->btimer = 70;
            }
        }
        break;
    case B_BURST:
        if (--e->btimer == 0) {
            int n = phase2 ? 12 : 8;
            for (int k = 0; k < n; ++k) {
                int a = k * 1024 / n + (int)(ticks & 63);
                int32_t sx = e->x + (icos(a) >> 7), sy = e->y + (isin(a) >> 7); /* from its rim */
                proj_spawn(PJ_BOSS_SPIT, sx, sy, e->x + (icos(a) >> 4), e->y + (isin(a) >> 4), 36, 10);
            }
            puddle_spawn(e->x, e->y);
            sfx(SFX_SPIT);
            e->bstate = B_CHASE;
            e->btimer = 80;
        }
        break;
    case B_SUMMON:
        if (--e->btimer == 0) {
            if (enemies_alive(0, EF_BOSS | EF_OBJECTIVE) < 6) {
                for (int k = -1; k <= 1; k += 2) {
                    int32_t mx = e->x + k * 200, my = e->y;
                    if (!can_stand(mx, my, 60)) mx = e->x, my = e->y + k * 200;
                    if (can_stand(mx, my, 60)) {
                        int m = enemy_spawn(b->minion, mx, my, 1);
                        if (m >= 0) enemies[m].z = 200;
                    }
                }
                sfx(SFX_SKITTER);
            }
            e->bstate = B_CHASE;
            e->btimer = 90;
        }
        break;
    case B_SHELL:
        if (--e->btimer == 0) {
            e->flags &= (uint8_t)~EF_INVULN;
            e->flags |= EF_EXPOSED;
            e->bstate = B_STUN;
            e->btimer = 60;
            message("THE SHELL CRACKS OPEN! ATTACK!", 45);
        }
        break;
    case B_JUMP:
        e->btimer--;
        if (e->btimer > 32) {
            e->z = (int16_t)(e->z + 30);                   /* up */
        } else if (e->btimer > 12) {
            if (d > 330) {                                 /* hover: the shadow creeps */
                int32_t ox = e->x, oy = e->y;              /* to a tile short of Fed */
                e->x += (pl.x - e->x) / 6;
                e->y += (pl.y - e->y) / 6;
                if (!can_stand(e->x, e->y, 90)) {
                    e->x = ox;
                    e->y = oy;
                }
            }
        } else {
            e->z = (int16_t)IMAX(0, e->z - 40);            /* crash down */
        }
        if (e->btimer == 0) {
            e->z = 0;
            sfx(SFX_THUD);
            quake = 12;
            if (dist_q8(pl.x - e->x, pl.y - e->y) < 420) player_damage(b->dmg + 4, DMG_SHOCK);
            e->bstate = B_STUN;
            e->btimer = 30;
            e->flags |= EF_EXPOSED;
        }
        break;
    default: /* B_STUN */
        if (--e->btimer == 0) {
            e->flags &= (uint8_t)~EF_EXPOSED;
            e->bstate = B_CHASE;
            e->btimer = phase2 ? 40 : 60;
        }
        break;
    }
}

static void boss_sprite(const enemy_t *e, int *spr, int *pal, int *flags, int *ww, int *wh) {
    const boss_def_t *b = &boss_defs[e->boss];
    int queen = b->spr == SPR_QUEEN_A, beetle = b->spr == SPR_BEETLE_A;
    int walk = (int)(ticks >> 2) & 1;
    int busy = e->bstate == B_WINDUP || e->bstate == B_BURST || e->bstate == B_SUMMON ||
               e->bstate == B_RAPID;
    *pal = b->pal;
    *ww = b->ww;
    *wh = b->wh;
    *flags = e->flash ? VF_FLASH : 0;
    if (e->bstate == B_WINDUP && (ticks & 2)) *flags = VF_FLASH;
    if (e->flags & EF_INVULN) *flags |= VF_DARK;
    if (e->state == AI_DEAD) {
        *spr = queen ? SPR_QUEEN_A : beetle ? SPR_BEETLE_DEAD : SPR_ROACH_DEAD;
        *flags |= VF_DARK;
    } else if (queen) {
        *spr = SPR_QUEEN_A;
        if (busy && (ticks & 4)) *flags |= VF_FLASH;
    } else if (beetle) {
        *spr = (e->flags & EF_EXPOSED) || busy ? SPR_BEETLE_OPEN : (walk ? SPR_BEETLE_B : SPR_BEETLE_A);
    } else {
        *spr = busy ? SPR_ROACH_ATK : (walk ? SPR_ROACH_B : SPR_ROACH_A);
    }
}
