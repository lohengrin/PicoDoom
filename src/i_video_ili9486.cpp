// Real video driver for the Pico port: replaces doom/i_video_null.c.
// Blits DOOM's screens[0] (palette-indexed, native SCREENWIDTHxSCREENHEIGHT
// -- 480x300, see doom/doomdef.h) to the Waveshare 3.5" ILI9486 LCD via
// Pico-Toolset's pico_toolset::Ili9486, 1:1, centered on the panel's 480x320
// landscape frame in a small top/bottom letterbox (kOffsetY, currently 10px
// each side -- see docs/PLAN.md). An earlier 320x200 build tried a 3:2
// blit-time upscale to fill the panel and reverted it (SPI feed time scales
// with byte count, so more bytes was strictly worse at the time); this is a
// different thing -- the engine itself now renders natively at 480x300 (see
// doom/doomdef.h's UI_SCALE), so there's no upscale step here at all, just
// the same 1:1 blit at a bigger native size. C++ driver behind a plain C
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
// Hardware-validated (did not regress) at 320x200; still in place unchanged
// through Phase 8's native-480x300 banding below, which just calls this
// same fused convert loop once per band instead of once per frame.
//
// Phase 8 (2026-09, native 480x300, see doomdef.h): current measured floor
// on this panel is ~12fps, core1-dma pinned ~99% -- i.e. genuinely SPI-bus-
// bandwidth-bound (480*300*2 = 288000B/frame at this panel's 33MHz clean
// ceiling is ~70ms of transfer time alone, a ~14.3fps hard cap regardless
// of buffering strategy). Banding (kBandRows below) got buffers back into
// SRAM and off the PSRAM-fallback path, but doesn't and can't beat this
// bus-bandwidth ceiling -- the next real lever is the higher-SPI-clock
// panel swap this native-resolution work was originally motivated by.
#include "pico_toolset/ili9486.h"
#include "pico_toolset/display_panel.h"
#include "pico_toolset/psram.h"
#include "i_video_core1.hpp"
#include "board_config.hpp"
#include "i_frame_stats.hpp"

extern "C" {
#include "doomdef.h"
#include "i_video.h"
#include "i_system.h"
#include "v_video.h"
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <span>

#include "hardware/sync.h"
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
// Once-guard: I_InitGraphics() is invoked by both the boot WAD-selection
// menu (Phase 4, src/wad_menu.cpp) and the engine's own startup
// (D_DoomMain() -> I_InitGraphics) -- the second call must be a no-op: the
// panel was already brought up and the blit buffers already allocated (and
// g_screen_buf's SRAM/PSRAM fallback already decided). Without it, the boot
// menu would work but the game boot would re-init over a live core1.
bool g_graphics_init_done = false;
// Routes every call this file makes that's part of the shared
// pico_toolset::DisplayPanel contract (set_window/write_pixels/
// start_pixels_dma/pixels_busy/finish_pixels_dma/end_write/fill_solid)
// through the interface type instead of the concrete Ili9486 -- so a future
// board using a different DisplayPanel (e.g. St7789) only needs g_display's
// declaration/init() call and the ILI9486-specific pixel-clock-tuning calls
// below changed, not this file's render loop. init()/pixel_clock_hz()/
// set_pixel_clock_hz()/pixel_clock_actual_hz() stay on the concrete
// g_display -- config types and clock tuning are driver-specific, not part
// of DisplayPanel by design.
pico_toolset::DisplayPanel& g_panel = g_display;
uint16_t g_rgb565_wire_lut[256];
// Gamma factor applied at RGB565-LUT-build time (set from the serial console,
// src/i_serial_console.cpp; F3/F4 keys that used to tune it are freed for
// vanilla DOOM) -- this whole file is LCD-build-only
// (PICODOOM_VIDEO_OUTPUT=lcd), see CMakeLists.txt. Defaults to 1.4 (a
// slight brighten -- gammatable[usegamma]'s + gamma table reads a bit dark
// on this panel; kept as the baseline); pressing down to exactly 1.0
// engages an identity fast path in rebuild_rgb565_lut() so an uncorrected
// palette is byte-identical to the pre-gamma build.
// Core0-owned: written by I_SetPalette()/i_video_set_gamma(), read only --
// indirectly, via the LUT -- in I_FinishUpdate(). core1 never sees either
// (the Phase 6 blit is pure DMA of already-converted buffers), so no
// cross-core synchronization is needed.
float g_gamma = 1.4f;
// Raw PLAYPAL byte data from the last I_SetPalette() -- kept so a serial
// 'gamma' command can rebuild the LUT at a new factor without the engine
// re-handing the palette over (it only calls I_SetPalette() when it itself
// changes the palette, e.g. level start or the options-menu gamma setting).
byte g_palette_cache[256 * 3];

// Core0-only (see g_gamma/g_palette_cache above). Runs the cached raw
// palette through gamma + gammatable[usegamma] into the wire-order RGB565
// LUT I_FinishUpdate() indexes per pixel. Rebuilt wholesale on every
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
        uint16_t rgb565 = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        // Byte-swapped for MSB-first-over-SPI wire order (see
        // pico_toolset::Ili9486::write_pixels()).
        g_rgb565_wire_lut[i] = static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8));
    }
}

