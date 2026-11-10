# CD-i / CD-RTOS notes for static recompilation

These are facts worked out while porting *Link: The Faces of Evil*. The Green
Book (CD-i Full Functional Specification, May 1994) describes the APIs but
mostly omits numeric codes. The codes below were inferred from the game's
library wrappers and the parameter lists in the spec. An entry marked *(seen)*
was confirmed by actual use in that game.

## Disc and boot

* Sectors are raw 2352 bytes, Mode 2: sync (12), header (4), subheader
  (file, channel, submode, coding, ×2), then data. Form 1 carries 2048 bytes
  and Form 2 carries 2324 (audio uses 2304).
* The disc label is at LSN 16 (`"CD-I "` at data offset 1). The application
  identifier at label offset 574 names the boot module, for example `cdi_link`.
* Real-time files (`.rtr`, `.rtf`) have file number ≥ 1. Their directory size
  counts only that file's sectors, and padding sectors use file number 0.
  Empty sectors (submode with no V/A/D bit) can still carry EOR, trigger or EOF
  and **must be processed** for play-control purposes.

## OS-9 process start (what the runtime emulates)

* The kernel copies idata into static storage and applies irefs. The first
  table is code-relative (+module base) and the second data-relative (+data
  base).
* On entry, **a6 = static storage + 0x8000**, a5 = a7 = parameter area,
  a3 = module, d5 = parameter size and d6 = total memory.
* MPW-built programs (Link): idata begins with a jump table header
  `[size, a5 offset]` followed by `dc.w seg; jmp abs.l` stubs copied to
  `a5+0x20`. `jsr N(a5)` jumps to a stub in RAM, which costs one extra
  instruction. Count it, or cycle-exact comparisons with the interpreter
  drift. Function names are present as MacsBug symbols after each function.
* Microware C programs (the Philips "bumper"): a6-relative data and calls of
  the form `movea.l #off,aN / jsr d8(pc,aN.l)`.

## Kernel behaviour the games rely on

* **Signals** are delivered asynchronously. Games busy-wait on flags that
  their intercept handlers set, so translated code must yield at backward
  branches.
* **F$Sleep and F$Wait clear the signal mask.** The standard pattern is
  "mask, start an operation, sleep".
* While an intercept routine runs, signals are masked. F$RTE restores the
  context and unmasks.
* F$Alarm `A$Cycle` (d1=2) with time bit 31 set means the time is in 1/256 s.
  Link's 16 Hz game tick is `0x80000010`.
* F$Fork + F$Wait: a forked child (for example the bumper) runs to completion
  while the parent waits. Asynchronous signal sources must target the process
  that requested them.
* Common system call numbers: F$Link 00, F$Load 01, F$UnLink 02, F$Fork 03,
  F$Wait 04, F$Exit 06, F$Send 08, F$Icpt 09, F$Sleep 0A, F$ID 0C, F$Time 15,
  F$UnLoad 1D, F$RTE 1E, F$TLink 21, F$SetSys 27, F$SRqMem 28, F$SRtMem 29,
  F$Alarm 56, F$SigMask 57, F$SRqCMem 5C, I$Create 83, I$Open 84, I$Delete 87,
  I$Seek 88, I$Read 89, I$Write 8A, I$ReadLn 8B, I$WritLn 8C, I$GetStt 8D,
  I$SetStt 8E, I$Close 8F.
* Error codes: E$BMode CB, E$MemFul CF, E$UnkSvc D0, E$EOF D3, E$PNNF D8,
  E$MNF DD, E$DevBsy FA.

## Devices and the CSD

* Games read `/nvr/csd`. It holds text Device Status Descriptors of the form
  `type:name:params` ending in CR. Types:
  * 1 is the CD control unit (`/cd`);
  * 2 is the audio processor (`/ap`);
  * 3 is video (`/vid`, with `LI="525"` or `"625"`);
  * 4 is NVRAM (`/nvr`, `SZ#8`);
  * 5 is the pointer (`/ptr`, `CL="d"` for a manoeuvring device).
