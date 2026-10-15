# Vulkan seam visibility prototype

This is an opt-in experiment following `5dff53f7c0`. It accelerates the independent visibility score assigned to each seam candidate. It does not change seam alignment, ray casting, polygon clipping, Local-Z scheduling, or G-code emission.

Measured on Windows on 2026-10-14. Runtime DLL SHA-256: `aa6d02e549ecbc444a6358f9b13b4e6a6a992794784ffdc2c342affa75de07ee`. Project SHA-256: `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2`.

Result: NVIDIA accelerates this operation by **2.67 times**, saving about **0.374 seconds** including setup and transfers. Intel also completes it faster than the CPU in one run. **No complete-slice speedup was demonstrated**, and the feature remains disabled by default.

Subsequent work: the [Local-Z clipping experiment](Vulkan-LocalZ-Clipping-Experiment.md) reuses the backend infrastructure and introduces a shared Vulkan instance with explicit application shutdown. The measurements in this seam report precede that refactoring; the numeric seam hardware checks were repeated afterward.

## Design

`SeamPlacer::calculate_candidates_visibility()` uploads the existing mesh-sample KD tree, sample normals and sample visibility values. One compute invocation performs one radius query and weighted score. The shader preserves the CPU tree's preorder traversal and strict radius predicate, with explicit float32 arithmetic and disabled contraction for the score expressions. Floating-point equality is measured, not assumed.

Queries run in batches of at most 16,384. The mesh-sample tree stays on the device for the entire object. The implementation uses device-local storage buffers, a reusable host staging buffer, explicit transfer/compute/host barriers and a fence before reading results. Buffer and device lifetimes are bounded to the operation. Timing includes backend setup and node upload, all query uploads/dispatches/readbacks, CPU packing and result assignment, and backend teardown. It is not a GPU-kernel-only timer.

Vulkan 1.0 compute and float32 are sufficient. The prototype does not require int64, float64, subgroups, hardware ray tracing, graphics surfaces, or OpenGL interoperability. Device selection prefers a discrete GPU, with a name filter for testing integrated GPUs. The loader is opened dynamically; CPU-only builds do not require Vulkan headers, a loader or shader tools.

Windows loads the system Vulkan loader. Linux uses `libvulkan.so.1`. The macOS loader paths include Vulkan and MoltenVK libraries and enable portability enumeration/subset extensions when advertised. macOS runtime packaging and hardware validation are not completed by this Windows experiment.

## Build and operation

