# PLAN — PicoDoom on Waveshare RP2350-PiZero

Goal: run the classic id DOOM engine (doom/ = Linux DOOM 1.10 source, 1997) on the
RP2350-PiZero with a 3.5" Waveshare RPi LCD (A) (ILI9486 + XPT2046), input via PIO-USB,
WADs from the µSD card, and sound.

Current state (verified **on real hardware**, 2026-09-10): the engine compiles, links,
boots, mounts the µSD, loads `doom.wad`, and **reaches a live running game loop** over
USB serial on `waveshare_rp2350_pizero` -> `build/PicoDoom.uf2`. PSRAM-backed zone
heap (`doom/i_system.c` `#ifdef PICO` -> `psram_malloc`), WAD I/O through `src/sd_stdio.c`
(FatFs-backed `_open/_read/_lseek/_fstat/_stat` newlib syscall shim over PIO-SPI uSD),
and USB-serial console output via pico_stdio. Video/sound/net are still null stubs
(`doom/i_*_null.c`) — the engine ticks at 35 Hz and processes `screens[]` (dropped).

Getting from "builds clean" to "boots on hardware" took six real bugs, all fixed
(three more turned up in Phase 2/3, see below):
- `IdentifyVersion` (d_main.c) only set the WAD-path pointers under `NORMALUNIX`, never
  defined here -> wild-pointer `access()` calls, hard fault, killed USB before it could
  enumerate. Added a `#ifdef PICO` path setting them to plain SD-root filenames.
- `I_AllocLow`'s 250 KB screen-buffer `malloc` (i_system.c) didn't fit in the ~172 KB
  free after DOOM's static tables ate most of the 512 KB SRAM -> now PSRAM-backed like
  the zone heap.
- `W_AddFile`/`W_Reload` (w_wad.c) used `alloca()` for the WAD directory table (tens of
  KB for a full IWAD) on this target's ~2 KB stack -> heaped instead.
- `doomtype.h` makes `boolean` a real C99 `bool` under PICO (`<stdbool.h>` is
  unconditionally included, so the classic `enum{false,true}` trick won't even compile
  here) -- but two places in the engine relied on a `boolean` holding -1 as a tri-state
  "unset" sentinel via `memset(...,-1,...)`, which isn't well-defined for a real 1-byte
  `bool`: `p_spec.c`'s `animdefs[]` terminator (fixed by bounding the loop with
  `sizeof()` instead of the sentinel) and `spriteframe_t.rotate` (r_defs.h, retyped to
  `signed char`). Grepped for more instances of this pattern; these were the only two.
- `i_net_null.c`'s `I_InitNetwork` was a pure no-op, leaving the global `doomcom`
  pointer unset -> `D_CheckNetGame` (d_net.c) dereferences it unconditionally. Now
  mirrors `i_net.c`'s real single-player (no `-net` arg) setup path.
- `doomdef.h`'s `VERSION` was `110`; verified against `wad/DOOM.WAD`'s own DEMO1/2/3
  lumps (first byte of each) that v1.9 IWADs actually embed `109` -> every built-in
  demo failed the version check at the title screen. Fixed to `109`.

Seventh bug, found via a systematic type-size audit (below) after Phase 2 made visible
rendering corruption possible to actually see: `maptexture_t` (r_data.c) is cast
directly onto raw `TEXTURE1`/`TEXTURE2` WAD bytes and had a `boolean masked` field --
never actually read anywhere, but its size still has to match the file format (4 bytes,
same as the original `enum` `boolean`) or every field after it (`width`, `height`, ...,
`patches[]`) gets read from the wrong file offset. Confirmed via `offsetof()` on the
real toolchain: under PICO's 1-byte `boolean`, `width`/`height` landed 2 bytes off from
canonical, i.e. every texture's dimensions were being misread. Retyped `masked` to
`int`, which pins the layout regardless of what `boolean` means on a given platform.

### Type-size audit (Linux x86-32 vs. this ARM/PICO build)
Prompted by "some rendering engine bugs, not related to the LCD" after Phase 2 first
lit up the panel. Checked with the real toolchain, not assumed:
- `int`/`long`/`short`/pointer are all the same size on both (ILP32 either way) --
  not a source of bugs here.
- Endianness matches (both little-endian).
- Bitfields: none used anywhere in `doom/`.
- `char` signedness differs: **signed by default on x86 Linux GCC, unsigned by default
  on this ARM EABI toolchain** (confirmed by compiling a `CHAR_MIN < 0` test both
  ways). `doomtype.h`'s `MINCHAR`/`MAXCHAR` macros assume signed and are wrong here,
  but grepped and neither is ever actually used anywhere in the engine -- dead macros,
  left alone.
