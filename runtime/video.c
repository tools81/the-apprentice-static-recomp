/* UCM video driver HLE + MCD212-style display renderer. */
#include "recomp.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>

#define E_UNKSVC 0xD0
#define E_UNID   0xBC
#define E_IDFULL 0xBD

/* The display is rendered at double horizontal resolution (768) so that
   CLUT4/RL3 double-resolution planes fit; normal resolution pixels are
   written twice. */
#define SCREEN_LINES 240
#define SCREEN_W     768
int video_w = SCREEN_W, video_h = SCREEN_LINES;
uint32_t *video_fb;

/* ---------------------------------------------------------------- tables */
#define MAXFCT 32
#define MAXLCT 64
#define LCT_COLS 8

typedef struct {
    int used, plane, len, res;
    uint32_t *ins;
    int lct, lct_line;          /* FLnk */
} FCT;

/* Line control tables live in emulated RAM, as the MCD212's display
   control areas (DCA) do: one 64-byte segment (16 instructions) per display
   line.  Games may locate them (the link instruction of DC_LLnk carries the
   address) and write display instructions straight into memory. */
#define DCA_BYTES 64
typedef struct {
    int used, plane, lines, res;    /* lines = physical lines */
    uint32_t addr;                  /* RAM: lines * DCA_BYTES */
} LCT;

static FCT fcts[MAXFCT];
static LCT lcts[MAXLCT];
static int exec_fct[2];             /* active FCT id per plane (0 = none) */
static int ssig_sig, ssig_count = -1, ssig_pid = 1;
static int interlace;

#define RELOAD_DCP 0x20000000u      /* next line's DCA at the address */

static int lct_phys_line(LCT *l, int ucm_line) {
    return l->res ? ucm_line : ucm_line / 2;
}

/* ---------------------------------------------------------------- state */
typedef struct {
    uint32_t vsr;           /* current pixel fetch address */
    int vsr_set;
    uint32_t ddr;           /* display parameters ($78) */
    int icf;
    int mosaic, mosaic_en;
    uint32_t transp_color, mask_color;
    uint32_t dyuv_abs;
    int clut_bank;
} Plane;

static struct {
    uint32_t clut[256];
    uint32_t icm;           /* image coding methods ($C0) */
    uint32_t tcr;           /* transparency control ($C1) */
    uint32_t porder;        /* plane order ($C2) */
    uint32_t backdrop;
    uint32_t matte[8];
    Plane p[2];
    int interrupts;         /* $60 hits this field */
} vs;

/* graphics cursor (16x16, 1 bpp) */
static struct {
    int show, res;
    int hx, hy;             /* hit point position (high-res coords) */
    int hotx, hoty, w, h;   /* hit point within pattern, pattern size */
    int ox, oy;             /* cursor origin */
    uint32_t color;
    uint32_t blink;
    uint16_t ptn[32];
} gc;
static unsigned field_count;

/* ---------------------------------------------------------------- UCM */
static int alloc_id(int isfct) {
    if (isfct) { for (int i = 1; i < MAXFCT; i++) if (!fcts[i].used) return i; }
    else { for (int i = 1; i < MAXLCT; i++) if (!lcts[i].used) return i; }
    return -1;
}

static uint32_t lct_addr(int id, int line) {
    LCT *l = &lcts[id];
    if (line < 0) line = 0;
    if (line >= l->lines) line = l->lines - 1;
    return l->addr + (uint32_t)line * DCA_BYTES;
}

static void dc_write_lct(int id, int line, int col, uint32_t ins) {
    if (id <= 0 || id >= MAXLCT || !lcts[id].used) return;
    LCT *l = &lcts[id];
    if (line < 0 || line >= l->lines || col < 0 || col >= LCT_COLS) return;
    WL(l->addr + (uint32_t)line * DCA_BYTES + (uint32_t)col * 4, ins);
}

static uint32_t dc_read_lct(int id, int line, int col) {
    LCT *l = &lcts[id];
    if (line < 0 || line >= l->lines || col < 0 || col >= LCT_COLS) return 0;
    return RL(l->addr + (uint32_t)line * DCA_BYTES + (uint32_t)col * 4);
}

