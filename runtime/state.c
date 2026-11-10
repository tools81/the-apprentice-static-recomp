/* Machine state snapshots.  A snapshot is taken between fields, at the top
   of the run loop, where the 68k state is entirely in m68k and RAM. */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include "game_config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define STATE_MAGIC   0x41505354u   /* "APST" */
#define STATE_VERSION 1
#define KERNEL_AREA   0x00F00000u   /* os9.c */
#define KERNEL_SIZE   0x00021000u

void st_io(St *s, void *p, size_t n) {
    if (s->load) {
        if (s->pos + n > s->n) { s->err = 1; memset(p, 0, n); return; }
        memcpy(p, s->buf + s->pos, n);
        s->pos += n;
        return;
    }
    if (s->n + n > s->cap) {
        size_t c = s->cap ? s->cap : 1 << 20;
        while (c < s->n + n) c *= 2;
        uint8_t *nb = (uint8_t *)realloc(s->buf, c);
        if (!nb) { s->err = 1; return; }
        s->buf = nb;
        s->cap = c;
    }
    memcpy(s->buf + s->n, p, n);
    s->n += n;
}

void st_free(St *s) { free(s->buf); memset(s, 0, sizeof *s); }

/* the main program identifies the game a snapshot belongs to */
static uint32_t program_crc(void) {
    static uint32_t crc;
    if (crc) return crc;
    uint32_t c = 0xFFFFFFFFu, end = MAIN_MODULE_BASE + RL(MAIN_MODULE_BASE + 4);
    for (uint32_t a = MAIN_MODULE_BASE; a < end && a < MEM_SIZE; a++) {
        c ^= RB(a);
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return crc = ~c | 1;
}

static void machine(St *s) {
    uint32_t magic = STATE_MAGIC, version = STATE_VERSION, crc = program_crc();
    ST(s, magic); ST(s, version); ST(s, crc);
    if (s->load && (magic != STATE_MAGIC || version != STATE_VERSION || crc != program_crc())) { s->err = 2; return; }
    int64_t cyc = m68k.cyc;
    ST(s, m68k);
    /* the RAM in use: everything below the heap's high-water mark, and the
       kernel's scratch area (stub code, system stacks, system globals) */
    uint32_t hwm = ram_top;
    ST(s, hwm);
    if (hwm > KERNEL_AREA) { s->err = 1; return; }
    st_io(s, RAM, hwm);
    st_io(s, RAM + KERNEL_AREA, KERNEL_SIZE);
    os9_state(s);
    cdfm_state(s);
    audio_state(s);
    video_state(s);
    uint32_t end = STATE_MAGIC;
    ST(s, end);
    if (s->load && end != STATE_MAGIC) s->err = 1;
    (void)cyc;
}

int state_save(St *s) {
    if (!os9_can_snapshot()) return -1;
    s->load = 0; s->n = 0; s->pos = 0; s->err = 0;
    machine(s);
    return s->err ? -1 : 0;
}

int state_load(St *s) {
    s->load = 1; s->pos = 0; s->err = 0;
    machine(s);
    return s->err;
}

/* ---------------------------------------------------------------- slots */
/* on disk: runs of zero bytes are packed (RAM is mostly empty) */
static void slot_path(int slot, char *out, size_t n) {
    snprintf(out, n, "%s/quick_%02d.state", g_data_dir, slot);
}

int state_save_slot(int slot) {
    St s = {0};
    if (state_save(&s)) { st_free(&s); return -1; }
    char p[600];
    slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) { st_free(&s); return -1; }
    uint32_t raw = (uint32_t)s.n;
    fwrite(&raw, 4, 1, f);
    for (size_t i = 0; i < s.n;) {
        size_t z = 0;
        while (i + z < s.n && s.buf[i + z] == 0 && z < 0x7FFFFFFF) z++;
        if (z >= 8) {   /* 0x00 marker, 4-byte run length */
            uint8_t m = 0; uint32_t len = (uint32_t)z;
            fwrite(&m, 1, 1, f); fwrite(&len, 4, 1, f);
            i += z;
            continue;
        }
        size_t lit = 0;   /* 0x01 marker, length, literal bytes */
        while (i + lit < s.n && lit < 0x7FFFFFFF) {
            size_t zz = 0;
            while (i + lit + zz < s.n && s.buf[i + lit + zz] == 0 && zz < 8) zz++;
            if (zz >= 8) break;
            lit += zz ? zz : 1;
        }
        uint8_t m = 1; uint32_t len = (uint32_t)lit;
        fwrite(&m, 1, 1, f); fwrite(&len, 4, 1, f); fwrite(s.buf + i, 1, lit, f);
        i += lit;
    }
    int bad = ferror(f);
    fclose(f);
    st_free(&s);
    return bad ? -1 : 0;
}

int state_load_slot(int slot) {
    if (!os9_can_snapshot()) return -1;
    char p[600];
    slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    uint32_t raw = 0;
    if (fread(&raw, 4, 1, f) != 1 || raw > (64u << 20)) { fclose(f); return -1; }
    St s = {0};
    s.buf = (uint8_t *)calloc(raw ? raw : 1, 1);
    s.n = s.cap = raw;
    size_t at = 0;
    uint8_t m; uint32_t len;
    while (at < raw && fread(&m, 1, 1, f) == 1 && fread(&len, 4, 1, f) == 1) {
        if (len > raw - at) break;
        if (m == 1 && fread(s.buf + at, 1, len, f) != len) break;
        at += len;
    }
    fclose(f);
    int r = -1;
    if (at == raw) {
        /* keep the running state in case the file does not fit this game */
        St cur = {0};
        if (state_save(&cur) == 0) {
            r = state_load(&s);
            if (r) state_load(&cur);
        }
        st_free(&cur);
    }
    st_free(&s);
    return r ? -1 : 0;
}

int state_slot_exists(int slot, char *when, size_t n) {
    char p[600];
    slot_path(slot, p, sizeof p);
    struct stat st;
    if (stat(p, &st)) return 0;
    if (when) {
        struct tm *tm = localtime(&st.st_mtime);
        strftime(when, n, "%Y-%m-%d %H:%M", tm);
    }
    return 1;
}
