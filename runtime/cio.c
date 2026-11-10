/* The Microware C I/O trap library "cio" (trap #13), emulated.

   On a CD-i player cio lives in ROM.  Programs reach it through stubs of the
   form "func: trap #13 / dc.w N", one after another with no rts between
   them: the handler returns straight to the stub's caller.  Arguments follow
   the Microware C convention: the first two in d0 and d1, the rest on the
   stack above the return address; the result is in d0, and errors go to the
   program's errno (offset 0x8008 from its static storage base, a6 - 0x7FF8).

   Function numbers (from the game's symbol table, cdi_app.stb):
     0x0E sprintf  0x1A sscanf  0x1B access  0x1C create  0x1D creat
     0x1E open     0x1F mknod   0x20 unlink  0x21 unlinkx 0x22 dup
     0x23 read     0x24 readln  0x25 write   0x26 writeln 0x27 lseek
     0x28 getstat  0x29 setstat 0x2A close   0x2B (stdio cleanup, from exit)
     0x2C-0x31 directory functions, 0x32 os9exec, 0x33 _prgname,
     0x34 modloadp, 0x35 getenv, 0x36-0x3A time functions, 0x40 makdir */
#include "recomp.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

int os9_hle_call(uint32_t code);      /* os9.c: run a non-blocking system call; 0 or error */

static uint32_t arg(int n) {          /* n >= 2: stack arguments */
    return RL(A(7) + 4 + 4 * (uint32_t)(n - 2));
}

static void set_errno(uint32_t e) { WL(A(6) - 0x7FF8, e); }

/* ---------------------------------------------------------------- sprintf */
static double be_double(uint32_t a) {
    uint64_t v = ((uint64_t)RL(a) << 32) | RL(a + 4);
    double d;
    memcpy(&d, &v, sizeof d);
    return d;
}

