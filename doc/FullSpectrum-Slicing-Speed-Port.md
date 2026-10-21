# FullSpectrum slicing-speed port

This branch ports the performance series from the local
`ratdouxdoux-OrcaSlicer` branch `proposal_UI_changes_GPU_Accelerate` through
`87930d936e` onto FullSpectrum `cc152a84dd` (0.9.14). The source includes 35
commits beyond its remote-tracking branch. The port uses that local tip.

## Scope

- CPU wall/support outline reuse, lazy shell projections, bounded seam caching,
  and ordered parallel tree-overhang filtering.
- Borrowed extrusion traversal and preview input buffers, reduced preview
  retention, faster Klipper velocity-limit parsing, and exact LocalZ export
  quality-distance batches.
- LocalZ mask filtering, parallel wipe-tower eligibility, bounded parallel
  layer geometry preparation, and reuse of fixed masks and painting geometry.
- Deterministic painted shell/slab processing, reused selectors, compact strict
  meshes, reduced graph allocations, and reused region bounds/surface buckets.
- Optional Vulkan LocalZ rejection and seam visibility, their persisted
  preferences, CPU fallbacks, build tooling, tests, and profiling/replay tools.

CPU improvements are enabled by default. GPU preferences remain off by default.
The Windows release helper provisions pinned Vulkan build tools unless
`ORCA_ENABLE_VULKAN_COMPUTE=OFF` is set. Direct CMake configuration accepts
`AUTO`, `ON`, or `OFF`; runtime GPU availability is independent of build support.
The final source revision's rollback of indexed travel containment is included.

## FullSpectrum adaptations

The port preserves FullSpectrum's image mapping, adaptive perimeter ownership,
intact LocalZ loops, role filtering, gradient ramps, and canonical mixed-filament
recipes. LocalZ clippers borrow stable mask storage after context assembly.
Adaptive base paths keep separate external and non-external exclusion masks;
entities using multiple role-specific masks stay on exact CPU clipping because
the optional rejection classifier accepts one exclusion mask per entity.

Painting preparation retains FullSpectrum's painted-line injector and protected
adaptive side states. Only immutable LocalZ geometry runs in new parallel
batches. Cadence, gradient state, dependency IDs, and pass emission remain in
layer order. New batch workers read cancellation without mutating the runtime
memory guard's sampling state.

The channel-map regression follows FullSpectrum's canonical virtual IDs:
disabled legacy rows retain their slots, while tombstoned rows do not. The
prime-tower fixture uses canonical recipes, explicit extrusion widths, and
explicit direct/independent LocalZ settings instead of source-branch defaults.

FullSpectrum already contains the prime-tower scheduling fix from `707c6e1853`.
Its end-to-end test is ported separately and extended beyond the 64-layer batch
boundary. The unrelated gradient-dialog change `8106485824` is excluded.
Source-only pointillism helpers are not inserted into FullSpectrum.

