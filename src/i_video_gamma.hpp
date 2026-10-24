#pragma once

// PLAYPAL -> display colour, shared by every video backend (they differ only in how they encode the
// result: RGB565 wire order for the SPI LCDs, native RGB565 for DVI, panel palette registers or an
// st7701-encoded LUT on the Presto). The engine only calls I_SetPalette() when it changes the palette
// itself (level start, the options-menu gamma), so the raw PLAYPAL is cached to let the serial
// `gamma` command rebuild the table at a new factor.
extern "C" {
#include "doomdef.h"
#include "v_video.h"
}

#include <cmath>
#include <cstdint>
#include <cstring>

namespace picodoom {

struct Rgb8 {
    uint8_t r, g, b;
};

class GammaPalette {
public:
    // 256 x RGB (the PLAYPAL lump). Core0 only.
    void set_palette(const byte* playpal) { std::memcpy(m_playpal, playpal, sizeof(m_playpal)); }

    [[nodiscard]] float gamma() const { return m_gamma; }

    // Clamped to [0.1, 10]; returns the value in effect.
    float set_gamma(float gamma)
    {
        m_gamma = gamma < 0.1f ? 0.1f : (gamma > 10.0f ? 10.0f : gamma);
        return m_gamma;
    }

    // Entry `i` through the engine's gammatable[usegamma] and then this extra power curve
    // (v/255)^(1/gamma): > 1 brightens, < 1 darkens. At exactly 1.0 the curve is skipped rather than
    // perturbing the palette with float rounding ((v/255) only round-trips exactly for multiples of 255).
    [[nodiscard]] Rgb8 color(int i) const
    {
        byte r = gammatable[usegamma][m_playpal[i * 3 + 0]];
        byte g = gammatable[usegamma][m_playpal[i * 3 + 1]];
        byte b = gammatable[usegamma][m_playpal[i * 3 + 2]];
        if (m_gamma != 1.0f) {
            const float inv = 1.0f / m_gamma;
            r = static_cast<byte>(255.0f * powf(r / 255.0f, inv) + 0.5f);
            g = static_cast<byte>(255.0f * powf(g / 255.0f, inv) + 0.5f);
            b = static_cast<byte>(255.0f * powf(b / 255.0f, inv) + 0.5f);
        }
        return {r, g, b};
    }

    [[nodiscard]] static uint16_t rgb565(const Rgb8& c)
    {
        return static_cast<uint16_t>(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
    }

private:
    byte m_playpal[256 * 3] = {};
    float m_gamma = 1.4f;
};

} // namespace picodoom
