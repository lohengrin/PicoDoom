#pragma once

// DOOM MUS-format music player + small software synth (HDMI+audio build,
// Phase C of docs/HDMI_PLAN.md). Core0-only: every entry point, including
// mus_render(), is called from the game loop's thread (I_*Song from game
// logic, mus_render from I_SubmitSound()'s mix_tic()), so there is no locking.
//
// Not OPL2/General-MIDI emulation: each MUS note becomes a square/pulse/
// triangle/saw voice (picked from the channel's GM program number), with a
// decay envelope for plucked/struck instruments and a short noise/triangle
// burst for percussion. Recognizable chip-tune rendering, not the original
// sound.

#include <cstdint>

// Validates and starts `data` (a cached D_* lump). Returns false (and plays
// nothing) if it isn't a MUS lump. `data` must stay valid until mus_stop().
bool mus_start(const void* data, bool looping);
void mus_stop();
void mus_pause();
void mus_resume();
// DOOM music volume, 0..127 (snd_MusicVolume).
void mus_set_volume(int volume);
// Adds `n` samples of 44100Hz mono music, centered, into both buffers.
// Cheap no-op when nothing is playing or fading out.
void mus_render(float* mix_l, float* mix_r, int n);