// The game's frame size is chosen at boot (320x200 or 480x300, see
// i_video_lcd_set_hires() below and doom/doomdef.h's SCREENWIDTH comment) --
// SCREENWIDTH/SCREENHEIGHT are runtime globals here, so what used to be
// constexprs derived from them are runtime values (refreshed by
// i_video_lcd_set_hires(), defaults = the largest mode until then), or sized
// for the LARGEST mode (MAX_SCREENWIDTH/HEIGHT) where something has to be
// allocated once up front. Letterbox offsets: 480x300 -> (0,10), 320x200 ->
// (80,60) on this 480x320 panel.
int g_offset_x = (pico_toolset::Ili9486::kWidth - MAX_SCREENWIDTH) / 2;
int g_offset_y = (pico_toolset::Ili9486::kHeight - MAX_SCREENHEIGHT) / 2;

// --- core0 -> core1 blit handoff (banded ping-pong) ---
// Phase 8 (2026-09, native 480x300): two FULL-FRAME buffers (480*300*2 =
// 288000B each) don't fit in SRAM at all -- bigger than the entire SRAM
// heap budget outright, not just tight -- and I_InitGraphics's psram_malloc
// fallback, while it keeps the boot alive, leaves core1 DMA'ing straight
// out of PSRAM every frame, measured ~12fps with core1-dma pinned at
// ~100%. Banding fixes this at the root: each buffer only ever holds
// kBandRows rows (see kBandRows below), a small constant independent of
// screen resolution, so both slots fit SRAM again regardless of how big
// SCREENWIDTH/SCREENHEIGHT are. I_FinishUpdate() below now converts and
// pushes one band at a time instead of the whole frame; i_video_core1_step()
// DMAs one band at a time to a correspondingly small window on the panel.
// Same ping-pong contract as before (g_blit_buf_free), just at band
// granularity instead of frame granularity -- core0 blocks on a slot only
// if core1's SPI feed hasn't drained that slot's *previous* band yet.
// 20 rows (15 bands/frame) was tried first and measured WORSE on real
// hardware (~9fps, core0-wait 60%) than the full-frame PSRAM fallback it
// replaced (~9-12fps) -- each band pays pico_toolset::Ili9486::set_window()'s
// full command sequence (CASET+PASET+RAMWR, 11 separate CS-toggle SPI
// transactions at the 8MHz command baud, plus two spi_set_baudrate() calls
// to switch to/from the pixel clock), and at 15 bands/frame that fixed
// per-band cost was apparently big enough to outweigh what banding saved.
// Total bytes moved over SPI per frame is unchanged by band count (that's
// set by SCREENWIDTH*SCREENHEIGHT, not by this), so fewer/bigger bands
// only pays down that per-call command overhead -- 100 rows (3 bands/frame)
// cuts it 5x vs the 15-band attempt while still using far less SRAM
// (96000B/buffer, 192000B total) than a full frame would (288000B/buffer).
constexpr int kBandRows = 100;
// Both supported frame heights (200 and 300) must split into whole bands, and
// the blit buffers below are sized for the widest mode.
static_assert(MAX_SCREENHEIGHT % kBandRows == 0, "kBandRows must divide the tallest frame height evenly");
static_assert(200 % kBandRows == 0, "kBandRows must divide the 320x200 frame height evenly");
constexpr int kMaxBandPixels = MAX_SCREENWIDTH * kBandRows;
// Per-mode band geometry, refreshed by i_video_lcd_set_hires() -- read by
// core0 (I_FinishUpdate) and core1 (i_video_core1_step), which is why they're
// plain ints set once before any blit starts rather than derived per call.
int g_num_bands = MAX_SCREENHEIGHT / kBandRows;
int g_band_pixels = MAX_SCREENWIDTH * kBandRows;
int g_dst_width = MAX_SCREENWIDTH;

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

    float tic_pct = 0.0f, render3d_pct = 0.0f, draw2d_pct = 0.0f;
    float bsp_walls_pct = 0.0f, planes_pct = 0.0f, sprites_pct = 0.0f;
    i_frame_stats_take(elapsed_us, &tic_pct, &render3d_pct, &draw2d_pct,
                        &bsp_walls_pct, &planes_pct, &sprites_pct);
    uint32_t wad_cache_misses = i_frame_stats_take_wad_cache_misses();

    printf("PicoDoom: SRAM %u/%uKB  PSRAM %u/%uKB  FPS %.1f  "
           "core0-wait %.0f%%  core0-convert %.0f%%  core1-dma %.0f%%  "
           "tic %.0f%%  render3d %.0f%% (bsp+walls %.0f%%  planes %.0f%%  sprites %.0f%%)  draw2d %.0f%%  "
           "wad-cache-miss %u/10s\n",
           static_cast<unsigned>(sram_used / 1024), static_cast<unsigned>(sram_total / 1024),
           static_cast<unsigned>(psram_used / 1024), static_cast<unsigned>(psram_total / 1024),
           fps, wait_pct, convert_pct, dma_pct, tic_pct, render3d_pct,
           bsp_walls_pct, planes_pct, sprites_pct, draw2d_pct,
           static_cast<unsigned>(wad_cache_misses));

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
    if (g_graphics_init_done)
        return;
    g_graphics_init_done = true;

    // core1 is already running PicoUsbKeyboard's loop by this point (started
    // from src/PicoDoom.cpp before D_DoomMain()) and will start calling
    // i_video_core1_step() immediately -- but g_blit_buf_free starts all-true
    // and the inter-core FIFO starts empty, so it just no-ops until
    // I_FinishUpdate() below ever pushes an index. Both slots SRAM (newlib
    // malloc -- see g_screen_buf's doc comment): banding (Phase 8) keeps
    // kBlitBytes a small constant (kBandRows worth of rows, not the whole
    // frame) so this should always succeed on SRAM now regardless of
    // SCREENWIDTH/SCREENHEIGHT -- but keep the PSRAM fallback as a defensive
    // backstop.
    //
    // Don't just try malloc() and catch NULL: this SDK's pico_malloc wrapper
    // has PICO_MALLOC_PANIC=1 by default (project doesn't override it, and
    // shouldn't -- that panic is a genuinely useful diagnostic for a real
    // unexpected OOM elsewhere), so a malloc() call that's going to fail
    // panics immediately instead of returning NULL, and this function's own
    // NULL-check/PSRAM-fallback code below never gets a chance to run --
    // check headroom first and skip straight to psram_malloc() when there's
    // clearly not enough room, instead of finding out via a panic.
    //
    // mallinfo()'s fordblks (free bytes already inside the newlib arena) is
    // the WRONG thing to check here: this runs very early in boot (before
    // WAD loading, one of the first mallocs at all), so the arena hasn't
    // grown via sbrk() yet and fordblks reads near-zero even though sbrk()
    // can still grow all the way up to __StackLimit -- using it made this
    // always take the PSRAM path regardless of how small kBlitBytes got
    // (confirmed on real hardware: still PSRAM-fallback after banding
    // dropped kBlitBytes to ~19KB with 293KB actually free). uordblks (bytes
    // actually allocated) minus the total SRAM heap budget is accurate
    // regardless of arena growth state -- same computation report_stats_if_due
    // uses for the "SRAM used/total" stats line.
    constexpr size_t kBlitBytes = kMaxBandPixels * sizeof(uint16_t);
    bool all_sram = true;
    for (uint16_t*& buf : g_screen_buf) {
        struct mallinfo mi = mallinfo();
        size_t sram_total = static_cast<size_t>(&__StackLimit - &__bss_end__);
        size_t sram_used = static_cast<size_t>(mi.uordblks);
        size_t sram_free_estimate = sram_total > sram_used ? sram_total - sram_used : 0;
        buf = (sram_free_estimate >= kBlitBytes)
            ? static_cast<uint16_t*>(malloc(kBlitBytes))
            : nullptr;
        if (!buf) {
            buf = static_cast<uint16_t*>(pico_toolset::psram_malloc(kBlitBytes));
            all_sram = false;
        }
        if (!buf)
            // I_Error's signature predates `const` (1993 C) -- cast, not a
            // real mutation.
            I_Error(const_cast<char*>("I_InitGraphics: failed to allocate blit buffer"));
        // Not load-bearing (I_FinishUpdate() always fills a slot before it's
        // ever handed to core1), just avoids a stray uninitialized-SRAM/PSRAM
        // read if that ever stops being true.
        memset(buf, 0, kBlitBytes);
    }
    puts(all_sram ? "PicoDoom: blit buffers in SRAM"
                  : "PicoDoom: blit buffers in PSRAM (SRAM heap short, fallback)");

    g_display.init(picodoom::ili9486_config());
    // Black out the whole panel once, independent of any palette/game state
    // -- confirms the panel is alive and gives a clean border around the
    // centered scaled game viewport that I_FinishUpdate never touches. Runs
    // here on core0, synchronously, before D_DoomMain() ever calls
    // I_FinishUpdate() -- the one and only time core0 touches g_display;
    // every call after this is from core1 (i_video_core1_step()).
    g_panel.fill_solid(0);
    // Boot-time clock report (see PicoDoom.cpp's clk_sys/clk_peri/flash line
    // above) -- fill_solid() already called set_window() at least once, so
    // pixel_clock_actual_hz() reflects the real applied rate, not just the
    // static config's request.
    printf("PicoDoom: SPI pixel clock requested=%u Hz  actual=%u Hz  gamma %.2f\n",
           (unsigned)g_display.pixel_clock_hz(), (unsigned)g_display.pixel_clock_actual_hz(), g_gamma);
}

