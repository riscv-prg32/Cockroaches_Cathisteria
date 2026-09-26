/*
 * raycast.c -- the renderer: a Wolfenstein-style raycaster with textured
 * floors and ceilings, z-buffered billboard sprites and a small retained
 * list of 2D overlay operations (text, rectangles, scaled sprites).
 *
 * Nothing is drawn straight to the display. Each frame is composed in
 * 8-row strips inside a 2,560-byte buffer of cube colour indices, and every
 * finished strip is sent to the firmware as one 8-bit indexed sprite
 * (prg32_sprite_draw_indexed). That keeps the ABI call count at ~25 per
 * frame and needs no framebuffer of our own in the 64 KiB cartridge RAM.
 *
 * Palette: 216 RGB565 entries, one per cube colour, with components chosen
 * so the ESP32-C6 quantiser maps every entry back to the same cube colour.
 * shade_tab[] darkens (fog, flicker, far walls) or brightens (hit flash) a
 * cube index; every pixel goes through one of its six rows.
 */

static uint16_t cube565[216];
static uint8_t shade_tab[6][216]; /* 0 = lit ... 4 = darkest, 5 = flash */
static uint8_t strip[SW * STRIP_H];
static prg32_indexed_sprite_t strip_desc;

#define SHADE_FLASH 5

/* Per-ray results of the wall cast (one ray per 2-pixel column). */
static uint16_t ray_z[NRAYS];          /* perpendicular distance, Q8 */
static int16_t ray_top[NRAYS], ray_bot[NRAYS];
static uint16_t ray_vstep[NRAYS];      /* texels per pixel, Q12 */
static uint16_t ray_tex[NRAYS];        /* byte offset of the texel column */
static uint8_t ray_pal[NRAYS];         /* palette of the wall texture */
static uint8_t ray_shift[NRAYS], ray_shade[NRAYS], ray_anim[NRAYS];
static uint8_t sky_h[NRAYS], sky_idx[NRAYS];

/* Camera (set from the player every frame). Direction and plane are Q12. */
static int32_t cam_x, cam_y;
static int cam_ang, cam_dx, cam_dy, cam_px, cam_py;

static uint16_t row_dist[VIEW_H];      /* floor/ceiling distance per row, Q8 */
static uint8_t row_shade[VIEW_H];
static uint8_t sky_row[HORIZON];
static uint8_t sky_building, sky_window, sky_stars;
static int fog_step, dark_extra;

static void render_init(void) {
    static const uint8_t lv5[6] = {0, 7, 13, 19, 25, 31};
    static const uint8_t lv6[6] = {0, 13, 26, 38, 51, 63};
    for (int i = 0; i < 216; ++i) {
        int r = i / 36, g = (i / 6) % 6, b = i % 6;
        cube565[i] = (uint16_t)((lv5[r] << 11) | (lv6[g] << 5) | lv5[b]);
        for (int s = 0; s < 5; ++s) {
            int k = 5 - s;
            shade_tab[s][i] = CUBE((r * k + 2) / 5, (g * k + 2) / 5, (b * k + 2) / 5);
        }
        shade_tab[SHADE_FLASH][i] = CUBE(IMIN(5, r + 3), IMIN(5, g + 3), IMIN(5, b + 3));
    }
    strip_desc.pixels = strip;
    strip_desc.palette = cube565;
    strip_desc.width = SW;
    strip_desc.height = STRIP_H;
    strip_desc.frame_count = 1;
    strip_desc.palette_count = 216;
    strip_desc.bits_per_pixel = PRG32_SPRITE_BPP_8;
    strip_desc.transparent_index = -1;
}

static int shade_for(int32_t dist) {
    int s = (int)(dist / fog_step) + dark_extra;
    return ICLAMP(s, 0, 4);
}

static uint8_t mix_cube(uint8_t a, uint8_t b, int t, int n) {
    int ar = a / 36, ag = (a / 6) % 6, ab = a % 6;
    int br = b / 36, bg = (b / 6) % 6, bb = b % 6;
    return CUBE(ar + (br - ar) * t / n, ag + (bg - ag) * t / n, ab + (bb - ab) * t / n);
}

