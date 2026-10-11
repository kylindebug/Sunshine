/** @file tests/unit/platform/windows/test_native_display.cpp
 * @brief Native display policy and CCD construction regressions; no hardware is mutated.
 */
#include "src/native_display.h"
#include "src/native_display_state.h"
#ifdef _WIN32
  #include "src/platform/windows/native_display_layout.h"

  #include <fstream>
  #include <gtest/gtest.h>

TEST(NativeDisplay, ExistingApplicationsRemainDisabled) {
  EXPECT_EQ(native_display::parse(""), native_display::policy::disabled);
  EXPECT_EQ(native_display::parse("disabled"), native_display::policy::disabled);
  EXPECT_EQ(native_display::parse("physical"), native_display::policy::physical);
  EXPECT_EQ(native_display::parse("privacy"), native_display::policy::privacy);
  EXPECT_THROW(native_display::parse("privcy"), std::invalid_argument);
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
