// Presto input backend: Bluetooth HID host (keyboard / gamepad / mouse over the
// CYW43 radio, Pico-Toolset's pico_toolset_bluetooth_hid, BLE + Classic with
// bonding) merged with a Pimoroni QwSTPad on the QW/ST connector (D-pad, A/B/X/Y,
// +/-), exposed to the shared event code (src/i_input_tic.cpp) and the boot
// menu as one HidSource. No Wi-Fi: only cyw43_arch_none + BTstack is linked.
//
// Threading: BluetoothHid's callbacks and state updates run in BTstack's async
// context; its accessors are the toolset's own cross-context snapshots. The
// QwSTPad (I2C0) is polled from i_input_backend_poll() on core0 only. Core1 does
// nothing but panel scanout out of RAM, which is what makes flash writes from
// Bluetooth bonding safe (PICO_FLASH_ASSUME_CORE1_SAFE, see CMakeLists.txt).
#include "hid_source.hpp"
#include "pico_toolset/bluetooth.h"
#include "pico_toolset/bluetooth_hid.h"
#include "pico_toolset/qwstpad.h"
#include "pico_toolset/qwstpad_configs.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

void i_leds_init(void);   // src/i_leds_presto.cpp
void i_leds_update(void);

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using pico_toolset::BluetoothHid;
using pico_toolset::BtHidEvent;
using pico_toolset::BtHidEventType;
using pico_toolset::GamepadState;
namespace btn = pico_toolset::qwstpad_button;

pico_toolset::Bluetooth g_bt;
BluetoothHid g_hid;
pico_toolset::QwstPad g_pad;
bool g_bt_ok = false;
bool g_pad_ok = false;
uint16_t g_pad_buttons = 0;

// Virtual cursor (canonical 640x480, like the USB mouse path) for the menu, plus
// raw motion for mouselook: both fed by draining BluetoothHid in the poll hook.
int g_cursor_x = 320, g_cursor_y = 240;
int g_acc_dx = 0, g_acc_dy = 0, g_acc_wheel = 0;

volatile uint32_t g_passkey = 0;
volatile bool g_pairing_failed = false;

void on_event(const BtHidEvent& e, void*)
{
    switch (e.type) {
    case BtHidEventType::PasskeyDisplay:
        g_passkey = e.passkey;
        break;
    case BtHidEventType::Paired:
    case BtHidEventType::Connected:
        g_passkey = 0;
        g_pairing_failed = false;
        printf("PicoDoom: Bluetooth %s %s\n",
               e.type == BtHidEventType::Paired ? "paired" : "connected", e.device.name);
        break;
    case BtHidEventType::Disconnected:
        printf("PicoDoom: Bluetooth disconnected %s\n", e.device.name);
        break;
    case BtHidEventType::PairingFailed:
        g_passkey = 0;
        g_pairing_failed = true;
        printf("PicoDoom: Bluetooth pairing failed (status 0x%02x reason 0x%02x)\n", e.status, e.reason);
        break;
    default:
        break;
    }
}

class PrestoHidSource final : public HidSource {
public:
    bool is_key_down(uint8_t usage) const override { return g_bt_ok && g_hid.is_key_down(usage); }
    bool is_modifier_down(uint8_t mask) const override { return g_bt_ok && g_hid.is_modifier_down(mask); }

    HidMouse mouse_state() const override
    {
        HidMouse m;
        if (!g_bt_ok)
            return m;
        const BluetoothHid::MouseState s = g_hid.mouse_state();
        m.present = s.present;
        m.x = g_cursor_x;
        m.y = g_cursor_y;
        m.left = s.left;
        m.right = s.right;
        m.middle = s.middle;
        return m;
    }

    void consume_mouse_delta(int& dx, int& dy) override
    {
        dx = g_acc_dx;
        dy = g_acc_dy;
        g_acc_dx = g_acc_dy = 0;
    }

    int consume_mouse_wheel() override
    {
        const int w = g_acc_wheel;
        g_acc_wheel = 0;
        return w;
    }

