// USB HID keyboard/mouse input via Pico-Toolset's pico_toolset_usb_hid
// (PIO-USB TinyUSB host). Also owns core1: this stack's SOF-timer IRQ binds
// to whichever core calls tuh_init(), so init() happens from inside
// core1_entry() (via UsbHidHost::init() with run_on_core1=false --
// kWaveshareRp2350PiZeroLcdManualCore1 -- rather than letting init() spawn
// and own core1 itself), letting this same core1 loop also drive
// i_video_core1_step() (the ILI9486 chunked blit, src/i_video_ili9486.cpp)
// interleaved with tuh_task() -- see i_video_core1.hpp for why that
// interleaving (not one blocking per-frame blit call) matters: Pico-PIO-USB's
// software-timed bus servicing would otherwise starve for the ~41ms a full
// frame's SPI feed takes.
//
// I_StartTic (doom/i_system.h) polls the resulting keyboard/mouse state and
// posts DOOM ev_keydown/ev_keyup/ev_mouse events for whatever changed since
// the last tic -- Phase 3, see docs/PLAN.md. C++ driver behind a plain C
// interface, same bridging pattern as src/i_video_ili9486.cpp.
#include "pico_toolset/usb_hid_host.h"
#include "pico_toolset/usb_hid_configs.h"
#include "i_video_core1.hpp"

extern "C" {
#include "doomdef.h"
#include "d_event.h"
#include "d_main.h"
#include "doomstat.h" // mouseSensitivity, for F10/F11's driver-level adjust below
}

#include "pico/multicore.h"

#include <cstdint>
#include <cstdio>

