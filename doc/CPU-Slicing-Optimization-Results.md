# CPU slicing optimization review and benchmark

This review targets the CPU implementation in this worktree. It does not implement the optional Vulkan design in [Vulkan-Compute-Acceleration-Design.md](Vulkan-Compute-Acceleration-Design.md).

## Implemented changes

1. **Borrow extrusion leaves for read-only work.** `ExtrusionEntityCollection::flatten(false)` clones every leaf and its geometry. A new `flatten_refs()` returns const pointers in the same traversal order. Local-Z clipping, height inspection, wipe-tower eligibility, pointillism splitting and multi-perimeter assignment now use the borrowed traversal. Output collections still own their generated paths. References never escape the lifetime of the source collection.
2. **Parallelize independent mixed-filament channels.** Painted-component activation tracking now assigns independent channels to TBB workers, while processing layers sequentially within each channel. Trackers borrow immutable segmentation masks and reuse their small metadata buffers. Splits inherit the existing activation start; merges inherit the earliest overlapping start; empty layers reset the run. Channel recognition happens before entering the parallel region.
3. **Make Local-Z diagnostic files opt-in.** Normal slicing previously serialized a plan JSON and potentially thousands of SVG files. Set `ORCA_LOCAL_Z_DEBUG=1` before launching the application to retain this developer diagnostic. This does not control printer output or the application's preview.
4. **Remove unnecessary geometry copies.** Tree-support settings inspection, bridge candidate grouping, polyhole grouping and conical-overhang hole inspection now iterate by const reference. The geometry operations themselves retain their original order and precision.
5. **Fix the CLI benchmark entry path.** Mixed-filament expansion in `PartPlate::get_extruders_under_cli` previously accessed `GUI_App` without a GUI instance and crashed on these projects. It now reconstructs mixed IDs from the project's saved physical filaments and recipes. Both benchmark builds include this fix; its effect is excluded from optimization credit.

## Review of remaining CPU work

| Area | Existing behavior and dependency | Decision |
|---|---|---|
| Triangle/plane intersections | Multi-plane slicing already transforms vertices once and limits each triangle to intersecting Z planes. Segment appends use 64 striped locks. | Defer thread-local batching: the first Starry run reached the subsequent overhang pass in about 1.4 seconds. Additional temporary storage and ordering changes need a mesh-dominated benchmark. |
| Painted-region geometry | Projection, region construction and several layer passes already use TBB. Polygon topology and Voronoi work remain costly. | Preserve existing geometry algorithms. A spatial index for component overlap is a possible next step if profiles show enough components to amortize building it. |
| Mixed activation tracking | A channel depends on its own preceding layer, not on another channel. Source segmentation remains immutable throughout tracking. | Parallelize channels, preserve layer order and borrow masks. Do not parallelize layers with unresolved activation state. |
| Local-Z planning | Cadence, gradient progression and dependency groups propagate through layers. | Retain ordered propagation. Remove diagnostic serialization from normal execution. A future split into parallel geometry preparation and ordered cadence resolution needs separate correctness tests. |
| Walls, fill and shells | Many inner layer loops already run in TBB; some top-level object loops remain serial. | Do not add unrestricted nested object parallelism. Shared caches, shared-object handling, cancellation, progress reporting and peak memory need auditing first. |
| Tree support | Collision/avoidance caches and much geometry work are parallel; branch growth has cross-layer dependencies. A grain expression currently evaluates to 1. | Remove the verified outline copy. Defer task-size tuning: grain size 1 does not mean one scheduled task per layer, and a guessed replacement is not evidence of a speedup. |
| Wipe-tower planning | Local-Z pass eligibility repeatedly inspects extrusion geometry. Tool changes depend on the active tool and pass dependencies. | Borrow extrusion leaves; preserve ordered tool scheduling. |
| G-code generation | An existing TBB pipeline overlaps stages, but generation, cooling, pressure/fan processing and output each preserve stream order. Travel position, seam choices and active tool carry between layers. Numeric parsing already uses `fast_float` in the main reader. | Remove read-only cloning. Do not make `process_layer` a parallel filter or replace the parser without a demonstrated bottleneck. |
| Allocator behavior | Polygon buffers and many temporary paths allocate frequently; previously observed allocator-retained memory is not itself a CPU profile. | Reduce unnecessary ownership and copies. Do not trim allocators during slicing or add broad caches that increase peak memory. |

This is a source audit plus stage-level measurement, not a function-level sampling profile. A stage duration cannot be attributed entirely to one helper inside that stage.

