# Pimoroni Presto variant

(Technical reference for the `presto` build; the project map is in [AGENTS.md](../AGENTS.md).)

`cmake -S . -B build-presto -DPICODOOM_VIDEO_OUTPUT=presto [-DPICODOOM_PRESTO_MODE=scanline|palette]
[-DPICODOOM_PRESTO_BT_CLASSIC=ON|OFF]` (board `pimoroni_presto` from the SDK, defines `PICODOOM_PRESTO`,
`PICODOOM_AUDIO`, `PICO_FLASH_ASSUME_CORE1_SAFE`; `PICODOOM_PRESTO_PALETTE` in palette mode). Install
name `PicoDoom-presto-<mode>-<type>`. Hardware: 480x480 ST7701 RGB-parallel panel, FT6236 touch, 8 MB
PSRAM, uSD (SPI0), piezo GPIO43, QW/ST I2C0 (QwSTPad 0x21), CYW43 radio used for **Bluetooth only**.
`-DPICODOOM_PRESTO_SYSCLK_MHZ=` (default 264, hardware-verified, 35 FPS; 150 = SDK stock: flash QMI div 2, PSRAM at clk_sys/2 = 132 MHz, CYW43 PIO SPI divider `ceil(sys/75)`; 150 = SDK stock with PSRAM at the toolset's 50 MHz). If a board won't boot or PSRAM fails its self-test, rebuild with 200 or 150.

- **Video** (`src/i_video_presto.cpp`): the game fills the top **480x300** (full width), the lower
  180 lines are a black band (`St7701Config::visible_height`, no memory behind it). 320x200 is scaled
  1.5x nearest, 480x300 is native ("High res" boot checkbox, as on the LCD builds; default low-res =
  tear-free). The RGB bus has no GRAM, so the panel reads SRAM continuously (PSRAM cannot feed it):
  - `scanline` (default): `Scanline565`, no framebuffer; core1 runs `scanline_task()` and the toolset's
    `St7701IndexedSource` (RAM callback) converts one source line per panel line through its 256-entry RGB565 LUT. Low-res reads
    a 64 KB copy of `screens[0]` taken in `I_FinishUpdate` (tears only during the ~0.2 ms copy);
    hi-res scans a full 480x300 copy of `screens[0]` made each frame (allocated at the first level frame, after `screens[0]`, from the contiguous heap top; if SRAM is ever short it falls back to the bottom-anchored 3D-view rows and scans the rest live). That copy needs the 40 KB freed by relocating `states[]`/`mobjinfo[]` to PSRAM (`PICODOOM_PRESTO` in `doom/info.c`, `I_RelocateInfoTables()` called from `main()`; they are written only by G_Fast parms). Watch `late-lines`/`missed-frames` in the stats line.
  - `palette`: `Palette8`, 480x300 byte framebuffer (144 KB) + the panel PIO's hardware palette
    (`I_SetPalette` -> `set_palette_colour`). Hi-res: with the SRAM freed by the `states[]`/`mobjinfo[]` relocation, `screens[0]` is a separate 144 KB buffer copied into the framebuffer once per frame (tear-free but for the ~0.3 ms copy); only if the heap is too short does it fall back to `screens[0]` *being* the framebuffer (V_Init hook `i_video_presto_screen0`; zero copy, tears); low-res expands 1.5x into it per frame. The black band shows palette
    entry 0, so pain/pickup flashes tint it.
  - A full RGB565 framebuffer (288-450 KB) does not fit next to the engine; not offered.
  - core1 only serves scanout (IRQs + `scanline_task`), all from RAM -- required because Bluetooth
    bonding erases/programs flash from core0 (`PICO_FLASH_ASSUME_CORE1_SAFE=1`, no
    multicore_lockout, see the toolset AGENTS.md). Never put flash/PSRAM-touching code on core1.
  - The LVGL boot menu renders (core0) into an RGB332 480x300 surface scanned out like the game
    (`lvgl_display_presto.cpp`): the palette-mode framebuffer, or a temporary 144 KB buffer in
    scanline mode that `i_video_lcd_set_hires()` frees before the engine allocates `screens[0]`.
- **Beam-synchronised copies** (deferred): every per-frame copy (scanline low/hi-res, palette double buffer / 1.5x expand) runs only while `St7701::display_row()` >= 300, i.e. the beam is on the black band/porches where nothing is read from the buffer; a top-to-bottom copy started there stays ahead of the next frame's beam, so there is no seam (~40% of each ~8 ms frame is such a window; refresh = dot clock / 264k, dot clock = clk_sys/even divider, cap 34 MHz). `I_FinishUpdate()` only *requests* the copy; `presto_video_poll()` (called first thing in `I_StartTic()`, which the engine polls in a tight loop while idling for the next tic) performs it when the beam gets there, and `presto_video_flush()` (top of `D_Display()`, the only place that draws; `doom/d_main.c` shim) blocks if it still is not done. Stats field `beam-sync-wait` = time spent in that blocking wait (should now be small).
- **Input**: `HidSource` (Pico-Toolset `pico_toolset/hid_source.h`, re-exported by `src/hid_source.hpp`, with `UsbHidSource` / `BluetoothHidSource` adapters) is the backend-neutral input view; shared event
  posting is `src/i_input_tic.cpp` (`I_StartTic`), backends are `i_input_usbhid.cpp` (PiZero) and
  `i_input_presto.cpp` (Bluetooth HID via `pico_toolset_bluetooth_hid` + QwSTPad D-pad/A/B/X/Y/+/-;
  QwSTPad is pad 0 and ORs into a Bluetooth pad). Pad Start (QwSTPad +) posts Esc, Back (-) posts Tab,
  on every backend. Touch = FT6236 indev for the boot menu, which also has Pair BT / Forget buttons
  and a status line (passkey for keyboards). Bonded devices reconnect by themselves; pairing starts
  automatically when nothing is bonded. Gamepad button order for Bluetooth pads is a guess upstream
  (`gamepad_raw_buttons()` shows the real usages).
- **Audio** (`src/presto_mixer.cpp`, `src/i_sound_piezo.cpp`): one shared mono mixer (toolset `SampleMixer<8>` for the 8 DMX voices with
  linear interpolation/pitch/volume + the MUS synth `mus_render()`), rendered on demand by whichever output is
  active. **Piezo**: Pico-Toolset's `PiezoPcm` (DMA-paced PWM duty cycle on GPIO43, 44.1 kHz) fed by a 5 ms
  repeating timer (interrupt context, so long game frames cannot starve it) that keeps ~18 ms queued; the output
  pauses (pin low) when nothing plays. **Headset**: the A2DP source callback renders the same mixer (below); the
  piezo timer steps aside while streaming. `render()` has a try-lock so overlapping contexts give silence rather
  than corruption; game-thread voice/`mus_*` access is inside `pico_toolset::IrqLock`. Fallback if `PiezoPcm`
  cannot start (no free DMA channel/timer): the original single-voice tone approximation (per-sound contour of
  25 ms windows of zero-crossing pitch + loudness, music as the MUS sequencer's lead voice via `mus_step`/
  `mus_lead`). Production is paced at the sample rate from the 5 ms timer (PiezoPcm only learns the DMA read position every 10 ms and wipes the ring on an underrun, so topping up from `queued()` loses audio). Level: the piezo is nearly deaf below ~500 Hz and DOOM's audio is bass-heavy, so the timer applies a first-difference treble boost (`piezo pre`, default on) then `g_pcm_gain` (default 50; 100 verified audible: heavy clipping, rough -- smarter processing, e.g. band-pass before the clip, is a later idea) with a hard clip. Console: `piezo beep [hz [ms]]` (full-scale square through the same path -- note it ignores gain/pre), `piezo gain [n]`, `piezo pre [on|off]`, `piezo diag on|off`. `doom/s_sound.c` PICO stubs are gated on `PICODOOM_AUDIO` (hdmi + presto). Muted at start;
  BOOTSEL toggles (`pico_toolset::bootsel_pressed()`), a streaming headset unmutes.
- **Bluetooth audio (A2DP)** (`src/presto_a2dp.cpp`, `-DPICODOOM_PRESTO_A2DP=ON` default): when a headset is
  *streaming*, `i_sound_piezo.cpp` stops the piezo and the real audio goes there. There is **no buffer**: the SBC
  encoder asks `DoomSource::fill()` (BTstack context, preempts the game thread) for each frame of PCM and `fill()`
  renders the shared mixer on the spot plus the connect chime. (An earlier design pumped a PSRAM ring once per frame from `I_SubmitSound()`; any frame longer
  than the ring's ~58 ms starved it -> audible cuts, and it added that latency.) Game-thread access to voices and
  to every `mus_*` call is wrapped in a short interrupt-off `IrqLock` so the Bluetooth context never sees a
  half-updated state; mixing buffers are static (the BT context runs on core 0's 4 KB stack). BOOTSEL mute =
  `presto_mixer_set_enabled(false)` (silence, music not rendered). SBC preset `Mono` (154 kbps). The last headset
  reconnects after boot; the boot menu has Pair/Forget buttons for pad/keyboard and for audio and a combined status
  line. A headset starting to stream unmutes. Costs ~36 KB SRAM (AVDTP/AVRCP pools, 1 KB ACL buffers,
  `PICO_STACK_SIZE=0x1000`; core1 stack 6 KB; `states[]`/`mobjinfo[]` relocation pays it back, heap ~315 KB).
  Bluetooth mice work through the same HID path (motion, buttons, wheel = weapon cycle; the menu cursor shows only
  while a mouse is present). Console diagnostics every 5 s while streaming (`PicoDoom: audio: ...`).
- **LEDs** (`src/i_leds_presto.cpp`, 7 WS2812 on PIO0 SM3): health gradient green(>=100%)->orange(50%)->red(0%) at 25% brightness; 450 ms flashes red (health down), green (health up), blue (any other gain: ammo/armour/weapon/key/power-up). Driven by diffing `players[consoleplayer]` from `i_input_backend_poll()` -- no engine edits; dark outside a level. `i_leds_init()` must run before `cyw43_arch_init()` (PIO0 gpio_base 16).
- **SRAM** (520 KB): engine statics ~215 KB incl. Bluetooth (~18 KB without lwIP/mbedtls, thanks to
  `PICO_TOOLSET_BLUETOOTH_NO_WIFI`) -> ~300 KB heap. No blit slots here (the RGB bus needs none).
- **Hardware checkpoints** (verify in order, per feedback_phased_hardware_rollout): 1) menu appears,
  touch works, both modes; 2) E1M1 renders in lo/hi res, record FPS/late-lines; 3) QwSTPad plays,
  BLE keyboard + Classic gamepad pair/reconnect, no hang during bonding; 4) piezo SFX/music.

