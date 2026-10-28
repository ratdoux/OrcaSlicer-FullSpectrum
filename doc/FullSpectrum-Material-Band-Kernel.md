# FullSpectrum material bands

The opt-in `material_bands` engine implements shared material interfaces,
actual-solid clipping, XYZ deposition, support/access scheduling, purge planning,
production export and logical-cycle preview. `legacy` remains the default.
Physical qualification (M8) is deferred. Numerical test envelopes and profiles
are not recommended printer settings.

Branch `material-band-kernel` starts at
`63117732a0683220530cf690f385055d1f3dd54d`. No vendored dependency was changed.

## Controls and production behavior

Advanced process settings under **Multimaterial / Mixed Filaments** and
**Others / Mixed Filaments** contain **Local Z engine → Material bands
(experimental)**, the material-boundary policy, transition width and local
dose allowance, and gradient band dose allowance. RepRapFirmware profiles require
a complete nozzle/head envelope and a declaration that tool selection performs
no XYZ or E motion. Snapmaker U1 profiles automatically use the moving-tool
adapter described below; leave the stationary declaration off. An empty U1
envelope selects the conservative stock clearance cylinder. Explicit envelopes
are comma-separated bottom height, top height and radius triples in millimeters.
All seven settings persist through production 3MF save/load. Gradient band dose
allowance defaults to zero (exact fractions). A nonzero value explicitly permits
local fraction adjustment to meet minimum band thickness; it does not relax the
total per-material volume budget.

An accepted print has one immutable `FinalPrintPlan`. Object deposition, prime
and purge structure, physical tool transitions, retractions, travel, homing,
heating, cooling and shutdown operations are its authority. The writer cannot
invent a second tool sequence. Unsupported input publishes a precise reason and
falls back for the whole print before new G-code is emitted. Existing legacy
validation still applies to fallback. For example, material-band export supports
absolute and relative E, while fallback with the legacy wipe tower requires
relative E and reports an error if it is unavailable.

The preview retains Orca's layout and physical-filament coloring. Its vertical
slider indexes logical macro cycles rather than changing nozzle height or XYZ
segment count. Bead width and deposited height come from metadata. Optional
`MB_CYCLE`, `MB_BEAD` and `MB_OPERATION` comments preserve that information when
G-code is loaded from disk. Partial or absent metadata uses the existing external
file interpretation. Logical rows can contain disjoint operation ranges.

## Support matrix

Every accepted geometry also passes combined support and complete swept-head
checks. Geometry-service acceptance alone is not print acceptance.

| Area | Production scope |
| --- | --- |
| Materials | Two to four physical filaments for whole-solid recipes and U1 painted regions; two for the earlier stationary painted-wall adapter; actual pure domains without epsilon residue |
| Height gradients | Two to four colors, saved order/stops/solid widths, sampled at macrocycle centers; kernel-owned volume solve and paths |
| Process | U1: Classic walls, normal sparse patterns including gyroid, separate solid skins, brim and first-layer process; earlier stationary adapter: dense monotonic/rectilinear/concentric fill and fixed macro height |
| Uniform solids | U1 mesh-section process plus verified boxes, cylinders, annuli and holed plates; earlier exact-clipper route retains its qualification limits |
| Painted regions | U1 leaf-facet side paint, top/bottom paint, adjacent pure materials and mixed recipes on qualified solids; independent base interior. Earlier stationary adapter retains its opposing-box-stripe scope. |
| Transitions | Declared width/local dose allowance, separately checked plateau and global shell dose |
| Tapers | Actual inward-taper clipping/path services; selected pure and mixed tapers pass production scheduling |
| Instances | Translations, tested axis-aligned rotations and mirrors, plate-origin conversion, independent object identities |
| Machine | Snapmaker U1 / Klipper moving-tool adapter; or stationary RepRapFirmware with a declared envelope |
| Purge | Exact scheduled physical-tool runs, dense columns and priming, combined access validation |

Configured wall counts apply to clipped curved/pure domains as well as boxes;
every complete wall must fit. The solid-infill direction alternates by 90 degrees
between macro cycles. Internal, top and bottom patterns are selected separately,
including the minimum shell thickness and layer count. Concentric fill uses
nested contours and bounded-width lanes for its narrow central remainder. The
pure interior of a painted box contains infill rather than an extra hidden wall.
Unsupported patterns, custom direction templates and incompatible region
overrides have explicit fallbacks.

The selected holed plate passes with concentric fill. Its alternating 45-degree
lane variant still fails `unsupported_bead_footprint` at small cap gaps; a
regression test preserves that failure. The earlier stationary adapter retains its limited paint/solid scope. Opposing
material orders, more than four physical filaments, Arachne, supports, variable
layers, sequential printing, image maps and opaque custom code have explicit
fallback paths. Sparse fill is supported through the U1 layer-process route;
adaptive cubic, support cubic and lightning still require volume preprocessing. They are not
silently projected through the interior. Dependency cycles fail explicitly;
automatic path splitting is not claimed.

The three/four-material extension rejects the tested inward taper with
`stack_planned_material_budget`: prismatic bead columns omit stair-step volume
that exceeds the stricter per-material budget. Selected 64-segment cylinders,
annuli with monotonic fill, and holed plates with concentric fill pass. The
32-segment annulus with concentric fill still exceeds its coverage budget; the
32-segment holed plate still has unsupported small bead caps. Regression tests
retain these failures rather than widening volume or support tolerances.

The earlier stationary machine adapter excludes brims. Both adapters exclude
skirts, spiral/arc modes, firmware
retraction, manual tool changes, shrink compensation, bed exclusions, nonzero
Z/tool offsets and post-processing. Paths use nominal line width and conservative
speed/flow/Z bounds. It owns constant nozzle/bed temperatures and fan-off
operation. Physical qualification must establish a suitable actual profile.

## Architecture and numerical contracts

### Gradient intent versus gradient execution

The color sequence is input intent: for example, A → B → C describes the
requested progression over normalized object height. It is not a tool-change
sequence. The input adapter reuses only `mixed_gradient_components`,
`mixed_gradient_stops` and `mixed_gradient_solid_half_widths` to decode the
existing saved recipe controls. It copies that definition into owned
`GradientKnot` values. No legacy gradient sampler, Local-Z path planner,
height correction or G-code rewriting participates in material-band planning.
Legacy Local-Z process settings do not supply defaults to the adapter.

