// Host tests: joystick mapping, LED colours, piezo contour analysis. (Row scaling / LUT conversion is
// tested with its code, in Pico-Toolset: st7701_indexed_rows.h.)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "joystick.hpp"
#include "led_colours.hpp"
#include "piezo_contour.hpp"

using namespace picodoom;
using pico_toolset::GamepadState;

static int g_failures = 0;
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static void test_joystick() {
    GamepadState pad;
    pad.present = true;
    Joy j = joystick_from(pad);
    CHECK(j.buttons == 0 && j.x == 0 && j.y == 0);

    pad.lx = 128 + 41; // just outside the deadzone
    pad.ly = 128 - 41;
    j = joystick_from(pad);
    CHECK(j.x == 1 && j.y == -1);
    pad.lx = 128 + 40; // exactly on the deadzone edge: still centred
    j = joystick_from(pad);
    CHECK(j.x == 0);

    pad.buttons = pico_toolset::kBtLeft | pico_toolset::kBtDown | pico_toolset::kBtA | pico_toolset::kBtY;
    j = joystick_from(pad); // D-pad overrides the stick; A and Y are bits 0 and 3
    CHECK(j.x == -1 && j.y == 1 && j.buttons == 0b1001);
    pad.buttons = pico_toolset::kBtB | pico_toolset::kBtX | pico_toolset::kBtStart;
    j = joystick_from(pad); // Start is not a joystick button (it is posted as a key)
    CHECK(j.buttons == 0b0110);
}

static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

static void test_led_colours() {
    Rgb c = health_colour(100);
    CHECK(near(c.r, 0) && near(c.g, 255) && near(c.b, 0));
    c = health_colour(250); // overheal clamps to green
    CHECK(near(c.g, 255) && near(c.r, 0));
    c = health_colour(50);
    CHECK(near(c.r, 255) && near(c.g, 110) && near(c.b, 0));
    c = health_colour(0);
    CHECK(near(c.r, 255) && near(c.g, 0));
    c = health_colour(-20); // dead: red
    CHECK(near(c.r, 255) && near(c.g, 0));
    c = health_colour(25); // halfway red -> orange
    CHECK(near(c.r, 255) && near(c.g, 55));
    c = health_colour(75); // halfway orange -> green
    CHECK(near(c.r, 127.5f) && near(c.g, 182.5f));

    const Rgb base{10, 20, 30};
    Rgb f = apply_flash(base, Flash::Red, 0, 450); // full flash
    CHECK(near(f.r, 255) && near(f.g, 0) && near(f.b, 0));
    f = apply_flash(base, Flash::Blue, 225, 450); // halfway
    CHECK(near(f.b, (30 + 255) / 2.0f) && near(f.r, 5));
    f = apply_flash(base, Flash::Green, 450, 450); // over
    CHECK(near(f.r, 10) && near(f.g, 20) && near(f.b, 30));
    f = apply_flash(base, Flash::None, 0, 450);
    CHECK(near(f.r, 10));
}

static void test_piezo_contour() {
    // A 1 kHz square wave sampled at 11025 Hz: ~1000 crossings per second = 500 Hz of "half" pairs.
    const uint32_t rate = 11025, n = 276; // 25 ms
    std::vector<uint8_t> square(n);
    for (uint32_t i = 0; i < n; ++i) {
        const double phase = std::fmod(i * 1000.0 / rate, 1.0);
        square[i] = phase < 0.5 ? 200 : 56;
    }
    ContourWindow w = analyze_window(square.data(), n, rate, 150.0f, 3600.0f);
    CHECK(w.hz >= 900 && w.hz <= 1100);
    CHECK(w.level == 255); // deviation 72 on a 40 scale saturates

    // Silence: no crossings -> clamped to the minimum pitch, zero level.
    std::vector<uint8_t> silence(n, 128);
    w = analyze_window(silence.data(), n, rate, 150.0f, 3600.0f);
    CHECK(w.hz == 150 && w.level == 0);

    // Hiss inside the +-6 hysteresis band does not count as pitch.
    std::vector<uint8_t> hiss(n);
    for (uint32_t i = 0; i < n; ++i) hiss[i] = (i & 1) ? 131 : 125;
    w = analyze_window(hiss.data(), n, rate, 150.0f, 3600.0f);
    CHECK(w.hz == 150);

    // Empty window is safe.
    w = analyze_window(silence.data(), 0, rate, 150.0f, 3600.0f);
    CHECK(w.level == 0);
}

int main() {
    test_joystick();
    test_led_colours();
    test_piezo_contour();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all host tests passed\n");
    return 0;
}
