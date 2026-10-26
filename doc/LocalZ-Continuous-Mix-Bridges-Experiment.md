# Continuous Local-Z mix bridges

Branch: `experiment/local-z-continuous-mix-bridges`  
Starting checkpoint: `63117732a0`  
Status: experimental; ordinary builds disable the engine by default, while the current experimental build enables it. No physical print validation.

## Behavior

Adjacent recipes can share continuous physical-color islands for their common physical
filaments even when their component thicknesses differ. The planner forms
connected groups from touching paint masks separately on each layer:

- Equal component sets share a complete stack, with any number of positive,
  printable components. Every cumulative interface varies continuously.
- Shared interfaces can cross nominal layer planes. Both sides of each bead
  follow the same continuous fields; Z and flow taper across the boundary.
  There is no private-flat-pass fallback or abrupt thickness switch.
- Different sets can share all their common colors through a common stack.
  For example, CM, CMY and CMYK join C and M; Y also joins across CMY/CMYK.
  Extra colors form closed islands immediately before a common carrier color, tapering inside
  their authored domains towards regions lacking them. Where the conservative
  shared-stack bounds fail, the older single-interval common-anchor planner
  remains available.
- Separate groups can coexist with ordinary Local-Z recipes on the same
  object. Recipes without a usable connection retain their original planner.

Each physical pass is generated from the union of its eligible color masks.
Its outer and inner walls form closed islands before height modulation. A
thin, lower part and a thick, higher part of the same color are one non-planar
island, not independent planar islands joined by clipped wall fragments.
Disconnected XY components stay separate. Per-pass walls replace all original
walls in the owned footprint, including nominal intervals with no pass.

When Local-Z infill is enabled, infill is regenerated inside these physical
islands with their own wall overlap, using the same height fields as the walls.
The cycle conservatively retains solid coverage from any nominal layer it
spans, so a floor or roof cannot become sparse simply because the scheduling
layer was sparse. This can thicken skins within one cycle. With Local-Z infill
disabled, the existing nominal interior remains. Tower prediction and G-code
export use the same cached island geometry and physical-tool ownership.

If the narrow-infill Voronoi solver rejects an island's skin geometry, that
island retries with rectilinear fill at the same solid/sparse densities and
flow. Partial output is discarded first. The fallback is limited to the
Voronoi failure, preserves the island's walls, and is reported in the log.
The recovered stretched cyan project exercises this at Z=50.12 mm.

Pure neighbors keep every nominal layer. Their common filament becomes the
last mixed component when a printable connection exists. The pure-boundary
interfaces prefer low nearby nominal planes. Remaining nominal salmon layers
extend a short distance into the mixed region as tapering tongues, instead of
lifting the black edge to meet the penultimate pure plane. The final salmon
bead fills the remaining height, becoming thicker toward the mixed interior.

Tongues have closed polygon ownership, sampled at 0.05 mm. Their delivered
upper surface is also the next bead's lower surface: omitted tips are returned
to the final bead rather than leaving unowned volume. Merging and extra-color island tips
are an explicit experimental exception to the configured minimum height:
they may taper to **0.01 mm**. Ordinary recipe components retain their height
bounds. The tongue edge can have a 0.01 mm height change; this is not a claim of
perfectly smooth or physically validated zero-thickness extrusion.

## Enable

Ordinary builds default to disabled. An experimental build can enable bridges
without a launcher environment variable:

```powershell
cmake -S . -B build-dev-release -DORCA_LOCAL_Z_MIX_BRIDGES_DEFAULT=ON
cmake --build build-dev-release --config Release --target Snapmaker_Orca_app_gui --parallel 4
```

`ORCA_LOCAL_Z_MIX_BRIDGES=1` enables bridges for a fresh process;
`ORCA_LOCAL_Z_MIX_BRIDGES=0` disables them even in an experimental build.
The **Continuous mixed-color islands** process setting (`dithering_local_z_mix_bridges`) is saved with projects and presets. It must also be enabled. The environment variable remains a developer override of engine availability; restart and reslice after changing it or the build.
G-code records the effective engine switch as `; local_z_mix_bridges_enabled = 0/1`. This records availability, not a claim that every region joined. Runtime folders cited below are historical validation snapshots.

The log reports `Local-Z mix bridges ... cycle_span=... passes=...`.
G-code contains `; local-z mix bridge component=N` markers. N identifies the
ordered pass within a group, including residual branches.

## Eligibility and limits

Whole-object Local-Z and either setting of mixed-region collapse are supported.
Fixed recipes require positive physical component weights and printable bead
heights. Gradient recipes are sampled per cycle. Manual patterns, adaptive image maps, explicit A/B height
overrides, rafts and spiral vase retain their existing planner. Preserve-first-
layer is required. Wall widths, counts, speeds and scarf settings must match.
Infill density, pattern, flow and speed may differ between regions; generated fills retain their region ownership.

A cycle may span several nominal layers. The planner chooses the shortest span
admitting the most eligible recipes, respecting the configured minimum and each
tool's maximum height. It tiles the complete post-first-layer height; a short
tail is absorbed into a longer printable cycle. No ratio is clamped to fit.
For the reported 0.08 mm nominal/minimum case, 50:50 and 51:49 recipes use a
0.24 mm cycle: 0.12/0.12 and 0.1224/0.1176 mm respectively.

When all active recipes use the same physical component set, a component can
occupy several consecutive beads. The slot count for each physical material is
shared across those recipes. Consecutive slots of the same color can merge locally
into fewer beads, while retaining continuous color boundaries, closed island
walls and matching fill. The planner evaluates both unsplit and
split candidates. It recomputes split counts after excluding incompatible
recipes, so an extreme recipe cannot force excessive subdivision on its neighbors.
The chosen slot table also governs longer closing cycles. Unlike sets may use
repeated slots for their common colors; each extra color must fit one bead.
Common slots merge using actual authored thickness, after subtracting extras,
so normalization does not introduce unnecessary doubles in thinner regions.

For a repeated-color run with bottom B and top T, optional internal interfaces
are `max(B, T - remaining_slots * cap)`. A shared cap is chosen so every mixed
plateau uses the number of beads required by the tool maximum, each at least
the configured normal minimum. Lower beads taper out where the total thickness
fits the upper bead. Their closed masks use the existing 0.05 mm grid and
0.01 mm same-color tip exception. Omitted tip volume returns to the upper bead;
the cap reserves room for the tip and the grid interpolation error. The travel
ceiling includes that tip allowance. The physical color boundaries and total
material volume are unchanged.

A run joined to a pure region of that same color retains its original split,
as do runs with no feasible common cap. Repeated-component cycles distribute
closing height across shorter cycles where possible, rather than concentrating
it in a large final stack that loses the ability to merge.

For the six-recipe example, ordinary 0.48 mm cycles use a magenta cap of
0.2616 mm. The 50% region prints one 0.24 mm magenta bead; the 86% region prints
0.1512 plus 0.2616 mm. Two magenta beads remain necessary in the latter because
its 0.4128 mm total exceeds the 0.32 mm tool maximum.

The six cyan/magenta recipes reported on 2026-10-23 range from 50:50 to 14:86.
With a 0.06 mm minimum and 0.32 mm maximum, one bead per material cannot represent
83% or 86% magenta at any shared cycle height. Rejecting these touching recipes
also rejected their neighboring groups, producing zero bridge passes in the old
build. The new plan uses a 0.48 mm cycle: one cyan bead and two magenta beads.
For 50:50 their heights are 0.24/0.12/0.12 mm; for 14:86 they are
0.0672/0.2064/0.2064 mm. All remain within the configured limits. The closing
cycles can span seven or eight nominal intervals with the same slot counts.

Multi-interval cycles currently require a constant object cross-section.
Paint is sampled at the cycle's middle interval. A short painted run absent at
that sample is retained using its nearest active interval. Masks stay frozen
throughout the cycle: vertical paint transitions are quantized to the cycle
height. Every active painted mask must be covered by its group's complete
footprint; otherwise its original planner is retained. Solid neighbors can
appear or disappear without disabling other complete cycles. Shared solid
regions are included when finding paths between disconnected mixed masks.

Equal-set passes are scheduled by the minimum of their local top targets,
including pure-region pins. This emits a ramp before nearby planar passes
advance above its low end, even when its upper end crosses a nominal plane.
The maximum top remains the travel reference; scheduling does not flatten the
actual surface. Compatible unlike sets also span multiple intervals. Changing
cross-sections, infeasible shared-stack bounds and impossible tails retain the
original planner or the existing single-interval anchor fallback.
Rejection restores a complete run, including every group that depends on it.

Shared walls and enabled infill now advance together inside the owned island.
Tongues also depend on the preceding pure nominal bead, even when their tip
is lower elsewhere. The pass ceiling remains the travel reference. This is
not a full nozzle-shape clearance proof; physical validation remains needed.

As a fallback within one nominal interval, unlike sets can share a common bottom
filament where their feasible height ranges intersect. Residual branches remain
separate. Equal component sets share the full stack. Pure neighbors can join one
terminal physical color per group when printable boundary targets exist and
the pure stack can keep all its nominal beads. A small dynamic program
chooses those targets from the pure region's nominal planes, respecting every
component minimum/maximum. Mixed targets may span those planes. Otherwise the pure
region keeps separate nominal beads while compatible mixtures can still join.
An unjoined pure zone does not constrain the mixed interfaces. Keeping its flat
passes in the cycle footprint lets short closing cycles retain complete paint
coverage without changing the pure layer schedule.
Real XY gaps stay clipped
out. Nearby unowned geometry, including physical colors absent from the group,
on any covered interval prevents a varying group, using a guard of half
the largest participating nozzle diameter plus 0.10 mm. Neither this guard nor
the slope bound proves full nozzle-body clearance; physical validation remains
outstanding.

## Shared interfaces

For each cumulative target height `h_i` and distance `d_i(x)` to its original
zone polygon, use the following height above the shared cycle bottom:

```text
T(h)(x) = clamp((min_i(h_i + s*d_i(x)) + max_i(h_i - s*d_i(x))) / 2,
                min_i(h_i), max_i(h_i)),    s = 0.12 mm/mm
```

Distances, envelopes and clamping preserve the slope bound. This operator is
monotone and translation-equivariant: if every target interface is at least m
above its predecessor, the smoothed interfaces remain at least m apart. The
corresponding maximum bound also holds. This prevents interfaces crossing at
multi-zone junctions. Every pass uses the same zone set and spatial distances,
independent of path seam and direction. Bead heights sum to the shared cycle height.

For a joined pure zone, each intermediate target is an original nominal plane.
The interface is additionally clamped between `target - s*distance` and
`target + s*distance` for that zone. Inside the pure mask the bounds coincide,
pinning the interface exactly. These bounds use the same ordered target vector,
so the composed field retains the slope and bead-height bounds. Earlier pure
beads print at every nominal plane. Planes above the last foreign component
can extend into the mixed region as the same-color tongues described above.

For unlike component sets, the common bottom bead also observes spatial bounds:

