# PLAN — PicoDoom HDMI + audio output variant

Goal: a second, selectable PicoDoom build (`-DPICODOOM_VIDEO_OUTPUT=hdmi`)
that drives the game out the Waveshare RP2350-PiZero's onboard DVI/HDMI
TMDS connector instead of the external ILI9486 SPI LCD, with full DOOM
sound (SFX + music) riding the same cable via HDMI data-island digital
audio. The default LCD build (`-DPICODOOM_VIDEO_OUTPUT=lcd`, or no flag at
all) stays byte-for-byte unchanged.

Uses Pico-Toolset's `pico_toolset_dvi_hdmi` component (PIO-based DVI/TMDS
serialiser + optional HDMI digital audio), ported from the sibling project
TOM6809's `PicoDviVideoOutput`/`PicoHdmiAudioOutput` (same board, same
library, real-hardware-validated there) rather than from that component's
own never-hardware-tested example. See
[[project-pico-toolset-migration]]/[[feedback-phased-hardware-rollout]] for
the general cross-repo hardware-rollout discipline this follows.

Rolled out in three hardware-verify-then-proceed phases, same discipline as
`docs/PLAN.md`'s own phase history:

## Phase A — video only (current)

`src/i_video_dvi.cpp` implements `doom/i_video.h` against the onboard TMDS
connector: a persistent 320x240 RGB565 framebuffer (`g_framebuf`, 153600
bytes) that core1 re-encodes continuously at the DVI's own 60Hz via the
library's standard (pixel-doubling) 16bpp TMDS encoder —
`tmds_encode_data_channel_16bpp()` with `n_pix=pixwidth/2`, the same
n_pix convention `dvi.c`'s own scanbuf path uses, so DOOM's native 320-wide
picture needs no explicit horizontal-repeat: the encoder doubles it to 640
physical columns itself. Vertical 2x is the library's own
`DVI_VERTICAL_REPEAT`. `I_FinishUpdate()` converts `screens[0]`
(palette-indexed, never relocated — same invariant
`src/i_video_ili9486.cpp` documents) into `g_framebuf` every tic; core1
free-runs independently, so a slow tic tears a frame rather than wedging
the display (the failure mode of feeding the library's hard-real-time
`dvi_scanbuf_main_16bpp()` once-per-tic instead, which TOM6809 already hit
and fixed this way).

**Framebuffer lives in plain SRAM (`.bss`), matching TOM6809 exactly** —
getting there took two wrong turns, both real-hardware-tested:

