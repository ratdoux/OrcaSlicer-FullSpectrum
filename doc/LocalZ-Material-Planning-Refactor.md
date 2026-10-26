# LocalZ material planning refactor

Plan dated 26 October 2026. The new planner must choose the smallest feasible local recipe cycle from the active ratios and layer-height limits before committing material regions or interfaces. Continuous mixed-color islands require explicit material boundaries and agreement between neighbors. A failed joining attempt must not silently turn those islands into colored fragments of a common wall.

This is a staged replacement of planning inside the existing LocalZ pipeline. Reuse slicing, polygon operations, tested interface evaluation, perimeter and fill generation, G-code modulation, and travel handling. Keep the current implementation available for comparison while the replacement is validated.

## Required behavior

- Choose cycle size locally. A difficult recipe elsewhere on the object must not enlarge an unrelated region's cycle.
- Treat maximum layer height as a constraint on an individual bead, never a preferred cycle size.
- Choose the smallest feasible cycle before constructing interfaces. Geometry and neighboring regions then validate and constrain that choice; they must not trigger an unexplained jump to a large default cycle.
- Keep ordinary bead heights within resolved per-tool minimum and maximum limits. Explicitly modeled taper tips have separate validated rules and cannot be used to evade the ordinary minimum in recipe plateaus.
- Give each physical material region an explicit boundary. Its paths must either continue through a compatible same-material connection or terminate as part of a valid island. Every perimeter path need not be a closed loop, but arbitrary color clipping cannot substitute for material-boundary construction.
- Preserve the target recipe in region interiors. Account separately for quantization, transition redistribution, and geometric volume error. Never compensate an error in one disconnected region by changing a remote region of the same recipe.
- Prefer quality over toolchange savings. A performance or search-budget limit must be reported as such, not presented as proof that a coarse stack is necessary.

## Existing constraints to replace

The bridge preflight in `PrintObjectSlice.cpp` selects one nominal-layer span that accommodates a large subset of recipes. Cycle construction then samples painted zones, builds local interfaces, and validates them. `LocalZMixBridge::shorter_cycles` refines an already selected parent using one repeat count; changes in child group or topology and any failed child reject the complete refinement.

The independent cadence builder in the same file already starts from the smallest positive recipe component. These paths need one shared feasibility solver rather than different definitions of the preferred cycle.

`LocalZMixBridgePaths::build_partitioned` builds a common perimeter and clips it into color owners for nominal bands. This protects some narrow shell coverage but does not provide the material-island invariant above. Removing it before replacement domains can preserve that coverage would reintroduce holes.

The previous Tyranid result contains 7.375 m of sloped cross-recipe perimeter out of 302.216 m total, including internal perimeters. It retains 131.100 m of nominal fallback perimeter. These are a reproducible baseline, not exterior-continuity acceptance criteria. Baseline artifacts and hashes are in `sandboxes/local-z-mix-bridges/continuous-recovery-v10/validation.json`.

## Cycle size and layer limits

Use distinct quantities throughout the API and diagnostics:

| Quantity | Meaning |
| --- | --- |
| Recipe cycle height T | Height over which the active component proportions are realized once |
| Component contribution | Total height assigned to one physical material within that cycle |
| Bead height | Thickness deposited by one pass; a contribution may require several passes |
| Planning window | Bounded region of space used to solve contacts; it does not set cycle size |
| Sampling interval | Resolution of geometry and paint samples; it does not quantize all output heights |

For a constant-section recipe with normalized positive thickness fractions w_i and one bead per component:

    h_i = w_i * T
    T_lower = max_i(min_height_i / w_i)
    T_upper = min_i(max_height_i / w_i)

Choose T_lower if it is feasible after output precision is considered. Increasing T cannot repair a component already exceeding its maximum in this one-bead model.

Examples below assume the stated minimum is the effective limit for every active tool, with no additional boundary constraint:

| Ratio | Effective minimum | Smallest continuous-valued cycle | Component contributions |
| --- | --- | --- | --- |
| 50 / 50 | 0.06 mm | 0.12 mm | 0.06 + 0.06 mm |
| 40 / 60 | 0.06 mm | 0.15 mm | 0.06 + 0.09 mm |
| 40 / 60 | 0.08 mm | 0.20 mm | 0.08 + 0.12 mm |
| 33 / 67 | 0.06 mm | approximately 0.181818 mm | 0.06 + approximately 0.121818 mm |
| 15 / 85 | 0.06 mm | 0.40 mm | 0.06 + 0.34 mm before splitting the dominant contribution |