```text
lower(x) = max_i(min_anchor_i - s*d_i(x))
upper(x) = min_i(max_anchor_i + s*d_i(x))
anchor(x) = clamp(shared_target(x), lower(x), upper(x))
```

These are Lipschitz fields too. At a zone boundary its residual beads stay within
bounds; away from transitions the target ratio is retained. A residual interface
with cumulative residual fraction r is
`anchor(x) + r * (z_hi - anchor(x))`. It starts on the shared lower interface,
stays within the slope bound, and ends at the nominal top plane. Scheduling uses
the lower envelope in increasing order; travel uses the corresponding ceiling. All physical tools used by
Local-Z passes are registered even when absent from the nominal tool sequence.

The existing Local-Z volume convention scales linearly with bead height and
uses projected XY length. Under that convention, component volumes sum to the
original full-height volume. This is not a calibrated bead cross-section model.

For two equal-set zones with an unconstrained straight boundary, ramp slope is
s/2 and the transition extends `abs(h_A-h_B)/s` into each zone. A 0.14/0.06 mm
interface therefore has a transition about 1.33 mm wide. Different-set bounds
can make the ramp asymmetric. Ratios are blended inside these bands; narrow
zones can lose their constant-ratio interior. Distance is Euclidean, so thin
features, holes and separate islands of one recipe can influence the field.

G-code samples transitions at no more than 0.05 mm spacing. Constant-height,
constant-flow samples within an original straight edge are coalesced. Different
seam cuts can yield slightly different linear approximations: the Lipschitz
bound is 0.003 mm per profile, or 0.006 mm between two independently sampled
profiles, plus coordinate rounding. This is not a cached common discretization.
A physical coupon is still needed to assess scraping, extrusion stability,
color blur and surface improvement.

## Validation

The `[MixBridge]` tests cover ordered four-component interfaces at a three-zone
junction, complementary volume, minimum/maximum heights for two-versus-three
component recipes, slope, reversed/closed paths, separate groups, real gaps and
infeasible shared-height ranges. Export fixtures exercise equal and different
component sets, multiple groups, changing connectivity, fallback cadence,
first-layer handling, fill coverage, tool registration and prime-tower lengths.

```powershell
cmake --build build-dev-release --config Release --target libslic3r_tests Snapmaker_Orca_app_gui --parallel 4
& .\build-dev-release\mix-bridges-merged\libslic3r_tests.exe '[MixBridge]' --reporter compact
$env:ORCA_LOCAL_Z_MIX_BRIDGES = '0'
& .\build-dev-release\mix-bridges-merged\libslic3r_tests.exe '[LocalZ]~[.],[SlicingSpeedPort]~[.],[MixedFilament]~[.]~[!shouldfail],[WallSupport]~[.],[PaintIslands]~[.]' --reporter compact
```

The 2026-10-22 island revision passes 83,597 assertions in the 12 bridge
cases and 292,244 assertions in 176 regression cases. Added checks cover
closed generated perimeters, deposited coverage of tongue extensions,
pointwise support and volume through multiple tongues, preserved pure
nominal layers, varying-Z paths and prime-tower tool/length parity.

The latest recovered 27 mm cube has 61:39 and 36:64 salmon/black recipes,
0.08 mm nominal layers, and a 0.06 mm normal minimum. Its export has 418
physical passes over 314 owned intervals and 78 uninterrupted black ramps
across both mixtures. One inspected black pass varies from Z=6.294 to
6.354 mm, with bead heights 0.0936 to 0.1536 mm, including its own closed
walls and matching infill. The 335 checked far-pure salmon segments remain
on their nominal planes, with preview-reported heights within 0.0001 mm of
0.08 mm. The maximum pass ceiling above its scheduling interval is 0.096 mm;
this is not a measured nozzle collision.

The final release application and tests built successfully. The hidden startup
smoke completed. All six recovered-project exports passed with the environment
override absent, including the stretched cyan case's narrow-infill fallback.
The cyan cases retain 335 and 624 checked pure segments, with the correct tool,
flat nominal planes, and reported heights within 0.0001 mm of 0.08 mm.

Artifacts under `sandboxes/local-z-mix-bridges/island-repro/` include the
recovered mesh/settings, old and new G-code, all-extrusion CSV, a front-wall
profile comparison, a 3D view of one black pass, and the verification script.
The front plot shows centerline bead strips; rounded XY corners are not
represented as deposited-volume boundaries. No physical print validation
has been performed.

Logs are under ignored `sandboxes/local-z-mix-bridges/`.

The 2026-10-23 split-component revision passes 127,811 assertions in 13 bridge
cases and 336,458 assertions in 177 regression cases. New coverage includes
the six adjacent ratios, repeated physical slots, pointwise volume/support,
pure neighbors of either component, prime-tower parity and an adjacent physical
color absent from the recipes.

The latest recovered project reproduces the old failure with zero bridge passes.
With the change it exports 668 physical passes over 502 owned nominal intervals.
The actual front-wall G-code has 83 uninterrupted cyan paths and 166 uninterrupted
magenta paths crossing all six mixtures. A sampled magenta path varies from
Z=2.705 to 2.806 mm, with reported bead heights 0.108197 to 0.2064 mm.
All 502 checked far-pure cyan segments retain the correct physical tool,
flat nominal planes, and heights within 0.0001 mm of 0.08 mm. The maximum pass
ceiling above its scheduling interval is 0.2304 mm, not a measured nozzle collision.
The reconstruction uses the latest autosave's paint, settings and transform with
its source cube mesh; its slicer interval count differs by one from the saved GUI
export. Both reproduce the zero-bridge failure before the change.
The release application and tests built successfully; the hidden `--help` startup
smoke exited 0. The prior salmon/black and stretched cyan projects also re-export successfully.
Their external-wall and all-extrusion CSVs are byte-for-byte identical to the
previous island build, including its preserved pure-color planes and tools.

Artifacts are in `sandboxes/local-z-mix-bridges/oct23-repro/`, including original
and new G-code, extrusion CSVs, `verification.json`, `verify_paths.py` and
`connection-comparison.png`. The plot shows centerline strips using average
segment heights; rounded XY boundary corners are not deposited-volume contours.

The regular regression command excludes two existing `[!shouldfail]` cases.
The earlier unfiltered run exited 42 because `m1: compute uses num_physical
bound, remove_physical uses kMax=64 (differential)` passes its assertion despite
that marker. `batch_remap mixed pair-fallback (stable_id=0)` fails as expected.
Those tests and their mixed-filament implementation are unchanged.

## Local merge performance revision

The progress label "Slicing mesh" includes Local-Z cycle planning and taper
mask generation. Profiling the six-recipe reconstruction attributed 82.1 of
83.4 seconds of preparation to 168 tongue masks. These repeatedly evaluated
polygon distances and unioned thousands of grid-cell triangles.

The revised implementation builds immutable spatial indexes for zone, tongue
and optional-bead boundaries, reuses distances across interfaces at a point,
and coalesces fully included cells into row rectangles before polygon union.
It retains the 0.05 mm boundary sampling. Independent cycles build concurrently;
selection, ownership and scheduling still run in deterministic cycle order.
Overlap/hole tests compare indexed fields with the original geometry queries
within 0.000001 mm. Single-cycle construction, shared cap bounds, nested masks,
pointwise support/volume, travel ceilings, closed walls and coverage of optional
bead boundaries have deterministic regression coverage.

The final recovered project prepares in 3.51 s and exports in 9.29 s, versus
83.40 s and 16.13 s for the previous build on this machine. Before the closing-cycle
refinement it measured 3.22 s and 9.01 s. The same extrusion CSVs were produced with serial and parallel cycle
construction. A separate whole-program one-worker harness attempt stalled before
slicing; it was stopped and its test-only concurrency override removed. The
serial-cycle comparison does not restrict other existing slicer parallel work.

Artifacts and timings are in `sandboxes/local-z-mix-bridges/merge-slots/`.
`merge-comparison.png` shows the recorded wall profiles. At X=121 mm between
Z=5 and 15 mm, 42 magenta beads of 0.12 mm become 21 beads of approximately
0.24 mm. The 86% region retains its required second bead. All 502 checked
far-pure cyan segments keep the original tool and nominal planes.
The final result has 83 continuous cyan and 83 continuous magenta front paths;
closing cycles no longer require uniform doubles in the 50% region. It retains
668 physical passes over 502 nominal intervals; optional lower magenta islands
cover only the thicker regions. The maximum pass ceiling above its scheduling
interval is 0.2416 mm, not a measured nozzle collision.

Final validation passes 181,013 assertions in 15 bridge cases and 389,660 in
179 regression cases. The prior salmon/black and stretched cyan projects also
export successfully, preserving 335 and 624 checked pure segments respectively.
Their geometry is no longer byte-identical because indexed distances avoid the
old integer projection rounding and raster-cell union tessellation changes;
field equivalence is covered to 0.000001 mm. The release DLL/launcher linked
successfully and the hidden `--help` startup smoke exited 0.

The dependency/test build uses the existing CMake configuration. The DLL and
launcher were linked through their generated MSBuild projects with
`BuildProjectReferences=false` and `OutDir` set to `mix-bridges-merged/`, using
the updated libraries. This avoids replacing the user's running runtime or
triggering another complete GUI reconfiguration. `BUILD.txt` records hashes.

## Three- and four-color stacks

The recovered 15:13 autosave contains CM recipes with 39%, 66% and 50% M,
plus CMY at 33:33:34, with 0.08 mm nominal layers, a 0.06 mm minimum and
0.32 mm maximum. The earlier unlike-set planner exported zero bridges.
The shared planner uses ordinary four-interval cycles and a printable closing
cycle. The actual export contains 201 uninterrupted C front paths and 201 M
front paths through the CMY region, with Y confined to its own closed island.
Replacing that recipe with equal CMYK gives 161 shared paths of each common
color, using ordinary five-interval cycles. Both preserve all 806 checked pure
C layers at nominal height. Preparation measured about 4.1 seconds for the
three-color version; this is a different, larger object than the six-recipe
timing above. Export produces more paths than the independent fallback.

For unlike sets, the stack is ordered as non-carrier common colors, extra
islands, then the terminal common carrier. Carrier selection prefers a touching
pure region when that color has enough capacity; otherwise it favors the common
color with the most available thickness. Other common heights remain authored.
Extra thickness is added to the carrier target slots, then subtracted only from
that carrier when evaluating the actual surfaces. Yellow retreat therefore no
longer enlarges magenta in a CMY/pure-C transition. Each column still sums to H,
shared colors connect, and absent material stays outside neighboring recipes.

The extra prefix is capped by the smallest local carrier slot capacity above
the normal minimum. This replaces the old global normalization constraints.
Pure interface targets use the ordinary minimum and nearest nominal planes.
Repeated carriers reserve capacity in every slot; same-color merging still uses
the actual authored plateau thickness. Extra schedule keys follow the preceding
non-carrier interface and are strictly ordered below the smallest printable tip.
Ceilings include extra height and the local capacity bound.

