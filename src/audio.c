/*
 * audio.c -- sound effects and a minimal three-voice music player.
 *
 * The AUDIO block (audio/cc_audio.json) holds four single-cycle PCM waves
 * and three firmware synth instruments. Everything else is sequenced here
 * at the 30 Hz logic rate, so audio needs no timers of its own.
 *
 *   voices 0-2  music: bass, lead, drums
 *   voice  5    portal ambience
 *   voices 6-7  sound effects
 */

enum { INS_PULSE, INS_SAW, INS_TRI, INS_NOISE, INS_BASS, INS_KICK, INS_BOOM };
#define SFX_END 255
#define SFX_REST 254
#define CH_AMBIENT 5
#define CH_SFX 6

/* Effect steps: instrument, MIDI note, volume, length in ticks. A rising
 * saw is the cat's battle cry; noise is roach business. */
static const uint8_t sfx_steps[] = {
    /* LAUNCH  */ INS_SAW, 67, 190, 1, INS_SAW, 71, 200, 1, INS_SAW, 75, 210, 1, INS_PULSE, 80, 200, 2, INS_PULSE, 84, 150, 2, SFX_END,
    /* MEOW    */ INS_TRI, 76, 200, 1, INS_TRI, 80, 220, 2, INS_TRI, 81, 220, 3, INS_TRI, 78, 190, 2, INS_TRI, 73, 150, 3, SFX_END,
    /* HIT     */ INS_NOISE, 86, 230, 1, INS_NOISE, 76, 220, 1, INS_SAW, 58, 200, 1, INS_NOISE, 70, 160, 2, SFX_END,
    /* DIE     */ INS_NOISE, 62, 255, 2, INS_NOISE, 52, 230, 2, INS_BOOM, 40, 255, 5, SFX_END,
    /* SPIT    */ INS_NOISE, 90, 150, 1, INS_NOISE, 82, 160, 1, INS_NOISE, 74, 140, 2, INS_PULSE, 50, 120, 1, SFX_END,
    /* SHIELD  */ INS_TRI, 60, 200, 1, INS_TRI, 64, 200, 1, INS_TRI, 67, 210, 1, INS_TRI, 72, 220, 3, SFX_END,
    /* SH_HIT  */ INS_PULSE, 86, 180, 1, INS_TRI, 74, 220, 3, SFX_END,
    /* HURT    */ INS_SAW, 52, 230, 2, INS_SAW, 47, 220, 3, SFX_END,
    /* PICKUP  */ INS_PULSE, 72, 180, 1, INS_PULSE, 76, 180, 1, INS_PULSE, 79, 190, 1, INS_PULSE, 84, 200, 3, SFX_END,
    /* WARN    */ INS_PULSE, 67, 200, 5, INS_PULSE, 72, 200, 5, INS_PULSE, 67, 200, 5, INS_PULSE, 72, 200, 5, SFX_END,
    /* DOOR    */ INS_BOOM, 36, 255, 3, INS_NOISE, 50, 200, 3, SFX_END,
    /* SKITTER */ INS_NOISE, 98, 70, 1, SFX_REST, 0, 0, 1, INS_NOISE, 94, 60, 1, SFX_END,
    /* BOING   */ INS_TRI, 55, 220, 1, INS_TRI, 62, 220, 1, INS_TRI, 70, 220, 1, INS_TRI, 62, 180, 2, SFX_END,
    /* PULSE   */ INS_BOOM, 30, 255, 6, INS_SAW, 40, 180, 4, SFX_END,
    /* COLLAPSE*/ INS_BOOM, 28, 255, 10, INS_NOISE, 40, 230, 10, INS_BOOM, 24, 255, 14, SFX_END,
    /* FALL    */ INS_SAW, 72, 200, 2, INS_SAW, 66, 200, 2, INS_SAW, 60, 200, 2, INS_SAW, 54, 200, 2, INS_BOOM, 36, 255, 3, SFX_END,
    /* UI      */ INS_PULSE, 79, 170, 2, SFX_END,
    /* THUD    */ INS_BOOM, 32, 255, 4, SFX_END,
};

