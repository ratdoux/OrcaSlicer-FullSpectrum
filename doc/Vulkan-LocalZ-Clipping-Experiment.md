# Vulkan Local-Z empty-path clipping experiment

This experiment follows the seam-visibility prototype. Its target is time spent in the sequential G-code generator, rather than summed CPU time in an already parallel stage. It is opt-in: both the Vulkan build option and runtime acceleration remain disabled by default.

On the original Starry Night project, two alternating CPU/Vulkan timing pairs reduced complete slicing from a median **182.71 s to 160.42 s (12.2%)** on the RTX 5060 Laptop GPU. Export fell from **83.45 s to 65.06 s (22.0%)**. These are complete application runs, including GPU initialization, transfers, CPU fallback, and shutdown. The sample is small and individual times vary; it is evidence of a useful improvement on this model, not a prediction for every project.

## Direct measurement and captured inputs

The original Starry Night project is `build_logs/cpu_optimization_benchmark/inputs/starry.3mf`, SHA-256 `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2`. The initial instrumented capture completed in 187.89 seconds. Its DLL SHA-256 was `ab00be9bd8d5818d9a9dc3e3438cf3e6f84e8160e2def69d7a2ecf71b5c5ba4a`.

| Operation | Calls | Direct elapsed time inside calls | Time producing empty results |
|---|---:|---:|---:|
| Intersection with filament masks | 566,732 | 11.062 s | 8.792 s |
| Subtraction of mixed masks from base paths | 250,994 | 22.151 s | 12.299 s |
| Total | 817,726 | **33.213 s** | **21.090 s** |

These are summed steady-clock intervals around actual clipping calls, excluding diagnostic serialization and statistics collection. The generator is `serial_in_order`; it can migrate between TBB worker threads without running concurrent layer-generation calls. This is therefore different from the earlier sampled estimate of about 36.8 CPU-seconds. These timers exclude seam placement, fragment ordering, mask construction, and output emission. The captured run includes tracing overhead and is not the uninstrumented performance baseline.

Every 257th operation was captured, including complete masks, input polylines, operation, and exact ordered integer output. All 3,182 samples fit below the 128 MiB capture cap (93,160,831 bytes), so the capture spans the complete run. Replaying every sample with the current CPU clipper reproduced its recorded output exactly.

## What Vulkan does

The prototype proves that a complete extrusion entity will produce **no valid clipped paths**. It does not generate intersection coordinates or replace Clipper for boundary-crossing or retained paths.

Each GPU invocation examines a source segment against the mask edges. Exact bounded integer orientation tests conservatively reject any boundary crossing, touch, or near-boundary case. A nonzero winding test then determines whether the segment lies wholly in the region that the clipping operation discards. An entity is skipped only when every segment of every path has a discard decision. All other entities use the original CPU clipping operation.

The captured-input classifier identified 2,294 of 2,301 empty cases, representing 60.9% of the captured clipping time. Its CPU implementation was too expensive to be a useful replacement: classification alone took about as long as the existing clipping. This motivates the GPU experiment.

On an initial NVIDIA replay, the original clipping took 136.3 ms. GPU classification took 4.6–4.9 ms, and classification plus remaining CPU clipping took 57.1–64.0 ms. Initial device/pipeline creation and uploading the captured masks added 594.1 ms. These are isolated replay results, **not complete slicing speedups**. The complete-slice integration amortizes initialization over an export and uploads masks by layer.

## Integration and correctness boundaries