Sparse groups use an experimental maximum surface slope of 0.24 mm/mm;
equal-set groups retain 0.12. For a single carrier, half the budget goes to
common targets and half to extras, divided in proportion to their maximum
heights. Repeated carrier slots additionally budget for local capacity changes.
Extra masks use the existing 0.05 mm sampling and 0.01 mm tip exception. They
are sampled across their domains because capacity can also bind away from
absent-color edges. Dropped extra tips return to the carrier, with a possible
tip-sized step at the contour.

Incoming pure carrier layers extend into the retreating extra islands. The
reference terminal surface is evaluated without pure neighbors; the tongue
uses the larger of its original ramp and that reference capped by its nominal
plane. This keeps the final carrier bead near its authored thickness instead
of temporarily inflating it as an extra disappears. It also accounts for changes
in other common interfaces caused by nominal pure-plane alignment. These closed
tongues remain confined to the pure influence band and use the existing 0.01 mm
taper exception. Under-minimum extrusion in this band remains experimental.

This remains conservative: the unlike group needs a common physical color,
extra colors requiring multiple beads fall back, and palette-wide preflight
may reject some independently feasible disconnected groups. A pure neighbor
whose color is not common to the group cannot join the terminal pass. Existing
cross-section, ownership, wall-setting and physical-clearance limitations apply.

Validation includes equal three-color stacks, equal four-color stacks, a
CM→CMY→CMYK sequence, pure nominal layers, per-point support and volume, bounds,
closed extra-color island coverage, absent-color exclusion, and a repeated
common color that merges only where it fits. All 18 bridge cases and 182 cases
in the broader regression selection pass (208,240 and 416,887 assertions).
The preceding six-CM-recipe project exports byte-identical wall paths to the
working two-color build. Artifacts, reproduction settings and an exported-path
comparison plot are in `sandboxes/local-z-mix-bridges/multicolor-repro/`.

## Compact transition validation

The recovered 16:01 project uses CM recipes at 38%, 50% and 68% M plus
CMY at 33:33:34, with 0.08 mm nominal layers, 0.06 mm minimum and 0.32 mm
maximum. Ordinary cycles now span three nominal intervals instead of four.
The yellow field reaches its 0.0816 mm plateau within 0.68 mm of an absent
boundary, compared with 1.813 mm for the old 0.1088 mm plateau. At fixed cycle
height the doubled extra slope alone halves that taper width.

Exported front paths confirm that M stays at 0.0792 mm in the final CMY region,
where the previous build grew from 0.1056 mm to about 0.15 mm near pure C.
There are 120 uninterrupted front paths of each shared color and all 361
checked pure-C nominal layers remain 0.08 mm. An equal CMYK variant also
retains 120 shared C/M paths and all pure layers; its ordinary components
are 0.06 mm in a 0.24 mm cycle. Plateaus preserve authored ratios.

The actual CMY export has 601 bridge passes and 754,619 moves, versus 541
passes and 916,078 moves previously. Preparation/export measured 1.49/8.01
seconds versus 1.22/7.29 seconds on this machine; this change does not claim a
slicing speedup. The six-CM-recipe regression remains byte-identical to the
previous working two-color build. Artifacts and the exported-profile plot are
in `sandboxes/local-z-mix-bridges/compact-transitions/`.

All 19 bridge cases pass (241,759 assertions), as do 183 cases in the broader
regression selection (450,406 assertions). Focused tests cover compact
three/four-color ramps, stable non-carrier height,
terminal carrier thickness, pure-layer intrusion, plateau confinement, support,
volume, slope limits, and repeated carriers with a pure neighbor. Runtime and
regression results are recorded in the release folder's `BUILD.txt`.

## Transition control and nominal preview layers

The Advanced **Mix transition length** setting in **Others > Mixed Filaments**
scales the existing transition distance. Its default is 100%; 50% halves the
distance and 200% doubles it, within a 10–1000% range. Actual millimeters depend
on the recipe and component heights. The setting scales the interface slope
budget inversely; shorter transitions are steeper. It is saved with presets
and projects, and changing it invalidates slicing so the island fields rebuild.

Non-planar previews now group extrusion moves into spatial bands at the
configured process layer height, with the initial layer height kept separate.
A move belongs to the band containing its endpoint. Returning to a previous
band does not create another layer, and travel lifts do not add slider layers.
Render paths split at band boundaries so selecting a range also handles
descending ramps and noncontiguous visits by different tools. Actual geometry,
G-code and print sequence are unchanged. Planar, scarf and vase previews retain
their existing layer detection. Exported and reopened G-code use the same grid.

The recovered project reports 362 nominal preview bands at 0.08 mm, including
its 0.20 mm initial layer. Its default wall export is byte-identical to the
preceding compact-transition build. At 50%, the yellow plateau extends another
0.341 mm toward the pure-C boundary, consistent with halving the 0.68 mm taper.
Both exports keep 120 continuous paths per common color and all 361 checked
pure-C nominal layers; ordinary magenta plateau heights stay unchanged.

The focused bridge/processor selection passes 27 cases and 242,391 assertions;
the broader regression selection passes 184 cases and 450,865 assertions.
Tests cover 0.08/0.12 mm preview bands, descending/repeated visits, travel hops,
scarf/vase preservation, transition scaling, saved settings, re-slicing after
an edit, and reopening exported G-code. Reproduction artifacts are in
`sandboxes/local-z-mix-bridges/controls/`. Physical print validation is still
required for these experimental ramps.

## Claude discussion

Read-only discussion used the
[claude-conversation skill](C:/Users/radug/.codex/skills/claude-conversation/SKILL.md),
session `9b547d4c-cb82-44c4-993f-629c93cdd277`, titled "Local-Z continuous mix bridges".
Claude reviewed the shared-interface math, first-layer handling, ownership,
ceiling-based scheduling, mask partitioning and foreign-group contact. The
implementation uses a common bottom filament with separate residual stacks for
unlike sets, rather than requiring all recipes to split into the same number
of beads. It uses shared spatial fields and multi-interval wall cycles. Claude also
reviewed the external-wall overlay, quiet intervals, first-layer handoff,
physical paint filtering and foreign-neighbor checks. The follow-up discussion
covered preservation of pure nominal planes, terminal common-color joins and
pinning height transitions to the mixed side. The red/black follow-up reviewed
the all-or-nothing scheduling rejection. In the latest review Claude proposed
splitting ramps at nominal planes to constrain scheduling. That option was not
adopted because it breaks the requested uninterrupted extrusion. The lower-end
schedule retains non-planar ramps and explicitly leaves full nozzle clearance
unverified. Model agreement is not print validation.

The island follow-up reviewed the loss of internal color boundaries, pure-edge
height pinning, infill duplication, skin coverage, and tongue volume. It found
an actual multi-tool infill duplication bug, which was fixed. The final change
regenerates both perimeters and enabled infill per physical island; it also
preserves solid skin coverage across each cycle. Tongue coverage and per-point
support were checked independently in tests, rather than assuming the polygon
mask alone proves deposited coverage.

The split-component follow-up reviewed repeated physical slots, closing-cycle
consistency, ineligible recipe influence and contact with unowned physical colors.
Those concerns informed the slot-table selection, repeated-anchor exclusion,
geometry guard and regression fixtures. Numerical cycle bounds were checked
independently; the implemented 0.48 mm stack and exported continuity were verified
with the recovered project, rather than relying on model agreement.

The local-merge/performance follow-up in the same Claude session reviewed the
capped interfaces, optional mask ownership, omitted-tip volume and scheduling.
It identified a missing tip allowance in travel ceilings, which was corrected,
and unnecessary minimum-height constraints on pure-region targets. Numeric
claims and performance were checked independently. Existing multi-interval,
foreign-neighbor and physical-print-validation limitations still apply.

The multicolor follow-up proposed the normalized common stack and extra-color
thickness fields. Independent checks corrected the worst-case bound to the
sum of individual maxima. Review also identified a real interaction between
normalized heights and same-color merging; the cap now uses physical plateau
thickness, covered by a regression test. Strict extra-pass scheduling was
added, and failed normalization no longer partially rewrites input zones.
The remaining conservative preflight limitation is documented above. Agreement
with Claude is not physical print validation.

The compact-transition follow-up reviewed carrier ordering, repeated-slot
capacity and the pure-layer intrusion. Claude confirmed the initial swelling
was caused by distributing disappearing extra volume into every common color.
The implementation also preserves a mixed-only reference for incoming pure
layers, covering both extra retreat and nominal alignment of other interfaces.
The existing 0.01 mm tongue exception is retained; the suggested strict-minimum
alternative would keep a thickness step and was not adopted for this experiment.
Source arguments were checked with per-column tests and recovered-project exports.

## Starry Night vase diagnosis (2026-10-23)

The saved `STARRY NIGHT PRINT.3mf` mesh was sliced with SML, full-domain
subdivision and Local-Z infill enabled to match the supplied settings screenshot.
Direct multicolor, independent heights, an unsplit first layer and the 0.20 mm
gradient height were also enabled. Other settings and recipes came from the
saved archive, so this is not an exact reproduction of any unsaved recipe edits.

At 0.08 mm nominal layers and a 0.06 mm component minimum, preflight selected
one eligible recipe with a three-interval cycle. The object-wide geometry check
then rejected layer index 2 at Z = 0.36 mm against reference layer index 1.
The resulting 1,257-layer plan contained zero bridge passes. Enabling full-domain
subdivision did not change that result. Three gradient recipes in the saved
palette were separately excluded before the geometry check.

The multi-interval implementation currently samples one cross-section per cycle,
then requires effectively identical cross-sections across the entire object
(0.00001 mm containment tolerance). Any change disables all multi-interval
joins. Supporting this vase requires geometry and ownership checks for each
candidate island over its own Z span, with paths following the changing outline;
removing the global check alone would leave frozen paths on a changing surface.
The complete-run ownership fallback and gradient recipe exclusion are additional
constraints to address independently.

This diagnosis adds rejection logging and the opt-in `[MixBridgeArchive]`
reproduction test; it does not change join eligibility. The reproduction uses
`ORCA_BRIDGE_ARCHIVE`, `ORCA_BRIDGE_ARCHIVE_ENABLE_SML=1` and
`ORCA_BRIDGE_ARCHIVE_FULL_DOMAIN=1`. Logs are under
`sandboxes/local-z-mix-bridges/starry-diagnosis/`. The archive slice completed and
all 20 `[MixBridge]` regression cases passed (242,218 assertions).


### Curved-model ownership and local fallback (24 October 2026)

The whole-object unchanged-outline veto has been removed. Candidate islands are planned before independent cadence, over short nominal-layer cycles. Their reference masks are clipped to the real model and contracted against their own support across the cycle, including the preceding layer. Unsupported fragments remain outside bridge ownership instead of disabling every occurrence of a recipe. Source masks stay aggregated, avoiding a combinatorial search over the vase's many small painted fragments.

