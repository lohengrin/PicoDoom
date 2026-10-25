// SPI LCD builds (ILI9486 / ST7796U): the game's own DisplayPanel as the LVGL display and the XPT2046
// touch controller sharing the panel's SPI bus. No extra controls.
#include "menu_board.hpp"

#include "pico_toolset/display_panel.h"
#include "pico_toolset/xpt2046.h"
#include "pico_toolset/xpt2046_configs.h"

// DisplayPanel&, not the concrete Ili9486/St7796: the menu only needs the shared windowed /
// DMA-streaming contract, so it is the same code for either panel (src/i_video_spi_lcd.cpp).
extern "C" pico_toolset::DisplayPanel& i_video_lcd_display(void);
extern "C" void lvgl_display_lcd_init(void);                       // lvgl_display_lcd.cpp
extern "C" void lvgl_indev_init_touch(pico_toolset::Xpt2046Touch&); // lvgl_indev.cpp

namespace menu_board {

namespace {
// Must outlive the menu: touch_read_cb dereferences it on every LVGL tick.
pico_toolset::Xpt2046Touch g_touch;
} // namespace

void display_init() { lvgl_display_lcd_init(); }

void touch_init() {
    auto cfg = pico_toolset::configs::xpt2046::kWaveshareRp2350PiZero;
    cfg.spi_instance = i_video_lcd_display().spi();
    g_touch.init(cfg);
    lvgl_indev_init_touch(g_touch);
}

int reserved_bottom_px() { return 0; }
void add_widgets(const Ui&) {}
void poll() {}

} // namespace menu_board
