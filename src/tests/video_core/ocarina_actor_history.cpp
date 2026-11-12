// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "video_core/ocarina_actor_history.h"

namespace {

using namespace VideoCommon;

constexpr std::size_t BoneBytes = OcarinaActorBoneCount * 12 * sizeof(f32);

const int channel_a{};
const int channel_b{};

const std::array<f32, 4> origin{};

OcarinaActorGeometryKey MakeKey(const void* channel, GPUVAddr vertices) {
    OcarinaActorGeometryKey key;
    key.channel = channel;
    key.vertices = vertices;
    key.index_count = 3;
    key.index_bytes = 2;
    return key;
}

std::array<u8, BoneBytes> MakeBones(u8 tag) {
    std::array<u8, BoneBytes> bones{};
    bones.fill(tag);
    return bones;
}

} // namespace

TEST_CASE("Ocarina actor history keeps one pose per geometry and locks in ambiguity",
          "[video_core][ocarina][actor_history]") {
    const auto key = MakeKey(&channel_a, 0x1000);
    const auto bones_a = MakeBones(1);
    const auto bones_b = MakeBones(2);
    std::array<u8, BoneBytes> rewritten_a = bones_a;
    rewritten_a[BoneBytes - 1] = 0x7f;

    OcarinaActorHistory history;
    history.BeginFrame(1);
    const u32 pose_a = history.FindOrAddPose(bones_a, origin);
    CHECK(history.FindOrAddPose(bones_a, origin) == pose_a);
    // The guest can rewrite the same binding address; identical addresses are not identical poses.
    CHECK(history.FindOrAddPose(rewritten_a, origin) != pose_a);
    const u32 pose_b = history.FindOrAddPose(bones_b, origin);
    CHECK(pose_b != pose_a);

    const auto first = history.Observe(key, pose_a);
    CHECK(first.result == OcarinaActorHistory::Result::New);
    CHECK(history.Observe(key, pose_a).result == OcarinaActorHistory::Result::Duplicate);
    CHECK(history.Observe(key, pose_b).result == OcarinaActorHistory::Result::Ambiguous);
    // A third draw with the original pose must not resurrect the ambiguous geometry.
    const auto third = history.Observe(key, pose_a);
    CHECK(third.result == OcarinaActorHistory::Result::Ambiguous);
    CHECK(third.index == first.index);
    REQUIRE(history.Entries().size() == 1);
    CHECK(history.Entries()[first.index].ambiguous);
    CHECK_FALSE(history.Entries()[first.index].previous_pose.has_value());
}

TEST_CASE("Ocarina actor history only follows the immediately preceding frame",
          "[video_core][ocarina][actor_history]") {
    const auto key = MakeKey(&channel_a, 0x2000);
    const auto other_channel_key = MakeKey(&channel_b, 0x2000);
    const auto bones = MakeBones(3);

    OcarinaActorHistory history;
    history.BeginFrame(10);
    const u32 pose = history.FindOrAddPose(bones, origin);
    REQUIRE(history.Observe(key, pose).result == OcarinaActorHistory::Result::New);

    history.BeginFrame(11);
    const auto second = history.Observe(key, pose);
    CHECK(second.result == OcarinaActorHistory::Result::New);
    REQUIRE(history.Entries()[second.index].previous_pose.has_value());
    CHECK(*history.Entries()[second.index].previous_pose == pose);

    // A new channel drawing the same geometry has no history of its own.
    const auto other_channel = history.Observe(other_channel_key, pose);
    CHECK(other_channel.result == OcarinaActorHistory::Result::New);
    CHECK_FALSE(history.Entries()[other_channel.index].previous_pose.has_value());

    // Repeating the frame number must not discard the frame that is still being replayed.
    const size_t entries_before = history.Entries().size();
    const size_t poses_before = history.Poses().size();
    history.BeginFrame(11);
    CHECK(history.Entries().size() == entries_before);
    CHECK(history.Poses().size() == poses_before);

    // A skipped frame is not a predecessor: the geometry restarts without motion.
    history.BeginFrame(13);
    const auto skipped = history.Observe(key, pose);
    CHECK(skipped.result == OcarinaActorHistory::Result::New);
    CHECK_FALSE(history.Entries()[skipped.index].previous_pose.has_value());
}

TEST_CASE("Ocarina actor history drops a rejected geometry from every later frame",
          "[video_core][ocarina][actor_history]") {
    const auto key = MakeKey(&channel_a, 0x3000);
    const auto bones = MakeBones(4);

    OcarinaActorHistory history;
    history.BeginFrame(1);
    const u32 pose = history.FindOrAddPose(bones, origin);
    const auto observed = history.Observe(key, pose);
    REQUIRE(observed.result == OcarinaActorHistory::Result::New);
    history.Reject(observed.index);
    CHECK(history.Observe(key, pose).result == OcarinaActorHistory::Result::Ambiguous);

    history.BeginFrame(2);
    const auto next = history.Observe(key, pose);
    CHECK(next.result == OcarinaActorHistory::Result::New);
    CHECK_FALSE(history.Entries()[next.index].previous_pose.has_value());
}