int ucm_setstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    int16_t d3 = (int16_t)D(3), d4 = (int16_t)D(4), d5 = (int16_t)D(5), d6 = (int16_t)D(6), d7 = (int16_t)D(7);
    if (code == 0x56) { /* SS_DC */
        switch (fn) {
        case 0x00: { /* DC_CrFCT plane, length, res */
            int id = alloc_id(1);
            if (id < 0) return E_IDFULL;
            FCT *f = &fcts[id];
            memset(f, 0, sizeof *f);
            f->used = 1; f->plane = d3 & 1; f->len = d4; f->res = D(5) & 0xFF;
            f->ins = (uint32_t *)calloc(f->len + 1, 4);
            for (int i = 0; i < f->len; i++) f->ins[i] = 0x10000000;
            D(0) = (uint32_t)id;
            TRACE("DC_CrFCT plane %d len %d -> %d", f->plane, f->len, id);
            return 0;
        }
        case 0x01: case 0x02: { /* DC_RdFCT / DC_WrFCT id, start, count, a0 */
            if (d3 <= 0 || d3 >= MAXFCT || !fcts[d3].used) return E_UNID;
            FCT *f = &fcts[d3];
            for (int i = 0; i < d5; i++) {
                int k = d4 + i;
                if (k < 0 || k >= f->len) continue;
                if (fn == 2) f->ins[k] = RL(A(0) + i * 4);
                else WL(A(0) + i * 4, f->ins[k]);
            }
            return 0;
        }
        case 0x03: /* DC_RdFI */
            if (d3 <= 0 || d3 >= MAXFCT || !fcts[d3].used) return E_UNID;
            D(0) = (d4 >= 0 && d4 < fcts[d3].len) ? fcts[d3].ins[d4] : 0;
            return 0;
        case 0x04: /* DC_WrFI id, index, instr */
            if (d3 <= 0 || d3 >= MAXFCT || !fcts[d3].used) return E_UNID;
            if (d4 >= 0 && d4 < fcts[d3].len) fcts[d3].ins[d4] = D(5);
            return 0;
        case 0x05: /* DC_DlFCT */
            if (d3 > 0 && d3 < MAXFCT && fcts[d3].used) { free(fcts[d3].ins); fcts[d3].used = 0; }
            return 0;
        case 0x06: { /* DC_CrLCT plane, lines (UCM), res */
            int id = alloc_id(0);
            if (id < 0) return E_IDFULL;
            LCT *l = &lcts[id];
            memset(l, 0, sizeof *l);
            l->used = 1; l->plane = d3 & 1; l->res = D(5) & 0xFF;
            l->lines = l->res ? d4 : (d4 + 1) / 2;
            if (l->lines < 1) l->lines = 1;
            l->addr = mem_alloc((uint32_t)l->lines * DCA_BYTES, NULL);
            if (!l->addr) { l->used = 0; return 0xCF; }
            for (int i = 0; i < l->lines * DCA_BYTES / 4; i++) WL(l->addr + (uint32_t)i * 4, 0x10000000);
            D(0) = (uint32_t)id;
            TRACE("DC_CrLCT plane %d lines %d -> %d", l->plane, l->lines, id);
            return 0;
        }
        case 0x07: case 0x08: case 0x1B: case 0x1C: { /* Rd/WrLCT, PRd/PWrLCT */
            if (d3 <= 0 || d3 >= MAXLCT || !lcts[d3].used) return E_UNID;
            LCT *l = &lcts[d3];
            int phys = (fn >= 0x1B);
            int wr = (fn == 0x08 || fn == 0x1C);
            int n = 0;
            for (int y = 0; y < d6; y++) {
                int line = phys ? d4 + y : lct_phys_line(l, d4 + y);
                for (int x = 0; x < d7; x++) {
                    uint32_t a = A(0) + (uint32_t)(y * d7 + x) * 4;
                    int col = d5 + x;
                    if (line < 0 || line >= l->lines || col >= LCT_COLS) continue;
                    if (wr) dc_write_lct(d3, line, col, RL(a));
                    else WL(a, dc_read_lct(d3, line, col));
                    n++;
                }
            }
            if (!wr) D(0) = (uint32_t)n;
            return 0;
        }
        case 0x09: /* DC_RdLI */
            if (d3 <= 0 || d3 >= MAXLCT || !lcts[d3].used) return E_UNID;
            {
                LCT *l = &lcts[d3];
                int line = lct_phys_line(l, d4);
                D(0) = dc_read_lct(d3, line, d5);
            }
            return 0;
        case 0x0A: /* DC_WrLI id, line, col, instr */
            if (d3 <= 0 || d3 >= MAXLCT || !lcts[d3].used) return E_UNID;
            dc_write_lct(d3, lct_phys_line(&lcts[d3], d4), d5, D(6));
            return 0;
        case 0x0B: /* DC_DlLCT */
            if (d3 > 0 && d3 < MAXLCT && lcts[d3].used) {
                mem_free(lcts[d3].addr, (uint32_t)lcts[d3].lines * DCA_BYTES);
                lcts[d3].used = 0;
            }
            return 0;
        case 0x0C: /* DC_FLnk fct, lct, line */
            if (d3 <= 0 || d3 >= MAXFCT || !fcts[d3].used) return E_UNID;
            if (d4 <= 0 || d4 >= MAXLCT || !lcts[d4].used) return E_UNID;
            fcts[d3].lct = d4;
            fcts[d3].lct_line = lct_phys_line(&lcts[d4], d5);
            TRACE("DC_FLnk fct %d -> lct %d line %d", d3, d4, d5);
            return 0;
        case 0x0D: /* DC_LLnk lct1, line1, lct2, line2 */
            if (d3 <= 0 || d3 >= MAXLCT || !lcts[d3].used) return E_UNID;
            if (d5 <= 0 || d5 >= MAXLCT || !lcts[d5].used) return E_UNID;
            dc_write_lct(d3, lct_phys_line(&lcts[d3], d4), LCT_COLS - 1,
                         RELOAD_DCP | lct_addr(d5, lct_phys_line(&lcts[d5], d6)));
            TRACE("DC_LLnk lct %d line %d -> lct %d line %d", d3, d4, d5, d6);
            return 0;
        case 0x0E: /* DC_Exec fctA, fctB */
            exec_fct[0] = d3;
            exec_fct[1] = d4;
            TRACE("DC_Exec %d %d", d3, d4);
            return 0;
        case 0x11: /* DC_SSig signal, count */
            ssig_sig = D(3) & 0xFFFF;
            ssig_count = d4;
            ssig_pid = os9_cur_pid();
            return 0;
        case 0x12: /* DC_Relea */
            ssig_count = -1;
            return 0;
        case 0x0F: case 0x13: case 0x10:
            TRACE("DC fn %x (%d) ignored", fn, d3);
            if (fn == 0x13) D(0) = 0;
            return 0;
        default:
            LOG("DC function %x not implemented (d3=%d d4=%d d5=%d d6=%d d7=%d)", fn, d3, d4, d5, d6, d7);
            return 0;
        }
    }
    if (code == 0x59) return ptr_setstat(code);
    if (code == 0x52) { /* SS_GC: graphics cursor */
        switch (fn) {
        case 0: gc.hx = (int16_t)(D(3) >> 16); gc.hy = (int16_t)D(3); break;      /* GC_Pos */
        case 1: gc.show = 1; break;                                               /* GC_Show */
        case 2: gc.show = 0; break;                                               /* GC_Hide */
        case 3: {                                                                 /* GC_Ptn */
            gc.hotx = (int16_t)(D(3) >> 16); gc.hoty = (int16_t)D(3);
            gc.w = (int16_t)(D(4) >> 16); gc.h = (int16_t)D(4);
            gc.res = D(5) & 0xFFFF;
            if (gc.h > 32) gc.h = 32;
            int wpl = (gc.w + 15) / 16;
            for (int y = 0; y < gc.h; y++) gc.ptn[y] = wpl ? RW(A(0) + y * wpl * 2) : 0;
            break;
        }
        case 4: gc.color = D(3); break;                                           /* GC_Col */
        case 5: gc.ox = (int16_t)(D(3) >> 16); gc.oy = (int16_t)D(3); break;      /* GC_Org */
        case 6: gc.blink = D(3) & 0xFFFF; break;                                  /* GC_Blnk */
        }
        TRACE("GC fn %x d3=%08x", fn, D(3));
        return 0;
    }
    LOG("UCM setstat %x fn %x not implemented", code, fn);
    return E_UNKSVC;
}

