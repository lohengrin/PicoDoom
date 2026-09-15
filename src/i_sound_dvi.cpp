// SFX driver for the HDMI+audio variant (Phase B of docs/HDMI_PLAN.md):
// implements doom/i_sound.h's SFX half (I_InitSound/I_StartSound/
// I_StopSound/I_SoundIsPlaying/I_UpdateSoundParams/I_GetSfxLumpNum/
// I_SetChannels/I_UpdateSound/I_SubmitSound/I_ShutdownSound) against
// Pico-Toolset's pico_toolset_dvi_hdmi HDMI data-island digital audio,
// riding the same cable src/i_video_dvi.cpp's picture does -- no extra
// wiring. Music (I_Init/RegisterSong/PlaySong/etc.) stays stubbed, same as
// doom/i_sound_null.c -- that's Phase C, not this one; doom/s_sound.c's
// S_ChangeMusic() is still compiled out for this build (see its own
// #ifdef PICO, deliberately not widened to !PICODOOM_HDMI yet).
//
// Selected at configure time (PICODOOM_VIDEO_OUTPUT=hdmi, CMakeLists.txt)
// alongside src/i_video_dvi.cpp; doom/s_sound.c's own #ifdef PICO gates
// widen to `!defined(PICODOOM_HDMI)` for this build, so the engine's real
// channel-mixing/distance-attenuation logic runs instead of the LCD
// build's no-op stand-ins -- see that file's own comments.
//
// DMX sample format (doom/i_sound.c, the excluded Linux reference driver,
// is the in-repo template this follows): an 8-byte header (format uint16,
// sample-rate uint16, sample-count uint32) followed by that many unsigned
// 8-bit PCM samples, typically 11025Hz. W_CacheLumpNum() caches the whole
// lump (header included) into PSRAM via the zone system, same as every
// other WAD asset this engine loads -- no separate copy/pad step the
// reference driver's own getsfx() does (that padding is for its
// SAMPLECOUNT-chunked streaming design, which this driver doesn't use).
//
// Mixer: 8 voices (kNumVoices, matching the reference driver's
// NUM_CHANNELS -- doom/s_sound.c's own `numChannels` logical-channel count,
// default 3, is independent and smaller), each an 8-bit-unsigned source
// resampled to 44100Hz via linear interpolation (cheaper nearest-neighbor
// duplication would alias audibly on top of already-lossy 8-bit source
// audio) using a 16.16 fixed-point position/step, matching addsfx()'s own
// idiom. Left/right volume uses the exact x^2-separation formula
// addsfx()/S_StartSoundAtVolume() already compute (`sep`, 1..256) --
// converted to a float gain here instead of the reference driver's 32KB
// `vol_lookup` table (unnecessary once mixing in float, and this target's
// SRAM has no room to spare for it -- see src/i_video_dvi.cpp's own SRAM
// history). Mixed from I_SubmitSound() -- not I_UpdateSound(), despite that
// function's more obviously-matching name: doom/doomdef.h unconditionally
// `#define SNDSERV 1` (true for every build, including the original Linux
// target), which compiles out doom/d_main.c's `#ifndef SNDSERV
// I_UpdateSound(); #endif` call entirely, while its neighboring
// `#ifndef SNDINTR I_SubmitSound(); #endif` always runs (SNDINTR is never
// defined) -- confirmed the hard way on real hardware (see I_SubmitSound()'s
// own doc comment below). No d_main.c change needed either way. Note the
// mix is wall-clock-paced (time_us_64), not 35Hz-tic-paced -- D_DoomLoop
// calls I_SubmitSound() once per render-bound outer iteration (see
// mix_tic()), while the DVI consumer drains at a fixed 44.1kHz.
//
// Pushed into the HDMI audio ring (dvi.h's audio_ring_t, embedded in the
// same dvi_inst src/i_video_dvi.cpp's core1 already drives) non-blocking,
// drop-on-full -- same contract TOM6809's PicoHdmiAudioOutput uses for the
// identical ring. Real stereo (from DMX `sep`), not the mono-duplicated-
// to-both-channels convention that class uses (Thomson hardware has no
// stereo output; DOOM's positional audio does).
#include "dvi.h"
#include "audio_ring.h"
#include "i_video_dvi.hpp"
#include "pico/time.h"
#include "pico_toolset/psram.h"