// Applies the boot menu's "High res" choice: 480x300 (hires) or 320x200.
// Called exactly once, by src/PicoDoom.cpp after wad_menu_run() returns and
// before D_DoomMain() -- see doom/doomdef.h's SCREENWIDTH comment for what
// that ordering has to guarantee (nothing sized from SCREENWIDTH/HEIGHT may
// have been allocated or cached yet). Runs on core0 while core1 is idle
// (nothing has been pushed to the blit FIFO yet), so it's safe to touch the
// panel directly here, same as I_InitGraphics()'s one-time fill_solid().
void i_video_lcd_set_hires(int hires) {
    g_screenwidth = hires ? 480 : 320;
    g_screenheight = hires ? 300 : 200;

    g_dst_width = SCREENWIDTH;
    g_num_bands = SCREENHEIGHT / kBandRows;
    g_band_pixels = SCREENWIDTH * kBandRows;
    g_offset_x = (pico_toolset::Ili9486::kWidth - SCREENWIDTH) / 2;
    g_offset_y = (pico_toolset::Ili9486::kHeight - SCREENHEIGHT) / 2;

    // The boot menu painted the whole panel; a 320x200 game window is
    // smaller than that, and I_FinishUpdate() only ever writes inside the
    // window, so anything the menu left in the border (skip/cancel exits tear
    // the menu down without a full-screen "Loading..." repaint) would stay
    // on screen for the whole game. Clear it once, now.
    g_panel.fill_solid(0);

    // core1 reads g_offset_*/g_dst_width/g_band_pixels the moment the first
    // band reaches it via the FIFO; make sure these stores are visible first.
    __dmb();

    printf("PicoDoom: game frame %dx%d (%s), panel offset %d,%d\n",
           SCREENWIDTH, SCREENHEIGHT, hires ? "high res" : "low res", g_offset_x, g_offset_y);
}

