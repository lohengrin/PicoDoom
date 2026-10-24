// Presto Bluetooth audio: when a headset/speaker is connected and streaming (A2DP via
// Pico-Toolset's pico_toolset_bluetooth_a2dp), the game's sound goes there instead of the
// piezo (src/i_sound_piezo.cpp stops its output while presto_a2dp_streaming() is true).
//
// No buffer between the game and the encoder, so no added latency: the SBC encoder (BTstack
// context) asks DoomSource::fill() for the next frames of PCM and fill() renders them on the
// spot from the shared mixer (src/presto_mixer.cpp: DMX voices + MUS synth), plus the connect
// chime. Nothing here blocks or prints from the Bluetooth context.
#include "pico_toolset/bluetooth_a2dp.h"
#include "pico_toolset/irq_lock.h"
#include "presto_mixer.hpp"
#include "pico_toolset/audio_source.h"
#include "pico_toolset/psram.h"

#include "hardware/sync.h"
#include "pico/time.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using pico_toolset::A2dpEvent;
using pico_toolset::A2dpEventType;
using pico_toolset::A2dpState;

volatile uint8_t g_channels = 1;
volatile uint32_t g_rate_hz = 44100;

pico_toolset::BluetoothA2dp g_a2dp;
bool g_a2dp_ok = false;

// Last headset event, for the menu status line and a state-change log (callback context:
// plain stores only).
volatile A2dpEventType g_last_event = A2dpEventType::Disconnected;
char g_name[24];
volatile bool g_name_valid = false;
A2dpState g_logged_state = A2dpState::Idle;

// Diagnostics (printed every 5 s while streaming) and the connect chime.
volatile uint32_t g_fill_calls = 0;
uint64_t g_last_diag_us = 0;
uint32_t g_chime_left = 0, g_chime_phase = 0;

class DoomSource final : public pico_toolset::AudioSource {
public:
    void prepare(uint32_t rate, uint8_t channels) override
    {
        g_channels = channels;
        g_rate_hz = rate;
        presto_mixer_set_rate(rate); // flushes sounds in flight (their steps depend on the rate)
    }

    void fill(int16_t* pcm, size_t frames) override
    {
        g_fill_calls = g_fill_calls + 1;
        static int16_t chunk[kMixerMaxChunk];
        const uint8_t ch = g_channels;
        while (frames > 0) {
            const int n = static_cast<int>(std::min<size_t>(frames, kMixerMaxChunk));
            if (!presto_mixer_render(chunk, n)) // another context is rendering: silence this chunk
                std::memset(chunk, 0, sizeof(int16_t) * static_cast<size_t>(n));
            if (g_chime_left > 0) { // short "connected" tone, 880 Hz square, fading out
                const uint32_t inc = static_cast<uint32_t>(880.0 * 4294967296.0 / g_rate_hz);
                for (int i = 0; i < n && g_chime_left > 0; ++i, --g_chime_left) {
                    g_chime_phase += inc;
                    const int32_t amp = static_cast<int32_t>(6000.0f * g_chime_left / (g_rate_hz / 4 + 1));
                    chunk[i] = static_cast<int16_t>(std::clamp<int32_t>(
                        chunk[i] + ((g_chime_phase & 0x80000000u) ? amp : -amp), -32768, 32767));
                }
            }
            for (int i = 0; i < n; ++i)
                for (uint8_t c = 0; c < ch; ++c)
                    *pcm++ = chunk[i];
            frames -= static_cast<size_t>(n);
        }
    }
};
DoomSource g_source;

void on_event(const A2dpEvent& e, void*)
{
    g_last_event = e.type;
    if (e.type == A2dpEventType::Connecting || e.type == A2dpEventType::Connected ||
        e.type == A2dpEventType::DeviceFound) {
        std::memcpy(g_name, e.device.name, sizeof(g_name));
        g_name[sizeof(g_name) - 1] = '\0';
        g_name_valid = g_name[0] != '\0';
    }
}

} // namespace

