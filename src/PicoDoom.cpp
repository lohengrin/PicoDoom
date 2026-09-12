// PicoDoom entry point for RP2350 (Waveshare RP2350-PiZero).
// Replaces doom/i_main.c: bare-metal main() that brings up the board
// (PSRAM, uSD out of which the WAD is loaded via the sd_stdio.c syscall
// shim), then hands over to the engine.
#include <cstdio>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

extern "C" {
#include "d_main.h"
#include "m_argv.h"
}

#include "pico_toolset/psram.h"
#include "pico_toolset/psram_configs.h"
#include "pico_toolset/fault_handler.h"
extern "C" bool sd_init(void);
extern "C" void usb_hid_core1_init(void); // src/i_input_usbhid.cpp

static void fatal(const char* msg)
{
    printf("PicoDoom: %s\n", msg);
    while (true)
        tight_loop_contents();
}

int main(void)
{
    // Overclock clk_sys from the RP2350 default 150MHz to 200MHz (2026-09
    // performance work): lets PSRAM's QMI clock hit exactly 100MHz at
    // divisor=2 (clk_sys/2 -- see psram_configs.h, whose max_clock_hz=75MHz
    // was itself the ceiling reachable at 150MHz without exceeding the
    // chip's ~100MHz spec). vreg_set_voltage() bump is the standard
    // convention for RP2040/RP2350 stability above the ~133-150MHz
    // default-voltage ceiling; 1.15V is within VREG_VOLTAGE_MAX (1.30V) so
    // vreg_disable_voltage_limit() isn't needed. Every peripheral
    // initialized below queries the actual current clock at ITS OWN init
    // time, so doing this first, before anything else, is what lets it
    // propagate correctly -- must not move later in boot. NOT YET validated
    // on real hardware -- this is a whole-chip timing change (unlike the
    // earlier PSRAM-only clock change), so watch for ANY instability
    // (resets, corrupted PSRAM/flash reads, USB dropouts), not just display
    // artifacts, across multiple cold boots.
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(10);
    set_sys_clock_khz(200'000, true);

    // clk_peri (the SPI baud generator's clock source, see
    // src/i_video_ili9486.cpp's F1/F2 tuning) does NOT automatically follow
    // clk_sys by pico-sdk default: set_sys_clock_khz()/set_sys_clock_pll()
    // only re-source it from clk_sys when the SDK is built with
    // PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK (off here) -- otherwise
    // it's left on CLKSRC_PLL_USB, a fixed 48MHz regardless of clk_sys
    // (clocks.c). Confirmed on real hardware (2026-09): the clk_sys
    // overclock above measurably sped up PSRAM (memcpy/convert both
    // dropped) but F1/F2's live pixel-clock requests plateaued at exactly
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

    // If the previous boot ended in a hard fault, its diagnostic registers
    // were stashed in the watchdog's scratch registers (survive the reset)
    // instead of being printed from fault context -- see
    // pico_toolset/fault_handler.h's header comment for why. Report it now,
    // first thing, before it's lost to the next fault (or the next normal
    // reboot).
    pico_toolset::report_pending_hard_fault("PicoDoom");

    printf("PicoDoom: PSRAM init...\n");
    pico_toolset::PsramStatus psram =
        pico_toolset::psram_init(pico_toolset::configs::psram::kWaveshareRp2350PiZero);
    if (!psram.present)
        fatal("PSRAM not detected");
    if (!psram.test_ok)
        fatal("PSRAM detected but self-test failed");
    printf("PicoDoom: PSRAM OK (%u KB)\n", (unsigned)(psram.size_bytes / 1024));

    printf("PicoDoom: uSD mount...\n");
    if (!sd_init())
        fatal("uSD mount failed (is a FAT32 card with the WAD inserted?)");
    printf("PicoDoom: uSD mounted\n");

    // Phase 3: USB-PIO HID keyboard/mouse host (GPIO28/29), on its own
    // core1 -- started here, before D_DoomMain(), so it has the whole
    // WAD-load/engine-init stretch to enumerate a keyboard before the game
    // loop starts polling it (see src/i_input_usbhid.cpp).
    printf("PicoDoom: USB-PIO keyboard host init (core1)...\n");
    usb_hid_core1_init();

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