# Vulkan candidates after the CPU follow-up

Date: 2026-10-15. Source: `24a8e7f2f6`. This is source review and analysis of the final profiles, not a new GPU implementation or benchmark of a proposed kernel.

The current Starry Night profiles do **not establish another large, low-risk Vulkan win**. Existing Vulkan acceleration remains available. The evidence favors reducing repeated CPU work and exposing independent CPU jobs before adding GPU transfers. This conclusion is specific to these workloads and this machine; it does not mean GPU compute cannot help slicing.

This follows the [indexed CPU/Vulkan comparison](LocalZ-Indexed-Classifier-Results.md) and the [implemented CPU changes and before/after results](CPU-Overhang-Parallelism-Results.md).

## Evidence and priorities

Final profiles use the optimized Release binary on the Core Ultra 9 285H / RTX 5060 Laptop system. Ordinary and Local-Z complete profiled processes take 92.02 and 131.97 seconds respectively. These are separate from the unprofiled before/after measurements. Entries below are estimated inclusive CPU-seconds from weighted running-state stacks, not exact function timers or predicted elapsed-time savings. CPU time across workers can exceed stage elapsed time; nested entries overlap.

| Candidate | Ordinary CPU-s | Local-Z CPU-s | Decision |
|---|---:|---:|---|
| Line-distance queries during extrusion-quality evaluation | 2.17 | 2.02 | Possible small GPU experiment after separating unsigned distance and inside/outside costs. Currently called per path during serial emission. |
| Line-distance queries during curl estimation | 2.46 | 3.12 | Shares a potential query kernel, but layers depend on previous-layer geometry. Investigate CPU work within each layer first. |
| Seam-candidate support distance queries | 3.65 | 5.05 | Lower priority: already part of parallel per-layer setup. CPU-seconds do not translate directly into elapsed-time savings. |
| Voronoi construction during painted-region processing | 53.32 | 53.83 | Large CPU cost, but already parallel; exact predicates and topology make full GPU replacement a substantial algorithm project. |
| Local-Z extrusion clipping | Not applicable | 15.65 | CPU parallel preparation and a rigorously validated unchanged-path bypass are more direct possibilities. |
| Local-Z mask compensation during export | Not applicable | 4.11 | Polygon offsets/Boolean operations need separate profiling and correctness work, not a direct shader translation. |

Seam-query attribution includes anonymous worker frames at `SeamPlacer.cpp:1188/1196`; looking only for a named `SeamPlacer::init` ancestor misses those stacks. Mask compensation also accounts for about 2.18 CPU-seconds in Local-Z tower/finalization, separate from export.

The combined extrusion-quality/curl query estimates are 4.63 seconds ordinary and 5.13 seconds Local-Z. Even assuming these costs are entirely on the critical path, they are only an optimistic removal ceiling. A nearest-distance GPU kernel would retain CPU work, incur packing/transfers/synchronization/rechecks, and might move a pipeline bottleneck elsewhere. They are **not expected savings**.

Mesh slicing takes about 1.42-1.46 elapsed seconds on this model. The newly parallel tree filter takes about 0.67 seconds. Neither is a compelling next GPU target here. Earlier visibility and seam-cache experiments likewise show why an isolated kernel improvement is insufficient evidence for a complete-slice improvement.

## What a distance-query experiment must preserve

[`estimate_points_properties`](../src/libslic3r/GCode/ExtrusionProcessor.hpp) is adaptive. It first queries original points, then inserts intersections and additional samples based on those results. Export invokes it after seam selection and clipping, per path. Large batches require a preparation phase with stable inputs; dispatching for every small path is unlikely to pay off.

Curl estimation calls it with intersection insertion enabled in [`SupportSpotsGenerator.cpp`](../src/libslic3r/Support/SupportSpotsGenerator.cpp). The resulting geometry becomes the next layer's input, alongside curl heights. This prevents naive independent cross-layer batching in the current algorithm, not every possible reformulation.

A correction from source review: [`LinesDistancer::distance_from_lines_extra`](../src/libslic3r/AABBTreeLines.hpp) obtains the unsigned nearest distance, takes its square root, then multiplies by `outside(point)` for signed queries. `outside()` performs coordinate-aligned ray counting and parity checks, with another-axis retry and boundary/uncertainty handling. **The sign does not come from the nearest line.** A nearest-line kernel alone cannot replace the signed query.

