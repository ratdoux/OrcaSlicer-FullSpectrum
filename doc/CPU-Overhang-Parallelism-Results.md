# CPU follow-up after the Vulkan geometry review

## Changes

The follow-up implements four CPU changes that apply to ordinary and Local-Z slicing where the relevant stage is used:

1. **Compile Klipper velocity-limit patterns once.** `GCodeProcessor::process_SET_VELOCITY_LIMIT` now shares three immutable regexes. The expressions, first-match behavior, `std::stof` conversions and error behavior are unchanged. A public-API characterization test was committed and run against the original implementation first. It compares motion-time estimates against equivalent M203/M204/M205 commands, including repeated commands, whitespace, similarly named parameters and malformed numbers. This change benefits Klipper G-code processing, not every firmware flavor.
2. **Reuse the lower-layer bridge-support outline across regions.** The bridge-removal helper computes the same offset once per call instead of once per material region, and removes an unused conversion to lines. The offset uses the shared extrusion-width argument; each region's different bridge flow still controls its own bridge inflation. Empty region lists still skip the offset.
3. **Remove shared writes from initial tree-overhang detection.** Each layer records its flags and maximum cantilever distance; a serial reduction combines them. An atomic flag synchronizes the existing global expensive-detail cutoff. The cutoff policy itself, including its dependence on elapsed time and scheduling, is retained. This is a race fix, not a claim of deterministic slicing.
4. **Run independent final overhang filtering concurrently.** A worker owns one layer's overhang/cantilever vectors and temporary geometry. It reads finalized slices, region paths, blockers/enforcers and sharp-tail data. A TBB arena limits this phase to four workers, honoring a smaller enclosing arena. Once workers finish, serial reduction builds the polygon-pointer map and summaries in layer order. Sharp-tail propagation and cluster construction remain sequential.

These CPU changes are included by normal release builds without extra build flags or preferences. The GPU preferences are unchanged. The separate [indexed-classifier experiment](LocalZ-Indexed-Classifier-Results.md) remains diagnostic-only.

The final-filter diagnostic control is `ORCA_TREE_OVERHANG_FILTER`:

| Value | Behavior |
|---|---|
| unset / `parallel` | Use the bounded parallel filter. |
| `serial` | Use the same filter serially, for measurement and rollback of this stage. |
| `validate` | Compute serial reference outputs first, then compare worker outputs exactly. |

The other three changes have separate commits for rollback. Revert dependent changes newest first.

## Review and validation scope

Codex and Claude discussed the implementation through the CLI conversation **Vulkan GPU geometry acceleration review**, session `d30d83f1-1057-401e-b61d-16c037d2700e`. Claude had read-only repository access, with no edits, builds, command execution or desktop automation. Its review identified the pre-existing shared-state races; Codex verified them in source before changing them. The subsequent implementation review found no blockers. Agreement between reviewers is not a substitute for the checks below.

The full focused suite passed **396,772 assertions in 34 cases** before adding the cancellation case. The final targeted tree suite then passed **99 assertions**, including cancellation while workers are active. The Release application build passed. The deterministic fixture includes multiple material regions, a bridge, cantilevers, a floating component, support blockers/enforcers and critical-regions-only on/off. It checks ordered overhang/cantilever polygons, per-polygon type labels, sharp-tail/cantilever flags and maximum cantilever distance across one- and four-worker execution.

`validate` compares contour and hole point order, detected/enforced index boundaries and the per-layer sharp-tail flag. References are computed before any worker moves a layer's vectors. This is an exact differential check of serial versus parallel execution of the **new** filter for those inputs. It is not an independent old-implementation oracle, a race detector, a proof about every model, or a comparison of the entire downstream slice. The source review separately checked the extracted filter against the previous inline operation order. The fixture checks published type-map entries and flags; the diagnostic comparison does not independently recompute every final summary counter.

