// Video driver for the Pimoroni Presto (PICODOOM_VIDEO_OUTPUT=presto): a 480x480
// ST7701 RGB-parallel panel scanned out continuously by PIO + DMA
// (pico_toolset::St7701, served by core1's interrupts). The game occupies the
// top 480x300 of the panel -- full width, black band at the bottom -- either
// natively (480x300, "High res") or as 320x200 scaled by 1.5 (nearest).
//
// The RGB bus has no GRAM to leave pixels in, so there is no band hashing /
// blit pipeline here; what reaches the panel is whatever the scanout reads
// from SRAM (PSRAM cannot feed the DMA at pixel rate). Two build-time modes
// (-DPICODOOM_PRESTO_MODE=, CMake) trade SRAM, tearing and core1 time:
//
//   palette  (PICODOOM_PRESTO_PALETTE)  St7701Format::Palette8, 480x300 bytes
//     (144 KB) of colour indices expanded to RGB by the panel PIO's 256-entry
//     table. Hi-res: screens[0] IS that framebuffer (zero copy, fixed address,
//     so the "screens[0] never moves" rule holds; the renderer is visible
//     mid-draw => some tearing). Low-res: screens[0] is a separate 64 KB
//     buffer, expanded 1.5x into the framebuffer in I_FinishUpdate (~1 ms).
//     Palette changes are 256 register-width writes; core1 only idles.
//
//   scanline (default)  St7701Format::Scanline565, no framebuffer: core1 runs
//     scanline_task(), whose callback converts one source line per panel line
//     through the RGB565 LUT just ahead of the beam (~one line time of ~25 us).
//     Hi-res reads screens[0] directly (tears); low-res reads a 64 KB copy made
//     at the end of each frame, so only the ~0.2 ms copy can tear.
//
// Boot menu: LVGL renders (core0) into an 8-bit RGB332 surface of the same
// kind (palette mode: the framebuffer; scanline mode: a temporary 144 KB
// buffer freed in i_video_lcd_set_hires()), so the menu travels the same
// scanout path as the game -- see lvgl_display_presto.cpp.
#include "pico_toolset/st7701.h"
#include "pico_toolset/st7701_configs.h"
#include "pico_toolset/psram.h"
#include "pico_toolset/sram.h"
#include "board_config.hpp"
#include "i_frame_stats.hpp"
#include "i_video_gamma.hpp"
#include "pixel_scale.hpp"

