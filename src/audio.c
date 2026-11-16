/*
 * audio.c -- SID-like sound effects and an adaptive Chicago blues band.
 *
 * The AUDIO block (audio/cc_audio.json, written by tools/gen_assets.py)
 * holds sixteen procedural synth instruments and no PCM bytes: every sound
 * is a firmware oscillator (triangle, saw, pulse, noise) with its own ADSR
 * envelope and resonant low-pass filter. Everything is sequenced here at
 * the 30 Hz logic rate, so audio needs no timers of its own.
 *
 *   voice 0     bass
 *   voice 1     lead (harmonica, guitar or bell)
 *   voice 2     kick drum
 *   voice 3     organ stabs
 *   voice 4     snare and hi-hat
 *   voice 5     portal hum, or the lead's echo when there is no hum
 *   voices 6-7  sound effects, panned towards their source
 */

enum {
    INS_BASS, INS_KICK, INS_SNARE, INS_HAT, INS_HARP, INS_GUITAR, INS_ORGAN, INS_BELL,
    INS_BOOM, INS_ZAP, INS_HUM, INS_GROWL, INS_HISS, INS_ECHO, INS_SWEEP, INS_CHIME
};
#define SFX_END 255
#define SFX_REST 254
#define CH_LEAD 1
#define CH_KICK 2
#define CH_ORGAN 3
#define CH_SNARE 4
#define CH_AMBIENT 5
#define CH_SFX 6

/* Effect steps: instrument, MIDI note, volume, length in ticks (33 ms).
 * One-tick steps on the sustained SWEEP voice make SID-style pitch slides;
 * noise pitch sets its colour (high notes hiss, low notes rumble). */
static const uint8_t sfx_steps[] = {
    /* LAUNCH  */ INS_SWEEP, 62, 170, 1, INS_SWEEP, 67, 185, 1, INS_SWEEP, 72, 200, 1, INS_SWEEP, 77, 210, 1,
                  INS_SWEEP, 82, 200, 1, INS_ZAP, 86, 170, 2, SFX_END,
    /* MEOW    */ INS_HARP, 76, 190, 1, INS_HARP, 80, 215, 1, INS_HARP, 82, 220, 2, INS_HARP, 81, 210, 1,
                  INS_HARP, 78, 190, 2, INS_HARP, 74, 150, 3, SFX_END,
    /* HIT     */ INS_SNARE, 110, 230, 1, INS_GROWL, 50, 200, 1, INS_SNARE, 96, 180, 1, INS_HISS, 90, 140, 2, SFX_END,
    /* DIE     */ INS_GROWL, 55, 220, 1, INS_GROWL, 49, 220, 1, INS_GROWL, 43, 210, 1, INS_BOOM, 72, 255, 5, SFX_END,
    /* SPIT    */ INS_HISS, 118, 160, 1, INS_HISS, 108, 170, 1, INS_HISS, 100, 150, 1, INS_SWEEP, 52, 120, 1,
                  INS_SWEEP, 46, 90, 1, SFX_END,
    /* SHIELD  */ INS_SWEEP, 60, 180, 1, INS_SWEEP, 64, 190, 1, INS_SWEEP, 67, 200, 1, INS_SWEEP, 72, 210, 1,
                  INS_CHIME, 84, 200, 3, SFX_END,
    /* SH_HIT  */ INS_CHIME, 91, 210, 1, INS_ZAP, 79, 220, 1, INS_SWEEP, 67, 160, 1, INS_SWEEP, 60, 120, 1, SFX_END,
    /* HURT    */ INS_GROWL, 52, 230, 2, INS_GROWL, 47, 220, 2, INS_GROWL, 44, 200, 2, SFX_END,
    /* PICKUP  */ INS_ZAP, 72, 180, 1, INS_ZAP, 76, 185, 1, INS_ZAP, 79, 190, 1, INS_CHIME, 84, 210, 1,
                  INS_CHIME, 91, 180, 3, SFX_END,
    /* WARN    */ INS_ZAP, 67, 200, 4, INS_ZAP, 72, 200, 4, INS_ZAP, 67, 200, 4, INS_ZAP, 72, 200, 4, SFX_END,
    /* DOOR    */ INS_BOOM, 60, 255, 2, INS_SWEEP, 40, 170, 1, INS_SWEEP, 38, 150, 1, INS_SWEEP, 36, 120, 2,
                  INS_HISS, 80, 120, 3, SFX_END,
    /* SKITTER */ INS_HISS, 124, 80, 1, SFX_REST, 0, 0, 1, INS_HISS, 120, 70, 1, SFX_REST, 0, 0, 1,
                  INS_HISS, 126, 60, 1, SFX_END,
    /* BOING   */ INS_SWEEP, 50, 210, 1, INS_SWEEP, 57, 215, 1, INS_SWEEP, 64, 220, 1, INS_SWEEP, 70, 210, 1,
                  INS_SWEEP, 64, 180, 1, INS_SWEEP, 58, 140, 1, SFX_END,
    /* PULSE   */ INS_BOOM, 50, 255, 3, INS_GROWL, 40, 200, 2, INS_GROWL, 34, 180, 3, SFX_END,
    /* COLLAPSE*/ INS_BOOM, 48, 255, 8, INS_GROWL, 31, 220, 6, INS_BOOM, 40, 255, 10, INS_HISS, 70, 160, 10, SFX_END,
    /* FALL    */ INS_SWEEP, 76, 200, 1, INS_SWEEP, 73, 200, 1, INS_SWEEP, 70, 200, 1, INS_SWEEP, 67, 200, 1,
                  INS_SWEEP, 64, 200, 1, INS_SWEEP, 61, 200, 1, INS_SWEEP, 58, 200, 1, INS_BOOM, 64, 255, 3, SFX_END,
    /* UI      */ INS_ZAP, 79, 160, 1, INS_CHIME, 86, 140, 2, SFX_END,
    /* THUD    */ INS_KICK, 31, 255, 1, INS_BOOM, 56, 230, 3, SFX_END,
    /* GROAN   */ INS_HUM, 28, 150, 6, INS_HUM, 27, 130, 6, INS_HUM, 25, 100, 8, SFX_END,
};

