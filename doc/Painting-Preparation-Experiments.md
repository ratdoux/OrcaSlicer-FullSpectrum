# Painting preparation CPU experiments

Date: 2026-10-16. Performance checkpoint: `046ef0c7af`. Subsequent commit `f0d9347251` adds only opt-in preparation snapshots; it does not change slicing algorithms or defaults.

The useful CPU changes are implemented and enabled by default. They reduce repeated painting deserialization, parallelize independent surface trimming and Local-Z geometry preparation, and reduce region-tracking and graph-allocation work. Normal `build_release_vs2022.bat slicer` builds include them without a new option or build flag. No new Vulkan kernel or preference was added. These geometry caches reuse work within the current slice; they do not depend on a previous slice.

The experiments also found thread-dependent painted geometry. Those correctness fixes precede the performance changes: timings alone would not have been a sufficient acceptance test.

## Controlled results

| Workload | Complete process before | Complete process after | Reduction | Preparation before / after |
|---|---:|---:|---:|---:|
| U1, Local-Z on | 37.39 s (36.71-38.07) | 33.37 s (33.18-33.55) | 10.8% | 9.12 / 4.62 s |
| Starry Night, Local-Z on | 150.64 s (148.54-152.73) | 134.48 s (132.80-136.15) | 10.7% | 37.73 / 18.86 s |
| Starry Night, Local-Z off | 88.36 s (87.62-89.11) | 80.37 s (79.64-81.10) | 9.0% | 28.18 / 16.93 s |

Process CPU time and memory medians are recorded separately from elapsed time:

| Workload | Process CPU time before / after | Peak resident working set before / after | Peak process commit before / after |
|---|---:|---:|---:|
| U1, Local-Z on | 150.1 / 143.2 s | 1.85 / 1.35 GiB | 2.77 / 1.48 GiB |
| Starry Night, Local-Z on | 578.6 / 581.1 s | 5.35 / 3.84 GiB | 8.09 / 4.08 GiB |
| Starry Night, Local-Z off | 492.1 / 482.3 s | 5.45 / 3.75 GiB | 8.17 / 3.86 GiB |

CPU seconds sum work across threads. Parallel preparation can lower elapsed time without lowering total CPU time; Starry Local-Z CPU time is essentially flat in this small sample. The memory figures are slicing-process peaks, not idle GUI memory or GPU VRAM. Commit measures private memory backing, while the working set measures resident pages.

These are fresh CLI processes on an Intel Core Ultra 9 285H with 16 logical processors, using the same final Release executable. **Before** selects `ORCA_PREP_EXPERIMENTS=none`; **after** leaves that variable unset, using normal defaults. Both retain the determinism fixes and compact graph adjacency storage. The table therefore measures the switchable CPU improvements, rather than attributing geometry changes or the adjacency-storage change to a speedup.

Two runs per configuration and workload, in alternating order, with all observations retained. Ranges describe this small sample; they are not confidence intervals. Complete process time includes project loading, slicing, G-code export and cleanup, but no GUI preview creation. The preparation interval spans `Starting the slicing process` through `Generating walls`, corresponding to the broad early mesh/painting/Local-Z phase rather than mesh-plane intersections alone.

All timed slices ran sequentially, without concurrent builds, reference comparisons or stack profilers. `ORCA_PREP_PROFILE=1` enables the same lightweight stage timers in both configurations. Independent reference-validation runs are excluded. Background slicer process snapshots are recorded per run; no other Snapmaker Orca process appeared in the recorded before/after snapshots. Power settings and clocks were not changed.

Common controls retain CPU Local-Z clipping and CPU polygon computation, seam caching and parallel tree-support filtering. This isolates preparation improvements from earlier GPU experiments. These totals should not be compared directly with older reports using Vulkan Local-Z clipping.

The original projects are unchanged. U1 uses the identical benchmark copy of `U1+有换料暂停+打印前必须看说明.3mf`; Starry uses `STARRY NIGHT PRINT.3mf` with Local-Z enabled and the existing separate copy with Local-Z disabled.

Timed DLL SHA-256: `ef43548ced1400ef0eeecb568ac1b2c0b4d16fba1c2fdb0db5b3e4e02b2ef313`.