- One accelerator belongs to one G-code export. Layer generation remains sequential. The context may move between TBB threads, but its Vulkan queue is never used concurrently.
- Each layer gathers jobs from the same extrusion collections and prepared include/exclude masks used by the original code. Masks are uploaded once for that layer; queries dispatch in batches of at most 16,384 segments.
- Source loops are classified before seam placement. The original seam operation still executes, in its original order, even for an entity predicted to be empty. Projection/rounding guards send near-boundary cases to the CPU. Validation checks the actual post-seam clipping before accepting any skip.
- Nonempty output geometry, fragment ordering, flow-height assignment, tool ordering, travel planning, and G-code generation keep their existing implementation.
- Coordinates must lie within +/- 2^29 integer units. This bounds intermediate cross products within signed 64-bit arithmetic. Out-of-range geometry falls back to CPU. A two-coordinate-unit boundary guard is conservative; it is not an allowed geometric error or an approximate output tolerance.
- The shader requires `shaderInt64`, which is queried and enabled explicitly. This optional feature is documented in the [Vulkan physical-device features reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html). Unsupported hardware uses the CPU path.
- Per-layer limits are 1,048,576 mask edges, 1,048,576 segment queries, and 262,144 entity/mask entries. Oversized layers fall back completely. GPU storage plus staging is approximately 33 MiB, in addition to bounded CPU preparation buffers and driver overhead; it does not retain the whole model on the GPU.
- Cancellation is checked before initialization and between batches. Device/setup/dispatch failures discard the layer's incomplete decisions and disable acceleration for the rest of that export. Validation mode fails instead of silently falling back.
- The shared Vulkan backend retains one loader-library reference and shares one Vulkan instance. Devices, buffers, and queues belong to their individual backends. The application explicitly releases the shared instance after its jobs finish, before Windows DLL teardown.

The existing complete-G-code variability documented in [the CPU results](LocalZ-CPU-Optimization-Results.md) and [the seam experiment](Vulkan-Seam-Visibility-Prototype.md) remains a separate issue. Checking every skipped result within a run provides a stronger local correctness test than comparing unrelated full-output hashes, but does not establish physical printer validation or eliminate that pre-existing variability.

## Controls and reproduction

Build using the same `ORCA_ENABLE_VULKAN_COMPUTE=ON` option and Vulkan headers/glslang paths as the seam prototype. CPU-only builds require no Vulkan headers, shader compiler, or runtime.

| Environment variable | Behavior |
|---|---|
| `ORCA_LOCAL_Z_CLIP=cpu` | Default; original CPU clipping. |
| `ORCA_LOCAL_Z_CLIP=vulkan` | Layer-batched conservative empty-result rejection with CPU fallback. |
| `ORCA_LOCAL_Z_CLIP=validate` | GPU preparation, then original CPU clipping for every predicted-empty entity; abort if any has valid output. |
| `ORCA_VULKAN_DEVICE=NVIDIA` / `Intel` | Explicit device-name filter shared with the seam prototype. |
| `ORCA_LOCAL_Z_PROFILE=<file.json>` | Process-wide direct timing of remaining export clipping calls. No clocks are read when tracing is disabled. |
| `ORCA_LOCAL_Z_CAPTURE=<file.jsonl>` | Bounded systematic sample of export clipping inputs and outputs. Use CPU mode for representative baseline capture. |
| `ORCA_LOCAL_Z_REPLAY=<file.jsonl>` | Input to the hidden `[LocalZReplay]` test. |
| `ORCA_LOCAL_Z_REPLAY_GPU=1` | Include GPU classification and remaining CPU clipping in replay. |
| `ORCA_LOCAL_Z_REPLAY_REPORT=<file.json>` | Persist replay results. |

```powershell
$env:ORCA_LOCAL_Z_CLIP = 'validate'
$env:ORCA_VULKAN_DEVICE = 'NVIDIA'
$env:ORCA_SEAM_VISIBILITY = 'cpu'
python scripts/benchmark_cpu_slicing.py --exe build-dev-release/src/Release/snapmaker-orca.exe --project build_logs/cpu_optimization_benchmark/inputs/starry.3mf --output build_logs/new_local_z_validation

# Change the mode to cpu / vulkan and use a fresh output directory for timings.
# Keep capture and reference validation disabled for timing comparisons.
```

Retained artifacts are under `build_logs/local_z_vulkan/`, including direct profiles, captured inputs, replay reports, build logs, hardware tests, complete slice logs, and generated G-code. Full-model results and final validation follow.

## Validation and lifecycle investigation

- The relevant regression suite passes 252 cases, with two pre-existing expected failures (170,503 passing assertions).
- Each Windows GPU passes three hardware cases covering both compute kernels, with 52,206 assertions. Local-Z fixtures include large signed coordinates, boundaries, winding around holes, degenerate segments, malformed ranges, incomplete workgroups, oversized batches, replacement mask uploads, and missing-device selection.
- The real-input replay passes 139,808 assertions on each GPU: every captured CPU output matches exactly, every GPU segment decision matches its bounded-integer CPU reference, and every final retained/fallback output matches the capture.
- Deterministic CPU geometry fixtures compare conservative empty decisions against actual Clipper output, in addition to the captured real geometry.
- A Vulkan-disabled standalone build still compiles and returns the unavailable-backend result without loading Vulkan.

