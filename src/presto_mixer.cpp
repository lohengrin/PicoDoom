#include "presto_mixer.hpp"

#include "pico_toolset/irq_lock.h"
#include "pico_toolset/sample_mixer.h"
#include "mus_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using pico_toolset::IrqLock;

pico_toolset::SampleMixer<8> g_samples; // DMX voices (guarded by IrqLock against the render contexts)

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
    g_samples.set_rate(hz); // stops the voices if it changed (their steps depend on it)
}

uint32_t presto_mixer_rate()
{
    return g_samples.rate();
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
    // DOOM's pitch parameter: 128 = unchanged, 64 steps per octave.
    const float speed = std::pow(2.0f, static_cast<float>(pitch - 128) / 64.0f);
    IrqLock lock;
    const bool ok = g_samples.start(handle, data, length, rate, vol, speed);
    if (ok)
        ++g_sfx_started;
    return ok;
}

void presto_mixer_stop(int handle)
{
    IrqLock lock;
    g_samples.stop(handle);
}

bool presto_mixer_playing(int handle)
{
    IrqLock lock;
    return g_samples.playing(handle);
}

void presto_mixer_update(int handle, int vol)
{
    IrqLock lock;
    g_samples.set_volume(handle, vol);
}

void presto_mixer_flush()
{
    IrqLock lock;
    g_samples.stop_all();
}

bool presto_mixer_active()
{
    if (!g_enabled)
        return false;
    return g_samples.active() || (g_music_volume > 0 && mus_is_playing());
}

bool presto_mixer_render(int16_t* out, int n)
{
    if (n > kMixerMaxChunk)
        n = kMixerMaxChunk;
    if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE))
        return false;

    std::memset(g_acc, 0, sizeof(int32_t) * static_cast<size_t>(n));
    if (g_enabled) {
        g_samples.mix(g_acc, n);
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