`StackDoseSolver` samples the owned curve at each macrocycle center, with the
actual clipped cycle volume as its weight. It supports up to four physical
materials in a constant blend; a color sequence normally blends its two
neighboring colors at a time. An explicit gradient dose allowance may redistribute
fractions between cycles so each present band meets the minimum printable
thickness. The total requested dose of every physical material, integrated over
those sampled cycles, is preserved. Zero-dose bands remain absent. The bounded
active-set selection can reject a problem even if another selection is feasible.

`StackPlanner` determines the shared interfaces from actual clipped volumes,
generates perimeters and infill within each band, and hands the result to the
material-band scheduler and final print planner. Bands use increasing physical
material ID within each cycle; the color sequence does not override that order.
The height-gradient adapter does not interpret images or arbitrary spatial
paint fields. Those require a separate domain/field adapter.

Sources are under `src/libslic3r/MaterialBand/`. Length is in millimeters and
volume in cubic millimeters. Physical IDs are 1-based and independent of display
colors and virtual recipe IDs. Band identity includes domain, cycle, material
and stack slot. Failures contain a reason, revision and, when available, a band
and witness. Failed stages publish no partially accepted output.

`InputAdapter` copies actual source triangles and painted leaf facets, including
TriangleSelector subdivision. Base object/volume intent remains present even
when every visible facet is painted. Transforms correct mirrored orientation.
No borrowed mutable paint masks survive invalidation.

`SlabSolver` uses deterministic projected gradient descent with Dykstra
projections. It enforces thickness/order, local recipe intervals, triangle
gradient halfspaces and an independently supplied material-volume constraint,
then independently rechecks the result. Conservative component bounds
`abs(gx), abs(gy) <= maximum_gradient / sqrt(2)` imply the actual norm bound.
Hard steps default to `discontinuous_interface`; permitted extra error is
confined to the transition neighborhood. Pure domains use a separate one-band
construction instead of retaining a forbidden epsilon band.

`BoxPlanner` verifies an actual closed outward six-face solid and a covering
interface triangulation. Mitered walls and dense lanes partition its footprint.
The painted adapter keeps the shell and pure interior separate and independently
checks original step-paint shell dose. Paint coordinates merge at the declared
XY quantum, including roundoff from axis-aligned rotations.

`SolidClipper` uses the existing CGAL exact construction kernel. Actual-solid
intersection and difference produce complementary bands with one shared
interface. Bounded scalar dose correction rechecks clipped volume while
preserving nodal, thickness, slope and macro-boundary constraints. Converted
vertices weld on `min(1e-9, interface_tolerance/100)` mm before closed-topology and
volume checks. Immutable AABB trees provide membership and distance queries.

`ClippedPlanner` follows the actual contour and holes with a globally affine
field and convex contour boundaries. It proves vertical column containment for
vertical sides/inward tapers and integrates bead cells. Fill lanes are inset by
the actual virtual tip envelope. Rotated lanes cover the full diagonal extent.
Residual cap triangles can join a nearby fill segment only within its declared
bead width and end cap, with recomputed volume and flow limits. Unmanufacturable
fragments remain missing coverage and can invalidate support in the next band;
excess is bounded separately. Tests use independent analytic volume,
membership and fine raster oracles, not just total E.

`Topology` derives birth, termination, split, merge and material events from
positive printable overlap after bead erosion. It does not match nearest
vertices or merge separate objects. The sphere-pair oracle checks the analytic
thresholds `2.5 ± sqrt(5)` mm. Correspondence does not replace physical support.

`Scheduler` verifies cell dose, nonoverlap, support from the bed/earlier cells,
phase and access dependencies. Complete head sections are circumscribed
16-sided cylinders swept along whole segments, checked by separating axes.
Only tip contact receives the declared surface tolerance. Lift, XY travel,
descent and final lift are validated against deposited/occupied geometry.
Canonical ordering is deterministic; a balanced 3D AABB tree bounds local
queries. The broad phase queries each head section at its own radius and height
before applying the unchanged exact swept-volume checks. Copied objects retain
separate operation identities.

`FinalPrintPlan` finalizes object paths and tool runs, inserts prime/purge and
state operations, then validates the combined explicit order before freezing.
Failed tower insertion rejects the new plan. Export uses the production writer's
separate XY/Z travel functions and authoritative deposition volume. RRF
temperature commands retain the physical `P` parameter, distinguishing them
from firmware retraction. Final export remains serial, consistent with existing
writer/extruder state.

Budgets are 1e-5 mm shared-interface tolerance, 0.001 mm macro closure, 0.01 mm
path approximation, 0.5 percentage-point plateau error, and
`max(1e-4 * target_volume, 1e-4 mm3)` analytic volume error unless the fixture
declares another absolute budget. Simple clipped coverage targets 1% missing
volume with excess separately bounded. Source mesh approximation is reported
separately. Integer XY clipping uses a 1e-6 mm quantum; support/overlap comparisons
allow only a perimeter-scaled eight-quantum rounding bound for two rotated
coordinate frames and clipping. A 0.001 mm unsupported-strip regression is
rejected. Export uses 0.001 mm
XYZ and 0.00001 mm E precision, per-material accumulated E rounding bounds and
feed rounded downward.

## Caching, cancellation and bounded work

Geometry, paint, recipe, process and machine edits invalidate the immutable
production result. Generation checks prevent stale/canceled publication.
Unchanged prints reuse their result; translated copies reuse generated geometry
before owned translation. Changed revisions conservatively rebuild. Edit/revert
is compared with a fresh plan at 1, 2 and 4 TBB threads. The legacy disk format
does not serialize `FinalPrintPlan`; new-engine cache loads request fresh
planning. Feature-off caches follow the existing path.

Limits cover generated Boolean output and deep-copy allocation, not merely
input batches. Defaults include 400,000 input triangles, 200,000 solver vertices,
20,000 clipping input faces, 400,000 generated clipping elements, 100,000 clipping
output faces, 24 correction iterations, 100,000 paths, one million
segments/cells/dependencies, ten million scheduling candidates, 100 million
index visits and two million final operations. Paint stations and face products
have separate admission bounds. Estimated storage limits are 128 MiB for
solver/topology, 256 MiB for clipping/paths and 512 MiB for combined scheduling,
instances/finalization. These are conservative admission estimates, not a
metered allocator for every CGAL exact-number internal allocation.