Early development runs exposed a lifecycle defect: repeatedly destroying the last Vulkan instance could crash this NVIDIA driver (`nvoglv64.dll`), and deferring shared-instance destruction to the application's DLL teardown could also fail after a successful export. Retaining only the loader library did not solve it. Sharing the instance passed 300 NVIDIA create/dispatch/destroy cycles across ten processes and 30 Intel cycles. A minimal DLL-host experiment then reproduced the teardown problem: implicit destruction during DLL unload hung, while explicit shutdown before unload passed ten consecutive processes. The application entry point now performs that explicit shutdown.

`validate_nvidia_1` finished export and checked 602,076 proposed empty entities without a geometry mismatch, but exited with an access violation during shutdown. **It is a failed run and is excluded from performance results.** The successful final-build runs below supersede it. The build and failure logs are retained instead of removing evidence of the lifecycle problem.

NVIDIA's final captured replay took a median 59.5 ms for GPU classification plus remaining CPU clipping, versus 136.6 ms for CPU clipping alone; setup/upload added 184.5 ms. Intel's corresponding median was 68.9 ms versus 143.9 ms, with 125.3 ms setup/upload. These medians are three repetitions within each replay process, not three full-model slices. Neither isolated replay includes the full-slice layer scheduling costs.

Hardware validation is Windows-only on the RTX 5060 Laptop GPU and Intel Arc integrated GPU. AMD, Linux, macOS, and Vulkan validation-layer runs have not been performed.

## Full-model validation

`validate_nvidia_2` completed the original Starry Night model and exited successfully using DLL SHA-256 `b5c83204b1692337f5aab6745e679957ea14285a6a3ec6c87af16fad001e2682`. It examined 817,389 entity/mask combinations and verified **602,110 proposed skips** by running the original clipping after seam placement. No proposed skip produced a valid clipped path.

The GPU processed 10,536,786 source segments. Preparation took 3.608 seconds, including 0.122 seconds setup, 1.520 seconds mask uploads, and 1.644 seconds dispatch/readback; the remainder is CPU preparation/reduction. The maximum layer contained 10,708 edges, 20,475 segment queries, and 1,771 entity/mask entries. One layer had no GPU work. This validation run still performs the reference clipping and is excluded from speedup measurements.

## Complete slicing measurements

The following runs completed on 2026-10-14 using the same final DLL and original project hashes given above. The order was CPU 1, Vulkan 1, CPU 2, Vulkan 2. Direct clipping timers, capture, and reference validation were disabled; seam visibility remained on CPU. Each run used a fresh isolated data/output directory and sliced plate 1 through the CLI. All four exited successfully.

| Mode / run | Whole application | G-code export | GPU preparation included in export |
|---|---:|---:|---:|
| CPU 1 | 181.194 s | 82.540 s | — |
| Vulkan 1 | 166.173 s | 67.701 s | 5.045 s |
| CPU 2 | 184.219 s | 84.369 s | — |
| Vulkan 2 | 154.671 s | 62.415 s | 3.668 s |
| **CPU median** | **182.707 s** | **83.454 s** | — |
| **Vulkan median** | **160.422 s** | **65.058 s** | **4.356 s** |
| **Time saved** | **22.285 s / 12.2%** | **18.396 s / 22.0%** | |

Whole-run improvement exceeds the export improvement by 3.89 seconds because stages outside export also varied between runs. Attribute the clipping change to the measured export gain; do not treat the extra difference as GPU acceleration of other stages. CPU process time fell from a median 812.37 to 776.34 CPU-seconds. Median peak working set increased from 5,375 to 5,548 MiB and peak private commit from 8,200 to 8,287 MiB. These are full-slicing peaks, not idle preview memory or a direct measurement of GPU allocations.