// Called once from presto_input_init() after the HID host is up.
void presto_a2dp_init()
{
    pico_toolset::A2dpConfig cfg;
    cfg.quality = pico_toolset::A2dpQuality::Mono; // mono mixer; leaves radio time for the pad
    cfg.source = &g_source;
    cfg.callback = on_event;
    cfg.local_name = "PicoDoom";
    cfg.initial_volume = 64;
    g_a2dp_ok = g_a2dp.init(cfg);
    printf("PicoDoom: Bluetooth audio %s%s\n", g_a2dp_ok ? "ready" : "init failed",
           g_a2dp_ok && g_a2dp.has_last_device() ? " (reconnecting to the last headset)" : "");
}

bool presto_a2dp_streaming()
{
    return g_a2dp_ok && g_a2dp.state() == A2dpState::Streaming;
}

bool g_stream_started_flag = false;
bool presto_a2dp_take_stream_started()
{
    const bool f = g_stream_started_flag;
    g_stream_started_flag = false;
    return f;
}

// Logs state changes; called from the input poll hook (game thread).
void presto_a2dp_poll()
{
    if (!g_a2dp_ok)
        return;
    const A2dpState s = g_a2dp.state();
    const uint64_t now_us = time_us_64();
    if (s == A2dpState::Streaming) {
        if (now_us - g_last_diag_us > 5'000'000) {
            g_last_diag_us = now_us;
            const pico_toolset::A2dpStats st = g_a2dp.stats();
            const PrestoMixerStats ms = presto_mixer_take_stats();
            printf("PicoDoom: audio: rate %u ch %u  fills %u  sfx %u music-chunks %u peak %u  "
                   "packets %u dropped %u\n",
                   static_cast<unsigned>(g_rate_hz), static_cast<unsigned>(g_channels),
                   static_cast<unsigned>(g_fill_calls),
                   static_cast<unsigned>(ms.sfx_started), static_cast<unsigned>(ms.music_chunks),
                   static_cast<unsigned>(ms.peak), static_cast<unsigned>(st.packets_sent),
                   static_cast<unsigned>(st.dropped_frames));
        }
    }
    if (s != g_logged_state) {
        if (s == A2dpState::Streaming) {
            g_chime_left = g_rate_hz / 4; // 250 ms
            g_stream_started_flag = true;
        }
        if (g_logged_state == A2dpState::Streaming && s != A2dpState::Streaming)
            presto_mixer_set_rate(44100); // back to the piezo's rate
        g_logged_state = s;
        static const char* const names[] = {"idle", "pairing", "connecting", "connected", "streaming"};
        printf("PicoDoom: Bluetooth audio %s%s%s\n", names[static_cast<int>(s)],
               g_name_valid ? ": " : "", g_name_valid ? g_name : "");
    }
}

void presto_a2dp_start_pairing()
{
    if (g_a2dp_ok)
        g_a2dp.start_pairing();
}

void presto_a2dp_forget()
{
    if (g_a2dp_ok) {
        g_a2dp.disconnect();
        g_a2dp.forget_last();
        g_name_valid = false;
    }
}

// Menu status text ("Audio: ...").
const char* presto_a2dp_status()
{
    static char text[48];
    if (!g_a2dp_ok) {
        snprintf(text, sizeof(text), "Audio: unavailable");
        return text;
    }
    const char* name = g_name_valid ? g_name : (g_a2dp.has_last_device() ? g_a2dp.last_device().name : "");
    switch (g_a2dp.state()) {
    case A2dpState::Pairing: snprintf(text, sizeof(text), "Audio: pairing (headset in pairing mode)"); break;
    case A2dpState::Connecting: snprintf(text, sizeof(text), "Audio: connecting %s", name); break;
    case A2dpState::Connected: snprintf(text, sizeof(text), "Audio: %s connected", name); break;
    case A2dpState::Streaming: snprintf(text, sizeof(text), "Audio: %s", name); break;
    default:
        snprintf(text, sizeof(text), name[0] ? "Audio: %s (off)" : "Audio: piezo", name);
        break;
    }
    return text;
}