Per-layer footprints follow bounded outline changes (a quarter nozzle diameter). Swept foreign paint is excluded before rebuilding candidate islands. Simple shared stacks and compatible common-anchor stacks can pin their interfaces to their scheduled heights at flat fallback boundaries. Pinning preserves bead bounds; joined pure regions must retain their nominal planes. Extra-color, tongue and repeated-bead cases retain conservative checks where that guarantee is unavailable.

Gradient recipes use the existing active-pair sampler within each cycle. Original cadence closes at ownership handovers and abrupt geometry changes; smooth changes and successive cycles with unchanged ownership do not repeatedly reset it. A short residual bead at a handover is absorbed or rebalanced within printable height bounds.

The saved `STARRY NIGHT PRINT.3mf`, with SML/full-domain/infill enabled, previously produced zero joining passes. The repaired planner produces 542 joining passes across four recipe sources in that saved project. The user's unsaved recipe edits were not available for this reproduction. The recovered constant-section CMY reference retains byte-identical exported wall paths.

Regression coverage includes sloped walls, a ledge with partial fallback and continuous vertical coverage, an unrelated disappearing island, cycle-sampled gradients, and shared-stack boundary height/slope bounds. The headless fixture now initializes its plate origin explicitly, matching application setup. Physical print validation remains outstanding.


The actual vase path audit found 7,423 generated perimeter entities, including **1,371 closed perimeter paths crossing recipe boundaries**. Its 241 MB G-code export contains 145,030 non-planar object extrusion segments; the measured segment heights are 0.0624?0.179748 mm, with no non-finite coordinates. All 23 bridge regression cases pass (251,736 assertions). The final boundary clearance check also covers per-layer outline extensions, not only the reference footprint. Flat bounded passes bypass spatial raster checks, and repeated paint unions/ownership offsets are reused or skipped where unnecessary.

The saved vase still exercises conservative fallbacks; this is not a promise that every color patch will join. The headless full export reports Arachne/Voronoi fallback warnings on small regions and the existing multicolor support-analysis warning, but completes successfully. The checked output is for the saved recipes plus the displayed SML switches, not the user's unsaved recipe edits.

The final saved-vase archive import plus slice took 119.2 seconds, down from 150.8 seconds before reusing/skipping redundant geometry operations, with the same 542 joining passes. This remains additional work compared with the old planner rejecting all curved-model bridges.

### Textured Tyranid and incomplete cycles (24 October 2026)

The saved `1_Meshy_AI_tyranid_alien_predato_1016020716_image-to-3d-texture.3mf` already enables SML, independent heights, direct multicolor and full-domain subdivision. Its 23 recipes pass preflight with a five-interval cycle, but its 747 intervals after the first layer cannot be tiled completely within the recipes' height bounds. The old exact-tiling requirement therefore produced zero joining passes, before any local geometry checks ran.

For changing cross-sections, cycle selection now maximizes the number of joined intervals, then minimizes excess cycle length. A short unfillable remainder retains ordinary Local-Z cadence, with ownership handing over on a nominal plane. Complete tiling remains preferred when possible; fixed-section planning is unchanged. A short final fallback can have a different color balance because it cannot fit every recipe component.

With the saved settings unchanged, the repaired model produces 288 joining passes involving 19 recipe sources. The path audit finds 1,394 perimeter entities, including 254 closed paths crossing recipe boundaries. Unsupported fragments still use local fallback. Import, slicing and the path audit took 52.0 seconds on this machine; this includes actual island construction that the previous planner skipped.

The added regression forces an unfillable final interval and verifies that lower cycles still join, the final interval retains fallback coverage, bridge ownership stops before it, and sampled columns have no gaps or overlapping height intervals through the handover. All 23 bridge cases pass with 251,974 assertions. Reproduction artifacts are in `sandboxes/local-z-mix-bridges/tyranid-diagnosis/`.

Full processing and export completes in 91.4 seconds with a 73.3 MB G-code file. Its 6,670 non-planar object extrusion segments have reported heights from 0.064265 to 0.279226 mm and finite coordinates. Small-region Voronoi fallback diagnostics and the existing multicolor support-analysis warning remain. The separately built runtime is `build-dev-release/mix-bridges-partial-cycles/snapmaker-orca.exe`; its startup check succeeds. The original archive is unchanged.

### Volume ownership correction (24 October 2026)

The user confirmed the same saved Starry Night archive. With the displayed SML/full-domain/infill switches enabled, an audit of exported outer walls between Z=60 and 65 mm found 1,018 pairs of near-coincident, different-tool segments with more than 0.015 mm of overlapping vertical bead extent. Of these, 850 involved two ordinary Local-Z passes and 168 involved an ordinary pass and a joined pass. The previous connection counts therefore did not establish correct extrusion volumes.

A bead may span several nominal layers. Expanding a joined island separately on each scheduling layer, or letting an ordinary thick bead follow a different painted mask at each nominal height, assigned the same volume to different colors. Joined footprints now use the common geometry over their complete cycle. Full-domain independent masks no longer receive independent 0.10 mm expansions at recipe boundaries.

For curved full-domain objects using direct multicolor and independent heights, the remaining recipe domains are partitioned into short blocks. A stable core keeps one mask through the block and receives bounded flat layers. The changing remainder uses nominal-height paths clipped from the original layer, avoiding standalone perimeters that cannot fit inside thin strips. Core and remainder masks are disjoint, and both are reserved against ordinary/base extrusion. Color-height error carries across blocks rather than resetting to the first component; the bounded allocator also checks that its remainder can close within every bead's minimum/maximum height limits. This branch requires nominal layers at least as thick as the configured component minimum.

Opening stable cores removes features too narrow for their own walls, returning them to nominal paths. Sub-micron-area integer remnants do not become standalone islands. These are feature-resolution decisions, not permission to discard a printable region. Core support is checked against the preceding nominal layer. First-layer and constant-section bridge behavior remain under their existing guards.

The audit examines near-coincident outer-wall centerlines and their reported Z/height spans; it is not an exhaustive swept-nozzle or swept-bead collision proof. Physical print validation remains outstanding. The nominal bands can occupy a meaningful part of a highly textured curved model and retain local color cadence rather than joining every patch. Generating and checking this geometry costs more than rejecting the curved model's joins entirely.

A second export-only defect was isolated to nominal layer 805 (Z=64.60 mm) in Starry Night. The base-path clipper followed its mask, but redundant unions of coincident pass masks changed the exclusion topology. The captured old mask let 403.106 mm of ordinary wall paths through. Bridge ownership is now added only once, and independent managed masks are not rebuilt from their constituent passes. The corrected capture contains no ordinary wall paths on that fully managed layer. A temporary per-layer capture was removed from the shipping source; optional pass trace comments and containment diagnostics remain behind `ORCA_LOCAL_Z_TRACE_PASSES`.

The corrected Starry Night export has 1,279 joining passes, 5,326 closed perimeter paths crossing recipe boundaries, and 335,899 non-planar extrusion segments. Its original Z=60-65 mm audit now detects zero qualifying overlaps, down from 1,018. A separate full-height scan of 2,032,852 outer-wall segments also detects zero. The audit filters out segments shorter than 0.008 mm and checks nearly parallel, different-tool centerlines within 0.008 mm XY with more than 0.015 mm overlapping vertical extent. Its Z-binned full-height variant preserves the same detection intent; neither is an exhaustive bead-width/nozzle-clearance proof.

All 24 bridge regression cases pass (306,468 assertions), including the bounded color allocator and curved core/band partition checks. Release compilation and a hidden `--help` startup check pass. The final Starry Night import and slice took approximately 255 seconds on this machine; full processing/export took approximately 530 seconds while other checks ran. These are not controlled performance benchmarks, but this correction adds work compared with the previous curved planner. Its 341,823,186-byte export is larger than the earlier 241 MB version. Performance remains a tradeoff, not a claimed improvement.

Reproduction logs, exported G-code, clipping captures, and the two audit scripts are retained under `sandboxes/local-z-mix-bridges/overlap-fix/`. The saved Starry Night archive and its sandbox copy have matching SHA256 `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2`. Claude session `9b547d4c-cb82-44c4-993f-629c93cdd277` reviewed ownership, nominal fallback, color allocation and clipping hypotheses. The geometry and export findings above were checked independently.

The final Tyranid export, with its saved settings unchanged, contains 567 joining passes and 312 closed cross-recipe paths. Its full-height audit checks 447,924 outer-wall segments and detects zero qualifying overlaps. The export completes with 1,966,872 processed moves and 9,404 non-planar extrusion segments (84,199,883 bytes). An earlier run of the same volume-partition approach produced 564 passes / 300 closed cross-recipe paths and also passed a full-height audit; the slight fragment-count variation has not been separately diagnosed. The checked export counts are observations, not a guarantee of identical fragmentation on every run.

The runnable build is `build-dev-release/mix-bridges-volume-ownership/snapmaker-orca.exe`, with mix bridges enabled by default. `BUILD.txt` beside it records the source/runtime hashes and validation scope. Prior runtime folders and the source archives were retained.

### Cylinder residuals and pure-boundary pinning (24 October 2026)

The saved TestCylinder project exposed a numerical failure on rounded outlines. Boolean differences left micron-wide unpainted ribbons that the transition distance field treated as real pure-cyan regions. Those false targets changed the magenta interface and introduced extra cyan tongues along otherwise constant-color plateaus. The end sections also failed clearance checks and switched to block fallback, producing visibly different bands.

Pure-region distance indexes now ignore residual features below approximately 0.01 mm width, while exact ownership masks remain intact. Cleaning ownership itself would create holes and trigger more fallback; the distinction is covered by a regression with noisy pure-region masks. Geometric clearance checks close sub-resolution cracks before offsetting the owned footprint and remove numerical residual obstacles. Explicit foreign-paint checks remain in force.

Benchy also exposed an unnecessarily strict boundary pin: a joined pure region had to match the minimum mixed-recipe interface exactly. Shared stacks without optional tongues, extra-color islands, or split runs can now pin to the joined pure region's nominal planes. Component scheduling uses those planes. The interior may lie below its scheduling key, but the foreign-material clearance band is flat at that key and the component passes remain ordered. Tests verify pure-plane preservation, distant plateau preservation, slopes, ordered coverage, and bead-height bounds.

The cylinder has 184 three-interval cycles after its preserved first layer. All now join, including the top cycles; none requires block-core fallback. Reproduction files and before/after radial G-code profiles are in `sandboxes/local-z-mix-bridges/cylinder-benchy/`. Original archives and previous runnable builds are retained.

The final saved-cylinder export has 736 joining passes and 1,065 closed paths crossing recipe boundaries. Benchy has 897 joining passes across all 299 candidate cycles and 2,233 closed cross-recipe paths (baseline 308). Moving edge bands still retain nominal fallback. Full-height centerline audits examine 381,902 cylinder and 155,544 Benchy outer-wall segments and detect zero qualifying overlaps under the audit limits described above. Coordinates and heights are finite. All 26 bridge cases pass with 315,432 assertions; release compilation and hidden startup succeed. No physical print validation was performed.

