// DOOM's music engine: the MUS parser + chip-tune synth now lives in Pico-Toolset
// (pico_toolset::MusPlayer, audio_synth/mus_player.h); this file keeps the C-style function API the
// sound backends call (i_sound_dvi.cpp, presto_mixer.cpp, i_sound_piezo.cpp) over one global player.
// Output rate 44.1 kHz on every backend (the HDMI ring, PiezoPcm and the A2DP link all use it).
#include "mus_player.hpp"

#include "pico_toolset/mus_player.h"

namespace {
pico_toolset::MusPlayer g_player(44100);
}

bool mus_start(const void* data, bool looping) { return g_player.start(data, looping); }
void mus_stop() { g_player.stop(); }
void mus_pause() { g_player.pause(); }
void mus_resume() { g_player.resume(); }
void mus_set_volume(int volume) { g_player.set_volume(volume); }
void mus_render(float* mix_l, float* mix_r, int n) { g_player.render(mix_l, mix_r, n); }
void mus_step(int ticks) { g_player.step(ticks); }
bool mus_lead(float* hz, float* level) { return g_player.lead(hz, level); }
bool mus_is_playing() { return g_player.playing(); }
