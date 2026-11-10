/* CPU support: dispatcher, ALU helpers that are too large to inline. */
#include "recomp.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

M68K m68k;
uint8_t *RAM;
uint32_t jt_expected;

static RecompFn *dispatch;      /* indexed by (pc - recomp_code_lo) / 2 */
static uint32_t dispatch_n;

void cpu_init(void) {
    dispatch_n = (recomp_code_hi - recomp_code_lo) / 2 + 1;
    dispatch = (RecompFn *)calloc(dispatch_n, sizeof(RecompFn));
    for (int i = 0; i < recomp_num_entries; i++) {
        uint32_t a = recomp_entries[i].addr;
        dispatch[(a - recomp_code_lo) / 2] = recomp_entries[i].fn;
    }
}

const char *func_name(uint32_t pc) {
    /* nearest symbol (MacsBug or .stb) at or below pc */
    const char *best = "?";
    uint32_t ba = 0;
    for (int i = 0; i < recomp_num_names; i++) {
        if (recomp_names[i].addr <= pc && recomp_names[i].addr >= ba) {
            ba = recomp_names[i].addr;
            best = recomp_names[i].name;
        }
    }
    return best;
}

/* Is there translated code for this exact address?  Addresses inside a
   recompiled module that the analyzer did not discover have none: they run in
   the interpreter, which hands back to translated code at the next known
   entry point. */
int recomp_has_entry(uint32_t pc) {
    if (pc < recomp_code_lo || pc >= recomp_code_hi || (pc & 1)) return 0;
    return dispatch[(pc - recomp_code_lo) / 2] != NULL;
}

static void note_undiscovered(uint32_t pc) {
    static uint32_t seen[256];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == pc) return;
    if (nseen < 256) seen[nseen++] = pc;
    LOG("note: code at %06x (%s) was not found by the analyzer; it runs in the interpreter "
        "(add it to extra_entries in game.json)", pc, func_name(pc));
}

/* Resolve pc to a host function; follows "jmp abs.l" stubs in the a5/a6
   jump tables that live in RAM. */
static RecompFn lookup(uint32_t *ppc) {
    uint32_t pc = *ppc;
    for (int hops = 0; hops < 4; hops++) {
        if (pc >= recomp_code_lo && pc < recomp_code_hi && !(pc & 1)) {
            RecompFn f = dispatch[(pc - recomp_code_lo) / 2];
            if (f) { *ppc = pc; return f; }
            break;
        }
        if (RW(pc) == 0x4EF9) {          /* jmp abs.l (jump table stub): count it as executed */
            pc = RL(pc + 2);
            m68k.cyc++;
            continue;
        }
        break;
    }
    *ppc = pc;
    return NULL;
}

void fatal(const char *fmt, ...);
uint32_t interp_run(uint32_t pc);

uint32_t recomp_bad_entry(uint32_t pc, uint32_t func) {
    /* a label of another function's body, or an address in the middle of a
       function: let the dispatcher (or the interpreter) take it */
    (void)func;
    if (recomp_has_entry(pc)) return m68k_call(pc);
    note_undiscovered(pc);
    return interp_run(pc);
}

uint32_t interp_run(uint32_t pc);

uint32_t m68k_call(uint32_t target) {
    extern int g_interp_all;
    uint32_t pc = target;
    if (g_interp_all) return interp_run(pc);
    RecompFn f = lookup(&pc);
    if (!f) {
        if (pc >= recomp_code_lo && pc < recomp_code_hi) note_undiscovered(pc);
        return interp_run(pc);
    }
    return f(pc);
}

void jt_check(int reg, uint32_t addr) {
    fatal("A%d changed (%08x, expected %08x) at %06x", reg, A(reg), jt_expected, addr);
}

uint32_t m68k_exception(int vec, uint32_t addr) {
    fatal("CPU exception %d at %06x (%s)", vec, addr, func_name(addr));
    return 0;
}

/* ---------------------------------------------------------------- run loop */
void cpu_run_until(volatile int *flag) {
    extern int g_interp_all;
    while (!*flag) {
        uint32_t pc = m68k.pc;
        RecompFn f = g_interp_all ? NULL : lookup(&pc);
        uint32_t r;
        if (f) r = f(pc);
        else {
            if (!g_interp_all && pc >= recomp_code_lo && pc < recomp_code_hi) note_undiscovered(pc);
            r = interp_run(pc);
        }
        /* PC_UNWIND: a system call already put the resume address in m68k.pc */
        if (r != PC_UNWIND) m68k.pc = r;
        /* the scheduler runs only where code yielded (backward branches), so
           translated and interpreted execution see events at the same points */
        if (m68k.yield) {
            m68k.yield = 0;
            sched_service();
        }
    }
}

void cpu_run(void) {
    os9_run();      /* the OS-9 scheduler: runs every process until it gives up the CPU */
}