extern "C" {
#include "doomdef.h"
#include "i_video.h"
#include "i_system.h"
#include "v_video.h"
#include "doomstat.h"
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"


namespace {

constexpr int kPanelW = 480;
constexpr int kVisibleH = 300; // game rows; 300..479 are the black band
constexpr size_t kSurfaceBytes = static_cast<size_t>(kPanelW) * kVisibleH;

#ifdef PICODOOM_PRESTO_PALETTE
constexpr pico_toolset::St7701Format kFormat = pico_toolset::St7701Format::Palette8;
constexpr const char* kModeName = "palette8";
#else
constexpr pico_toolset::St7701Format kFormat = pico_toolset::St7701Format::Scanline565;
constexpr const char* kModeName = "scanline565";
#endif

pico_toolset::St7701 g_lcd;
bool g_graphics_init_done = false;
volatile bool g_core1_ready = false;
volatile bool g_lcd_ok = false;
pico_toolset::St7701Config g_cfg;

// 480x300 indexed surface: Palette8 framebuffer / scanline-mode menu surface.
uint8_t* g_surface = nullptr;
#ifdef PICODOOM_PRESTO_PALETTE
bool g_double_buffer = false; // hi-res: screens[0] is separate from the framebuffer
#endif

picodoom::GammaPalette g_pal;

// core1 stack: St7701::init() runs on it (SPI init sequence, PIO/DMA setup);
// the scanout loop afterwards is shallow. Not the 2 KB scratch default.
constexpr size_t kCore1StackWords = 1536; // 6 KB (A2DP costs ~36 KB of SRAM; hi-res wants the rest)
uint32_t g_core1_stack[kCore1StackWords];

#ifndef PICODOOM_PRESTO_PALETTE
// Scanline mode: what the callback reads. Set from core0 only.
alignas(4) uint16_t g_lut[256];
const uint8_t* volatile g_src = nullptr; // nullptr = black
volatile int g_src_w = kPanelW;
uint8_t* g_lowres_copy = nullptr; // 320x200 tear-free copy (low-res only)
// Hi-res: a 480-wide copy of the top rows (everything above the status bar),
// refreshed once per frame, so the 3D view / menus / HUD sprites -- drawn in
// stages, sprites last -- never show half-drawn. The full 144 KB copy would not
// fit next to screens[0]; the status bar below it redraws incrementally, so it
// is scanned straight from screens[0].
uint8_t* g_head_copy = nullptr;
size_t g_head_alloc_rows = 0;
volatile int g_head_rows = 0; // rows of g_head_copy that are valid
int g_head_start = 0;         // first panel row the copy covers (anchored at the bottom of the 3D view)
#endif

void set_entry(int i, uint8_t r, uint8_t g, uint8_t b) {
#ifdef PICODOOM_PRESTO_PALETTE
    g_lcd.set_palette_colour(static_cast<uint8_t>(i), r, g, b);
#else
    const uint16_t rgb565 = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    g_lut[i] = pico_toolset::st7701_encode(rgb565);
#endif
}

void rebuild_game_palette() {
    for (int i = 0; i < 256; ++i) {
        const picodoom::Rgb8 c = g_pal.color(i);
        set_entry(i, c.r, c.g, c.b);
    }
}

// RGB332 colour cube the LVGL menu surface is quantised to (see
// lvgl_display_presto.cpp's flush).
void set_menu_palette() {
    for (int i = 0; i < 256; ++i) {
        const uint8_t r = static_cast<uint8_t>(((i >> 5) & 7) * 255 / 7);
        const uint8_t g = static_cast<uint8_t>(((i >> 2) & 7) * 255 / 7);
        const uint8_t b = static_cast<uint8_t>((i & 3) * 255 / 3);
        set_entry(i, r, g, b);
    }
}

#ifndef PICODOOM_PRESTO_PALETTE
// Renders panel line y (0..299) from the 8-bit source through the LUT.
// Runs on core1 in thread context under a ~one-line-time deadline: RAM code,
// SRAM data only, word-wide moves.
void __not_in_flash_func(line_callback)(int y, uint16_t* line, void*) {
    const uint8_t* src = g_src;
    uint32_t* d = reinterpret_cast<uint32_t*>(line);
    if (!src) {
        for (int i = 0; i < kPanelW / 2; ++i)
            d[i] = 0;
        return;
    }
    const uint16_t* lut = g_lut;
    if (g_src_w == kPanelW) {
        const int hy = y - g_head_start; // copy covers [g_head_start, g_head_start + g_head_rows)
        const uint8_t* row = (hy >= 0 && hy < g_head_rows) ? g_head_copy + hy * kPanelW : src + y * kPanelW;
        picodoom::convert_row_lut(d, row, lut);
    } else {
        picodoom::convert_row_lut_1p5(d, src + picodoom::source_row_1p5(y) * 320, lut);
    }
}
#endif

void __not_in_flash_func(core1_entry)() {
    g_lcd_ok = g_lcd.init(g_cfg);
    if (g_lcd_ok)
        g_lcd.set_backlight(255);
    g_core1_ready = true;
    // Everything from here runs from RAM only: Bluetooth bonding erases/
    // programs flash from core0 while this loop (and the scanout IRQs) keep
    // running -- see PICO_FLASH_ASSUME_CORE1_SAFE in CMakeLists.txt.
    while (true) {
#ifndef PICODOOM_PRESTO_PALETTE
        g_lcd.scanline_task();
#else
        __wfe();
#endif
    }
}

#ifndef PICODOOM_PRESTO_PALETTE
// Hi-res scanline: allocates the top-rows copy once the engine is up (first level
// frame). It must come AFTER screens[0]: the freed 144 KB menu surface is exactly the
// hole screens[0] needs, and taking part of it first fragments the heap (observed:
// "V_Init: allocate screens -> Out of memory"). Sized from the contiguous space above
// the heap break so it cannot starve the engine's later SRAM allocations either.
bool g_head_tried = false;
void try_alloc_head_copy();
#endif

uint64_t g_sync_wait_us_in_window = 0; // time spent waiting for the beam to reach the blank band

// Waits until the beam is in the black band below the game area (panel rows >= kVisibleH, including
// the porches), where nothing is read from the display buffer: a top-to-bottom copy started there
// stays ahead of the next frame's beam, so it never shows a seam. Bounded, in case scanout is down.
void wait_for_blank_band() {
    const uint64_t t0 = time_us_64();
    while (g_lcd.display_row() < kVisibleH && time_us_64() - t0 < 20'000)
        tight_loop_contents();
    g_sync_wait_us_in_window += time_us_64() - t0;
}

using pico_toolset::in_sram;
using pico_toolset::sram_free_estimate;

#ifndef PICODOOM_PRESTO_PALETTE
void try_alloc_head_copy() {
    g_head_tried = true;
    constexpr size_t kReserve = 6 * 1024; // slack: R_Init has already run by the first level frame
    const size_t top = pico_toolset::sram_contiguous_free();
    size_t rows = SCREENHEIGHT; // the whole frame if it fits, else the 3D view (everything above the status bar)
    rows = top > kReserve ? std::min<size_t>(rows, (top - kReserve) / kPanelW) : 0;
    if (rows) {
        g_head_copy = static_cast<uint8_t*>(malloc(rows * kPanelW));
        g_head_alloc_rows = g_head_copy ? rows : 0;
    }
    // Anchor the copy at the BOTTOM of the 3D view: the weapon, menu text and most sprites that
    // flicker are drawn last and low; the rows left out at the top are mostly ceiling/sky/walls.
    if (g_head_alloc_rows >= static_cast<size_t>(SCREENHEIGHT))
        g_head_start = 0; // whole frame: nothing is scanned from the live buffer any more
    else {
        if (g_head_alloc_rows > static_cast<size_t>(SCREENHEIGHT - 48))
            g_head_alloc_rows = SCREENHEIGHT - 48; // (free() not needed: just copy fewer rows)
        g_head_start = static_cast<int>((SCREENHEIGHT - 48) - g_head_alloc_rows);
    }
    printf("PicoDoom: hi-res scanline: tear-free copy of %u rows ending at the status bar (from row %d)\n", static_cast<unsigned>(g_head_alloc_rows), g_head_start);
}
#endif


// ---- stats ----------------------------------------------------------------
uint64_t g_last_frame_start_us = 0;
uint64_t g_stats_window_start_us = 0;
uint32_t g_frames_in_window = 0;
uint64_t g_frame_us_in_window = 0;
uint64_t g_copy_us_in_window = 0;
uint32_t g_prev_late = 0, g_prev_missed = 0;

void report_stats_if_due(uint64_t now_us) {
    constexpr uint64_t kIntervalUs = 10'000'000;
    if (g_stats_window_start_us == 0) {
        g_stats_window_start_us = now_us;
        return;
    }
    const uint64_t elapsed_us = now_us - g_stats_window_start_us;
    if (elapsed_us < kIntervalUs)
        return;

    const float fps = static_cast<float>(g_frames_in_window) * 1'000'000.0f / static_cast<float>(elapsed_us);
    const float copy_pct = g_frame_us_in_window
        ? 100.0f * static_cast<float>(g_copy_us_in_window) / static_cast<float>(g_frame_us_in_window) : 0.0f;
    const float sync_pct = g_frame_us_in_window
        ? 100.0f * static_cast<float>(g_sync_wait_us_in_window) / static_cast<float>(g_frame_us_in_window) : 0.0f;

    struct mallinfo mi = mallinfo();
    const size_t sram_total = pico_toolset::sram_heap_total();
    const size_t sram_used = static_cast<size_t>(mi.uordblks);
    const size_t psram_used = pico_toolset::psram_used_bytes();
    const size_t psram_total = pico_toolset::psram_status().size_bytes;

    float tic_pct = 0, render3d_pct = 0, draw2d_pct = 0, bsp_walls_pct = 0, planes_pct = 0, sprites_pct = 0;
    i_frame_stats_take(elapsed_us, &tic_pct, &render3d_pct, &draw2d_pct,
                        &bsp_walls_pct, &planes_pct, &sprites_pct);
    const uint32_t wad_cache_misses = i_frame_stats_take_wad_cache_misses();

    uint32_t late = 0, missed = 0;
#ifndef PICODOOM_PRESTO_PALETTE
    late = g_lcd.late_lines();
    missed = g_lcd.missed_frames();
#endif
    printf("PicoDoom: SRAM %u/%uKB  PSRAM %u/%uKB  FPS %.1f  present %.0f%%  beam-sync-wait %.0f%%  "
           "late-lines %u  missed-frames %u (per 10s)  "
           "tic %.0f%%  render3d %.0f%% (bsp+walls %.0f%%  planes %.0f%%  sprites %.0f%%)  draw2d %.0f%%  "
           "wad-cache-miss %u/10s\n",
           static_cast<unsigned>(sram_used / 1024), static_cast<unsigned>(sram_total / 1024),
           static_cast<unsigned>(psram_used / 1024), static_cast<unsigned>(psram_total / 1024),
           fps, copy_pct, sync_pct,
           static_cast<unsigned>(late - g_prev_late), static_cast<unsigned>(missed - g_prev_missed),
           tic_pct, render3d_pct, bsp_walls_pct, planes_pct, sprites_pct, draw2d_pct,
           static_cast<unsigned>(wad_cache_misses));
    g_prev_late = late;
    g_prev_missed = missed;

    g_stats_window_start_us = now_us;
    g_frames_in_window = 0;
    g_frame_us_in_window = 0;
    g_copy_us_in_window = 0;
    g_sync_wait_us_in_window = 0;
}

#ifdef PICODOOM_PRESTO_PALETTE
#endif

} // namespace

extern "C" {

void I_InitGraphics(void) {
    if (g_graphics_init_done)
        return;
    g_graphics_init_done = true;

    if (sram_free_estimate() < kSurfaceBytes)
        I_Error(const_cast<char*>("I_InitGraphics: no SRAM for the 480x300 surface"));
    g_surface = static_cast<uint8_t*>(malloc(kSurfaceBytes));
    if (!g_surface)
        I_Error(const_cast<char*>("I_InitGraphics: surface allocation failed"));
    memset(g_surface, 0, kSurfaceBytes);

    g_cfg = pico_toolset::configs::st7701::kPimoroniPrestoFullRes;
    g_cfg.format = kFormat;
    g_cfg.visible_height = kVisibleH;
#ifdef PICODOOM_PRESTO_PALETTE
    g_cfg.framebuffer = g_surface;
#else
    g_cfg.line_callback = line_callback;
#endif

    set_menu_palette();
#ifndef PICODOOM_PRESTO_PALETTE
    g_src_w = kPanelW;
    g_src = g_surface;
#endif

    multicore_reset_core1();
    multicore_launch_core1_with_stack(core1_entry, g_core1_stack, sizeof(g_core1_stack));
    const absolute_time_t timeout = make_timeout_time_ms(3000);
    while (!g_core1_ready && !time_reached(timeout))
        tight_loop_contents();
    if (!g_lcd_ok)
        printf("PicoDoom: ST7701 init FAILED (no free PIO state machine / DMA channel?)\n");
    else
        printf("PicoDoom: ST7701 480x480 up, mode %s, game area 480x%d, surface %u KB, gamma %.2f\n",
               kModeName, kVisibleH, static_cast<unsigned>(kSurfaceBytes / 1024), g_pal.gamma());
}

// Called once after the boot menu, before D_DoomMain().
void i_video_lcd_set_hires(int hires) {
    g_screenwidth = hires ? 480 : 320;
    g_screenheight = hires ? 300 : 200;

#ifdef PICODOOM_PRESTO_PALETTE
    memset(g_surface, 0, kSurfaceBytes); // index 0 is black in the menu palette
    // Hi-res: give the renderer its own screens[0] and copy it into the framebuffer once per
    // frame (tear-free apart from the ~0.3 ms copy), if the heap can hold a second 144 KB.
    // Otherwise screens[0] stays the framebuffer itself (zero copy, tears: see i_video_presto_screen0).
    g_double_buffer = hires && sram_free_estimate() >= kSurfaceBytes + 16 * 1024;
    if (hires)
        printf("PicoDoom: palette hi-res: %s\n", g_double_buffer
               ? "separate render buffer, copied to the framebuffer each frame (tear-free)"
               : "SRAM short, rendering straight into the framebuffer (tears)");
#else
    // Stop scanning the menu surface, give the callback a frame to leave it,
    // then hand its 144 KB back to the heap for screens[0] (and the low-res copy).
    g_src = nullptr;
    __dmb();
    sleep_ms(50);
    free(g_surface);
    g_surface = nullptr;
    if (!hires) {
        const size_t bytes = 320 * 200;
        g_lowres_copy = static_cast<uint8_t*>(sram_free_estimate() >= bytes ? malloc(bytes) : nullptr);
        if (!g_lowres_copy)
            I_Error(const_cast<char*>("i_video_lcd_set_hires: no SRAM for the low-res display copy"));
        memset(g_lowres_copy, 0, bytes);
    }
    g_head_rows = 0;
    g_src_w = SCREENWIDTH;
#endif
    __dmb();
    printf("PicoDoom: game frame %dx%d (%s)\n", SCREENWIDTH, SCREENHEIGHT, hires ? "high res" : "low res");
}

// doom/v_video.c V_Init() hook: Palette8 hi-res plays straight into the framebuffer.
byte* i_video_presto_screen0(size_t size) {
#ifdef PICODOOM_PRESTO_PALETTE
    if (SCREENWIDTH == kPanelW && size == kSurfaceBytes && !g_double_buffer)
        return g_surface;
#else
    (void)size;
#endif
    return nullptr;
}

// LVGL menu surface (480x300 RGB332 indices, stride 480).
uint8_t* i_video_presto_menu_surface(void) {
    return g_surface;
}

void I_ShutdownGraphics(void) {}

void I_SetPalette(byte* palette) {
    g_pal.set_palette(palette);
    rebuild_game_palette();
}

void I_UpdateNoBlit(void) {}

} // extern "C"

namespace {

// The per-frame copy into what the panel scans (see the header comment). Done once the beam is
// in the blank band, never mid-picture: I_FinishUpdate() only *requests* it, and the engine's
// idle time between frames (I_StartTic() is polled in a tight loop while it waits for the next
// tic) performs it as soon as the beam gets there; presto_video_flush(), called at the top of
// D_Display() (the only place that draws), blocks if it still has not happened -- the old
// unconditional wait, now rarely reached. screens[0] is stable until then: nothing draws
// between I_FinishUpdate() and the next D_Display().
bool g_copy_pending = false;

void do_copy() {
    const uint64_t t0 = time_us_64();
#ifdef PICODOOM_PRESTO_PALETTE
    if (g_double_buffer) {
        memcpy(g_surface, screens[0], kSurfaceBytes);
    } else if (SCREENWIDTH != kPanelW) {
        // 320x200 -> 480x300 into the framebuffer: source rows 2k -> panel rows 3k, 3k+1; 2k+1 -> 3k+2.
        const byte* src = screens[0];
        for (int k = 0; k < 100; ++k) {
            uint8_t* d = g_surface + (3 * k) * kPanelW;
            picodoom::expand_row_1p5(d, src + (2 * k) * 320);
            memcpy(d + kPanelW, d, kPanelW);
            picodoom::expand_row_1p5(d + 2 * kPanelW, src + (2 * k + 1) * 320);
        }
    }
#else
    if (SCREENWIDTH != kPanelW) {
        memcpy(g_lowres_copy, screens[0], 320 * 200);
        if (g_src != g_lowres_copy) {
            __dmb();
            g_src = g_lowres_copy;
        }
    } else {
        if (!g_head_tried && gamestate == GS_LEVEL)
            try_alloc_head_copy();
        if (g_head_alloc_rows) {
            memcpy(g_head_copy, screens[0] + g_head_start * kPanelW, g_head_alloc_rows * kPanelW);
            g_head_rows = static_cast<int>(g_head_alloc_rows);
        }
        if (g_src != screens[0]) {
            if (!in_sram(screens[0]))
                printf("PicoDoom: WARNING screens[0] is not in SRAM -- scanline565 hi-res cannot scan it\n");
            __dmb();
            g_src = screens[0];
        }
    }
#endif
    g_copy_us_in_window += time_us_64() - t0;
}

bool copy_needed() {
#ifdef PICODOOM_PRESTO_PALETTE
    return g_double_buffer || SCREENWIDTH != kPanelW;
#else
    return true;
#endif
}

} // namespace

extern "C" {

// Non-blocking: performs the pending copy if the beam is in the blank band right now.
void presto_video_poll(void) {
    if (g_copy_pending && g_lcd.display_row() >= kVisibleH) {
        do_copy();
        g_copy_pending = false;
    }
}

// Blocking: the copy must be done before anything draws into screens[0] again.
void presto_video_flush(void) {
    if (!g_copy_pending)
        return;
    wait_for_blank_band();
    do_copy();
    g_copy_pending = false;
}

void I_FinishUpdate(void) {
    const uint64_t frame_start_us = time_us_64();
    if (g_last_frame_start_us != 0)
        g_frame_us_in_window += frame_start_us - g_last_frame_start_us;
    g_last_frame_start_us = frame_start_us;

    presto_video_flush(); // a previous request nobody completed (init screens, wipes)
    if (copy_needed()) {
        g_copy_pending = true;
        presto_video_poll();
    }
    ++g_frames_in_window;
    report_stats_if_due(frame_start_us);
}

void I_ReadScreen(byte* scr) {
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

float i_video_gamma(void) {
    return g_pal.gamma();
}

void i_video_set_gamma(float gamma) {
    g_pal.set_gamma(gamma);
    rebuild_game_palette();
    printf("PicoDoom: gamma %.2f\n", g_pal.gamma());
}

void I_StartFrame(void) {}

} // extern "C"
