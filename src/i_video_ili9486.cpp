// Real video driver for the Pico port: replaces doom/i_video_null.c.
// Blits DOOM's 320x200 palette-indexed screens[0] to the Waveshare 3.5"
// ILI9486 LCD via Ili9486Display, 1:1 (no scaling), centered on the panel's
// 480x320 landscape frame in an 80px/60px border -- see docs/PLAN.md
// Phase 2. Was briefly upscaled 3:2 to fill the panel (320x200 -> 480x300),
// reverted: SPI feed time turned out to scale with byte count as expected
// (DMA vs. spi_write_blocking made no measurable difference -- the
// bottleneck is bus throughput, not CPU-side overhead), so fewer bytes
// wins until the achieved SPI clock is sorted out (see the one-shot log
// in Ili9486Display::set_window()). C++ driver behind a plain C
// interface, same bridging pattern as src/PicoDoom.cpp.
//
// Phase 4.5 (performance): I_FinishUpdate() (core0) no longer does the
// palette->RGB565 conversion or the SPI feed itself -- both moved to core1
// (i_video_core1_step(), driven from PicoUsbKeyboard's core1 loop, see
// i_video_core1.hpp) so core0 can start the next tic instead of blocking on
// the ~41ms SPI transfer.
//
// Phase 4.6 (memory): screens[0] itself is ping-ponged between two
// SRAM-resident buffers (g_screen_buf[2]) instead of copying it into
// separate PSRAM blit buffers every frame -- the earlier design deliberately
// avoided this (see git history) over a real concern: v_video.c's V_Init()
// allocates screens[0..3] as one contiguous PSRAM block, and r_draw.c's
// R_InitBuffer() caches per-row POINTERS INTO screens[0] (`ylookup[i] =
// screens[0] + ...`) -- but only refreshes them when the view size changes
// (menu screen-size +/-), not every frame. Swapping screens[0] without
// also refreshing that cache left the renderer drawing into a stale
// buffer. Fixed by calling R_InitBuffer() again ourselves after every
// swap (cheap: SCREENHEIGHT pointer computations, not a per-pixel cost).
// This removes both the per-frame memcpy (g_memcpy_us_in_window, gone) and
// one whole buffer's worth of PSRAM -- and, the actual point, fits both
// buffers in SRAM instead: freed via making doom/tables.c's
// finesine/finetangent/tantoangle `const` (~64KB back to .rodata/flash --
// see docs/PLAN.md; they turned out to be unconditionally read-only, the
// runtime-rewrite code that made them look mutable is `#if 0`'d out by id
// Software themselves). g_screen_buf being SRAM also means core1's
// palette->RGB565 conversion loop now reads SRAM instead of PSRAM -- the
// other half of what was costing ~50% of core1's blit time per the
// core1-convert/core1-dma stats split.
#include "Ili9486Display.hpp"
#include "Psram.hpp"
#include "i_video_core1.hpp"

