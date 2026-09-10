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
(two more turned up in Phase 2/3, see below):
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

Mouse/gamepad input and the XPT2046 touch panel are not implemented --
keyboard alone already satisfies "playable" below.

Done state: playable with a USB keyboard. ⏳ Pending hardware test.

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