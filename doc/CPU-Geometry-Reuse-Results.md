# CPU geometry reuse and slicing results

Date: 2026-10-15. Follow-up to [the shared Vulkan experiment](Vulkan-Shared-Geometry-Experiment.md).

## Changes

* **Classic wall support offsets:** reuse the positive lower-layer offset between wall-width series. Also reuse the already prepared overhang outline when the actual scaled float offset matches exactly. The two original float/double expressions are not assumed equivalent. Width-dependent offsets retain their original calculation.
* **Seam neighborhoods:** borrow cached ordered lists during candidate selection, avoid duplicate map lookups on hits, and collect per-query clock timings only in diagnostic modes. The caller releases the borrowed view before another lookup can evict it. Candidate selection and mutable finalized-perimeter state remain unchanged. CPU caching is now the default; `cpu` still selects the original search path.
* **Shell filtering:** calculate neighboring-layer intersections and internal-volume closing only when the small-fragment predicate needs them. Share immutable layer intersections across eligible material regions.
* **Shell projections:** reuse the original ordered top/bottom projection calculation when the layer and shell settings match. The key includes top/bottom layer counts, thicknesses and external-wall spacing. Region-specific final clipping and regularization remain unchanged. Projection reuse is disabled when interface shells require different source geometry for each region.
* **Empty internal regions:** skip shell projection entirely when a region has no internal, internal-void or internal-solid polygons. Both later clipping expressions would necessarily be empty. This avoids work before it reaches the polygon library.
* **Unnecessary copies:** borrow lower-layer outlines during wall processing instead of returning them by value; remove a duplicate colored-line buffer in paint graph construction and reserve its segment buffer.

Shell caches share a 64 MiB accounted geometry budget, with a 4 MiB limit per entry. Only layers with at least two eligible internal regions retain shared geometry. Entries have one writer per layer; region passes join before the next pass starts. First-fit admission can select different cached layers with different thread schedules, but cached results and traversal order are identical to their CPU references. The cache is released after shell discovery, before the seam cache is created. Accounting excludes allocator overhead and small cache metadata.

## Painted-island experiment

The original experiment constructed separate Voronoi graphs for mixed-color islands. Small fixtures passed, but full-model validation found a geometric difference on layer 1255 of the ordinary Starry Night project. That implementation was removed from the final path.

The retained opt-in shortcut bypasses graph construction only when **every island, including its holes, has a single color**. If any island contains multiple colors, the entire layer uses the original graph calculation. A hole with a different color therefore prevents the shortcut. Nested islands remain separate ExPolygons.

Full-model checks compared all 35 multi-island layers in each project. None qualified for the restricted shortcut, so this is not a source of the reported Starry Night improvement. Deterministic fixtures exercise uniform islands, differently colored holes, mixed boundaries and islands nested inside holes. The shortcut remains off by default.

A later [U1 project experiment](U1-Paint-Shortcut-Results.md) exercises the shortcut on 41 layers and separately compares complete-slice performance with normal CPU processing and the Vulkan polygon prototype.

## Runtime controls

The CPU improvements below are enabled in a normal release build without extra flags. The Windows `build_release_vs2022.bat slicer` command now also includes Vulkan support by default. It uses a complete `VULKAN_SDK` installation when available; otherwise `scripts/setup_vulkan_compute.ps1` downloads pinned Khronos Vulkan-Headers 1.3.290 and glslang 16.6.0 into the ignored `build-tools/vulkan/` directory. Both archives are checked against fixed SHA-256 hashes before extraction and cached for subsequent builds. These are the same versions used in the validated prototypes. Download or verification failures stop the build rather than silently omitting GPU support. End users need a compatible driver, not the SDK.

In **Preferences → General → Slicing performance**, there are two independent, persistent options, both off by default:

* **Use GPU for Local-Z slicing (experimental)** controls Local-Z path clipping. Its preference is sampled when each G-code export starts; changing it does not modify an active export's context.
* **Use GPU for seam visibility (experimental)** controls candidate visibility scoring in both ordinary and Local-Z slicing. Its preference is sampled once per object's visibility calculation. This is separate from seam-neighbor prefetch: [the visibility experiment](Vulkan-Seam-Visibility-Prototype.md) measured 598.738 ms CPU versus 224.601 ms NVIDIA, including setup and transfers, but did not demonstrate a whole-slice speedup. Those stage timings came from a Local-Z project; the same stage also runs in ordinary slicing. The slower GPU neighbor and polygon experiments remain diagnostic-only.

