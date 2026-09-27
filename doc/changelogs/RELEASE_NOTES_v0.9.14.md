# Snapmaker Orca FullSpectrum v0.9.14

Gradient editing and preview improvements, plus fixes for SML on copied objects and a startup crash.

## Quick Overview

- Fixes copied objects losing **SML / Local-Z subdivision** when reusing an existing object's sliced layers.
- Adds individual **Solid color width** controls for interior gradient colors.
- Smooths Local-Z layer-height transitions around solid-color zones.
- Fixes gradient preview seams and overly thick preview layers on scaled displays.
- Makes **FilamentMixer** blends meet the selected filament colors at their endpoints.
- Fixes an immediate startup crash caused by reading a gradient percentage setting incorrectly.

## Gradient Controls And Slicing

- Select an interior color stop and enter its solid-color width as a percentage of the complete gradient, or drag the square handles below the gradient bar.
- New gradients use a **3%** solid-color width for interior stops. Older recipes without individual widths retain the existing global width setting.
- Limits solid-color widths to the neighboring transition midpoints so adjacent zones do not overlap.
- Gradually increases the dominant filament's Local-Z pass height near a solid-color zone while preserving the fading filament's pass and respecting configured maximum layer heights.
- Uses shared gradient sampling for slicing and previews so stop positions and solid-color zones follow the same rules.
- Saves individual widths in FullSpectrum 3MF projects and the legacy mixed-filament definition format.

## Fixes

- **Copy and paste:** preserves the SML subdivision plan when identical objects share sliced layers. Covers copies made before slicing and copies made after the original has already been sliced, including independent Local-Z heights.
- **Gradient rendering:** removes seams caused by overlapping drawing segments in the gradient editor and blended preview.
- **Layered preview:** corrects layer spacing on scaled displays so the simulated layers are no longer enlarged by an extra DPI scaling step.
- **FilamentMixer endpoints:** removes color jumps between a blend and its pure filament colors while retaining the mixer's nonlinear color transitions.
- **Startup:** handles both percentage and numeric gradient settings correctly, preventing the application from closing immediately during initialization.

## Notes

- Re-slice projects containing copied SML objects to regenerate their toolpaths with the fix.
- **Known issue:** KM/K-S can still show abrupt color changes at solid-color boundaries because its spectral predictions and displayed filament colors are not fully consistent. This release fixes preview rendering; it does not resolve that prediction mismatch.
- Release downloads: [FullSpectrum Releases](https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases).
