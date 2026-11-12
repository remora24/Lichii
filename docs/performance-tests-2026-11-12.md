# Local Linux performance experiments, 2026-11-11–12

This branch archives results only. Do not merge rejected experiments into `nx2`.
Sanitized measurements are in `performance-tests-2026-11-12.json`. Raw emulator logs,
shader/texture/buffer captures, binaries, temporary scripts, and build directories are not uploaded.

## Retained build

The owner kept only `lichii-test-c-oot`, the clang 23.1.4 PGO build of source
`c908fc877e13f9d13c3a8adebe998a5f4d59b047` with the combined Mario Kart World/Ocarina profile
from upstream PR #1205. The profile's Git blob is `5136a56910ced54824c620c87db132ff41dfd455`.
It includes the FSR actor-motion ghosting fixes and output-resolution selector, but none of
PR #1103, PR #1129, actor-batch reuse, or frame-overlap experiments.

The profile is integrated into this fork's `nx2` by PR #6 (`10169537a6`). The retained binary
predates that documentation/CI merge; its rendering source and profile match test C.
Its SHA-256 and profile checksum are recorded in the JSON archive.

Hardware: Radeon RX 7800 XT, RADV/Mesa 26.2.6. PGO uses `lichii-linux.profdata`; LTO is off.
The A/B/C/D tests used FSR 3.1, 2560×1440 render to 3840×2160 output, and an unlocked frame rate.

## Results and interpretation

### Idle A/B/C/D, then baseline repeat

A = original Mario Kart profile. B = A plus PR #1103. C = Ocarina profile only.
D = PR #1103 plus Ocarina profile. Each run contains six five-second FPS samples.

| Run | Mean reported FPS |
|---|---:|
| A first | 62.91 |
| B | 61.40 |
| C | 65.61 |
| D | 64.31 |
| A repeat | 65.42 |

The baseline changed by 2.51 FPS between launches. These runs do not establish an idle
performance gain or loss from either change.

### Castle Town save, A/B/C/D/A

Each run contains twelve five-second FPS samples. The interval statistics cover only the
bounded interval history returned by the debug server (1,440 intervals per run), not all frames.

| Run | Mean reported FPS | Median interval | p99 interval |
|---|---:|---:|---:|
| A first | 54.17 | 18.0 ms | 23.3 ms |
| B | 57.00 | 17.4 ms | 21.8 ms |
| C | 58.74 | 16.9 ms | 21.3 ms |
| D | 58.47 | 16.9 ms | 21.7 ms |
| A repeat | 57.20 | 17.2 ms | 22.4 ms |

Moving NPCs/chickens change scene load. Between-launch variation again prevents a firm causal
claim. The owner chose C and integrated the Ocarina profile. PR #1103 remains experimental.

### PR #1129: raised texture-cache ceilings

Baseline and candidate both used the Ocarina profile and `EDEN_NX2_STUTTER_TRACE=1`.
Each capture lasted about 121 seconds. Complete ten-second summary windows cover about
110.15 seconds per run, excluding partial boundary windows. Their histograms include every
presented frame in those windows, at 1 ms resolution. Approximate percentiles are bin midpoints.

| Metric | Baseline | PR #1129 |
|---|---:|---:|
| Mean reported FPS across 24 samples | 58.26 | 42.51 |
| Complete-window frames | 6,456 | 4,536 |
| Frames over 33 ms in complete windows | 18 | 959 |
| Frames over 100 ms in complete windows | 2 | 2 |
| Approximate p99 | 24.5 ms | 58.5 ms |
| Critical threshold | 9,573 MiB | 12,926–13,808 MiB |

The owner reported severe slowdown and asked to stop; the measurement had completed by the
 time cancellation arrived. Candidate FPS samples fell to 17.2 before recovering. The largest
logged candidate hitch was 399.7 ms, compared with 211.7 ms on baseline. The major initial
hitches involved texture uploads. Memory retention/driver stalls are a hypothesis, not a proven
root cause. Reject this candidate for the owner's setup; it is not merged.

## Preserved branches

All branches below are on the owner's fork, not proposed to upstream Lichii.

| Branch | Commit | Outcome |
|---|---|---|
| `perf/radv-driver-cache` | `ca049ab98c` | Port of upstream #1103; no demonstrated benefit in these tests |
| `test/texture-budget-1129` | `935cf6f490` | Port of upstream #1129; severe slowdown, rejected |
| `perf/actor-assembly` | `509ceca200` | Batch/geometry upload reuse; about 0.12 ms/frame less measured host work, no demonstrated FPS gain |
| `perf/fsr4-frame-overlap` | `2add88b001` | Ring-tick waits and cached input views; removed most per-frame GPU-thread waiting, no demonstrated FPS gain |
| `feat/dlss-shared-actor-motion` | `747086a1af` | Optional DLSS-sharing variant; built/tested on Linux, NVIDIA gameplay untested |

The actor ghosting fix remains enabled in the retained build. Approximately 0.7 ms/frame of
host actor work was observed with profiling; its exact causal contribution to total frame time
was not isolated. Do not treat subtraction of sequential runs as an exact cost measurement.

## Cleanup policy

Keep the retained C launcher/build, source repositories, required bundled assets, PGO profile,
and normal game data. Remove other local test launchers/builds, temporary measurement/profile
files, and diagnostic game captures. Leave saves, mods, firmware/keys, settings, and ordinary
emulator/Mesa shader caches untouched.
