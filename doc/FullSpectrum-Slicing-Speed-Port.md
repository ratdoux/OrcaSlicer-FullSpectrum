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
Source-only pointillism helpers and per-component painted-cadence tracking are
not inserted into FullSpectrum's different nominal-layer cadence. The tracker
utility and its regression tests are retained for source traceability.

Each of the 52 selected commits records its source hash with `cherry picked
from`. The imported performance reports are historical source-branch evidence;
their speedup figures do not establish FullSpectrum performance.

## Validation

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
