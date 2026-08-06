# Simplified Gradients, Direct Multicolor Local-Z, Independent Layer Heights, and Surface Paint Only

This document explains the simplified gradient workflow and the **Surface paint only** option in FullSpectrum.

## Simplified gradient workflow

Gradients no longer depend on enabling the global **Subdivide all mix layers** or **Full domain for all mixes** options before they can work.

The global options are now overrides:

- A gradient automatically uses Local-Z subdivision.
- **Subdivide all mix layers** extends Local-Z subdivision to ordinary non-gradient mixed filaments.
- A Local-Z mixed filament assigned to an entire object or volume automatically receives full-domain treatment.
- **Full domain for all mixes** extends full-domain behavior to other eligible mixed painted regions.

The automatic behavior is scoped to the relevant definition and assignment. A gradient assigned to an object does not make separately painted physical-filament or ordinary mixed-filament areas use Local-Z.

### Automatic sublayer heights

Gradients use the dedicated **Gradient Local-Z layer height** as their independent cadence budget. They no longer use or modify the object's normal process layer height.

- Configuration key: `dithering_local_z_gradient_layer_height`
- Default: `0.20 mm`
- The effective value is automatically raised to at least twice **Local-Z minimum sublayer height**.

The requested component ratio is applied to the gradient cadence budget, so separate A/B layer heights do not need to be configured.
Gradient definitions ignore the legacy explicit A/B Local-Z cadence heights; those controls remain available for non-gradient mixes.

For a gradient Local-Z height `H` and a requested B percentage `p`:

```text
requested B height = H × p / 100
requested A height = H − requested B height
```

Each active component must satisfy **Local-Z minimum sublayer height**:

- Configuration key: `mixed_filament_height_lower_bound`
- Default: `0.06 mm`
- Hard minimum: `0.01 mm`

Example at a `0.20 mm` gradient Local-Z height, regardless of whether the process layer height is `0.08`, `0.16`, or `0.24 mm`:

```text
Requested ratio: 75% A / 25% B
Requested heights: 0.15 mm A / 0.05 mm B
Minimum sublayer: 0.06 mm
Printed heights: 0.14 mm A / 0.06 mm B
```

Solid endpoints remain solid. A 100/0 or 0/100 endpoint is not forced into two passes.

Gradient passes may cross process-layer boundaries. A process layer that is too short to contain both components therefore does not collapse the gradient to a single component. The normal layer height continues to control geometric slicing, while the gradient Local-Z height controls color cadence.

### First-layer protection

**Keep first layer unsplit** defaults to enabled. In full-domain mode it preserves the first object layer at its nominal height so a thin Local-Z pass does not compromise bed adhesion.

### Painted versus object-assigned gradients

The automatic full-domain rule applies when the mixed filament is the color of the entire object or volume.

A gradient applied with the color-painting tool remains limited to its painted masks unless **Full domain for all mixes** explicitly expands the eligible domain. Physical paint and ordinary non-gradient mixed paint remain at their normal cadence unless their own settings opt them into subdivision.

Manual-pattern mixed filaments do not automatically use the gradient Local-Z path.

## Direct multicolor Local-Z

**Use direct multicolor Local-Z solver** is an experimental option for static, non-gradient recipes containing three or more physical filaments.

- Configuration key: `dithering_local_z_direct_multicolor`
- Default: disabled
- Requires the mixed row to use Local-Z
- Ignored when either explicit Local-Z A/B cadence height is configured

Without the direct solver, a static recipe with three or more components is represented through a sequence of A/B pair combinations. With the direct solver, the slicer allocates the available Local-Z passes directly among every positively weighted physical component.

For every nominal-layer interval, the solver:

1. calculates each component's target share of the nominal height;
2. selects a legal number of passes based on the Local-Z minimum and the row's optional maximum-sublayer limit;
3. creates pass heights that sum to the nominal layer height;
4. assigns each pass directly to a physical component;
5. carries unrepresented height error into following layers;
6. discourages selecting the same component in consecutive passes when another component has comparable remaining demand.