The next useful experiment is CPU-only instrumentation or capture/replay separating:

1. Unsigned nearest-distance traversal from inside/outside testing.
2. Original-point queries from adaptive queries and intersection insertion.
3. Per-layer query counts and batch sizes, including acceleration-structure setup and lifetime.

Only if this reveals enough removable work should a Vulkan replay be built. A possible implementation conservatively returns candidate lines, with CPU arithmetic rechecking distances and retaining inside/outside and downstream decisions. Candidate completeness needs an error bound derived from the actual arithmetic and supported coordinate range; an arbitrary tolerance is insufficient. Preserve nearest-line identity/tie behavior wherever downstream code consumes it, particularly curl estimation. Unsupported or ambiguous cases fall back to the original CPU query. This candidate approach need not assume GPU FP64 support or throughput, but float32 plus rechecks alone does not establish correctness.

Measure packing, structure construction, upload, dispatch, readback, retained CPU work and fallback rate together. Validate final processed points, speeds and overlaps as well as query results. Use bounded memory and several workloads, including unpainted overhang-heavy and dense-mesh models. Live integration needs repeated paired complete-slice measurements demonstrating a gain beyond observed variability. Claude's proposed “three times the run spread” rule is a screening heuristic, not a significance test.

## More promising CPU follow-ups

These are source-verified opportunities, **not additional implemented changes or measured speedups** in this series:

* **Deserialize painting data once per volume.** The volumes-by-filament-state loop in [`segmentation_top_and_bottom_layers`](../src/libslic3r/MultiMaterialSegmentation.cpp) calls `FacetsAnnotation::get_facets_strict` for each state. That wrapper in [`Model.cpp`](../src/libslic3r/Model.cpp) reconstructs a `TriangleSelector` and deserializes the same annotation each time. The selector's extraction method is `const`. Reusing a selector could remove repeated work; bounded per-state slab slicing and ordered merging are a subsequent option. Constness alone does not prove thread safety or exact output equivalence. Fresh profiles attribute 6.72/6.76 inclusive CPU-seconds to `FacetsAnnotation::get_facets_strict` during painting, but do not isolate how much reuse eliminates. Whole-stage selector construction/deserialization includes other callers and must not be added to those inclusive figures. Claude's older approximately 13-second serial-window observation is not a fresh timer.
* **Within-layer Local-Z preparation.** Independent clipping jobs could precede ordered consumption, with owned geometry and immutable snapshots. Seams, arc metadata, mixed heights, pointillism, cancellation and memory bounds need explicit validation. Stateful G-code emission stays ordered.
* **Within-layer curl work and unnecessary copies.** Independent perimeter work may permit ordered parallel execution within one layer. `ExtrusionQualityEstimator::prepare_for_new_layer` also copies the next layer's distance structures into the previous layer's slots before replacing them; moving those structures is worth auditing and measuring. Cross-layer dependencies remain.
* **Unchanged-path admission.** A classifier can recognize geometrically contained paths, but bypassing Clipper also requires preserving point normalization/order, direction and extrusion/arc metadata. The diagnostic `KeepWhole` result remains inactive in production.

## Review record

The discussion used the reusable `claude-conversation` skill and Claude Code's CLI, without controlling the desktop. Conversation: **Vulkan GPU geometry acceleration review**, session `d30d83f1-1057-401e-b61d-16c037d2700e`.

Claude reviewed the CPU approach, reviewed the implementation read-only, then reviewed future GPU candidates against updated profiles. Codex independently inspected material source claims. In a follow-up, Claude accepted the correction about inside/outside testing and lowered the distance kernel's priority further. Agreement between reviewers is not validation of an unimplemented algorithm.

Raw evidence and transcripts are retained under ignored `build_logs/classifier_review/`: `cpu_profile_ordinary`, `cpu_profile_local`, `query_profile_summary.json`, `distance_callers.json`, `claude_vulkan_next_review.json` and `claude_vulkan_correction.json`. No new Vulkan kernel or GPU default was introduced by this final review.
