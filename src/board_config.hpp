#pragma once

// PicoDoom-specific clock tuning, layered on top of Pico-Toolset's generic
// Waveshare RP2350-PiZero presets (pico_toolset::configs::psram::
// kWaveshareRp2350PiZero, pico_toolset::configs::ili9486::
// kWaveshareRp2350PiZero) rather than mutating those shared defaults in
// place -- those stay the toolset's board-standard, hardware-validated
// baseline for any consumer that hasn't opted into PicoDoom's own
// overclocking. The two builds run different clk_sys, both at
// VREG_VOLTAGE_1_20 (see PicoDoom.cpp): the "hdmi" variant on exactly
// 252MHz (a hard DVI requirement), the "lcd" variant on 264MHz (raised by
// instruction). clk_peri is re-sourced to follow clk_sys, so both builds
// land on the divisors below: flash and PSRAM at QMI div 2 (=clk_sys/2,
// 126MHz HDMI / 132MHz LCD), and the LCD SPI pixel clock's request
// resolving to exactly 264/8 = 33.0MHz. A consumer at the toolset's stock
// clocks should use the stock presets unmodified, not these values.
//
// 2026-09 performance work; unified 252MHz (2026-09), LCD raised to 264MHz (2026-09).

#ifndef PICODOOM_HDMI
#include "pico_toolset/ili9486_configs.h"
#endif
#include "pico_toolset/psram_configs.h"

namespace picodoom {

inline pico_toolset::PsramConfig psram_config() {
    pico_toolset::PsramConfig c = pico_toolset::configs::psram::kWaveshareRp2350PiZero;
    // Divisor=2 at this build's clk_sys, matching the flash CLKDIV=2
    // chosen for both builds -- the frequencies differ per build because
    // clk_sys does: HDMI = 252/2 = 126MHz, LCD = 264/2 = 132MHz. This is
    // deliberately ABOVE the chip's ~100MHz spec ceiling (and above the
    // toolset preset's 30MHz) -- requested by instruction to unify on
    // divisor 2, not tuned to a frequency. max_clock_hz must REQUEST the
    // exact half-clk_sys value for the QMI's ceil(clk_sys/max) math to come
    // out at exactly 2 (any value in [clk_sys/2, clk_sys)MHz would, the
    // exact one is used); see pico_toolset::psram_set_clock_hz()'s doc
    // comment (psram.h) for the divisor math if reusing this at a different
    // clock.
#ifdef PICODOOM_HDMI
    // HDMI build: clk_sys=252MHz.
    c.max_clock_hz = 126'000'000;
#else
    // LCD build: clk_sys=264MHz.
    c.max_clock_hz = 132'000'000;
#endif
    return c;
}

#ifndef PICODOOM_HDMI
inline pico_toolset::Ili9486Config ili9486_config() {
    pico_toolset::Ili9486Config c = pico_toolset::configs::ili9486::kWaveshareRp2350PiZero;
    // Request stays at ~33.33MHz, with clk_peri at the LCD build's 264MHz
    // (PicoDoom.cpp): spi_set_baudrate() picks the largest achievable rate
    // AT OR BELOW the request, and total SPI divisors that fit the hardware
    // (prescale/CPSDVSR is even, postdiv is any 1..256 integer, so the
    // product is even) around 264/33.33 = 7.92 land exactly on 264/8 =
    // 33.0MHz -- right in the clean range confirmed at the original clock
    // (33.33MHz clean, everything from 40MHz up corrupted: 264/6=44MHz and
    // above are unreachable by this request anyway, since the SDK rounds
    // down, never up). Request value 33'333'333 keeps F1/F2
    // (i_video_bump_pixel_clock_hz(), src/i_video_ili9486.cpp) live-tuning
    // from the same baseline it was validated at.
    c.pixel_freq_hz = 33'333'333;
    return c;
}
#endif

} // namespace picodoom
