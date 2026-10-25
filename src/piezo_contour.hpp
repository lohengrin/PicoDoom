#pragma once

// Pitch / loudness estimate of one window of 8-bit unsigned PCM, for the piezo's tone fallback
// (src/i_sound_piezo.cpp): zero crossings with a hysteresis band (so hiss around the centre does not
// count) give the pitch, the mean absolute deviation the loudness. Hardware-free.
#include <algorithm>
#include <cstdint>

namespace picodoom {

struct ContourWindow {
    uint16_t hz;
    uint8_t level; // 0..255
};

inline ContourWindow analyze_window(const uint8_t* pcm, uint32_t n, uint32_t rate, float min_hz, float max_hz) {
    if (n == 0 || rate == 0) return {static_cast<uint16_t>(min_hz), 0};
    uint32_t crossings = 0, absdev = 0;
    bool above = pcm[0] >= 128;
    for (uint32_t i = 0; i < n; ++i) {
        const int dev = static_cast<int>(pcm[i]) - 128;
        absdev += static_cast<uint32_t>(dev < 0 ? -dev : dev);
        if (above && dev < -6) { above = false; ++crossings; }
        else if (!above && dev > 6) { above = true; ++crossings; }
    }
    const float seconds = static_cast<float>(n) / static_cast<float>(rate);
    const float hz = std::clamp(static_cast<float>(crossings) * 0.5f / seconds, min_hz, max_hz);
    const float mad = static_cast<float>(absdev) / static_cast<float>(n);
    return {static_cast<uint16_t>(hz), static_cast<uint8_t>(std::min(255.0f, mad * (255.0f / 40.0f)))};
}

} // namespace picodoom
