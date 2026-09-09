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

Getting from "builds clean" to "boots on hardware" took six real bugs, all fixed:
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

A `PICODOOM_DIAG_STAGE` CMake option (default 3 = normal boot) was added to
`src/PicoDoom.cpp`/`CMakeLists.txt` to bisect exactly this kind of "no USB output"
failure without a debug probe — heartbeats at each init stage instead of falling
through. Worth keeping for Phase 2/3 bring-up.

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

Replace `doom/i_video_null.c` with a real ILI9486 driver (C++, `extern "C"`).

- `I_InitGraphics` — init SPI + ILI9486 (reuse TOM6809 Ili9486Display), configure
  320x200 viewport on the 480x320 panel (scale 1.5x or 1:1 with border).
- `I_SetPalette` — cache PLAYPAL (256x3) and (re)build a 256-entry RGB565 lookup table.
- `I_FinishUpdate` — LUT `screens[0]` -> RGB565 and DMA it to the panel every tic.
- `I_ReadScreen`, `I_UpdateNoBlit`, `I_StartTic`, `I_StartFrame` — trivial once the
  framebuffer path works (I_ReadScreen just memcpy of screens[0]).
- Keep the 35 Hz pacing in I_GetTime (time_us_64); don't busy-wait the whole tic.

Performance: 320*200 = 64k px; ~192 KB/frame RGB565 over SPI — use DMA/dual-buffer
follow the validated TOM6809 streaming pattern. If too slow, flip to 1:1 + border.

Done state: DOOM is visible and playable-by-menu at 35 fps.

## Phase 3 — Input

`PicoUsbHidInput` (PIO-USB host on GPIO28/29) for keyboard/mouse/gamepad.

- Feed DOOM event_t via `I_StartTic`/`D_PostEvent` (keydown/keyup/mouse).
- Map a joypad stick to turn/forward ticcmds (synthesize key events, or fill
  `I_BaseTiccmd` ticcmd directly — engine reads those every tic).
- XPT2046 touch could trigger menu/pause actions later (non-blocking).

Done state: playable with a USB gamepad/keyboard.

## Phase 4 — Sound (defer)

Port `s_sound.c` output to a single mixed 11 kHz SFX stream (DOOM mixes ~8 voices,
2 at once typically). Timer IRQ -> DMA to PWM or a PIO DAC. Ignore music (no MIDI on
the Pico) or replace with a simple noise/beat. Lowest priority.

## Phase 5 — Polish

- Save games to µSD (p_saveg already uses files via w_wad/m_misc -> FS shim).
- Pause button, on-screen FPS, doomstat console.
- `I_Quit` -> reboot instead of exit.
- Update README.md (stale: still describes the PCIRAM SSD1306 demo).

## Key engine facts

- Renderer writes `screens[0]`: 320x200, palette-indexed 0-255. `I_SetPalette` gets
  PLAYPAL (256x3). `I_FinishUpdate` is the per-tic blit hook.
- Engine paces itself: I_GetTime in 1/35 s tics (TICRATE).
- RAM budget: ~520 KB SRAM + 8 MB PSRAM (total). Heap must live in PSRAM.
- Engine is C11/gnu90 split in CMakeLists (SDK headers need C11; 1993 engine code
  needs -std=gnu90). Keep that split.