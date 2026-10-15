# Vulkan compute acceleration for Snapmaker Orca

Date: 2026-10-13. Source baseline: `9a791daff1`.

Status: design proposal and source audit. No GPU slicing backend has been implemented or benchmarked as part of this document. Candidate benefits below are engineering hypotheses, not measured speedups. The existing CPU implementation remains the correctness reference and fallback.

Follow-ups: the [Vulkan seam visibility prototype](Vulkan-Seam-Visibility-Prototype.md) accelerated its operation but saved only 0.374 seconds and demonstrated no complete-slice improvement. The [Local-Z clipping experiment](Vulkan-LocalZ-Clipping-Experiment.md) targets directly timed work in sequential export: two CPU/Vulkan timing pairs on Starry Night reduced median complete slicing from 182.7 to 160.4 seconds (12.2%), and export from 83.5 to 65.1 seconds (22.0%). Its captures, conservative GPU rejection, validation, and output-comparison limits are recorded separately. The priorities below preserve the original proposal, rather than overriding those subsequent measurements.

## 1. Proposed direction

Add an optional Vulkan compute backend for selected, expensive batches of geometry calculations. Keep the slicing dependency graph, polygon topology, printer-state handling and final ordered output on the CPU initially. Continue using TBB for CPU parallelism.

The first implementation should accelerate one kernel with a demonstrated contribution to end-to-end slicing time. Leading candidates are batched spatial queries and mesh/plane intersections. Preview geometry generation is another candidate, but must be measured separately because it improves preview loading rather than the production of printable toolpaths.

The compute backend can coexist with the current OpenGL renderer. Its initial tasks should return compact results to the CPU; sharing graphics buffers with OpenGL is a separate, platform-dependent project.

Selecting the discrete GPU for OpenGL does not enable GPU slicing. Vulkan must enumerate devices, select a compute-capable device and explicitly dispatch compute workloads.

## 2. Current architecture and dependency boundaries

Relevant source entry points:

| Source | Current behavior relevant to acceleration |
|---|---|
| [Print.cpp](../src/libslic3r/Print.cpp), `Print::process` | Coordinates slicing stages, shared objects, tool ordering, wipe tower, skirt/brim and simplification. Several object passes are sequential; support generation is parallel across objects. |
| [TriangleMeshSlicer.cpp](../src/libslic3r/TriangleMeshSlicer.cpp), `slice_make_lines`, `slice_facet_at_zs`, `slice_mesh` | Parallel triangle slicing, per-layer intersection output, edge identifiers and contour reconstruction. The multi-layer path already transforms vertices before processing faces. |
| [PrintObject.cpp](../src/libslic3r/PrintObject.cpp) | Parallel perimeters, infill, ironing and several surface-classification passes. |
| [PrintObjectSlice.cpp](../src/libslic3r/PrintObjectSlice.cpp) | Volume slicing, painted-region processing, mixed-filament masks and Local-Z planning. |
| [TreeSupport.cpp](../src/libslic3r/Support/TreeSupport.cpp) and [TreeModelVolumes.cpp](../src/libslic3r/Support/TreeModelVolumes.cpp) | Support geometry, spatial constraints, polygon caches and cross-layer support dependencies. |
| [LocalZOrderOptimizer.hpp](../src/libslic3r/LocalZOrderOptimizer.hpp) | Pass ordering carries the active extruder between decisions. |
| [GCode.cpp](../src/libslic3r/GCode.cpp), `GCode::process_layers` | Ordered generation/filter stages overlap through `tbb::parallel_pipeline`; individual stages use `serial_in_order`. |
| [GCodeViewer.cpp](../src/slic3r/GUI/GCodeViewer.cpp) | Expands moves into renderable geometry and uploads OpenGL buffers. |

A stage depending on an earlier stage does not make every operation within it sequential. Surface classification can read neighboring layers concurrently once those slices are complete. Tree growth needs successive dependency frontiers, but many queries within one frontier can run concurrently.

