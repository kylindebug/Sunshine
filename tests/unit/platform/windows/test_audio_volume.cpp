/**
 * @file tests/unit/platform/windows/test_audio_volume.cpp
 * @brief Regression checks for master mute, queued audio and exactly-once attenuation.
 */
#include "src/audio_silence.h"
#include "src/platform/windows/audio_volume_policy.h"

#include <array>
#include <atomic>
#include <gtest/gtest.h>
#include <limits>

using namespace platf::audio::volume_policy;

TEST(AudioVolume, ZeroSliderMustSilenceEvenWithoutMasterMute) {
  EXPECT_TRUE(silence_required(true, false, 0.0f));
  EXPECT_TRUE(silence_required(true, true, 0.5f));
  EXPECT_TRUE(silence_required(false, false, 1.0f));
  EXPECT_TRUE(silence_required(true, false, std::numeric_limits<float>::quiet_NaN()));
  EXPECT_FALSE(silence_required(true, false, 0.5f));
}

TEST(AudioVolume, AlreadyRestoredRolesStillRequireFinalLogicalVolumeTransfer) {
  EXPECT_FALSE(skip_transition(true, true));
  EXPECT_FALSE(skip_transition(true, false));
  EXPECT_FALSE(skip_transition(false, false));
  EXPECT_TRUE(skip_transition(false, true));
}

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

TEST(AudioVolume, OrdinaryLoopbackMustNotApplyMasterAttenuationTwice) {
  std::array<float, 2> samples {0.5f, -0.5f};
  apply(samples, {true, false, -20.0f}, false);
  EXPECT_EQ(samples, (std::array<float, 2> {0.5f, -0.5f}));
  EXPECT_EQ(gain({true, false, 6.0f}, false), 1.0f);
}

TEST(AudioVolume, UnmuteAppliesTheLatestLevel) {
  EXPECT_EQ(gain({true, true, -6.0f}, false), 0.0f);
  EXPECT_EQ(gain({true, false, -6.0f}, false), 1.0f);
  EXPECT_EQ(gain({true, false, 0.0f}, false), 1.0f);
}

TEST(AudioVolume, QueuedFramesObserveMuteAtEncodingRatherThanCapture) {
  std::array<std::array<float, 2>, 3> queued {{{0.5f, -0.5f}, {0.4f, -0.4f}, {0.3f, -0.3f}}};
  std::atomic_bool muted {false};
  auto check = [&] {
    return muted.load();
  };
  audio::silence_if_needed(queued[0], check);
  EXPECT_EQ(queued[0][0], 0.5f);
  muted.store(true);
  for (int i = 1; i < 3; ++i) {
    audio::silence_if_needed(queued[i], check);
    EXPECT_EQ(queued[i], (std::array<float, 2> {}));
  }
}

TEST(AudioVolume, FailedRoleRollbackCannotRestoreAnUnmutedDestination) {
  int restored_roles {};
  bool destination_muted {false};
  const bool result = rollback(3, [&](int role) {
    ++restored_roles;
    return role != 1;
  },
                               [&](bool safe_to_unmute) {
                                 destination_muted = !safe_to_unmute;
                               });
  EXPECT_FALSE(result);
  EXPECT_EQ(restored_roles, 3);
  EXPECT_TRUE(destination_muted);
}

TEST(AudioVolume, SuccessfulRoleRollbackAllowsPreviousControls) {
  int restored_roles {};
  bool controls_restored {};
  EXPECT_TRUE(rollback(3, [&](int) {
    ++restored_roles;
    return true;
  },
                       [&](bool safe) {
                         controls_restored = safe;
                       }));
  EXPECT_EQ(restored_roles, 3);
  EXPECT_TRUE(controls_restored);
}
