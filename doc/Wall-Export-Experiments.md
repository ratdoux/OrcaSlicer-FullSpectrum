# Wall generation and G-code export experiments

Date: 2026-10-16. Baseline: `c50411f002`. These experiments follow the painting/preparation work described in [Painting-Preparation-Experiments.md](Painting-Preparation-Experiments.md).

## Scope and method

The targets were lower-layer wall-support offsets, the remaining G-code generation/filter/output pipeline, and other polygon unions/intersections/subtractions. Workloads were the U1 project and Starry Night with and without Local-Z. U1 uses Arachne; the tested Starry Night projects use classic walls.

Each slice ran in a fresh Windows Release process with isolated configuration and output directories, on an Intel Core Ultra 9 285H (16 logical processors) and NVIDIA RTX 5060 Laptop GPU. The existing Vulkan Local-Z clipping and seam visibility options were enabled throughout the main comparisons. New polygon operations stayed on CPU except in explicit replay experiments. Builds, tests and output analysis were not run concurrently with timed slices. Background process CPU use was recorded. End-to-end times still vary, so isolated replay times and whole-slice times must not be conflated.

Evidence, immutable baseline executable, captures, scripts, per-run environments and logs are under `build_logs/wall_export_experiments_20261016/` (ignored). The baseline DLL SHA256 is `e437f540523f179857ee297a2e5e38015f246d306ec41f0398915c0f5f8b6d99`. The first candidate DLL is `252aa8f1c9e91f20cf5f7df8e0440bf7f3e56f4d71d50665174c4577bee005bf`; the second candidate is `2fef78e02eb2766dd2e4da5ddbe535784ba9051bf203f66d903f3d3ba961c860`.

The final retained-source DLL is `3719ee5bc7571734f8f85f66c9bb655a5dda6d0b3dd50c8d1f66ccb17a82892b`.

## Experiments

| Operation | Experiment | Evidence and disposition |
|---|---|---|
| Classic wall support masks | Generate only the consumed positive offset and share it across wall widths | Every consumer uses the final outline. The unused negative offsets and duplicate width variants can be omitted. Classic wall elapsed time consistently fell from about 7.4–7.6 to 4.6 seconds in ordinary Starry Night, and 9.1 to about 5.0 seconds with Local-Z. Does not accelerate Arachne. Retain. |
| Travel containment | Exact indexed segment classification before polygon clipping | Immediate index creation regressed an ordinary slice. Delayed construction still took 2.797 s in build/query/fallback work versus 2.530–2.678 s for original clipping. Removed; retained initialization of the layer-cache pointer and regression coverage. |
| Klipper velocity commands | Replace three repeated regex searches with a grammar-compatible scanner | Replay uses all 602,826 real commands (30,442,513 bytes) from ordinary Starry Night. It includes command processing, excludes loading/extracting the file. First two reference runs took 2.50–2.67 s versus 0.44–0.47 s with the scanner. Malformed commands retain historical matching behavior. Retain. |
| Preview input | Borrow the existing NUL-terminated G-code output buffer | Removes a full temporary string copy per output buffer; preserves parser boundaries and ownership. Retain; no separate end-to-end speed claim. |
| Extrusion quality | Skip unused export curvature, reuse speed sections, move layer indexes | Export uses point positions/distances, not curvature. Support/curl callers still compute curvature. Real captured-path replay and in-slice exact comparisons validate the change. |
| Quality boundary distances | Bounded CPU batch cache | Precompute unique known perimeter/bridge vertices in parallel; clipped/seam/new points fall back. Cap 65,536 points per object/layer and 131,072 total. Retain for Local-Z objects only; ordinary-slicing results were inconsistent. |
| Local-Z export masks | Reuse identical fixed-mask guards | 1,843 cache hits, but mask-context work took 6.854 s versus 6.838 s without caching. Removed. |
| Local-Z export masks | Parallel compensation per filament | No eligible passes with multiple active masks in the tested Local-Z workload; zero parallel jobs. No demonstrated benefit, so removed. This does not prove parallelism cannot help a different plan shape. |
| Quality boundary distances | Vulkan FP64 BVH traversal | Correct on captured queries, but slower with realistic batching; rejected. |
| Wall Boolean operations | CPU bounding-box pruning and independent-component unions | Small replay savings; component partitioning changes polygon order. Rejected for production. |
| Wall Boolean operations | Existing Vulkan empty-result classifier | Classification costs more than avoided clipping on this workload. Rejected. |