The initial design preserves these ordered boundaries:

- Layer-height and configuration planning must establish the geometry inputs for dependent passes.
- Polygon reconstruction must receive complete segment sets for each affected layer/component.
- Tree-support growth and merging must respect their cross-layer dependencies.
- Mixed-zone continuity must propagate component start layers in the same way as the CPU implementation.
- Local-Z pass/tool order must preserve the active-extruder and height constraints.
- G-code generation must preserve motion, extrusion, tool, cooling, pressure and custom-command state.

Some of these algorithms could later be reformulated for more parallelism. They should not be declared independent merely because their outer loop iterates over layers.

## 3. Candidate compute operations

| Operation | Proposed GPU work | CPU work retained initially | Priority and limitations |
|---|---|---|---|
| Mesh preprocessing | Vertex transforms, bounds and triangle classification | Mesh ownership, repair and authoritative topology | Useful supporting kernels; small inputs may not justify transfers. Avoid duplicating transformations already done once on the CPU. |
| Triangle/plane intersection | Relevant-plane ranges, candidate classification, count/scan/emit, eventually validated segment arithmetic | Degenerate cases, contour stitching, topology repair and polygon unions | Strong candidate for dense meshes and many planes. Existing CPU range searches already avoid testing every triangle against every plane. |
| Spatial queries | Batched ray, distance, overlap and clearance queries against an acceleration structure | Exact borderline predicates and decisions consuming results | Strong candidate when query batches are large and the structure can be reused. Software traversal is the baseline; hardware ray tracing is optional future work. |
| Support analysis | Overhang classification, collision candidates and distance-field sampling | Support placement decisions, branch merging, cross-layer propagation | Start with query kernels, not a full support-generator rewrite. Raster/distance-field approximations require explicit error bounds. |
| Mixed-filament processing | Large batches of mask candidate tests, sampling and independent color calculations | Region topology, calibration semantics, vertical continuity and Local-Z scheduling | Interesting for heavily painted models. Color arithmetic is only valuable to accelerate if enough evaluations occur in a batch. |
| Infill pattern generation | Parallel evaluation of regular patterns or implicit fields | Exact clipping, connectivity, endpoint ordering and extrusion semantics | Pattern evaluation may be cheap relative to clipping; profile both separately. |
| Polygon offsets and Boolean operations | Potential future intersection/predicate subroutines | Existing robust polygon operations | High complexity: irregular output, shared topology, numerical corner cases and variable iteration counts. Defer whole-algorithm replacement. |
| Preview geometry | Expand segments into vertices/indices, normals, colors and bounds | Move semantics, layer metadata and UI bookkeeping | Strong parallel fit, but renderer interoperability or readback/re-upload can erase the benefit. Track as a separate preview project. |

Final G-code text generation, cooling/pressure filters, tool ordering and purge planning are not first-wave GPU targets. Their state dependencies and irregular work make CPU/pipeline improvements more attractive initially.

## 4. Vulkan platform and capability policy