* GetStat 0x43 on `/nvr` returns free NVRAM bytes in d2 *(seen)*.

## CDFM (`/cd` files)

| SetStat | Code | Notes |
|---------|------|-------|
| SS_Play | 0x2D | a0 = PCB *(seen)* |
| SS_Abort | 0x31 | *(seen)* |
| SS_Seek | 0x30 | |
| SS_Enable / SS_Disable / SS_Cont / SS_Stop / SS_Pause | 0x39 / 0x38 / 0x37 / 0x36 / 0x33 | probable numbering |

* **PCB:** Stat.w, Sig.w, Rec.l, Chan.l, AChan.w, then CIL pointers for video,
  audio and data.
  * PCB_Sig is sent on EOR, trigger and finish.
  * Rec is decremented on each EOR, and the play ends when it reaches 0.
* **PCL:** Ctrl.b, res, SMode.b, Type.b, Sig.w, Nxt.l, Buf.l, BufSz.l (in
  sectors), Err.l, res.w, Cnt.l.
  * When the buffer fills, Ctrl bit 0 is set, the CIL entry advances to Nxt,
    and PCL_Sig is sent.
  * A full PCL that comes round again is an overrun. That ends the play with
    an error.
* Plays run at 75 sectors per second (1×). Audio sectors whose channel is in
  AChan go to the audio processor. Otherwise they go to the audio CIL.

## Audio (`/ap`)

* SS_SM = 0x3B. These are SetStat unless marked GetStat:
  * Creat 0 (coding, groups) returns the id and a0 = data;
  * Out 1 (id, a0 = asynchronous status block);
  * Off 2;
  * Close 3;
  * Info 0 is GetStat and returns a0 = descriptor.
* **SM_Out on a busy processor queues the new soundmap** to start when the
  current sector finishes. Games double-buffer music this way and pace
  themselves on the completion signal (ASY_Sig), so completions must be
  sample-exact.
* SS_SC = 0x3D, SC_Atten d3: bytes 2 and 0 are the direct paths (L→L, R→R),
  bytes 3 and 1 the cross paths. 0xFF is off.
* ADPCM coding byte:
  * bits 0-1 select stereo (01 = stereo);
  * bits 2-3 select the rate (00 = 37.8 kHz, 01 = 18.9 kHz);
  * bits 4-5 select the sample size (00 = 4-bit, 01 = 8-bit).

  A sound group is 128 bytes: 16 parameter bytes (copies at 4..11) followed by
  112 data bytes. The filter coefficients are the XA ones.

## Video (`/vid`)

* SS_DC = 0x56 (d2 = function):

  | Code | Function | Code | Function |
  |------|----------|------|----------|
  | 0 | CrFCT | 0xA | WrLI |
  | 1 | RdFCT | 0xB | DlLCT |
  | 2 | WrFCT | 0xC | FLnk |
  | 3 | RdFI | 0xD | LLnk |
  | 4 | WrFI | 0xE | Exec |
  | 5 | DlFCT | 0x11 | SSig |
  | 6 | CrLCT | 0x12 | Relea |
  | 7 | RdLCT | 0x1B | PRdLCT |
  | 8 | WrLCT | 0x1C | PWrLCT |
  | 9 | RdLI | | |

  Seen: 0, 2, 4, 6, 7, 0xA, 0xC, 0xD, 0xE, 0x11 and 0x1C. 0xF and 0x13 are
  probably Intl and SetCmp; they're ignored.
* LCT line numbers are in UCM coordinates: normal-resolution tables hold half
  the lines. "P" variants use physical lines.
* SS_GC = 0x52: Pos 0, Show 1, Hide 2, Ptn 3, Col 4, Org 5, Blnk 6.
  Coordinates are high resolution (768×480).
* SS_PT = 0x59: Coord 0 (GetStat) returns d0 = buttons (bit 15 = active) and
  d1 = H:V. SSig 1, Relea 2, **Pos 3, Org 4** (note this order).
