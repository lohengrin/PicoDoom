// Presto: the ST7701 surface as the LVGL display (lvgl_display_presto.cpp), FT6236 touch, and a
// Bluetooth section: a status line (pad/keyboard/mouse connections, the pairing passkey, the
// headset) and a row of Pair / Forget buttons for input devices and for the audio headset.
#include "menu_board.hpp"

#include "pico/time.h"

#include <cstdint>

extern "C" {
void lvgl_display_presto_init(void);      // lvgl_display_presto.cpp
void lvgl_indev_init_touch_ft6236(void);  // lvgl_indev.cpp
void presto_bt_start_pairing(void);       // i_input_presto.cpp
void presto_bt_forget_all(void);
void presto_audio_start_pairing(void);
void presto_audio_forget(void);
const char* presto_bt_status(void);
}

namespace menu_board {

namespace {
lv_obj_t* g_bt_label = nullptr;
int scale_x(const Ui& ui, int v) { return v * ui.hor_res / 480; }
int scale_y(const Ui& ui, int v) { return v * ui.ver_res / 320; }
} // namespace

void display_init() { lvgl_display_presto_init(); }
void touch_init() { lvgl_indev_init_touch_ft6236(); }

// Status line + button row + hint.
int reserved_bottom_px() { return 42; }

void add_widgets(const Ui& ui) {
    g_bt_label = lv_label_create(ui.screen);
    lv_label_set_text(g_bt_label, presto_bt_status());
    lv_obj_set_style_text_font(g_bt_label, &lv_font_montserrat_12, 0);
    // Device names make the line long: fixed width, scrolling instead of wrapping over the buttons.
    lv_obj_set_width(g_bt_label, ui.hor_res - 2 * scale_x(ui, 8));
    lv_label_set_long_mode(g_bt_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(g_bt_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_bt_label, LV_ALIGN_BOTTOM_MID, 0, -scale_y(ui, 8 + 28 + 14));
    if (!ui.sd_available)
        return;

    lv_obj_t* row = lv_obj_create(ui.screen);
    lv_obj_set_size(row, ui.hor_res - 2 * scale_x(ui, 8), scale_y(ui, 28));
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -scale_y(ui, 8 + 14));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, scale_x(ui, 6), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto make_button = [&](const char* text, lv_event_cb_t cb) {
        lv_obj_t* b = lv_button_create(row);
        lv_obj_set_size(b, scale_x(ui, 104), scale_y(ui, 26));
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
        ui.style_nav_button(b);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_center(l);
        lv_group_add_obj(ui.group, b);
    };
    make_button("Pair pad/kbd", [](lv_event_t*) { presto_bt_start_pairing(); });
    make_button("Forget pad", [](lv_event_t*) { presto_bt_forget_all(); });
    make_button("Pair audio", [](lv_event_t*) { presto_audio_start_pairing(); });
    make_button("Forget audio", [](lv_event_t*) { presto_audio_forget(); });
}

void poll() {
    static uint32_t last_ms = 0;
    const uint32_t t = to_ms_since_boot(get_absolute_time());
    if (g_bt_label != nullptr && t - last_ms >= 250) {
        last_ms = t;
        lv_label_set_text(g_bt_label, presto_bt_status());
    }
}

} // namespace menu_board
