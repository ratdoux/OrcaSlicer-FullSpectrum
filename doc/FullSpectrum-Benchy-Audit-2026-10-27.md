# Painted Benchy and U1 compatibility audit

Updated 2026-10-28. The unchanged original Benchy now passes complete new-kernel
planning and export in test build 118 and application build 119. Independent
geometry, U1 command and full segment/dose audits pass. The listed compatibility
fixes are verified; performance remains experimental and physical qualification
is deferred.

Source: `C:/Users/radug/Downloads/3DBenchy.3mf`, SHA256
`92e44b529c7857c3909f62c1b8aee4a1cfd85bf772ff2808eefceb38c38ee62b`.
The source project is unchanged. Experiments and reports are under
`build-material-band/evidence/`.

## Compatibility changes

| Original blocker | Implemented behavior |
| --- | --- |
| 5 mm automatic brim and 15% gyroid | U1 layer process generates normal sparse infill, separate solid skins, Classic walls and the configured brim. |
| Paint stream counted as triangles | Admission counts decoded paint leaves and preserves original triangle identities. The 1,429,240 paint bits are not a face count. |
| 11 zero-area triangles | Exact zero-area faces are skipped while preserving paint indexing. Nonfinite vertices, malformed paint and empty geometry remain errors. |
| 225,154 faces exceed the exact clipper's 20,000-face bound | The U1 process uses owned mesh sections, with a 400,000-input-face admission bound. The earlier exact clipper keeps its independent limits. |
| 0.08 mm nominal cycles cannot hold the requested active bands | Adjacent nominal cycles are combined to satisfy the configured 0.06 mm minimum active thickness. The minimum is not reduced. |
| Separate 0.2 mm first layer | The first-layer height, speeds and process settings are retained. Benchy's initial grid has 299 logical cycles, starting at 0.2 mm, then approximately 0.160403 mm per cycle. Support-driven refinement may combine later cycles. |

The new process calls the shared infill geometry utilities. It does not connect
the new kernel to the legacy gradient/Local-Z execution machinery.

## Completed original-project qualification

Both the test harness and the actual application's CLI accepted the unchanged
original project with `local_z_engine=material_bands`, without legacy fallback.
The first layer remains 0.2 mm, the active minimum remains 0.06 mm, and the
configured 5 mm automatic brim, two Classic walls and 15% gyroid remain enabled.
Support refinement converges through 299, 240, 234 and 232 logical cycles.

- The independent source-section audit checks all 232 cycles and finds zero
  perimeter or infill centerline length outside the 0.06 mm model tolerance.
  It records 159 hole sections, 342,388 object paths, 44,649 sparse-fill segments
  and 24,988.66 mm of gyroid paths. Five rendered sections were visually reviewed.
- Complete object/brim support, access and travel validation passes for
  2,726,325 cells. Combined object/brim/tower validation passes for 3,727,125
  cells and 374,166 paths. It finishes at 1,543,942,747 index visits, under the
  unchanged two-billion work limit and 3 GiB temporary-memory limit. All 64
  full-model support checkpoints match between the harness and application.
- Both exports pass 470 U1 docking returns, with the lowest docking Z at 50 mm
  above the 48 mm print. Transition purging uses the configured prime tower;
  neither export substitutes transition-bin purging.
- Both exports independently match all 500,027 captured object segments,
  including endpoints, physical material and individual dose. Maximum XYZ
  component error is 0.0005 mm and maximum E error is below 0.000005 mm filament.
  Object, brim and tower totals remain within accumulated E rounding bounds.
  No object segment collapses to zero length or zero extrusion.
- The native parser retains all 232 logical preview cycles and reports no
  invalid moves. Native GUI automation was unavailable because its desktop
  pipe could not be connected; the reviewed images are actual canonical section
  renders, not GUI screenshots.

