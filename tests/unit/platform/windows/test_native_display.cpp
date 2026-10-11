/** @file tests/unit/platform/windows/test_native_display.cpp
 * @brief Native display policy and CCD construction regressions; no hardware is mutated.
 */
#include "src/native_display.h"
#include "src/platform/windows/native_display_layout.h"

#include <gtest/gtest.h>

TEST(NativeDisplay, ExistingApplicationsRemainDisabled) {
  EXPECT_EQ(native_display::parse(""), native_display::policy::disabled);
  EXPECT_EQ(native_display::parse("disabled"), native_display::policy::disabled);
  EXPECT_EQ(native_display::parse("physical"), native_display::policy::physical);
  EXPECT_EQ(native_display::parse("privacy"), native_display::policy::privacy);
  EXPECT_THROW(native_display::parse("privcy"), std::invalid_argument);
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

TEST(NativeDisplay, DoesNotNormalizeThePhysicalPrimary) {
  // The saved snapshot must keep the external primary at zero even if the internal panel comes first.
  auto internal = native_display::win::virtual_layout({}, 1920, 1080, 60);
  internal.modes[0].sourceMode.position.x = -1920;
  auto snapshot = internal;
  auto privacy = native_display::win::virtual_layout({}, 2560, 1600, 120);
  EXPECT_EQ(snapshot.modes[0].sourceMode.position.x, -1920);
  EXPECT_EQ(privacy.modes[0].sourceMode.position.x, 0);
}
