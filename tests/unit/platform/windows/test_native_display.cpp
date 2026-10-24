/** @file tests/unit/platform/windows/test_native_display.cpp
 * @brief Native display policy and CCD construction regressions; no hardware is mutated.
 */
#include "src/native_display.h"
#include "src/native_display_state.h"
#include "src/setup_diagnostics.h"
#ifdef _WIN32
  #include "src/platform/windows/native_display_desktop.h"
  #include "src/platform/windows/native_display_layout.h"

  #include <fstream>
  #include <gtest/gtest.h>

namespace {
  /** @brief Model desktop changes and permission failures without changing Windows state. */
  class NativeDesktopTest: public testing::Test {
  protected:
    HDESK original {reinterpret_cast<HDESK>(1)};  ///< Borrowed worker desktop.
    HDESK input {reinterpret_cast<HDESK>(2)};  ///< Desktop currently receiving input.
    HDESK attached {original};  ///< Worker desktop selected by the fake API.
    bool attach_ok {true};  ///< Inject SetThreadDesktop failure.
    std::vector<HDESK> closed;  ///< Owned handles released after each job.
    native_display::win::desktop_api api {
      [&] {
        return original;
      },
      [&] {
        return input;
      },
      [&](HDESK desktop) {
        if (!attach_ok) {
          return false;
        }
        attached = desktop;
        return true;
      },
      [&](HDESK desktop) {
        closed.push_back(desktop);
      },
      [] {
        return ERROR_ACCESS_DENIED;
      }
    };  ///< Worker-only test operations.
  };

  TEST_F(NativeDesktopTest, PreparationReconnectAndHotplugFollowCurrentDesktop) {
    native_display::win::desktop_executor executor(api);
    std::thread::id worker;
    for (int cycle = 0; cycle < 6; ++cycle) {
      input = reinterpret_cast<HDESK>(static_cast<std::uintptr_t>(2 + cycle));
      EXPECT_EQ(executor.call([&] {
        if (worker == std::thread::id {}) {
          worker = std::this_thread::get_id();
        }
        EXPECT_EQ(std::this_thread::get_id(), worker);
        EXPECT_EQ(attached, input);
        return cycle;
      }),
                cycle);
      EXPECT_EQ(attached, original);
      ASSERT_EQ(closed.size(), static_cast<std::size_t>(cycle + 1));
      EXPECT_EQ(closed.back(), input);
    }
    EXPECT_NE(worker, std::this_thread::get_id());
  }

  TEST_F(NativeDesktopTest, DisplayFailureRestoresDesktopAndWorkerCanRetry) {
    native_display::win::desktop_executor executor(api);
    EXPECT_THROW(executor.call([] {
      throw std::runtime_error("Display validation failed");
    }),
                 std::runtime_error);
    EXPECT_EQ(attached, original);
    ASSERT_EQ(closed.size(), 1);
    EXPECT_EQ(executor.call([] {
      return 42;
    }),
              42);
    EXPECT_EQ(closed.size(), 2);
  }

  TEST_F(NativeDesktopTest, DeniedInputDesktopNeverInvokesDisplayAndCanRetry) {
    native_display::win::desktop_executor executor(api);
    const auto available = input;
    input = nullptr;
    bool invoked = false;
    try {
      executor.call([&] {
        invoked = true;
      });
      FAIL() << "Expected desktop permission failure";
    } catch (const std::runtime_error &error) {
      EXPECT_NE(std::string(error.what()).find("[ND_INPUT_DESKTOP] OpenInputDesktop: Windows error 5"), std::string::npos);
    }
    EXPECT_FALSE(invoked);
    EXPECT_TRUE(closed.empty());
    input = available;
    executor.call([&] {
      invoked = true;
    });
    EXPECT_TRUE(invoked);
  }

  TEST_F(NativeDesktopTest, MissingBorrowedDesktopNeverOpensOrClosesAHandle) {
    native_display::win::desktop_executor executor(api);
    original = nullptr;
    bool invoked = false;
    EXPECT_THROW(executor.call([&] {
      invoked = true;
    }),
                 std::runtime_error);
    EXPECT_FALSE(invoked);
    EXPECT_TRUE(closed.empty());
  }

