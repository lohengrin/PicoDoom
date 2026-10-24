// LVGL input devices for the boot WAD-selection menu (Phase 4,
// src/wad_menu.cpp): bridges lv_indev_t to PicoDoom's USB HID host and (LCD
// build only) the XPT2046 touchscreen.
//
// Three indevs, mirroring TOM6809's LvglMouseIndev/LvglTouchIndev where they
// exist and adding a keypad indev this project's menu needs (WAD buttons are
// a uniform list on one lv_group, so LVGL's native keypad navigation --
// arrows/move, Enter/click, plus Tab = LV_KEY_NEXT -- drives it with zero
// custom tracking; see wad_menu.cpp):
//
//  - pointer + mouse   (both builds): maps the USB mouse's canonical 640x480
//    cursor into the LVGL canvas -- 480x320 LCD or 320x240 DVI -- exactly
//    like TOM6809's x_num/x_den scaling, and attaches a visible crosshair
//    cursor (no Font Awesome icon font in this project, so a "+" at 28px
//    Montserrat replaces TOM6809's arrow-pointer glyph).
//  - pointer + touch   (LCD build): same Xpt2046Calibration defaults as
//    TOM6809 (measured corners on this exact panel, swap_axes=true), panel
//    space == LVGL canvas space directly.
//  - keypad            (both builds): keyboard (arrows/Enter/Esc/Tab, HID
//    usage IDs) + gamepad (D-pad/stick -> arrows, A/Start -> Enter, B ->
//    Esc) -> LV_KEY_*. One lv_group.
//
// Countdown "user is present" sensing: any of the three indevs marks
// g_input_seen when actual input happens (pointer move/button, touch press,
// any mapped key/button), which stops the menu's 30s auto-start countdown
// (see wad_menu.cpp). Held state re-marks it every poll -- harmless: the
// menu clears the flag once after noticing it, so holding a key still stops
// (and has stopped) the countdown.
#include "hid_source.hpp"
#if defined(PICODOOM_PRESTO)
#include "pico_toolset/ft6236.h"
#include "pico_toolset/ft6236_configs.h"
#elif defined(PICODOOM_SPI_LCD)
#include "pico_toolset/display_panel.h"
#include "pico_toolset/xpt2046.h"
#include "pico_toolset/xpt2046_calibration.h"
#endif
#include "lvgl.h"

#include <cstdint>

#ifdef PICODOOM_SPI_LCD
// DisplayPanel&, not the concrete Ili9486/St7796 -- see wad_menu.cpp's
// identically-declared forward decl for why (works unchanged for either LCD
// panel this project supports).
extern "C" pico_toolset::DisplayPanel& i_video_lcd_display(void); // i_video_spi_lcd.cpp
#endif

