/* Sprite interpolation: the in-between pictures of an animation.

   The game shows a new picture every 4-6 fields.  Between the picture on
   screen (p) and the next one (n, found by looking ahead), the picture is
   split into 8x8 blocks of the normal resolution image (16x8 of the 768
   wide picture):

     unchanged blocks stay as they are;
     blocks whose content moved (a character walking, an object sliding)
       are found again in p by a coarse-to-fine search, and move along that
       path: at fraction a of the interval the block shows p shifted by a of
       the motion (first half) or n shifted back by the rest (second half),
       never a blend, so the art stays crisp;
     blocks that changed shape (a new drawing of a hand-drawn animation)
       switch to n when n is due, as they did originally.

   When most of the picture changes and little of it is motion, it is a
   cut, and nothing is interpolated. */
#include "motion.h"
#include "cartoon.h"
#include <stdlib.h>
#include <string.h>

#define PW 384          /* normal resolution analysis picture */
#define PH 240
#define BS 8
#define BW (PW / BS)
#define BH (PH / BS)
#define RX 24           /* search range in normal resolution pixels */
#define RY 16

enum { B_STATIC, B_MOTION, B_CUT };
static uint8_t kind[BH][BW];
static int sad_zero[BH][BW];
long g_interp_pairs, g_interp_moving;    /* picture changes seen / interpolated (log) */
static int8_t mvx[BH][BW], mvy[BH][BW];     /* n(x) = p(x + mv) */
static int all_cut;

static uint8_t lp[PH][PW], ln[PH][PW];       /* luma */
static uint8_t lp2[PH / 2][PW / 2], ln2[PH / 2][PW / 2];