The comparison allows the validated scheduler to reorder construction paths.
It enforces a one-to-one mapping of loader-specific domain IDs while preserving
every geometric/material match. Its checks accept reordered and relabelled
paths in both E modes and reject twelve endpoint, dose, missing-segment,
material, domain-alias and preview-metadata mutations. The canonical report's
two legacy FNV fields are not populated for kernel paths and are not used as
equality evidence. Complete segment matching and the byte-identical 724 MB
object capture establish the geometry comparison instead.

Durable results and the reviewed image are under
`tests/data/material_band/evidence/benchy-layer118/`. Current binary hashes and
the full accepted report are in `benchy-verification.json`. The previous
build-114 rejection is retained separately in `benchy-build114-verification.json`.
All 112 kernel cases / 4,690,361 assertions and the existing painted, layered
and application-fixture export audits pass with this kernel implementation.

## Performance and test artifacts

This is a compatibility success, with substantial performance limits. The
application export is 360,137,854 bytes and contains 89,372 retractions. Its
commanded feed rates imply a **19.72-hour motion-only lower bound**, excluding
acceleration, docking macros, heating and startup/end operations. The legacy
file carries a 10h 23m 13s slicer estimate; those are different metrics, so this
is not a controlled timing comparison. The kernel currently caps deposition
at 30 mm/s and retains conservative travel. This result is not a production
readiness or print-speed claim.

The two traced runs ran concurrently. Sampled CPU time was approximately
55.8 minutes for the application and 59.8 minutes for the harness, with peak
working sets of 2.65 GB and 2.43 GB respectively. Wall time also includes an
unexplained approximately twenty-minute observation gap; see `run-context.json`.
Do not quote these runs as controlled performance benchmarks.

The application G-code is
`build-material-band/evidence/layer119-application-benchy/plate_1.gcode`.
`build-material-band/evidence/benchy-ready/3DBenchy-material-bands.3mf` is a
convenience copy with only the engine selector changed; all other archive
payloads match the original. It was qualified through the equivalent original
project plus CLI engine override, rather than by a separate reslice of the copy.

## Findings from full-model testing

The entries below record the development sequence, including earlier failures
and then-pending runs. The completed qualification above supersedes those
intermediate statuses.

Independent coverage checks exposed an empty solid-fill result from excessive
raw-generator overlap. Explicit input expansion and normal generator overlap
fix this. Required solid coverage is checked independently, so a missing skin
cannot be described as intentional sparse air. Concentric skin pockets receive
bounded gap fill inside the required solid area.

Paint-edge clipping preserves wall and infill geometry, including lanes partly
covered by a wall. Planar cap normalization removes duplicated microscopic
triangles and recalculates the actual extrusion dose. The reduced regression
fixture is `tests/data/material_band/benchy-cap-normalization.json`.

Planar support queries are cached per run, while every deposition cell still
receives support and overlap checks. Bridges may use actual lower-face contact
or earlier independently supported, full-height planar deposition as lateral
anchors. Future paths and floating anchors do not qualify. The complete bridge
span and finite end-cap contact remain bounded.

Build 60 generated 443,680 object paths and 3,020,459 cells. Its complete object
geometry was retained for independent scheduling replay. Subsequent scheduling
fixes passed the first 26 cycles and reached a painted bridge at cycle 35. This
is still a rejected full-model result, not a qualified export.

Cap fitting now trims only small protrusions, within one quarter of the bead
width and at most 1% of that segment's volume. It recalculates physical dose,
charges the removed volume to coverage, and repeats the local material coverage
and fraction checks. Deeper spikes, excessive missing volume and other
unsupported geometry remain subject to rejection.

Actual side-face contact within a finite bridge end cap now qualifies as an
anchor. This corrects the XY-area test for disjoint, adjacent owned beads.
Future, floating, partial-height, separated and insufficient contacts are still
rejected. Short runs require their entire centerline to be anchored when the
run is shorter than one bead width. Layer-process cap repair also requires an
actual shared edge, preventing a detached triangle from borrowing another
segment's support.

