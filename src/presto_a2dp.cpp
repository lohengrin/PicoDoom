// Presto Bluetooth audio: when a headset/speaker is connected and streaming (A2DP via
// Pico-Toolset's pico_toolset_bluetooth_a2dp), the game's real sound goes there instead
// of the piezo's tone approximation (src/i_sound_piezo.cpp hands over while
// presto_a2dp_streaming() is true).
//
// Data flow -- no buffer between the game and the encoder, so no added latency:
//   BTstack context: SBC encoder asks DoomSource::fill() for the next frames of PCM,
//   fill() mixes them right there: the 8 DMX voices (mono, linear interpolation) and the
//   10-voice MUS synth (mus_render()), plus the connect chime.
// The game thread only changes mixer state (start/stop a voice, music commands) and does so
// inside short interrupt-off sections (IrqLock), so the Bluetooth context, which preempts it,
// never sees a half-updated voice or sequencer. The same IrqLock guards every mus_* call made
// from src/i_sound_piezo.cpp. Nothing here blocks or prints from the Bluetooth context.
#include "pico_toolset/bluetooth_a2dp.h"
#include "pico_toolset/audio_source.h"
#include "pico_toolset/psram.h"
#include "mus_player.hpp"

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

constexpr int kNumVoices = 8;
constexpr int kChunk = 128; // largest fill() request is one SBC frame (blocks * subbands <= 128)

volatile uint32_t g_rate = 44100;
volatile uint8_t g_channels = 1;
volatile bool g_rate_changed = false;
volatile bool g_enabled = false;  // false: silence (game muted); music is not rendered either
volatile int g_music_volume = 127;

// RAII interrupt-off section for game-thread access to state the Bluetooth context also uses.
struct IrqLock {
    uint32_t flags = save_and_disable_interrupts();
    ~IrqLock() { restore_interrupts(flags); }
};

// Mixing buffers: static, not stack (the Bluetooth context runs on core 0's small stack).
int32_t g_acc[kChunk];
float g_mus_l[kChunk], g_mus_r[kChunk];

struct Voice {
    bool active = false;
    int handle = 0;
    const uint8_t* data = nullptr;
    uint32_t length = 0;
    uint32_t pos = 0;  // 16.16 source position
    uint32_t step = 0; // 16.16 source samples per output sample
    int vol = 0;       // 0..127
};
Voice g_voices[kNumVoices];

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
uint32_t g_sfx_started = 0, g_music_chunks = 0, g_peak = 0;
uint64_t g_last_diag_us = 0;
uint32_t g_chime_left = 0, g_chime_phase = 0;

int16_t clamp16(int32_t v);
void mix_chunk(int n);

class DoomSource final : public pico_toolset::AudioSource {
public:
    void prepare(uint32_t rate, uint8_t channels) override
    {
        g_rate = rate;
        g_channels = channels;
        g_rate_changed = true;
    }

