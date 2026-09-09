// uSD (FatFs) mount + newlib syscall shim for the Pico DOOM port.
//
// The engine does all its I/O through POSIX/stdio calls (open/read/lseek/
// close, fopen/fread/fseek, stat/access); on the Pico those funnel into
// newlib's weak syscall layer, which this file replaces with FatFs-backed
// implementations over the Waveshare RP2350-PiZero's uSD socket via PIO-bit-
// banged SPI (CS=43, MOSI=31, MISO=40, SCK=30) -- same wiring and config as
// TOM6809's validated PicoSdCard_Waveshare.cpp. Console fds (0/1/2) pass
// through to pico_stdio unchanged. This file also carries the other libc
// syscalls the still-Linux-flavoured engine core needs to link (time, sleep).
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "ff.h"
#include "tf_card.h"

#define MAX_FILE_FDS 12

typedef struct {
    FIL file;
    bool open;
} sd_file_t;

static sd_file_t s_files[MAX_FILE_FDS];
static bool s_sd_mounted;
static FATFS s_fatfs;

// ---------------------------------------------------------------------------
// Mount
// ---------------------------------------------------------------------------

bool sd_init(void)
{
    // SD-in-SPI-mode over the card's 4-bit SDIO wiring (CS=D3/43, MOSI=CMD/31,
    // MISO=D0/40, SCK=CLK/30). spi_inst=nullptr requests PIO-bit-banged SPI:
    // these pins have no native hardware-SPI function that pico_fatfs accepts
    // (SPI1 is reserved for the LCD). pin_miso=40 is >31, so pico/CMakeLists
    // sets PICO_PIO_USE_GPIO_BASE=1 (without it the PIO program addresses
    // GPIO40 as GPIO8 and the mount hangs forever -- RP2350B alias).
    pico_fatfs_spi_config_t config = {
        /*spi_inst=*/NULL,
        CLK_SLOW_DEFAULT,
        10 * MHZ,
        /*pin_miso=*/40,
        /*pin_cs=*/43,
        /*pin_sck=*/30,
        /*pin_mosi=*/31,
        /*pullup=*/true,
    };
    if (!pico_fatfs_set_config(&config)) {
        // PIO-SPI path (the only one here), mirroring TOM6809: use PIO1/SM0
        // and widen its GPIO window to 16-47 before f_mount() does the
        // deferred PIO init -- pico_fatfs's defaults (PIO0, window 0-31)
        // cannot address MISO on GPIO40.
        pio_set_gpio_base(pio1, 16);
        pico_fatfs_config_spi_pio(pio1, 0);
    }

    FRESULT res = f_mount(&s_fatfs, "", 1);
    s_sd_mounted = (res == FR_OK);
    return s_sd_mounted;
}

int sd_mount_result(void)
{
    return s_sd_mounted ? FR_OK : FR_NO_FILESYSTEM;
}

// ---------------------------------------------------------------------------
// newlib file syscalls (fds 3+ -> FatFs; 0/1/2 -> pico_stdio)
// ---------------------------------------------------------------------------

int _open(const char* path, int flags, ...)
{
    if (!s_sd_mounted || !path || !path[0])
        return -1;

    BYTE mode = 0;
    int acc = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    if (acc != O_WRONLY)
        mode |= FA_READ;
    if (acc != O_RDONLY)
        mode |= FA_WRITE;
    if (flags & O_CREAT) {
        if (flags & O_TRUNC)
            mode |= FA_CREATE_ALWAYS;
        else if (flags & O_APPEND)
            mode |= FA_OPEN_APPEND;
        else
            mode |= FA_OPEN_ALWAYS;
    }

    for (int i = 0; i < MAX_FILE_FDS; i++) {
        if (!s_files[i].open) {
            if (f_open(&s_files[i].file, path, mode) != FR_OK)
                return -1;
            s_files[i].open = true;
            return 3 + i;
        }
    }
    return -1;
}

int _close(int fd)
{
    if (fd <= 2)
        return 0;
    int i = fd - 3;
    if (i < 0 || i >= MAX_FILE_FDS || !s_files[i].open)
        return -1;
    FRESULT res = f_close(&s_files[i].file);
    s_files[i].open = false;
    return res == FR_OK ? 0 : -1;
}

int _read(int fd, char* buf, int len)
{
    if (fd == 0)
        return stdio_get_until(buf, len, at_the_end_of_time);
    int i = fd - 3;
    if (i < 0 || i >= MAX_FILE_FDS || !s_files[i].open)
        return -1;

    UINT got = 0;
    if (f_read(&s_files[i].file, buf, (UINT)len, &got) != FR_OK)
        return -1;
    return (int)got;
}

int _write(int fd, const char* buf, int len)
{
    if (fd == 1 || fd == 2) {
        stdio_put_string(buf, len, false, true);
        return len;
    }
    int i = fd - 3;
    if (i < 0 || i >= MAX_FILE_FDS || !s_files[i].open)
        return -1;

    UINT done = 0;
    if (f_write(&s_files[i].file, buf, (UINT)len, &done) != FR_OK)
        return -1;
    return (int)done;
}

off_t _lseek(int fd, off_t pos, int whence)
{
    int i = fd - 3;
    if (i < 0 || i >= MAX_FILE_FDS || !s_files[i].open)
        return -1;

    FSIZE_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = f_tell(&s_files[i].file); break;
    case SEEK_END: base = f_size(&s_files[i].file); break;
    default: return -1;
    }

    int64_t target = (int64_t)base + pos;
    if (target < 0)
        return -1;
    if (f_lseek(&s_files[i].file, (FSIZE_t)target) != FR_OK)
        return -1;
    return (off_t)target;
}

int _fstat(int fd, struct stat* st)
{
    if (fd <= 2)
        return -1;
    int i = fd - 3;
    if (i < 0 || i >= MAX_FILE_FDS || !s_files[i].open)
        return -1;

    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_size = (off_t)f_size(&s_files[i].file);
    return 0;
}

int _stat(const char* path, struct stat* st)
{
    if (!s_sd_mounted || !path)
        return -1;

    FILINFO info;
    if (f_stat(path, &info) != FR_OK)
        return -1;

    memset(st, 0, sizeof(*st));
    st->st_mode = (info.fattrib & AM_DIR) ? S_IFDIR : S_IFREG;
    st->st_size = (off_t)info.fsize;
    return 0;
}

int _isatty(int fd)
{
    return fd <= 2;
}

// ---------------------------------------------------------------------------
// time / misc glue (strong overrides of pico-sdk's weak defaults)
// ---------------------------------------------------------------------------

int _gettimeofday(struct timeval* tv, void* tz)
{
    uint64_t us = time_us_64();
    tv->tv_sec = us / 1000000;
    tv->tv_usec = us % 1000000;
    return 0;
}

int usleep(useconds_t usec)
{
    busy_wait_us_32(usec);
    return 0;
}