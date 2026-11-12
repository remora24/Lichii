// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "video_core/ocarina_actor_motion.h"

namespace VideoCommon {

struct OcarinaActorGeometryKey {
    const void* channel{};
    u64 mapping_serial{};
    u32 instance_tag{};
    GPUVAddr vertices{};
    GPUVAddr indices{};
    s32 base_vertex{};
    u32 index_count{};
    u32 index_bytes{};
    OcarinaSkinningLayout layout;

    bool operator==(const OcarinaActorGeometryKey&) const = default;
};

// Only the preceding frame survives. Swapping the two slots keeps their high-water capacities.
class OcarinaActorHistory {
public:
    struct Entry {
        OcarinaActorGeometryKey key;
        u32 pose{};
        std::optional<u32> previous_pose;
        bool ambiguous{};
    };

    enum class Result { New, Duplicate, Ambiguous };

    struct Observation {
        size_t index;
        Result result;
    };

    void BeginFrame(u64 frame) {
        if (current_frame == frame) {
            return;
        }
        const bool consecutive = current_frame && *current_frame + 1 == frame;
        previous_entries.swap(entries);
        previous_poses.swap(poses);
        entries.clear();
        poses.clear();
        if (!consecutive) {
            previous_entries.clear();
            previous_poses.clear();
        }
        current_frame = frame;
    }

    // Compare the live bytes before reusing a pose: a guest may rewrite the same binding address.
    u32 FindOrAddPose(std::span<const u8, OcarinaActorBoneCount * 12 * sizeof(f32)> bones,
                      const std::array<f32, 4>& origin) {
        for (u32 index = 0; index < poses.size(); ++index) {
            if (poses[index].origin == origin &&
                std::memcmp(poses[index].bones.data(), bones.data(), bones.size()) == 0) {
                return index;
            }
        }
        const u32 index = static_cast<u32>(poses.size());
        auto& pose = poses.emplace_back();
        std::memcpy(pose.bones.data(), bones.data(), bones.size());
        pose.origin = origin;
        return index;
    }

    Observation Observe(const OcarinaActorGeometryKey& key, u32 pose) {
        for (size_t index = 0; index < entries.size(); ++index) {
            auto& entry = entries[index];
            if (entry.key != key) {
                continue;
            }
            if (entry.pose != pose || entry.ambiguous) {
                entry.ambiguous = true;
                return {index, Result::Ambiguous};
            }
            return {index, Result::Duplicate};
        }
        std::optional<u32> previous;
        for (const auto& entry : previous_entries) {
            if (entry.key == key && !entry.ambiguous) {
                previous = entry.pose;
                break;
            }
        }
        const size_t index = entries.size();
        entries.push_back({key, pose, previous, false});
        return {index, Result::New};
    }

    void Reject(size_t index) {
        entries[index].ambiguous = true;
    }

    const std::vector<Entry>& Entries() const {
        return entries;
    }

    const std::vector<OcarinaActorPose>& Poses() const {
        return poses;
    }

    const std::vector<OcarinaActorPose>& PreviousPoses() const {
        return previous_poses;
    }

private:
    std::optional<u64> current_frame;
    std::vector<Entry> entries;
    std::vector<Entry> previous_entries;
    std::vector<OcarinaActorPose> poses;
    std::vector<OcarinaActorPose> previous_poses;
};

} // namespace VideoCommon
