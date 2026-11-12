// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "video_core/ocarina_actor_motion.h"

namespace {
using namespace VideoCommon;

template <typename T>
void Store(std::vector<u8>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

struct Fixture {
    OcarinaSkinningLayout layout;
    std::vector<u8> bytes;
    std::array<f32, 8> weights{};
};

Fixture MakeFixture(u32 influences) {
    Fixture fixture;
    auto& layout = fixture.layout;
    layout.influences = influences;
    layout.bone_count = OcarinaActorBoneCount;
    layout.position = {1, 3, OcarinaSkinningComponent::Float32};
    const u32 weight_offset = 16 + influences * static_cast<u32>(sizeof(u16)) + 3;
    layout.stride = weight_offset + influences + 5;
    layout.joints[0] = {16, std::min(influences, 4U), OcarinaSkinningComponent::UInt16};
    layout.weights[0] = {weight_offset, std::min(influences, 4U),
                         OcarinaSkinningComponent::UNorm8};
    if (influences > 4) {
        layout.joints[1] = {24, influences - 4, OcarinaSkinningComponent::UInt16};
        layout.weights[1] = {weight_offset + 4, influences - 4,
                             OcarinaSkinningComponent::UNorm8};
    }
    fixture.bytes.assign(layout.stride, 0xc7);
    Store(fixture.bytes, 1, 1.25f);
    Store(fixture.bytes, 5, -2.5f);
    Store(fixture.bytes, 9, 3.75f);
    for (u32 influence = 0; influence < influences; ++influence) {
        Store(fixture.bytes, 16 + influence * sizeof(u16), static_cast<u16>(10 + influence));
        const u8 weight = static_cast<u8>(255 / influences +
                                         (influence == influences - 1 ? 255 % influences : 0));
        fixture.bytes[weight_offset + influence] = weight;
        fixture.weights[influence] = static_cast<f32>(weight) / 255.0f;
    }
    return fixture;
}

// A synthetic pose translates bone j by (2j, -3j, j), exposing wrong joint/weight lanes.
std::array<f32, 3> Skin(const OcarinaActorVertex& vertex) {
    std::array<f32, 3> result{};
    for (size_t influence = 0; influence < 8; ++influence) {
        const f32 joint = static_cast<f32>(vertex.joints[influence]);
        result[0] += (vertex.position[0] + 2.0f * joint) * vertex.weights[influence];
        result[1] += (vertex.position[1] - 3.0f * joint) * vertex.weights[influence];
        result[2] += (vertex.position[2] + joint) * vertex.weights[influence];
    }
    return result;
}
} // namespace

TEST_CASE("Ocarina canonical skinning preserves all one-to-eight influence variants",
          "[video_core][ocarina][actor_motion]") {
    for (u32 influences = 1; influences <= 8; ++influences) {
        INFO("influences=" << influences);
        auto fixture = MakeFixture(influences);
        if (influences == 1) {
            // The one-weight program does not fetch a weight attribute.
            fixture.layout.weights = {};
        }
        std::vector<OcarinaActorVertex> vertices;
        REQUIRE(AppendOcarinaActorVertices(fixture.bytes, fixture.layout, 0, vertices));
        REQUIRE(vertices.size() == 1);
        std::array<f32, 3> expected{};
        for (u32 influence = 0; influence < influences; ++influence) {
            const f32 joint = static_cast<f32>(10 + influence);
            expected[0] += (1.25f + 2 * joint) * fixture.weights[influence];
            expected[1] += (-2.5f - 3 * joint) * fixture.weights[influence];
            expected[2] += (3.75f + joint) * fixture.weights[influence];
        }
        const auto result = Skin(vertices[0]);
        for (size_t component = 0; component < 3; ++component) {
            CHECK(result[component] == Catch::Approx(expected[component]).margin(0.00001));
        }
        for (u32 influence = influences; influence < 8; ++influence) {
            CHECK(vertices[0].weights[influence] == 0.0f);
        }
    }
}

TEST_CASE("Ocarina skinning retains narrow UNorm16 and float weights",
          "[video_core][ocarina][actor_motion]") {
    OcarinaSkinningLayout layout{
        .stride = 32,
        .influences = 2,
        .bone_count = OcarinaActorBoneCount,
        .position = {0, 3, OcarinaSkinningComponent::Float32},
        .joints = {{{12, 2, OcarinaSkinningComponent::UInt32}, {}}},
        .weights = {{{20, 2, OcarinaSkinningComponent::UNorm16}, {}}},
    };
    std::vector<u8> bytes(layout.stride);
    Store(bytes, 12, u32{279});
    Store(bytes, 16, u32{0});
    Store(bytes, 20, u16{1});
    Store(bytes, 22, u16{65534});
    std::vector<OcarinaActorVertex> vertices;
    REQUIRE(AppendOcarinaActorVertices(bytes, layout, 0, vertices));
    CHECK(Skin(vertices[0])[0] == Catch::Approx(558.0f / 65535.0f).margin(0.00000001));

    layout.weights[0] = {20, 2, OcarinaSkinningComponent::Float32};
    Store(bytes, 20, 0.1234567f);
    Store(bytes, 24, 0.8765433f);
    vertices.clear();
    REQUIRE(AppendOcarinaActorVertices(bytes, layout, 0, vertices));
    CHECK(Skin(vertices[0])[0] == Catch::Approx(558.0f * 0.1234567f).margin(0.000001));
}

TEST_CASE("Ocarina skinning ignores unused joint sentinels but rejects active invalid bones",
          "[video_core][ocarina][actor_motion]") {
    auto fixture = MakeFixture(2);
    const auto weight_offset = fixture.layout.weights[0].offset;
    fixture.bytes[weight_offset] = 255;
    fixture.bytes[weight_offset + 1] = 0;
    Store(fixture.bytes, 18, u16{65535});
    std::vector<OcarinaActorVertex> vertices;
    REQUIRE(AppendOcarinaActorVertices(fixture.bytes, fixture.layout, 0, vertices));
    const auto original = Skin(vertices[0]);
    fixture.bytes[weight_offset + 1] = 1;
    const auto first = fixture.bytes;
    fixture.bytes.insert(fixture.bytes.end(), first.begin(), first.end());
    // A valid first vertex followed by an invalid second vertex must not leak partial geometry.
    fixture.bytes[weight_offset + 1] = 0;
    CHECK_FALSE(AppendOcarinaActorVertices(fixture.bytes, fixture.layout, 1, vertices));
    REQUIRE(vertices.size() == 1);
    CHECK(Skin(vertices[0]) == original);
}

TEST_CASE("Ocarina skinning decodes eight-bit joints split across two attribute groups",
          "[video_core][ocarina][actor_motion]") {
    const std::array<u16, 5> joints{3, 17, 255, 8, 1};
    OcarinaSkinningLayout layout{
        .stride = 40,
        .influences = 5,
        .bone_count = OcarinaActorBoneCount,
        .position = {0, 3, OcarinaSkinningComponent::Float32},
        .joints = {{{16, 4, OcarinaSkinningComponent::UInt8}, {20, 1, OcarinaSkinningComponent::UInt8}}},
        .weights = {{{24, 4, OcarinaSkinningComponent::UNorm8}, {28, 1, OcarinaSkinningComponent::UNorm8}}},
    };
    std::vector<u8> bytes(layout.stride, 0);
    Store(bytes, 0, 1.5f);
    Store(bytes, 4, -0.5f);
    Store(bytes, 8, 2.25f);
    std::array<f32, 5> weights{};
    std::array<f32, 3> expected{};
    for (u32 influence = 0; influence < joints.size(); ++influence) {
        bytes[16 + influence] = static_cast<u8>(joints[influence]);
        const u8 encoded = static_cast<u8>(influence == 4 ? 153 : 51);
        bytes[24 + influence] = encoded;
        weights[influence] = static_cast<f32>(encoded) / 255.0f;
        const f32 joint = static_cast<f32>(joints[influence]);
        expected[0] += (1.5f + 2 * joint) * weights[influence];
        expected[1] += (-0.5f - 3 * joint) * weights[influence];
        expected[2] += (2.25f + joint) * weights[influence];
    }
    std::vector<OcarinaActorVertex> vertices;
    REQUIRE(AppendOcarinaActorVertices(bytes, layout, 2, vertices));
    REQUIRE(vertices.size() == 1);
    CHECK(vertices[0].pose_index == 2);
    for (u32 influence = 0; influence < joints.size(); ++influence) {
        INFO("influence=" << influence);
        CHECK(vertices[0].joints[influence] == joints[influence]);
        CHECK(vertices[0].weights[influence] == Catch::Approx(weights[influence]).margin(0.000001));
    }
    const auto result = Skin(vertices[0]);
    for (size_t component = 0; component < 3; ++component) {
        CHECK(result[component] == Catch::Approx(expected[component]).margin(0.00001));
    }
}

TEST_CASE("Ocarina skinning rejects truncated layouts and non-finite geometry atomically",
          "[video_core][ocarina][actor_motion]") {
    auto fixture = MakeFixture(5);
    std::vector<OcarinaActorVertex> vertices;
    REQUIRE(AppendOcarinaActorVertices(fixture.bytes, fixture.layout, 0, vertices));
    const auto original = Skin(vertices[0]);
    auto bad = fixture.layout;
    bad.joints[1].offset = bad.stride - 1;
    CHECK_FALSE(AppendOcarinaActorVertices(fixture.bytes, bad, 1, vertices));
    CHECK_FALSE(AppendOcarinaActorVertices(std::span<const u8>{fixture.bytes}.first(fixture.bytes.size() - 1),
                                          fixture.layout, 1, vertices));
    Store(fixture.bytes, 1, std::numeric_limits<f32>::quiet_NaN());
    CHECK_FALSE(AppendOcarinaActorVertices(fixture.bytes, fixture.layout, 1, vertices));
    REQUIRE(vertices.size() == 1);
    CHECK(Skin(vertices[0]) == original);
}

TEST_CASE("Ocarina skinning bounds active joints by the actor's captured palette",
          "[video_core][ocarina][actor_motion]") {
    auto fixture = MakeFixture(2);
    fixture.layout.bone_count = 13;
    const auto weight_offset = fixture.layout.weights[0].offset;
    fixture.bytes[weight_offset] = 255;
    fixture.bytes[weight_offset + 1] = 0;
    Store(fixture.bytes, 16, u16{12});
    Store(fixture.bytes, 18, u16{65535});
    std::vector<OcarinaActorVertex> vertices;
    REQUIRE(AppendOcarinaActorVertices(fixture.bytes, fixture.layout, 0, vertices));
    const auto original = Skin(vertices[0]);
    CHECK(original == std::array<f32, 3>{25.25f, -38.5f, 15.75f});

    Store(fixture.bytes, 16, u16{13});
    CHECK_FALSE(AppendOcarinaActorVertices(fixture.bytes, fixture.layout, 1, vertices));
    Store(fixture.bytes, 16, u16{12});
    Store(fixture.bytes, 18, u16{13});
    fixture.bytes[weight_offset + 1] = 1;
    CHECK_FALSE(AppendOcarinaActorVertices(fixture.bytes, fixture.layout, 1, vertices));
    fixture.layout.bone_count = 0;
    CHECK_FALSE(AppendOcarinaActorVertices(fixture.bytes, fixture.layout, 1, vertices));
    fixture.layout.bone_count = OcarinaActorBoneCount + 1;
    CHECK_FALSE(AppendOcarinaActorVertices(fixture.bytes, fixture.layout, 1, vertices));
    REQUIRE(vertices.size() == 1);
    CHECK(Skin(vertices[0]) == original);
}

TEST_CASE("Ocarina skinning decodes half-float positions and ignores their fourth component",
          "[video_core][ocarina][actor_motion]") {
    // The shield's layout: RGBA16F position, two 8-bit joints and weights, 24-byte stride.
    OcarinaSkinningLayout layout{
        .stride = 24,
        .influences = 2,
        .bone_count = 5,
        .position = {0, 3, OcarinaSkinningComponent::Float16},
        .joints = {{{20, 2, OcarinaSkinningComponent::UInt8}, {}}},
        .weights = {{{22, 2, OcarinaSkinningComponent::UNorm8}, {}}},
    };
    std::vector<u8> bytes(layout.stride);
    Store(bytes, 0, u16{0x3e00}); // 1.5
    Store(bytes, 2, u16{0xc100}); // -2.5
    Store(bytes, 4, u16{0x0001}); // smallest subnormal, 2^-24
    Store(bytes, 6, u16{0x7c00}); // infinity in the unused fourth component
    bytes[20] = 4;
    bytes[22] = 255;
    std::vector<OcarinaActorVertex> vertices;
    REQUIRE(AppendOcarinaActorVertices(bytes, layout, 0, vertices));
    CHECK(vertices[0].position == std::array<f32, 3>{1.5f, -2.5f, 0x1p-24f});
    CHECK(vertices[0].joints[0] == 4);

    Store(bytes, 2, u16{0x7e00}); // NaN position
    CHECK_FALSE(AppendOcarinaActorVertices(bytes, layout, 0, vertices));
    Store(bytes, 2, u16{0xfc00}); // -infinity position
    CHECK_FALSE(AppendOcarinaActorVertices(bytes, layout, 0, vertices));
    layout.position.offset = 19; // six bytes no longer fit
    CHECK_FALSE(AppendOcarinaActorVertices(bytes, layout, 0, vertices));
    REQUIRE(vertices.size() == 1);
}