The cycle-35 failure is a different problem: a painted island cuts a bridge
into pieces at different first-material heights. Simple height splitting and
reordering were insufficient and are not counted as fixes. The current
implementation builds a continuous common-first-material bridge pass where all
active regions share that material. It allocates this pass back to each original
region for local recipe checks. Remaining passes preserve the intended volume
and retain the configured minimum and maximum thickness. When necessary, nearby
logical cycles are merged and their source sections rebuilt; the first layer
stays separate. The original painted sequence is checked first using actual
preceding top faces and the normal scheduler. A foundation or merge is requested
only after that sequence fails support validation. Full object and tower
validation remains mandatory. Unsupported combinations still fail atomically.

The new two-, three- and four-material bridge fixture checks local material
amounts, every pass's thickness, total volume and scheduling. A too-thin
remainder is rejected, and an interface consumed by the common pass keeps its
shared patch identity. A separate manifold side-recess model with two painted
recipes passes complete production planning and checks all physical pass
heights. Its reviewed geometry image is under
`build-material-band/evidence/layer71-painted-recess/painted-bridge-geometry.png`.

Build 71 passes 657 assertions in eight focused scheduling cases. Build 72
passes 74,047 assertions in 19 layer-process cases. Full unchanged Benchy
qualification is still running. Build 70's aggressive 262-cycle candidate
failed support on a curved wall after an unnecessary early merge; it is not
accepted evidence. Build 72 leaves that early sequence intact and requests
changes only where support qualification fails.

Subsequent full-model runs found painted perimeter fragments with the same
support problem. The common first-material pass now applies to those fragments
as well as solid bridge infill. A hollow side-recess regression with no solid
skins or sparse infill passes production planning and preserves its empty
interior. Its geometry image is in `layer75-painted-recess` under the evidence
directory.

The cycle-109 capture exposed a duplicated cap triangle within and between
neighboring wall paths. General triangulation now normalizes all contours
together, rather than using a large domain's aggregate rounding budget to
decide whether per-island triangulation needs correction. The unchanged capture
fails on build 76 and passes on build 77; `benchy-cap-109.json` retains this
regression input.

The cycle-47 capture exposed a dependency between gyroid endpoints and solid
gap repair. Construction preflight can now reorder paths within one coplanar
material/domain pass. Each anchor must first pass its own complete support
checks; a group without an independently supported starting path is rejected.
The accepted order is retained in the generated plan, and final object/tower
validation still uses a fixed explicit sequence. The captured 5,423-path cycle
passes this check on build 78. This is local-cycle evidence, not acceptance of
the complete Benchy.

Build 76 passes 98 kernel cases (4,679,359 assertions), and its 55 test exports
pass the U1 audit with 1,467 dock returns. Build 78 passes 20 focused layer-process
cases (75,441 assertions). Builds 80–81 trim source motion only at the external
ends of open infill runs, preserving internal gyroid joins and closed seams.
Build 81 passes 101 kernel cases (4,678,385 assertions). Its full original Benchy
run still rejected a curved outer hull in the refined 235-cycle candidate.

Build 85 qualifies an adjacent inner wall as an anchor only when its complete
centerline is on actual floor and its footprint stays within the half-bead floor
allowance. A side-supported wall cannot relay support to another wall. The
closed outer wall must share a real side with that anchor and remain within one
wall width. Exact support offsets preserve the small corners that the general
offset helper decimates. The unchanged captured hull cycle now passes, along
with 689 assertions in ten focused scheduling cases. Build 86 adds the permanent
`benchy-wall-46.json` regression and verifies both construction reordering and
the resulting fixed order (four assertions). Full original Benchy qualification
on this production code is still running; these local results do not establish
complete acceptance.

The complete build-85 material-band suite passes 102 cases / 4,685,267 assertions.
Its 55 painted exports pass 1,467 docking returns and eight negative mutations;
the six combined gyroid/brim/tower exports pass another 171 docking returns.
Their retained audits are under `tests/data/material_band/evidence/u1-layer85/`.

