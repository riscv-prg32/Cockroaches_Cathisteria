/*
 * game.h -- the shared vocabulary of every module: enums, records, the
 * global game state and forward declarations.
 *
 * All state lives in fixed-size static arrays (object pools). A pool entry
 * with `active == 0` is free; spawning scans for a free entry and simply
 * does nothing when the pool is full.
 */
#ifndef CC_GAME_H
#define CC_GAME_H

/* ---- top-level game states (game.c owns every transition) -------------- */
enum {
    ST_TITLE,          /* title screen and help page */
    ST_INTRO,          /* the story pages */
    ST_LEVEL_START,    /* level name card over the frozen view */
    ST_PLAYING,        /* enemy waves; after the boss: find the exit */
    ST_ROOM_CLEARED,   /* waves done: warning siren before the boss */
    ST_BOSS,           /* the room boss (or the invasion leaders) */
    ST_PLAYER_DEAD,    /* humorous death screen: retry or give up */
    ST_LEVEL_COMPLETE, /* tally, then the next level */
    ST_FINAL_PORTAL,   /* last level: destroy the portal anchors */
    ST_ENDING,         /* collapse, epilogue and credits */
    ST_GAME_OVER
};

/* ---- level vocabulary used by the generated level_data.h --------------- */
enum { SKY_NONE, SKY_DAY, SKY_DUSK, SKY_NIGHT };
enum { HAZ_NONE, HAZ_DEBRIS, HAZ_HOLES, HAZ_DARK, HAZ_FLICKER, HAZ_PORTAL };
enum { MUS_NONE, MUS_TITLE, MUS_EXPLORE, MUS_TENSE, MUS_BOSS, MUS_ENDING, MUS_COUNT };
#define LF_DROP 1 /* enemies drop in from above (the chimney) */

enum {
    BOSS_LAWN, BOSS_GRILL, BOSS_CRUMB, BOSS_SOOT, BOSS_GUTTER, BOSS_DUST,
    BOSS_BATHTUB, BOSS_LIBRARIAN, BOSS_BUTLER, BOSS_COUCH, BOSS_FURNACE,
    BOSS_QUEEN, BOSS_GENERAL, BOSS_COUNT
};

/* Map tile codes (tools/gen_levels.py documents the ASCII legend). */
enum {
    T_EMPTY = 0, T_WALL_A = 1, T_DOOR = 2, T_PROP_A = 3, T_EXIT = 4, T_BOSS = 5,
    T_CAT = 6, T_HEALTH = 7, T_ANCHOR = 8, T_WALL_B = 9, T_WALL_C = 10,
    T_FOOD = 11, T_YARN = 12, T_HOLE = 13, T_PROP_B = 14
};

/* Strings are offsets into level_text[]; LV_TEXT(field) gives the text. */
typedef struct {
    uint16_t name, place, intro[2], quip, outro[2];
    uint16_t map_off;
    uint8_t w, h;
    uint16_t spawn_off;
    uint8_t spawn_count;
    uint8_t start_x, start_y, start_dir; /* start_dir: angle / 4 */
    uint8_t boss_x, boss_y;
    uint8_t sky, floor_tex, ceil_tex, fog;
    uint8_t wall_tex[3], door_tex;
    uint8_t props[2];
    uint8_t music, hazard, waves, boss, flags;
} level_def_t;

#define LV_TEXT(field) (level_text + lv->field)

/* ---- actors ---------------------------------------------------------- */
enum { E_ROACH, E_RUNNER, E_SPITTER, E_BEETLE, E_ARMORED, E_ELITE, E_FLYER,
       E_BOSS, E_ANCHOR, E_TYPE_COUNT };

/* The enemy state machine requested by the design: IDLE, CHASE, ATTACK,
 * SPIT, HURT and DEAD. CHARGE is the beetle rush; bosses add their own
 * pattern states in boss.c (bstate). */
enum { AI_IDLE, AI_CHASE, AI_ATTACK, AI_SPIT, AI_CHARGE, AI_HURT, AI_DEAD };

#define EF_BOSS 1       /* counts towards the boss bar */
#define EF_OBJECTIVE 2  /* anchors: not part of any wave */
#define EF_INVULN 4     /* shell up / anchor sealed */
#define EF_EXPOSED 8    /* beetle belly or boss shell open: double damage */

typedef struct {
    int32_t x, y;        /* Q8 map position */
    int16_t z;           /* height above the floor, Q8 (drop-ins, jumps) */
    int16_t vx, vy;      /* knockback or charge velocity, Q8 per tick */
    int16_t hp, maxhp;
    uint8_t active, type, state, timer;
    uint8_t cd, anim, flash, flags;
    uint8_t boss;        /* boss table index when type == E_BOSS */
    uint8_t bstate, btimer, bcount;
    uint8_t stuck;       /* ticks spent sidestepping an obstacle */
    int8_t side;         /* sidestep direction */
} enemy_t;

enum { CAT_IDLE, CAT_OUT, CAT_MAUL, CAT_RETURN };

