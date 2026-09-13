#pragma once

// Exposes the libdvi instance src/i_video_dvi.cpp owns, so a later HDMI
// audio driver (src/i_sound_dvi.cpp, not yet added -- see
// docs/HDMI_PLAN.md's phased rollout) can configure the same instance's
// audio ring (dvi_audio_sample_buffer_set()/dvi_set_audio_freq(), dvi.h)
// without this header needing to name `struct dvi_inst` itself.
extern "C" struct dvi_inst* i_video_dvi_instance(void);
