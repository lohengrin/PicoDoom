// Real video driver for the Pico port: replaces doom/i_video_null.c.
// PICODOOM_VIDEO_OUTPUT=lcd-st7796 -- the ST7796U panel replacing the
// ILI9486 one on the same Waveshare RP2350-PiZero board/header wiring (same
// XPT2046 touch controller, same GPIO/SPI pins; only the panel controller
// chip changed). NOT YET HARDWARE-VALIDATED as of this file's addition --
// prepared ahead of the physical panel swap so the build variant exists and
// compiles cleanly; bench-confirm the usual first-boot suspects (MADCTL
// orientation, SPI clock ceiling, the banding tuning below) once the panel
// is in hand.
//
// Structure, banding architecture, and the whole core0/core1 blit pipeline
// are copied verbatim from src/i_video_ili9486.cpp (see that file for the
// full performance history -- Phase 4.5 through Phase 8's native-480x300
// banded blit) since both panels share the same DisplayPanel contract and
// this project's SCREENWIDTH/SCREENHEIGHT are unaffected by which LCD panel
// is attached. Two differences from that file:
//   1. St7796 (unlike Ili9486) is config-driven for width/height rather than
//      compile-time constants (kWidth/kHeight) -- ST7796U ships in several
//      panel sizes, so g_offset_x/g_offset_y are computed at runtime in
//      I_InitGraphics() instead of being namespace-scope constexpr.
//   2. board_config.hpp's st7796_config() starts at the SAME 33.33MHz SPI
//      clock the validated ILI9486 preset runs at (by instruction: "keep
//      actual frequency" for the first implementation), not this panel's
//      much higher 125MHz spec ceiling -- raise it once bench-tested via
//      i_video_set_pixel_clock_hz() below, the same live-tuning path the
//      ILI9486 build's serial console `pclk` command already uses.
// kBandRows (100, 3 bands/frame) is carried over from the ILI9486 file's
// own hardware-tuned value; re-measure and retune once this panel is
// actually running (a different controller/wiring may have a different
// sweet spot between per-band command overhead and SRAM footprint -- see
// that constant's own comment in i_video_ili9486.cpp for the tuning story).
#include "pico_toolset/st7796.h"
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

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"

// Linker-provided symbols (pico-sdk's memmap linker script), not this file's
// -- extern "C" so the namespace below doesn't mangle the names we need to
// match.
extern "C" char __StackLimit;
extern "C" char __bss_end__;

namespace {
pico_toolset::St7796 g_display;
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
// through the interface type instead of the concrete St7796 -- see
// i_video_ili9486.cpp's identically-shaped g_panel for the rationale.
// init()/pixel_clock_hz()/set_pixel_clock_hz()/pixel_clock_actual_hz() stay
// on the concrete g_display -- config types and clock tuning are
// driver-specific, not part of DisplayPanel by design.
pico_toolset::DisplayPanel& g_panel = g_display;
uint16_t g_rgb565_wire_lut[256];
// Gamma factor applied at RGB565-LUT-build time (set from the serial console,
// src/i_serial_console.cpp; F3/F4 keys that used to tune it are freed for
// vanilla DOOM) -- this whole file is lcd-st7796-build-only, see
// CMakeLists.txt. Defaults to 1.4, matching the ILI9486 build's validated
// baseline -- re-tune once this panel's own color response is known,
// nothing guarantees the same factor looks right on a different panel.
// Core0-owned: written by I_SetPalette()/i_video_set_gamma(), read only --
// indirectly, via the LUT -- in I_FinishUpdate(). core1 never sees either
// (the blit is pure DMA of already-converted buffers), so no cross-core
// synchronization is needed.
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
        // pico_toolset::St7796::write_pixels()).
        g_rgb565_wire_lut[i] = static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8));
    }
}

constexpr int kDstWidth = SCREENWIDTH;   // 480, native (see doom/doomdef.h)
constexpr int kDstHeight = SCREENHEIGHT; // 300
// Unlike Ili9486::kWidth/kHeight, St7796 has no compile-time size constants
// (config-driven, see st7796.h) -- computed once in I_InitGraphics() after
// g_display.init(), before anything (including i_video_core1_step()) reads
// them.
int g_offset_x = 0;
int g_offset_y = 0;

// --- core0 -> core1 blit handoff (banded ping-pong) ---
// See i_video_ili9486.cpp's identically-shaped block for the full
// SRAM-budget rationale (two full-frame buffers don't fit SRAM at all;
// banding bounds each buffer to kBandRows worth of rows instead) and the
// kBandRows tuning story (20 rows/15 bands measured worse than the
// full-frame PSRAM fallback it replaced, due to per-band set_window()
// command overhead; 100 rows/3 bands cuts that 5x). Carried over unchanged
// as a starting point -- re-measure on this panel once it's in hand.
constexpr int kBandRows = 100;
static_assert(kDstHeight % kBandRows == 0, "kBandRows must divide kDstHeight evenly");
constexpr int kNumBands = kDstHeight / kBandRows;
constexpr int kBandPixels = kDstWidth * kBandRows;