namespace {

bool g_input_seen = false;
lv_obj_t* g_cursor = nullptr; // crosshair, hidden on the loading screen

// ---------------------------------------------------------------------------
// pointer + mouse
// ---------------------------------------------------------------------------
struct MouseCtx {
    HidSource* usb = nullptr;
    // Scale factor from the host's canonical 640x480 cursor space into this
    // LVGL canvas. LCD: 480/640, 320/480. DVI (320x240 canvas): 320/640,
    // 240/480.
    int x_num = 1, x_den = 1;
    int y_num = 1, y_den = 1;
    int32_t pos_x = 0, pos_y = 0;
    bool have_pos = false; // false until the first absolute report (skip it)
} g_mouse;

void mouse_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    auto& ctx = g_mouse;
    (void)indev;
    if (ctx.usb == nullptr) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    HidMouse mouse = ctx.usb->mouse_state();
    if (g_cursor != nullptr) {
        if (mouse.present)
            lv_obj_remove_flag(g_cursor, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(g_cursor, LV_OBJ_FLAG_HIDDEN);
    }
    if (!mouse.present) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Map the canonical 640x480 cursor into the LVGL canvas, remembered so
    // the crosshair stays put between (sparse) HID reports. The very first
    // absolute report only establishes the starting position -- it is not
    // "user input" (the cursor was seeded to canvas centre in
    // lvgl_indev_init_mouse, so the first report would otherwise look like a
    // giant mouse jump and cancel the menu's auto-start countdown on boot).
    const int32_t new_x = mouse.x * ctx.x_num / ctx.x_den;
    const int32_t new_y = mouse.y * ctx.y_num / ctx.y_den;
    const bool moved = ctx.have_pos && (new_x != ctx.pos_x || new_y != ctx.pos_y);
    ctx.pos_x = new_x;
    ctx.pos_y = new_y;
    ctx.have_pos = true;

    const bool pressed = mouse.left || mouse.right || mouse.middle;
    if (pressed || moved)
        g_input_seen = true;

    data->point.x = ctx.pos_x;
    data->point.y = ctx.pos_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ---------------------------------------------------------------------------
// pointer + touch (LCD build only)
// ---------------------------------------------------------------------------
#if defined(PICODOOM_PRESTO)
pico_toolset::Ft6236 g_ft6236;

void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    (void)indev;
    const pico_toolset::TouchSample t = g_ft6236.read();
    if (!t.pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    // Presto's FT6236 reports panel pixels directly (kPimoroniPrestoFullRes
    // scales to 480x480); the menu canvas is the top 480x300 of the panel.
    g_input_seen = true;
    data->point.x = t.raw_x;
    data->point.y = t.raw_y;
    data->state = LV_INDEV_STATE_PRESSED;
}
#elif defined(PICODOOM_SPI_LCD)
void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    auto& touch = *static_cast<pico_toolset::Xpt2046Touch*>(lv_indev_get_user_data(indev));

    pico_toolset::Xpt2046Touch::RawSample raw = touch.read();
    if (!raw.pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Default Xpt2046Calibration (measured on this exact panel:
    // swap_axes=true and the four raw corner values in
    // xpt2046_calibration.h), mapping directly into panel pixel space --
    // which IS the LVGL canvas space on the LCD build.
    pico_toolset::Xpt2046Calibration cal;
    const uint16_t raw_h = cal.swap_axes ? raw.raw_y : raw.raw_x;
    const uint16_t raw_v = cal.swap_axes ? raw.raw_x : raw.raw_y;

    pico_toolset::DisplayPanel& panel = i_video_lcd_display();
    const double lcd_x = pico_toolset::touch_calibration_linear_map(
        raw_h, cal.raw_h_min, cal.raw_h_max, 0.0, static_cast<double>(panel.width() - 1));
    const double lcd_y = pico_toolset::touch_calibration_linear_map(
        raw_v, cal.raw_v_min, cal.raw_v_max, 0.0, static_cast<double>(panel.height() - 1));

    g_input_seen = true;
    data->point.x = static_cast<int32_t>(lcd_x);
    data->point.y = static_cast<int32_t>(lcd_y);
    data->state = LV_INDEV_STATE_PRESSED;
}
#endif // touch

// ---------------------------------------------------------------------------
// keypad (both builds)
// ---------------------------------------------------------------------------
void keypad_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    auto* usb = static_cast<HidSource*>(lv_indev_get_user_data(indev));
    data->key = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    if (usb == nullptr)
        return;

    uint32_t key = 0;

    // Keyboard first (HID usage IDs, same set i_input_usbhid.cpp maps).
    // Arrows navigate selection via LV_KEY_PREV/NEXT: LVGL only moves group
    // focus on NEXT/PREV -- raw UP/DOWN keys are handed to the focused
    // widget and a scrollable parent turns them into scrolling instead of
    // selection. Tab stays as a secondary NEXT.
    if (usb->is_key_down(0x52)) key = LV_KEY_PREV;       // Keyboard Up
    else if (usb->is_key_down(0x51)) key = LV_KEY_NEXT;  // Keyboard Down
    else if (usb->is_key_down(0x50)) key = LV_KEY_PREV;  // Keyboard Left
    else if (usb->is_key_down(0x4F)) key = LV_KEY_NEXT;  // Keyboard Right
    else if (usb->is_key_down(0x28)) key = LV_KEY_ENTER; // Keyboard Enter
    else if (usb->is_key_down(0x29)) key = LV_KEY_ESC;   // Keyboard Esc
    else if (usb->is_key_down(0x2B)) key = LV_KEY_NEXT;  // Keyboard Tab

    // Gamepad fallback (only if no keyboard key is down).
    if (key == 0) {
        pico_toolset::GamepadState pad = usb->gamepad_state(0);
        if (pad.present) {
            if (pad.down(pico_toolset::kBtUp)) key = LV_KEY_PREV;
            else if (pad.down(pico_toolset::kBtDown)) key = LV_KEY_NEXT;
            else if (pad.down(pico_toolset::kBtLeft)) key = LV_KEY_PREV;
            else if (pad.down(pico_toolset::kBtRight)) key = LV_KEY_NEXT;
            else if (pad.down(pico_toolset::kBtA) || pad.down(pico_toolset::kBtStart)) key = LV_KEY_ENTER;
            else if (pad.down(pico_toolset::kBtB)) key = LV_KEY_ESC;
        }
    }

    if (key != 0)
        g_input_seen = true;

    data->key = key;
    data->state = key != 0 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

} // namespace

extern "C" void lvgl_indev_init_mouse(HidSource& usb, int canvas_w, int canvas_h)
{
    g_mouse.usb = &usb;
    if (canvas_w > 0 && canvas_h > 0) {
        g_mouse.x_num = canvas_w;
        g_mouse.x_den = 640;
        g_mouse.y_num = canvas_h;
        g_mouse.y_den = 480;
    }
    g_mouse.pos_x = canvas_w / 2;
    g_mouse.pos_y = canvas_h / 2;

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, mouse_read_cb);

    // Visible crosshair cursor. LVGL anchors the cursor widget's top-left at
    // the pointer point (lv_indev.c, "move the cursor if set and moved");
    // a "+" label would therefore hang BELOW the real hit point -- the hover
    // highlight would sit above where the crosshair's centre looks. Translate
    // the label box by its own half-size so the crosshair's visual centre IS
    // the pointer point (good enough precision-wise for the menu's large
    // buttons). White on the dark default theme (this project has no TOM6809
    // Font-Awesome icon font for an arrow glyph).
    lv_obj_t* cursor = lv_label_create(lv_scr_act());
    lv_label_set_text(cursor, "+");
    lv_obj_set_style_text_color(cursor, lv_color_white(), 0);
    lv_obj_set_style_text_font(cursor, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_opa(cursor, LV_OPA_COVER, 0);
    lv_obj_update_layout(cursor); // size the label before reading w/h
    lv_obj_set_style_translate_x(cursor, -(lv_obj_get_width(cursor) / 2), 0);
    lv_obj_set_style_translate_y(cursor, -(lv_obj_get_height(cursor) / 2), 0);
    lv_indev_set_cursor(indev, cursor);
    g_cursor = cursor;
}

#if defined(PICODOOM_PRESTO)
extern "C" void lvgl_indev_init_touch_ft6236(void)
{
    g_ft6236.init(pico_toolset::configs::ft6236::kPimoroniPrestoFullRes);
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
}
#elif defined(PICODOOM_SPI_LCD)
extern "C" void lvgl_indev_init_touch(pico_toolset::Xpt2046Touch& touch)
{
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev, &touch);
    lv_indev_set_read_cb(indev, touch_read_cb);
}
#endif

extern "C" void lvgl_indev_init_keypad(HidSource& usb, lv_group_t* group)
{
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, keypad_read_cb);
    lv_indev_set_user_data(indev, &usb);
    if (group != nullptr)
        lv_indev_set_group(indev, group);
}

extern "C" bool lvgl_indev_input_seen(void)
{
    return g_input_seen;
}

extern "C" void lvgl_indev_input_seen_clear(void)
{
    g_input_seen = false;
}

extern "C" void lvgl_indev_hide_cursor(void)
{
    if (g_cursor != nullptr)
        lv_obj_add_flag(g_cursor, LV_OBJ_FLAG_HIDDEN);
}