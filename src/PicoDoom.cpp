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
extern "C" bool sd_init(void);

static void fatal(const char* msg)
{
    printf("PicoDoom: %s\n", msg);
    while (true)
        tight_loop_contents();
}

#ifndef PICODOOM_DIAG_STAGE
#define PICODOOM_DIAG_STAGE 3
#endif

// Bring-up diagnostic: bisect which init stage kills USB enumeration, no
// probe needed. Each stage prints a heartbeat forever instead of falling
// through, so "device enumerates but goes silent/vanishes at stage N" is
// visible from dmesg/a terminal alone. Build with
// -DPICODOOM_DIAG_STAGE={0,1,2,3}; default (3) is the normal full boot.
// 0=stdio only, 1=+PSRAM, 2=+uSD mount, 3=+D_DoomMain (full boot).
static void heartbeat(const char* stage_label)
{
    unsigned n = 0;
    while (true) {
        printf("PicoDoom: %s alive (%u)\n", stage_label, n++);
        sleep_ms(500);
    }
}

// Callable from doom/d_main.c (a C file) to plant checkpoints deeper inside
// D_DoomMain than main() can reach -- see PICODOOM_DIAG_STAGE 4/5 there.
extern "C" void picodoom_diag_heartbeat(const char* stage_label)
{
    heartbeat(stage_label);
}

int main(void)
{
    stdio_init_all();
    printf("PicoDoom boot (diag stage %d)\n", PICODOOM_DIAG_STAGE);

#if PICODOOM_DIAG_STAGE == 0
    heartbeat("stage0 (stdio only)");
#endif

    printf("PicoDoom: PSRAM init...\n");
    PsramStatus psram = psram_hw_init();
    if (!psram.present)
        fatal("PSRAM not detected");
    if (!psram.test_ok)
        fatal("PSRAM detected but self-test failed");
    printf("PicoDoom: PSRAM OK (%u KB)\n", (unsigned)(psram.size_bytes / 1024));

#if PICODOOM_DIAG_STAGE == 1
    heartbeat("stage1 (+PSRAM)");
#endif

    printf("PicoDoom: uSD mount...\n");
    if (!sd_init())
        fatal("uSD mount failed (is a FAT32 card with the WAD inserted?)");
    printf("PicoDoom: uSD mounted\n");

#if PICODOOM_DIAG_STAGE == 2
    heartbeat("stage2 (+uSD mount)");
#endif

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