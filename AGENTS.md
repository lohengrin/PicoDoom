# AGENTS.md

## What this is

A Raspberry Pi **Pico 2 (RP2350)** port of DOOM.

- `doom/` — the classic **id Software Linux DOOM 1.10** source release (1997-12-23),
  kept close to upstream and lightly `#ifdef PICO` ported. The game engine here is
  otherwise unmodified except for the platform layer (the `i_*` files) and a small
  number of genuine portability bugs hit during hardware bring-up (see docs/PLAN.md's
  Phase 1 bug list — `alloca()` on a tiny stack, a `boolean`-holds -1 sentinel trick
  that breaks under PICO's C99 `bool`, a stale `VERSION` constant).
- `sndserv/` — DOS sound server, irrelevant to this port.
- `src/PicoDoom.cpp` — the Pico entry point (`main()`), replaces `doom/i_main.c`.
- Target hardware (user-specified): **Waveshare RP2350-PiZero** (RP2350B, 16 MB flash,
  8 MB PSRAM on GPIO47, PIO-USB on GPIO28/29, µSD card, and a **Waveshare 3.5" RPi
  LCD (A)** — SPI, ILI9486 controller + XPT2046 touch).
- Current state: Phase 1 (boot) and Phase 2 (ILI9486 LCD video) are **done and
  verified on real hardware** — the engine boots, mounts the µSD, loads the WAD, and
  DOOM renders on the panel (not yet a full 35 fps — see docs/PLAN.md's Performance
  notes). Phase 3 (USB-PIO HID keyboard) is implemented, not yet hardware-verified.
  Sound is still an intentional "bring-up" stub (`doom/i_sound_null.c`) — real audio
  is Phase 4. `build/`, board `waveshare_rp2350_pizero`.

## Key reference project (read this first)

`/home/lohengrin/Dev/TOM6809` is the same user's project already ported and **validated on
this exact hardware** (pizero + same 3.5" LCD + PSRAM + µSD + PIO-USB). Reuse it, don't
reinvent:

- `pico/CMakeLists.txt` — the `pizero_lcd` build profile (toolchain, board header choice,
  feature flags `WAVESHARE_LCD_SUPPORT` / `WAVESHARE_PSRAM_SUPPORT` / `WAVESHARE_SD_SUPPORT`
  / `WAVESHARE_USB_HID_SUPPORT`).
- `pico/boards/waveshare_rp2350_pizero.h` — custom board header that adds what the SDK's
  version lacks: `PICO_PSRAM_CS_PIN` (47), PIO-USB D+/D− pins, etc.
- `include/pico/Ili9486Display.hpp` + `src/ui_pico/Ili9486Display.cpp` — SPI ILI9486 driver
- `Xpt2046Touch.cpp`, `PicoSdCard_Waveshare.cpp`, `PicoUsbHidInput.cpp`, `Psram.cpp`
- `README-PICO.md` — build profiles and GPIO/pin table

LCD wiring used there (SPI1): SCK=GPIO10, MOSI=GPIO11, MISO=GPIO12 (touch), CS=GPIO8,
D/C=GPIO24, RST=GPIO25. Panel uses a 16-bit shift-register protocol with CMD/param bytes
(0x00 pad), CS pulsed per command, RGB565 big-endian pixel data.

## Build & firmware

```bash
cmake -S . -B build \
  -DPICO_SDK_PATH=/home/lohengrin/.pico-sdk/sdk/2.3.0
cmake --build build -j$(nproc)   # → build/PicoDoom.uf2
```

- Toolchain: `/home/lohengrin/.pico-sdk/toolchain/15_2_Rel1` (arm-none-eabi-gcc → rp2350 arm).
  The system `/usr/bin/arm-none-eabi-gcc` (gcc 14.2.1) also works and is what `build/`
  currently links with.
- SDK 2.3.0. `pico_sdk_import.cmake` picks it up via `PICO_SDK_PATH` (values seen:
  `2.0.0`/`2.2.0`/`2.3.0` under `~/.pico-sdk/sdk/`).
- `PICO_BOARD`/`PICO_BOARD_HEADER_DIRS` are set in `CMakeLists.txt` **before**
  `pico_sdk_import.cmake` (its `pico_pre_load_platform`, which decides rp2040 vs rp2350
  and the toolchain, runs at include time). `PICO_BOARD_HEADER_DIRS` points at this
  repo's `boards/`, and our `waveshare_rp2350_pizero.h` (copied from TOM6809) takes
  precedence over the SDK's — it adds `PICO_PSRAM_CS_PIN` (47) and PIO-USB D+/D− pins
  the SDK's version lacks.
- `pico_fatfs` is pulled in via FetchContent (same repo as TOM6809) and built with
  `PICO_PIO_USE_GPIO_BASE=1` INTERFACE (needed because the uSD MISO pin is GPIO40 > 31;
  without it the PIO-SPI driver addresses GPIO8 as GPIO40 and the mount hangs).
- `target_compile_definitions(PicoDoom PRIVATE PICO)` is what activates the `#ifdef PICO`
  block in `doom/d_main.c` / `doom/i_system.c`.
- Engine is 1993-era C: it only compiles under `-std=gnu90`. The Pico SDK headers need
  C11, so `CMAKE_C_STANDARD=11` globally and per-file `-std=gnu90` for every `DOOMSRC`
  file except `d_main.c` (which includes SDK headers). Keep that split in CMakeLists.txt.

## Doom port architecture: the `i_*` platform layer

The engine calls the platform through small interfaces (see `doom/i_*.h`). The classic
Linux `doom/` tree provides X11 implementations; this port replaces them per-file:

| file | upstream role | this port |
|------|--------------|-----------|
| `i_main.c` | `main()` → `D_DoomMain()` | not linked; `src/PicoDoom.cpp` is `main` |
| `i_system.c` | `I_GetTime`/`I_ZoneBase`/`I_Error`/`I_Quit`/`I_Init` | `I_ZoneBase` → PSRAM heap under `#ifdef PICO`; the rest rides the `/linux` syscall shim (`time_us_64`, `usleep`) |
| `i_video.c`, `i_video_pimoroni.c` | X11 video (`sys/ipc.h`, XShm) | **cannot compile for Pico**; replaced by `src/i_video_ili9486.cpp` (real ILI9486 driver, Phase 2) |
| `i_sound.c`, `i_net.c` | Linux OSS / IPX | not linked; `i_sound_null.c` stub for now, `i_net_null.c` mirrors `i_net.c`'s single-player `doomcom` setup |
| `i_sound_null.c`, `i_net_null.c` | — | headless bring-up stubs so the engine links |
| `d_main.c` | `D_DoomMain`, game startup | `#ifdef PICO` (stdio); stray `mkdir("c:\\doomdata",0)` in the `-cdrom` branch is guarded by `#ifndef PICO` |

Headless sound/net stubs implement the full symbol surface the engine references:
all sound `I_*` (Init/Start/Stop/Update/…) and `I_InitNetwork/I_NetCmd` (net now sets
up a real single-player `doomcom`, not a no-op — see docs/PLAN.md's Phase 1 bug list).

`doom/s_sound.c` itself (the layer above `i_sound_null.c`) is `#ifdef PICO`'d out at
each public entry point (`S_Init`/`S_Start`/`S_StartSoundAtVolume`/`S_StopSound`/
`S_UpdateSounds`/`S_ChangeMusic`) rather than left to run for nothing against the null
driver — it was still doing real work with no output to show for it: allocating/
scanning the mixing `channels[]` array every tic (`S_UpdateSounds`), and, worse,
`W_CacheLumpNum()`-loading a full music lump into the zone heap on every level/
finale/intermission transition (`S_ChangeMusic`) only for `i_sound_null.c`'s
`I_RegisterSong` to discard it. `S_Init` still sets `snd_SfxVolume`/`snd_MusicVolume`
so the options menu's volume sliders keep working; it just skips the now-unused
`channels[]` allocation, which is why every other guarded function must also stay
guarded (`channels` is NULL under `#ifdef PICO`).

### Video: src/i_video_ili9486.cpp (Phase 2, done; Phase 4.5 moved the blit to core1; Phase 4.6 moved it into SRAM)
- DOOM renderer writes `screens[0]`: 320×200, palette-indexed (indices 0–255).
- `I_SetPalette()` receives the PLAYPAL lump (256×3 RGB, full 8-bit values) each time
  the palette changes; rebuilds a 256-entry RGB565 (wire/big-endian) LUT via
  `gammatable[usegamma]`, matching the reference X11 driver's `UploadNewPalette`.
- `I_FinishUpdate()` (core0) no longer touches the LCD directly, and no longer
  `memcpy`s: `screens[0]` itself is ping-ponged between two plain SRAM arrays
  (`g_screen_buf[2]`, `.bss`, 128000 bytes total) — it hands the buffer the renderer
  just finished to core1 over the SDK inter-core FIFO, points `screens[0]` at the
  *other* one (already confirmed free) for the renderer to draw into next, and calls
  `R_InitBuffer(scaledviewwidth, viewheight)` to refresh `ylookup[]`/`columnofs[]`
  against the new address — see r_draw.c note below for why that call is mandatory,
  not optional. Only blocks if core1 hasn't freed the target buffer yet (i.e. only
  if core1's SPI feed, not core0's game logic/render, is the bottleneck).
  `src/i_video_core1.hpp`'s `i_video_core1_step()` does the actual work — LUT-convert
  one row + DMA-feed it to `Ili9486Display`, one row per call — from core1's loop
  (see below), centered 1:1 (no scaling) in an 80/60px black border. Stepped one row
  at a time (not one blocking per-frame call) so it interleaves with `tuh_task()`
  rather than starving Pico-PIO-USB's software-timed bus servicing for the ~41ms a
  full frame's SPI feed takes at 25 MHz.
- **`r_draw.c`'s `R_InitBuffer()` caches per-row pointers *into* `screens[0]`**
  (`ylookup[i] = screens[0] + ...`), refreshed only on view-size changes (the menu's
  screen-size +/- keys), not every frame — this is exactly why Phase 4.5 originally
  avoided ping-ponging `screens[0]` directly. Phase 4.6 does it anyway, but calls
  `R_InitBuffer()` itself after every swap to keep that cache valid. Any future code
  that reassigns `screens[0]` must do the same, or the renderer silently draws into
  a stale buffer.
- `src/Ili9486Display.{hpp,cpp}` — low-level SPI1 driver, ported verbatim from the
  validated TOM6809 project (shift-register wire protocol, panel init sequence);
  needs C++20 (`std::span`), hence `CMAKE_CXX_STANDARD 20`. Only ever touched from
  core0 during the one-time `I_InitGraphics()`/`fill_solid()` bring-up call and from
  core1 thereafter (`i_video_core1_step()`) — never concurrently.
- Stats line (10s interval): `core0-wait%` (backpressure — high means core1 is the
  bottleneck), `core1-convert%` (the palette→RGB565 LUT loop — now reads SRAM, not
  PSRAM) and `core1-dma%` (the actual `write_pixels()` SPI feed) — split so the two
  very different-sounding hypotheses for "why isn't this faster" (RAM bandwidth vs.
  SPI bus) show up as two different numbers instead of one opaque blended one. See
  docs/PLAN.md's Phase 4.5/4.6 for the full history: a row-batching attempt
  (`kRowsPerChunk=8`, since reverted to 1) made things both slower *and* caused a
  hang after ~20s — plausibly Pico-PIO-USB's software-timed bus servicing needing
  `tuh_task()` called more often than one chunk's worth of convert+DMA time allowed,
  though unconfirmed; the convert/dma split then showed a near-50/50 PSRAM-vs-SPI
  cost, which Phase 4.6 addresses by moving the buffers to SRAM.
- `doom/tables.c`'s `finesine`/`finetangent`/`tantoangle` (~64KB) are `const` under
  `#ifdef PICO` (`.rodata`/flash instead of `.data`/SRAM) — confirmed safe by finding
  that `R_InitPointToAngle()`/`R_InitTables()`, the only code that ever assigns to
  them, are both `#if 0`'d out by id Software themselves. This is what freed the SRAM
  for `g_screen_buf` above; see docs/PLAN.md's Phase 4.6 memory audit for what was
  and wasn't safe to relocate (most of DOOM's big SRAM users are hot renderer state —
  `visplanes`, `openings`, etc. — genuinely not movable without slowing the renderer).

### Timing / loop
The engine paces itself: `I_GetTime()` in tics (35 Hz, `TICRATE`), `I_StartTic`,
`I_StartFrame`, `I_WaitVBL`. On bare metal, `I_GetTime` must come from a 1 MHz timer
(`time_us_64()`) — do NOT block the whole CPU for 1/35 s; let vbl/sync come from the
LCD driver.

## Memory constraints (critical)

- `I_GetHeapSize`/`I_ZoneBase` in `doom/i_system.c` default to **6 MB** (`mb_used=6`) — far
  over the RP2350 SRAM. Under `#ifdef PICO`, `I_ZoneBase` now allocates from PSRAM via
  `psram_malloc` (`src/Psram.cpp` + `hardware_psram` QMI, XIP 0x1100_0000, zone = 6 MB).
- total RAM ≈ 520 KB SRAM + 8 MB PSRAM.

## WAD / filesystem

- `w_wad.c`, `m_misc.c`, `d_main.c` use heavy `fopen/fread/fseek/access` on the host OS.
  Since Phase 1 the classic Linux syscalls are shimmed onto FatFs by `src/sd_stdio.c`:
  it mounts the uSD over PIO-SPI (GPIO30/31/40/43 incl. `PICO_PIO_USE_GPIO_BASE=1` for
  MISO>31) and provides strong `_open/_read/_write/_lseek/_close/_fstat/_stat/_isatty`
  (+ `_gettimeofday`/`usleep`) that newlib's weak stubs defer to — so
  `fopen`/`read`/`access` hit the FAT filesystem; console fds 0/1/2 pass to pico_stdio.
  The engine itself is untouched.
- If a canonical wad (`doom1.wad`/`doom.wad`/…) sits on the SD root, `IdentifyVersion`
  finds it via `access()`. `src/PicoDoom.cpp` still seeds `myargc=1/myargv`.

## Doom engine facts worth keeping in mind

- `D_DoomMain()` handles all startup: argv parsing (`-file`, `-warp`, …), wad selection,
  defaults file, then `D_DoomLoop()`.
- Title/credits/finale/menu/intermission/status bar all render through `v_video` onto
  `screens[0]`; the automap overlays the same framebuffer. Full 320×200 index pipeline.
- `doomstat`/`gamestate` etc. are plain globals; a native `pause` and the console output
  can be added without touching the engine.
- C11 (C standard), C++20 for drivers (needed for `std::span` in Ili9486Display);
  engine is C → easy `extern "C"` binding, but the *definitions* need `extern "C"`
  too when they live in a `.cpp` file (see src/i_video_ili9486.cpp), not just the
  included header's declarations, or the linker gets C++-mangled symbol names.

## Conventions

- Keep `doom/` engine files untouched (upstream, ported only via `#ifdef PICO`).
  New Pico-specific files go under `src/` (e.g. `PicoDoom.cpp`, drivers) and `doom/` only
  for small `i_*` shims.
- Follow the validated TOM6809 code for any Pico hardware access.
- No comments where the code is self-evident; keep the user's style.
- `README.md` is stale (still describes the old PiCoMonitor SSD1306 demo) — update it
  when the port progresses, not before.

## Project layout (current)

```
CMakeLists.txt        # PicoDoom target; DOOMSRC list; pico_fatfs FetchContent; board vars
src/PicoDoom.cpp      # bare-metal main(): PSRAM init → sd_init() → D_DoomMain()
src/Psram.cpp/.hpp    # hardware_psram driver + psram_malloc/free (zone heap), free-list allocator
src/sd_stdio.c        # uSD mount (pico_fatfs PIO-SPI) + newlib syscall shim (_open/_read/…)
src/Ili9486Display.cpp/.hpp  # low-level ILI9486/SPI1 driver, ported from TOM6809
src/i_video_ili9486.cpp      # DOOM i_video.h impl: screens[0] ping-ponged (SRAM) -> core1 -> LUT -> LCD
src/i_video_core1.hpp        # i_video_core1_step(): one row of pending blit work, called from core1
src/PicoUsbKeyboard.cpp/.hpp # USB-PIO HID keyboard host on core1 (Phase 3); owns the
                              # shared tuh_hid_*_cb callbacks (TinyUSB allows only one each);
                              # core1_entry()'s loop also drives i_video_core1_step() (Phase 4.5)
src/PicoUsbMouse.cpp/.hpp    # USB HID mouse (Phase 3), dispatched from PicoUsbKeyboard's callbacks
src/i_input_usbhid.cpp       # I_StartTic: HID reports -> DOOM event_t/D_PostEvent
src/tusb_config.h            # TinyUSB config: device (stdio_usb) + host (keyboard)
boards/               # waveshare_rp2350_pizero.h (adds PSRAM CS + PIO-USB pins)
doom/                 # upstream DOOM 1.10 + null i_* stubs + PICO guards
doom/i_sound_null.c   # sound stub
doom/i_net_null.c     # net stub
docs/                 # PLAN.md + Waveshare product docs (saved pages)
build/                # RP2350 build (rm -rf safe, see Build & firmware above)
```