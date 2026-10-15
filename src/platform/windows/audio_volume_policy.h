/**
 * @file src/platform/windows/audio_volume_policy.h
 * @brief Host master-volume policy for loopback packets and endpoint transitions.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <span>

namespace platf::audio::volume_policy {
  /** @brief Observed endpoint controls; unreadable controls remain silent. */
  struct state_t {
    bool valid {};  ///< Both mute and master level were read successfully.
    bool muted {true};  ///< Host master mute.
    float db {};  ///< Master attenuation in decibels, not the slider scalar.
  };

  /**
   * @brief Calculate software attenuation only for pre-volume loopback.
   * @param state Observed master controls.
   * @param post_volume Whether Windows already applied master volume.
   * @return Linear gain, or zero for mute and unreadable controls.
   */
  inline float gain(const state_t &state, bool post_volume) {
    if (!state.valid || state.muted || !std::isfinite(state.db)) {
      return 0.0f;
    }
    return post_volume ? 1.0f : std::pow(10.0f, std::min(state.db, 0.0f) / 20.0f);
  }

  /**
   * @brief Apply current mute to the entire outgoing packet, including queued samples.
   * @param samples Outgoing floating-point PCM packet.
   * @param state Current endpoint controls.
   * @param post_volume Whether Windows already applied master volume.
   */
  inline void apply(std::span<float> samples, const state_t &state, bool post_volume) {
    const auto scale = gain(state, post_volume);
    if (scale == 0.0f) {
      std::fill(samples.begin(), samples.end(), 0.0f);
    } else if (scale != 1.0f) {
      for (auto &sample : samples) {
        sample *= scale;
      }
    }
  }

}  // namespace platf::audio::volume_policy
