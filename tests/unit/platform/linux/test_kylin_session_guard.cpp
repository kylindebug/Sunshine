/** @file tests/unit/platform/linux/test_kylin_session_guard.cpp
 * @brief Socket lease regression tests without display, input or lock changes.
 */
#ifdef __linux__
  #include "src/platform/linux/session_guard.h"

  #include <future>
  #include <gtest/gtest.h>
  #include <thread>

namespace {
  /** @brief Isolated local peer for protocol fragmentation and crash tests. */
  class KylinSessionGuardTest: public testing::Test {
  protected:
    int listener {-1};  ///< Listening Unix socket.
    std::string path;  ///< Unique socket name.
    std::string previous;  ///< Previous opt-in environment.
    bool had_previous {};  ///< Distinguish unset from empty environment.

    /** @brief Create an isolated peer and save the existing opt-in environment. */
    void SetUp() override {
      const char *old = std::getenv("SUNSHINE_KYLIN_SESSION_SOCKET");
      had_previous = old != nullptr;
      previous = old ? old : "";
      char directory[] = "/tmp/sunshine-guard-XXXXXX";
      ASSERT_NE(mkdtemp(directory), nullptr);
      path = std::string(directory) + "/socket";
      listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      ASSERT_GE(listener, 0);
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::strcpy(address.sun_path, path.c_str());
      ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
      ASSERT_EQ(listen(listener, 4), 0);
      setenv("SUNSHINE_KYLIN_SESSION_SOCKET", path.c_str(), 1);
    }

    /** @brief Remove test endpoints and restore the caller's environment. */
    void TearDown() override {
      close(listener);
      unlink(path.c_str());
      rmdir(path.substr(0, path.rfind('/')).c_str());
      if (had_previous) {
        setenv("SUNSHINE_KYLIN_SESSION_SOCKET", previous.c_str(), 1);
      } else {
        unsetenv("SUNSHINE_KYLIN_SESSION_SOCKET");
      }
    }
  };

  TEST_F(KylinSessionGuardTest, FragmentedAcknowledgementAndImmediateDisconnect) {
    auto server = std::async(std::launch::async, [&] {
      const auto peer = accept(listener, nullptr, nullptr);
      EXPECT_GE(peer, 0);
      EXPECT_EQ(send(peer, "O", 1, MSG_NOSIGNAL), 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      EXPECT_EQ(send(peer, "K\n", 2, MSG_NOSIGNAL), 2);
      char byte;
      EXPECT_EQ(recv(peer, &byte, 1, 0), 0);
      close(peer);
    });
    platf::kylin::session_guard guard;
    EXPECT_TRUE(guard.alive());
    guard.disconnect();
    server.get();
    EXPECT_FALSE(guard.alive());
  }

  TEST_F(KylinSessionGuardTest, RefusedProtectionNeverStartsUnprotectedStream) {
    auto server = std::async(std::launch::async, [&] {
      const auto peer = accept(listener, nullptr, nullptr);
      close(peer);
    });
    EXPECT_THROW(platf::kylin::session_guard {}, std::runtime_error);
    server.get();
  }

  TEST_F(KylinSessionGuardTest, OptInIsRequiredAndMissingGuardFailsClosed) {
    unsetenv("SUNSHINE_KYLIN_SESSION_SOCKET");
    EXPECT_NO_THROW(platf::kylin::session_guard {});
    setenv("SUNSHINE_KYLIN_SESSION_SOCKET", (path + ".missing").c_str(), 1);
    EXPECT_THROW(platf::kylin::session_guard {}, std::runtime_error);
  }
}  // namespace
#endif
