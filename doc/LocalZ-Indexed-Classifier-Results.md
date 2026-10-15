# Local-Z indexed CPU classifier review

## Scope and decision protocol

This experiment compares an indexed CPU classifier with the existing Vulkan empty-path classifier. It does not implement GPU polygon construction or bypass nonempty clipping. The source checkpoint before this experiment was `c794213bf7`.

The new `LocalZClipIndex` owns a mask's edges and builds an integer bounding-volume tree. A query visits nodes that may intersect the segment's expanded bounding box or its positive-X winding ray. Leaves use the same orientation, winding and two-unit boundary guard as the existing brute-force reference. Invalid ranges, unsupported coordinates and ambiguous boundaries return `Unknown`. The supported coordinate range remains plus or minus 2^29.

The optional diagnostic protocol adds `KeepWhole = 2`, alongside `Unknown = 0` and `Empty = 1`. Existing production queries leave `classify_keep` zero, preserving the binary protocol. Consumers discard only decisions equal to `Empty`. A geometric keep decision does **not** prove Clipper's ordered output, direction, duplicate-point handling or arc-fitting metadata. Nonempty clipping still runs on the CPU.

Two diagnostic export modes are available:

* `ORCA_LOCAL_Z_CLIP=indexed` uses the CPU index to reject provably empty entities.
* `ORCA_LOCAL_Z_CLIP=indexed_validate` additionally runs the existing Clipper reference check for every discarded entity.

Existing `cpu`, `vulkan` and `validate` meanings and GUI defaults are unchanged. Indexes are built per prepared layer, shared by queries using that mask, and released after classification. Existing limits on packed edges, queries and entities still apply. There is no cross-layer speculative queue.

## Captured replay

Input: `build_logs/local_z_vulkan/capture_1.jsonl`, the existing systematic one-in-257 capture from Starry Night. It contains 3,182 operations and 37,347 segments. Of the reference outputs, 2,301 are empty and 881 have exactly the same ordered point arrays as the input; zero captured cases are geometrically changed. This is one sampled workload, not a statistical bound on all clipping calls or other models.

Both CPU and GPU classifiers match the brute-force decisions exactly. The shadow keep classifier admits all 881 unchanged cases with no ordered-point mismatches in this capture. That observation is insufficient to enable a nonempty bypass.

| Measurement | Indexed CPU | NVIDIA Vulkan |
|---|---:|---:|
| Setup, including index construction or GPU creation/upload | 312.19 ms | 153.88 ms |
| Query median, five repetitions | 9.77 ms | 5.24 ms |
| Query range | 9.09–10.40 ms | 4.97–5.79 ms |

The brute-force CPU classifier takes 131.44 ms; reference clipping takes 132.99 ms on the same captured operations. The index improves CPU queries substantially, but NVIDIA wins this replay, including setup. The replay constructs an index per captured operation and retains approximately 126.77 MiB of accounted edge/node storage; unlike the live exporter, it does not deduplicate repeated masks. Its setup and memory totals must not be extrapolated directly to the live per-layer path.

## Full-export comparison

Three fresh-process runs per backend used the same application DLL (`ae3e0a27d91c33ae7f55387c7d68acb07606592f6565dba1ebd3837d5f45134a`). Run order was Vulkan, index, index, Vulkan, Vulkan, index; a separate validating index run preceded them.

| Measurement | Indexed CPU | NVIDIA Vulkan |
|---|---:|---:|
| Complete classifier preparation median | **2.696 s** | 3.552 s |
| Preparation range | 2.453–2.765 s | 3.337–3.895 s |
| Index construction median | 0.404 s | — |
| Index queries and entity reduction median | 2.027 s | — |
| GPU initialization median | — | 0.137 s |
| GPU uploads median | — | 1.389 s |
| GPU dispatch/readback median | — | 1.551 s |
| Complete CLI process median | 167.57 s | 143.68 s |
| Complete CLI process range | 149.20–172.75 s | 139.83–177.93 s |
| G-code export median | 61.08 s | 58.91 s |

The indexed CPU classifier saves a median 0.855 seconds of preparation (24.1%) versus Vulkan in the live exporter. This includes mask construction, query packing and all backend preparation, rather than shader time alone. Each run prepares 1,254 layers, about 10.54 million segments and 817 thousand entities. Approximate mean preparation per layer is 2.15 ms for the index versus 2.83 ms for Vulkan; this average is not a per-layer latency distribution.

**No complete-slice speedup is established.** Large timing drift also affects slicing stages that finish before this classifier is used. The table retains all runs rather than removing inconvenient samples. The earlier profiled binary completed the same model in 137.64 seconds, but that is neither an unprofiled control nor the same build. The defensible conclusion is that live reuse and batch size reverse the isolated replay ranking; the overall gain is small enough to require more controlled timing. The index therefore remains a diagnostic choice, and GPU preferences are not changed.

The final source also handles an empty ring sharing the next packed mask's offset; a dedicated export fixture covers that case. The full-model measurements above precede that edge-case fix. No nonempty keep bypass is enabled.

The complete-output comparison did not pass normalized byte equality, including repeated runs within each backend. All six runs did retain the same Local-Z dependency schedule and warning counts. This matches the earlier nondeterminism concern; it does not establish that every output difference is harmless. The comparison report is retained as a failed equality check, not relabeled as successful validation.

## Validation and reproducibility

Machine: Windows, Intel Core Ultra 9 285H, 16 logical processors, NVIDIA GeForce RTX 5060 Laptop GPU. Power settings are unchanged; clocks are not locked.

The focused `[LocalZ],[VulkanCompute]` suite passed 265,382 assertions in 20 cases. Deterministic fixtures exercise random segments, holes, overlapping masks, boundary neighborhoods, degenerate edges and segments, invalid ranges, empty masks and coordinate limits. NVIDIA hardware tests cover binary and three-state queries. Captured replay passed 227,243 assertions.

After the empty-ring fixture and an unrelated velocity-limit characterization test were added, the combined `[LocalZ],[VulkanCompute],[VelocityLimits]` suite passed 265,414 assertions in 22 cases; the Release application build also passed.

The full-model `indexed_validate` export checked 10,536,721 segments and verified all 602,080 rejected entities against Clipper. The largest live layer index accounted for 320,660 bytes of edge/node storage. This excludes packed queries, maps and allocator overhead. Validation timings are not performance measurements.

Input hashes:

* Local-Z Starry Night: `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2`.
* Controlled ordinary copy used for CPU profiling: `1fba784fec8ae005528cac2573a61ab9a3d0eba0aaaa4a5eaefa9ffdaed6449b`.

Timing runs use fresh CLI processes and separate output/data directories. CPU seam caching stays enabled; seam visibility and polygon compute stay on CPU; the painted-island shortcut stays disabled. Vulkan runs explicitly select NVIDIA. No builds or hardware tests compete with the slices. The user's GUI is not controlled or closed.

Raw evidence is under ignored `build_logs/classifier_review/`, including the preserved original runtime, profiles, environment records, test logs, replay JSON and complete slice outputs. Complete G-code equality is a separate concern: prior unchanged baseline repeats already differ, so classifier equality is not presented as full-output or physical-print validation.

Build and test commands:

```powershell
cmake --build build-dev-release --config Release --target libslic3r_tests Snapmaker_Orca_app_gui --parallel 8
& build-dev-release/tests/libslic3r/Release/libslic3r_tests.exe '[LocalZ],[VulkanCompute]'
```