The log records `worker_limit`; a one-worker validation run does not exercise concurrency. Diagnostic references retain an additional set of results, so their memory and timing must not be used as normal performance numbers. The four-worker limit bounds concurrent jobs, not a strict byte count on Clipper's internal allocations.

The ordinary and Local-Z Starry Night validation exports both completed with `worker_limit=4`, comparing all **1,257 layers** in each run without a mismatch.

Cancellation now aborts this stage with `CanceledException`, rather than breaking out of the old loop and continuing with unfiltered layers. The pointer map is published only after successful completion. A validation mismatch or worker exception also aborts the slice; partially processed layer state must not be used as a successful result. Clearing the map before publication has no effect in normal call paths, which construct a fresh `TreeSupport`. Other pre-existing repeat-detection assumptions about derived layer state are unchanged.

Complete G-code equality and physical printer validation are separate questions. Earlier baseline repeats already differed, and this series does not claim to resolve all nondeterminism.

The new complete-output comparisons also **fail normalized byte equality**, including the two unchanged baseline repeats and the two optimized repeats in each workload. Both comparison tools return exit code 1, retained in the evidence. All checked Local-Z schedules and warning counts match. Ordinary filament estimates are 235.25–235.26 g before and 235.25 g after; Local-Z estimates are 292.97 g before and 292.99 g after. These similar totals do not prove every difference harmless. Exact per-layer checks, characterization fixtures and source review are the correctness evidence for this series; full-output equivalence remains unproven.

## Decisions to defer

The subsequent [Vulkan target review](Vulkan-Next-Targets-Review.md) records the final profiles, the additional discussion with Claude and the next CPU/GPU experiments. It does not claim another implemented GPU speedup.

* **Cross-layer Local-Z export parallelism:** configuration, writer position, seam selection and borrowed masks require immutable snapshots and owned jobs with explicit memory limits. The stateful emission loop remains serial.
* **Within-layer Local-Z clipping:** source inspection supports a narrower future preparation phase: configuration and last position are stable during grouping, and seam placement reads precomputed data. Implementing it still requires immutable classification access/counter handling, ordered task consumption and exact comparison of full extrusion metadata, including `Polyline::fitting_result`. Nearest/staggered seams, mixed heights and pointillism need fixtures. This series does not add a superficial parallel loop around grouping.
* **Curl estimation across layers:** previous-layer extrusion geometry and curl heights form dependencies. An independent geometry/height split is not established here.
* **Nonempty keep-whole bypass and full GPU clipping:** containment alone does not preserve Clipper output normalization, order, direction or arc metadata. The sampled unchanged outputs are encouraging evidence for further diagnostics, not permission to bypass clipping.
* **Deterministic sharp-tail cutoff policy:** synchronization is fixed, but changing the existing 30-second / complex-layer global cutoff would change behavior and is not part of this performance change.

## Measurements

Two fresh, unprofiled processes per build and workload, with the second pair reversing build order. These are medians of two observations on one model/configuration pair, not confidence intervals or a representative benchmark suite. The baseline DLL is `ae3e0a27d91c33ae7f55387c7d68acb07606592f6565dba1ebd3837d5f45134a`; the optimized DLL is `99be853a305ca6a1ffbc37e6d55bfd2ca178b5eef8589face5c24b6867fee234`.

| Complete CLI process | Before median (range) | After median (range) | Reduction |
|---|---:|---:|---:|
| Ordinary | 98.79 s (97.70–99.89) | 96.91 s (96.20–97.62) | **1.9%** |
| Local-Z | 140.71 s (133.71–147.70) | 136.16 s (132.14–140.18) | **3.2%** |

The stage-level evidence is stronger than the small whole-process difference. Tree-support processing drops consistently; unrelated intervals vary, and export does not show a gain in these runs.

