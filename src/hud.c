/*
 * hud.c -- the status bar, the overlays drawn over the 3D view, the two
 * firmware status bands and every full-screen page (title, story, death,
 * game over, ending).
 *
 * The status bar is recomposed only when one of the values it shows has
 * changed (hud_signature), and never more than ~10 times per second, so
 * most frames send only the 3D view to the display.
 */

#define HUD_BG CUBE(1, 0, 1)
#define COL_TEXT CUBE(5, 5, 4)
#define COL_DIM CUBE(3, 2, 3)
#define COL_RED CUBE(5, 1, 1)
#define COL_GREEN CUBE(2, 5, 1)
#define COL_CYAN CUBE(1, 4, 5)
#define COL_GOLD CUBE(5, 4, 0)
#define COL_PINK CUBE(5, 2, 4)

static uint32_t hud_sig_last = 0xffffffffu;
static int hud_force, hud_age;
static char band_top_last[48], band_bottom_last[48];

static int dazed_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_PICKUPS; ++i)
        if (pickups[i].active && pickups[i].type == PK_DAZED) n++;
    return n;
}

static int fed_face(void) {
    if (pl.health <= 0 || pl.hurt) return SPR_FED_OUCH;
    if (pl.shield_on || state == ST_BOSS) return SPR_FED_FIERCE;
    return SPR_FED_OK;
}

static uint32_t hud_signature(void) {
    uint32_t s = (uint32_t)pl.health;
    s = s * 31u + (uint32_t)cats_max;
    s = s * 31u + (uint32_t)cats_out;
    s = s * 31u + (uint32_t)dazed_count();
    s = s * 31u + (uint32_t)(pl.shield / 4);
    s = s * 31u + (uint32_t)(pl.shield_on | (pl.shield_broken << 1) | ((pl.poison > 0) << 2) |
                             ((pl.yarn > 0) << 3));
    s = s * 31u + (uint32_t)fed_face();
    s = s * 31u + (uint32_t)level;
    if (pl.poison || (pl.health > 0 && pl.health < 25)) s = s * 31u + ((ticks >> 3) & 1);
    return s;
}

static int hud_needs_redraw(void) {
    uint32_t sig = hud_signature();
    hud_age++;
    if (!hud_force && (sig == hud_sig_last || hud_age < 3)) return 0;
    hud_sig_last = sig;
    hud_force = 0;
    hud_age = 0;
    return 1;
}

static void bar(int x, int y, int w, int h, int value, int max, uint8_t fg, uint8_t bg) {
    int fill = max > 0 ? ICLAMP(value, 0, max) * (w - 2) / max : 0;
    op_rect(x, y, w, h, CUBE(0, 0, 0));
    op_rect(x + 1, y + 1, w - 2, h - 2, bg);
    if (fill > 0) op_rect(x + 1, y + 1, fill, h - 2, fg);
}

static void hud_ops(void) {
    int y = HUD_Y;
    op_rect(0, y, SW, 2, CUBE(3, 1, 3));
    /* Fed's portrait */
    op_rect(4, y + 6, 52, 52, CUBE(0, 0, 0));
    op_rect(5, y + 7, 50, 50, CUBE(2, 1, 3));
    op_sprite(fed_face(), PAL_FED, 6, y + 8, 48, 48, 0);
    /* health */
    op_text(64, y + 6, 1, COL_DIM, 0, "FED");
    sp_begin();
    sp_int(pl.health);
    uint8_t hc = pl.health < 25 ? ((ticks >> 3) & 1 ? COL_RED : COL_TEXT) :
                 pl.health < 50 ? COL_GOLD : COL_TEXT;
    op_text(64, y + 17, 3, hc, OPF_SHADOW, sp_end());
    bar(64, y + 44, 80, 7, pl.health, 100, pl.health < 25 ? COL_RED : COL_GREEN, CUBE(1, 0, 0));
    /* the armada */
    sp_begin();
    sp_str("CATS ");
    sp_int(cats_ready());
    sp_char('/');
    sp_int(cats_max);
    op_text(156, y + 6, 1, COL_TEXT, 0, sp_end());
    int dz = dazed_count();
    if (dz) {
        sp_begin();
        sp_int(dz);
        sp_str(" DAZED");
        op_text(244, y + 6, 1, COL_RED, 0, sp_end());
    }
    for (int k = 0; k < cats_max; ++k)
        op_sprite(SPR_CAT_SIT, PAL_CAT_ORANGE + k % 5, 156 + k * 13, y + 16, 12, 12,
                  k >= cats_ready() ? VF_DARK : 0);
    /* the feline shield */
    sp_begin();
    sp_str("SHIELD ");
    if (pl.shield_broken) sp_str("BROKEN");
    else {
        sp_int(pl.shield * 100 / IMAX(1, shield_max()));
        sp_char('%');
        if (pl.shield_on) sp_str(" UP");
    }
    op_text(156, y + 32, 1, pl.shield_broken ? COL_RED : pl.shield_on ? COL_CYAN : COL_TEXT, 0, sp_end());
    bar(156, y + 43, 150, 7, pl.shield, 100, pl.shield_broken ? COL_RED : pl.yarn ? COL_PINK : COL_CYAN,
        CUBE(0, 0, 1));
    if (shield_max() < 100) op_rect(156 + shield_max() * 148 / 100 + 1, y + 43, 1, 7, COL_DIM);
    if (pl.poison && ((ticks >> 3) & 1)) op_text(64, y + 54, 1, COL_GREEN, 0, "POISONED");
    if (pl.yarn) op_text(156, y + 54, 1, COL_PINK, 0, "YARN POWER!");
}

