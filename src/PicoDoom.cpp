// PicoDoom entry point for RP2350 (Waveshare RP2350-PiZero).
// Replaces doom/i_main.c: bare-metal main() that brings up the board
// (PSRAM, uSD out of which the WAD is loaded via the sd_stdio.c syscall
// shim), then hands over to the engine.
#include <cstdio>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"

extern "C" {
#include "d_main.h"
#include "m_argv.h"
}

#include "pico_toolset/psram.h"
#include "pico_toolset/fault_handler.h"
#include "board_config.hpp"
extern "C" bool sd_init(void);
extern "C" bool wad_menu_run(void); // src/wad_menu.cpp (Phase 4 boot WAD-selection menu)
#ifndef PICODOOM_HDMI
extern "C" void usb_hid_core1_init(void); // src/i_input_usbhid.cpp
#else
extern "C" void usb_hid_core0_init(void); // src/i_input_usbhid.cpp
#endif

static void fatal(const char* msg)
{
    printf("PicoDoom: %s\n", msg);
    while (true)
        tight_loop_contents();
}

int main(void)
{
    // Flash's own QMI (M0) clock divider (CLKDIV) is a FIXED register value
    // set once by boot2 for the chip's ORIGINAL ~150MHz clk_sys --
    // boot_stage2/boot2_w25q080.S:45's PICO_FLASH_SPI_CLKDIV=2, i.e. flash
    // SPI clock = clk_sys/2 = ~75MHz at boot. Unlike clk_peri (which needs
    // explicit re-sourcing to follow clk_sys at all -- see below), flash's
    // QMI computes its SPI clock as clk_sys/CLKDIV directly with no
    // separate clock-tree source, so raising clk_sys WITHOUT touching this
    // divisor would silently raise the physical flash SPI clock too.
    // RP2350's own QMI_M0_TIMING_CLKDIV register docs explicitly warn about
    // exactly this ordering: "If software is increasing CLKDIV in
    // anticipation of an increase [in clk_sys], the increase must be
    // applied before the clk_sys increase" -- so write it here, first,
    // safe to do on-the-fly per those same docs (unlike the other M0
    // timing fields, which need the QMI idle).
    //
    // CLKDIV=2 unconditionally, both builds, so flash SPI = clk_sys/2 tracks
    // the (build-specific) clk_sys below: 264/2 = 132MHz for the LCD build,
    // 252/2 = 126MHz for HDMI. That deliberately exceeds the ~75MHz proven
    // stable at boot2's own divisor=2/~150MHz clk_sys -- chosen by
    // instruction to unify on divisor 2 across flash and PSRAM rather than
    // stay under that baseline, and far riskier than a corrupted pixel since
    // ALL code except explicitly RAM-placed functions executes via XIP from
    // this same flash: a misread instruction fails very differently (and
    // worse). If XIP misreads show up, raise this divisor, not the hardware.
    constexpr uint32_t kFlashClkDiv = 2;
    hw_write_masked(&qmi_hw->m[0].timing,
                     kFlashClkDiv << QMI_M0_TIMING_CLKDIV_LSB,
                     QMI_M0_TIMING_CLKDIV_BITS);

    // clk_sys differs per build, both at VREG_VOLTAGE_1_20. HDMI build:
    // exactly 252MHz, a hard requirement -- confirmed on real hardware in
    // TOM6809 (same board, same pico_toolset_dvi_hdmi): src/i_video_dvi.cpp's
    // DVI/TMDS PIO serialiser derives its bit clock directly from whatever
    // clk_sys is at dvi_init() time (dvi_timing_640x480p_60hz.bit_clk_khz ==
    // 252000; that timing's own comment says "we do this mode properly, with
    // a pretty comfortable clk_sys (252 MHz)", and every real consumer of it
    // -- including Waveshare's own hello_dvi demo for this exact board --
    // sets clk_sys to exactly this before dvi_init()). Left at any other
    // clk_sys, the PIO clock-divider math is wrong and the DMA/PIO scanout
    // pipeline stalls indefinitely with no video signal at all (confirmed:
    // this is exactly what an earlier revision of this file did, at
    // 200MHz, and why). LCD build: 264MHz (by instruction, raised from the
    // previously-unified 252MHz) -- clk_peri follows at 264MHz (see below),
    // letting the ~33MHz SPI pixel-clock request resolve to exactly
    // 264/8 = 33.0MHz (see board_config.hpp's ili9486_config()).
    // Both clocks keep the QMI divisors at 2 (flash + PSRAM = clk_sys/2 =
    // 132MHz LCD / 126MHz HDMI, see board_config.hpp's psram_config()) and
    // satisfy Pico-PIO-USB's separate requirement (pico_toolset_usb_hid's
    // hcd_pio_usb.c derives its bit timing the same way, needing an exact
    // multiple of 12MHz: 264/12=22, 252/12=21) regardless of which core
    // hosts it, so there's no conflict between the clock-sensitive
    // subsystems in either build. VREG_VOLTAGE_1_20 is the
    // real-hardware-validated recipe for 252MHz -- the standard convention
    // for RP2040/RP2350 stability above the ~133-150MHz default-voltage
    // ceiling (within VREG_VOLTAGE_MAX (1.30V), so
    // vreg_disable_voltage_limit() isn't needed) -- carried over to the LCD
    // build's higher 264MHz. Must happen here, before anything else (uSD,
    // PSRAM) runs: psram_init() below calibrates its own QMI timing
    // divisors against whatever clk_sys is active at that point, so
    // changing clk_sys afterward would desync it.
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
#ifdef PICODOOM_HDMI
    set_sys_clock_khz(252'000, true);
#else
    set_sys_clock_khz(264'000, true);
#endif

    // clk_peri (the SPI baud generator's clock source, see
    // src/i_video_ili9486.cpp plus the serial console's 'pclk' command) does NOT automatically follow
    // clk_sys by pico-sdk default: set_sys_clock_khz()/set_sys_clock_pll()
    // only re-source it from clk_sys when the SDK is built with
    // PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK (off here) -- otherwise
    // it's left on CLKSRC_PLL_USB, a fixed 48MHz regardless of clk_sys
    // (clocks.c). Confirmed on real hardware (2026-09): the clk_sys
    // overclock above measurably sped up PSRAM (memcpy/convert both
    // dropped) but the pixel-clock requests plateaued at exactly
    // 24MHz no matter how high requested -- clk_peri really was still stuck
    // at 48MHz (next achievable SPI divider step up from 24MHz is 48MHz
    // itself, nothing in between), unaffected by the clk_sys change.
    // Re-source clk_peri from clk_sys explicitly instead of flipping that
    // global SDK build option, so this stays a one-line, self-contained
    // decision here rather than a build-wide default.
    clock_configure_undivided(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, clock_get_hz(clk_sys));

    stdio_init_all();
    // Wait up to 5 seconds (5000 ms) for a USB CDC terminal to attach
    absolute_time_t timeout = make_timeout_time_ms(5000);
    while (!stdio_usb_connected() && !time_reached(timeout)) {
        sleep_ms(100);
    }

    printf("PicoDoom boot\n");

    // Boot-time clock report (2026-09 performance work): every clock/divisor
    // touched above, printed once so a hardware regression (or the next
    // round of tuning) can always be checked against what's actually
    // running, not just what the source says it should be. PSRAM's and the
    // SPI pixel clock's achieved rates print further down, right after
    // their own init (psram_init() below; I_InitGraphics(),
    // src/i_video_ili9486.cpp, deep inside D_DoomMain()) since neither is
    // known until then.
    {
        uint32_t flash_clkdiv = (qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
        uint32_t sys_hz = clock_get_hz(clk_sys);
        printf("PicoDoom: clk_sys=%u Hz  clk_peri=%u Hz  flash CLKDIV=%u (flash SPI=%u Hz)\n",
               (unsigned)sys_hz, (unsigned)clock_get_hz(clk_peri),
               (unsigned)flash_clkdiv, (unsigned)(sys_hz / flash_clkdiv));
    }

    // If the previous boot ended in a hard fault, its diagnostic registers
    // were stashed in the watchdog's scratch registers (survive the reset)
    // instead of being printed from fault context -- see
    // pico_toolset/fault_handler.h's header comment for why. Report it now,
    // first thing, before it's lost to the next fault (or the next normal
    // reboot).
    pico_toolset::report_pending_hard_fault("PicoDoom");

    printf("PicoDoom: PSRAM init...\n");
    pico_toolset::PsramStatus psram = pico_toolset::psram_init(picodoom::psram_config());
    if (!psram.present)
        fatal("PSRAM not detected");
    if (!psram.test_ok)
        fatal("PSRAM detected but self-test failed");
    printf("PicoDoom: PSRAM OK (%u KB, clock=%u Hz)\n",
           (unsigned)(psram.size_bytes / 1024), (unsigned)psram.clock_hz);

    printf("PicoDoom: uSD mount...\n");
    if (!sd_init())
        fatal("uSD mount failed (is a FAT32 card with the WAD inserted?)");
    printf("PicoDoom: uSD mounted\n");

    // Phase 3: USB-PIO HID keyboard/mouse host (GPIO28/29) -- started here,
    // before D_DoomMain(), so it has the whole WAD-load/engine-init stretch
    // to enumerate a keyboard before the game loop starts polling it (see
    // src/i_input_usbhid.cpp). LCD build: its own core1. HDMI build: core0
    // (core1 belongs to src/i_video_dvi.cpp's DVI encode loop instead) --
    // see src/i_input_usbhid.cpp's file header for why.
#ifndef PICODOOM_HDMI
    printf("PicoDoom: USB-PIO keyboard host init (core1)...\n");
    usb_hid_core1_init();
#else
    printf("PicoDoom: USB-PIO keyboard host init (core0)...\n");
    usb_hid_core0_init();
#endif

    // Phase 4: boot WAD-selection menu (LVGL, core0) -- lists every IWAD on
    // the uSD root and lets the user pick, persist and auto-start the last
    // one (see src/wad_menu.cpp). IdentifyVersion() reads the choice from
    // wad_boot.cpp before its fixed-name scan (doom/d_main.c, #ifdef PICO).
    wad_menu_run();

    // Engine globals (m_argv.c); no command-line parameters for now --
    // IdentifyVersion finds the WAD (doom1.wad/doom.wad/... ) on the SD root.
    static char arg0[] = "doom";
    static char* argv[] = {arg0, nullptr};
    myargc = 1;
    myargv = argv;

    D_DoomMain();

    // not reached (I_Error/I_Quit exit); shut the compiler up.
    return 0;
}