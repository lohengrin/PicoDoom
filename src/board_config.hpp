#pragma once

// PicoDoom-specific clock tuning, layered on top of Pico-Toolset's generic
// Waveshare RP2350-PiZero presets (pico_toolset::configs::psram::
// kWaveshareRp2350PiZero, pico_toolset::configs::ili9486::
// kWaveshareRp2350PiZero) rather than mutating those shared defaults in
// place -- those stay the toolset's board-standard, hardware-validated
// baseline for any consumer that hasn't opted into PicoDoom's own
// overclocking. This project raises clk_sys from the RP2350 default
// 150MHz to 200MHz and re-sources clk_peri from it (see PicoDoom.cpp)
// specifically to reach the higher clocks below safely; a consumer at the
// toolset's stock clocks should use the stock presets unmodified, not
// these values.
//
// 2026-09 performance work.

#include "pico_toolset/ili9486_configs.h"
#include "pico_toolset/psram_configs.h"

namespace picodoom {

inline pico_toolset::PsramConfig psram_config() {
    pico_toolset::PsramConfig c = pico_toolset::configs::psram::kWaveshareRp2350PiZero;
    // Divisor=2 at PicoDoom.cpp's 200MHz clk_sys lands exactly on the
    // chip's ~100MHz spec ceiling, with no rounding. Requires that
    // overclock -- this value means something different (a different,
    // unintended divisor) at any other clk_sys; see
    // pico_toolset::psram_set_clock_hz()'s doc comment (psram.h) for the
    // divisor math if reusing this at a different clock.
    c.max_clock_hz = 100'000'000;
    return c;
}

inline pico_toolset::Ili9486Config ili9486_config() {
    pico_toolset::Ili9486Config c = pico_toolset::configs::ili9486::kWaveshareRp2350PiZero;
    // Live-tuned via F1/F2 (i_video_bump_pixel_clock_hz(),
    // src/i_video_ili9486.cpp) with clk_peri at PicoDoom.cpp's full
    // 200MHz clk_sys: confirmed clean at 33.33MHz (clk_peri/6, the
    // nearest EVEN SPI-divider step -- CPSDVSR only supports even
    // values, so odd divisors like 5 (which would give exactly 40MHz)
    // are never reachable at any clk_peri/request combination). Corrupted
    // at both 50MHz (clk_peri/4) and, after retargeting clk_peri to
    // 160MHz specifically to reach it, 40MHz (160/4) too -- so the real
    // ceiling sits somewhere in (33.33, 40)MHz, untested further.
    c.pixel_freq_hz = 33'333'333;
    return c;
}

} // namespace picodoom
