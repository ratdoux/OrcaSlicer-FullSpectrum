# Local-Z CPU optimization and repeat profile

## Scope

The pre-change worktree was committed as `df79223de4` ("Optimize CPU slicing and add reproducible profiling tools"). The changes described here follow that checkpoint.

The workload is plate 1 of the original **STARRY NIGHT PRINT.3mf**, retained at `build_logs/cpu_optimization_benchmark/inputs/starry.3mf`. Its SHA-256 is `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2`. The first three mixed recipes still enable Local-Z; this is not the Local-Z-disabled variant from the previous comparison.

The complete CLI run fell from **304.1 seconds to 184.1 seconds**, a **39.5% reduction**. Targeted clipping and wipe-eligibility validation passed on the full model. Complete G-code equivalence is **not established**: the output differences described below are larger in internal solid infill than the earlier unchanged-build variation.

## Unprofiled timing results

The before column is one fresh run of the preserved executable. The after column is the median of two runs, **183.26 and 184.89 seconds**. Older runs of the same baseline DLL took 311.5–312.5 seconds; they are not included in this comparison. Neither profiling nor reference validation was enabled in these timing runs.

| Measurement | Before | After | Reduction |
|---|---:|---:|---:|
| Complete CLI run | 304.09 s | 184.07 s | 39.5% |
| G-code export | 181.59 s | 82.25 s | 54.7% |
| Wipe tower and slicing finalization | 28.51 s | 2.47 s | 91.3% |
| Process CPU time, summed across cores | 911.38 s | 820.39 s | 10.0% |
| Mesh, paint and initial Local-Z preparation | 37.30 s | 37.82 s | No improvement |
| Peak working set | 5,448.9 MiB | 5,380.6 MiB | 1.3% |
| Peak commit | 8,092.8 MiB | 8,180.8 MiB | 1.1% increase |

This removes roughly **two minutes** from the complete run. Elapsed time improves more than total CPU consumption because the removed work was concentrated in serial export, and wipe eligibility now uses multiple cores. This is not a memory-reduction result; peak commit is slightly higher and remains dominated by earlier slicing work.

## New profile

![Before and after phase times](../build_logs/local_z_cpu_optimization/phase_elapsed_comparison.png)

The separate profiling run took **188.60 seconds**, compared with **309.62 seconds** in the previous enabled-Local-Z profile. Do not substitute those sampled-run totals for the unprofiled comparison above.

| Phase | Before elapsed | After elapsed | After CPU | After average busy cores |
|---|---:|---:|---:|---:|
| Loading/configuration | 5.20 s | 5.03 s | 4.94 s | 0.98 |
| Mesh slicing | 1.53 s | 1.51 s | 13.04 s | 8.61 |
| Paint and Local-Z preparation | 36.75 s | 37.44 s | 178.52 s | 4.77 |
| Walls | 19.35 s | 21.41 s | 176.50 s | 8.25 |
| Solid surfaces | 4.67 s | 4.83 s | 57.93 s | 11.99 |
| Vertical shells | 16.71 s | 18.01 s | 205.56 s | 11.41 |
| External surfaces, bridges and infill | 0.64 s | 0.66 s | 2.79 s | 4.21 |
| Tree support | 11.09 s | 11.68 s | 55.41 s | 4.74 |
| Wipe tower/finalization | 28.60 s | 2.46 s | 19.41 s | 7.91 |
| G-code export | 183.09 s | 83.59 s | 108.99 s | 1.30 |
| Cleanup | 1.99 s | 1.98 s | 0.60 s | 0.30 |

Function-level estimates below multiply inclusive sampled share by the phase's process CPU time. They are **approximate CPU seconds**, not directly timed elapsed seconds, and inclusive functions must not be added indiscriminately.

| Work | Before estimated CPU | After estimated CPU |
|---|---:|---:|
| Local-Z export clipping | 116.0 s | 36.8 s |
| Local-Z wipe eligibility/preparation | 27.5 s | 9.2 s |
| Seam initialization | 14.3 s | 15.6 s |
| Extrusion emission | 11.4 s | 11.8 s |
| G-code parsing | 8.2 s | 9.2 s |

The Local-Z export-clipping estimate falls by about **68%**. The old leak diagnostic, previously approximately 19 CPU-seconds, is absent from the normal export path. Wipe eligibility uses both less CPU work and more parallelism.

