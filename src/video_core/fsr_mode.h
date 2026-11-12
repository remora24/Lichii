// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace VideoCommon::FsrMode {

using Quality = Settings::FsrQuality;

struct Extent {
    u32 width;
    u32 height;
};

/// AMD's upscale ratio of a quality mode (FidelityFX SDK), 1 for native AA, 0 for Automatic.
constexpr float Ratio(Quality mode) {
    switch (mode) {
    case Quality::Native:
        return 1.0f;
    case Quality::Quality:
        return 1.5f;
    case Quality::Balanced:
        return 1.7f;
    case Quality::Performance:
        return 2.0f;
    case Quality::UltraPerformance:
        return 3.0f;
    case Quality::Automatic:
        break;
    }
    return 0.0f;
}

/// Whether a mode applies to the game's render and output sizes. Like DLSS, FSR can't change the
/// game's render size: native AA outputs at the render size, and an explicit upscaling mode only
/// applies when the game's own ratio is that mode's (AMD's render size for the output, within a
/// pixel, as ffxQueryDescUpscaleGetRenderResolutionFromQualityMode truncates).
constexpr bool Fits(Quality mode, Extent render, Extent output) {
    if (render.width == 0 || render.height == 0 || output.width == 0 || output.height == 0) {
        return false;
    }
    if (mode == Quality::Automatic || mode == Quality::Native) {
        return true;
    }
    const float ratio = Ratio(mode);
    const auto near = [ratio](u32 rendered, u32 shown) {
        const u32 expected = static_cast<u32>(static_cast<float>(shown) / ratio);
        return rendered + 1 >= expected && rendered <= expected + 1;
    };
    return near(render.width, output.width) && near(render.height, output.height);
}

/// The mode FSR runs in: the requested one where it fits, otherwise Automatic.
constexpr Quality Select(Quality requested, Extent render, Extent output) {
    return Fits(requested, render, output) ? requested : Quality::Automatic;
}

/// FSR's output size for the game's render and output sizes: the render size in native AA,
/// otherwise the game's output.
constexpr Extent Output(Quality mode, Extent render, Extent output) {
    return mode == Quality::Native ? render : output;
}

} // namespace VideoCommon::FsrMode
