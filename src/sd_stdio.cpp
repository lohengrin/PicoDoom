// uSD (FatFs) mount for the Pico DOOM port. POSIX/stdio syscalls
// (open/read/lseek/fstat/... -- what the engine's WAD loader and libc calls
// actually need) are provided by Pico-Toolset's pico_toolset_sdcard
// component (PICO_TOOLSET_SDCARD_STDIO=ON in this project's CMakeLists.txt;
// see third_party/pico-toolset/components/sdcard/src/sdcard_stdio.cpp) --
// this file only mounts the card, plus the other libc syscalls (time,
// sleep) the still-Linux-flavoured engine core needs to link.
#include "pico_toolset/sdcard.h"
#include "pico_toolset/sdcard_configs.h"

#include "pico/time.h"

#include <sys/time.h>
#include <unistd.h>

namespace {
pico_toolset::SdCard g_sd_card;
} // namespace

extern "C" bool sd_init(void)
{
    return g_sd_card.init(pico_toolset::configs::sdcard::kWaveshareRp2350PiZero);
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
