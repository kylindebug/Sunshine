/** @file src/audio_silence.h
 * @brief Final mute gate for PCM waiting in the encoder queue.
 */
#pragma once
#include <algorithm>
#include <span>

namespace audio {
  /**
   * @brief Check current mute after dequeuing without applying master attenuation twice.
   * @param samples PCM about to be encoded.
   * @param muted Thread-safe callback observing the latest host mute.
   */
  template<class Muted>
  void silence_if_needed(std::span<float> samples, Muted muted) {
    if (muted()) {
      std::fill(samples.begin(), samples.end(), 0.0f);
    }
  }
}  // namespace audio