extern "C" {
#include "doomdef.h"
#include "doomstat.h"
#include "i_video.h"
#include "i_system.h"
#include "r_defs.h"
#include "r_draw.h"
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
Ili9486Display g_display;
uint16_t g_rgb565_wire_lut[256];

constexpr int kDstWidth = SCREENWIDTH;   // 320, 1:1 -- see file header
constexpr int kDstHeight = SCREENHEIGHT; // 200
constexpr int kOffsetX = (Ili9486Display::kWidth - kDstWidth) / 2;   // 80
constexpr int kOffsetY = (Ili9486Display::kHeight - kDstHeight) / 2; // 60

// core1's per-call blit granularity (see i_video_core1_step() below):
// convert+DMA-transfer this many rows per call. TRIED 8 (on the theory that
// 200 dma_channel_configure() + 200 tuh_task() calls/frame were the
// overhead above the ~41ms SPI-bound theoretical minimum) -- measured WORSE
// (9.1-9.6fps vs. 10.5fps at chunk=1) AND hung after ~20-25s (core1-blit%
// climbing toward 100% right before it did). Root cause never confirmed,
// but the strong suspect is Pico-PIO-USB itself: batching stretched the gap
// between tuh_task() calls from ~one row's DMA wait (~200us) to one whole
// chunk's (~1.6ms at 8 rows -- convert-then-transfer, so the *whole* chunk's
// CPU+DMA time elapses between tuh_task() calls, not just the DMA part),
// which plausibly starves whatever software-timed bus servicing (SOF
// generation, retry windows) Pico-PIO-USB needs on a tighter cadence than
// that -- unconfirmed, but not worth re-risking a hang to find out. Back to
// 1 (known-stable) until g_core1_convert_us_accum/g_core1_dma_us_accum
// below (added at the same time as this revert) show where the real ~95ms/
// frame is actually going, instead of guessing again.
constexpr int kRowsPerChunk = 1;

// Nearest-neighbor source index per destination pixel, precomputed once
// (I_InitGraphics) so the per-row conversion is a plain table lookup
// instead of a per-pixel multiply+divide. Read by core1 (i_video_core1_step)
// -- written once at init, before core1 ever has a frame to consume, so no
// synchronization needed.
int g_xsrc[kDstWidth];
int g_ysrc[kDstHeight];

// --- core0 -> core1 blit handoff (ping-pong, see file header) ---
// screens[0] itself points into one of these at any given time -- SRAM
// (not psram_malloc'd), which is the actual point of the Phase 4.6 change:
// core1's conversion loop reads this directly instead of a PSRAM copy.
// .bss (zero-initialized) rather than an explicit memset -- the one frame
// this shows before the renderer has drawn anything (see the first-call
// handling in I_FinishUpdate()) comes out black, same as I_InitGraphics's
// own fill_solid(0).
byte g_screen_buf[2][SCREENWIDTH * SCREENHEIGHT];
// true = free for the renderer (screens[0]) to draw into. Flip conventions
// match PicoUsbKeyboard.cpp's g_active_buf: plain volatile bool, one writer
// per flag direction (core0 only ever clears its target index, core1 only
// ever sets the index it just finished), safe on this platform without a
// lock.
volatile bool g_blit_buf_free[2] = {true, true};

// --- Stats line (SRAM/PSRAM usage, FPS, timing breakdown) ---
// "SRAM" reports newlib's heap (small transient allocations -- WAD
// directory tables, FatFs, sound data; the 512KB budget also carries
// DOOM's static tables and stacks, which don't change at runtime and so
// aren't useful to poll). "PSRAM" is the psram_malloc pool backing the
// zone heap + screen buffer (see doom/i_system.c, i_video_ili9486.cpp).
// "wait%" is core0's own I_FinishUpdate() time spent blocked on backpressure
// (waiting for core1 to free a buffer) -- 0% means core0 is the bottleneck
// (game logic/render), high% means core1's blit is. "convert%"/"dma%" split
// core1's own per-frame time (relative to the stats window) between the
// palette->RGB565 LUT loop and the actual write_pixels() SPI feed -- added
// to find where the ~95ms/frame single-core-equivalent blit time was
// actually going; now that g_screen_buf is SRAM (Phase 4.6), convert% is
// the number to watch to confirm that actually helped.
uint64_t g_last_frame_start_us = 0;
uint64_t g_stats_window_start_us = 0;
uint32_t g_frames_in_window = 0;
uint64_t g_wait_us_in_window = 0;
uint64_t g_frame_us_in_window = 0;

// Written by core1 (i_video_core1_step, after each row/chunk), read and
// reset by core0's report_stats_if_due() -- plain accumulators like
// PicoUsbKeyboard's cross-core fields; a torn read would only skew one 10s
// diagnostic line, not correctness.
volatile uint64_t g_core1_convert_us_accum = 0;
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
    float convert_pct = elapsed_us
        ? 100.0f * static_cast<float>(g_core1_convert_us_accum) / static_cast<float>(elapsed_us)
        : 0.0f;
    float dma_pct = elapsed_us
        ? 100.0f * static_cast<float>(g_core1_dma_us_accum) / static_cast<float>(elapsed_us)
        : 0.0f;

    struct mallinfo mi = mallinfo();
    size_t sram_total = static_cast<size_t>(&__StackLimit - &__bss_end__);
    size_t sram_used = static_cast<size_t>(mi.uordblks);
    size_t psram_used = psram_used_bytes();
    size_t psram_total = psram_status().size_bytes;

    printf("PicoDoom: SRAM %u/%uKB  PSRAM %u/%uKB  FPS %.1f  "
           "core0-wait %.0f%%  core1-convert %.0f%%  core1-dma %.0f%%\n",
           static_cast<unsigned>(sram_used / 1024), static_cast<unsigned>(sram_total / 1024),
           static_cast<unsigned>(psram_used / 1024), static_cast<unsigned>(psram_total / 1024),
           fps, wait_pct, convert_pct, dma_pct);

    g_stats_window_start_us = now_us;
    g_frames_in_window = 0;
    g_wait_us_in_window = 0;
    g_frame_us_in_window = 0;
    g_core1_convert_us_accum = 0;
    g_core1_dma_us_accum = 0;
}
} // namespace

