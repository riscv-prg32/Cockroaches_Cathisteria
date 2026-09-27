/*
 * fixmath.c -- the freestanding runtime: memset/memcpy, integer trigonometry,
 * square roots, random numbers and a tiny string builder.
 *
 * Angles are in 1/1024 of a turn. isin()/icos() return Q14 values
 * (16384 = 1.0) from a generated quarter-wave table. Positions are Q8.
 */

#if defined(__GNUC__) && !defined(__clang__)
#define CC_NO_LOOP_TO_CALL __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define CC_NO_LOOP_TO_CALL
#endif

/* GCC may emit calls to these for struct copies and array clears. Host test
 * builds link the C library instead. */
#if CC_HOST
#include <string.h>
#else
CC_NO_LOOP_TO_CALL
void *memset(void *dst, int value, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)value;
    return dst;
}

CC_NO_LOOP_TO_CALL
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}
#endif

static int sin_quarter(int q) { /* q in 0..256, linear between table steps */
    int i = q >> 2, f = q & 3;
    return f ? sin_q14[i] + (((sin_q14[i + 1] - sin_q14[i]) * f) >> 2) : sin_q14[i];
}

static int isin(int a) {
    a &= 1023;
    if (a < 256) return sin_quarter(a);
    if (a < 512) return sin_quarter(512 - a);
    if (a < 768) return -sin_quarter(a - 512);
    return -sin_quarter(1024 - a);
}

static int icos(int a) { return isin(a + 256); }

static uint32_t isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* Length of a Q8 vector (valid for the |d| < 128 tiles of any map). */
static int32_t dist_q8(int32_t dx, int32_t dy) {
    return (int32_t)isqrt((uint32_t)(dx * dx + dy * dy));
}

/* Velocity of the given speed (Q8 per tick) pointing from a to b. */
static void aim(int32_t ax, int32_t ay, int32_t bx, int32_t by, int speed,
                int16_t *vx, int16_t *vy) {
    int32_t dx = bx - ax, dy = by - ay, d = dist_q8(dx, dy);
    if (d == 0) d = 1;
    *vx = (int16_t)(dx * speed / d);
    *vy = (int16_t)(dy * speed / d);
}

/* xorshift32: deterministic, so the host tests replay exactly. */
static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int rnd_range(int lo, int hi) { /* inclusive */
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* A bump-allocated string pool for text built during one frame. */
static char str_pool[256];
static int str_used, str_mark;

static void sp_reset(void) { str_used = 0; }

static void sp_begin(void) { str_mark = str_used; }

static void sp_char(char c) {
    if (str_used < (int)sizeof(str_pool) - 1) str_pool[str_used++] = c;
}

static void sp_str(const char *s) {
    while (*s) sp_char(*s++);
}

static void sp_int(int v) {
    char buf[12];
    int n = 0;
    if (v < 0) {
        sp_char('-');
        v = -v;
    }
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 11);
    while (n) sp_char(buf[--n]);
}

static char *sp_end(void) {
    sp_char(0);
    if (str_used >= (int)sizeof(str_pool) - 1) str_pool[sizeof(str_pool) - 1] = 0;
    return &str_pool[str_mark];
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, int cap) {
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}
