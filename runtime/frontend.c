/* Front end: what happens between two fields, when no 68k process is
   running (os9_run calls frame_boundary after each field): input, hotkeys,
   save states, run-ahead, sprite interpolation and presentation.

   Both editions:
     F2 save to the current quicksave slot, F3 next slot (1-10), F4 load it
     F5 invincibility (the player only)
     F6 run-ahead: off, 1, 2 or 3 fields (default 1)
     F7 sprite interpolation to 60 fps on/off (default on)
   HD edition (apprentice_hd.exe):
     F8 upscaler: Cartoon HD, Cartoon HD+, off

   Run-ahead: after each real field the machine is snapshotted, run R more
   fields with the same input, that field is shown, and the snapshot is
   restored.  The game reacts to input R fields sooner on screen.

   Sprite interpolation: when the game holds a picture for several fields
   (its cut scenes animate at a few pictures a second), the emulation looks
   ahead from the same snapshot for the next picture and moves what moved
   along its path (motion.c), so the motion is shown at 60 frames a second.
   Gameplay already updates every field (60 Hz); it is shown as it is. */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include "cartoon.h"
#include "motion.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 768
#define H 240
#define NPIX (W * H)
#define LOOKAHEAD_MAX 16     /* fields searched for the next picture */
#define LOOKAHEAD_STEP 6     /* fields searched per frame */

extern int g_edition_hd, g_headless, g_turbo;
int g_audio_muted, g_log_muted;
int g_invincible;

/* settings (saves/enhancements.ini, command line, F5-F8) */
static int runahead = 1;
static int interp_on = 1;
static int upscaler = CARTOON_HD;
static int slot = 1;
static int cli_runahead = -1, cli_interp = -1, cli_upscaler = -1;

/* ---------------------------------------------------------------- OSD */
static char osd_msg[96];
static int osd_frames;

static void osd(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(osd_msg, sizeof osd_msg, fmt, ap);
    va_end(ap);
    osd_frames = 150;
    LOG("%s", osd_msg);
}

/* 5x7 font: ' ' .. 'Z' (lower case is shown as upper case) */
static const uint8_t font5x7[][7] = {
    {0,0,0,0,0,0,0},{4,4,4,4,4,0,4},{10,10,0,0,0,0,0},{10,31,10,10,31,10,0},{4,15,20,14,5,30,4},
    {24,25,2,4,8,19,3},{12,18,20,8,21,18,13},{4,4,0,0,0,0,0},{2,4,8,8,8,4,2},{8,4,2,2,2,4,8},
    {0,4,21,14,21,4,0},{0,4,4,31,4,4,0},{0,0,0,0,12,4,8},{0,0,0,31,0,0,0},{0,0,0,0,0,12,12},
    {0,1,2,4,8,16,0},{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},
    {2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,2,12},{0,12,12,0,12,12,0},{0,12,12,0,12,4,8},{2,4,8,16,8,4,2},{0,0,31,0,31,0,0},
    {8,4,2,1,2,4,8},{14,17,1,2,4,0,4},{14,17,1,13,21,21,14},{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14},{28,18,17,17,17,18,28},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},
    {16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},
    {31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},
    {17,17,10,4,10,17,17},{17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
};

/* text at the bottom left; the picture is w x h for a 384x240 screen */
static void osd_draw(uint32_t *img, int w, int h) {
    if (osd_frames <= 0) return;
    int sx = w / 384 > 1 ? w / 384 : 1, sy = h / 240 > 1 ? h / 240 : 1;
    int x0 = 8 * sx, y0 = h - 16 * sy;
    for (int pass = 0; pass < 2; pass++) {          /* shadow, then text */
        uint32_t col = pass ? 0xFFFFE070u : 0xFF000000u;
        int ox = pass ? 0 : sx, oy = pass ? 0 : sy;
        int x = x0;
        for (const char *p = osd_msg; *p; p++, x += 6 * sx) {
            int c = *p;
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c < ' ' || c > 'Z') c = ' ';
            const uint8_t *g = font5x7[c - ' '];
            for (int yy = 0; yy < 7; yy++)
                for (int xx = 0; xx < 5; xx++) {
                    if (!(g[yy] & (16 >> xx))) continue;
                    for (int ry = 0; ry < sy; ry++)
                        for (int rx = 0; rx < sx; rx++) {
                            int px = x + xx * sx + rx + ox, py = y0 + yy * sy + ry + oy;
                            if (px >= 0 && px < w && py >= 0 && py < h) img[py * w + px] = col;
                        }
                }
        }
    }
}

/* ---------------------------------------------------------------- settings */
static const char *upscaler_name(int u) {
    return u == CARTOON_HDPLUS ? "Cartoon HD+" : u == CARTOON_HD ? "Cartoon HD" : "off (original pixels)";
}

