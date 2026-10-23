// Presto Bluetooth audio: when a headset/speaker is connected and streaming (A2DP via
// Pico-Toolset's pico_toolset_bluetooth_a2dp), the game's real sound goes there instead
// of the piezo's tone approximation (src/i_sound_piezo.cpp hands over while
// presto_a2dp_streaming() is true).
//
// Data flow -- the game thread produces, the Bluetooth context consumes:
//   I_StartSound -> presto_pcm_start()   8-voice mono mixer (DMX 8-bit samples, linear
//   mus_render() (src/mus_player.cpp)    interpolation) + the 10-voice MUS synth
//        |  presto_a2dp_pump(), once per I_SubmitSound(), tops a 4096-sample mono ring up to
//        v  ~2560 samples (~58 ms at 44.1 kHz): consumption-driven, so no clock drift
//   ring --> DoomSource::fill() (BTstack context, SBC encoder) --> headset
// The ring is single-producer/single-consumer with monotonic indices; an empty ring
// plays silence. Nothing here blocks or prints from the Bluetooth context.
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

constexpr uint32_t kRingSize = 4096; // mono samples, power of two
constexpr uint32_t kRingTarget = 2560;
constexpr int kNumVoices = 8;
constexpr int kChunk = 256;

// 8 KB, in PSRAM (SRAM is what the hi-res tear-free screen copy needs). Core 0 only: written by the
// game thread, read by the Bluetooth context -- both on core 0, so XIP/flash rules are unaffected.
int16_t* g_ring = nullptr;
volatile uint32_t g_head = 0; // producer (game thread)
volatile uint32_t g_tail = 0; // consumer (Bluetooth context)
volatile uint32_t g_rate = 44100;
volatile uint8_t g_channels = 1;
volatile bool g_rate_changed = false;

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
volatile uint32_t g_fill_calls = 0, g_underrun_samples = 0;
uint32_t g_sfx_started = 0, g_music_chunks = 0, g_peak = 0;
uint64_t g_last_game_pump_us = 0, g_last_diag_us = 0;
uint32_t g_chime_left = 0, g_chime_phase = 0;

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
        const uint8_t ch = g_channels;
        uint32_t tail = g_tail;
        const uint32_t head = g_head;
        g_fill_calls = g_fill_calls + 1;
        for (size_t i = 0; i < frames; ++i) {
            int16_t s = 0;
            if (tail != head)
                s = g_ring[tail++ & (kRingSize - 1)];
            else
                g_underrun_samples = g_underrun_samples + 1;
            for (uint8_t c = 0; c < ch; ++c)
                *pcm++ = s;
        }
        __dmb();
        g_tail = tail;
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

void mix_chunk(int n, bool music)
{
    int32_t acc[kChunk] = {};
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
            acc[i] += (s - 128) * v.vol;
            pos += step;
        }
        v.pos = pos;
    }
    if (music) {
        float l[kChunk] = {}, r[kChunk] = {};
        mus_render(l, r, n);
        for (int i = 0; i < n; ++i)
            acc[i] += static_cast<int32_t>(l[i] * 20000.0f);
    }
    if (g_chime_left > 0) { // short "connected" tone, 880 Hz square, fading out
        const uint32_t inc = static_cast<uint32_t>(880.0 * 4294967296.0 / g_rate);
        for (int i = 0; i < n && g_chime_left > 0; ++i, --g_chime_left) {
            g_chime_phase += inc;
            const int32_t amp = static_cast<int32_t>(6000.0f * g_chime_left / (g_rate / 4 + 1));
            acc[i] += (g_chime_phase & 0x80000000u) ? amp : -amp;
        }
    }
    uint32_t head = g_head;
    for (int i = 0; i < n; ++i) {
        const int16_t o = clamp16(acc[i]);
        g_peak = std::max<uint32_t>(g_peak, static_cast<uint32_t>(o < 0 ? -o : o));
        g_ring[head++ & (kRingSize - 1)] = o;
    }
    __dmb();
    g_head = head;
}

} // namespace

static void pump_impl(bool music);

// Called once from presto_input_init() after the HID host is up.
void presto_a2dp_init()
{
    g_ring = static_cast<int16_t*>(pico_toolset::psram_status().test_ok
                                       ? pico_toolset::psram_malloc(kRingSize * sizeof(int16_t))
                                       : std::malloc(kRingSize * sizeof(int16_t)));
    if (!g_ring) {
        printf("PicoDoom: Bluetooth audio: no memory for the PCM ring\n");
        return;
    }
    std::memset(g_ring, 0, kRingSize * sizeof(int16_t));
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
        // Outside the game loop (boot menu, loading) nothing calls the pump: keep the ring fed so
        // the headset hears the connect chime and silence instead of underruns.
        if (now_us - g_last_game_pump_us > 200'000)
            pump_impl(false);
        if (now_us - g_last_diag_us > 5'000'000) {
            g_last_diag_us = now_us;
            const pico_toolset::A2dpStats st = g_a2dp.stats();
            printf("PicoDoom: audio: rate %u ch %u  fills %u underrun %u  sfx %u music-chunks %u peak %u  "
                   "packets %u dropped %u\n",
                   static_cast<unsigned>(g_rate), static_cast<unsigned>(g_channels),
                   static_cast<unsigned>(g_fill_calls), static_cast<unsigned>(g_underrun_samples),
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
    Voice* slot = nullptr;
    for (Voice& v : g_voices)
        if (!v.active) { slot = &v; break; }
    if (!slot) { // steal the oldest (lowest handle)
        slot = &g_voices[0];
        for (Voice& v : g_voices)
            if (v.handle < slot->handle)
                slot = &v;
    }
    const float pitch_mult = std::pow(2.0f, static_cast<float>(pitch - 128) / 64.0f);
    const double step = static_cast<double>(rate) * 65536.0 / static_cast<double>(g_rate) * pitch_mult;
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
    for (Voice& v : g_voices)
        if (v.active && v.handle == handle)
            v.active = false;
}

bool presto_pcm_playing(int handle)
{
    for (const Voice& v : g_voices)
        if (v.active && v.handle == handle)
            return true;
    return false;
}

void presto_pcm_update(int handle, int vol)
{
    for (Voice& v : g_voices)
        if (v.active && v.handle == handle)
            v.vol = std::clamp(vol, 0, 127);
}

void presto_pcm_flush()
{
    for (Voice& v : g_voices)
        v.active = false;
}

// Tops the ring up to kRingTarget samples with the mixed voices (+ music). Game thread.
static void pump_impl(bool music)
{
    if (g_rate_changed) { // negotiated rate changed: sounds in flight were stepped for the old one
        g_rate_changed = false;
        presto_pcm_flush();
    }
    const uint32_t level = g_head - g_tail;
    int need = level < kRingTarget ? static_cast<int>(kRingTarget - level) : 0;
    while (need > 0) {
        const int n = std::min(need, kChunk);
        mix_chunk(n, music);
        if (music)
            ++g_music_chunks;
        need -= n;
    }
}

void presto_a2dp_pump(bool music)
{
    g_last_game_pump_us = time_us_64();
    pump_impl(music);
}
