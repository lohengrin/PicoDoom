PicoDoom
--------

# Introduction

A port of the classic id Software Linux DOOM 1.10 source (`doom/`) to bare metal on
two RP2350B boards, both with 16MB flash, 8MB PSRAM and a µSD card:

- the [Waveshare RP2350-PiZero](https://www.waveshare.com/rp2350-pizero.htm) (PIO-USB
  input) with a choice of output, and
- the [Pimoroni Presto](https://shop.pimoroni.com/products/presto) (built-in 480x480
  display, touch, Bluetooth input and audio, piezo, LEDs).

| Build (`-DPICODOOM_VIDEO_OUTPUT=`) | Output | Game resolution | Audio |
|---|---|---|---|
| `lcd` (default) | 3.5" ILI9486 SPI LCD (480x320) | 320x200 or 480x300 (boot-menu choice) | none |
| `lcd-st7796` | ST7796U SPI LCD (480x320), 132MHz SPI | 320x200 or 480x300 | none |
| `hdmi` | onboard DVI/HDMI connector (640x480) | 320x200 | SFX + music over HDMI |
| `presto` | Presto's 480x480 ST7701 panel (game in the top 480x300, black band below) | 320x200 scaled 1.5x, or native 480x300 | piezo, or SFX + music to a Bluetooth headset |

- **Video (LCD)**: the palette-indexed framebuffer lives in SRAM, is converted to
  RGB565 in bands and fed to the panel over SPI/DMA by core1, in parallel with
  core0's game loop. Bands that didn't change since the last frame are skipped.
- **Video + audio (HDMI)**: core1 TMDS-encodes the frame; SFX (8-voice PCM mixer)
  and music (MUS parser + chip-tune synth, not the original GM/OPL sound) go out
  as HDMI data-island digital audio on the same cable.
- **Video (Presto)**: the RGB panel has no memory of its own, so it is scanned out of
  SRAM continuously by PIO + DMA (core1 only serves that scanout). Two build-time modes:
  `scanline` (default, no framebuffer: core1 converts one line at a time just ahead of
  the beam through a palette LUT) and `palette` (480x300 8-bit framebuffer expanded by
  the panel PIO). Both are tear-free: the frame is copied into the scanned buffer only
  while the beam is in the black band below the game area.
- **Audio (Presto)**: the piezo plays SFX as pitch contours and a lead-voice version of
  the music (muted by default; the **BOOTSEL** button mutes/unmutes). When a Bluetooth
  headset is connected (A2DP) the piezo stays off and the real SFX and music play there,
  mixed on demand inside the Bluetooth stream callback (no buffering delay); a headset
  starting to stream unmutes. The 7 addressable LEDs show your health (green to orange
  to red) and flash red when hit, green on health pickups and blue on other pickups.
- **Input (PiZero)**: USB keyboard, mouse and gamepad over PIO-USB (GPIO28/29),
  independent of the native USB port (which stays a CDC serial console for diagnostics
  and a small tuning console).
- **Input (Presto)**: Bluetooth keyboard, mouse and gamepad (BLE and Classic, bonded and
  reconnected automatically), a Pimoroni QwSTPad on the QW/ST connector (D-pad, A/B/X/Y,
  `+` = menu, `-` = automap), and the touch screen in the boot menu.
- **Boot menu**: an LVGL screen lists every IWAD on the µSD card (touch, mouse,
  keyboard or gamepad), remembers the last pick and auto-starts it after 30s. The
  LCD and Presto builds also offer a *High res* (480x300) checkbox. On the Presto the menu
  also has buttons to pair or forget Bluetooth pads/keyboards and a Bluetooth headset,
  and shows the pairing passkey for keyboards.
- **Storage**: WADs, config, and savegames live on a µSD card (FatFs over
  PIO-SPI), loaded through a newlib syscall shim so the engine's original
  `fopen`/`open`/`read` calls work unmodified.
- **Memory**: DOOM's zone heap lives in PSRAM (RP2350's ~520KB SRAM can't hold
  DOOM's usual working set); the frame buffer, the hottest renderer tables and the
  hot renderer code stay in SRAM where it matters for speed.

