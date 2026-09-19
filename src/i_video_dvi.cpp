// DVI/HDMI video driver for the Pico port (Phase A of the HDMI+audio
// variant, see docs/HDMI_PLAN.md): drives DOOM's 320x200 palette-indexed
// screens[0] out the Waveshare RP2350-PiZero's onboard TMDS connector via
// Pico-Toolset's pico_toolset_dvi_hdmi, instead of the external ILI9486 SPI
// LCD src/i_video_ili9486.cpp drives. Selected at configure time
// (PICODOOM_VIDEO_OUTPUT=hdmi, CMakeLists.txt) -- mutually exclusive with
// that file, never built together.
//
// Architecture ported from TOM6809's PicoDviVideoOutput (same board, same
// library, real-hardware-validated there -- see
// ~/Dev/TOM6809/src/ui_pico/PicoDviVideoOutput.cpp), NOT from this
// library's own dvi_hdmi_example.cpp or dvi_scanbuf_main_16bpp(): that API
// is a hard-real-time per-scanline feed (an 8-entry q_colour_valid queue),
// and feeding it once per Doom tic (which can exceed its 16.67ms deadline)
// wedges it into a permanent solid-red screen -- exactly the failure
// TOM6809 hit and fixed. Instead: one persistent RGB565 framebuffer in SRAM
// that core1 re-encodes continuously at the DVI's own 60Hz, independent of
// how often core0 actually has a new Doom frame ready, via plain CPU loads
// (no DMA indirection -- see below) -- core0 just writes into it whenever
// I_FinishUpdate() runs; worst case a frame tears, never blanks or wedges.
//
// Framebuffer location: plain SRAM, matching TOM6809's g_framebuf exactly
// -- an earlier revision of this file moved it to PSRAM (read via a
// per-row DMA copy into a small SRAM scratch buffer, to sidestep PSRAM's
// XIP-cache cross-core-coherency question for CPU loads) to solve a real
// SRAM-exhaustion bug this framebuffer caused (see below), but that
// produced no HDMI signal at all on real hardware -- whether from the DMA
// scheme itself or something else was never isolated, and vs. TOM6809's
// exact, already-proven-on-this-board SRAM+CPU-load design, trading a
// known-good pattern for an unverified one wasn't worth it once the real
// SRAM problem had a better fix available (below). Reverted.
//
// The actual SRAM-exhaustion bug: this board's ~512KB SRAM has only
// ~172KB free after DOOM's static tables (see doom/i_system.c's own
// comment on why the zone heap is PSRAM-backed) -- this framebuffer
// (153600 bytes) plus core1's stack ate nearly all of that margin, so
// doom/w_wad.c's WAD-directory/lumpinfo/lumpcache allocations (tens of KB,
// permanent for lumpinfo/lumpcache) failed with pico-sdk's
// PICO_MALLOC_PANIC "Out of memory". Fixed at the actual source instead:
// doom/w_wad.c's own large PICO-path allocations now go to PSRAM
// (psram_malloc, mirroring the zone heap's existing precedent), freeing
// enough SRAM for this framebuffer to live there directly, at full
// quality, the simple way.
//
// Unlike the LCD driver, there is no SPI bus-time reason to minimize bytes
// transferred -- core1 re-scans the whole fixed-size canvas every refresh
// regardless of content. So this scales DOOM's 320x200 up to fill the
// 640x480 signal, rather than reusing the LCD driver's 1:1-centered
// approach. Unlike TOM6809's GPL-family renderer (which needs a genuine
// 640-wide source and so uses the `_fullres_16bpp` 1:1 encoder, doubling
// memory), DOOM's 320-wide picture is exactly this library's designed
// narrow-source case: dvi.c's own scanbuf path
// (_dvi_prepare_scanline_16bpp()) calls the plain (non-fullres)
// tmds_encode_data_channel_16bpp() with n_pix=pixwidth/2, i.e. it already
// expects a 320-element source row and duplicates every pixel to 2 physical
// columns *inside* the TMDS encode itself -- no explicit horizontal-repeat
// loop, and the framebuffer only needs to be 320 wide, not 640 (half the
// SRAM of TOM6809's narrow-family approach, which predates this realization
// and duplicates explicitly at 640 wide). 20 canvas rows (40 physical
// pixels, after DVI_VERTICAL_REPEAT) of top/bottom border, no side border
// -- same border layout TOM6809 settled on, just produced differently.
//
// screens[0] itself is never relocated here either -- same invariant
// src/i_video_ili9486.cpp's header comment documents (R_InitBuffer()'s
// cached row pointers, st_stuff.c's diff-redraw, f_wipe.c's melt effect all
// assume stable address/content across frames) -- just read out every
// I_FinishUpdate() call.
//
// core1 ownership: this file launches core1, unconditionally, the moment
// I_InitGraphics() runs -- moved here from src/i_input_usbhid.cpp, which
// owns core1 in the LCD build instead. Pico-PIO-USB's SOF-timer IRQ cannot
// share a core with this library's per-scanline DMA IRQ (confirmed on real
// hardware in TOM6809: no picture at all) -- so for this build,
// src/i_input_usbhid.cpp's USB-PIO HID host stack runs on core0 instead
// (PICODOOM_HDMI, set from CMakeLists.txt).
//
// clk_sys: src/PicoDoom.cpp's main() sets clk_sys to exactly 252MHz for
// this build (not the LCD build's 200MHz) -- this library's PIO/TMDS
// serialiser derives its bit clock from whatever clk_sys is at
// dvi_init() time, and dvi_timing_640x480p_60hz requires exactly 252MHz
// (see that file's own comment for the full real-hardware-confirmed
// rationale).
#include "dvi.h"
#include "dvi_serialiser.h"
#include "dvi_timing.h"
#include "common_dvi_pin_configs.h"
extern "C" {
// tmds_encode.h has no extern "C" guard of its own; without this wrapper
// these calls mangle as C++ and fail to link against tmds_encode.S.
#include "tmds_encode.h"
}
#include "pico_toolset/psram.h"
#include "i_video_dvi.hpp"
#include "i_frame_stats.hpp"

