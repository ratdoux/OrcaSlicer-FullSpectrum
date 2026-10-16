# Painted-island and Vulkan polygon tests on the U1 project

Date: 2026-10-16. Source checkpoint: `4515f26f26`. No slicing code or defaults were changed for this experiment.

**Outcome:** this model exercises the uniform-island shortcut and real GPU empty-result skips in walls/shells, but neither establishes a useful complete-slice improvement. Painting-only Vulkan skips no operations. Keep both experiments opt-in; these measurements do not justify changing defaults.

Input: `C:\Users\radug\Downloads\U1+有换料暂停+打印前必须看说明.3mf`, SHA-256 `8e6e6441decac1408721b30d76597a4d343b81bf06882adb9feeb9ffe69edb8e`. The original file is unchanged; tests use an identical copy named `u1.3mf`. Plate 1 is the project's only plate. The saved configuration includes four physical filaments, twenty mixed-filament definitions, a nominal 0.08 mm layer height, supports and enabled Local-Z processing.

## Protocol

The same existing Release application is used throughout, with DLL SHA-256 `99be853a305ca6a1ffbc37e6d55bfd2ca178b5eef8589face5c24b6867fee234`. Machine: Windows, Intel Core Ultra 9 285H (16 logical processors), NVIDIA GeForce RTX 5060 Laptop GPU, NVIDIA driver `32.0.15.7705`. GPU experiments explicitly select NVIDIA. Existing power settings are retained; clocks are not locked.

Every run uses a fresh CLI process and separate data/output directories. Project settings, geometry and mixing recipes are unchanged. The benchmark raises only CLI triangle/time limits. Final timing slices run sequentially, without concurrent builds, profilers or output comparisons. Existing GUI instances remain untouched; their process CPU increments are recorded around each final timing run.

The user confirmed that a separate GUI slice overlapped the first timing series. **All first-series timings are excluded**, including apparently favorable observations. The original evidence remains under `build_logs/u1_paint_shortcuts_20261016/` with `TIMINGS_INVALID.txt`. Only timings from `build_logs/u1_paint_shortcuts_retry_20261016/` are used below. Geometry validation observations remain applicable; concurrent work affects timing, not the meaning of a completed reference comparison.

Common controls: `ORCA_LOCAL_Z_CLIP=cpu`, `ORCA_SEAM_VISIBILITY=cpu`, `ORCA_SEAM_ALIGNMENT=cache`, `ORCA_TREE_OVERHANG_FILTER=parallel`, and `ORCA_VULKAN_DEVICE=NVIDIA`. Existing CPU wall/shell reuse stays enabled. Local-Z remains enabled by the project; only its separate GPU clipping backend is held off to isolate the requested polygon experiment.

| Mode | `ORCA_PAINT_ISLANDS` | `ORCA_POLYGON_COMPUTE` | `ORCA_POLYGON_STAGES` |
|---|---|---|---|
| Baseline | `cpu` | `cpu` | unset |
| Uniform islands | `islands` | `cpu` | unset |
| Vulkan, painting only | `cpu` | `vulkan` | `paint` |
| Vulkan, all supported stages | `cpu` | `vulkan` | unset (paint, walls, shells) |

Reference-validation and CPU counter runs are separate from performance runs. Painting/preparation duration spans the existing `Make overhang printable...` and `Generating walls...` markers; it includes Local-Z planning and region application, not only Voronoi graph construction. Complete process time includes loading, slicing, G-code export and cleanup, but no GUI preview creation.

## Clean timing results

Three fresh processes per mode. Run order is baseline/islands/paint-GPU/all-GPU, then islands/all-GPU/baseline/paint-GPU, then all-GPU/paint-GPU/islands/baseline. All observations are retained. These are medians and ranges on one model and machine, not confidence intervals or general performance guarantees.

| Mode | Complete process median (range) | Painting/preparation median | Shell stage median | Total process CPU median |
|---|---:|---:|---:|---:|
| Baseline | **38.55 s** (38.13-41.70) | 9.145 s | 0.561 s | 152.67 CPU-s |
| Uniform islands | 39.57 s (38.60-39.76) | 9.058 s | 0.550 s | 152.27 CPU-s |
| Vulkan, painting only | 37.67 s (37.65-39.06) | 9.070 s | 0.542 s | 150.23 CPU-s |
| Vulkan, all supported stages | 40.13 s (38.65-40.66) | 9.099 s | **1.311 s** | 155.00 CPU-s |

The uniform shortcut qualifies on 41 layers, but its approximately 0.087-second difference in median painting/preparation is smaller than baseline variability (8.839-9.886 seconds). Its complete-process median is higher, with overlapping ranges. Neither a reliable speedup nor a general regression is established for this shortcut by three runs.