int ptr_getstat(uint32_t code);
int ptr_setstat(uint32_t code);

int ucm_getstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    if (code == 0x59) return ptr_getstat(code);
    if (code == 0x56 && fn == 0x03) return ucm_setstat(code);
    LOG("UCM getstat %x fn %x not implemented", code, fn);
    return E_UNKSVC;
}

/* ---------------------------------------------------------------- pointer */
InputState input;
static int ptr_x = 384, ptr_y = 240;
static int ptr_sig, ptr_sig_pid = 1;

int ptr_getstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    if (code == 0x59 && fn == 0) { /* PT_Coord */
        D(0) = 0x8000u | (input.b1 ? 1 : 0) | (input.b2 ? 2 : 0) | (input.b3 ? 3 : 0);
        D(1) = ((uint32_t)(uint16_t)ptr_x << 16) | (uint16_t)ptr_y;
        if (log_level >= 3) TRACE("PT_Coord %d,%d buttons %x", ptr_x, ptr_y, D(0) & 0xFFFF);
        return 0;
    }
    LOG("PTR getstat %x fn %x", code, fn);
    return E_UNKSVC;
}

int ptr_setstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    if (code == 0x59) {
        switch (fn) {
        case 1: ptr_sig = D(3) & 0xFFFF; ptr_sig_pid = os9_cur_pid(); return 0;  /* PT_SSig */
        case 2: ptr_sig = 0; return 0;              /* PT_Relea */
        case 4: return 0;                            /* PT_Org */
        case 3:                                      /* PT_Pos */
            ptr_x = (int16_t)(D(3) >> 16);
            ptr_y = (int16_t)D(3);
            TRACE("PT_Pos %d,%d", ptr_x, ptr_y);
            return 0;
        }
    }
    LOG("PTR setstat %x fn %x", code, fn);
    return E_UNKSVC;
}