Shared hardware drivers (PSRAM, µSD, ILI9486/ST7796/ST7701, FT6236, USB HID,
Bluetooth HID/A2DP, QwSTPad, piezo, WS2812, DVI/HDMI, fault handler) come from the [Pico-Toolset](https://github.com/lohengrin/Pico-Toolset)
git submodule.

See [docs/PRESTO.md](docs/PRESTO.md) and [docs/SPI_LCD.md](docs/SPI_LCD.md) for the Presto and SPI LCD
technical references, [docs/PLAN.md](docs/PLAN.md) and [docs/HDMI_PLAN.md](docs/HDMI_PLAN.md) for the
phase-by-phase build log (including every bug found along the way and why),
[PROFILING.md](PROFILING.md) for the profiling tools, and [AGENTS.md](AGENTS.md)
for a technical map of the codebase, aimed at whoever (human or AI) picks this up next.

# Status

Fully playable on real hardware on all four builds: rendering, keyboard/mouse/
gamepad input, savegames, and (HDMI, Presto) sound effects and music.

Performance, `lcd-st7796` at 480x300: about 33 FPS while moving, the 35Hz tick cap
when standing still. The Presto build runs at the 35Hz cap at 264MHz in both panel modes
(SRAM is tight there: the engine's state and monster tables live in PSRAM to leave room
for the full-frame display copy). The `lcd` (ILI9486) build is limited by its slower SPI bus (about 12 FPS at
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
cmake -S . -B build-presto -DPICO_SDK_PATH=/path/to/pico-sdk -DPICODOOM_VIDEO_OUTPUT=presto
cmake --build build -j$(nproc)
```

Presto options (all `-D...`): `PICODOOM_PRESTO_MODE=scanline|palette` (panel feed, default
`scanline`), `PICODOOM_PRESTO_SYSCLK_MHZ` (default 264; use 150 for the SDK stock clock if
a board will not boot), `PICODOOM_PRESTO_A2DP=ON|OFF` (Bluetooth headset audio, default ON;
costs ~36KB of SRAM), `PICODOOM_PRESTO_BT_CLASSIC=ON|OFF` (older Bluetooth pads/keyboards,
default ON; A2DP needs it). The Presto build uses the board definition shipped with
pico-sdk 2.3+ (`pimoroni_presto`).

(`git submodule update --init` if you cloned without `--recurse-submodules`.)
The board header is picked up from the submodule's `boards/` directory, so no extra
flags are needed for the target board.

`cmake --build <dir> --target install` copies the images to `install/`, renamed
after the variant (e.g. `install/PicoDoom-lcd-st7796-release.uf2`, or
`install/PicoDoom-presto-scanline-release.uf2`) so every build can sit side by side.

# Installation

- Copy the `.uf2` to the board in BOOTSEL mode, or via picotool:
```bash
picotool load -x -f build/PicoDoom.uf2
```
- Insert a µSD card (FAT32) with one or more DOOM IWADs (`doom.wad`, `doom1.wad`,
  `doom2.wad`, ...) at its root.
- PiZero: plug in a USB keyboard (and optionally a mouse or gamepad) via the board's
  PIO-USB port.
- Presto: nothing to plug in. With nothing paired, the board starts Bluetooth pairing by
  itself: put your keyboard/gamepad/mouse in pairing mode (a keyboard shows a passkey in
  the boot menu: type it on the keyboard and press Enter). For a headset use *Pair audio*
  in the boot menu with the headset in pairing mode; it reconnects on later boots. Pairings are stored in a flash
  area shared with other Pico-Toolset applications (they survive reflashing, and a device paired in another toolset
  application for the Presto works here without pairing again); see docs/PRESTO.md.
- Power on — pick a WAD in the boot menu (or wait 30s for the last pick). Boot and
  diagnostic messages are available over the native USB port as a serial console
  (115200 8N1, or any terminal — USB CDC needs no baud negotiation).

# Controls

Standard vanilla DOOM defaults: arrow keys to move/turn, Space to use
(open doors, flip switches), Ctrl to fire, Shift to run, all F-keys as in vanilla
(help, save/load, volume, quit, gamma, ...). Mouse look and gamepads work when
connected; the gamepad drives the menus too.

Presto: a QwSTPad gives D-pad movement and A/B/X/Y as fire/strafe/run/use, `+` opens the
menu (Esc) and `-` toggles the automap (Tab); Bluetooth gamepads (same mapping, Start/Back for
menu/automap), keyboards and mice work as above. Sound is muted at start: press BOOTSEL to
toggle it (connecting a headset unmutes).

Pico-specific tuning lives in the serial console (type `help`):
- `gamma [up|down|<0.1-10>]` — brightness
- `mouse [on|off]`, `sens [up|down|<0-9>]` — mouse input and sensitivity

# License

`doom/` is id Software's original DOOM source, distributed under the terms of
the DOOM Source Code License (see license headers in that directory). Everything
else (`src/`, `boards/`, build files) has no license header yet — treat as
all-rights-reserved pending an explicit choice.
