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
#include "pico_toolset/bluetooth_hid_source.h"
#include "pico_toolset/bluetooth_store.h"
#include "pico_toolset/qwstpad.h"
#include "pico_toolset/qwstpad_gamepad.h"
#include "pico_toolset/qwstpad_configs.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

void i_leds_init(void);   // src/i_leds_presto.cpp
void i_leds_update(void);
#ifdef PICODOOM_A2DP
void presto_a2dp_init();            // src/presto_a2dp.cpp
void presto_a2dp_poll();
void presto_a2dp_start_pairing();
void presto_a2dp_forget();
const char* presto_a2dp_status();
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using pico_toolset::BluetoothHid;
using pico_toolset::BtHidEvent;
using pico_toolset::BtHidEventType;
using pico_toolset::GamepadState;

pico_toolset::Bluetooth g_bt;
BluetoothHid g_hid;
pico_toolset::QwstPad g_pad;
bool g_bt_ok = false;
bool g_pad_ok = false;
uint16_t g_pad_buttons = 0;

// Bluetooth devices as a HidSource: keeps the virtual 640x480 cursor for the menu and a raw
// motion/wheel accumulator for mouselook; poll() folds the host's motion into both.
pico_toolset::BluetoothHidSource g_bt_source(g_hid);

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

// Bluetooth keyboard/mouse/gamepad plus the QwSTPad: the pad is gamepad 0 and ORs its buttons into
// a Bluetooth pad's.
class PrestoHidSource final : public HidSource {
public:
    bool is_key_down(uint8_t usage) const override { return g_bt_ok && g_bt_source.is_key_down(usage); }
    bool is_modifier_down(uint8_t mask) const override { return g_bt_ok && g_bt_source.is_modifier_down(mask); }
    HidMouse mouse_state() const override { return g_bt_ok ? g_bt_source.mouse_state() : HidMouse{}; }
    void consume_mouse_delta(int& dx, int& dy) override { g_bt_source.consume_mouse_delta(dx, dy); }
    int consume_mouse_wheel() override { return g_bt_source.consume_mouse_wheel(); }

    GamepadState gamepad_state(size_t index) const override
    {
        GamepadState bt;
        if (g_bt_ok)
            bt = g_bt_source.gamepad_state(index);
        if (index != 0 || !g_pad_ok)
            return bt;
        return pico_toolset::merge(bt, pico_toolset::qwstpad_gamepad_state(g_pad_buttons));
    }

    uint8_t connected_keyboard_count() const override { return g_bt_ok ? g_bt_source.connected_keyboard_count() : 0; }
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
#ifdef PICODOOM_A2DP
    presto_a2dp_poll();
#endif
    if (g_pad_ok)
        g_pad_buttons = g_pad.read_buttons();
    if (g_bt_ok)
        g_bt_source.poll();
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
    {
        // Bonds live in BTstack's flash store, shared with every Pico-Toolset application built for
        // this board: a device paired in one works in the others (docs/components/bluetooth-store.md in
        // the toolset). Dump or erase it with picotool save/erase -r <address> ...
        const pico_toolset::BluetoothStoreInfo si = pico_toolset::bluetooth_store_info();
        printf("PicoDoom: bond store at 0x%08x (%u KB, %u link keys, %u LE bonds, format v%u) -- shared with other toolset apps\n",
               static_cast<unsigned>(si.xip_address), static_cast<unsigned>(si.size_bytes / 1024),
               si.link_key_slots, si.device_db_slots, si.format_version);
    }
    if (n == 0)
        g_hid.start_pairing();
#ifdef PICODOOM_A2DP
    presto_a2dp_init();
#endif
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

extern "C" void presto_audio_start_pairing(void)
{
#ifdef PICODOOM_A2DP
    presto_a2dp_start_pairing();
#endif
}

extern "C" void presto_audio_forget(void)
{
#ifdef PICODOOM_A2DP
    presto_a2dp_forget();
#endif
}

// One-line status for the menu: connection summary, pairing state or passkey.
extern "C" const char* presto_bt_status(void)
{
    static char text[120];
    if (!g_bt_ok) {
        snprintf(text, sizeof(text), "Bluetooth unavailable");
    } else if (g_passkey != 0) {
        snprintf(text, sizeof(text), "Type %06u on the keyboard, then Enter", static_cast<unsigned>(g_passkey));
    } else if (g_hid.pairing()) {
        snprintf(text, sizeof(text), "Pairing... put the device in pairing mode");
    } else if (g_pairing_failed) {
        snprintf(text, sizeof(text), "Pairing failed");
    } else {
        // Names of the connected devices (the toolset stores them with the bond, reads them from LE
        // devices that were bonded without one and asks Classic devices for them); fall back to the kind.
        pico_toolset::BtHidDevice dev[BluetoothHid::kMaxBonded];
        const size_t n = g_hid.bonded_devices(dev, BluetoothHid::kMaxBonded);
        size_t used = static_cast<size_t>(snprintf(text, sizeof(text), "BT: "));
        unsigned connected = 0;
        for (size_t i = 0; i < n && used < sizeof(text) - 1; ++i) {
            if (!dev[i].connected)
                continue;
            const char* label = dev[i].name;
            if (!label[0])
                label = (dev[i].kinds & pico_toolset::bt_hid_kind::Gamepad) ? "gamepad"
                        : (dev[i].kinds & pico_toolset::bt_hid_kind::Keyboard) ? "keyboard"
                        : (dev[i].kinds & pico_toolset::bt_hid_kind::Mouse) ? "mouse" : "device";
            used += static_cast<size_t>(snprintf(text + used, sizeof(text) - used, "%s%.20s", connected ? ", " : "", label));
            ++connected;
        }
        if (connected == 0)
            snprintf(text + used, sizeof(text) - used, "none connected (%u bonded)", static_cast<unsigned>(n));
    }
#ifdef PICODOOM_A2DP
    if (g_bt_ok && g_passkey == 0) {
        const size_t used = strlen(text);
        snprintf(text + used, sizeof(text) - used, "  |  %s", presto_a2dp_status());
    }
#endif
    return text;
}
