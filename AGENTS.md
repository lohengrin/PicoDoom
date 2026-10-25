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
- Second board: **Pimoroni Presto** (`-DPICODOOM_VIDEO_OUTPUT=presto`, `build-presto/`) --
  see "Presto variant" below. Written without hardware access: **builds clean, not yet
  hardware-verified** (phased checkpoints in that section).
- Output variants (`-DPICODOOM_VIDEO_OUTPUT=`): `lcd` (ILI9486, default, `build/`),
  `lcd-st7796` (ST7796U panel, 132 MHz SPI, `build-st7796/`), `hdmi` (onboard DVI +
  HDMI audio, `build-hdmi/`). All three are playable on real hardware. LCD builds have
  no audio; the HDMI build has SFX (`src/i_sound_dvi.cpp`) and music
  (`src/mus_player.cpp`, MUS parser + chip-tune synth, not GM/OPL). Boot WAD menu,
  USB keyboard/mouse/gamepad, savegames all done. `cmake --build <dir> --target install`
  copies renamed images to `install/`. Board `waveshare_rp2350_pizero`.
- Performance state (2026-10, `lcd-st7796` 480x300): ~33 FPS moving, 35 (tick cap)
  standing still, up from 22.7 at the start of the perf pass -- see "Video" below for
  what got it there and what didn't work.

## Presto variant (Pimoroni Presto, RP2350B)

Full reference: **[docs/PRESTO.md](docs/PRESTO.md)**. In short: `-DPICODOOM_VIDEO_OUTPUT=presto` (board `pimoroni_presto`,
build dir `build-presto/`), 480x480 ST7701 RGB panel scanned out of SRAM by PIO+DMA with the game in the top 480x300
(`-DPICODOOM_PRESTO_MODE=scanline|palette`), FT6236 touch, Bluetooth HID (keyboard/mouse/gamepad) + QwSTPad, sound on the
piezo (PCM) or a Bluetooth headset (A2DP), health-driven WS2812 LEDs, 264 MHz by default. Key rules: core1 runs only
RAM code (Bluetooth bonding writes flash from core0); never put a big per-frame buffer in PSRAM; frame copies are
beam-synchronised; the engine's `states[]`/`mobjinfo[]` live in PSRAM to free SRAM. Hardware-verified.

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

## Pico-Toolset (shared driver submodule)

