// uSD (FatFs) mount for the Pico DOOM port. POSIX/stdio syscalls
// (open/read/lseek/fstat/... -- what the engine's WAD loader and libc calls
// actually need) are provided by Pico-Toolset's pico_toolset_sdcard
// component (PICO_TOOLSET_SDCARD_STDIO=ON in this project's CMakeLists.txt;
// see third_party/pico-toolset/components/sdcard/src/sdcard_stdio.cpp) --
// this file only mounts the card, plus the other libc syscalls (time,
// sleep) the still-Linux-flavoured engine core needs to link.
#include "pico_toolset/sdcard.h"
#include "pico_toolset/sdcard_configs.h"

#include "ff.h"
#include "pico/time.h"

#include <cstdio>

#include <sys/time.h>
#include <unistd.h>

namespace {
pico_toolset::SdCard g_sd_card;
} // namespace

// Everything the port reads or writes (WADs, default.cfg, savegames) lives in this folder of the card: the
// FatFs current directory is moved there once after mounting, so every relative path the engine, the boot menu
// and SdCard::list_files() use resolves inside it. Created on first boot if the card has no such folder.
static constexpr const char* kDataDir = "/PicoDoom";

static void enter_data_dir()
{
    FRESULT res = f_chdir(kDataDir);
    if (res == FR_NO_PATH || res == FR_NO_FILE) {
        res = f_mkdir(kDataDir);
        if (res == FR_OK) {
            printf("PicoDoom: created %s on the uSD card -- put the WAD files there\n", kDataDir);
            res = f_chdir(kDataDir);
        }
    }
    if (res != FR_OK)
        printf("PicoDoom: cannot use %s on the uSD card (FatFs error %d)\n", kDataDir, static_cast<int>(res));
}

extern "C" bool sd_init(void)
{
#ifdef PICODOOM_PRESTO
    const bool ok = g_sd_card.init(pico_toolset::configs::sdcard::kPimoroniPresto);
#else
    const bool ok = g_sd_card.init(pico_toolset::configs::sdcard::kWaveshareRp2350PiZero);
#endif
    if (ok)
        enter_data_dir();
    return ok;
}

// SD-card accessor for the boot WAD-selection menu (Phase 4, src/wad_menu.cpp):
// pico_toolset::SdCard::list_files({"wad"}) enumerates the .WAD candidates of
// the current directory (/PicoDoom, see enter_data_dir()) (see that component's sdcard.h). The menu only lists;
// actual file access goes through the POSIX stdio shim (fopen/fread) the
// way the engine itself reads the WAD. Boot-menu-only, core0.
extern "C" pico_toolset::SdCard& sd_card(void)
{
    return g_sd_card;
}

// ---------------------------------------------------------------------------
// time / misc glue (strong overrides of pico-sdk's weak defaults)
// ---------------------------------------------------------------------------

extern "C" int _gettimeofday(struct timeval* tv, void* tz)
{
    (void)tz; // no timezone support
    uint64_t us = time_us_64();
    tv->tv_sec = us / 1000000;
    tv->tv_usec = us % 1000000;
    return 0;
}

extern "C" int usleep(useconds_t usec)
{
    busy_wait_us_32(usec);
    return 0;
}
