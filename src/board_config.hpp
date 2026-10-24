#pragma once

// PicoDoom-specific clock tuning, layered on top of Pico-Toolset's generic
// Waveshare RP2350-PiZero presets (pico_toolset::configs::psram::
// kWaveshareRp2350PiZero, pico_toolset::configs::ili9486::
// kWaveshareRp2350PiZero, pico_toolset::configs::st7796::
// kWaveshareRp2350PiZero) rather than mutating those shared defaults in
// place -- those stay the toolset's board-standard, hardware-validated
// baseline for any consumer that hasn't opted into PicoDoom's own
// overclocking. All three PICODOOM_VIDEO_OUTPUT builds (lcd/lcd-st7796/hdmi)
// run different clk_sys, all at VREG_VOLTAGE_1_20 (see PicoDoom.cpp): the
// "hdmi" variant on exactly 252MHz (a hard DVI requirement), both LCD
// variants on 264MHz (raised by instruction; lcd-st7796 shares the ILI9486
// panel's clock plumbing for its "first implementation, keep actual
// frequency" starting point -- see st7796_config() below). clk_peri is
// re-sourced to follow clk_sys, so all builds land on the divisors below:
// flash and PSRAM at QMI div 2 (=clk_sys/2, 126MHz HDMI / 132MHz LCD), and
// the LCD SPI clock's request resolving to exactly 264/8 = 33.0MHz. A
// consumer at the toolset's stock clocks should use the stock presets
// unmodified, not these values.
//
// 2026-09 performance work; unified 252MHz (2026-09), LCD raised to 264MHz
// (2026-09); ST7796U variant added (2026-09, not yet hardware-validated).

#ifdef PICODOOM_ILI9486
#include "pico_toolset/ili9486_configs.h"
#endif
#ifdef PICODOOM_ST7796
#include "pico_toolset/st7796_configs.h"
#endif
#include "pico_toolset/psram_configs.h"

namespace picodoom {

#ifdef PICODOOM_PRESTO
// Pimoroni Presto: toolset preset as-is (APS6404L, 50MHz ceiling, validated by
// examples/presto at the stock 150MHz clk_sys).
inline pico_toolset::PsramConfig psram_config() {
    pico_toolset::PsramConfig c = pico_toolset::configs::psram::kPimoroniPresto;
    // Stock 150MHz keeps the preset's 50MHz; any overclock runs PSRAM at clk_sys/2
    // (264MHz -> 132MHz, the same recipe the PiZero LCD builds validated).
    if (PICODOOM_PRESTO_SYSCLK_MHZ != 150)
        c.max_clock_hz = PICODOOM_PRESTO_SYSCLK_MHZ * 1'000'000 / 2;
    return c;
}
#else
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
#endif // PICODOOM_PRESTO

#ifdef PICODOOM_ILI9486
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
    // down, never up). Request value 33'333'333 is the baseline it was
    // validated at.
    c.pixel_freq_hz = 33'333'333;
    return c;
}
#endif

#ifdef PICODOOM_ST7796
inline pico_toolset::St7796Config st7796_config() {
    pico_toolset::St7796Config c = pico_toolset::configs::st7796::kWaveshareRp2350PiZero;
    // Bench-tested on real hardware (2026-09) with a live pixel-clock tuning
    // command (since removed) -- confirmed clean (no artifacts) all the way
    // up to 132MHz, which is the fastest rate reachable at all from this
    // build's 264MHz clk_peri (SPI's divisor floor is 2: prescale=2,
    // postdiv=1 -- see ili9486_config()'s comment for the same divisor
    // math). That's ~5.6% above this panel's own datasheet ceiling
    // (125MHz) but confirmed artifact-free, so kept as the default by
    // instruction. Requesting the exact validated number (not an
    // unbounded "as fast as possible" value) means this stays correct if
    // clk_peri is ever raised later -- spi_set_baudrate() would then find a
    // divisor landing closer to this same 132MHz instead of silently
    // exceeding the validated ceiling.
    c.spi_freq_hz = 132'000'000;
    return c;
}
#endif

} // namespace picodoom