Thus a 0.4 mm parent containing 0.16/0.24 mm is unnecessary for a free 40/60 region. With an effective 0.08 mm minimum the desired cycle is 0.20 mm; with 0.06 mm it is 0.15 mm. The new planner must not retain an artificial 0.4 mm window and then require the smaller cycle to divide it exactly.

### Multiple beads within a component

For n_i beads of material i, the thickness-only feasibility condition is:

    n_i * min_height_i <= w_i * T <= n_i * max_height_i

Each component has a union of feasible intervals over permitted positive integer n_i. These intervals can have gaps. Intersect the unions across components rather than testing only one guessed split count. Use an ordered sweep of interval lower endpoints and, at each candidate T, test:

    ceil(w_i * T / max_height_i) <= n_i <= floor(w_i * T / min_height_i)

Respect actual authored pass-count limits. Keep a computational budget distinct from an authored constraint. Handle equal minimum and maximum explicitly; a formula dividing by their difference is invalid in that case. Invalid bounds, zero or negative weights, missing tools, and a cell too short for any complete recipe need distinct results.

At the minimum feasible T, initially choose the fewest component subdivisions that satisfy the maximum and geometry limits. Additional subdivisions may be needed for contour quality. Splitting the 0.34 mm contribution above into 0.17 + 0.17 mm satisfies a 0.32 mm bead cap, but does not improve the frequency of the minority color. Report those as two passes in one recipe cycle, not two finer recipes. Never merge adjacent same-tool passes beyond their resolved limits.

The scalar solver can establish a minimum for its declared limits, slot-count rules, and precision model. It cannot prove that a particular footprint or set of neighboring interfaces is printable. Keep these results separate.

### Resolving settings

Create one limit resolver shared by independent and continuous LocalZ planning. Resolve physical tools through the existing filament and nozzle mapping, including object and region overrides.

For the new planner, use the greater of the LocalZ minimum sublayer height and the printer's effective minimum for that tool as the ordinary bead floor. Use the printer's effective maximum, including its existing nozzle-based default when unset, as the initial ceiling. Apply any applicable explicit component cap and validated geometric ceiling as additional upper bounds. Reject a minimum above the maximum; do not silently raise the maximum or lower the minimum.

This is an intentional compatibility boundary: the current bridge code uses the LocalZ floor directly, which can be below the printer minimum. Preserve that behavior in the legacy backend. Record the new planner and limit-policy version in project settings, show the effective limits in diagnostics, and do not silently reinterpret an existing project on load. The examples above describe effective limits, not necessarily the values in the current Tyranid printer profile.

Honor explicitly authored manual patterns, fixed component heights, and pass limits. Automatic minimum-cycle selection must not override those settings. Normalize recipes and any applicable calibration once; the feasibility solver consumes those resolved targets. Resolve and document existing differences between adaptive and independent component caps rather than introducing another hidden constant.

Changes to a ratio, nozzle, minimum, maximum, region configuration, or geometry must invalidate the affected cadence and neighboring contact plans. Unrelated cells can retain cached results.

### Precision and finite boundaries

Output precision must be part of feasibility. Quantize interface coordinates using the actual G-code coordinate policy; do not assume a firmware step size that the profile does not provide. Recheck emitted bead heights and bounded recipe error after quantization. Carry numerical residual only within the same persistent region, and reconcile it at a real termination. Do not repeatedly round a period and accumulate unbounded Z drift.

Object tops, first-layer boundaries, domain births and deaths are real endpoints. A planning-window boundary is not. Carry phase and unresolved material contributions between windows. For a finite domain height H, search complete cycles and a bounded closure jointly. H divided by floor(H/T_min) is only a candidate: it must belong to the feasible interval set. Prefer distributing necessary closure adjustment over a small terminal span to stretching one final stack to its cap. Report closure-induced enlargement separately from ratio-driven cycle size.

If an exact complete recipe cannot fit a finite feature, return that result. Approximation may use an explicitly defined error policy; it must not silently drop a minority component or relabel a clipped wall as a completed island.

