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
#include "Ili9486Display.hpp"
#include "Psram.hpp"

extern "C" {
#include "doomdef.h"
#include "i_video.h"
#include "v_video.h"
}

#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <span>

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

// Nearest-neighbor source index per destination pixel, precomputed once
// (I_InitGraphics) so I_FinishUpdate's hot loop is a plain table lookup
// instead of a per-pixel multiply+divide.
int g_xsrc[kDstWidth];
int g_ysrc[kDstHeight];

// --- Stats line (SRAM/PSRAM usage, FPS, game vs. LCD-feed time split) ---
// All timing is taken around this file's own blit, since that's the one
// thing on the critical path we control here; no engine changes needed.
// "SRAM" reports newlib's heap (small transient allocations -- WAD
// directory tables, FatFs, sound data; the 512KB budget also carries
// DOOM's static tables and stacks, which don't change at runtime and so
// aren't useful to poll). "PSRAM" is the psram_malloc pool backing the
// zone heap + screen buffer (see doom/i_system.c, i_video_ili9486.cpp).
uint64_t g_last_frame_start_us = 0;
uint64_t g_stats_window_start_us = 0;
uint32_t g_frames_in_window = 0;
uint64_t g_spi_us_in_window = 0;
uint64_t g_frame_us_in_window = 0;

void report_stats_if_due(uint64_t now_us) {
    constexpr uint64_t kIntervalUs = 1'000'000; // once a second
    if (g_stats_window_start_us == 0) {
        g_stats_window_start_us = now_us;
        return;
    }
    uint64_t elapsed_us = now_us - g_stats_window_start_us;
    if (elapsed_us < kIntervalUs)
        return;

    float fps = static_cast<float>(g_frames_in_window) * 1'000'000.0f / static_cast<float>(elapsed_us);
    float spi_pct = g_frame_us_in_window
        ? 100.0f * static_cast<float>(g_spi_us_in_window) / static_cast<float>(g_frame_us_in_window)
        : 0.0f;
    float game_pct = 100.0f - spi_pct;

    struct mallinfo mi = mallinfo();
    size_t sram_total = static_cast<size_t>(&__StackLimit - &__bss_end__);
    size_t sram_used = static_cast<size_t>(mi.uordblks);
    size_t psram_used = psram_used_bytes();
    size_t psram_total = psram_status().size_bytes;

    printf("PicoDoom: SRAM %u/%uKB  PSRAM %u/%uKB  FPS %.1f  game %.0f%%  SPI %.0f%%\n",
           static_cast<unsigned>(sram_used / 1024), static_cast<unsigned>(sram_total / 1024),
           static_cast<unsigned>(psram_used / 1024), static_cast<unsigned>(psram_total / 1024),
           fps, game_pct, spi_pct);

    g_stats_window_start_us = now_us;
    g_frames_in_window = 0;
    g_spi_us_in_window = 0;
    g_frame_us_in_window = 0;
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

    g_display.init();
    // Black out the whole panel once, independent of any palette/game state
    // -- confirms the panel is alive and gives a clean border around the
    // centered scaled game viewport that I_FinishUpdate never touches.
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
    static uint16_t row[kDstWidth];

    uint64_t frame_start_us = time_us_64();
    if (g_last_frame_start_us != 0)
        g_frame_us_in_window += frame_start_us - g_last_frame_start_us;
    g_last_frame_start_us = frame_start_us;

    const byte* src = screens[0];
    uint64_t spi_start_us = time_us_64();
    g_display.set_window(kOffsetX, kOffsetY,
                          kOffsetX + kDstWidth - 1, kOffsetY + kDstHeight - 1);
    for (int y = 0; y < kDstHeight; ++y) {
        const byte* srcrow = src + g_ysrc[y] * SCREENWIDTH;
        for (int x = 0; x < kDstWidth; ++x)
            row[x] = g_rgb565_wire_lut[srcrow[g_xsrc[x]]];
        g_display.write_pixels(std::span<const uint16_t>(row, kDstWidth));
    }
    g_display.end_write();
    uint64_t now_us = time_us_64();
    g_spi_us_in_window += now_us - spi_start_us;

    ++g_frames_in_window;
    report_stats_if_due(now_us);
}

void I_ReadScreen(byte* scr) {
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

// I_StartTic lives in src/i_input_usbhid.cpp (Phase 3, USB-PIO keyboard).
void I_StartFrame(void) {}

} // extern "C"