static inline int luma(uint32_t c) {
    return (int)((((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29) >> 8);
}

static int sad_full(int bx, int by, int dx, int dy, int limit) {
    int s = 0;
    for (int y = 0; y < BS; y++) {
        int py = by * BS + y + dy;
        if (py < 0 || py >= PH) return 1 << 30;
        const uint8_t *a = ln[by * BS + y] + bx * BS;
        for (int x = 0; x < BS; x++) {
            int px = bx * BS + x + dx;
            if (px < 0 || px >= PW) return 1 << 30;
            s += abs(a[x] - lp[py][px]);
        }
        if (s >= limit) return s;
    }
    return s;
}

static int sad_half(int bx, int by, int dx, int dy, int limit) {
    int s = 0;
    for (int y = 0; y < BS / 2; y++) {
        int py = by * (BS / 2) + y + dy;
        if (py < 0 || py >= PH / 2) return 1 << 30;
        for (int x = 0; x < BS / 2; x++) {
            int px = bx * (BS / 2) + x + dx;
            if (px < 0 || px >= PW / 2) return 1 << 30;
            s += abs(ln2[by * (BS / 2) + y][bx * (BS / 2) + x] - lp2[py][px]);
        }
        if (s >= limit) return s;
    }
    return s;
}

static const uint32_t *gp, *gn;

static void block_rows(void *ctx, int y0, int y1) {
    (void)ctx;
    for (int by = y0; by < y1; by++) {
        for (int bx = 0; bx < BW; bx++) {
            /* changed at all? (compare the real pixels) */
            int changed = 0;
            for (int y = 0; y < BS && !changed; y++) {
                const uint32_t *a = gp + (by * BS + y) * 768 + bx * BS * 2;
                const uint32_t *b = gn + (by * BS + y) * 768 + bx * BS * 2;
                if (memcmp(a, b, BS * 2 * 4)) changed = 1;
            }
            mvx[by][bx] = mvy[by][bx] = 0;
            if (!changed) { kind[by][bx] = B_STATIC; continue; }
            int sad0 = sad_full(bx, by, 0, 0, 1 << 30);
            /* coarse search at half resolution, then refine */
            int best = 1 << 30, bdx = 0, bdy = 0;
            for (int dy = -RY / 2; dy <= RY / 2; dy++)
                for (int dx = -RX / 2; dx <= RX / 2; dx++) {
                    int s = sad_half(bx, by, dx, dy, best);
                    if (s < best || (s == best && abs(dx) + abs(dy) < abs(bdx) + abs(bdy))) { best = s; bdx = dx; bdy = dy; }
                }
            int fbest = 1 << 30, fx = 0, fy = 0;
            for (int dy = bdy * 2 - 1; dy <= bdy * 2 + 1; dy++)
                for (int dx = bdx * 2 - 1; dx <= bdx * 2 + 1; dx++) {
                    int s = sad_full(bx, by, dx, dy, fbest);
                    if (s < fbest) { fbest = s; fx = dx; fy = dy; }
                }
            sad_zero[by][bx] = sad0;
            /* a near exact match, clearly better than standing still */
            if ((fx || fy) && fbest <= BS * BS * 2 && fbest * 8 < sad0) {
                kind[by][bx] = B_MOTION;
                mvx[by][bx] = (int8_t)fx;
                mvy[by][bx] = (int8_t)fy;
            } else {
                kind[by][bx] = B_CUT;
            }
        }
    }
}

void motion_prepare(const uint32_t *p, const uint32_t *n) {
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) {
            lp[y][x] = (uint8_t)luma(p[y * 768 + x * 2]);
            ln[y][x] = (uint8_t)luma(n[y * 768 + x * 2]);
        }
    for (int y = 0; y < PH / 2; y++)
        for (int x = 0; x < PW / 2; x++) {
            lp2[y][x] = (uint8_t)((lp[2 * y][2 * x] + lp[2 * y][2 * x + 1] + lp[2 * y + 1][2 * x] + lp[2 * y + 1][2 * x + 1]) / 4);
            ln2[y][x] = (uint8_t)((ln[2 * y][2 * x] + ln[2 * y][2 * x + 1] + ln[2 * y + 1][2 * x] + ln[2 * y + 1][2 * x + 1]) / 4);
        }
    gp = p; gn = n;
    parallel_rows(BH, block_rows, NULL);
    /* a moving block must belong to a group moving together (an object
       sliding, not the scattered matches of a zoom or a new drawing) */
    static uint8_t keep[BH][BW];
    for (int by = 0; by < BH; by++)
        for (int bx = 0; bx < BW; bx++) {
            keep[by][bx] = 0;
            if (kind[by][bx] != B_MOTION) continue;
            int same = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int x = bx + dx, y = by + dy;
                    if ((dx || dy) && x >= 0 && x < BW && y >= 0 && y < BH && kind[y][x] == B_MOTION &&
                        mvx[y][x] == mvx[by][bx] && mvy[y][x] == mvy[by][bx]) same++;
                }
            keep[by][bx] = same >= 2;
        }
    for (int by = 0; by < BH; by++)
        for (int bx = 0; bx < BW; bx++)
            if (kind[by][bx] == B_MOTION && !keep[by][bx]) kind[by][bx] = B_CUT;
    /* changed blocks at the edge of a group (part object, part background)
       go with the group when its motion explains them well */
    for (int by = 0; by < BH; by++)
        for (int bx = 0; bx < BW; bx++) {
            if (kind[by][bx] != B_CUT) continue;
            for (int k = 0; k < 4; k++) {
                int x = bx + (k == 0) - (k == 1), y = by + (k == 2) - (k == 3);
                if (x < 0 || x >= BW || y < 0 || y >= BH || !keep[y][x]) continue;
                int s2 = sad_full(bx, by, mvx[y][x], mvy[y][x], 1 << 30);
                if (s2 * 2 < sad_zero[by][bx]) {
                    kind[by][bx] = B_MOTION;
                    mvx[by][bx] = mvx[y][x];
                    mvy[by][bx] = mvy[y][x];
                    break;
                }
            }
        }
    /* each connected group of changed blocks is judged on its own: only a
       group that is (almost) all clean motion is interpolated, its other
       blocks moving with it; any other group is held and changes when it
       did originally.  Mixing moving and held blocks inside one object
       would tear it apart; groups are separated by unchanged blocks. */
    static int16_t comp[BH][BW], stack[BW * BH];
    for (int by = 0; by < BH; by++)
        for (int bx = 0; bx < BW; bx++) comp[by][bx] = -1;
    int ncomp = 0, any = 0;
    for (int sy = 0; sy < BH; sy++)
        for (int sx = 0; sx < BW; sx++) {
            if (kind[sy][sx] == B_STATIC || comp[sy][sx] >= 0) continue;
            int sp = 0, total = 0, moving = 0;
            int vx[BW * BH], vy[BW * BH];
            comp[sy][sx] = (int16_t)ncomp;
            stack[sp++] = (int16_t)(sy * BW + sx);
            int members[BW * BH], nm = 0;
            while (sp) {
                int b = stack[--sp], bx = b % BW, by = b / BW;
                members[nm++] = b;
                total++;
                if (kind[by][bx] == B_MOTION) { vx[moving] = mvx[by][bx]; vy[moving] = mvy[by][bx]; moving++; }
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int x = bx + dx, y = by + dy;
                        if (x < 0 || x >= BW || y < 0 || y >= BH || kind[y][x] == B_STATIC || comp[y][x] >= 0) continue;
                        comp[y][x] = (int16_t)ncomp;
                        stack[sp++] = (int16_t)(y * BW + x);
                    }
            }
            ncomp++;
            if (moving * 10 >= total * 9) {
                /* the group's most common motion, for its remaining blocks */
                int best = 0, bc = -1;
                for (int i = 0; i < moving; i++) {
                    int c = 0;
                    for (int j = 0; j < moving; j++) c += vx[j] == vx[i] && vy[j] == vy[i];
                    if (c > bc) { bc = c; best = i; }
                }
                for (int i = 0; i < nm; i++) {
                    int bx = members[i] % BW, by = members[i] / BW;
                    if (kind[by][bx] == B_CUT) { kind[by][bx] = B_MOTION; mvx[by][bx] = (int8_t)vx[best]; mvy[by][bx] = (int8_t)vy[best]; }
                }
                any = 1;
            } else {
                for (int i = 0; i < nm; i++) {
                    int bx = members[i] % BW, by = members[i] / BW;
                    kind[by][bx] = B_CUT;
                }
            }
        }
    all_cut = !any;
    g_interp_pairs++;
    if (any) g_interp_moving++;
}