/* Byte offset of each effect in sfx_steps (filled once by audio_init()). */
static uint16_t sfx_off[SFX_COUNT];

typedef struct {
    uint16_t pos;   /* next step, or 0xffff when idle */
    uint8_t left;   /* ticks left on the current step */
    uint8_t id;
} sfx_voice_t;

static sfx_voice_t sfxv[2];
static uint8_t sfx_next;

static void audio_init(void) {
    int id = 0;
    unsigned i = 0;
    sfx_off[0] = 0;
    while (i < sizeof(sfx_steps) && id < SFX_COUNT - 1) {
        if (sfx_steps[i] == SFX_END) {
            sfx_off[++id] = (uint16_t)(i + 1);
            i++;
        } else {
            i += 4;
        }
    }
    sfxv[0].pos = sfxv[1].pos = 0xffff;
}

static void sfx(int id) {
    int v;
    if (id < 0 || id >= SFX_COUNT) return;
    if (id == SFX_SKITTER) {
        /* Ambient roach noise never interrupts anything. */
        if (sfxv[1].pos != 0xffff) return;
        v = 1;
    } else if (sfxv[0].pos == 0xffff) {
        v = 0;
    } else if (sfxv[1].pos == 0xffff || sfxv[1].id == SFX_SKITTER) {
        v = 1;
    } else {
        v = sfx_next;
        sfx_next ^= 1;
    }
    sfxv[v].pos = sfx_off[id];
    sfxv[v].left = 0;
    sfxv[v].id = (uint8_t)id;
}

static void sfx_tick(void) {
    for (int v = 0; v < 2; ++v) {
        sfx_voice_t *s = &sfxv[v];
        if (s->pos == 0xffff) continue;
        if (s->left) {
            s->left--;
            continue;
        }
        const uint8_t *st = &sfx_steps[s->pos];
        if (st[0] == SFX_END) {
            prg32_audio_note_off((uint8_t)(CH_SFX + v));
            s->pos = 0xffff;
            continue;
        }
        if (st[0] == SFX_REST) prg32_audio_note_off((uint8_t)(CH_SFX + v));
        else prg32_audio_note_on((uint8_t)(CH_SFX + v), st[0], st[1], st[2]);
        s->left = (uint8_t)(st[3] - 1);
        s->pos = (uint16_t)(s->pos + 4);
    }
}

/* ---- music: an adaptive Chicago blues band ----------------------------
 *
 * One original 12-bar blues in A (I-I-I-I-IV-IV-I-I-V-IV-I-V), played in
 * swung eighths by a tiny band: synth bass (voice 0), a harmonica/guitar
 * lead (voice 1), drums (voice 2), organ stabs on the dominant-seventh
 * chord tones (voice 3) and, near the portal, alien noises (voice 4).
 *
 * Every mood (exploration, danger, combat, boss, portal, rescue, finale,
 * victory) is a different combination of the same short patterns over the
 * same progression, so the music never restarts: a mood change waits for
 * the next bar line and the band just changes what it plays.
 *
 *   mus_warp    0..3  alien corruption: wrong intervals, displaced drums,
 *                     portal noises, and at 3 a faster tempo
 *   mus_layers  1..4  for the finale crescendo: drums, +bass, +organ, +lead
 */

#define N_REST (-128)
#define N_HOLD (-127)
#define R N_REST
#define H N_HOLD

static const uint8_t blues_roots[12] = {0, 0, 0, 0, 5, 5, 0, 0, 7, 5, 0, 7};

/* Bass: semitones above the chord root, from A2. */
enum { BP_NONE, BP_SPARSE, BP_WALK, BP_BOOGIE, BP_HEAVY, BP_DOOM };
static const int8_t bass_pats[6][8] = {
    {R, R, R, R, R, R, R, R},
    {0, R, R, R, 7, R, R, R},              /* relaxed: root and fifth */
    {0, H, 4, H, 7, H, 9, H},              /* walking quarters */
    {0, 4, 7, 9, 10, 9, 7, 4},             /* boogie-woogie eighths */
    {-12, -12, 0, -12, -2, -12, -5, -12},  /* heavy, low register */
    {-12, H, H, -11, -12, H, -6, H},       /* flat second and tritone */
};

