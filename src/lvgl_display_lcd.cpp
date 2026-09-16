// LVGL display driver for the LCD build of the boot WAD-selection menu
// (Phase 4, src/wad_menu.cpp): bridges lv_display_t to the same panel the
// game itself drives (i_video_lcd_display()), via the DisplayPanel
// interface rather than a concrete Ili9486/St7796 -- this file works
// unchanged for either LCD panel this project supports. The LVGL canvas IS
// the panel (480x320), full size -- the menu is its own screen, not a
// layer over DOOM's 320x200 viewport.
//
// Mirrors TOM6809's LvglDisplayDriver (validated on this exact board/panel):
// partial render mode with a small tile buffer (40 rows, 480*40*2 = 38,400
// bytes), flush callback drives the same set_window()/write_pixels()/
// end_write() sequence that core1's game blit uses -- pixels byte-swapped to
// big-endian wire order in place first (write_pixels() expects that, and
// skipping the swap produces wrong/scrambled colors -- TOM6809 confirmed).
//
// Draw buffer: PSRAM-backed like TOM6809's DVI driver's (psram_malloc with a
// plain-heap fallback), NOT the static SRAM array TOM6809's LCD version
// uses: this whole project is much tighter on SRAM than TOM6809 (DOOM's
// tables + the game's two 256KB blit buffers -- see src/i_video_ili9486.cpp).
// Safe because the menu runs entirely on core0 before the game starts; once
// the game begins, LVGL is done and core1 owns the panel.
//
// Ownership handoff: the menu's flush is the only SPI1 bus user while it
// runs (core1 is idle in its blit state machine -- no FIFO work to do), and
// the game's core1 blit never starts until after the menu exits.
#include "pico_toolset/display_panel.h"
#include "pico_toolset/psram.h"
#include "lvgl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

extern "C" pico_toolset::DisplayPanel& i_video_lcd_display(void); // src/i_video_ili9486.cpp / src/i_video_st7796.cpp

namespace {
constexpr int kBufRows = 40;
// Allocated once at init(), never freed -- a boot-time allocation, same
// lifetime convention as TOM6809's drivers.
uint16_t* g_draw_buf = nullptr;

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    auto& lcd = *static_cast<pico_toolset::DisplayPanel*>(lv_display_get_user_data(disp));

    const size_t pixel_count =
        static_cast<size_t>(area->x2 - area->x1 + 1) * static_cast<size_t>(area->y2 - area->y1 + 1);
    auto* pixels = reinterpret_cast<uint16_t*>(px_map);

    // DisplayPanel::write_pixels() expects pixels already byte-swapped to
    // big-endian wire order by the caller (see its doc comment). LVGL's
    // draw buffer holds native (little-endian on this
    // Cortex-M33) RGB565 regardless of LV_COLOR_FORMAT_RGB565's name --
    // identical swap TOM6809's LCD flush does. Swapped in place: this buffer
    // is LVGL's own and not touched again until the next render.
    for (size_t i = 0; i < pixel_count; ++i) {
        uint16_t v = pixels[i];
        pixels[i] = static_cast<uint16_t>((v << 8) | (v >> 8));
    }

    lcd.set_window(area->x1, area->y1, area->x2, area->y2);
    lcd.write_pixels(std::span<const uint16_t>(pixels, pixel_count));
    lcd.end_write();

    lv_display_flush_ready(disp);
}
} // namespace

extern "C" void lvgl_display_lcd_init(void)
{
    pico_toolset::DisplayPanel& panel = i_video_lcd_display();

    const size_t kBufBytes = static_cast<size_t>(panel.width()) * kBufRows * sizeof(uint16_t);
    void* buf = pico_toolset::psram_status().test_ok ? pico_toolset::psram_malloc(kBufBytes)
                                                     : std::malloc(kBufBytes);
    if (!buf) {
        printf("PicoDoom: LVGL draw buffer alloc failed (%u bytes)\n", static_cast<unsigned>(kBufBytes));
        return;
    }
    g_draw_buf = static_cast<uint16_t*>(buf);

    lv_display_t* disp = lv_display_create(panel.width(), panel.height());
    lv_display_set_user_data(disp, &panel);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, g_draw_buf, nullptr, kBufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
}