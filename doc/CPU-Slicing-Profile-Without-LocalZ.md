# Starry Night CPU profile with Local-Z / SML disabled

On the same optimized Release build, disabling Local-Z reduced the profiled run from **309.62 to 117.25 seconds**: **192.37 seconds saved, 62.1% less elapsed time, a runtime ratio of 2.64:1**. G-code export fell from **183.09 to 30.71 seconds**; wipe-tower preparation and slicing finalization fell from **28.60 to 1.47 seconds**.

This compares two different feature configurations, not an optimization producing equivalent output. The original project's gradient recipes activate Local-Z even though its global SML switch is already off. Disabling Local-Z therefore also requires disabling those gradients. The model, painting, recipe components/weights, printer, quality settings and supports remain the same.

The earlier profile and sampling methodology are in [CPU-Slicing-Profile.md](CPU-Slicing-Profile.md).

## Exact configuration change

The benchmark input is a separate copy, `build_logs/cpu_slicing_profile/inputs/starry_no_local_z.3mf`. The original downloaded project and open GUI instance were not changed.

| Setting | Earlier profile | Disabled profile |
|---|---|---|
| Global `dithering_local_z_mode` / SML | `0` | `0` |
| Global `mixed_filament_gradient_mode` | `0` | `0` |
| Gradient flag in mixed recipe rows 1, 2 and 3 | `r1/0.8000/0.2000` | `r0/0.8000/0.2000` |
| All ten recipes' IDs, components, weights and remaining fields | Original | Preserved |

`mixed_filament_definition_uses_local_z` in `src/libslic3r/MixedFilament/Common.cpp` enables Local-Z when either the global flag or the individual recipe's gradient flag is enabled. Turning off the global flag alone would reproduce the previous workload. The three recipe flags are the only semantic project changes; other Local-Z parameters remain stored but dormant.

[prepare_no_local_z_profile.py](../scripts/prepare_no_local_z_profile.py) creates the copy and records a manifest. It verifies that every uncompressed ZIP member except `Metadata/project_settings.config` is byte-identical, and that the source file's hash remains unchanged.

- Original project SHA-256: `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2`.
- Disabled copy SHA-256: `1fba784fec8ae005528cac2573a61ab9a3d0eba0aaaa4a5eaefa9ffdaed6449b`.
- Both DLLs: `a2ad2ad01995712d8f0f27dcdb090ea603d79e750d64a4a0a9e8cddb3fa8ee99`.
- Same machine, power plan, CLI arguments, Release PDB and sampler; fresh process/data/output directories, `use_cache=0`.

## Measured comparison

Times are elapsed seconds from the two profiled runs. Average busy cores are derived independently from process CPU counters, interpolated at stage boundaries.

| Phase | Local-Z active | Local-Z disabled | Seconds saved | Busy cores, active / disabled |
|---|---:|---:|---:|---:|
| Loading and configuration | 5.20 | 5.24 | -0.04 | 0.95 / 0.90 |
| Initial mesh slicing | 1.53 | 1.45 | 0.08 | 8.00 / 8.22 |
| Overhang / paint / slicing preparation | 36.75 | 27.00 | 9.75 | 4.84 / 6.20 |
| Walls | 19.35 | 15.57 | 3.78 | 8.50 / 8.24 |
| Solid surface detection | 4.67 | 4.31 | 0.36 | 12.22 / 12.28 |
| Vertical shells | 16.71 | 17.86 | -1.15 | 11.97 / 12.16 |
| Remaining external surfaces, bridges and infill | 0.64 | 1.16 | -0.52 | 4.42 / 7.82 |
| Tree support | 11.09 | 10.86 | 0.23 | 4.87 / 5.01 |
| Wipe tower and slicing finalization | **28.60** | **1.47** | **27.13** | 1.30 / 7.20 |
| G-code export | **183.09** | **30.71** | **152.39** | 1.13 / 1.86 |
| Cleanup | 1.99 | 1.63 | 0.37 | 0.30 / 0.28 |
| **Total** | **309.62** | **117.25** | **192.37** | **2.96 / 6.09** |