The complete build-85 Benchy run passed the former hull failure and then rejected
a sparse segment in cycle 52. Its entire footprint was on actual preceding
material; the expanded support offset had simplified away a narrow cap. Build
87 retains actual contact alongside the expanded allowance. The complete
3,670-path captured cycle now passes, and `benchy-gyroid-floor-52.json` is a
smaller regression that fails on build 85. Build 88 passes all 104 kernel cases /
4,685,274 assertions and all 12 focused scheduling cases / 696 assertions.

A wider recessed-roof test exposed a separate direction-selection issue: the
first direction had anchored endpoints but an overlong diagonal span. Build 90
checks spans while choosing a direction, allowing the shorter axis-aligned
bridge. The same production test rejects a roof too wide in every direction;
the 20 mm span limit is unchanged. Its five scenarios pass 4,482 assertions.
Full original Benchy and application qualification remain in progress.

Build 90 passes the complete 104-case suite / 4,685,283 assertions. Its 55 painted
exports pass 1,467 docking checks and eight negative mutations; six layered
exports pass another 171 docking checks. The actual application's reported
four-tool and gyroid exports pass 403 and 16 docking checks respectively and are
byte-identical to the corresponding reviewed build-85 exports. Current fixture
audits, binary hashes and the reviewed gyroid/material-dose plot are retained in
`tests/data/material_band/evidence/u1-layer90/` and
`painted-regions-verification.json`. These results still do not qualify Benchy.

The complete build-88 and build-90 Benchy runs passed the former cycle-52 failure
and rejected a later open hull seam in cycle 57. Requiring the entire adjacent
inner wall to be grounded discarded its useful grounded sections whenever a
distant section crossed a recess. Builds 91–93 classify each bounded motion
segment separately: at least half its footprint must touch actual preceding
material, and the complete footprint must remain within the half-bead floor
allowance. Only these sections of an already validated inner wall may support
the outer wall. The reach includes the square-cap diagonal; it does not extend
the bridge-span limit. A side-supported or bridging section cannot relay support.

Build 93 passes the unchanged 3,868-path cycle-57 capture. Build 94 adds
`benchy-wall-57.json` to ordinary regressions, checking both construction
reordering and the resulting fixed order. All 12 focused scheduling cases pass
706 assertions, including narrow floor seams and rejection of larger gaps and
floating wall chains. Full original Benchy qualification is still pending.

Build 94 passes all 104 kernel cases / 4,685,293 assertions. Its 55 painted
exports pass 1,467 docking returns and eight negative mutations; six layered
exports pass another 171 returns. Both actual application fixture exports are
byte-identical to build 90 and pass their 403 and 16 docking returns. These
audits and the reviewed gyroid plot are retained under `u1-layer94/`; the
painted-region verification manifest records the build-94 binary hashes.

The complete build-94 Benchy run refined 299 cycles to 241 and passed the former
cycle-57 seam, then rejected a later hull cap in cycle 61. The integer union had
split the wall into tiny pieces at point contacts; the per-component contact
test treated these pieces as isolated deposits. Build 99 groups numerically
touching wall pieces with the existing two-coordinate-quantum allowance for
the contact test, clips back to the original footprint, and retains the same
grounded-anchor and reach requirements. It passes the unchanged 3,777-path
capture and the 12 focused scheduling cases. Build 100 adds
`benchy-wall-61.json` to the fixed-order regression checks. Complete Benchy
qualification is being repeated; build 94's recorded outcome remains fallback.

Build 100 passes all 104 ordinary kernel cases / 4,685,297 assertions, including
both flexible and fixed ordering for the cycle-61 fixture. Test build 101 adds a
separate three-assertion regression: point-touching wall coverage attaches, while
a cap separated by 0.001 mm rejects. Production code is unchanged from build
100. The 55 painted and six layered exports again pass 1,467 and 171 docking
returns plus eight negative mutations. Both actual application fixture exports
remain byte-identical to the reviewed outputs. Detailed audits are retained in
`tests/data/material_band/evidence/u1-layer100/`.