Export is still the largest elapsed stage. Its remaining Local-Z clipping is approximately 34% of export CPU, followed by seam initialization at 14%, extrusion emission at 11%, and G-code parsing at 8%. Across the complete run, Clipper remains about 58% of exclusive source-file samples, largely because painting, walls, and vertical shells still use it. More parallelism has limited headroom in walls and shells, which already average 8–12 busy cores; reducing repeated geometry work is the next CPU target. Further export parallelism would require separating seam-dependent preparation from emission while preserving ordered state.

## Implementation

- `LocalZClipper` prepares polygon bounds once for each immutable mask set. Intersection queries reject disjoint bounds. Other queries borrow complete polygons overlapping the subject's vertical scanline range, including holes, in their original order. The existing integer Clipper implementation and nonzero winding rules still perform the boolean operation. No approximation, vertex reduction, or vendored-library change is involved.
- Export reuses these prepared bounds for all extrusion collections in the current layer. Disjoint intersection queries avoid Clipper entirely. Differences still use Clipper even when no polygons are nearby, preserving its path normalization and orientation. Completed clipped paths are moved into extrusion paths instead of copied.
- Wipe-tower eligibility queries inspect Clipper's result tree directly rather than constructing a second polyline collection. Independent print heights run on the existing TBB pool before the unchanged, sequential tool-order planner consumes their results. Only active workers hold temporary masks; retained results contain pass references and eligible tool IDs, not whole-model clipped extrusion geometry.
- The repeated base-path leak diagnostic is opt-in through `ORCA_LOCAL_Z_VALIDATE=1`. This mode also compares every filtered intersection/difference against the original full-mask operation, checking every point and fragment order, and checks wipe eligibility against full clipping. It additionally recomputes eligibility sequentially at each visited print height and compares it with the parallel result. A mismatch aborts the validation run.

Seam placement, travel planning, tool-order decisions, and G-code emission retain their existing sequential behavior. Clipper edge structures are still built for each query; the reusable preparation here is the mask bounds. Planning and export do not share clipped extrusion results because extrusion simplification occurs between those stages.

The exact-output tests caught why unrestricted two-dimensional polygon filtering is unsafe here: removing a polygon beside a path also removes scanline events, which can alter Clipper's integer rounding. Even vertical filtering can change tied output-fragment ordering. The implementation therefore preserves all relevant scanlines, and falls back to the full-mask operation when fragments share a maximum Y coordinate. This conservative fallback retains the existing output order.

## Measurement method

Use the same isolated CLI command, embedded project settings, Release configuration, and 16 logical processors as the preceding profile. The open GUI and downloaded project remain untouched. Unprofiled runs measure elapsed time and process CPU time; separate process-only sampling runs locate the remaining costs. CPU seconds summed across cores are different from elapsed seconds.

The original executable, DLL, matching PDB, dependency DLLs, and resource junction are preserved in `build_logs/local_z_cpu_optimization/before_runtime/`. The baseline DLL SHA-256 is `a2ad2ad01995712d8f0f27dcdb090ea603d79e750d64a4a0a9e8cddb3fa8ee99`.

Two unchanged-build outputs already differ in normalized full G-code hashes. Their Local-Z schedules and warning counts match exactly; see `build_logs/local_z_cpu_optimization/baseline_repeat_comparison.json`. Full-file equality therefore cannot serve as the only regression criterion. The within-run exact clipping comparisons, deterministic geometry tests, schedules, warnings, and output statistics provide separate checks.

## Correctness checks

- Release application and test targets build successfully.
- The relevant CPU slicing, product-port, mixed-filament, Local-Z, and preview-memory suite reports **248 passed cases and 2 pre-existing expected failures**, with **144,612 passed assertions and 2 expected failures**.
- Three new clipping cases cover holes, boundary and corner touches, empty/degenerate input, duplicate points, path reversal, closed paths, overlapping masks, large coordinates, concurrent queries, and 800 deterministic randomized mask/path sets. Results are compared point-for-point in the original fragment order.
- `validation_starry_2/` completed the entire model using the final DLL (`17f99c61942bd77338ba9b6d76376d2e954aa1674512aefd35e137ccf2bf2295`) and `local_z_validation: true`. Every optimized clipping operation matched full-mask clipping, and every prepared wipe eligibility result matched sequential preparation. This deliberately expensive run is excluded from performance measurements.

Physical printer validation has not been performed. Existing Local-Z wipe-tower scheduling warnings are outside this optimization's scope and must not be mistaken for newly validated printer behavior.

### End-to-end output differences

All compared runs retain **874 Local-Z schedules**, hash `9d71da65ae22d2772f5a8afb62dfc82705c2d7aad7654aaf2cffec854e73e857`. Existing warnings remain 348 preplanned sequence mismatches, 490 exhausted reserves, 93 unreconciled trailing transitions, and 256 no-path passes. The full validation run reports no base-clip leaks or local-pass height mismatches.