uint16_t* g_screen_buf[2] = {nullptr, nullptr};
// true = free for core0 to write screens[0]'s converted pixels into. Flip
// conventions match PicoUsbKeyboard.cpp's g_active_buf: plain volatile
// bool, one writer per flag direction (core0 only ever clears its target
// index, core1 only ever sets the index it just finished), safe on this
// platform without a lock.
volatile bool g_blit_buf_free[2] = {true, true};

// --- Stats line (SRAM/PSRAM usage, FPS, timing breakdown) ---
// See i_video_ili9486.cpp's identically-shaped report_stats_if_due() for
// the full doc comment on what each field means.
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
    // malloc): banding keeps kBlitBytes a small constant (kBandRows worth of
    // rows, not the whole frame) so this should always succeed on SRAM
    // regardless of SCREENWIDTH/SCREENHEIGHT -- but keep the PSRAM fallback
    // as a defensive backstop.
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
    // can still grow all the way up to __StackLimit -- see
    // i_video_ili9486.cpp's identically-shaped comment for the real-hardware
    // confirmation of this on the ILI9486 build. uordblks (bytes actually
    // allocated) minus the total SRAM heap budget is accurate regardless of
    // arena growth state -- same computation report_stats_if_due uses for
    // the "SRAM used/total" stats line.
    constexpr size_t kBlitBytes = kBandPixels * sizeof(uint16_t);
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

    g_display.init(picodoom::st7796_config());
    // St7796's width()/height() are only known after init() (config-driven,
    // unlike Ili9486::kWidth/kHeight) -- compute the letterbox offset here,
    // before anything (including i_video_core1_step(), which runs on core1
    // once blits start flowing) reads g_offset_x/g_offset_y.
    g_offset_x = (g_display.width() - kDstWidth) / 2;
    g_offset_y = (g_display.height() - kDstHeight) / 2;
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

    // screens[0] itself is never touched here (see i_video_ili9486.cpp's
    // Phase 4.6.2 note on why not) -- just read out. One band at a time:
    // fused copy+palette-to-RGB565 convert does one pass per pixel,
    // kBandRows worth at a time instead of the whole frame, so both
    // wait-for-slot backpressure and the SPI feed interleave at band
    // granularity instead of once per frame.
    const byte* src = screens[0];
    for (int band = 0; band < kNumBands; ++band) {
        // Backpressure: only blocks if core1 hasn't finished DMA'ing this
        // same slot's previous band yet, i.e. if core1's SPI feed is the
        // bottleneck rather than core0's game logic/render.
        uint64_t wait_start_us = time_us_64();
        while (!g_blit_buf_free[next_idx])
            tight_loop_contents();
        uint64_t convert_start_us = time_us_64();
        g_wait_us_in_window += convert_start_us - wait_start_us;

        const byte* band_src = src + static_cast<size_t>(band) * kBandPixels;
        uint16_t* dst = g_screen_buf[next_idx];
        for (int i = 0; i < kBandPixels; ++i)
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
// vanilla DOOM). Same live-tuning contract as the ILI9486 build's
// identically-named functions: find the panel/wiring's real corruption
// ceiling on actual hardware instead of guessing from a datasheet --
// especially relevant here, since board_config.hpp's st7796_config()
// deliberately starts conservative (33.33MHz) well below this panel's
// 125MHz spec ceiling. pico_toolset::St7796::set_pixel_clock_hz() takes
// effect on the next set_window() call (core1's i_video_core1_step()), not
// immediately.
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
// this panel's SPI instance for the XPT2046). Returns the DisplayPanel&
// base, not the concrete St7796& -- same contract as
// i_video_ili9486.cpp's identically-named accessor, so
// src/lvgl_display_lcd.cpp and src/wad_menu.cpp work unchanged regardless
// of which of the two is compiled in for this build. Boot-menu-only, core0;
// the game's own core1 blit never goes through it.
pico_toolset::DisplayPanel& i_video_lcd_display(void) {
    return g_display;
}

// I_StartTic lives in src/i_input_usbhid.cpp (Phase 3, USB-PIO keyboard).
void I_StartFrame(void) {}

} // extern "C"

namespace {
// core1-side blit state machine: pure DMA, no conversion left on this core
// (that happens in I_FinishUpdate() on core0). Idle -> Transferring (poll
// until the non-blocking DMA finishes) -> Idle each band. Stepped once per
// call by i_video_core1_step() -- see i_video_core1.hpp for why this can't
// just be "pop a band, DMA it and block until done": core1's caller
// (PicoUsbKeyboard::core1_entry()) needs to get back to tuh_task() every
// iteration, not just between whole bands, or Pico-PIO-USB's software-timed
// bus servicing starves for however long a band's SPI feed takes.
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
                            g_offset_x + kDstWidth - 1, y0 + kBandRows - 1);
        g_dma_start_us = time_us_64();
        g_panel.start_pixels_dma(std::span<const uint16_t>(g_screen_buf[g_blit_idx], kBandPixels));
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
