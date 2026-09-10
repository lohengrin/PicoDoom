#pragma once
#include <cstdint>

/**
 * USB-PIO HID keyboard host input (Phase 3), on the Waveshare RP2350-PiZero's
 * integrated USB-PIO port (GPIO28=D+/GPIO29=D-, PICO_DEFAULT_PIO_USB_DP_PIN).
 * A second, PIO-emulated USB *host* controller: the RP2350's native USB
 * peripheral stays committed to stdio_usb's serial console (device mode) --
 * see src/tusb_config.h and CMakeLists.txt for how the two coexist (ported
 * from TOM6809's validated same-board integration; every one of those
 * settings was found the hard way there).
 *
 * The TinyUSB host stack runs entirely on core1 (init() launches it as its
 * own dedicated core) -- Pico-PIO-USB's 1ms SOF timer IRQ is timing-critical
 * and core1 has nothing else to do here. is_key_down()/is_modifier_down()
 * are read from core0 (DOOM's I_StartTic(), see src/i_input_usbhid.cpp); the
 * held-key snapshot is double-buffered so core0 never observes a partially-
 * rebuilt report mid-write (same cross-core convention as TOM6809's
 * PicoUsbHidInput -- a shared single-buffer version showed a real-hardware
 * glitch there from a transiently-all-zero read mid-rebuild).
 */
class PicoUsbKeyboard {
public:
    /// Launches the TinyUSB host stack on core1. Call once, from main(),
    /// before D_DoomMain() -- gives the keyboard time to enumerate while the
    /// WAD loads and the engine initializes, rather than only starting once
    /// the game loop begins.
    static void init();

    /// True if the given USB HID keyboard usage ID (USB HID Usage Tables
    /// 1.12, table 12 -- e.g. 0x04='A', 0x29=Escape, 0x52=Up Arrow) is
    /// currently held, per the most recent report. Core0-safe (see class
    /// doc comment).
    static bool is_key_down(uint8_t hid_usage_id);

    /// True if any modifier bit in `mask` (kMod* below) is currently held.
    static bool is_modifier_down(uint8_t mask);

    /// True if a USB HID keyboard interface is currently mounted. Plain
    /// bool read/write, core0-safe (see class doc comment) -- diagnostic/
    /// UI use (confirming enumeration actually happened), not itself part
    /// of the input-reading path.
    static bool is_keyboard_connected();

    // Internal: called from tuh_hid_mount_cb/tuh_hid_umount_cb in
    // PicoUsbKeyboard.cpp. Runs on core1.
    static void set_keyboard_connected(bool connected);

    static constexpr uint8_t kModLeftCtrl = 0x01;
    static constexpr uint8_t kModLeftShift = 0x02;
    static constexpr uint8_t kModLeftAlt = 0x04;
    static constexpr uint8_t kModRightCtrl = 0x10;
    static constexpr uint8_t kModRightShift = 0x20;
    static constexpr uint8_t kModRightAlt = 0x40;

    // Internal: called from the free-function TinyUSB host callback
    // (tuh_hid_report_received_cb) in PicoUsbKeyboard.cpp, which has no
    // user-data parameter to carry state through. Runs on core1.
    static void on_keyboard_report(const uint8_t* report, uint16_t len);

private:
    static void host_stack_setup();
    static void core1_entry();
};