## Safety nets kept on purpose
Two fallbacks are rarely taken now that the heap is large (the boot log says which path was chosen), but they keep the
game running instead of crashing if a future change eats SRAM, so they stay:
- Scanline hi-res: if the heap cannot hold the full 480x300 display copy, the copy covers the bottom of the 3D view and the
  rest is scanned live (`try_alloc_head_copy()` logs "tear-free copy of N rows ...").
- Palette hi-res: if a second 144 KB buffer does not fit, `screens[0]` *is* the framebuffer (zero copy, tears;
  logged "SRAM short, rendering straight into the framebuffer").
- Piezo: if `PiezoPcm` cannot start (no free DMA channel/timer) the old tone-contour output is used.

## Where the reusable parts live
Pico-Toolset: `St7701IndexedSource` + `st7701_indexed_rows.h` (indexed frame -> scanline source), `SampleMixer`,
`MusPlayer`/`MusSource`, `HidSource` with USB/Bluetooth adapters, `qwstpad_gamepad_state()`/`merge()`, `PiezoPcm`,
`rp_util` (`IrqLock`, `bootsel_pressed()`, SRAM headroom). PicoDoom keeps only glue: `presto_mixer.cpp`, `presto_a2dp.cpp`,
`i_sound_piezo.cpp`, `i_input_presto.cpp`, `i_leds_presto.cpp`, `i_video_presto.cpp`, `menu_board_presto.cpp`.

