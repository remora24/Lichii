// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

inline constexpr u32 OcarinaActorBoneCount = 280;

// Canonical interleaved input shared by every supported skinning variant.
struct OcarinaActorVertex {
    std::array<f32, 3> position;
    std::array<u16, 8> joints;
    std::array<f32, 8> weights;
    u32 pose_index;
};
static_assert(sizeof(OcarinaActorVertex) == 64);
static_assert(offsetof(OcarinaActorVertex, joints) == 12);
static_assert(offsetof(OcarinaActorVertex, weights) == 28);
static_assert(offsetof(OcarinaActorVertex, pose_index) == 60);

struct alignas(16) OcarinaActorPose {
    std::array<f32, OcarinaActorBoneCount * 12> bones;
    std::array<f32, 4> origin;
};
static_assert(sizeof(OcarinaActorPose) == 13456);

/// Contract trace only: what one replayed mesh is, for the overlay's depth-match probe.
struct OcarinaActorMeshInfo {
    u32 first_index{};
    u32 index_count{};
    u32 vertex_count{};
    u64 shader_hash{};
    u32 influences{};
    u32 bone_count{};
    u32 instance_tag{};
    std::array<f32, 3> low{};  ///< skinned bounds, relative to the actor origin
    std::array<f32, 3> high{};
};

struct OcarinaActorMotion {
    std::vector<OcarinaActorVertex> vertices;
    std::vector<u32> indices;
    /// Changes whenever vertices or indices do, so an uploaded copy can be kept while it doesn't.
    u64 geometry_serial{};
    std::vector<OcarinaActorPose> current_poses;
    std::vector<OcarinaActorPose> previous_poses;
    std::vector<OcarinaActorMeshInfo> meshes; ///< empty unless the contract trace is on
    u32 mesh_count{};

    /// Drops the frame's poses. The geometry stays for the next frame's batch to reuse; without
    /// poses nothing replays it.
    void ClearPoses() {
        current_poses.clear();
        previous_poses.clear();
        meshes.clear();
        mesh_count = 0;
    }
};

enum class OcarinaSkinningComponent : u8 {
    UInt8,
    UInt16,
    UInt32,
    UNorm8,
    UNorm16,
    Float32,
    Float16, ///< positions only
};

struct OcarinaSkinningAttribute {
    u32 offset{};
    u32 components{};
    OcarinaSkinningComponent component{};

    bool operator==(const OcarinaSkinningAttribute&) const = default;
};

struct OcarinaSkinningLayout {
    u32 stride{};
    u32 influences{};
    u32 bone_count{};
    OcarinaSkinningAttribute position;
    std::array<OcarinaSkinningAttribute, 2> joints;
    std::array<OcarinaSkinningAttribute, 2> weights;

    bool operator==(const OcarinaSkinningLayout&) const = default;
};

// On failure the destination remains unchanged. A one-influence shader has an implicit unit weight.
[[nodiscard]] bool AppendOcarinaActorVertices(std::span<const u8> source,
                                             const OcarinaSkinningLayout& layout, u32 pose_index,
                                             std::vector<OcarinaActorVertex>& destination);

} // namespace VideoCommon