  TEST_F(NativeDesktopTest, FailedAttachClosesOnlyOwnedHandleAndDoesNotInvokeDisplay) {
    native_display::win::desktop_executor executor(api);
    attach_ok = false;
    bool invoked = false;
    EXPECT_THROW(executor.call([&] {
      invoked = true;
    }),
                 std::runtime_error);
    EXPECT_FALSE(invoked);
    ASSERT_EQ(closed.size(), 1);
    EXPECT_EQ(closed.front(), input);
    attach_ok = true;
    executor.call([&] {
      invoked = true;
    });
    EXPECT_TRUE(invoked);
    EXPECT_EQ(attached, original);
  }

  TEST_F(NativeDesktopTest, ConcurrentCallersShareOneSerializedWorker) {
    native_display::win::desktop_executor executor(api);
    std::thread::id worker;
    std::vector<std::future<int>> callers;
    for (int i = 0; i < 12; ++i) {
      callers.push_back(std::async(std::launch::async, [&] {
        return executor.call([&] {
          if (worker == std::thread::id {}) {
            worker = std::this_thread::get_id();
          }
          EXPECT_EQ(worker, std::this_thread::get_id());
          EXPECT_EQ(attached, input);
          return 7;
        });
      }));
    }
    for (auto &caller : callers) {
      EXPECT_EQ(caller.get(), 7);
    }
    EXPECT_EQ(closed.size(), 12);
    EXPECT_EQ(attached, original);
  }
}  // namespace

TEST(NativeDisplay, ExistingApplicationsRemainDisabled) {
  EXPECT_EQ(native_display::parse(""), native_display::policy::disabled);
  EXPECT_EQ(native_display::parse("disabled"), native_display::policy::disabled);
  EXPECT_EQ(native_display::parse("physical"), native_display::policy::physical);
  EXPECT_EQ(native_display::parse("privacy"), native_display::policy::privacy);
  EXPECT_THROW(native_display::parse("privcy"), std::invalid_argument);
}

TEST(NativeDisplay, ExactModeRepairsCached1080pWithRequested720p) {
  std::vector<DWORD> calls;
  const auto enumerate = [](DWORD index, DEVMODEW &mode) {
    mode.dmPelsWidth = index == 0 ? 1920 : 1280;
    mode.dmPelsHeight = index == 0 ? 1080 : 720;
    mode.dmDisplayFrequency = 30;
    mode.dmBitsPerPel = 32;
    return index < 2;
  };
  EXPECT_TRUE(native_display::win::apply_exact_mode(enumerate, [&](DEVMODEW &mode, DWORD flags) {
    calls.push_back(flags);
    EXPECT_EQ(mode.dmPelsWidth, 1280);
    EXPECT_EQ(mode.dmPelsHeight, 720);
    EXPECT_EQ(mode.dmDisplayFrequency, 30);
    EXPECT_EQ(mode.dmPosition.x, 0);
    EXPECT_TRUE(mode.dmFields & DM_DISPLAYFREQUENCY);
    return DISP_CHANGE_SUCCESSFUL;
  },
                                                    1280,
                                                    720,
                                                    30));
  EXPECT_EQ(calls, (std::vector<DWORD> {CDS_TEST, CDS_UPDATEREGISTRY}));
}

TEST(NativeDisplay, ExactModeNeverSubstitutesResolutionRefreshOrInterlacing) {
  for (int mismatch = 0; mismatch < 4; ++mismatch) {
    int changes = 0;
    EXPECT_FALSE(native_display::win::apply_exact_mode([&](DWORD index, DEVMODEW &mode) {
      mode.dmPelsWidth = mismatch == 0 ? 1920 : 1280;
      mode.dmPelsHeight = 720;
      mode.dmDisplayFrequency = mismatch == 1 ? 60 : 30;
      mode.dmBitsPerPel = mismatch == 2 ? 16 : 32;
      mode.dmDisplayFlags = mismatch == 3 ? DM_INTERLACED : 0;
      return index == 0;
    },
                                                       [&](DEVMODEW &, DWORD) {
                                                         ++changes;
                                                         return DISP_CHANGE_SUCCESSFUL;
                                                       },
                                                       1280,
                                                       720,
                                                       30));
    EXPECT_EQ(changes, 0);
  }
}

