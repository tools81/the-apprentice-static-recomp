/* Cartoon HD / Cartoon HD+ upscaling for the HD edition.

   The CD-i draws most pictures at normal resolution (384x240, shown here as
   768x240 with doubled pixels).  The upscaler recovers that picture and
   enlarges it 4x with an edge-directed 2x step applied twice:

     Cartoon HD   384x240 -> 768x480 -> 1536x960.  Each pixel becomes 2x2;
                  where a diagonal outline crosses a pixel corner (decided
                  from the colour differences of a 5x5 neighbourhood, as in
                  Hyllian's xBR) the corner takes the colour across the
                  outline, partly or fully depending on how shallow or steep
                  the outline is.  Flat fills stay flat and staircases
                  become smooth curves.
     Cartoon HD+  Cartoon HD, then the line art is inked (pixels darker
                  than their surroundings are darkened a little more, so
                  outlines stay bold after enlarging) and an adaptive
                  sharpen restores the crispness of edges.

   Pictures that use high resolution (pixel pairs that differ) are line
   doubled to 768x480 and given one 2x step. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cartoon.h"

#define OW 1536
#define OH 960

/* ---------------------------------------------------------------- threads */
#define MAXT 16
static int nthreads;
static HANDLE th[MAXT], go_sem, done_ev;
static volatile LONG next_row, active;
static RowFn job_fn;
static void *job_ctx;
static int job_rows;

static void run_chunks(void) {
    for (;;) {
        LONG y0 = InterlockedExchangeAdd(&next_row, 4);
        if (y0 >= job_rows) break;
        int y1 = y0 + 4 < job_rows ? (int)y0 + 4 : job_rows;
        job_fn(job_ctx, (int)y0, y1);
    }
}

static DWORD WINAPI worker(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(go_sem, INFINITE);
        run_chunks();
        if (InterlockedDecrement(&active) == 0) SetEvent(done_ev);
    }
    return 0;
}

void parallel_rows(int rows, RowFn fn, void *ctx) {
    if (nthreads <= 1) { fn(ctx, 0, rows); return; }
    job_fn = fn; job_ctx = ctx; job_rows = rows;
    next_row = 0;
    active = nthreads;
    ResetEvent(done_ev);
    ReleaseSemaphore(go_sem, nthreads - 1, NULL);
    run_chunks();                               /* the caller works too */
    if (InterlockedDecrement(&active) != 0) WaitForSingleObject(done_ev, INFINITE);
}

/* ---------------------------------------------------------------- 2x step */
static inline uint32_t to_yuv(uint32_t c) {
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    int y = (r * 299 + g * 587 + b * 114) / 1000;
    int u = (-r * 169 - g * 331 + b * 500) / 1000;
    int v = (r * 500 - g * 419 - b * 81) / 1000;
    return (uint32_t)y | ((uint32_t)(u + 128) << 8) | ((uint32_t)(v + 128) << 16);
}

