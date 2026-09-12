// Real video driver for the Pico port: replaces doom/i_video_null.c.
// Blits DOOM's 320x200 palette-indexed screens[0] to the Waveshare 3.5"
// ILI9486 LCD via Pico-Toolset's pico_toolset::Ili9486, 1:1 (no scaling), centered on the panel's
// 480x320 landscape frame in an 80px/60px border -- see docs/PLAN.md
// Phase 2. Was briefly upscaled 3:2 to fill the panel (320x200 -> 480x300),
// reverted: SPI feed time turned out to scale with byte count as expected
// (DMA vs. spi_write_blocking made no measurable difference -- the
// bottleneck is bus throughput, not CPU-side overhead), so fewer bytes
// wins until the achieved SPI clock is sorted out (see the one-shot log
// in pico_toolset::Ili9486::set_window()). C++ driver behind a plain C
// interface, same bridging pattern as src/PicoDoom.cpp.
//
// Phase 4.5 (performance): I_FinishUpdate() (core0) no longer does the
// palette->RGB565 conversion or the SPI feed itself -- both moved to core1
// (i_video_core1_step(), driven from PicoUsbKeyboard's core1 loop, see
// i_video_core1.hpp) so core0 can start the next tic instead of blocking on
// the ~41ms SPI transfer.
//
// Phase 4.6 tried ping-ponging screens[0] itself between two buffers
// instead of memcpy'ing it into separate ones every frame, to kill the
// memcpy and get some of screens[0] into SRAM. Reverted (Phase 4.6.2):
// v_video.c's V_Init() allocates screens[0..3] as one contiguous block,
// and it turns out more than one thing in the engine assumes screens[0]'s
// *address* stays stable across many tics, not just within one --
// r_draw.c's R_InitBuffer() caches per-row pointers into it (`ylookup[i] =
// screens[0] + ...`, fixed by re-running R_InitBuffer() after every swap),
// but also: doom/st_stuff.c's status bar does incremental "diff" redraws
// (ST_diffDraw()/STlib_update*()) that skip repainting a widget if its
// value hasn't changed, relying on last tic's pixels still being there;
// doom/f_wipe.c's level-transition melt effect caches `wipe_scr =
// screens[0]` ONCE at the start of a multi-tic animation and keeps writing
// into that same captured pointer for its whole duration. Both silently
// break if screens[0] points somewhere else by the time they run again --
// confirmed on real hardware as corrupted/blinking status bar and
// transition screens. Not a one-off fix like R_InitBuffer(): unlike a
// pointer *cache* that can be refreshed, these assume *content*
// persists across frames, which a ping-ponged buffer fundamentally can't
// guarantee. So: screens[0] is never touched here again -- see g_screen_buf
// below for what replaced the plain memcpy this fed into.
//
// Phase 5 (performance, first attempt) tried fusing that memcpy with core1's
// palette conversion into one pass on core0, producing ready-to-DMA RGB565
// output directly, plus collapsing core1's per-row DMA into one
// dma_channel_configure() per frame. REVERTED at the time: FPS dropped from
// 11.4 to ~7.5 on real hardware, because PSRAM was still capped at a
// conservative 30MHz then -- fusing forced both blit buffers to be
// always-PSRAM and doubled the per-pixel write width (byte -> uint16_t), and
// at 30MHz that write cost far outweighed removing the double-traversal.
//
// Phase 6 (performance): retrying Phase 5's exact fusion, now that PSRAM
// runs at 100MHz (PicoDoom.cpp's clk_sys/clk_peri overclock + updated
// psram_configs.h -- over 3x Phase 5's 30MHz) and the SPI pixel clock is
// 33.33MHz (up from 25MHz). Motivation: hardware measurement after the
// clock-tuning pass alone (11.4 -> ~18.7fps) still showed core1-dma
// dominating (~70% of the frame) while core0 sat idle waiting ~45-55% of
// its own frame time -- the two cores' work isn't balanced, and neither
// __not_in_flash_func() on the hot renderer loops (doom/r_draw.c) nor
// batching core1's DMA calls moved the needle, so the remaining lever is
// shifting work from core1 (the bottleneck) to core0 (which has slack) by
// moving conversion back onto core0, fused with the copy, same as Phase 5 --
// but this time with PSRAM fast enough that the fused pass might cost less
// than Phase 4.6.2's separate memcpy+convert did *combined*, not more.
// core1 becomes pure DMA: pop an already-converted buffer, one
// start_pixels_dma() call for the whole frame (non-blocking, polled from
// the caller's loop so tuh_task() still runs every iteration -- see
// i_video_core1.hpp), no per-row chunking needed at all since there's no
// conversion left to interleave with USB servicing.
// NOT YET hardware-validated. If this regresses like Phase 5 did, the fix
// is the same: revert to Phase 4.6.2's split memcpy (core0) + per-row
// convert+DMA (core1), which is known-good at these clock speeds too
// (measured ~18.7fps).
#include "pico_toolset/ili9486.h"
#include "pico_toolset/ili9486_configs.h"
#include "pico_toolset/psram.h"
#include "i_video_core1.hpp"