/* format into emulated memory at buf; varargs start at emulated address ap */
static uint32_t do_format(uint32_t buf, uint32_t fmt, uint32_t ap) {
    uint32_t out = buf;
    for (;;) {
        char c = (char)RB(fmt++);
        if (!c) break;
        if (c != '%') { WB(out++, (uint8_t)c); continue; }
        char spec[32];
        int k = 0;
        spec[k++] = '%';
        c = (char)RB(fmt++);
        while (c && strchr("-+ #0", c) && k < 8) { spec[k++] = c; c = (char)RB(fmt++); }
        if (c == '*') { k += snprintf(spec + k, sizeof spec - k, "%d", (int32_t)RL(ap)); ap += 4; c = (char)RB(fmt++); }
        else while (c >= '0' && c <= '9' && k < 16) { spec[k++] = c; c = (char)RB(fmt++); }
        if (c == '.') {
            spec[k++] = c; c = (char)RB(fmt++);
            if (c == '*') { k += snprintf(spec + k, sizeof spec - k, "%d", (int32_t)RL(ap)); ap += 4; c = (char)RB(fmt++); }
            else while (c >= '0' && c <= '9' && k < 24) { spec[k++] = c; c = (char)RB(fmt++); }
        }
        int sh = 0;
        while (c == 'l' || c == 'h') { if (c == 'h') sh = 1; c = (char)RB(fmt++); }
        char tmp[512];
        tmp[0] = 0;
        switch (c) {
        case 'd': case 'i': {
            int32_t v = (int32_t)RL(ap); ap += 4;
            if (sh) v = (int16_t)v;
            spec[k++] = 'd'; spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uint32_t v = RL(ap); ap += 4;
            if (sh) v &= 0xFFFF;
            spec[k++] = c; spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'p': snprintf(tmp, sizeof tmp, "%x", RL(ap)); ap += 4; break;
        case 'c': spec[k++] = 'c'; spec[k] = 0; snprintf(tmp, sizeof tmp, spec, (int)(RL(ap) & 0xFF)); ap += 4; break;
        case 's': {
            char str[256];
            uint32_t sp = RL(ap); ap += 4;
            int n = 0;
            while (n < 255 && sp && RB(sp + (uint32_t)n)) { str[n] = (char)RB(sp + (uint32_t)n); n++; }
            str[n] = 0;
            spec[k++] = 's'; spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, str);
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            double v = be_double(ap); ap += 8;
            spec[k++] = c; spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case '%': tmp[0] = '%'; tmp[1] = 0; break;
        case 0: fmt--; break;
        default: tmp[0] = '%'; tmp[1] = c; tmp[2] = 0; break;
        }
        for (char *t = tmp; *t; t++) WB(out++, (uint8_t)*t);
    }
    WB(out, 0);
    return out - buf;
}

/* ---------------------------------------------------------------- dispatcher */
uint32_t cio_trap(uint32_t func, uint32_t next) {
    (void)next;
    uint32_t ret_pc = RL(A(7));            /* the stub's caller */
    M68K saved = m68k;
    uint32_t a0 = D(0), a1 = D(1);
    int32_t result = -1;
    int err = 0;
    switch (func) {
    case 0x0E: /* sprintf(buf, fmt, ...) */
        result = (int32_t)do_format(a0, a1, A(7) + 4);
        break;
    case 0x1B: /* access(name, mode) */
        A(0) = a0; D(0) = a1 & 0xFF;
        err = os9_hle_call(0x84);
        if (!err) { os9_hle_call(0x8F); result = 0; }
        break;
    case 0x1C: /* create(name, mode, perm [, size]) */
        A(0) = a0; D(0) = a1 & 0xFF; D(1) = arg(2) & 0xFFFF; D(2) = arg(3);
        err = os9_hle_call(0x83);
        if (!err) result = (int32_t)(D(0) & 0xFFFF);
        break;
    case 0x1D: /* creat(name, perm) */
        A(0) = a0; D(0) = 2; D(1) = a1 & 0xFFFF;
        err = os9_hle_call(0x83);
        if (!err) result = (int32_t)(D(0) & 0xFFFF);
        break;
    case 0x1E: /* open(name, mode) */
        A(0) = a0; D(0) = a1 & 0xFF;
        err = os9_hle_call(0x84);
        if (!err) result = (int32_t)(D(0) & 0xFFFF);
        break;
    case 0x20: case 0x21: /* unlink(name) */
        A(0) = a0; D(0) = 0;
        err = os9_hle_call(0x87);
        if (!err) result = 0;
        break;
    case 0x22: /* dup(fd) */
        D(0) = a0;
        err = os9_hle_call(0x82);
        if (!err) result = (int32_t)(D(0) & 0xFFFF);
        break;
    case 0x23: case 0x24: /* read / readln(fd, buf, n) */
    case 0x25: case 0x26: /* write / writeln(fd, buf, n) */
        D(0) = a0; A(0) = a1; D(1) = arg(2);
        err = os9_hle_call(func == 0x23 ? 0x89 : func == 0x24 ? 0x8B : func == 0x25 ? 0x8A : 0x8C);
        if (!err) result = (int32_t)D(1);
        else if (err == 0xD3) { err = 0; result = 0; }     /* end of file: 0 bytes */
        break;
    case 0x27: { /* lseek(fd, offset, whence) */
        uint32_t whence = arg(2), pos = a1;
        if (whence == 1 || whence == 2) {
            D(0) = a0; D(1) = whence == 1 ? 5 : 2;         /* SS_Pos / SS_Size */
            err = os9_hle_call(0x8D);
            if (err) break;
            pos += D(2);
        }
        D(0) = a0; D(1) = pos;
        err = os9_hle_call(0x88);
        if (!err) result = (int32_t)pos;
        break;
    }
    case 0x2A: /* close(fd) */
        D(0) = a0;
        err = os9_hle_call(0x8F);
        if (!err) result = 0;
        break;
    case 0x2B: /* stdio cleanup at exit: nothing buffered here */
        result = 0;
        break;
    case 0x3A: { /* time(tloc) */
        extern int g_headless;
        time_t t = g_headless ? (time_t)(820454400 + m68k.cyc / CPU_IPS) : time(NULL);
        result = (int32_t)t;
        if (a0) WL(a0, (uint32_t)t);
        break;
    }
    default: {
        static uint8_t warned[256];
        if (!warned[func & 0xFF]) {
            warned[func & 0xFF] = 1;
            LOG("cio function $%02x at %06x (%s) not implemented: returning -1", func, ret_pc, func_name(ret_pc));
        }
        err = 0xD0;
        break;
    }
    }
    int64_t cyc = m68k.cyc, lim = m68k.cyc_limit;
    m68k = saved;
    m68k.cyc = cyc;
    m68k.cyc_limit = lim;
    if (err) { set_errno((uint32_t)err); result = -1; }
    TRACE("cio $%02x(%x, %x) -> %d (err %x) from %06x", func, a0, a1, result, err, ret_pc);
    D(0) = (uint32_t)result;
    A(7) += 4;
    return ret_pc;
}
