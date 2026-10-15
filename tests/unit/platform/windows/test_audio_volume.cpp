/**
 * @file tests/unit/platform/windows/test_audio_volume.cpp
 * @brief Regression checks for master mute, queued audio and exactly-once attenuation.
 */
#include "src/platform/windows/audio_volume_policy.h"

#include <array>
#include <gtest/gtest.h>
#include <limits>

using namespace platf::audio::volume_policy;

TEST(AudioVolume, MuteClearsPreviouslyBufferedNonzeroAudio) {
  for (bool post : {false, true}) {
    std::array<float, 4> samples {0.6f, -0.5f, 1.0f, -1.0f};
    apply(samples, {true, true, 0.0f}, post);
    EXPECT_EQ(samples, (std::array<float, 4> {}));
  }
}

TEST(AudioVolume, UnknownAndNonfiniteControlsRemainSilent) {
  EXPECT_EQ(gain({}, true), 0.0f);
  EXPECT_EQ(gain({true, false, std::numeric_limits<float>::quiet_NaN()}, false), 0.0f);
  EXPECT_EQ(gain({true, false, std::numeric_limits<float>::infinity()}, true), 0.0f);
}

TEST(AudioVolume, PostVolumePacketsAreNotAttenuatedTwice) {
  std::array<float, 2> samples {0.5f, -0.5f};
  apply(samples, {true, false, -20.0f}, true);
  EXPECT_EQ(samples, (std::array<float, 2> {0.5f, -0.5f}));
}

TEST(AudioVolume, PreVolumeUsesDecibelsRatherThanSliderFraction) {
  std::array<float, 2> samples {0.5f, -0.5f};
  apply(samples, {true, false, -20.0f}, false);
  EXPECT_NEAR(samples[0], 0.05f, 0.00001f);
  EXPECT_NEAR(samples[1], -0.05f, 0.00001f);
  EXPECT_EQ(gain({true, false, 6.0f}, false), 1.0f);
}

TEST(AudioVolume, UnmuteAppliesTheLatestLevel) {
  EXPECT_EQ(gain({true, true, -6.0f}, false), 0.0f);
  EXPECT_NEAR(gain({true, false, -6.0f}, false), 0.501187f, 0.00001f);
  EXPECT_EQ(gain({true, false, 0.0f}, false), 1.0f);
}