## Planning representation

Introduce connected recipe domains before physical material cells. A recipe domain tracks where a target mixture belongs through Z; after cadence and contacts are agreed, material cells describe the physical regions occupied by each tool.

| Proposed type | Responsibility |
| --- | --- |
| LocalZResolvedLimits | Effective bounds, mapping, precision, setting provenance, and policy version |
| LocalZCadenceCandidate | Period, ordered contributions and bead slots, phase, feasible ranges, and minimum certificate within the scalar model |
| LocalZRecipeDomain | Stable ID, sampled footprints and targets, configuration identity, adjacency, and topology events |
| LocalZInterfaceContract | Shared boundary geometry, matched material passes, interface heights, termination rules, and dependencies |
| LocalZMaterialCell | Final owner, lower and upper interface patches, footprint samples, and validated neighbors |
| LocalZPlanDiagnostic | Precise failure or enlargement reason, affected domain and contact, and supported versus unsearched cases |

Keep connected components separate even when they share a recipe. Match components through successive samples using geometric overlap, motion, and configuration compatibility, with deterministic tie-breaking. Preserve many-to-many relations for splits and merges; a one-to-one assignment must not erase topology events. A small or disappearing overlap is an event to examine, not an automatic deletion based on one nozzle-width threshold. Changing gradient targets need not reset domain identity.

Use bounded overlapping Z windows with committed boundary state and sufficient lookahead for the candidate cycles, support and transition distance. If required lookahead exceeds a resource bound, report it without forcing an artificial maximum-height stack. Refine source-mesh and paint sampling locally where the smallest candidate requires resolution finer than existing nominal slices. Interpolation alone cannot recover topology absent from those samples.

The planning sequence is:

    Resolve targets and limits
      -> calculate minimum cadence candidates
      -> track connected domains and topology events
      -> validate geometry and solve neighboring interfaces
      -> finalize physical material cells and ownership
      -> generate island paths and fills
      -> schedule dependencies and emit G-code

Geometry and contact failures feed back to the affected domains and cadence candidates before publication. The early cadence is an input to this solve, not an irrevocable extrusion plan.

## Neighboring periods and fallback

Keep each region's smallest feasible cadence. Do not enlarge a fine region merely to match a distant recipe, reduce toolchanges, or put all periods on a convenient common ladder.

Equal-period neighbors are the first supported contract. Different periods require explicit matching of some same-material passes and valid births, terminations or merges of the additional passes. Initially implement verified small-integer-ratio cases, without rounding arbitrary periods onto that ratio. General unequal periods remain explicit unsupported contacts until their transition solve exists. This restriction limits the joins available, not the region's chosen quality.

Choose color order and phase per contact group, preserving already committed boundaries. Validate the actual interface heights and bead counts at both sides. Never assume the same slot index represents the same connected material when cycle periods differ. Keep a dependency graph so the resulting material cells have an executable print order and valid clearance; a compatible height field alone is insufficient.

Pure regions and the preserved first layer retain their configured treatment. Contacts may require boundary interfaces pinned to nominal heights while a mixed region retains its smaller cadence internally. These are explicit contracts, not a requirement that every internal cycle fit the nominal grid.

If a fine candidate fails, first reconsider its local contour sampling, footprint decomposition, interface layout, and permitted pass subdivisions. Revalidate both the replacement and its remainder. Do not reintroduce the previous cutback approach that preserved the new island while stranding a narrow neighboring shell.

Each fallback owns complete material cells and satisfies its boundary contract, or remains visibly classified as unsupported. In strict continuous-island output, unsupported regions cannot silently invoke `build_partitioned`. During migration the legacy path can still be selected explicitly for those regions, with coverage and discontinuity attributed to legacy output. Once finalized, fallback must not recolor or enlarge a neighbor's material cell.

The optimization order is: hard geometry, ownership, height and scheduling validity; recipe fidelity within declared tolerances; smallest local period and avoidance of unnecessary coarse regions; supported exterior continuity; toolchange and runtime cost. A constraint conflict produces a diagnostic and explicit termination or unsupported region. It must not be hidden by relaxing a higher-priority requirement.

## Implementation sequence and exit criteria

### 1 Establish diagnostics and regression fixtures