static void pointer_field(void) {
    static int ob1, ob2;
    int moved = input.dx || input.dy;
    ptr_x += input.dx;
    ptr_y += input.dy;
    if (ptr_x < 0) ptr_x = 0;
    if (ptr_x > 767) ptr_x = 767;
    if (ptr_y < 0) ptr_y = 0;
    if (ptr_y > 479) ptr_y = 479;
    if (ptr_sig && (moved || ob1 != input.b1 || ob2 != input.b2)) sig_send_pid(ptr_sig_pid, (uint32_t)ptr_sig);
    ob1 = input.b1; ob2 = input.b2;
}

/* ---------------------------------------------------------------- render */
static FORCEINLINE int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void exec_ins(int plane, uint32_t ins) {
    uint32_t op = ins >> 24, prm = ins & 0xFFFFFF;
    Plane *P = &vs.p[plane];
    if (op >= 0x80 && op <= 0xBF) {
        int bank = P->clut_bank & 3;
        vs.clut[bank * 64 + (op - 0x80)] = prm;
        return;
    }
    switch (op) {
    case 0x10: break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        P->vsr = prm; P->vsr_set = 1; break;                 /* RELOAD VSR */
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        vs.interrupts++; break;
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        P->ddr = prm; break;
    case 0xC0: if (plane == 0) vs.icm = prm; break;
    case 0xC1: if (plane == 0) vs.tcr = prm; break;
    case 0xC2: if (plane == 0) vs.porder = prm; break;
    case 0xC3: P->clut_bank = prm & 3; break;
    case 0xC4: vs.p[0].transp_color = prm; break;
    case 0xC6: vs.p[1].transp_color = prm; break;
    case 0xC7: vs.p[0].mask_color = prm; break;
    case 0xC9: vs.p[1].mask_color = prm; break;
    case 0xCA: vs.p[0].dyuv_abs = prm; break;
    case 0xCB: vs.p[1].dyuv_abs = prm; break;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        vs.matte[op - 0xD0] = prm; break;
    case 0xD8: vs.backdrop = prm & 15; break;
    case 0xD9: vs.p[0].mosaic = prm & 0xFF; vs.p[0].mosaic_en = (prm >> 23) & 1; break;
    case 0xDA: vs.p[1].mosaic = prm & 0xFF; vs.p[1].mosaic_en = (prm >> 23) & 1; break;
    case 0xDB: vs.p[0].icf = prm & 63; break;
    case 0xDC: vs.p[1].icf = prm & 63; break;
    default:
        if ((op & 0xF0) == 0x20) break;   /* link handled by caller */
        break;
    }
}

/* DYUV quantization table (Green Book Figure V.18) */
static const int dyuv_q[16] = { 0, 1, 4, 9, 16, 27, 44, 79, 128, 177, 212, 229, 240, 247, 252, 255 };