/* Drums: 1 kick, 2 hat, 3 snare, 4 ghost note. Steps are swung eighths. */
enum { DP_NONE, DP_SOFT, DP_LIGHT, DP_SHUFFLE, DP_DRIVE, DP_ALIEN };
static const uint8_t drum_pats[6][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},
    {4, 0, 0, 0, 4, 0, 0, 0},
    {1, 0, 3, 4, 1, 0, 3, 0},
    {1, 2, 3, 2, 1, 2, 3, 2},
    {1, 2, 3, 2, 1, 1, 3, 3},
    {1, 0, 2, 3, 0, 1, 3, 0},              /* deliberately displaced */
};

/* Lead riffs: semitones above A4, following the chord root. */
enum { LP_NONE, LP_CALL, LP_RESP, LP_TENSION, LP_FAST, LP_BOSS, LP_SAD, LP_HARP, LP_HAPPY, LP_ALIEN };
static const int8_t lead_pats[10][8] = {
    {R, R, R, R, R, R, R, R},
    {12, H, 10, 7, H, R, R, R},            /* the call */
    {R, R, R, R, 3, 5, 3, 0},              /* the response */
    {-12, -9, -12, -6, -12, -9, -12, -6},  /* tension riff with the blue note */
    {7, 10, 12, 10, 7, 6, 5, 3},           /* fast blues-scale run */
    {0, H, 3, 0, 6, 5, 3, H},              /* boss riff */
    {12, H, H, 10, H, 7, H, R},            /* slow, sad guitar */
    {R, R, R, R, 3, H, 0, R},              /* harmonica answer */
    {7, 9, 12, H, 9, 7, 4, H},             /* playful, major sixth and third */
    {0, 1, 6, H, 11, R, 1, R},             /* alien intervals */
};
#undef R
#undef H

#define MF_SPARSE 1 /* the lead only plays now and then */
#define MF_SOFT 2   /* brushes instead of sticks */
#define MF_MEOW 4   /* a cat joins in */

typedef struct {
    uint8_t beat;             /* ticks per beat (multiple of 3: swung eighths) */
    uint8_t bass, drum, call, resp, comp, ins, flags;
} mood_t;

static const mood_t moods[MUS_COUNT] = {
    /* NONE      */ {18, BP_NONE, DP_NONE, LP_NONE, LP_NONE, 0, INS_PULSE, 0},
    /* EXPLORE   */ {18, BP_SPARSE, DP_LIGHT, LP_CALL, LP_NONE, 0, INS_PULSE, MF_SPARSE | MF_SOFT},
    /* DANGER    */ {15, BP_WALK, DP_SHUFFLE, LP_TENSION, LP_NONE, 0, INS_SAW, 0},
    /* COMBAT    */ {15, BP_BOOGIE, DP_SHUFFLE, LP_FAST, LP_RESP, 1, INS_SAW, 0},
    /* BOSS      */ {12, BP_HEAVY, DP_DRIVE, LP_BOSS, LP_FAST, 1, INS_SAW, 0},
    /* PORTAL    */ {18, BP_DOOM, DP_ALIEN, LP_ALIEN, LP_NONE, 0, INS_TRI, MF_SPARSE},
    /* SAD       */ {24, BP_NONE, DP_SOFT, LP_SAD, LP_HARP, 0, INS_SAW, MF_SOFT},
    /* RESCUE    */ {18, BP_WALK, DP_LIGHT, LP_CALL, LP_HARP, 0, INS_PULSE, MF_SOFT},
    /* CRESCENDO */ {12, BP_BOOGIE, DP_DRIVE, LP_FAST, LP_ALIEN, 1, INS_SAW, 0},
    /* VICTORY   */ {18, BP_BOOGIE, DP_SHUFFLE, LP_HAPPY, LP_RESP, 1, INS_PULSE, MF_MEOW},
};

static int mus_song, mus_next, mus_step, mus_bar, mus_wait, mus_drum_left;
static int mus_warp, mus_layers = 4, mus_carry;

static void music_silence(void) {
    for (uint8_t ch = 0; ch < 5; ++ch) prg32_audio_note_off(ch);
}