Capture deterministic small fixtures for color-clipped walls, a wide region attached to a narrow feature, changing paint on a curved wall, and unequal neighboring cadences. Include the existing cylinder, cube, Benchy and original Tyranid archive, with fixed cameras and G-code provenance.

Add diagnostics for ideal scalar period, chosen period, effective bounds, owner kind, contact outcome, and reasons for coarse or unsupported output. Separate duplicate component emission from adjacent same-tool passes and from necessary subdivisions of a dominant contribution.

Exit: each reported symptom maps to a reproducible fixture and an assertion or measured exterior region. Existing baseline audits remain reproducible.

### 2 Implement the shared cadence solver

Add `LocalZCadence.hpp/.cpp` with pure feasibility, settings resolution, precision and finite-boundary planning. Use exact small reference enumeration to check the interval solver. Compare independent and bridge planning against it in a diagnostic mode before changing paths.

Cover the example table, per-tool unequal limits, zero-weight components, maximum-induced subdivisions, disjoint feasible intervals, equal minimum and maximum, pass-count bounds, quantization, finite tails, configuration overrides and invalid inputs. Test monotonic behavior: increasing a minimum cannot lower the scalar minimum period; increasing a maximum cannot increase it under the same other constraints.

Exit: both consumers obtain consistent minimum-cycle candidates, with reproducible reasons for exceptions. A disconnected difficult recipe cannot alter a simple region's candidate.

### 3 Build persistent domains

Add `LocalZMaterialGraph.hpp/.cpp` for domains, contacts and topology events. Build it from immutable geometry and paint samples. Validate stable identities and complete ownership through splits, merges, narrow features and changing contours before driving extrusion from it.

Then route isolated domains through their own minimum cadence using existing interface and contour fitting. Add local resampling where needed. Preserve complete ownership in Z when a cycle crosses nominal slabs.

Exit: isolated test regions use their own minimum periods and remain covered through real domain endpoints. No double emission, lost small component, or artificial boundary at a planning-window edge.

### 4 Solve contacts and material cells

Add `LocalZMaterialPlanner.hpp/.cpp`. Implement equal-period contracts first, then explicit different-period transitions for verified small integer ratios, then more general contacts as separate increments. Reuse `LocalZMixBridge` interface mathematics behind an adapter; remove assumptions that a group has one fixed source list, one unchanging slot topology, and one object-wide cycle.

Commit immutable material cells only when both sides of every affected contact and all footprint remainders validate. Maintain bounded deterministic search and retain diagnostics for unresolved cases. The scalar cadence optimum and the heuristic contact solve must have separate status fields.

Exit: the controlled fixtures show actual same-material bridges or valid island terminations, with no unexplained wall color cuts. A narrow failure does not coarsen an unaffected broad region. Pure and first-layer contracts preserve coverage.

### 5 Integrate paths and retire silent fallback

Reduce `PrintObjectSlice.cpp` to orchestration around the new planner. Adapt `PrintObject.cpp` to generate perimeters and fills from final material cells, preserving region and modifier configuration. Carry cell and contact identity through the sublayer plan for diagnostics and validation.

Reuse existing sloped extrusion and G-code machinery, but validate dependency ordering, wipe-tower demand, travel clearance and preview metadata for the new plan. Remove object-wide bridge-cycle selection, post-hoc refinement as the primary quality policy, and color partitioning as an implicit substitute for island construction. Keep the legacy backend available while unsupported cases are identified.

Exit: paths agree with finalized ownership, internal material boundaries are represented, and export never reassigns physical colors independently of the plan.

### 6 Validate and roll out

Run targeted unit and integrated fixtures, then full exports of the reference models. Inspect matching front, top and oblique close-ups, including the user's marked Tyranid regions. Use single-layer and selected-cell views to distinguish path endpoints, bead contacts and geometry hidden behind the shell. Export estimates and software audits do not substitute for a physical print.

Keep the replacement behind an experimental planner version until the following gates hold:

