// Engine-facing input: I_StartTic() polls the active HidSource (src/hid_source.hpp)
// and posts DOOM ev_keydown/ev_keyup/ev_mouse/ev_joystick events for whatever
// changed since the last tic. Backend-neutral; the USB host stack lives in
// src/i_input_usbhid.cpp and Bluetooth HID + QwSTPad in src/i_input_presto.cpp.
// C++ driver behind a plain C interface, same bridging pattern as src/i_video_*.cpp.
#include "hid_source.hpp"
#include "joystick.hpp"
// Pico-specific runtime tuning commands over the USB CDC -- the replacement
// for this file's old F1/F2/F3/F4/F10/F11/F12 intercepts (now freed for
// vanilla DOOM use, see hid_to_doom_key()'s doc comment below).
#include "i_serial_console.hpp"

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
        // F1-F4 (0x3A-0x3D) and F10-F12 (0x43-0x45) are now plain DOOM keys: they
        // used to be intercepted here for Pico-specific tuning (SPI pixel
        // clock, gamma factor, mouse enable/sensitivity) until that control
        // moved to the serial console (src/i_serial_console.cpp). Vanilla
        // bindings they were shadowing resumed: F1 help, F2 save, F3 load,
        // F4 sound volume, F10 quit, F11 gamma toggle (m_menu.c), F12
        // spy-mode (doom/g_game.c).
        table[0x3A] = KEY_F1; table[0x3B] = KEY_F2;
        table[0x3C] = KEY_F3; table[0x3D] = KEY_F4;
        table[0x3E] = KEY_F5; table[0x3F] = KEY_F6; table[0x40] = KEY_F7; table[0x41] = KEY_F8;
        table[0x42] = KEY_F9;
        table[0x43] = KEY_F10; table[0x44] = KEY_F11; table[0x45] = KEY_F12;
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
bool g_prev_mouse_connected = false;
uint8_t g_prev_mouse_buttons = 0;
bool g_prev_gamepad_connected = false;
uint8_t g_prev_joy_buttons = 0;
int g_prev_joy_x = 0;
int g_prev_joy_y = 0;
bool g_prev_pad_start = false;
bool g_prev_pad_back = false;

// Mouselook posting: "on by default if mouse detected" -- g_mouse_enabled
// starts true; whether anything actually gets posted also depends on the
// mouse's presence, so no mouse plugged in already behaves as "off" without
// touching this flag. Toggled from the serial console (`mouse on|off`,
// src/i_serial_console.cpp) since F10/F11/F12 went back to their vanilla
// DOOM meanings.
bool g_mouse_enabled = true;

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

// Mouselook-posting accessors for the serial console (src/i_serial_console.cpp)
// -- the F12 toggle that used to drive g_mouse_enabled is gone.
extern "C" bool i_input_mouse_enabled(void)
{
    return g_mouse_enabled;
}

extern "C" void i_input_set_mouse_enabled(bool enabled)
{
    g_mouse_enabled = enabled;
}

