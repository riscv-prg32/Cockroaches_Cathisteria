/*
 * collision.c -- map queries, circle-versus-tile movement and line of sight.
 *
 * Actors are circles of radius r (Q8) tested at the four corners of their
 * bounding square, which is exact enough on a tile grid. Movement is split
 * into x and y steps, so an actor pushing diagonally into a wall slides
 * along it instead of stopping dead.
 */

/* Is tile (cx, cy) solid for walking? Outside the map counts as solid. */
static int map_solid(int cx, int cy) {
    if ((unsigned)cx >= (unsigned)map_w || (unsigned)cy >= (unsigned)map_h) return 1;
    switch (map[cy * map_w + cx]) {
    case T_WALL_A:
    case T_WALL_B:
    case T_WALL_C:
    case T_DOOR:
    case T_PROP_A:
    case T_PROP_B:
        return 1;
    case T_EXIT:
        return !exit_open;
    default:
        return 0;
    }
}

static int solid_at(int32_t x, int32_t y) {
    if (x < 0 || y < 0) return 1;
    return map_solid((int)(x >> 8), (int)(y >> 8));
}

static int can_stand(int32_t x, int32_t y, int r) {
    return !solid_at(x - r, y - r) && !solid_at(x + r, y - r) &&
           !solid_at(x - r, y + r) && !solid_at(x + r, y + r);
}

/* Move by (dx, dy), sliding along walls. Returns 1 when any axis blocked. */
static int move_actor(int32_t *x, int32_t *y, int dx, int dy, int r) {
    int blocked = 0;
    if (dx) {
        if (can_stand(*x + dx, *y, r)) *x += dx;
        else blocked = 1;
    }
    if (dy) {
        if (can_stand(*x, *y + dy, r)) *y += dy;
        else blocked = 1;
    }
    return blocked;
}

/* Walls and closed doors block sight; furniture does not. Steps of a
 * quarter tile along the segment are plenty for room-sized maps. */
static int line_of_sight(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int32_t dx = x1 - x0, dy = y1 - y0;
    int n = (int)(IMAX(IABS(dx), IABS(dy)) >> 6) + 1;
    for (int i = 1; i < n; ++i) {
        int32_t x = x0 + dx * i / n, y = y0 + dy * i / n;
        int cx = (int)(x >> 8), cy = (int)(y >> 8);
        if ((unsigned)cx >= (unsigned)map_w || (unsigned)cy >= (unsigned)map_h) return 0;
        int t = map[cy * map_w + cx];
        if (t == T_WALL_A || t == T_WALL_B || t == T_WALL_C || t == T_DOOR ||
            t == T_EXIT)
            return 0;
    }
    return 1;
}

static int tile_at(int32_t x, int32_t y) {
    int cx = (int)(x >> 8), cy = (int)(y >> 8);
    if ((unsigned)cx >= (unsigned)map_w || (unsigned)cy >= (unsigned)map_h) return T_WALL_A;
    return map[cy * map_w + cx];
}