    void fill(int16_t* pcm, size_t frames) override
    {
        g_fill_calls = g_fill_calls + 1;
        if (g_rate_changed) { // sounds in flight were stepped for the old rate
            g_rate_changed = false;
            for (Voice& v : g_voices)
                v.active = false;
        }
        const uint8_t ch = g_channels;
        while (frames > 0) {
            const int n = static_cast<int>(std::min<size_t>(frames, kChunk));
            mix_chunk(n);
            for (int i = 0; i < n; ++i) {
                const int16_t o = clamp16(g_acc[i]);
                for (uint8_t c = 0; c < ch; ++c)
                    *pcm++ = o;
            }
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

int16_t clamp16(int32_t v)
{
    return static_cast<int16_t>(std::clamp<int32_t>(v, -32768, 32767));
}

void mix_chunk(int n)
{
    std::memset(g_acc, 0, sizeof(int32_t) * static_cast<size_t>(n));
    if (g_enabled) {
        for (Voice& v : g_voices) {
            if (!v.active)
                continue;
            uint32_t pos = v.pos;
            const uint32_t step = v.step;
            const uint32_t len = v.length;
            for (int i = 0; i < n; ++i) {
                const uint32_t idx = pos >> 16;
                if (idx + 1 >= len) {
                    v.active = false;
                    break;
                }
                const int s0 = v.data[idx], s1 = v.data[idx + 1];
                const int s = s0 + (((s1 - s0) * static_cast<int>((pos >> 8) & 0xFF)) >> 8);
                g_acc[i] += (s - 128) * v.vol;
                pos += step;
            }
            v.pos = pos;
        }
        if (g_music_volume > 0 && mus_is_playing()) {
            std::memset(g_mus_l, 0, sizeof(float) * static_cast<size_t>(n));
            std::memset(g_mus_r, 0, sizeof(float) * static_cast<size_t>(n));
            mus_render(g_mus_l, g_mus_r, n);
            ++g_music_chunks;
            for (int i = 0; i < n; ++i)
                g_acc[i] += static_cast<int32_t>(g_mus_l[i] * 20000.0f);
        }
    }
    if (g_chime_left > 0) { // short "connected" tone, 880 Hz square, fading out
        const uint32_t inc = static_cast<uint32_t>(880.0 * 4294967296.0 / g_rate);
        for (int i = 0; i < n && g_chime_left > 0; ++i, --g_chime_left) {
            g_chime_phase += inc;
            const int32_t amp = static_cast<int32_t>(6000.0f * g_chime_left / (g_rate / 4 + 1));
            g_acc[i] += (g_chime_phase & 0x80000000u) ? amp : -amp;
        }
    }
    for (int i = 0; i < n; ++i) {
        const int32_t a = g_acc[i] < 0 ? -g_acc[i] : g_acc[i];
        g_peak = std::max<uint32_t>(g_peak, static_cast<uint32_t>(std::min<int32_t>(a, 32767)));
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
            printf("PicoDoom: audio: rate %u ch %u  fills %u  sfx %u music-chunks %u peak %u  "
                   "packets %u dropped %u\n",
                   static_cast<unsigned>(g_rate), static_cast<unsigned>(g_channels),
                   static_cast<unsigned>(g_fill_calls),
                   static_cast<unsigned>(g_sfx_started), static_cast<unsigned>(g_music_chunks),
                   static_cast<unsigned>(g_peak), static_cast<unsigned>(st.packets_sent),
                   static_cast<unsigned>(st.dropped_frames));
            g_peak = 0;
        }
    }
    if (s != g_logged_state) {
        if (s == A2dpState::Streaming) {
            g_chime_left = g_rate / 4; // 250 ms
            g_stream_started_flag = true;
        }
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

// ---- mixer API used by src/i_sound_piezo.cpp ---------------------------------
// `data`/`length`: DMX 8-bit unsigned samples at `rate` Hz. Returns false if the sound is
// empty. `handle` is chosen by the caller (shared with the piezo path).
bool presto_pcm_start(int handle, const uint8_t* data, uint32_t length, uint32_t rate, int vol, int pitch)
{
    if (length < 2 || rate == 0)
        return false;
    const float pitch_mult = std::pow(2.0f, static_cast<float>(pitch - 128) / 64.0f);
    const double step = static_cast<double>(rate) * 65536.0 / static_cast<double>(g_rate) * pitch_mult;
    IrqLock lock; // the Bluetooth context mixes these voices
    Voice* slot = nullptr;
    for (Voice& v : g_voices)
        if (!v.active) { slot = &v; break; }
    if (!slot) { // steal the oldest (lowest handle)
        slot = &g_voices[0];
        for (Voice& v : g_voices)
            if (v.handle < slot->handle)
                slot = &v;
    }
    slot->data = data;
    slot->length = length;
    slot->pos = 0;
    slot->step = std::max<uint32_t>(1, static_cast<uint32_t>(step));
    slot->vol = std::clamp(vol, 0, 127);
    slot->handle = handle;
    slot->active = true;
    ++g_sfx_started;
    return true;
}

void presto_pcm_stop(int handle)
{
    IrqLock lock;
    for (Voice& v : g_voices)
        if (v.active && v.handle == handle)
            v.active = false;
}

bool presto_pcm_playing(int handle)
{
    IrqLock lock;
    for (const Voice& v : g_voices)
        if (v.active && v.handle == handle)
            return true;
    return false;
}

void presto_pcm_update(int handle, int vol)
{
    IrqLock lock;
    for (Voice& v : g_voices)
        if (v.active && v.handle == handle)
            v.vol = std::clamp(vol, 0, 127);
}

void presto_pcm_flush()
{
    IrqLock lock;
    for (Voice& v : g_voices)
        v.active = false;
}

// Game mute / music volume, applied by the next fill() (silence and no music rendering while off).
void presto_a2dp_set_enabled(bool on)
{
    g_enabled = on;
}

void presto_a2dp_set_music_volume(int volume)
{
    g_music_volume = volume;
}
