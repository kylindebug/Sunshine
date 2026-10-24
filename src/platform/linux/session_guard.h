/** @file src/platform/linux/session_guard.h
 * @brief Optional UKUI session protection lease tied to a live streaming socket.
 */
#pragma once

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace platf::kylin {
  /** @brief A private local socket held until disconnect; EOF also recovers process crashes. */
  class session_guard {
    int fd {-1};  ///< Owned lease socket, or disabled.

  public:
    /** @brief Acquire protection before starting capture; throw if configured protection fails. */
    session_guard() {
      const auto path = std::getenv("SUNSHINE_KYLIN_SESSION_SOCKET");
      if (!path || !*path) {
        return;
      }
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      if (std::strlen(path) >= sizeof(address.sun_path)) {
        throw std::runtime_error("Kylin session guard socket path is too long");
      }
      std::strcpy(address.sun_path, path);
      fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      timeval timeout {10, 0};
      if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) || connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
        reset();
        throw std::runtime_error("Kylin session guard unavailable; refusing unprotected stream");
      }
      char reply[3] {};
      size_t received = 0;
      while (received < sizeof(reply)) {
        const auto count = recv(fd, reply + received, sizeof(reply) - received, 0);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          break;
        }
        received += count;
      }
      if (received != sizeof(reply) || std::string(reply, received) != "OK\n") {
        reset();
        throw std::runtime_error("Kylin session guard refused stream; see sunshine-kylin-session journal");
      }
    }

    /** @brief Release on ordinary teardown and failed startup. */
    ~session_guard() {
      reset();
    }

    session_guard(const session_guard &) = delete;
    session_guard &operator=(const session_guard &) = delete;

    /** @brief Detect loss of the protector without consuming data or blocking.
     * @return True if disabled or the guard still owns the peer socket.
     */
    bool alive() const {
      if (fd < 0) {
        return true;
      }
      char byte;
      const auto result = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
      return result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }

    /** @brief Notify disconnect before potentially slow encoder/audio joins finish. */
    void disconnect() const {
      if (fd >= 0) {
        shutdown(fd, SHUT_WR);
      }
    }

    /** @brief Close the lease; the guard locks before restoring local access. */
    void reset() {
      if (fd >= 0) {
        close(fd);
        fd = -1;
      }
    }
  };
}  // namespace platf::kylin
