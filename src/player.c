/*
 * player.c -- Fed: movement, the feline shield, damage and pickups.
 *
 * Controls (classic Doom-style movement):
 *   UP / DOWN       walk forward / back
 *   LEFT / RIGHT    turn (hold SELECT to strafe instead)
 *   A               send a cat at the enemy in the crosshair (hold to repeat)
 *   B               hold to raise the feline shield
 *   SELECT (tap)    pause
 *
 * The shield is one number, pl.shield. It drains while held and regrows
 * after a short delay when released; its maximum grows with the armada, so
 * losing cats weakens the shield. When it runs dry it breaks and must
 * regrow to 30 before it can be raised again: never permanent safety.
 */

#define PLAYER_R 64
#define WALK_SPEED 30
#define BACK_SPEED 20
#define TURN_SPEED 20

static const char cat_names[MAX_CATS][10] = {
    "PUDDING", "MOCHA", "NAPOLEON", "BISCUIT", "MARGOT", "ZIGGY", "BARTOLO",
    "TIGERLILY", "SERGEANT",
};

static char msg_text[40];
static int msg_ticks, msg_serial;

static void message(const char *text, int t) {
    str_copy(msg_text, text, sizeof(msg_text));
    msg_ticks = t;
    msg_serial++;
}

/* "<NAME><suffix>" for cat number n (0-based). */
static void cat_message(int n, const char *suffix) {
    sp_begin();
    sp_str(cat_names[ICLAMP(n, 0, MAX_CATS - 1)]);
    sp_str(suffix);
    message(sp_end(), 60);
}

static int shield_max(void) { return IMIN(100, 40 + 10 * cats_max); }

static void player_damage(int amount, int kind) {
    if (pl.health <= 0 || state == ST_PLAYER_DEAD) return;
    /* A short grace period after each hit: a volley of spit or a pile of
     * roaches hurts, but cannot delete Fed in a single frame. */
    if (pl.iframes && (kind == DMG_BITE || kind == DMG_POISON || kind == DMG_SHOCK)) return;
    if (pl.shield_on && kind != DMG_PUDDLE && kind != DMG_FALL) {
        int cost = kind == DMG_POISON ? 8 : kind == DMG_DEBRIS ? 10 :
                   kind == DMG_PORTAL ? 15 : amount + 6;
        if (pl.yarn) cost /= 2;
        pl.shield_hit = 6;
        sfx(SFX_SHIELD_HIT);
        if (pl.shield >= cost) {
            pl.shield -= cost;
            return;
        }
        amount /= 2; /* the barrier breaks and softens the blow */
        pl.shield = 0;
        pl.shield_on = 0;
        pl.shield_broken = 1;
        message("THE FELINE SHIELD BROKE!", 45);
    }
    if (CC_TEST_GOD) amount = 0;
    pl.health -= amount;
    pl.hurt = 8;
    pl.iframes = 8;
    sfx(SFX_HURT);
    if (kind == DMG_POISON) {
        pl.poison = 90;
        if (rnd() % 4 == 0) cats_lose_one(pl.x, pl.y); /* a spooked cat hides */
    }
    if (pl.health <= 0) {
        pl.health = 0;
        game_set_state(ST_PLAYER_DEAD);
    }
}

static void shield_update(uint32_t in) {
    int want = (in & PRG32_BTN_B) != 0;
    int top = shield_max();
    if (pl.shield_broken && pl.shield >= 30) pl.shield_broken = 0;
    if (want && !pl.shield_broken && pl.shield > 0) {
        if (!pl.shield_on) sfx(SFX_SHIELD_ON);
        pl.shield_on = 1;
        if (!pl.yarn || (ticks & 1)) pl.shield--;
        pl.shield_delay = 15;
        if (pl.shield <= 0) {
            pl.shield = 0;
            pl.shield_on = 0;
            pl.shield_broken = 1;
            message("SHIELD EXHAUSTED! LET THE CATS REST.", 45);
        }
    } else {
        pl.shield_on = 0;
        if (pl.shield_delay) pl.shield_delay--;
        else if (ticks & 1) pl.shield++;
    }
    pl.shield = ICLAMP(pl.shield, 0, top);
}

/* Try one axis step, refusing to walk into an enemy. */
static void player_step(int dx, int dy) {
    int32_t ox = pl.x, oy = pl.y;
    move_actor(&pl.x, &pl.y, dx, 0, PLAYER_R);
    if (enemy_blocks(pl.x, pl.y, PLAYER_R)) pl.x = ox;
    move_actor(&pl.x, &pl.y, 0, dy, PLAYER_R);
    if (enemy_blocks(pl.x, pl.y, PLAYER_R)) pl.y = oy;
}

