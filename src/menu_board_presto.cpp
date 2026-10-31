// Presto: the ST7701 surface as the LVGL display (lvgl_display_presto.cpp), FT6236 touch, and a
// Bluetooth button that opens Pico-Toolset's common Bluetooth page (its LVGL view, libs/bluetooth_ui_lvgl)
// over the menu: the page takes the keypad while it is open and gives it back when closed.
#include "menu_board.hpp"

#include "presto_bluetooth.hpp"
#include "pico_toolset/bluetooth_page_lvgl.h"
#include "pico_toolset/bluetooth_ui_hid.h"
#ifdef PICODOOM_A2DP
#include "pico_toolset/bluetooth_ui_a2dp.h"
#endif

#include "pico/time.h"


#include <cstdint>

extern "C" {
void lvgl_display_presto_init(void);      // lvgl_display_presto.cpp
void lvgl_indev_init_touch_ft6236(void);  // lvgl_indev.cpp
void lvgl_indev_input_seen_set(void);
void i_input_reset_menu_input(void);      // i_input_tic.cpp
void lvgl_indev_keypad_set_group(lv_group_t* group); // lvgl_indev.cpp
}

namespace menu_board {

namespace {
int scale_x(const Ui& ui, int v) { return v * ui.hor_res / 480; }
int scale_y(const Ui& ui, int v) { return v * ui.ver_res / 320; }

using namespace pico_toolset;

lv_group_t* g_menu_group = nullptr;
BtUiHidBackend* g_hid_backend = nullptr;
BluetoothDevices g_devices;
BluetoothPageController g_controller(g_devices);
BluetoothPageLvgl g_page(g_controller, {.title = "Bluetooth",
                                        .title_font = &lv_font_montserrat_22,
                                        .body_font = &lv_font_montserrat_14,
                                        .passkey_font = &lv_font_montserrat_28,
                                        .pad = 8,
                                        .row_height = 32,
                                        .button_height = 30});

void bluetooth_clicked(lv_event_t*) {
    if (g_page.created())
        return;
    if (g_hid_backend == nullptr) {
        static BtUiHidBackend hid_backend(presto_bt_hid());
        g_hid_backend = &hid_backend;
        g_devices.add_backend(hid_backend);
#ifdef PICODOOM_A2DP
        if (BluetoothA2dp* a2dp = presto_a2dp_device()) {
            static BtUiA2dpBackend a2dp_backend(*a2dp);
            g_devices.add_backend(a2dp_backend);
        }
#endif
    }
    g_controller.open();
    g_page.create(lv_layer_top());
    lvgl_indev_keypad_set_group(g_page.group());
    lvgl_indev_input_seen_set(); // the user is here: no auto-start
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
    g_menu_group = ui.group;
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, "Bluetooth");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);
    lv_group_add_obj(ui.group, b);
}

void poll() {
    static uint32_t last_ms = 0;
    if (!g_page.created())
        return;
    const uint32_t t = to_ms_since_boot(get_absolute_time());
    if (t - last_ms < 30)
        return;
    last_ms = t;
    g_page.update();
    if (g_controller.closed()) {
        g_page.destroy();
        lvgl_indev_keypad_set_group(g_menu_group);
        i_input_reset_menu_input();
        lvgl_indev_input_seen_set();
    }
}

} // namespace menu_board
