// PicoDoom entry point for RP2350 (Waveshare RP2350-PiZero).
// Replaces doom/i_main.c: bare-metal main() that brings up the board
// (PSRAM, uSD out of which the WAD is loaded via the sd_stdio.c syscall
// shim), then hands over to the engine.
#include <cstdio>
#include "pico/stdlib.h"

extern "C" {
#include "d_main.h"
#include "m_argv.h"
}

#include "pico_toolset/psram.h"
#include "pico_toolset/psram_configs.h"
#include "pico_toolset/fault_handler.h"
#include "PicoUsbKeyboard.hpp"
extern "C" bool sd_init(void);

static void fatal(const char* msg)
{
    printf("PicoDoom: %s\n", msg);
    while (true)
        tight_loop_contents();
}

int main(void)
{
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

    // Phase 3: USB-PIO HID keyboard host (GPIO28/29), on its own core1 --
    // started here, before D_DoomMain(), so it has the whole WAD-load/
    // engine-init stretch to enumerate a keyboard before the game loop
    // starts polling it (see src/PicoUsbKeyboard.cpp).
    printf("PicoDoom: USB-PIO keyboard host init (core1)...\n");
    PicoUsbKeyboard::init();

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