// Re-baselines every edge-tracking latch in this file to the current HID
// state, so a key/button still held when the user confirms (or cancels) the
// boot menu does not get re-posted as a fresh press into the first tics of
// gameplay -- the user is literally still holding Enter from the menu, and
// without this, that same press would fire DOOM's "use" (and the menu Enter)
// on entry. Also drains any accumulated mouse delta. Called once from
// src/wad_menu.cpp, on every exit path (selection or cancel).
extern "C" void i_input_reset_menu_input(void)
{
    HidSource& hid = i_input_hid();
    int dx = 0, dy = 0;
    hid.consume_mouse_delta(dx, dy);
    hid.consume_mouse_wheel();

    for (int i = 0x04; i < 0x60; ++i)
        g_prev_key_down[i] = hid.is_key_down(static_cast<uint8_t>(i));

    constexpr uint8_t kModLeftCtrl = 0x01, kModRightCtrl = 0x10;
    constexpr uint8_t kModLeftShift = 0x02, kModRightShift = 0x20;
    constexpr uint8_t kModLeftAlt = 0x04, kModRightAlt = 0x40;
    g_prev_ctrl = hid.is_modifier_down(kModLeftCtrl | kModRightCtrl);
    g_prev_shift = hid.is_modifier_down(kModLeftShift | kModRightShift);
    g_prev_alt = hid.is_modifier_down(kModLeftAlt | kModRightAlt);

    HidMouse mouse = hid.mouse_state();
    g_prev_mouse_connected = mouse.present;
    g_prev_mouse_buttons = mouse.present ? static_cast<uint8_t>((mouse.left ? 1 : 0) |
                                                                (mouse.right ? 2 : 0) |
                                                                (mouse.middle ? 4 : 0))
                                         : 0;

    pico_toolset::GamepadState pad = hid.gamepad_state(0);
    g_prev_gamepad_connected = pad.present;
    g_prev_joy_buttons = 0;
    g_prev_joy_x = 0;
    g_prev_joy_y = 0;
    g_prev_pad_start = pad.present && pad.down(pico_toolset::kBtStart);
    g_prev_pad_back = pad.present && pad.down(pico_toolset::kBtBack);
    if (pad.present) {
        // Same derivation I_StartTic() uses, so the next ev_joystick edge compares against reality.
        const picodoom::Joy j = picodoom::joystick_from(pad);
        g_prev_joy_x = j.x;
        g_prev_joy_y = j.y;
        g_prev_joy_buttons = j.buttons;
    }

    g_prev_connected = hid.connected_keyboard_count() > 0;
}

#ifdef PICODOOM_DEFERRED_VIDEO_COPY
extern "C" void presto_video_poll(void); // src/i_video_presto.cpp
#endif

