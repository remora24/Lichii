# Native Linux FSR 4.1.2 — BeamNG-derived Vulkan backend

This development branch integrates the **INT8 FSR 4.1.2** Vulkan runtime from
[Mob1K7105/beamng-fsr4-linux](https://github.com/Mob1K7105/beamng-fsr4-linux)
into Lichii's Ocarina of Time upscaling path. It does not use AMD's Windows
FidelityFX DLLs, Wine, or Proton.

**Status: experimental, awaiting native build and on-device image validation.**
The option is OFF by default. The regular Windows AMD FSR 4 integration and
default Linux FSR 3.1 path are unchanged. Do not merge or release as stable
without a successful native build and actual Radeon test.

## Build on Arch Linux / CachyOS (fish shell)

First ensure the build dependencies for Lichii are installed, including CMake
3.31+, Ninja, GCC/Clang, glslangValidator, Vulkan headers/loader, Qt6,
and the multimedia libraries listed in Lichii's build documentation.

Keep the BeamNG source tree pinned to **v0.2.1** (commit
`10791f325b66bff7413577c65c5aa7dfb`), the version used by this adapter:

```fish
git clone https://github.com/remora24/Lichii.git
cd Lichii
git switch feature/linux-fsr412-beamng
cd ..
git clone --branch v0.2.1 --depth 1 https://github.com/Mob1K7105/beamng-fsr4-linux.git
git -C beamng-fsr4-linux rev-parse HEAD
cd Lichii
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LICHII_LINUX_FSR412=ON \
    -DLICHII_BEAMNG_FSR412_ROOT=(realpath ../beamng-fsr4-linux)
cmake --build build --target yuzu --parallel (nproc)
```

The binary is `build/bin/lichii`. The full release build has additional
optional CMake switches; the dedicated CI workflow uses the same native
backend flag and an explicit Qt configuration.

If the build configuration can't find the external FSR source, check the
`LICHII_BEAMNG_FSR412_ROOT` path. The source and assets are *not* copied
into Lichii.

## Enable for testing

The source checkout's `assets/` directory contains the INT8 shader/model
sets. In a **fish** shell, from inside the Lichii checkout:

```fish
set -x LICHII_FSR412_ASSETS_DIR (realpath ../beamng-fsr4-linux/assets)
./build/bin/lichii
```

In Lichii's Graphics settings, choose the AMD RADV Vulkan device, then check:

- **Use AMD FSR 3.1 upscaling in Ocarina of Time**
- **Use native FSR 4.1.2 (Linux, experimental)**

Start or restart Ocarina after applying the settings. No official AMD DLL
download is necessary. The checkbox appears only in opt-in Linux builds
and on AMD RADV devices.

Alternatively, for command-line diagnostics, set
`LICHII_FSR412_EXPERIMENTAL=1` in the environment before starting the
app; this bypasses the FSR 4 checkbox but still requires Ocarina's FSR
replacement to be selected. To force the FSR 3.1 comparison, unset it and
uncheck the FSR 4 setting.

## What this branch implements

- Native Linux-only selection of the external
  `Fsr411::Upscaler::Record()` backend. The original Windows official AMD
  FFX runtime path is preserved.
- An Ocarina D32 standard-depth **to reversed-Z R32F** compute pass. FSR 4.1.2
  gets the converted depth, not the unconverted scene depth.
- Lichii's tonemapped RGBA16F scene color, RG16F motion vectors in render
  pixels, reconstructed camera jitter, reset and sharpening are forwarded
  to the BeamNG 4.1.2 backend.
- Cached input Vulkan image views, an RGBA16F output image and barriers
  around conversion, upscaling, and presentation.
- Required compute-derivative device feature negotiation on AMD for the
  opt-in build, and runtime checks for INT8/dot-product capabilities.
- Output-format/size and asset validation with fallback to FSR 3.1.
- A temporary `scheduler.Finish()` after each evaluation to protect the
  BeamNG backend's host-mapped eight-slot uniform-buffer ring and guarantee
  failures are detected before handing the output to presentation.

**Performance results with this interim per-frame wait must not be used as
a final comparison to FSR 3.1.** The GPU timeline synchronization still
needs to be optimized once correctness is established.

## Important remaining validation

- **Build:** the dedicated native-backend GitHub Actions workflow must
  compile cleanly with GCC 13 and `ENABLE_WERROR=ON`.
- **GPU:** test on Radeon RX 7800 XT/RADV with validation layers, confirming
  that required Vulkan features are present and that the FSR 4.1.2 shader
  variant is correctly selected from the external asset sets.
- **Depth and motion:** compare moving camera/actors, occlusion transitions,
  thin features and previous-frame history to the known-good FSR 4.1.1
  Vulkan implementation. The converted R32F view must be compatible with
  the translated shader's sampled depth descriptor.
- **Failed frame / resizing:** test missing assets, unsupported size, game
  restart, live resolution changes, sampler image replacement and fallback.
- **Quality:** BeamNG's public `Fsr411::Frame` does not expose a reactive
  mask. Lichii's existing effects bias/reactive mask is not supplied to
  that backend. Inspect for trails around particles and effects.

## Logs and test report

When reporting the first GPU test, collect the renderer logs containing
`Linux FSR 4.1.2`, `Ocarina FSR 4 evaluated` or `using FSR 3.1`.
Also record whether the shader assets were found, GPU and driver,
render/output resolution, selected quality preset, and symptoms.

Start with still scenes, then camera pans and actors passing in front of
background geometry. Compare to the built-in FSR 3.1 path; do not disable
fallback if the image is black or the Vulkan validation layer reports errors.

## Source, licenses and acknowledgments

- [BeamNG independent FSR 4.1.2 implementation](https://github.com/Mob1K7105/beamng-fsr4-linux) — source pinned at `v0.2.1`.
- [FSR 4.1.1-linux / bbport](https://github.com/Mob1K7105/beamng-fsr4-linux/blob/v0.2.1/docs/LICENSING.md) — prior work from which the Vulkan code derives.
- [AMD SDK 2.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) — original FSR 4.1.2 model and shader source.
- [Lichii PR #915](https://github.com/zortos293/Lichii/pull/915) — earlier native 4.1.1 integration reference.
- [Lichii PR #1062](https://github.com/zortos293/Lichii/pull/1062) — signed Windows SDK 2.4 integration.

The external C++ implementation is GPL-2.0-or-later, compatible with
Lichii's GPL-3.0-or-later source. AMD-derived shader/model assets have
separate notices and license terms in the external project; preserve
`assets/NOTICE.md`, `notices/AMD-SDK-LICENSE.md` and all required notices
if you redistribute the assets. This integration is **unofficial**.
