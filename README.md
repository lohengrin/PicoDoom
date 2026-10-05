PicoDoom
--------

# Introduction

A port of the classic id Software Linux DOOM 1.10 source (`doom/`) to bare metal on
a [Waveshare RP2350-PiZero](https://www.waveshare.com/rp2350-pizero.htm) board:
RP2350B, 16MB flash, 8MB PSRAM, PIO-USB, µSD card, and a choice of output:

| Build (`-DPICODOOM_VIDEO_OUTPUT=`) | Output | Game resolution | Audio |
|---|---|---|---|
| `lcd` (default) | 3.5" ILI9486 SPI LCD (480x320) | 320x200 or 480x300 (boot-menu choice) | none |
| `lcd-st7796` | ST7796U SPI LCD (480x320), 132MHz SPI | 320x200 or 480x300 | none |
| `hdmi` | onboard DVI/HDMI connector (640x480) | 320x200 | SFX + music over HDMI |

- **Video (LCD)**: the palette-indexed framebuffer lives in SRAM, is converted to
  RGB565 in bands and fed to the panel over SPI/DMA by core1, in parallel with
  core0's game loop. Bands that didn't change since the last frame are skipped.
- **Video + audio (HDMI)**: core1 TMDS-encodes the frame; SFX (8-voice PCM mixer)
  and music (MUS parser + chip-tune synth, not the original GM/OPL sound) go out
  as HDMI data-island digital audio on the same cable.
- **Input**: USB keyboard, mouse and gamepad over PIO-USB (GPIO28/29), independent
  of the native USB port (which stays a CDC serial console for diagnostics and a
  small tuning console).
- **Boot menu**: an LVGL screen lists every IWAD on the µSD card (touch, mouse,
  keyboard or gamepad), remembers the last pick and auto-starts it after 30s. The
  LCD builds also offer a *High res* (480x300) checkbox.
- **Storage**: WADs, config, and savegames live on a µSD card (FatFs over
  PIO-SPI), loaded through a newlib syscall shim so the engine's original
  `fopen`/`open`/`read` calls work unmodified.
- **Memory**: DOOM's zone heap lives in PSRAM (RP2350's ~520KB SRAM can't hold
  DOOM's usual working set); the frame buffer, the hottest renderer tables and the
  hot renderer code stay in SRAM where it matters for speed.

Shared hardware drivers (PSRAM, µSD, ILI9486/ST7796, USB HID, DVI/HDMI, fault
handler) come from the [Pico-Toolset](https://github.com/lohengrin/Pico-Toolset)
git submodule.

See [docs/PLAN.md](docs/PLAN.md) and [docs/HDMI_PLAN.md](docs/HDMI_PLAN.md) for the
phase-by-phase build log (including every bug found along the way and why),
[PROFILING.md](PROFILING.md) for the profiling tools, and [AGENTS.md](AGENTS.md)
for a technical map of the codebase, aimed at whoever (human or AI) picks this up next.

# Status

Fully playable on real hardware on all three builds: rendering, keyboard/mouse/
gamepad input, savegames, and (HDMI) sound effects and music.

Performance, `lcd-st7796` at 480x300: about 33 FPS while moving, the 35Hz tick cap
when standing still. The `lcd` (ILI9486) build is limited by its slower SPI bus (about 12 FPS at
480x300 when last measured, before the recent optimizations); 320x200 is faster. HDMI renders at 320x200.
Music is a simple chip-tune synth, not the original General-MIDI/OPL sound.

# Compilation

Needs:
- [pico-sdk](https://github.com/raspberrypi/pico-sdk) 2.3.0+
- `arm-none-eabi-gcc` toolchain

```bash
git clone --recurse-submodules https://github.com/lohengrin/PicoDoom.git
cd PicoDoom
cmake -S . -B build -DPICO_SDK_PATH=/path/to/pico-sdk                 # ILI9486 LCD
cmake -S . -B build-st7796 -DPICO_SDK_PATH=/path/to/pico-sdk -DPICODOOM_VIDEO_OUTPUT=lcd-st7796
cmake -S . -B build-hdmi -DPICO_SDK_PATH=/path/to/pico-sdk -DPICODOOM_VIDEO_OUTPUT=hdmi
cmake --build build -j$(nproc)
```

(`git submodule update --init` if you cloned without `--recurse-submodules`.)
The board header is picked up from the submodule's `boards/` directory, so no extra
flags are needed for the target board.

`cmake --build <dir> --target install` copies the images to `install/`, renamed
after the variant (e.g. `install/PicoDoom-lcd-st7796-release.uf2`) so all three can
sit side by side.

# Installation

- Copy the `.uf2` to the board in BOOTSEL mode, or via picotool:
```bash
picotool load -x -f build/PicoDoom.uf2
```
- Insert a µSD card (FAT32) with one or more DOOM IWADs (`doom.wad`, `doom1.wad`,
  `doom2.wad`, ...) at its root.
- Plug in a USB keyboard (and optionally a mouse or gamepad) via the board's PIO-USB port.
- Power on — pick a WAD in the boot menu (or wait 30s for the last pick). Boot and
  diagnostic messages are available over the native USB port as a serial console
  (115200 8N1, or any terminal — USB CDC needs no baud negotiation).

# Controls

Standard vanilla DOOM defaults: arrow keys to move/turn, Space to use
(open doors, flip switches), Ctrl to fire, Shift to run, all F-keys as in vanilla
(help, save/load, volume, quit, gamma, ...). Mouse look and gamepads work when
connected; the gamepad drives the menus too.

Pico-specific tuning lives in the serial console (type `help`):
- `gamma [up|down|<0.1-10>]` — brightness
- `pclk [up|down|<hz>]` — SPI pixel clock (LCD builds only)
- `mouse [on|off]`, `sens [up|down|<0-9>]` — mouse input and sensitivity

# License

`doom/` is id Software's original DOOM source, distributed under the terms of
the DOOM Source Code License (see license headers in that directory). Everything
else (`src/`, `boards/`, build files) has no license header yet — treat as
all-rights-reserved pending an explicit choice.
