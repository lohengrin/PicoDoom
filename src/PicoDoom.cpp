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

#include "Psram.hpp"
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
    sleep_ms(2000); // Give host time to attach terminal

    printf("PicoDoom boot\n");

    printf("PicoDoom: PSRAM init...\n");
    PsramStatus psram = psram_hw_init();
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