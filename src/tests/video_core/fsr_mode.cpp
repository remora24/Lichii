// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/fsr_mode.h"

namespace Mode = VideoCommon::FsrMode;
using Quality = Settings::FsrQuality;

TEST_CASE("FSR modes fit only the game's own ratio and fall back to Automatic", "[fsr_mode]") {
    // Ocarina docked: 2560x1440 -> 3840x2160 is AMD's Quality ratio.
    constexpr Mode::Extent render{2560, 1440};
    constexpr Mode::Extent output{3840, 2160};
    REQUIRE(Mode::Select(Quality::Quality, render, output) == Quality::Quality);
    REQUIRE(Mode::Select(Quality::Balanced, render, output) == Quality::Automatic);
    REQUIRE(Mode::Select(Quality::Performance, render, output) == Quality::Automatic);
    REQUIRE(Mode::Select(Quality::UltraPerformance, render, output) == Quality::Automatic);
    REQUIRE(Mode::Select(Quality::Automatic, render, output) == Quality::Automatic);
    // AMD truncates the render size: 1920 / 1.7 = 1129.4, 1080 / 1.7 = 635.3.
    REQUIRE(Mode::Fits(Quality::Balanced, {1129, 635}, {1920, 1080}));
    REQUIRE(Mode::Fits(Quality::Balanced, {1130, 636}, {1920, 1080}));
    REQUIRE_FALSE(Mode::Fits(Quality::Balanced, {1131, 635}, {1920, 1080}));
    REQUIRE_FALSE(Mode::Fits(Quality::Quality, {}, output));
}

TEST_CASE("FSR native AA outputs at the render size, other modes at the game's", "[fsr_mode]") {
    constexpr Mode::Extent render{2560, 1440};
    constexpr Mode::Extent output{3840, 2160};
    REQUIRE(Mode::Select(Quality::Native, render, output) == Quality::Native);
    const auto native = Mode::Output(Quality::Native, render, output);
    REQUIRE((native.width == 2560 && native.height == 1440));
    const auto quality = Mode::Output(Quality::Quality, render, output);
    REQUIRE((quality.width == 3840 && quality.height == 2160));
    const auto automatic = Mode::Output(Quality::Automatic, render, output);
    REQUIRE((automatic.width == 3840 && automatic.height == 2160));
}