The carried error is important when a nominal layer cannot contain enough legal passes to represent every component exactly. The requested aggregate ratio is approached over subsequent layers instead of emitting sublayers below the configured minimum.

Example:

```text
Recipe: 50% A / 25% B / 25% C
Nominal layer: 0.20 mm
Local-Z minimum: 0.04 mm
```

The solver may divide the nominal layer into several legal passes and assign them directly to A, B, and C. The exact component order may vary between layers as accumulated error is corrected.

### Scope and limitations

- It applies to non-gradient recipes with at least three available, positively weighted physical components.
- Spatial multi-stop gradients continue to use adjacent-pair gradient cadence.
- Manual-pattern definitions are excluded.
- Ordinary non-gradient mixes still need **Subdivide all mix layers** to opt into Local-Z.
- A component is not guaranteed to appear in every nominal layer if the layer cannot accommodate enough minimum-height passes.
- More directly represented components can increase toolchanges.
- **Apply subdivision to infill** controls whether the Local-Z passes are also used for infill inside the mixed region.

## Independent Local-Z layer heights

**Use independent Local-Z layer heights** is an experimental extension for static direct-multicolor recipes. Gradients use their dedicated independent **Gradient Local-Z layer height** automatically and do not require this checkbox.

- Configuration key: `dithering_local_z_independent_layer_height`
- Default: disabled
- Requires **Use direct multicolor Local-Z solver**
- Ignored when either explicit Local-Z A/B cadence height is configured
- Excludes spatial gradients, which use their automatic gradient-specific independent cadence, and manual patterns

Although the direct solver primarily targets recipes with three or more components, independent-height mode can construct a cadence for any eligible non-gradient Local-Z recipe containing at least two positively weighted components.

### Exact ratio construction

The smallest positive component weight receives one **Local-Z minimum sublayer height**. Every other component's total cadence height is scaled from it:

```text
component height =
    Local-Z minimum × component weight / smallest positive weight
```

For a `1/1/3` recipe and a `0.04 mm` minimum:

```text
A: 0.04 × 1 / 1 = 0.04 mm
B: 0.04 × 1 / 1 = 0.04 mm
C: 0.04 × 3 / 1 = 0.12 mm

Repeating cadence: A 0.04 / B 0.04 / C 0.12 mm
Cadence height: 0.20 mm
```

If the normal layer height is `0.08 mm`, the cadence is not compressed into that height. It continues across nominal-layer boundaries:

```text
Z 0.04: A pass
Z 0.08: B pass
Z 0.20: C pass
```

This preserves the requested `1/1/3` thickness ratio instead of clamping the dominant component to what fits inside each `0.08 mm` nominal layer.

### Maximum-layer-height protection

Each component's total cadence height is checked against that physical filament's configured maximum layer height.

If a required height is too large, it is divided into equal passes while preserving the component's total contribution. When no explicit maximum is available, the fallback maximum is 75% of that component's nozzle diameter.

For example, with a `0.20 mm` maximum:

```text
Required dominant-component height: 0.28 mm
Emitted passes: 0.14 mm + 0.14 mm
```

Extreme ratios may therefore create several consecutive passes of the dominant component between appearances of the smallest component.

### Where the cadence runs

The independent cadence maintains its own Z cursor while the same painted mixed region remains active. It may span several nominal layers, and its passes are kept as a dependency-ordered sequence for G-code and wipe-tower planning.

If the mixed region ends before the next full cadence pass fits, the remaining height is emitted at the end of the interval. Exact ratios are therefore best expressed over a sufficiently tall, continuous region; very short painted regions may end on a partial cadence.

All active mixed rows in an interval must have a valid independent cadence before the interval uses this mode. Fixed physical-filament areas keep their ordinary nominal-layer treatment.