/* Change mood. Most changes wait for the next bar line; silence, the sad
 * phrase and anything after silence start at once. */
static void music_play(int song) {
    mus_next = song;
    if (song == mus_song) return;
    if (song == MUS_NONE || song == MUS_SAD || mus_song == MUS_NONE) {
        music_silence();
        mus_song = song;
        mus_step = 0;
        mus_wait = 0;
    }
}

static void music_tick(void) {
    if (mus_drum_left && --mus_drum_left == 0) prg32_audio_note_off(2);
    if (mus_song <= MUS_NONE || mus_song >= MUS_COUNT) return;
    if (mus_wait) {
        mus_wait--;
        return;
    }
    if (mus_step == 0 && mus_next != mus_song) { /* bar line: change mood */
        mus_song = mus_next;
        if (mus_song == MUS_NONE) {
            music_silence();
            return;
        }
    }
    const mood_t *m = &moods[mus_song];
    int s = mus_step, root = blues_roots[mus_bar], n;
    int layers = mus_song == MUS_CRESCENDO ? mus_layers : 4;
    int soft = (m->flags & MF_SOFT) != 0;

    if (layers >= 2) {
        n = bass_pats[m->bass][s];
        if (n > N_HOLD) prg32_audio_note_on(0, INS_BASS, (uint8_t)(45 + root + n), mus_song == MUS_BOSS ? 210 : 170);
    }
    int d = drum_pats[m->drum][s];
    if (mus_warp >= 2 && d && (rnd() & 7) == 0) { /* push the hit a step late */
        mus_carry = d;
        d = 0;
    } else if (!d && mus_carry) {
        d = mus_carry;
        mus_carry = 0;
    }
    if (d == 1) prg32_audio_note_on(2, INS_KICK, 36, soft ? 120 : 200);
    else if (d) {
        prg32_audio_note_on(2, INS_NOISE, d == 3 ? 64 : 100, d == 3 ? (soft ? 50 : 110) : (d == 4 ? 25 : 50));
        mus_drum_left = d == 3 ? 2 : 1;
    }
    if (m->comp && layers >= 3) {
        if (s == 1 || s == 5) prg32_audio_note_on(3, INS_TRI, (uint8_t)(57 + root + (s == 1 ? 4 : 10)), 60);
        else if (s == 2 || s == 6) prg32_audio_note_off(3);
    }
    int call = (mus_bar & 1) == 0;
    int pat = call ? m->call : m->resp;
    if ((m->flags & MF_SPARSE) && (mus_bar & 7) > 1) pat = LP_NONE;
    if (layers >= 4 && pat) {
        n = lead_pats[pat][s];
        if (n == N_REST) prg32_audio_note_off(1);
        else if (n != N_HOLD) {
            n += 69 + root;
            if (mus_warp && (int)(rnd() & 7) < mus_warp) n += (rnd() & 1) ? 1 : 6;
            prg32_audio_note_on(1, call ? m->ins : INS_PULSE, (uint8_t)n, 95);
        }
    } else if (s == 0) {
        prg32_audio_note_off(1);
    }
    if (mus_warp >= 2 && s == 0 && (mus_bar & 1))
        prg32_audio_note_on(4, INS_BOOM, (uint8_t)(24 + (rnd() & 7)), 120); /* the portal groans */
    if ((m->flags & MF_MEOW) && (mus_bar & 3) == 3 && s == 6) sfx(SFX_MEOW);

    int beat = m->beat - (mus_warp >= 3 ? 3 : 0);
    mus_wait = ((s & 1) ? beat / 3 : beat * 2 / 3) - 1;
    if (++mus_step == 8) {
        mus_step = 0;
        mus_bar = (mus_bar + 1) % 12;
    }
}

static int ambient_on;

static void ambient(int on) {
    if (on == ambient_on) return;
    ambient_on = on;
    if (on) prg32_audio_note_on(CH_AMBIENT, INS_SAW, 31, 55);
    else prg32_audio_note_off(CH_AMBIENT);
}

static void audio_tick(void) {
    sfx_tick();
    music_tick();
}
