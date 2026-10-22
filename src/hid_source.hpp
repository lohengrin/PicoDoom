#pragma once

// Backend-neutral view of the attached HID input (keyboard / mouse / gamepad),
// read by I_StartTic() (src/i_input_tic.cpp) and the boot menu's LVGL indevs
// (src/lvgl_indev.cpp). Implemented by the USB host stack (src/i_input_usbhid.cpp)
// and by Bluetooth HID + QwSTPad on the Presto (src/i_input_presto.cpp).
#include "pico_toolset/hid_gamepad.h"

#include <cstddef>
#include <cstdint>

struct HidMouse {
    bool present = false;
    int x = 0, y = 0; // absolute cursor (backend-clamped), menu use only
    bool left = false, right = false, middle = false;
};

class HidSource {
public:
    virtual ~HidSource() = default;
    // HID usage IDs (Keyboard/Keypad page); modifier mask is the boot-protocol byte.
    virtual bool is_key_down(uint8_t usage) const = 0;
    virtual bool is_modifier_down(uint8_t mask) const = 0;
    virtual HidMouse mouse_state() const = 0;
    virtual void consume_mouse_delta(int& dx, int& dy) = 0;
    virtual int consume_mouse_wheel() = 0;
    virtual pico_toolset::GamepadState gamepad_state(size_t index) const = 0;
    virtual uint8_t connected_keyboard_count() const = 0;
};

// Provided by the input backend selected at build time.
HidSource& i_input_hid(void);
// Per-poll service hook: called first thing in I_StartTic() and from the boot
// menu's loop (USB on core0 on HDMI; I2C pad reads on Presto; no-op otherwise).
void i_input_backend_poll(void);
