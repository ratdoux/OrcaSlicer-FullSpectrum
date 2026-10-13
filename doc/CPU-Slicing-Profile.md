# CPU slicing profile: Starry Night

Profiled on 13 October 2026, using the optimized Release build from [the CPU optimization comparison](CPU-Slicing-Optimization-Results.md).

**The main elapsed-time bottleneck is serial Local-Z geometry work during G-code export.** Export occupies about 59% of the run, averaging only 1.13 busy CPU cores. Within export, about 56% of weighted running-thread samples are in Local-Z path clipping, and another 9% are in a diagnostic intersection check. Wipe-tower preparation repeats related intersection work before export begins.

This is a completed function-level sampling run, supplemented by process CPU counters and stage timestamps. The original project and the open GUI instance were left untouched.

The follow-up [profile with Local-Z/SML disabled](CPU-Slicing-Profile-Without-LocalZ.md) compares the same build after disabling the three gradient recipes that independently activate Local-Z.

## Workload and timing

- Input: an exact copy of `C:\Users\radug\Downloads\STARRY NIGHT PRINT.3mf`, with the saved printer, quality, support, painting and mixed-filament settings. No slicing cache.
- Project SHA-256: `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2`.
- DLL SHA-256: `a2ad2ad01995712d8f0f27dcdb090ea603d79e750d64a4a0a9e8cddb3fa8ee99`.
- Intel Core Ultra 9 285H, 16 logical processors, 64 GB RAM, Windows Balanced power plan.
- Fresh CLI process, PID 36360, completed successfully. No application rebuild or slicing algorithm changes were needed for profiling.
- Profiled process lifetime: **309.62 seconds**. Aggregate CPU time: **917.88 CPU-seconds**. CPU time adds the work of all threads, so it can exceed elapsed time.
- The earlier two unprofiled optimized runs took 311.54 and 312.53 seconds. Keep those runs as the performance-comparison reference; this profiling run is not evidence of a further speedup.

| Phase | Elapsed seconds | Share of run | CPU-seconds | Average busy cores |
|---|---:|---:|---:|---:|
| Loading and configuration | 5.20 | 1.7% | 4.94 | 0.95 |
| Initial mesh slicing | 1.53 | 0.5% | 12.27 | 8.00 |
| Overhang / paint segmentation / Local-Z preparation | 36.75 | 11.9% | 177.93 | 4.84 |
| Walls | 19.35 | 6.3% | 164.55 | 8.50 |
| Solid surface detection | 4.67 | 1.5% | 57.03 | 12.22 |
| Vertical shells | 16.71 | 5.4% | 199.90 | 11.97 |
| Remaining external surfaces, bridges and infill | 0.64 | 0.2% | 2.82 | 4.42 |
| Tree support | 11.09 | 3.6% | 54.01 | 4.87 |
| Wipe tower and slicing finalization | 28.60 | 9.2% | 37.17 | 1.30 |
| G-code export, including generation and processing | **183.09** | **59.1%** | **206.65** | **1.13** |
| Cleanup | 1.99 | 0.6% | 0.60 | 0.30 |

The phase boundaries come from existing log messages. In particular, the 36.75-second phase is a combined window, and the 28.60-second phase also includes final path simplification and related finishing work. CPU counters are interpolated at these boundaries. Loading/cleanup have subsecond timing uncertainty from launch and exit polling. These figures exclude GUI/OpenGL preview construction.

![CPU activity and stage duration](../build_logs/cpu_slicing_profile/starry_1/cpu_timeline.png)

## What export is doing

The following are shares of weighted running-thread samples **within export**, not percentages of the whole run and not directly measured elapsed seconds. The first three rows identify separate call sites in `GCode::process_layer` and do not overlap with each other.

| Export work | Sample share | Source |
|---|---:|---|
| Remove mixed areas from the ordinary/base paths | **29.7%** | `GCode.cpp:5682`, `clip_extrusion_collection_for_local_z` with exclusion masks |
| Clip paths into each Local-Z pass / physical filament region | **26.5%** | `GCode.cpp:5638`, the same helper with inclusion masks |
| Check the resulting base paths for leaks back into mixed areas | **9.1%** | `GCode.cpp:5696`, diagnostic `intersection_pl` |

Together, those three call sites account for approximately **65% of sampled export work**. The clipping helper traverses extrusion paths and calls `intersection_pl` or `diff_pl` for individual paths. The caller repeats this across pass buckets and physical extruders, then computes the remaining base paths. The lower-level `_clipper_pl_open` constructs a clipping operation with the subject paths and mask polygons each time.

