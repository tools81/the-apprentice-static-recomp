/* CD File Manager HLE: file reads and real-time play (SS_Play). */
#include "recomp.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>

#define E_EOF   0xD3
#define E_BPNUM 0xC9
#define E_DEVBSY 0xFA
#define E_UNKSVC 0xD0

enum { H_FILE, H_DIR, H_RAW };
typedef struct {
    int used;
    int kind;              /* H_FILE, H_DIR (raw directory records) or H_RAW ("/cd@", the whole disc) */
    const DiscFile *f;     /* file or directory (NULL for H_RAW) */
    uint32_t pos;          /* byte position (2048 per sector) */
} CdHandle;

static CdHandle hs[16];

/* active play */
static struct {
    int active;
    int h;
    uint32_t pcb;
    uint32_t sector;       /* index within file */
    uint32_t nsec;
    int pid;               /* process that started the play */
} play;

void cdfm_init(void) { memset(hs, 0, sizeof hs); memset(&play, 0, sizeof play); }

/* CD-DA play (SS_CDDA): Red Book sectors from the disc image to the mixer */
static struct {
    int active;
    int h;
    uint32_t lsn, left;    /* next sector, sectors still to play */
    uint32_t stat;         /* status block (a0) */
    int pid;
} cdda;

void audio_feed_cdda(const uint8_t *sector2352);

/* name: path below /cd ("music.rtf", "CDDA/track2.cda", "CDDA", "@") */
int cdfm_open(const char *name, int mode) {
    int kind = H_FILE;
    const DiscFile *f = NULL;
    if (!strcmp(name, "@")) kind = H_RAW;
    else {
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        f = disc_find(base);
        if (!f) { LOG("cdfm: file '%s' not found", name); return -1; }
        if (f->attr & 0x8000) kind = H_DIR;
        else if (mode & 0x80) { LOG("cdfm: '%s' is not a directory", name); return -1; }
    }
    for (int i = 0; i < 16; i++) {
        if (!hs[i].used) {
            hs[i].used = 1; hs[i].kind = kind; hs[i].f = f; hs[i].pos = 0;
            return i;
        }
    }
    return -1;
}

/* Table of contents as the drive reports it: Q-subchannel lead-in entries of
   5 bytes (control/adr, point, minutes, seconds, frames in BCD).  Points A0
   (first track, disc type 0x10 = CD-i), A1 (last track) and A2 (lead-out),
   then one entry per track.  Track 1 is the CD-i track; the CD-DA tracks
   are the files in the disc's CDDA directory. */