static void settings_path(char *p, size_t n) { snprintf(p, n, "%s/enhancements.ini", g_data_dir); }

static void settings_load(void) {
    char p[600];
    settings_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "runahead=%d", &v) == 1 && v >= 0 && v <= 3) runahead = v;
        if (sscanf(line, "interpolation=%d", &v) == 1) interp_on = v != 0;
        if (sscanf(line, "upscaler=%d", &v) == 1 && v >= 0 && v < CARTOON_MODES) upscaler = v;
        if (sscanf(line, "slot=%d", &v) == 1 && v >= 1 && v <= 10) slot = v;
    }
    fclose(f);
}

static void settings_save(void) {
    if (g_headless) return;
    char p[600];
    settings_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "runahead=%d\ninterpolation=%d\nupscaler=%d\nslot=%d\n", runahead, interp_on, upscaler, slot);
    fclose(f);
}

/* --runahead N, --interpolation on|off, --upscale hd|hdplus|off, --invincible */
int frontend_option(const char *opt, const char *val) {
    if (!strcmp(opt, "--runahead") && val) { cli_runahead = atoi(val); return 2; }
    if (!strcmp(opt, "--interpolation") && val) { cli_interp = strcmp(val, "off") != 0; return 2; }
    if (!strcmp(opt, "--upscale") && val) {
        cli_upscaler = !strcmp(val, "hdplus") ? CARTOON_HDPLUS : !strcmp(val, "off") ? CARTOON_OFF : CARTOON_HD;
        return 2;
    }
    if (!strcmp(opt, "--invincible")) { g_invincible = 1; return 1; }
    return 0;
}

/* ---------------------------------------------------------------- presentation */
static uint32_t *work;          /* the picture being prepared */

static void present(const uint32_t *pic) {
    if (g_edition_hd && upscaler != CARTOON_OFF) {
        if (pic != work) memcpy(work, pic, NPIX * 4);
        int ow, oh;
        uint32_t *img = cartoon_upscale(work, W, H, upscaler, &ow, &oh);
        osd_draw(img, ow, oh);
        if (osd_frames > 0) osd_frames--;
        host_present(img, ow, oh);
        return;
    }
    if (osd_frames > 0) {
        if (pic != work) memcpy(work, pic, NPIX * 4);
        osd_draw(work, W, H);
        osd_frames--;
        pic = work;
    }
    host_present(pic, W, H);
}

/* ---------------------------------------------------------------- run-ahead, interpolation */
static St snap;
static uint32_t *pic_p, *pic_n; /* picture shown since tp, and the next one (tn) */
static int64_t tp = -1, tn = -1;
static int n_valid;             /* pic_n/tn known */
static int64_t quiet_until = -1;/* looked ahead: no change before this field */
static int n_b1, n_b2, n_dx, n_dy;  /* input when the look-ahead ran */
static int pair_ready;          /* pic_p -> pic_n has been analysed */
static St la_snap;              /* where the search for the next picture stopped */
static int64_t la_t;            /* ... its field */
static int la_valid;
static int64_t t_real;          /* real fields run */

static void interp_reset(void) {
    tp = tn = quiet_until = -1;
    n_valid = 0;
    la_valid = 0;
    pair_ready = 0;
}

/* The real field t_real has just been emulated (video_fb).  Returns 0 if
   nothing could be snapshotted (then the real field is shown). */