* Video interrupts come only from DCP instruction `$60`. DC_SSig counts them.
* DCP opcodes:

  | Opcode | Meaning |
  |--------|---------|
  | 10 | NOP |
  | 20 | link (driver-internal) |
  | 40 | display line start |
  | 60 | interrupt |
  | 78 | display parameters (RMS bits 0-1, 10 = run-length) |
  | 80-BF | CLUT entry within the current bank |
  | C0 | coding: CM0 bits 0-3, CM1 bits 8-11, NM bit 19, CS bit 22 |
  | C1 | transparency: TA bits 0-3, TB bits 8-11, bit 23 = no mix |
  | C2 | plane order |
  | C3 | bank |
  | C4 / C6 | transparent colour |
  | C7 / C9 | mask colour |
  | CA / CB | DYUV start values |
  | D0-D7 | mattes |
  | D8 | backdrop |
  | D9 / DA | mosaic |
  | DB / DC | ICF (0-63) |

* RL7: a byte with bit 7 set starts a run of colour b&0x7F whose length is the
  next byte (0 = to the end of the line). Otherwise the byte is a single pixel.
* DYUV deltas: 0,1,4,9,16,27,44,79,128,177,212,229,240,247,252,255.
* Colours are in studio range (black 16, white 235).
* Link draws sprites with **compiled sprite routines loaded from disc into
  RAM**. They run in the interpreter.

## Timing model used by the runtime

* Emulated time counts executed 68k instructions (`CPU_IPS` = 4M per second).
* Video fields (60 Hz), CD sectors (75 Hz), alarms and soundmap completions are
  scheduled on that clock. Pacing to the wall clock happens once per field.
* Translated code yields only at backward branches (`BUDGET`), and the
  interpreter yields by the same rule. That keeps `--interp` runs comparable
  frame for frame.

## Learned from The Apprentice

### Disc

* **CD-i Ready layout.** The CD-i track lives in the pregap of track 1
  (69150 sectors) and is followed by 22 Red Book tracks. A CHD stores that
  pregap as audio, byte-swapped like all CD audio in a CHD. Swap it back to get
  Mode 2 sectors. LSN 0 is the first pregap sector, at absolute MSF 00:02:00.
* **CD-DA files.** Directory `/CDDA` holds `trackNN.cda` entries (attribute bit
  14). Each entry's LSN is the track start, and its size counts 2048 bytes per
  sector.
* **The table of contents.** Reading the raw device `/cd@` from position 0, or
  SetStat $3A with a0 = buffer and d2 = size (0x1FE), returns the lead-in TOC
  as 5-byte Q-subchannel entries: control/adr, point, then M, S, F in BCD.
  * Points A0, A1 and A2 come first. A0 holds the first track, then the disc
    type: 0x00 for CD-DA, 0x10 for CD-i.
  * One entry per track follows.
* **SS_CDDA = 0x32.** Plays d2 sectors of CD-DA from the path's position: seek
  the raw device to LSN×2048. a0 points to a status block:
  * byte 0, bit 7: error;
  * byte 1, bit 0: finished;
  * word 2: error code.

  SS_Abort must mark the block finished, because games poll it.
* **SS_Abort and SS_Play.** SS_Abort of an SS_Play likewise sets PCB_Stat bit 8
  (finished). The game's `cd_wait` polls it.
* **Directory paths.** I$Open of a directory with mode 0x81 returns its raw
  directory records on I$Read, 2048 bytes per sector.

### Kernel