/* decode one line of a plane into idx[] (CLUT index, or -1 transparent-free RGB in rgb[]) */
typedef struct {
    int mode;              /* 0 off, 1 clut8/rgb555, 3 clut7, 4 dual clut7, 5 dyuv, 11 clut4 */
    uint32_t rgb[SCREEN_W];
    uint8_t is_tbit[SCREEN_W];  /* RGB555 transparency bit */
    uint8_t idx_valid;
} LineBuf;

static LineBuf lb[2];

static uint32_t yuv_to_rgb(int y, int u, int v) {
    int b = y + (u - 128) * 1733 / 1000;
    int r = y + (v - 128) * 1371 / 1000;
    int g = (y * 1000 - 299 * r - 114 * b) / 587;
    return ((uint32_t)clampi(r, 0, 255) << 16) | ((uint32_t)clampi(g, 0, 255) << 8) | (uint32_t)clampi(b, 0, 255);
}

static void decode_line(int plane) {
    Plane *P = &vs.p[plane];
    LineBuf *L = &lb[plane];
    int cm = plane == 0 ? (vs.icm & 15) : ((vs.icm >> 8) & 15);
    L->mode = cm;
    uint32_t a = P->vsr;
    int rms = P->ddr & 3;
    int bp = (P->ddr >> 8) & 3;
    memset(L->is_tbit, 0, sizeof L->is_tbit);
    if (cm == 0) return;

    int clut_base = 0;
    if (cm == 3) clut_base = plane == 0 ? 0 : 128;
    if (cm == 4) clut_base = (vs.icm & 0x400000) ? 128 : 0;
    if (cm == 11) clut_base = plane == 0 ? 0 : 128;

    if (rms == 2) {
        /* run-length coded: RL7 (8 bpp) or RL3 (4 bpp pairs) */
        int x = 0;
        int dbl = (cm == 11);
        int maxx = dbl ? 384 : 384;   /* units: pixels (RL7) or pixel pairs (RL3) */
        int guard = 0;
        while (x < maxx && guard++ < 2000) {
            uint8_t b = RB(a++);
            int run = 1;
            if (b & 0x80) {
                run = RB(a++);
                if (run == 0) run = maxx - x;
            }
            for (int k = 0; k < run && x < maxx; k++, x++) {
                if (!dbl) {
                    uint32_t c = vs.clut[(clut_base + (b & 0x7F)) & 255];
                    L->rgb[x * 2] = L->rgb[x * 2 + 1] = c;
                } else {
                    uint32_t c0 = vs.clut[(clut_base + ((b >> 4) & 7)) & 255];
                    uint32_t c1 = vs.clut[(clut_base + (b & 7)) & 255];
                    L->rgb[x * 2] = c0; L->rgb[x * 2 + 1] = c1;
                }
            }
        }
        P->vsr = a;
        return;
    }
    switch (cm) {
    case 1:
        if (plane == 0) { /* CLUT8 */
            for (int x = 0; x < 384; x++) {
                uint32_t c = vs.clut[RB(a++)];
                L->rgb[x * 2] = L->rgb[x * 2 + 1] = c;
            }
        } else { /* RGB555: upper byte from plane A store (we use same addr scheme) */
            uint32_t aa = vs.p[0].vsr;
            for (int x = 0; x < 384; x++) {
                uint8_t hi = RB(aa++), lo = RB(a++);
                uint32_t v = ((uint32_t)hi << 8) | lo;
                int r = ((v >> 10) & 31) * 8, g = ((v >> 5) & 31) * 8, bb = (v & 31) * 8;
                L->rgb[x * 2] = L->rgb[x * 2 + 1] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bb;
                L->is_tbit[x * 2] = L->is_tbit[x * 2 + 1] = (hi >> 7) & 1;
            }
        }
        break;
    case 3: case 4: /* CLUT7 */
        for (int x = 0; x < 384; x++) {
            uint32_t c = vs.clut[(clut_base + (RB(a++) & 0x7F)) & 255];
            L->rgb[x * 2] = L->rgb[x * 2 + 1] = c;
        }
        break;
    case 11: { /* CLUT4: 768 pixels from 384 bytes (double res) */
        int n = (bp == 1) ? 384 : 192;
        for (int x = 0; x < n; x++) {
            uint8_t b = RB(a++);
            uint32_t c0 = vs.clut[(clut_base + (b >> 4)) & 255], c1 = vs.clut[(clut_base + (b & 15)) & 255];
            if (n == 384) { L->rgb[x * 2] = c0; L->rgb[x * 2 + 1] = c1; }
            else { L->rgb[x * 4] = L->rgb[x * 4 + 1] = c0; L->rgb[x * 4 + 2] = L->rgb[x * 4 + 3] = c1; }
        }
        break;
    }
    case 5: { /* DYUV */
        int y = (P->dyuv_abs >> 16) & 255, u = (P->dyuv_abs >> 8) & 255, v = P->dyuv_abs & 255;
        int pu = u, pv = v;
        for (int x = 0; x < 384; x += 2) {
            uint8_t b1 = RB(a++), b2 = RB(a++);
            int du = b1 >> 4, dy0 = b1 & 15, dv = b2 >> 4, dy1 = b2 & 15;
            int nu = (u + dyuv_q[du]) & 255, nv = (v + dyuv_q[dv]) & 255;
            int y0 = (y + dyuv_q[dy0]) & 255;
            int y1 = (y0 + dyuv_q[dy1]) & 255;
            /* interpolate chroma for the second pixel */
            uint32_t c0 = yuv_to_rgb(y0, (pu + nu) / 2, (pv + nv) / 2);
            uint32_t c1 = yuv_to_rgb(y1, nu, nv);
            L->rgb[x * 2] = L->rgb[x * 2 + 1] = c0;
            L->rgb[x * 2 + 2] = L->rgb[x * 2 + 3] = c1;
            pu = nu; pv = nv;
            u = nu; v = nv; y = y1;
        }
        break;
    }
    default:
        memset(L->rgb, 0, sizeof L->rgb);
        break;
    }
    P->vsr = a;
}