static int enhanced_frame(void) {
    if (!os9_can_snapshot() || state_save(&snap)) return 0;
    int R = runahead;
    g_audio_muted = g_log_muted = 1;
    if (R > 0) os9_run_fields(R);
    int64_t t_disp = t_real + R;
    if (tp < 0 || memcmp(video_fb, pic_p, NPIX * 4)) {         /* a new picture */
        memcpy(pic_p, video_fb, NPIX * 4);
        tp = t_disp;
        n_valid = 0;
        pair_ready = 0;
        quiet_until = -1;
        la_valid = 0;
    }
    /* look ahead only for a picture that is held: a picture that appeared
       this field is usually followed by another one (gameplay changes the
       picture every field, and is shown as it is) */
    if (interp_on && t_disp > tp) {
        /* the input changed the future */
        int changed_input = input.b1 != n_b1 || input.b2 != n_b2 || input.dx != n_dx || input.dy != n_dy;
        int need = (!(n_valid && tn > t_disp) && t_disp >= quiet_until) || (n_valid && changed_input);
        if (changed_input) quiet_until = -1;
        if (need || changed_input) {
            /* the search goes on from where the last frame left it, a few
               fields per frame, so no frame takes much longer than another */
            if (n_valid || changed_input) la_valid = 0;
            n_valid = 0;
            pair_ready = 0;
            if (la_valid && la_t > t_disp) {
                if (state_load(&la_snap)) la_valid = 0;
            } else la_valid = 0;
            if (!la_valid) { la_t = t_disp; n_b1 = input.b1; n_b2 = input.b2; n_dx = input.dx; n_dy = input.dy; }
            for (int k = 0; k < LOOKAHEAD_STEP && la_t - t_disp < LOOKAHEAD_MAX; k++) {
                os9_run_fields(1);
                la_t++;
                if (memcmp(video_fb, pic_p, NPIX * 4)) {
                    memcpy(pic_n, video_fb, NPIX * 4);
                    tn = la_t;
                    n_valid = 1;
                    break;
                }
            }
            la_valid = 0;
            if (!n_valid) {
                if (la_t - t_disp >= LOOKAHEAD_MAX) quiet_until = la_t;
                else la_valid = state_save(&la_snap) == 0;
            }
        }
    }
    if (state_load(&snap)) fatal("run-ahead: snapshot restore failed (error %d)", snap.err);
    g_audio_muted = g_log_muted = 0;
    const uint32_t *show = pic_p;
    if (interp_on && n_valid && tn - tp > 1 && t_disp > tp) {
        if (!pair_ready) {
            motion_prepare(pic_p, pic_n);
            pair_ready = 1;
        }
        motion_render(work, pic_p, pic_n, (float)(t_disp - tp) / (float)(tn - tp));
        show = work;
    }
    present(show);
    return 1;
}

/* ---------------------------------------------------------------- hotkeys */
int host_take_hotkey(void);

static void hotkeys(void) {
    int k;
    while ((k = host_take_hotkey()) != 0) {
        char when[32];
        switch (k) {
        case HOTKEY_SAVE:
            if (state_save_slot(slot)) osd("Slot %d: save failed", slot);
            else osd("Saved to slot %d", slot);
            break;
        case HOTKEY_LOAD:
            if (!state_slot_exists(slot, NULL, 0)) { osd("Slot %d is empty", slot); break; }
            if (state_load_slot(slot)) osd("Slot %d: cannot load (not a save of this game)", slot);
            else { interp_reset(); osd("Loaded slot %d", slot); }
            break;
        case HOTKEY_SLOT:
            slot = slot % 10 + 1;
            if (state_slot_exists(slot, when, sizeof when)) osd("Slot %d  (%s)", slot, when);
            else osd("Slot %d  (empty)", slot);
            settings_save();
            break;
        case HOTKEY_INVINCIBLE:
            g_invincible = !g_invincible;
            interp_reset();
            osd("Invincibility: %s", g_invincible ? "on" : "off");
            break;
        case HOTKEY_RUNAHEAD:
            runahead = (runahead + 1) % 4;
            interp_reset();
            osd("Run-ahead: %d field%s", runahead, runahead == 1 ? "" : "s");
            settings_save();
            break;
        case HOTKEY_INTERP:
            interp_on = !interp_on;
            interp_reset();
            osd("Sprite interpolation: %s", interp_on ? "on (60 fps)" : "off");
            settings_save();
            break;
        case HOTKEY_UPSCALER:
            if (!g_edition_hd) break;
            upscaler = (upscaler + 1) % CARTOON_MODES;   /* Cartoon HD -> HD+ -> off */
            osd("Upscaler: %s", upscaler_name(upscaler));
            settings_save();
            break;
        }
    }
}

/* ---------------------------------------------------------------- frame boundary */
static int inited;

static void init(void) {
    inited = 1;
    work = (uint32_t *)calloc(NPIX, 4);
    pic_p = (uint32_t *)calloc(NPIX, 4);
    pic_n = (uint32_t *)calloc(NPIX, 4);
    settings_load();
    if (g_headless) { runahead = 0; interp_on = 0; }    /* tests compare plain runs */
    if (cli_runahead >= 0 && cli_runahead <= 3) runahead = cli_runahead;
    if (cli_interp >= 0) interp_on = cli_interp;
    if (cli_upscaler >= 0) upscaler = cli_upscaler;
    if (g_edition_hd) cartoon_init();
    LOG("%s edition: run-ahead %d, sprite interpolation %s%s%s%s", g_edition_hd ? "HD" : "original", runahead,
        interp_on ? "on" : "off", g_edition_hd ? ", upscaler " : "", g_edition_hd ? upscaler_name(upscaler) : "",
        g_invincible ? ", invincible" : "");
}

void frame_boundary(void) {
    if (!inited) init();
    host_poll();
    hotkeys();
    t_real++;
    if ((runahead > 0 || interp_on) && !g_turbo && enhanced_frame()) return;
    interp_reset();
    present(video_fb);
}
