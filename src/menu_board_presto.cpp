// Presto: the ST7701 surface as the LVGL display (lvgl_display_presto.cpp), FT6236 touch, and a
// Bluetooth button that opens Pico-Toolset's common Bluetooth page (src/bluetooth_page_presto.cpp).
#include "menu_board.hpp"

#include "pico/time.h"

#include <cstdint>

extern "C" {
void lvgl_display_presto_init(void);      // lvgl_display_presto.cpp
void lvgl_indev_init_touch_ft6236(void);  // lvgl_indev.cpp
void lvgl_indev_input_seen_set(void);
void i_input_reset_menu_input(void);      // i_input_tic.cpp
void presto_bluetooth_page_run(void);     // bluetooth_page_presto.cpp
}
bool presto_bt_available(void);           // i_input_presto.cpp

namespace menu_board {

namespace {
int scale_x(const Ui& ui, int v) { return v * ui.hor_res / 480; }
int scale_y(const Ui& ui, int v) { return v * ui.ver_res / 320; }

void bluetooth_clicked(lv_event_t*) {
    presto_bluetooth_page_run();
    i_input_reset_menu_input();
    lvgl_indev_input_seen_set(); // the user is here: no auto-start
    lv_obj_invalidate(lv_screen_active());
}
} // namespace

void display_init() { lvgl_display_presto_init(); }
void touch_init() { lvgl_indev_init_touch_ft6236(); }

int reserved_bottom_px() { return 0; }

void add_widgets(const Ui& ui) {
    if (!presto_bt_available())
        return;
    lv_obj_t* b = lv_button_create(ui.screen);
    lv_obj_set_size(b, scale_x(ui, 104), scale_y(ui, 26));
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -scale_x(ui, 8), -scale_y(ui, 8));
    lv_obj_add_event_cb(b, bluetooth_clicked, LV_EVENT_CLICKED, nullptr);
    ui.style_nav_button(b);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, "Bluetooth");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);
    lv_group_add_obj(ui.group, b);
}

void poll() {}

} // namespace menu_board