### When to use it

Independent heights are useful when:

- a static mixed recipe has a large ratio difference between components;
- the nominal layer height is too small to contain all required minimum-height passes;
- exact physical thickness ratios matter more than keeping every cadence inside one nominal layer.

Leave it disabled when:

- strict alignment to nominal layers is more important;
- the additional Local-Z passes or toolchanges are undesirable;
- the recipe is a spatial gradient, which uses the gradient pair-cadence path.

## Surface paint only

**Surface paint only** keeps color-painted regions in the object's exterior shell instead of allowing those regions to extend through the full solid area.

- Configuration key: `fs_surface_paint_only`
- Default: disabled
- Location: **Print Settings > Multimaterial > Color Mixing (Experimental)**

This setting changes the depth of facet-painted segmentation. It does not change the selected filament, gradient ratio, Local-Z height calculation, or perimeter-modulation strength.

### Calculated shell depth

The slicer derives the permitted sidewall depth from the configured wall shell:

```text
shell width = outer wall width + (wall count − 1) × inner wall width
```

For three `0.45 mm` walls:

```text
shell width = 0.45 + (3 − 1) × 0.45 = 1.35 mm
```

Painted side regions outside the inset interior are retained; deeper segmented geometry is removed.

When **Surface paint only** is disabled, **Maximum width of a segmented region** controls the penetration depth instead. A value of zero disables that width limit. When **Surface paint only** is enabled, the calculated shell width replaces the configured maximum width for this segmentation pass.

### What it affects

Surface-only confinement applies to facet-derived color regions, including:

- ordinary MMU/color-painter marks;
- painted physical filaments;
- painted mixed filaments and gradients;
- transient Normal Mix image-map facets;
- transient palette ownership used by perimeter-modulated image maps.

It does not restrict a filament or gradient assigned directly to an entire object or volume because that is an object/volume material assignment, not a painted surface mask.

### Top and bottom surfaces

“Surface paint only” does not mean “outermost extrusion only.”

Sidewall paint is limited to the calculated wall-shell width. Painted horizontal faces are still projected through the configured top or bottom shell layers. Top/bottom solid skin is part of the visible shell and may therefore use the painted filament.

### Extra perimeters in painted zones

**Extra perimeters in painted zones** adds walls only to regions where the painted filament differs from the parent wall filament.

- Configuration key: `fs_painted_zone_extra_perimeters`
- Range: `0` to `8`
- Default: `0`

This can create a thicker colored shell without increasing the wall count for the rest of the object. A same-filament paint mark keeps the parent wall count to avoid overlapping walls where there is no actual material transition.

Because the surface shell is calculated from the active region wall counts and widths, additional painted-zone perimeters may also increase the depth available to the painted shell.

### Limitations and interactions

- The current implementation selects the maximum calculated shell width across the object's print regions. It is not independently recalculated for every painted point.
- Beam interlocking bypasses the segmented-region width cut. Do not rely on strict surface-only confinement while **Use beam interlocking** is enabled.
- A painted gradient is normally shell-confined, but a gradient assigned as the entire object or volume color follows automatic full-domain behavior.
- Very thin features may contain little or no unpainted interior because the wall shell occupies most or all of the cross-section.

## Implementation references

- [`MixedFilament/Common.cpp`](../src/libslic3r/MixedFilament/Common.cpp)
- [`PrintObjectSlice.cpp`](../src/libslic3r/PrintObjectSlice.cpp)
- [`MultiMaterialSegmentation.cpp`](../src/libslic3r/MultiMaterialSegmentation.cpp)
- [`PrintConfig.cpp`](../src/libslic3r/PrintConfig.cpp)
- [`PrintApply.cpp`](../src/libslic3r/PrintApply.cpp)
- [`tests/fff_print/test_print.cpp`](../tests/fff_print/test_print.cpp)