void motion_render(uint32_t *out, const uint32_t *p, const uint32_t *n, float alpha) {
    if (all_cut || alpha <= 0.f) { memcpy(out, p, 768 * 240 * 4); return; }
    if (alpha >= 1.f) { memcpy(out, n, 768 * 240 * 4); return; }
    /* the held picture, with the places the moving blocks left uncovered
       from n (which shows what is behind them there) */
    memcpy(out, p, 768 * 240 * 4);
    for (int by = 0; by < BH; by++)
        for (int bx = 0; bx < BW; bx++) {
            if (kind[by][bx] != B_MOTION) continue;
            int sx = (bx * BS + mvx[by][bx]) * 2, sy = by * BS + mvy[by][bx];
            for (int y = 0; y < BS; y++) {
                int ty = sy + y;
                if (ty < 0 || ty >= PH) continue;
                for (int x = 0; x < BS * 2; x++) {
                    int tx = sx + x;
                    if (tx >= 0 && tx < 768) out[ty * 768 + tx] = n[ty * 768 + tx];
                }
            }
        }
    /* then the moving blocks, at their position along the path */
    for (int by = 0; by < BH; by++)
        for (int bx = 0; bx < BW; bx++) {
            if (kind[by][bx] != B_MOTION) continue;
            int dx = mvx[by][bx], dy = mvy[by][bx];
            /* position at alpha: n's block, moved back towards where it was in p */
            int ox = (int)(dx * (1.f - alpha) * 2.f + (dx > 0 ? 0.5f : -0.5f));   /* 768 wide units */
            int oy = (int)(dy * (1.f - alpha) + (dy > 0 ? 0.5f : -0.5f));
            for (int y = 0; y < BS; y++) {
                int ty = by * BS + y + oy;
                if (ty < 0 || ty >= PH) continue;
                const uint32_t *srow = n + (by * BS + y) * 768;
                uint32_t *drow = out + ty * 768;
                for (int x = 0; x < BS * 2; x++) {
                    int tx = bx * BS * 2 + x + ox;
                    if (tx >= 0 && tx < 768) drow[tx] = srow[bx * BS * 2 + x];
                }
            }
        }
}