The complete build-100 Benchy run passed the captured wall failures and reached
cycle 157 of the 241-cycle candidate, then rejected
`painted_fitted_cap_coverage_budget` after 1,130 seconds of path planning. The
application CLI reproduced that fallback. Optional per-segment cap fitting can
consume too much of a small painted region's first-material allocation when a
shared foundation spans a larger region. Build 103 checks the complete local
volume and recipe budgets before selecting a fitted candidate; otherwise it
retains the original geometry for subsequent support validation. It also keeps
actual preceding material in the fitter's support region, alongside the offset
allowance. A seven-assertion regression preserves both local material volumes
and rejects a floating copy. Full Benchy qualification is being repeated.

Build 103 passes all 106 kernel cases / 4,685,307 assertions. Its 55 painted
exports pass 1,467 docking returns and eight negative mutations; six layered
exports pass another 171 returns. Both application fixture exports remain
byte-identical to the reviewed outputs and pass 403 and 16 returns. Detailed
audits and the reviewed plot are under `u1-layer103/`, with current hashes in the
painted-region manifest. These results do not yet qualify the original Benchy.

The complete build-103 run reached cycle 157 again. More precise diagnostics
identified domain 1537 / material 1: the unfitted foundation ledger reported
14.645429 mm³ against a 14.640417 mm³ target. That target intersected the painted
region with source-path coverage, while the remaining phases targeted the full
intended region, including bounded gap repair. This inconsistency understated
the target; optional fitting alone could not resolve it.

Build 104 makes the foundation use the same intended painted footprint as every
remaining pass. `benchy-foundation-157.json` retains all seven bands, the source
geometry and the preceding support. Build 105 tests its complete partition,
volume/thickness bounds and support scheduling. The two focused cases pass
4,996 assertions, including a negative case that rejects a locally underfilled
foundation instead of hiding the gap by lowering the target. Full Benchy and
application qualification are being repeated on this production code.

Build 105 passes all 106 kernel cases / 4,690,261 assertions, including the
complete cycle-157 partition and support regression. The 55 painted exports
pass 1,467 docking returns and eight negative mutations; six layered exports
pass another 171 returns. Both application fixture exports remain byte-identical
to the reviewed outputs and pass 403 and 16 returns. Current fixture evidence
is retained under `tests/data/material_band/evidence/u1-layer105/`. The complete
original Benchy run remains pending.

The complete build-105 run passed cycle 157 and reached cycle 182, then
rejected `unsupported_bead_footprint` at the upper cabin after 1,181 seconds of
path planning. The application reproduced the same fallback. Its inner wall
starts and ends over an unsupported corner, preventing bridge anchoring despite
the supported remainder of the loop. The original paths and floor are retained
in `benchy-seam-182.json`; supported seam placement is being tested before
repeating complete qualification.

Build 110 adds supported seam placement before wall bead construction. It also
measures wall bridge spans over the actual unsupported bead intervals rather
than counting already certified lateral support as air. Closed loops require
actual start/end cap anchors. All 15 focused scheduling cases / 742 assertions
pass, including captured cabin coverage and final explicit ordering, and the
existing negative floor-gap, detached-wall and chained-support cases. Complete
Benchy, fixture and application qualification are running again on this build.

The build-110 full suite passed 107 of 108 cases; its frozen cycle-157
source still used an unsupported closed-wall start. Build 111 rechecks complete
closed paths against final preceding deposition after partitioning and cap
fitting, rotating vertices with their matching segments without changing bead
cells or dose. All 16 focused cases / 5,726 assertions pass, including the old
foundation capture and the new cabin capture. The superseded build-110 full
Benchy runs were cancelled, and complete qualification restarted on build 111.

Build 111 passes all 108 kernel cases / 4,690,290 assertions. The 55 painted
exports pass 1,467 docking returns and eight negative mutations; six layered
exports pass 171 returns. Both application fixture exports pass 403 and 16
returns and remain byte-identical to the reviewed outputs. Durable fixture
evidence is under `u1-layer111/`. The original full Benchy run remains pending.