The U1 section-based production route has separate bounded admission: 500,000
paths, two million segments and 1 GiB retained path geometry. Its finalization
bounds are four million cells/dependencies/commands, 16 million index nodes,
100 million candidate pairs, two billion index visits, a 3 GiB temporary
scheduling estimate and a 2 GiB final-operation estimate. Construction-only XY triangles are released
after each completed cycle; its 3D bead cells remain authoritative. These are
upper admission limits, not measured peak RAM or a promise that every input
within one individual limit is feasible.

## Corpus and evidence

`tests/data/material_band/fixtures.json` enumerates the 16 core configurations
across 12 families, exact outcomes and selected feasible curved variants. MB01,
MB02-transition and MB03 must use the new production engine. MB03's production
integer-percentage recipe steps require explicit transition allowance; its
analytic ramp is tested independently. Complex painted cases assert exact
fallback. A planner that always falls back fails the suite.

Two 3MFs under `tests/data/material_band/painted/` were independently painted and
saved in the native GUI. One stroke crosses source triangles; the second file
adds an intersecting second virtual mix. Production reload tests check split
leaves, recipe identities and fractions. Their unsupported top paint asserts
the domain-adapter fallback. The rebuilt native app loaded and sliced MB01 and
MB02-transition, displayed exactly ten logical cycles, filtered cycle five at
1.20 mm and rendered physical-filament, bead-height and line-width coloring.
The transition height legend spans 0.06–0.24 mm. Native verification caught and
fixed an incompatible numeric-list settings control and an invalid demo prime
volume. The ready-to-open projects are under `tests/data/material_band/production/`;
they use virtual numerical profiles and are not physically qualified.

The 64 seeded stress rows vary requested paint patches, recipes, physical
filaments, topology, height change, resolution, process, transforms and threads.
Requested pair coverage is 680/680; measured connected-region coverage is
659/680. All 21 uncovered valid measured pairs are retained in the report.
Height edits/touching patches change region counts in 13 rows. Three rows must
produce accepted plans; 61 check supported rejection. Each run retains a 3MF,
profile, manifest, metrics and accepted G-code.

Feature-off references under `baseline/` come from the pinned production code.
Normalization changes only CRLF, generated timestamps and known runtime object
IDs in comments. Commands, Z/E and tool order remain intact. The paired runner
alternates old/new order, discards a warmup and keeps at least five measurements.
It publishes median/range, never a p95 from five samples. Final counts, hashes,
performance and baseline failure comparison are in
`tests/data/material_band/evidence/verification.json`; compressed logs and
benchmark/coverage summaries accompany it.

Paired verification exposed previously uninitialized `Print` printer-family and
plate-origin fields. Production now initializes them. Both capture harnesses
explicitly set the family and zero origin: the BBL path reproduces the original
three goldens, and separate generic-printer comparisons exercise the other
branch. These runtime inputs are recorded alongside each effective profile;
no differing machine commands are normalized away.

The earlier feature-off comparison uses 100 measured pairs for each of the three
models in both BBL and generic runtime modes. The primary paired-time statistic
is `median(new_time_i / old_time_i)` for adjacent old/new captures, with alternating
order. Its maximum overhead is 2.13%; the maximum ratio of median peak working
sets is 1.58%. All 600 measured pairs reproduce identical normalized G-code.
The report also preserves the ratio of separate time medians, which reaches
5.12% for the small painted BBL case and therefore misses a strict 5% gate under
that alternative estimator. Earlier 5-, 20- and 100-pair measurements and every
sample remain available. These are point estimates on this shared Windows
workstation with CPU affinity `0x3`, not a statistical confidence bound or a
universal overhead guarantee.

After the infill extension, all six feature-off model/family combinations were
compared again with the pinned baseline: normalized commands remain identical
and preview moves remain 104 bytes. This latest check is functional; the
100-pair performance estimates above refer to the earlier binary.

The expanded regression run passed 435 of 443 cases. All 56 new cases passed;
the same eight baseline failures remain (configuration serialization, a known
differential expectation, placeholder scripting, cached nozzle slots, image-map
modulation and three bed-temperature cases). The combined material-band and
stress runs passed 3,114,945 assertions (55 ordinary cases and one 64-row stress
case). The ordinary filter excludes opt-in artifact writers that require an
output directory.

All 21 enabled benchmark workloads passed the declared median budgets: 0.5 s
snapshot, 8 s paths, 20 s finalization, 1 s export, 256 MiB peak working set and
10 ms cache lookup. With the updated infill implementation, eight cycles take
2.63 s median; eight concentric hole-plate copies take 8.94 s, reuse seven
geometry plans and produce 7.33 times the candidates of one copy. The largest
median peak working set is 58.99 MiB. The edit/revert cache-hit median is 0.009 ms.
These measurements use the named virtual fixtures, not a universal
model-performance promise. The holed-plate workload now explicitly selects
concentric fill, and the mixed taper is an accepted workload; timings are not
an identical-workload speedup comparison with the earlier release.

## Downloaded models and infill audit

The local audit imports the original project configuration, preserves image maps
and paint, and never rewrites the source file. Nine projects were tested: two
Tyranid projects, painted Benchy, TestCylinder, CubeTest, SlicerProblem_Painting,
Cute Dragon, Chibi Panther Chameleon and 1MPolygonBridge. The last file actually
contains 2,179,126 source triangles. Together the projects contain 4,859,185
triangles and produce 4,034 object layers.

All nine currently take the explicit `stationary_reprap_tool_selection_required`
fallback. Their original profiles also include unsupported combinations such as
four physical filaments, image maps, sparse fill and supports. These are legacy
geometry/regression results, not proof that the new kernel accepts those meshes.
The audit checks complete perimeter and infill centerline segments against each
slice, including cavities, with a recorded 0.06 mm tolerance. Every tested layer
has zero slicing errors and zero outside-centerline length. Five cross-sections
per object are retained as SVGs. The final run audits 824,597 paths and exports
and parses all nine projects: 13,610,169 moves with zero invalid coordinates,
extrusion values or extrusion dimensions. This check does not prove bead-volume coverage
or physical adhesion; the new-engine numerical fixtures separately check dose,
coverage, overlap, support, clearance and final export.