The runnable build is `build-dev-release/mix-bridges-cylinder-benchy/snapmaker-orca.exe`, with bridges enabled by default. `BUILD.txt` records source/runtime hashes, final export counts, and audit scope. The final exports are named `*-release.gcode`; earlier intermediate exports remain for diagnosis.


### Review corrections (24 October 2026)

Rejected sparse joins now rebuild from retained authored recipes, before common-carrier normalization. An empty isolated result leaves that region to the ordinary planner rather than indexing an empty vector. This fixes loss of extra colors such as Y when a CM/CMY join is rejected.

Both stable cores and moving edge strips use the bounded physical-tool allocator. A nominal 0.20 mm strip with a 0.12 mm component limit becomes multiple valid beads. Color debt is accumulated in footprint area times height, so a sequence of wide and narrow strips tracks volume rather than counting nominal layers equally. The preserved first layer and 0.01 mm merging tips remain explicit exceptions to ordinary component-height rules.

Shared perimeters keep compatible wall settings. Each fill collection now retains its source region, including infill modifiers and the export settings used for speed and flow. Cycle skin geometry is cached once per Z range before parallel fill generation. Solid coverage is conservative over the cycle, while top/bottom roles are assigned only to the terminal/initial bead at the corresponding exterior plane. Turning Local-Z infill on or off explicitly invalidates cached fills, including projects with density modifiers.

The joined-pure distance field reserves a nozzle guard band before beginning the ramp. Tongue sampling includes that band so the taper can close completely. Pure regions that retain nominal layers without joining are also included in boundary pinning and clearance rejection. Passes are sorted again after pinning changes their scheduling heights. The guard uses the largest physical nozzle radius plus 0.10 mm and a quarter-nozzle allowance for changing geometry. This is a conservative heuristic, not a nozzle-body model.

Non-planar repositioning now lifts vertically above the deposited ceiling, traverses in XY, then descends at the next path start. This applies without configured retraction or Z hop, and avoids lazy/spiral diagonal descent through an earlier pass. A 2D wipe path is discarded for modulated paths. Tower moves use the planned tower height independently of travel clearance. Bridge pass ceilings are checked against generated extrusion Z. The stored ceiling resets after transfer to the next sequential object, preventing needless full-object-height hops throughout the next print.

Regression coverage includes rejected sparse recipes, unequal infill modifiers, bounded moving strips, area-weighted color debt, both directions of incremental infill toggling, joined/unjoined pure guards, project-level disable, repeat slicing, travel with zero hop and spiral hop/wipe, and preview with a printer Z offset. The parser already removes the printer offset before assigning preview bands; no extra offset adjustment was needed. Preview bands remain a spatial grid based on the configured nominal height, not a reconstruction of each object's variable-height slicing planes.

The travel replay checks crossings of previously deposited centerlines. Existing stack tests check complementary volume, ordered surfaces and heights. Neither proves nozzle-body clearance or validates actual extrusion at taper tips. Pressure/flow calibration, overhang behavior, and physical print quality still require printer experiments. The earlier Tyranid fragment-count difference is not explained by a deterministic synthetic fixture alone. Historical model-export counts above should not be reused as validation of these new changes.

Validation of the review corrections: **67 Local-Z/GCodeProcessor cases pass (560,194 assertions)**. The broader 200-case run reproduces the same unrelated mixed-filament remapping failures as the preceding runtime. Release library, GUI, DLL and launcher builds succeed; hidden `--help` startup exits 0. The new runtime is `build-dev-release/localz-review-fixes/snapmaker-orca.exe`; `BUILD.txt` records source and binary hashes.

Unmodified saved Cylinder and Benchy projects both slice and export. Cylinder retains 736 joining passes and has 1,239 closed cross-recipe paths; Benchy retains 897 joining passes and 2,233 such paths. Full-height overlap scans find zero qualifying overlaps in 290,114 and 144,318 outer-wall segments. Full-height phase-b travel scans find zero crossings inside reported beads across 14,273 and 29,574 travels respectively (652,000 and 474,096 extrusion segments). The same centerline limitations apply; arbitrary custom toolchange motion is outside this guard's guarantees. Exports, logs and reproducible audit scripts are in `sandboxes/local-z-mix-bridges/review-fixes/`. Claude conversation `f279e363-023c-45fa-9134-f1fdebe80a51` reviewed the implementation; its findings were checked against source and regressions.

### Superseded nominal-shell workaround (24 October 2026)

The subsequent Benchy screenshots exposed a coverage defect that the overlap and travel checks did not measure. A cycle's common footprint can sit inside a growing hull. Its remaining nominal band could be narrower than half a wall, so clipping the original wall centerline against that band produced no wall there. Nevertheless, ownership excluded the original wall. The visible surface then came from the inset island, with fragmented surface colors where a band became wide enough to intercept a path. On the saved Benchy, 401 of 441 bands below 12 mm missed the estimated nominal external centerline entirely.

Changing portions of a cycle's outline now reserve a shell one external-wall width inward from the swept contour. Both joined islands and fallback cores exclude that reservation. Stable portions retain their joined nonplanar walls. The moving bands generate fitted walls and fills from their masks, superseding the earlier source-centerline clipping described above. This also avoids clipping old inner walls across the new partition and closes the ends where moving and stationary contours meet. Nominal bands with identical physical stacks and compatible wall settings merge before path generation, allowing continuous extrusion across their paint boundaries. Infill modifiers retain their existing source-region handling.

The moving shell uses nominal-layer color cadence; it does not promise a continuous nonplanar path through every curved patch. The preserved first layer remains unchanged. Color debt is still volume-weighted per recipe, so arbitrary disconnected fragments do not have an independently guaranteed optical color ratio. Corners, seams, incompatible schedules and topology changes can still require separate paths.

Regression coverage now includes expanding and narrowing striped hulls: every nominal layer has its visible wall at the intended surface, both stripe ratios remain within six percentage points of their targets, both tools have uninterrupted shell paths across compatible paint boundaries, and the bead footprint covers the ends adjoining a stationary side. All 67 Local-Z/GCodeProcessor cases pass with 575,450 assertions. `ORCA_BRIDGE_ARCHIVE_DUMP` optionally exports slice masks, per-pass ownership and generated wall paths for archive diagnosis.

The unchanged saved Benchy exports 897 joining passes, 1,130 fallback passes, 2,394 closed cross-recipe paths and 31,793 nonplanar extrusion segments. Cylinder retains 736 joining passes, 1,239 closed cross-recipe paths and 75,920 nonplanar extrusion segments. The Benchy full-height audit finds zero qualifying overlaps in 201,936 outer-wall segments and zero prior-centerline bead crossings in 31,842 phase-B travels. Cylinder's corresponding scans check 290,114 outer-wall segments and 14,269 travels with zero detections. These retain the centerline limits documented above.

An additional sampled contour check approximates beads by their reported width and height. On five lower-hull planes from Z=0.56 to 8.16 mm, remaining uncovered contour lengths are 0.19-0.62 mm out of 98-170 mm, versus 98-134 mm before the correction. This measures improved surface placement, not removal of an equivalent length of empty wall: the earlier inset wall often still contained material. The check closes sub-0.01 mm segmentation cracks, probes 0.03 mm inside the outline and allows 0.005 mm bead-radius tolerance. It is approximate; seams and corners higher on Benchy still produce residuals. It is not a full swept-bead or physical-print proof.

The separate runnable build is `build-dev-release/localz-benchy-shell/snapmaker-orca.exe`. Release compilation, linking and hidden `--help` startup pass. Reproduction G-code, scripts, logs, Claude reviews and the hash manifest are in `sandboxes/local-z-mix-bridges/benchy-shell/`. The same Claude conversation independently identified the thin-band mechanism and reviewed shell-end and overlap risks; implementation and validation were performed locally. No physical print was performed during this correction.


### Shared exterior contours and bead coalescing (25 October 2026)

The nominal-shell workaround above did not solve continuous Local-Z printing on the visible hull. Each recipe retained an independent color-debt schedule; exact-stack merging only joined coincident schedules. This produced zipper seams, long single-color runs and consecutive thin beads of one color. Aggregate path counts, whole-object color averages and a test requiring each tool to cross the shell only once were insufficient acceptance criteria.

Accepted curved cycles now retain the common color interfaces through the exterior. Each physical pass receives a cross-section at its local bead midpoint, from a signed-distance interpolation between nominal slice outlines. The per-layer ownership used to suppress fallback stays distinct from those pass contours. Walls and infill are generated from the fitted contours, and extrusion height follows the shared interfaces. Non-planar scheduling, source-region settings, first-layer preservation and deposited-height travel clearance remain in force.

The contour sampler also had a row-coverage defect: scanning only a grid row's midpoint omitted boundary strips shorter than half a grid step. A 0.22 mm band on the 0.05 mm grid lost its final 0.02 mm row, separating its moving contour from the interior. Boundary-edge spans now cover partial rows and thin features between scanlines. The 45-degree hull regression fails before this correction and passes afterward. Sampled taper boundaries are capped at their shared nominal interface. Zero-volume portions of optional taper paths are omitted, preventing a sampled edge outside the exact taper from becoming a spurious extrusion above its travel ceiling.

Thicker beads must fit within a quarter-nozzle lateral contour error. Mixed beads require support within half a nozzle width, including the slice below a cycle. Missing and excess geometry are checked at three heights in every nominal slab, including components that disappear completely. Ordinary-height pure regions retain their nominal overhang behavior. Failed regions are expanded by a nozzle-clearance guard and removed locally before rebuilding and rechecking the remaining island. Retry count is bounded; unjoinable geometry retains fallback. Independent cycles are validated in parallel with separate outputs and a serial merge of selected sources.

Where a shared join is unavailable, compatible recipes first retain their exact component heights in surface-following flat stacks. Their tool order follows a compatible joined stack, avoiding a phase reversal at the handover. The union of every foreign owner across the cycle is excluded before fitting, so these stacks can move only at the model exterior. Residual geometry still uses bounded core/band fallback.

The saved Benchy exposed another source of adjacent same-color beads: a pure neighbor being removed by local clipping reversed the mixed stack from cyan/magenta to magenta/cyan. Equal-material stacks now use one object-wide terminal preference through initial construction and every rebuild. The preference ranks physical colors by their contact with mixed regions across candidate cycles, including unpainted base regions; a distant pure body does not influence the ranking. Sparse carriers use the same preference subject to capacity constraints. A pure neighbor with another color keeps its nominal ownership and clearance checks. Sparse capacity changes and anchor-mode transitions can still require different orders; this is not an unconditional merge across every possible recipe change.

The exact-recipe fallback also removes rejected geometry locally and retries within a bounded nozzle guard. A small disappearing fragment therefore need not return the full recipe on a smooth hull to nominal-layer color dithering.

