// Boot WAD-selection menu (Phase 4): an LVGL screen, shown once from
// src/PicoDoom.cpp between USB-host init and D_DoomMain(), that lists every
// IWAD on the uSD root and starts Doom with the chosen one.
//
// Flow: I_InitGraphics() (once-guarded, see the video drivers) brings the
// output up first -- LCD build: the ILI9486 panel; HDMI build: g_framebuf
// + core1's DVI encoder, which starts scanning as soon as this menu
// begins. Then lv_init() + the per-build LVGL display driver
// (lvgl_display_lcd.cpp / lvgl_display_dvi.cpp), then this file's widgets,
// then a pump loop (lv_tick_inc + lv_timer_handler +, on the HDMI build,
// i_input_menu_usb_task() -- the LCD build's core1 already pols the USB
// stack every loop).
//
// Selection is handed to the engine via wad_boot.cpp's
// pico_wad_select()/pico_selected_wad(); doom/d_main.c's IdentifyVersion()
// reads it (before the fixed-name scan). The last choice persists in
// default.cfg (pico_wad_save_last) so the next boot pre-highlights it and
// M_LoadDefaults() keeps it for a clean-quit re-save -- see wad_boot.cpp.
//
// 30s countdown (visible) auto-starts the pre-highlighted WAD unless any
// input happens first -- the indevs (src/lvgl_indev.cpp) mark "user is
// present" and the menu stops the countdown. Esc / the Skip button / gamepad
// B all cancel; cancelling means pico_selected_wad() stays NULL and the
// engine falls back to its fixed-name scan.
//
// Only ever runs on core0, only once per boot; the game never sees it again.
#include "lvgl.h"
#include "hardware/watchdog.h"
#include "pico_toolset/psram.h"
#include "pico_toolset/sdcard.h"
#include "pico_toolset/usb_hid_host.h"
#ifndef PICODOOM_HDMI
#include "pico_toolset/display_panel.h"
#include "pico_toolset/xpt2046.h"
#include "pico_toolset/xpt2046_configs.h"
#endif
#include "wad_boot.hpp"

#include "pico/time.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// -- external phone-home points (all defined in other src/ TUs) ------------
extern "C" {
void I_InitGraphics(void); // i_video_ili9486.cpp / i_video_dvi.cpp (once-guarded)
void i_input_menu_usb_task(void); // i_input_usbhid.cpp (HDMI build only; no-op on LCD)
void i_input_reset_menu_input(void); // i_input_usbhid.cpp (edge-latch re-baseline)
pico_toolset::UsbHidHost& i_input_usb_hid(void); // i_input_usbhid.cpp
#ifndef PICODOOM_HDMI
// DisplayPanel&, not the concrete Ili9486/St7796 -- this menu only needs
// the shared windowed/DMA-streaming contract (see display_panel.h), so it
// works unchanged for either LCD panel this project supports (see
// src/i_video_ili9486.cpp / src/i_video_st7796.cpp, whichever is compiled
// in for this build's PICODOOM_VIDEO_OUTPUT).
pico_toolset::DisplayPanel& i_video_lcd_display(void); // i_video_ili9486.cpp / i_video_st7796.cpp
void lvgl_display_lcd_init(void); // lvgl_display_lcd.cpp
void lvgl_indev_init_touch(pico_toolset::Xpt2046Touch& touch); // lvgl_indev.cpp
#else
void lvgl_display_dvi_init(void); // lvgl_display_dvi.cpp
#endif
pico_toolset::SdCard& sd_card(void); // sd_stdio.cpp
void lvgl_indev_init_mouse(pico_toolset::UsbHidHost& usb, int canvas_w, int canvas_h);
void lvgl_indev_init_keypad(pico_toolset::UsbHidHost& usb, lv_group_t* group);
bool lvgl_indev_input_seen(void);
void lvgl_indev_input_seen_clear(void);
void lvgl_indev_hide_cursor(void);
}

namespace {

// UI state read by LVGL's C-style callbacks and the pump loop.
volatile int g_selected = -1; // index into g_wads, -1 = none yet
volatile bool g_cancelled = false;
// Set when wad_menu_run(false) is called (no uSD card mounted) -- gates the
// "No uSD card" screen/reboot-only flow below instead of the normal WAD list.
bool g_no_sd_mode = false;

std::vector<std::string> g_wads;

// Arms a watchdog reset and spins until it fires -- same pattern as
// doom/i_system.c's I_Quit (watchdog_reboot(0,0,100) only ARMS the reset;
// the caller must wait for it, not return). Never returns.
[[noreturn]] void reboot_now()
{
    watchdog_reboot(0, 0, 100);
    for (;;)
        tight_loop_contents();
}

bool is_iwad(const char* name)
{
    FILE* f = fopen(name, "rb");
    if (!f)
        return false;
    char magic[4];
    const bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "IWAD", 4) == 0;
    fclose(f);
    return ok;
}

