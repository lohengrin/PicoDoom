#pragma once

// ev_joystick payload from a gamepad (doom/d_event.h): data1 bits 0-3 = A/B/X/Y (the engine's
// fire/strafe/run/use bindings, joyb_* in doom/m_misc.c), data2/data3 = x/y as -1/0/1 -- the engine
// only compares them to 0, so a deadzoned sign is both correct and sufficient. High ly = down
// (pico_toolset's gamepad convention, so no inversion); the D-pad overrides the stick.
#include "pico_toolset/hid_gamepad.h"

#include <cstdint>

namespace picodoom {

struct Joy {
    uint8_t buttons;
    int x, y;
};

inline Joy joystick_from(const pico_toolset::GamepadState& pad) {
    constexpr int kStickCenter = 128;
    constexpr int kDeadzone = 40; // out of 128 each direction from centre
    const int lx = static_cast<int>(pad.lx) - kStickCenter;
    const int ly = static_cast<int>(pad.ly) - kStickCenter;
    Joy j;
    j.x = (lx > kDeadzone) ? 1 : (lx < -kDeadzone) ? -1 : 0;
    j.y = (ly > kDeadzone) ? 1 : (ly < -kDeadzone) ? -1 : 0;
    if (pad.down(pico_toolset::kBtLeft)) j.x = -1;
    else if (pad.down(pico_toolset::kBtRight)) j.x = 1;
    if (pad.down(pico_toolset::kBtUp)) j.y = -1;
    else if (pad.down(pico_toolset::kBtDown)) j.y = 1;
    j.buttons = static_cast<uint8_t>((pad.down(pico_toolset::kBtA) ? 1 : 0) |
                                     (pad.down(pico_toolset::kBtB) ? 2 : 0) |
                                     (pad.down(pico_toolset::kBtX) ? 4 : 0) |
                                     (pad.down(pico_toolset::kBtY) ? 8 : 0));
    return j;
}

} // namespace picodoom