## GPU and polygon replay findings

The quality capture contains 200,284 points in 9,928 paths across 77 sampled layers. Of these paths, 9,815 contain fewer than 256 points. CPU serial signed-distance queries took **174.33 ms**, CPU parallel queries **56.82 ms**, and batched Vulkan queries **288.02 ms**, with another **203.16 ms** for GPU initialization. Dispatching individual paths took **10,117.08 ms**. All double and float results matched exactly. The replay includes the BVH preparation/upload and the CPU sign query required by the GPU version. This prototype was archived and removed from production source; isolated parallel CPU queries motivated the bounded CPU cache experiment.

The wall polygon replay sampled 13,630 real operations. Bounding-box pruning across 9,565 clipping operations reduced **52.64 to 48.73 ms**. Partitioning 4,065 unions reduced **9.335 to 8.961 ms**; only 27 operations split into multiple components, and three changed output ordering despite equal geometric coverage. These savings do not justify additional production paths here.

For 44 eligible wall operations, GPU empty-result classification took **67.89 ms**. Sixteen were provably empty, avoiding only **5.97 ms** of CPU work (all original operations together took **17.12 ms**). No new wall Boolean GPU option is enabled.

The wall profiler's approximately 9.04 seconds inside clipping is the **sum of timed operations across worker threads**, not nine seconds of sequential wall time. Similarly, its 1.13 seconds in eligible empty results is not an achievable elapsed-time saving.

The velocity-command replay ran three times per mode, alternating order: original **2.667, 2.503, 2.752 s**, scanner **0.465, 0.439, 0.478 s**. Median reduction is **2.202 s (82.6%)** for that replay. Estimated motion time remained identical.

Skipping export-only curvature reduced the real-path geometry replay from **206.3–207.0 ms to 128.8–130.3 ms**. All 200,940 output positions and distances matched. This replay uses captured boundaries and paths but representative fixed width 0.4 mm and minimum distance 0.04 mm; it does not reproduce every original per-path option. In-slice validation covers the actual path widths and settings.

## Correctness and limits

Developer validation (`ORCA_SLICE_VALIDATE=1`) compares travel containment decisions, velocity tokens, every processed extrusion-quality point/speed/overlap, and reused mask guards against their original calculations. Both candidate rounds completed U1, Local-Z Starry Night and ordinary Starry Night with these comparisons enabled.

The final selected normal regression suite passed **240 cases**, with **two pre-existing expected failures** (192,643 assertions total). Reference-mode checks passed **186,876 assertions in 23 cases**. An initial test-filter expression accidentally included the repository's hidden, intentionally failing `config_extruder cascade` documentation case; the corrected filter excludes hidden cases. This is unrelated to slicing acceleration.

Full G-code comparisons normalize only generation timestamps and runtime object identifiers in printing-object comments. **Every final U1 and Local-Z output matched the original baseline**, including the additional Local-Z control with distance batching disabled. The experimental variants matched too.

Ordinary Starry Night differs between untouched baseline repeats, affecting support, gap fill and internal solid fill across the experiment series. In the final series, differences were confined to gap fill and internal solid fill; the second optimized run matched the original baseline exactly. Local-Z schedules, warning summaries, filament consumption and cost matched throughout. One optimized ordinary run reported an estimated print time of 2d 1h 48m 36s instead of 2d 1h 48m 35s; the other final statistics matched. Internal exact checks are useful evidence, but do not establish byte-for-byte equivalence for every ordinary run or prove every difference is pre-existing. See `comparisons_final.json` and `compare_final.log`.

## Scheduling constraints

Independent geometric queries and filament mask slots can run in parallel. Output ordering still matters: travel decisions, retraction, tool changes, extrusion state, cooling/pressure processing and emitted commands carry state between paths or layers. These experiments preserve ordered assembly and the existing export pipeline. Sending the whole stateful exporter to GPU, or generating layers independently without reconciling that state, is not an equivalent replacement.

## Final selection and repeated slice timings

Retain the classic support-offset reduction, velocity parser, borrowed preview input, simpler extrusion-quality calculation and Local-Z distance batching. Remove travel indexes and both mask experiments. The GPU distance prototype and polygon pruning/partitioning remain rejected experiments, not production options.