static uint8_t bcd(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static void msf(uint32_t lsn, uint8_t *p) {
    uint32_t a = lsn + 150;
    p[0] = bcd(a / 4500); p[1] = bcd((a / 75) % 60); p[2] = bcd(a % 75);
}
#define TOC_SIZE 0x1FE
static uint8_t toc[TOC_SIZE];
static int toc_len;

static void build_toc(void) {
    uint32_t start[100] = {0}, end_lsn = 0;
    int last = 1;
    for (int i = 0; i < disc_num_files(); i++) {
        const DiscFile *f = disc_file(i);
        if (!(f->attr & 0x4000) || _strnicmp(f->name, "track", 5)) continue;
        int t = atoi(f->name + 5);
        if (t < 2 || t > 99) continue;
        start[t] = f->lsn;
        if (t > last) last = t;
        uint32_t e = f->lsn + (f->size + 2047) / 2048;
        if (e > end_lsn) end_lsn = e;
    }
    memset(toc, 0, sizeof toc);
    uint8_t *p = toc;
    p[0] = 0x41; p[1] = 0xA0; p[2] = 1; p[3] = 0x10; p += 5;          /* first track 1, CD-i disc */
    p[0] = 0x01; p[1] = 0xA1; p[2] = bcd((uint32_t)last); p += 5;      /* last track */
    p[0] = 0x01; p[1] = 0xA2; msf(end_lsn, p + 2); p += 5;            /* lead-out */
    p[0] = 0x41; p[1] = 0x01; msf(0, p + 2); p += 5;                  /* track 1: CD-i */
    for (int t = 2; t <= last && p + 5 <= toc + TOC_SIZE; t++) {
        if (!start[t]) continue;
        p[0] = 0x01; p[1] = bcd((uint32_t)t); msf(start[t], p + 2); p += 5;
    }
    toc_len = (int)(p - toc);
}

int cdfm_read_toc(uint32_t buf, uint32_t n) {
    if (!toc_len) build_toc();
    for (uint32_t i = 0; i < n; i++) WB(buf + i, i < TOC_SIZE ? toc[i] : 0);
    return 0;
}

void cdfm_close(int h) {
    if (play.active && play.h == h) play.active = 0;
    if (cdda.active && cdda.h == h) cdda.active = 0;
    hs[h].used = 0;
}

uint32_t cdfm_getpos(int h) { return hs[h].pos; }
uint32_t cdfm_size(int h) { return hs[h].f ? hs[h].f->size : 0x7FFFFFFFu; }
int cdfm_seek(int h, uint32_t pos) { hs[h].pos = pos; return 0; }
int cdfm_busy(void) { return play.active; }

int cdfm_read(int h, uint32_t buf, uint32_t n, uint32_t *got) {
    CdHandle *c = &hs[h];
    const DiscFile *f = c->f;
    if (c->kind == H_RAW) {
        /* reading the raw device from the start returns the table of contents */
        if (c->pos == 0) {
            cdfm_read_toc(buf, n);
            *got = n;
            return 0;
        }
        uint8_t raw[2352];
        uint32_t done = 0;
        while (done < n) {
            uint32_t sec = c->pos / 2048, off = c->pos % 2048;
            disc_read_raw(sec, raw);
            uint32_t k = 2048 - off;
            if (k > n - done) k = n - done;
            memcpy(RAM + ((buf + done) & MEM_MASK), raw + 24 + off, k);
            done += k;
            c->pos += k;
        }
        *got = done;
        return 0;
    }
    /* files: their data; directories: their raw directory records */
    if (c->pos >= f->size) return E_EOF;
    if (n > f->size - c->pos) n = f->size - c->pos;
    uint8_t raw[2352];
    uint32_t done = 0;
    while (done < n) {
        uint32_t sec = c->pos / 2048, off = c->pos % 2048;
        disc_read_raw(f->lsn + sec, raw);
        uint32_t k = 2048 - off;
        if (k > n - done) k = n - done;
        memcpy(RAM + ((buf + done) & MEM_MASK), raw + 24 + off, k);
        done += k;
        c->pos += k;
    }
    *got = done;
    return 0;
}

/* ---------------------------------------------------------------- play */
static void play_finish(int err) {
    uint32_t pcb = play.pcb;
    uint16_t st = RW(pcb);
    st |= 0x0100;
    if (err) { st |= 0x8000; }
    WW(pcb, st);
    uint16_t sig = RW(pcb + 2);
    if (err) WW(pcb + 2, (uint32_t)err);
    play.active = 0;
    TRACE("play finished (err %d), sig %u", err, sig);
    if (sig) sig_send_pid(play.pid, sig);
}

static void deliver_to_pcl(uint32_t cil, int chan, const uint8_t *raw, uint32_t size, uint8_t submode, uint8_t coding) {
    uint32_t pcl = RL(cil + chan * 4);
    if (!pcl) return;
    uint8_t ctrl = RB(pcl);
    if (ctrl & 1) {
        LOG("play overrun on PCL %06x (chan %d)", pcl, chan);
        play_finish(0xF4);
        return;
    }
    uint32_t buf = RL(pcl + 10);
    uint32_t bufsz = RL(pcl + 14);
    uint32_t cnt = RL(pcl + 24);
    WB(pcl + 2, submode);
    WB(pcl + 3, coding);
    if (buf) {
        memcpy(RAM + ((buf + cnt) & MEM_MASK), raw + 24, size);
        cnt += size;
        WL(pcl + 24, cnt);
    }
    /* buffer full? (size counted in sectors) */
    uint32_t nsec = size ? cnt / size : 0;
    if (!buf || nsec >= bufsz) {
        WB(pcl, ctrl | 1);
        WL(cil + chan * 4, RL(pcl + 6));     /* CIL -> next PCL */
        uint16_t sig = RW(pcl + 4);
        TRACE("PCL %06x full (%u sectors), sig %u", pcl, nsec, sig);
        if (sig) sig_send_pid(play.pid, sig);
    }
}

static void cdda_finish(uint16_t err) {
    if (cdda.stat) {
        uint16_t st = (uint16_t)(RW(cdda.stat) | 0x0001);
        if (err) { st |= 0x8000; WW(cdda.stat + 2, err); }
        WW(cdda.stat, st);
    }
    cdda.active = 0;
    TRACE("CD-DA play finished");
}

static void cdda_tick(void) {
    if (!cdda.active) return;
    if (!cdda.left) { cdda_finish(0); return; }
    uint8_t raw[2352];
    disc_read_raw(cdda.lsn, raw);
    audio_feed_cdda(raw);
    cdda.lsn++;
    cdda.left--;
    hs[cdda.h].pos = cdda.lsn * 2048;
}

void cdfm_sector_tick(void) {
    cdda_tick();
    if (!play.active) return;
    if (play.sector >= play.nsec) { play_finish(0); return; }
    CdHandle *c = &hs[play.h];
    uint8_t raw[2352];
    disc_read_raw(c->f->lsn + play.sector, raw);
    play.sector++;
    c->pos = play.sector * 2048;
    uint32_t pcb = play.pcb;
    uint8_t fnum = raw[16], chan = raw[17] & 31, submode = raw[18], coding = raw[19];
    if (log_level >= 3) TRACE("  sector %u fn %u ch %u sm %02x ci %02x", play.sector - 1, fnum, chan, submode, coding);
    if (fnum != c->f->fnum) return;
    uint32_t chanmask = RL(pcb + 8);
    int is_v = submode & 2, is_a = submode & 4, is_d = submode & 8;
    if (chanmask & (1u << chan)) {
        uint16_t st = (uint16_t)(RW(pcb) & 0x0100);
        st |= submode & 1 ? 0x01 : 0;
        st |= is_v ? 0x02 : 0;
        st |= is_a ? 0x04 : 0;
        st |= is_d ? 0x08 : 0;
        st |= submode & 0x10 ? 0x10 : 0;
        st |= submode & 0x20 ? 0x20 : 0;
        st |= submode & 0x40 ? 0x40 : 0;
        st |= submode & 0x80 ? 0x80 : 0;
        WW(pcb, st);
        uint32_t size = (submode & 0x20) ? (is_a ? 2304 : 2324) : 2048;
        uint16_t achan = RW(pcb + 12);
        if (!(is_v || is_a || is_d)) {
            /* empty sector: only its EOR/trigger/EOF bits matter */
        } else if (is_a && chan < 16 && (achan & (1u << chan))) {
            audio_feed_sector(raw);
        } else {
            uint32_t cil = is_v ? RL(pcb + 14) : is_a ? RL(pcb + 18) : RL(pcb + 22);
            if (cil && !(is_a && chan >= 16)) deliver_to_pcl(cil, chan, raw, size, submode, coding);
            if (!play.active) return;
        }
        int ev = submode & 0x11;      /* EOR or trigger */
        if (submode & 1) {
            uint32_t rec = RL(pcb + 4);
            if (rec) { rec--; WL(pcb + 4, rec); }
            if (rec == 0) { play_finish(0); return; }
        }
        if (ev) {
            uint16_t sig = RW(pcb + 2);
            if (sig) sig_send_pid(play.pid, sig);
        }
        if (RL(pcb + 4) == 0) { play_finish(0); return; }
    }
    if (submode & 0x80) { play_finish(0); return; }
}

int cdfm_setstat(int h, uint32_t code) {
    switch (code) {
    case 0x2D: { /* SS_Play */
        if (play.active) {
            char bt[256]; int k = 0;
            for (int i = 0; i < 40 && k < 200; i++) {
                uint32_t v = RL(A(7) + i * 2);
                if (v >= recomp_code_lo && v < recomp_code_hi && !(v & 1)) k += sprintf(bt + k, " %06x", v);
            }
            LOG("SS_Play while busy (old pcb %06x %s, new pcb %06x %s):%s", play.pcb, hs[play.h].f->name,
                A(0), hs[h].f->name, bt);
        }
        play.active = 1;
        play.h = h;
        play.pcb = A(0);
        play.pid = os9_cur_pid();
        play.sector = hs[h].pos / 2048;
        play.nsec = (hs[h].f->size + 2047) / 2048;
        WW(play.pcb, 0);
        TRACE("SS_Play %s pcb %06x from sector %u chan %08x achan %04x rec %u",
              hs[h].f->name, play.pcb, play.sector, RL(play.pcb + 8), RW(play.pcb + 12), RL(play.pcb + 4));
        if (log_level >= 2) {
            const char *nm[3] = {"video", "audio", "data"};
            for (int t = 0; t < 3; t++) {
                uint32_t cil = RL(play.pcb + 14 + t * 4);
                if (!cil) continue;
                for (int c = 0; c < (t == 1 ? 16 : 32); c++) {
                    uint32_t pcl = RL(cil + c * 4);
                    if (pcl) TRACE("  %s cil %06x chan %d -> pcl %06x ctrl %02x buf %06x sz %u cnt %u nxt %06x sig %u",
                                   nm[t], cil, c, pcl, RB(pcl), RL(pcl + 10), RL(pcl + 14), RL(pcl + 24), RL(pcl + 6), RW(pcl + 4));
                }
            }
        }
        return 0;
    }
    case 0x31: /* SS_Abort: the PCB reports the play as finished (games poll for it) */
        if (play.active) {
            TRACE("SS_Abort");
            WW(play.pcb, RW(play.pcb) | 0x0100);
            play.active = 0;
        }
        if (cdda.active) {      /* the status block reports the play as finished */
            TRACE("SS_Abort (CD-DA)");
            cdda_finish(0);
        }
        return 0;
    case 0x32: { /* SS_CDDA: play d2 Red Book sectors from the path's position, a0 = status block */
        uint32_t lsn = hs[h].kind == H_RAW ? hs[h].pos / 2048 : (hs[h].f ? hs[h].f->lsn : 0) + hs[h].pos / 2048;
        cdda.active = 1;
        cdda.h = h;
        cdda.lsn = lsn;
        cdda.left = D(2);
        cdda.stat = A(0);
        cdda.pid = os9_cur_pid();
        if (cdda.stat) WW(cdda.stat, 0);
        LOG("SS_CDDA from sector %u, %u sectors", lsn, D(2));
        return 0;
    }
    case 0x3A: /* table of contents (a0 = buffer, d2 = size) */
        cdfm_read_toc(A(0), D(2) & 0xFFFF);
        return 0;
    case 0x30: /* SS_Seek (async) */
        hs[h].pos = D(2);
        return 0;
    case 0x33: /* SS_Pause */
    case 0x36: /* SS_Stop */
    case 0x37: /* SS_Cont */
    case 0x38: /* SS_Disable */
    case 0x39: /* SS_Enable */
        LOG("cdfm setstat %x ignored", code);
        return 0;
    }
    if (code >= 0x3B && code <= 0x3D) return audio_setstat(code);
    LOG("cdfm: unknown setstat %x", code);
    return E_UNKSVC;
}

int cdfm_getstat(int h, uint32_t code) {
    (void)h;
    if (code == 0x3A) { cdfm_read_toc(A(0), D(2) & 0xFFFF); return 0; }
    if (code >= 0x3B && code <= 0x3D) return audio_getstat(code);
    LOG("cdfm: unknown getstat %x", code);
    return E_UNKSVC;
}

/* ---------------------------------------------------------------- snapshots */
#include "state.h"

void cdfm_state(St *s) {
    for (int i = 0; i < 16; i++) {
        int fi = disc_file_index(hs[i].f);     /* files are saved as indices into the disc's table */
        ST(s, hs[i].used);
        ST(s, hs[i].kind);
        ST(s, hs[i].pos);
        ST(s, fi);
        if (s->load) hs[i].f = fi >= 0 && fi < disc_num_files() ? disc_file(fi) : NULL;
    }
    ST(s, play);
    ST(s, cdda);
}
