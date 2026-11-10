/* Entry point: load the disc, set up the HLE OS and run the recompiled game. */
#include "recomp.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "find_image.inc"
#include "game_config.h"

int log_level = 1;
static FILE *logfp;
extern int g_headless, g_dump_frames, g_max_frames;

void logf_(int lvl, const char *fmt, ...) {
    (void)lvl;
    if (g_log_muted && lvl > 0) return;     /* fields emulated ahead are not logged */
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
    if (logfp) { fprintf(logfp, "%s\n", buf); fflush(logfp); }
}

void fatal(const char *fmt, ...) {
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logf_(0, "FATAL: %s", buf);
    logf_(0, "  pc=%06x d=%08x %08x %08x %08x %08x %08x %08x %08x", m68k.pc,
          D(0), D(1), D(2), D(3), D(4), D(5), D(6), D(7));
    logf_(0, "  a=%08x %08x %08x %08x %08x %08x %08x %08x",
          A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7));
    if (!RAM) goto out;
    /* crude 68k stack backtrace: print code addresses found on the stack */
    for (int i = 0; i < 64; i++) {
        uint32_t v = RL(A(7) + i * 4);
        if (v >= recomp_code_lo && v < recomp_code_hi)
            logf_(0, "  stack+%03x: %06x (%s)", i * 4, v, func_name(v));
    }
    {
        char rp[600];
        snprintf(rp, sizeof rp, "%s/ramdump.bin", g_data_dir);
        FILE *rf = fopen(rp, "wb");
        if (rf) { fwrite(RAM, 1, MEM_SIZE, rf); fclose(rf); }
    }
out:
#ifdef _WIN32
    if (!g_headless) {
        MessageBoxA(NULL, buf, "Recomp fatal error", MB_ICONERROR);
    }
#endif
    exit(1);
}

/* hooked instructions (game.json "trace_hooks"): logged with --trace */
int game_skip_insn(uint32_t addr) {
    TRACE("hook %06x (%s) d0=%08x d1=%08x a0=%06x a6=%06x", addr, func_name(addr), D(0), D(1), A(0), A(6));
    return 0;
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

int frontend_option(const char *opt, const char *val);

int main(int argc, char **argv) {
    const char *img_arg = NULL;
    unsigned pokes[16][2];
    int npokes = 0, no_update = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--no-update")) no_update = 1;
        else if (!strcmp(argv[i], "--portable")) { /* always portable: accepted for shortcuts */ }
        else if (!strcmp(argv[i], "--trace")) log_level = 2;
        else if (!strcmp(argv[i], "--trace3")) log_level = 3;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) g_dump_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aspect") || !strcmp(argv[i], "--filter") || !strcmp(argv[i], "--size")) {
            extern void host_video_option(const char *, const char *);
            if (i + 1 < argc) { host_video_option(argv[i], argv[i + 1]); i++; }
        }
        else if (!strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "--windowed") || !strcmp(argv[i], "--gdi")) {
            extern void host_video_option(const char *, const char *);
            host_video_option(argv[i], NULL);
        }
        else if (frontend_option(argv[i], i + 1 < argc ? argv[i + 1] : NULL)) {
            if (frontend_option(argv[i], i + 1 < argc ? argv[i + 1] : NULL) == 2) i++;
        }
        else if (!strcmp(argv[i], "--interp")) { extern int g_interp_all; g_interp_all = 1; }
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) { extern int g_fuzz; g_fuzz = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { extern void host_wav_open(const char *); host_wav_open(argv[++i]); }
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) { extern void host_set_script(const char *); host_set_script(argv[++i]); }
        else if (!strcmp(argv[i], "--dump-from") && i + 1 < argc) { extern unsigned long long g_dump_from; g_dump_from = strtoull(argv[++i], NULL, 10); }
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) { extern uint32_t g_watch; g_watch = (uint32_t)strtoul(argv[++i], NULL, 16); }
        else if (!strcmp(argv[i], "--ramdump-at") && i + 1 < argc) {
            extern unsigned g_ramdump_at[16]; extern int g_nramdump;
            for (char *q = argv[++i]; *q && g_nramdump < 16;) { g_ramdump_at[g_nramdump++] = (unsigned)strtoul(q, &q, 10); if (*q == ',') q++; }
        }
        else if (!strcmp(argv[i], "--callprof") && i + 1 < argc) { extern int g_callprof_from; g_callprof_from = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) {
            /* debug: ADDR:BYTE written after the main program is loaded (several allowed) */
            unsigned a, v;
            if (sscanf(argv[++i], "%x:%x", &a, &v) == 2 && npokes < 16) { pokes[npokes][0] = a; pokes[npokes][1] = v; npokes++; }
        }
        else img_arg = argv[i];
    }
    init_exe_dir();
    setup_data_dir();
    {
        char lp[600];
        snprintf(lp, sizeof lp, "%s/recomp.log", g_data_dir);
        logfp = fopen(lp, "w");
    }
    LOG("%s %s", GAME_TITLE, APP_VERSION);
    if (!g_headless && !no_update) {
        /* <exe_name>.ini: update_check=1 ask (default), auto, 0 off */
        char v[16] = "1";
        ini_get("update_check", v, sizeof v);
        if (v[0] != '0') {
            extern void update_check_start(const char *exe_dir, int automatic);
            update_check_start(exe_dir, !_stricmp(v, "auto"));
        }
    }
    char img[1024];
    if (find_image(img_arg, img, sizeof img) && (g_headless || img_arg || pick_image(img, sizeof img))) {
        fatal("Could not find the " GAME_TITLE " disc image.\n\n"
              "Put the .bin/.cue in a 'rom' folder next to the program, or pass the path to the .cue or .bin as an argument.\n"
              "(A .chd image is converted to .bin/.cue by build.bat, or with: python tools/chd.py rom/<image>.chd)");
    }
    LOG("disc image: %s", img);
    if (disc_open(img)) fatal("failed to open disc image %s", img);

    RAM = (uint8_t *)calloc(MEM_SIZE + 16, 1);
    cpu_init();
    os9_init();
    host_init();
    video_init();
    audio_init();
    cdfm_init();
    sched_init();

    const DiscFile *df = disc_find(MAIN_MODULE);
    if (!df) fatal("program module '%s' not found on the disc (is this the right game?)", MAIN_MODULE);
    uint8_t *mod; uint32_t len;
    disc_read_file(df, &mod, &len);
    os9_load_main_module(mod, len);
    free(mod);
    for (int i = 0; i < npokes; i++) {
        LOG("poke %06x = %02x", pokes[i][0], pokes[i][1]);
        RAM[pokes[i][0] & MEM_MASK] = (uint8_t)pokes[i][1];
    }
    sched_init();

    cpu_run();
    return 0;
}