/* Byte offset of each effect in sfx_steps (filled once by audio_init()). */
static uint16_t sfx_off[SFX_COUNT];

typedef struct {
    uint16_t pos;   /* next step, or 0xffff when idle */
    uint8_t left;   /* ticks left on the current step */
    uint8_t id;
    int8_t pan;     /* -64 left .. +63 right */
    uint8_t att;    /* distance attenuation, 64 = full volume */
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

static int sfx_low(int id) { return id == SFX_SKITTER || id == SFX_GROAN; }

/* Start an effect with a stereo position and a distance attenuation. */
static void sfx_pan(int id, int pan, int att) {
    int v;
    if (id < 0 || id >= SFX_COUNT) return;
    if (sfx_low(id)) {
        /* Ambient noises never interrupt anything. */
        if (sfxv[1].pos != 0xffff) return;
        v = 1;
    } else if (sfxv[0].pos == 0xffff) {
        v = 0;
    } else if (sfxv[1].pos == 0xffff || sfx_low(sfxv[1].id)) {
        v = 1;
    } else {
        v = sfx_next;
        sfx_next ^= 1;
    }
    sfxv[v].pos = sfx_off[id];
    sfxv[v].left = 0;
    sfxv[v].id = (uint8_t)id;
    sfxv[v].pan = (int8_t)ICLAMP(pan, -64, 63);
    sfxv[v].att = (uint8_t)ICLAMP(att, 8, 64);
}

/* An effect from Fed herself (or the whole room): centred, full volume. */
static void sfx(int id) { sfx_pan(id, 0, 64); }

/* An effect from a point in the world: panned to the side it comes from and
 * quieter with distance. Positions are Q8 tiles. */
static void sfx_at(int id, int32_t wx, int32_t wy) {
    int32_t rx = wx - pl.x, ry = wy - pl.y;
    int c = icos(pl.ang & 1023), s = isin(pl.ang & 1023);
    int32_t depth = (rx * c + ry * s) >> 14;
    int32_t lat = (ry * c - rx * s) >> 14;
    int32_t dist = IABS(depth) + IABS(lat);
    int pan = (int)(lat * 60 / (dist + 64));
    int att = (int)(64 * 1024 / (1024 + dist));
    sfx_pan(id, pan, att);
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
        else prg32_audio_note_on_pan((uint8_t)(CH_SFX + v), st[0], st[1],
                                     (uint8_t)(st[2] * s->att >> 6), s->pan);
        s->left = (uint8_t)(st[3] - 1);
        s->pos = (uint16_t)(s->pos + 4);
    }
}