void I_ShutdownGraphics(void) {}

// Takes full 8 bit values (i_video.h).
void I_SetPalette(byte* palette) {
    memcpy(g_palette_cache, palette, sizeof(g_palette_cache));
    rebuild_rgb565_lut();
}

void I_UpdateNoBlit(void) {}

void I_FinishUpdate(void) {
    // Ping-pong: alternate which buffer each band targets. Static local
    // instead of a namespace global -- this function is the only writer,
    // nothing else needs to see it.
    static int next_idx = 0;

    uint64_t frame_start_us = time_us_64();
    if (g_last_frame_start_us != 0)
        g_frame_us_in_window += frame_start_us - g_last_frame_start_us;
    g_last_frame_start_us = frame_start_us;

    // screens[0] itself is never touched here (see file header) -- just
    // read out. One band at a time (Phase 8, see g_screen_buf's doc
    // comment): fused copy+palette-to-RGB565 convert (Phase 6) still does
    // one pass per pixel, just kBandRows worth at a time instead of the
    // whole frame, so both wait-for-slot backpressure and the SPI feed
    // interleave at band granularity instead of once per frame.
    const byte* src = screens[0];
    // Band geometry for the current frame mode (set once, before any blit,
    // by i_video_lcd_set_hires()); read into locals so the loop below keeps
    // the same shape/codegen as when these were compile-time constants.
    const int num_bands = g_num_bands;
    const int band_pixels = g_band_pixels;
    for (int band = 0; band < num_bands; ++band) {
        // Backpressure: only blocks if core1 hasn't finished DMA'ing this
        // same slot's previous band yet, i.e. if core1's SPI feed is the
        // bottleneck rather than core0's game logic/render -- see
        // g_wait_us's doc comment above.
        uint64_t wait_start_us = time_us_64();
        while (!g_blit_buf_free[next_idx])
            tight_loop_contents();
        uint64_t convert_start_us = time_us_64();
        g_wait_us_in_window += convert_start_us - wait_start_us;

        const byte* band_src = src + static_cast<size_t>(band) * band_pixels;
        uint16_t* dst = g_screen_buf[next_idx];
        for (int i = 0; i < band_pixels; ++i)
            dst[i] = g_rgb565_wire_lut[band_src[i]];
        uint64_t convert_done_us = time_us_64();
        g_convert_us_in_window += convert_done_us - convert_start_us;

        g_blit_buf_free[next_idx] = false;
        // Pack (band, slot) into one FIFO word -- i_video_core1_step()
        // decodes both to pick the right window offset and free-flag.
        multicore_fifo_push_blocking((static_cast<uint32_t>(band) << 1) | static_cast<uint32_t>(next_idx));
        next_idx ^= 1;
    }

    uint64_t now_us = time_us_64();
    ++g_frames_in_window;
    report_stats_if_due(now_us);
}