/* ---- firmware status bands ------------------------------------------ */

static void band_text(int band, const char *text) {
    char *last = band == PRG32_BAND_TOP ? band_top_last : band_bottom_last;
    int i = 0;
    while (text[i] && last[i] == text[i]) i++;
    if (text[i] == 0 && last[i] == 0) return;
    str_copy(last, text, 48);
    prg32_band_set_text((uint8_t)band, text);
}

static void bands_update(void) {
    int world = world_state(state);
    sp_begin();
    if (world) {
        /* "3/12 KITCHEN - GROUND FLOOR, BACK", the place only if it fits */
        sp_int(level + 1);
        sp_str("/12 ");
        sp_str(LV_TEXT(name));
        if (str_len(LV_TEXT(name)) + str_len(LV_TEXT(place)) + 8 <= 40) {
            sp_str(" - ");
            sp_str(LV_TEXT(place));
        }
    } else {
        sp_str("COCKROACHES CATHISTERIA");
    }
    band_text(PRG32_BAND_TOP, sp_end());
    sp_begin();
    if (!world) {
        sp_str(state == ST_ENDING ? "THANK YOU FOR PLAYING" :
               state == ST_GAME_OVER ? "THE ROACHES WIN THIS ROUND" :
               "A PRG32 CARTRIDGE - HYDE PARK, CHICAGO");
    } else if (msg_ticks) {
        sp_str(msg_text);
    } else if (state == ST_PLAYING && !exit_open) {
        sp_str("CLEAR THE ROOM: ");
        sp_int(enemies_alive(0, EF_OBJECTIVE));
        sp_str(" LEFT  (WAVE ");
        sp_int(wave);
        sp_char('/');
        sp_int(waves_total);
        sp_char(')');
    } else if (state == ST_PLAYING) {
        sp_str("THE EXIT IS OPEN: FIND IT!");
    } else if (state == ST_BOSS) {
        sp_str("DEFEAT THE BOSS! B SHIELDS, A POUNCES");
    } else if (state == ST_FINAL_PORTAL) {
        sp_str("DESTROY THE ANCHORS: ");
        sp_int(anchors_left);
        sp_str(" LEFT");
    } else {
        sp_str("A: CAT  B: SHIELD  SELECT+DIR: STRAFE");
    }
    band_text(PRG32_BAND_BOTTOM, sp_end());
}

/* ---- overlays on the 3D view ----------------------------------------- */

static int last_variant(void) { return (cat_launches + 4) % 5; }

static void border(int w, uint8_t col) {
    op_rect(0, 0, SW, w, col);
    op_rect(0, VIEW_H - w, SW, w, col);
    op_rect(0, 0, w, VIEW_H, col);
    op_rect(SW - w, 0, w, VIEW_H, col);
}

static void boss_bar_ops(void) {
    int hp = 0, max = 0, first = -1;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &enemies[i];
        if (!e->active || !(e->flags & EF_BOSS)) continue;
        if (first < 0) first = i;
        hp += e->hp;
        max += e->maxhp;
    }
    if (first < 0 || state != ST_BOSS) return;
    const char *name = lv->boss == BOSS_QUEEN ? "THE INVASION LEADERS" : boss_defs[enemies[first].boss].name;
    op_text(0, 4, 1, COL_GOLD, OPF_CENTER | OPF_SHADOW, name);
    bar(60, 14, 200, 6, hp, max, COL_RED, CUBE(1, 0, 0));
}