Consecutive fallback strips of the same tool are coalesced when their combined height fits the tool limit, source sets and region settings agree, and swept ownership cannot enter a neighbor's volume at any covered layer. The new bead is fitted and checked before replacing every covered layer's owner. This is conservative: a topology change, incompatible settings or changing internal ownership may still require separate beads. It does not promise an arbitrary same-color pair can merge merely because its height is below the cap.

Regression checks inspect every shared hull cycle and both tools, verify emitted midpoint placement, require a complete exterior stack without gaps or overlapping height intervals, reject unnecessary consecutive same-tool beads on the supported hull, and measure integrated plateau color ratios. Fixtures include expanding and narrowing hulls at two slopes, a ledge exactly at a cycle boundary, a vanishing component, a closing hole, and a separate painted island that ends while the hull continues.

Additional phase regressions alternate pure neighbors around an unchanged pair of recipes and verify preserved local thickness and color order. A full slicing/export fixture checks that an unpainted touching pure region still joins when another, larger pure body is distant. Dense taper-column checks require complementary height intervals and prohibit extrusion above optional-pass ceilings.

Stationary contours retain their previous planner path. Micron-scale differences between rounded circular slices, including tiny remnants left by integer polygon opening, must not classify a cylinder as a changing hull. This is covered by a sub-resolution contour-drift fixture and the saved cylinder's 184 accepted cycles.

Artifacts for this work are under `sandboxes/local-z-mix-bridges/shared-shell/`; the separate runtime is `build-dev-release/localz-shared-shell/snapmaker-orca.exe`. The side-profile audit includes both outer and overhang wall roles and clips line intersections rather than discarding segments whose endpoints lie outside the viewport. It measures projected emitted beads, not preview seam-marker meshes. Centerline overlap and travel audits are still approximate and do not establish swept-nozzle clearance or physical extrusion quality. Claude session `f279e363-023c-45fa-9134-f1fdebe80a51` reviewed the design, support/topology risks and coalescing; the grid-row defect was isolated locally and verified by regression. Physical print validation remains outstanding.

Final validation uses the `*-validated.gcode` exports and matching JSON audits, not the intermediate exports retained alongside them. All 70 Local-Z/GCodeProcessor cases pass with 578,136 assertions. Release compilation/linking, `git diff --check`, and a hidden startup check pass; mix bridges remain enabled by default. `BUILD.txt` records source, runtime, archive and export hashes.

On the saved Benchy, the four lower-hull probes at local X=-10, -4, 0 and 6 mm, sampled from Z=0.201 to 9.999 mm, detect 1, 0, 0 and 0 adjacent same-color pairs, compared with 38, 28, 23 and 28 in the nominal-shell workaround. No sampled height is missing. The remaining pair near X=-10, Z=9.96 mm straddles a sharp contour change: the wall centerlines differ by approximately 0.256 mm in Y. The planner retains separate beads at this fallback boundary. These four probes are a focused regression measurement, not an exhaustive claim that every same-color adjacency on every surface has been eliminated.

Benchy retains joined stacks in 292 of 299 candidate cycles; the cylinder retains all 184 cycles. Benchy exports 868,167 processed moves and 40,182 nonplanar extrusion segments; the cylinder exports 750,288 and 75,909 respectively. The full-height different-tool centerline audits detect zero qualifying overlaps among 177,198 Benchy and 289,757 cylinder outer/overhang-wall segments. Travel audits detect zero qualifying prior-bead crossings in 24,188 Benchy and 14,229 cylinder phase-B XY travels, checking against 481,380 and 651,574 prior extrusion segments respectively. The numerical tolerances and finite-width/nozzle limitations of these audits remain as documented above.

### Tyranid archive, fragmented paint and touching holes (25 October 2026)

The user-supplied `1_Meshy_AI_tyranid_alien_predato_1016020716_image-to-3d-texture.3mf` has 748 nominal layers, four physical colors and 29 enabled mixed recipe definitions. Its saved settings use 0.08 mm layers, a 0.20 mm first layer, 0.06–0.32 mm component bounds, classic walls and disabled thin-wall detection. The original archive is retained unchanged; its SHA-256 is `f682d3b7e00814942d8a400afc5347393f38934e2eaf792a01ffc09ff8d71f13`.

The initial reproduction failed during perimeter generation on an ownerless polygon of approximately 0.000011 mm² at Z=17.8 mm. Integer erosion itself retained a degenerate remnant, defeating the previous emptiness check. Ownerless remnants below 0.0001 mm² now also have an independent area guard. Larger unowned islands still fail explicitly; this does not silently reassign printable geometry to another region.

Stack sampling identified repeated full-band clipping in `sample_region`: each 0.05 mm scan row rebuilt Clipper's edge table. All independent horizontal scans now use one clipping sweep, retaining the grid resolution, boundary-edge spans and partial-row coverage. Regressions include 400 disconnected fragments, partial last rows and horizontal boundaries exactly on a scanline. The first repaired Tyranid export completed in approximately 302 seconds; the initial failure took approximately 810 seconds. These are local elapsed measurements, not controlled benchmarks.

The first successful export exposed 15 near-coincident different-tool wall overlaps. A reduced three-polygon fixture shows that a hole touching its contour at integer precision can be filled by a non-zero union when neighboring recipe masks are combined. Total area alone does not detect the topology change. Local-Z ownership, pass domains and zone distance geometry now use positive winding for these unions. Both recipe orders are tested. A separate captured fixture reproduces an offset-generated perimeter loop outside its input mask, inside another owner's hole. Generated paths are clipped back to ownership when they escape the 0.005 mm tolerance by more than 0.01 mm of path length; fill areas are also intersected with ownership. Valid complete loops remain intact.

Coverage inspection found another problem that overlap checks cannot detect: a small painted patch can be unprintable as its own closed island even when the surrounding model wall is wide enough. Compatible nominal fallback bands now generate one perimeter outline before splitting its paths by physical color. Only single beads of nominal height with equal ceilings, equal nozzle diameters and identical source-region settings after normalizing filament IDs may share this outline. Fitted nonplanar stacks and coalesced multi-layer strips keep their own geometry. Shared fill boundaries are intersected with each color owner, and centerlines on exact paint boundaries are assigned once in a stable order. The project’s wall generator and thin-wall setting are preserved. Regressions compare total path length, deposited volume, fill coverage and ownership for a sub-nozzle stripe and an exact centerline boundary.

Further coverage probes at Z=34.56 and 50.56 mm exposed unprinted fixed fallback cores. Opening the candidate core before support clipping and moving-shell subtraction did not ensure the remaining core could carry a wall. Captured 0.197 and 0.117 mm² remnants owned volume but emitted no perimeter. Core opening now runs after all trimming and uses the largest configured external-wall width among its physical tools and source regions, rather than only the minimum nozzle diameter. Rejected area returns to nominal bands. Regressions cover both trimming operations, a strip wider than the nozzle but narrower than the configured wall, preservation of a printable neighbor, and disjoint, complete core/band ownership.

A planar diagnostic export provides a comparison for tiny isolated spikes and fine outline details. Its surface-offset handling differs from Local-Z, so aggregate contour residuals are not interchangeable; point probes use the same physical XY and Z. The contour audit approximates beads with their reported widths and heights, samples nominal slab midpoints and probes an inset contour. It does not establish physical extrusion quality, optical color accuracy, support suitability or swept-nozzle clearance. The original project settings and source mesh are not altered by this diagnostic.

Reproduction exports, geometry dumps, audit scripts and before/after fixtures are in `sandboxes/local-z-mix-bridges/tyranid-shared-shell/`. Claude conversation `f279e363-023c-45fa-9134-f1fdebe80a51` reviewed the changes and identified the need to use configured wall width in the final core filter. Its proposed sparse-to-exact-fallback phase mismatch was not established on this archive: the examined CMY exact stacks already followed the joined order. The measured winding, small-patch and post-trim core failures were isolated locally and tested independently.

Final results for this correction use only the `*-verified.gcode` exports. All **75 Local-Z/GCodeProcessor cases pass (579,033 assertions)**. Release library, DLL and launcher builds, `git diff --check`, and hidden `--help` startup pass. The runnable build is `build-dev-release/localz-tyranid/snapmaker-orca.exe`, with source, runtime and export hashes in `BUILD.json`. No physical print was performed.

The unchanged Tyranid archive exports 6,177 passes, including 315 joining passes; its 29,515 island paths include 4,412 closed paths crossing recipe boundaries. The export contains 2,318,417 processed moves and 7,532 nonplanar extrusion segments. The geometry audit finds no nonfinite coordinates or nonpositive reported dimensions; nonplanar heights range from 0.063247 to 0.183744 mm. All three reproduced missing-color probes at Z=8.56, 34.56 and 50.56 mm are covered by the reported bead footprints. Of 30 fixed physical probes selected from the initial export's contour residuals, 27 improve by more than 0.01 mm and none worsen by that amount. This sampled comparison is not proof of complete coverage of all fine mesh features. `verified-patches.png` shows the three targeted corrections.

Tyranid's full-height wall audit checks 704,066 segments and retains **three qualifying near-coincident pairs**, all at one nominal height per pair. Replaying each captured common domain through the same classic generator before color partitioning reproduces both legs of each pair in one thin closed loop, within 0.002 mm at the exported endpoints. They are not duplicate paths introduced by color splitting; they remain narrow-loop overlap behavior in the generated classic walls. The flags are retained in the audit and recorded in `verified-unpartitioned-matches.json`, not suppressed. Fine features and these three loops remain limitations of this build. The travel audit checks 223,611 phase-B XY travels against 963,831 prior extrusion segments and detects zero crossings inside reported beads.

Benchy retains 292 joined cycles and the cylinder all 184. Their new exports contain 872,158 and 750,146 processed moves, respectively, with 40,180 and 75,844 nonplanar segments. Full-height wall audits detect zero qualifying overlaps among 177,624 Benchy and 289,685 cylinder segments. Travel audits detect zero prior-bead crossings across 24,886 and 14,226 phase-B travels, respectively. Benchy's four lower-hull probes still find 1, 0, 0 and 0 adjacent same-color pairs and no missing height samples; the remaining pair is the previously documented sharp-contour fallback. These checks preserve the earlier shell and thicker-bead correction without claiming arbitrary surfaces can always be joined.

### Exact-stack and nominal-band cadence (25 October 2026)

The next Tyranid screenshots exposed a limitation missed by coverage and collision checks: adjoining parts of the same recipe could use unrelated color sequences. The thick magenta itself is expected in Local-Z. A saved 67% magenta / 33% cyan recipe in a 0.4 mm cycle produces a 0.268 mm magenta bead and a 0.132 mm cyan bead; the 66/17/17 CMY recipe produces 0.264/0.068/0.068 mm. In the preceding export, 534 of 565 thick-magenta plan rows came from separate surface-following exact fallback stacks and 31 from fixed fallback cores. This explains why the exact-stack/band handover dominates the visible patchwork even when the reported volume is covered.

