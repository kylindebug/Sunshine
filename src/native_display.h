/**
 * @file src/native_display.h
 * @brief Opt-in Windows display transactions owned by streaming sessions.
 */
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace native_display {
  /** @brief Per-application native policy; disabled preserves upstream behavior. */
  enum class policy {
    disabled,  ///< Preserve upstream and legacy-script behavior.
    physical,  ///< Keep the physical layout and lock when streaming ends.
    privacy  ///< Prepare an exclusive virtual display and restore afterward.
  };
  /** @brief Parse a policy, rejecting unknown values. @param text Stored policy. @return Parsed policy. */
  policy parse(const std::string &text);
  /** @brief Initialize crash recovery.
   * @param path Private journal.
   * @param log Diagnostic sink.
   * @param stop_streams Nonblocking request to terminate active streams.
   */
  void init(const std::filesystem::path &path, std::function<void(const std::string &)> log, std::function<void()> stop_streams);
  /** @brief Restore and join the controller before process shutdown. */
  void shutdown();
  /** @brief Prepare before encoder probing; failures never silently expose a physical display.
   * @param session_id Launch owning this preparation.
   * @param mode Application policy. @param width Requested width. @param height Requested height.
   * @param fps Requested refresh rate. @return Empty on success, otherwise actionable error.
   */
  std::string prepare(std::uint32_t session_id, policy mode, int width, int height, int fps);
  /** @brief Commit an unexpired setup before starting video. @param session_id Owning launch. @return Success. */
  bool connected(std::uint32_t session_id);
  /** @brief Restore on last-stream disconnect, setup failure, cancellation, or process exit. */
  void release();
  /** @brief Select the prepared native output. @param fallback Upstream selector. @return Capture selector. */
  std::string output(const std::string &fallback);
  /** @brief Whether capture must never fall back to a different display.
   * @return True while an exclusive privacy transaction owns the display.
   */
  bool exclusive();
  /** @brief Validate the actual DXGI output against current private target identity.
   * @param name Actual GDI output name. @param monitor Actual DXGI HMONITOR, represented without Windows headers.
   * @return Whether capture is permitted.
   */
  bool capture_allowed(const std::string &name, std::uintptr_t monitor);
  /** @brief Cancel only an abandoned setup belonging to this launch. @param session_id Owning launch. */
  void cancel_setup(std::uint32_t session_id);
}  // namespace native_display
