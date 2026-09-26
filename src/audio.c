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

/* ---- music ------------------------------------------------------------ */

/* Songs: 32 steps per voice. Bass/lead: MIDI note, 0 = rest, 1 = hold.
 * Drums: 1 kick, 2 hat, 3 snare. */
static const uint8_t song_step[MUS_COUNT] = {0, 5, 4, 4, 3, 5};
static const uint8_t songs[MUS_COUNT - 1][3][32] = {
    { /* TITLE: a creeping D-minor march */
        {38, 0, 38, 0, 45, 0, 38, 0, 41, 0, 41, 0, 45, 0, 44, 0, 38, 0, 38, 0, 45, 0, 38, 0, 46, 0, 45, 0, 44, 0, 41, 0},
        {62, 1, 65, 1, 69, 1, 68, 1, 67, 1, 65, 1, 64, 1, 61, 0, 62, 1, 65, 1, 69, 1, 74, 1, 72, 1, 70, 1, 69, 1, 1, 0},
        {1, 0, 2, 0, 3, 0, 2, 0, 1, 0, 2, 0, 3, 0, 2, 2, 1, 0, 2, 0, 3, 0, 2, 0, 1, 0, 2, 1, 3, 0, 3, 3},
    },
    { /* EXPLORE: an E-minor walking groove */
        {40, 0, 47, 0, 40, 0, 50, 47, 45, 0, 52, 0, 45, 0, 50, 48, 40, 0, 47, 0, 40, 0, 50, 47, 43, 0, 50, 0, 47, 0, 46, 0},
        {0, 0, 0, 0, 64, 1, 67, 0, 0, 0, 0, 0, 69, 1, 67, 64, 0, 0, 0, 0, 64, 1, 67, 0, 71, 1, 69, 1, 67, 1, 66, 1},
        {1, 0, 2, 0, 3, 0, 2, 0, 1, 0, 2, 1, 3, 0, 2, 0, 1, 0, 2, 0, 3, 0, 2, 0, 1, 1, 2, 0, 3, 0, 3, 2},
    },
    { /* TENSE: a pedal C against a flat second */
        {36, 36, 0, 36, 37, 0, 36, 0, 36, 36, 0, 36, 39, 0, 37, 0, 36, 36, 0, 36, 37, 0, 36, 0, 42, 0, 41, 0, 39, 0, 37, 0},
        {72, 1, 1, 1, 73, 1, 1, 0, 0, 0, 0, 0, 78, 1, 77, 0, 72, 1, 1, 1, 73, 1, 1, 0, 75, 1, 73, 1, 72, 1, 1, 0},
        {1, 0, 0, 2, 0, 0, 1, 0, 1, 0, 0, 2, 0, 0, 3, 0, 1, 0, 0, 2, 0, 0, 1, 0, 1, 0, 3, 0, 3, 0, 3, 2},
    },
    { /* BOSS: driving sixteenths */
        {38, 38, 50, 38, 38, 50, 38, 38, 41, 41, 53, 41, 40, 40, 52, 40, 38, 38, 50, 38, 38, 50, 38, 38, 46, 46, 45, 45, 44, 44, 43, 43},
        {74, 1, 0, 74, 0, 77, 1, 0, 76, 1, 0, 74, 72, 1, 70, 1, 74, 1, 0, 74, 0, 77, 1, 0, 81, 1, 79, 1, 77, 1, 76, 1},
        {1, 2, 3, 2, 1, 2, 3, 2, 1, 2, 3, 2, 1, 2, 3, 3, 1, 2, 3, 2, 1, 2, 3, 2, 1, 2, 3, 2, 3, 3, 3, 3},
    },
    { /* ENDING: C major, cats fed */
        {36, 0, 43, 0, 41, 0, 43, 0, 36, 0, 43, 0, 38, 0, 43, 0, 36, 0, 43, 0, 41, 0, 45, 0, 43, 0, 43, 0, 36, 0, 0, 0},
        {72, 1, 76, 1, 79, 1, 76, 1, 77, 1, 81, 1, 79, 1, 1, 0, 76, 1, 72, 1, 74, 1, 77, 1, 76, 1, 74, 1, 72, 1, 1, 0},
        {1, 0, 2, 0, 3, 0, 2, 0, 1, 0, 2, 0, 3, 0, 2, 0, 1, 0, 2, 0, 3, 0, 2, 0, 1, 0, 2, 2, 3, 0, 0, 0},
    },
};

static int mus_song, mus_pos, mus_wait, mus_drum_left;

static void music_play(int song) {
    if (song == mus_song) return;
    for (uint8_t ch = 0; ch < 3; ++ch) prg32_audio_note_off(ch);
    mus_song = song;
    mus_pos = 0;
    mus_wait = 0;
}

static void music_tick(void) {
    if (mus_song <= MUS_NONE || mus_song >= MUS_COUNT) return;
    if (mus_drum_left && --mus_drum_left == 0) prg32_audio_note_off(2);
    if (mus_wait) {
        mus_wait--;
        return;
    }
    const uint8_t (*s)[32] = songs[mus_song - 1];
    int bass = s[0][mus_pos], lead = s[1][mus_pos], drum = s[2][mus_pos];
    if (bass > 1) prg32_audio_note_on(0, INS_BASS, (uint8_t)bass, 170);
    if (lead > 1) prg32_audio_note_on(1, mus_song == MUS_TENSE ? INS_TRI : INS_PULSE, (uint8_t)lead, 90);
    else if (lead == 0) prg32_audio_note_off(1);
    if (drum == 1) {
        prg32_audio_note_on(2, INS_KICK, 36, 200);
        mus_drum_left = 0;
    } else if (drum == 2) {
        prg32_audio_note_on(2, INS_NOISE, 100, 50);
        mus_drum_left = 1;
    } else if (drum == 3) {
        prg32_audio_note_on(2, INS_NOISE, 64, 110);
        mus_drum_left = 2;
    }
    mus_pos = (mus_pos + 1) & 31;
    mus_wait = song_step[mus_song] - 1;
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