1. First attempt put it in SRAM verbatim (as above) and panicked with
   `PICO_MALLOC_PANIC`'s "Out of memory" while loading `doom.wad`: this
   board's ~512KB SRAM only has ~172KB free after DOOM's static tables
   (see `doom/i_system.c`'s comment on why the zone heap is PSRAM-backed),
   and the 153600-byte framebuffer plus core1's stack ate nearly all of
   that margin, starving `doom/w_wad.c`'s WAD-directory `malloc()` and its
   permanent `lumpinfo`/`lumpcache` tables.
2. Second attempt moved the framebuffer to PSRAM instead (8MB, effectively
   unconstrained), with core1 DMA-copying each row into a tiny SRAM scratch
   buffer before TMDS-encoding it (sidestepping PSRAM's XIP-cache
   cross-core-coherency question for CPU reads, the same reasoning
   `src/i_video_ili9486.cpp`'s own PSRAM screen buffers rely on — that
   driver's core1 only ever touches PSRAM via DMA too). This produced no
   HDMI signal at all on real hardware; whether the DMA scheme itself was
   the cause was never isolated.

Fixed properly instead, at the actual source of the SRAM pressure:
`doom/w_wad.c`'s own large PICO-path allocations (the WAD-directory buffer,
and the permanent `lumpinfo`/`lumpcache` tables — tens of KB, competing
directly with this driver's framebuffer for the same ~172KB margin) now go
to PSRAM via `psram_malloc`/`psram_free` (plus a small local
`w_psram_realloc` helper for `lumpinfo`'s incremental growth), mirroring
the zone heap's existing precedent. That frees enough SRAM for the
framebuffer to stay there directly, at full RGB565 quality, via the same
simple CPU-load pattern TOM6809 already has proven on this exact hardware
— no DMA indirection, no PSRAM-cache question to even raise. core1's stack
also shrank from TOM6809's usual 16KB to 4KB (it only runs `encode_row()`'s
small call chain + libdvi's own IRQ handler, not a USB host stack).

`src/i_input_usbhid.cpp`'s USB-PIO HID host stack moves from core1 to
core0 for this build (`PICODOOM_HDMI`, `pico_toolset::configs::usb_hid::
kWaveshareRp2350PiZeroHdmi`) — core1 belongs exclusively to the DVI encode
loop; libdvi's per-scanline DMA IRQ can't share a core with Pico-PIO-USB's
SOF-timer IRQ (confirmed on real hardware in TOM6809: no picture at all).
`UsbHidHost::task()` is polled once per tic from `I_StartTic()` instead.

Sound stays the existing `doom/i_sound_null.c` no-op stub for this phase —
`PICO_TOOLSET_DVI_HDMI_AUDIO` is OFF.

**`src/PicoDoom.cpp`'s clk_sys differs per build** — the HDMI build sets
`clk_sys` to exactly 252MHz (`vreg_set_voltage(VREG_VOLTAGE_1_20)`,
`set_sys_clock_khz(252'000, true)`), not the LCD build's 200MHz. A real
hardware test without this fix produced no video signal at all: libdvi's
PIO/TMDS serialiser derives its bit clock directly from whatever `clk_sys`
is at `dvi_init()` time, and `dvi_timing_640x480p_60hz`'s own comment says
"we do this mode properly, with a pretty comfortable clk_sys (252 MHz)" —
confirmed as a hard requirement (not just a suggestion) via TOM6809's
real-hardware history on this same board/library. 252MHz conveniently also
satisfies Pico-PIO-USB's separate requirement (an exact multiple of 12MHz)
for this build's core0-hosted USB-PIO stack, so there's no conflict between
the two clock-sensitive subsystems this build combines.

**Status: hardware-verified 2026-09-13.** Picture confirmed present over
HDMI on real hardware, game loop running (~20-27fps), USB
keyboard/mouse/gamepad detected. Took three real-hardware round-trips to
get here: "Out of memory" (SRAM exhaustion, fixed by moving
`doom/w_wad.c`'s allocations to PSRAM), then "no signal" (wrong `clk_sys`,
fixed to 252MHz), then "no signal" again (the PSRAM+DMA framebuffer
detour, reverted to plain SRAM) — all three fixes landed together above.
Not yet separately re-confirmed: the unmodified `lcd` build still works
unregressed (the `doom/w_wad.c` PSRAM change is shared between both
variants) — worth a quick flash-and-check before moving on, since nothing
in this phase exercised that path on real hardware.

## Phase B — SFX (implemented, not yet hardware-verified)

`src/i_sound_dvi.cpp` implements `doom/i_sound.h`'s SFX half: an 8-voice
software PCM mixer decoding DMX-format WAD lumps (via `W_CacheLumpNum`,
PSRAM-backed like every other WAD asset), resampled 11025→44100Hz via
linear interpolation, mixed once per tic (1260 samples, `I_UpdateSound()`
— `doom/d_main.c`'s `D_DoomLoop()` already calls this unconditionally every
tic, no change needed there) and pushed into the HDMI data-island audio
ring (`dvi.h`'s `audio_ring_t`, non-blocking, drop-on-full, same ring
`src/i_video_dvi.cpp`'s `dvi_inst` already owns). `PICO_TOOLSET_DVI_HDMI_AUDIO`
is now ON for the `hdmi` build. `doom/s_sound.c`'s `#ifdef PICO` blocks
widened to `#if defined(PICO) && !defined(PICODOOM_HDMI)` (5 of them —
`S_Init`/`S_Start`/`S_StartSoundAtVolume`/`S_StopSound`/`S_UpdateSounds`;
`S_ChangeMusic`'s stays plain `#ifdef PICO`, music is Phase C). Music in
`i_sound_dvi.cpp` itself stays stubbed (same no-ops as `doom/i_sound_null.c`).

**Cross-repo SRAM fix, also needed**: enabling `PICO_TOOLSET_DVI_HDMI_AUDIO`
(audio ring, +8KB SRAM) plus this driver's own small state dropped the
`hdmi` build's free-SRAM margin to ~16KB — uncomfortably close to the
threshold that already panicked once in Phase A. Root cause wasn't new
code here: `pico_toolset_usb_hid`'s `UsbHidHost::init()` (canonical
`~/Dev/Pico-Toolset`, `components/usb_hid/src/usb_hid_host.cpp`) declares
its core1 stack as a function-local `static uint32_t core1_stack[4096]`
inside `if (m_config.run_on_core1)` — C++ local statics get fixed storage
regardless of whether that branch ever runs, so this build (which sets
`run_on_core1=false`, core1 belongs to DVI) was permanently paying 16KB of
SRAM for a stack it never uses. Fixed in the canonical Pico-Toolset clone
(not yet committed there, and not yet ported into PicoDoom's submodule pin
— currently only patched directly in the submodule checkout for local
testing) to `malloc()` that stack only when `run_on_core1` is actually
true. Recovered the `hdmi` build's margin to ~32KB — better than before
enabling audio.

**Real-hardware finding: no audio at all (video still fine).** Root cause:
`I_InitSound()` runs from `doom/i_system.c`'s `I_Init()`, which
`D_DoomMain()` calls well before `D_DoomLoop()` ever reaches
`I_InitGraphics()` (confirmed by line numbers in `doom/d_main.c`: `I_Init()`
~1086, `D_DoomLoop()` ~1125+) — the exact ordering hazard flagged (but not
actually guarded against) during planning. `I_InitSound()` was calling
`dvi_audio_sample_buffer_set()`/`dvi_set_audio_freq()` against a `dvi_inst`
that `dvi_init()` hadn't touched yet. Fixed by deferring that configuration
to first use instead (`ensure_ring_configured()`, called from `mix_tic()`,
which only ever runs from `I_UpdateSound()` — itself only reachable once
`D_DoomLoop()`'s per-tic loop is running, strictly after
`I_InitGraphics()`). No engine-file reordering needed.

**Second real-hardware finding, same symptom (no audio, video/game loop
fine)**: after the ordering fix, `S_StartSoundAtVolume`'s own debug print
(`"16bit and not pre-cached - wtf?"`) confirmed sound events WERE firing —
but still nothing came out. Cause: `doom/doomdef.h` unconditionally
`#define SNDSERV 1` (true for every build, including the original Linux
target) — this compiles out `doom/d_main.c`'s `#ifndef SNDSERV
I_UpdateSound(); #endif` per-tic call entirely, while the neighboring
`#ifndef SNDINTR I_SubmitSound(); #endif` always runs (`SNDINTR` is never
defined). The actual per-tic mix call had been placed in `I_UpdateSound()`
— the function that sounds like it should be the one, but is dead code on
every build of this engine. Moved the mix call to `I_SubmitSound()`
instead; `I_UpdateSound()` is now the no-op.

**Status: hardware-verified 2026-09-13.** SFX confirmed audible on real
hardware after the `I_SubmitSound()` fix. Also silenced
`doom/s_sound.c`'s `S_StartSoundAtVolume` debug print (`"16bit and not
pre-cached - wtf?"`) for this build — it fired on every first play of each
distinct sound effect (this driver's own `load_sfx()` does the real
caching elsewhere; that 1993 dead-code path's print was just noise) and
was flooding the serial console during gameplay.

Not separately re-confirmed on real hardware: the `lcd` build (unaffected
by any Phase B source change; only risk is the shared `usb_hid_host.cpp`
fix, which that build now exercises via a different, but equivalent,
code path).

## Phase C — music (not started)

Add MUS-format lump decode + a simple per-channel square/triangle-wave
synth (not OPL2 FM emulation — explicitly out of scope, a possible future
follow-up) feeding the same mixer.

Verify: recognizable track playback, correct tempo/looping, coexists with
SFX without starving either.