* **F$Fork.** d4.w is the priority (0 = the parent's). A module with attribute
  bit 5 (SupStat) runs in system state.
* **System-state "threads".** The game's thread module `cdi_sysproc` finds
  `"SYS0"`, a code address and an a6 in its parameters. It pops the magic and
  returns into the parent's code: a thread sharing the parent's data. OS-9
  does not time-slice system-state processes.
* **F$Alarm.** A$Wake (d1 = 1) sends S$Wake after d3 ticks. A process blocked
  in F$Wait wakes up, and F$Wait returns with d0 = 0.
* **Events.** Ev$Delet wakes the processes waiting on the event, and their
  wait fails with E$EvNF. Games use that to stop a waiting thread.
* **F$DatMod = 0x25.**
  * Inputs: d0 = data size, d1 = attr/rev, d2 = access, d3 = type/lang,
    a0 = name.
  * Results: a1 = data, a2 = header.
  * The data starts at M$Exec, so F$Link of the module gives a1 = data.
* **F$SetSys.** Offset 0x8A6 is D_MinPty; the game raises it to 0xFFFF while
  its thread runs. 0x48 is D_PthDBT (path descriptor table), 0x3A4/0x3A8 are
  the system/user dispatch tables. Location 0 holds the address of the system
  globals.
* **F$GPrDsc = 0x18.** Copies the process descriptor. P$Path, at offset 0x168,
  maps process path numbers to system path numbers.
* **F$SRtMem.** It must reject blocks outside free memory. The Apprentice frees
  a directory buffer with size and address swapped, and on a player that is a
  harmless error.

### Trap libraries

* **cio (trap #13)** is the Microware C library in ROM. Stubs are
  `func: trap #13 / dc.w N`, packed one after another without rts, and the
  handler returns to the stub's caller.
  * Arguments: d0, d1, then the stack.
  * errno: a6 - 0x7FF8.
  * When cio is linked with F$TLink, the caller puts 9 at (a3) and cio
    replies by clearing it.
  * Numbers:

    | Range | Functions |
    |-------|-----------|
    | 0x0E-0x1D | sprintf ... sscanf (0x1A), access (0x1B), create (0x1C), creat (0x1D) |
    | 0x1E-0x23 | open, mknod, unlink, unlinkx, dup, read |
    | 0x24-0x2A | readln, write, writeln, lseek, getstat, setstat, close |
    | 0x2B | called by exit before F$Exit |
    | 0x2C-0x31 | directory functions |
    | 0x3A | time |

* **Unlinked user traps.** OS-9 programs link cio and math lazily: an unlinked
  trap enters the program's exception entry, which links the library and backs
  the pc up by 4.
* **`cdi_vbltrap` (trap #9, Philips).**
  * fn 3 patches the display driver (`dspdriv`).
  * fn 1 (d0 = event id, d1 = routine) hooks F$Event in both dispatch tables.
    A pulse (Ev$Pulse, 9 or $8009) of that event runs the routine instead,
    with the a6 saved when the trap was linked. That event is the driver's
    vertical-blank `line_event`, so it amounts to a VBL callback.
  * fn 0 removes the hook.
  * fn 2 (F$Sleep hook) and fn 4 (bigger interrupt stack) are not used by
    the game.

### Video

* **Line control tables live in system RAM** as MCD212 display control areas:
  one 64-byte segment of 16 instructions per display line.
  * The DCP moves to the next segment after each line unless an instruction
    reloads it:
    * `0x2xxxxxxx` reloads the DCP;
    * `0x3xxxxxxx` reloads the DCP and stops;
    * `0x0xxxxxxx` stops the line;
    * `0x4x`/`0x5x` reload VSR, the latter also stopping.
  * DC_LLnk writes `0x20000000 | address` in column 7.
* **Finding the LCT's address.** The game creates a one-line LCT, links it to
  its LCT and reads the link back with DC_RdLI (column 7). The low 24 bits
  are the address. After that it writes display instructions straight into
  the table, 280 lines × 64 bytes.
* **Start line.** CSD `LI="525"`: the game uses LCT line 0 as the first
  visible line (startline 0). For 625 lines it uses 20.

### Audio

* **One looping soundmap.** The game streams its music (ADPCM from
  `music.rtf`, received into buffers through SS_Play PCLs) and its sound
  effects into a single soundmap, so the audio processor must read the map
  from RAM while it plays. SS_SM function 4 releases the map at exit.