The subsequent [function-level CPU profile](CPU-Slicing-Profile.md) now identifies the dominant clipping call sites, per-stage CPU utilization and the remaining optimization priorities.

## Benchmark protocol

- Machine: Intel Core Ultra 9 285H, 16 logical processors, 64 GB RAM, Windows; VS 2022 Release build; Windows Balanced power plan. No CPU affinity or power-plan changes were made.
- Baseline: commit `9a791daff1` plus the common CLI loading fix described above.
- Input: an exact copy of `C:\Users\radug\Downloads\STARRY NIGHT PRINT.3mf`; SHA-256 `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2`.
- Use the saved project settings: Snapmaker U1, 0.4 mm nozzle, 0.08 mm nominal layers, tree supports, four physical FullSpectrum filaments and saved mixed recipes. Individual recipes enable Local-Z processing even though the global mode is off. Output reports 1,255 layers.
- Preserve separate baseline and optimized runtime directories; run one slice at a time, with no concurrent compilation. Each run starts a fresh process with a fresh data/output directory. Leave the open GUI instance alone.
- Raise only CLI triangle/time limits to allow the complex project to complete. Do not change geometry, print quality or mixing settings. Confirm the log reports `use_cache=0`.
- Measure process lifetime with a monotonic clock, CPU time with `GetProcessTimes`, working set/commit with `GetProcessMemoryInfo`, and stage times from existing log timestamps. The 0.5-second polling interval limits process wall-time precision; memory peaks are the largest OS peak counters observed during polling.
- The end-to-end result includes loading, slicing, G-code generation/processing and cleanup. It excludes GUI/OpenGL preview construction. These are warm filesystem-cache runs, not controlled cold-cache measurements.
- Compare the full G-code after normalizing only the generated-at timestamp. Retain motion, extrusion, ordering, settings, print estimates and other comments in the hash. Repeated baseline output must also agree before claiming unchanged output.

Example commands, from the repository root:

```powershell
python scripts/benchmark_cpu_slicing.py --exe build_logs/cpu_optimization_benchmark/baseline_runtime/snapmaker-orca.exe --project build_logs/cpu_optimization_benchmark/inputs/starry.3mf --output build_logs/cpu_optimization_benchmark/baseline_starry_new
python scripts/benchmark_cpu_slicing.py --exe build_logs/cpu_optimization_benchmark/optimized_runtime/snapmaker-orca.exe --project build_logs/cpu_optimization_benchmark/inputs/starry.3mf --output build_logs/cpu_optimization_benchmark/optimized_starry_new
python scripts/compare_cpu_slicing.py --before build_logs/cpu_optimization_benchmark/baseline_starry_new --after build_logs/cpu_optimization_benchmark/optimized_starry_new --output build_logs/cpu_optimization_benchmark/comparison_new.json
```

Raw logs, binaries, inputs, G-code and process samples belong in ignored `build_logs/cpu_optimization_benchmark/`, not in Git. Original downloaded projects remain unchanged.

## Results

Four complete fresh-process slices ran in baseline / optimized / baseline / optimized order. The source and settings stayed fixed throughout the repeat runs. Values below are seconds; the median of two runs is their midpoint.

| Metric | Baseline median (range) | Optimized median (range) | Result |
|---|---:|---:|---|
| Full CLI run | 320.42 (319.05–321.78) | 312.04 (311.54–312.53) | **8.38 s / 2.62% faster** |
| Slicing, before G-code export | 129.62 (128.00–131.24) | 121.30 (120.92–121.67) | **6.42% faster** |
| Mesh / paint / Local-Z preparation, a subset of slicing | 48.52 (48.01–49.03) | 38.39 (38.35–38.42) | **20.88% faster** |
| G-code generation and processing | 183.71 (183.66–183.76) | 183.65 (183.55–183.76) | Effectively unchanged |
| Aggregate process CPU time | 933.73 (928.59–938.88) | 934.18 (929.63–938.73) | Effectively unchanged (+0.05%) |
| Peak working set, MiB | 5,496 (5,392–5,601) | 5,442 (5,399–5,485) | Ranges overlap |
| Peak private commit, MiB | 8,296 (8,253–8,339) | 8,206 (8,160–8,252) | Small change; do not generalize from two runs |

The visible benefit is shorter elapsed slicing time, not a demonstrated reduction in total CPU consumption. The roughly 10-second early-stage improvement becomes an 8-second overall improvement. G-code export remains about 59% of optimized elapsed time and showed no measurable gain from removing extrusion clones on this project. Keep the ownership cleanup because it removes verified unnecessary work without changing the clipping algorithm, but do not attribute an export speedup to it.