void I_ReadScreen(byte* scr) {
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

// SPI pixel-clock accessors for the serial console
// (src/i_serial_console.cpp; F1/F2 keys that used to tune this are freed for
// vanilla DOOM). 2026-09 performance work: live tuning so the actual
// hardware corruption ceiling can be found by hand instead of guessed from
// a datasheet. pico_toolset::Ili9486::set_pixel_clock_hz() takes effect on
// the next set_window() call (core1's i_video_core1_step()), not
// immediately -- see that method's own doc comment on why calling it from
// core0 here, while core1 runs the blit loop, needs no extra
// synchronization.
uint32_t i_video_pixel_clock_hz(void) {
    return g_display.pixel_clock_hz();
}

void i_video_set_pixel_clock_hz(uint32_t hz) {
    constexpr uint32_t kMinPixelClockHz = 1'000'000;
    uint32_t next = hz < kMinPixelClockHz ? kMinPixelClockHz : hz;
    g_display.set_pixel_clock_hz(next);
    printf("PicoDoom: SPI pixel clock requested=%u Hz  last actual=%u Hz\n",
           static_cast<unsigned>(next), static_cast<unsigned>(g_display.pixel_clock_actual_hz()));
}

// Gamma-factor accessors for the serial console (src/i_serial_console.cpp;
// F3/F4 keys that used to tune this are freed for vanilla DOOM). Adjusts the
// factor I_SetPalette()'s LUT was built at -- see rebuild_rgb565_lut() above
// for what that does. Rebuilding from the cached raw PLAYPAL means the
// engine is never involved and the new setting takes effect from the very
// next I_FinishUpdate(); the value is echoed to the serial console, same
// pattern as the pixel-clock report. The setter clamps to [0.1, 10].
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

// LCD-display accessor for the boot WAD-selection menu (Phase 4,
// src/wad_menu.cpp): hands the panel to the LVGL display driver
// (src/lvgl_display_lcd.cpp) and to the touch-screen init (which reuses
// this panel's SPI instance for the XPT2046, LCD build only). Returns the
// DisplayPanel& base, not the concrete Ili9486& -- the menu and touch init
// only need the shared windowed/DMA-streaming contract, so the same
// call sites work unchanged in src/i_video_st7796.cpp's identically-named
// accessor. Boot-menu-only, core0; the game's own core1 blit never goes
// through it.
pico_toolset::DisplayPanel& i_video_lcd_display(void) {
    return g_display;
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
        uint32_t word = multicore_fifo_pop_blocking();
        g_blit_idx = static_cast<int>(word & 1u);
        int band = static_cast<int>(word >> 1);
        int y0 = g_offset_y + band * kBandRows;
        g_panel.set_window(g_offset_x, y0,
                            g_offset_x + g_dst_width - 1, y0 + kBandRows - 1);
        g_dma_start_us = time_us_64();
        g_panel.start_pixels_dma(std::span<const uint16_t>(g_screen_buf[g_blit_idx], g_band_pixels));
        g_blit_state = BlitState::Transferring;
        return;
    }

    // Transferring: don't block here -- return immediately if the DMA is
    // still running so the caller's loop gets back to tuh_task() before
    // checking again next iteration.
    if (g_panel.pixels_busy())
        return;

    g_panel.finish_pixels_dma();
    g_core1_dma_us_accum += time_us_64() - g_dma_start_us;
    g_panel.end_write();
    // Release last: only after set_window/DMA/finish/end_write are all done
    // does core0 get to write this buffer's next frame.
    g_blit_buf_free[g_blit_idx] = true;
    g_blit_state = BlitState::Idle;
}