static FORCEINLINE int color_key(uint32_t c, uint32_t key, uint32_t mask) {
    uint32_t m = 0xFCFCFC & ~mask;
    return ((c ^ key) & m) == 0;
}

static FORCEINLINE int transparent(int tsel, uint32_t c, int tbit, int mf0, int mf1, uint32_t key, uint32_t mask) {
    int ck;
    switch (tsel) {
    case 0: return 1;
    case 1: return color_key(c, key, mask);
    case 2: return tbit;
    case 3: return mf0;
    case 4: return mf1;
    case 5: return mf0 || color_key(c, key, mask);
    case 6: return mf1 || color_key(c, key, mask);
    case 8: return 0;
    case 9: ck = color_key(c, key, mask); return !ck;
    case 10: return !tbit;
    case 11: return !mf0;
    case 12: return !mf1;
    case 13: return !mf0 || !color_key(c, key, mask);
    case 14: return !mf1 || !color_key(c, key, mask);
    }
    return 0;
}

static FORCEINLINE int apply_icf(int c, int icf) { return (icf * (c - 16)) / 63 + 16; }

static FORCEINLINE uint32_t to_host(int r, int g, int b) {
    r = clampi((r - 16) * 255 / 219, 0, 255);
    g = clampi((g - 16) * 255 / 219, 0, 255);
    b = clampi((b - 16) * 255 / 219, 0, 255);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void compose_line(int y) {
    uint32_t *out = video_fb + y * SCREEN_W;
    int ta = vs.tcr & 15, tb = (vs.tcr >> 8) & 15;
    int mix = !((vs.tcr >> 23) & 1);
    int b_front = (vs.porder & 7) == 1;
    int bk = vs.backdrop;
    int kk = (bk & 8) ? 235 : 126;
    int bdr = (bk & 4) ? kk : 16, bdg = (bk & 2) ? kk : 16, bdb = (bk & 1) ? kk : 16;
    int icfa = vs.p[0].icf, icfb = vs.p[1].icf;
    int two_mattes = (vs.icm >> 19) & 1;
    /* matte processing */
    int mf[2] = {0, 0};
    int next_m[2] = {0, two_mattes ? 4 : 8};
    int end_m[2] = {two_mattes ? 4 : 8, 8};
    int hold[2] = {vs.p[0].mosaic_en ? vs.p[0].mosaic : 1, vs.p[1].mosaic_en ? vs.p[1].mosaic : 1};
    uint32_t heldc[2] = {0, 0};
    for (int x = 0; x < SCREEN_W; x++) {
        for (int s = 0; s < (two_mattes ? 2 : 1); s++) {
            while (next_m[s] < end_m[s]) {
                uint32_t mc = vs.matte[next_m[s]];
                int op = (mc >> 20) & 15;
                int pos = mc & 0x3FF;
                if (op == 0) { next_m[s] = end_m[s]; break; }
                if (pos > x) break;
                int flag = two_mattes ? s : ((mc >> 16) & 1);
                if (op & 8) mf[flag] = op & 1;
                if ((op & 6) == 4) icfa = (mc >> 10) & 63;
                if ((op & 6) == 6) icfb = (mc >> 10) & 63;
                next_m[s]++;
            }
        }
        uint32_t ca = lb[0].rgb[x], cb = lb[1].rgb[x];
        if (hold[0] > 1) { if ((x / 2) % hold[0] == 0 && !(x & 1)) heldc[0] = ca; ca = heldc[0]; }
        if (hold[1] > 1) { if ((x / 2) % hold[1] == 0 && !(x & 1)) heldc[1] = cb; cb = heldc[1]; }
        int tra = lb[0].mode == 0 || transparent(ta, ca, lb[0].is_tbit[x], mf[0], mf[1], vs.p[0].transp_color, vs.p[0].mask_color);
        int trb = lb[1].mode == 0 || transparent(tb, cb, lb[1].is_tbit[x], mf[0], mf[1], vs.p[1].transp_color, vs.p[1].mask_color);
        int ar = apply_icf((ca >> 16) & 255, icfa), ag = apply_icf((ca >> 8) & 255, icfa), ab = apply_icf(ca & 255, icfa);
        int br = apply_icf((cb >> 16) & 255, icfb), bg = apply_icf((cb >> 8) & 255, icfb), bb = apply_icf(cb & 255, icfb);
        int r, g, b;
        if (mix) {
            if (tra) { ar = ag = ab = 16; }
            if (trb) { br = bg = bb = 16; }
            if (tra && trb) { r = bdr; g = bdg; b = bdb; }
            else { r = ar + br - 16; g = ag + bg - 16; b = ab + bb - 16; }
        } else {
            if (b_front) {
                if (!trb) { r = br; g = bg; b = bb; }
                else if (!tra) { r = ar; g = ag; b = ab; }
                else { r = bdr; g = bdg; b = bdb; }
            } else {
                if (!tra) { r = ar; g = ag; b = ab; }
                else if (!trb) { r = br; g = bg; b = bb; }
                else { r = bdr; g = bdg; b = bdb; }
            }
        }
        out[x] = to_host(clampi(r, 0, 255), clampi(g, 0, 255), clampi(b, 0, 255));
    }
}

static void draw_cursor(void) {
    if (!gc.show) return;
    if (gc.blink) {
        int on = (gc.blink >> 8) & 0x7F, off = gc.blink & 0x7F;
        if (off) {
            unsigned period = 12u * (unsigned)(on + off);
            if (field_count % period >= 12u * (unsigned)on) {
                if (!(gc.blink & 0x8000)) return;   /* normal blink: transparent when off */
            }
        }
    }
    int I = (gc.color >> 31) & 1;
    int k = I ? 255 : 127;
    uint32_t col = ((gc.color >> 23) & 1 ? (uint32_t)k << 16 : 0) |
                   ((gc.color >> 15) & 1 ? (uint32_t)k << 8 : 0) |
                   ((gc.color >> 7) & 1 ? (uint32_t)k : 0);
    /* positions are in high resolution (768 x 480) */
    int px = gc.ox + gc.hx - gc.hotx;
    int py = gc.oy + gc.hy - gc.hoty;
    int xs = gc.res == 0 ? 2 : 1;          /* pattern pixel width in double-res pixels */
    for (int y = 0; y < gc.h && y < 32; y++) {
        int sy = (py + y * (gc.res == 2 ? 1 : 2)) / 2;
        if (sy < 0 || sy >= SCREEN_LINES) continue;
        uint16_t bits = gc.ptn[y];
        for (int x = 0; x < 16; x++) {
            if (!(bits & (0x8000 >> x))) continue;
            for (int r = 0; r < xs; r++) {
                int sx = px + x * xs + r;
                if (sx >= 0 && sx < SCREEN_W) video_fb[sy * SCREEN_W + sx] = col;
            }
        }
    }
}

void video_init(void) {
    video_fb = (uint32_t *)calloc(SCREEN_W * SCREEN_LINES, 4);
    memset(&vs, 0, sizeof vs);
    vs.p[0].icf = vs.p[1].icf = 63;
}

/* One display line of a plane: the DCP points at the line's DCA segment.
   By default the next line's segment follows (64 bytes on); RELOAD DCP
   redirects it.  A STOP ends the segment early. */
static void process_dca(int p, uint32_t *dcp) {
    uint32_t a = *dcp & 0xFFFFFC;
    *dcp = a + DCA_BYTES;
    for (int i = 0; i < DCA_BYTES / 4; i++) {
        uint32_t ins = RL(a + (uint32_t)i * 4);
        uint32_t op = ins >> 24;
        if (op < 0x10) break;                                   /* STOP */
        if (op >= 0x20 && op < 0x30) { *dcp = ins & 0xFFFFFC; continue; }   /* RELOAD DCP */
        if (op >= 0x30 && op < 0x40) { *dcp = ins & 0xFFFFFC; break; }      /* RELOAD DCP, STOP */
        if (op >= 0x50 && op < 0x60) {                          /* RELOAD VSR, STOP */
            vs.p[p].vsr = ins & 0xFFFFFF; vs.p[p].vsr_set = 1;
            break;
        }
        exec_ins(p, ins);
    }
}

void video_field(void) {
    pointer_field();
    vs.interrupts = 0;
    uint32_t dcp[2] = {0, 0};
    /* field control tables (ICA): end with the address of the first line's DCA */
    for (int p = 0; p < 2; p++) {
        vs.p[p].vsr_set = 0;
        int id = exec_fct[p];
        if (id <= 0 || id >= MAXFCT || !fcts[id].used) continue;
        FCT *f = &fcts[id];
        if (f->lct > 0 && f->lct < MAXLCT && lcts[f->lct].used) dcp[p] = lct_addr(f->lct, f->lct_line);
        for (int i = 0; i < f->len; i++) {
            uint32_t ins = f->ins[i], op = ins >> 24;
            if (op < 0x10) break;                                           /* STOP */
            if (op >= 0x20 && op < 0x40) { dcp[p] = ins & 0xFFFFFC; if (op >= 0x30) break; continue; }
            if (op >= 0x50 && op < 0x60) { vs.p[p].vsr = ins & 0xFFFFFF; vs.p[p].vsr_set = 1; break; }
            exec_ins(p, ins);
        }
    }
    int planes_on = (exec_fct[0] > 0) || (exec_fct[1] > 0);
    for (int y = 0; y < SCREEN_LINES; y++) {
        for (int p = 0; p < 2; p++)
            if (dcp[p]) process_dca(p, &dcp[p]);
        if (planes_on) {
            decode_line(0);
            decode_line(1);
            compose_line(y);
        } else {
            memset(video_fb + y * SCREEN_W, 0, SCREEN_W * 4);
        }
    }
    /* video interrupts -> DC_SSig */
    int irq = vs.interrupts;
    if (irq == 0 && planes_on) irq = 0;
    for (int k = 0; k < irq && ssig_count >= 0; k++) {
        if (ssig_count == 0) {
            int s = ssig_sig;
            ssig_count = -1;
            sig_send_pid(ssig_pid, (uint32_t)s);
        } else ssig_count--;
    }
    field_count++;
    draw_cursor();
    /* the front end (frontend.c) presents the picture and polls the input */
}

/* ---------------------------------------------------------------- snapshots */
#include "state.h"

void video_state(St *s) {
    for (int i = 0; i < MAXFCT; i++) {
        FCT *f = &fcts[i];
        uint32_t *keep = f->ins;
        ST(s, *f);
        if (s->load) {
            f->ins = keep;
            if (f->used) {
                free(f->ins);
                f->ins = (uint32_t *)calloc((size_t)(f->len > 0 ? f->len : 0) + 1, 4);
            }
        }
        if (f->used && f->len > 0 && f->ins) st_io(s, f->ins, (size_t)f->len * 4);
    }
    ST(s, lcts);
    ST(s, exec_fct);
    ST(s, ssig_sig);
    ST(s, ssig_count);
    ST(s, ssig_pid);
    ST(s, interlace);
    ST(s, vs);
    ST(s, gc);
    ST(s, field_count);
    ST(s, ptr_x);
    ST(s, ptr_y);
    ST(s, ptr_sig);
    ST(s, ptr_sig_pid);
}