Normal Local-Z diagnostic output fell from **2,849 files / about 181 MiB per slice to zero**. This is a direct, repeatable reduction. Stage measurements combine the changes; no individual optimization was isolated in an ablation benchmark.

| Run | Full run | Slicing | Export | CPU time |
|---|---:|---:|---:|---:|
| Baseline 1 | 319.05 | 128.00 | 183.76 | 928.59 |
| Optimized 1 | 312.53 | 121.67 | 183.55 | 938.73 |
| Baseline 2 | 321.78 | 131.24 | 183.66 | 938.88 |
| Optimized 2 | 311.54 | 120.92 | 183.76 | 929.63 |

The preserved DLL SHA-256 values are:

- Baseline: `87f5007d5fa0b119049522c1b559e07c52fa58e6a8f7f87f079d56bb8162a666`.
- Optimized: `a2ad2ad01995712d8f0f27dcdb090ea603d79e750d64a4a0a9e8cddb3fa8ee99`.

This is a small, single-machine sample on one complex project. Both optimized times are below both baseline times, but these results are not a general promise for all printers, settings or models. The U1 Chinese-named project was inspected/prepared but was not included in the measured comparison. Raw measurements and detailed output diagnostics are in `build_logs/cpu_optimization_benchmark/comparison_final.json`.

### Output comparison

**Full normalized G-code equality did not pass.** Repeating the unchanged baseline also fails equality: after ignoring progress commands for diagnosis, its first motion difference is in a support-interface path at line 1,501. The first baseline/optimized motion difference is a support path at line 1,826. Per-role diagnostics show small wall and infill variations too, including between baseline repeats. It would be incorrect to describe these as timestamp-only differences or to claim exact geometric equivalence.

The comparison script retains the strict hash check and returns a nonzero exit status for these differences. Its additional per-role hashes cover positive-E coordinate commands; they are diagnostic summaries, not a modal G-code parser or geometric-equivalence proof.

- All four runs report the same Local-Z warning counts: 348 preplanned sequence mismatches, 490 exhausted reserves, 93 unreconciled trailing transitions, zero base-clip leaks and zero local-pass height mismatches. Each reports 256 layers with no emitted path passes.
- All four runs have the same sequence of 874 logged Local-Z wipe-tower pass schedules, including starting tool, pass count, chain count and tool-change count.
- Baselines estimate 279.07–279.09 g and 2d 8h 42m 1s–16s. Optimized runs both estimate 279.09 g, with 2d 8h 41m 49s–2d 8h 42m 15s. These totals are close; they do not prove identical toolpaths.
- The files contain about 8.75 million lines. Original project bytes, embedded settings, layer count and print-quality parameters are unchanged.

Acceptance here rests on the targeted deterministic tests, the ownership/dependency audit, successful complete slicing and the repeat-run diagnostics. Existing output nondeterminism and Local-Z warnings remain unresolved; physical printer correctness has not been established.

### Next CPU work

The largest remaining target on this project is **ordered Local-Z G-code generation and processing**, not triangle intersection. The next experiment should collect function-level CPU/allocation samples during export, separating seam placement, repeated mask clipping, path ordering, formatting and downstream filters. Then evaluate sharing or preparing immutable compensated masks between wipe-tower analysis and export with a bounded memory budget. Preserve the ordered active-tool, travel and cadence state. Do not assume that adding more threads or porting the small triangle-intersection stage will deliver a large end-to-end gain.

## Regression checks and existing limitations

The Release application and test target built successfully. The four new CPU tests passed all 27 assertions. The targeted non-hidden suite passed with 245 passing cases and two explicitly expected failures (114,762 assertions total):

```powershell
build-dev-release/tests/libslic3r/Release/libslic3r_tests.exe '[CPUSlicing]~[.],[ProductPort]~[.],[MixedFilament]~[.],[LocalZ]~[.],[PreviewMemory]~[.]' --reporter compact
```

An initial explicit tag selection also included the hidden `config_extruder cascade: per-deletion decrement under-counts cascade rows (CURRENT BUG)` reproduction. Its known assertion fails; its test and production remapping code were not changed here. The normal suite excludes hidden reproductions, and the two `!shouldfail` cases remain reported as expected failures. Logs for both selections are retained.

The Starry baseline completes but reports Local-Z wipe-tower sequence mismatches, exhausted tool-change reserves and unreconciled trailing transitions. Benchmark validation compares these warning counts as well as output. The reported checks do not certify the pre-existing Local-Z printer behavior. No physical printer validation was performed.
