#pragma once

// Colour logic of the Presto health LEDs (src/i_leds_presto.cpp), hardware-free.
#include <algorithm>
#include <cstdint>

namespace picodoom {

struct Rgb {
    float r, g, b;
};

enum class Flash : uint8_t { None, Blue, Green, Red };

inline Rgb lerp(const Rgb& a, const Rgb& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// Green at >= 100% health, orange at 50%, red at 0%, linear in between (0..255 channel values).
inline Rgb health_colour(int health) {
    constexpr Rgb kGreen{0, 255, 0}, kOrange{255, 110, 0}, kRed{255, 0, 0};
    const int hp = std::clamp(health, 0, 100);
    return hp >= 50 ? lerp(kOrange, kGreen, static_cast<float>(hp - 50) / 50.0f)
                    : lerp(kRed, kOrange, static_cast<float>(hp) / 50.0f);
}

inline Rgb flash_colour(Flash f) {
    switch (f) {
    case Flash::Red: return {255, 0, 0};
    case Flash::Green: return {0, 255, 0};
    default: return {0, 0, 255};
    }
}

// `base` faded into the flash colour: full flash at age 0, none from `duration_ms` on.
inline Rgb apply_flash(const Rgb& base, Flash f, uint32_t age_ms, uint32_t duration_ms) {
    if (f == Flash::None || age_ms >= duration_ms) return base;
    return lerp(base, flash_colour(f), 1.0f - static_cast<float>(age_ms) / static_cast<float>(duration_ms));
}

} // namespace picodoom