The painted-zone pattern fix from `2c0e48757c` (upstream PR #363, bugs 34/43) is
integrated as a follow-up to the initial performance port. Each connected
ordinary painted mix starts its pattern at its first active layer, continues
through connected splits/merges, and restarts after a gap. Disconnected zones
can select different physical filaments on the same nominal layer. This
intentionally changes the old FullSpectrum behavior, which used the object's
absolute layer index. Incoming upstream code contained this fix during the
July/September merges, but FullSpectrum's merge results retained the old path.

The production integration now uses the performance series' borrowed-mask
tracker, parallel channels with ordered per-channel layers, and short-circuited
overlap checks (`df79223de4`, `b21f39fbd1`). Worker cancellation only reads the
cancel flag; the full callback runs before and after the new parallel phase.
`ORCA_PREP_VALIDATE_REUSE=1` compares activation indices with the exhaustive
reference tracker. `ORCA_PREP_EXPERIMENTS=none` disables the overlap shortcut
without reverting the pattern-start fix. Canonical recipe lookup, LocalZ and
perimeter-modulation exclusions, and disabled region-collapse behavior remain
in place.

Each of the 52 selected commits records its source hash with `cherry picked
from`. The imported performance reports are historical source-branch evidence;
their speedup figures do not establish FullSpectrum performance.

## Painted-zone follow-up validation

- The new sliced-geometry regression reproduces incorrect elevated/restarted
  manual-pattern assignments against the pre-fix library. With the fix, all
  546 assertions pass, covering two mixed channels, simultaneous disconnected
  zones, gaps, manual and ratio-based patterns, disabled collapse, and one/four
  worker execution. The exhaustive tracking mode also passes the same test.
- Reference-enabled `[SlicingSpeedPort],[CPUSlicing],[SMLCopy],[GradientPort]`
  passes **29 cases / 231,082 assertions**, including LocalZ tower output,
  copy reuse, gradients, and fast-versus-exhaustive activation comparisons.
- Release application, libslic3r tests, and FFF tests rebuild successfully.
- Full CTest run: **414/422 passed**. The same eight failures listed below
  remain; no new failing test was introduced.
- The normal-workflow Starry CLI project completes and exports G-code with
  `ORCA_PREP_VALIDATE_REUSE=1`. Production tracking exercises multiple mixed
  channels, including one with 99 disconnected components, with no activation
  mismatch against the exhaustive reference. This is a correctness run, not
  an isolated performance benchmark.

Local evidence: `cadence-before.log`, `cadence-reference-validation.log`,
`cadence-reference-only.log`, `build-cadence.log`, and `ctest-cadence.log`.
The completed CLI run is in
`build-speed/validation/starry-painted-cadence-final/`; its DLL SHA-256 is
`bf6052f6f56b41c7392cc702be4fe4fe35aac1bf4a9a2364554df106d8a357e9`.

## Validation of the initial performance series

Validated on Windows with Visual Studio 2022, CMake 3.28.4, and Catch2 3.7.1.
The out-of-source `build-speed` configuration enables `BUILD_TESTS`,
`ORCA_TOOLS`, and `ORCA_ENABLE_VULKAN_COMPUTE=ON`, using existing dependency
installations without modifying their sources. Vulkan headers are 1.3.290;
glslang is 16.6.0.

- `cmake --build build-speed --config Release --target ALL_BUILD --parallel 6`
  and `cmake --install build-speed --config Release` passed. Installation is
  local to this worktree's build directory.
- A separate CPU-only compute backend smoke build with
  `ORCA_ENABLE_VULKAN_COMPUTE=OFF` passed and returned the expected unavailable
  backend result.
- `ctest --test-dir build-speed -C Release --output-on-failure --parallel 2
  --timeout 300`: **413/421 passed**. See the remaining failures below.
- `[SlicingSpeedPort],[CPUSlicing]`: **19 cases / 187,442 assertions passed**
  with `ORCA_PREP_VALIDATE_REUSE=1`, `ORCA_PREP_VALIDATE_THREADS=1`,
  `ORCA_LOCAL_Z_VALIDATE=1`, and `ORCA_SLICE_VALIDATE=1`. This includes mixed
  recipes, gradients, independent single-pass intervals, and verification of
  every planned prime-tower layer's exported extrusion length.
- Explicit hidden `[VulkanComputeHardware],[LocalZVulkanHardware],
  [SharedVulkanHardware]` tests passed on both NVIDIA GeForce RTX 5060 Laptop
  GPU and Intel Arc Graphics: **4 cases / 53,022 assertions per device**.
  Maximum seam-visibility reference error was below `3.6e-7`.
- The six imported Python profiling/benchmark scripts passed syntax parsing.

The following failures also reproduce with the existing FullSpectrum test
binaries dated 2026-10-12 in the original worktree. Those binaries provide
pre-port evidence, not a fresh rebuild of pristine `cc152a84dd`:

| Test | Observed result |
| --- | --- |
| DynamicPrintConfig serialization | Round-trip config inequality |
| m1 physical-filament bound differential | Assertion passes in a test marked `!shouldfail` |
| Placeholder parser scripting | Segmentation violation |
| Complete cached slots override direct nozzle data atomically | Flow list unexpectedly nonempty |
| Persistent image maps geometry-first perimeter modulation V2 | Width/inset expectations fail |
| Mixed print uses the highest bed temperature | 35 instead of 65 |
| Brim-introduced extruder with zero wall loops | 35 instead of 65 |
| Initial single bed temperature expands to maximum | Missing `M190 S65` |

CLI smoke slices use the source branch's existing `u1.3mf` LocalZ project and
`starry_no_local_z.3mf` normal-workflow project, through
`scripts/benchmark_cpu_slicing.py`. Both completed successfully and exported
G-code. The normal-workflow run enabled preparation reuse/thread comparisons
and slicing reference validation. A second U1 run also enabled those checks,
`ORCA_LOCAL_Z_VALIDATE=1`, `ORCA_LOCAL_Z_CLIP=validate`, and NVIDIA selection.
It validated 4,159,610 GPU clipping segments across 716 layers, with no fallback
layers or validation failures. Painting's side/shell/merged thread comparisons
reported zero ordered or geometric differences. Compared with the CPU U1 run,
every executable G-code command and every role's positive extrusion commands
are identical. Full-file hashes differ only in one generation timestamp and
4,390 runtime object-ID comments; tower schedules and print statistics match.

These are correctness smoke runs, with validation overhead and concurrent test
activity; they do not measure a FullSpectrum speedup. No physical printer
validation was performed.

Detailed local logs are `build-speed-final.log`, `ctest-speed-final.log`,
`port-reference-validation.log`, `gpu-nvidia-validation.log`,
`gpu-intel-validation.log`, and `baseline-existing-*.log`. CLI logs, metrics,
and G-code are under `build-speed/validation/`. Build artifacts and raw logs
are untracked.

The work was performed in the separate `OrcaSlicer-FullSpectrum-speed` worktree
on `slicing-speed-optimizations`. The original worktree's uncommitted import
changes were not included or modified.

## Local-Z whole-path reuse follow-up (2026-10-21)

Implementation commit: `50c0d4f666` (one rollback unit for the dependent geometry changes).

Ported the CPU geometry index (`db8987b950`), exact clipping-result cache
(`a2a071be96`), and guarded whole-path reuse (`720a634227`) from
`ratdouxdoux-OrcaSlicer`, branch `proposal_UI_changes_GPU_Accelerate_V2`, into
`OrcaSlicer-FullSpectrum` on `slicing-speed-optimizations`.

The index uses bounded integer predicates to prove empty operations or complete
path containment. Repeated identical mask/path operations share a per-export-layer
cache with a 32 MiB entry budget. A fully contained path retains its original XY
vertices and direction instead of being reconstructed by Clipper. Ambiguous
boundaries, holes, crossings, degenerate segments, unsupported coordinates and
oversized masks retain exact clipping. Fitted arcs are cleared for routed open
paths, matching the existing clipping contract. Boundary-clipped paths retain
the existing fragment order.

FullSpectrum's intact-loop handling is retained: the containment proof accelerates
its completeness test, while the exporter still clones complete loops for normal
seam placement. Unproven loops retain the previous exact completeness test.
Adaptive external walls and ordinary inner-wall/infill paths continue to use their
distinct exclusion masks. Role filtering, mask compensation, perimeter modulation,
adaptive non-planar rendering, pass heights, dependency ordering and tower scheduling
remain in the existing FullSpectrum pipeline. Pass flow is rescaled once before
any downstream adaptive-height renderer applies its local height adjustment.

These CPU improvements are enabled by default in a normal release build. They do
not require Vulkan or a new preference. Developer comparison switches:

- `ORCA_LOCAL_Z_CPU=legacy` disables the new CPU index/cache; `all` is the default.
- `ORCA_LOCAL_Z_REUSE=0` disables whole-path reuse and the fast intact-loop proof;
  `1` is the default.
- Use both switches above to compare against the pre-port geometry path in the
  same executable.
- `ORCA_LOCAL_Z_VALIDATE=1` independently checks clipping against full-mask
  Clipper operations, and reused paths for retained length and no discarded
  geometry. Validation is deliberately expensive and excluded from benchmarks.

The local build includes preexisting uncommitted changes to tiny-path/tower
planning and mesh import. They are identical in both benchmark modes and are
kept out of the port commits. No input project or existing GUI process is modified.

### Follow-up validation

Release application, libslic3r tests and FFF print tests build successfully.
Reference-enabled core selection `[LocalZ],[CPUSlicing],[ClipIndex],
[SlicingSpeedPort],[SMLCopy],[GradientPort]` passes **53 cases / 385,596 assertions**.
The new role-mask regression checks that a path excluded from an adaptive external
pass can still remain in the ordinary inner-wall pass, without reusing the wrong
mask decision or modifying the original loop. FFF `[LocalZ]` passes **6 cases /
684 assertions**, covering adaptive non-planar height/flow, gradient seams,
independent heights and component ratios. The core selection includes prime-tower
extrusion/scheduling checks, including the worktree's tiny-loop regression.

The same six FFF cases also pass with both optimizations disabled, including
reference geometry validation. Raw local evidence is in
`build/local-z-reuse-port-20261021/`.

### FullSpectrum before/after benchmark

Project: `开启混色细分层高导致局部层高过小.3mf`, SHA-256
`0747970025e0dbc758023833e51173c42395c82966ece97f44445b62e64e2f63`.
Two fresh-process CPU-only runs per mode, sequentially, using the same Release
DLL (`89b1b065d4d3cef542cf24e9e58435dbc2679b5016ba860c91745d6ccd5eca17`)
and embedded project settings. Before uses `ORCA_LOCAL_Z_CPU=legacy` and
`ORCA_LOCAL_Z_REUSE=0`; after uses the default `all`/`1`. Validation and diagnostic
clipping timers/capture are off. Ordinary debug logs supply stage boundaries.

| Measurement | Before, median of 2 | After, median of 2 | Reduction |
| --- | ---: | ---: | ---: |
| Complete CLI slice | 137.93 s | 18.55 s | 86.6% (7.44× faster) |
| G-code export | 124.89 s | 5.55 s | 95.6% |
| Local-Z path-routing windows within export | 121.75 s | 2.37 s | 98.1% |
| Process CPU time | 180.76 CPU-s | 65.70 CPU-s | 63.7% |

Individual complete times: **138.22, 137.64 s before** and **18.52, 18.58 s after**.
The routing windows run from the existing `Local-Z context` to `clipping summary`
log entries; they exclude earlier mask assembly and are included in export time.
This comparison includes all three CPU changes ported here; it does not isolate
the contribution of reuse alone. These are FullSpectrum measurements, distinct
from the earlier source-branch results. Peak private commit overlaps across runs
(782.8–810.7 MiB before, 781.0–793.2 MiB after); no memory improvement is claimed.

Both optimized runs reuse approximately **104,000 open paths**, with the remaining
approximately 60,000 operations using the exact-clipping interface, including its
indexed empty-result rejection. Intact-loop proofs are
additional and are not counted by the open-path routing counters. All four runs
have the same Local-Z pass and Local-Z tower schedule hashes, covering the 21
nominal layers from Z=5.4 through 9.4 mm, and no reported Local-Z leak, pass-height
or tower-sequence mismatch.

Whole-file G-code equivalence is **not** claimed. Even the two disabled baseline
runs differ by 77.95 mm of positive XY-extrusion filament (0.54%), and their
Z/tool/role/height groups differ. Large per-tool differences occur above the
Local-Z section; the optimized runs also show this run-to-run variation. Total
reported mass is 45.72/45.47 g before and 45.47/46.01 g after. Full-file tool
sequences can differ even though the Local-Z schedules match. Reusing open paths
also intentionally preserves their authored direction, which can change travel
and command order. Independent per-operation full-mask validation and the
flow/seam/tower regressions provide the correctness checks; the whole-file audit
does not establish identical output or resolve the preexisting nondeterminism.

The separate full-model run with `ORCA_LOCAL_Z_VALIDATE=1` completed successfully:
**103,664 reused open paths**, no reference clipping/containment failure, and zero
reported base leaks, pass-height mismatches or Local-Z tower-sequence errors.
Its 368.31 s elapsed time includes the expensive independent reference work and
is excluded from the benchmark table. Validation also covers fast intact-loop
proofs, although those are not included in the open-path reuse counter.

The built application is `build/src/Release/snapmaker-orca.exe`; the regular
Windows release helper includes this code without extra flags. There has been no
physical printer validation. The port does not fix the separately observed
whole-slice output nondeterminism.
