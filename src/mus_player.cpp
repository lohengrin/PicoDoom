#include "mus_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int kMixRate = 44100;
constexpr int kTickHz = 140; // MUS tick rate
constexpr int kSamplesPerTick = kMixRate / kTickHz; // 315, exact
constexpr int kVoices = 10;
constexpr int kChannels = 16;
constexpr int kPercussionChannel = 15;

// Per-voice peak before the master scale below; 10 voices at full velocity
// would sum to 1.8, so master is kept well under 1 -- typical scores keep a
// handful of voices sounding at once.
constexpr float kVoiceGain = 0.18f;
constexpr float kMasterScale = 0.5f;
constexpr float kReleaseDecay = 0.99887f; // ~20ms time constant at 44.1kHz
constexpr float kSilence = 0.002f;

enum Shape : uint8_t { Square, Pulse, Tri, Saw, Noise };

struct Channel {
    uint8_t program = 0;
    uint8_t volume = 100;
    uint8_t expression = 127;
    uint8_t last_vel = 100;
    uint8_t bend = 128; // 0..255, 128 centered
};

struct Voice {
    bool active = false;
    bool released = false;
    uint8_t chan = 0;
    uint8_t note = 0;
    uint8_t vel = 0;
    Shape shape = Square;
    uint32_t phase = 0;
    uint32_t inc = 0;
    uint32_t rng = 1;
    float env = 0.0f;
    float decay = 1.0f;
    uint32_t serial = 0;
};

Channel g_chan[kChannels];
Voice g_voice[kVoices];
uint32_t g_serial = 0;

const uint8_t* g_score = nullptr;
const uint8_t* g_pos = nullptr;
const uint8_t* g_end = nullptr;
bool g_playing = false;
bool g_paused = false;
bool g_looping = false;
int g_delay_ticks = 0;
int g_samples_to_tick = kSamplesPerTick;
float g_master = 0.0f;

float note_freq(int note, int bend) {
    const float semis = static_cast<float>(note - 69) + static_cast<float>(bend - 128) * (2.0f / 128.0f);
    return 440.0f * std::exp2(semis * (1.0f / 12.0f));
}

uint32_t freq_to_inc(float hz) {
    return static_cast<uint32_t>(hz * (4294967296.0f / static_cast<float>(kMixRate)));
}

// Shape/decay per GM program group (melodic channels).
void program_voice(int program, Shape& shape, float& decay) {
    decay = 1.0f; // sustained
    if (program < 8) { shape = Pulse; decay = 0.99985f; }          // pianos
    else if (program < 16) { shape = Tri; decay = 0.9997f; }       // chromatic percussion
    else if (program < 24) { shape = Square; }                     // organs
    else if (program < 32) { shape = Saw; decay = 0.9999f; }       // guitars
    else if (program < 40) { shape = Tri; }                        // basses
    else if (program < 48) { shape = Tri; }                        // strings
    else if (program < 56) { shape = Saw; }                        // ensemble/voices
    else if (program < 72) { shape = Square; }                     // brass/reeds
    else if (program < 80) { shape = Tri; }                        // pipes
    else if (program < 88) { shape = Square; }                     // synth leads
    else if (program < 104) { shape = Tri; }                       // pads/fx
    else { shape = Pulse; decay = 0.9998f; }                       // ethnic/percussive
}

// Percussion: GM drum note -> voice shape, rate and decay.
void percussion_voice(int note, Shape& shape, uint32_t& inc, float& decay) {
    switch (note) {
    case 35: case 36: // kick
        shape = Tri; inc = freq_to_inc(55.0f); decay = 0.9996f; break;
    case 38: case 40: // snare
        shape = Noise; inc = freq_to_inc(9000.0f); decay = 0.9996f; break;
    case 41: case 43: case 45: case 47: case 48: case 50: // toms
        shape = Tri; inc = freq_to_inc(note_freq(note + 12, 128)); decay = 0.9995f; break;
    case 42: case 44: // closed hats
        shape = Noise; inc = freq_to_inc(22000.0f); decay = 0.9990f; break;
    case 46: case 49: case 51: case 52: case 53: case 55: case 57: case 59: // open hat / cymbals
        shape = Noise; inc = freq_to_inc(22000.0f); decay = 0.9999f; break;
    default:
        shape = Noise; inc = freq_to_inc(14000.0f); decay = 0.9993f; break;
    }
}

