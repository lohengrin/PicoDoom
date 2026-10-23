// Presto sound: one piezo (PWM square wave, Pico-Toolset's pico_toolset_piezo on
// GPIO43) behind doom/i_sound.h. There is exactly one voice and no PCM, so:
//
//  * SFX: each DMX sample is reduced, the first time it is played, to a contour of
//    25 ms windows (zero-crossing pitch + loudness). Playing a sound steps the
//    piezo through that contour on wall-clock time, so shots, doors, pickups and
//    pain/death sounds keep their recognisable pitch movement as chirps. One sound
//    at a time: a new one replaces the playing one when its priority is at least as
//    important (DOOM priorities: lower number = more important).
//  * Music: src/mus_player.cpp's sequencer, stepped without synthesis, lends its
//    loudest/highest melodic voice to the piezo while no SFX is sounding.
//
// While a Bluetooth headset is streaming (src/presto_a2dp.cpp) the piezo stays off and the
// real samples/music go to the headset instead.
// Muted by default; the BOOTSEL button toggles mute (polled once per I_SubmitSound()).
//
// Everything runs from I_SubmitSound(), which the main loop calls once per iteration
// (I_UpdateSound() is compiled out by SNDSERV -- see src/i_sound_dvi.cpp's note).
#include "pico_toolset/piezo.h"
#include "pico_toolset/piezo_configs.h"
#include "pico_toolset/psram.h"
#include "mus_player.hpp"

#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/time.h"

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
void presto_a2dp_pump(bool music);
bool presto_pcm_start(int handle, const uint8_t* data, uint32_t length, uint32_t rate, int vol, int pitch);
void presto_pcm_stop(int handle);
bool presto_pcm_playing(int handle);
void presto_pcm_update(int handle, int vol);
void presto_pcm_flush();
#else
inline bool presto_a2dp_streaming() { return false; }
inline bool presto_a2dp_take_stream_started() { return false; }
inline void presto_a2dp_pump(bool) {}
inline bool presto_pcm_start(int, const uint8_t*, uint32_t, uint32_t, int, int) { return false; }
inline void presto_pcm_stop(int) {}
inline bool presto_pcm_playing(int) { return false; }
inline void presto_pcm_update(int, int) {}
inline void presto_pcm_flush() {}
#endif

namespace {

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

// BOOTSEL shares the flash chip-select line: float it and read it back. Runs from
// RAM with interrupts off, since flash is unreachable while CS is released (core1
// executes RAM only on this build, so nothing else touches flash meanwhile).
bool __no_inline_not_in_flash_func(read_bootsel)() {
    const uint32_t flags = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[1].ctrl, GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i) {
    }
    const bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
    hw_write_masked(&ioqspi_hw->io[1].ctrl, GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

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
    mus_step(static_cast<int>(ticks));

    float hz = 0.0f, level = 0.0f;
    if (g_music_volume > 0 && mus_lead(&hz, &level)) {
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
    g_piezo.init(pico_toolset::configs::piezo::kPimoroniPresto);
    printf("I_InitSound: piezo on GPIO43 (single voice, contour SFX + lead-voice music; Bluetooth headset takes over when streaming), muted -- BOOTSEL toggles\n");
}

void I_UpdateSound(void) {}

void I_SubmitSound(void) {
    const uint64_t now_us = time_us_64();
    if (now_us - g_bootsel_last_us >= 30'000) { // ~30 ms poll doubles as debounce
        g_bootsel_last_us = now_us;
        const bool pressed = read_bootsel();
        if (pressed && !g_prev_bootsel) {
            g_muted = !g_muted;
            printf("PicoDoom: sound %s (BOOTSEL)\n", g_muted ? "muted" : "on");
            if (g_muted) {
                g_sfx.active = false;
                g_music_last_us = 0; // no tick catch-up after unmuting
                presto_pcm_flush();
                set_silent();
            }
        }
        g_prev_bootsel = pressed;
    }
    if (presto_a2dp_take_stream_started() && g_muted) {
        g_muted = false; // connecting a headset is a deliberate "I want sound"; BOOTSEL mutes again
        printf("PicoDoom: sound on (headset streaming; BOOTSEL toggles)\n");
    }
    if (g_muted)
        return;
    if (presto_a2dp_streaming()) { // headset connected: real audio there, piezo off
        g_sfx.active = false;
        g_music_last_us = 0;
        set_silent();
        presto_a2dp_pump(mus_is_playing() && g_music_volume > 0);
        return;
    }
    if (g_sfx.active) {
        if (play_sfx(now_us))
            return;
        g_sfx.active = false;
        set_silent();
    }
    if (mus_is_playing())
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
    if (!presto_a2dp_streaming() && g_sfx.active && priority > g_sfx.priority)
        return -1; // the playing sound matters more
    load_sfx(&S_sfx[id]);
    if (presto_a2dp_streaming()) {
        const auto* hdr = static_cast<const DmxHeader*>(S_sfx[id].data);
        const int handle = g_next_handle++;
        return presto_pcm_start(handle, reinterpret_cast<const uint8_t*>(hdr) + sizeof(DmxHeader),
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
    presto_pcm_stop(handle);
    if (g_sfx.active && g_sfx.handle == handle) {
        g_sfx.active = false;
        set_silent();
    }
}

int I_SoundIsPlaying(int handle) {
    return (g_sfx.active && g_sfx.handle == handle) || presto_pcm_playing(handle);
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch) {
    (void)sep;
    (void)pitch;
    presto_pcm_update(handle, vol);
    if (g_sfx.active && g_sfx.handle == handle)
        g_sfx.gain = std::clamp(static_cast<float>(vol) / 127.0f, 0.0f, 1.0f);
}

void I_InitMusic(void) {}
void I_ShutdownMusic(void) { mus_stop(); }
void I_SetMusicVolume(int volume) { g_music_volume = volume; mus_set_volume(volume); }
void I_PauseSong(int handle) { (void)handle; mus_pause(); }
void I_ResumeSong(int handle) { (void)handle; mus_resume(); }
int I_RegisterSong(void* data) { g_song = data; return 1; }
void I_PlaySong(int handle, int looping) { (void)handle; mus_start(g_song, looping != 0); }
void I_StopSong(int handle) { (void)handle; mus_stop(); }
void I_UnRegisterSong(int handle) { (void)handle; g_song = nullptr; }

} // extern "C"
