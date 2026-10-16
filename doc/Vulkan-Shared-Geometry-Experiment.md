# Shared slicing compute experiment

Date: 2026-10-15. This follows the shared-target audit in
[Vulkan-Compute-Acceleration-Design.md](Vulkan-Compute-Acceleration-Design.md#14-next-shared-target-fresh-profile-audit-2026-10-15).

Subsequent [CPU geometry reuse improvements](CPU-Geometry-Reuse-Results.md) enable CPU seam caching by default and reduce wall/shell work. The defaults and measurements below describe this earlier experiment.

The later [U1 project experiment](U1-Paint-Shortcut-Results.md) tests polygon rejection on a different painted model, including actual empty wall/shell operations and a separate uniform-island comparison.

**Outcome:** the new Vulkan paths remain experimental and off by default. CPU seam-neighborhood caching produces the useful shared improvement: about 3.0% ordinary / 5.2% Local-Z whole-process reduction in these two-run comparisons. GPU prefetch does not establish an additional benefit over caching. The polygon empty-result prepass costs more than it saves. To evaluate the winning CPU path, use `ORCA_SEAM_ALIGNMENT=cache`; its default remains the original `cpu` path.

## Scope

The experiment adds two mechanisms to stages used with and without Local-Z:

* A Vulkan KD-tree kernel produces **ordered seam-neighbor lists**. The CPU still chooses seams, checks finalized perimeters, follows seam strings and fits curves. A bounded CPU cache is measured separately from GPU prefetching.
* A Vulkan crossing/containment prepass can prove that a complete closed-polygon intersection or difference is empty. It is connected to selected Boolean operations in **wall generation, vertical shells and painted-region preparation**. All other operations retain the original Clipper execution.

This is **not a GPU implementation of nonempty polygon clipping, offset cleanup, or painted-region Voronoi construction**. Those remain the larger, harder geometry tasks. In particular, the wall hook does not intercept ClipperOffset's internal union cleanup. No vendored dependency was changed.

Seam work is shared between Local-Z and ordinary slicing when `spAligned`, `spRear` or `spAlignedBack` is selected. Nearest/random seam modes do not run alignment and cannot benefit from this cache/kernel. The paint hook applies to painted-region preparation, not every ordinary single-material print.

## Runtime controls

The existing `ORCA_ENABLE_VULKAN_COMPUTE` CMake option remains OFF by default. The local experimental build enables it. Neither new GPU path is enabled by default.

| Variable | Values / behavior |
|---|---|
| `ORCA_SEAM_ALIGNMENT` | `cpu` (default): original lookups; `profile`: original lookups plus reuse counters; `cache`: CPU lookup cache; `vulkan`: cache with GPU prefetch; `validate`: compare all usable GPU lists and cache hits with the original ordered CPU result. |
| `ORCA_SEAM_NEIGHBORS_PREFETCH` | `seeds` (default): adjacent-layer perimeter seam candidates; `all`: all adjacent-layer candidates. Both include the current requested query. |
| `ORCA_SEAM_ALIGNMENT_REPORT` | Optional JSONL report path, one record per alignment scope. CPU mode times only the scope, so its individual lookup counters are zero. |
| `ORCA_POLYGON_COMPUTE` | `cpu` (default); `profile` (original CPU plus counters/capture); `vulkan` (skip proven-empty operations); `validate` (always run original CPU and reject any false-empty proof). |
| `ORCA_POLYGON_STAGES` | Optional comma-separated selection from `walls,shells,paint`; omitted/empty selects all three. |
| `ORCA_POLYGON_PROFILE` | Optional JSON summary path. |
| `ORCA_POLYGON_CAPTURE` | Optional JSONL capture, every 127th eligible operation per stage, capped at 64 MiB total. Diagnostic only. |
| `ORCA_VULKAN_DEVICE` | Existing device-name filter, e.g. `NVIDIA` or `Intel`. |

No Vulkan dependency is needed in a disabled build. Unsupported devices and runtime backend failures fall back to CPU in `vulkan` mode; `validate` treats backend failures as failed experiments. Input outside the supported geometry/query bounds stays on CPU.

## Correctness and bounds

The seam kernel preserves CPU KD-tree traversal order and strict radius semantics. Near floating-point decision boundaries, or when a list exceeds 256 neighbors, the **whole query** falls back to CPU. Degenerate zero-radius and other unsupported queries also remain on CPU. A full-model validation initially exposed this zero-radius case; it was fixed before performance measurement and has a regression test.

The neighbor cache accounts for at most 64 MiB of entries, with an additional 131,072-entry limit. This is an accounting limit, not a process-memory measurement. It is destroyed after each object's alignment. GPU prefetch bookkeeping is limited to 8,192 layer/radius/Z groups. Each KD-tree has at most 1,048,576 slots; GPU batches contain at most 16,384 queries. A result occupies 1,032 bytes, so a maximum result batch is about 16.1 MiB, plus staging, node storage and CPU allocations. Only immutable geometric results are cached; mutable seam selection remains on CPU.

Closed-polygon proofs check containment in **both directions**, using nonzero winding and the existing bounded int64 Local-Z segment predicate. Touches, crossings and a two-integer-coordinate rounding guard force CPU execution. A proof skips the complete operation; it never filters arbitrary input rings or constructs approximate intersection coordinates. Admission requires at most 16,384 edges, coordinates within ±2^29, and an edge-count product between 2^18 and 2^23. Union, XOR, unsupported fill rules and larger cases remain on CPU.

The shared polygon backend serializes upload/dispatch/readback. Its queue can therefore block otherwise parallel CPU workers. This cost is part of the experiment, not an assumed acceleration. Application shutdown releases the backend before unloading the application DLL and before the shared Vulkan instance is destroyed.

## Polygon opportunity and replay measurements

Initial instrumented captures used Starry Night with Local-Z enabled and a controlled copy with Local-Z disabled. The latter still includes painting and ordinary mixed-filament regions; it is not a single-material benchmark.

| Eligible work | Ordinary slicing: operations / empty | Local-Z: operations / empty |
|---|---:|---:|
| Walls | 7,409 / 2,703 | 5,366 / 2,269 |
| Vertical shells | 188,378 / 1,023 | 186,592 / 199 |
| Paint preparation | 2,063 / 0 | 2,069 / 0 |

The CPU times summed across eligible empty calls were approximately 1.04 / 0.77 seconds for walls, 0.69 / 0.13 seconds for shells, and zero for paint (ordinary / Local-Z). These are sums of elapsed time inside concurrent calls, **not serial wall time or measured process CPU time**. They include scheduling effects and do not describe all geometry work in a stage.

The ordinary capture retained 17 paint, 59 wall and 979 shell operations before the total capture-size limit. Thus it is a bounded sample, not a uniform sample of the entire model. Serial replay measured packing, upload, dispatch and readback, excluding backend initialization and the independent validation oracle:

| Captured stage | CPU clipping | NVIDIA prepass | CPU clipping avoided | Intel prepass | Proven empty |
|---|---:|---:|---:|---:|---:|
| Paint | 10.50 ms | 26.50 ms | 0 | 44.66 ms | 0 / 17 |
| Walls | 19.87 ms | 58.64 ms | 8.60 ms | 141.22 ms | 27 / 59 |
| Shells | 511.55 ms | 1,504.79 ms | 4.96 ms | 3,043.81 ms | 11 / 979 |

The remaining CPU clipping must **still be added** to GPU prepass time. None of these stages earns back its prepass cost on this capture. The opt-in polygon path is retained for experiments and validation; it is not recommended for these workloads. A meaningful shared polygon speedup requires accelerating the nonempty topology work rather than this empty-result shortcut.

## Seam timing experiment

All timing runs use DLL SHA-256 `fbd355273d4ebc306fc14ecdcd0d86c5342370794fa9c0cc6b0ae20b0633796a`, on an Intel Core Ultra 9 285H (16 logical processors) under the existing Windows Balanced power plan. Jobs run sequentially, with no build or hardware-test jobs competing during timing. Clock frequencies are not locked. The open GUI instances are left untouched.

The Local-Z project SHA-256 is `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2`; the controlled ordinary copy is `1fba784fec8ae005528cac2573a61ab9a3d0eba0aaaa4a5eaefa9ffdaed6449b`. Every variant retains `ORCA_LOCAL_Z_CLIP=vulkan`, `ORCA_SEAM_VISIBILITY=cpu`, and `ORCA_POLYGON_COMPUTE=cpu`. Local-Z clipping dispatches no work in the ordinary project. Capture, reference validation and sampling profilers are disabled for these runs.

The first matched series is exploratory, with one run per row:

| Workload / seam mode | Seam alignment | Complete process | G-code export, including seams |
|---|---:|---:|---:|
| Ordinary, original CPU | 6.655 s | 126.157 s | 33.527 s |
| Ordinary, CPU cache | 3.050 s | 122.196 s | 29.242 s |
| Ordinary, Vulkan + cache, seeds | 4.145 s | 123.183 s | 30.876 s |
| Ordinary, Vulkan + cache, all candidates | 9.012 s | 122.585 s | 34.868 s |
| Local-Z, original CPU | 13.651 s | 173.728 s | 68.149 s |
| Local-Z, CPU cache | 5.979 s | 163.226 s | 60.164 s |
| Local-Z, Vulkan + cache, seeds | 6.820 s | 164.215 s | 62.150 s |

**The useful improvement is CPU reuse, not incremental GPU acceleration.** Seed prefetch adds about 0.9–1.1 seconds to the cache path in these runs. Ordinary seed prefetch computes 262,889 lists, but only 78,575 lookups hit GPU-produced entries. Prefetching all candidates computes 4,656,813 lists, causes 4,717,736 evictions and serves only 46,145 GPU cache hits. These are lookup counts, not counts of distinct useful lists. Its combined preparation/dispatch/cache-insertion cost is 5.786 seconds, and whole alignment becomes slower than the original CPU path.

The large-batch row also demonstrates whole-job noise: its total process happens to be shorter than the first CPU baseline despite making alignment slower. Other geometry stages vary independently. This single-run total must not be advertised as a GPU speedup. The final CPU/cache comparison repeats both configurations in reverse order; the GPU rows remain exploratory single runs.

The CPU cache accounts for approximately 46.4 MiB at peak in ordinary slicing and 49.7 MiB in the first Local-Z run, and is destroyed after alignment. Whole-process peak commit is dominated by earlier model loading/preparation and is not a clean measurement of this cache's incremental cost.

### Repeated CPU/cache comparison

Two fresh runs per configuration, with the second pair in reverse order. All exited successfully and use the same DLL and project hashes above. These are medians of two runs on one model per configuration, not a broad benchmark suite or confidence interval.

| Workload / metric | Original CPU median | CPU cache median | Reduction |
|---|---:|---:|---:|
| Ordinary, seam alignment | 6.491 s | 3.009 s | 53.7% |
| Ordinary, complete process | 124.170 s | 120.427 s | 3.0% |
| Ordinary, G-code export | 32.836 s | 29.227 s | 11.0% |
| Local-Z, seam alignment | 14.059 s | 6.093 s | 56.7% |
| Local-Z, complete process | 173.219 s | 164.246 s | 5.2% |
| Local-Z, G-code export | 68.330 s | 60.538 s | 11.4% |

Complete-process ranges were 122.184–126.157 / 118.659–122.196 seconds for ordinary CPU/cache, and 172.711–173.728 / 163.226–165.266 seconds for Local-Z CPU/cache. Alignment ranges were 6.327–6.655 / 2.967–3.050 seconds and 13.651–14.468 / 5.979–6.207 seconds respectively. Scope timing supports the reuse benefit more directly than whole-job totals, which include unrelated variable geometry work. Alignment timing ends before cache/backend destruction; whole-process timing includes cleanup.

### Output comparison limits

`compare_cpu_slicing.py` compared complete G-code, normalizing only the generated-at timestamp. **The files are not byte-identical, including between the two original CPU runs in each workload.** Consequently these comparisons do not establish full-output equivalence or prove every difference harmless.

In the CPU/cache comparisons, ordinary total filament estimates were 235.25–235.26 g; Local-Z estimates were 292.97–292.99 g. All four Local-Z runs retained the same 998 dependency schedules (digest prefix `a35e95bf06`) and identical diagnostic counts: zero sequence mismatch, reserve exhaustion, trailing transition, clip leak or pass-height mismatch, and 256 existing “no path passes” messages. Ordinary runs had zero Local-Z schedules and zero such diagnostics. Infill command counts vary even in the CPU controls; complete per-role hashes/counts remain in the comparison JSON files.

The seed-GPU runs also differ from the CPU controls in normalized full G-code. Their schedule digests and diagnostic counts match those controls; total filament estimates are 235.25 g ordinary and 292.99 g Local-Z. Comparison-script exit code 1 records these output differences, rather than a successful byte-equivalence check. No claim that all differences are harmless follows from the matching diagnostics.

The stronger correctness checks for the new code are the **within-run ordered CPU/GPU neighbor comparisons**, complete-operation empty-proof validation, and integration tests described below. Physical-print validation and complete geometry equivalence are not established by these diagnostics.

## Evidence

Full-model `validate_plain_2` and `validate_local_2` completed with exit code 0. They checked 262,706 / 401,111 usable GPU lists and 2,782,482 / 5,995,252 cache hits against the CPU, preserving list content and order. Eight / ten unsupported queries used the CPU directly. Wall and paint integration validated 9,475 / 7,441 GPU operations; 2,700 / 2,267 wall empty proofs were confirmed by Clipper, with no false-empty result. Paint yielded no empty proof. Shells were validated through captured replay, not a full-model shell validation run.

`validate_all_plain_1` separately completed with exit code 0 using all-candidate GPU prefetch. Another 4,654,062 usable GPU lists and 2,787,345 cache hits matched the CPU exactly. This validates the larger policy even though its performance result is negative. Output-comparison scripts ran concurrently with this diagnostic job; its duration is not used as a performance measurement. Across the three model validations, 5,317,879 GPU lists and 11,565,079 cache hits were checked; these are checks, not unique queries.

Captured ordinary polygon replay plus the existing kernel regressions passed 3,307,126 assertions on each GPU. Captured Local-Z replay plus old/new kernel regressions passed 3,309,488 assertions on each GPU. The CPU suite (`[PolygonCompute],[LocalZ],[VulkanCompute]`) passed 123,527 assertions in 17 cases; Clipper regression tests passed 99 assertions in four cases. The Vulkan-disabled compute-library smoke build also passed. Local-Z replay ran concurrently with a model **validation** job; its timings are not used as performance measurements.

The hidden `[SharedPolygonIntegration]` test passed 28 assertions on each GPU. It exercises both `Paths` and `PolyTree` return types through the actual Clipper wrapper, requires GPU dispatch in each of the three scopes, verifies four skips per scope, and compares nonempty results with the original CPU output. It needs `ORCA_POLYGON_COMPUTE=vulkan`, all stages selected, and an `ORCA_POLYGON_PROFILE` report path.

To repeat a model timing with the existing Windows benchmark harness, select a fresh output directory for every run:

```powershell
$env:ORCA_VULKAN_DEVICE = 'NVIDIA'
$env:ORCA_LOCAL_Z_CLIP = 'vulkan'
$env:ORCA_SEAM_VISIBILITY = 'cpu'
$env:ORCA_POLYGON_COMPUTE = 'cpu'
$env:ORCA_SEAM_ALIGNMENT = 'vulkan' # also compare cpu and cache
$env:ORCA_SEAM_NEIGHBORS_PREFETCH = 'seeds'
python scripts/benchmark_cpu_slicing.py --exe build-dev-release/src/Release/snapmaker-orca.exe --project '<project.3mf>' --output '<new-run-directory>'
```

Keep the Local-Z, visibility and polygon settings identical across seam comparisons. Clear capture/reference-validation variables for timing. Run the hidden `[SharedVulkanHardware]`, `[SharedPolygonReplay]`, `[VulkanComputeHardware]` and `[LocalZVulkanHardware]` tests explicitly when validating hardware; replay requires `ORCA_POLYGON_REPLAY` to name a capture file.

Local logs and captures are under `build_logs/vulkan_shared_geometry/` (ignored by Git). `capture_plain_1` and `capture_local_1` are diagnostic runs, not headline timing baselines. Early failed validation/build-test iterations are retained alongside corrected runs rather than overwritten.

`cpu_plain_1/2`, `cache_plain_1/2`, `cpu_local_1/2`, `cache_local_1/2`, `gpu_plain_1`, `gpu_local_1` and `gpu_all_plain_1` retain the measured jobs. `compare_cache_plain.json`, `compare_cache_local.json`, `compare_gpu_plain.json` and `compare_gpu_local.json` contain complete output comparisons and timing aggregates. The `gpu_*` comparison files use a single GPU run and must not be treated as repeated GPU benchmarks. `timing_summary.json` combines direct seam timers with process metrics.

Hardware coverage is Windows on NVIDIA GeForce RTX 5060 Laptop GPU and Intel Arc integrated graphics. AMD, Linux, macOS/MoltenVK and physical-print validation have not been performed. Vulkan portability alone does not establish those results.