typedef struct {
    int32_t x, y;
    int16_t dx, dy;      /* straight-flight direction when there is no target */
    uint8_t state, target, timer, variant;
} cat_t;

enum { PK_CAT, PK_HEALTH, PK_FOOD, PK_YARN, PK_DAZED };

typedef struct {
    int32_t x, y;
    uint8_t active, type, variant, wait; /* wait: ticks before it can be taken */
} pickup_t;

enum { PJ_SPIT, PJ_BOSS_SPIT, PJ_DEBRIS };

typedef struct {
    int32_t x, y;
    int16_t vx, vy, z;
    uint8_t active, type, ttl, dmg;
} proj_t;

typedef struct { int32_t x, y; uint16_t ttl; } puddle_t;
typedef struct { int32_t x, y; uint8_t kind; } prop_t;

typedef struct {
    int32_t x, y;        /* Q8 */
    int ang;             /* 0..1023, 0 = east, 256 = south (map y grows down) */
    int health;
    int shield;          /* shield power, 0..shield_max() */
    int shield_on, shield_broken, shield_delay;
    int yarn;            /* ticks of yarn-boosted shield */
    int poison;          /* ticks of poisoning */
    int cat_cd;          /* ticks until the next cat can be sent */
    int leap;            /* ticks of the cat-leap overlay animation */
    int hurt;            /* ticks of the pained face / red border */
    int iframes;         /* ticks of immunity to bites and spit after a hit */
    int shield_hit;      /* ticks of the white shield flash */
    int bob;             /* walk cycle */
    int32_t safe_x, safe_y;
} player_t;

/* A snapshot taken when a level starts, restored by "try again". */
typedef struct {
    int health, cats_max;
} checkpoint_t;

/* ---- global state ------------------------------------------------------ */
static int state, state_timer, paused;
static int level;                 /* 0..LEVEL_COUNT-1 */
static const level_def_t *lv;     /* runtime pointer: fine outside tables */
static uint8_t map[MAP_CELLS];
static int map_w, map_h;
static int32_t exit_x, exit_y, boss_x, boss_y;
static int wave, waves_total, wave_delay, doors_open, exit_open;
static int boss_defeated, bosses_spawned, anchors_left, portal_timer, collapse;
static int flicker, quake;
static uint32_t rng_state = 0x2545f491u;
static uint32_t ticks;            /* logic ticks since boot */
static int squashed;              /* enemies defeated this level */

static player_t pl;
static int cats_max, cats_out;
static checkpoint_t checkpoint;
static enemy_t enemies[MAX_ENEMIES];
static cat_t cats[MAX_CATS];
static pickup_t pickups[MAX_PICKUPS];
static proj_t projs[MAX_PROJECTILES];
static puddle_t puddles[MAX_PUDDLES];
static prop_t props[MAX_PROPS];
static int nprops;

/* One-shot event counters, read by the tests. */
static int ev_level_complete, ev_boss_defeated, ev_ending;

/* ---- cross-module declarations ---------------------------------------- */
static int map_solid(int cx, int cy);
static int solid_at(int32_t x, int32_t y);
static int can_stand(int32_t x, int32_t y, int r);
static int move_actor(int32_t *x, int32_t *y, int dx, int dy, int r);
static int line_of_sight(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
static int32_t dist_q8(int32_t dx, int32_t dy);

static void player_damage(int amount, int kind);
static int shield_max(void);
static void cats_lose_one(int32_t x, int32_t y);
static void cats_reset(void);
static int cats_ready(void);
static void enemy_damage(int i, int amount, int from_cat);
static int enemy_spawn(int type, int32_t x, int32_t y, int drop);
static int enemies_alive(int flags_any, int flags_none);
static int enemy_blocks(int32_t x, int32_t y, int r);
static int tile_at(int32_t x, int32_t y);
static void cats_launch(void);
static void level_complete(void);
static void boss_spawn_level(void);
static void boss_think(enemy_t *e, int i);
static void proj_spawn(int type, int32_t x, int32_t y, int32_t tx, int32_t ty, int speed, int dmg);
static void pickup_spawn(int type, int32_t x, int32_t y, int variant);
static void puddle_spawn(int32_t x, int32_t y);
static void on_boss_defeated(void);
static void message(const char *text, int ticks);
static void game_set_state(int s);
static int world_state(int s);

static void sfx(int id);
static void music_play(int song);

/* Damage kinds, for the shield rules in player.c. */
enum { DMG_BITE, DMG_POISON, DMG_DEBRIS, DMG_SHOCK, DMG_PUDDLE, DMG_FALL, DMG_PORTAL };

/* Sound effects (audio.c). */
enum {
    SFX_LAUNCH, SFX_MEOW, SFX_HIT, SFX_DIE, SFX_SPIT, SFX_SHIELD_ON, SFX_SHIELD_HIT,
    SFX_HURT, SFX_PICKUP, SFX_WARN, SFX_DOOR, SFX_SKITTER, SFX_BOING, SFX_PULSE,
    SFX_COLLAPSE, SFX_FALL, SFX_UI, SFX_THUD, SFX_COUNT
};

#endif