Painting-only GPU mode dispatches 564/564/565 operations and skips **zero** in every run. Its lower complete-process median must not be credited to polygon acceleration. Export, where this paint hook is inactive, varies from a 17.44-second baseline median to 16.42 seconds in these GPU runs; this unrelated difference alone exceeds the complete-process median difference. The painting/preparation intervals overlap closely.

All-stage GPU mode dispatches 3,162 operations per run and skips exactly **242**: 214 wall operations and 28 shell operations. Shell elapsed time is consistently higher: **1.257-1.345 seconds**, versus **0.548-0.714 seconds** for the CPU baseline. GPU preparation, transfers and synchronization cost more than avoiding those 28 shell operations saves. Wall-stage medians are 3.442 seconds CPU and 3.537 seconds all-GPU. This broader mode demonstrates real skips without a useful net speedup.

The two open GUI processes together accumulate 0.375-4.531 CPU-seconds during each complete timing run (most runs below one CPU-second), compared with roughly 148-156 CPU-seconds in the CLI process. This is consistent with low GUI activity rather than another full slice. It does not eliminate ordinary scheduling, clock or other background variability. Neither GUI was closed or reconfigured.

## Eligibility and validation

The island validation run compares **672 multi-island layers**, with **41 layers taking the new shortcut**. Every checked per-color result passes the two-way polygon-difference comparison against original whole-layer segmentation. This checks geometric equivalence of those regions; it does not require identical polygon/vertex order or prove complete G-code equivalence. Layers containing a mixed-color island retain the original graph calculation. The existing shortcut for an entirely single-color layer is not counted as a new optimization.

The fresh all-stage GPU validation run reports:

| Stage | GPU-tested operations | Empty CPU reference results | GPU-proven empty |
|---|---:|---:|---:|
| Painting | 565 | 0 | 0 |
| Walls | 228 | 214 | 214 |
| Shells | 2,369 | 28 | 28 |

Eligibility is limited to the prototype's supported operation, fill-rule, edge-count and workload-size conditions; the GPU additionally checks supported coordinates and conservative geometric predicates. Many cheap empty operations do not qualify, and an eligible empty result is not necessarily provable by the GPU. Minor call-count differences occur between runs. Profile `cpu_ms` and `prepare_ms` counters sum elapsed time inside concurrent calls, including scheduling and mutex wait; they are neither process CPU-seconds nor serial elapsed-time savings.

The all-stage validation run reports no invalid rejection or backend fallback. Validation executes the CPU operation even for a GPU proof, hence its `skipped` counters are zero by design. A separate painting-only validation dispatched 564 operations and proved zero empty. These checks confirm actual GPU execution and the validity of 242 wall/shell empty proofs, but do not exercise a successful painting skip.

An initial combined diagnostic enabled island reference validation and polygon counters together. Its extra comparison operations contributed **204 eligible empty painting calls**. Those are validation work, not a production opportunity, and are excluded from the table and performance conclusions. The separate CPU counter run and GPU validation establish the normal painting workload's zero eligible empty results. A final `islands_profile` run enables the island shortcut without reference comparisons: it reports **556 eligible painting operations and zero eligible empty results**, confirming that enabling islands does not create the earlier apparent empty-result opportunity either. This counter run is excluded from timing medians.

## Complete-output checks

All twelve retained timing slices exit successfully. They retain the same 716 Local-Z dependency schedule records, with the same schedule hash. All six checked Local-Z warning categories are zero in every run. Every output estimates 160.39 g of filament; estimated print duration varies from 1d 1h 19m 14s to 1d 1h 19m 16s. The same 19 existing support `Not precalculated Placeable areas requested` messages occur in every run, including the baseline.

**Normalized complete G-code equality does not pass.** Each group has three distinct hashes, including the three unchanged CPU baselines. Only the generated timestamp is normalized. Equal schedules and close totals do not prove every difference harmless. The per-region island comparison and GPU rejection reference checks provide narrower correctness evidence; complete-output equivalence and physical printer validation remain unproven.

The original downloaded project and the copied input have identical hashes at the end of the experiment, and the application DLL hash is unchanged.

## Reproduction and evidence

Final raw inputs, exact child-process environment files, commands, logs, G-code, metrics and summaries are under ignored `build_logs/u1_paint_shortcuts_retry_20261016/`. Original validation evidence and excluded timings remain in `build_logs/u1_paint_shortcuts_20261016/`. `run_experiment.py` drives the existing `scripts/benchmark_cpu_slicing.py`; `summarize.py` extracts stage intervals and uses the existing complete-output comparison functions. No mesh, presets, slicer preferences or printer state are changed.

Related experiments: [CPU geometry reuse and the island shortcut](CPU-Geometry-Reuse-Results.md), [shared Vulkan polygon experiment](Vulkan-Shared-Geometry-Experiment.md).