Local-Z export-pipeline measurements with the simpler CPU changes already enabled were **42.096 and 42.024 s** without distance batching, versus **40.448 and 39.132 s** with it. Batching trades some additional CPU work for lower elapsed time; it is not a reduction in total CPU work. Ordinary measurements did not show a consistent benefit, so the final caller admits only objects with a Local-Z plan.

These CPU changes are enabled in normal builds with no user preference or build flag. Developer controls only: `ORCA_SLICE_EXPERIMENTS=none` selects reference implementations; a comma-separated subset of `support,parser,output,quality,quality_batch` isolates retained changes; `ORCA_SLICE_VALIDATE=1` compares available reference calculations; `ORCA_QUALITY_PROFILE=1` reports cache statistics. Existing Vulkan Local-Z clipping and seam visibility retain their existing preferences and CPU fallback.

The final release application built successfully. All three final validation slices passed with `ORCA_SLICE_EXPERIMENTS` unset. The caller correctly omitted distance batching in ordinary Starry Night. Two timed runs per workload and executable followed, alternating baseline/candidate order. These are full CLI process times, including loading and export; validation and the new diagnostic timers were disabled.

| Workload | Before runs (s) | After runs (s) | Before mean (s) | After mean (s) | Observed change |
|---|---:|---:|---:|---:|---:|
| U1, Arachne | 29.20, 29.11 | 28.68, 29.13 | 29.15 | 28.90 | 0.9%; too small to establish a whole-job gain |
| Starry Night, ordinary | 75.05, 82.50 | 71.76, 73.75 | 78.78 | 72.76 | 7.6% faster |
| Starry Night, Local-Z | 110.47, 109.88 | 106.17, 105.77 | 110.18 | 105.97 | 3.8% faster |

Elapsed stage means, before → after:

| Workload | Wall generation (s) | Curled-extrusion estimation (s) | Entire G-code export (s) |
|---|---:|---:|---:|
| U1 | 2.10 → 2.16 | 0.99 → 0.98 | 14.07 → 13.61 |
| Starry Night, ordinary | 7.63 → 4.59 | 5.59 → 5.53 | 26.28 → 25.33 |
| Starry Night, Local-Z | 9.06 → 5.28 | 6.53 → 6.52 | 55.07 → 54.39 |

Classic wall generation improves approximately **40–42%**. Curled-extrusion estimation is effectively unchanged. The final whole-export gain is modest, especially for Local-Z, despite the large isolated parser improvement and the earlier batching comparison. Isolated operation savings cannot simply be added to the elapsed time of a pipelined exporter.

Aggregate process CPU time fell from **457.02 to 403.58 CPU-seconds** for ordinary Starry Night (11.7%) and **505.95 to 461.87 CPU-seconds** for Local-Z (8.7%). U1 was nearly unchanged. Peak private commit was also essentially unchanged. CPU-seconds sum execution across cores and are not elapsed slice time.

The two-run averages describe these measured workloads and desktop conditions; they are not universal speedup estimates. The ordinary baseline range is particularly wide. Raw values and environments are preserved in `final_summary.json` and the `final_baseline1`, `final1`, `final_baseline2`, `final2` run directories.

A final same-executable Local-Z control disabled **only distance batching**: it took **107.66 s overall**, **56.81 s exporting**, and **45.30 s in the export pipeline**, versus the enabled two-run means of **105.97**, **54.39**, and **42.94 s** respectively. This single additional control supports retaining the cache after removing the failed experiments; it is not a further two-run performance estimate. Its evidence is in `final_no_batch/`.

## Commits and rollback

| Commit | Change |
|---|---|
| `e93899ef0c` | Classic wall offsets and shared developer experiment controls |
| `beecb11b36` | Initial travel-index experiment |
| `37d869fd2a` | Velocity-command parser |
| `874ea12936` | Borrowed G-code preview input |
| `c9a3e13031` | Remove travel indexes after regressions; keep cache initialization and coverage |
| `d79fd3791d` | Simpler extrusion-quality calculations |
| `feb0b1e1ba` | Bounded Local-Z distance batching and final defaults |
| `8784142ea1` | Replay tests and polygon-capture diagnostics |

Batching depends on the quality-calculation change: revert batching before reverting that underlying change. Several retained features use the experiment-control header introduced with the support-offset commit; preserve that header when rolling back only support offsets. For an immediate diagnostic comparison without source changes, the comma-separated developer control selects features independently (`quality_batch` also requires `quality`).