A geometric audit selected 41 same-recipe exact-stack/nominal-band pairs with stable adjoining footprints in seven sampled cycles. One 60% magenta / 40% yellow stack used yellow then magenta, while its nominal band used magenta, magenta, yellow, magenta, yellow in five 0.08 mm slabs. The two columns agreed on material for only 20% of the cycle despite having identical overall proportions. Thus a correct average color ratio does not establish a coherent interface.

When an exact fallback stack is accepted, its remaining single-bead nominal bands now follow that stack's tool order. Slab heights, masks and allocated tool-slot counts remain unchanged. Reassigning a tool to a differently sized footprint credits the removed volume and debits the added volume in the existing per-recipe error account. Reordering is rejected atomically if it would exceed a tool's height cap or if the exact stack requires interleaved runs. A row bordering an already joined stack with a conflicting order retains its existing cadence. Alignment happens before same-layer grouping and thick-bead coalescing, so those operations see the final tool identities.

The regression checks ordering, unchanged slab heights, exact per-color volume-debt accounting, rejection without mutation, and long-run ratios across constant, alternating 95/5-area, and steadily growing footprints. This correction aligns color order; exact interfaces can still fall between nominal planes, and incompatible geometry still needs separate paths. It does not turn every fallback boundary into one continuous nonplanar extrusion.

Claude session `f279e363-023c-45fa-9134-f1fdebe80a51` reviewed the diagnosis and the bounded ordering correction. A broader proposal to pin all component interfaces to nominal planes was rejected: five 0.08 mm slots cannot represent a 66/17/17 recipe while retaining every component and its height limits. The current change preserves the original thick recipe beads instead of substituting that infeasible quantization.

Validation: **77 Local-Z/GCodeProcessor cases pass (636,057 assertions)**, with successful Release library/DLL/launcher builds and hidden startup. The new runtime is `build-dev-release/localz-tyranid-cadence/snapmaker-orca.exe`; artifacts are in `sandboxes/local-z-mix-bridges/tyranid-cadence/`. The previous runtime is retained separately.

At the same 41 physical band probes, same-material height agreement improves from a mean of 47.78% to 83.61%. Forty probes improve and one declines from 44% to 36% as the carried volume correction changes that cycle's tool allocation. None has missing or multiply owned height intervals in this mask-based check. This measures improved cadence, not a complete solution to local optical color or continuous path joining. The three previous missing-color probes remain covered, and none of the 30 earlier physical coverage probes regresses by more than 0.02 mm in the reported-bead approximation.

Tyranid exports 6,148 passes, including 315 joining passes, 28,656 island paths, 5,691 closed paths crossing recipe boundaries, 2,256,912 processed moves and 7,566 nonplanar segments. The geometry audit finds finite coordinates, positive reported dimensions and nonplanar heights from 0.063247 to 0.183702 mm. The full-height wall audit retains two qualifying overlaps among 697,122 segments; both reproduce in single thin loops of the common classic generator before color splitting. The travel replay detects zero prior-bead crossings in 211,926 phase-B XY travels against 956,322 extrusion segments. These are the same approximate centerline/reported-bead checks as before, not physical-print or swept-nozzle proofs.

Benchy and cylinder also export successfully. Their wall audits detect zero qualifying overlaps among 177,621 and 289,685 segments, and their travel audits detect zero crossings in 24,774 and 14,225 phase-B travels. All four Benchy lower-hull probes now have zero adjacent same-color pairs and zero missing samples. The remaining Tyranid cadence mismatch, nominal-plane quantization, separate fallback paths and classic thin-loop overlaps remain explicit limitations. No physical print or GUI preview verification was performed during this correction.


### Local minimum cadences within shared envelopes (25 October 2026)

The earlier explanation of thick magenta beads was conditional on the chosen 0.4 mm cycle. It did not justify that cycle for every recipe. The preflight maximizes how many recipes fit one object-wide envelope, so a 15% component at the 0.06 mm Local-Z minimum can enlarge a separate 40/60 recipe unnecessarily. The envelope remains a scheduling boundary; it no longer implies a single recipe occurrence everywhere inside it.

Independent surface-following exact fallback stacks now repeat their complete recipe as many times as all component minimums permit. For example, 0.16/0.24 mm becomes 0.08/0.12 mm twice; 0.132/0.268 becomes 0.066/0.134 twice. Total height per physical color is unchanged. A recipe with a 0.06 mm component cannot repeat. Maximum heights, supported contours, ownership and region compatibility are still checked. Stacks touching an existing joined group within one nozzle width retain that group's cadence. A sequence beginning and ending with the same tool is not repeated, avoiding new same-color seam beads.

A joined group can also replace its envelope with shorter cycles on nominal planes. The partition maximizes complete feasible cycles, then balances their heights; every connected recipe in the group shares that partition. Refinement is restricted to shared stacks without foreign neighbors, pure joins, extra-color islands, split runs, tongues or pinned interfaces. Each child rebuilds its fields from the authored recipe, retains the corresponding ownership footprints, and refits moving contours. Surface-following children require at least two sampled footprints. The entire replacement is discarded if any child fails; publication never mixes a partial replacement with the parent. Support below later children is conservatively restricted to the preceding owned footprint.

Nominal bands alongside repeated exact stacks use a minimum-cost assignment instead of sorting each tool into one run. The assignment maximizes area-weighted overlap with the actual repeated color intervals, respects real slab start heights (including missing slabs), preserves tool counts and slab heights, checks height caps, and adjusts carried per-color volume debt. This remains discrete color approximation, not an exact interface match or a new continuous connection. Unrepeated stacks retain the previous ordering correction.

Claude session `f279e363-023c-45fa-9134-f1fdebe80a51` reviewed the implementation read-only. The repeated-seam and too-short surface-child cases from that review have regression coverage. New tests also check optimum nominal assignment against exhaustive permutations, missing slots, atomic cap rejection, long-run volume debt on alternating footprints, disconnected groups with different limiting recipes, per-color height conservation and atomic child-fit rejection.

Validation: **82 Local-Z/GCodeProcessor cases pass (644,154 assertions)**. Release core, app DLL and launcher builds succeed; hidden `--help` startup exits 0. The runtime is `build-dev-release/localz-local-cycles/snapmaker-orca.exe`; final exports, comparison scripts, plot and audits are in `sandboxes/local-z-mix-bridges/local-cycles-final/`. The original downloaded archive retains SHA-256 `f682d3b7e00814942d8a400afc5347393f38934e2eaf792a01ffc09ff8d71f13`.

The final Tyranid export contains 163 matched exact stacks with finer repeats and no invalid exact-component heights. Each matched repeat preserves its total height per physical color. Magenta outer/overhang wall centerline length printed above 0.2 mm falls from 18,579.41 to 15,000.88 mm (19.26%); the maximum magenta height remains 0.28 mm where refinement is constrained. Estimated filament changes increase from 4,998 to 5,230, printing time from 32h27m25s to 33h35m31s, and total material from 114.42 to 115.73 g. These are slicer estimates, not measured print results; material includes the entire export, not an isolated purge-volume measurement.

The export has 6,568 passes (315 joining, 6,253 fallback), 30,862 island paths, 5,804 closed paths crossing recipe boundaries, 2,296,718 processed moves and 7,492 nonplanar extrusions. The reported-bead audit finds finite coordinates, positive dimensions and no out-of-bounds nonplanar heights. None of the 30 earlier fixed physical coverage probes worsens. The wall audit flags one pair among 705,749 segments: its endpoints, tools and heights exactly match the previously replayed common classic thin-loop overlap. Travel replay detects zero prior-bead crossings among 218,416 phase-B XY travels against 974,745 prior extrusions.

Of the previous boundary probes, 40 still have a comparable exact stack. Agreement with each export's own exact cadence averages 83.38% before and 83.80% after: three improve, four worsen, the rest are unchanged, and none has missing or ambiguous owned intervals. In particular, a 40/60 stack previously matching five nominal slots exactly can only match 80% after repeating inside the same five 0.08 mm slots. These residuals are an explicit quantization limitation; the change does not establish continuous paths across every fallback boundary.

Benchy and cylinder path dumps are byte-for-byte identical to the preceding cadence build. Fresh full-height wall audits find zero overlaps among 177,621 / 289,685 segments, and travel audits find zero crossings among 24,774 / 14,226 travels respectively. These are approximate centerline and reported-bead checks. No physical print or GUI preview verification was performed.

### Finer repeats across nominal planes and pinned boundaries (26 October 2026)

The remaining large magenta bands exposed two restrictions in the preceding refinement: joined cycles could end only on nominal planes, and proximity to any joined group vetoed exact fallback repeats. In the previous export, 96 thick magenta passes belonged to single-source joined 33/67 cyan/magenta stacks, carrying approximately 13.73 m of perimeter centerline at 0.268 mm height. Five nominal 0.08 mm slabs cannot be partitioned into two nominal-aligned copies of this recipe: a 0.16 mm cycle would make cyan only 0.0528 mm. Two 0.20 mm cycles instead produce printable 0.066/0.134 mm beads.

Shared equal-set groups now use the largest whole repeat count permitted by every authored component's minimum. Each child retains all original contour samples, including those beyond the child's own span, and is registered in every nominal slab it overlaps. Existing `(slab bottom, slab top]` scheduling emits each pass once; children partition the envelope in Z even when two own the same XY footprint inside one nominal slab. Each child rebuilds and re-pins its boundary, sorts its scheduling keys, and validates its fitted contours. Later children use the preceding child's final fitted footprint for support. Any failed child retains the original complete stack. Pure joins, optional borrowed colors, tongues and split runs retain their previous behavior.

An explicit skin envelope preserves the original solid coverage and exterior roles when a stack is split. An internal repeat boundary does not become a roof or floor. Tests exercise actual generated top/solid infill, complete pass emission in integrated slicing, the 33/67 off-plane case, per-color height conservation, boundary pinning and atomic fit failure.

Exact fallback fragments sharing a recipe with a joined group must match its period, phase and tool order before repeating. A different recipe may repeat independently beside a pinned group: the joined surface equals its scheduling key throughout the foreign clearance band, while every flat fallback top equals its own key. Unpinned optional fields still veto this relaxation. Fixed cores can repeat their allocated bead sequence without changing allocated per-color volume. Claude session `f279e363-023c-45fa-9134-f1fdebe80a51` independently checked these scheduling conditions and identified why the rule must remain conservative for unpinned fields. The new regression interleaves three flat repeats with two pinned joined repeats.

The first full export also exposed a separate nominal-wall regression at a fixed shell probe. Color reassignment changed boolean-operation order, leaving extremely narrow cracks in the common outline. Two captured partitions differed by only about 0.00024 mm² but produced nearest centerlines 0.351 and 0.588 mm from the same probe. Closing ten integer grid units (0.00001 mm) before common-wall generation produces approximately 0.332 mm in both cases. Generated paths and fills are still clipped to their original owners. The captured fixture checks coverage stability and that no meaningful path escapes its owner.

These changes do not force every stack to become thin. A small color component can set the minimum feasible repeat period, and a finer contour fit may fail even when the coarser stack is accepted. Nominal fallback bands also retain discrete color approximation. Repeated tool slots and optional interface topologies remain conservative cases; shortening them requires their own geometry validation rather than bypassing these checks.

