PicoDoom
--------

# Introduction

A port of the classic id Software Linux DOOM 1.10 source (`doom/`) to bare metal on
a [Waveshare RP2350-PiZero](https://www.waveshare.com/rp2350-pizero.htm) board:
RP2350B, 16MB flash, 8MB PSRAM, PIO-USB, µSD card, and a 3.5" ILI9486 LCD (480x320,
DOOM rendered 1:1 centered in a black border).

- **Video**: DOOM's 320x200 palette-indexed framebuffer is converted to RGB565 and
  fed to the LCD over SPI1/DMA, on core1, in parallel with core0's game loop.
- **Input**: USB keyboard + mouse over PIO-USB (GPIO28/29), independent of the
  native USB port (which stays a CDC serial console for boot/diagnostic output).
- **Storage**: WADs, config, and savegames live on a µSD card (FatFs over
  PIO-SPI), loaded through a newlib syscall shim so the engine's original
  `fopen`/`open`/`read` calls work unmodified.
- **Memory**: DOOM's zone heap and screen buffers live in PSRAM (RP2350's ~520KB
  SRAM can't hold DOOM's usual working set); hot per-frame data stays in SRAM
  where it matters for speed.

See [docs/PLAN.md](docs/PLAN.md) for the full phase-by-phase build log
(bring-up, video, input, performance/memory tuning — including every bug found
along the way and why) and [AGENTS.md](AGENTS.md) for a technical map of the
codebase, aimed at whoever (human or AI) picks this up next.

# Status

Boots, loads a WAD from the µSD card, and runs a fully playable game: rendering,
USB keyboard + mouse input, and savegames all work on real hardware. Sound is not
implemented yet (no audio output wired up — see docs/PLAN.md's Phase 4 notes).
Performance is a work in progress: current builds run somewhere in the 10-15fps
range depending on scene complexity, short of the original 35Hz target.

# Compilation

Needs:
- [pico-sdk](https://github.com/raspberrypi/pico-sdk) 2.3.0+
- `arm-none-eabi-gcc` toolchain

```bash
cmake -S . -B build -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build -j$(nproc)
```

`PICO_BOARD`/`PICO_BOARD_HEADER_DIRS` default to this project's own
`boards/waveshare_rp2350_pizero.h` in `CMakeLists.txt`, so no extra flags are
needed for the target board.

# Installation

- Copy `build/PicoDoom.uf2` to the board in BOOTSEL mode, or via picotool:
```bash
picotool load -x -f build/PicoDoom.uf2
```
- Insert a µSD card (FAT32) with a DOOM IWAD (`doom.wad`, `doom1.wad`,
  `doom2.wad`, ...) at its root.
- Plug in a USB keyboard (and optionally a mouse) via the board's PIO-USB port.
- Power on — boot/diagnostic messages are available over the native USB port as
  a serial console (115200 8N1, or just any terminal — no baud negotiation
  needed over USB CDC).

# Controls

Standard vanilla DOOM defaults: arrow keys to move/turn, Space to use
(open doors, flip switches), Ctrl to fire, Shift to run. Mouse look/movement
works when a mouse is connected. A few Pico-specific additions:
- **F12**: toggle mouse input on/off (on by default if a mouse is detected).
- **F11 / F10**: increase / decrease mouse sensitivity.

# License

`doom/` is id Software's original DOOM source, distributed under the terms of
the DOOM Source Code License (see license headers in that directory). Everything
else (`src/`, `boards/`, build files) has no license header yet — treat as
all-rights-reserved pending an explicit choice.
