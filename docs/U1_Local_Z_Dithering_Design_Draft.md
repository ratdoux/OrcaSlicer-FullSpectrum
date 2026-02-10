# U1 Local Z Dithering Design Draft

Status: Draft for later implementation
Last updated: 2026-02-10
Owner: ratdoux

## 1. Problem Statement

Current dithering support can alternate component filaments in Z, but layer height is still resolved globally per object layer.
This means:

- `dithering_z_step_size` can only change full layers (or full Z bands), not only painted XY zones.
- The requested behavior (`---===`) is not achievable today:
  - non-painted area keeps base height (example: `0.12`)
  - painted mixed area is subdivided (example: `0.06 + 0.06`) in the same nominal Z interval

## 2. Current Architecture Constraints

Key code paths:

- Layer heights are created before segmentation:
  - `src/libslic3r/PrintObjectSlice.cpp` (`update_layer_height_profile`, `generate_object_layers`)
- Mixed painting segmentation is applied after layers already exist:
  - `src/libslic3r/PrintObjectSlice.cpp` (`apply_mm_segmentation`)
  - `src/libslic3r/MultiMaterialSegmentation.cpp`
- `Layer` has one `height`, `slice_z`, `print_z` for the entire layer:
  - `src/libslic3r/Layer.hpp`

Conclusion: existing pipeline assumes one global Z step per layer. Local per-XY sublayering needs a new planning model.

## 3. Goals

- Support local Z subdivision for mixed-painted zones only.
- Keep base regions at user base layer height whenever possible.
- Preserve current mixed filament alternation logic (A/B cadence).
- Keep existing behavior when local mode is disabled.
- Avoid regressions in non-mixed prints.

## 4. Non-Goals (Phase 1)

- Full non-planar slicing.
- Arbitrary local adaptive mesh refinement outside mixed-painted zones.
- Rewriting all perimeter/infill algorithms from scratch.

## 5. Proposed Feature Model

Add a new mode on top of current dithering:

- `dithering_local_z_mode` (bool, default `false`)
- Existing `dithering_z_step_size` remains the micro step for painted zones.
- Existing `dithering_step_painted_zones_only` remains as compatibility switch for current global mode.

When `dithering_local_z_mode = true`:

- For each base Z interval `[z0, z1]`:
  - if no mixed paint intersects interval: print normally at base layer height.
  - if mixed paint intersects interval: split interval into sublayers at `dithering_z_step_size`.
  - mixed-painted XY polygons are printed on each sublayer with alternating components.
  - non-mixed XY polygons are printed as a base-height pass in that interval (not duplicated every sublayer).

## 6. High-Level Architecture

Introduce a two-level planning pipeline:

1. Base Layer Plan (existing):
   - Build base object layers as today.
2. Local Z Expansion Plan (new):
   - For base layers that intersect mixed-painted areas, build `SubLayerPlan` entries.
3. Toolpath Assignment Plan (new):
   - Route painted polygons to sublayers.
   - Route non-painted polygons to one base-height pass within same interval.
4. G-code Scheduler (extended):
   - Emit sublayer passes in Z order while respecting extrusion height per pass.

## 7. New Data Structures (Draft)

Add new planning structs (names tentative):

```cpp
struct LocalZInterval {
    double z_lo;
    double z_hi;
    double base_height;      // e.g. 0.12
    double sublayer_height;  // e.g. 0.06
    bool   has_mixed_paint;
};

struct SubLayerPlan {
    double z_lo;
    double z_hi;
    double print_z;
    double flow_height;
    // per extruder painted masks for this sublayer
    std::vector<ExPolygons> painted_masks_by_extruder;
    // polygons printed as normal/non-mixed in this pass
    ExPolygons base_masks;
};
```

Attach `std::vector<SubLayerPlan>` to `PrintObject` (or a dedicated planner cache) without immediately replacing `Layer`.

## 8. Algorithm Draft

### 8.1 Build Local Z Intervals

- Start from base layer intervals from `generate_object_layers`.
- For each base interval, query whether mixed-painted states are present in that Z range.
- If not present, interval remains single-pass.
- If present, split into `N = ceil(base_height / z_step)` subintervals.

