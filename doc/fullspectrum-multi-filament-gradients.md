# Multi-Filament Gradients

This document explains FullSpectrum gradients containing three or more physical filaments.

## Overview

A multi-filament gradient is a spatial gradient whose material recipe contains an ordered set of physical filaments. It is different from:

- a static multi-filament recipe, which keeps the same aggregate proportions throughout the object;
- a two-filament gradient, which only interpolates between one A/B pair;
- a manual wall pattern, which follows an explicit repeating sequence.

The slicer moves through the ordered component list over the gradient domain. At any one position it normally blends an adjacent pair using Local-Z sublayers.

Example:

```text
Red → Yellow → White
```

The early part of the gradient blends red and yellow. The later part blends yellow and white. A centered solid-yellow window separates those two pair transitions at the middle stop.

## Components, weights, and stops

The canonical mixed-filament definition stores:

- ordered physical-filament references;
- normalized component weights;
- whether spatial-gradient behavior is enabled;
- the gradient endpoints;
- normalized stop positions.

For `N` components, a spatial multi-filament gradient uses `2N − 1` stop positions.

For three components, five positions describe:

1. the first component anchor;
2. the first/second transition midpoint;
3. the middle component anchor and pair-transition join;
4. the second/third transition midpoint;
5. the final component anchor.

Conceptually:

```text
Component 1          Component 2          Component 3
    |--------- blend 1/2 ---------|
                              |--------- blend 2/3 ---------|
    0       midpoint          join       midpoint             1
```

All stop positions are normalized to the gradient domain from `0.0` to `1.0`.

If explicit positions are unavailable for a gradient with three or more components, the slicer derives default boundaries from the component weights. Larger weights give a component more space in the gradient.

## Independent cadence resolution

For every independent gradient cadence cycle, the slicer:

1. determines the cycle's normalized physical-Z progress through the gradient domain;
2. identifies the active adjacent component pair;
3. interpolates the pair ratio at that position;
4. converts that ratio into sublayer heights using the configured **Gradient Local-Z layer height**;
5. constrains active passes to the configured minimum sublayer height and each filament's maximum layer height;
6. carries the independent cadence across normal process-layer boundaries;
7. orders the passes to avoid duplicating the same physical component across cadence boundaries where possible.

The gradient therefore uses physical-Z progression to choose the pair, while its independent cadence height controls the apparent ratio within that pair. Legacy explicit A/B Local-Z cadence heights are ignored for gradients.

### Middle-stop behavior

At an interior component such as yellow in `red → yellow → white`, the slicer reserves a solid middle-component band centered on that component's stop. The red/yellow transition reaches solid yellow at the lower window edge, and the yellow/white transition starts at solid yellow at the upper edge.

The setting `dithering_local_z_gradient_middle_filament_window` specifies the window width as a percentage of the complete gradient domain. The requested width is capped at the adjacent transition stops, which preserves both transition regions and prevents neighboring middle-filament windows from overlapping.

## Relationship to simplified Local-Z

Spatial gradients automatically use Local-Z subdivision. Users do not need to enable **Subdivide all mix layers** just to activate a gradient.

A gradient assigned to an entire object or volume also receives automatic full-domain handling. Painted gradients remain scoped to their painted regions unless the global full-domain override expands the eligible domain.

**Gradient Local-Z layer height** supplies the independent height budget. The object's normal process layer height only controls geometric slicing and no longer has to be changed to make the gradient subdivide. **Local-Z minimum sublayer height** may still limit extreme ratios. For example, a requested five-percent component cannot be represented exactly if five percent of the gradient Local-Z height is below the minimum.

## Persistence

Multi-filament gradients are dual-written in FullSpectrum 3MF projects.

### Canonical representation

The authoritative representation is stored in:

```text
/Metadata/fullspectrum/mixed-filaments.json
```

It uses stable physical-filament references and preserves:

- component order;
- weights;
- gradient enablement;
- endpoints;
- stop positions;
- related Local-Z and surface-bias behavior.

When spatial-gradient behavior is enabled, the package declares the required feature:

```text
fs.mixed-gradient.v1
```

### Legacy compatibility projection

A compact legacy representation is also written for older FullSpectrum readers. Stable IDs and canonical physical references remain authoritative; numeric or compact identifiers are compatibility fallbacks.

This dual-write behavior lets current projects retain complete multi-filament gradient information while remaining as compatible as possible with earlier FullSpectrum builds.

## Practical limitations

- Only adjacent components are normally active at one gradient position; this is not a simultaneous analytic blend of every physical filament.
- Minimum sublayer height limits extreme ratios.
- More transitions generally mean more physical toolchanges.
- The visible result depends on the configured physical colors, transmission properties, material calibration, and Gradient Local-Z layer height.
- Very short gradient domains provide fewer independent cadence cycles in which to express stops and smooth transitions.
- Manual patterns are a separate distribution mode and do not use this automatic gradient path.

## Validation status

The automated tests cover:

- automatic Local-Z routing for gradients;
- component and weight normalization;
- stop-position round trips;
- centered middle-filament Local-Z windows and their adjacent pair transitions;
- canonical FullSpectrum serialization;
- legacy compatibility serialization;
- stable-reference reconstruction.

A release candidate should also be checked visually with a sliced three-or-more-component model because appearance still depends on the selected materials and calibration.

## Implementation references

- [`MixedFilament/Gradient.cpp`](../src/libslic3r/MixedFilament/Gradient.cpp)
- [`MixedFilament/Definition.cpp`](../src/libslic3r/MixedFilament/Definition.cpp)
- [`PrintObjectSlice.cpp`](../src/libslic3r/PrintObjectSlice.cpp)
- [`Format/FullSpectrum3mf/Fs3mfLegacyBridge.cpp`](../src/libslic3r/Format/FullSpectrum3mf/Fs3mfLegacyBridge.cpp)
- [`tests/libslic3r/test_mixed_filament.cpp`](../tests/libslic3r/test_mixed_filament.cpp)