- All controlled minimum-cycle cases reach the scalar optimum after precision and real endpoint constraints; every enlargement is classified.
- Zero unintended component duplicates or missing ownership in the controlled fixtures and archive ownership audit.
- Every controlled recipe contact is either a verified same-material connection or an explicit valid termination; no unclassified color clipping.
- Exterior shell coverage, material-boundary paths and footprint remainders pass their local checks; the known classic-loop cusps remain separately tracked until resolved.
- Geometry, bead dimensions, dependencies and travel audits pass at least the existing test scope, with new cases for different periods.
- Visual inspection confirms the regression fixtures, with exterior continuity measured directly rather than inferred from pass counts.
- No project migration silently changes the limit policy or manual pattern semantics.

## Measurements that decide success

Report exterior shell coverage by final material cells, legacy fallback and unresolved geometry. At recipe contacts, measure same-material continuations versus valid terminations and unresolved cuts. Use sampled exterior area and contact-length fractions with explicit sampling resolution; do not divide a length by an area or count internal ramps as visible-shell coverage.

For cadence, report chosen versus ideal period per domain, area-weighted coarse regions, dominant-color run height, individual bead height, and a reason histogram. Measure recipe error and displaced boundaries locally as well as total material volume. Show toolchanges, purge demand, runtime and peak memory as costs alongside quality.

The first implementation milestone is the shared cadence solver and diagnostic comparison on the Tyranid. It establishes whether a large stack is justified before the more complex connectivity refactor can use it.

## Source and review references

- `src/libslic3r/PrintObjectSlice.cpp`: independent cadence, limit resolution, bridge preflight, cycle construction, and fallback planning.
- `src/libslic3r/LocalZMixBridge.cpp/.hpp`: interface representation, refinement, contour fitting, own-region and partitioned path generation.
- `src/libslic3r/Slicing.cpp` and `PrintConfig.cpp`: existing printer limits and their defaults.
- `doc/LocalZ-Continuous-Mix-Bridges-Experiment.md`: baseline validation and known limitations.
- Claude technical review through `C:/Users/radug/.codex/skills/claude-conversation/SKILL.md`, session `f279e363-023c-45fa-9134-f1fdebe80a51`. Review identified interval gaps, finite-domain closure, limit-policy compatibility, and the need to represent extra passes at unequal-period contacts explicitly.

## Implementation status — 2026-10-26

The first experimental backend is implemented and built. This is a foundation for the full refactor, not a completed Tyranid fix. The existing backend remains the default. The replacement is selected with `dithering_local_z_planner = material_v1`, shown as **Minimum stacks and material islands (experimental)** under **Local-Z planning**.

Implemented:

- `LocalZCadence` resolves physical-tool minimum/maximum heights and finds the scalar minimum through feasible slot-count intervals. It distinguishes a recipe period from same-color bead subdivisions, quantizes output, carries local rounding debt, and preserves minimum-period prefixes before actual endpoint closure.
- `LocalZMaterialGraph` tracks connected source/configuration domains through slices, including splits, merges, lateral contacts and vertical handovers. These are sampled domains; the graph does not reconstruct missing mesh/paint topology between samples.
- `LocalZMaterialPlanner` creates contact-bounded planning runs while retaining persistent graph identity. Stable runs use their own minimum periods. Equal-period/phase, equal-slot contacts can share the existing interface solver. Per-tool limits, swept footprints, foreign ownership, contour fitting and endpoint ownership are checked before publication.
- Moving material boundaries retain explicitly classified nominal cells in the affected Z runs. Same-tool nominal cells are unioned before paths are generated. This mode cannot use `build_partitioned`; paths originate from the material cells.
- A failed contour now repairs only the enclosing range whose endpoints coincide with both recipe-cycle and nominal-slab boundaries. Distant valid cycles survive. When such boundaries do not exist, the repair still expands to the whole run; local re-tiling and spatial repair remain future work. Reported rounding debt excludes cycles removed by repair, and nominal repair volume debt has separate units.
- Minimum-cadence diagnostics are computed before applying feature height. A short-lived feature reports `domain_too_short` and retains its actual required period, rather than reporting an unexplained zero-period height failure.
- Versioned configuration, preset persistence, GUI control, invalidation, user-visible fallback warnings and volume-based diagnostics are integrated. Automatic fixed recipes and a preserved first layer are supported; manual patterns, gradients, adaptive rows, rafts and spiral mode remain explicitly unsupported in this backend.
- The Visual Studio encoding-check target now uses one command sequence. The previous repeated PRE_BUILD commands generated duplicate batch jump labels and could loop after CMake regeneration.

