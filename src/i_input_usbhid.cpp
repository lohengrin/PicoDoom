// I_StartTic (doom/i_system.h): polls the USB-PIO HID keyboard
// (src/PicoUsbKeyboard.cpp) and posts DOOM ev_keydown/ev_keyup events for
// whatever changed since the last tic -- Phase 3, see docs/PLAN.md.
// C++ driver behind a plain C interface, same bridging pattern as
// src/i_video_ili9486.cpp.
#include "PicoUsbKeyboard.hpp"

extern "C" {
#include "doomdef.h"
#include "d_event.h"
#include "d_main.h"
}

#include <cstdint>
#include <cstdio>

namespace {

// USB HID Usage Tables 1.12, table 12 (Keyboard/Keypad Page) usage IDs ->
// DOOM keycodes (doomdef.h). Unmapped entries are 0 (never matches a real
// usage ID, which starts at 0x04) and simply ignored below. Letters/digits
// map to plain lowercase ASCII, matching the classic DOS/X11 convention
// DOOM's menu/cheat-code input expects; everything else uses doomdef.h's
// KEY_* constants. Ctrl/Shift/Alt are NOT here -- boot-protocol keyboards
// report those via the separate modifier bitmask, not this keycode array
// (see I_StartTic() below).
//
// Built by a plain function rather than a designated-initializer array
// literal: G++ doesn't support the GNU array-index `[N] = value` extension
// ("sorry, unimplemented: non-trivial designated initializers"), only C's
// compiler does. Runs once (function-local static, C++11 guarantees
// exactly-once init before first use).
const uint8_t* hid_to_doom_key()
{
    static uint8_t table[256];
    static bool built = false;
    if (!built) {
        table[0x04] = 'a'; table[0x05] = 'b'; table[0x06] = 'c'; table[0x07] = 'd';
        table[0x08] = 'e'; table[0x09] = 'f'; table[0x0A] = 'g'; table[0x0B] = 'h';
        table[0x0C] = 'i'; table[0x0D] = 'j'; table[0x0E] = 'k'; table[0x0F] = 'l';
        table[0x10] = 'm'; table[0x11] = 'n'; table[0x12] = 'o'; table[0x13] = 'p';
        table[0x14] = 'q'; table[0x15] = 'r'; table[0x16] = 's'; table[0x17] = 't';
        table[0x18] = 'u'; table[0x19] = 'v'; table[0x1A] = 'w'; table[0x1B] = 'x';
        table[0x1C] = 'y'; table[0x1D] = 'z';
        table[0x1E] = '1'; table[0x1F] = '2'; table[0x20] = '3'; table[0x21] = '4';
        table[0x22] = '5'; table[0x23] = '6'; table[0x24] = '7'; table[0x25] = '8';
        table[0x26] = '9'; table[0x27] = '0';
        table[0x28] = KEY_ENTER; table[0x29] = KEY_ESCAPE; table[0x2A] = KEY_BACKSPACE;
        table[0x2B] = KEY_TAB; table[0x2C] = ' ';
        table[0x2D] = KEY_MINUS; table[0x2E] = KEY_EQUALS;
        table[0x36] = ','; table[0x37] = '.'; table[0x38] = '/';
        table[0x3A] = KEY_F1; table[0x3B] = KEY_F2; table[0x3C] = KEY_F3; table[0x3D] = KEY_F4;
        table[0x3E] = KEY_F5; table[0x3F] = KEY_F6; table[0x40] = KEY_F7; table[0x41] = KEY_F8;
        table[0x42] = KEY_F9; table[0x43] = KEY_F10; table[0x44] = KEY_F11; table[0x45] = KEY_F12;
        table[0x48] = KEY_PAUSE;
        table[0x4F] = KEY_RIGHTARROW; table[0x50] = KEY_LEFTARROW;
        table[0x51] = KEY_DOWNARROW; table[0x52] = KEY_UPARROW;
        built = true;
    }
    return table;
}

// Held state as of the last I_StartTic() poll, indexed the same way as
// hid_to_doom_key()'s table -- diffed each call so only actual press/
// release edges generate events (D_PostEvent expects edges, not level
// state).
bool g_prev_key_down[256];
bool g_prev_ctrl = false;
bool g_prev_shift = false;
bool g_prev_alt = false;
bool g_prev_connected = false;

void post_key(int doomkey, bool down)
{
    event_t ev;
    ev.type = down ? ev_keydown : ev_keyup;
    ev.data1 = doomkey;
    ev.data2 = 0;
    ev.data3 = 0;
    D_PostEvent(&ev);
}

} // namespace

extern "C" void I_StartTic(void)
{
    int i;
    bool ctrl, shift, alt;
    const uint8_t* keymap = hid_to_doom_key();

    bool connected = PicoUsbKeyboard::is_keyboard_connected();
    if (connected != g_prev_connected) {
        printf("PicoDoom: USB keyboard %s\n", connected ? "connected" : "disconnected");
        g_prev_connected = connected;
    }

    for (i = 0x04; i < 0x60; ++i) {
        uint8_t doomkey = keymap[i];
        if (!doomkey)
            continue;
        bool down = PicoUsbKeyboard::is_key_down(static_cast<uint8_t>(i));
        if (down != g_prev_key_down[i]) {
            post_key(doomkey, down);
            g_prev_key_down[i] = down;
        }
    }

    ctrl = PicoUsbKeyboard::is_modifier_down(PicoUsbKeyboard::kModLeftCtrl | PicoUsbKeyboard::kModRightCtrl);
    if (ctrl != g_prev_ctrl) {
        post_key(KEY_RCTRL, ctrl);
        g_prev_ctrl = ctrl;
    }

    shift = PicoUsbKeyboard::is_modifier_down(PicoUsbKeyboard::kModLeftShift | PicoUsbKeyboard::kModRightShift);
    if (shift != g_prev_shift) {
        post_key(KEY_RSHIFT, shift);
        g_prev_shift = shift;
    }

    alt = PicoUsbKeyboard::is_modifier_down(PicoUsbKeyboard::kModLeftAlt | PicoUsbKeyboard::kModRightAlt);
    if (alt != g_prev_alt) {
        post_key(KEY_RALT, alt);
        g_prev_alt = alt;
    }
}