static void view_ops(void) {
    int bob = pl.shield_on ? 0 : (isin(pl.bob * 40) >> 12);
    int ready = cats_ready();
    /* the armada peeking in at the bottom corners */
    if (!pl.shield_on && ready >= 1)
        op_sprite(SPR_CAT_SIT, PAL_CAT_BLACK, 4, VIEW_H - 30 + bob, 44, 44, 0);
    if (!pl.shield_on && ready >= 2)
        op_sprite(SPR_CAT_SIT, PAL_CAT_CALICO, SW - 48, VIEW_H - 30 - bob, 44, 44, VF_FLIP);
    /* a cat leaping away from Fed towards the target */
    if (pl.leap) {
        int t = 8 - pl.leap, size = 80 - t * 7;
        op_sprite(SPR_CAT_LEAP, PAL_CAT_ORANGE + last_variant(), SW / 2 - size / 2,
                  VIEW_H - size + 16 - t * 10, size, size, 0);
    }
    /* the feline shield: cats circling Fed, claws out */
    if (pl.shield_on) {
        int n = IMIN(6, IMAX(1, ready));
        for (int k = 0; k < n; ++k) {
            int a = (int)ticks * 10 + k * 1024 / n;
            int x = SW / 2 + (icos(a) * 140 >> 14), y = HORIZON + (isin(a) * 50 >> 14);
            op_sprite(SPR_CAT_POUNCE, PAL_CAT_ORANGE + k % 5, x - 16, y - 16, 32, 32, 0);
        }
        border(2, pl.shield_hit ? CUBE(5, 5, 5) : pl.yarn ? COL_PINK : COL_CYAN);
    }
    if (pl.hurt) border(3, COL_RED);
    else if (pl.poison && ((ticks >> 2) & 1)) border(2, COL_GREEN);
    if (quake && (ticks & 1)) border(1, CUBE(5, 5, 5));
    /* crosshair: gold when a cat would find a target */
    uint8_t cross = cat_find_target() >= 0 ? COL_GOLD : CUBE(4, 4, 4);
    op_rect(SW / 2 - 4, HORIZON, 3, 1, cross);
    op_rect(SW / 2 + 2, HORIZON, 3, 1, cross);
    op_rect(SW / 2, HORIZON - 3, 1, 2, cross);
    op_rect(SW / 2, HORIZON + 2, 1, 2, cross);
    boss_bar_ops();
    if (state == ST_BOSS && state_timer < 70) {  /* boss introduction */
        const char *who = lv->boss == BOSS_QUEEN ? "THE QUEEN AND GENERAL" : boss_defs[lv->boss].name;
        op_text(0, 34, 2, COL_RED, OPF_CENTER | OPF_SHADOW, who);
        op_text(0, 54, 1, COL_TEXT, OPF_CENTER | OPF_SHADOW,
                lv->boss == BOSS_COUCH ? "IT IS THE COUCH." : "A BIGGER COCKROACH APPEARS!");
    }
}

/* ---- full-screen pages and banners -------------------------------------- */

static void banner_box(int y, int h) {
    op_dim(8, y, SW - 16, h);
    op_rect(8, y, SW - 16, 1, COL_GOLD);
    op_rect(8, y + h - 1, SW - 16, 1, COL_GOLD);
}

static void lines(int y, uint8_t col, const char *a, const char *b, const char *c) {
    if (a) op_text(0, y, 1, col, OPF_CENTER | OPF_SHADOW, a);
    if (b) op_text(0, y + 11, 1, col, OPF_CENTER | OPF_SHADOW, b);
    if (c) op_text(0, y + 22, 1, col, OPF_CENTER | OPF_SHADOW, c);
}

static void press_a(int y, const char *text) {
    if ((ticks >> 4) & 1) op_text(0, y, 1, COL_GOLD, OPF_CENTER | OPF_SHADOW, text);
}

static void level_start_ops(void) {
    sp_begin();
    banner_box(10, 116);
    sp_str("LEVEL ");
    sp_int(level + 1);
    sp_str(" OF 12");
    op_text(0, 16, 1, COL_DIM, OPF_CENTER, sp_end());
    op_text(0, 28, str_len(LV_TEXT(name)) > 12 ? 2 : 3, COL_GOLD, OPF_CENTER | OPF_SHADOW, LV_TEXT(name));
    op_text(0, 56, 1, COL_CYAN, OPF_CENTER, LV_TEXT(place));
    lines(72, COL_TEXT, LV_TEXT(intro[0]), LV_TEXT(intro[1]), 0);
    sp_begin();
    sp_str("FED: ");
    sp_str(LV_TEXT(quip));
    op_text(0, 98, 1, COL_PINK, OPF_CENTER | OPF_SHADOW, sp_end());
    press_a(112, "PRESS A");
}