The Vulkan runs tested approximately 817,000 entity/mask combinations and skipped approximately 602,000 of them. Preparation includes packing, initialization, mask uploads, dispatch/readback, and reduction. It excludes subsequent CPU fallback clipping and lookup costs, which are included in the complete export and application timings.

`cpu_profile_1` contains a long interruption (3,847.8 s elapsed for 791.3 CPU-seconds); even some direct call timers span that interruption. It is excluded from both timing and diagnostic comparisons. Its logs are retained. The repeat diagnostic run is recorded separately below.

### Direct clipping breakdown

These separate instrumented runs use the final DLL: `cpu_profile_2` completed successfully on 2026-10-15, and `nvidia_profile_1` completed on 2026-10-14. Capture and reference validation were disabled. They explain where the improvement comes from and are not additional uninstrumented timing pairs.

| Directly measured work | CPU diagnostic | Vulkan diagnostic |
|---|---:|---:|
| CPU intersection calls | 11.006 s / 566,662 calls | 2.307 s / 118,307 calls |
| CPU difference calls | 22.089 s / 250,969 calls | 10.186 s / 97,099 calls |
| GPU preparation, transfers, dispatch, reduction | — | 3.093 s |
| **Clipping plus GPU preparation** | **33.095 s** | **15.586 s** |

This measured subtotal falls by **17.510 seconds (52.9%, about 2.12 times faster)**. The CPU diagnostic spent 20.980 seconds producing empty results; the Vulkan diagnostic spent only 0.103 seconds in remaining empty-result CPU calls. Nonempty CPU clipping is still approximately 12.4 seconds. That remaining work, seam handling, path ordering, wipe-tower processing, and G-code emission explain why the total export does not speed up by the same factor.

The subtotal excludes entity lookup/skip handling and teardown, so use complete export timing to evaluate the net gain. Operation counts differ from entity counts because an entity can contain several paths. The slight input/output count variation between separate slices is covered by the full-output comparison below.

### Complete-output comparison and remaining limits

`scripts/compare_cpu_slicing.py` read all four complete G-code files, normalizing only the generated-at timestamp. Its report is `build_logs/local_z_vulkan/full_comparison.json`. It returned a difference result, not an equivalence pass:

- All four runs have **998 identical Local-Z scheduler records**, including identical record hashes.
- The monitored warning counts are identical: zero sequence mismatches, reserve exhaustion, unreconciled trailing transitions, base-clip leaks, or pass-height mismatches. Each has the same 256 `no path passes` messages.
- Total filament estimates are 292.97 / 292.92 g for CPU and 292.96 / 292.95 g for Vulkan.
- Normalized full-output hashes differ even between CPU repeats. For example, CPU internal-solid-infill command counts are 71,437 and 60,842; Vulkan counts are 70,608 and 68,789. Other roles also have small differences. The Vulkan infill counts lie within the CPU repeat range, but this does not prove every difference is harmless or pre-existing.

The strict replay and 602,110 within-run post-seam skip validations establish the tested rejection behavior. They do **not** establish complete-model output equality or printer validation. The complete-output nondeterminism remains unresolved, as it was in earlier CPU and seam measurements.

The feature remains experimental and opt-in. Before default enablement, expand the model/device/platform corpus and bound per-dispatch edge/query work: the current memory/count caps do not guarantee a short dispatch for every pathological mask. The current full-model speedup is measured only on this Windows/NVIDIA configuration; Intel has correctness and captured-replay results, not a full-model speedup measurement.

To reproduce the complete comparison after the timing runs:

```powershell
python scripts/compare_cpu_slicing.py --before build_logs/local_z_vulkan/cpu_timing_1 build_logs/local_z_vulkan/cpu_timing_2 --after build_logs/local_z_vulkan/nvidia_timing_1 build_logs/local_z_vulkan/nvidia_timing_2 --output build_logs/local_z_vulkan/full_comparison.json
```

The retained `summary.json` includes diagnostic and historical failed runs with their exit codes for traceability; its headline medians select only `cpu_timing_*` and `nvidia_timing_*`. Final regression, NVIDIA/Intel hardware, replay, and Vulkan-disabled build evidence is in `tests_final_regression.log`, `tests_shutdown_*.log`, `replay_final_*.json`, and `build_disabled_final.log`, respectively. Build logs are in `build_shutdown.log`.