Tyranid output is not byte-deterministic in the legacy engine: two identical
legacy runs with the same binary and two-thread arenas produced a two-path
difference (less than 0.9 mm in total perimeter length). A subsequent legacy
run exactly matched the fallback run's sorted geometry, ordered points and
noncomment G-code commands. The comparisons retain both outcomes rather than
normalizing away differences. All these runs passed the geometric bounds and
finite-move checks.

The new-engine infill regression covers MB01 and MB02-transition with three
patterns at 0, 17 and 45 degrees, with both cycle parities, top/bottom shell
selection and independent volume checks. Six ready-to-open projects are in
`tests/data/material_band/production/infill/`. The native GUI also loaded and
sliced the concentric MB02 project, displayed ten cycles, and exposed the
concentric interior at cycle five (1.20 mm). The exported-G-code comparison plot
shows the three patterns at cycles five and six. Full test outcomes and source
hashes are in `tests/data/material_band/evidence/infill-verification.json`.

## Three/four-material and height-gradient verification

The stack extension accepts constant whole-solid blends of three or four
physical materials and height gradients with two to four colors. It uses the
saved color sequence as input intent, with no call into the old Local-Z gradient
sampler or rewriting pipeline. Changing the old Local-Z enable switch or middle
window does not change the kernel's decoded curve or accepted print operations.
The explicit `material_band_gradient_error` setting defaults to zero; the
gradient demonstration projects select 10 percentage points so the solver can
meet minimum active band thickness while preserving the global sampled dose.

Four demonstration projects are under `tests/data/material_band/production/stack/`:
`MB-3-blend.3mf`, `MB-4-blend.3mf`, `MB-3-gradient.3mf` and
`MB-4-gradient.3mf`. They use virtual qualification profiles, ten 0.24 mm
macrocycles, dense monotonic fill, asymmetric purge matrices and all physical
tools. The retained dose plot is reconstructed from object extrusion in the
exported G-code; prime and purge extrusion are excluded.

Regression coverage includes absolute and relative extrusion, saved project
round trips, adjacent shared interfaces, individual and total deposited volume,
thread counts 1/2/4, cache invalidation, cancellation and bounded work. Cylinders,
annuli and plates with holes are accepted at the tested 64-segment resolution.
The tests retain explicit rejection for inward tapers with missing staircase
volume, a 32-segment annulus with unresolved concentric fill coverage, and a
32-segment hole plate with unsupported caps. Arbitrary painted three/four-color
shells and image-based gradients still require a spatial domain adapter.

The first native GUI check exposed a pre-existing undo-archive gap for
`coFloatsOrPercents`, triggered by `overhang_1_4_speed` while snapshotting the
previous project. Both nullable and ordinary vectors now have archive dispatch,
with round-trip regression checks for values, percent flags and nil entries.
The earlier configuration-serialization test no longer swallows exceptions.

