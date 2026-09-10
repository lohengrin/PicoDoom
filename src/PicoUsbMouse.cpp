#include "PicoUsbMouse.hpp"

#include <cstdint>

namespace {
volatile bool g_connected = false;
volatile uint8_t g_buttons = 0;

// Accumulator: core1's on_report() adds into it every HID report; core0's
// take_delta() reads and resets it once per tic. Plain volatile ints, no
// lock -- same cross-core convention PicoUsbKeyboard/TOM6809's
// PicoUsbHidInput use elsewhere in this codebase for low-stakes analog
// input: a read on core0 racing a concurrent add on core1 could rarely
// drop or double-count a few units from one report, imperceptible for
// mouse-look smoothness and not worth a lock around a per-tic counter.
volatile int g_accum_dx = 0;
volatile int g_accum_dy = 0;
} // namespace

bool PicoUsbMouse::is_connected() { return g_connected; }

uint8_t PicoUsbMouse::buttons() { return g_buttons; }

void PicoUsbMouse::take_delta(int* dx, int* dy)
{
    *dx = g_accum_dx;
    *dy = g_accum_dy;
    g_accum_dx = 0;
    g_accum_dy = 0;
}

void PicoUsbMouse::on_mount() { g_connected = true; }

void PicoUsbMouse::on_unmount()
{
    g_connected = false;
    g_buttons = 0;
}

void PicoUsbMouse::on_report(const uint8_t* report, uint16_t len)
{
    // Boot protocol: buttons(1) + dx(1, signed) + dy(1, signed), no
    // report-ID prefix (boot-protocol reports are never numbered) -- a
    // report may be longer (padding, a wheel byte), extra bytes ignored.
    // Framing must come from protocol knowledge, not a data-dependent
    // guess: see TOM6809's PicoUsbHidInput::on_mouse_report() doc comment
    // for the real-hardware bug where treating a leading 0x01 as a
    // report-ID prefix misparsed an ordinary held-left-button report
    // (buttons=0x01) as buttons=0.
    if (len < 3)
        return;

    g_buttons = report[0] & 0x07;
    int8_t hid_dx = static_cast<int8_t>(report[1]);
    int8_t hid_dy = static_cast<int8_t>(report[2]);
    g_accum_dx += hid_dx;
    g_accum_dy -= hid_dy; // inverted, see take_delta()'s doc comment
}