| Stage median | Ordinary before | Ordinary after | Local-Z before | Local-Z after |
|---|---:|---:|---:|---:|
| Mesh slicing | 1.42 s | 1.51 s | 1.50 s | 1.45 s |
| Painting / Local-Z preparation | 27.38 s | 28.04 s | 37.19 s | 36.62 s |
| Walls | 13.24 s | 13.73 s | 16.49 s | 16.61 s |
| Tree support | **11.22 s** | **6.31 s** | **11.47 s** | **5.53 s** |
| G-code export | 28.30 s | 29.58 s | 56.93 s | 57.92 s |

The new final-filter timer is 0.666/0.688 seconds ordinary and 0.672/0.659 seconds Local-Z. This is only the extracted filtering phase, not the entire support stage. The eight timing logs retain the same 232 overhang-layer summaries and the same reported maximum cantilever distance. None triggered the existing expensive-detail cutoff.

A separate same-binary ordinary control with `ORCA_TREE_OVERHANG_FILTER=serial` takes **2.472 seconds** in that final filter, versus 0.666–0.688 seconds in the two parallel runs. Its complete support interval is 7.602 seconds. This supports the parallel phase's benefit after bridge reuse is already present, but it is only one serial observation. Its whole process happens to finish in 93.29 seconds, faster than the parallel timing runs because other intervals differ; it must not be used as proof that serial filtering is faster overall.

Total process CPU time does **not** demonstrate an overall reduction: ordinary medians are 501.65 to 515.65 CPU-seconds, and Local-Z 567.94 to 568.28. Those totals include all worker activity and vary independently of the changed stage. The regex reuse should not be advertised as a measured complete-export speedup from this table.

The separate ordinary profiles help explain the parser change: estimated inclusive CPU time in `process_SET_VELOCITY_LIMIT` falls from **6.00 to 1.99 seconds**, and export's measured process CPU time from 53.56 to 47.04 seconds, while export elapsed time stays 26.45/26.47 seconds. The processor is saving work without becoming the pipeline's elapsed-time bottleneck. Function estimates come from weighted running-state stack samples, not exact per-function timers; inclusive estimates overlap. These profiled runs are not substituted for the unprofiled timing table.

Peak working-set medians are 5,378 to 5,448 MiB ordinary and 5,454 to 5,375 MiB Local-Z; peak commit medians are 8,242 to 8,261 MiB and 8,242 to 8,192 MiB respectively. This is not evidence of a material peak-memory improvement or regression. Whole-process peaks are dominated by other phases, especially model loading/preparation, and do not isolate the four workers' temporary allocations.

## Reproduction

Raw evidence is under ignored `build_logs/classifier_review/`: preserved binaries, exact environment records, validation and timing runs, profiles, comparisons and the two CPU review transcripts. `run_cpu_comparison.py` runs the isolated jobs sequentially. Inputs, machine and classifier controls are recorded in the [classifier report](LocalZ-Indexed-Classifier-Results.md).

The benchmark keeps Local-Z Vulkan clipping enabled on NVIDIA for both binaries; seam visibility and polygon compute stay on CPU, and CPU seam caching stays enabled. Every timing run is a fresh process with a separate data/output directory. No builds or hardware tests compete with the timed slices. GUI instances are untouched. Profiling and reference validation are separate from unprofiled timing runs.

```powershell
cmake --build build-dev-release --config Release --target libslic3r_tests Snapmaker_Orca_app_gui --parallel 8
& build-dev-release/tests/libslic3r/Release/libslic3r_tests.exe '[GCodeProcessor],[CPUSlicing],[LocalZ],[VulkanCompute]'
```

## Atomic changes

| Commit | Change |
|---|---|
| `7c85c45667` | Indexed CPU/Vulkan classifier experiment and results. |
| `045dedb7e8` | Klipper behavior characterization, before optimization. |
| `3a157fc02e` | Reuse compiled velocity-limit patterns. |
| `d675f8c132` | Reuse lower support outlines across bridge regions. |
| `42e6b8ffd9` | Race-free initial overhang summaries and worker-count fixture. |
| `24a8e7f2f6` | Bounded final filtering, ordered publication, validation and cancellation. |
