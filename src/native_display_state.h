/** @file src/native_display_state.h
 * @brief Deterministic handshake and lock-confirmation decisions.
 */
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>

namespace native_display {
  /** @brief A preparation belongs to exactly one unexpired RTSP launch. */
  struct setup_lease {
    std::optional<std::uint32_t> id;  ///< Owning launch, including valid ID zero.
    std::chrono::steady_clock::time_point expires;  ///< Latest commit time.

    /** @brief Consume a matching, unexpired preparation exactly once. */
    bool commit(std::uint32_t candidate, std::chrono::steady_clock::time_point now) {
      if (!id || *id != candidate || now >= expires) {
        return false;
      }
      id.reset();
      return true;
    }
  };

  /** @brief Require positive lock confirmation; a successful request alone is insufficient.
   * @param query Returns 0 locked, 1 unlocked, -1 unknown. @param request Requests locking.
   * @param wait Waits before polling again. @return Whether locking was confirmed.
   */
  template<class Query, class Request, class Wait>
  bool confirm_lock(Query query, Request request, Wait wait) {
    if (query() == 0) {
      return true;
    }
    if (!request()) {
      return false;
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
      wait();
      if (query() == 0) {
        return true;
      }
    }
    return false;
  }
}  // namespace native_display
