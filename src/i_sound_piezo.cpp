// Presto sound behind doom/i_sound.h. All the real audio comes from one shared mixer
// (src/presto_mixer.cpp: 8 DMX voices + the MUS synth, mono) rendered on demand by whichever
// output is active:
//
//  * Bluetooth headset streaming (src/presto_a2dp.cpp): the SBC encoder callback renders it.
//  * Otherwise the piezo as a PCM output (Pico-Toolset's PiezoPcm on GPIO43: DMA-paced PWM duty
//    cycle): a 5 ms repeating timer keeps ~18 ms of audio queued, in interrupt context, so long
//    game frames cannot starve it. The output pauses (pin low) when nothing is playing.
//  * Fallback if PiezoPcm cannot start (no free DMA channel/timer): the original single-voice
//    tone approximation -- each DMX sample reduced to a contour of 25 ms windows (zero-crossing
//    pitch + loudness) stepped on wall-clock time, music as the MUS sequencer's lead voice.
//
// Muted by default; the BOOTSEL button toggles mute (polled once per I_SubmitSound()), and a
// headset starting to stream unmutes. I_SubmitSound() is the main loop's per-iteration hook
// (I_UpdateSound() is compiled out by SNDSERV -- see src/i_sound_dvi.cpp's note).
#include "pico_toolset/bootsel.h"
#include "pico_toolset/irq_lock.h"
#include "pico_toolset/piezo.h"
#include "pico_toolset/piezo_configs.h"
#include "pico_toolset/piezo_pcm.h"
#include "pico_toolset/psram.h"
#include "mus_player.hpp"
#include "presto_mixer.hpp"

#include "pico/time.h"
#include "pico/types.h"