`third_party/pico-toolset` (git submodule, `github.com/lohengrin/Pico-Toolset`) holds
reusable RP2040/RP2350 drivers extracted from this project, TOM6809, and PiCoMonitor,
built as independent CMake components (`pico_toolset_<name>` targets, gated by
top-level `PICO_TOOLSET_BUILD_<NAME>` options set in this project's `CMakeLists.txt`).
Phase 1 (done) moved PSRAM, the SD card mount + POSIX/stdio syscalls, the hard-fault
handler, and the shared board header onto it — see `pico_toolset_psram`/
`pico_toolset_sdcard`/`pico_toolset_fault_handler` and their headers under
`third_party/pico-toolset/components/`/`libs/`. Phase 2 (done) moved the ILI9486
display and USB-HID keyboard/mouse input onto `pico_toolset_ili9486`/
`pico_toolset_usb_hid` too — `src/i_video_spi_lcd.cpp` now drives `pico_toolset::Ili9486`
directly (no more `src/Ili9486Display.*`), and `src/i_input_usbhid.cpp` owns core1 itself
(launching it, then calling `pico_toolset::UsbHidHost::init()` with
`configs::usb_hid::kWaveshareRp2350PiZeroLcdManualCore1`, `run_on_core1=false`) so that
same core1 loop can interleave `UsbHidHost::task()` with `i_video_core1_step()` — no more
`src/PicoUsbKeyboard.*`/`src/PicoUsbMouse.*`. That preset, plus `UsbHidHost`'s
`consume_mouse_delta()` (raw relative movement for mouselook, alongside the existing
clamped-cursor `mouse_state()`) and `MouseState::middle_button`, were contributed to
Pico-Toolset as part of this move — see that repo's AGENTS.md "Source lineage". When
extending or fixing a driver PicoDoom uses via the submodule, prefer fixing/extending
it in Pico-Toolset (then bumping the submodule pin) over patching a local fork — that's
the whole point of sharing it with TOM6809.

## Build & firmware

```bash
cmake -S . -B build \
  -DPICO_SDK_PATH=/home/lohengrin/.pico-sdk/sdk/2.3.1
cmake --build build -j$(nproc)   # → build/PicoDoom.uf2
```

- Toolchain: `/home/lohengrin/.pico-sdk/toolchain/15_2_Rel1` (arm-none-eabi-gcc → rp2350 arm).
  The system `/usr/bin/arm-none-eabi-gcc` (gcc 14.2.1) also works and is what `build/`
  currently links with.
- SDK 2.3.0. `pico_sdk_import.cmake` picks it up via `PICO_SDK_PATH` (values seen:
  `2.0.0`/`2.2.0`/`2.3.0` under `~/.pico-sdk/sdk/`).
- `PICO_BOARD`/`PICO_BOARD_HEADER_DIRS` are set in `CMakeLists.txt` **before**
  `pico_sdk_import.cmake` (its `pico_pre_load_platform`, which decides rp2040 vs rp2350
  and the toolchain, runs at include time). `PICO_BOARD_HEADER_DIRS` points at
  `third_party/pico-toolset/boards/` (a git submodule, shared with TOM6809 — see
  "Pico-Toolset" below), whose `waveshare_rp2350_pizero.h` takes precedence over the
  SDK's — it adds `PICO_PSRAM_CS_PIN` (47) and PIO-USB D+/D− pins the SDK's version
  lacks.
- `pico_fatfs` and Pico-PIO-USB/`tinyusb_host` come from Pico-Toolset's
  `add_subdirectory(third_party/pico-toolset)` (its `cmake/pico_fatfs.cmake`/
  `cmake/pico_pio_usb.cmake`), not this project's own `FetchContent` blocks anymore.
  `pico_fatfs` is built with `PICO_PIO_USE_GPIO_BASE=1` INTERFACE there (needed because
  the uSD MISO pin is GPIO40 > 31; without it the PIO-SPI driver addresses GPIO8 as
  GPIO40 and the mount hangs).
- `target_compile_definitions(PicoDoom PRIVATE PICO)` is what activates the `#ifdef PICO`
  block in `doom/d_main.c` / `doom/i_system.c`.
- Engine is 1993-era C: it only compiles under `-std=gnu90`. The Pico SDK headers need
  C11, so `CMAKE_C_STANDARD=11` globally and per-file `-std=gnu90` for every `DOOMSRC`
  file except `d_main.c` (which includes SDK headers). Keep that split in CMakeLists.txt.

### Variant identity vs capabilities
`PICODOOM_HDMI` / `_ST7796` / `_ILI9486` / `_PRESTO` (set per `PICODOOM_VIDEO_OUTPUT` in `CMakeLists.txt`) are for code that is
really per-board (clocks, pins, board config). Everything else tests a **capability** macro, so a new board only has to
say what it has: `PICODOOM_SPI_LCD` (SPI panel via `DisplayPanel` + XPT2046 touch + USB host on core1),
`PICODOOM_RUNTIME_RES` (runtime 320x200/480x300, `screens[0]` in SRAM), `PICODOOM_DEFAULT_HIRES` (0/1),
`PICODOOM_INFO_IN_PSRAM` (`states[]`/`mobjinfo[]` in PSRAM, `doom/info.c`), `PICODOOM_DEFERRED_VIDEO_COPY`
(`presto_video_poll()`/`presto_video_flush()` hooks), `PICODOOM_VIDEO_OWNS_SCREEN0`, `PICODOOM_AUDIO`. Add a macro there
instead of another `!defined(PICODOOM_HDMI)`.

## Doom port architecture: the `i_*` platform layer

The engine calls the platform through small interfaces (see `doom/i_*.h`). The classic
Linux `doom/` tree provides X11 implementations; this port replaces them per-file:

| file | upstream role | this port |
|------|--------------|-----------|
| `i_main.c` | `main()` → `D_DoomMain()` | not linked; `src/PicoDoom.cpp` is `main` |
| `i_system.c` | `I_GetTime`/`I_ZoneBase`/`I_Error`/`I_Quit`/`I_Init` | `I_ZoneBase` → PSRAM heap under `#ifdef PICO`; the rest rides the `/linux` syscall shim (`time_us_64`, `usleep`) |
| `i_video.c`, `i_video_pimoroni.c` | X11 video (`sys/ipc.h`, XShm) | **cannot compile for Pico**; replaced by `src/i_video_spi_lcd.cpp` (real ILI9486 driver, Phase 2) |
| `i_sound.c`, `i_net.c` | Linux OSS / IPX | not linked; `i_sound_null.c` stub for now, `i_net_null.c` mirrors `i_net.c`'s single-player `doomcom` setup |
| `i_sound_null.c`, `i_net_null.c` | — | headless bring-up stubs so the engine links |
| `d_main.c` | `D_DoomMain`, game startup | `#ifdef PICO` (stdio); stray `mkdir("c:\\doomdata",0)` in the `-cdrom` branch is guarded by `#ifndef PICO` |

Headless sound/net stubs implement the full symbol surface the engine references:
all sound `I_*` (Init/Start/Stop/Update/…) and `I_InitNetwork/I_NetCmd` (net now sets
up a real single-player `doomcom`, not a no-op — see docs/PLAN.md's Phase 1 bug list).

HDMI build: `S_ChangeMusic` is live (guard is `PICO && !PICODOOM_HDMI`, like the other
five) and drives `src/mus_player.cpp` through `i_sound_dvi.cpp`'s `I_*Song` functions
(MUS lumps, 140 Hz tick, mixed wall-clock-paced in `mix_tic()`).

`doom/s_sound.c` itself on LCD builds (the layer above `i_sound_null.c`) is `#ifdef PICO`'d out at
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

### Video: SPI LCD builds (ILI9486 `lcd`, ST7796U `lcd-st7796`)

Full reference: **[docs/SPI_LCD.md](docs/SPI_LCD.md)** (`src/i_video_spi_lcd.cpp`, one file for both panels). In short:
`screens[0]` (320x200 or 480x300, indexed) lives in SRAM and is never reassigned (the status bar and the wipe depend on
its content/address persisting); `I_FinishUpdate` hashes 25-row bands, skips unchanged ones, converts the rest through the
RGB565 LUT into SRAM slots and core1 DMAs them to the panel; never write the converted frame to PSRAM (writes are slow).

### Timing / loop
The engine paces itself: `I_GetTime()` in tics (35 Hz, `TICRATE`), `I_StartTic`,
`I_StartFrame`, `I_WaitVBL`. On bare metal, `I_GetTime` must come from a 1 MHz timer
(`time_us_64()`) — do NOT block the whole CPU for 1/35 s; let vbl/sync come from the
LCD driver.

## Memory constraints (critical)

- `I_GetHeapSize`/`I_ZoneBase` in `doom/i_system.c` default to **6 MB** (`mb_used=6`) — far
  over the RP2350 SRAM. Under `#ifdef PICO`, `I_ZoneBase` now allocates from PSRAM via
  `psram_malloc` (Pico-Toolset's `pico_toolset_psram` component, `hardware_psram` QMI,
  XIP 0x1100_0000, zone = 6 MB — see "Pico-Toolset" above).
- total RAM ≈ 520 KB SRAM + 8 MB PSRAM.

## WAD / filesystem

- `w_wad.c`, `m_misc.c`, `d_main.c` use heavy `fopen/fread/fseek/access` on the host OS.
  Since Phase 1 the classic Linux syscalls are shimmed onto FatFs by Pico-Toolset's
  `pico_toolset_sdcard` component (`PICO_TOOLSET_SDCARD_STDIO=ON`, see "Pico-Toolset"
  above): it provides strong `_open/_read/_write/_lseek/_close/_fstat/_stat/_isatty`
  that newlib's weak stubs defer to, so `fopen`/`read`/`access` hit the FAT filesystem;
  console fds 0/1/2 pass to pico_stdio. `src/sd_stdio.cpp` just mounts the card
  (`pico_toolset::SdCard::init()`, PIO-SPI GPIO30/31/40/43 incl.
  `PICO_PIO_USE_GPIO_BASE=1` for MISO>31) and carries `_gettimeofday`/`usleep`. The
  engine itself is untouched.
- If a canonical wad (`doom1.wad`/`doom.wad`/…) sits on the SD root, `IdentifyVersion`
  finds it via `access()`. `src/PicoDoom.cpp` still seeds `myargc=1/myargv`.

## Boot WAD-selection menu (Phase 6, LVGL)

- `src/PicoDoom.cpp` runs `wad_menu_run()` (declared `extern "C" bool`) between USB-host
  init and `D_DoomMain()`: an LVGL v9.2.2 screen listing every IWAD on the uSD root
  (touch / mouse / 640x480-scaled / keyboard / gamepad), 30s countdown auto-starts the
  persisted pick, Esc/Skip/gamepad-B cancels. Cancelling (or the display failing to come
  up) falls through to the engine's fixed-name scan untouched.
- **Engine handoff (doom/d_main.c, `IdentifyVersion`)**: a `#ifdef PICO` block sits
  before the fixed-name scan — `pico_selected_wad()` from `src/wad_boot.cpp`, checked
  with `access()`, then `gamemode = pico_wad_gamemode(sel); D_AddFile(sel); return;`.
  NOT `-file` (shareware `I_Error`s on `-file`). Gamemode is sniffed from the WAD's
  lump directory in 256-entry batches (no fancy malloc), per `wad_boot.cpp`.
- **Persistence (doom/m_misc.c)**: `#ifdef PICO` `extern char* pico_last_wad;` (defined
  in `wad_boot.cpp`) and a `defaults[]` entry `{"picodoom_lastwad", (int*)&pico_last_wad,
  (int)""}` so clean-quit `M_SaveDefaults()` re-saves it; the menu itself rewrites
  `default.cfg` at select time (replace-or-append) so a power-cut still pre-highlights
  next boot. C90 rules apply in `doom/` — keep to `/* */` comments and K&R style there.
- **`I_InitGraphics()` is once-guarded** in both `src/i_video_spi_lcd.cpp` and
  `src/i_video_dvi.cpp` (the menu calls it first; the second call must be a no-op —
  a DVI re-init would wedge the TMDS lane).
- **Input exit hygiene**: `i_input_reset_menu_input()` (`src/i_input_usbhid.cpp`) is
  called on every menu exit path — re-baselines all `g_prev_*` HID edge latches and
  drains mouse delta so the still-held Enter from confirming the menu doesn't post as
  "use" into the first gameplay tics. USB polling during the menu: LCD build no-op
  (core1 owns the stack), HDMI build `i_input_menu_usb_task()` from the menu's pump.
- **LVGL config** (`src/lv_conf.h`, forced via `LV_CONF_PATH` before LVGL's
  `FetchContent`): dark theme, Montserrat 14/22/28 only, `LV_MEM_SIZE` 56 KB all
  PSRAM-backed by `src/lvgl_mem_pool.cpp`'s `lvgl_mem_pool()` — safe because the menu
  is core0-only. Display tile buffers are also PSRAM (40-row: 38,400 B LCD /
  25,600 B DVI). The HDMI LVGL canvas is 320x240 = `g_framebuf` 1:1 native-endian
  (NOT TOM6809's 640x480 fold driver); the LCD canvas is the full 480x320 panel with
  the same byte-swapped flush the game blit uses.
- **Indevs** (`src/lvgl_indev.cpp`): LVGL's native keypad `lv_group` drives the WAD
  buttons (arrows/Enter/Esc/Tab via HID usages, gamepad fallback). Buttons carry
  `LV_OBJ_FLAG_EVENT_BUBBLE` so group-key events reach the screen's `LV_EVENT_KEY`
  handler (ESC is `lv_indev_get_key(lv_indev_active())` — there is no
  `lv_event_get_key()` in v9.2). `lvgl_indev_input_seen()`/`_clear()` let the menu
  stop its countdown on any input.

## Doom engine facts worth keeping in mind

- `D_DoomMain()` handles all startup: argv parsing (`-file`, `-warp`, …), wad selection,
  defaults file, then `D_DoomLoop()`.
- Title/credits/finale/menu/intermission/status bar all render through `v_video` onto
  `screens[0]`; the automap overlays the same framebuffer. Full 320×200 index pipeline.
- `doomstat`/`gamestate` etc. are plain globals; a native `pause` and the console output
  can be added without touching the engine.
- C11 (C standard), C++20 for drivers (needed for `std::span` in `pico_toolset::Ili9486`);
  engine is C → easy `extern "C"` binding, but the *definitions* need `extern "C"`
  too when they live in a `.cpp` file (see src/i_video_spi_lcd.cpp), not just the
  included header's declarations, or the linker gets C++-mangled symbol names.
- `doom/r_draw.c`'s column/span drawers (`R_DrawColumn` and friends) cache
  `dc_source`/`dc_colormap`/etc. into locals before their hot loops under
  `#ifdef PICO` (Phase 4.7) — GCC otherwise reloads these globals every pixel,
  confirmed via disassembly, since it can't prove the `*dest` store doesn't
  alias them. Same fix id Software's own `#if 0`'d reference versions already
  used, just never enabled. If touching these functions, keep new reads going
  through the cached locals, not the `dc_*`/`ds_*` globals, inside the loop.

## Tests

Host (PC) unit tests for the hardware-free logic live in `tests/host`
(`cmake -S tests/host -B tests-build && cmake --build tests-build && ctest --test-dir tests-build`):
`pixel_scale.hpp` (1.5x row expansion / LUT conversion), `joystick.hpp`, `led_colours.hpp`, `piezo_contour.hpp`.
New pure logic goes in a header under `src/` so it can be tested there (the toolset has its own
`tests/host` for `SampleMixer`, audio sources and HID parsing).

## Conventions

- Keep `doom/` engine files untouched (upstream, ported only via `#ifdef PICO`).
  New Pico-specific files go under `src/` (e.g. `PicoDoom.cpp`, drivers) and `doom/` only
  for small `i_*` shims.
- Follow the validated TOM6809 code for any Pico hardware access.
- No comments where the code is self-evident; keep the user's style.
- `README.md` describes the current builds (all four variants, options, controls) -- keep it in
  sync when a variant, CMake option or control changes.

## Project layout (current)

```
CMakeLists.txt        # PicoDoom target; DOOMSRC list; Pico-Toolset add_subdirectory; board vars
src/PicoDoom.cpp      # bare-metal main(): PSRAM init → sd_init() → D_DoomMain()
src/sd_stdio.cpp       # uSD mount (pico_toolset::SdCard) + _gettimeofday/usleep
                        # (POSIX stdio syscalls themselves now live in pico_toolset_sdcard)
src/i_video_gamma.hpp        # picodoom::GammaPalette: PLAYPAL cache + gammatable/extra gamma curve, shared by every video backend
src/i_video_spi_lcd.cpp      # DOOM i_video.h impl, both SPI LCD panels (PICODOOM_ST7796 picks St7796, else Ili9486; all via
                              # DisplayPanel): SRAM screens[0] -> hash/skip + LUT convert into SRAM slots -> core1 DMA
src/i_video_core1.hpp        # i_video_core1_step(): one band of pending blit work, called from core1
src/i_video_dvi.cpp          # HDMI build's i_video.h impl (g_framebuf + core1 DVI encoder) +, like
                              # i_video_spi_lcd.cpp, an I_InitGraphics() once-guard and the
                              # i_video_dvi_framebuf()/width()/height() menu accessors
src/i_input_usbhid.cpp       # (now only the USB backend: HidSource adapter, core1 launch; event posting is in
                              # src/i_input_tic.cpp, Presto's backend in src/i_input_presto.cpp)
                              # ... plus menu glue: i_input_hid() (the UsbHidHost& the menu's indevs
                              # read), i_input_menu_usb_task() (HDMI build polls UsbHidHost::task()
                              # from the menu loop; no-op on LCD where core1 owns it), and
                              # i_input_reset_menu_input() (drain mouse delta + re-baseline every
                              # g_prev_* edge latch so a held menu key can't fire into the first
                              # gameplay tics -- called on every menu exit path)
                              # (launches it, then init()s the host stack from inside core1_entry()
                              # with run_on_core1=false, since Pico-PIO-USB's IRQ binds to whichever
                              # core calls tuh_init() -- see file header); I_StartTic: HID state ->
                              # DOOM event_t (ev_keydown/up, ev_mouse, ev_joystick)/D_PostEvent --
                              # doom/g_game.c and doom/m_menu.c already have full native joystick
                              # support (gamepad_state(0)'s A/B/X/Y -> fire/strafe/run/use, left
                              # stick+D-pad -> turn/move, menu navigation all pre-existing 1993 code,
                              # just never fed real input before); core1_entry()'s loop also drives
                              # i_video_core1_step() (Phase 4.5, kept from the old PicoUsbKeyboard.cpp);
                              # ALL F-keys post vanilla DOOM keys (help/save/load/volume/quit/gamma/
                              # spy-mode, doom/m_menu.c + doom/g_game.c) -- the Pico tuning controls
                              # that used to shadow F1-F4/F10-F12 live in the serial console instead
src/i_serial_console.cpp     # Pico tuning console over the USB CDC (same link as printf, polled
                              # from I_StartTic()): line-oriented commands gamma/
                              # mouse/sens replace the freed F-key intercepts (see i_video_spi_lcd.cpp,
                              # i_video_dvi.cpp, i_input_usbhid.cpp accessors); engine never reads
                              # stdin on Pico, so this owns CDC input exclusively
src/lv_conf.h              # LVGL config (Phase 6 boot menu): dark theme, Montserrat 14/22/28,
                           # LV_MEM_SIZE 56KB PSRAM-backed via LV_MEM_POOL_ALLOC
src/lvgl_mem_pool.cpp      # LVGL heap hook: psram_malloc() (else malloc) -- core0-only safe
src/lvgl_display_lcd.cpp   # LVGL display driver, LCD build: full-panel 480x320 on the game's own
                           # pico_toolset::Ili9486 (byte-swapped wire order), 40-row PSRAM tile buf
src/lvgl_display_dvi.cpp   # LVGL display driver, HDMI build: 320x240 canvas = g_framebuf 1:1
                           # (native endian, NO fold/swap -- not TOM6809's 640x480 fold driver)
src/lvgl_indev.cpp         # LVGL indevs: mouse (scales 640x480 cursor), touch (LCD, Xpt2046Calibration
                           # defaults), keypad (keyboard+gamepad -> LV_KEY_*) + input_seen flag that
                           # stops the menu's auto-start countdown
src/wad_boot.h/.hpp        # boot WAD selection + gamemode sniffing + default.cfg read/rewrite
src/menu_board.hpp         # per-board parts of the boot menu (display init, touch init, extra widgets, poll): menu_board_{spi_lcd,hdmi,presto}.cpp;
                           # wad_menu.cpp itself has no board #ifs
src/wad_menu.cpp           # boot WAD-selection menu: title, per-IWAD buttons (one lv_group), 30s
                           # auto-start countdown, Skip; calls I_InitGraphics() (once-guarded) first
third_party/pico-toolset/    # git submodule: shared PSRAM/SD-card/fault-handler/board-header/
                              # ILI9486/USB-HID drivers (see "Pico-Toolset" above) -- pico_toolset_psram,
                              # pico_toolset_sdcard, pico_toolset_fault_handler, pico_toolset_ili9486,
                              # pico_toolset_usb_hid, boards/
doom/                 # upstream DOOM 1.10 + null i_* stubs + PICO guards
doom/i_sound_null.c   # sound stub (LCD builds)
src/i_sound_dvi.cpp   # HDMI build: 8-voice SFX mixer -> HDMI audio ring, mixes in music
src/mus_player.cpp/.hpp  # C-style wrappers (mus_start/render/...) over pico_toolset::MusPlayer (the MUS parser + 10-voice chip-tune synth now live in Pico-Toolset's audio_synth)
doom/i_net_null.c     # net stub
docs/                 # PLAN.md + Waveshare product docs (saved pages)
build/                # RP2350 LCD (ILI9486) build; also build-st7796/ (-DPICODOOM_VIDEO_OUTPUT=lcd-st7796) (rm -rf safe, see Build & firmware above)
build-hdmi/           # RP2350 HDMI build (rm -rf safe; -DPICODOOM_VIDEO_OUTPUT=hdmi)
```