The diagnostic check is a particularly concrete avoidable cost: `base_clip_leak_warnings < 3` limits **warnings**, not checks. The counter increments only when a leak is found. With zero leaks, successful checks continue across all eligible collections. This check logs a warning; it does not repair the generated paths. Making this expensive diagnostic opt-in, or checking a deliberately bounded sample, is a candidate for the next change. Its performance effect still needs an unprofiled before/after measurement.

Other identifiable export costs, shown as inclusive function shares, are:

| Function / work | Export sample share |
|---|---:|
| `SeamPlacer::init` and descendants on its calling thread | 6.9% |
| `GlobalModelInfo::calculate_point_visibility` | 3.4% |
| `GCode::_extrude` / `extrude_path` | 5.5% |
| `GCodeProcessor::process_buffer` | 4.0% |
| `GCodeProcessor::finalize` | 2.0% |
| `local_z_compensate_masks` | 1.8% |
| `CoolingBuffer::process_layer` | 1.4% |
| `AdaptivePAProcessor::process_layer` | 0.9% |

Inclusive functions include their children and can overlap. Worker callbacks do not retain the scheduling parent's stack, so `SeamPlacer::init` alone understates all seam-preparation work. These rows are useful attribution points, not an additive partition of every export sample.

The export label should not be interpreted as simply writing a file. The samples show substantial polygon geometry work before commands are emitted. Move parsing (`GCodeProcessor::process_G1`) is about **0.7%** of export samples and storing a preview move (`store_move_vertex`) about **0.4%**, inclusive. Move compression is therefore not the first CPU optimization indicated by this profile; memory savings are a separate question.

`GCode::process_layers` already uses a TBB pipeline. Its generator invokes `process_layer` as `serial_in_order`; other ordered stages can overlap with it. Active tool, travel, seam and stream state constrain the generator. The measured 1.13-core average confirms that pipeline overlap is small for this workload. Independent geometry preparation is a more plausible parallelization boundary than making the entire generator parallel.

## What happens before export

| Phase | Function-level evidence | Interpretation |
|---|---|---|
| Paint / Local-Z preparation | `build_graph` appears in 32.2% of this phase's samples; colored-line Voronoi construction in 30.2%; Clipper execution in 33.3%. | Painted-region topology and polygon processing are substantial. The graph and Voronoi percentages overlap. This phase also contains ordered Local-Z work. |
| Walls | `PerimeterGenerator::process_classic` appears in 95.9%; `generate_lower_polygons_series` in 54.8%; Clipper offset execution in 70.3%. | Repeated lower-layer polygon offsets are a concrete target. This phase already averages 8.5 busy cores. |
| Vertical shells | Clipper execution appears in 83.0%; intersection in 58.6%; union in 35.5%. | Predominantly polygon boolean operations, already using about 12 cores on average. |
| Tree support | Clipper execution appears in 69.4%; offset execution in 46.7%. | Geometry and collision/avoidance preparation matter; the entire support phase is only 11 seconds here. |
| Wipe tower and finalization | `collect_local_z_wipe_tower_toolchanges` appears in 74.0%; `extrusion_collection_has_local_z_segment` in 67.2%. | Most of the CPU work is checking whether paths intersect Local-Z masks before scheduling tool changes. |

The wipe-tower existence helper calls `intersection_pl` and constructs clipped output merely to determine whether a valid segment exists. It can stop after a successful path, but it still pays for full clipping of each tested path. Bounding-box rejection, a carefully equivalent existence query, and sharing bounded immutable geometry preparation with export are worth evaluating.

Across the complete run, **about 60% of weighted leaf samples land directly in `deps_src/clipper/clipper.cpp`**. This is an exclusive source-file attribution, so it does not double-count parent functions. It explains the overall CPU consumption, while the serial export and wipe-tower work explain much of the elapsed-time bottleneck. This finding does not by itself justify replacing the vendored clipping library or moving it wholesale to a GPU.

## Recommended order for subsequent CPU work

1. Make the repeated base-clip leak diagnostic opt-in or explicitly bounded, preserving a full validation mode. It currently accounts for about 9% of export samples.
2. Reduce Local-Z clipping work: reject disjoint path/mask bounds early, investigate reusing mask preparation, and evaluate batching compatible paths while preserving fragment ordering, seam anchors and flow metadata. The clipping call sites account for about 56% of export samples.
3. Improve wipe-tower intersection eligibility checks and avoid repeating equivalent geometry work between planning and export. Preserve active-tool ordering and pass dependencies.
4. Separate immutable Local-Z geometry preparation from ordered emission and assess bounded parallel work. Avoid caching every pass for the whole model without a memory budget.
5. Investigate repeated lower-layer offsets in wall generation and repeated polygon sets in vertical-shell discovery. These are substantial CPU users, but more parallelism alone has less headroom here.