extern "C" {
#include "doomdef.h"
#include "i_video.h"
#include "i_system.h"
#include "v_video.h"
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>

#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/time.h"

// Linker-provided symbols (pico-sdk's memmap linker script) -- same stats
// use as src/i_video_ili9486.cpp.
extern "C" char __StackLimit;
extern "C" char __bss_end__;

namespace {

// Once-guard: I_InitGraphics() is invoked by both the boot WAD-selection
// menu (Phase 4, src/wad_menu.cpp) and the engine's own startup
// (D_DoomMain() -> I_InitGraphics) -- the second call must be a no-op:
// dvi_init()/core1 relaunch over an already-running encoder would wedge the
// TMDS pipeline (the first core1 is already scanning out g_framebuf).
bool g_graphics_init_done = false;

// Canvas pixel dimensions -- see file header's scaling rationale.
// DVI_VERTICAL_REPEAT (dvi_config_defs.h, default 2) doubles each canvas
// row to 2 physical scanlines, so kCanvasH=240 produces the full 480-line
// active picture. kCanvasW=320 (not 640): the non-fullres 16bpp TMDS
// encoder used below (encode_row()) doubles each source column to 2
// physical columns internally, so the framebuffer itself stays source
// (320) width, not physical (640) width.
constexpr int kCanvasW = 320;
constexpr int kCanvasH = 240;

constexpr int kSrcRows = SCREENHEIGHT;  // 200
constexpr int kSrcCols = SCREENWIDTH;   // 320
constexpr int kBorderRows = (kCanvasH - kSrcRows) / 2; // 20

dvi_inst g_dvi;

// THE framebuffer -- a single, persistent, always-valid RGB565 image core1
// re-encodes continuously at the DVI's 60Hz, independently of core0's own
// (much less regular) Doom-tic rate. Plain SRAM (.bss) -- see file header
// for why, and for why this is affordable now (doom/w_wad.c's own large
// allocations moved to PSRAM to make room). alignas(4): the encoder takes
// a `const uint32_t*` (320*2=640-byte rows stay 4-aligned). 320*240*2 =
// 153600 bytes total.
alignas(4) uint16_t g_framebuf[kCanvasW * kCanvasH];

uint16_t g_rgb565_lut[256];
// Gamma factor applied at RGB565-LUT-build time (set from the serial
// console, src/i_serial_console.cpp; F3/F4 keys that used to tune it are
// freed for vanilla DOOM) -- same live-tunable brighten/darken curve the LCD
// build has (src/i_video_ili9486.cpp), for the DVI/HDMI path. Defaults to
// 1.4 (a slight brighten -- gammatable[usegamma]'s + gamma table reads a bit
// dark on the LCD panel; kept as the baseline here too, and FDIV/FPU
// cost is trivial since it only runs on LUT rebuilds, never per pixel).
// Pressing down to exactly 1.0 engages an identity fast path in
// rebuild_rgb565_lut() so an uncorrected palette is byte-identical to the
// pre-gamma build.
float g_gamma = 1.4f;
// Raw PLAYPAL byte data from the last I_SetPalette() -- kept so a serial
// 'gamma' command can rebuild the LUT at a new factor without the engine
// re-handing the palette over. Core0-only, same ownership as the LCD
// build's copy.
byte g_palette_cache[256 * 3];

// Core0-only (see g_gamma/g_palette_cache above). Same rebuild-on-demand
// design as src/i_video_ili9486.cpp's rebuild_rgb565_lut(): runs the cached
// raw palette through gamma + gammatable[usegamma] into the native-endian
// RGB565 LUT I_FinishUpdate() indexes per pixel. Rebuilt wholesale on every
// I_SetPalette() and every serial 'gamma' change -- 256 entries, one-time
// cost, not per frame.
void rebuild_rgb565_lut() {
    for (int i = 0; i < 256; ++i) {
        byte r = gammatable[usegamma][g_palette_cache[i * 3 + 0]];
        byte g = gammatable[usegamma][g_palette_cache[i * 3 + 1]];
        byte b = gammatable[usegamma][g_palette_cache[i * 3 + 2]];
        if (g_gamma != 1.0f) {
            // Per-channel power curve (v/255)^(1/gamma): gamma > 1
            // brightens, < 1 darkens. (v/255) only round-trips exactly
            // through float for v divisible by 255, hence the identity fast
            // path -- at gamma 1.0 we skip the divide-and-back entirely
            // rather than perturb the palette with float rounding.
            const float inv = 1.0f / g_gamma;
            r = static_cast<byte>(255.0f * powf(r / 255.0f, inv) + 0.5f);
            g = static_cast<byte>(255.0f * powf(g / 255.0f, inv) + 0.5f);
            b = static_cast<byte>(255.0f * powf(b / 255.0f, inv) + 0.5f);
        }
        // Native-endian RGB565 -- no SPI-wire byte-swap needed here, unlike
        // src/i_video_ili9486.cpp's LUT (this encoder reads native uint32
        // words, see encode_row()).
        g_rgb565_lut[i] = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
}

// core1's stack -- carved out of ordinary SRAM rather than the default
// multicore_launch_core1() mechanism's tiny fixed SCRATCH_X bank, matching
// both TOM6809's PicoDviVideoOutput and this project's own
// src/i_input_usbhid.cpp convention. Smaller than that convention's usual
// 16KB: unlike i_input_usbhid.cpp's core1 (TinyUSB host stack +
// Pico-PIO-USB's own bit-banging call chains, genuinely deep), this core1
// only ever runs encode_row()'s small call chain plus libdvi's own
// per-scanline IRQ handler.
constexpr size_t kCore1StackWords = 1024; // 4KB
uint32_t g_core1_stack[kCore1StackWords];

// core1-only row cursor -- advances one scanline per core1_step(),
// wrapping at kCanvasH.
int g_row = 0;

bool g_border_painted = false;

// --- Stats line (SRAM/PSRAM usage, FPS, convert time) ---
// Same intent as src/i_video_ili9486.cpp's stats line, trimmed: there's no
// core0<->core1 handoff/backpressure left to report (core1 free-runs,
// never blocks core0), so this only tracks core0's own convert cost.
uint64_t g_last_frame_start_us = 0;
uint64_t g_stats_window_start_us = 0;
uint32_t g_frames_in_window = 0;
uint64_t g_convert_us_in_window = 0;
uint64_t g_frame_us_in_window = 0;

void report_stats_if_due(uint64_t now_us) {
    constexpr uint64_t kIntervalUs = 10'000'000; // once every 10s
    if (g_stats_window_start_us == 0) {
        g_stats_window_start_us = now_us;
        return;
    }
    uint64_t elapsed_us = now_us - g_stats_window_start_us;
    if (elapsed_us < kIntervalUs)
        return;

    float fps = static_cast<float>(g_frames_in_window) * 1'000'000.0f / static_cast<float>(elapsed_us);
    float convert_pct = g_frame_us_in_window
        ? 100.0f * static_cast<float>(g_convert_us_in_window) / static_cast<float>(g_frame_us_in_window)
        : 0.0f;

    struct mallinfo mi = mallinfo();
    size_t sram_total = static_cast<size_t>(&__StackLimit - &__bss_end__);
    size_t sram_used = static_cast<size_t>(mi.uordblks);
    size_t psram_used = pico_toolset::psram_used_bytes();
    size_t psram_total = pico_toolset::psram_status().size_bytes;

    float tic_pct = 0.0f, render3d_pct = 0.0f, draw2d_pct = 0.0f;
    float bsp_walls_pct = 0.0f, planes_pct = 0.0f, sprites_pct = 0.0f;
    i_frame_stats_take(elapsed_us, &tic_pct, &render3d_pct, &draw2d_pct,
                        &bsp_walls_pct, &planes_pct, &sprites_pct);
    uint32_t wad_cache_misses = i_frame_stats_take_wad_cache_misses();

    printf("PicoDoom: SRAM %u/%uKB  PSRAM %u/%uKB  FPS %.1f  core0-convert %.0f%%  "
           "tic %.0f%%  render3d %.0f%% (bsp+walls %.0f%%  planes %.0f%%  sprites %.0f%%)  draw2d %.0f%%  "
           "wad-cache-miss %u/10s\n",
           static_cast<unsigned>(sram_used / 1024), static_cast<unsigned>(sram_total / 1024),
           static_cast<unsigned>(psram_used / 1024), static_cast<unsigned>(psram_total / 1024),
           fps, convert_pct, tic_pct, render3d_pct,
           bsp_walls_pct, planes_pct, sprites_pct, draw2d_pct,
           static_cast<unsigned>(wad_cache_misses));

    g_stats_window_start_us = now_us;
    g_frames_in_window = 0;
    g_convert_us_in_window = 0;
    g_frame_us_in_window = 0;
}

// core1: encode one 320-wide framebuffer row into one 640-physical-pixel
// TMDS buffer, same encoder+n_pix convention dvi.c's own
// _dvi_prepare_scanline_16bpp() uses for its scanbuf path (n_pix=pixwidth/2
// -- the encoder duplicates each source pixel to 2 physical columns
// itself), just reading straight from g_framebuf's row cursor (a plain CPU
// load -- see file header) instead of popping from q_colour_valid.
void __not_in_flash_func(encode_row)(const uint16_t* row) {
    uint32_t* tmdsbuf;
    queue_remove_blocking_u32(&g_dvi.q_tmds_free, &tmdsbuf);
    const uint pixwidth = g_dvi.timing->h_active_pixels;
    const uint words_per_channel = pixwidth / DVI_SYMBOLS_PER_WORD;
    const auto* pix = reinterpret_cast<const uint32_t*>(row);
    tmds_encode_data_channel_16bpp(pix, tmdsbuf + 0 * words_per_channel, pixwidth / 2,
                                   DVI_16BPP_BLUE_MSB, DVI_16BPP_BLUE_LSB);
    tmds_encode_data_channel_16bpp(pix, tmdsbuf + 1 * words_per_channel, pixwidth / 2,
                                   DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
    tmds_encode_data_channel_16bpp(pix, tmdsbuf + 2 * words_per_channel, pixwidth / 2,
                                   DVI_16BPP_RED_MSB, DVI_16BPP_RED_LSB);
    queue_add_blocking_u32(&g_dvi.q_tmds_valid, &tmdsbuf);
}

void __not_in_flash_func(core1_entry)() {
    // Registers this core's exclusive IRQ handler then starts wiggling
    // TMDS pairs. g_framebuf is already valid (zeroed, i.e. black) by the
    // time this runs -- see I_InitGraphics() -- so scanout can start
    // immediately, same as TOM6809's core1_setup()/gui_demo's core1_main().
    dvi_register_irqs_this_core(&g_dvi, DMA_IRQ_0);
    dvi_start(&g_dvi);
    for (;;) {
        encode_row(g_framebuf + static_cast<size_t>(g_row) * kCanvasW);
        g_row = (g_row + 1) % kCanvasH;
    }
}

} // namespace

extern "C" {

struct dvi_inst* i_video_dvi_instance(void) { return &g_dvi; }

// g_framebuf accessors for the boot WAD-selection menu (Phase 4,
// src/wad_menu.cpp): the LVGL display driver for this build
// (src/lvgl_display_dvi.cpp, 320x240 canvas == g_framebuf 1:1 -- see that
// file) writes native-endian RGB565 straight into the same buffer core1's
// DVI encoder re-scans at 60Hz. Boot-menu-only, core0 (this file already
// guarantees the encoder is the only other reader).
uint16_t* i_video_dvi_framebuf(void) { return g_framebuf; }

int i_video_dvi_width(void) { return kCanvasW; }

int i_video_dvi_height(void) { return kCanvasH; }

void I_InitGraphics(void) {
    if (g_graphics_init_done)
        return;
    g_graphics_init_done = true;

    g_dvi.timing = &dvi_timing_640x480p_60hz;
    // pico_sock_cfg (common_dvi_pin_configs.h): pins_tmds={36,34,32},
    // pins_clk=38 -- this board's actual onboard TMDS connector pins
    // (Waveshare's own official DVI demo, real-hardware confirmed).
    g_dvi.ser_cfg = pico_sock_cfg;
    // RP2350B's TMDS pins (32-39) sit outside the PIO block's default
    // 0-31 GPIO window -- move the window to 16-47 before dvi_init().
    // libdvi never calls this itself (unlike pico_fatfs).
    pio_set_gpio_base(g_dvi.ser_cfg.pio, 16);
    dvi_init(&g_dvi, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    memset(g_framebuf, 0, sizeof(g_framebuf));

    // core1 is this file's, exclusively -- nothing else may register an
    // IRQ or loop on it (see file header on why Pico-PIO-USB cannot share
    // it for this build).
    multicore_reset_core1();
    multicore_launch_core1_with_stack(core1_entry, g_core1_stack, sizeof(g_core1_stack));

    printf("PicoDoom: DVI/HDMI 640x480p60, %dx%d canvas (hardware 2x horiz/vert scale, %d-row border)\n",
           kCanvasW, kCanvasH, kBorderRows);
}

void I_ShutdownGraphics(void) {}

// Takes full 8 bit values (i_video.h). Caches the raw bytes (so a serial
// 'gamma' command can rebuild the LUT, see rebuild_rgb565_lut()) and applies
// gamma + gammatable[usegamma] via that same helper.
void I_SetPalette(byte* palette) {
    memcpy(g_palette_cache, palette, sizeof(g_palette_cache));
    rebuild_rgb565_lut();
}

void I_UpdateNoBlit(void) {}

void I_FinishUpdate(void) {
    uint64_t frame_start_us = time_us_64();
    if (g_last_frame_start_us != 0)
        g_frame_us_in_window += frame_start_us - g_last_frame_start_us;
    g_last_frame_start_us = frame_start_us;

    // Paint the border bands once -- they never change after this (no
    // border-colour-changing menu screens in this port), so there's no
    // need for the invalidate()/re-paint-on-change dance TOM6809's
    // multi-screen-mode renderer needs.
    if (!g_border_painted) {
        std::fill(g_framebuf, g_framebuf + static_cast<size_t>(kBorderRows) * kCanvasW, static_cast<uint16_t>(0));
        std::fill(g_framebuf + static_cast<size_t>(kBorderRows + kSrcRows) * kCanvasW,
                  g_framebuf + static_cast<size_t>(kCanvasH) * kCanvasW, static_cast<uint16_t>(0));
        g_border_painted = true;
    }

    // screens[0] itself is never touched here (see file header) -- just
    // read out. Straight palette->RGB565 copy, no scaling in this loop at
    // all: kCanvasW==kSrcCols (320), and both the horizontal 2x (the
    // encoder, see encode_row()) and vertical 2x (DVI_VERTICAL_REPEAT) are
    // hardware/library doubling, not anything this loop needs to do.
    const byte* src = screens[0];
    for (int row = 0; row < kSrcRows; ++row) {
        const byte* src_row = src + static_cast<size_t>(row) * kSrcCols;
        uint16_t* dst_row = g_framebuf + static_cast<size_t>(kBorderRows + row) * kCanvasW;
        for (int col = 0; col < kSrcCols; ++col)
            dst_row[col] = g_rgb565_lut[src_row[col]];
    }

    uint64_t now_us = time_us_64();
    g_convert_us_in_window += now_us - frame_start_us;
    ++g_frames_in_window;
    report_stats_if_due(now_us);
}

void I_ReadScreen(byte* scr) {
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

// Gamma-factor accessors for the serial console (src/i_serial_console.cpp;
// F3/F4 keys that used to tune this are freed for vanilla DOOM). Adjusts the
// factor I_SetPalette()'s LUT was built at -- see rebuild_rgb565_lut() above
// for what that does. Rebuilding from the cached raw PLAYPAL means the
// engine is never involved and the new setting takes effect from the very
// next I_FinishUpdate(); the value is echoed to the serial console, same
// pattern as the LCD build. The setter clamps to [0.1, 10].
float i_video_gamma(void) {
    return g_gamma;
}

void i_video_set_gamma(float gamma) {
    constexpr float kMinGamma = 0.1f;
    constexpr float kMaxGamma = 10.0f;
    g_gamma = gamma;
    if (g_gamma < kMinGamma)
        g_gamma = kMinGamma;
    else if (g_gamma > kMaxGamma)
        g_gamma = kMaxGamma;
    rebuild_rgb565_lut();
    printf("PicoDoom: gamma %.2f\n", g_gamma);
}

// I_StartTic lives in src/i_input_usbhid.cpp.
void I_StartFrame(void) {}

} // extern "C"