Rounding can affect displayed differences. Phase names denote existing log windows, not separately instrumented timers for every internal operation. Small differences outside the major hotspots mix configuration-dependent geometry with normal run variation.

![Stage comparison](../build_logs/cpu_slicing_profile/local_z_comparison/local_z_comparison.png)

Export and wipe-tower/finalization account for **93.3% of the elapsed-time reduction**. Aggregate process CPU time drops from **917.88 to 714.09 CPU-seconds**, a **22.2%** reduction. The much larger elapsed-time reduction occurs because the removed work was predominantly serial; the remaining polygon stages already use many cores.

The generated G-code grows slightly, from **217.56 to 223.57 MB**. The export reduction therefore is not explained by fewer output bytes. Peak private commit changes comparatively little, from approximately **8,307 to 8,098 MiB**: removing serial computation does not remove the expensive model/paint geometry's peak memory demand.

### Unprofiled timing cross-check

One additional disabled run completed without the sampler, using the same DLL, copied project and CLI arguments:

| Metric | Earlier unprofiled Local-Z-active runs | New unprofiled disabled run |
|---|---:|---:|
| Process lifetime | 311.54-312.53 s | **114.85 s** |
| Slicing before export | 120.92-121.67 s | **77.89 s** |
| Export | 183.55-183.76 s | **30.17 s** |
| Aggregate CPU time | 929.63-938.73 CPU-s | **706.42 CPU-s** |

This independently confirms the large difference; it is not a statistical benchmark over many runs. The disabled profile was about 2.4 seconds longer than the unprofiled disabled check, combining sampling effects with normal run variation. The check also reports zero Local-Z schedules or warnings. Its G-code contains 8,835,621 lines, approximately 223.56 MB, and estimates 235.26 g / 2d 1h 48m 55s, further illustrating that this is a changed feature/output configuration.

## Verification that Local-Z is inactive

| Evidence in the completed run | Active | Disabled |
|---|---:|---:|
| Local-Z plans built | 1 | 0 |
| Local-Z phase-b preparation messages | 1,254 | 0 |
| Local-Z phase-b emission messages | 998 | 0 |
| Local-Z wipe-tower dependency schedules | 874 | 0 |
| Preplanned sequence mismatch warnings | 348 | 0 |
| Tool-change reserve exhausted warnings | 490 | 0 |
| Unreconciled trailing transition warnings | 93 | 0 |

The disabled profile also contains zero sampled calls to `clip_extrusion_collection_for_local_z`, `append_clipped_path`, `local_z_compensate_masks`, `extrusion_collection_has_local_z_segment` or `collect_local_z_wipe_tower_toolchanges`. Configuration inspection, log evidence and call-stack evidence agree. Absence from samples alone would not prove that a very short function never ran.

Both slices complete successfully. Existing Voronoi warnings still occur. This comparison intentionally produces different paths and gradient behavior, so G-code equality is not an acceptance criterion.

## What is expensive without Local-Z

The largest remaining elapsed-time stages are export (**26.2% of the run**), paint/slicing preparation (**23.0%**), vertical shells (**15.2%**), walls (**13.3%**) and supports (**9.3%**).

**Painted-region preparation:** `build_graph` accounts for 33.9% of this phase's weighted running-thread samples; colored-line Voronoi construction for 31.8%; Clipper execution for 29.4%. The graph and Voronoi figures overlap. Turning off Local-Z does not eliminate segmentation of the painted mesh.

**Vertical shells:** Clipper execution appears in 82.4% of phase samples, intersection in 58.8%, and union in 32.8%. This phase averages 12.16 busy cores. Reducing redundant polygon operations is a better starting point than simply adding threads.

**Walls:** Classic perimeter generation appears in 95.4% of phase samples. Lower-layer polygon-series generation accounts for 47.3%, and Clipper offset execution for 65.8%. Reusing equivalent lower-layer offsets and reducing repeated polygon preparation remain relevant without Local-Z.

**Supports:** Clipper execution appears in 71.0% of phase samples. This phase remains about 11 seconds and is not responsible for the large active/disabled difference.