/* Called when a level loads: sky colours, fog and the per-row distances. */
static void render_level_setup(void) {
    static const uint16_t fog_steps[4] = {400, 640, 900, 1300};
    static const uint8_t sky_cols[4][5] = {
        /* top, horizon, buildings, windows, stars */
        {0, 0, 0, 0, 0},
        {CUBE(1, 3, 5), CUBE(4, 5, 5), CUBE(2, 3, 4), CUBE(3, 4, 4), 0},
        {CUBE(1, 0, 2), CUBE(5, 2, 1), CUBE(1, 0, 1), CUBE(5, 4, 1), 0},
        {CUBE(0, 0, 1), CUBE(1, 1, 3), CUBE(0, 0, 0), CUBE(5, 5, 2), CUBE(5, 5, 5)},
    };
    fog_step = fog_steps[lv->fog & 3];
    for (int y = 0; y < VIEW_H; ++y) {
        int d = y >= HORIZON ? 2 * y + 1 - 2 * HORIZON : 2 * HORIZON - 2 * y - 1;
        row_dist[y] = (uint16_t)IMIN(32 * ONE, (WALL_S * 256) / d);
    }
    const uint8_t *sc = sky_cols[lv->sky & 3];
    for (int y = 0; y < HORIZON; ++y) sky_row[y] = mix_cube(sc[0], sc[1], y, HORIZON - 1);
    sky_building = sc[2];
    sky_window = sc[3];
    sky_stars = sc[4];
}

static void camera_from_player(void) {
    cam_x = pl.x;
    cam_y = pl.y;
    cam_ang = pl.ang & 1023;
    cam_dx = icos(cam_ang) >> 2;
    cam_dy = isin(cam_ang) >> 2;
    cam_px = (-cam_dy * PLANE_Q12) >> 12;
    cam_py = (cam_dx * PLANE_Q12) >> 12;
}

/* ---- wall cast ---------------------------------------------------------- */

static int view_solid(int code) {
    return code == T_WALL_A || code == T_WALL_B || code == T_WALL_C ||
           code == T_DOOR || code == T_EXIT;
}

static int tile_texture(int code) {
    switch (code) {
    case T_WALL_B: return lv->wall_tex[1];
    case T_WALL_C: return lv->wall_tex[2];
    case T_DOOR: return lv->door_tex;
    case T_EXIT: return exit_open ? TEX_EXIT_OPEN : TEX_EXIT_LOCK;
    default: return lv->wall_tex[0];
    }
}

static void cast_rays(void) {
    int32_t px = cam_x, py = cam_y;
    int anim = (int)(ticks >> 1) & 15;
    for (int c = 0; c < NRAYS; ++c) {
        int camx = ((2 * c + 1) * 4096) / NRAYS - 4096;
        int rdx = cam_dx + ((cam_px * camx) >> 12);
        int rdy = cam_dy + ((cam_py * camx) >> 12);
        int32_t ddx = rdx ? IMIN(1 << 22, (1 << 20) / IABS(rdx)) : (1 << 22);
        int32_t ddy = rdy ? IMIN(1 << 22, (1 << 20) / IABS(rdy)) : (1 << 22);
        int mx = px >> 8, my = py >> 8, sx, sy, side = 0, code = T_WALL_A;
        int32_t sdx, sdy;
        if (rdx < 0) { sx = -1; sdx = ((px & 255) * ddx) >> 8; }
        else { sx = 1; sdx = ((256 - (px & 255)) * ddx) >> 8; }
        if (rdy < 0) { sy = -1; sdy = ((py & 255) * ddy) >> 8; }
        else { sy = 1; sdy = ((256 - (py & 255)) * ddy) >> 8; }
        for (int n = 0; n < 64; ++n) {
            if (sdx < sdy) { sdx += ddx; mx += sx; side = 0; }
            else { sdy += ddy; my += sy; side = 1; }
            CC_COUNT(1);
            if ((unsigned)mx >= (unsigned)map_w || (unsigned)my >= (unsigned)map_h) {
                code = T_WALL_A;
                break;
            }
            code = map[my * map_w + mx];
            if (view_solid(code)) break;
        }
        int32_t perp = side == 0 ? sdx - ddx : sdy - ddy;
        if (perp < 16) perp = 16;
        int32_t wx = side == 0 ? py + ((perp * rdy) >> 12) : px + ((perp * rdx) >> 12);
        int u = (int)(wx >> 4) & 15;
        if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) u = 15 - u;
        int h = (WALL_S * 256) / perp;
        if (h < 2) h = 2;
        int tex = tile_texture(code);
        ray_z[c] = (uint16_t)IMIN(perp, 65535);
        ray_top[c] = (int16_t)(HORIZON - h / 2);
        ray_bot[c] = (int16_t)(HORIZON - h / 2 + h);
        ray_vstep[c] = (uint16_t)((16u << 12) / (uint32_t)h);
        ray_tex[c] = (uint16_t)(tex_defs[tex][0] * 128 + (u >> 1));
        ray_shift[c] = (u & 1) ? 0 : 4;
        ray_pal[c] = tex_defs[tex][1];
        ray_shade[c] = (uint8_t)IMIN(4, shade_for(perp) + side);
        ray_anim[c] = (tex == TEX_SWIRL || tex == TEX_GROWTH) ? (uint8_t)anim : 0;
        int heading = (cam_ang + ((c - NRAYS / 2) * FOV_UNITS) / NRAYS) & 1023;
        sky_idx[c] = (uint8_t)(heading >> 3);
        sky_h[c] = skyline[heading >> 3];
    }
    for (int y = 0; y < VIEW_H; ++y) row_shade[y] = (uint8_t)shade_for(row_dist[y]);
}