// Case-insensitive match of a stored 8.3 name against the persisted
// last-selection (they may differ in case between the list and the cfg).
bool ci_equal(const std::string& a, const char* b)
{
    if (a.size() != strlen(b))
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return true;
}

void button_event_cb(lv_event_t* e)
{
    g_selected = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
}

void reboot_button_event_cb(lv_event_t* e)
{
    (void)e;
    reboot_now();
}

// Esc bubbles up from the focused button through LV_EVENT_KEY to the screen
// (buttons carry LV_OBJ_FLAG_EVENT_BUBBLE so the group's key events reach
// it) -- cancels the menu. Lv_indev is the keypad indev from lvgl_indev.cpp.
// In no-uSD mode there's no WAD to fall back to, so Esc reboots too, same as
// clicking/pressing the Reboot button -- there's no sensible "cancel" state.
void screen_key_event_cb(lv_event_t* e)
{
    (void)e;
    if (lv_indev_get_key(lv_indev_active()) == LV_KEY_ESC) {
        if (g_no_sd_mode)
            reboot_now();
        g_cancelled = true;
    }
}

} // namespace

// Returns true when the user picked a WAD (now in pico_selected_wad()),
// false when the menu was skipped/cancelled or couldn't come up at all.
// sd_available: false when src/PicoDoom.cpp's sd_init() failed (no card
// mounted, e.g. no card inserted) -- shows a "No uSD card" screen with a
// reboot button instead of trying to enumerate a filesystem that isn't there.
extern "C" bool wad_menu_run(bool sd_available)
{
    g_selected = -1;
    g_cancelled = false;
    g_no_sd_mode = !sd_available;
    g_wads.clear();

    // Output first, before any LVGL exists: LCD brings up + blackens the
    // panel; DVI launches core1's encoder (the menu is then visible as
    // LVGL paints g_framebuf). Both are once-guarded -- the engine calls
    // I_InitGraphics() again later and must be a no-op then.
    I_InitGraphics();

    lv_init();

    lv_display_t* disp = nullptr;
#ifndef PICODOOM_HDMI
    lvgl_display_lcd_init();
#else
    lvgl_display_dvi_init();
#endif
    disp = lv_display_get_default();
    if (disp == nullptr) {
        printf("PicoDoom: LVGL display init failed -- skipping WAD menu\n");
        return false;
    }
    const int hor_res = lv_display_get_horizontal_resolution(disp);
    const int ver_res = lv_display_get_vertical_resolution(disp);

    // Reference layout tuned against the 480x320 LCD panel, scaled to the
    // actual canvas (LCD: identity; DVI: 320x240) -- TOM6809's pattern.
    constexpr int kRefW = 480;
    constexpr int kRefH = 320;
    auto scale_x = [&](int v) { return v * hor_res / kRefW; };
    auto scale_y = [&](int v) { return v * ver_res / kRefH; };

    // Enumerate uSD root for real IWADs (magic-checked) -- skipped entirely
    // in no-uSD mode, nothing to enumerate.
    if (sd_available) {
        const std::vector<std::string> candidates = sd_card().list_files({"wad"});
        for (const auto& n : candidates) {
            if (is_iwad(n.c_str()))
                g_wads.push_back(n);
        }
        printf("PicoDoom: WAD menu -- %u IWAD candidate(s) on uSD\n", static_cast<unsigned>(g_wads.size()));
    }

    // ------------------------------------------------------------------
    // Widgets
    // ------------------------------------------------------------------
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_scr_load(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    const lv_color_t focus_bg = lv_palette_main(LV_PALETTE_ORANGE);
    const lv_color_t focus_fg = lv_color_white();
    const lv_color_t hover_bg = lv_palette_darken(LV_PALETTE_BLUE, 2);
    auto style_nav_button = [&](lv_obj_t* btn) {
        // Keyboard/gamepad selection must be unmistakable: the persisted
        // "default" WAD and whatever arrows move onto both render the
        // focused state, so give FOCUSED a bright, clearly distinct fill
        // (the default theme only shows a thin border). Hover (mouse) and
        // pressed keep lighter variants of the resting dark fill.
        lv_obj_set_style_bg_color(btn, focus_bg, LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(btn, focus_fg, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(btn, hover_bg, LV_STATE_HOVERED);
        lv_obj_set_style_bg_color(btn, lv_palette_darken(LV_PALETTE_BLUE, 3), LV_STATE_PRESSED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    };

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "PicoDoom");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, scale_y(6));

    lv_obj_t* credit = lv_label_create(scr);
    lv_label_set_text(credit, "by Lohengrin");
    lv_obj_set_style_text_font(credit, &lv_font_montserrat_14, 0);
    lv_obj_align(credit, LV_ALIGN_TOP_MID, 0, scale_y(46));

    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, sd_available ? "Enter play   Esc skip   Up/Down choose"
                                          : "Enter/Esc reboot");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -scale_y(8));

    // Countdown ("auto-start"): top-right, tucked between the credit line
    // and the list -- NOT at the bottom where it collides with the centered
    // hint on the 320px DVI canvas. Hidden once real input arrives.
    lv_obj_t* countdown = lv_label_create(scr);
    lv_obj_set_style_text_font(countdown, &lv_font_montserrat_12, 0);
    lv_obj_align(countdown, LV_ALIGN_TOP_RIGHT, -scale_x(10), scale_y(48));

    // WAD buttons: one per IWAD, one column filling the area between the
    // title block and the bottom hint row.
    lv_obj_t* list = lv_obj_create(scr);
    lv_obj_set_size(list, hor_res - 2 * scale_x(16), ver_res - scale_y(64) - scale_y(40));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, scale_y(64));
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, scale_y(6), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);

    if (!sd_available) {
        lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN); // no list content in this mode
        lv_obj_t* empty = lv_label_create(scr);
        lv_label_set_text(empty, "No uSD card");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_22, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -scale_y(20));
    } else if (g_wads.empty()) {
        lv_obj_t* empty = lv_label_create(scr);
        lv_label_set_text(empty, "No IWADs found on uSD");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_22, 0);
        lv_obj_center(empty);
    }

    // Navigation group: every WAD button plus the Skip button (or, in
    // no-uSD mode, just the Reboot button below). Pre-focused to the
    // persisted last choice (or the first WAD).
    lv_group_t* group = lv_group_create();
    lv_group_set_default(group);

    std::vector<lv_obj_t*> buttons;
    buttons.reserve(g_wads.size());
    const char* last = pico_wad_last();
    int focus_idx = 0;
    for (size_t i = 0; i < g_wads.size(); ++i) {
        lv_obj_t* btn = lv_button_create(list);
        // 25% narrower than the list's content width: the scrollbar rides on
        // the list's right edge (outside its content box), so leave it room
        // instead of letting wide buttons slide under it.
        lv_obj_set_size(btn, (lv_obj_get_width(list) - 2 * scale_x(8)) * 3 / 4, scale_y(36));
        lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(btn, button_event_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        style_nav_button(btn);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, g_wads[i].c_str());
        lv_obj_center(lbl);

        lv_group_add_obj(group, btn);
        buttons.push_back(btn);
        if (focus_idx == 0 && ci_equal(g_wads[i], last))
            focus_idx = static_cast<int>(i);
    }

    lv_obj_t* reboot_btn = nullptr;
    if (!sd_available) {
        reboot_btn = lv_button_create(scr);
        lv_obj_set_size(reboot_btn, scale_x(160), scale_y(40));
        lv_obj_align(reboot_btn, LV_ALIGN_CENTER, 0, scale_y(30));
        lv_obj_add_event_cb(reboot_btn, reboot_button_event_cb, LV_EVENT_CLICKED, nullptr);
        style_nav_button(reboot_btn);

        lv_obj_t* reboot_lbl = lv_label_create(reboot_btn);
        lv_label_set_text(reboot_lbl, "Reboot");
        lv_obj_center(reboot_lbl);

        lv_group_add_obj(group, reboot_btn);
    }

    lv_obj_add_event_cb(scr, screen_key_event_cb, LV_EVENT_KEY, nullptr);

    if (reboot_btn != nullptr)
        lv_group_focus_obj(reboot_btn);
    else if (!buttons.empty())
        lv_group_focus_obj(buttons[static_cast<size_t>(focus_idx)]);

    // ------------------------------------------------------------------
    // Indevs
    // ------------------------------------------------------------------
    lvgl_indev_init_mouse(i_input_usb_hid(), hor_res, ver_res);
