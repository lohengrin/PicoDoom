// LVGL display driver for the Presto build: LVGL renders RGB565 tiles (PSRAM
// draw buffer, core0 only) and the flush quantises them to RGB332 indices in
// the 480x300 surface that the panel scanout reads (see i_video_presto.cpp:
// the palette-mode framebuffer or the scanline-mode menu surface; both are
// pointed at a RGB332 palette while the menu is up).
#include "pico_toolset/psram.h"
#include "lvgl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" uint8_t* i_video_presto_menu_surface(void); // src/i_video_presto.cpp

namespace {
constexpr int kWidth = 480;
constexpr int kHeight = 300;
constexpr int kBufRows = 40;
uint16_t* g_draw_buf = nullptr;

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    uint8_t* surface = i_video_presto_menu_surface();
    const auto* pixels = reinterpret_cast<const uint16_t*>(px_map);
    if (surface != nullptr) {
        const int w = area->x2 - area->x1 + 1;
        for (int y = area->y1; y <= area->y2; ++y) {
            uint8_t* dst = surface + y * kWidth + area->x1;
            for (int x = 0; x < w; ++x) {
                const uint16_t v = *pixels++;
                // RGB565 -> RGB332: top 3 bits red, top 3 green, top 2 blue.
                dst[x] = static_cast<uint8_t>(((v >> 8) & 0xE0) | ((v >> 6) & 0x1C) | ((v >> 3) & 0x03));
            }
        }
    }
    lv_display_flush_ready(disp);
}
} // namespace

extern "C" void lvgl_display_presto_init(void)
{
    const size_t buf_bytes = static_cast<size_t>(kWidth) * kBufRows * sizeof(uint16_t);
    void* buf = pico_toolset::psram_status().test_ok ? pico_toolset::psram_malloc(buf_bytes)
                                                     : std::malloc(buf_bytes);
    if (!buf) {
        printf("PicoDoom: LVGL draw buffer alloc failed (%u bytes)\n", static_cast<unsigned>(buf_bytes));
        return;
    }
    g_draw_buf = static_cast<uint16_t*>(buf);

    lv_display_t* disp = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, g_draw_buf, nullptr, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
}
