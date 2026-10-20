/**
 * @file tests/unit/platform/windows/test_mouse_keys.cpp
 * @brief Regressions for mouse-less disconnects, reconnects and Mouse Keys ownership.
 */
#ifdef _WIN32
  #include "src/platform/windows/mouse_keys_guard.h"

  #include <gtest/gtest.h>

namespace {
  using namespace platf::cursor;

  /** @brief Model physical mouse availability independently of the synthetic Mouse Keys mouse. */
  class MouseKeysTest: public testing::Test {
  protected:
    MOUSEKEYS settings {sizeof(MOUSEKEYS), MKF_AVAILABLE | MKF_HOTKEYACTIVE | MKF_MODIFIERS, 71, 432, 9, 0, 0};  ///< User preferences.
    MOUSEKEYS original {settings};  ///< Exact starting preferences.
    bool hardware {false};  ///< Physical mouse availability.
    bool read_ok {true};  ///< Inject a read failure.
    int failed_write {0};  ///< Fail this numbered write, or none when zero.
    int writes {0};  ///< Number of attempted writes.
    mouse_keys_guard_t guard;  ///< Policy under test.
    mouse_keys_api_t api {
      [&] {
        return hardware || (settings.dwFlags & MKF_MOUSEKEYSON);
      },
      [&](MOUSEKEYS &value) {
        value = settings;
        return read_ok;
      },
      [&](const MOUSEKEYS &value) {
        if (++writes == failed_write) {
          return false;
        }
        settings = value;
        return true;
      }
    };  ///< Deterministic settings API with failure injection.

    /** @brief Verify that all settings, including hotkeys and speeds, match the initial snapshot. */
    void expect_original() {
      EXPECT_EQ(settings.dwFlags, original.dwFlags);
      EXPECT_EQ(settings.iMaxSpeed, original.iMaxSpeed);
      EXPECT_EQ(settings.iTimeToMaxSpeed, original.iTimeToMaxSpeed);
      EXPECT_EQ(settings.iCtrlSpeed, original.iCtrlSpeed);
    }
  };

  TEST_F(MouseKeysTest, DisconnectWithoutHardwareRetainsVisibleCursor) {
    EXPECT_EQ(guard.ensure(api), result_e::enabled);
    EXPECT_EQ(guard.restore(api), result_e::deferred);
    EXPECT_TRUE(guard.deferred());
    EXPECT_TRUE(api.mouse_present());
    EXPECT_EQ(settings.dwFlags, original.dwFlags | MKF_MOUSEKEYSON);
    EXPECT_EQ(settings.iMaxSpeed, original.iMaxSpeed);
    EXPECT_EQ(settings.iTimeToMaxSpeed, original.iTimeToMaxSpeed);
    EXPECT_EQ(settings.iCtrlSpeed, original.iCtrlSpeed);
  }

  TEST_F(MouseKeysTest, ManyFramesDoNotOverwriteTheOriginalSnapshot) {
    guard.ensure(api);
    for (int i = 0; i < 100; ++i) {
      EXPECT_EQ(guard.ensure(api), result_e::unchanged);
    }
    EXPECT_EQ(writes, 1);
    hardware = true;
    EXPECT_EQ(guard.restore(api), result_e::restored);
    expect_original();
  }

  TEST_F(MouseKeysTest, RepeatedReconnectsRetainTheFirstDisabledSnapshot) {
    for (int i = 0; i < 4; ++i) {
      guard.begin_session();
      guard.ensure(api);
      EXPECT_EQ(guard.restore(api), result_e::deferred);
    }
    hardware = true;
    EXPECT_EQ(guard.restore(api), result_e::restored);
    expect_original();
    EXPECT_FALSE(guard.deferred());
  }

  TEST_F(MouseKeysTest, HardwareMouseNeedsNoSettingsChange) {
    hardware = true;
    EXPECT_EQ(guard.ensure(api), result_e::unchanged);
    EXPECT_EQ(guard.restore(api), result_e::unchanged);
    EXPECT_EQ(guard.observe(api), result_e::unchanged);
    EXPECT_EQ(writes, 0);
  }

  TEST_F(MouseKeysTest, OriginallyEnabledMouseKeysAreNeverTakenOver) {
    settings.dwFlags |= MKF_MOUSEKEYSON;
    EXPECT_EQ(guard.ensure(api), result_e::unchanged);
    EXPECT_EQ(guard.restore(api), result_e::unchanged);
    EXPECT_TRUE(settings.dwFlags & MKF_MOUSEKEYSON);
    EXPECT_EQ(writes, 0);
  }

  TEST_F(MouseKeysTest, HotUnplugDuringStreamingEnablesFallback) {
    hardware = true;
    guard.begin_session();
    guard.ensure(api);
    hardware = false;
    EXPECT_EQ(guard.ensure(api), result_e::enabled);
    EXPECT_EQ(guard.restore(api), result_e::deferred);
    EXPECT_TRUE(api.mouse_present());
  }

  TEST_F(MouseKeysTest, LocalMouseReturnRestoresAllPreferences) {
    guard.ensure(api);
    guard.restore(api);
    hardware = true;
    EXPECT_EQ(guard.observe(api), result_e::unchanged);
    EXPECT_EQ(guard.restore(api), result_e::restored);
    EXPECT_EQ(guard.restore(api), result_e::unchanged);
    expect_original();
  }

