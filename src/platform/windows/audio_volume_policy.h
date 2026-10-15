/**
 * @file src/platform/windows/audio_volume_policy.h
 * @brief Host master-volume policy for loopback packets and endpoint transitions.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <span>

namespace platf::audio::volume_policy {
  /**
   * @brief Treat a zero master slider as silence even when Windows mute is false.
   * @param known Whether both master controls were read.
   * @param muted Windows master-mute state.
   * @param slider Windows master slider scalar.
   * @return True for mute, zero volume, or unreadable controls.
   */
  inline bool silence_required(bool known, bool muted, float slider) {
    return !known || muted || !std::isfinite(slider) || slider <= 0.0f;
  }

  /**
   * @brief Skip an unchanged route only when no final logical controls need restoring.
   * @param restoring Whether a routed session is returning to the host endpoints.
   * @param roles_match Whether all current roles already match the requested route.
   * @return True only for an unchanged non-restoration route.
   */
  inline bool skip_transition(bool restoring, bool roles_match) {
    return roles_match && !restoring;
  }

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

  /**
   * @brief Restore each role before allowing old endpoint controls to unmute.
   * @param role_count Number of original Windows audio roles.
   * @param restore_role Callback restoring one role and returning success.
   * @param restore_controls Callback receiving whether unmuting is safe.
   * @return True only if all roles were restored.
   */
  template<class RestoreRole, class RestoreControls>
  bool rollback(int role_count, RestoreRole restore_role, RestoreControls restore_controls) {
    bool restored {true};
    for (int role = 0; role < role_count; ++role) {
      if (!restore_role(role)) {
        restored = false;
      }
    }
    restore_controls(restored);
    return restored;
  }
}  // namespace platf::audio::volume_policy
