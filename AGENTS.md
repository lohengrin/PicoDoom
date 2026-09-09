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
- Current state: Phase 1 is **done and verified on real hardware** — the engine boots,
  mounts the µSD, loads the WAD, and runs a live game loop over USB serial, headless
  (`build/`, board `waveshare_rp2350_pizero`). The classic video/sound/net files
  are NOT linkable on Pico, so video, sound and net are intentional "bring-up" stubs for
  now. Bringing the real LCD/input/sound (Phase 2/3, see docs/PLAN.md) is the current
  development goal.

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
| `i_video.c`, `i_video_pimoroni.c` | X11 video (`sys/ipc.h`, XShm) | **cannot compile for Pico** — replaced by null for now; real driver: ILI9486 (see below) |
| `i_sound.c`, `i_net.c` | Linux OSS / IPX | not linked; `i_sound_null.c`,`i_net_null.c` stubs for now |
| `i_video_null.c`, `i_sound_null.c`, `i_net_null.c` | — | headless bring-up stubs so the engine links |
| `d_main.c` | `D_DoomMain`, game startup | `#ifdef PICO` (stdio); stray `mkdir("c:\\doomdata",0)` in the `-cdrom` branch is guarded by `#ifndef PICO` |

Headless stubs currently implement the full symbol surface the engine references:
`I_InitGraphics/ShutdownGraphics/SetPalette/UpdateNoBlit/FinishUpdate/ReadScreen/WaitVBL/StartTic/StartFrame/GetEvent`,
all sound `I_*` (Init/Start/Stop/Update/…) and `I_InitNetwork/I_NetCmd`.

### Rendering contract (for the future LCD driver)
- DOOM renderer writes `screens[0]`: 320×200, palette-indexed (indices 0–255).
- `I_SetPalette()` receives the PLAYPAL lump (256×3 RGB); cache it until next change.
- `I_FinishUpdate()` is called at the end of every tic → blit `screens[0]` to the LCD
  (convert index→RGB565; scale 320×200 to the 480×320 panel or 1:1 with border).
- Rendering happens synchronously on core 0 in the tic loop (single-core, no RTOS).

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
- C11 (C standard), C++17 for drivers; engine is C → easy `extern "C"` binding.

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
boards/               # waveshare_rp2350_pizero.h (adds PSRAM CS + PIO-USB pins)
doom/                 # upstream DOOM 1.10 + null i_* stubs + PICO guards
doom/i_video_null.c   # headless video stub
doom/i_sound_null.c   # sound stub
doom/i_net_null.c     # net stub
docs/                 # PLAN.md + Waveshare product docs (saved pages)
build/                # RP2350 build (rm -rf safe, see Build & firmware above)
```