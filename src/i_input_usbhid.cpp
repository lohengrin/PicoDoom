// USB HID keyboard/mouse input via Pico-Toolset's pico_toolset_usb_hid
// (PIO-USB TinyUSB host).
//
// LCD build (default, !PICODOOM_HDMI): also owns core1 -- this stack's
// SOF-timer IRQ binds to whichever core calls tuh_init(), so init() happens
// from inside core1_entry() (via UsbHidHost::init() with run_on_core1=false
// -- kWaveshareRp2350PiZeroLcdManualCore1 -- rather than letting init()
// spawn and own core1 itself), letting this same core1 loop also drive
// i_video_core1_step() (the ILI9486 chunked blit, src/i_video_ili9486.cpp)
// interleaved with tuh_task() -- see i_video_core1.hpp for why that
// interleaving (not one blocking per-frame blit call) matters: Pico-PIO-USB's
// software-timed bus servicing would otherwise starve for the ~41ms a full
// frame's SPI feed takes.
//
// HDMI build (PICODOOM_HDMI): core1 belongs exclusively to
// src/i_video_dvi.cpp's DVI encode loop -- libdvi's per-scanline DMA IRQ
// has zero scheduling slack and cannot share a core with Pico-PIO-USB's
// 1ms SOF-timer IRQ (confirmed on real hardware in TOM6809: no picture at
// all when shared). So the USB host stack runs on core0 instead, using the
// toolset's real-hardware-validated kWaveshareRp2350PiZeroHdmi preset
// (run_on_core1=false, its own PIO instance) -- init() happens once from
// src/PicoDoom.cpp's main() (before D_DoomMain(), same as the LCD build's
// core1 launch), and UsbHidHost::task() is polled once per tic from
// I_StartTic() below, which already runs exactly once per tic with no
// engine restructuring needed.
//
// I_StartTic (doom/i_system.h) polls the resulting keyboard/mouse state and
// posts DOOM ev_keydown/ev_keyup/ev_mouse events for whatever changed since
// the last tic -- Phase 3, see docs/PLAN.md. C++ driver behind a plain C
// interface, same bridging pattern as src/i_video_ili9486.cpp.
#include "pico_toolset/usb_hid_host.h"
#include "pico_toolset/usb_hid_configs.h"
#ifndef PICODOOM_HDMI
#include "i_video_core1.hpp"
#endif
// Pico-specific runtime tuning commands over the USB CDC -- the replacement
// for this file's old F1/F2/F3/F4/F10/F11/F12 intercepts (now freed for
// vanilla DOOM use, see hid_to_doom_key()'s doc comment below).
#include "i_serial_console.hpp"

extern "C" {
#include "doomdef.h"
#include "d_event.h"
#include "d_main.h"
}

#include "pico/multicore.h"

#include <cstdint>
#include <cstdio>