extern "C" {
#include "doomdef.h"
#include "i_video.h"
#include "i_system.h"
#include "v_video.h"
}

#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <span>

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"

// Linker-provided symbols (pico-sdk's memmap linker script), not this file's
// -- extern "C" so the namespace below doesn't mangle the names we need to
// match.
extern "C" char __StackLimit;
extern "C" char __bss_end__;

namespace {
pico_toolset::Ili9486 g_display;
uint16_t g_rgb565_wire_lut[256];

constexpr int kDstWidth = SCREENWIDTH;   // 320, 1:1 -- see file header
constexpr int kDstHeight = SCREENHEIGHT; // 200
constexpr int kOffsetX = (pico_toolset::Ili9486::kWidth - kDstWidth) / 2;   // 80
constexpr int kOffsetY = (pico_toolset::Ili9486::kHeight - kDstHeight) / 2; // 60
constexpr int kFramePixels = kDstWidth * kDstHeight;

// --- core0 -> core1 blit handoff (ping-pong, see file header) ---
// I_FinishUpdate() converts screens[0] (palette-indexed) into whichever of
// these is free, writing ready-to-DMA RGB565-wire pixels directly (Phase 6);
// core1 just DMAs one straight from PSRAM to the panel, then flags that slot
// free again. Both slots PSRAM-backed: a single 128000-byte (kFramePixels *
// sizeof(uint16_t)) slot is already most of Phase 4.6.1's proven SRAM-OOM
// threshold on its own, so two of them (256000B total) stay off the newlib
// heap entirely, same reasoning as that phase's doc history -- only the
// clock got faster, not the SRAM budget.
uint16_t* g_screen_buf[2] = {nullptr, nullptr};
// true = free for core0 to write screens[0]'s converted pixels into. Flip
// conventions match PicoUsbKeyboard.cpp's g_active_buf: plain volatile
// bool, one writer per flag direction (core0 only ever clears its target
// index, core1 only ever sets the index it just finished), safe on this
// platform without a lock.
volatile bool g_blit_buf_free[2] = {true, true};

// --- Stats line (SRAM/PSRAM usage, FPS, timing breakdown) ---
// "SRAM" reports newlib's heap (small transient allocations -- WAD
// directory tables, FatFs, sound data; the 512KB budget also carries
// DOOM's static tables and stacks, which don't change at runtime and so
// aren't useful to poll). "PSRAM" is the psram_malloc pool backing the
// zone heap + screen buffer (see doom/i_system.c, i_video_ili9486.cpp).
// "wait%" is core0's own I_FinishUpdate() time spent blocked on backpressure
// (waiting for core1 to free a buffer) -- 0% means core0 is the bottleneck
// (game logic/render), high% means core1's DMA is. "convert%" is core0's
// fused screens[0]->g_screen_buf copy+palette-to-RGB565 pass (Phase 6),
// also relative to its own frame time. "dma%" is core1's per-frame time
// (relative to the stats window) spent in the
// start_pixels_dma()/pixels_busy()-poll/finish_pixels_dma() sequence
// feeding the whole converted frame to the panel.
uint64_t g_last_frame_start_us = 0;
uint64_t g_stats_window_start_us = 0;
uint32_t g_frames_in_window = 0;
uint64_t g_wait_us_in_window = 0;
uint64_t g_convert_us_in_window = 0;
uint64_t g_frame_us_in_window = 0;

// Written by core1 (i_video_core1_step), read and reset by core0's
// report_stats_if_due() -- plain accumulator like PicoUsbKeyboard's
// cross-core fields; a torn read would only skew one 10s diagnostic line,
// not correctness.
volatile uint64_t g_core1_dma_us_accum = 0;

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
    float wait_pct = g_frame_us_in_window
        ? 100.0f * static_cast<float>(g_wait_us_in_window) / static_cast<float>(g_frame_us_in_window)
        : 0.0f;
    float convert_pct = g_frame_us_in_window
        ? 100.0f * static_cast<float>(g_convert_us_in_window) / static_cast<float>(g_frame_us_in_window)
        : 0.0f;
    float dma_pct = elapsed_us
        ? 100.0f * static_cast<float>(g_core1_dma_us_accum) / static_cast<float>(elapsed_us)
        : 0.0f;