- `boolean` (1 byte under PICO vs. the original 4-byte `enum`) was already known to
  break value-carrying sentinel tricks (see above); this audit's job was checking
  every WAD/file-format struct (`doomdata.h`'s map structs, `r_data.c`'s texture/patch
  structs, `w_wad.h`'s directory structs) for a `boolean` field that would shift
  everything after it. Found and fixed the one in `maptexture_t`; every other
  WAD-mapped struct uses only `char`/`short`/`int`/`byte` fields, which are
  identically sized on both platforms, so no other layout mismatches exist there.
  Every other `boolean` in the engine is a plain flag on a self-consistent (same
  build reads and writes it) runtime struct or variable -- fine regardless of size.

Eighth bug, found chasing "keyboard works in menu but not to move the player"
(Phase 3): `m_misc.c`'s `defaults[]` table -- what `M_LoadDefaults()` uses to
initialize `key_up`/`key_down`/`key_left`/`key_right`/`key_fire`/`key_use`/
`key_strafe`/`key_speed` -- has the entire block of key-binding entries
wrapped in `#ifdef NORMALUNIX`. Same root cause as the very first Phase 1 bug
(`IdentifyVersion`'s WAD-path pointers): `NORMALUNIX` is a Linux-Makefile-only
macro this CMake build never defines, so those entries were never in the
table at all, and `key_up` etc. stayed at their BSS zero-init value (0) --
`gamekeydown[key_up]` then checks `gamekeydown[0]`, which a real keypress
(HID usage IDs start at 0x04) never sets. Menu navigation, screen-size +/-,
and ESC all worked throughout because `m_menu.c` compares hardcoded `KEY_*`
constants directly, never going through this table -- only gameplay movement
(`g_game.c`'s `G_BuildTiccmd`, which reads the configurable `key_*`
variables) was affected, which is what made the symptom look input-pipeline-
specific rather than a missing-default. Fixed with a `#ifdef PICO` block
alongside the `NORMALUNIX` one (not widening that condition -- `SNDSERV` is
unconditionally defined in doomdef.h, so folding `PICO` in would also pull
in an unrelated `sndserver_filename` entry that doesn't exist for this
build).

Ninth bug, found chasing "keyboard/mouse can move forward but not backward,
insensitive to mouse sensitivity" (Phase 3 mouse follow-up): `d_ticcmd.h`'s
`ticcmd_t.forwardmove`/`sidemove` are declared plain `char`, not `signed
char`. Same root mechanism as the type-size audit's `char`-signedness
finding above (signed by default on x86 Linux GCC, unsigned by default on
this ARM toolchain) -- except this time it has a real, live effect instead
of landing on dead macros: `G_BuildTiccmd` (g_game.c) does
`cmd->forwardmove += forward`, where `forward` is a signed `int` that can be
negative (backward movement, or a negative mouse-Y delta); writing a small
negative value into an effectively-`unsigned char` field wraps it to a large
value near 256, later misread downstream as a large *positive* (forward)
move -- backward became impossible, replaced by an always-near-max-speed
forward, and "insensitive to sensitivity" followed directly since -1 and -5
both wrap to a value near 256 either way. `G_ReadDemoTiccmd` (same file,
~30 lines away) already explicitly casts `(signed char)` when reading these
exact fields back from a demo buffer -- confirming the original id Software
authors' own intent, just never stated in the struct declaration itself.
Diagnosed by printing the value at three points (driver's raw HID dy ->
engine's `mousey` -> final `cmd->forwardmove`) and watching a correct small
negative number turn into 254/255/253 at the very last step. Fixed by
declaring both fields `signed char` -- identical size and byte layout to
plain `char` (so no effect on demo file or network wire format, only on how
the same byte gets sign-interpreted after reading).

Tenth bug, found chasing a hard freeze during startup ("P_Init: Init
Playloop state." then nothing, no further output) after Phase 4.5's video
performance work (2026-09-11) -- turned out completely unrelated to that
work, just newly exposed by it (first time this specific commercial
`doom.wad` -- Doom II's IWAD, not the shareware/registered one used in
earlier tests -- made it this far). Bisected with temporary `#ifdef PICO`
printfs at each step of `P_Init()` and then each sprite name inside
`R_InitSpriteDefs()` (r_things.c): hung right after printing garbage for
sprite index `[138/145]`, immediately following a *correct* `[137/145]
TLP2` -- the last of `info.c`'s real 138 `sprnames[]` entries. Root cause:
`info.h` declares `extern char *sprnames[NUMSPRITES]` (exactly 138, no
extra slot), but `R_InitSpriteDefs()` counts entries by scanning for a NULL
terminator (`while (*check != NULL) check++`) -- reading `sprnames[138]` is
undefined behavior on *any* platform; it happened to "work" on the original
x86_64 Linux build purely because whatever the linker placed right after
`sprnames[]` in memory there contained a zero soon enough. On this build's
memory layout it doesn't: the scan walked 7 pointers past the real array
before finding a zero (`numsprites=145` printed, not 138), and
`R_InitSpriteDefs()` then dereferenced those 7 garbage "pointers" as sprite
name strings, hard-faulting silently (no crash message -- `printf("%s",
garbage_pointer)` walked off into unmapped memory) on the first one whose
garbage bytes didn't luckily form a short valid-looking C string. Fixed at
the actual bug site rather than working around it in `R_InitSpriteDefs()`'s
generic-looking scan: `info.h`/`info.c` now declare `sprnames[NUMSPRITES+1]`
under `#ifdef PICO`, and C's aggregate-initialization rules zero-fill the
one slot `info.c`'s 138-entry initializer list doesn't cover, giving the
scan a real terminator instead of relying on adjacent-memory luck. Same
class of "relies on the platform's incidental memory layout" bug as the
`{-1}`-sentinel `animdefs[]` bug (sixth bug above), but this one isn't a
sentinel-*value* problem (boolean-size-related) -- it's an out-of-bounds
*read* that both platforms technically commit, one just gets away with it.

Eleventh bug, found chasing a reproducible mid-game hard freeze
(2026-09-11) -- gameplay, not a boot issue like the others, and the first
one that wasn't caused by a platform-specific type-size/layout difference.
Reported as "freezes running the demo by itself, always at the same
point"; bisected entirely through printf instrumentation rather than
static analysis, since the freeze prints nothing (silent, no `I_Error`) --
the exact signature of a genuine infinite loop, not a controlled failure:
1. A periodic `gametic` heartbeat in `G_Ticker()` (`doom/g_game.c`),
   unconditional (not gated on `demoplayback`) since the user suspected it
   might be level-content-triggered rather than demo-specific, narrowed
   the freeze to a ~35-tic (1s) window, then to the exact tic pair
   (830/831) once narrowed further with a per-tic version of the same
   print restricted to that window.
2. Added periodic calls to `Z_CheckHeap()` (`doom/z_zone.c`) -- DOOM's own
   built-in zone-heap corruption checker, normally only called once, right
   after a level loads -- alongside the heartbeat, on the theory that a
   corrupted block list could cause exactly this kind of silent hang
   (`Z_Malloc`'s purge-scan loop has no bound if the list is broken).
   `Z_CheckHeap()` passed clean on every tic right up to the freeze,
   ruling that specific failure mode out.
3. The user's own qualitative report -- "close after a barrel exploding and
   while opening a secret wall" -- pointed at thinker removal (a killed
   barrel's mobj gets torn down) as the trigger. Checked
   `P_RunThinkers()` (`doom/p_tick.c`) and found a real, well-known bug in
   id Software's own original source: when removing a thinker, the code
   calls `Z_Free(currentthinker)` and then reads
   `currentthinker = currentthinker->next` -- **after** that exact memory
   was just freed. Undefined behavior that happened to "work" on the
   original target only because that build's `Z_Free` doesn't touch a
   freed block's payload bytes immediately, with no other allocation
   between the free and the read. `doom/z_zone.c` is unmodified vanilla
   code here (confirmed no `#ifdef PICO` divergence), so this isn't a
   behavior difference in the allocator itself -- more likely this port's
   different allocation history (PSRAM zone, different lazy-loading
   pattern from `precache=false` during demo playback) produces a heap
   layout where the freed block's `next` field doesn't happen to survive
   intact, and the thinker-list walk then follows a garbage pointer that
   never finds its way back to the list's `&thinkercap` sentinel --
   spinning forever, silently, matching every observed symptom exactly
   (no error, fully reproducible, tied to a thinker-removal event).
   Fixed under `#ifdef PICO` by capturing `next` from the list *before*
   calling `Z_Free`, instead of re-reading it through the already-freed
   pointer afterward -- the standard fix several other source ports
   (Chocolate Doom included) apply for this exact bug.

   **Hardware result: real but incomplete.** Same repro, freeze moved from
   tic 829 to 832 -- confirms the fix had a genuine effect (not a no-op),
   but something else hangs 3 tics later. Checked `doom/p_saveg.c`'s
   `P_UnArchiveThinkers()` for the same anti-pattern (it also frees
   thinkers mid-walk) -- already correctly captures `next` before freeing,
   not a second instance of this bug.

   Reconsidered the user's own description -- a secret door revealing a
   lot of new geometry at once -- against a *second*, better-known vanilla
   DOOM bug: `doom/r_segs.c`'s `R_StoreWallRange()` writes into
   `openings[]` (via `lastopening`, 3 sites: masked-texture columns, top/
   bottom sprite clip) with **no bounds check at the write site at all**.
   The only check in the original source (`r_plane.c`'s `R_DrawPlanes()`)
   runs once per frame, *after* every wall for that frame -- including
   whichever one overflows -- has already been rendered. A silent
   out-of-bounds write into whatever `.bss` data happens to follow
   `openings[MAXOPENINGS]` (20480 entries), not a crash or an error, just
   quiet corruption of something else -- exactly the kind of thing that
   could corrupt an unrelated structure like the thinker list, explaining
   why fixing the thinker-list symptom shifted the freeze without curing
   it. A sudden reveal of lots of new wall segments in one frame (a secret
   door opening) is precisely the scenario that pushes `lastopening`
   furthest.

   Fixed under `#ifdef PICO`: moved `MAXOPENINGS`/`openings[]` into
   `r_plane.h` (previously local to `r_plane.c`) so `r_segs.c` can see the
   real bound, and added a check before each of the 3 write/advance sites
   -- turns the silent corruption into a controlled `I_Error()` naming
   which one hit the limit, instead of corrupting memory quietly.

   **Hardware result: ruled out.** Same repro, `lastopening` never
   overflowed (no `R_StoreWallRange` error) -- freeze still happened,
   this time at tic 831 (829, then 832, now 831 across three builds).
   Demo playback is fully deterministic -- the recorded demo drives every
   input, nothing reads a live clock or real controls -- so a stable bug
   should freeze on the exact same tic every time. Drifting by a tic or two
   with every code change, instead, points at memory corruption whose
   visible symptom depends on exact `.bss` layout, which shifts slightly
   whenever code is added anywhere in the program.

   Rather than keep guessing at named functions one at a time (three
   hardware round-trips so far), instrumented the two loops most likely to
   actually be the one hanging, directly: `P_RunThinkers()`'s thinker-list
   walk (`doom/p_tick.c`, the loop containing the already-fixed
   use-after-free) and `P_BlockThingsIterator()`'s blockmap-cell walk
   (`doom/p_maputl.c`, walked by `PIT_RadiusAttack` -- the barrel
   explosion's own damage pass, which can kill other things mid-walk).
   Both get a generous step bound (100000 -- no real DOOM II level's
   thinker count or single blockmap cell's occupancy comes remotely close)
   that turns "spins forever, silently" into a named, controlled
   `I_Error()` if either list's traversal is actually what never
   terminates.

   **Hardware result: both cleared.** Neither bound tripped; freeze still
   happened, this time at tic 830 (829, 832, 831, now 830 across four
   builds) -- consistent with the same layout-dependent-corruption pattern
   continuing, just not caused by either of these two loops specifically.

   Looked one level further into what `PIT_RadiusAttack`'s `P_CheckSight`
   call actually does: `P_PathTraverse()` (`doom/p_maputl.c`) builds a list
   of `intercepts[]` (every line/thing crossed along a traced line) via
   `PIT_AddLineIntercepts()`/`PIT_AddThingIntercepts()`, both of which
   write through `intercept_p++` with **no bounds check anywhere in the
   original source** -- the exact same unchecked-array-write pattern as
   `openings[]` above, except `MAXINTERCEPTS` is only **128** entries (vs.
   `openings[]`'s 20480), and `P_PathTraverse()` is used by both of the
   user's described trigger events: `P_CheckSight` (the barrel
   explosion's own sight-check) and `P_UseLines` (a secret door). A long
   trace through busy geometry -- exactly what a newly-revealed secret
   area produces -- can plausibly exceed 128 crossings.

   Given the strength of the fit and how cheap `intercept_t` is (~12
   bytes), fixed more decisively this time instead of just adding a
   diagnostic: bounds-checked both write sites (`I_Error()` instead of
   silent overflow, same as `openings[]`) *and* raised `MAXINTERCEPTS`
   from 128 to 1024 under `#ifdef PICO` (`doom/p_local.h`) -- confirmed via
   the linker map: `intercepts[]` is now exactly 12288 bytes (12KB), heap
   budget still ~126KB free, comfortably above what's needed.

   **Hardware result: cleared too.** No new error, freeze still happened
   (tic 831 this time -- 829/832/831/830/831 across five builds, still
   wobbling in the same narrow band).

   Checked one more well-known DOOM danger zone: `P_SetMobjState()`
   (`doom/p_mobj.c`) chains through zero-tic states in a `do { ... } while
   (!mobj->tics)` loop with **no bound at all** -- if a state chain ever
   cycles (corrupted `states[]` data, or a corrupted `state`/`mobj->tics`
   value reaching this call), this hangs immediately, entirely self-
   contained, no other function involved. This is literally what drives a
   barrel's explosion animation. Added the same style of step bound (1000
   -- no real DOOM state chain is remotely this long) to confirm or rule
   this out too. ⏳ Not yet confirmed on hardware.

   Running tally of what's been fixed regardless of whether it's *the*
   cause (all real, confirmed-unchecked-write or confirmed-UB bugs, worth
   having fixed either way): `P_RunThinkers()` use-after-free (bug #11,
   confirmed real -- shifted the freeze point when fixed), `r_segs.c`'s
   `openings[]` overflow (checked, not implicated), `intercepts[]`
   overflow (checked, not implicated, headroom raised regardless).

   **Hardware result: cleared too, same tic (831) as the previous test.**
   Six hardware round-trips now, all testing the same theory -- some loop
   spins forever -- and all six coming back clean, including
   `P_SetMobjState()`'s completely self-contained state-chain loop. That
   run of clean results is itself the signal: reconsidered the theory
   rather than the eighth candidate loop.

   A silent freeze with *zero* output isn't only what an infinite loop
   looks like -- it's also exactly what an unhandled CPU hard fault looks
   like (an invalid memory access, or calling through a corrupted function
   pointer -- `state_t::action`, `thinker_t::function` -- and jumping to
   garbage). Checked what this build's fault handling actually does:
   `src/rp2_common/pico_crt0/crt0.S`'s `isr_hardfault` is a *weak* symbol
   whose default body is a single `bkpt #0` -- no debug probe attached (this
   board runs standalone over USB CDC), so that instruction executes
   silently and leaves the core halted. No message, no reboot, nothing:
   indistinguishable from every one of the six "infinite loop" hypotheses
   already tested, and none of those loop-bound guards can ever catch it,
   because nothing is actually looping.

   Added `src/FaultHandler.cpp`: a real `isr_hardfault` (a strong symbol
   overrides the SDK's weak default -- confirmed via the linked ELF's
   vector table, which now points at it) that prints the faulting PC/LR
   and the standard Cortex-M fault status registers (CFSR/HFSR/MMFAR/BFAR)
   before halting, instead of doing nothing. One real wrinkle: HardFault
   runs at the highest configurable exception priority, so the low-priority
   IRQ `stdio_usb`'s printf normally relies on to actually push bytes out
   over USB can never preempt us to finish that -- worked around by calling
   TinyUSB's `tud_task()` directly and repeatedly ourselves instead of
   waiting for an interrupt that will never come. ⏳ Not yet hardware-tested
   -- if this really is a hard fault (plausible after six clean loop
   checks), this should finally show the actual faulting address, which
   `arm-none-eabi-addr2line -e build/PicoDoom.elf <PC>` (or
   `PicoDoom.elf.map`) can map straight back to a source line -- no more
   guessing which function to instrument next. Diagnostics (tic heartbeat,
   `Z_CheckHeap()`, three step bounds, the `lastopening` checks) all still
   in place too, in case it's still a loop somewhere none of the six have
   covered.

### `<stdint.h>` migration (post-mortem on the ninth bug)

Three real bugs (`d_ticcmd.h`, `spriteframe_t.rotate`, `maptexture_t.masked`)
all came from the same root cause: a plain C type (`char`, `boolean`) whose
size or signedness quietly differs between this port's two platforms.
`doomtype.h` now pulls in `<stdint.h>` (available everywhere that already
includes it, which is nearly the whole engine), and every struct that
crosses a real boundary -- a WAD/demo file, the event queue between driver
and engine, or a would-be network packet -- is pinned to explicit-width
types instead of `int`/`short`/`char`/`long`:
- `ticcmd_t` (`d_ticcmd.h`) -- `int8_t forwardmove/sidemove`,
  `int16_t angleturn/consistancy`, `uint8_t chatchar/buttons`.
- `spriteframe_t.rotate` (`r_defs.h`) -- `int8_t` (was `signed char`,
  identical fix, just spelled with the self-documenting width).
- `maptexture_t`/`mappatch_t` (`r_data.c`, the struct directly responsible
  for the earlier `masked` bug) -- every field pinned (`int32_t`/`int16_t`),
  not just the one that already bit us.
- `event_t` (`d_event.h`) -- `data1/2/3` to `int32_t` (`type` stays the real
  `evtype_t` enum -- enums size consistently on both platforms already, not
  the same risk class, see the type-size audit above).
- `doomdata_t`/`doomcom_t` (`d_net.h`) -- every field, including `long id`
  -> `int32_t` (`long` is 8 bytes on plenty of real LP64 targets, even
  though it's 4 here on both of this port's platforms).

Deliberately NOT touched: the other ~500KB of engine code (rendering,
physics, menu, etc.), where `int`/`short` sizes are already proven identical
between the original platform and this one -- rewriting working, unrelated
code to `<stdint.h>` types there would be pure churn with no bug behind it.
If a genuinely different-width target is ever in scope, that's the point to
revisit it.

A `PICODOOM_DIAG_STAGE` CMake option was added during Phase 1 to bisect exactly
this kind of "no USB output" failure without a debug probe — heartbeats at each
init stage instead of falling through. Removed once boot was solid through
Phase 3 (all nine bugs above found and fixed) — if a similar silent-boot
failure ever needs bisecting again, `git log` has the mechanism to resurrect.

Build:
```
cmake -S . -B build -DPICO_SDK_PATH=/home/lohengrin/.pico-sdk/sdk/2.3.0
cmake --build build -j$(nproc)
```
(`PICO_BOARD`/`PICO_BOARD_HEADER_DIRS` default in CMakeLists before
`pico_sdk_import.cmake`.)

Reference project (read first, reuse everything): /home/lohengrin/Dev/TOM6809 —
validated on this exact hardware. Its `build-pico-pizero-lcd` profile uses
`pico/boards/waveshare_rp2350_pizero.h` (adds `PICO_PSRAM_CS_PIN=47`, PIO-USB pins),
`include/pico/Ili9486Display.hpp` + `src/ui_pico/Ili9486Display.cpp`, `Psram.cpp`,
`PicoSdCard_Waveshare.cpp`, `PicoUsbHidInput.cpp`, `Xpt2046Touch.cpp`, and
`pico_fatfs`. LCD wiring (SPI1): SCK=GPIO10, MOSI=GPIO11, MISO=GPIO12 (touch),
CS=GPIO8, D/C=GPIO24, RST=GPIO25.

## Phase 1 — Boot to title screen (critical path)

1. **PSRAM zone heap.** ✔ Done — `I_ZoneBase` under `#ifdef PICO` returns
   `psram_malloc(6 MB)` (src/Psram.cpp). `PICO_BOARD_HEADER_DIRS` points at the copied
   TOM6809 board header so `PICO_PSRAM_CS_PIN` exists.
2. **WAD from µSD.** ✔ Done — src/sd_stdio.c mounts the uSD (PIO-SPI CS43/MOSI31/
   MISO40/SCK30) and replaces newlib's weak file syscalls with FatFs ones, so the
   engine's `fopen/fread/fseek/access` hit the card. Leave the engine untouched. Drop
   doom1.wad/doom2.wad on a FAT32 µSD root.
3. **argv seeding.** ✔ Done — `myargc=1`/`myargv={"doom"}` in src/PicoDoom.cpp;
   `IdentifyVersion` finds the wad by `access()`. default.cfg load/save works via the
   same FS shim.
4. **Boot test.** ✔ Done, on real hardware — `W_InitMultipleFiles` + the game state
   machine run over serial with `doom.wad` on the card. Video is still null, so it
   prints, not draws.

Done state: **"silent boot"** independent of video — the engine ticks at 35 Hz and
processes screens[] (dropped). ✔ Verified on hardware over USB serial.

## Phase 2 — Video (LCD)

`doom/i_video_null.c` replaced by `src/i_video_ili9486.cpp` (C++, `extern "C"`
definitions -- not just declarations, see AGENTS.md) + `src/Ili9486Display.{hpp,cpp}`
(low-level SPI1/ILI9486 driver, ported verbatim from TOM6809's validated code).
✔ Done, verified on hardware: panel lights up, DOOM renders.

- `I_InitGraphics` — `Ili9486Display::init()` (panel reset + register sequence),
  then `fill_solid(0)` to black out the whole 480x320 panel once.
- `I_SetPalette` — builds a 256-entry RGB565 LUT (wire/big-endian byte order) from
  the incoming PLAYPAL bytes via `gammatable[usegamma]`, matching the reference
  X11 driver's `UploadNewPalette`.
- `I_FinishUpdate` — LUTs `screens[0]` row by row and streams it via
  `set_window`/`write_pixels`/`end_write`. Settled on 1:1 (no scaling), 320x200
  centered in an 80px/60px black border, after a brief 3:2 upscale (480x300,
  filling the panel width) proved too slow on real hardware (see Performance).
- `I_ReadScreen` — `memcpy` of `screens[0]`. `I_UpdateNoBlit` — no-op.
  `I_StartTic` moved to Phase 3 (src/i_input_usbhid.cpp).
- CMAKE_CXX_STANDARD bumped 17 -> 20 for `std::span` (Ili9486Display.hpp).
- A `PicoDoom: SRAM/PSRAM/FPS/game%/SPI%` stats line prints once a second
  from `I_FinishUpdate` (heap usage via `mallinfo()`+linker symbols, PSRAM
  via `psram_used_bytes()`, frame timing via `time_us_64()`) -- the tool that
  diagnosed the performance findings below, worth keeping for Phase 4/5.

Performance, found through real hardware measurement, not assumed:
- `write_pixels()` switched from `spi_write_blocking()` (CPU polls the TX
  FIFO one byte at a time) to a DMA channel feeding the SPI peripheral
  directly. Measured **zero difference** -- the bottleneck was bus
  throughput, not CPU-side overhead. Kept anyway (immune to USB IRQ jitter
  stealing cycles mid-transfer, and simpler than the polling loop).
  `Ili9486Display` claims its DMA channel via a peek-then-release
  (`dma_claim_unused_channel()` immediately `dma_channel_unclaim()`'d) so
  Phase 3's PIO-USB host, which wants a *specific* channel by hardcoded
  index, doesn't collide with it.
- `spi_set_baudrate()` never rounds up past what's requested, and this
  board's `clk_peri` runs at 150MHz (RP2350 default, not RP2040's 125MHz)
  -- the achievable steps near the requested 24MHz are 150/8=18.75MHz and
  150/6=25MHz, nothing between. Requesting 24MHz silently landed on
  18.75MHz. Bumped the request to 25MHz (confirmed via a one-shot
  `spi_get_baudrate()` log in `set_window()`) -- ~4% above TOM6809's
  tested-safe 24MHz ceiling on this exact panel, visually clean so far.
- 3:2 upscale (480x300, 144000px/frame) measured ~9-11 fps. Reverted to 1:1
  (320x200, 64000px/frame, 2.25x less data) for better throughput -- ~9fps
  at that lower pixel count too, suggesting the practical ceiling on this
  SPI link (shift-register-limited, see Ili9486Display's own doc comment)
  is well under 35fps for a full-frame redraw regardless of scale factor.
  Not yet chased further; the real next lever if more speed is needed is
  decoupling display rate from the 35Hz simulation rate (render every 2nd/
  3rd tic) rather than more bus tuning -- a `d_main.c` change, flagged but
  not attempted.

Done state: DOOM is visible and playable-by-menu. ✔ Verified on hardware,
not yet at a full 35 fps (see Performance above).

### Phase 4.5 — Performance: split the blit onto core1 (2026-09, ⏳ not yet hardware-tested)

Objective: 30Hz+ (up from the ~9fps measured above). Moved the LCD blit
(palette->RGB565 convert + DMA-fed SPI push) off core0 entirely, onto
core1 (previously dedicated to Pico-PIO-USB's `tuh_task()` loop, Phase 3) --
see AGENTS.md's Video section for the full mechanism (ping-pong `memcpy` of
`screens[0]` + inter-core FIFO handoff + one-row-per-call stepping so
`tuh_task()` still gets serviced during the transfer). Deliberately did not
ping-pong the engine's `screens[0]` pointer itself, to avoid auditing 1993
engine code for hidden assumptions that its address is stable across a tic.

**Hardware result (first cut, one row + one DMA transfer + one `tuh_task()`
call per call to `i_video_core1_step()`)**: `FPS 10.5  core0-wait 30%
core1-blit 100%` -- no improvement over the single-core baseline. `core1-
blit 100%` means core1 is saturated (never idle between frames), so it *is*
the bottleneck, confirming the SPI feed dominates after all -- but the
measured ~95ms/frame blit time is over 2x the ~41ms theoretical minimum for
128000 bytes at 25MHz. The gap wasn't in the SPI transfer itself; it was in
doing that transfer as 200 separate row-sized DMA operations each followed
by a `tuh_task()` call: `dma_channel_configure()` has real per-call
overhead, and `tuh_task()` (never profiled in isolation, but plausible given
Pico-PIO-USB's software-timed bus servicing) likely costs a few hundred µs
itself -- 200 calls/frame of either is enough to account for the ~54ms gap.

Tried (reverted): batched `i_video_core1_step()` to convert+DMA-transfer
`kRowsPerChunk=8` rows per call instead of 1, on the theory above (200
`dma_channel_configure()`/`tuh_task()` calls per frame both costing real
per-call overhead). **Result: worse, and it hung.** `FPS 9.1-9.6` (down from
10.5), `core0-wait 18-28%`, `core1-blit` climbing toward `100%` across the
two stats lines that printed before the board stopped responding entirely
(~20-25s in, matching two 10s windows) -- consistent with a genuine hang,
not a crash-and-reboot: core0's own `I_FinishUpdate()` would block forever
on `g_blit_buf_free[]` if core1 got stuck, which would exactly explain "only
two stats lines, then nothing" (stats print from `report_stats_if_due()`,
called only from `I_FinishUpdate()`).

Two things this ruled in/out:
- The `#ifdef PICO`-nesting in the same commit's `doom/s_sound.c` sound-
  disable change was checked and is correctly balanced -- not the cause.
- Batching made throughput *worse*, not just "not better enough" -- that's
  the real tell. It contradicts the per-call-overhead theory outright (fewer
  calls should never cost more), and points instead at either (a) real
  PSRAM-bandwidth contention between core0's screens[0]->blit-buffer memcpy
  and core1's own PSRAM reads during conversion, worsened by core1 issuing
  bigger/less-frequent bursts, or (b) Pico-PIO-USB's software-timed bus
  servicing needing a tighter cadence than one call per ~1.6ms chunk (vs.
  ~200us/row before) -- stretching the gap between `tuh_task()` calls from
  "one row's DMA wait" to "one whole chunk's convert+DMA time" plausibly
  wedges it. Neither confirmed; both plausible; not worth re-risking a hang
  to find out by guessing again.

Reverted `kRowsPerChunk` to 1 (the known-stable, if unhelpful, config) and
added real profiling instead of continuing to guess: `I_FinishUpdate()` now
times the memcpy separately from the backpressure wait (`core0-memcpy%`),
and `i_video_core1_step()` times the LUT-conversion loop separately from
`write_pixels()`'s DMA feed (`core1-convert%`/`core1-dma%`) -- see
AGENTS.md's Video section for the new stats line shape. This should finally
show, directly, whether the ~95ms/frame is PSRAM-read-bound (conversion),
SPI-bus-bound (DMA), or something in core0's added memcpy pass -- ⏳ not yet
hardware-tested. Once that's known: if DMA-bound, the lever is the bus
clock (`kPixelBaud` past 25MHz, next clk_peri/N step 30MHz, at real risk of
pixel corruption on this shift-register panel); if convert/memcpy-bound,
the lever is PSRAM traffic (e.g. converting straight from `screens[0]` on
core1 without an intermediate core0 memcpy at all, at the cost of losing
the double-buffer's safety margin -- needs more thought).

**Hardware result (split timing)**: `FPS 8.6-10.4  core0-wait 14-30%
core0-memcpy 22-26%  core1-convert 43-47%  core1-dma 43-51%`. `convert` and
`dma` came out almost exactly 50/50 -- the palette->RGB565 LUT loop
(reading `g_blit_buf`, PSRAM) costs nearly as much as the actual SPI
transfer, contradicting the original "just SPI-bound" assumption. And
`core0-memcpy` (22-26% of core0's *own* frame time) confirmed the
architecture's real added cost: that `screens[0]`->blit-buffer copy didn't
exist in the single-core version at all.

### Phase 4.6 — Memory: get the frame buffer into SRAM (2026-09-11)

Asked: could `g_blit_buf` (and ideally `screens[0]` itself) live in SRAM
instead of PSRAM, closing both gaps above at once (no more PSRAM read
latency in the hot convert loop, no more memcpy)? Required a real memory
audit rather than guessing, since SRAM was already down to ~82KB free
(`SRAM 52/134KB`) and 2 buffers need 125KB.

**Why 3 buffers, not 1**: `screens[1..4]` (`doom/v_video.c`/`st_stuff.c`)
are real, load-bearing DOOM features sharing scratch space -- `screens[1]`:
savegame serialization buffer, the fuzz/partial-invisibility effect's
background read, intermission-screen background restore; `screens[2]`/
`screens[3]`: the level-transition "wipe" (melt) effect's before/after
snapshots (`f_wipe.c`); `screens[4]`: status-bar backing store. None of
these are our video pipeline's 3rd buffer, though -- that was our own
`g_blit_buf[2]` (ping-pong copies of `screens[0]`) plus `screens[0]` itself.

**Full `.bss`/`.data` audit** (every symbol >=400 bytes, 74 total) found the
big SRAM consumers are almost entirely off-limits: `visplanes` (82.5KB),
`openings` (40KB), `drawsegs`/`zlight`/`scalelight`/`ylookup`/`columnofs`
(~30KB combined) are all rebuilt and read every single frame during the
BSP walk -- moving them to PSRAM would slow the *renderer* more than it
helps the blit. `g_core1_stack` (16KB, ours) is a live CPU stack, same
problem. Genuinely cold/safe candidates (our own `sd_stdio.c`'s `s_files`
table, `R_InitSpriteDefs`'s startup-only `sprtemp` scratch, `M_LoadDefaults`
config buffers, etc.) totaled only ~15.6KB -- nowhere near the ~43KB
minimum needed.

**MinSizeRel build type**: measured, not assumed. `-Os` shaved ~58KB off
`.text` (flash) but only **~1.8KB** off `.data`+`.bss` combined (SRAM) --
declared array sizes (`visplanes[128]`, `openings[SCREENWIDTH*64]`, ...)
don't change with optimization level. No help for this problem.

**The actual find**, from reading github.com/kilograham/rp2040-doom's
memory-optimization writeup (a *much* more constrained port -- RP2040 has
zero PSRAM, they fit the entire engine in ~264KB SRAM; most of their
toolkit -- 16-bit pointer compression, boolean bitsets, a custom `mobj_t`
split -- solves a problem we don't have) and then verifying against our own
source rather than copying blind: `finesine`/`finetangent`/`tantoangle`
(`doom/tables.c`, ~64KB combined) looked mutable (grep found what looked
like runtime writes in `r_main.c`) but turned out not to be --
`R_InitPointToAngle()`/`R_InitTables()`, the *only* code that ever assigns
to them, are both **entirely `#if 0`'d out**, with id Software's own
comment: `// UNUSED - now getting from tables.c`. The literal initializers
in `tables.c` are the only values ever used, on every platform, always have
been. `const`-qualifying them under `#ifdef PICO` (`doom/tables.h`,
`doom/tables.c`, `doom/r_main.c`'s `finecosine` pointer) moves them from
`.data` (copied into SRAM at boot) to `.rodata` (flash, XIP-mapped) --
confirmed via the linker map: **exactly 65536 bytes (64KB)** off `.data`,
zero change to `.bss`, `finesine`/`finetangent`/`tantoangle` all now at
flash addresses (`0x1000_xxxx`) with `.rodata` type.

With that headroom, did the buffer-count reduction too: `I_FinishUpdate()`
(`src/i_video_ili9486.cpp`) now ping-pongs `screens[0]` itself between two
plain SRAM arrays (`g_screen_buf[2]`, `.bss`, 128000 bytes) instead of
copying it into separate PSRAM blit buffers -- removing both the per-frame
memcpy and one buffer's worth of memory outright. This is exactly the
pointer-swap the Phase 4.5 header comment originally ruled out over a real,
now-understood risk: `r_draw.c`'s `R_InitBuffer()` caches per-row *pointers
into* `screens[0]` (`ylookup[i] = screens[0] + ...`), but only refreshes
them on view-size changes (the menu's screen-size +/- keys), not every
frame -- swap `screens[0]` without also refreshing that cache and the
renderer keeps drawing into a stale/wrong buffer. Fixed by calling
`R_InitBuffer(scaledviewwidth, viewheight)` again after every swap (cheap:
`SCREENHEIGHT` pointer computations, not a per-pixel cost). First-call
special case: `V_Init()` runs *after* `I_InitGraphics()` (`d_main.c`), so
`screens[0]` still points at `V_Init()`'s original PSRAM allocation for the
very first tic -- that frame's content is discarded in favor of
`g_screen_buf[0]`'s zero-initialized (black) contents once the swap
happens, indistinguishable from `I_InitGraphics()`'s own `fill_solid(0)`.
The original PSRAM `screens[0]` slot (part of `V_Init()`'s contiguous
`screens[0..3]` block) is simply never touched again -- 64000 bytes of
PSRAM sit unused rather than being reclaimed, not worth the `v_video.c`
change to avoid given PSRAM isn't remotely scarce.

**Result, first cut**: linker map showed SRAM budget (`__StackLimit -
__bss_end__`) dropping from ~134KB to ~73.9KB free-for-heap -- expected
(freed 64KB via const, spent 128KB on `g_screen_buf`, net -64KB). Estimated
~22KB headroom from the ~52KB *steady-state* (in-gameplay) usage reported
by an earlier stats line, and judged that positive-but-tighter margin
acceptable.

**That estimate was wrong on hardware**: flashed and got `*** PANIC ***
Out of memory` during `W_Init()` -- `adding doom.wad` -- before the game
even reached the title screen. This is the Pico SDK's `pico_malloc`
wrapper (`src/rp2_common/pico_malloc/malloc.c`'s `check_alloc()`), which
panics if the heap grows past `__StackLimit`. The ~52KB figure was
*post*-load steady state; loading this WAD's directory table (a full
commercial IWAD, ~2900+ lumps) apparently needs more than the ~73.9KB
budget at its *peak*, during the load itself -- a distinction the earlier
estimate didn't account for.

**Phase 4.6.1 fix**: only `g_screen_buf[0]` is SRAM (plain static array);
`g_screen_buf[1]` is back to `psram_malloc`, same pattern as the original
pre-Phase-4.6 blit buffers (`src/i_video_ili9486.cpp`). Gives back ~62.5KB
of the ~64KB the const-ified trig tables freed. New budget (confirmed via
linker map): **136.4KB** free-for-heap -- slightly *more* than the original
pre-Phase-4.6 baseline that already proved sufficient to load this exact
WAD, so this should be solidly past the panic threshold, not just barely
past it. Trade-off: only half of core1's convert-loop benefit remains
(alternates SRAM/PSRAM reads frame to frame, screens[0] ping-pongs between
the two slots) instead of the full SRAM win Phase 4.6 was aiming for.

**Hardware result**: panic fixed, boots and plays. FPS ~13-15 (up from
~10.5 pre-Phase-4.6), `core1-convert` dropped to 23-29% (from ~45%
fully-PSRAM) confirming the partial SRAM win landed, `core1-dma` rose to
64-74% (now the dominant cost, as expected once convert got cheaper).

**But**: the status bar (bottom of screen) and level-transition/demo
screens came back corrupted and blinking. Root cause, found by reading
`doom/st_stuff.c` and `doom/f_wipe.c`: `screens[0]` ping-ponging between
two different physical buffers every tic is incompatible with parts of the
engine that assume the framebuffer's *content* (not just its address)
persists across multiple tics, not just within one:
- `ST_Drawer()`'s incremental "diff" mode (`ST_diffDraw()` ->
  `STlib_update*(..., refresh=false)`) skips repainting a status-bar widget
  whose value hasn't changed since last tic, relying on last tic's pixels
  still being visible. If `screens[0]` pointed at the *other* buffer last
  tic, those pixels were never drawn into *this* buffer at all.
- `f_wipe.c`'s level-transition melt effect is worse: `wipe_ScreenWipe()`
  caches `wipe_scr = screens[0]` **once** at the start of a multi-tic (~30
  tic) animation and keeps writing into that same captured pointer for the
  whole animation, never re-reading `screens[0]`. Every tic after the
  first writes into an increasingly stale, eventually-orphaned buffer.

Unlike `R_InitBuffer()`'s `ylookup[]`/`columnofs[]` cache (a pointer cache,
fixable by re-running the same init function after every swap), these are
*content*-persistence assumptions -- there's no equivalent "just refresh
it" fix; a ping-ponged buffer fundamentally cannot provide "what I drew N
tics ago is still visible now" the way a single stable buffer does.

**Phase 4.6.2 (revert)**: `screens[0]` is never reassigned again.
`I_FinishUpdate()` goes back to the Phase 4.5 shape -- `memcpy` `screens[0]`
into whichever of `g_screen_buf[0]`/`[1]` is free, hand the index to core1,
same backpressure/FIFO handoff as before. The buffers themselves are
unchanged (slot 0 SRAM, slot 1 PSRAM, from Phase 4.6.1) so core1's convert
loop keeps the same partial SRAM benefit; what's lost is only the
render-time benefit of `screens[0]` itself sometimes being SRAM (never
directly measured, folded into the ~13-15fps number above) and the memcpy
cost comes back (`core0-memcpy%`, reinstated in the stats line). ⏳ Not yet
hardware-tested -- expect FPS to land somewhere between the ~10.5fps
fully-PSRAM baseline and the ~15fps ping-pong number, and the status
bar/transitions to render correctly again.

**"Use" key (Space) not opening doors, investigated in parallel**: added
temporary `#ifdef PICO` diagnostics at `G_BuildTiccmd` (`doom/g_game.c`,
confirms `BT_USE` gets set), `P_UseLines` (`doom/p_map.c`, confirms
gameplay logic is reached), and `PTR_UseTraverse` (confirms what
`P_PathTraverse` finds in front of the player). Hardware log showed the
full chain working correctly: `BT_USE` set every tic held, `P_UseLines`
called exactly once per press (correct edge-triggered behavior via
`player->usedown`, not once per tic), `PTR_UseTraverse` reached and
reporting `special=0` -- i.e. it found a line in front of the player, but
that line has no special (a plain wall, not a door/switch). **Not a bug**:
the whole input->gameplay pipeline is confirmed correct end to end; the
test just wasn't aimed at an actual use-triggered linedef at that moment.
Diagnostics removed.

### Phase 4.7 — CPU: renderer inner-loop tuning (2026-09-11, ⏳ not yet hardware-tested)

Looked at RP2350-specific renderer optimizations while waiting for hardware
access. Two ideas considered, verified by actually compiling and
disassembling (`arm-none-eabi-objdump -d`) rather than assumed:

- **Hardware interpolators (`SIO_INTERP0`/`INTERP1`)** for `R_DrawColumn`'s
  DDA texture-mapping loop (`frac += fracstep; idx = (frac>>FRACBITS)&127`)
  -- the standout idea from reading kilograham/rp2040-doom's writeup, and
  the textbook use case for this peripheral. **Not pursued**: disassembly
  showed GCC `-O3` already compiles `(frac>>FRACBITS)&127` to a single
  `UBFX` (bitfield-extract) instruction -- there's no instruction count to
  save, and trading an already-optimal ALU op for a peripheral round-trip
  is as likely to be a wash or a slight loss as a win, with no hardware
  available yet to actually measure it either way. `R_DrawSpan`'s Y-index
  (`(yfrac>>10)&(63*64)`, pre-shifted so it can OR with the X-index without
  a multiply) does *not* compile to a single instruction (needs a separate
  `AND` after the shift, since the result isn't right-aligned) -- a
  narrower, still-unconfirmed candidate, not implemented.
- **Loop-invariant reloads** (implemented): disassembly of `R_DrawColumn`/
  `R_DrawColumnLow`/`R_DrawTranslatedColumn`/`R_DrawSpan`/`R_DrawSpanLow`
  (`doom/r_draw.c`) showed `dc_source`/`dc_colormap`/`dc_translation`/
  `ds_source`/`ds_colormap`/`ds_xstep`/`ds_ystep` reloaded from memory on
  *every* pixel despite never changing across any of these loops -- GCC
  can't rule out that the `*dest` store aliases the global pointer
  variables themselves without whole-program visibility (no LTO in this
  build). Fixed by caching each into a local before the loop, `#ifdef
  PICO` (behaviorally a no-op on any platform, gated for diff-auditability
  consistency with the rest of this file, same as every other doom/
  change this session) -- not a novel idea, id Software's own `#if 0`'d
  "UNUSED, loop unrolled" reference versions of `R_DrawColumn`/`R_DrawSpan`
  already do exactly this caching, just never enabled. Confirmed via
  disassembly: zero reloads left inside any of the 5 loop bodies
  (previously 2-4 per iteration depending on the function);
  `R_DrawSpanLow` (2 pixels/source-lookup, blocky mode) had the worst case
  since every reload was paid twice per source pixel. `R_DrawColumn`'s
  loop itself went from 10 to 9 instructions/pixel; the bigger win is in
  the multi-pointer functions (`R_DrawSpan`: 4 reloads removed/iteration).

## Phase 3 — Input

USB-PIO HID keyboard host (GPIO28/29), `src/PicoUsbKeyboard.{hpp,cpp}` +
`src/i_input_usbhid.cpp`. Config/integration ported from TOM6809's validated
`PicoUsbHidInput`/`WAVESHARE_USB_HID_SUPPORT` (same board) rather than
re-derived -- every compile-definition fix in CMakeLists.txt's Phase-3
block was found the hard way there; see that file's own comments.
Implemented, ⏳ not yet verified on hardware.

- `PicoUsbKeyboard::init()` launches the TinyUSB host stack
  (`tuh_configure`/`tuh_init`, rhport 1) on **core1** (its own dedicated
  core, 16KB stack carved from ordinary SRAM via
  `multicore_launch_core1_with_stack()` -- the default core1 stack lives in
  a ~2KB SCRATCH_X bank, too small). Called from `main()` (PicoDoom.cpp),
  before `D_DoomMain()`, so a keyboard has the whole WAD-load/engine-init
  stretch to enumerate.
- `tuh_hid_report_received_cb` (core1) parses boot-protocol
  `hid_keyboard_report_t` reports into a double-buffered 256-entry held-key
  snapshot (indexed by HID usage ID), flipped atomically so `I_StartTic()`
  (core0) never reads a partially-rebuilt report -- same cross-core
  convention TOM6809's `PicoUsbHidInput` uses (adopted there after a real
  hardware glitch from a shared single buffer).
- `I_StartTic()` (`i_input_usbhid.cpp`) diffs that snapshot against the
  previous poll and posts `D_PostEvent` keydown/keyup for whatever changed
  -- arrow keys, Ctrl/Shift/Alt (from the report's modifier byte, not the
  keycode array), Enter/Escape/Tab/Space/Backspace, F1-F12, Pause, and
  lowercase letters/digits for menu/cheat-code entry.
- Two independent USB controllers coexist in one TinyUSB build: rhport 0
  (native peripheral, device mode) stays stdio_usb's serial console;
  rhport 1 (Pico-PIO-USB, host mode) is the new keyboard. Getting there
  needs a project-owned `src/tusb_config.h` (the SDK's own
  `pico_stdio_usb`-supplied one goes *blank* the moment `tinyusb_host` is
  linked) plus several `PICO_STDIO_USB_*`/`PICO_*_USB_RESET_*` compile
  defines that default to *off* under the same condition and silently kill
  either device-mode enumeration or picotool's BOOTSEL-reboot interface if
  missed -- see CMakeLists.txt's own comments for which does what.
- Linking `tinyusb_host` also had a side effect worth knowing about:
  `family_support.cmake` sets `-Wall -Wextra` as `PUBLIC` compile options
  on that target, which propagates onto this whole executable, including
  untouched 1993 `doom/*.c` that predates `-Wall` by decades. Suppressed
  with `-w` on every `DOOMSRC` file (CMakeLists.txt) rather than restyling
  upstream code -- keeps "zero warnings" meaning what it always meant here:
  clean on the code this project actually maintains.

Keyboard ✔ verified on hardware (including the movement fix above).

USB HID mouse (`src/PicoUsbMouse.{hpp,cpp}`) added as a follow-up: shares
PicoUsbKeyboard's TinyUSB host stack/core1 (TinyUSB's C callback ABI allows
only one `tuh_hid_*_cb` definition per program, so `PicoUsbKeyboard.cpp`
owns them and dispatches to `PicoUsbMouse` for `HID_ITF_PROTOCOL_MOUSE`
reports too). Parses the fixed 3-byte boot-protocol report (buttons + signed
dx + signed dy, no report-ID prefix -- framing knowledge ported from
TOM6809's `PicoUsbHidInput::on_mouse_report()`, whose doc comment documents
a real-hardware bug from guessing that framing instead). Unlike TOM6809's
own `MouseState` (an accumulated *absolute* cursor position, for positioning
an LVGL cursor), DOOM's `ev_mouse` wants *relative* deltas since the last
poll -- `take_delta()` accumulates raw HID units and resets on read, one
poll per `I_StartTic()` tic. Y is inverted from the raw HID sign to match
`doom/i_video.c`'s original X11 `MotionNotify` convention (push the mouse
away from you -> move forward). ⏳ Not yet hardware-tested.

Gamepad input and the XPT2046 touch panel are not implemented -- keyboard
(+ now mouse) already satisfies "playable" below.

Done state: playable with a USB keyboard. ✔ Verified on hardware.

## Phase 4 — Sound (defer)

Port `s_sound.c` output to a single mixed 11 kHz SFX stream (DOOM mixes ~8 voices,
2 at once typically). Timer IRQ -> DMA to PWM or a PIO DAC. Ignore music (no MIDI on
the Pico) or replace with a simple noise/beat. Lowest priority.

### USB Audio Class output -- investigated, ruled out (2026-09)

Tried routing sound to a USB sound card (C-Media UAC1 adapter, VID 0x0d8c PID
0x000c) over the same Pico-PIO-USB host port used for keyboard/mouse (Phase
3). Confirmed infeasible without a from-scratch driver, by direct source
inspection rather than speculation:

- TinyUSB ships `class/audio/audio_device.c` only -- no host-side USB Audio
  Class driver (`tuh_audio_*`) exists anywhere in the SDK tree.
- Pico-PIO-USB's own host transfer engine (`pio_usb_host.c`) is built
  entirely around the control/bulk/interrupt handshake: every transaction
  path waits for `USB_PID_ACK`/`USB_PID_NAK` and retries on NAK
  (`TRANSACTION_MAX_RETRY`). Isochronous transfers have no handshake phase
  at all -- the string "isochronous" appears exactly once in the whole
  vendored library, as an unused enum value (`EP_ATTR_ISOCHRONOUS` in
  `usb_definitions.h`), never referenced by any transfer/scheduling code.
  Sending an isochronous OUT transfer through this engine as-is would just
  hang waiting for a handshake the device never sends.
- Making this work would mean writing a new no-handshake isochronous
  transfer path at the PIO/`pio_usb_ll` level, plus a UAC1 host driver on
  top -- both from scratch, with no reference implementation in this
  codebase or TOM6809 to port from (unlike every other driver so far).

Decision: paused. Revisit the original plan above (PWM or PIO DAC driven
straight off a GPIO, no USB) instead of USB Audio Class.

## Phase 5 — Polish (2026-09-11)

- **`I_Quit` -> reboot instead of exit.** ✔ Done. `doom/i_system.c`: newlib's
  default `exit()`/`_exit()` on this bare-metal build just hangs the CPU
  forever (confirmed in the Pico SDK's `crt0.S` -- "calls exit (which should
  eventually hang the processor via `_exit`)"), leaving the board frozen
  until a manual power cycle. `I_Quit()` now calls `watchdog_reboot(0, 0,
  100)` under `#ifdef PICO` instead -- a full chip reboot back through the
  normal boot sequence, same as a power cycle, after a brief delay. Forward-
  declared `watchdog_reboot()` rather than `#include`ing `hardware/
  watchdog.h` -- this file compiles as gnu90 (see CMakeLists.txt), and Pico
  SDK headers generally need C11 (same reason `d_main.c` has its own
  carve-out). `I_Error()` deliberately left unchanged (still prints +
  hangs) -- this whole project's debugging has depended on reading its
  message off the serial console; an immediate reboot on a real crash would
  erase that. Added `hardware_watchdog` to `target_link_libraries`.
  ⏳ Not yet hardware-tested.
- **Pause.** ✔ Already done, no change needed, ✔ verified on hardware:
  `KEY_PAUSE` (HID usage 0x48, a keyboard's dedicated Pause key) is already
  mapped in `src/i_input_usbhid.cpp`'s HID->DOOM keycode table, wired
  straight into vanilla DOOM's own pause handling (`sendpause`/`paused` in
  `doom/g_game.c`). This board has no onboard physical buttons to wire up
  as an alternative (checked `boards/waveshare_rp2350_pizero.h`).
- **Save games to µSD.** ✔ Verified on hardware, both directions
  (save-then-load). Reviewed, not changed: `p_saveg.c`/`g_game.c`'s
  `G_DoSaveGame`/`G_DoLoadGame` and `m_menu.c`'s `M_ReadSaveStrings` all go
  through plain `open`/`read`/`write`/`close` (`doom/m_misc.c`'s
  `M_WriteFile`/`M_ReadFile` do too) -- the exact same `_open`/`_read`/
  `_write`/`_close` syscalls `src/sd_stdio.c` already implements and that
  WAD loading has exercised since Phase 1. `_open()` already handles
  `O_CREAT|O_TRUNC` (maps to FatFs's `FA_CREATE_ALWAYS`) for writing new
  saves, and `M_ReadSaveStrings` already handles a missing/nonexistent save
  slot gracefully (`open()` returning -1 -> empty slot, not a crash). None
  of this session's earlier type-size fixes (`ticcmd_t`, `event_t`,
  `maptexture_t`, etc.) touch anything `p_saveg.c` serializes, and savegames
  are self-consistent regardless of struct layout since the same build
  always reads what it wrote.
- **On-screen FPS / doomstat console.** Not started -- deferred. Needs real
  design work, not a quick add: DOOM has no built-in on-screen counter (a
  modern-source-port feature, not in 1993 vanilla), so this means either a
  custom bitmap-font stamped onto the RGB565 buffer during core1's convert
  pass (own code, `i_video_core1_step()`, no `doom/` changes -- can render
  into the unused 80/60px black border around the 320x200 view, avoiding
  any interference with actual gameplay) or hooking into `st_stuff.c`'s
  status-bar widget system (touches `doom/`, more invasive). Leaning toward
  the former; not implemented yet.
- **README.md.** ✔ Done -- was entirely the old PiCoMonitor/SSD1306 project's
  README. Rewritten to describe PicoDoom: hardware, build/flash
  instructions, controls, current status, pointers to docs/PLAN.md and
  AGENTS.md for detail.

## Phase 6 — Boot WAD-selection menu (LVGL v9.2.2, 2026-09, ⏳ not yet hardware-tested)

A boot-time menu (shown once, from `src/PicoDoom.cpp` between USB-host init and
`D_DoomMain()`), so a card full of WADs isn't limited to whichever one matches the
engine's fixed-name scan (`doom1.wad`/`doom.wad`/`doomu.wad`/`plutonia.wad`/`tnt.wad`/
`doom2.wad`/`doom2f.wad`). It lists every IWAD on the uSD root, the user picks one
(touch / mouse / keyboard / gamepad), the last choice is remembered in `default.cfg`
and pre-highlighted next boot, and a visible 30-second countdown auto-starts the
persisted pick unless any input arrives first. Esc / the Skip button / gamepad B all
cancel -> the engine falls through to its fixed-name scan exactly as before.

- **LVGL, not TOM6809's widget code, but TOM6809's wiring.** Same repo/tag/build
  recipe as TOM6809's `WAVESHARE_LVGL_UI_SUPPORT` block (`lvgl` v9.2.2 via
  `FetchContent`, `LV_CONF_PATH` forced to `src/lv_conf.h`, examples/demos disabled).
  `src/lv_conf.h` is a copy of TOM6809's with the fonts reduced to Montserrat 14/22/28
  and dark theme forced on. LVGL's heap is PSRAM-backed via `LV_MEM_POOL_ALLOC`
  (`src/lvgl_mem_pool.cpp`, `lvgl_mem_pool()` = `psram_malloc` else `malloc`, mirroring
  TOM6809's `PsramLvglPool.cpp`) -- safe because the menu is core0-only, unlike the
  core1-touched PSRAM buffers the game keeps in SRAM. Verified against the fetched
  v9.2.2 headers before writing (a `lv_event_get_key()` that doesn't exist, etc. --
  ESC's key value comes from `lv_indev_get_key(lv_indev_active())`).
- **Displays.** LCD build: `src/lvgl_display_lcd.cpp` drives this project's own
  `pico_toolset::Ili9486` (`i_video_lcd_display()`) at full 480x320, flush byte-swaps
  RGB565 to big-endian wire order like the game blit does, 40-row PSRAM tile buffer
  (38,400 B). HDMI build: `src/lvgl_display_dvi.cpp` renders a 320x240 canvas that IS
  `g_framebuf` 1:1 (native-endian row memcpy, no fold, no swap -- deliberately NOT
  TOM6809's 640x480 fold driver, because `g_framebuf` is only 320 wide and the encoder
  doubles axes), 40-row PSRAM buffer (25,600 B). Both call `lv_display_flush_ready`.
  `I_InitGraphics()` got a static once-guard in both video drivers (`src/i_video_ili9486.cpp`,
  `src/i_video_dvi.cpp`) because the menu calls it before `D_DoomMain()` does -- the
  second call must be a no-op (a DVI re-`dvi_init()` mid-boot wedges the TMDS lane).
- **Input.** `src/lvgl_indev.cpp`: pointer+mouse (scales the host's canonical 640x480
  cursor to the canvas; visible "+" Montserrat-28 crosshair -- no Font-Awesome icon
  font here), pointer+touch (LCD build only; `Xpt2046Calibration` defaults measured on
  this exact panel, `swap_axes=true`, mapped straight into panel pixels), keypad
  (keyboard HID usages -> arrows/Enter/Esc/Tab, gamepad D-pad/stick/A/B/Start
  fallback). All three set `g_input_seen` on any real input, which stops the menu's
  auto-start countdown (the menu then clears it). Gamepad navigation rides vanilla
  LVGL `lv_group` -- no per-button input glue.
- **Engine handoff.** `d_main.c` `IdentifyVersion()` gets a `#ifdef PICO` block, before
  the fixed-name scan: if the menu picked a WAD and it's `access()`ible,
  `gamemode = pico_wad_gamemode(sel); D_AddFile(sel); return;`. The gamemode is
  sniffed by `src/wad_boot.cpp` reading the WAD's own lump directory in bounded
  256-entry batches (Freedoom2 has ~19k lumps -- no whole-dir malloc at boot):
  MAP01 -> commercial; else E4M1 -> retail; E3M1 -> registered; E1M1 -> shareware;
  nothing -> commercial. IWADs on the menu are magic-checked (`IWAD` header). NOT
  `-file` -- shareware builds `I_Error()` on `-file` (d_main.c).
- **`default.cfg` persistence.** `m_misc.c` gets a `#ifdef PICO`
  `{"picodoom_lastwad", (int*)&pico_last_wad, (int)""}` defaults[] entry plus the
  extern (`char* pico_last_wad`, defined in `src/wad_boot.cpp`) -- so on clean quit
  `M_SaveDefaults()` writes it (the default value is a valid pointer, required because
  `M_SaveDefaults` dereferences unconditionally). The menu writes/rewrites
  `default.cfg` itself at select time (replace-or-append the key) so a power-cut/boot
  still pre-highlights the remembered WAD even without a clean `I_Quit`. Layout matches
  what `M_LoadDefaults()` parses (`key\t\t"value"`).
- **Exit hygiene.** `src/i_input_usbhid.cpp` gains `i_input_reset_menu_input()`,
  called on every menu exit path: drains accumulated mouse delta and re-baselines
  every edge latch (`g_prev_*` keyboard/modifiers/mouse/gamepad) to current HID state,
  so the Enter still held to confirm the menu (or a ghost from enumeration) doesn't
  fire `ev_keydown` into the first gameplay tics as "use". Servicing the USB stack
  during the menu: LCD build no-op (core1 already polls `UsbHidHost::task()` in its
  loop); HDMI build `i_input_menu_usb_task()` polls it from the menu's pump.
- **Build/test status:** both `build/` (LCD) and a new `build-hdmi/`
  (`-DPICODOOM_VIDEO_OUTPUT=hdmi`) configure and link cleanly
  (SDK 2.3.1, LVGL fetched into each build's `_deps/`). ⏳ **Not yet hardware-tested**
  -- first flash should exercise: menu appears over the boot black screen, buttons
  list every IWAD on the card, keyboard/d-pad navigate + Enter launches, touch taps,
  auto-start after 30s, `default.cfg` is written with the pick and pre-highlighted on
  the next boot, Skip/Esc falls through to the fixed-name scan.

Reproducible silent freeze, first seen during demo autoplay, MAP05
(`demo1`, skill 3), consistently mid-gameplay — user's own testing suggests
"close after a barrel exploding and while opening a secret wall," and
possibly reachable in manual play too, not demo-specific. No crash message,
no reboot, just a dead board. Long investigation, several rounds of
hardware-in-the-loop hypothesis testing, most of them negative results:

- Added step-bound guards (`#ifdef PICO`, `I_Error` if exceeded) to every
  loop that could plausibly spin forever on corrupted data: `P_RunThinkers`
  (`doom/p_tick.c`), `P_BlockThingsIterator` (`doom/p_maputl.c`),
  `P_SetMobjState`'s zero-tic state chain (`doom/p_mobj.c`). Also added
  bounds checks to two genuinely unchecked buffer overflows found along the
  way (`openings[]` in `doom/r_plane.h`/`r_segs.c`, `intercepts[]` in
  `doom/p_maputl.c`, the latter's `MAXINTERCEPTS` also raised 128->1024 in
  `doom/p_local.h`). **None of these ever fired** on the actual freeze.
- Fixed one genuine bug found along the way: a use-after-free in
  `P_RunThinkers` (`Z_Free(currentthinker)` then reading
  `currentthinker->next` through the freed pointer) — real, but confirmed
  (via freeze-point shift) not the cause of this specific freeze either.
- Added `src/FaultHandler.cpp`, a real `isr_hardfault` (the SDK's default is
  a silent `bkpt #0` with no debugger attached — indistinguishable from an
  infinite loop, which is exactly what this investigation had been
  chasing). First version tried to force a printf out over USB CDC from
  fault context by calling `tud_task()` in a loop. **Also produced no
  output** on a reproducible freeze test.
- Added 3-second timeouts to the two unbounded cross-core waits in the
  Phase 4.5/4.6 video pipeline (`I_FinishUpdate`'s wait on
  `g_blit_buf_free[]` in `src/i_video_ili9486.cpp`; the DMA-completion wait
  in `Ili9486Display::write_pixels`, `src/Ili9486Display.cpp`) on the theory
  that core1 stalling (stuck in `tuh_task()`, a wedged DMA/SPI transfer)
  would hang core0 completely silently, matching the symptom exactly and
  explaining why nothing DOOM-engine-side was catching it. **Also silent**
  — freeze reproduced again with neither timeout firing.

Six clean loop-bound results plus a hard-fault handler plus two cross-core
timeouts, all silent, was strong negative evidence — but not because
nothing was happening. **The debug probe told the real story** (see
`debug_notes.md`, three independent GDB captures, all landing on the exact
same call stack): a genuine hard fault, every time, in `V_DrawPatch`
(`doom/v_video.c:223`) called with a corrupted `patch` argument
(`0xffffffff`) and corrupted `x` (`24191`), from `STlib_updateMultIcon`
(`doom/st_lib.c:237`) for `w_arms[0]` (the status-bar weapon-ownership
icon), from `ST_drawWidgets` (`doom/st_stuff.c:1078`), from `D_Display`.
`w_arms` is a plain `static st_multicon_t w_arms[6]` in `st_stuff.c` — not
zone-heap memory, so this isn't a purge/reuse-after-`Z_ChangeTag` issue, it
reads as a stray write from somewhere else landing on fixed .bss memory
(same class of bug as the `openings[]`/`intercepts[]` overflows already
fixed, just with a static-memory target this time instead of a stack
buffer) — consistent with the freeze point drifting slightly build to
build (829→832→831→830→831→831→832): a fixed-size overflow whose visible
consequence depends on exactly what's adjacent in memory, which shifts as
the .bss/.data layout shifts with any code change. **Not yet located.**

**And the hard-fault handler wasn't actually failing to catch anything —
its own recovery path was deadlocking.** `hard_fault_handler_c`'s original
`tud_task()`-flush loop, called from fault context, re-enters TinyUSB's
device-stack state machine (queues, spinlock-guarded critical sections
shared with core1's `tuh_task()` host stack) — state that can legitimately
be mid-update at the exact instant a fault interrupts normal execution.
Calling back into it re-entrantly from an asynchronous exception context
blocked forever on that same lock (see `debug_notes.md`'s
`critical_section_enter_blocking`/`spin_lock_blocking` frames on both
cores). So the mechanism built specifically to make faults visible was
itself hanging silently — indistinguishable from "no fault happened" — and
actively misled this investigation for a full round of hardware testing.

**Fixed** (`src/FaultHandler.cpp`, rewritten): the handler no longer
touches USB (or anything else with cross-core/interrupt state) from fault
context at all. It stashes PC/LR/CFSR in the watchdog's scratch registers
(`watchdog_hw->scratch[0..3]` — `[4]` is reserved by the SDK's own
`watchdog_enable()`/`watchdog_reboot()` bookkeeping; survive a watchdog
reset, unlike ordinary SRAM) and reboots via `watchdog_reboot()`, the same
mechanism `I_Quit()` already uses successfully. The next boot, in normal
context with stdio/USB fully initialized (no reentrancy risk), checks those
scratch registers and prints the stashed fault before starting the game —
`report_pending_hard_fault()`, called from `src/PicoDoom.cpp`'s `main()`
right after `stdio_init_all()`. This should surface the exact same crash
debug_notes.md found, automatically, over plain USB serial, no debug probe
needed.

Also added a step-bound guard to `Z_CheckHeap()` itself
(`doom/z_zone.c`) — its block-list walk's only exit condition is reaching
the sentinel (`block->next == &mainzone->blocklist`); it's been called
every tic this whole investigation as a corruption *detector* but was
never itself suspected as a hang candidate, and is the one loop that was
never given a bound.

**Next step**: reflash, reproduce, read whatever the next boot prints
(either the stashed hard-fault PC/LR/CFSR — feed PC to
`arm-none-eabi-addr2line -e build/PicoDoom.elf` to confirm/locate the
`V_DrawPatch` call site precisely — or the `Z_CheckHeap` cycle-detected
message, if that's what actually fires instead). From there, find what
writes past its own buffer and lands on `w_arms`' address — candidates tied
to the "barrel exploding + secret door" trigger not yet ruled out: sound
channel management in `s_sound.c` (`S_StartSound`'s channel-search/steal
logic runs even though the low-level `i_sound_null.c` backend is a no-op —
many simultaneous sound requests from a chain-reacting explosion could
stress it in a way nothing before has), or something explosion/radius-
damage-specific in `p_mobj.c`/`p_enemy.c`. Ruled out: a "secret revealed"
HUD message buffer overflow — checked, this vanilla 1.10 codebase has no
such message at all (`p_spec.c`'s `P_SecretFound` only increments
`secretcount`, doesn't touch `player->message`; that's a Boom/later-port
feature).

**Correction from the user** (owns the debug probe, actually ran the GDB
session `debug_notes.md` came from): compiled Debug, the freeze does not
reproduce at all. Compiled Release or RelWithDebInfo, it does, and from the
outside it is genuinely just a visual freeze, no crash — the `debug_notes.md`
captures were taken by manually breaking in with Ctrl+C once the game was
already frozen (not an automatic break on a fault) and reading both cores'
call stacks, repeatedly, getting the same picture every time. That the CPU
lands in the *identical* spot on every independent interrupt (rather than
different points, as sampling a genuinely still-running program would show)
is itself good evidence both cores really are stuck, not just slow — this
part of the read doesn't depend on trusting the deeper, harder-to-verify
frames. `hard_fault_handler_c`/`V_DrawPatch` appearing above that in the
unwound stack is not necessarily wrong (a real HardFault entering that
handler, whose old `tud_task()`-flush loop then deadlocks exactly at
`osal_queue_receive`/`spin_lock_blocking` — frames #0-3, matching perfectly
— would *look like* "just a freeze, no crash message" from outside, since
that old handler's whole flush path never got anywhere near printing
anything). But it doesn't need to be trusted either: this doesn't change
what to do next, since the already-shipped fault-handler rewrite settles it
either way (a report on the next boot confirms a real fault + pinpoints it;
continued silence confirms a genuine hang with no exception ever taken, and
redirects investigation onto the cross-core TinyUSB locking itself).

**New, load-bearing clue**: Debug vs. Release is the one axis that actually
changes the outcome, and this project's `CMakeLists.txt` overrides no
per-build-type compiler flags at all — so the entire difference is CMake's
own stock `-O0` (Debug) vs. `-O2`/`-O3 -DNDEBUG` (Release/RelWithDebInfo).
That an optimizer-only difference flips a memory-corruption bug on and off
is the textbook signature of a **strict-aliasing violation**: GCC's
`-fstrict-aliasing` only takes effect (only actually acts on the
type-based-alias assumptions it licenses, e.g. reordering/eliding loads
across pointers of different types it assumes cannot overlap) from `-O2`
up — at `-O0`/`-Og` the same violating code simply happens to still produce
correct results. This 1993 codebase reinterprets raw WAD lump bytes as
`patch_t`/other structs via pointer casts constantly — precisely
`V_DrawPatch`'s whole reason for being — which is exactly the pattern that
violates strict aliasing, and exactly why most surviving DOOM source ports
(chocolate-doom included) build with `-fno-strict-aliasing`. **Fixed**:
added `-fno-strict-aliasing` to every `doom/` engine source's compile
options in `CMakeLists.txt`, alongside the existing `-std=gnu90`/`-w`. Not
yet hardware-confirmed — needs a round-trip like everything else this
round — but well-motivated enough to ship ahead of that confirmation, and
it composes fine with the fault-handler fix regardless of whether it turns
out to be the whole story.

### Root cause found (2026-09-12)

The fault-handler rewrite worked exactly as designed: the very next boot
printed `*** PREVIOUS BOOT ENDED IN A HARD FAULT ***` with PC/LR/CFSR,
automatically, over plain USB serial, no debug probe needed — and a live
GDB session (debug probe still attached, breakpoint set directly in
`hard_fault_handler_c` this time rather than a post-hoc Ctrl+C) independently
confirmed the identical fault, so this was always a genuine HardFault, not
a plain hang (see the `hard_fault_handler_c`/`<signal handler called>`
frames in the previous round's `debug_notes.md` capture too — that read
turned out to be right after all). `-fno-strict-aliasing` did **not** fix
it — same crash, same site, ruling that theory out (kept anyway; harmless,
and still the right thing to build 1993 C against on principle).

CFSR `0x01000000` decodes to UFSR bit 8 (bit 24 of CFSR) = **UNALIGNED** —
a usage fault from an instruction that unconditionally requires natural
alignment (LDRD/STRD/LDM-class), not an ordinary bad-address bus fault.
That's consistent with, not contradictory to, `patch` being the same
`0xffffffff` seen before: an all-ones address is odd (fails any alignment
check) as well as almost certainly unmapped, so whichever of the two the
CPU checks first (alignment, for these instruction classes) is what
surfaces.

Traced it to a real, confirmed bug, found by inspecting every
`STlib_initMultIcon()` call site in `doom/st_stuff.c`: every other one
(`w_faces`, `w_keyboxes[0..2]`) passes a genuine `int*`/`int[]` (`st_faceindex`,
`keyboxes[]`); only `w_arms[]`'s init passed `(int *) &plyr->weaponowned[i+1]`
— casting a pointer into `player_t`'s `boolean weaponowned[NUMWEAPONS]`
array to `int*`, which `STlib_updateMultIcon` then dereferences as a full
4-byte int to select which patch to draw
(`V_DrawPatch(mi->x, mi->y, FG, mi->p[*mi->inum])`). Harmless on original
Linux DOOM, where `boolean` was an int-sized enum — but `doom/doomtype.h`
makes `boolean` a real 1-byte `bool` for `PICO`/C++ builds (`#if
defined(__cplusplus) || defined(PICO)`), so this reads 3 bytes *past* the
single flag it means to read, composing a garbage "icon index" out of
whichever `weaponowned[]` entries happen to sit next to it (or, for the
last of the six arm icons, `i+4` reaches index 9 of a 9-entry array —
genuinely out of bounds, into whatever `player_t` field follows it) — then
indexes the 2-entry `arms[i][]` patch-pointer array with that garbage
value, walking off into arbitrary nearby static memory. This explains
every observed detail: reproducible at specific moments (a weapon-pickup
event flips one of the relevant bytes from 0, changing the garbage
composite from a safe small value to a huge one), freeze point drifting
build to build (the garbage depends on whatever memory happens to be
adjacent, which shifts with layout), and only manifesting past `-O0` in
one respect worth noting for later — the bug itself isn't
optimization-dependent (it's a plain out-of-bounds read, present at any
optimization level), so Debug not reproducing it is presumably down to
incidental memory-layout/adjacent-value differences between build types
rather than this bug needing the optimizer, unlike the aliasing dead end
above.

**Fixed** (`doom/st_stuff.c`, `#ifdef PICO`): added `static int
armsowned[6]`, refreshed every tic from `plyr->weaponowned[i+1]` in the
same place and the same way `keyboxes[]` already is (`ST_updateWidgets`),
and pointed `w_arms[]`'s `STlib_initMultIcon()` call at `&armsowned[i]`
instead of the unsafe cast. Non-`PICO` builds keep the original vanilla
line unchanged. Builds clean, zero warnings.

**✔ Confirmed fixed on hardware (2026-09-12).** Demo autoplay now runs
through multiple demo sequences across multiple maps with no freeze —
previously died reliably around gametic 830 on MAP05 every single time.
Investigation closed.

Kept from this investigation even though they weren't the actual cause:
the `P_RunThinkers` use-after-free fix, the `openings[]`/`intercepts[]`
bounds checks (all three are real bugs, just not this one), the
`Z_CheckHeap()` step-bound guard, the `-fno-strict-aliasing` build flag,
and — most valuably — the rewritten `FaultHandler.cpp`, which turned "board
goes silent" into an actual crash report over plain USB serial and is what
made this whole bug findable without a debug probe.

## Key engine facts

- Renderer writes `screens[0]`: 320x200, palette-indexed 0-255. `I_SetPalette` gets
  PLAYPAL (256x3). `I_FinishUpdate` is the per-tic blit hook.
- Engine paces itself: I_GetTime in 1/35 s tics (TICRATE).
- RAM budget: ~520 KB SRAM + 8 MB PSRAM (total). Heap must live in PSRAM.
- Engine is C11/gnu90 split in CMakeLists (SDK headers need C11; 1993 engine code
  needs -std=gnu90). Keep that split.