The backend automatically prefers a compatible dedicated GPU and falls back to CPU if initialization or compute fails. Both controls are disabled with an explanation in a CPU-only build. CPU reuse remains enabled regardless of either checkbox. Explicit `ORCA_LOCAL_Z_CLIP` and `ORCA_SEAM_VISIBILITY` environment settings take precedence over the respective GUI preferences and disable those controls for the session. CLI users can continue using these variables (`cpu`, `vulkan`, or `validate`). These UI/build changes do not change the earlier timings or binary hashes recorded in this report.

For a deliberately CPU-only Windows release, set `$env:ORCA_ENABLE_VULKAN_COMPUTE = 'OFF'` before running the build helper; this skips the downloads entirely. `AUTO` (default) and `ON` both provision tools in the Windows helper. Direct CMake builds still use tool detection for `AUTO`, require tools for `ON`, and omit GPU support for `OFF`. A preference cannot add GPU code to an existing CPU-only package: rebuild and restart the application to use the new controls.

The preference integration was checked with a successful Release application build and **254,801 assertions in 30 cases**, including per-export preference sampling and explicit environment overrides. Separate build probes compiled `AUTO` with and without tools and explicit `OFF`; explicit `ON` without tools and invalid modes produced the expected configuration errors. An isolated GUI instance verified the dark-theme control, saved value and persistence after an application restart. Logs are under `build_logs/gpu_preferences/`. These checks validate build selection and preference wiring, not a new performance measurement.

The follow-up packaging/seam-preference fix passed **256,098 assertions in 31 cases**. A fresh tool download verified both pinned hashes, a second setup reused the cache, and a deliberately corrupt archive was rejected before extraction. The exact `build_release_vs2022.bat slicer` command completed with Vulkan ON and installed the updated package in `build/Snapmaker_Orca/`. Small independent AUTO/OFF build probes checked preference selection, an actual NVIDIA shader dispatch using the downloaded tools, and CPU-only fallback. An isolated GUI instance verified that both controls were enabled and the seam preference remained checked after a restart. Evidence is under `build_logs/gpu_preferences_followup/`. No new slicing timings or algorithm changes are claimed by this follow-up.

The remaining controls are diagnostic environment variables:

| Variable | Default | Alternatives |
|---|---|---|
| `ORCA_SEAM_ALIGNMENT` | `cache` | `cpu`, `profile`, `vulkan`, `validate`; existing meanings retained. Detailed per-query timings are collected only in `profile` / `validate`, reported by `detailed_timing`. |
| `ORCA_WALL_SUPPORT` | reuse | `cpu` recomputes offsets; `validate` compares reused offsets with the original ordered polygons. |
| `ORCA_SHELL_VOLUME` | reuse | `cpu` prepares original filter inputs eagerly; `validate` also compares the complete filtered shell result with the original predicate. |
| `ORCA_SHELL_PROJECTION` | reuse | `cpu` recomputes projections; `validate` compares each hit with the original ordered shell/hole polygons. |
| `ORCA_PAINT_ISLANDS` | `cpu` | `islands` enables the restricted shortcut; `validate` compares per-color geometry in both directions. |

The empty-region early exit and copy removals have no runtime switch. Use their commits for rollback. No Vulkan path is enabled by default. Existing Local-Z Vulkan clipping is held constant during the comparisons below.

## Validation and benchmark protocol

The checkpoint includes the existing gradient and prime-tower fixes, followed by three commits that preserve the previously uncommitted Vulkan work. The new baseline is rebuilt from that checkpoint; older experiment timings are not substituted for this baseline.

Both baseline and optimized timing runs explicitly enable the previously available CPU seam cache, so the comparison measures additional improvements rather than counting its earlier gain again. They use `ORCA_LOCAL_Z_CLIP=vulkan`, `ORCA_VULKAN_DEVICE=NVIDIA`, `ORCA_SEAM_VISIBILITY=cpu`, `ORCA_POLYGON_COMPUTE=cpu`, and `ORCA_PAINT_ISLANDS=cpu`. Expensive reference validation and extra geometry captures are disabled for timing; lightweight process counters remain enabled. Every slice uses a fresh process and output/data directory. The open GUI instances remain untouched. Timing jobs run sequentially without competing builds or tests.

Machine: Windows, Intel Core Ultra 9 285H, 16 logical processors, NVIDIA GeForce RTX 5060 Laptop GPU. Existing power settings are unchanged and clocks are not locked.

