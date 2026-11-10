#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <commdlg.h>
#endif

static char exe_dir[512] = ".";

static void init_exe_dir(void) {
#ifdef _WIN32
    char p[512];
    DWORD n = GetModuleFileNameA(NULL, p, sizeof p);
    if (n > 0 && n < sizeof p) {
        char *s = strrchr(p, '\\');
        if (s) { *s = 0; snprintf(exe_dir, sizeof exe_dir, "%s", p); }
    }
#endif
}

/* <exe_name>.ini next to the executable: settings the installer writes and the
   user may edit (rom = the disc image or its folder, update_check = 0/1).
   Like everything else the program writes, it stays in the program's folder. */
static void ini_path(char *out, size_t n) { snprintf(out, n, "%s\\%s.ini", exe_dir, EXE_NAME); }

static int ini_get(const char *key, char *out, size_t n) {
    char p[600], line[1024];
    ini_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t kl = strlen(key);
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        if (_strnicmp(line, key, kl) || line[kl] != '=') continue;
        char *v = line + kl + 1;
        v[strcspn(v, "\r\n")] = 0;
        snprintf(out, n, "%s", v);
        found = 0;
    }
    fclose(f);
    return found;
}

/* replace or add key=value, keeping the other lines */
static void ini_set(const char *key, const char *value) {
    char p[600], line[1024], buf[16384] = "";
    ini_path(p, sizeof p);
    size_t kl = strlen(key), used = 0;
    FILE *f = fopen(p, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (!_strnicmp(line, key, kl) && line[kl] == '=') continue;
            size_t L = strlen(line);
            if (used + L + 1 < sizeof buf) { memcpy(buf + used, line, L); used += L; buf[used] = 0; }
        }
        fclose(f);
    }
    f = fopen(p, "w");
    if (!f) return;
    fputs(buf, f);
    if (used && buf[used - 1] != '\n') fputc('\n', f);
    fprintf(f, "%s=%s\n", key, value);
    fclose(f);
}

/* saves (NVRAM files), the log and debug dumps live in <exe dir>/saves */
static void setup_data_dir(void) {
    snprintf(g_data_dir, sizeof g_data_dir, "%s/saves", exe_dir);
#ifdef _WIN32
    _mkdir(g_data_dir);
#endif
}

/* Resolve a .cue to the .bin it references. */
static void cue_to_bin(const char *cue, char *out, size_t n) {
    snprintf(out, n, "%s", cue);
    FILE *f = fopen(cue, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!strncmp(line, "FILE", 4) && q1 && q2) {
            *q2 = 0;
            char dir[512];
            snprintf(dir, sizeof dir, "%s", cue);
            char *sl = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
            if (bs > sl) sl = bs;
            if (sl) { sl[1] = 0; snprintf(out, n, "%s%s", dir, q1 + 1); }
            else snprintf(out, n, "%s", q1 + 1);
            break;
        }
    }
    if (f) fclose(f);
}

/* Is this a raw 2352-byte-sector CD-i image? (disc label "CD-I " at sector 16) */
static int is_cdi_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[8] = {0};
    int ok = fseek(f, 16L * 2352 + 24 + 1, SEEK_SET) == 0 && fread(buf, 1, 5, f) == 5 && !memcmp(buf, "CD-I ", 5);
    fclose(f);
    return ok;
}

static int scan_dir_for_image(const char *dir, char *out, size_t n) {
#ifdef _WIN32
    char pat[600];
    snprintf(pat, sizeof pat, "%s\\*.bin", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        char p[800];
        snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        if (is_cdi_image(p)) { FindClose(h); snprintf(out, n, "%s", p); return 0; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    (void)dir; (void)out; (void)n;
#endif
    return -1;
}

static int find_image(const char *arg, char *out, size_t n) {
    if (arg) {
        size_t L = strlen(arg);
        if (L > 4 && !_stricmp(arg + L - 4, ".cue")) cue_to_bin(arg, out, n);
        else snprintf(out, n, "%s", arg);
        return is_cdi_image(out) ? 0 : -1;
    }
    /* the location given at installation or picked at first start */
    char cfg[600];
    if (!ini_get("rom", cfg, sizeof cfg) && cfg[0]) {
        size_t L = strlen(cfg);
        if (L > 4 && !_stricmp(cfg + L - 4, ".cue")) { cue_to_bin(cfg, out, n); if (is_cdi_image(out)) return 0; }
        else if (L > 4 && !_stricmp(cfg + L - 4, ".bin")) { snprintf(out, n, "%s", cfg); if (is_cdi_image(out)) return 0; }
        else if (!scan_dir_for_image(cfg, out, n)) return 0;
    }
    /* look for rom/*.bin next to the working directory or the executable, up to 4 levels up */
    const char *bases[2] = {".", exe_dir};
    for (int b = 0; b < 2; b++) {
        char dir[600];
        snprintf(dir, sizeof dir, "%s", bases[b]);
        for (int up = 0; up <= 4; up++) {
            char romdir[700];
            snprintf(romdir, sizeof romdir, "%s\\rom", dir);
            if (!scan_dir_for_image(romdir, out, n)) return 0;
            if (!scan_dir_for_image(dir, out, n)) return 0;
            size_t L = strlen(dir);
            if (L + 3 < sizeof dir) strcat(dir, "\\..");
        }
    }
    return -1;
}


/* no disc found: ask where it is, and remember it in the .ini */
static int pick_image(char *out, size_t n) {
#ifdef _WIN32
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "CD-i disc image (*.cue, *.bin)\0*.cue;*.bin\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.lpstrTitle = "Locate the " GAME_TITLE " disc image (.cue or .bin)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    for (;;) {
        if (!GetOpenFileNameA(&ofn)) return -1;
        size_t L = strlen(file);
        if (L > 4 && !_stricmp(file + L - 4, ".cue")) cue_to_bin(file, out, n);
        else snprintf(out, n, "%s", file);
        if (is_cdi_image(out)) { ini_set("rom", file); return 0; }
        MessageBoxA(NULL, "That file is not a CD-i disc image (raw 2352-byte sectors, .bin with its .cue).",
                    GAME_TITLE, MB_ICONWARNING);
    }
#else
    (void)out; (void)n;
    return -1;
#endif
}