void start_note(int chan, int note, int vel) {
    Voice* slot = nullptr;
    for (Voice& v : g_voice) {
        if (v.active && v.chan == chan && v.note == note) { // retrigger
            slot = &v;
            break;
        }
    }
    if (!slot) {
        for (Voice& v : g_voice) {
            if (!v.active) { slot = &v; break; }
        }
    }
    if (!slot) { // steal the quietest voice
        slot = &g_voice[0];
        for (Voice& v : g_voice) {
            if (v.env < slot->env)
                slot = &v;
        }
    }

    Voice& v = *slot;
    v.active = true;
    v.released = false;
    v.chan = static_cast<uint8_t>(chan);
    v.note = static_cast<uint8_t>(note);
    v.vel = static_cast<uint8_t>(vel);
    v.phase = 0;
    v.env = 1.0f;
    v.serial = ++g_serial;
    if (chan == kPercussionChannel) {
        percussion_voice(note, v.shape, v.inc, v.decay);
    } else {
        program_voice(g_chan[chan].program, v.shape, v.decay);
        v.inc = freq_to_inc(note_freq(note, g_chan[chan].bend));
    }
}

void release_voice(Voice& v) {
    v.released = true;
    v.decay = kReleaseDecay;
}

void note_off(int chan, int note) {
    if (chan == kPercussionChannel)
        return; // percussion rings out on its own decay
    for (Voice& v : g_voice) {
        if (v.active && !v.released && v.chan == chan && v.note == note)
            release_voice(v);
    }
}

void channel_notes_off(int chan) {
    for (Voice& v : g_voice) {
        if (v.active && !v.released && v.chan == chan)
            release_voice(v);
    }
}

void all_notes_off() {
    for (Voice& v : g_voice) {
        if (v.active && !v.released)
            release_voice(v);
    }
}

void reset_channels() {
    for (Channel& c : g_chan)
        c = Channel();
}

void set_bend(int chan, int bend) {
    g_chan[chan].bend = static_cast<uint8_t>(bend);
    if (chan == kPercussionChannel)
        return;
    for (Voice& v : g_voice) {
        if (v.active && v.chan == chan)
            v.inc = freq_to_inc(note_freq(v.note, bend));
    }
}

bool at_end() {
    return g_pos >= g_end;
}

// Runs every event due now, up to and including the group's trailing delay.
void run_events() {
    // A malformed score of nothing but zero-delay events with looping on
    // would otherwise spin forever.
    int guard = 4096;
    while (g_playing && g_delay_ticks == 0 && guard--) {
        if (at_end()) {
            if (g_looping) { g_pos = g_score; reset_channels(); all_notes_off(); continue; }
            g_playing = false;
            all_notes_off();
            return;
        }
        const uint8_t ev = *g_pos++;
        const bool last = (ev & 0x80) != 0;
        const int type = (ev >> 4) & 7;
        const int chan = ev & 15;
        switch (type) {
        case 0: // release note
            if (!at_end()) note_off(chan, *g_pos++ & 127);
            break;
        case 1: { // play note
            if (at_end()) break;
            const uint8_t b = *g_pos++;
            if ((b & 0x80) && !at_end())
                g_chan[chan].last_vel = *g_pos++ & 127;
            start_note(chan, b & 127, g_chan[chan].last_vel);
            break;
        }
        case 2: // pitch bend
            if (!at_end()) set_bend(chan, *g_pos++);
            break;
        case 3: { // system event
            if (at_end()) break;
            const int c = *g_pos++;
            if (c == 10 || c == 11) channel_notes_off(chan);
            break;
        }
        case 4: { // controller
            if (g_end - g_pos < 2) { g_pos = g_end; break; }
            const int c = g_pos[0];
            const int val = g_pos[1] & 127;
            g_pos += 2;
            if (c == 0) g_chan[chan].program = static_cast<uint8_t>(val);
            else if (c == 3) g_chan[chan].volume = static_cast<uint8_t>(val);
            else if (c == 5) g_chan[chan].expression = static_cast<uint8_t>(val);
            break;
        }
        case 5: // end of measure
            break;
        case 6: // score end
            g_pos = g_end;
            break;
        default:
            break;
        }
        if (last && type != 6) {
            int d = 0;
            while (!at_end()) {
                const uint8_t b = *g_pos++;
                d = (d << 7) | (b & 127);
                if (!(b & 0x80))
                    break;
            }
            g_delay_ticks = d;
        }
    }
}

