// HDMI build: DVI canvas as the LVGL display, no touch panel, no extra controls.
#include "menu_board.hpp"

extern "C" void lvgl_display_dvi_init(void); // lvgl_display_dvi.cpp

namespace menu_board {

void display_init() { lvgl_display_dvi_init(); }
void touch_init() {}
int reserved_bottom_px() { return 0; }
void add_widgets(const Ui&) {}
void poll() {}

} // namespace menu_board