### 8.2 Build Painted Masks Per SubLayer

- Re-run or adapt `multi_material_segmentation_by_painting` to produce painted masks at sublayer Z samples (not only base layer Z samples).
- For each sublayer:
  - derive active mixed pair (A/B) based on cadence index.
  - assign painted XY polygons to active physical extruder for that sublayer.

### 8.3 Preserve Base Regions at Base Height

- For non-painted polygons inside a locally-split base interval:
  - emit once with `flow_height = base_height` in a designated pass (typically final sublayer pass of interval).
  - do not emit these polygons on intermediate sublayers.

### 8.4 Boundary Handling

- At boundaries between painted and non-painted masks:
  - enforce overlap/tolerance compensation to avoid cracks.
  - clip with robust polygon booleans and min-area filtering.
- Add seam strategy notes for transitions (TBD in implementation).

## 9. Required Code Areas

Primary:

- `src/libslic3r/PrintObjectSlice.cpp` (new local-Z planning stage, call ordering)
- `src/libslic3r/MultiMaterialSegmentation.cpp` (sublayer mask generation)
- `src/libslic3r/PrintObject.cpp` (cache invalidation + storage for local-Z plans)
- `src/libslic3r/GCode/*` (scheduler/tool ordering to emit sublayer passes correctly)

Likely:

- `src/libslic3r/Layer*` (if promoting local-z plan into first-class layer abstraction)
- `src/libslic3r/PrintApply.cpp` (config propagation and reset handling)
- `src/slic3r/GUI/Tab.cpp` and `src/libslic3r/PrintConfig.*` (new option + tooltips)

## 10. Compatibility and Migration

- Keep existing `dithering_z_step_size` behavior when `dithering_local_z_mode=false`.
- Hide/disable local-Z checkbox unless mixed virtual filament is enabled.
- Project files without new key must load as current behavior.
- Fallback: if local-Z planner fails, auto-fallback to current global mode with warning.

## 11. Validation Plan

### Unit/logic tests

- Interval splitting:
  - no mixed paint -> no split
  - mixed paint -> expected number of sublayers
- Cadence:
  - A/B alternation across sublayers matches configured ratio/step
- Config changes:
  - step size changes between slices must invalidate planner cache

### Integration tests

- Painted stripe through object:
  - verify non-painted region keeps base pass count
  - verify painted region gets subdivided passes
- Multi-object plate with only one mixed object.
- Support + mixed paint interaction.

### G-code assertions

- In affected intervals:
  - expected `Z` move cadence in mixed zones
  - base region extrusion appears once per base interval
- In unaffected intervals:
  - unchanged base layer Z cadence

## 12. Risks

- High complexity around perimeter/fill continuity at mask boundaries.
- Performance impact from finer slicing and extra polygon clipping.
- Increased memory use for per-sublayer painted masks.
- Potential regressions in wipe tower/tool ordering and support synchronization.

## 13. Rollout Plan

Phase A - Planner skeleton (no G-code changes yet):

- Build and debug `LocalZInterval` and `SubLayerPlan`.
- Add debug export (SVG/JSON) for visual verification.

Phase B - Limited emission path:

- Enable local-Z only for perimeter walls on painted regions.
- Keep infill/base regions on current behavior for early validation.

Phase C - Full emission:

- Perimeters + infill + top/bottom in local-Z path.
- Boundary compensation and seam cleanup.

Phase D - Stabilization:

- Performance tuning, cache policy, presets/profile updates.
- Expand regression suite and add fixture models.

## 14. Open Questions

- Should non-painted regions in split intervals be printed on first or last sublayer pass?
- Do we allow base-height extrusion in an interval where painted sublayers already deposited material nearby, or enforce sublayer-only flow there?
- How should top/bottom skin logic behave when only part of a layer is sublayered?
- Should local-Z mode require a minimum nozzle/step ratio gate for print safety?

## 15. Recommended Next Step

Prototype only Phase A:

- Build `LocalZInterval`/`SubLayerPlan` and dump debug artifacts.
- No toolpath emission changes yet.
- Use this to validate that painted masks and interval splitting are stable before committing to full pipeline rewrite.