Validation of the final build:

- **112 LocalZ/G-code test cases, 651,625 assertions passed.** This includes 21 cadence/graph/material-planner cases and an integrated G-code export fixture.
- The controlled adjacent 40/60 and 60/40 fixture uses **0.15 mm cycles**, with **0.06–0.09 mm outer-wall beads** and actual sloped extrusion moves. Its wall audit found 0 overlaps among 488 checked segments; its travel audit found 0 crossings in 120 checked travels against 4,904 extrusion segments.
- Cylinder, Benchy and the original Tyranid archive exported successfully in `material_v1`. Successful export is not evidence of continuous bridging. The archive diagnostic no longer requires ramps when the experimental plan explicitly reports separate/fallback cells; the controlled integration fixture still requires real ramps.
- The final Tyranid export has **zero joined cycles and zero nonplanar extrusion moves**. Nearly all 14,992.7 mm³ of its mixed domain remains nominal fallback. Its sampled geometry contains about 181,700 connected domains; the dominant failure is a domain/run too short to contain a complete recipe, followed by moving internal boundaries. The nominal 0.08 mm sampling and printer minimum constrain these cases. Repeated archive runs show small upstream segmentation variations in tiny fragment counts; graph identity is deterministic for a given sampled input, not a promise of bitwise reproducibility for the full pipeline.
- Benchy retains about 2.54 mm³ at the minimum cadence out of 4,216.75 mm³ of mixed volume, with zero joined cycles. The cylinder now retains 3,157.3 mm³ at minimum cadence out of 10,675.2 mm³ (29.6%), with 7,517.9 mm³ nominal fallback and zero joined cycles. These archives do **not** meet the replacement's quality/continuity rollout gates.
- Tyranid wall audit: 0 reported overlaps across 449,289 checked outer-wall segments (Z 0.2–60 mm). Travel audit: 0 crossings in 107,293 checked travels against 786,080 extrusion segments. The updated cylinder also reports 0 overlaps across 116,658 checked outer-wall segments and 0 crossings in 15,777 travels. These tests do not certify complete shell coverage, nozzle-body clearance or print quality.
- The generated Tyranid G-code was inspected in the existing GUI viewer, from the front and in a closer shell view with seam markers hidden. Fragmented color boundaries remain visible. The new application DLL and launcher link successfully; the GUI inspection used the existing v10 viewer to load the newly generated G-code.

Still required before calling the full refactor complete:

1. Shared moving-boundary material geometry, including local source-mesh/paint resampling and bounded consolidation of ephemeral paint fragments. Independent midpoint fits on either side cannot safely substitute for this shared volume.
2. Ordered interface topology for unequal periods, including the planned small-integer-ratio cases. The present adapter only joins compatible equal-period/phase stacks; it never raises another domain's period to force a match.
3. Spatially local repair and termination of failing portions of a planning run, with complete remainders and bounded local recipe error. Current contour repair preserves valid cycles only outside the nearest safe nominal/cadence boundary pair; incommensurate cadences can still fall back across an entire run. Re-solving retained segments and jointly accounting for transition-volume error remain required.
4. Exterior coverage/contact measurements and matching close-up visual acceptance on the real archives, followed by physical validation. The current overlap/travel checks and nominal fallback exports are insufficient.

Build: `build-dev-release/localz-material-planner/snapmaker-orca.exe`. Logs, fixtures, audits and build provenance: `sandboxes/local-z-mix-bridges/material-planner/`. Do not promote `material_v1` to the default based on the current results.

The latest Tyranid artifact is `tyranid-bounded-repair.gcode` with the corresponding `tyranid-bounded-repair-export.log`; the earlier `tyranid.gcode` was held open by the preview, so an attempted overwrite failed and the export was repeated to this separate path. `before-bounded-repair/` preserves the earlier measurements.

Claude reviewed the solver, graph and adapter in session `f279e363-023c-45fa-9134-f1fdebe80a51`, through the claude-conversation skill. Verified findings led to bounded endpoint closure, vertical contacts, swept rather than midpoint ownership, explicit moving-boundary handling and stricter interface eligibility. The suggestion to emit an out-of-limit nominal bead when no valid termination exists was rejected; that case remains an explicit error.
