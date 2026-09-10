#pragma once
#include <cstdint>

/**
 * USB-PIO HID mouse host input (Phase 3 follow-up), sharing
 * PicoUsbKeyboard's TinyUSB host stack (rhport 1, core1). TinyUSB's C
 * callback ABI allows only one definition of tuh_hid_mount_cb/
 * tuh_hid_report_received_cb/tuh_hid_umount_cb in the whole program, so
 * those live in PicoUsbKeyboard.cpp and dispatch into this class for
 * HID_ITF_PROTOCOL_MOUSE reports -- the same reason TOM6809's single
 * PicoUsbHidInput class fields every HID device kind from one set of
 * callbacks rather than one set per device class.
 *
 * DOOM's ev_mouse event wants RELATIVE deltas since the last poll (see
 * doom/g_game.c's G_Responder: mousex/mousey come straight from
 * ev->data2/data3, scaled by mouseSensitivity) -- NOT an absolute cursor
 * position like TOM6809's own PicoUsbHidInput::MouseState (that positions
 * an LVGL cursor, a different use case). take_delta() accumulates raw HID
 * dx/dy between calls and resets to zero on read, so each I_StartTic() poll
 * (src/i_input_usbhid.cpp) gets exactly the movement since the previous
 * poll, however many HID reports arrived in between.
 */
class PicoUsbMouse {
public:
    /// True if a USB HID mouse is currently mounted.
    static bool is_connected();

    /// Left/right/middle button state as of the most recent report --
    /// bit0/1/2, matching doom/d_event.h's ev_mouse data1 convention
    /// exactly (G_Responder: mousebuttons[0..2] = data1 & 1/2/4), so this
    /// can be posted as data1 unmodified.
    static uint8_t buttons();

    /// Accumulated X/Y movement since the last call, then reset to zero --
    /// call exactly once per I_StartTic() poll. Y is inverted from the raw
    /// HID report (see on_report()) to match DOOM's convention (push the
    /// mouse away from you -> positive data3 -> move forward), mirroring
    /// the original X11 driver's own MotionNotify handling
    /// (doom/i_video.c: `lastmousey - X_event.xmotion.y`).
    static void take_delta(int* dx, int* dy);

    // Internal: called from PicoUsbKeyboard.cpp's TinyUSB host callbacks.
    // Run on core1.
    static void on_mount();
    static void on_report(const uint8_t* report, uint16_t len);
    static void on_unmount();
};
