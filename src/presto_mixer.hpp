#pragma once

// Mono game-audio mixer shared by the Presto's two outputs: the piezo (PiezoPcm, fed from a
// repeating timer, src/i_sound_piezo.cpp) and the Bluetooth headset (A2DP source callback,
// src/presto_a2dp.cpp). It mixes 8 DMX sample voices (8-bit unsigned, linear interpolation,
// per-voice volume and pitch) and the 10-voice MUS synth (src/mus_player.cpp).
//
// Threading: the game thread calls start/stop/update/flush/playing (short interrupt-off
// sections inside); exactly one interrupt-context caller at a time renders -- the render has a
// try-lock, so if the timer and the Bluetooth callback ever overlap, the loser gets `false` and
// outputs silence for that chunk instead of corrupting the voices.
#include <cstdint>

constexpr int kMixerMaxChunk = 256; // samples per presto_mixer_render() call

void presto_mixer_set_rate(uint32_t hz);   // output rate; flushes the voices (their steps depend on it)
uint32_t presto_mixer_rate();
void presto_mixer_set_enabled(bool on);    // false: silence, and the music synth is not run
void presto_mixer_set_music_volume(int volume);

// `data`/`length`: DMX 8-bit unsigned samples at `rate` Hz. False if the sound is empty.
bool presto_mixer_start(int handle, const uint8_t* data, uint32_t length, uint32_t rate, int vol, int pitch);
void presto_mixer_stop(int handle);
bool presto_mixer_playing(int handle);
void presto_mixer_update(int handle, int vol);
void presto_mixer_flush();

// Any voice playing or music audible (and the mixer enabled): the piezo output idles otherwise.
bool presto_mixer_active();

// Renders `n` (<= kMixerMaxChunk) mono samples. Interrupt-safe; false if another context is
// rendering right now (out is left untouched).
bool presto_mixer_render(int16_t* out, int n);

struct PrestoMixerStats {
    uint32_t sfx_started, music_chunks, peak;
};
PrestoMixerStats presto_mixer_take_stats(); // peak is reset by the call