/* ---- music: an adaptive Chicago blues band ----------------------------
 *
 * One original 12-bar blues in A (I-I-I-I-IV-IV-I-I-V-IV-I-V), played in
 * swung eighths by a small SID band: pulse bass, a harmonica, guitar or
 * bell lead with a delayed echo, kick, snare and hats on their own voices,
 * and organ stabs on the dominant-seventh chord tones.
 *
 * Every mood (exploration, danger, combat, boss, portal, rescue, finale,
 * victory) is a different combination of the same short patterns over the
 * same progression, so the music never restarts: a mood change waits for
 * the next bar line and the band just changes what it plays. The last bar
 * of every chorus ends with a drum fill.
 *
 *   mus_warp    0..3  alien corruption: wrong intervals, displaced drums,
 *                     portal groans, and at 3 a faster tempo
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

/* Drums: 1 kick, 2 hat, 3 snare, 4 ghost snare, 5 kick + hat. */
enum { DP_NONE, DP_SOFT, DP_LIGHT, DP_SHUFFLE, DP_DRIVE, DP_ALIEN };
static const uint8_t drum_pats[6][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},
    {4, 0, 2, 0, 4, 0, 2, 0},
    {1, 2, 3, 4, 1, 2, 3, 2},
    {5, 2, 3, 2, 5, 2, 3, 2},
    {5, 2, 3, 2, 5, 1, 3, 3},
    {1, 0, 2, 3, 0, 1, 3, 0},              /* deliberately displaced */
};
/* The fill on the last beats of the chorus (steps 4..7 of bar 12). */
static const uint8_t drum_fill[2][4] = {{3, 4, 3, 3}, {4, 0, 4, 3}};

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
    /* NONE      */ {18, BP_NONE, DP_NONE, LP_NONE, LP_NONE, 0, INS_HARP, 0},
    /* EXPLORE   */ {18, BP_SPARSE, DP_LIGHT, LP_CALL, LP_NONE, 0, INS_HARP, MF_SPARSE | MF_SOFT},
    /* DANGER    */ {15, BP_WALK, DP_SHUFFLE, LP_TENSION, LP_NONE, 0, INS_GUITAR, 0},
    /* COMBAT    */ {15, BP_BOOGIE, DP_SHUFFLE, LP_FAST, LP_RESP, 1, INS_GUITAR, 0},
    /* BOSS      */ {12, BP_HEAVY, DP_DRIVE, LP_BOSS, LP_FAST, 1, INS_GUITAR, 0},
    /* PORTAL    */ {18, BP_DOOM, DP_ALIEN, LP_ALIEN, LP_NONE, 0, INS_BELL, MF_SPARSE},
    /* SAD       */ {24, BP_NONE, DP_SOFT, LP_SAD, LP_HARP, 0, INS_GUITAR, MF_SOFT},
    /* RESCUE    */ {18, BP_WALK, DP_LIGHT, LP_CALL, LP_HARP, 0, INS_HARP, MF_SOFT},
    /* CRESCENDO */ {12, BP_BOOGIE, DP_DRIVE, LP_FAST, LP_ALIEN, 1, INS_GUITAR, 0},
    /* VICTORY   */ {18, BP_BOOGIE, DP_SHUFFLE, LP_HAPPY, LP_RESP, 1, INS_BELL, MF_MEOW},
};

static int mus_song, mus_next, mus_step, mus_bar, mus_wait, mus_snare_left;
static int mus_warp, mus_layers = 4, mus_carry;
static int echo_note; /* the lead note to echo on the next step */
static int ambient_on;