Use Vulkan compute as the common interface for supported NVIDIA, AMD and Intel devices on Windows/Linux. Add macOS validation through MoltenVK if acceleration is to ship there. MoltenVK implements a Vulkan portability subset over Metal; availability of a Vulkan device does not imply every desired feature is present. See [Vulkan platform information](https://www.vulkan.org/) and [MoltenVK](https://github.com/KhronosGroup/MoltenVK).

Proposed initial baseline: Vulkan 1.1 core compute and storage buffers, subject to a real device-matrix review before implementation. Do not require the newest API version for its own sake. Build the initial queue and synchronization path using capabilities available at that baseline; newer facilities can be optional.

Query and record device type, vendor/device IDs, driver information, queue families, memory heaps and kernel-relevant limits. Account for maximum storage-buffer ranges, dispatch dimensions, workgroup size, shared memory and alignment. Split work to fit limits rather than assuming the capabilities of the development machine.

Suggested capability tiers:

| Tier | Requirements | Enabled workloads |
|---|---|---|
| CPU | No usable Vulkan compute device required | Complete slicing functionality. |
| Portable compute | Core compute, storage buffers and suitable limits | 32-bit classification, conservative candidate generation and validated approximate queries. |
| Integer geometry | `shaderInt64`, plus kernel-specific validated behavior | Selected integer-coordinate calculations. This alone does not make polygon algorithms portable or correct. |
| Double arithmetic | `shaderFloat64`, acceptable measured throughput and a validated numerical contract | Selected kernels requiring double-precision intermediates. |

Vulkan exposes 64-bit integer and floating-point support as explicit device features. Check and enable only what each kernel needs; use a CPU fallback for unsupported kernels. [Vulkan feature reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html)

Prefer an eligible discrete device initially, but retain an explicit override and workload-size thresholds. An integrated GPU can be competitive for some transfer-sensitive workloads. Do not assume that launcher GPU-preference exports select Vulkan's physical device.

Use one compute device per slicing job initially. Multi-GPU scheduling would duplicate state and introduce more transfer, balancing and testing work.

## 5. Proposed software boundary and packaging

Keep Vulkan types out of `PrintObject`, `Layer`, polygon containers and public geometry interfaces. Introduce a small operation-level service whose inputs are immutable compact buffers and whose outputs follow the same contracts as the CPU helpers.

Suggested responsibilities, with names illustrative rather than committed APIs:

- `GeometryComputeBackend`: capability checks, workload admission and operation dispatch.
- `CpuGeometryBackend`: existing/reference behavior and safe fallback.
- `VulkanComputeContext`: device, queues, pipelines, command buffers and device lifetime.
- `ComputeBufferPool`: bounded reusable input, scratch and output allocations.
- Per-operation adapters: pack selected geometry, dispatch, validate and integrate results.

Add an optional CMake build switch, keep CPU-only builds working, and load Vulkan at runtime so a missing loader/driver does not prevent the application from starting. Compile compute shaders into SPIR-V during development/build packaging; end users should not need a shader compiler or Vulkan SDK. Pin shader tools and dependency versions and record their provenance.

A reasonable first shader language is GLSL compiled to SPIR-V, with fixed, documented buffer layouts. Keep kernel arithmetic independent of renderer shaders. Avoid vendor-specific extensions in the baseline.

Vulkan Memory Allocator is a candidate for buffer allocation and suballocation; its documentation covers memory selection, staging, readback and budgets. Dependency integration must follow this repository's vendoring policy. [VMA documentation](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/)

## 6. Data layout and a possible intersection pipeline

GPU input should contain flat vertex arrays, triangle indices, stable triangle/edge identifiers, sorted slicing heights and explicit transform/scaling metadata. Do not upload a C++ object graph or assume Eigen/C++ struct padding matches the shader layout. Specify every field's type, offset, alignment and interpretation.

Use offsets/counts for variable-length outputs and separate frequently accessed fields where measurement supports it. Retain stable IDs so output ordering and fallback do not depend on GPU execution order.

A possible triangle-intersection experiment:

1. Capture an immutable mesh/configuration revision and the required slice heights.
2. Reuse transformed mesh data where its lifetime and precision contract permit. Upload once per reusable mesh snapshot, not once per layer.
3. Determine the relevant height range for each triangle, preserving the current range-culling behavior.
4. Count candidate/output records for a bounded batch. An exclusive prefix sum assigns disjoint output ranges.
5. Emit records without a global lock or unbounded append counter. Check count/offset overflow and allocation limits before emission.
6. Route numerically ambiguous cases to the CPU. Return flags identifying exactly which records need recomputation.
7. Group results using stable layer/triangle/edge keys when required by the CPU integration contract.
8. Run existing contour reconstruction and polygon validation, then publish the completed batch atomically at the operation boundary.

For a conservative candidate-only kernel, the GPU must produce a superset of true candidates: false positives cost CPU work, while false negatives can remove printable geometry. A candidate-only experiment may fail the performance gate because the current CPU broad phase is already inexpensive. That outcome should stop or redirect the experiment.

Count/scan/emit may repeat some arithmetic and requires scratch buffers. Compare it with bounded per-workgroup output and CPU batching; do not assume it is automatically faster.

## 7. Numerical correctness

The current core uses `coord_t = int64_t` and `coordf_t = double` in [libslic3r.h](../src/libslic3r/libslic3r.h). Triangle slicing uses transformed float vertices with double intermediates for some intersections, rounded integer output and specific rules for shared edges and planes through vertices.

The GPU contract must preserve or explicitly validate:

- Coordinate transforms, scale, clipping ranges and integer rounding.
- Shared-edge ownership and consistent edge orientation.
- Horizontal/coplanar triangles and planes through one or more vertices.
- Zero-length segments, duplicate segments, thin walls and near-touching contours.
- Stable triangle/edge IDs and the semantics needed by contour stitching.

Do not enable aggressive floating-point transformations by default. Fused operations, reassociation and different intermediate precision can change a rounding decision. Hardware double precision alone does not guarantee identical results across vendors.

For each kernel define whether the required contract is exact integer output, conservative classification or an explicitly bounded approximation. Manufacturing geometry must retain topology; a small average coordinate error is insufficient evidence of correctness. Approximate GPU queries need conservative bounds and CPU refinement near the boundary.

Keep authoritative polygon Boolean operations on the CPU until a separate project establishes their correctness and value on the GPU. Availability of 64-bit shader arithmetic is not a substitute for robust geometric predicates or wider intermediate arithmetic where an algorithm needs it.

## 8. Synchronization, cancellation and failure

Start with a single compute-capable queue and explicit upload, compute and readback dependencies. A dedicated transfer queue is optional later work because queue-family ownership transfers and synchronization can outweigh overlap benefits.

Use barriers between producing and consuming shader passes, and fences for CPU-visible completion. Handle flushing/invalidation for non-coherent mapped memory. Avoid waiting for the entire device after every dispatch. Optional timeline semaphores or synchronization2 can simplify a later implementation if the chosen device supports them. [Khronos synchronization guide](https://docs.vulkan.org/guide/latest/synchronization.html)

Keep the UI thread out of GPU waits. Bound submitted batches so cancellation and driver responsiveness remain acceptable; an already-submitted shader dispatch cannot be treated as instantly cancellable.

Tag results with the mesh/configuration revision. Discard completed work if the project changes or the job is cancelled. Publish only complete validated results, so a CPU retry cannot duplicate geometry.

On unsupported features, allocation failure, device loss or validation failure, abandon incomplete GPU results and retry the affected operation through the CPU path when recovery is safe. Quiesce or retire resources appropriately; never recycle buffers still referenced by in-flight work. A failed backend can be disabled for the session with a concise diagnostic reason.

## 9. Memory policy

The existing memory audit showed that preview, slicing arrays, allocator caches and driver allocations can coexist. GPU acceleration must have a separate memory budget instead of allocating another unbounded copy of the entire job.

- Bound input, output, scratch and staging bytes as well as the number of in-flight batches.
- Size batches from device limits and available budget; fall back or reduce the batch when memory is tight.
- Use reusable buffers with a cap on retained capacity and an eviction policy for cached meshes/acceleration structures.
- Keep geometry cache keys tied to mesh revision, transform and relevant settings. Painting and layer-height changes invalidate different derived data.
- Release staging and temporary CPU packing arrays after their last consumer completes.
- Include renderer allocations and other applications when deciding how much VRAM to consume.
- Measure process working set, private commit, device memory and staging allocations separately; do not add overlapping accounting views.

Use `VK_EXT_memory_budget` where available as a changing estimate, not an allocation guarantee. Provide conservative behavior without the extension. [Memory-budget extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_budget.html)

## 10. Validation and performance gates

Build a CPU/GPU comparison harness for each operation before wiring it into normal slicing. Run on NVIDIA, AMD and Intel hardware; add Apple/MoltenVK coverage before enabling that platform. Missing capabilities must exercise the CPU path successfully.

The corpus should include ordinary models, high-triangle meshes, painted multi-material objects, static and gradient Local-Z mixtures, variable layer heights, tree supports, coplanar/degenerate triangles, tiny features, large coordinates and malformed inputs handled by the CPU implementation. Include repeated slicing, cancellation, edited-project invalidation, memory pressure and repeated preview switching.

Compare intermediate geometry before downstream repair can hide mistakes. Validate contour closure, holes, orientation, topology, material assignment, layer/sublayer heights and tool changes. Preserve existing output checksums where order is contractual; otherwise use canonical geometric comparisons and explicit semantic checks. Retain deterministic tie-breaking in downstream path planning.

Measure at least:

| Metric | Purpose |
|---|---|
| Full slice/export/preview wall times, separately | Establish user-visible benefit and avoid attributing preview speed to slicing. |
| CPU packing, upload, kernel, readback and CPU refinement time | Expose transfer and conversion overhead. |
| Cold and warm pipeline/cache runs | Include first-use compilation and initialization costs. |
| CPU scaling, lock waits and allocation activity | Compare against a competent CPU implementation. |
| Peak/retained host memory and device memory | Prevent a speed improvement from creating a memory regression. |
| Cancellation latency and UI responsiveness | Avoid long uninterruptible batches. |

Use repeated runs and report variability, input/configuration versions, device/driver and cache state. Set an enablement threshold based on end-to-end benefit and resource cost, not kernel-only speed. Small jobs should remain on the CPU where dispatch and transfers lose.

If a target accounts for 20% of total runtime, eliminating it entirely can only improve total time by 1.25x. A measured stage breakdown is required before estimating overall gains.

## 11. Implementation sequence

1. **Measure and improve the CPU baseline.** Add stage/operation timings and allocation/lock measurements. Fix verified waste separately so GPU comparisons do not depend on avoidable CPU inefficiency.
2. **Define one kernel contract.** Select an operation from profiles, establish precision requirements and create CPU fixtures and benchmarks.
3. **Build the optional Vulkan foundation.** Device selection, capability tiers, shader packaging, bounded memory, cancellation and CPU fallback.
4. **Implement one experimental kernel.** Prefer batched spatial queries or intersection processing when profiles justify them. Keep ordinary slicing on the CPU by default during development.
5. **Validate across vendors.** Compare intermediate and final results, profile full transfer costs and evaluate memory/cancellation behavior.
6. **Enable selectively.** Use capability and workload thresholds. Record backend/driver information for reproducible diagnostics.
7. **Expand based on evidence.** Reuse mesh data and acceleration structures across additional kernels. Treat preview interop, robust GPU polygon operations and multi-GPU execution as separate later proposals.

## 12. CPU improvements to investigate alongside Vulkan

These are the source observations and proposed experiments recorded when this GPU proposal was written. The subsequent CPU changes and measured results are tracked in [CPU-Slicing-Optimization-Results.md](CPU-Slicing-Optimization-Results.md); the table below preserves the original investigation list.

| Candidate | Observed implementation | Proposed investigation |
|---|---|---|
| Remove unintended large copies | `TreeModelVolumes` iterates `for (auto data_pair : m_layer_outlines)`, where each pair owns a `std::vector<Polygons>`, while the loop only reads settings. | Iterate by const reference and audit similar read-only loops. Validate tree-support output. This is a concrete avoidable copy; its total runtime contribution is unknown. |
| Correct task-size heuristics | A tree-outline range uses `min(1, max(16, ...))`, which always yields 1. | Establish the intended grain-size policy and benchmark several values. Grain size 1 does not imply TBB necessarily creates one task per layer. |
| Reduce intersection output lock contention | Each emitted segment takes one of 64 striped mutexes before appending to its layer vector. Different layers can share the same mutex. | Try bounded task-local buffers and bulk merge, or count/prefix-sum/write. Preserve downstream behavior and compare extra storage/merge work. |
| Improve mixed-zone continuity tracking | Each current component scans all previous active components, rejects by bounding box, then tests polygon overlap. It copies each component mask into the next tracking state. | Add a spatial candidate index where component counts justify it; borrow immutable masks during the tracking phase; compute overlap tests in parallel and preserve ordered start-layer propagation. |
| Balance independent object/layer work | Top-level wall/infill/ironing passes process objects sequentially while layers already use TBB. | Overlap eligible objects with one shared scheduler and a memory budget after auditing shared caches, progress reporting, cancellation and shared-object handling. |
| Reduce allocation churn | Polygon points use TBB scalable allocation; the previous live snapshot contained substantial allocator-held free/cached memory. | Reuse bounded scratch buffers and reserve predictable outputs. Separate speed improvements from post-slice cache reclamation; aggressive trimming can slow repeat slices. |
| Avoid repeated geometry work | Slice, surface, support and mixed-region passes contain expensive polygon operations; some already maintain caches. | Measure repeated identical inputs, then extend existing revision-aware caches only where hit rate and memory cost justify them. Do not add a second cache blindly. |
| Improve G-code processing throughput | Ordered filters can overlap, but each stage is serial for its own stream. | Profile parsing, formatting, string allocations and filter costs before redesigning stateful stages. Preserve ordering and printer semantics. |

For painted Local-Z workloads, mixed-zone tracking and polygon work deserve a separate timing category rather than being buried inside generic slicing. For tree-supported workloads, the concrete copy and task-size issues are suitable early experiments. General lock/allocation changes should be driven by profiles from representative models.

## 13. Open decisions

- Which operation dominates the expensive model: intersections, painted-region topology, perimeters, supports, G-code processing or preview generation?
- Is a Vulkan 1.1 baseline sufficient for the chosen first kernel, and which real devices need CPU fallback?
- Which numerical cases require double or wider integer intermediates, and how frequently would CPU refinement occur?
- How much geometry can remain reusable on the GPU without materially increasing total memory?
- Does native OpenGL/Vulkan interop justify its platform work for preview generation, or should preview stay separate initially?

Resolve these through instrumentation and one bounded experiment before committing to a broad port of the slicing engine.

## 14. Next shared target: fresh profile audit, 2026-10-15

After the Local-Z Vulkan experiment, the best next contained GPU investigation is **batched neighbor searches for aligned seams**. This is a different operation from the already tested seam-visibility calculation. Polygon booleans remain the larger, more difficult shared opportunity.

Two new process-only profiles use the same final DLL, SHA-256 `b5c83204b1692337f5aab6745e679957ea14285a6a3ec6c87af16fad001e2682`, with `ORCA_LOCAL_Z_CLIP=vulkan`, `ORCA_VULKAN_DEVICE=NVIDIA`, and `ORCA_SEAM_VISIBILITY=cpu`. The original Starry Night project and the existing controlled Local-Z-disabled copy retain their previously documented hashes. The disabled copy still has painting and ordinary mixed-filament regions; it is not a single-material benchmark. No new slicing optimization was implemented for this audit.

Both slices and samplers exited successfully. Local-Z clipping dispatched about 10.54 million segments on NVIDIA in the active run. The disabled run reported zero prepared layers, zero segments, zero dispatch time, and no Vulkan device initialization. Neither direct clipping capture nor reference validation was enabled.

### Current shared costs

| Work | Local-Z + Vulkan clipping | Local-Z disabled | Scope and interpretation |
|---|---:|---:|---|
| Complete profiled process | 169.62 s | 120.45 s | One diagnostic run per configuration, not a new speedup benchmark. |
| Paint / initial preparation | 37.74 s | 28.19 s | Includes painted-region Voronoi/topology work; only the first run includes Local-Z preparation. |
| Walls | 20.38 s | 16.06 s | Classic walls in this project; already approximately 8 busy cores. |
| Vertical shells | 17.82 s | 18.10 s | Approximately 11.4 / 11.7 busy cores. |
| Tree support | 11.43 s | 11.58 s | Support geometry remains shared. |
| G-code export | 63.15 s | 31.03 s | Includes seam preparation and all remaining ordered generation/filtering. |
| Seam alignment, within export | **about 12.8 s** | **about 6.4 s** | Sampled call span, not a direct scope timer; one observed thread in each run. |
| Initial mesh slicing | 1.46 s | 1.48 s | Too small here to be a substantial next whole-job target. |

Stage durations come from log boundaries. Seam alignment is a nested operation and must not be added to export. Its first-to-last sample span was 12.828 / 6.398 s; interval-weighted occupied samples were 12.718 / 6.404 s. Independently, its inclusive sample share times export CPU consumption estimates 12.10 / 6.38 CPU-seconds. These methods agree that this is substantial serial work, while remaining approximate sampling evidence. Direct scope and query-count instrumentation should precede a new kernel benchmark.

### Recommended next bounded experiment: seam-alignment neighborhoods

Relevant source: [SeamPlacer.cpp](../src/libslic3r/GCode/SeamPlacer.cpp), `align_seam_points`, `find_seam_string`, and `find_next_seam_in_layer`; [KDTreeIndirect.hpp](../src/libslic3r/KDTreeIndirect.hpp), `find_nearby_points`.

Alignment repeatedly follows a candidate into adjacent layers and tries alternative starting points for seam strings. Each lookup traverses a layer's KD-tree, constructs a vector of nearby candidates, then chooses among them while respecting already finalized perimeters. In these profiles, `find_next_seam_in_layer` accounts for approximately 11.68 / 5.91 CPU-seconds. Candidate KD-tree traversal accounts for approximately 9.31 / 4.74 CPU-seconds within export, so the entire alignment duration must not be advertised as removable GPU work.

Candidate positions and layer trees are immutable during alignment. The ordered choices and `perimeter.finalized` flags are not. This creates a useful boundary:

1. Capture query centers, target layer, actual search radius, neighbor IDs/order, and result-list sizes. Measure repeated query keys and candidate-list memory before choosing a preparation strategy.
2. Compare a bounded CPU neighborhood cache against the current implementation. Alternative seam strings can repeat geometric searches even though the final choice changes with finalized state. A CPU cache may capture much of the benefit without GPU overhead.
3. Try batching immutable adjacent-layer neighborhood discovery on Vulkan, with bounded buffers and compact candidate-ID lists. The radius derives from the starting perimeter's flow width, so it cannot be assumed constant for all queries. Avoid a GPU dispatch/readback for every sequential lookup; that would serialize the CPU around many small GPU jobs.
4. Preserve CPU radius-boundary checks, neighbor traversal order/tie behavior, comparator logic, finalized-state filtering, seam-string selection, and curve fitting. An unordered set of the same neighbors is insufficient because the CPU chooses initial candidates from the first result. Near-boundary floating-point cases require conservative candidate output and CPU refinement.
5. Compare complete aligned seam decisions and output, then time both full slicing variants against the CPU cache as well as the original path. Limit cached layer pairs/candidate lists; precomputing every possible neighbor for millions of points could erase both the time and memory benefit.

This benefits Local-Z and ordinary slicing **when seam alignment is enabled** (`spAligned`, `spRear`, or `spAlignedBack`). It does not accelerate the alignment stage for nearest/random seam modes, which do not run it. The previous visibility kernel took only 0.582 / 0.565 s in these same runs despite high aggregate CPU samples; the proposed alignment work is materially different because it runs on one thread.

No new GPU speedup is established. Even eliminating all alignment would save at most roughly 12.8 / 6.4 seconds on these profiles, and GPU preparation, remaining CPU selection, and transfers reduce that ceiling. Halving the complete alignment stage would save approximately 6.4 / 3.2 seconds; that is a scenario for evaluating value, not a forecast.

### Larger shared project: polygon intersections and topology

The most substantial general geometry target remains closed-polygon intersections/unions and offset cleanup, starting with [PrintObject.cpp](../src/libslic3r/PrintObject.cpp), `discover_vertical_shells`. This stage repeatedly intersects holes from neighboring layers, merges projected top/bottom surfaces, clips shells to internal regions, and regularizes the result. It is used independently of Local-Z when the corresponding shell-thickness mode is active.

Clipper's Boolean execution appears in 78.2% / 78.6% of vertical-shell CPU samples. Across the whole processes, 57.0% / 55.8% of exclusive source-file samples land in `deps_src/clipper/clipper.cpp`. These are CPU attribution shares, not elapsed seconds or achievable speedups. The costly paths include scanbeam edge updates, intersection-list construction, and output topology, not just coordinate arithmetic.

A contained initial study would capture closed-polygon operations from vertical-shell generation and measure how much time is spent on provably empty intersection/difference results. A batched Vulkan crossing/containment prepass could avoid those complete operations while retaining Clipper for the rest. Closed polygons require containment checks in both directions and correct hole/winding handling: absence of edge crossings alone does not prove an empty intersection. Neighboring-layer polygons may mostly overlap, so the useful rejection fraction is unknown and must be measured before implementing an integrated prepass. Preserve complete-operation semantics; do not assume arbitrary polygon filtering preserves Clipper's rounding or output order.

Accelerating the remaining nonempty booleans would require a larger exact clipping/topology project. Calling Vulkan separately inside every scanbeam is unlikely to amortize synchronization. A useful design must batch independent operation inputs and preserve degenerate/coincident-edge, winding, integer-rounding, and contour-order behavior. No vendored library was modified for this audit.

**Wall-offset arithmetic alone is a poor next GPU target.** `generate_lower_polygons_series` accounts for 55.7% / 47.5% of wall CPU samples, but `ClipperOffset::DoOffset` arithmetic is only 4.9% / 4.0% of the whole wall stage. `ClipperOffset::Execute` also runs a Boolean union to clean the offset geometry; accelerating just normals and displaced vertices leaves most of the cost intact.

There is a separate concrete CPU reuse candidate in [PerimeterGenerator.cpp](../src/libslic3r/PerimeterGenerator.cpp): every `generate_lower_polygons_series(width)` computes the same positive `end_offset = 0.5 * nozzle_diameter` for the same lower slices, including the smaller-width series. The negative/start offset depends on width. Reuse should key the exact offset value and all relevant geometry/settings; the separately computed `m_lower_slices_polygons` expression has different float/double evaluation and must not be assumed bit-identical without checking. These lower slices are the merged lower-layer geometry, also shared across compatible regions. Measure duplicate work and memory before extending reuse beyond one generator.

Painted-region Voronoi construction is another substantial shared cost for painted prints: `build_graph` accounts for approximately 59 CPU-seconds in either profile. Its event-driven construction and extended-precision predicates make an exact GPU replacement considerably more difficult than batched neighborhood queries. A raster approximation would change the numerical/geometry contract and is not part of this proposal.

### Retained evidence

`build_logs/vulkan_next_target/local_z_1/` and `no_local_z_1/` contain complete logs, G-code, metrics, raw stacks, exact-build symbol mappings, CPU timelines, and `analysis.json`. `target_summary.json` and the local `summarize.py` retain the selected function estimates and seam sample-span calculation. The runs captured 21,617 / 25,228 stacks; each had three capture failures and no depth-limit truncations. As with prior profiles, brief thread suspension, running-state sampling, and interval weighting introduce uncertainty. These profiles locate targets; they are not physical-print validation, full-output equivalence checks, or measurements of a newly implemented optimization.

The subsequent implementation and measurements are recorded in [Vulkan-Shared-Geometry-Experiment.md](Vulkan-Shared-Geometry-Experiment.md). That experiment adds seam-neighborhood compute and a bounded closed-polygon empty-result prepass in walls, shells and paint preparation. It does not replace nonempty polygon topology or Voronoi construction.
