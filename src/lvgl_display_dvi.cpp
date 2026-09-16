// LVGL display driver for the HDMI build of the boot WAD-selection menu
// (Phase 4, src/wad_menu.cpp): bridges lv_display_t to the DVI framebuffer
// (src/i_video_dvi.cpp's g_framebuf) that core1's encoder re-scans at 60Hz.
//
// The LVGL canvas is 320x240 -- exactly g_framebuf's own dimensions (see
// src/i_video_dvi.cpp's header for why the game itself is 320x240 here: the
// TMDS encoder doubles each source pixel to 2 physical columns and
// DVI_VERTICAL_REPEAT doubles rows, so one framebuffer pixel = 2x2 physical
// = square). This is deliberately NOT TOM6809's PicoDviLvglDisplayDriver
// shape (their canvas is 640x480 with a 2:1 vertical box-filter fold):
// PicoDoom's g_framebuf is only 320 wide, and the DVI canvas sharing its
// resolution means flush_cb is a plain 1:1 native-endian RGB565 copy -- no
// fold, no byte swap (this encoder reads native uint32 words, unlike the
// LCD's SPI wire order).
//
// Draw buffer: PSRAM-backed (psram_malloc with plain-heap fallback), same
// single-core reasoning as src/lvgl_display_lcd.cpp -- the menu runs on
// core0 only; core1's encoder is pure read from g_framebuf. Core0 writing
// g_framebuf rows while core1 scans them can tear a row mid-update (exactly
// the game's own behavior, see I_FinishUpdate) -- cosmetic only.
#include "pico_toolset/psram.h"
#include "lvgl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" uint16_t* i_video_dvi_framebuf(void); // src/i_video_dvi.cpp
extern "C" int i_video_dvi_width(void);
extern "C" int i_video_dvi_height(void);

namespace {
constexpr int kBufRows = 40;
// Allocated once at init(), never freed (boot-time allocation).
uint16_t* g_draw_buf = nullptr;

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    uint16_t* framebuf = i_video_dvi_framebuf();
    const int canvas_w = i_video_dvi_width();
    const int canvas_h = i_video_dvi_height();

    // Clamp to the canvas like TOM6809's DVI flush does -- LVGL is expected
    // to clip every flush area itself, but an out-of-bounds copy here would
    // write past g_framebuf's end into whatever static memory follows it.
    int x1 = area->x1 < 0 ? 0 : area->x1;
    int y1 = area->y1 < 0 ? 0 : area->y1;
    int x2 = area->x2 >= canvas_w ? canvas_w - 1 : area->x2;
    int y2 = area->y2 >= canvas_h ? canvas_h - 1 : area->y2;
    if (x2 < x1 || y2 < y1) {
        lv_display_flush_ready(disp);
        return;
    }

    const size_t area_w = static_cast<size_t>(area->x2 - area->x1 + 1); // px_map's own stride
    const auto* src = reinterpret_cast<const uint16_t*>(px_map) + (x1 - area->x1) + static_cast<size_t>(y1 - area->y1) * area_w;
    uint16_t* dst = framebuf + static_cast<size_t>(y1) * canvas_w + x1;

    const size_t row_bytes = static_cast<size_t>(x2 - x1 + 1) * sizeof(uint16_t);
    for (int row = y1; row <= y2; ++row) {
        memcpy(dst, src, row_bytes);
        src += area_w;
        dst += canvas_w;
    }

    lv_display_flush_ready(disp);
}
} // namespace

extern "C" void lvgl_display_dvi_init(void)
{
    const int canvas_w = i_video_dvi_width();
    const int canvas_h = i_video_dvi_height();
    if (canvas_w <= 0 || canvas_h <= 0)
        return;

    const size_t buf_bytes = static_cast<size_t>(canvas_w) * kBufRows * sizeof(uint16_t);
    void* buf = pico_toolset::psram_status().test_ok ? pico_toolset::psram_malloc(buf_bytes)
                                                     : std::malloc(buf_bytes);
    if (!buf) {
        printf("PicoDoom: LVGL draw buffer alloc failed (%u bytes)\n", static_cast<unsigned>(buf_bytes));
        return;
    }
    g_draw_buf = static_cast<uint16_t*>(buf);

    lv_display_t* disp = lv_display_create(canvas_w, canvas_h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, g_draw_buf, nullptr, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
}