extern "C" {
#include "doomdef.h"
#include "i_sound.h"
#include "i_system.h"
#include "sounds.h"
#include "w_wad.h"
// z_zone.h's Z_ChangeTag macro (1993 code, unused here -- only PU_STATIC is
// needed) concatenates __FILE__ directly against a string literal, which
// G++ (unlike the DOOMSRC .c files, built with -w -- see CMakeLists.txt)
// flags as -Wliteral-suffix. Not this project's code to restyle.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wliteral-suffix"
#include "z_zone.h"
#pragma GCC diagnostic pop
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kNumVoices = 8;
constexpr int kMixRate = 44100;
// Nominal samples per 35Hz game tic -- 44100/35 (doomdef.h's TICRATE) = 1260
// exactly. Kept as the reference cadence only: actual per-call production is
// wall-clock-paced (see mix_tic()), so this stays meaningful as documentation
// even though no code path forces exactly one tic's worth per call.
constexpr int kSamplesPerTic = kMixRate / TICRATE;
// Largest single I_SubmitSound() production, ~50ms of audio at kMixRate.
// The DVI consumer drains at a hard 44.1kHz from core1's per-scanline IRQ,
// but D_DoomLoop calls I_SubmitSound() once per outer-loop iteration, which
// is render-bound (no I_WaitVBL paces it) and drops well below 35Hz on this
// hardware -- so producing a fixed 1260/call would under-supply the ring and
// leave it near-empty/choppy. mix_tic() instead produces elapsed-wall-ms's
// worth of samples per call; this cap bounds the burst after a stall
// (e.g. a load) so it clips rather than flooding a near-space-free ring.
constexpr int kMaxSamplesPerCall = kMixRate / 20; // 2205, ~50ms

// HDMI audio ring storage -- plain SRAM (.bss), not PSRAM: the consumer
// side (dvi.c's per-scanline DMA IRQ, on core1) reads this via a plain CPU
// load on the same hard-real-time path src/i_video_dvi.cpp's own framebuffer
// history already ruled PSRAM unsafe for (XIP-cache cross-core coherency --
// see that file's header comment). 2048 stereo frames (~46ms at 44100Hz),
// same size TOM6809's PicoHdmiAudioOutput uses.
constexpr uint32_t kRingFrames = 2048;
alignas(4) audio_sample_t g_ring_storage[kRingFrames];

// Mix accumulators (sized for the largest possible single call,
// kMaxSamplesPerCall) -- core0-only (this file's own I_SubmitSound()/
// mix_tic() produce them, then copy into the SRAM ring above), so PSRAM is
// fine here: no cross-core CPU-load concern, just saving ~10KB of this
// board's scarce SRAM (see src/i_video_dvi.cpp's own SRAM-budget history
// for why that matters).
float* g_mix_l = nullptr;
float* g_mix_r = nullptr;

dvi_inst* g_dvi = nullptr;

// 8-byte DMX lump header (doom/i_sound.c's getsfx() is the reference for
// this layout) -- read directly from the cached lump, no copy.
struct DmxHeader {
    uint16_t format;
    uint16_t sample_rate;
    uint32_t sample_count;
};

struct Voice {
    bool active = false;
    int handle = 0;
    const uint8_t* data = nullptr; // first sample, i.e. 8 bytes past DmxHeader
    uint32_t length = 0;           // sample_count
    uint32_t pos_frac = 0;         // 16.16 fixed-point position in source samples
    uint32_t step = 0;             // 16.16 fixed-point source-samples-per-output-sample
    float left_vol = 0.0f;
    float right_vol = 0.0f;
};
Voice g_voices[kNumVoices];
int g_next_handle = 1;

// Ensures sfx->data/lumpnum are resolved and the lump is cached (once,
// permanently -- PU_STATIC, same zone tag the reference driver's getsfx()
// uses). No-op if already cached (sfx->data non-null).
void load_sfx(sfxinfo_t* sfx) {
    if (sfx->data)
        return;
    if (sfx->lumpnum < 0)
        sfx->lumpnum = I_GetSfxLumpNum(sfx);
    sfx->data = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
}

// Configures the HDMI audio ring on first use, not from I_InitSound() --
// I_InitSound() runs from doom/i_system.c's I_Init(), which D_DoomMain()
// calls well before D_DoomLoop() ever calls I_InitGraphics()
// (src/i_video_dvi.cpp) -- confirmed by grepping doom/d_main.c: I_Init() is
// line ~1086, D_DoomLoop() (and the I_InitGraphics() inside it) isn't
// reached until ~1125+. dvi_audio_sample_buffer_set()/dvi_set_audio_freq()
// need dvi_init() to have already run (see dvi.h's own doc comment on
// those functions) -- calling them from I_InitSound() configured a
// dvi_inst that hadn't been dvi_init()'d yet, a real-hardware-confirmed
// silent-no-audio bug (video worked, SFX never played). I_SubmitSound() --
// see its own doc comment for why that's the function that actually runs
// per tic here, not I_UpdateSound() -- is only ever called from
// D_DoomLoop()'s per-tic loop, strictly after I_InitGraphics() by then, so
// deferring to here is correct without needing to reorder any shared
// engine file.
bool g_ring_configured = false;
void ensure_ring_configured() {
    if (g_ring_configured || !g_dvi)
        return;
    // Half-pre-fill the ring so the rate-matched producer/consumer pair
    // starts mid-way between underrun and overflow instead of right at
    // empty (same reasoning TOM6809's PicoHdmiAudioOutput docs).
    dvi_audio_sample_buffer_set(g_dvi, g_ring_storage, static_cast<int>(kRingFrames));
    audio_ring_set_write_offset(&g_dvi->audio_ring, kRingFrames / 2);
    // CEA-861 ACR values for 44100Hz at dvi_timing_640x480p_60hz's 25.2MHz
    // pixel clock -- same constants TOM6809's PicoHdmiAudioOutput uses,
    // independently verified there against the HDMI spec tables.
    dvi_set_audio_freq(g_dvi, kMixRate, /*cts=*/28000, /*n=*/6272);
    g_ring_configured = true;
    printf("PicoDoom: HDMI audio ring configured (%d Hz, %u-frame ring, %d voices)\n",
           kMixRate, static_cast<unsigned>(kRingFrames), kNumVoices);
}

void mix_tic() {
    ensure_ring_configured();

    // Wall-clock paced production, not 35Hz-tic paced: D_DoomLoop calls
    // I_SubmitSound() once per outer-loop iteration, which with no I_WaitVBL
    // is render-bound and runs slower than TICRATE on this hardware, while
    // the DVI consumer drains at a fixed 44.1kHz from core1's per-scanline
    // IRQ. Producing the elapsed-wall time's worth of samples per call keeps
    // the producer matched to that real-time rate at any loop speed -- a
    // 25Hz loop (40ms/iteration) supplies ~1764 frames/call instead of a
    // starved 1260. Ceiling rounding (not round-to-nearest) so the producer
    // can never systematically fall behind the consumer; the excess
    // sub-sample is bounded by the ring's ~46ms of buffering. Cap the burst
    // after a stall so a long hitch clips instead of flooding a near-full
    // ring (kMaxSamplesPerCall, ~50ms < ring capacity).
    static uint64_t s_last_mix_us = 0;
    uint64_t now_us = time_us_64();
    const uint64_t elapsed_us = s_last_mix_us ? now_us - s_last_mix_us : 0;
    s_last_mix_us = now_us;
    int n = static_cast<int>((elapsed_us * static_cast<uint64_t>(kMixRate) + 999'999u) / 1'000'000u);
    if (n > kMaxSamplesPerCall)
        n = kMaxSamplesPerCall;

    std::fill(g_mix_l, g_mix_l + n, 0.0f);
    std::fill(g_mix_r, g_mix_r + n, 0.0f);

    for (Voice& v : g_voices) {
        if (!v.active)
            continue;
        for (int i = 0; i < n; ++i) {
            uint32_t idx = v.pos_frac >> 16;
            if (idx + 1 >= v.length) {
                v.active = false;
                break;
            }
            uint32_t frac = v.pos_frac & 0xFFFFu;
            float s0 = (static_cast<int>(v.data[idx]) - 128) / 128.0f;
            float s1 = (static_cast<int>(v.data[idx + 1]) - 128) / 128.0f;
            float s = s0 + (s1 - s0) * (static_cast<float>(frac) / 65536.0f);
            g_mix_l[i] += s * v.left_vol;
            g_mix_r[i] += s * v.right_vol;
            v.pos_frac += v.step;
        }
    }

    if (!g_dvi)
        return;
    audio_ring_t& ring = g_dvi->audio_ring;
    uint32_t free_frames = audio_ring_get_write_size(&ring);
    uint32_t to_write = std::min<int>(n, static_cast<int>(free_frames));
    if (to_write == 0)
        return; // ring full -- drop this batch, never block the game loop

    audio_sample_t* buf = audio_ring_get_buffer(&ring);
    uint32_t wpos = audio_ring_get_write_offset(&ring);
    uint32_t mask = ring.size - 1;
    for (uint32_t i = 0; i < to_write; ++i) {
        float l = std::clamp(g_mix_l[i], -1.0f, 1.0f);
        float r = std::clamp(g_mix_r[i], -1.0f, 1.0f);
        buf[wpos].channels[0] = static_cast<int16_t>(l * 32767.0f);
        buf[wpos].channels[1] = static_cast<int16_t>(r * 32767.0f);
        wpos = (wpos + 1) & mask;
    }
    audio_ring_advance_write(&ring, to_write);
}

// Left/right gain from DOOM's volume/separation pair -- exact formula
// addsfx() (doom/i_sound.c) uses, just producing a float gain instead of
// an index into a precomputed table. `sep` is 0..255 on entry (NORM_SEP
// 128 == centered); `vol` is 0..127 (S_MAX_VOLUME).
void compute_pan(int vol, int sep, float& left_vol, float& right_vol) {
    int separation = sep + 1;
    int leftvol = vol - ((vol * separation * separation) >> 16);
    separation = separation - 257;
    int rightvol = vol - ((vol * separation * separation) >> 16);
    leftvol = std::clamp(leftvol, 0, 127);
    rightvol = std::clamp(rightvol, 0, 127);
    left_vol = leftvol / 127.0f;
    right_vol = rightvol / 127.0f;
}

} // namespace

extern "C" {

void I_InitSound(void) {
    // Just grabs the (not-yet-dvi_init()'d) dvi_inst pointer and allocates
    // the mix buffers -- neither needs dvi_init() to have run. The ring
    // itself is configured lazily, on first use -- see
    // ensure_ring_configured()'s doc comment for why.
    g_dvi = i_video_dvi_instance();

    g_mix_l = static_cast<float*>(pico_toolset::psram_malloc(kMaxSamplesPerCall * sizeof(float)));
    g_mix_r = static_cast<float*>(pico_toolset::psram_malloc(kMaxSamplesPerCall * sizeof(float)));
    if (!g_mix_l || !g_mix_r)
        I_Error(const_cast<char*>("I_InitSound: failed to allocate mix buffers"));
}

// doom/doomdef.h unconditionally `#define SNDSERV 1` (not PICO-specific --
// true for every build, including the original Linux target) and leaves
// SNDINTR undefined, so doom/d_main.c's `#ifndef SNDSERV I_UpdateSound();
// #endif` around the per-tic mix call is always compiled OUT, while its
// `#ifndef SNDINTR I_SubmitSound(); #endif` right after it always runs --
// confirmed on real hardware (sound fired -- S_StartSoundAtVolume's own
// debug print proved it -- but nothing came out, because the mixing was
// sitting in the function that never gets called). So the real per-tic mix
// lives in I_SubmitSound() here, not I_UpdateSound() -- the reverse of what
// SNDSERV's own naming would suggest, but matching what d_main.c actually
// calls.
void I_UpdateSound(void) {}
void I_SubmitSound(void) { mix_tic(); }
void I_ShutdownSound(void) {}

void I_SetChannels(void) {}

int I_GetSfxLumpNum(sfxinfo_t* sfx) {
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority) {
    (void)priority;
    if (id < 1 || id >= NUMSFX)
        return -1;

    sfxinfo_t* sfx = &S_sfx[id];
    load_sfx(sfx);
    auto* hdr = reinterpret_cast<const DmxHeader*>(sfx->data);
    if (!hdr || hdr->sample_count == 0)
        return -1;

    // Free voice if one exists, else steal the oldest (lowest handle --
    // handles increase monotonically with time, same idea as addsfx()'s
    // channelstart[]-based oldest search).
    int slot = -1;
    for (int i = 0; i < kNumVoices; ++i) {
        if (!g_voices[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < kNumVoices; ++i)
            if (g_voices[i].handle < g_voices[slot].handle)
                slot = i;
    }

    Voice& v = g_voices[slot];
    v.data = reinterpret_cast<const uint8_t*>(hdr) + sizeof(DmxHeader);
    v.length = hdr->sample_count;
    v.pos_frac = 0;

    float pitch_mult = std::pow(2.0f, static_cast<float>(pitch - 128) / 64.0f);
    double base_step = static_cast<double>(hdr->sample_rate) * 65536.0 / kMixRate;
    v.step = static_cast<uint32_t>(base_step * pitch_mult);
    if (v.step == 0)
        v.step = 1;

    compute_pan(vol, sep, v.left_vol, v.right_vol);

    v.handle = g_next_handle++;
    v.active = true;
    return v.handle;
}

void I_StopSound(int handle) {
    for (Voice& v : g_voices)
        if (v.active && v.handle == handle)
            v.active = false;
}

int I_SoundIsPlaying(int handle) {
    for (const Voice& v : g_voices)
        if (v.active && v.handle == handle)
            return 1;
    return 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch) {
    (void)pitch; // pitch isn't re-applied mid-play, same as the reference driver's own (unused) implementation
    for (Voice& v : g_voices) {
        if (v.active && v.handle == handle) {
            compute_pan(vol, sep, v.left_vol, v.right_vol);
            return;
        }
    }
}

// Music -- deferred to Phase C (docs/HDMI_PLAN.md), same no-ops as
// doom/i_sound_null.c. doom/s_sound.c's S_ChangeMusic() never calls any of
// these on this build (its own #ifdef PICO still returns early).
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int volume) { (void)volume; }
void I_PauseSong(int handle) { (void)handle; }
void I_ResumeSong(int handle) { (void)handle; }
int I_RegisterSong(void* data) { (void)data; return 0; }
void I_PlaySong(int handle, int looping) { (void)handle; (void)looping; }
void I_StopSong(int handle) { (void)handle; }
void I_UnRegisterSong(int handle) { (void)handle; }

} // extern "C"