| Input | SHA-256 |
|---|---|
| U1, Local-Z on | `8e6e6441decac1408721b30d76597a4d343b81bf06882adb9feeb9ffe69edb8e` |
| Starry Night, Local-Z on | `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2` |
| Starry Night, Local-Z off | `1fba784fec8ae005528cac2573a61ab9a3d0eba0aaaa4a5eaefa9ffdaed6449b` |

## Where the savings occur

Median elapsed seconds from direct timers:

| Operation | U1 before / after | Starry Local-Z before / after |
|---|---:|---:|
| Painted top/bottom processing | 4.737 / 1.400 | 13.376 / 4.012 |
| Side projection | 0.385 / 0.260 | 1.146 / 0.830 |
| Side region construction | 1.131 / 1.096 | 5.878 / 6.265 |
| Local-Z plan preparation | 1.097 / 0.116 | 9.152 / 1.136 |
| Mixed-region tracking | 0.031 / 0.016 | 3.269 / 1.704 |
| Region installation | 0.267 / 0.274 | 1.582 / 1.607 |

Whole-stage entries above are elapsed durations. Per-layer timers inside parallel workers accumulate overlapping elapsed durations; those sums are neither process CPU time nor predicted wall-time savings. The parallel Local-Z figure uses the enclosing plan timer, not the sum of worker timers.

## Decisions for each preparation step

### 1 Outline cleanup and spatial grids

Cleanup already runs across layers in parallel. Grid construction measured approximately 20 ms on U1 and 81 ms on Starry in the initial direct profiles. This is a small removal ceiling, so an additional parallel grid path, broader geometry caches and Vulkan grid construction were not prototyped. Geometry simplification tolerances remain unchanged.

### 2 Recovering and projecting painting

One immutable `TriangleSelector` is reused per volume within each processing phase; absent states are skipped. The new strict extraction resolves T-joints first, then removes unused vertices while preserving original vertex order, triangle order and coordinates. Sinking detection still considers all live vertices, as the original extraction did, even when a particular color uses only a subset.

Reference checks compare side-extraction arrays and projected top/bottom polygons against the old extraction on the same input. Tests cover subdivided adjacent triangles, unused vertices and absent states.

After reuse, the full parallel side-projection stage measured roughly 0.25 seconds on U1 and 0.81 seconds on Starry in the first direct profile. Transform reuse, task-local projection buffers and a Vulkan triangle/query pilot were screened out at this stage: their removable share is smaller still, while output buffering, candidate generation and exact CPU checks would remain. No claim is made that these techniques cannot help a different workload.

### 3 Building graphs and extracting material regions

Graph nodes now keep their small adjacency lists in inline contiguous storage, preserving insertion and erase order. Continuation candidates store an arc pointer and angle directly, removing nested single-element vector allocations. Diagnostic validation compares every chosen continuation with the original traversal on the same graph.

Voronoi construction itself retains the existing robust implementation and layer parallelism. The measured distribution did not reveal a large isolated tail: the initial Starry sample had a 124 ms 95th-percentile layer and a 196 ms maximum across 1,257 layers. Heaviest-first scheduling and allocator pools were therefore deferred rather than credited with hypothetical gains.

General mixed-color island splitting remains rejected because the earlier experiment changed geometry. The uniform-island shortcut remains opt-in, following the [U1 shortcut results](U1-Paint-Shortcut-Results.md). A full GPU Voronoi/topology implementation remains a separate algorithm project, not a small kernel replacement justified by these measurements.

### 4 Painted top and bottom surfaces

Occlusion trimming now runs over independent state/layer output slots while reading immutable neighboring outlines. The shell-propagation loop reuses color-independent cumulative outline intersections. Each active source-layer cache retains at most 32,768 vertices and 16 intersections per direction; larger/deeper work continues in per-color scratch storage. Color-specific offsets and polygon operation order remain unchanged.

Tests exercise both directions, repeated colors, holes, depth limits and forced memory-budget fallback. Diagnostic slices compare every cached intersection with the original ordered polygon result. The shell cache is a smaller contribution than selector reuse and parallel occlusion; combined timing does not isolate its complete-process benefit.

