// The boot menu's Bluetooth page on the Presto: Pico-Toolset's common page (libs/bluetooth_ui --
// bonded devices, pair new, connect, forget, for both the HID host and the A2DP headset) drawn
// straight onto the menu surface (the 480x300 RGB332 surface LVGL also flushes to), with its own
// small loop while it is open. LVGL is idle meanwhile; the caller repaints the menu when this returns.
#include "presto_bluetooth.hpp"

#include "hid_source.hpp"
#include "pico_toolset/bluetooth_page_input.h"
#include "pico_toolset/bluetooth_ui_hid.h"
#ifdef PICODOOM_A2DP
#include "pico_toolset/bluetooth_ui_a2dp.h"
#endif

#include "pico/time.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

extern "C" {
uint8_t* i_video_presto_menu_surface(void);       // src/i_video_presto.cpp
bool lvgl_indev_touch_ft6236(int* x, int* y);     // src/lvgl_indev.cpp
}

namespace {

using namespace pico_toolset;

constexpr int kWidth = 480;
constexpr int kHeight = 300;

// Menu surface as a DisplayDriver: RGB565 colours quantised to the RGB332 cube the menu palette uses.
class SurfaceDisplay final : public DisplayDriver {
public:
    explicit SurfaceDisplay(uint8_t* surface) : m_surface(surface) {}

    int width() const override { return kWidth; }
    int height() const override { return kHeight; }

    void set_pixel(int x, int y, Color c) override
    {
        if (x < 0 || y < 0 || x >= kWidth || y >= kHeight)
            return;
        m_surface[y * kWidth + x] = to332(c);
    }
    void fill_rect(int x0, int y0, int x1, int y1, Color c) override
    {
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, kWidth - 1);
        y1 = std::min(y1, kHeight - 1);
        if (x0 > x1)
            return;
        for (int y = y0; y <= y1; ++y)
            std::memset(m_surface + y * kWidth + x0, to332(c), static_cast<size_t>(x1 - x0 + 1));
    }

private:
    static uint8_t to332(Color c)
    {
        const uint16_t v = c.rgb565;
        return static_cast<uint8_t>(((v >> 8) & 0xE0) | ((v >> 6) & 0x1C) | ((v >> 3) & 0x03));
    }
    uint8_t* m_surface;
};

// The menu's HidSource keeps its mouse cursor in 640x480; the page wants display pixels.
class PageHidSource final : public HidSource {
public:
    explicit PageHidSource(HidSource& inner) : m_inner(inner) {}

    bool is_key_down(uint8_t usage) const override { return m_inner.is_key_down(usage); }
    bool is_modifier_down(uint8_t mask) const override { return m_inner.is_modifier_down(mask); }
    HidMouse mouse_state() const override
    {
        HidMouse m = m_inner.mouse_state();
        m.x = m.x * (kWidth - 1) / 639;
        m.y = m.y * (kHeight - 1) / 479;
        return m;
    }
    void consume_mouse_delta(int& dx, int& dy) override { m_inner.consume_mouse_delta(dx, dy); }
    int consume_mouse_wheel() override { return m_inner.consume_mouse_wheel(); }
    GamepadState gamepad_state(size_t index) const override { return m_inner.gamepad_state(index); }
    uint8_t connected_keyboard_count() const override { return m_inner.connected_keyboard_count(); }

private:
    HidSource& m_inner;
};

// Something the page uses is still held (the key that closed it, a finger...).
bool any_input_down(HidSource& hid)
{
    int x, y;
    if (lvgl_indev_touch_ft6236(&x, &y))
        return true;
    const HidMouse m = hid.mouse_state();
    if (m.left || m.right || m.middle)
        return true;
    if (hid.gamepad_state(0).buttons != 0)
        return true;
    for (uint8_t usage : {0x28, 0x29, 0x2A, 0x2C, 0x58, 0x4F, 0x50, 0x51, 0x52, 0x2B})
        if (hid.is_key_down(usage))
            return true;
    return false;
}

void draw_cursor(DisplayDriver& d, int x, int y)
{
    d.fill_rect(x - 5, y - 1, x + 5, y + 1, kColorBlack);
    d.fill_rect(x - 1, y - 5, x + 1, y + 5, kColorBlack);
    d.fill_rect(x - 4, y, x + 4, y, kColorWhite);
    d.fill_rect(x, y - 4, x, y + 4, kColorWhite);
}

constexpr int kCursorReach = 6;

} // namespace

// Runs the page until it is closed. Does nothing if the radio is not up.
extern "C" void presto_bluetooth_page_run(void)
{
    uint8_t* surface = i_video_presto_menu_surface();
    if (!presto_bt_available() || surface == nullptr)
        return;

    static BtUiHidBackend hid_backend(presto_bt_hid());
    static BluetoothDevices devices;
    static bool backends_added = false;
    if (!backends_added) {
        devices.add_backend(hid_backend);
#ifdef PICODOOM_A2DP
        if (BluetoothA2dp* a2dp = presto_a2dp_device()) {
            static BtUiA2dpBackend a2dp_backend(*a2dp);
            devices.add_backend(a2dp_backend);
        }
#endif
        backends_added = true;
    }
    static BluetoothPageController page(devices);
    static BluetoothPageView view(page, {{0, 0, kWidth, kHeight}, bt_ui_input::Touch});
    static PageHidSource hid(i_input_hid());
    static BluetoothPageInput input({.hid = &hid, .has_touch = true});

    SurfaceDisplay display(surface);
    page.open();
    bool full_redraw = true;
    bool cursor_valid = false;
    int cursor_x = 0, cursor_y = 0;

    while (!page.closed()) {
        i_input_backend_poll(); // QwSTPad, Bluetooth mouse motion, LEDs, headset state
        int tx = 0, ty = 0;
        const BtUiPointer finger{lvgl_indev_touch_ft6236(&tx, &ty), tx, ty};
        input.poll(page, view, &finger);
        if (page.closed())
            break;
        if (page.update()) {
            view.draw(display);
            full_redraw = true;
        }

        const HidMouse m = hid.mouse_state();
        if (m.present) {
            if (full_redraw || !cursor_valid || m.x != cursor_x || m.y != cursor_y) {
                if (!full_redraw && cursor_valid) {
                    const BtUiRect spot{cursor_x - kCursorReach, cursor_y - kCursorReach, 2 * kCursorReach + 1,
                                        2 * kCursorReach + 1};
                    view.draw(display, &spot);
                }
                cursor_x = m.x;
                cursor_y = m.y;
                cursor_valid = true;
                draw_cursor(display, cursor_x, cursor_y);
            }
        } else {
            cursor_valid = false;
        }
        full_redraw = false;
        sleep_ms(16);
    }

    // Leave only once nothing used here is held, or the menu behind would take it as a click.
    const absolute_time_t give_up = make_timeout_time_ms(1000);
    while (any_input_down(i_input_hid()) && absolute_time_diff_us(get_absolute_time(), give_up) > 0) {
        i_input_backend_poll();
        sleep_ms(16);
    }
}