// Definitions, not just the declarations pulled in above, need C linkage --
// a plain (unqualified) definition here would get C++-mangled and leave
// doom/d_main.c's calls to e.g. I_StartFrame unresolved at link time.
extern "C" {

void I_InitGraphics(void) {
    for (int x = 0; x < kDstWidth; ++x)
        g_xsrc[x] = x * SCREENWIDTH / kDstWidth;
    for (int y = 0; y < kDstHeight; ++y)
        g_ysrc[y] = y * SCREENHEIGHT / kDstHeight;

    // core1 is already running PicoUsbKeyboard's loop by this point (started
    // from src/PicoDoom.cpp before D_DoomMain()) and will start calling
    // i_video_core1_step() immediately -- but g_blit_buf_free starts all-true
    // and the inter-core FIFO starts empty, so it just no-ops until
    // I_FinishUpdate() below ever pushes an index. g_screen_buf itself is a
    // plain static array (see its doc comment) -- nothing to allocate here
    // anymore.

    g_display.init();
    // Black out the whole panel once, independent of any palette/game state
    // -- confirms the panel is alive and gives a clean border around the
    // centered scaled game viewport that I_FinishUpdate never touches. Runs
    // here on core0, synchronously, before D_DoomMain() ever calls
    // I_FinishUpdate() -- the one and only time core0 touches g_display;
    // every call after this is from core1 (i_video_core1_step()).
    g_display.fill_solid(0);
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
        // Ili9486Display::write_pixels()).
        g_rgb565_wire_lut[i] = static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8));
    }
}

void I_UpdateNoBlit(void) {}

void I_FinishUpdate(void) {
    // active_idx: which g_screen_buf[] slot screens[0] currently points to
    // (i.e. what the renderer just finished drawing into). Static local --
    // this function is the only reader/writer, nothing else needs to see it.
    static int active_idx = -1;

    uint64_t frame_start_us = time_us_64();
    if (g_last_frame_start_us != 0)
        g_frame_us_in_window += frame_start_us - g_last_frame_start_us;
    g_last_frame_start_us = frame_start_us;

    if (active_idx < 0) {
        // First call: screens[0] still points at V_Init()'s original PSRAM
        // buffer (V_Init() runs after I_InitGraphics(), so this couldn't be
        // done any earlier -- see file header). Whatever the renderer drew
        // into that buffer for this first tic is discarded in favor of
        // g_screen_buf[0]'s zero-initialized (black) contents -- matches
        // I_InitGraphics()'s own fill_solid(0), not a visible regression.
        // R_InitBuffer() must run again against the new screens[0] before
        // the *next* tic renders, or ylookup[]/columnofs[] stay stale --
        // see file header.
        screens[0] = g_screen_buf[0];
        R_InitBuffer(scaledviewwidth, viewheight);
        active_idx = 0;
    }

    // Backpressure: only blocks if core1 hasn't finished blitting the
    // *other* buffer from two frames ago yet, i.e. if core1's SPI feed is
    // the bottleneck rather than core0's game logic/render -- see
    // g_wait_us's doc comment above. Must resolve before the renderer is
    // allowed to start drawing into that buffer as the new screens[0].
    int next_idx = 1 - active_idx;
    uint64_t wait_start_us = time_us_64();
    while (!g_blit_buf_free[next_idx])
        tight_loop_contents();
    uint64_t now_us = time_us_64();
    g_wait_us_in_window += now_us - wait_start_us;

    // Hand off the buffer the renderer just finished (still screens[0]) to
    // core1 -- no copy, core1 reads g_screen_buf[active_idx] directly.
    g_blit_buf_free[active_idx] = false;
    multicore_fifo_push_blocking(static_cast<uint32_t>(active_idx));

    // Point screens[0] at the other (already-confirmed-free) buffer for the
    // renderer to draw the next tic into, and refresh ylookup[]/columnofs[]
    // to match -- see file header.
    screens[0] = g_screen_buf[next_idx];
    R_InitBuffer(scaledviewwidth, viewheight);
    active_idx = next_idx;

    ++g_frames_in_window;
    report_stats_if_due(now_us);
}

