/**
 * @file src/platform/windows/mouse_keys_guard.h
 * @brief Ownership-aware, nonpersistent cursor fallback for mouse-less Windows sessions.
 */
#pragma once

#include <functional>
#include <optional>
#include <Windows.h>

namespace platf::cursor {
  /** @brief Injectable Windows operations used by the cursor fallback state machine. */
  struct mouse_keys_api_t {
    std::function<bool()> mouse_present;  ///< Query system mouse availability, including Mouse Keys.
    std::function<bool(MOUSEKEYS &)> read;  ///< Read the current Mouse Keys settings.
    std::function<bool(const MOUSEKEYS &)> write;  ///< Apply runtime settings without profile persistence.
  };

  /** @brief Observable outcomes of a cursor fallback transition. */
  enum class result_e {
    unchanged,  ///< No settings change was necessary.
    enabled,  ///< Acquired ownership and enabled the cursor fallback.
    deferred,  ///< Kept the fallback because restoring would remove the last mouse.
    restored,  ///< Restored the original settings with a mouse still available.
    user_changed,  ///< Relinquished ownership after an external settings change.
    read_failed,  ///< Could not inspect settings; ownership was retained for retry.
    write_failed,  ///< Could not apply settings; ownership was retained where applicable.
  };

  /**
   * @brief Preserve the first snapshot across reconnects and defer unsafe restoration.
   * @details Callers serialize access. This class never persists settings or infers a
   * hardware mouse from SM_MOUSEPRESENT while its own Mouse Keys fallback is enabled.
   */
  class mouse_keys_guard_t {
  public:
    /** @brief Start a new session without replacing an outstanding original snapshot. */
    void begin_session() {
      user_override_ = false;
    }

    /**
     * @brief Enable only the required bits when Windows has no usable mouse.
     * @param api Windows settings operations.
     * @return Transition outcome.
     */
    result_e ensure(const mouse_keys_api_t &api) {
      if (user_override_ || api.mouse_present()) {
        return result_e::unchanged;
      }
      MOUSEKEYS current {};
      if (!api.read(current)) {
        return result_e::read_failed;
      }
      if (original_ && !same(current, *applied_)) {
        return relinquish();
      }
      if (current.dwFlags & MKF_MOUSEKEYSON) {
        return result_e::unchanged;
      }
      auto desired = current;
      desired.dwFlags |= MKF_MOUSEKEYSON | MKF_AVAILABLE;
      if (!api.write(desired)) {
        return result_e::write_failed;
      }
      if (!original_) {
        original_ = current;
      }
      applied_ = desired;
      retry_ = false;
      return result_e::enabled;
    }

    /**
     * @brief Restore the original settings, retaining a runtime fallback if no mouse remains.
     * @param api Windows settings operations.
     * @return Transition outcome; failed writes can be retried without losing ownership.
     */
    result_e restore(const mouse_keys_api_t &api) {
      if (!original_) {
        return result_e::unchanged;
      }
      MOUSEKEYS current {};
      if (!api.read(current)) {
        retry_ = true;
        return result_e::read_failed;
      }
      if (!same(current, *applied_)) {
        return relinquish();
      }
      if (!api.write(*original_)) {
        retry_ = true;
        return result_e::write_failed;
      }
      applied_ = *original_;
      // Query after removing our synthetic mouse, so it cannot mask missing hardware.
      if (api.mouse_present()) {
        original_.reset();
        applied_.reset();
        deferred_ = retry_ = false;
        return result_e::restored;
      }
      auto fallback = *original_;
      fallback.dwFlags |= MKF_MOUSEKEYSON | MKF_AVAILABLE;
      deferred_ = true;
      if (!api.write(fallback)) {
        retry_ = true;
        return result_e::write_failed;
      }
      applied_ = fallback;
      retry_ = false;
      return result_e::deferred;
    }

    /**
     * @brief Detect manual settings changes without overwriting them.
     * @param api Windows settings operations.
     * @return User-change or read-failure status, otherwise unchanged.
     */
    result_e observe(const mouse_keys_api_t &api) {
      if (!original_) {
        return result_e::unchanged;
      }
      MOUSEKEYS current {};
      if (!api.read(current)) {
        return result_e::read_failed;
      }
      return same(current, *applied_) ? result_e::unchanged : relinquish();
    }

    /** @brief Report whether a mouse-less disconnect still owns a temporary fallback. */
    bool deferred() const {
      return deferred_;
    }

    /** @brief Report whether a failed restore needs another attempt. */
    bool needs_retry() const {
      return retry_;
    }

  private:
    /** @brief Compare user-configurable fields, ignoring transient button state bits. */
    static bool same(const MOUSEKEYS &a, const MOUSEKEYS &b) {
      constexpr DWORD transient = MKF_LEFTBUTTONSEL | MKF_RIGHTBUTTONSEL | MKF_LEFTBUTTONDOWN | MKF_RIGHTBUTTONDOWN;
      return (a.dwFlags & ~transient) == (b.dwFlags & ~transient) &&
             a.iMaxSpeed == b.iMaxSpeed && a.iTimeToMaxSpeed == b.iTimeToMaxSpeed &&
             a.iCtrlSpeed == b.iCtrlSpeed && a.dwReserved1 == b.dwReserved1 && a.dwReserved2 == b.dwReserved2;
    }

    /** @brief Stop managing settings after a user override, until another session begins. */
    result_e relinquish() {
      original_.reset();
      applied_.reset();
      deferred_ = retry_ = false;
      user_override_ = true;
      return result_e::user_changed;
    }

    std::optional<MOUSEKEYS> original_;  ///< First unmodified settings snapshot.
    std::optional<MOUSEKEYS> applied_;  ///< Last settings written by this guard.
    bool deferred_ {false};  ///< Restoration would currently remove the last cursor provider.
    bool retry_ {false};  ///< An operation failed without losing ownership.
    bool user_override_ {false};  ///< Avoid reasserting the fallback after a manual change.
  };

  /** @brief Begin cursor protection for the first active streaming session. */
  void streaming_started();
  /** @brief Restore or defer cursor settings after the final streaming session ends. */
  void streaming_stopped();
  /** @brief Maintain cursor availability during streaming and mouse hot-unplug. */
  void ensure_mouse_cursor();
}  // namespace platf::cursor