A second fixed probe exposed why finer fallback fitting must not enlarge ownership. At Z=42.56 mm, a refined yellow/magenta stack accepted more of the candidate envelope than its coarse predecessor. This narrowed an adjoining nominal strip until its common wall disappeared. Exact fallback now first validates the coarse envelope and then refines within that same ownership; a rejected refinement retains the fitted coarse stack. Finer layer heights therefore cannot promote a previously rejected footprint or consume a printable neighboring band.

Final validation uses `sandboxes/local-z-mix-bridges/fine-islands-verified/` and the runtime `build-dev-release/localz-fine-islands/snapmaker-orca.exe`. **84 Local-Z/GCodeProcessor cases pass, with 644,387 assertions**. Release library, DLL and launcher builds, hidden startup and `git diff --check` pass. The original downloaded archive is unchanged. Earlier `remaining-thick/` and `fine-islands-final/` exports are intermediate diagnostics, not this final build's validation.

Tyranid magenta outer/overhang wall centerline length printed above 0.20 mm falls from 15,000.88 to 7,911.87 mm, a **47.26% reduction**. The length-weighted mean height falls from 0.105859 to 0.099604 mm. There are 316 matched exact fallback stacks with additional repeats; their total height per physical color is conserved to floating-point precision. The single-source cyan/magenta joined stacks increase from 100 to 162 complete stacks, with each component emitted exactly once. Thirty-eight joined magenta passes at 0.268 mm remain, and other retained coarse stacks include contour-fit failures and recipes limited by small components. The maximum magenta height is still 0.28 mm. `measured-repeat.png` plots the actual before/after 33/67 stack at Z=0.20–0.60 mm.

The final export has 7,431 passes (455 joining and 6,976 fallback), 32,657 island paths, 5,200 closed paths crossing recipe boundaries, 2,403,777 processed moves and 7,579 nonplanar extrusions. The reported-bead audit checks 1,051,191 segments, finds finite coordinates and positive dimensions, and bounds nonplanar heights between 0.060267 and 0.183702 mm. None of the 30 fixed coverage probes worsens by more than 0.02 mm; the worst change is 0.00943 mm. The Z=42.56 mm probe remains covered, and the Z=4.56 mm rounding-crack probe improves. Travel replay detects zero prior-bead crossings in 233,548 phase-B XY travels against 1,024,957 extrusions.

The full-height wall audit flags three near-coincident pairs among 742,769 outer/overhang segments. Each pair reproduces as two legs of one closed thin loop in the common classic generator before color splitting, within 0.002 mm at the exported endpoints. These overlaps remain in the audit; this is not a zero-overlap result. Benchy and cylinder export successfully, with zero qualifying wall overlaps among 177,604 and 289,685 segments and zero travel crossings among 24,777 and 14,226 travels, respectively.

Finer repeats have costs and limits. Estimated Tyranid filament changes increase from 5,230 to 5,598, printing time from 33h35m31s to 35h17m38s, and total material from 115.73 to 117.26 g. Forty comparable nominal-band probes have mean agreement with their own exact cadence of 79.68%, versus 83.80% before; one improves and nine worsen, with no missing or ambiguous owned intervals. This remains nominal-plane color approximation, rather than complete continuity at every boundary. The audits use centerlines and reported bead dimensions; no physical print or GUI preview verification was performed.

### Connected local groups and common-color handovers (26 October 2026)

The next Tyranid close-ups showed that the previous checks did not establish continuous paths across useful parts of the model. A perimeter census of `fine-islands-verified/` measured only 848.07 mm of sloped perimeter centerline, out of approximately 345.7 m. A group could contain disconnected occurrences of the same recipes, so a difficult occurrence could reject otherwise usable islands elsewhere.

Candidate construction now separates physically connected components before fitting and validates shorter printable repeats before rejecting a coarse joined surface. An optional-color group that cannot be pinned at a foreign boundary gets one bounded retry with a common-color interface. Pure and optional regions excluded by that retry return to nominal ownership. This retry repeats the geometry, clearance, fit and configuration checks; it does not bypass them.

Common-anchor groups now keep the preferred terminal material at the top. Residual materials retain their original relative order below it. This removes the terminal-to-terminal handover introduced when the common anchor was always placed first. Fine children must preserve the parent's mode, anchor, sources and component order, with minimum-height, ceiling and boundary scheduling checks.

Same-material fallback matching uses physical overlap rather than identical recipe-ID lists. Compatible overlap can become a thick fitted core while changing edges retain nominal ownership, provided both core and remainders can carry their own perimeter. A width reserve includes a 0.01 mm offset-simplification margin. An opening check rejects narrow appendages even when the wide main body survives erosion. Untouched nominal islands regroup after matching to avoid publishing a separate plan for every clipping fragment.

Fitted path configuration lookup retains a region from the original footprint before paint segmentation. A tapered island whose scheduling footprint disappears can retain the owner from another validated footprint in the same span. Perimeter compatibility is checked across all covered layers. Infill still respects its individual modifier regions; any fitted interior outside surviving nominal fragments uses the retained owner rather than becoming an unintended void.

Claude session `f279e363-023c-45fa-9134-f1fdebe80a51` reviewed the component split, common-anchor retry, terminal order, scheduling ceilings, configuration ownership and thin-tail checks. Intermediate `continuous-recovery` through `continuous-recovery-v6` directories include failed or superseded diagnostics and are not successful Tyranid validation.

The first completed iteration (`continuous-recovery-v7`) still failed visual inspection: sloped perimeter length increased only from 848 to 1,060 mm, and two fixed coverage probes regressed. One regression came from a fitted island too narrow to generate any wall: it consumed nominal ownership while emitting no perimeter. Fitting now checks that each required connected contour can carry the nozzle width before publishing ownership. Rejected fragments return to fallback.

The principal continuity blocker was inconsistent temporal ownership. Candidate construction already assigned one disjoint XY recipe partition to a cycle, but a later all-layer foreign-paint check carved away that ownership whenever another eligible mixed recipe crossed it on a different nominal slab. Adjacent recipes repeatedly lost their shared boundary. Eligible mixed paint now yields only inside that already-assigned owner for the cycle; outside it, the paint remains a clearance obstacle. Physical/pure paint and ineligible recipes still participate in foreign-paint rejection. Original slice unions are cached while this immutable planning stage runs.

This is bounded temporal paint quantization, not exact preservation of each nominal paint edge. A recipe boundary can move within the original cycle span (0.4 mm in this archive). The offline audit measures source-to-source reassigned volume and checks overlapping accepted owners in XYZ, including off-plane child boundaries. For the intermediate v8 export, 2,016.68 mm³ of 15,059.78 mm³ changed recipe assignment, with zero overlapping continuous owners. Authored-percentage accounting estimates net M/C/Y/W volume changes of +0.35%, +0.26%, -0.20%, and -4.35%, respectively; this excludes component bias and optical color prediction. Pure white ownership changes only by approximately 9e-7 mm³ of clipping precision.

V8 restored 7,386.72 mm of actual sloped cross-recipe perimeter, but increased thick magenta extrusion. Replaying 131 coarse source-7 candidates showed most fine fits rejected a small disconnected contour below nozzle width. An attempted fine-footprint cutback in v9 reduced coarse joined paths, but increased nominal fallback and regressed three fixed coverage probes (maximum 0.2663 mm), so that cutback was removed. Connected coarse ownership remains when finer contours cannot preserve its features. This is a geometry restriction in addition to the component-height minimum; dividing all authored heights by two alone is insufficient.

Common-anchor children inherit the union of all preceding fitted passes as support; the last scheduled residual pass alone covers only one recipe zone. Surface candidates no longer repeat the identical failed refinement calculation after coarse fitting. Debug rejection records distinguish topology, minimum height, child grouping/order/clearance, and contour failure; contour reason bits are 1 vanished, 2 width, 4 staircase, 8 support, and 16 sampled coverage. The captured-cycle replay can report the failed fine footprint without modifying the parent.

The retained build and export are `build-dev-release/localz-continuous-recovery-v10/` and `sandboxes/local-z-mix-bridges/continuous-recovery-v10/`. **91 Local-Z/GCodeProcessor tests pass (649,390 assertions)**, alongside the Tyranid, Benchy and cylinder archive exports. Release core, DLL, launcher and hidden startup pass. `validation.json` records source, binary and archive hashes; the original downloaded 3MF is unchanged.

The Tyranid export contains 16,788 passes (11,652 joined, 5,136 fallback), 48,378 island paths, 9,012 closed paths crossing recipe boundaries and 67,140 nonplanar extrusions. Actual sloped cross-recipe perimeter length is **7,375.40 mm**, versus 848.07 mm before this work (8.70 times). Nominal fallback perimeter length decreases from 244.54 to 131.10 m. The sampled same-material contact diagnostic decreases from 90 to 77 (magenta 44 to 30); this approximation includes pure material and curved shell intersections and is not a claim that every repeated-color layer disappeared. All 1,621 single-source cyan/magenta joined stacks emit each component exactly once. `continuous-bridge-example.png` plots a real closed cyan perimeter crossing recipes 14, 17 and 24 and its continuous bead surfaces.

Geometry replay checks 1,206,788 segments: finite coordinates, positive dimensions, and nonplanar heights within the existing tip-to-cap range (0.0192764–0.233312 mm). Travel replay detects zero crossings inside previously reported beads among 201,249 phase-B XY travels and 1,184,642 extrusions. The wall audit retains three near-coincident pairs among 722,317 segments. Two match baseline classic-loop cusps; the third at XY (143.980, 112.766), Z=38.76 reproduces as adjacent legs of one closed classic perimeter before color splitting (`continuous-recovery-v8/overlap-band-comparison.json`). This is not a zero wall-overlap result. Benchy and cylinder have zero qualifying wall overlaps and zero detected travel crossings.

Thirty fixed coverage probes show one worsening above 0.02 mm: 0.021542 mm at Z=2.56, XY (153.403, 124.561). The larger v7/v9 regressions are absent. Coarse passes remain: magenta outer/overhang length above 0.20 mm increases from 7,911.87 to 14,097.87 mm as more surface uses actual joined stacks instead of nominal dithering; maximum height is 0.32 mm. The shortest component is considered first, but a finer contour that loses a printable feature retains its validated coarse stack. The implementation does not guarantee thin beads or a continuous join at every texture boundary.

The final temporal ownership audit reports zero overlapping accepted continuous owners and 2,016.07 mm³ of reassigned recipe volume out of 15,059.75 mm³. Authored-percentage net M/C/Y/W volume estimates change by +0.34%, +0.27%, -0.18%, and -4.32%; these are not optical-color predictions. Export estimates are 6,036 filament changes, 35h40m26s and 128.03 g, versus 5,598, 35h17m38s and 117.26 g in the preceding baseline. Native front and top previews are checked against that baseline using the unchanged viewer; all measurements remain software validation, not a physical print.