static uint8_t sky_pixel(int c, int y) {
    int idx = sky_idx[c];
    if (y >= HORIZON - sky_h[c]) {
        if (sky_h[c] > 2 && ((y * 7 + idx * 13) % 9) == 0 && y < HORIZON - 1) return sky_window;
        return sky_building;
    }
    if (sky_stars && ((idx * 31 + y * 17) & 127) == 5) return sky_stars;
    return sky_row[y];
}

/* Walls, floor and ceiling for rows sy..sy+h-1 of the 3D view. */
static void world_strip(int sy, int h) {
    int holes = lv->hazard == HAZ_HOLES;
    uint8_t lut[16];
    for (int r = 0; r < h; ++r) {
        int y = sy + r;
        uint8_t *row = strip + r * SW;
        int ceiling = y < HORIZON;
        int sky = ceiling && lv->sky != SKY_NONE;
        const uint8_t *fpat = 0;
        int32_t fx = 0, fy = 0, fsx = 0, fsy = 0;
        if (!sky) {
            int tex = ceiling ? lv->ceil_tex : lv->floor_tex;
            const uint8_t *pal = pal_sets[tex_defs[tex][1]];
            const uint8_t *sh = shade_tab[row_shade[y]];
            int32_t d = row_dist[y];
            fpat = tex_patterns[tex_defs[tex][0]];
            for (int k = 0; k < 16; ++k) lut[k] = sh[pal[k]];
            fsx = (d * 2 * cam_px / NRAYS) >> 4;
            fsy = (d * 2 * cam_py / NRAYS) >> 4;
            fx = (cam_x << 8) + ((d * (cam_dx - cam_px)) >> 4) + fsx / 2;
            fy = (cam_y << 8) + ((d * (cam_dy - cam_py)) >> 4) + fsy / 2;
        }
        for (int c = 0; c < NRAYS; ++c) {
            uint8_t col;
            if (y >= ray_top[c] && y < ray_bot[c]) {
                int v = ((int)(((uint32_t)(y - ray_top[c]) * ray_vstep[c]) >> 12) + ray_anim[c]) & 15;
                int nib = (((const uint8_t *)tex_patterns)[ray_tex[c] + (v << 3)] >> ray_shift[c]) & 15;
                col = shade_tab[ray_shade[c]][pal_sets[ray_pal[c]][nib]];
            } else if (sky) {
                col = sky_pixel(c, y);
            } else {
                int u = (int)(fx >> 12) & 15, v = (int)(fy >> 12) & 15;
                col = lut[(fpat[(v << 3) | (u >> 1)] >> ((u & 1) ? 0 : 4)) & 15];
                if (holes && !ceiling) {
                    int mx = (int)(fx >> 16), my = (int)(fy >> 16);
                    if ((unsigned)mx < (unsigned)map_w && (unsigned)my < (unsigned)map_h &&
                        map[my * map_w + mx] == T_HOLE)
                        col = (u + v) & 1 ? CUBE(0, 0, 1) : CUBE(0, 0, 0);
                }
            }
            fx += fsx;
            fy += fsy;
            row[2 * c] = col;
            row[2 * c + 1] = col;
        }
        CC_COUNT(NRAYS);
    }
}

/* ---- sprites and overlays ------------------------------------------------ */

#define VF_FLIP 1
#define VF_FLASH 2
#define VF_DARK 4

typedef struct {
    int16_t x0, y0, w, h;
    uint16_t depth;   /* 0 for overlays: no z-buffer test */
    uint8_t spr, pal, flags, shade;
} vis_t;

static vis_t vis[MAX_VIS];
static int nvis;

/* Project a world-space billboard. (wx, wy) is its foot, z its height above
 * the floor and ww x wh its size, all Q8 tile units. */