Across the complete disabled run, **56.1% of weighted leaf samples are directly in `deps_src/clipper/clipper.cpp`**, versus 60.3% with Local-Z. This is exclusive source-file attribution; it does not double-count calling functions. Polygon booleans and offsets remain the dominant aggregate CPU work.

### Remaining export work

The table uses inclusive shares of export's weighted running-thread samples. Parent/child functions overlap, and a larger percentage in the disabled run does not imply more absolute work: the export CPU total shrank from 206.65 to 57.24 CPU-seconds.

| Function | Active share | Disabled share |
|---|---:|---:|
| Local-Z extrusion clipping | 56.2% | 0.0% |
| `GCode::_extrude` | 5.5% | 17.9% |
| `SeamPlacer::init`, calling thread and descendants | 6.9% | 14.4% |
| `GlobalModelInfo::calculate_point_visibility` | 3.4% | 12.3% |
| `GCodeProcessor::process_buffer` | 4.0% | 14.0% |
| `GCodeProcessor::finalize` | 2.0% | 6.5% |
| `GCodeProcessor::process_G1` | 0.7% | 2.6% |

Seam placement and visibility involve KD-tree and AABB-tree queries. Worker callbacks are not children of the scheduling thread's stack, so the `SeamPlacer::init` percentage alone is not all seam-preparation work. Runtime allocation/free samples also remain visible, but this stack sampler is not an allocation-size profiler and does not identify individual retained-memory owners.

For optimizing ordinary mixing, prioritize painted-region topology, repeated wall/shell clipping and offsets, and seam preparation. The Local-Z-specific clipping fixes from the earlier report remain valuable when Local-Z is enabled; they will not accelerate the disabled path.

## Sampling quality and artifacts

The disabled process was PID 22512. The sampler captured **18,763 stacks**, including **14,847 running-state stacks**, in **3,042 batches**. Median sampled-batch spacing was 31.7 ms; the maximum was 152.1 ms. Twenty-two intervals exceeded the analysis's 100 ms weighting cap. Four captures failed, none reached the frame-depth limit, 83.0% of weighted leaf samples had source lines, and 2.1% had wait-like leaf symbols. These are approximate running-thread samples, with the same state-transition and runtime-symbol limitations as the earlier report.

Summed suspension intervals across threads were 11.20 seconds, with a maximum individual pause of 47.8 ms. This sum is not added wall time. A separate unprofiled run is used as a timing cross-check.

Retained local artifacts:

- Disabled profile: `build_logs/cpu_slicing_profile/starry_no_local_z_1/`.
- Comparison JSON and chart: `build_logs/cpu_slicing_profile/local_z_comparison/`.
- Controlled-input manifest: `build_logs/cpu_slicing_profile/inputs/starry_no_local_z.manifest.json`.
- Unprofiled cross-check: `build_logs/cpu_slicing_profile/starry_no_local_z_unprofiled_1/`.

Reproduce with new output names:

```powershell
python scripts/prepare_no_local_z_profile.py build_logs/cpu_optimization_benchmark/inputs/starry.3mf build_logs/cpu_slicing_profile/inputs/starry_no_local_z_new.3mf
python scripts/profile_cpu_slicing.py --exe build-dev-release/src/Release/snapmaker-orca.exe --project build_logs/cpu_slicing_profile/inputs/starry_no_local_z_new.3mf --sampler build_logs/cpu_slicing_profile/sampler.exe --symbols build-dev-release/src/Release --output build_logs/cpu_slicing_profile/starry_no_local_z_new
python scripts/analyze_cpu_profile.py build_logs/cpu_slicing_profile/starry_no_local_z_new --plot --label "Local-Z/SML and recipe gradients disabled"
python scripts/compare_cpu_profiles.py --active build_logs/cpu_slicing_profile/starry_1 --disabled build_logs/cpu_slicing_profile/starry_no_local_z_new --manifest build_logs/cpu_slicing_profile/inputs/starry_no_local_z_new.manifest.json --output build_logs/cpu_slicing_profile/local_z_comparison_new --plot
```

The raw profiles and charts remain ignored local artifacts. No production slicing code was changed for this comparison.