The full build-111 run refined 299 cycles to 239 and rejected cycle 28 after
681 seconds of path planning. The application reproduced the same failure.
The closed loop has a 62% finite-cap contact point, but the placement heuristic
required 75% and left its unsupported original start unchanged. The scheduler
already accepts 50% actual contact. Build 112 selects the strongest valid point
when the preferred 75% contact is unavailable, retaining the same anchoring
contract. `benchy-seam-28.json` preserves the captured loop and preceding floor.

Build 112 passes all 108 kernel cases / 4,690,304 assertions, including both
seam captures and all earlier foundation/support regressions. All 55 painted
and six layered exports pass 1,467 and 171 docking returns, with eight negative
mutations rejected. Both application fixture outputs remain byte-identical to
the reviewed versions and pass 403 and 16 returns. Durable fixture evidence is
under `u1-layer112/`. Replaying all 173 paths of the captured cycle 28 with only
the offending wall start rotated to the measured anchor passes scheduling.
The complete original Benchy and application runs are still pending.

The full build-112 run passed the low-hull loop but rejected refined cycle 39
after 718 seconds of path planning; the application reproduced it. Both failing
walls have vertices with over 80% actual cap contact. Seam selection also
required their preceding vertices, almost 2 mm away, to be grounded. Build 113
limits that extra check to closing edges no longer than one bead width, where
first-cap ownership can remove the closing motion. All 16 focused cases / 5,756
assertions pass. Replaying the complete captured 218-path cycle with only those
two seams rotated passes scheduling. `benchy-seam-39.json` retains both walls
and their preceding floor. Complete build-113 qualification is running.

Build 113 passes all 108 kernel cases / 4,690,320 assertions. All 55 painted
and six layered exports pass 1,467 and 171 docking returns, with eight negative
mutations rejected. Both application fixture outputs remain byte-identical to
the reviewed versions and pass 403 and 16 returns. Durable fixture evidence is
under `u1-layer113/`. Complete original Benchy qualification remains pending.

The complete build-113 run converged from 299 to 240, 234 and finally 232
cycles. All cycle preflights and both material-volume budgets passed, producing
342,388 object paths and 2,712,994 cells before the brim. Whole-object validation
then hit `schedule_edge_limit` at cycle 149: contacts had accumulated four
million dependency links despite the established phase chain already ordering
those paths. Path planning took 2,644 seconds and final validation another 253
seconds in this traced run alongside the application check. This remains a
rejected export, not qualified new-kernel G-code.

Build 114 omits support/access links that are already implied by an established
per-object phase chain or structurally verified explicit sequence. Every contact
still undergoes geometry validation; backward links remain, and flexible
construction ordering cannot use this shortcut. All 29 scheduling cases / 1,485
assertions pass, including a two-edge budget for three supported paths, missing
floor rejection and reversed-order rejection. Complete qualification is running
again, retaining the completed path plan for diagnostic replay.

Build 114 passes all 109 kernel cases / 4,690,332 assertions. The 55 painted
exports pass 1,467 docking returns and eight negative mutations; six layered
exports pass 171 returns. Both application fixture exports pass 403 and 16
returns. All 61 fixture G-code files and both application outputs are
byte-identical to build 113. Durable fixture evidence is under `u1-layer114/`.
The unchanged original Benchy still requires complete qualification.

The full build-114 run passes the complete object scheduler, resolving the edge
limit. The combined object and tower contain 3,727,125 cells; validation then
rejects at cycle 187 with `schedule_support_cache_limit`. The original application
also exports legacy fallback. Path planning took 2,710 seconds and final
validation 682 seconds in the traced concurrent run. The final 232-cycle object
capture is retained in `benchy-layer-process-114/01-3DBenchy/cycle-paths.json`.
It precedes brim insertion and is diagnostic geometry, not a qualified export.

Build 115 releases cached grounded-wall polygons once no unvalidated path can
use their height for coplanar anchoring. A conservative tolerance window retains
anchors across deferred paths and nonmonotonic object/tower returns. Actual bead
cells and ordinary floor footprints remain available. Temporary support-work
accounting is released with its vectors; mutable graph adjacency is released
after the validated order is frozen, before travel construction.