extern "C" {
#include "doomdef.h"
#include "i_sound.h"
#include "i_system.h"
#include "sounds.h"
#include "w_wad.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wliteral-suffix"
#include "z_zone.h"
#pragma GCC diagnostic pop
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Bluetooth audio (src/presto_a2dp.cpp): while a headset is streaming it takes over from the piezo.
#ifdef PICODOOM_A2DP
bool presto_a2dp_streaming();
bool presto_a2dp_take_stream_started(); // true once per transition to Streaming
#else
inline bool presto_a2dp_streaming() { return false; }
inline bool presto_a2dp_take_stream_started() { return false; }
#endif

namespace {

// The MUS sequencer is shared with the Bluetooth context (presto_a2dp.cpp renders it from the
// A2DP source callback while streaming), so every mus_* call from the game thread is atomic.
using pico_toolset::IrqLock;

constexpr int kWindowMs = 25;
constexpr int kMaxWindows = 80;      // 2 s of contour per sound
constexpr float kMinHz = 150.0f;     // below this a piezo is hardly audible
constexpr float kMaxHz = 3600.0f;
constexpr int kMusicTickHz = 140;

struct DmxHeader {
    uint16_t format;
    uint16_t sample_rate;
    uint32_t sample_count;
} __attribute__((packed));

struct Contour {
    uint16_t windows;
    uint16_t* freq;  // Hz
    uint8_t* level;  // 0..255
};

pico_toolset::Piezo g_piezo;
Contour g_contours[NUMSFX];
bool g_contour_built[NUMSFX];

struct SfxVoice {
    bool active = false;
    int handle = 0;
    int id = 0;
    int priority = 0;
    float gain = 0.0f; // 0..1 from the engine's volume
    uint64_t start_us = 0;
} g_sfx;

int g_next_handle = 1;

// PCM piezo output (see the header comment).
pico_toolset::PiezoPcm g_pcm;
bool g_pcm_ok = false;
repeating_timer_t g_pcm_timer;
constexpr size_t kPcmTargetQueued = 800; // ~18 ms at 44.1 kHz

// Whether SFX/music go through the shared mixer (headset or PCM piezo) rather than the tone fallback.
bool use_mixer() { return g_pcm_ok || presto_a2dp_streaming(); }

// 5 ms timer, interrupt context: top the PCM ring up from the mixer. Stays out of the way while
// a headset is streaming (the Bluetooth callback owns the mixer then) and idles the output.
bool pcm_timer_cb(repeating_timer_t*) {
    if (presto_a2dp_streaming()) {
        if (!g_pcm.paused())
            g_pcm.pause();
        return true;
    }
    const size_t queued = g_pcm.queued();
    if (!presto_mixer_active()) {
        if (queued == 0 && !g_pcm.paused())
            g_pcm.pause(); // pin low: no carrier hiss while silent
        return true;
    }
    static int16_t chunk[kMixerMaxChunk];
    size_t need = queued < kPcmTargetQueued ? kPcmTargetQueued - queued : 0;
    while (need > 0) {
        const int n = static_cast<int>(std::min<size_t>(need, kMixerMaxChunk));
        if (!presto_mixer_render(chunk, n))
            break;
        g_pcm.write(chunk, static_cast<size_t>(n));
        need -= static_cast<size_t>(n);
    }
    return true;
}

// Last state written to the piezo, to avoid redundant (glitchy) reprogramming.
bool g_tone_on = false;
uint32_t g_tone_hz = 0;
float g_tone_duty = 0.0f;

// Music
const void* g_song = nullptr;
uint64_t g_music_last_us = 0;
uint32_t g_music_tick_rem = 0; // microseconds not yet turned into a tick
int g_music_volume = 127;

bool g_muted = true;
bool g_prev_bootsel = false;
uint64_t g_bootsel_last_us = 0;

void set_silent() {
    if (g_tone_on) {
        g_piezo.stop();
        g_tone_on = false;
    }
}

void set_tone(float hz, float duty) {
    const uint32_t f = static_cast<uint32_t>(hz);
    if (g_tone_on && f == g_tone_hz && std::fabs(duty - g_tone_duty) < 0.01f)
        return;
    if (!g_piezo.set_tone(f, duty)) {
        set_silent();
        return;
    }
    g_tone_on = true;
    g_tone_hz = f;
    g_tone_duty = duty;
}

// Estimates pitch and loudness per window from the 8-bit unsigned PCM.
void build_contour(int id, const uint8_t* pcm, uint32_t count, uint32_t rate) {
    g_contour_built[id] = true;
    if (rate == 0 || count == 0)
        return;
    const uint32_t per_window = std::max<uint32_t>(1, rate * kWindowMs / 1000);
    uint32_t windows = (count + per_window - 1) / per_window;
    if (windows > kMaxWindows)
        windows = kMaxWindows;
    auto* freq = static_cast<uint16_t*>(pico_toolset::psram_malloc(windows * sizeof(uint16_t)));
    auto* level = static_cast<uint8_t*>(pico_toolset::psram_malloc(windows));
    if (!freq || !level)
        return;

    for (uint32_t w = 0; w < windows; ++w) {
        const uint32_t a = w * per_window;
        const uint32_t b = std::min(count, a + per_window);
        uint32_t crossings = 0;
        uint32_t absdev = 0;
        bool above = pcm[a] >= 128;
        for (uint32_t i = a; i < b; ++i) {
            const int dev = static_cast<int>(pcm[i]) - 128;
            absdev += static_cast<uint32_t>(dev < 0 ? -dev : dev);
            // Hysteresis band keeps hiss around the centre from counting.
            if (above && dev < -6) { above = false; ++crossings; }
            else if (!above && dev > 6) { above = true; ++crossings; }
        }
        const uint32_t n = b - a;
        const float seconds = static_cast<float>(n) / static_cast<float>(rate);
        float hz = seconds > 0.0f ? static_cast<float>(crossings) * 0.5f / seconds : 0.0f;
        hz = std::clamp(hz, kMinHz, kMaxHz);
        const float mad = n ? static_cast<float>(absdev) / static_cast<float>(n) : 0.0f;
        freq[w] = static_cast<uint16_t>(hz);
        level[w] = static_cast<uint8_t>(std::min(255.0f, mad * (255.0f / 40.0f)));
    }
    g_contours[id].windows = static_cast<uint16_t>(windows);
    g_contours[id].freq = freq;
    g_contours[id].level = level;
}

void load_sfx(sfxinfo_t* sfx) {
    if (!sfx->data) {
        if (sfx->lumpnum < 0)
            sfx->lumpnum = I_GetSfxLumpNum(sfx);
        sfx->data = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
    }
}

// Piezo path only: the pitch/loudness contour is built on first use.
void ensure_contour(sfxinfo_t* sfx, int id) {
    if (!g_contour_built[id]) {
        const auto* hdr = static_cast<const DmxHeader*>(sfx->data);
        // DMX: 8-byte header, then (count - 32) PCM bytes padded by 16 each end.
        uint32_t count = hdr->sample_count;
        if (count > 32)
            count -= 32;
        build_contour(id, reinterpret_cast<const uint8_t*>(hdr) + sizeof(DmxHeader) + 16, count, hdr->sample_rate);
    }
}

// Writes the tone for the playing sound; returns false once it has finished.
bool play_sfx(uint64_t now_us) {
    const Contour& c = g_contours[g_sfx.id];
    const uint32_t idx = static_cast<uint32_t>((now_us - g_sfx.start_us) / (kWindowMs * 1000u));
    if (c.windows == 0 || idx >= c.windows)
        return false;
    const float level = static_cast<float>(c.level[idx]) / 255.0f;
    if (level < 0.04f) {
        set_silent();
    } else {
        // Square-wave loudness peaks at 50% duty, so scale that down.
        set_tone(static_cast<float>(c.freq[idx]), std::max(0.03f, 0.5f * g_sfx.gain * level));
    }
    return true;
}

void play_music(uint64_t now_us) {
    if (g_music_last_us == 0)
        g_music_last_us = now_us;
    uint64_t elapsed = now_us - g_music_last_us + g_music_tick_rem;
    g_music_last_us = now_us;
    uint32_t ticks = static_cast<uint32_t>(elapsed * kMusicTickHz / 1'000'000);
    g_music_tick_rem = static_cast<uint32_t>(elapsed - static_cast<uint64_t>(ticks) * 1'000'000 / kMusicTickHz);
    ticks = std::min<uint32_t>(ticks, 30);
    float hz = 0.0f, level = 0.0f;
    bool lead;
    {
        IrqLock lock;
        mus_step(static_cast<int>(ticks));
        lead = g_music_volume > 0 && mus_lead(&hz, &level);
    }
    if (lead) {
        while (hz < 200.0f)
            hz *= 2.0f; // fold bass into the piezo's audible range
        while (hz > kMaxHz)
            hz *= 0.5f;
        // Quieter than SFX so effects cut through.
        set_tone(hz, std::max(0.02f, 0.18f * level * static_cast<float>(g_music_volume) / 127.0f));
    } else {
        set_silent();
    }
}

} // namespace

extern "C" {

void I_InitSound(void) {
    pico_toolset::PiezoPcmConfig pcm_cfg;
    pcm_cfg.pin = pico_toolset::configs::piezo::kPimoroniPresto.pin;
    g_pcm_ok = g_pcm.init(pcm_cfg);
    if (g_pcm_ok) {
        add_repeating_timer_us(-5000, pcm_timer_cb, nullptr, &g_pcm_timer);
    } else {
        g_piezo.init(pico_toolset::configs::piezo::kPimoroniPresto);
    }
    printf("I_InitSound: piezo on GPIO43 (%s; a Bluetooth headset takes over when streaming), muted -- BOOTSEL toggles\n",
           g_pcm_ok ? "PCM: real SFX and music" : "PCM unavailable: single-voice tone fallback");
}

void I_UpdateSound(void) {}

void I_SubmitSound(void) {
    const uint64_t now_us = time_us_64();
    if (now_us - g_bootsel_last_us >= 30'000) { // ~30 ms poll doubles as debounce
        g_bootsel_last_us = now_us;
        const bool pressed = pico_toolset::bootsel_pressed();
        if (pressed && !g_prev_bootsel) {
            g_muted = !g_muted;
            presto_mixer_set_enabled(!g_muted);
            printf("PicoDoom: sound %s (BOOTSEL)\n", g_muted ? "muted" : "on");
            if (g_muted) {
                g_sfx.active = false;
                g_music_last_us = 0; // no tick catch-up after unmuting
                presto_mixer_flush();
                set_silent();
            }
        }
        g_prev_bootsel = pressed;
    }
    if (presto_a2dp_take_stream_started() && g_muted) {
        g_muted = false; // connecting a headset is a deliberate "I want sound"; BOOTSEL mutes again
        presto_mixer_set_enabled(true);
        printf("PicoDoom: sound on (headset streaming; BOOTSEL toggles)\n");
    }
    if (g_muted)
        return;
    if (use_mixer()) { // PCM piezo (timer) or headset (SBC callback) renders on demand
        g_sfx.active = false;
        g_music_last_us = 0;
        set_silent();
        return;
    }
    if (g_sfx.active) {
        if (play_sfx(now_us))
            return;
        g_sfx.active = false;
        set_silent();
    }
    bool playing;
    {
        IrqLock lock;
        playing = mus_is_playing();
    }
    if (playing)
        play_music(now_us);
    else
        set_silent();
}

void I_ShutdownSound(void) {
    set_silent();
}

void I_SetChannels(void) {}

int I_GetSfxLumpNum(sfxinfo_t* sfx) {
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority) {
    (void)sep;
    if (id < 1 || id >= NUMSFX)
        return -1;
    if (g_muted)
        return -1;
    if (!use_mixer() && g_sfx.active && priority > g_sfx.priority)
        return -1; // the playing sound matters more
    load_sfx(&S_sfx[id]);
    if (use_mixer()) {
        const auto* hdr = static_cast<const DmxHeader*>(S_sfx[id].data);
        const int handle = g_next_handle++;
        return presto_mixer_start(handle, reinterpret_cast<const uint8_t*>(hdr) + sizeof(DmxHeader),
                                hdr->sample_count, hdr->sample_rate, vol, pitch)
                   ? handle : -1;
    }
    ensure_contour(&S_sfx[id], id);
    if (g_contours[id].windows == 0)
        return -1;

    g_sfx.active = true;
    g_sfx.handle = g_next_handle++;
    g_sfx.id = id;
    g_sfx.priority = priority;
    g_sfx.gain = std::clamp(static_cast<float>(vol) / 127.0f, 0.0f, 1.0f);
    g_sfx.start_us = time_us_64();
    play_sfx(g_sfx.start_us);
    return g_sfx.handle;
}

void I_StopSound(int handle) {
    presto_mixer_stop(handle);
    if (g_sfx.active && g_sfx.handle == handle) {
        g_sfx.active = false;
        set_silent();
    }
}

int I_SoundIsPlaying(int handle) {
    return (g_sfx.active && g_sfx.handle == handle) || presto_mixer_playing(handle);
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch) {
    (void)sep;
    (void)pitch;
    presto_mixer_update(handle, vol);
    if (g_sfx.active && g_sfx.handle == handle)
        g_sfx.gain = std::clamp(static_cast<float>(vol) / 127.0f, 0.0f, 1.0f);
}

void I_InitMusic(void) {}
void I_ShutdownMusic(void) { IrqLock l; mus_stop(); }
void I_SetMusicVolume(int volume) { g_music_volume = volume; presto_mixer_set_music_volume(volume); IrqLock l; mus_set_volume(volume); }
void I_PauseSong(int handle) { (void)handle; IrqLock l; mus_pause(); }
void I_ResumeSong(int handle) { (void)handle; IrqLock l; mus_resume(); }
int I_RegisterSong(void* data) { g_song = data; return 1; }
void I_PlaySong(int handle, int looping) { (void)handle; IrqLock l; mus_start(g_song, looping != 0); }
void I_StopSong(int handle) { (void)handle; IrqLock l; mus_stop(); }
void I_UnRegisterSong(int handle) { (void)handle; g_song = nullptr; }

} // extern "C"
