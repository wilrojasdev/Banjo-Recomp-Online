// Android audio output for BK64-Online.
//
// Phase 7 deliverable: Oboe-backed implementation of ultramodern::audio_callbacks_t.
// Replaces the desktop SDL2 audio path (which isn't compiled on Android).
//
// The recompiled MIPS code calls queue_samples() to push int16 stereo samples,
// queries get_frames_remaining() to know how full the buffer is, and may call
// set_frequency() if the game changes its audio rate (Banjo-Kazooie hands us
// 32000 Hz typically). Oboe pulls samples from our ring buffer in its own
// callback thread.

#pragma once

#ifdef __ANDROID__

#include "ultramodern/ultramodern.hpp"

namespace banjo_android::audio {

// Open the Oboe stream and prepare the ring buffer. Idempotent — safe to call
// before make_audio_callbacks() is registered. Returns false if the device has
// no audio output at all (rare, mostly emulators).
bool start();

// Close the Oboe stream and release the ring buffer.
void stop();

// Build callbacks ready to register with ultramodern.
ultramodern::audio_callbacks_t make_audio_callbacks();

}  // namespace banjo_android::audio

#endif  // __ANDROID__