static inline int dist(uint32_t a, uint32_t b) {
    int dy = (int)(a & 255) - (int)(b & 255);
    int du = (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
    int dv = (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
    return 48 * abs(dy) + 7 * abs(du) + 6 * abs(dv);
}

/* a + (b - a) * q / 4 per channel */
static inline uint32_t mix4(uint32_t a, uint32_t b, int q) {
    uint32_t r = 0;
    for (int s = 0; s < 24; s += 8) {
        int ca = (a >> s) & 255, cb = (b >> s) & 255;
        r |= (uint32_t)(ca + ((cb - ca) * q) / 4) << s;
    }
    return r;
}

/* the 21 neighbours of E, named for the bottom right corner:
          A1 B1 C1
       A0 A  B  C  C4
       D0 D  E  F  F4
       G0 G  H  I  I4
          G5 H5 I5                                                         */
enum { nA1, nB1, nC1, nA0, nA, nB, nC, nC4, nD0, nD, nE, nF, nF4, nG0, nG, nH, nI, nI4, nG5, nH5, nI5, NN };
static const int8_t nb_dx[NN] = {-1, 0, 1, -2, -1, 0, 1, 2, -2, -1, 0, 1, 2, -2, -1, 0, 1, 2, -1, 0, 1};
static const int8_t nb_dy[NN] = {-2, -2, -2, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2};
/* corner handled by each rotation, and the sub-pixels next to it along a
   shallow / steep outline (0 TL, 1 TR, 2 BL, 3 BR) */
static const int corner_p[4] = {3, 2, 0, 1}, corner_h[4] = {2, 0, 1, 3}, corner_v[4] = {1, 3, 2, 0};

/* index into the 5x5 window (row major) of each neighbour, per rotation */
static int8_t rot_idx[4][NN];

static void rot_init(void) {
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < NN; k++) {
            int dx = nb_dx[k], dy = nb_dy[k], rx, ry;
            switch (r) {        /* the neighbourhood turned by r x 90 degrees */
            case 0: rx = dx; ry = dy; break;
            case 1: rx = -dy; ry = dx; break;
            case 2: rx = -dx; ry = -dy; break;
            default: rx = dy; ry = -dx; break;
            }
            rot_idx[r][k] = (int8_t)((ry + 2) * 5 + rx + 2);
        }
}

/* Only the parts of the picture that changed are upscaled again: a region
   of a stage's input, and what it affects in the next stages. */
typedef struct { int x0, y0, x1, y1; } Rect;   /* [x0, x1) x [y0, y1) */

static Rect grow(Rect r, int m, int w, int h) {
    r.x0 = r.x0 - m < 0 ? 0 : r.x0 - m;
    r.y0 = r.y0 - m < 0 ? 0 : r.y0 - m;
    r.x1 = r.x1 + m > w ? w : r.x1 + m;
    r.y1 = r.y1 + m > h ? h : r.y1 + m;
    return r;
}

static Rect twice(Rect r) { Rect o = {r.x0 * 2, r.y0 * 2, r.x1 * 2, r.y1 * 2}; return o; }

typedef struct {
    const uint32_t *src; uint32_t *dst; uint32_t *q;    /* q: YUV of src */
    int sw, sh;
    Rect r;                                             /* source pixels to do */
} Step;

static void yuv_rows(void *ctx, int y0, int y1) {
    Step *st = (Step *)ctx;
    for (int y = st->r.y0 + y0; y < st->r.y0 + y1; y++)
        for (int x = st->r.x0; x < st->r.x1; x++) st->q[y * st->sw + x] = to_yuv(st->src[y * st->sw + x]);
}

static void step_rows(void *ctx, int r0, int r1) {
    Step *st = (Step *)ctx;
    int sw = st->sw, sh = st->sh, dw = sw * 2;
    const uint32_t *yuv = st->q;
    for (int y = st->r.y0 + r0; y < st->r.y0 + r1; y++) {
        int rows[5];
        for (int k = 0; k < 5; k++) {
            int yy = y + k - 2;
            rows[k] = (yy < 0 ? 0 : yy >= sh ? sh - 1 : yy) * sw;
        }
        for (int x = st->r.x0; x < st->r.x1; x++) {
            uint32_t E = st->src[rows[2] + x];
            uint32_t *o = st->dst + (size_t)(y * 2) * dw + x * 2;
            int xl = x > 0 ? x - 1 : 0, xr = x < sw - 1 ? x + 1 : sw - 1;
            /* flat area: nothing to do */
            if (st->src[rows[1] + xl] == E && st->src[rows[1] + x] == E && st->src[rows[1] + xr] == E &&
                st->src[rows[2] + xl] == E && st->src[rows[2] + xr] == E &&
                st->src[rows[3] + xl] == E && st->src[rows[3] + x] == E && st->src[rows[3] + xr] == E) {
                o[0] = o[1] = o[dw] = o[dw + 1] = E;
                continue;
            }
            uint32_t win[25], wq[25];
            for (int ky = 0; ky < 5; ky++)
                for (int kx = 0; kx < 5; kx++) {
                    int xx = x + kx - 2;
                    xx = xx < 0 ? 0 : xx >= sw ? sw - 1 : xx;
                    win[ky * 5 + kx] = st->src[rows[ky] + xx];
                    wq[ky * 5 + kx] = yuv[rows[ky] + xx];
                }
            uint32_t out[4] = {E, E, E, E};
            for (int r = 0; r < 4; r++) {
                const int8_t *ix = rot_idx[r];
#define C(n) win[ix[n]]
#define Q(n) wq[ix[n]]
                int e = dist(Q(nE), Q(nC)) + dist(Q(nE), Q(nG)) + dist(Q(nI), Q(nF4)) + dist(Q(nI), Q(nH5)) +
                        4 * dist(Q(nH), Q(nF));
                int i = dist(Q(nH), Q(nD)) + dist(Q(nH), Q(nI5)) + dist(Q(nF), Q(nI4)) + dist(Q(nF), Q(nB)) +
                        4 * dist(Q(nE), Q(nI));
                if (e >= i) continue;
                uint32_t px = dist(Q(nE), Q(nF)) <= dist(Q(nE), Q(nH)) ? C(nF) : C(nH);
                int ke = dist(Q(nF), Q(nG)), ki = dist(Q(nH), Q(nC));
                int shallow = 2 * ke <= ki && C(nE) != C(nG) && C(nD) != C(nG);
                int steep = 2 * ki <= ke && C(nE) != C(nC) && C(nB) != C(nC);
#undef C
#undef Q
                int p = corner_p[r];
                if (shallow && !steep) {
                    out[p] = mix4(out[p], px, 3);
                    out[corner_h[r]] = mix4(out[corner_h[r]], px, 1);
                } else if (steep && !shallow) {
                    out[p] = mix4(out[p], px, 3);
                    out[corner_v[r]] = mix4(out[corner_v[r]], px, 1);
                } else {
                    out[p] = mix4(out[p], px, 2);
                }
            }
            o[0] = out[0]; o[1] = out[1]; o[dw] = out[2]; o[dw + 1] = out[3];
        }
    }
}

/* 2x step of the source pixels in r (whose own pixels changed in ch);
   returns the destination region written */
static Rect step2x(const uint32_t *src, uint32_t *q, int sw, int sh, uint32_t *dst, Rect ch) {
    Step st = {src, dst, q, sw, sh, ch};
    parallel_rows(ch.y1 - ch.y0, yuv_rows, &st);
    st.r = grow(ch, 2, sw, sh);           /* pixels whose 5x5 neighbourhood changed */
    parallel_rows(st.r.y1 - st.r.y0, step_rows, &st);
    return twice(st.r);
}

/* ---------------------------------------------------------------- HD+ */
static uint32_t *src_s, *mid, *outx, *outf, *tmp_b;
static uint32_t *yuv1, *yuv2;   /* per stage input pixel: y | (u+128)<<8 | (v+128)<<16 */
static int16_t *lum, *hsum;     /* HD+ luma and its 5-wide row sums */
static Rect hr;                 /* region of the current HD+ stage */

static inline int luma(uint32_t c) {
    return (int)((((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29) >> 8);
}

static void luma_rows(void *ctx, int y0, int y1) {
    (void)ctx;
    for (int y = hr.y0 + y0; y < hr.y0 + y1; y++)
        for (int x = hr.x0; x < hr.x1; x++) lum[y * OW + x] = (int16_t)luma(outx[y * OW + x]);
}

/* ink: a pixel darker than the mean of its 5x5 surroundings (edges
   repeated) is darkened further, up to 30%, which keeps outlines bold after
   enlarging.  The 5x5 sums are row sums (hsum) added over 5 rows. */
static void hsum_rows(void *ctx, int y0, int y1) {
    (void)ctx;
    for (int y = hr.y0 + y0; y < hr.y0 + y1; y++) {
        const int16_t *l = lum + y * OW;
        int16_t *h = hsum + y * OW;
        for (int x = hr.x0; x < hr.x1; x++) {
            int s = 0;
            for (int dx = -2; dx <= 2; dx++) {
                int xx = x + dx;
                s += l[xx < 0 ? 0 : xx >= OW ? OW - 1 : xx];
            }
            h[x] = (int16_t)s;
        }
    }
}

static void ink_rows(void *ctx, int y0, int y1) {
    (void)ctx;
    for (int y = hr.y0 + y0; y < hr.y0 + y1; y++) {
        const int16_t *r[5];
        for (int k = 0; k < 5; k++) {
            int yy = y + k - 2;
            r[k] = hsum + (yy < 0 ? 0 : yy >= OH ? OH - 1 : yy) * OW;
        }
        for (int x = hr.x0; x < hr.x1; x++) {
            uint32_t c = outx[y * OW + x];
            int mean = (r[0][x] + r[1][x] + r[2][x] + r[3][x] + r[4][x]) / 25;
            int d = mean - lum[y * OW + x] - 10;
            if (d > 0) {
                int f = d * 3 > 77 ? 77 : d * 3;        /* /256 */
                uint32_t o = 0;
                for (int s = 0; s < 24; s += 8) o |= (uint32_t)((((c >> s) & 255) * (256 - f)) >> 8) << s;
                c = o;
            }
            tmp_b[y * OW + x] = c;
        }
    }
}

/* adaptive sharpen: c + k (c - mean of the 4 neighbours), weaker where the
   local contrast is already high so lines do not ring */
static void sharpen_rows(void *ctx, int y0, int y1) {
    (void)ctx;
    for (int y = hr.y0 + y0; y < hr.y0 + y1; y++) {
        int yu = y > 0 ? y - 1 : 0, yd = y < OH - 1 ? y + 1 : OH - 1;
        for (int x = hr.x0; x < hr.x1; x++) {
            int xl = x > 0 ? x - 1 : 0, xr = x < OW - 1 ? x + 1 : OW - 1;
            uint32_t c = tmp_b[y * OW + x];
            uint32_t n4[4] = {tmp_b[yu * OW + x], tmp_b[yd * OW + x], tmp_b[y * OW + xl], tmp_b[y * OW + xr]};
            int mn = 255, mx = 0;
            for (int k = 0; k < 4; k++) { int l = luma(n4[k]); mn = l < mn ? l : mn; mx = l > mx ? l : mx; }
            int contrast = mx - mn;
            if (contrast == 0) { outf[y * OW + x] = c; continue; }
            int k16 = 6 - contrast / 32;               /* strength in 1/16 */
            if (k16 < 1) k16 = 1;
            uint32_t r = 0;
            for (int s = 0; s < 24; s += 8) {
                int v = (c >> s) & 255;
                int m = (((n4[0] >> s) & 255) + ((n4[1] >> s) & 255) + ((n4[2] >> s) & 255) + ((n4[3] >> s) & 255)) / 4;
                v += (v - m) * k16 / 16;
                r |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << s;
            }
            outf[y * OW + x] = r;
        }
    }
}

static void hdplus(Rect x) {
    hr = x;                         parallel_rows(hr.y1 - hr.y0, luma_rows, NULL);
    hr = grow(x, 2, OW, OH);        parallel_rows(hr.y1 - hr.y0, hsum_rows, NULL);
    hr = grow(x, 2, OW, OH);        parallel_rows(hr.y1 - hr.y0, ink_rows, NULL);
    hr = grow(x, 3, OW, OH);        parallel_rows(hr.y1 - hr.y0, sharpen_rows, NULL);
}

/* ---------------------------------------------------------------- entry */
static uint32_t *prev_in;       /* input of the last upscale */
static int prev_mode = -1, prev_hires = -1;
static uint32_t *present_buf;

void cartoon_init(void) {
    if (outx) return;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    /* half the logical processors: a worker preempted by another program
       holds up the whole picture, so leave room */
    nthreads = (int)si.dwNumberOfProcessors;
    if (nthreads > MAXT) nthreads = MAXT;
    if (nthreads < 1) nthreads = 1;
    go_sem = CreateSemaphoreA(NULL, 0, MAXT, NULL);
    done_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < nthreads - 1; i++) th[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    rot_init();
    src_s = (uint32_t *)calloc(384 * 240, 4);
    mid = (uint32_t *)calloc(768 * 480, 4);
    yuv1 = (uint32_t *)calloc(384 * 240, 4);
    yuv2 = (uint32_t *)calloc(768 * 480, 4);
    outx = (uint32_t *)calloc(OW * OH, 4);
    outf = (uint32_t *)calloc(OW * OH, 4);
    tmp_b = (uint32_t *)calloc(OW * OH, 4);
    lum = (int16_t *)calloc(OW * OH, 2);
    hsum = (int16_t *)calloc(OW * OH, 2);
    prev_in = (uint32_t *)calloc(768 * 240, 4);
    present_buf = (uint32_t *)calloc(OW * OH, 4);
}

/* one band of changed input, through every stage */
static void upscale_region(const uint32_t *src, Rect in, int hires, int mode) {
    Rect x;
    if (!hires) {
        Rect s = {in.x0 / 2, in.y0, (in.x1 + 1) / 2, in.y1};    /* 384x240 source */
        for (int y = s.y0; y < s.y1; y++)
            for (int xx = s.x0; xx < s.x1; xx++) src_s[y * 384 + xx] = src[y * 768 + xx * 2] & 0xFFFFFFu;
        Rect m = step2x(src_s, yuv1, 384, 240, mid, s);
        x = step2x(mid, yuv2, 768, 480, outx, m);
    } else {
        Rect m = {in.x0, in.y0 * 2, in.x1, in.y1 * 2};          /* line doubled */
        for (int y = m.y0; y < m.y1; y++)
            for (int xx = m.x0; xx < m.x1; xx++) mid[y * 768 + xx] = src[(y / 2) * 768 + xx] & 0xFFFFFFu;
        x = step2x(mid, yuv2, 768, 480, outx, m);
    }
    if (mode == CARTOON_HDPLUS) hdplus(x);
}

uint32_t *cartoon_upscale(uint32_t *src, int w, int h, int mode, int *ow, int *oh) {
    if (mode == CARTOON_OFF || !outx || w != 768 || h != 240) { *ow = w; *oh = h; return src; }
    /* normal resolution unless pixel pairs differ */
    int differ = 0;
    for (int i = 0; i < w * h; i += 2) differ += src[i] != src[i + 1];
    /* with hysteresis: a change of path redoes the whole picture */
    static int normal_run;
    int pairs = w * h / 2, hires = prev_hires == 1;
    if (differ * 250 >= pairs) { hires = 1; normal_run = 0; }
    else if (differ * 1000 < pairs && ++normal_run >= 30) hires = 0;
    if (prev_hires < 0) hires = differ * 250 >= pairs;
    static int force_full = -1;       /* TW_UPSCALE_FULL=1: always the whole picture (testing) */
    if (force_full < 0) force_full = getenv("TW_UPSCALE_FULL") != NULL;
    if (mode != prev_mode || hires != prev_hires || force_full) {
        Rect all = {0, 0, 768, 240};
        upscale_region(src, all, hires, mode);
    } else {
        /* bands of 16 lines: the columns that changed in each; neighbouring
           changed bands are done together (more rows to share out) */
        Rect cur = {768, 0, 0, 0};
        for (int b = 0; b <= 240; b += 16) {
            int x0 = 768, x1 = 0;
            for (int y = b; y < b + 16 && y < 240; y++) {
                const uint32_t *a = src + y * 768, *p = prev_in + y * 768;
                if (!memcmp(a, p, 768 * 4)) continue;
                int l = 0, r = 767;
                while (a[l] == p[l]) l++;
                while (a[r] == p[r]) r--;
                if (l < x0) x0 = l;
                if (r + 1 > x1) x1 = r + 1;
            }
            if (x0 < x1) {
                if (cur.x0 >= cur.x1) { cur.x0 = x0; cur.x1 = x1; cur.y0 = b; }
                else { cur.x0 = x0 < cur.x0 ? x0 : cur.x0; cur.x1 = x1 > cur.x1 ? x1 : cur.x1; }
                cur.y1 = b + 16;
            } else if (cur.x0 < cur.x1) {
                Rect in = {cur.x0 & ~1, cur.y0, (cur.x1 + 1) & ~1, cur.y1};
                upscale_region(src, in, hires, mode);
                cur.x0 = 768; cur.x1 = 0;
            }
        }
    }
    memcpy(prev_in, src, 768 * 240 * 4);
    prev_mode = mode;
    prev_hires = hires;
    /* the caller draws on what it gets: hand out a copy */
    memcpy(present_buf, mode == CARTOON_HDPLUS ? outf : outx, OW * OH * 4);
    *ow = OW; *oh = OH;
    return present_buf;
}