Inputs:

* Local-Z Starry Night: SHA-256 `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2`.
* Controlled ordinary copy: SHA-256 `1fba784fec8ae005528cac2573a61ab9a3d0eba0aaaa4a5eaefa9ffdaed6449b`. Painting and mixed materials remain present; this is not a single-material workload.

The focused tests cover cache eviction and borrowing, exact wall offsets for several nozzle values including differing float/double expressions, clipping and Local-Z regressions, painted islands, and full slicing of adjoining material regions with interface shells on/off.

The final focused suite passed **254,791 assertions in 29 cases** (`tests_reuse_9.log`). The adjoining-material fixture requires two distinct printing regions and compares all final internal/void/solid fill polygons between the original and validating implementations, with interface shells both enabled and disabled. The application built successfully in `build_reuse_7.log`; the final test-only rebuild is `build_tests_9.log`.

`validate_plain_3` and `validate_local_3` completed successfully with the shared-budget implementation. They checked 2,807,791 / 5,871,253 cache hits and 262,631 / 401,028 usable GPU neighborhood results against ordered CPU results. Eight / ten unsupported queries retained the original CPU search. Shell projection validation checked 82 / 141 actual reuse hits. Both runs also checked wall-offset reuse and the complete shell-filter predicate. The later admission refinement avoids retaining geometry for layers without a second eligible internal region; the final adjoining-material fixture tests that refinement. It does not change cache keys, cached geometry or the underlying calculation.

The final application DLL is `d9ca0407633f328ab4945b0b99018962b493ca7a60f0a2586168dd93326cf25f`; the preserved checkpoint DLL is `75ea900e66de5dd3637284c651a14d18f0c04f82f9cd5c5e54b6201de9af289d`. The earlier shared-budget diagnostic DLL was `3455b79608ee1e76bbdce5606d5f8505939c2efa0f68c15e5e8ea2b82ba07bcd`. Do not substitute diagnostic durations for final timing runs.

Full-model diagnostic runs compare reused geometry with original calculations and preserve ordered seam-neighbor lists. Earlier failures and superseded builds are retained in the logs. Complete G-code comparison is a separate check: previous unchanged baseline repeats already exhibit output differences, so similar totals or matching Local-Z diagnostics alone do not establish complete output equivalence. Physical printer validation is not part of this experiment.

## Measurements

Two fresh-process runs per build and mode, using the Starry Night projects above. Times are seconds; the headline is the complete CLI process, including project loading, slicing, export and shutdown. These results cover one complex painted model on one Windows machine, not a representative benchmark suite.

| Mode | Baseline median (range) | Optimized median (range) | Time saved | Reduction |
|---|---:|---:|---:|---:|
| Ordinary | 112.30 (112.06–112.54) | 95.34 (94.83–95.85) | 16.96 | **15.1%** |
| Local-Z | 155.41 (154.61–156.22) | 136.63 (135.85–137.40) | 18.79 | **12.1%** |

Total process CPU time, summed across threads, fell from **708.96 to 498.83 seconds (29.6%)** for ordinary slicing and from **775.12 to 559.67 seconds (27.8%)** for Local-Z. CPU seconds are not additional elapsed seconds.

The slicing interval before G-code export fell from 78.75 to 62.10 seconds for ordinary slicing and from 92.10 to 74.20 seconds for Local-Z. Median broad stage intervals from log timestamps:

| Stage | Ordinary before | Ordinary after | Local-Z before | Local-Z after |
|---|---:|---:|---:|---:|
| Mesh slicing | 1.40 | 1.43 | 1.44 | 1.44 |
| Painting / preparation before walls | 27.08 | 26.88 | 36.46 | 36.15 |
| Walls | 15.44 | 13.32 | 19.17 | 16.00 |
| Surface classification / preparation | 4.13 | 4.18 | 4.48 | 4.52 |
| Vertical shells | **16.74** | **2.66** | **16.26** | **2.03** |
| External surfaces / infill preparation | 1.13 | 1.09 | 0.64 | 0.61 |
| Support interval | 11.42 | 11.14 | 11.30 | 11.06 |
| Post-support / tower interval | 1.41 | 1.41 | 2.36 | 2.38 |
| G-code export | 26.51 | 26.30 | 56.19 | 55.12 |