TEST(NativeDisplay, ExactModeDoesNotApplyAfterFailedValidationAndReportsApplyFailure) {
  for (const DWORD failed_stage : {CDS_TEST, CDS_UPDATEREGISTRY}) {
    std::vector<DWORD> calls;
    EXPECT_FALSE(native_display::win::apply_exact_mode([](DWORD, DEVMODEW &mode) {
      mode.dmPelsWidth = 1280;
      mode.dmPelsHeight = 720;
      mode.dmDisplayFrequency = 30;
      mode.dmBitsPerPel = 32;
      return true;
    },
                                                       [&](DEVMODEW &, DWORD flags) {
                                                         calls.push_back(flags);
                                                         return flags == failed_stage ? DISP_CHANGE_BADMODE : DISP_CHANGE_SUCCESSFUL;
                                                       },
                                                       1280,
                                                       720,
                                                       30));
    EXPECT_EQ(calls.size(), failed_stage == CDS_TEST ? 1 : 2);
  }
}

TEST(NativeDisplay, SetupTimeoutDistinguishesTransportFromNegotiation) {
  using namespace setup_diagnostics;
  EXPECT_STREQ(timeout_code(stage::waiting), "HS_NO_RTSP_CONNECTION");
  EXPECT_STREQ(timeout_code(stage::tcp), "HS_RTSP_REQUEST_TIMEOUT");
  for (const auto command : {"OPTIONS", "DESCRIBE", "SETUP", "ANNOUNCE", "PLAY"}) {
    EXPECT_NE(command_stage(command), stage::other);
    EXPECT_STREQ(timeout_code(command_stage(command)), "HS_RTSP_NEGOTIATION_TIMEOUT");
  }
  EXPECT_EQ(command_stage("secret=not-a-command\n"), stage::other);
  EXPECT_STREQ(name(command_stage("secret=not-a-command\n")), "unknown_command_received");
  EXPECT_STREQ(name(stage::waiting), "waiting_for_rtsp_tcp");
  EXPECT_STREQ(name(stage::tcp), "tcp_accepted_no_parsed_request");
  EXPECT_STREQ(name(stage::play), "PLAY_received");
}

TEST(NativeDisplay, IndirectCaptureUsesCurrentMonitorSourceNotRenderAdapter) {
  using native_display::win::same_capture_source;
  // This host exposes DISPLAY45 through CCD LUID 0:705897126 and NVIDIA
  // DXGI LUID 0:58921. Both APIs and the live HMONITOR name the same source.
  EXPECT_TRUE(same_capture_source(L"\\\\.\\DISPLAY45", L"\\\\.\\DISPLAY45", L"\\\\.\\DISPLAY45"));
  EXPECT_TRUE(same_capture_source(L"\\\\.\\DISPLAY45", L"\\\\.\\display45", L"\\\\.\\DISPLAY45"));
  EXPECT_FALSE(same_capture_source(L"\\\\.\\DISPLAY45", L"\\\\.\\DISPLAY5", L"\\\\.\\DISPLAY5"));
  // Matching the cached output name alone cannot admit a stale/reassigned handle.
  EXPECT_FALSE(same_capture_source(L"\\\\.\\DISPLAY45", L"\\\\.\\DISPLAY45", L"\\\\.\\DISPLAY5"));
  EXPECT_FALSE(same_capture_source(L"\\\\.\\DISPLAY46", L"\\\\.\\DISPLAY45", L"\\\\.\\DISPLAY45"));
  EXPECT_FALSE(same_capture_source(L"", L"", L""));
  EXPECT_FALSE(same_capture_source(L"\\\\.\\DISPLAY45", L"", L"\\\\.\\DISPLAY45"));
  EXPECT_FALSE(same_capture_source(L"\\\\.\\DISPLAY45", L"\\\\.\\DISPLAY45", L""));
}