static void pickups_update(void) {
    for (int i = 0; i < MAX_PICKUPS; ++i) {
        pickup_t *p = &pickups[i];
        if (!p->active) continue;
        if (p->wait) {
            p->wait--;
            continue;
        }
        if (IABS(p->x - pl.x) > 110 || IABS(p->y - pl.y) > 110) continue;
        switch (p->type) {
        case PK_CAT:
            if (cats_max < MAX_CATS) {
                cats_max++;
                cat_message(cats_max - 1, " JOINS THE ARMADA!");
            } else {
                pl.health = IMIN(100, pl.health + 10);
                message("THE ARMADA IS FULL. FREE CUDDLES.", 60);
            }
            sfx(SFX_MEOW);
            break;
        case PK_DAZED:
            if (cats_max < MAX_CATS) cats_max++;
            cat_message(cats_max - 1, " SHAKES IT OFF AND RETURNS!");
            sfx(SFX_MEOW);
            break;
        case PK_HEALTH:
            if (pl.health >= 100) continue; /* leave it for later */
            pl.health = IMIN(100, pl.health + 35);
            message("PIZZA ALLA NERANO! FED FEELS BETTER.", 60);
            sfx(SFX_PICKUP);
            break;
        case PK_FOOD:
            pl.shield = shield_max();
            pl.shield_broken = 0;
            pl.poison = 0;
            message("CAT FOOD! THE ARMADA IS RECHARGED.", 60);
            sfx(SFX_PICKUP);
            break;
        default: /* PK_YARN */
            pl.yarn = 300;
            pl.shield = shield_max();
            pl.shield_broken = 0;
            message("YARN BALL! SUPER SHIELD FOR 10 SECONDS.", 60);
            sfx(SFX_PICKUP);
            break;
        }
        p->active = 0;
    }
}

static void pickup_spawn(int type, int32_t x, int32_t y, int variant) {
    for (int i = 0; i < MAX_PICKUPS; ++i) {
        pickup_t *p = &pickups[i];
        if (p->active) continue;
        p->active = 1;
        p->type = (uint8_t)type;
        p->x = x;
        p->y = y;
        p->variant = (uint8_t)variant;
        p->wait = type == PK_DAZED ? 45 : 0;
        return;
    }
}

static void player_update(uint32_t in) {
    int fwd = 0, strafe = 0, speed;
    if (in & PRG32_BTN_SELECT) {
        if (in & PRG32_BTN_LEFT) strafe = -1;
        if (in & PRG32_BTN_RIGHT) strafe = 1;
    } else {
        if (in & PRG32_BTN_LEFT) pl.ang -= TURN_SPEED;
        if (in & PRG32_BTN_RIGHT) pl.ang += TURN_SPEED;
    }
    pl.ang &= 1023;
    if (in & PRG32_BTN_UP) fwd = WALK_SPEED;
    if (in & PRG32_BTN_DOWN) fwd = -BACK_SPEED;
    speed = pl.shield_on ? 3 : 5;
    if (fwd || strafe) {
        int c = icos(pl.ang), s = isin(pl.ang);
        int dx = (c * fwd - s * strafe * 24) >> 14;
        int dy = (s * fwd + c * strafe * 24) >> 14;
        player_step(dx * speed / 5, dy * speed / 5);
        pl.bob++;
    }

    int t = tile_at(pl.x, pl.y);
    if (t == T_HOLE) {
        pl.x = pl.safe_x;
        pl.y = pl.safe_y;
        sfx(SFX_FALL);
        message("FED GRABS THE GUTTER JUST IN TIME!", 60);
        player_damage(12, DMG_FALL);
    } else if (!solid_at(pl.x + 90, pl.y) && tile_at(pl.x + 90, pl.y) != T_HOLE &&
               tile_at(pl.x - 90, pl.y) != T_HOLE && tile_at(pl.x, pl.y + 90) != T_HOLE &&
               tile_at(pl.x, pl.y - 90) != T_HOLE) {
        pl.safe_x = pl.x;
        pl.safe_y = pl.y;
    }
    if (exit_open && t == T_EXIT) {
        level_complete();
        return;
    }

    shield_update(in);
    if (in & PRG32_BTN_A) cats_launch();

    if (pl.poison) {
        pl.poison--;
        if (pl.poison % 30 == 29) player_damage(1, DMG_PUDDLE);
    }
    if (pl.cat_cd) pl.cat_cd--;
    if (pl.leap) pl.leap--;
    if (pl.hurt) pl.hurt--;
    if (pl.iframes) pl.iframes--;
    if (pl.shield_hit) pl.shield_hit--;
    if (pl.yarn) pl.yarn--;
    pickups_update();
}
