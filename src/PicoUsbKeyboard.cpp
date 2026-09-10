#include "PicoUsbKeyboard.hpp"

#include "hardware/dma.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pio_usb.h"
#include "tusb.h"
#include "class/hid/hid.h"

#include <cstring>

namespace {
// rhport 0 stays the native USB peripheral, in device mode, for stdio_usb's
// serial console (see src/tusb_config.h). The PIO-USB host root hub is
// rhport 1 -- the standard Pico-PIO-USB convention.
constexpr uint8_t kTuhRhport = 1;

// core1's stack -- deliberately not the default multicore_launch_core1()
// mechanism, which reserves PICO_CORE1_STACK_SIZE out of the tiny, fixed
// SCRATCH_X SRAM bank (a couple KB at most -- too small for the TinyUSB
// host stack + Pico-PIO-USB's own bit-banging call chains, per TOM6809's
// real-hardware testing on this same board/library combination). 16KB
// carved out of ordinary SRAM instead, matching TOM6809's validated size.
constexpr size_t kCore1StackWords = 4096; // 16KB
uint32_t g_core1_stack[kCore1StackWords];

// Double-buffered held-key snapshot, indexed by HID usage ID (0-255):
// on_keyboard_report() (core1) builds the new state entirely into the
// *inactive* buffer, then flips g_active_buf (a single-byte write, atomic
// on this platform) -- is_key_down() (core0) always reads a fully-
// consistent snapshot, never a partial mix mid-rebuild. Same convention
// TOM6809's PicoUsbHidInput uses, adopted there after a shared-buffer
// version showed a real-hardware glitch from a transiently-all-zero read
// landing mid-rebuild.
bool g_key_state[2][256];
volatile uint8_t g_active_buf = 0;
volatile uint8_t g_modifiers = 0;
volatile bool g_keyboard_connected = false;
} // namespace

void PicoUsbKeyboard::on_keyboard_report(const uint8_t* report, uint16_t len)
{
    if (len < sizeof(hid_keyboard_report_t))
        return;
    const auto& kb = *reinterpret_cast<const hid_keyboard_report_t*>(report);

    uint8_t next = static_cast<uint8_t>(1 - g_active_buf);
    bool* buf = g_key_state[next];
    memset(buf, 0, 256);
    for (int i = 0; i < 6; ++i) {
        uint8_t code = kb.keycode[i];
        if (code != 0)
            buf[code] = true;
    }
    g_modifiers = kb.modifier;
    g_active_buf = next;
}

bool PicoUsbKeyboard::is_key_down(uint8_t hid_usage_id)
{
    return g_key_state[g_active_buf][hid_usage_id];
}

bool PicoUsbKeyboard::is_modifier_down(uint8_t mask)
{
    return (g_modifiers & mask) != 0;
}

bool PicoUsbKeyboard::is_keyboard_connected()
{
    return g_keyboard_connected;
}

void PicoUsbKeyboard::set_keyboard_connected(bool connected)
{
    g_keyboard_connected = connected;
}

void PicoUsbKeyboard::host_stack_setup()
{
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PICO_DEFAULT_PIO_USB_DP_PIN;

    // PIO_USB_DEFAULT_CONFIG's tx_ch defaults to a *hardcoded* DMA channel
    // index (0) -- pio_usb_bus_init() claims it via dma_claim_mask(), which
    // panics ("DMA channel already claimed") if that channel is already
    // taken. src/Ili9486Display.cpp's DMA-fed LCD pixel writes claim a
    // channel via dma_claim_unused_channel() during I_InitGraphics(); if
    // that lands on channel 0 first, this would panic. Peek-then-release a
    // genuinely free channel instead of trusting the hardcoded default --
    // same fix TOM6809 needed for an analogous DMA-channel conflict on this
    // same class of bug (see its own PicoUsbHidInput.cpp comment): a first
    // attempt using dma_claim_unused_channel() alone still panicked, since
    // pio_usb_bus_init()'s own dma_claim_mask() then tried to claim the
    // same already-ours channel again.
    pio_cfg.tx_ch = static_cast<uint8_t>(dma_claim_unused_channel(true));
    dma_channel_unclaim(pio_cfg.tx_ch);

    tuh_configure(kTuhRhport, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(kTuhRhport);
}

void PicoUsbKeyboard::core1_entry()
{
    host_stack_setup();
    while (true) {
        tuh_task();
    }
}

void PicoUsbKeyboard::init()
{
    multicore_reset_core1(); // defensive, matches Pico-PIO-USB's own reference examples
    multicore_launch_core1_with_stack(core1_entry, g_core1_stack, sizeof(g_core1_stack));
}

// --- TinyUSB host callback ABI ---------------------------------------------
// Free extern "C" functions: TinyUSB's C callback interface carries no
// user-data parameter. Only one keyboard is ever tracked (unlike TOM6809's
// PicoUsbHidInput, this doesn't also need to distinguish mice/gamepads/
// XInput pads), so these call straight into PicoUsbKeyboard's static state
// rather than through an instance pointer.

extern "C" void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
    (void)desc_report;
    (void)desc_len;
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        PicoUsbKeyboard::set_keyboard_connected(true);
    }
    // Re-arm the report queue, or no reports (not even the first) will ever
    // be delivered. Harmless to do unconditionally for a non-keyboard HID
    // interface too (e.g. a keyboard's own secondary consumer-control
    // collection) -- tuh_hid_report_received_cb() below simply never acts
    // on those reports.
    tuh_hid_receive_report(dev_addr, instance);
}

extern "C" void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        PicoUsbKeyboard::set_keyboard_connected(false);
    }
    // A yanked keyboard simply stops being updated -- on_keyboard_report()
    // won't fire again, so its last snapshot would read as "stuck held"
    // rather than "nothing held". Acceptable for v1 (single input device,
    // reconnect fixes it); revisit if this proves an issue in practice.
}

extern "C" void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        PicoUsbKeyboard::on_keyboard_report(report, len);
    }
    // Re-arm for the next report -- TinyUSB delivers exactly one report per
    // tuh_hid_receive_report() call.
    tuh_hid_receive_report(dev_addr, instance);
}