    GamepadState gamepad_state(size_t index) const override
    {
        GamepadState s;
        if (g_bt_ok)
            s = g_hid.gamepad_state(index);
        if (index != 0 || !g_pad_ok)
            return s;
        // QwSTPad is pad 0 and adds its buttons to a Bluetooth pad's, if any.
        s.present = true;
        uint16_t b = s.buttons;
        const uint16_t p = g_pad_buttons;
        if (p & btn::A) b |= pico_toolset::kBtA;
        if (p & btn::B) b |= pico_toolset::kBtB;
        if (p & btn::X) b |= pico_toolset::kBtX;
        if (p & btn::Y) b |= pico_toolset::kBtY;
        if (p & btn::Up) b |= pico_toolset::kBtUp;
        if (p & btn::Down) b |= pico_toolset::kBtDown;
        if (p & btn::Left) b |= pico_toolset::kBtLeft;
        if (p & btn::Right) b |= pico_toolset::kBtRight;
        if (p & btn::Plus) b |= pico_toolset::kBtStart;
        if (p & btn::Minus) b |= pico_toolset::kBtBack;
        s.buttons = b;
        return s;
    }

    uint8_t connected_keyboard_count() const override { return g_bt_ok ? g_hid.connected_keyboard_count() : 0; }
};

PrestoHidSource g_source;

} // namespace

HidSource& i_input_hid(void)
{
    return g_source;
}

void i_input_backend_poll(void)
{
    i_leds_update();
    if (g_pad_ok)
        g_pad_buttons = g_pad.read_buttons();
    if (!g_bt_ok)
        return;
    int dx = 0, dy = 0;
    g_hid.consume_mouse_delta(dx, dy);
    if (dx != 0 || dy != 0) {
        g_acc_dx += dx;
        g_acc_dy += dy;
        g_cursor_x = std::clamp(g_cursor_x + dx, 0, 639);
        g_cursor_y = std::clamp(g_cursor_y + dy, 0, 479);
    }
    g_acc_wheel += g_hid.consume_mouse_wheel();
}

// Called once from main(), after PSRAM/uSD and before the boot menu: QwSTPad on
// the QW/ST bus, then the radio (Bluetooth only), reconnecting bonded devices
// and pairing straight away when nothing is bonded yet.
extern "C" void presto_input_init(void)
{
    i_leds_init(); // before the radio, see i_leds_init()
    i2c_init(i2c0, 400'000);
    gpio_set_function(40, GPIO_FUNC_I2C);
    gpio_set_function(41, GPIO_FUNC_I2C);
    gpio_pull_up(40);
    gpio_pull_up(41);
    g_pad_ok = g_pad.init(pico_toolset::configs::qwstpad::kPimoroniPresto);
    printf("PicoDoom: QwSTPad %s\n", g_pad_ok ? "found" : "not found");

    if (cyw43_arch_init() != 0) {
        printf("PicoDoom: cyw43_arch_init failed -- no Bluetooth\n");
        return;
    }
    pico_toolset::BluetoothConfig bc;
    bc.name = "PicoDoom";
    if (!g_bt.init(bc)) {
        printf("PicoDoom: Bluetooth controller did not come up\n");
        return;
    }
    pico_toolset::BluetoothHidConfig hc;
    hc.callback = on_event;
    if (!g_hid.init(hc)) {
        printf("PicoDoom: Bluetooth HID init failed\n");
        return;
    }
    g_bt_ok = true;
    pico_toolset::BtHidDevice bonded[pico_toolset::BluetoothHid::kMaxBonded];
    const size_t n = g_hid.bonded_devices(bonded, pico_toolset::BluetoothHid::kMaxBonded);
    printf("PicoDoom: Bluetooth HID up, %u bonded device(s)\n", static_cast<unsigned>(n));
    if (n == 0)
        g_hid.start_pairing();
}

// -- boot menu hooks ----------------------------------------------------------
extern "C" void presto_bt_start_pairing(void)
{
    if (g_bt_ok) {
        g_pairing_failed = false;
        g_hid.start_pairing();
    }
}

extern "C" void presto_bt_forget_all(void)
{
    if (g_bt_ok)
        g_hid.forget_all();
}

// One-line status for the menu: connection summary, pairing state or passkey.
extern "C" const char* presto_bt_status(void)
{
    static char text[64];
    if (!g_bt_ok) {
        snprintf(text, sizeof(text), "Bluetooth unavailable");
    } else if (g_passkey != 0) {
        snprintf(text, sizeof(text), "Type %06u on the keyboard, then Enter", static_cast<unsigned>(g_passkey));
    } else if (g_hid.pairing()) {
        snprintf(text, sizeof(text), "Pairing... put the device in pairing mode");
    } else if (g_pairing_failed) {
        snprintf(text, sizeof(text), "Pairing failed");
    } else {
        snprintf(text, sizeof(text), "BT: %u kbd  %u pad  %u mouse",
                 g_hid.connected_keyboard_count(), g_hid.connected_gamepad_count(), g_hid.connected_mouse_count());
    }
    return text;
}