### 5 Mixed regions and Local-Z preparation

Mixed-region tracking tests the existing containment fallback before expensive boundary clipping. Once the earliest activation is known, candidates that cannot improve it are skipped. The overlap definition and split/merge cadence semantics are unchanged. Real-input validation compares every activation index with the original tracker, and fixtures cover holes, contact, splits, merges and gaps. Starry's busiest channels had fewer than 100 components per layer, so a spatial index was not added without evidence that its extra construction would pay off.

The rolling fixed-mask experiment passed exact checks but barely changed serial planning time on these inputs. The default parallel implementation instead shares fixed masks within each batch and its two neighboring layers. The rolling window remains available as a developer-only serial comparison.

Pure scan/geometry work is prepared in batches of at most 64 layers and an admission budget of 131,072 input vertices. An oversized single layer is admitted alone. This is an input bound, not a hard bound on Boolean output size. The measured retained batch vertex payload peaked around 2.5 MiB on U1 and 3.1 MiB on Starry, excluding container/allocator overhead and in-flight geometry temporaries. First/last layers, 64/65-layer boundaries, zero-point layers and oversized layers have deterministic tests.

Cadence resets, gradient state, error accumulation, sublayer decisions, dependency ordering and plan emission remain serial. Workers read a precomputed mixed-row lookup table: a `const` manager lookup can synchronize a borrowed legacy cache, so the new workers must not call it. Tests cover hidden/deleted rows and pending mutable legacy edits.

Reusing transition classifications between fixed passes was screened out: the entire serial plan-assembly timer was only about 32 ms on Starry before batching. The approximately nine-second geometry block was the useful target.

### 6 Installing regions

Final extruder bounds are calculated once after gathering masks, instead of repeatedly rescanning a growing collection. Surface-type buckets are shared across intersections against the same immutable parent region. Existing layer parallelism and bounding-box rejection remain in use.

The polygon operations and their order are preserved. Removing repeated clipping or reordering unions was not attempted because normalization and rounding can change even when two mathematical expressions describe the same area. The earlier Vulkan empty-polygon painting experiment skipped zero eligible operations on these projects; it remains opt-in and was not revived.

## Correctness and validation

The first same-input one-thread/default-thread U1 audit found 17 geometrically different layer/color pairs. Three mechanisms were addressed: incomplete projected-line tie-breaking; shell output groups inferred from arbitrary TBB chunk boundaries; and slab lines accumulated in worker-finish order before greedy loop chaining. Slab processing now restores source-face and within-face emission order, preserving the original serial ordering.

After these fixes, U1 and Starry had zero ordered or geometric differences in side regions, shell regions and merged segmentation under the same-input thread audit. This establishes the tested segmentation result, not global determinism of every slicer subsystem or every possible model.

All four final runs per workload were compared:

| Workload | Equal after timestamp normalization | Equal after timestamp and object-label identity normalization | Local-Z schedules and warning counts equal |
|---|---|---|---|
| U1, Local-Z on | No | Yes | Yes |
| Starry Night, Local-Z on | Yes | Yes | Yes |
| Starry Night, Local-Z off | No | No | Yes |

The secondary comparison renumbers only runtime IDs in the two `printing object` / `stop printing object` comment forms, preserving identity relationships, names, copy indices and every other byte. It does not remove arbitrary comments, executable commands, ordering, settings or print statistics. The strict timestamp-only comparison is retained separately; U1 is not described as byte-identical under that stricter test.

**Ordinary Starry does not pass full G-code equality.** Its two reference runs also differ from each other. Internal-solid-infill command counts range from 296,048 to 296,051 across the four final runs; one accelerated repeat also varies in gap infill. Wall, support, tower and other extrusion-role hashes match. Small path variations also affect travel, preheat timing and progress commands, so they are not dismissed as comment-only differences. Print material totals match at the reported precision; one estimated print time differs by one second.

A separate diagnostic build then captured the exact preparation boundary with the fast paths disabled and enabled. The two 182,808,633-byte files are byte-identical, including ordered layer outlines, region polygons and surface metadata (SHA-256 `a586f42eee9caa51e6e46082d3c074c95ca94d2501051f8454731ed6badc684c`). The enabled capture also passed per-operation reuse checks. This establishes the compared preparation geometry, while downstream ordinary infill nondeterminism remains unresolved. It does not establish complete ordinary G-code equivalence. Capture runs are excluded from timing.

