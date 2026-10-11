/**
 * @file src/native_display.h
 * @brief Opt-in Windows display transactions owned by streaming sessions.
 */
#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace native_display {
  /** @brief Per-application native policy; disabled preserves upstream behavior. */
  enum class policy {
    disabled,
    physical,
    privacy
  };
  /** @brief Parse a policy, rejecting unknown values. @param text Stored policy. @return Parsed policy. */
  policy parse(const std::string &text);
  /** @brief Initialize crash recovery. @param path Private journal. @param log Diagnostic sink. */
  void init(const std::filesystem::path &path, std::function<void(const std::string &)> log);
  /** @brief Restore and join the controller before process shutdown. */
  void shutdown();
  /** @brief Prepare before encoder probing; failures never silently expose a physical display.
   * @param mode Application policy. @param width Requested width. @param height Requested height.
   * @param fps Requested refresh rate. @return Empty on success, otherwise actionable error.
   */
  std::string prepare(policy mode, int width, int height, int fps);
  /** @brief Mark the first real stream as connected, cancelling setup expiry. */
  void connected();
  /** @brief Restore on last-stream disconnect, setup failure, cancellation, or process exit. */
  void release();
  /** @brief Select the prepared native output. @param fallback Upstream selector. @return Capture selector. */
  std::string output(const std::string &fallback);
}  // namespace native_display