These intervals include work between the named log markers, not isolated function samples. Per-stage medians need not sum exactly to the median process duration. Seam alignment is already included in export: its direct timer fell from **2.91 to 2.66 seconds** ordinary and **5.72 to 5.25 seconds** Local-Z. Most of the overall gain comes from avoiding shell projection for empty internal regions, followed by wall reuse. Paint and export remain substantial costs; the restricted island shortcut contributed no gain on this model.

The final shell cache reported 82 reuse hits ordinary and 141 Local-Z, retaining only **3.84 / 3.88 MiB**. The admission refinement reduced retained geometry from almost 64 MiB in the diagnostic build while preserving those hit counts.

This is a CPU-time improvement, not a demonstrated peak-memory reduction. Median peak working set was 5,403 → 5,451 MiB ordinary and 5,483 → 5,606 MiB Local-Z. Median peak process commit was 8,198 → 8,160 MiB ordinary and 8,277 → 8,381 MiB Local-Z. These are whole-process peaks, including project loading, and should not be confused with the retained shell cache or an open GUI's steady-state memory.

## Output comparison and limits

All eight timing slices completed successfully. `compare_plain.json` and `compare_local.json` record complete output comparisons using `scripts/compare_cpu_slicing.py`, which normalizes only the generated-at timestamp. **Neither comparison passed byte-for-byte equality.** Both unchanged baseline repeats also differ from each other, as do both optimized repeats, including motion/extrusion commands. This establishes existing nondeterminism; it does not prove that every before/after difference is harmless.

* Ordinary total filament was 235.25 g in both baseline runs and 235.25–235.26 g after optimization. Print estimates ranged from 2d 1h 48m 29s to 2d 1h 48m 55s across all runs.
* Local-Z total filament was 292.97–292.99 g in both groups. Print estimates ranged from 2d 9h 48m 0s to 2d 9h 48m 14s across all runs.
* All Local-Z runs had the same 998 wipe-tower dependency schedule records and the same schedule hash. They also had the same 256 `no path passes` messages. The checked sequence-mismatch, reserve-exhaustion, unreconciled-transition, clip-leak and pass-height-mismatch messages were absent in all runs.
* The positive-extrusion command hashes for bridges, internal bridges, top surfaces and custom commands matched across all runs within each mode. Ordinary prime-tower commands also matched. Other roles differed; per-role hashing is a diagnostic, not a modal geometry-equivalence test.

The exact reference checks and deterministic fixtures described above are therefore the correctness evidence for the changed algorithms. Full generated-output equivalence and physical print validation remain unproven. Both comparison tools return exit code 1 for the recorded inequality; this is retained rather than relabeled as a passing comparison.

Raw evidence belongs under ignored `build_logs/cpu_reuse_optimization/`: preserved baseline/final DLLs, build and test logs, per-run settings, process counters, seam reports, complete G-code, and comparison reports. `run.py` starts isolated slices and `collect.py` extracts stage boundaries and direct seam timers.

The successful final build and focused test commands were:

```powershell
cmake --build build-dev-release --config Release --target libslic3r_tests Snapmaker_Orca_app_gui --parallel 8
& build-dev-release/tests/libslic3r/Release/libslic3r_tests.exe '[CPUSlicing],[ClipperUtils],[LocalZ],[VulkanCompute]'
```

## Rollback

The pre-optimization checkpoint is `be55f05ae9`. Its preceding commits `b6f5376796` and `2700657c53` preserve the shared Vulkan backend/visibility and Local-Z clipping work respectively.

Each optimization/refinement has a separate commit. Later refinements can depend on earlier helpers; revert dependent commits newest first. Reverting the CPU changes does not require discarding the preserved Vulkan experiments.

| Commit | Change |
|---|---|
| `57b9c8fe85` | Reuse wall-support offsets between widths. |
| `c71e420763` | Borrow cached seam lists and reduce lookup overhead. |
| `071048ffce` | Lazy shell filter inputs and shared layer volumes. |
| `df1cce55d6` | Initial opt-in painted-island experiment. |
| `854e096b95` | Bounded shell-projection sharing and multi-material fixture. |
| `0e8200095c` | Skip shell work for regions without internal fill. |
| `08e67e7f9c` | Reuse the prepared overhang outline only for equal scaled offsets. |
| `a080237995` | Restrict the paint shortcut after the full-model validation failure. |
| `2dc608e57e` | Remove wall-outline and paint-input copies. |
| `bf76803c0d` | Enable CPU seam caching by default and record benchmark modes. |