The new 512-layer wall-stack regression fails on the previous scheduler under a
1,280 KiB budget and passes with the cache fix. All 30 focused scheduling cases /
1,486 assertions pass. A diagnostic object/tower replay uses the retained object
paths and original geometric tower settings; it omits the brim and does not
replace complete production qualification. Full build-115 test and application
runs are pending.

Build 115 passes all 110 kernel cases / 4,690,333 assertions. The 55 painted
exports pass 1,467 docking returns and eight negative mutations; six layered
exports pass 171 returns. Both application fixture exports pass 403 and 16
returns. All 61 fixture files and both application outputs remain byte-identical
to build 114. Durable fixture evidence is under `u1-layer115/`. Full unchanged
Benchy qualification and the captured object/tower replay remain pending.

The build-115 captured replay completes combined support validation without the
previous cache failure, then reaches `schedule_index_work_limit`. The two full
build-115 qualification runs were deliberately stopped after this diagnostic
failure; neither is an accepted original-project result.

Build 116 marks spatial-index subtrees whose cells have one owner. Planar contact
queries can skip an already admitted or ineligible owner without revisiting its
triangles. Candidate counts remain unchanged in the captured replay, but early
search-work savings are only a few percent.
Its complete captured replay also reaches `schedule_index_work_limit`.

Build 117 balances the index by centroid spread, with Z weighted for thin layers.
Stored bounding boxes, contact predicates and collision calculations remain in
original millimeters. This avoids repeatedly splitting an axis where long bead
boxes have identical centers. Temporary index-build sources are released after
construction. The new tessellated-floor regression uses 226,376 visits instead
of 277,712, fits a 250,000-visit budget, and still rejects a floor gap and
overlapping deposited cells. All 31 focused scheduling cases / 1,491 assertions
pass. Complete regression and captured Benchy replay are pending.

Five sections of the retained 232-cycle object capture show curved gyroid,
separate solid fill, walls and retained openings. These are planned paths before
brim insertion and finalization. Of 342,388 paths, 195,006 are shorter than
0.1 mm; none of the adjacent same-band/same-role paths have exactly shared
endpoints. Joining them would change geometry. Final travel and retraction cost
still requires assessment in a qualified original-project export.

Build 117 passes all 111 kernel cases / 4,690,338 assertions. The 55 painted
exports pass 1,467 docking returns and eight negative mutations; six layered
exports pass 171 returns. Both application fixture exports pass 403 and 16
returns. All 61 fixture files and both application outputs are byte-identical
to build 115. Durable fixture evidence is under `u1-layer117/`. The captured
Benchy's candidate counts remain unchanged while index work is substantially
lower; complete original qualification is still pending.

The build-117 captured replay reaches 3.7 million combined support cells with
about 900 million index visits, but later still reaches `schedule_index_work_limit`.
The original-project rerun was not started. Build 118 adds separate support,
access and travel checkpoints to identify any remaining limit precisely.

Build 118 adds the earliest explicit operation rank to each index subtree.
Motion queries prune a subtree only when every cell belongs to future deposition
in the structurally verified total sequence. Static geometry has rank zero and
is never pruned; the current deposition remains eligible for self-access checks.
Support and overlap queries retain future cells. Final departure resets the
rank bound so the last deposited path remains visible. The new regression accepts
a detailed neighboring obstacle when printed later and rejects the same obstacle
when printed earlier or supplied as static occupied geometry. All 32 scheduling
cases / 1,514 assertions pass. Full regression and captured replay are running;
the production work limit remains two billion index visits.

Build 118 passes all 112 kernel cases / 4,690,361 assertions. Its 55 painted
exports pass 1,467 docking returns and eight negative mutations; six layered
exports pass 171 returns. Both application fixture exports pass 403 and 16
returns. All 61 fixture files and both application outputs are byte-identical
to build 117. Durable fixture evidence is under `u1-layer118/`. The captured
original Benchy replay and complete original-project qualification remain pending.

