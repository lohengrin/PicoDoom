#include "presto_mixer.hpp"

#include "pico_toolset/irq_lock.h"
#include "mus_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using pico_toolset::IrqLock;

constexpr int kNumVoices = 8;

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

volatile uint32_t g_rate = 44100;
volatile bool g_enabled = false;
volatile int g_music_volume = 127;
volatile int g_busy = 0; // render try-lock

// Static, not stack: renders run in interrupt context on core 0's small stack.
int32_t g_acc[kMixerMaxChunk];
float g_mus_l[kMixerMaxChunk], g_mus_r[kMixerMaxChunk];

uint32_t g_sfx_started = 0, g_music_chunks = 0, g_peak = 0;

} // namespace

void presto_mixer_set_rate(uint32_t hz)
{
    IrqLock lock;
    if (hz == g_rate)
        return;
    g_rate = hz;
    for (Voice& v : g_voices)
        v.active = false;
}

uint32_t presto_mixer_rate()
{
    return g_rate;
}

void presto_mixer_set_enabled(bool on)
{
    g_enabled = on;
}

void presto_mixer_set_music_volume(int volume)
{
    g_music_volume = volume;
}

bool presto_mixer_start(int handle, const uint8_t* data, uint32_t length, uint32_t rate, int vol, int pitch)
{
    if (length < 2 || rate == 0)
        return false;
    const float pitch_mult = std::pow(2.0f, static_cast<float>(pitch - 128) / 64.0f);
    IrqLock lock;
    const double step = static_cast<double>(rate) * 65536.0 / static_cast<double>(g_rate) * pitch_mult;
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

void presto_mixer_stop(int handle)
{
    IrqLock lock;
    for (Voice& v : g_voices)
        if (v.active && v.handle == handle)
            v.active = false;
}

bool presto_mixer_playing(int handle)
{
    IrqLock lock;
    for (const Voice& v : g_voices)
        if (v.active && v.handle == handle)
            return true;
    return false;
}

void presto_mixer_update(int handle, int vol)
{
    IrqLock lock;
    for (Voice& v : g_voices)
        if (v.active && v.handle == handle)
            v.vol = std::clamp(vol, 0, 127);
}

void presto_mixer_flush()
{
    IrqLock lock;
    for (Voice& v : g_voices)
        v.active = false;
}

bool presto_mixer_active()
{
    if (!g_enabled)
        return false;
    for (const Voice& v : g_voices)
        if (v.active)
            return true;
    return g_music_volume > 0 && mus_is_playing();
}

bool presto_mixer_render(int16_t* out, int n)
{
    if (n > kMixerMaxChunk)
        n = kMixerMaxChunk;
    if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE))
        return false;

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
    for (int i = 0; i < n; ++i) {
        const int32_t v = std::clamp<int32_t>(g_acc[i], -32768, 32767);
        out[i] = static_cast<int16_t>(v);
        g_peak = std::max<uint32_t>(g_peak, static_cast<uint32_t>(v < 0 ? -v : v));
    }
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
    return true;
}

PrestoMixerStats presto_mixer_take_stats()
{
    IrqLock lock;
    const PrestoMixerStats s{g_sfx_started, g_music_chunks, g_peak};
    g_peak = 0;
    return s;
}
