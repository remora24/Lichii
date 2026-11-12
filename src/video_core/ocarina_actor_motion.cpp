// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "video_core/ocarina_actor_motion.h"

namespace VideoCommon {
namespace {

u32 ComponentBytes(OcarinaSkinningComponent component) {
    switch (component) {
    case OcarinaSkinningComponent::UInt8:
    case OcarinaSkinningComponent::UNorm8:
        return 1;
    case OcarinaSkinningComponent::UInt16:
    case OcarinaSkinningComponent::UNorm16:
    case OcarinaSkinningComponent::Float16:
        return 2;
    case OcarinaSkinningComponent::UInt32:
    case OcarinaSkinningComponent::Float32:
        return 4;
    }
    return 0;
}

bool Fits(const OcarinaSkinningAttribute& attribute, u32 components, u32 stride) {
    const u32 bytes = ComponentBytes(attribute.component);
    return bytes != 0 && attribute.components >= components && attribute.components <= 4 &&
           attribute.offset <= stride && attribute.components * bytes <= stride - attribute.offset;
}

bool IsJointComponent(OcarinaSkinningComponent component) {
    return component == OcarinaSkinningComponent::UInt8 ||
           component == OcarinaSkinningComponent::UInt16 ||
           component == OcarinaSkinningComponent::UInt32;
}

bool IsWeightComponent(OcarinaSkinningComponent component) {
    return component == OcarinaSkinningComponent::UNorm8 ||
           component == OcarinaSkinningComponent::UNorm16 ||
           component == OcarinaSkinningComponent::Float32;
}

template <typename T>
T Load(const u8* data) {
    T result;
    std::memcpy(&result, data, sizeof(result));
    return result;
}

f32 HalfToFloat(u16 half) {
    const u32 sign = u32{half & 0x8000U} << 16;
    u32 exponent = (half >> 10) & 0x1fU;
    u32 mantissa = half & 0x3ffU;
    if (exponent == 0x1f) {
        return std::bit_cast<f32>(sign | 0x7f800000U | (mantissa << 13));
    }
    if (exponent == 0) {
        if (mantissa == 0) {
            return std::bit_cast<f32>(sign);
        }
        // Subnormal: shift the leading one out of the mantissa.
        exponent = 1;
        while ((mantissa & 0x400U) == 0) {
            mantissa <<= 1;
            --exponent;
        }
        mantissa &= 0x3ffU;
    }
    return std::bit_cast<f32>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

u32 ReadJoint(const u8* data, OcarinaSkinningComponent component) {
    switch (component) {
    case OcarinaSkinningComponent::UInt8:
        return *data;
    case OcarinaSkinningComponent::UInt16:
        return Load<u16>(data);
    case OcarinaSkinningComponent::UInt32:
        return Load<u32>(data);
    default:
        return OcarinaActorBoneCount;
    }
}

f32 ReadWeight(const u8* data, OcarinaSkinningComponent component) {
    switch (component) {
    case OcarinaSkinningComponent::UNorm8:
        return static_cast<f32>(*data) / 255.0f;
    case OcarinaSkinningComponent::UNorm16:
        return static_cast<f32>(Load<u16>(data)) / 65535.0f;
    case OcarinaSkinningComponent::Float32:
        return Load<f32>(data);
    default:
        return 0.0f;
    }
}

} // namespace

bool AppendOcarinaActorVertices(std::span<const u8> source, const OcarinaSkinningLayout& layout,
                                u32 pose_index, std::vector<OcarinaActorVertex>& destination) {
    if (layout.stride == 0 || source.size() % layout.stride != 0 || layout.influences == 0 ||
        layout.influences > 8 || layout.bone_count == 0 ||
        layout.bone_count > OcarinaActorBoneCount ||
        (layout.position.component != OcarinaSkinningComponent::Float32 &&
         layout.position.component != OcarinaSkinningComponent::Float16) ||
        !Fits(layout.position, 3, layout.stride)) {
        return false;
    }
    for (u32 group = 0; group < (layout.influences > 4 ? 2U : 1U); ++group) {
        const u32 count = std::min(layout.influences - group * 4, 4U);
        if (!IsJointComponent(layout.joints[group].component) ||
            !Fits(layout.joints[group], count, layout.stride) ||
            (layout.influences != 1 &&
             (!IsWeightComponent(layout.weights[group].component) ||
              !Fits(layout.weights[group], count, layout.stride)))) {
            return false;
        }
    }
    const size_t begin = destination.size();
    const size_t count = source.size() / layout.stride;
    if (count > destination.max_size() - begin) {
        return false;
    }
    const auto fail = [&] {
        destination.resize(begin);
        return false;
    };
    for (size_t index = 0; index < count; ++index) {
        const u8* data = source.data() + index * layout.stride;
        OcarinaActorVertex vertex{};
        if (layout.position.component == OcarinaSkinningComponent::Float16) {
            for (size_t axis = 0; axis < 3; ++axis) {
                vertex.position[axis] =
                    HalfToFloat(Load<u16>(data + layout.position.offset + axis * 2));
            }
        } else {
            std::memcpy(vertex.position.data(), data + layout.position.offset,
                        sizeof(vertex.position));
        }
        if (!std::ranges::all_of(vertex.position, [](f32 value) { return std::isfinite(value); })) {
            return fail();
        }
        vertex.pose_index = pose_index;
        bool weighted = false;
        for (u32 influence = 0; influence < layout.influences; ++influence) {
            const auto& joint_attribute = layout.joints[influence / 4];
            const auto& weight_attribute = layout.weights[influence / 4];
            const u32 component = influence % 4;
            const f32 weight = layout.influences == 1
                                   ? 1.0f
                                   : ReadWeight(data + weight_attribute.offset +
                                                    component * ComponentBytes(weight_attribute.component),
                                                weight_attribute.component);
            if (!std::isfinite(weight) || weight < 0.0f) {
                return fail();
            }
            if (weight == 0.0f) {
                continue;
            }
            const u32 joint = ReadJoint(data + joint_attribute.offset +
                                           component * ComponentBytes(joint_attribute.component),
                                       joint_attribute.component);
            if (joint >= layout.bone_count) {
                return fail();
            }
            vertex.joints[influence] = static_cast<u16>(joint);
            vertex.weights[influence] = weight;
            weighted = true;
        }
        if (!weighted) {
            return fail();
        }
        destination.push_back(vertex);
    }
    return true;
}

} // namespace VideoCommon
