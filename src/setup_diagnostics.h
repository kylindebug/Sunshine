/** @file src/setup_diagnostics.h
 * @brief Non-sensitive, bounded streaming setup diagnostics.
 */
#pragma once
#include <string_view>

namespace setup_diagnostics {
  /** @brief Last observed setup operation, not a claim of successful completion. */
  enum class stage {
    waiting,  ///< No RTSP connection has been accepted.
    tcp,  ///< TCP connected without a parsed RTSP method.
    options,  ///< OPTIONS was received.
    describe,  ///< DESCRIBE was received.
    setup,  ///< SETUP was received.
    announce,  ///< ANNOUNCE was received.
    play,  ///< PLAY was received, without claiming control-channel completion.
    other  ///< An unrecognized method was received; its text is not retained.
  };

  /** @brief Classify a command without retaining untrusted request text.
   * @param command Parsed RTSP command. @return Bounded diagnostic stage.
   */
  inline stage command_stage(std::string_view command) {
    if (command == "OPTIONS") {
      return stage::options;
    }
    if (command == "DESCRIBE") {
      return stage::describe;
    }
    if (command == "SETUP") {
      return stage::setup;
    }
    if (command == "ANNOUNCE") {
      return stage::announce;
    }
    if (command == "PLAY") {
      return stage::play;
    }
    return stage::other;
  }

  /** @brief Format a bounded stage name.
   * @param value Last observed operation. @return Static diagnostic name.
   */
  inline const char *name(stage value) {
    switch (value) {
      case stage::waiting:
        return "waiting_for_rtsp_tcp";
      case stage::tcp:
        return "tcp_accepted_no_parsed_request";
      case stage::options:
        return "OPTIONS_received";
      case stage::describe:
        return "DESCRIBE_received";
      case stage::setup:
        return "SETUP_received";
      case stage::announce:
        return "ANNOUNCE_received";
      case stage::play:
        return "PLAY_received";
      default:
        return "unknown_command_received";
    }
  }

  /** @brief Classify an expired setup without asserting a network root cause.
   * @param value Last observed operation. @return Stable error code.
   */
  inline const char *timeout_code(stage value) {
    if (value == stage::waiting) {
      return "HS_NO_RTSP_CONNECTION";
    }
    if (value == stage::tcp) {
      return "HS_RTSP_REQUEST_TIMEOUT";
    }
    return "HS_RTSP_NEGOTIATION_TIMEOUT";
  }
}  // namespace setup_diagnostics