namespace {

pico_toolset::UsbHidHost g_usb_hid;

// core1's stack -- deliberately not the default multicore_launch_core1()
// mechanism, which reserves PICO_CORE1_STACK_SIZE out of the tiny, fixed
// SCRATCH_X SRAM bank (a couple KB at most -- too small for the TinyUSB host
// stack + Pico-PIO-USB's own bit-banging call chains, per TOM6809's
// real-hardware testing on this same board/library combination). 16KB
// carved out of ordinary SRAM instead, matching TOM6809's validated size
// (and pico_toolset_usb_hid's own run_on_core1=true default stack size).
constexpr size_t kCore1StackWords = 4096; // 16KB
uint32_t g_core1_stack[kCore1StackWords];

void core1_entry()
{
    // run_on_core1=false: init() only brings up the host stack (tuh_init())
    // on whichever core calls it -- here, core1, since this runs inside
    // core1's own entry function -- and returns immediately rather than
    // looping itself. This loop then drives both task() and the LCD blit
    // stepper, so a pending frame's SPI feed can't starve USB servicing.
    g_usb_hid.init(pico_toolset::configs::usb_hid::kWaveshareRp2350PiZeroLcdManualCore1);
    while (true) {
        pico_toolset::UsbHidHost::task();
        i_video_core1_step();
    }
}

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
        table[0x42] = KEY_F9;
        // F10/F11/F12 (0x43-0x45) deliberately NOT mapped here: they're
        // intercepted directly below for the mouse toggle/sensitivity
        // controls instead of posted as ordinary DOOM keys -- F10 already
        // means "quit DOOM?" and F11 "cycle gamma" in m_menu.c, and posting
        // both meanings for the same physical key would be confusing.
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

// Mouse on/off + sensitivity controls (F12/F11/F10) -- intercepted here
// rather than posted as ordinary DOOM keys, see hid_to_doom_key()'s doc
// comment on why F10/F11/F12 aren't in that table. "On by default if mouse
// detected": g_mouse_enabled starts true; whether anything actually gets
// posted also depends on the mouse's presence, so no mouse plugged in
// already behaves as "off" without touching this flag.
bool g_mouse_enabled = true;
bool g_prev_f10_down = false;
bool g_prev_f11_down = false;
bool g_prev_f12_down = false;

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

// Launches core1 (see core1_entry() above). Called once from main()
// (src/PicoDoom.cpp), before D_DoomMain() -- gives the keyboard time to
// enumerate while the WAD loads and the engine initializes, rather than
// only starting once the game loop begins.
extern "C" void usb_hid_core1_init(void)
{
    multicore_reset_core1(); // defensive, matches Pico-PIO-USB's own reference examples
    multicore_launch_core1_with_stack(core1_entry, g_core1_stack, sizeof(g_core1_stack));
}

extern "C" void I_StartTic(void)
{
    int i;
    bool ctrl, shift, alt;
    const uint8_t* keymap = hid_to_doom_key();

    bool connected = g_usb_hid.connected_keyboard_count() > 0;
    if (connected != g_prev_connected) {
        printf("PicoDoom: USB keyboard %s\n", connected ? "connected" : "disconnected");
        g_prev_connected = connected;
    }

    for (i = 0x04; i < 0x60; ++i) {
        uint8_t doomkey = keymap[i];
        if (!doomkey)
            continue;
        bool down = g_usb_hid.is_key_down(static_cast<uint8_t>(i));
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

    ctrl = g_usb_hid.is_modifier_down(kModLeftCtrl | kModRightCtrl);
    if (ctrl != g_prev_ctrl) {
        post_key(KEY_RCTRL, ctrl);
        g_prev_ctrl = ctrl;
    }

    shift = g_usb_hid.is_modifier_down(kModLeftShift | kModRightShift);
    if (shift != g_prev_shift) {
        post_key(KEY_RSHIFT, shift);
        g_prev_shift = shift;
    }

    alt = g_usb_hid.is_modifier_down(kModLeftAlt | kModRightAlt);
    if (alt != g_prev_alt) {
        post_key(KEY_RALT, alt);
        g_prev_alt = alt;
    }

    // Mouse toggle (F12) / sensitivity (F11 up, F10 down) -- edge-triggered
    // (fire once per physical press, not once per poll while held), and
    // consumed here rather than posted as DOOM keys (see hid_to_doom_key()).
    {
        bool f10_down = g_usb_hid.is_key_down(0x43);
        bool f11_down = g_usb_hid.is_key_down(0x44);
        bool f12_down = g_usb_hid.is_key_down(0x45);

        if (f12_down && !g_prev_f12_down) {
            g_mouse_enabled = !g_mouse_enabled;
            printf("PicoDoom: mouse %s\n", g_mouse_enabled ? "enabled" : "disabled");
        }
        if (f11_down && !g_prev_f11_down) {
            if (mouseSensitivity < 9)
                mouseSensitivity++;
            printf("PicoDoom: mouse sensitivity %d\n", mouseSensitivity);
        }
        if (f10_down && !g_prev_f10_down) {
            if (mouseSensitivity > 0)
                mouseSensitivity--;
            printf("PicoDoom: mouse sensitivity %d\n", mouseSensitivity);
        }

        g_prev_f10_down = f10_down;
        g_prev_f11_down = f11_down;
        g_prev_f12_down = f12_down;
    }

    {
        pico_toolset::UsbHidHost::MouseState mouse = g_usb_hid.mouse_state();
        if (mouse.present != g_prev_mouse_connected) {
            printf("PicoDoom: USB mouse %s\n", mouse.present ? "connected" : "disconnected");
            g_prev_mouse_connected = mouse.present;
        }

        if (mouse.present) {
            // d_event.h's ev_mouse data1 convention: bit0/1/2 = left/right/
            // middle, matching G_Responder's mousebuttons[0..2] = data1 & 1/2/4.
            uint8_t buttons = static_cast<uint8_t>((mouse.left_button ? 1 : 0) |
                                                    (mouse.right_button ? 2 : 0) |
                                                    (mouse.middle_button ? 4 : 0));
            int dx = 0, dy = 0;
            // Always drain the accumulator, even while disabled -- so
            // toggling back on with F12 doesn't dump a backlog of stale
            // movement into the next tic. consume_mouse_delta() gives raw,
            // unclamped relative movement (unlike mouse_state()'s x/y,
            // which is an absolute cursor clamped to UsbHidConfig's
            // mouse_max_x/y -- wrong for mouselook, since turning would cap
            // out once that virtual cursor pinned at an edge).
            g_usb_hid.consume_mouse_delta(dx, dy);
            // Y inverted from the raw HID report to match DOOM's convention
            // (push the mouse away from you -> positive data3 -> move
            // forward), mirroring the original X11 driver's own
            // MotionNotify handling (doom/i_video.c:
            // `lastmousey - X_event.xmotion.y`).
            dy = -dy;
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
}