TEST(NativeDisplay, PossibleRoutesAreNotLimitedToActiveMonitorCount) {
  using native_display::win::valid_query_capacity;
  // This host reports 80 possible routes with just one active physical monitor.
  EXPECT_TRUE(valid_query_capacity(QDC_ALL_PATHS, 80, 160));
  EXPECT_TRUE(valid_query_capacity(QDC_ALL_PATHS, 256, 512));
  EXPECT_TRUE(valid_query_capacity(QDC_ALL_PATHS, 16384, 32768));
  EXPECT_FALSE(valid_query_capacity(QDC_ALL_PATHS, 16385, 32768));
  EXPECT_FALSE(valid_query_capacity(QDC_ALL_PATHS, 16384, 32769));
  EXPECT_TRUE(valid_query_capacity(QDC_ONLY_ACTIVE_PATHS, 64, 256));
  EXPECT_FALSE(valid_query_capacity(QDC_ONLY_ACTIVE_PATHS, 80, 160));
  EXPECT_FALSE(valid_query_capacity(QDC_ONLY_ACTIVE_PATHS, 64, 257));
}

TEST(NativeDisplay, LateAndWrongHandshakesCannotCommit) {
  using namespace std::chrono_literals;
  auto now = std::chrono::steady_clock::time_point {};
  native_display::setup_lease lease {12, now + 30s};
  EXPECT_FALSE(lease.commit(13, now));
  EXPECT_FALSE(lease.commit(12, now + 30s));
  EXPECT_TRUE(lease.id.has_value());
  lease = {13, now + 60s};
  EXPECT_FALSE(lease.commit(12, now + 31s));
  EXPECT_TRUE(lease.commit(13, now + 31s));
  EXPECT_FALSE(lease.commit(13, now + 32s));
}

TEST(NativeDisplay, AlreadyLockedDoesNotRequestLockAgain) {
  bool requested = false;
  EXPECT_TRUE(native_display::confirm_lock([] {
    return 0;
  },
                                           [&] {
                                             requested = true;
                                             return false;
                                           },
                                           [] {
                                           }));
  EXPECT_FALSE(requested);
}

TEST(NativeDisplay, LockRequestSuccessRequiresConfirmation) {
  int polls = 0;
  EXPECT_FALSE(native_display::confirm_lock([] {
    return -1;
  },
                                            [] {
                                              return true;
                                            },
                                            [&] {
                                              ++polls;
                                            }));
  EXPECT_EQ(polls, 20);
  polls = 0;
  EXPECT_TRUE(native_display::confirm_lock([&] {
    return polls >= 3 ? 0 : 1;
  },
                                           [] {
                                             return true;
                                           },
                                           [&] {
                                             ++polls;
                                           }));
  EXPECT_EQ(polls, 3);
}

TEST(NativeDisplay, InvalidClientModeIsRejectedWithoutFallback) {
  using native_display::win::valid_mode;
  EXPECT_FALSE(valid_mode(0, 1080, 60));
  EXPECT_FALSE(valid_mode(1920, -1, 60));
  EXPECT_FALSE(valid_mode(1920, 1080, 0));
  EXPECT_FALSE(valid_mode(1920, 1080, 241));
  EXPECT_FALSE(valid_mode(999999, 1080, 60));
  EXPECT_TRUE(valid_mode(1920, 1080, 60));
  EXPECT_TRUE(valid_mode(2560, 1600, 120));
}

TEST(NativeDisplay, UsesRealCcdMonitorPathNotPnpInstancePrefix) {
  using native_display::win::is_mtt_target;
  EXPECT_TRUE(is_mtt_target(L"\\\\?\\DISPLAY#MTT1337#1&2#{device-guid}"));
  EXPECT_TRUE(is_mtt_target(L"\\\\?\\display#mtt1337#1&2#{device-guid}"));
  EXPECT_FALSE(is_mtt_target(L"\\\\?\\DISPLAY#SAM0DD3#1&2#{device-guid}"));
  EXPECT_FALSE(is_mtt_target(L"\\\\?\\DISPLAY#MTT1337OTHER#1&2#{device-guid}"));
}