template <Shape S>
void render_voice(Voice& v, float* out_l, float* out_r, int n, float gain) {
    uint32_t phase = v.phase;
    const uint32_t inc = v.inc;
    uint32_t rng = v.rng;
    float env = v.env;
    const float decay = v.decay;
    for (int i = 0; i < n; ++i) {
        const uint32_t np = phase + inc;
        int s;
        if constexpr (S == Noise) {
            if (np < phase)
                rng = rng * 1664525u + 1013904223u;
            s = static_cast<int8_t>(rng >> 24);
        } else {
            const int p = static_cast<int>(np >> 24);
            if constexpr (S == Square) s = p < 128 ? 112 : -112;
            else if constexpr (S == Pulse) s = p < 64 ? 112 : -112;
            else if constexpr (S == Tri) s = (p < 128 ? p : 255 - p) * 2 - 127;
            else s = p - 128;
        }
        phase = np;
        const float o = static_cast<float>(s) * gain * env;
        out_l[i] += o;
        out_r[i] += o;
        env *= decay;
    }
    v.phase = phase;
    v.rng = rng;
    v.env = env;
    if (env < kSilence)
        v.active = false;
}

void render_voices(float* out_l, float* out_r, int n) {
    for (Voice& v : g_voice) {
        if (!v.active)
            continue;
        const Channel& c = g_chan[v.chan];
        const float amp = (static_cast<float>(v.vel) / 127.0f) * (static_cast<float>(c.volume) / 127.0f)
                          * (static_cast<float>(c.expression) / 127.0f);
        const float gain = g_master * kVoiceGain * amp * (1.0f / 128.0f);
        switch (v.shape) {
        case Square: render_voice<Square>(v, out_l, out_r, n, gain); break;
        case Pulse: render_voice<Pulse>(v, out_l, out_r, n, gain); break;
        case Tri: render_voice<Tri>(v, out_l, out_r, n, gain); break;
        case Saw: render_voice<Saw>(v, out_l, out_r, n, gain); break;
        case Noise: render_voice<Noise>(v, out_l, out_r, n, gain); break;
        }
    }
}

bool any_voice_active() {
    for (const Voice& v : g_voice) {
        if (v.active)
            return true;
    }
    return false;
}

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

} // namespace

bool mus_start(const void* data, bool looping) {
    const uint8_t* d = static_cast<const uint8_t*>(data);
    if (!d || std::memcmp(d, "MUS\x1a", 4) != 0)
        return false;
    const uint16_t len = rd16(d + 4);
    const uint16_t start = rd16(d + 6);
    mus_stop();
    reset_channels();
    g_score = d + start;
    g_pos = g_score;
    g_end = g_score + len;
    g_looping = looping;
    g_paused = false;
    g_delay_ticks = 0;
    g_samples_to_tick = kSamplesPerTick;
    g_playing = true;
    run_events();
    return true;
}

void mus_stop() {
    g_playing = false;
    g_score = g_pos = g_end = nullptr;
    all_notes_off();
}

void mus_pause() {
    g_paused = true;
    all_notes_off();
}

void mus_resume() {
    g_paused = false;
}

void mus_set_volume(int volume) {
    g_master = kMasterScale * static_cast<float>(std::clamp(volume, 0, 127)) / 127.0f;
}

void mus_render(float* mix_l, float* mix_r, int n) {
    const bool sequencing = g_playing && !g_paused;
    if (!sequencing && !any_voice_active())
        return;
    int off = 0;
    while (off < n) {
        int seg = n - off;
        if (sequencing && g_playing && seg > g_samples_to_tick)
            seg = g_samples_to_tick;
        render_voices(mix_l + off, mix_r + off, seg);
        off += seg;
        if (sequencing && g_playing) {
            g_samples_to_tick -= seg;
            if (g_samples_to_tick == 0) {
                g_samples_to_tick = kSamplesPerTick;
                if (g_delay_ticks > 0)
                    --g_delay_ticks;
                if (g_delay_ticks == 0)
                    run_events();
            }
        }
    }
}