Application build 119 corrects the outdated dense/two-material engine tooltip;
its kernel implementation is unchanged from the 112-case-tested build 118.
After the build completed, both application fixture exports passed 403 and 16
docking returns and remained byte-identical to build 118. An earlier smoke launch
attempted before linking completed is recorded separately and is not qualification
evidence. The test binary remains build 118. Application audit copies are under
`u1-layer119/`.

The build-118 captured finalization passes: 3,713,794 combined cells, 374,154
paths, 375,091 dependencies and 2,127,356 final operations. Index visits finish
at 901,914,679 after support, 1,323,517,802 after access and 1,537,041,981 after
travel, below the unchanged two-billion limit. All 64 common support checkpoints
match build 115's candidate counts. This replay omits the original brim and is
diagnostic evidence. At 08:59 on October 28, full unchanged original-project runs
started with test binary 118 and application 119. Their canonical geometry and
exported G-code qualification remain pending.

## Existing U1 evidence

Build 71 passed 98 material-band cases / 4,678,001 assertions. Its 55 exported
painted fixtures passed 1,467 docking-return checks and eight negative docking
mutations. The actual application's export of the reported `U1-4-gradient`
project used all four physical tools and passed another 403 docking returns.
The build-85 actual application also exports the reported four-tool project
successfully, with all 403 dock returns passing. A second application export of
a four-material gradient with 15% gyroid, 5 mm brim and a configured prime tower
passes all 16 dock returns. Its exported paths and reconstructed material dose
are plotted in
`build-material-band/evidence/layer85-application-u1-gyroid/gyroid-material-sections.png`.
These application results do not establish acceptance of the pending Benchy.

U1 transitions use the configured `prime_volume` on a planned tower, including
separate XY strips for repeated changes at one height. Tower support, occupancy,
clearance and dose are checked with the object plan. Automatic transition-bin
substitution is disabled.

The earlier broad repository run passed 381 of 388 cases; seven recorded
baseline failures remained. The whole repository suite is not claimed green.
Physical printer qualification remains deferred.

## Legacy baseline

The original legacy slice had 599 layers, 55,800 perimeter/infill polylines and
420 hole sections. The audit found no slicing-error layers or centerlines outside
the solid within 0.06 mm tolerance. Its 763,724 parsed moves included 588,059
extrusion moves, with no invalid numeric or extrusion-metadata values. The
fallback matched a deliberate legacy slice exactly. This is fallback evidence.

The previous baseline report and section image are retained under
`tests/data/material_band/evidence/benchy/` and in `benchy-verification.json`.

## Reproduction

Build the tests with `cmake --build build-material-band --config Release --target
libslic3r_tests --parallel 4`. Run `[material_band]~[.]` for ordinary kernel
regressions. Hidden diagnostic/export cases require their documented environment
variables.

Run `scripts/material_band_real_models.py` with the test binary, Downloads path,
`--model 3DBenchy.3mf --export --require-kernel --timeout 7200`. The explicit
`--require-kernel` flag makes legacy fallback a failed qualification.

For a full geometry capture, set `MATERIAL_BAND_CAPTURE_CYCLE=last` and
`MATERIAL_BAND_CAPTURE_FULL=1` before running the audit. After a successful export,
run `scripts/material_band_export_reconcile.py` with `--capture` pointing to
`cycle-paths.json`, `--report` to the completed `report.json`, `--gcode` to the
export and `--output` to a new JSON evidence file. It independently compares every
object segment's endpoints, material and extrusion with the canonical capture,
then checks object, brim and tower volume against accumulated E rounding bounds.
Construction order and loader-specific domain numbers may differ; the comparison
still requires every segment exactly once and a bijection of domain identities.
The captured object excludes brim and tower, which are checked by their separate
totals and the U1 docking audit. XYZ output has a 0.001 mm quantum and E has a
0.00001 mm filament quantum. This check supplements geometry and scheduling
validation; it does not establish physical bead shape.

The GUI/CLI loader is `build/src/Release/snapmaker-orca.exe`; its adjacent
`Snapmaker_Orca.dll` contains the implementation. Existing running slicer
instances retain their loaded DLL and must be restarted to test a rebuilt copy.