The original prototype used an **OFF** build default. Current builds use `AUTO`; the Windows release helper provisions the build tools automatically. See [current build and preference controls](CPU-Geometry-Reuse-Results.md#runtime-controls). Explicit CMake configuration remains available:

```text
cmake -S . -B <existing-build> -DORCA_ENABLE_VULKAN_COMPUTE=ON
cmake --build <existing-build> --config Release --target libslic3r_tests Snapmaker_Orca_app_gui --parallel 8
```

Install the Vulkan SDK for development, or provide `ORCA_VULKAN_INCLUDE_DIR` and `ORCA_GLSLANG_VALIDATOR` explicitly. CMake compiles `src/libslic3r/Compute/seam_visibility.comp` and embeds the resulting SPIR-V in the backend. End users do not need the SDK or shader compiler.

Runtime controls:

| Variable | Behavior |
|---|---|
| `ORCA_SEAM_VISIBILITY=cpu` | Original TBB implementation, with stage timing. Overrides the GUI preference. When unset, the GUI preference applies (off by default); CLI defaults to CPU. |
| `ORCA_SEAM_VISIBILITY=vulkan` | GPU scoring; failure falls back to recomputing every score on the CPU. |
| `ORCA_SEAM_VISIBILITY=validate` | GPU scoring plus a CPU reference for every query; unavailable GPU or differences above `2e-6` fail the experiment. Not a speed measurement. |
| `ORCA_VULKAN_DEVICE=NVIDIA` / `Intel` | Case-insensitive device-name filter. An unavailable requested device is reported rather than silently selecting another GPU. |

Cancellation is checked between batches. The runtime default remains CPU; no automatic workload threshold or default-on policy is claimed by this experiment.

## Validation and benchmark protocol

`[VulkanComputeHardware]` tests run explicitly on each available GPU. They compare random neighborhoods against the CPU KD tree and Eigen weighting, including empty neighborhoods, duplicate samples, strict radius boundaries, incomplete workgroups, repeated dispatches, oversized-batch rejection and unavailable-device handling. `[VulkanCompute]` covers unavailable/invalid backend creation without requiring GPU hardware.

The full-model validation uses the original Starry Night project and its Local-Z settings. Subsequent performance runs use the same executable with CPU/GPU selected through the environment, run sequentially, and exclude reference validation. `scripts/benchmark_cpu_slicing.py` records the mode, device filter, DLL hash, project hash, elapsed time, process CPU time and memory.

Score comparisons alone do not establish complete G-code equivalence. Float32 differences can influence seam decisions. The pre-existing internal-solid-infill variation described in [Local-Z CPU results](LocalZ-CPU-Optimization-Results.md) also remains separate from this experiment.

## Development dependencies

Development tools were downloaded to the ignored `build_logs/vulkan_seam_prototype/tools` directory; no vendored source was modified:

- Khronos Vulkan-Headers `v1.3.290`, source archive SHA-256 `918a8bb31b8312fd649fd1d1826443da870f33f3187c16c4528aa94927409bab`.
- Khronos glslang `16.6.0`, Windows x86_64 release archive SHA-256 `82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b`.

Results and logs are retained under `build_logs/vulkan_seam_prototype/`.

## Validation results

- Release application and test targets build successfully. A separate build with `ORCA_ENABLE_VULKAN_COMPUTE=OFF` also builds and reports the unavailable GPU backend correctly.
- Regression suite: 250 tests passed, with two existing expected failures (145,204 assertions total).
- NVIDIA GeForce RTX 5060 Laptop GPU: both hardware test cases passed, 34,799 assertions. Maximum score error versus the CPU was `2.98023e-7`.
- Intel Arc integrated GPU: both hardware test cases passed, 34,799 assertions. Maximum score error was `3.57628e-7`.
- Full Starry Night reference validation on NVIDIA checked **2,596,069 queries**, with **zero** errors above `2e-6`. Maximum error was `2.98023e-7`; 991,792 scores differed at the bit level. Validation completed the full slice successfully.

These checks validate the score calculation within the stated tolerance, not identical downstream seam choices or complete G-code. Hardware validation was performed on Windows only. The Khronos Vulkan validation layer was not installed on this machine; no validation-layer-clean claim is made.

## CPU versus NVIDIA timing results

Two CPU and two NVIDIA runs alternated after full-model validation, all using the same executable and original project. No reference validation or stack sampling ran during these measurements.

| Measurement | CPU | NVIDIA Vulkan |
|---|---:|---:|
| Visibility scoring, run 1 | 596.912 ms | 243.732 ms |
| Visibility scoring, run 2 | 600.564 ms | 205.469 ms |
| Median visibility scoring | **598.738 ms** | **224.601 ms** |
| Full slice, run 1 | 180.605 s | 188.979 s |
| Full slice, run 2 | 183.655 s | 183.623 s |
| Median full slice | **182.130 s** | **186.301 s** |
| Median G-code export | 81.909 s | 81.744 s |
| Median peak working set | 5,601.3 MiB | 5,583.7 MiB |
| Median peak commit | 8,341.2 MiB | 8,332.4 MiB |

The targeted operation is **2.67 times faster**, reducing its elapsed cost by **62.5%**, or about **0.374 seconds**. The GPU times include initialization, data transfer, dispatch, result handling and backend teardown. Setup took 87–99 ms, dispatch plus readback took 79–108 ms, and remaining packing/assignment/teardown took about 39 ms.

**Complete slicing did not improve in these runs.** The measured overall median is higher for NVIDIA; most of that difference is outside G-code export, before this GPU operation executes. Two runs per mode cannot establish a whole-job speedup of approximately 0.2% amid several seconds of variation. Do not present the 2.67 times kernel-stage improvement as a slicing speedup.

Startup cost also varies. The initial reference-validation run spent 576 ms setting up Vulkan, versus 87–99 ms in subsequent timed runs. The timing series follows validation and therefore does not characterize a completely cold first use. Reusing a device/pipeline across operations may help amortize startup, but that is not implemented here.

The earlier sampled estimate of about seven CPU-seconds for this work summed multiple cores. Its directly measured CPU elapsed time is only about 0.6 seconds. This explains the limited whole-slice benefit despite useful acceleration of the operation itself.

## Intel integrated GPU result

One complete slice on Intel Arc took **186.641 seconds**, with visibility scoring at **351.016 ms**: 130.725 ms setup, 194.474 ms dispatch/readback and about 26 ms other handling. This is approximately **1.71 times faster** than the median CPU scoring time, but it is only one performance sample. The Intel hardware reference tests passed; the exhaustive full-model reference comparison above was performed on NVIDIA.

The backend was selected explicitly by name and logged the selected GPU. All six model runs (one validation, two CPU, two NVIDIA and one Intel) completed successfully. Candidate counts ranged from 2,596,009 to 2,596,158 between runs; the validation compares CPU and GPU scores against identical candidates within one execution.

## Generated-output comparison

The four CPU/NVIDIA timing runs have identical Local-Z schedule hashes (998 logged schedules) and identical Local-Z warning counts. Complete G-code hashes still differ after removing the generation timestamp, including between the two CPU runs. Internal-solid-infill positive moving-extrusion command counts are 61,276 and 71,453 for CPU, versus 71,462 and 71,390 for NVIDIA. Total reported filament is 292.93–292.99 g across these runs. This confirms that the earlier output-variability concern remains present with GPU scoring disabled; it does not prove that GPU rounding cannot affect seam choices.

The new CPU baseline also differs from the prior day's preserved build, which logged 874 schedules and roughly 279 g. The project hash and initial Local-Z plan summary match; the cause of this historical output drift is not established here. The GPU comparison therefore uses only the new executable with CPU/GPU toggled, and must not be interpreted as an output-equivalent comparison against that older build.

Retained evidence includes `summary.json`, `cpu_nvidia_comparison.json`, each run's `metrics.json` and logs, `tests_nvidia.log`, `tests_intel.log`, `tests_regression.log`, and `build_final.log`. GPU selection and numeric validation can be reproduced with:

```powershell
$env:ORCA_VULKAN_DEVICE = 'NVIDIA' # Use Intel to test the integrated GPU.
& build-dev-release/tests/libslic3r/Release/libslic3r_tests.exe '[VulkanComputeHardware]' --reporter compact

$env:ORCA_SEAM_VISIBILITY = 'validate' # cpu or vulkan for timing runs.
python scripts/benchmark_cpu_slicing.py --exe build-dev-release/src/Release/snapmaker-orca.exe --project build_logs/cpu_optimization_benchmark/inputs/starry.3mf --output build_logs/new_seam_validation
```