Normalized complete G-code hashes differ, including between unchanged baseline runs. However, that earlier variability alone does **not** explain away every new difference:

- The fresh baseline contains 71,455 positive moving-extrusion commands classified as internal solid infill. The two optimized unprofiled runs contain 60,829 and 61,096; the optimized profiled run contains 71,439.
- These files use relative extrusion. Summing positive E on moving commands within that role gives 1,186.225 mm before, 1,164.011/1,166.135 mm in the unprofiled runs, and 1,186.092 mm in the profiled run. Thus the change is not solely a different segmentation of identical E commands: unprofiled runs have about 1.7–1.9% less positive moving extrusion within this category.
- Total reported filament is 279.06 g before and 279.02/279.00 g after, under 0.03% difference overall. Print estimates differ by roughly two to three minutes over a 56-hour print.

The cause of this larger role-specific difference has not been established. The direct checks establish clipping and eligibility equivalence **within the validated execution**, not complete end-to-end equality across differently timed executions. Treat this as an outstanding output-regression investigation before asserting unchanged printed output. The measurements demonstrate a speed improvement, not physical-print validation.

## Retained evidence

All artifacts are under ignored `build_logs/local_z_cpu_optimization/`:

- `before_starry_2/`, `after_starry_1/`, `after_starry_2/`: successful unprofiled timings, logs and G-code. `before_starry_1/` is an aborted runtime-setup attempt and is excluded.
- `validation_starry_2/`: successful full reference validation of the final build. `validation_starry_1/` validates the clipping implementation before eligibility was moved across print heights.
- `timing_comparison.json`: timings, normalized G-code hashes, role command counts, schedules and warning comparisons.
- `role_extrusion_comparison.json`: relative positive moving-extrusion totals by role, including the profiled output.
- `profile_comparison.json`, `phase_elapsed_comparison.png`: phase and function comparisons.
- `after_profile_1/analysis.json`, raw stack/symbol/timeline files, `stacks.folded`, and `cpu_timeline.png`: the new profile and its analysis.
- `tests_final.log`, `build_parallel_heights.log`: final regression and build results.

The new profile captured 20,447 stacks, including 16,111 running-state stacks, across 5,084 batches. Median interval was 31.5 ms; 35 gaps exceeded 100 ms and the maximum gap was 928.5 ms. Analysis caps interval weights at 100 ms. Four captures failed, none reached the depth limit, and 84.5% of weighted leaf samples have source attribution. The largest individual thread suspension was 54.5 ms. These limits reinforce why function estimates are approximate and unprofiled runs supply the headline timings.

## Reproduction

```powershell
cmake --build build-dev-release --config Release --target Snapmaker_Orca_app_gui libslic3r_tests --parallel 8
build-dev-release/tests/libslic3r/Release/libslic3r_tests.exe "[CPUSlicing]~[.],[ProductPort]~[.],[MixedFilament]~[.],[LocalZ]~[.],[PreviewMemory]~[.]" --reporter compact

# Use a new output directory for every invocation.
python scripts/benchmark_cpu_slicing.py --exe build-dev-release/src/Release/snapmaker-orca.exe --project build_logs/cpu_optimization_benchmark/inputs/starry.3mf --output build_logs/local_z_cpu_optimization/new_timing_run

# Expensive correctness run; exclude this from speed measurements.
$env:ORCA_LOCAL_Z_VALIDATE = "1"
python scripts/benchmark_cpu_slicing.py --exe build-dev-release/src/Release/snapmaker-orca.exe --project build_logs/cpu_optimization_benchmark/inputs/starry.3mf --output build_logs/local_z_cpu_optimization/new_validation_run
Remove-Item Env:ORCA_LOCAL_Z_VALIDATE

python scripts/profile_cpu_slicing.py --exe build-dev-release/src/Release/snapmaker-orca.exe --project build_logs/cpu_optimization_benchmark/inputs/starry.3mf --sampler build_logs/cpu_slicing_profile/sampler.exe --symbols build-dev-release/src/Release --output build_logs/local_z_cpu_optimization/new_profile
python scripts/analyze_cpu_profile.py build_logs/local_z_cpu_optimization/new_profile --plot --label "Local-Z enabled, CPU geometry optimizations"
```

The sampler's limitations remain those documented in [CPU-Slicing-Profile.md](CPU-Slicing-Profile.md): it is process-only user-mode stack sampling, not ETW. Inclusive function percentages overlap. Use unprofiled timings for speed comparisons and matching PDBs for attribution.