namespace {

pico_toolset::UsbHidHost g_usb_hid;

#ifndef PICODOOM_HDMI
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
#endif // !PICODOOM_HDMI

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

#ifndef PICODOOM_HDMI
// Launches core1 (see core1_entry() above). Called once from main()
// (src/PicoDoom.cpp), before D_DoomMain() -- gives the keyboard time to
// enumerate while the WAD loads and the engine initializes, rather than
// only starting once the game loop begins.
extern "C" void usb_hid_core1_init(void)
{
    multicore_reset_core1(); // defensive, matches Pico-PIO-USB's own reference examples
    multicore_launch_core1_with_stack(core1_entry, g_core1_stack, sizeof(g_core1_stack));
}
#else
// HDMI build's counterpart: brings up the host stack on core0 (see file
// header) instead of launching core1 -- core1 belongs to
// src/i_video_dvi.cpp. Called once from main() (src/PicoDoom.cpp), same
// call site/timing as usb_hid_core1_init() above. task() is polled from
// I_StartTic() below, once per tic.
extern "C" void usb_hid_core0_init(void)
{
    g_usb_hid.init(pico_toolset::configs::usb_hid::kWaveshareRp2350PiZeroHdmi);
}
#endif

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

// Boot-menu glue (Phase 4, src/wad_menu.cpp). The LVGL menu needs the same
// USB host stack the game uses: the menu's indevs (src/lvgl_indev.cpp) read
// its keyboard/mouse/gamepad state directly.
extern "C" pico_toolset::UsbHidHost& i_input_usb_hid(void)
{
    return g_usb_hid;
}

// Services the USB host stack from the boot menu's event loop. LCD build:
// no-op -- core1 (core1_entry above) already calls UsbHidHost::task() every
// loop iteration, so the menu must not double-poll the stack from core0.
// HDMI build: core0 owns the stack (this file's whole PICODOOM_HDMI
// arrangement), and the menu loop has no tic cadence to hang a poll off --
// so it calls this directly instead of relying on I_StartTic().
extern "C" void i_input_menu_usb_task(void)
{
#ifdef PICODOOM_HDMI
    pico_toolset::UsbHidHost::task();
#endif
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
    int dx = 0, dy = 0;
    g_usb_hid.consume_mouse_delta(dx, dy);

    for (int i = 0x04; i < 0x60; ++i)
        g_prev_key_down[i] = g_usb_hid.is_key_down(static_cast<uint8_t>(i));

    constexpr uint8_t kModLeftCtrl = 0x01, kModRightCtrl = 0x10;
    constexpr uint8_t kModLeftShift = 0x02, kModRightShift = 0x20;
    constexpr uint8_t kModLeftAlt = 0x04, kModRightAlt = 0x40;
    g_prev_ctrl = g_usb_hid.is_modifier_down(kModLeftCtrl | kModRightCtrl);
    g_prev_shift = g_usb_hid.is_modifier_down(kModLeftShift | kModRightShift);
    g_prev_alt = g_usb_hid.is_modifier_down(kModLeftAlt | kModRightAlt);

    pico_toolset::UsbHidHost::MouseState mouse = g_usb_hid.mouse_state();
    g_prev_mouse_connected = mouse.present;
    g_prev_mouse_buttons = mouse.present ? static_cast<uint8_t>((mouse.left_button ? 1 : 0) |
                                                                (mouse.right_button ? 2 : 0) |
                                                                (mouse.middle_button ? 4 : 0))
                                         : 0;

    pico_toolset::GamepadState pad = g_usb_hid.gamepad_state(0);
    g_prev_gamepad_connected = pad.present;
    g_prev_joy_buttons = 0;
    g_prev_joy_x = 0;
    g_prev_joy_y = 0;
    if (pad.present) {
        // Same sign/button derivation I_StartTic() uses -- match it exactly so
        // the next ev_joystick edge compares against the current reality.
        constexpr int kStickCenter = 128;
        constexpr int kDeadzone = 40;
        int lx = static_cast<int>(pad.lx) - kStickCenter;
        int ly = static_cast<int>(pad.ly) - kStickCenter;
        int joyx = (lx > kDeadzone) ? 1 : (lx < -kDeadzone) ? -1 : 0;
        int joyy = (ly > kDeadzone) ? 1 : (ly < -kDeadzone) ? -1 : 0;
        if (pad.down(pico_toolset::kBtLeft)) joyx = -1;
        else if (pad.down(pico_toolset::kBtRight)) joyx = 1;
        if (pad.down(pico_toolset::kBtUp)) joyy = -1;
        else if (pad.down(pico_toolset::kBtDown)) joyy = 1;
        g_prev_joy_x = joyx;
        g_prev_joy_y = joyy;
        g_prev_joy_buttons = static_cast<uint8_t>((pad.down(pico_toolset::kBtA) ? 1 : 0) |
                                                   (pad.down(pico_toolset::kBtB) ? 2 : 0) |
                                                   (pad.down(pico_toolset::kBtX) ? 4 : 0) |
                                                   (pad.down(pico_toolset::kBtY) ? 8 : 0));
    }

    g_prev_connected = g_usb_hid.connected_keyboard_count() > 0;
}

extern "C" void I_StartTic(void)
{
    // Pico-specific runtime tuning (serial console, src/i_serial_console.cpp)
    // -- the F1/F2/F3/F4/F10/F11/F12 intercepts that used to do this are
    // gone; poll their replacement here, once per tic.
    i_serial_console_poll();

    int i;
    bool ctrl, shift, alt;
    const uint8_t* keymap = hid_to_doom_key();

#ifdef PICODOOM_HDMI
    // core0 owns the USB host stack in this build (see file header) --
    // service it once per tic. Looser than TinyUSB's native once-per-loop-
    // iteration polling (~28.5ms at 35 ticks/sec vs. effectively
    // continuous), a real and permanent cost (not a bug) this build accepts
    // since there's no SPI-feed core1 contention to avoid instead.
    pico_toolset::UsbHidHost::task();
#endif

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

    {
        pico_toolset::GamepadState pad = g_usb_hid.gamepad_state(0);
        if (pad.present != g_prev_gamepad_connected) {
            printf("PicoDoom: USB gamepad %s\n", pad.present ? "connected" : "disconnected");
            g_prev_gamepad_connected = pad.present;
        }

        if (pad.present) {
            // doom/d_event.h's ev_joystick: data1 bits 0-3 = joyarray[0..3]
            // (doom/g_game.c's joybuttons[], mapped via joyb_fire/joyb_strafe/
            // joyb_speed/joyb_use -- default indices 0/1/2/3, doom/m_misc.c),
            // data2/data3 = x/y. G_Responder/G_BuildTiccmd (and m_menu.c's
            // menu navigation) only ever compare these to 0 (joyxmove < 0 /
            // > 0) -- no magnitude, so a plain -1/0/1 sign is both correct
            // and sufficient; no analog turning-speed curve to tune here.
            constexpr int kStickCenter = 128;
            constexpr int kDeadzone = 40; // out of 128 each direction from center
            int lx = static_cast<int>(pad.lx) - kStickCenter;
            // NOT inverted -- pico_toolset::UsbHidHost::gamepad_state()'s
            // documented convention is "high ly = down", matching this
            // reading directly. An apparent inversion here (real-hardware
            // finding, 2026-09) turned out to be a real Pico-Toolset bug
            // instead: scale_axis() could silently wrap at one extreme of
            // an XInput stick's range (see that component's usb_hid_host.cpp),
            // which reads as a sign error at large deflection but is
            // actually non-monotonic overflow -- inverting the reading here
            // "fixed" small deflections while making large ones worse.
            // Checked first for the ARM/x86 char-signedness class of bug this
            // project hit before (docs/PLAN.md) -- ruled out: this whole path
            // uses uint8_t/int16_t throughout, no plain `char` anywhere.
            int ly = static_cast<int>(pad.ly) - kStickCenter;
            int joyx = (lx > kDeadzone) ? 1 : (lx < -kDeadzone) ? -1 : 0;
            int joyy = (ly > kDeadzone) ? 1 : (ly < -kDeadzone) ? -1 : 0;
            // D-pad overrides the stick when held -- it's inherently digital,
            // no deadzone judgment call needed.
            if (pad.down(pico_toolset::kBtLeft)) joyx = -1;
            else if (pad.down(pico_toolset::kBtRight)) joyx = 1;
            if (pad.down(pico_toolset::kBtUp)) joyy = -1;
            else if (pad.down(pico_toolset::kBtDown)) joyy = 1;

            // A/B/X/Y -> fire/strafe/run/use, matching m_misc.c's default
            // joyb_fire=0/joyb_strafe=1/joyb_speed=2/joyb_use=3 bindings.
            uint8_t buttons = static_cast<uint8_t>((pad.down(pico_toolset::kBtA) ? 1 : 0) |
                                                    (pad.down(pico_toolset::kBtB) ? 2 : 0) |
                                                    (pad.down(pico_toolset::kBtX) ? 4 : 0) |
                                                    (pad.down(pico_toolset::kBtY) ? 8 : 0));

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