static void music_silence(void) {
    for (uint8_t ch = 0; ch < 5; ++ch) prg32_audio_note_off(ch);
    if (!ambient_on) prg32_audio_note_off(CH_AMBIENT);
    echo_note = 0;
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

static void drum_hit(int d, int soft) {
    switch (d) {
    case 1: prg32_audio_note_on(CH_KICK, INS_KICK, 33, soft ? 140 : 230); break;
    case 5:
        prg32_audio_note_on(CH_KICK, INS_KICK, 33, soft ? 140 : 230);
        prg32_audio_note_on_pan(CH_SNARE, INS_HAT, 124, soft ? 30 : 55, 20);
        break;
    case 2: prg32_audio_note_on_pan(CH_SNARE, INS_HAT, 124, soft ? 35 : 60, 20); break;
    case 3: prg32_audio_note_on_pan(CH_SNARE, INS_SNARE, 100, soft ? 70 : 150, -8); break;
    case 4: prg32_audio_note_on_pan(CH_SNARE, INS_SNARE, 104, soft ? 30 : 50, -8); break;
    default: break;
    }
    if (d >= 2 && d <= 5) mus_snare_left = 3;
}

static void music_tick(void) {
    if (mus_snare_left && --mus_snare_left == 0) prg32_audio_note_off(CH_SNARE);
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

    /* the echo of the previous lead note, one swung step later */
    if (echo_note && !ambient_on) prg32_audio_note_on_pan(CH_AMBIENT, INS_ECHO, (uint8_t)echo_note, 50, 40);
    echo_note = 0;

    if (layers >= 2) {
        n = bass_pats[m->bass][s];
        if (n > N_HOLD)
            prg32_audio_note_on(0, INS_BASS, (uint8_t)(45 + root + n), mus_song == MUS_BOSS ? 220 : 180);
        else if (n == N_REST && s == 0)
            prg32_audio_note_off(0);
    }
    int d = drum_pats[m->drum][s];
    if (mus_bar == 11 && s >= 4 && m->drum != DP_NONE && m->drum != DP_ALIEN)
        d = drum_fill[soft][s - 4];
    if (mus_warp >= 2 && d && (rnd() & 7) == 0) { /* push the hit a step late */
        mus_carry = d;
        d = 0;
    } else if (!d && mus_carry) {
        d = mus_carry;
        mus_carry = 0;
    }
    drum_hit(d, soft);
    if (m->comp && layers >= 3) {
        if (s == 1 || s == 5) prg32_audio_note_on_pan(CH_ORGAN, INS_ORGAN, (uint8_t)(57 + root + (s == 1 ? 4 : 10)), 70, -24);
        else if (s == 2 || s == 6) prg32_audio_note_off(CH_ORGAN);
    }
    int call = (mus_bar & 1) == 0;
    int pat = call ? m->call : m->resp;
    if ((m->flags & MF_SPARSE) && (mus_bar & 7) > 1) pat = LP_NONE;
    if (layers >= 4 && pat) {
        n = lead_pats[pat][s];
        if (n == N_REST) prg32_audio_note_off(CH_LEAD);
        else if (n != N_HOLD) {
            n += 69 + root;
            if (mus_warp && (int)(rnd() & 7) < mus_warp) n += (rnd() & 1) ? 1 : 6;
            int ins = call ? m->ins : INS_HARP;
            prg32_audio_note_on_pan(CH_LEAD, (uint8_t)ins, (uint8_t)n, 110, 8);
            echo_note = n;
        }
    } else if (s == 0) {
        prg32_audio_note_off(CH_LEAD);
    }
    if (mus_warp >= 2 && s == 0 && (mus_bar & 1)) sfx(SFX_GROAN); /* the portal groans */
    if ((m->flags & MF_MEOW) && (mus_bar & 3) == 3 && s == 6) sfx(SFX_MEOW);

    int beat = m->beat - (mus_warp >= 3 ? 3 : 0);
    mus_wait = ((s & 1) ? beat / 3 : beat * 2 / 3) - 1;
    if (++mus_step == 8) {
        mus_step = 0;
        mus_bar = (mus_bar + 1) % 12;
    }
}

static void ambient(int on) {
    if (on == ambient_on) return;
    ambient_on = on;
    if (on) prg32_audio_note_on(CH_AMBIENT, INS_HUM, 33, 70);
    else prg32_audio_note_off(CH_AMBIENT);
}

static void audio_tick(void) {
    sfx_tick();
    music_tick();
}
