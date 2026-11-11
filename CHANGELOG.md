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

## [0.1.0]

- Initial release candidate