These are priorities supported by this workload, not promised speedups. The prior benchmark found output variation even between unchanged baseline runs and existing Local-Z warnings. Any new geometry optimization still needs deterministic unit cases, repeat-run output diagnostics and the same unprofiled slicing comparison.

## Collection method and limits

Windows Performance Recorder could not enable the performance-profiling policy (`0xc5585011`). The installed Visual Studio collector also failed to create a CPU session (`REGDB_E_CLASSNOTREG`). No machine policy or permissions were changed.

The fallback is [tools/profile_slicing_windows.cpp](../tools/profile_slicing_windows.cpp), a process-only x64 sampler:

- It queries thread states and selects running/ready threads belonging to the disposable CLI PID. Analysis uses only threads observed in the running state; ready samples remain in the raw data.
- It briefly suspends each selected thread, copies its register context and up to 64 KiB of stack, and resumes it **before** stack unwinding or symbol loading. It uses [Microsoft's StackWalk64 API](https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-stackwalk64) and the exact Release PDB. Thread enumeration uses [NtQuerySystemInformation](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation), whose layout is Windows-version dependent.
- Captured **31,733 stacks**, including **26,052 running-state stacks**, across **9,571 sampled batches**. Three captures failed; none reached the 96-frame limit. The actual median batch interval was **31.3 ms**, despite the requested 20 ms, reflecting timer and collection overhead. The largest interval between sampled batches was 91.2 ms.
- Analysis weights batches by their observed interval. Function percentages are approximate sampled CPU attribution. This is not kernel ETW sampling: state can change between enumeration and capture, kernel stacks are absent, and short functions can be missed. Wait-like leaf symbols account for about **1.3%** of weighted running-state samples and are retained as uncertainty rather than reassigned to productive work.
- Matching source-line information is available for **85.5% of weighted leaf samples**. Other addresses resolve to module/export symbols, which can name the nearest exported function rather than a private runtime function. Do not treat apparent runtime names such as `wcsrchr` in worker startup stacks as actual hot functions. The application/source-backed findings above avoid that interpretation.
- Summed suspension intervals across all captured threads were 11.06 seconds, with a maximum single suspension of 25.2 ms. Summing across threads does not give added wall time. Use unprofiled runs for performance comparisons.
- A separate synthetic workload ran a known function for three seconds and another for one second, with a sleeping worker. The sampler attributed **96 versus 32 stacks** to the two hot functions, matching the expected 3:1 ratio. This checks basic stack attribution and exclusion of the sleeping worker; it does not remove statistical or scheduling bias on the real workload.

## Reproduction and retained artifacts

Build the helper from an x64 Visual Studio developer shell, then run:

```powershell
cl /nologo /std:c++17 /O2 /EHsc tools/profile_slicing_windows.cpp /Fobuild_logs/cpu_slicing_profile/sampler.obj /Febuild_logs/cpu_slicing_profile/sampler.exe /link dbghelp.lib
python scripts/profile_cpu_slicing.py --exe build-dev-release/src/Release/snapmaker-orca.exe --project build_logs/cpu_optimization_benchmark/inputs/starry.3mf --sampler build_logs/cpu_slicing_profile/sampler.exe --symbols build-dev-release/src/Release --output build_logs/cpu_slicing_profile/starry_new
python scripts/analyze_cpu_profile.py build_logs/cpu_slicing_profile/starry_new --plot
```

Use a new output directory for each run. `--plot` requires matplotlib; the JSON/CSV analysis uses the Python standard library. Run against a disposable CLI process, because sampling briefly pauses its threads.

The captured run is retained in ignored `build_logs/cpu_slicing_profile/starry_1/`:

- `analysis.json`: phase CPU/wall totals, inclusive/exclusive functions, source files, generator call sites, thread totals and quality counters.
- `profile_stacks.csv`, `profile_symbols.jsonl`, `profile_timeline.csv`: raw stacks, symbol mapping and process CPU counters.
- `stacks.folded`: root-to-leaf stacks, weighted in microsecond units for flame-graph tooling. These units represent interval-weighted samples, not exact measured function time.
- `cpu_timeline.png`: the chart above.
- `sampler.log`, `profile_command.json`, `runner.log`, `slice/metrics.json`, slicing logs and generated G-code.

The chart and raw trace remain local ignored artifacts; keep that directory when sharing this report with its evidence. No claim of deterministic G-code equality or physical printer validation is added by this profiling run.