Release application and test targets build successfully. The selected CPU, preparation, triangle-selector and non-hidden mixed-filament suite reports 232 passed cases and two documented expected failures. Explicitly selecting the hidden tests also reproduces the existing `config_extruder cascade` failure (`6 != 4`) in unchanged code; it is a separate remapping reproducer, not a new optimization regression. No physical print or macOS/Linux build validation is claimed.

## Claude review

The existing **Vulkan GPU geometry acceleration review** conversation was resumed using the [claude-conversation skill](C:/Users/radug/.codex/skills/claude-conversation/SKILL.md), through its CLI without controlling the desktop. Session: `d30d83f1-1057-401e-b61d-16c037d2700e`.

Claude supported the CPU-first experiments, identified slab ordering as the remaining determinism lead, reviewed the geometry/cadence separation and then found the mutable-cache risk in apparently const manager calls. The implementation uses source-face ordering and the immutable lookup table. Its suggested missing layer-count guard was already present. Review agreement is not a benchmark or correctness proof; the reference comparisons and test results above provide the measured evidence.

The updated U1 painting-only Vulkan validation, explicitly selecting NVIDIA, completed 4,273 eligible GPU checks. The CPU references contained 0 eligible empty results, and the GPU proved 0 empty results. This was a correctness/counter run, not a timed comparison; it overlapped output-file analysis. There is still no demonstrated work to skip in this workload's eligible painting operations.

No new Vulkan kernel was added. Existing GPU paths remain available. The remaining substantial painting cost is robust Voronoi/Boolean geometry already spread across CPU workers; the small regular suboperations do not currently offer enough removable elapsed time to justify transfers and a new fallback path.

## Reproduction and rollback

Use the existing [benchmark runner](../scripts/benchmark_cpu_slicing.py) and [strict G-code comparison](../scripts/compare_cpu_slicing.py). Run each configuration in a fresh process. Unset `ORCA_PREP_EXPERIMENTS` for normal defaults; set it to `none` for the controlled reference. `ORCA_PREP_PROFILE=1` enables stage timings, `ORCA_PREP_VALIDATE_REUSE=1` enables per-operation reference checks, and `ORCA_PREP_VALIDATE_THREADS=1` runs the expensive segmentation thread audit. `ORCA_PREP_CAPTURE=<path-prefix>` writes native binary snapshots of the geometry entering perimeter/fill generation, one file per print object. Reference checks and captures must be off for timing. Do not enable `ORCA_PREP_LEGACY_THREADS=1` except to investigate historical behavior.

Evidence is under ignored `build_logs/preparation_experiments_20261016/`: final runs use `*_final_before_*` and `*_final_after_*`; `final_results.json` contains per-run values and comparisons, environment/background snapshots accompany each run, and the three `claude_*review.json`/determinism transcripts preserve the review. Preliminary timing runs before the immutable-map fix are excluded. Raw projects and G-code are not committed.

| Commit | Change |
|---|---|
| `c294f2b5e9` | Preparation timers and same-input thread audit |
| `72446f6d0a` | Deterministic painted-line, shell-group and slab ordering |
| `66e2897377` | Local-Z and region-installation timers |
| `02635b725a` | Selector reuse and compact strict meshes |
| `d6fb12f40f` | Parallel occlusion and bounded shell-outline reuse |
| `b21f39fbd1` | Exact tracker short-circuits |
| `93a9800635` | Serial rolling fixed-mask experiment |
| `6186e40419` | Region bounds and surface-type reuse |
| `020414d3d6` | Graph allocation reduction |
| `6d601c2bd0` | Bounded parallel Local-Z geometry |
| `ff7bc6b1d9` | Validated CPU paths enabled by default |
| `046ef0c7af` | Immutable mixed-row snapshot and batch-boundary tests |
| `f0d9347251` | Exact preparation-boundary snapshots for diagnostics |

For a rollback, revert dependent commits in reverse order. The normal-default selection is separate from the individual implementation commits.