static void vis_add(int32_t wx, int32_t wy, int z, int ww, int wh, int spr, int pal, int flags) {
    if (nvis >= MAX_VIS) return;
    int32_t rx = wx - cam_x, ry = wy - cam_y;
    int32_t depth = (rx * cam_dx + ry * cam_dy) >> 12;
    if (depth < 40) return;
    int32_t lat = (ry * cam_dx - rx * cam_dy) >> 12;
    int32_t sx = SW / 2 + lat * FOCAL / depth;
    int32_t w = ww * FOCAL / depth, h = wh * WALL_S / depth;
    int32_t bottom = HORIZON + ((ONE / 2 - z) * WALL_S) / depth;
    if (w < 1 || h < 1 || w > 1200 || h > 1200) return;
    if (sx + w / 2 < 0 || sx - w / 2 >= SW || bottom - h >= VIEW_H || bottom < 0) return;
    vis_t *v = &vis[nvis++];
    v->x0 = (int16_t)(sx - w / 2);
    v->y0 = (int16_t)(bottom - h);
    v->w = (int16_t)w;
    v->h = (int16_t)h;
    v->depth = (uint16_t)IMIN(depth, 65535);
    v->spr = (uint8_t)spr;
    v->pal = (uint8_t)pal;
    v->flags = (uint8_t)flags;
    v->shade = (uint8_t)shade_for(depth);
}

static void vis_sort(void) { /* far to near: insertion sort, n < 48 */
    for (int i = 1; i < nvis; ++i) {
        vis_t t = vis[i];
        int j = i - 1;
        while (j >= 0 && vis[j].depth < t.depth) {
            vis[j + 1] = vis[j];
            j--;
        }
        vis[j + 1] = t;
    }
}

/* Draw a scaled 4-bpp sprite into the current strip, clipped to its rows.
 * World sprites (depth > 0) are hidden behind nearer walls. */
static void blit_scaled(const vis_t *v, int sy, int sh, int limit_y) {
    int y0 = IMAX(v->y0, sy), y1 = IMIN(v->y0 + v->h, IMIN(sy + sh, limit_y));
    int x0 = IMAX(v->x0, 0), x1 = IMIN(v->x0 + v->w, SW);
    if (y0 >= y1 || x0 >= x1) return;
    const spr_info_t *si = &spr_info[v->spr];
    const uint8_t *pix = spr_pixels + si->off;
    const uint8_t *pal = pal_sets[v->pal];
    const uint8_t *sh_tab = shade_tab[(v->flags & VF_FLASH) ? SHADE_FLASH
                                      : (v->flags & VF_DARK) ? 3 : v->shade];
    uint32_t ustep = ((uint32_t)si->w << 16) / (uint32_t)v->w;
    uint32_t vstep = ((uint32_t)si->h << 16) / (uint32_t)v->h;
    for (int x = x0; x < x1; ++x) {
        if (v->depth && v->depth >= ray_z[x >> 1]) continue;
        int u = (int)(((uint32_t)(x - v->x0) * ustep) >> 16);
        if (u >= si->w) u = si->w - 1;
        if (v->flags & VF_FLIP) u = si->w - 1 - u;
        uint8_t *p = strip + (y0 - sy) * SW + x;
        for (int y = y0; y < y1; ++y, p += SW) {
            int vv = (int)(((uint32_t)(y - v->y0) * vstep) >> 16);
            int idx = vv * si->w + u;
            int b = pix[idx >> 1];
            int nib = (idx & 1) ? (b & 15) : (b >> 4);
            if (nib) *p = sh_tab[pal[nib]];
        }
        CC_COUNT(y1 - y0);
    }
}

/* ---- retained 2D operations --------------------------------------------- */

enum { OP_RECT, OP_DIM, OP_TEXT, OP_SPR };
#define OPF_SHADOW 1
#define OPF_CENTER 2

typedef struct {
    uint8_t type, col, scale, flags; /* OP_SPR: col = sprite, scale = palette */
    int16_t x, y, w, h;
    const char *s;
    uint8_t vflags;
} op_t;

static op_t ops[MAX_OPS];
static int nops;

static void ops_clear(void) { nops = 0; }

static op_t *op_new(int type) {
    if (nops >= MAX_OPS) return 0;
    op_t *o = &ops[nops++];
    o->type = (uint8_t)type;
    o->flags = 0;
    return o;
}

static void op_rect(int x, int y, int w, int h, uint8_t col) {
    op_t *o = op_new(OP_RECT);
    if (!o) return;
    o->x = (int16_t)x; o->y = (int16_t)y; o->w = (int16_t)w; o->h = (int16_t)h;
    o->col = col;
}

