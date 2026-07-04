# Full Spectrum TD99 Sidewall Color Plan

## Problem

FullSpectrum currently estimates mixed-filament colors mostly from filament profile colors and layer or pattern ratios. That is useful for a quick swatch, but it misses an important sidewall behavior: translucent layers are visually affected by the material above and below them. A yellow layer under a blue region should look different from the same yellow layer under a magenta region.

The model must stay sidewall-oriented. This is not HueForge-style top-surface blending.

## Filament TD

Add a filament profile value named transmission distance, stored in millimeters:

```text
filament_transmission_distance
```

The value is TD99: the material thickness that absorbs 99 percent of transmitted light. A value of zero means unknown and preserves legacy behavior.

For a layer thickness `t`:

```text
optical_depth = ln(100) * t / TD99
transmittance = exp(-optical_depth)
opacity = 1 - transmittance
```

Examples for `TD99 = 1.0mm`:

```text
1.0mm -> 1% transmitted
0.5mm -> 10% transmitted
0.2mm -> about 40% transmitted
```

## Spatial Stack Requirement

The color predictor must be spatial, not just vertical. A single layer can have multiple apparent colors at different XY sidewall locations.

Example:

```text
Layer N:   yellow across the whole cube
Layer N+1: blue on the left half, magenta on the right half
```

The yellow layer should be green-tinted under the blue region and orange/red-tinted under the magenta region. Blue must not contaminate the yellow under the magenta side, and magenta must not contaminate the yellow under the blue side.

The first implementation therefore uses strict spatial regions:

```text
contribution(layer j -> sample i)
    applies only when spatial_region_id(j) == spatial_region_id(i)
```

This intentionally assumes no lateral optical bleed. If real calibration prints show sideways diffusion, we can add a small lateral kernel later.

## Predictor

For each visible sidewall sample, collect samples above and below it in the same spatial region. Each contributor gets a weight based on:

```text
contributor_opacity * exp(-optical_depth_between_centers)
```

Where `optical_depth_between_centers` integrates Beer-Lambert attenuation through the same-region material stack between the two layer centers.

This makes TD determine the effective neighbor window. There is no fixed "+/- 3 layers" rule. Very opaque filaments stop the window quickly; translucent filaments allow farther layers to influence the apparent color.

## Blend Models

The sidewall predictor supports three modes:

```text
legacy
td_filament_mixer
td_yule_nielsen
```

`legacy` keeps the current behavior.

`td_filament_mixer` uses the TD99 optical weights but blends the weighted colors with the existing FilamentMixer/Mixbox-style path. This is likely the best first default with approximate spool colors because it keeps artist-friendly mixtures such as blue plus yellow tending toward green.

`td_yule_nielsen` uses the same TD99 optical weights but blends as a Yule-Nielsen-style reflectance average:

```text
R = (sum(weight_i * R_i^(1/n)))^n
```

This is useful as an experimental option and should become more meaningful once calibrated printed sample data exists.

## Calibration Position

For now, `filament_colour` remains the approximate spool/profile color. Later, calibration should introduce measured printed sidewall color and measured TD values per filament/profile.

## Implementation Plan

1. Add `filament_transmission_distance` to filament profiles.
2. Add `mixed_filament_sidewall_color_model` to the Color Mixing settings.
3. Add a pure libslic3r predictor in `MixedFilamentPreview`.
4. Preserve legacy behavior whenever the model is `legacy` or any relevant filament TD is unknown.
5. Use the predictor for mixed-filament display colors and preview stripes.
6. Keep strict spatial-region isolation for the first pass.
7. Add tests for TD99 math, legacy fallback, spatial isolation, and blend-model selection.

## Current UI Integration

The reusable predictor feeds:

- mixed-filament display swatches,
- mixed-filament dialog stripe previews,
- Prepare canvas virtual mixed-filament colors,
- Color Painting mixed-filament swatches,
- sliced Preview ColorPrint paths.

The sliced Preview implementation reconstructs spatial regions from rendered G-code paths by bucketing path samples in XY and evaluating the vertical stack inside each bucket. This avoids a single global layer stack, so different blue/magenta regions above the same yellow layer do not contaminate each other in the model. It is still an approximation because the G-code result currently stores resolved physical tool IDs, not the original mixed-region identity for every sidewall span.

Future refinement should carry stable mixed-region or sidewall-sample IDs out of slicing and into `GCodeProcessorResult`. That would let the Preview renderer color exact sidewall spans instead of averaging the reconstructed XY buckets back onto each rendered path.
