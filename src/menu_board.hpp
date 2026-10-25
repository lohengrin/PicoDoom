#pragma once

// The board-specific parts of the boot menu (src/wad_menu.cpp is board-neutral): which LVGL display
// driver and touch controller to bring up, what extra controls the board adds, and what to refresh
// while the menu runs. One implementation per board, picked by CMake:
//   menu_board_spi_lcd.cpp (ILI9486 / ST7796U + XPT2046), menu_board_hdmi.cpp (DVI canvas, no touch),
//   menu_board_presto.cpp (ST7701 + FT6236, Bluetooth pairing controls).
#include "lvgl.h"

namespace menu_board {

// What the board's extra widgets need from the menu.
struct Ui {
    lv_obj_t* screen;
    lv_group_t* group;     // add focusable widgets here
    int hor_res, ver_res;  // canvas size; widgets scale from the 480x320 reference layout
    bool sd_available;
    void (*style_nav_button)(lv_obj_t* button); // the menu's focus/hover styling
};

// Creates the default lv_display_t (after lv_init()); the menu is skipped if none exists afterwards.
void display_init();

// Creates the touch indev, if the board has a touch panel. Must not return before the controller
// object it reads stays alive for the rest of the program.
void touch_init();

// Height, in 480x320 reference pixels, of the board's widgets above the hint row (the WAD list shrinks).
int reserved_bottom_px();

// Adds the board's own controls (once, before the pump loop).
void add_widgets(const Ui& ui);

// Called on every pump-loop pass: refresh status labels etc.
void poll();

} // namespace menu_board
