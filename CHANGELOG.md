# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [Unreleased]

- Bundle native Linux FSR 4.1.2 shaders, model weights, notices, and backend source in builds and
  x86_64 AppImages/tarballs. Resolve assets relative to the executable without requiring
  `LICHII_FSR412_ASSETS_DIR` or a separate asset installation.
- Fix native Linux FSR 4.1.2 motion dilation by preserving Ocarina's standard depth instead of
  reversing it. The bundled prepass selects minimum depth, so reversed input selected background
  motion at foreground boundaries.
- Fix blurry, ghosting animated characters in the Ocarina temporal replacement. The actor motion
  replay matched one vertex shader, one 8-weight mesh and one pose per frame, so every other
  colour variant and skinned part kept camera-only motion. Capture now follows the game's skinning
  contract (1-8 bone influences, any position/bone/weight attribute layout) across all of a
  character's draws, deduplicates repeats, and replays each part with its own current and previous
  pose in one overlay draw.
- Fix Ocarina actor-motion capture rejecting every skinned draw because uniform-cache bindings
  do not retain guest GPU addresses. Resolve c3/c4/c6 through Maxwell's live constant-buffer
  state while preserving size, mapping, and memory-coherency checks.
- Fix native Ocarina actor geometry being rejected against unused legacy index and vertex
  limits. Actor capture now checks native byte-count bounds, including first-index offsets,
  before replaying current and previous poses into the FSR motion image.
- Support smaller actor bone palettes in Ocarina instead of requiring Link's 13568-byte c6
  binding. Capture only available matrices, reject active joints outside the captured palette,
  and zero-pad the fixed replay pose without reading past the binding.
- Fix ghosting on Link's hat and shield, and on most enemies, with Ocarina's temporal upscaling
  (FSR 3.1 and FSR 4). The replay dropped most visible actor pixels at a 16-ULP depth check
  (now 512), rejected half-float vertex positions (the shield), and read the bone palette from
  constant buffer 6 where some shader variants keep it in 7 (Link's hair, which is now replayed
  but still matches the scene depth on only about a quarter of its pixels). The contract trace
  now logs each rejected draw and measures per mesh how many visible fragments are written.
- Cut the Ocarina actor capture's per-frame cost from about 1.5 ms to 0.5 ms of GPU-thread time
  by keeping each skinned mesh's decoded geometry and reusing it while the guest bytes are
  unchanged (they are compared every frame). `EDEN_NX2_GPU_PROFILE` reports the capture's
  checks, pose, geometry and assembly time and the Ocarina upscale as separate zones.
- Add "FSR output resolution" (Linux and other non-Windows builds), shown while an FSR checkbox
  is on and mirroring NVIDIA DLSS's quality setting: Automatic (default, unchanged behaviour),
  Native AA, Quality, Balanced, Performance and Ultra Performance. Native AA keeps Ocarina's
  1440p mode at 1440p instead of reconstructing to 4K (texture LOD bias -1 instead of -1.58);
  explicit modes apply only when the game's sizes have AMD's ratio for them and otherwise fall
  back to Automatic with a log line.
- Keep NVIDIA DLSS's Ocarina actor replay as before (one Link draw, 16-ULP depth check); the
  actor-motion fixes above apply to FSR only.
- Enable the native Linux FSR 4.1.2 Vulkan features (compute derivatives, integer dot product)
  only on AMD with FSR 4.1.2 turned on; other GPUs, builds and settings keep the original device
  feature chain and no longer log a removed-extension warning.

## [0.1.0]

- Initial release candidate
