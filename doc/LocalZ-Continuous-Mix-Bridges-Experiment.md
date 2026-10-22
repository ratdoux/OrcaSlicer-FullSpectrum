# Continuous Local-Z mix bridges

Branch: `experiment/local-z-continuous-mix-bridges`  
Starting checkpoint: `63117732a0`  
Status: experimental, disabled by default; no physical print validation.

## Behavior

Adjacent recipes can share continuous physical-color islands for their common physical
filaments even when their component thicknesses differ. The planner forms
connected groups from touching paint masks separately on each layer:

- Equal component sets share a complete stack, with any number of positive,
  printable components. Every cumulative interface varies continuously.
- Shared interfaces can cross nominal layer planes. Both sides of each bead
  follow the same continuous fields; Z and flow taper across the boundary.
  There is no private-flat-pass fallback or abrupt thickness switch.
- Different sets can share one common bottom filament. Each recipe finishes
  with its own residual stack above that interface, preserving its residual
  proportions. For example, CM and CMY can join C without inventing Y in the
  CM zone or tapering it below the printable minimum. The remaining passes
  still end at the recipe boundary.
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
to the final bead rather than leaving unowned volume. Same-color merging tips
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
This switch is not a project or printer setting. Reslice after changing builds.
The corrected experimental runtime is `build-dev-release/mix-bridges-islands/`.

The log reports `Local-Z mix bridges ... cycle_span=... passes=...`.
G-code contains `; local-z mix bridge component=N` markers. N identifies the
ordered pass within a group, including residual branches.

## Eligibility and limits

Whole-object Local-Z and either setting of mixed-region collapse are supported.
Fixed recipes require positive physical component weights and printable bead
heights. Gradients, manual patterns, adaptive image maps, explicit A/B height
overrides, rafts and spiral vase retain their existing planner. Preserve-first-
layer is required. Wall widths, counts, speeds and scarf settings must match.

A cycle may span several nominal layers. The planner chooses the shortest span
admitting the most eligible recipes, respecting the configured minimum and each
tool's maximum height. It tiles the complete post-first-layer height; a short
tail is absorbed into a longer printable cycle. No ratio is clamped to fit.
For the reported 0.08 mm nominal/minimum case, 50:50 and 51:49 recipes use a
0.24 mm cycle: 0.12/0.12 and 0.1224/0.1176 mm respectively.

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
actual surface. Unlike component sets spanning multiple intervals, changing
cross-sections and impossible tails fall back to the original planner.
Rejection restores a complete run, including every group that depends on it.

Shared walls and enabled infill now advance together inside the owned island.
Tongues also depend on the preceding pure nominal bead, even when their tip
is lower elsewhere. The pass ceiling remains the travel reference. This is
not a full nozzle-shape clearance proof; physical validation remains needed.

Within one nominal interval, unlike component sets can share a common bottom
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
out. Nearby foreign mixed recipes on any covered interval prevent a varying group, using a guard of half
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
& .\build-dev-release\mix-bridges-islands\libslic3r_tests.exe '[MixBridge]' --reporter compact
$env:ORCA_LOCAL_Z_MIX_BRIDGES = '0'
& .\build-dev-release\mix-bridges-islands\libslic3r_tests.exe '[LocalZ]~[.],[SlicingSpeedPort]~[.],[MixedFilament]~[.]~[!shouldfail],[WallSupport]~[.],[PaintIslands]~[.]' --reporter compact
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

The regular regression command excludes two existing `[!shouldfail]` cases.
The earlier unfiltered run exited 42 because `m1: compute uses num_physical
bound, remove_physical uses kMax=64 (differential)` passes its assertion despite
that marker. `batch_remap mixed pair-fallback (stable_id=0)` fails as expected.
Those tests and their mixed-filament implementation are unchanged.

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
