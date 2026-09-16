#include "pico_toolset/psram.h"

#include <cstdlib>

// LVGL's memory-pool allocator hook (LV_MEM_POOL_ALLOC in src/lv_conf.h) --
// C linkage, since lv_conf.h is included by LVGL's own C library sources and
// declares this with `extern "C"`. Mirrors TOM6809's PsramLvglPool.cpp
// (validated on this exact board): PSRAM when pico_toolset::psram_init()
// (called once from src/PicoDoom.cpp before the boot menu) found the chip
// present and healthy, else ordinary SRAM heap. The whole boot menu runs on
// core0 only, so reaching into the single-core XIP-cached PSRAM region is
// safe (unlike the contrast elsewhere in this project: g_screen_buf/g_framebuf
// swap with core1 and deliberately stay in SRAM, see i_video_ili9486.cpp).
extern "C" void* lvgl_mem_pool(size_t size) {
    if (pico_toolset::psram_status().test_ok) {
        return pico_toolset::psram_malloc(size);
    }
    return std::malloc(size);
}