    struct mallinfo mi = mallinfo();
    size_t sram_total = static_cast<size_t>(&__StackLimit - &__bss_end__);
    size_t sram_used = static_cast<size_t>(mi.uordblks);
    size_t psram_used = pico_toolset::psram_used_bytes();
    size_t psram_total = pico_toolset::psram_status().size_bytes;

    printf("PicoDoom: SRAM %u/%uKB  PSRAM %u/%uKB  FPS %.1f  "
           "core0-wait %.0f%%  core0-convert %.0f%%  core1-dma %.0f%%\n",
           static_cast<unsigned>(sram_used / 1024), static_cast<unsigned>(sram_total / 1024),
           static_cast<unsigned>(psram_used / 1024), static_cast<unsigned>(psram_total / 1024),
           fps, wait_pct, convert_pct, dma_pct);

    g_stats_window_start_us = now_us;
    g_frames_in_window = 0;
    g_wait_us_in_window = 0;
    g_convert_us_in_window = 0;
    g_frame_us_in_window = 0;
    g_core1_dma_us_accum = 0;
}
} // namespace

// Definitions, not just the declarations pulled in above, need C linkage --
// a plain (unqualified) definition here would get C++-mangled and leave
// doom/d_main.c's calls to e.g. I_StartFrame unresolved at link time.
extern "C" {

void I_InitGraphics(void) {
    // core1 is already running PicoUsbKeyboard's loop by this point (started
    // from src/PicoDoom.cpp before D_DoomMain()) and will start calling
    // i_video_core1_step() immediately -- but g_blit_buf_free starts all-true
    // and the inter-core FIFO starts empty, so it just no-ops until
    // I_FinishUpdate() below ever pushes an index. Both slots PSRAM-backed
    // (see g_screen_buf's doc comment).
    for (uint16_t*& buf : g_screen_buf) {
        buf = static_cast<uint16_t*>(pico_toolset::psram_malloc(kFramePixels * sizeof(uint16_t)));
        if (!buf)
            // I_Error's signature predates `const` (1993 C) -- cast, not a
            // real mutation.
            I_Error(const_cast<char*>("I_InitGraphics: failed to allocate blit buffer"));
        // Not load-bearing (I_FinishUpdate() always fills a slot before it's
        // ever handed to core1), just avoids a stray uninitialized-PSRAM
        // read if that ever stops being true.
        memset(buf, 0, kFramePixels * sizeof(uint16_t));
    }

    g_display.init(pico_toolset::configs::ili9486::kWaveshareRp2350PiZero);
    // Black out the whole panel once, independent of any palette/game state
    // -- confirms the panel is alive and gives a clean border around the
    // centered scaled game viewport that I_FinishUpdate never touches. Runs
    // here on core0, synchronously, before D_DoomMain() ever calls
    // I_FinishUpdate() -- the one and only time core0 touches g_display;
    // every call after this is from core1 (i_video_core1_step()).
    g_display.fill_solid(0);
    // Boot-time clock report (see PicoDoom.cpp's clk_sys/clk_peri/flash line
    // above) -- fill_solid() already called set_window() at least once, so
    // pixel_clock_actual_hz() reflects the real applied rate, not just the
    // static config's request.
    printf("PicoDoom: SPI pixel clock requested=%u Hz  actual=%u Hz\n",
           (unsigned)g_display.pixel_clock_hz(), (unsigned)g_display.pixel_clock_actual_hz());
}

void I_ShutdownGraphics(void) {}

// Takes full 8 bit values (i_video.h).
void I_SetPalette(byte* palette) {
    for (int i = 0; i < 256; ++i) {
        byte r = gammatable[usegamma][*palette++];
        byte g = gammatable[usegamma][*palette++];
        byte b = gammatable[usegamma][*palette++];
        uint16_t rgb565 = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        // Byte-swapped for MSB-first-over-SPI wire order (see
        // pico_toolset::Ili9486::write_pixels()).
        g_rgb565_wire_lut[i] = static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8));
    }
}

void I_UpdateNoBlit(void) {}