extern "C" void I_StartTic(void)
{
#ifdef PICODOOM_DEFERRED_VIDEO_COPY
    presto_video_poll(); // finishes a pending beam-synchronised frame copy (engine idle time)
#endif
    // Pico-specific runtime tuning (serial console, src/i_serial_console.cpp)
    // -- the F1/F2/F3/F4/F10/F11/F12 intercepts that used to do this are
    // gone; poll their replacement here, once per tic.
    i_serial_console_poll();

    int i;
    bool ctrl, shift, alt;
    const uint8_t* keymap = hid_to_doom_key();

    i_input_backend_poll();
    HidSource& hid = i_input_hid();

    bool connected = hid.connected_keyboard_count() > 0;
    if (connected != g_prev_connected) {
        printf("PicoDoom: keyboard %s\n", connected ? "connected" : "disconnected");
        g_prev_connected = connected;
    }

    for (i = 0x04; i < 0x60; ++i) {
        uint8_t doomkey = keymap[i];
        if (!doomkey)
            continue;
        bool down = hid.is_key_down(static_cast<uint8_t>(i));
        if (down != g_prev_key_down[i]) {
            post_key(doomkey, down);
            g_prev_key_down[i] = down;
        }
    }

    // Boot-protocol modifier bitmask (USB HID 1.11 appendix B): bit0/4 =
    // left/right Ctrl, bit1/5 = left/right Shift, bit2/6 = left/right Alt.
    constexpr uint8_t kModLeftCtrl = 0x01, kModRightCtrl = 0x10;
    constexpr uint8_t kModLeftShift = 0x02, kModRightShift = 0x20;
    constexpr uint8_t kModLeftAlt = 0x04, kModRightAlt = 0x40;

    ctrl = hid.is_modifier_down(kModLeftCtrl | kModRightCtrl);
    if (ctrl != g_prev_ctrl) {
        post_key(KEY_RCTRL, ctrl);
        g_prev_ctrl = ctrl;
    }

    shift = hid.is_modifier_down(kModLeftShift | kModRightShift);
    if (shift != g_prev_shift) {
        post_key(KEY_RSHIFT, shift);
        g_prev_shift = shift;
    }

    alt = hid.is_modifier_down(kModLeftAlt | kModRightAlt);
    if (alt != g_prev_alt) {
        post_key(KEY_RALT, alt);
        g_prev_alt = alt;
    }

    {
        HidMouse mouse = hid.mouse_state();
        if (mouse.present != g_prev_mouse_connected) {
            printf("PicoDoom: mouse %s\n", mouse.present ? "connected" : "disconnected");
            g_prev_mouse_connected = mouse.present;
        }

        if (mouse.present) {
            // d_event.h's ev_mouse data1 convention: bit0/1/2 = left/right/
            // middle, matching G_Responder's mousebuttons[0..2] = data1 & 1/2/4.
            uint8_t buttons = static_cast<uint8_t>((mouse.left ? 1 : 0) |
                                                    (mouse.right ? 2 : 0) |
                                                    (mouse.middle ? 4 : 0));
            int dx = 0, dy = 0;
            // Always drain the accumulator, even while disabled -- so
            // toggling back on with F12 doesn't dump a backlog of stale
            // movement into the next tic. consume_mouse_delta() gives raw,
            // unclamped relative movement (unlike mouse_state()'s x/y,
            // which is an absolute cursor clamped to UsbHidConfig's
            // mouse_max_x/y -- wrong for mouselook, since turning would cap
            // out once that virtual cursor pinned at an edge).
            hid.consume_mouse_delta(dx, dy);
            // Y inverted from the raw HID report to match DOOM's convention
            // (push the mouse away from you -> positive data3 -> move
            // forward), mirroring the original X11 driver's own
            // MotionNotify handling (doom/i_video.c:
            // `lastmousey - X_event.xmotion.y`).
            dy = -dy;
            // Wheel detents are impulses: one key-down per detent (G_Responder
            // turns them into a weapon-cycle counter). Always drained so a
            // disabled mouse doesn't build up a backlog.
            int wheel = hid.consume_mouse_wheel();
            if (g_mouse_enabled) {
                for (; wheel > 0; --wheel) post_key(KEY_MWHEELUP, true);
                for (; wheel < 0; ++wheel) post_key(KEY_MWHEELDOWN, true);
            }
            // Matches the original X11 driver's own gate (doom/i_video.c's
            // MotionNotify handling): only post when something actually
            // changed, not an empty event every idle tic.
            if (g_mouse_enabled && (buttons != g_prev_mouse_buttons || dx != 0 || dy != 0)) {
                event_t ev;
                ev.type = ev_mouse;
                ev.data1 = buttons;
                ev.data2 = dx;
                ev.data3 = dy;
                D_PostEvent(&ev);
                g_prev_mouse_buttons = buttons;
            }
        }
    }

    {
        pico_toolset::GamepadState pad = hid.gamepad_state(0);
        if (pad.present != g_prev_gamepad_connected) {
            printf("PicoDoom: gamepad %s\n", pad.present ? "connected" : "disconnected");
            g_prev_gamepad_connected = pad.present;
        }

        // Start / Back (QwSTPad +/-, pad Start/Select) are keys, not joystick
        // buttons (ev_joystick only carries four): Start opens/closes the menu
        // (Esc), Back toggles the automap (Tab).
        const bool pad_start = pad.present && pad.down(pico_toolset::kBtStart);
        const bool pad_back = pad.present && pad.down(pico_toolset::kBtBack);
        if (pad_start != g_prev_pad_start) {
            post_key(KEY_ESCAPE, pad_start);
            g_prev_pad_start = pad_start;
        }
        if (pad_back != g_prev_pad_back) {
            post_key(KEY_TAB, pad_back);
            g_prev_pad_back = pad_back;
        }

        if (pad.present) {
            const picodoom::Joy j = picodoom::joystick_from(pad);
            const uint8_t buttons = j.buttons;
            const int joyx = j.x, joyy = j.y;

            if (buttons != g_prev_joy_buttons || joyx != g_prev_joy_x || joyy != g_prev_joy_y) {
                event_t ev;
                ev.type = ev_joystick;
                ev.data1 = buttons;
                ev.data2 = joyx;
                ev.data3 = joyy;
                D_PostEvent(&ev);
                g_prev_joy_buttons = buttons;
                g_prev_joy_x = joyx;
                g_prev_joy_y = joyy;
            }
        }
    }
}