void I_ReadScreen(byte* scr) {
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

// I_StartTic lives in src/i_input_usbhid.cpp (Phase 3, USB-PIO keyboard).
void I_StartFrame(void) {}

} // extern "C"

namespace {
// core1-side blit state machine, stepped one chunk of kRowsPerChunk rows
// (or one FIFO check) at a time by i_video_core1_step() -- see
// i_video_core1.hpp for why this can't just be "pop a frame, blit it all in
// one call": core1's caller (PicoUsbKeyboard::core1_entry()) needs to get
// back to tuh_task() between chunks, not just between whole frames, or
// Pico-PIO-USB's software-timed bus servicing starves for the ~41ms a full
// frame's SPI feed takes.
enum class BlitState { Idle, Blitting };
BlitState g_blit_state = BlitState::Idle;
int g_blit_idx = 0;
int g_blit_row = 0;
} // namespace

void i_video_core1_step() {
    if (g_blit_state == BlitState::Idle) {
        if (!multicore_fifo_rvalid())
            return;
        g_blit_idx = static_cast<int>(multicore_fifo_pop_blocking());
        g_blit_row = 0;
        g_display.set_window(kOffsetX, kOffsetY,
                              kOffsetX + kDstWidth - 1, kOffsetY + kDstHeight - 1);
        g_blit_state = BlitState::Blitting;
        return;
    }

    // Blitting: kRowsPerChunk rows converted, then ONE DMA transfer for the
    // whole chunk -- see kRowsPerChunk's doc comment above. chunk_buf is
    // core1-only (core0 no longer touches g_display or does any pixel
    // conversion), so a plain static local is fine -- no cross-core sharing
    // to worry about. Convert and DMA timed separately (not just bracketing
    // the whole call) so the stats line can show which one actually
    // dominates instead of a single opaque "blit" number -- see
    // g_core1_convert_us_accum/g_core1_dma_us_accum's doc comment above.
    static uint16_t chunk_buf[kRowsPerChunk * kDstWidth];
    int rows_this_chunk = kDstHeight - g_blit_row;
    if (rows_this_chunk > kRowsPerChunk)
        rows_this_chunk = kRowsPerChunk;

    uint64_t convert_start_us = time_us_64();
    for (int r = 0; r < rows_this_chunk; ++r) {
        const byte* srcrow = g_screen_buf[g_blit_idx] + g_ysrc[g_blit_row + r] * SCREENWIDTH;
        uint16_t* dstrow = chunk_buf + r * kDstWidth;
        for (int x = 0; x < kDstWidth; ++x)
            dstrow[x] = g_rgb565_wire_lut[srcrow[g_xsrc[x]]];
    }
    uint64_t dma_start_us = time_us_64();
    g_core1_convert_us_accum += dma_start_us - convert_start_us;

    g_display.write_pixels(std::span<const uint16_t>(chunk_buf, static_cast<size_t>(rows_this_chunk) * kDstWidth));
    g_core1_dma_us_accum += time_us_64() - dma_start_us;
    g_blit_row += rows_this_chunk;

    if (g_blit_row >= kDstHeight) {
        g_display.end_write();
        // Release last: only after set_window/every row/end_write are all
        // done does core0 get to point screens[0] back at this buffer.
        g_blit_buf_free[g_blit_idx] = true;
        g_blit_state = BlitState::Idle;
    }
}