#ifndef PICODOOM_HDMI
    // Xpt2046Touch must outlive the pump loop -- touch_read_cb dereferences
    // the pointer we hand to lvgl_indev_init_touch on every LVGL tick.
    pico_toolset::Xpt2046Touch touch;
    {
        auto cfg = pico_toolset::configs::xpt2046::kWaveshareRp2350PiZero;
        cfg.spi_instance = i_video_lcd_display().spi();
        touch.init(cfg);
    }
    lvgl_indev_init_touch(touch);
#endif
    lvgl_indev_init_keypad(i_input_usb_hid(), group);

    // ------------------------------------------------------------------
    // Pump loop with 30s auto-start countdown
    // ------------------------------------------------------------------
    constexpr uint32_t kAutoStartMs = 30'000;
    bool countdown_active = !g_wads.empty();
    uint32_t deadline_ms = 0;
    int last_sec = -1;
    if (countdown_active)
        deadline_ms = to_ms_since_boot(get_absolute_time()) + kAutoStartMs;

    absolute_time_t last_tick = get_absolute_time();
    while (g_selected < 0 && !g_cancelled) {
        absolute_time_t now = get_absolute_time();
        uint32_t elapsed_ms = static_cast<uint32_t>(absolute_time_diff_us(last_tick, now) / 1000);
        if (elapsed_ms > 0) {
            lv_tick_inc(elapsed_ms);
            last_tick = now;
        }
        lv_timer_handler();
        i_input_menu_usb_task();

        if (lvgl_indev_input_seen()) {
            lvgl_indev_input_seen_clear();
            countdown_active = false;
            lv_obj_add_flag(countdown, LV_OBJ_FLAG_HIDDEN);
        } else if (countdown_active) {
            const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            if (now_ms >= deadline_ms) {
                // Auto-start the currently focused entry.
                lv_obj_t* foc = lv_group_get_focused(group);
                if (foc != nullptr)
                    g_selected = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(foc)));
                break;
            }
            const int sec = static_cast<int>((deadline_ms - now_ms + 999) / 1000);
            if (sec != last_sec) {
                last_sec = sec;
                char text[32];
                snprintf(text, sizeof(text), "Auto-start in %ds", sec);
                lv_label_set_text(countdown, text);
            }
        }

        sleep_ms(5);
    }

    // Handoff: drain/re-baseline USB edges so whatever was held in the menu
    // doesn't fire into the first gameplay tics.
    i_input_reset_menu_input();

    if (g_selected >= 0 && static_cast<size_t>(g_selected) < g_wads.size()) {
        const char* chosen = g_wads[static_cast<size_t>(g_selected)].c_str();
        printf("PicoDoom: WAD menu chose %s\n", chosen);

        // Fullscreen "Loading..." screen: the engine's own boot (WAD load,
        // zone setup, first render) takes a couple of seconds, so leave the
        // panel on a clear, explicit screen rather than whatever partial
        // state LVGL had. Forced redraw synchronously (lv_refr_now) because
        // we never run the LVGL loop again after returning. Hide the crosshair
        // too -- it lives on the system layer, above every screen.
        lvgl_indev_hide_cursor();
        lv_obj_t* loading = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(loading, lv_color_black(), 0);
        lv_scr_load(loading);

        lv_obj_t* l_title = lv_label_create(loading);
        lv_label_set_text(l_title, "Loading...");
        lv_obj_set_style_text_font(l_title, &lv_font_montserrat_22, 0);
        lv_obj_align(l_title, LV_ALIGN_CENTER, 0, -scale_y(16));

        lv_obj_t* l_wad = lv_label_create(loading);
        lv_label_set_text(l_wad, chosen);
        lv_obj_set_style_text_font(l_wad, &lv_font_montserrat_14, 0);
        lv_obj_align(l_wad, LV_ALIGN_CENTER, 0, scale_y(20));

        lv_obj_del(scr);
        lv_refr_now(disp);

        pico_wad_select(chosen);
        pico_wad_save_last(chosen);
        return true;
    }

    // Skip/cancel path: tear down the menu screen (nothing else draws
    // over it; the engine takes the panel over immediately).
    lv_obj_del(scr);

    printf("PicoDoom: WAD menu skipped -- falling back to fixed-name scan\n");
    pico_wad_select(nullptr);
    return false;
}