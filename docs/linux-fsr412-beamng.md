# Experimental native Linux FSR 4.1.2 (BeamNG backend)

This branch adds the first native Linux adapter to the **FSR 4.1.2 INT8** Vulkan
implementation in [Mob1K7105/beamng-fsr4-linux](https://github.com/Mob1K7105/beamng-fsr4-linux).
It does not use AMD's Windows FidelityFX runtime, Wine, or Proton.

**STATUS: DEVELOPMENT ONLY — not visually correct, not performance-tested, and
not yet suitable for an ordinary gaming session.** The default build, existing
Windows FSR 4, Linux FSR 3.1 and all non-FSR paths remain unchanged.

## Why it is disabled

The BeamNG implementation assumes **reversed-Z depth**. Lichii's Ocarina FSR path
currently provides **standard (forward) D32 depth**, and the native adapter does
not yet convert it. Simply passing that image into BeamNG's backend can give
incorrect reconstruction. Until a correct depth conversion/reuse strategy is
implemented and checked against the reference, the constructor deliberately
throws and Lichii falls back to FSR 3.1.

The adapter also forces a GPU completion wait before every FSR 4 evaluation.
This protects BeamNG's 8-slot, host-mapped constant-buffer ring and image-view
lifetimes until Lichii's GPU-timeline ownership is integrated. It is not
appropriate for performance benchmarking in this form.

Do not interpret a successfully recorded compute dispatch as visual parity.

## Source and assets

The independent C++ implementation is external: we compile
`fsr411/fsr411.cpp` from a checked-out copy of
[Mob1K7105/beamng-fsr4-linux](https://github.com/Mob1K7105/beamng-fsr4-linux).
Development is pinned to the **v0.2.1** source release (2026-11-05); use that
version until changes are intentionally reviewed. The `assets/` directory
contains the 4.1.2 INT8 SPIR-V/model sets. Lichii does not copy these files or
automatically download/redistribute them.

The BeamNG source is GPL-2.0-or-later, with attribution to its original
authors (including the FSR-4.1.1-linux and bbport work). Lichii is
GPL-3.0-or-later. The upstream project states that its derived AMD shader/model
assets are subject to AMD's SDK 2.4 MIT exception and includes the associated
notices. Preserve `assets/NOTICE.md`, `notices/AMD-SDK-LICENSE.md` and other
required notices when redistributing the assets. This integration is unofficial
and is not endorsed by AMD.

## Build (Linux)

Check out the external project at its pinned release, adjacent to Lichii:

```fish
git clone https://github.com/Mob1K7105/beamng-fsr4-linux.git
cd beamng-fsr4-linux
git checkout v0.2.1
cd ..
```

Configure Lichii using the normal build flags for your installation, with
these additional options (example directory names only):

```fish
cmake -S Lichii -B Lichii/build -G Ninja \
  -DENABLE_LICHII_LINUX_FSR412=ON \
  -DLICHII_BEAMNG_FSR412_ROOT=(realpath beamng-fsr4-linux)
cmake --build Lichii/build --target yuzu -j (nproc)
```

The feature requires the Vulkan loader library in addition to the normal
Lichii build dependencies. It is **OFF by default**. On platforms other than
Linux it is a configuration error.

## Developer-only dispatch testing (unsafe image quality)

For controlled diagnostics **only**:

```fish
set -x LICHII_FSR412_EXPERIMENTAL 1
set -x LICHII_FSR412_ALLOW_STANDARD_DEPTH 1
set -x LICHII_FSR412_ASSETS_DIR (realpath beamng-fsr4-linux/assets)
```

Start Lichii from the same fish shell with the Ocarina FSR backend enabled.
The desktop AMD FSR 4 checkbox is intentionally still hidden on Linux: this
test mode uses environment variables instead.

The explicit depth override does **not** fix depth; it permits incorrect input
for low-level Vulkan testing and may produce wrong output. Without it (or
without assets, INT8, or push-descriptor support), FSR 3.1 takes over. If the
BeamNG runtime declines to record, the current frame may still be invalid and
FSR 3.1 takes over on the next frame, matching the prior asynchronous fallback
semantics. Do not distribute test builds as working FSR 4.1.2.

## Integration accomplished in this first stage

- Select the BeamNG 4.1.2 native backend only in explicit Linux opt-in builds;
  otherwise retain the existing AMD signed-Windows-DLL path.
- Forward the Ocarina scene color, depth, reconstructed motion, jitter, output
  and sharpening parameters to `Fsr411::Upscaler::Record()`.
- Create and cache input Vulkan image views; create the RGBA16F output image
  and preserve its existing Lichii presentation interface.
- Maintain `GENERAL` image layouts and add explicit GPU dependency barriers.
- Fail closed on missing assets, unexpected formats, incompatible Vulkan
  prerequisites and unsupported output sizes.
- Keep the existing FSR 3.1 fallback path.
- Synchronize every recorded frame for initial correctness investigations
  (temporary performance cost).

## Required before enabling normally

1. **Depth convention:** convert the standard Ocarina depth to the reversed-Z
   representation expected by the BeamNG prepass (or prove a different safe
   contract), then verify edge/disocclusion behavior with reference captures.
2. **Device features:** ensure the logical Vulkan device actually enables
   `shaderInt8`, integer dot products, 16-bit arithmetic,
   `VK_KHR_push_descriptor`, `VK_KHR_compute_shader_derivatives`, and any
   required shader capabilities. This adapter currently disables the optional
   mixed-float-dot prepass and subgroup-size-control optimization. Unsupported
   capabilities must produce a clean fallback, not a crash.
3. **Resource contracts:** verify the color and motion-vector conventions,
   depth aspect, shader-read image usage and any required reactive masks with
   validation layers enabled. BeamNG's `Fsr411::Frame` does not accept a
   reactive mask in its public interface.
4. **Synchronization:** replace the per-frame `scheduler.Finish()` with a
   bounded GPU timeline/fence guarantee for BeamNG's 8 in-flight slots.
5. **Error behavior:** validate asynchronous Record failures, shutdown, live
   resolution change, reset and FSR 3.1 history handoff. Ensure a failed frame
   can never consume uninitialized output.
6. **Parity and performance:** test on an RDNA 3 Radeon (RX 7800 XT is a useful
   target) with Ocarina of Time using fixed scenes, comparing FSR 3.1, 4.1.1
   and 4.1.2. Collect GPU-timestamp timings, flicker/ghosting captures and logs.
7. **User interface:** after correctness passes, expose a Linux FSR 4.1.2
   selector and asset-status information without showing the Windows-only AMD
   signed-DLL download panel.

The upstream BeamNG author's tests were performed on an RX 6700 XT (RDNA 2);
this is not evidence that Lichii's RDNA 3 path is already validated.

## References

- Original Lichii Linux FSR 4 proof of concept:
  https://github.com/zortos293/Lichii/pull/915
- Official Windows FFX SDK 2.4 integration:
  https://github.com/zortos293/Lichii/pull/1062
- Reusable BeamNG Vulkan FSR 4.1.2 code:
  https://github.com/Mob1K7105/beamng-fsr4-linux/blob/v0.2.1/fsr411/fsr411.h
- BeamNG correctness/compatibility:
  https://github.com/Mob1K7105/beamng-fsr4-linux/blob/v0.2.1/docs/VERIFICATION.md