  TEST_F(MouseKeysTest, UserSpeedChangeIsNotOverwritten) {
    guard.ensure(api);
    settings.iMaxSpeed = 99;
    EXPECT_EQ(guard.observe(api), result_e::user_changed);
    EXPECT_EQ(guard.restore(api), result_e::unchanged);
    EXPECT_EQ(settings.iMaxSpeed, 99);
    EXPECT_TRUE(settings.dwFlags & MKF_MOUSEKEYSON);
  }

  TEST_F(MouseKeysTest, UserDisabledFallbackStaysDisabledForTheSession) {
    guard.ensure(api);
    settings.dwFlags &= ~MKF_MOUSEKEYSON;
    EXPECT_EQ(guard.ensure(api), result_e::user_changed);
    EXPECT_EQ(guard.ensure(api), result_e::unchanged);
    EXPECT_EQ(guard.restore(api), result_e::unchanged);
    EXPECT_FALSE(api.mouse_present());
    guard.begin_session();
    EXPECT_EQ(guard.ensure(api), result_e::enabled);
  }

  TEST_F(MouseKeysTest, UserChangeDuringIdleRelinquishesDeferredOwnership) {
    guard.ensure(api);
    guard.restore(api);
    settings.dwFlags &= ~MKF_HOTKEYACTIVE;
    EXPECT_EQ(guard.restore(api), result_e::user_changed);
    EXPECT_FALSE(guard.deferred());
    EXPECT_FALSE(guard.needs_retry());
    EXPECT_FALSE(settings.dwFlags & MKF_HOTKEYACTIVE);
  }

  TEST_F(MouseKeysTest, FailedInitialReadDoesNotChangeSettings) {
    read_ok = false;
    EXPECT_EQ(guard.ensure(api), result_e::read_failed);
    EXPECT_EQ(writes, 0);
    EXPECT_EQ(guard.restore(api), result_e::unchanged);
  }

  TEST_F(MouseKeysTest, FailedInitialWriteDoesNotAcquireOwnership) {
    failed_write = 1;
    EXPECT_EQ(guard.ensure(api), result_e::write_failed);
    EXPECT_EQ(guard.restore(api), result_e::unchanged);
    expect_original();
    EXPECT_EQ(guard.ensure(api), result_e::enabled);
  }

  TEST_F(MouseKeysTest, FailedRestoreReadRetainsOwnershipForRetry) {
    guard.ensure(api);
    read_ok = false;
    EXPECT_EQ(guard.restore(api), result_e::read_failed);
    EXPECT_TRUE(guard.needs_retry());
    EXPECT_TRUE(api.mouse_present());
    read_ok = true;
    hardware = true;
    EXPECT_EQ(guard.restore(api), result_e::restored);
    expect_original();
  }

  TEST_F(MouseKeysTest, FailedRestoreWriteRetainsVisibleFallbackAndSnapshot) {
    guard.ensure(api);
    failed_write = 2;
    EXPECT_EQ(guard.restore(api), result_e::write_failed);
    EXPECT_TRUE(guard.needs_retry());
    EXPECT_TRUE(api.mouse_present());
    hardware = true;
    EXPECT_EQ(guard.restore(api), result_e::restored);
    expect_original();
  }

  TEST_F(MouseKeysTest, FailedFallbackReapplyCanBeRetried) {
    guard.ensure(api);
    failed_write = 3;
    EXPECT_EQ(guard.restore(api), result_e::write_failed);
    EXPECT_TRUE(guard.deferred());
    EXPECT_TRUE(guard.needs_retry());
    EXPECT_EQ(guard.restore(api), result_e::deferred);
    EXPECT_TRUE(api.mouse_present());
    EXPECT_FALSE(guard.needs_retry());
  }

  TEST_F(MouseKeysTest, NewStreamRecoversAnUnsuccessfulFallbackWrite) {
    guard.ensure(api);
    failed_write = 3;
    guard.restore(api);
    guard.begin_session();
    EXPECT_EQ(guard.ensure(api), result_e::enabled);
    hardware = true;
    EXPECT_EQ(guard.restore(api), result_e::restored);
    expect_original();
  }

  TEST_F(MouseKeysTest, FailedObservationDoesNotDiscardOriginalSettings) {
    guard.ensure(api);
    read_ok = false;
    EXPECT_EQ(guard.observe(api), result_e::read_failed);
    read_ok = true;
    hardware = true;
    EXPECT_EQ(guard.restore(api), result_e::restored);
    expect_original();
  }

  TEST_F(MouseKeysTest, RuntimeButtonSelectionIsNotMistakenForAUserPreferenceChange) {
    guard.ensure(api);
    settings.dwFlags |= MKF_LEFTBUTTONSEL | MKF_LEFTBUTTONDOWN;
    EXPECT_EQ(guard.observe(api), result_e::unchanged);
    hardware = true;
    EXPECT_EQ(guard.restore(api), result_e::restored);
    expect_original();
  }
}  // namespace
#endif