static void room_cleared_ops(void) {
    op_text(0, 30, 2, COL_GREEN, OPF_CENTER | OPF_SHADOW, "ROOM CLEARED!");
    if ((ticks >> 3) & 1) op_text(0, 56, 1, COL_RED, OPF_CENTER | OPF_SHADOW, "SOMETHING BIG IS COMING...");
}

static void level_complete_ops(void) {
    banner_box(8, 120);
    op_text(0, 14, 2, COL_GREEN, OPF_CENTER | OPF_SHADOW, "LEVEL CLEARED!");
    op_text(0, 34, 1, COL_GOLD, OPF_CENTER, LV_TEXT(name));
    lines(50, COL_TEXT, LV_TEXT(outro[0]), LV_TEXT(outro[1]), 0);
    sp_begin();
    sp_str("ROACHES SQUASHED: ");
    sp_int(squashed);
    op_text(0, 80, 1, COL_CYAN, OPF_CENTER, sp_end());
    sp_begin();
    sp_str("CATS IN THE ARMADA: ");
    sp_int(cats_max);
    op_text(0, 92, 1, COL_CYAN, OPF_CENTER, sp_end());
    press_a(112, "PRESS A TO GO DEEPER");
}

static void dead_ops(void) {
    op_dim(0, 0, SW, VIEW_H);
    op_rect(0, 0, SW, 2, COL_RED);
    op_text(0, 14, 2, COL_RED, OPF_CENTER | OPF_SHADOW, "HUMANITY HAS FALLEN");
    op_text(0, 36, 1, COL_TEXT, OPF_CENTER | OPF_SHADOW, "THE CATS REMAIN UNIMPRESSED.");
    op_sprite(SPR_FED_OUCH, PAL_FED, 112, 54, 48, 48, 0);
    op_sprite(SPR_CAT_SIT, PAL_CAT_GRAY, 164, 70, 32, 32, 0);
    press_a(110, "A: TRY AGAIN    B: GIVE UP");
}

static void portal_ops(void) {
    sp_begin();
    sp_str("ANCHORS LEFT: ");
    sp_int(anchors_left);
    op_text(0, 4, 1, COL_PINK, OPF_CENTER | OPF_SHADOW, sp_end());
    int phase = portal_timer % 150;
    if (phase >= 105 && ((ticks >> 2) & 1)) {
        op_text(0, 24, 2, COL_GREEN, OPF_CENTER | OPF_SHADOW, "PORTAL SURGE!");
        op_text(0, 44, 1, COL_TEXT, OPF_CENTER | OPF_SHADOW, "HOLD B: FELINE SHIELD");
    }
    if (collapse && (ticks & 1)) op_dim(0, 0, SW, VIEW_H);
}

static int page;

static void cats_row(int y, int n, int size) {
    int x0 = (SW - n * (size + 4)) / 2;
    for (int k = 0; k < n; ++k)
        op_sprite(SPR_CAT_SIT, PAL_CAT_ORANGE + k % 5, x0 + k * (size + 4), y, size, size, k & 1 ? VF_FLIP : 0);
}

static void title_ops(void) {
    if (page == 0) {
        op_text(0, 12, 3, CUBE(5, 3, 0), OPF_CENTER | OPF_SHADOW, "COCKROACHES");
        op_text(0, 40, 3, COL_GREEN, OPF_CENTER | OPF_SHADOW, "CATHISTERIA");
        op_text(0, 70, 1, COL_CYAN, OPF_CENTER, "A CAT-POWERED INVASION DEFENSE");
        op_sprite(SPR_QUEEN_A, PAL_QUEEN, 6, 86, 88, 88, 0);
        op_sprite(SPR_FED_FIERCE, PAL_FED, 124, 92, 72, 72, 0);
        op_sprite(SPR_CAT_POUNCE, PAL_CAT_ORANGE, 222, 90, 80, 80, 0);
        op_sprite(SPR_ROACH_A, PAL_ELITE, 96, 150, 28, 28, 0);
        op_sprite(SPR_ROACH_B, PAL_SPITTER, 196, 152, 26, 26, VF_FLIP);
        press_a(184, "PRESS A    B: HOW TO PLAY");
    } else {
        op_text(0, 10, 2, COL_GOLD, OPF_CENTER | OPF_SHADOW, "HOW TO PLAY");
        op_text(24, 38, 1, COL_TEXT, 0, "UP/DOWN      WALK");
        op_text(24, 50, 1, COL_TEXT, 0, "LEFT/RIGHT   TURN");
        op_text(24, 62, 1, COL_TEXT, 0, "SELECT+L/R   STRAFE");
        op_text(24, 74, 1, COL_TEXT, 0, "A            SEND A CAT");
        op_text(24, 86, 1, COL_TEXT, 0, "B (HOLD)     FELINE SHIELD");
        op_text(24, 98, 1, COL_TEXT, 0, "SELECT       PAUSE");
        lines(118, COL_CYAN, "CATS COME BACK AFTER EACH ATTACK.", "THE SHIELD NEEDS REST TO REGROW.",
              "RESCUE LOST CATS. EAT PIZZA.");
        cats_row(158, 5, 20);
        press_a(186, "PRESS A TO START");
    }
}