void I_FinishUpdate(void) {
    // Ping-pong: alternate which buffer this call targets. Static local
    // instead of a namespace global -- this function is the only writer,
    // nothing else needs to see it.
    static int next_idx = 0;

    uint64_t frame_start_us = time_us_64();
    if (g_last_frame_start_us != 0)
        g_frame_us_in_window += frame_start_us - g_last_frame_start_us;
    g_last_frame_start_us = frame_start_us;

    // Backpressure: only blocks if core1 hasn't finished DMA'ing this same
    // buffer from two frames ago yet, i.e. if core1's SPI feed is the
    // bottleneck rather than core0's game logic/render -- see g_wait_us's
    // doc comment above.
    uint64_t wait_start_us = frame_start_us;
    while (!g_blit_buf_free[next_idx])
        tight_loop_contents();
    uint64_t convert_start_us = time_us_64();
    g_wait_us_in_window += convert_start_us - wait_start_us;

    // screens[0] itself is never touched here (see file header) -- just
    // read out. Fused copy+palette-to-RGB565 convert (Phase 6): one pass
    // over the frame instead of a separate memcpy (here) and LUT loop
    // (core1's, pre-Phase-6), each of which used to make their own full
    // PSRAM pass. core1 reads its own ready-to-DMA copy from here on.
    const byte* src = screens[0];
    uint16_t* dst = g_screen_buf[next_idx];
    for (int i = 0; i < kFramePixels; ++i)
        dst[i] = g_rgb565_wire_lut[src[i]];
    uint64_t now_us = time_us_64();
    g_convert_us_in_window += now_us - convert_start_us;

    g_blit_buf_free[next_idx] = false;
    multicore_fifo_push_blocking(static_cast<uint32_t>(next_idx));
    next_idx ^= 1;

    ++g_frames_in_window;
    report_stats_if_due(now_us);
}

void I_ReadScreen(byte* scr) {
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

// Live SPI pixel-clock tuning (2026-09 performance work): called from
// src/i_input_usbhid.cpp's F1/F2 handling so the actual hardware
// corruption ceiling can be found by hand instead of guessed from a
// datasheet. delta_hz may be negative; clamped so it can't underflow past a
// sane floor. pico_toolset::Ili9486::set_pixel_clock_hz() takes effect on
// the next set_window() call (core1's i_video_core1_step()), not
// immediately -- see that method's own doc comment on why calling it from
// core0 here, while core1 runs the blit loop, needs no extra
// synchronization.
void i_video_bump_pixel_clock_hz(int32_t delta_hz) {
    constexpr uint32_t kMinPixelClockHz = 1'000'000;
    uint32_t cur = g_display.pixel_clock_hz();
    uint32_t next = (delta_hz < 0 && static_cast<uint32_t>(-delta_hz) >= cur)
        ? kMinPixelClockHz
        : static_cast<uint32_t>(static_cast<int64_t>(cur) + delta_hz);
    g_display.set_pixel_clock_hz(next);
    printf("PicoDoom: SPI pixel clock requested=%u Hz  last actual=%u Hz\n",
           static_cast<unsigned>(next), static_cast<unsigned>(g_display.pixel_clock_actual_hz()));
}

// I_StartTic lives in src/i_input_usbhid.cpp (Phase 3, USB-PIO keyboard).
void I_StartFrame(void) {}

} // extern "C"

namespace {
// core1-side blit state machine (Phase 6: pure DMA, no conversion left on
// this core -- see file header). Idle -> Transferring (poll until the
// non-blocking DMA finishes) -> Idle each frame. Stepped once per call by
// i_video_core1_step() -- see i_video_core1.hpp for why this can't just be
// "pop a frame, DMA it and block until done": core1's caller
// (PicoUsbKeyboard::core1_entry()) needs to get back to tuh_task() every
// iteration, not just between whole frames, or Pico-PIO-USB's
// software-timed bus servicing starves for the ~31ms a full frame's SPI
// feed takes at 33.33MHz.
enum class BlitState { Idle, Transferring };
BlitState g_blit_state = BlitState::Idle;
int g_blit_idx = 0;
uint64_t g_dma_start_us = 0;
} // namespace

void i_video_core1_step() {
    if (g_blit_state == BlitState::Idle) {
        if (!multicore_fifo_rvalid())
            return;
        g_blit_idx = static_cast<int>(multicore_fifo_pop_blocking());
        g_display.set_window(kOffsetX, kOffsetY,
                              kOffsetX + kDstWidth - 1, kOffsetY + kDstHeight - 1);
        g_dma_start_us = time_us_64();
        g_display.start_pixels_dma(std::span<const uint16_t>(g_screen_buf[g_blit_idx], kFramePixels));
        g_blit_state = BlitState::Transferring;
        return;
    }

    // Transferring: don't block here -- return immediately if the DMA is
    // still running so the caller's loop gets back to tuh_task() before
    // checking again next iteration.
    if (g_display.pixels_busy())
        return;

    g_display.finish_pixels_dma();
    g_core1_dma_us_accum += time_us_64() - g_dma_start_us;
    g_display.end_write();
    // Release last: only after set_window/DMA/finish/end_write are all done
    // does core0 get to write this buffer's next frame.
    g_blit_buf_free[g_blit_idx] = true;
    g_blit_state = BlitState::Idle;
}