The rebuilt native GUI successfully opens, slices and exports the four-material
gradient and the three-material constant blend, including switching projects
through the undo snapshot path. Both previews show all expected physical tools
and ten logical cycles. Every exported toolpath command matches its headless
fixture; the sole command difference is the GUI-selected bed target (45 C
instead of the fixture's 35 C). These are virtual test profiles, not qualified
printer presets. The final GUI DLL SHA-256 is
`6be44a2bf19e74a17e406f2678b86752290072cc4753ea0bb88ebca15cdfeb6c`.

After that fix, the automated runs pass 445 of 452 distinct cases: all 63
material-band cases (2,444,463 assertions), the 64-row stress case (1,606,551
assertions), and 381 of 388 other regression cases. Seven pre-existing failures
remain, including the placeholder parser crash; their names are retained in
`tests/data/material_band/evidence/stack-verification.json`. The stress matrix
covers 680/680 requested parameter pairs and 659/680 pairs when substituting
measured connected paint-component counts. Six rows use the new engine; 58
exercise explicit fallback contracts. Passing these rows does not mean all 64
models are accepted by the new engine.

The stack benchmark report contains 25 enabled workloads. Before the archive
fix, all passed the declared median budgets. Three-material constant and
gradient fixtures took 4.52 s and 2.92 s median process wall time; four-material
fixtures took 6.63 s and 2.65 s. These include schedule finalization; the path
construction medians alone were 226, 146, 307 and 128 ms. The six feature-off
comparisons produced identical normalized G-code against the pinned baseline.
Their paired time and memory estimates pass the 5% gate, while the report's
separate ratio-of-time-medians estimator misses it for two small generic cases.
All raw measurements and the exact measured binary hash are retained; these
timings are not a universal performance guarantee.

After the archive fix, 20 adjacent baseline/new pairs for each of the six
feature-off model/family combinations again produced identical normalized
commands and 104-byte preview moves. All six pass the paired time, separate
time-median and peak-memory 5% gates. Maximum paired time overhead is 2.6% and
maximum median peak-memory overhead is 1.8%. The earlier capture taken during
the large GUI rebuild failed those gates and is retained separately as a
contended measurement, not substituted for the post-build result.

## Reproduction

Windows builds use MSVC 19.44.35227, CMake 3.28.4, x64 Release, Catch2 3.7.1 and
the existing dependency build. Captures use two-thread TBB task arenas. The
pinned and new binaries use identical compiler/dependency settings. The selected
CTest domains exclude hardware-dependent GPU cases.

```powershell
cmake --build build-material-band --config Release --target libslic3r_tests fff_print_tests --parallel 4
& build-material-band/tests/libslic3r/Release/libslic3r_tests.exe '[material_band]~[.]~[stress]' --order rand --warn NoAssertions --rng-seed 20261026
ctest --test-dir build-material-band -C Release -R '^(libslic3r|fff_print):' -E 'baseline capture|benchmark capture|diagnostic artifacts|local project geometry audit' --output-on-failure --parallel 2
python scripts/material_band_stress.py --test-exe build-material-band/tests/libslic3r/Release/libslic3r_tests.exe --output build-material-band/evidence/stress
python scripts/material_band_benchmark.py --test-exe build-material-band/tests/libslic3r/Release/libslic3r_tests.exe --baseline-exe D:/FullSpectrum/material-band-baseline/build/tests/libslic3r/Release/libslic3r_tests.exe --output build-material-band/evidence/benchmarks --runs 5 --cpu-affinity 0x3
python scripts/material_band_benchmark.py --test-exe build-material-band/tests/libslic3r/Release/libslic3r_tests.exe --baseline-exe D:/FullSpectrum/material-band-baseline/build/tests/libslic3r/Release/libslic3r_tests.exe --output build-material-band/evidence/feature-off --only P0 --runs 100 --cpu-affinity 0x3
cmake --build build --config Release --target Snapmaker_Orca --parallel 4
python scripts/material_band_real_models.py --binary build-material-band/tests/libslic3r/Release/libslic3r_tests.exe --downloads C:/Users/radug/Downloads --output build-material-band/evidence/real-models/final-audit --export --timeout 900
python scripts/material_band_visual_audit.py --real-summary build-material-band/evidence/real-models/final-audit/summary.json --infill-directory build-material-band/evidence/infill-final --output build-material-band/evidence/infill-visuals
```

Set `MATERIAL_BAND_PRODUCTION_DIR` for retained MB01, MB02-transition and MB03
demonstration 3MFs and production G-code. Hidden `[material_band][diagnostic]`
artifacts retain `production_accepted: false`, distinguishing isolated stage
diagnostics from final-plan demonstrations.

M8 requires paired baseline/new specimens, surface/optical measurements,
mass/material usage, timing and defect checks on named calibrated machines and
material batches. Software agreement does not establish improved physical
finish, optical equivalence or a qualified real printer envelope.


## Snapmaker U1 adapter (2026-10-27)

Selecting `printer_model = Snapmaker U1` with Klipper and four independent
nozzles of matching diameter selects `MachineAdapter::SnapmakerU1`. The supplied
verification projects use 0.4 mm nozzles. The stationary-tool declaration
is unnecessary. The printable rectangle is (0.5, 1) to (270.5, 271) mm; maximum
Z is 270.05 mm. The kernel reserves another 2 mm above all planned object and
tower deposition for docking. Jobs without this clearance fail before export.

The firmware contract was inspected in the official
[Snapmaker U1 Klipper source](https://github.com/Snapmaker/u1-klipper/tree/93cb12faebf6e74c51bbc5f0be66e95124af14c8):
`klippy/kinematics/extruder.py` performs XY docking for `T0` through `T3`, restores
modal state and applies tool calibration, but leaves XY at the dock.
`lava/fluidd.cfg` defines initialization, nozzle cleaning, bin priming and ending.
These are firmware service operations, explicitly separated from bed deposition.

`U1Start` initializes the job, feeds/calibrates/primes used tools into the discard
area, cleans the nozzle, homes and probes the bed, then establishes a known
absolute XYZ and E mode. Its priming retractions are mirrored in the writer.
Feed and calibration checks address all four physical heads; firmware skips
unused heads. Bin priming uses logical tool indexes and preserves the firmware's
mapping, including two-material jobs mapped to physical heads 2 and 3.
The stock bed prime line is replaced by the kernel's validated prime structure.
Firmware service extrusion is outside the kernel object/tower volume accounting;
firmware calibration and bin waste are not included in the displayed estimate.

`U1ToolChange` is a frozen operation at validated clearance. It waits for the
incoming tool temperature, synchronizes, executes `Tn`, explicitly restores Z
at the dock, then restores both X and Y before resuming scheduled travel. Return
coordinates use the same formatter/precision as deposition. First-cycle and
normal nozzle/bed temperatures, per-material fans, E retraction state, logical
layer statistics and `PRINT_END` are represented in the final plan. Firmware
service motions are not expanded into individual segments in the preview.
Per-band timelapse/defect-detection motion macros are not inserted during printing.

Stock machine script templates are recognized; modified or arbitrary scripts
fail with `u1_custom_script_requires_adapter:<setting>`. Filament comments are
accepted, executable filament scripts are rejected. The adapter does not call
the old gradient sampler or Local-Z geometry/planning machinery.
Both the repository stock tool-change script and the installed 20260605 stock
variant are recognized; script whitespace normalization preserves quoted text.

The default envelope is a 72.5 mm radius cylinder from the nozzle plane to
140 mm, based on the stock clearance dimensions. This is deliberately broad,
not a measured nozzle cone. It can touch exactly coplanar deposited surfaces;
raised obstacles still cause rejection, including a tested 0.000001 mm rise.
The index prunes entirely lower cells and recognizes explicit total ordering
between the object and tower; collision budgets are unchanged. A measured
explicit envelope uses the normal nozzle/contact rules instead.

Place the prime tower outside that broad envelope when material runs span
multiple cycles: intermediate tower layers can be lower than already printed
object material. The supplied small demos place it at (220, 220) mm. A nearer
placement is not guaranteed to pass. Spatially sloping bands may also require
an explicit, qualified nozzle/head envelope.

The process scope is still Classic walls, 100% dense fill, no brim/skirt or
supports, and fixed layer height. This adapter does not add sparse infill,
Arachne, arbitrary unsupported mesh topology or physical print qualification. Rechecking the
nine Downloads projects, including the Tyranid models, now gets past the U1
firmware gate, but their original brim/process settings still cause explicit
fallback. Their files were not modified.

The U1 verification set covers two-, three- and four-material blends/height
gradients in both E modes, plus three-/four-material cylinders, annuli and
plates with holes. `scripts/material_band_u1_audit.py` independently replays
exported commands, simulates dock XY displacement and an incoming-tool Z offset,
and checks restoration, clearance, heating, bounds and material extrusion.
Machine tests also retain failures for incompatible firmware, shared-nozzle
profiles, mismatched nozzle diameters, custom motion, insufficient Z clearance
and raised neighboring solids.

The final U1 suite passes six cases / 466,376 assertions. All 18 exported jobs
pass the independent command audit, including 320 dock returns. Six complete
application CLI exports produce 215,034 executable commands identical to the
headless exports. The final four-material gradient also slices and exports in
the GUI without a fallback warning. Its only command differences are three bed
temperature commands (35 to 45 C) from the GUI's selected bed setting.

The existing 63 material-band cases pass 2,444,463 assertions. The broader
388-case regression run retains the same seven pre-existing failures recorded
in the stack report. Five paired feature-off runs for each of six profiles pass
the 5% time and memory gates with identical normalized G-code. These earlier
regression/performance captures precede the final U1-only script/startup and
fixture metadata corrections; their original binary hashes are retained in the
manifest rather than attributed to the final build.

Reproduce the U1 checks:

```powershell
$env:MATERIAL_BAND_U1_DIR='build-material-band/evidence/u1/production'
& build-material-band/tests/libslic3r/Release/libslic3r_tests.exe '[u1]'
python scripts/material_band_u1_audit.py build-material-band/evidence/u1/production --output build-material-band/evidence/u1/gcode-audit.json
& build/src/Release/snapmaker-orca.exe --allow-newer-file --slice 1 --outputdir build-material-band/evidence/u1/application/U1-4-gradient tests/data/material_band/production/u1/U1-4-gradient.3mf
```

`--allow-newer-file` is needed by this checkout's CLI version check when reading
its own newer 3MF project format. Opening the supplied projects in the GUI does
not need this flag.

The retained manifest is `tests/data/material_band/evidence/u1-verification.json`.
Relative-E demo projects are in `tests/data/material_band/production/u1/`.
Executable: `build/src/Release/snapmaker-orca.exe` with the adjacent rebuilt
`Snapmaker_Orca.dll`. Physical qualification remains deferred.


## U1 painted-region extension (2026-10-27)

`PaintedDomains` builds material regions from the owned leaf-facet snapshot.
For each macro cycle it clips the actual closed solid, retains holes, intersects
side facets at the cycle midpoint, and limits side colors to the configured wall
depth. Explicit upward/downward paint extends through the configured top/bottom
shell depth. The remaining interior retains the base material recipe. Pure
filaments, constant blends and independent height-gradient recipes can coexist.

The adapter shares `segment_painted_layer`, a geometric boundary/Voronoi helper.
It does not call the old gradient sampler or Local-Z planner. The new kernel
owns curve sampling, per-region volume budgets, thickness solving, material
bands, toolpaths, scheduling and export. Existing settings supply recipe data.
Region boundaries retain separate flat stacks, with paths ordered by actual Z;
this extension does not introduce a continuous sloping blend across paint edges.

The whole-object planner supplies the wall and dense-infill geometry. Paint
splits those paths at material boundaries without adding hidden walls around the
interior. Fitted bead caps and normal infill patterns are preserved. Residual
fragments are joined geometrically before additional lanes are fitted; every
footprint remains clipped to the unoccupied region. Curved Boolean boundaries
use a perimeter-derived integer-rounding allowance, followed by independent
material-volume, occupancy, support and clearance checks. Unsupported geometry
still fails atomically rather than silently extending a paint projection through
the solid. This initial painted adapter retained the Classic/100%-dense/
fixed-height/no-support scope. The U1 layer process described below adds sparse
infill, brim and a separate first layer.

Painted stacks can change tools multiple times at the same height. The U1
finalizer allocates separate XY strips on each tower layer, fills unused strips,
and constructs any intervening support layers before advancing the tower.
Tower layers use the bounded process thickness and can be above individual
painted sublayers; the complete combined schedule still checks head clearance.
The configured tower origin and width remain authoritative. The independent
U1 heads use `prime_volume` on every tool run, matching the normal U1 priming
setting; the shared-nozzle flush matrix is not applied to them. Disabling the
prime tower disables these planned tower volumes. Firmware startup priming
remains separate. Automatic discard-bin substitution has been removed.
Physical qualification is deferred.

Long painted gradients project each dose row onto its bounded simplex as one
constraint, then enforce integrated material volumes. Consumed layer templates
are released while paths are retained. Gap-cap fitting uses a local spatial
index. The U1 scheduler permits 500 million index visits while retaining the
existing cell, candidate and memory limits. Edge-only XY contacts are excluded
from support candidates because they have zero support or overlap area.

Reproduce the painted matrix and docking-protocol checks:

```powershell
$env:MATERIAL_BAND_PAINTED_DIR='build-material-band/evidence/benchy-review/painted-tower'
& build-material-band/tests/libslic3r/Release/libslic3r_tests.exe '[painted_regions]' --rng-seed 20261027
python scripts/material_band_u1_audit.py build-material-band/evidence/benchy-review/painted-tower --negative-checks --output build-material-band/evidence/benchy-review/painted-gcode-audit.json
```

The matrix covers two, three and four filaments; boxes, cylinders and annuli;
constant blends, height gradients, pure paint, adjacent pure/gradient regions,
explicit top/bottom paint, and adjacent different mixed recipes. Cap cases use
absolute E; the others use relative E. Tests check independent interior material,
paint colors, total and per-material volume, perimeter/infill presence, deposited
height metadata, tool order, purge accounting and bounded failures. The command
audit includes negative mutations of both E modes.


## Baseline Benchy audit and tower correction (2026-10-27)

At the baseline revision, the original painted Benchy fell back because brim
and sparse gyroid were unsupported. Diagnostic copies exposed zero-area source
triangles and the 20,000-face exact-clipper limit. The paint-bit/triangle-count
bug was corrected. The subsequent U1 layer-process work described below
supersedes those admission restrictions; full-model qualification is separate.
This is not a general-purpose engine qualification. See
[the detailed Benchy audit](FullSpectrum-Benchy-Audit-2026-10-27.md) for the
actual slice sections, remaining constraints, build and reproducible checks.
That baseline material-band suite passed 78 cases / 4,729,331 assertions;
381 of 388 broader regression cases pass, with the same seven recorded failures.
The two-object reported U1 project and 54 generated painted combinations pass
with tower priming. Current binary hashes and command audits are retained in
`tests/data/material_band/evidence/painted-regions-verification.json`.


## U1 layer process compatibility work (2026-10-27)

Qualification update, 2026-10-28: the unchanged original 225,154-face painted
`3DBenchy.3mf` passes complete planning and export with its 5 mm brim, 15% gyroid,
0.2 mm first layer and 0.06 mm minimum. Test build 118 and application build 119
pass all 232 section checks, 470 docking returns per export, and full matching
of 500,027 object segments within output rounding. The combined object/brim/tower
schedule stays inside the existing memory and work limits. Detailed evidence is
in the Benchy audit and `tests/data/material_band/evidence/benchy-layer118/`.
The output remains expensive: 360 MB, 89,372 retractions and a 19.72-hour
commanded-motion lower bound before firmware and acceleration overhead. This
establishes compatibility; broad performance and physical qualification remain
open. Desktop preview automation was unavailable; canonical renders and native
logical-cycle parsing were checked.

The U1 process route slices the owned source mesh directly into planar sections,
without the exact solid clipper's 20,000-face gate. Exactly zero-area source faces
are omitted while their original triangle indices remain available to decode
paint; nonzero thin faces are retained. The input triangle/paint-stream bounds
and malformed-data checks remain active.

The route constructs configured Classic walls, normal sparse infill (including
15% gyroid), solid top/bottom skin and inner/outer brim. Intentional sparse voids
are excluded from material-dose targets and occupancy. The first skin over sparse
fill uses bounded, anchored bridge lanes. Shared infill geometry utilities are
reused; legacy Local-Z and gradient execution are not called.

The requested first-layer height, first-layer speeds, temperatures and fan rules
are handled separately. Later material cycles combine nominal layer intervals
when the minimum active thickness requires it. Each active fraction must still
fit the thickness bounds, including the initial cycle. The original Benchy
settings produce 299 planned material cycles, beginning with a 0.2 mm cycle;
material strata within a cycle divide that height according to their recipes.

Residual paint-edge geometry is triangulated with all contours preserved. A
negative-area contour may describe a hole, so it is not discarded as a tiny
island. Triangulation checks cover total area, overlap and geometric containment.
Beads with coincident rounded G-code endpoints leave their area for bounded cap
repair and the explicit missing-coverage checks. Analytic band integration is
checked against independently integrated recipe targets at 0.01% relative error.
Manufactured path coverage uses the plan's separate 1% missing-volume budget,
with excess still bounded at 0.01%. Missing and excess volume are accumulated
separately across regions; they cannot cancel. Material fractions are checked
within each painted domain and cycle as well as globally, at 0.5 percentage
points absolute error. Painted boundaries remain unchanged.

An area tolerance does not authorize detached caps across a painted edge. When
a source path needs only a small coverage correction, its existing motion is
retained but its cap cells are clipped to the exact region. A synthetic adjacent
region regression covers the tiny detached cap found on Benchy's first cycle.

Captured Benchy fragments are retained in
`tests/data/material_band/benchy-cap-83.json` and `benchy-cap-239.json`. They
reproduce duplicate wall coverage from a discarded hole and an omitted valid
stripe beside a tiny disconnected interval. Their regressions check disjoint
deposited cells, domain containment and material volume. The hole-plate
regression also covers a crash in the abandoned polygon-set sweep.

Same-height moves using one U1 tool take a direct route only after the swept
head clears deposited geometry. Obstacles, height changes and tool changes
retain the lifted route. Short planar moves use the configured minimum travel
distance for retraction. On the reported four-material project this reduces
vertical travel from 394,029 mm to 21,612 mm, while the exported commands still
pass all 403 docking-return checks.

Current software evidence includes 89 passing material-band cases / 4,637,558
assertions after the direct-travel change, and a rebuilt
application export of the reported `U1-4-gradient` project. That export contains
all four tools and passes 403 docking returns with tower priming and no bin
substitution. Another 55 exported files pass 1,467 docking returns; eight
deliberately incomplete docking transactions are rejected. The original Benchy is still undergoing full planning, scheduling
and export verification; these component results do not yet qualify it.

### Solid-skin audit follow-up (2026-10-28)

The independent support check exposed missing solid infill in the original
Benchy. The raw rectilinear/monotonic fill API rejects an overlap larger than
half its spacing by returning no inner contours. The layer process now expands
the input region explicitly and leaves the generator's overlap at zero. It
checks required skin coverage independently of generated paths, so a missing
skin cannot be misclassified as intentional sparse air.

Concentric skins also require gap fill between their last lanes. The kernel
repairs those pockets within the required solid region, preserving holes and
sparse voids. Wall-overlapping fill lanes retain their motion where printable;
sub-output-precision fragments are retried against the remaining footprint.

Cap subdivision can produce the same microscopic triangle twice after integer
rounding. Planar segment normalization removes duplicate coverage and recomputes
physical volume and flow before final band coverage and recipe checks. A reduced
ten-cell fragment from Benchy reproduces this failure on the previous build and
passes after normalization (`benchy-cap-normalization.json`).

Planar runs query preceding support once for their shared lower face; each cell
is still checked against that contact coverage. Positive-volume overlap queries
remain separate, including overlaps within a single run. This avoids repeating
an expanded spatial search for every subdivision of a detailed cap.

Bridges may anchor to actual preceding lower faces, or to previously ordered
planar deposition at the same height that covers their full height. Those
anchor paths must themselves pass support validation. Future paths, floating
anchors, partial-height contact, and an expanded overhang margin do not qualify. Both
finite end-cap contact and the complete unsupported span remain bounded.

Small protruding caps can be fitted to the preceding layer's allowed overhang,
within one quarter of the bead width and at most 1% of the segment's volume.
Their removed dose is charged to coverage before local material coverage and
fraction checks are repeated. A deep, thin spike cannot pass solely because its
area is small. Per-layer spatial tiles cache the local support polygons used
for these queries. Solid-fill runs also carry their owned skin region, including
wall overlap, so generator expansion cannot fill neighboring sparse air.

Build 45 passes 91 material-band cases with 4,675,892 assertions, including 25
focused scheduling/layer-process cases with 72,639 assertions. Its 55 exported
painted fixtures pass 1,467 docking returns and eight negative mutations. The
actual application export of the reported four-material project passes another
403 docking returns. The captured first five Benchy cycles pass scheduling;
full-model qualification is still running. No legacy Benchy export is counted
as a material-band success.

### Painted bridge pass planning (2026-10-28, verification in progress)

The full Benchy replay exposed a bridge split across paint recipes with
different first-material heights. Scheduling those disconnected fragments
earlier cannot provide physical support. The layer process can now build a
continuous first-material foundation when all active regions share that first
physical material. Paint provenance is retained in local dose checks: the
foundation's covered volume is intersected with each original region before
checking its final recipe. Remaining passes preserve the original material
amounts and physical thickness limits.

If a foundation would leave a sub-minimum remainder, neighboring logical cycles
are merged and the owned mesh sections rebuilt. The initial layer stays
separate. The aggregate dose solver accounts for a material spanning several
passes; every emitted pass is still checked against the original thickness
bounds. A missing common first material or infeasible remainder cannot be
made printable by bypassing scheduling. There is no connection to the legacy
gradient execution path.

The ordinary painted sequence is qualified first against actual preceding top
faces. An existing manufacturable sequence keeps its geometry. Only a support
failure requests a common pass or cycle merge from the path planner. Local
qualification guides reconstruction; the final complete object and tower still
pass the unchanged combined scheduler. A consumed first material retains its
shared interface patch with the next material, even when it no longer has a
separate extrusion run.

Bridge endpoint qualification measures actual shared side length inside the
finite end cap as well as contact beneath it. Side bonds require earlier,
independently validated, full-height deposition. Narrow gap attachment and cap
repair likewise require real shared edges. The new bridge fixture covers two,
three and four physical materials, local volume conservation and rejection of
unprintable thin remainders. Full-model acceptance and export checks are pending.

Construction preflight also covers unsupported painted walls and sparse-fill
lanes. Its optional planar ordering mode keeps the material, domain, height and
macro-cycle boundaries fixed. Within a pass, it validates supported paths first
and retries paths waiting for their side anchors. Paths still waiting when no
progress is possible are rejected. Only independently validated paths can
supply coplanar contact, so floating groups cannot support one another. The
resulting order is stored in the path plan before finalization fixes the complete
object and tower sequence. Explicit final sequences cannot opt into reordering.

General cap triangulation normalizes all touching contours together before
tessellation. This prevents a domain-wide numerical area budget from hiding a
small duplicated island. The cycle-109 Benchy capture is retained as a regression
fixture. Infill motion over an already occupied prefix or suffix is trimmed at
the external ends of open runs; internal joins and closed seams stay continuous.
The dose is recalculated for the remaining motion and checked against the same
owned coverage.

Open and closed outer walls may follow a touching inner wall, using only its
independently grounded sections. Each qualifying motion segment needs at least
half its footprint on actual preceding material, with the whole footprint inside
the normal floor allowance capped at half a bead width. A narrow seam between
floor beads can satisfy this rule without covering the centerline itself.
Sections supported through bridging or a neighboring wall cannot relay support
farther outward. Attachment needs a real shared side and is bounded by one bead's
square-cap diagonal, including fitted corners. Endpoint checks use only the
qualified anchor faces. Fixed final sequences validate the same rule in their
declared order. Support offsets retain small corners instead of using the general
offset helper's edge decimation. Point-touching wall pieces are grouped within
two integer-coordinate quanta for the contact test and clipped back to the
unchanged footprint; a real gap still separates components.
`benchy-wall-46.json`, `benchy-wall-57.json` and `benchy-wall-61.json`
retain closed and open curved-hull regressions, alongside gap, late-anchor,
floating and chain-rejection tests.

Planar support always includes the actual preceding footprint before adding an
expanded allowance. A simplified positive offset cannot erase real material
under a narrow cap. `benchy-gyroid-floor-52.json` retains the captured regression.
The optional cap fitter follows the same actual-contact rule. A fitted candidate
must satisfy every painted region's total volume and recipe budgets, including
its share of a common foundation. If fitting would exceed those budgets, the
original coverage is retained and still requires independent support validation.
Foundation targets use the same intended painted footprints as the remaining
passes. Source-path gaps do not reduce those targets; actual coverage is measured
separately. `benchy-foundation-157.json` checks the complete captured partition and
its support scheduling, while a small-region negative case retains rejection of
an underfilled foundation.
Bridge direction selection checks the unsupported span as well as endpoint
contact, so it can try a shorter direction when a diagonal exceeds the span
limit. Final scheduling independently checks the resulting deposition again.

Closed perimeter source contours move their seam onto preceding material when
the default seam lacks support. Both seam directions prefer at least 75% actual finite-cap contact. The
preceding endpoint is checked too when the closing edge is no longer than one
bead width, since that short edge may be wholly owned by the starting cap. If unavailable, the strongest
candidate meeting the existing 50% anchor contract is used. The
contour points and direction are preserved; bead ownership is rebuilt after the
rotation. Scheduling still requires actual anchors, including the concrete start
of a closed loop. Closure alone cannot anchor the first extrusion.

A wall may bridge a bounded interruption between independently grounded side
anchors. Its certified ordinary support is excluded from the bridge span. All
remaining unsupported bead area, including off-center caps, is projected onto
path arclength, with contiguous intervals and closed-seam intervals combined.
The 20 mm limit and finite endpoint-anchor checks remain in force. Bridged
sections do not become grounded anchors for a subsequent wall. The captured
`benchy-seam-182.json` checks unchanged contours, coverage, construction order
and final explicit order. Separate cases reject an overlong gap, a missing
starting anchor, floating anchors and an undeclared bridge.

After paint partitioning and cap fitting, complete closed walls also recheck
the seam against the final preceding deposition. Rotating vertices and their
matching segments preserves every bead cell, speed, width and dose. The captured
cycle-157 foundation regression exercises this final-contact adjustment.

Established per-object phase chains and verified explicit final sequences
already imply ordering between nonadjacent operations. Support and access
contacts still receive their full geometry checks, but do not allocate redundant
forward dependency links within those chains. Backward links remain to expose
conflicts. Flexible construction preflight retains its explicit geometry links
until an order has been established.

Grounded-wall polygons are a coplanar-contact cache, distinct from the actual
bead cells and ordinary floor footprints. They are released only after no
remaining path can use that height, including deferred construction paths and
later object/tower returns. A conservative height window covers support-tolerance
rounding. Temporary support vectors return their memory allowance when destroyed;
mutable adjacency storage is released after ordering and before travel validation.
A 512-layer wall stack checks the resulting bounded memory use.

The cell index partitions by centroid spread, weighting Z for thin-layer queries
while retaining original millimeter bounds and exact geometric predicates. This
avoids ineffective splits through identical centers of long bead boxes. Uniform
owner subtrees allow planar contact queries to skip owners already represented
by their complete footprint, or unable to supply contact. Ordinary overlap and
swept-motion queries still examine the original cells. Index-build sources are
released before support validation; query work and memory remain bounded.

For a structurally verified explicit total sequence, index subtrees also record
their earliest operation rank. Motion queries can exclude a wholly future
subtree before reaching its leaves. Static geometry remains eligible, current
deposition remains visible to self-access tests, and final departure includes
the last deposited path. Support and overlap checks do not use this pruning.
Earlier-obstacle and static-obstacle regressions retain collision rejection.