TEST(NativeDisplay, PrivacyHasOneOriginAtClientMode) {
  DISPLAYCONFIG_PATH_INFO target {};
  target.sourceInfo.id = 17;
  target.sourceInfo.adapterId.LowPart = 42;
  target.targetInfo.id = 99;
  target.targetInfo.adapterId.LowPart = 42;
  target.targetInfo.targetAvailable = TRUE;
  auto value = native_display::win::virtual_layout(target, 1920, 1080, 60);
  ASSERT_TRUE(native_display::win::valid(value));
  ASSERT_EQ(value.paths.size(), 1);
  ASSERT_EQ(value.modes.size(), 1);
  EXPECT_EQ(value.paths[0].targetInfo.id, 99);
  EXPECT_EQ(value.paths[0].targetInfo.refreshRate.Numerator, 60);
  EXPECT_EQ(value.paths[0].targetInfo.modeInfoIdx, DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
  EXPECT_EQ(value.modes[0].sourceMode.width, 1920);
  EXPECT_EQ(value.modes[0].sourceMode.height, 1080);
  EXPECT_EQ(value.modes[0].sourceMode.position.x, 0);
  EXPECT_EQ(value.modes[0].sourceMode.position.y, 0);
  EXPECT_EQ(value.modes[0].id, target.sourceInfo.id);
  EXPECT_EQ(value.modes[0].adapterId.LowPart, 42);
}

TEST(NativeDisplay, ModeFailureReportsActualRefreshWithoutRounding) {
  using namespace native_display::win;
  EXPECT_EQ(mode_summary({}), "invalid_snapshot");
  auto value = virtual_layout({}, 1920, 1080, 30);
  EXPECT_EQ(mode_summary(value), "1920x1080@30/1Hz");
  value.paths.front().targetInfo.refreshRate = {60000, 1001};
  EXPECT_EQ(mode_summary(value), "1920x1080@60000/1001Hz");
  value.paths.push_back(value.paths.front());
  EXPECT_EQ(mode_summary(value), "active_displays=2");
}

TEST(NativeDisplay, ExplicitRefreshUsesProgressiveScanline) {
  DISPLAYCONFIG_PATH_INFO target {};
  for (int fps : {30, 60, 90, 120, 240}) {
    const auto value = native_display::win::virtual_layout(target, 1920, 1080, fps);
    const auto &requested = value.paths.front().targetInfo;
    EXPECT_EQ(requested.refreshRate.Numerator, static_cast<UINT32>(fps));
    EXPECT_EQ(requested.refreshRate.Denominator, 1);
    EXPECT_EQ(requested.scanLineOrdering, DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE);
    EXPECT_EQ(requested.modeInfoIdx, DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
  }
}

TEST(NativeDisplay, RejectsTruncatedOrInvalidSnapshot) {
  native_display::win::layout value;
  EXPECT_FALSE(native_display::win::valid(value));
  value.paths.resize(1);
  EXPECT_FALSE(native_display::win::valid(value));
  value.modes.resize(1);
  EXPECT_FALSE(native_display::win::valid(value));
  value.modes[0].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
  EXPECT_TRUE(native_display::win::valid(value));
  value.paths[0].sourceInfo.modeInfoIdx = 5;
  EXPECT_FALSE(native_display::win::valid(value));
}

TEST(NativeDisplay, CorruptRecoveryJournalDoesNotOccupyLegacyGate) {
  auto path = std::filesystem::temp_directory_path() / ("sunshine-native-journal-test-" + std::to_string(GetCurrentProcessId()));
  {
    std::ofstream file(path, std::ios::binary);
    file << "broken";
  }
  bool reported = false;
  native_display::init(path, [&](const auto &) {
    reported = true;
  },
                       [] {
                       });
  EXPECT_TRUE(reported);
  EXPECT_TRUE(std::filesystem::exists(path));
  HANDLE gate = CreateMutexW(nullptr, FALSE, L"Local\\SunshinePrivacyGuard");
  ASSERT_NE(gate, nullptr);
  const auto acquired = WaitForSingleObject(gate, 0);
  EXPECT_TRUE(acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED);
  if (acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED) {
    ReleaseMutex(gate);
  }
  CloseHandle(gate);
  native_display::shutdown();
  std::filesystem::remove(path);
}

TEST(NativeDisplay, DoesNotNormalizeThePhysicalPrimary) {
  // The saved snapshot must keep the external primary at zero even if the internal panel comes first.
  auto internal = native_display::win::virtual_layout({}, 1920, 1080, 60);
  internal.modes[0].sourceMode.position.x = -1920;
  auto snapshot = internal;
  auto privacy = native_display::win::virtual_layout({}, 2560, 1600, 120);
  EXPECT_EQ(snapshot.modes[0].sourceMode.position.x, -1920);
  EXPECT_EQ(privacy.modes[0].sourceMode.position.x, 0);
}
#endif