static void intro_ops(void) {
    if (page == 0) {
        lines(20, COL_GOLD, "HYDE PARK, CHICAGO.", "SOUTH HARPER AVENUE.", 0);
        lines(56, COL_TEXT, "FED LIVES IN A THREE-STORY HOUSE", "WITH HER FORMIDABLE", "FELINE ARMADA.");
        cats_row(110, 7, 26);
    } else if (page == 1) {
        lines(20, COL_TEXT, "ONE NIGHT, A PORTAL OPENS", "IN HER BASEMENT.", 0);
        lines(56, COL_GREEN, "GIANT ALIEN COCKROACHES", "AND BEETLES POUR OUT.", 0);
        op_sprite(SPR_QUEEN_A, PAL_QUEEN, 116, 90, 88, 88, 0);
        op_sprite(SPR_BEETLE_A, PAL_BEETLE, 40, 120, 56, 56, 0);
        op_sprite(SPR_ROACH_A, PAL_ROACH, 226, 124, 52, 52, VF_FLIP);
    } else {
        lines(16, COL_TEXT, "HUMANITY'S FIRST LINE OF DEFENSE", "IS A LOVELY, ECCENTRIC WOMAN.", 0);
        lines(44, COL_GOLD, "HER CATS ARE HER WEAPONS.", 0, 0);
        op_sprite(SPR_FED_FIERCE, PAL_FED, 124, 66, 72, 72, 0);
        cats_row(146, 6, 24);
    }
    press_a(186, "PRESS A");
}

static void ending_ops(void) {
    if (page == 0) {
        int s = IMAX(8, 160 - state_timer * 2);
        op_sprite(SPR_PORTAL, PAL_PORTAL, SW / 2 - s / 2, 90 - s / 2, s, s, (ticks >> 2) & 1 ? VF_FLIP : 0);
        lines(160, COL_TEXT, "THE PORTAL COLLAPSES", "WITH A SMALL, SAD POP.", 0);
    } else if (page == 1) {
        lines(24, COL_TEXT, "THE BASEMENT FALLS SILENT.", "THE LAST ROACH EVAPORATES.", "FED DUSTS OFF HER CARDIGAN.");
        op_sprite(SPR_FED_OK, PAL_FED, 124, 76, 72, 72, 0);
    } else if (page == 2) {
        lines(18, COL_TEXT, "SHE CLIMBS THE STAIRS.", "THE CATS FOLLOW AS IF NOTHING", "UNUSUAL HAPPENED.");
        lines(60, COL_PINK, "THEY DEMAND DINNER. IMMEDIATELY.", 0, 0);
        op_sprite(SPR_FED_OK, PAL_FED, 136, 80, 48, 48, 0);
        cats_row(140, 9, 24);
    } else {
        op_text(0, 14, 3, COL_GOLD, OPF_CENTER | OPF_SHADOW, "HUMANITY");
        op_text(0, 42, 3, COL_GREEN, OPF_CENTER | OPF_SHADOW, "SAVED");
        lines(80, COL_TEXT, "COCKROACHES CATHISTERIA", "A PRG32 CARTRIDGE", 0);
        lines(108, COL_CYAN, "CONCEPT: RAFFAELE MONTELLA", "CODE AND PIXELS: CLAUDE CODE", "UNIPARTHENOPE - RISC-V PRG32");
        lines(146, COL_DIM, "NO CATS WERE HARMED.", "SEVERAL COUCHES WERE.", 0);
    }
    press_a(186, page < 3 ? "PRESS A" : "PRESS A FOR THE TITLE");
}

static void game_over_ops(void) {
    op_text(0, 40, 3, COL_RED, OPF_CENTER | OPF_SHADOW, "GAME OVER");
    lines(84, COL_TEXT, "THE ROACHES KEEP THE HOUSE.", "THE CATS MOVE IN WITH", "THE NEIGHBORS.");
    op_sprite(SPR_CAT_SIT, PAL_CAT_BLACK, 144, 124, 32, 32, 0);
    press_a(180, "PRESS A");
}