static void op_dim(int x, int y, int w, int h) {
    op_t *o = op_new(OP_DIM);
    if (!o) return;
    o->x = (int16_t)x; o->y = (int16_t)y; o->w = (int16_t)w; o->h = (int16_t)h;
}

/* Text in the 8x8 font; flags OPF_CENTER centres it on the screen. */
static void op_text(int x, int y, int scale, uint8_t col, int flags, const char *s) {
    op_t *o = op_new(OP_TEXT);
    if (!o || !s) return;
    int w = str_len(s) * 8 * scale;
    o->x = (int16_t)((flags & OPF_CENTER) ? (SW - w) / 2 : x);
    o->y = (int16_t)y;
    o->w = (int16_t)w;
    o->h = (int16_t)(8 * scale);
    o->scale = (uint8_t)scale;
    o->col = col;
    o->flags = (uint8_t)flags;
    o->s = s;
}

static void op_sprite(int spr, int pal, int x, int y, int w, int h, int flags) {
    op_t *o = op_new(OP_SPR);
    if (!o || w <= 0 || h <= 0) return;
    o->x = (int16_t)x; o->y = (int16_t)y; o->w = (int16_t)w; o->h = (int16_t)h;
    o->col = (uint8_t)spr;
    o->scale = (uint8_t)pal;
    o->vflags = (uint8_t)flags;
}

static void fill_rows(int x, int y, int w, int h, int sy, int sh, uint8_t col, int dim) {
    int y0 = IMAX(y, sy), y1 = IMIN(y + h, sy + sh);
    int x0 = IMAX(x, 0), x1 = IMIN(x + w, SW);
    for (int yy = y0; yy < y1; ++yy) {
        uint8_t *p = strip + (yy - sy) * SW;
        for (int xx = x0; xx < x1; ++xx) p[xx] = dim ? shade_tab[3][p[xx]] : col;
    }
}

static void text_rows(const op_t *o, int dx, int dy, uint8_t col, int sy, int sh) {
    int sc = o->scale, x = o->x + dx;
    if (o->y + dy + o->h <= sy || o->y + dy >= sy + sh) return;
    for (const char *s = o->s; *s; ++s, x += 8 * sc) {
        int ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) ch = '?';
        const uint8_t *g = font8[ch - 32];
        for (int gy = 0; gy < 8; ++gy) {
            int py = o->y + dy + gy * sc;
            if (py + sc <= sy || py >= sy + sh) continue;
            for (int gx = 0; gx < 8; ++gx)
                if (g[gy] & (0x80 >> gx)) fill_rows(x + gx * sc, py, sc, sc, sy, sh, col, 0);
        }
    }
}

static void op_draw(const op_t *o, int sy, int sh) {
    switch (o->type) {
    case OP_RECT: fill_rows(o->x, o->y, o->w, o->h, sy, sh, o->col, 0); break;
    case OP_DIM: fill_rows(o->x, o->y, o->w, o->h, sy, sh, 0, 1); break;
    case OP_TEXT:
        if (o->flags & OPF_SHADOW) text_rows(o, o->scale, o->scale, 0, sy, sh);
        text_rows(o, 0, 0, o->col, sy, sh);
        break;
    default: {
        vis_t v;
        v.x0 = o->x; v.y0 = o->y; v.w = o->w; v.h = o->h;
        v.depth = 0;
        v.spr = o->col;
        v.pal = o->scale;
        v.flags = o->vflags;
        v.shade = 0;
        blit_scaled(&v, sy, sh, SH);
        break;
    }
    }
}

/* Compose rows y0..y1-1 strip by strip and hand each strip to the firmware.
 * `world` selects the 3D view as the base layer, otherwise `bg` fills it. */
static void render_band(int y0, int y1, int world, uint8_t bg) {
    for (int sy = y0; sy < y1; sy += STRIP_H) {
        int h = IMIN(STRIP_H, y1 - sy);
        if (world && sy < VIEW_H) {
            world_strip(sy, h);
            for (int i = 0; i < nvis; ++i) blit_scaled(&vis[i], sy, h, VIEW_H);
        } else {
            memset(strip, bg, (size_t)(SW * h));
        }
        for (int i = 0; i < nops; ++i) op_draw(&ops[i], sy, h);
        strip_desc.height = (uint16_t)h;
        prg32_sprite_draw_indexed(0, sy, &strip_desc, 0);
    }
}
