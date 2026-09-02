# Snapmaker Orca FullSpectrum v0.9.12

> **Experimental Image-Map Disclaimer:** Image projection, Simple Perimeter Modulation, Adaptive Modulation, and Adaptive Local-Z height modulation remain experimental. Carefully inspect the sliced preview and validate with a safe test print before relying on their generated geometry, variable-width extrusion, non-planar motion, toolchanges, or G-code for production use.

Interactive image projection, higher-detail perimeter modulation, expanded KM/K-S calibration, broader color-mesh import, a refined mixed-filament workflow, and the Snapmaker Orca 2.3.6 updates.

## Quick Overview

- Adds an interactive **Image Projection** gizmo for placing PNG and JPEG images directly on connected model surfaces.
- Adds live, printable-color image-map previews that react to image processing, color-model, sampling, and smoothing changes without closing the settings dialog.
- Adds Standard, Fine, Maximum, and Ultra perimeter sampling down to **0.02 mm**, plus a maximum-detail mode that bypasses broad path smoothing.
- Adds configurable Gaussian reconstruction, two broad smoothing passes, tone gamma, overhang contrast, exposure, image contrast, saturation, and edge boost.
- Adds four perimeter mechanics: reference wide path, printable-width path, hybrid path and width, and image-controlled variable width.
- Expands the KM/K-S engine with measured triple- and quadruple-mixture corrections trained from the FullSpectrum measurement set.
- Adds textured and colored FBX, glTF, GLB, Collada, PLY, and 3DS import through Assimp.
- Refines mixed-filament naming, cards, management, deletion, and transfer workflows.
- Incorporates Snapmaker Orca 2.3.6, updated profiles and web resources, and additional performance, stability, privacy, and device fixes.

## Interactive Image Projection

- Adds a dedicated toolbar tool for projecting a selected PNG or JPEG onto an existing model without requiring pre-authored UV coordinates.
- The first surface click places the image; later clicks move it to another surface location.
- Click-dragging the projected image follows the connected model surface while suppressing camera orbit for that gesture.
- Adds on-model controls for moving, resizing, proportional scaling, and rotation, including a two-arrow rotation handle.
- Adds numeric width, height, horizontal and vertical offset, rotation, projection depth, and surface-angle controls.
- Supports linked aspect ratio, horizontal and vertical flips, and one-click 90-degree rotation in either direction.
- Restricts projection to the clicked connected surface and preserves the selected background color outside the image bounds.
- Restores the visible projected-image overlay after the Snapmaker merge and separates its shader from the ordinary image-map texture preview.
- Refreshes the toolbar icon to match the existing FullSpectrum and Orca visual language.

## Color-Mesh Import

- Adds an Assimp-backed import path for FBX, glTF, GLB, Collada/DAE, PLY, and 3DS models.
- Preserves embedded or referenced textures, UV coordinates, vertex colors, and face colors when the source format provides them.
- Normalizes the unit conventions used by FBX, glTF, and Collada while keeping PLY aligned with the slicer's normal Z-up mesh workflow.
- Routes imported texture and color data into the same persistent image-map setup used by textured OBJ models.
- Keeps embedded image assets and image-map metadata available when the project is saved as a FullSpectrum 3MF.

## Perimeter-Modulation Quality And Controls

### Sampling And Image Processing

- Adds four horizontal sampling presets: **Standard (0.16 mm)**, **Fine (0.08 mm)**, **Maximum (0.04 mm)**, and **Ultra (0.02 mm)**.
- Adds **Maximum detail**, which disables both broad path-smoothing passes while retaining Gaussian reconstruction and the printable slope limiter.
- Adds independent controls for Gaussian reconstruction and the two broad smoothing passes.
- Adds source-image exposure, contrast, saturation, and edge-boost controls before color solving.
- Adds tone gamma and overhang contrast controls for tuning midtones and the separation between filament contributions.
- Uses compact per-sample normalization and continuous source sampling to retain more small text, reflections, shadows, and high-frequency detail.
- Stores the processing and quality controls with the persistent image map so saved projects reproduce the selected result.

### Live Printable-Color Preview

- Rebuilds the Prepare-tab texture as an attainable-color prediction rather than displaying the photographic source unchanged.
- Updates the prediction while the image-map dialog remains open when processing, smoothing, detail, filament, or color-model settings change.
- Simulates Gaussian reconstruction, broad path smoothing, tone gamma, overhang contrast, and the source-image processing stack in the texture prediction.
- Raises the interactive preview budget to approximately **2 megapixels**, with a higher source-preview ceiling for normal viewing.
- Debounces repeated edits, cancels superseded work, and prevents stale preview jobs from replacing newer results.
- Fixes progress jobs that could remain displayed at 2%, 10%, or 99% after a setting change.
- Keeps completed image-map previews available when switching between Prepare and Preview.

### Printable Perimeter Mechanics

- Adds **Reference wide path**, which keeps the configured maximum-width bead and moves its path to control exposure.
- Adds **Printable-width path**, which uses a practical fixed-width carrier and moves the path instead of forcing the full configured width everywhere.
- Adds **Hybrid path + line width**, which shares modulation between printable line-width reduction and path displacement.
- Adds **Image-controlled width**, which keeps the inner edge stable and varies extrusion width from the configured minimum to maximum only where the image requires it.
- Lowers the default **Image-map maximum outer wall width** from 0.95 mm to **0.75 mm** to reduce over-extrusion, nozzle drag, and poor surface quality on typical 0.4 mm setups.
- Preserves the requested wall-loop count and avoids creating a separate image-map shell with duplicate inner perimeters.
- Regenerates walls from one coherent modulated envelope so perimeters, overhangs, infill ownership, and adjacent regions agree on the same boundary.
- Improves corner handling, loop closure, thin-wall retention, and preview joins for highly modulated paths.
- Keeps the slope limiter and nozzle-derived displacement bounds active across all perimeter mechanics.

## Simple And Adaptive Image-Map Slicing

- Simple Perimeter Modulation now uses a strict equal cadence across its selected physical filaments while continuously varying their visible surface exposure.
- Adds automatic filament selection, an exact filament-count selector, and manual selection of the physical filaments used by the shared cadence.
- Adds **Apply image cadence to the whole object**, allowing walls, top and bottom surfaces, and infill to follow the image's one-filament-per-layer sequence.
- Adaptive Modulation creates localized physical-filament cycles and reuses compatible cycles across source regions.
- Adaptive perimeter mode uses zone-local ordinary-layer recipes without accidentally entering the general Local-Z subdivision path.
- Adds an experimental Adaptive **Local-Z height modulation** mode that keeps the XY perimeter centerline fixed and varies nozzle Z and deposited bead height along the path.
- Caps an Adaptive Local-Z component at 0.32 mm and redistributes the remaining cycle height among its sibling components.
- Improves tool ordering, G-code preview data, and wipe-tower planning for image-map cadence and adaptive passes.
- Keeps complete loops connected and removes duplicate inner-perimeter fragments at adaptive-region boundaries.

## Color Prediction And KM/K-S Calibration

- Keeps the image-map color engine selectable between **KM/K-S physical mixing** and the built-in **FilamentMixer** model.
- Uses the same closest-mix search and Oklab SoftCap4Dark4 perceptual score for both selectable engines.
- Extends the KM/K-S residual model beyond pairs with measured triple- and quadruple-mixture correction fields.
- Trains the checked-in higher-order profile from 391 compatible SCE, black-backed measurements: 162 pair samples, 120 triple samples, and 109 quadruple samples.
- Adds calibrated corrections for 12 triple combinations and 3 quadruple combinations across three four-material families.
- Makes every higher-order correction vanish on its lower-dimensional boundaries, preserving measured pure and pair behavior.
- Selects regularization by leave-one-out validation to avoid unstable exact interpolation between sparse higher-order measurements.
- Handles the split Panchroma SCE/SCI export explicitly and uses only the compatible SCE measurements.
- Keeps unmeasured material families and cross-family combinations on the calibrated pair model or ordinary KM/K-S fallback.
- Removes the third-party Pigment Painter solver path; release builds use only FullSpectrum's KM/K-S engine or FilamentMixer.

For the equations, measurement counts, and regeneration workflow, see [FullSpectrum KM/K-S Color Prediction](../fullspectrum-km-ks-model.md).

## Mixed-Filament Workflow Refinements

- Adds descriptive mixed-filament names based on the predicted color while retaining stable identities for project persistence.
- Uses consistent alphabetic numbering for mixed filaments throughout cards, dialogs, lists, and assignments.
- Adds **Transfer to** and **Merge with** actions for reorganizing filament assignments.
- Adds a dedicated deletion dialog that can transfer affected color and painting data before removing a filament.
- Improves physical and mixed filament cards, swatch rendering, visibility controls, row highlighting, and light/dark theme behavior.
- Improves Add/Edit Mix and batch-management resizing, layout, and state retention on Windows.
- Keeps Match-mode state when switching editor modes and respects the automatic-generation preference.
- Strengthens mixed-filament remapping when physical filament counts grow, shrink, or are reordered.

## Snapmaker Orca 2.3.6 And Platform Work

- Updates the Snapmaker Orca base version to 2.3.6 and refreshes the bundled web application and printer resources.
- Adds batch mixed-filament color matching with manual and recommended modes.
- Adds printer timelapse export to the local computer and a **Fit all** camera action.
- Adds a Cool Steel Plate option for Snapmaker U1 with compatibility gating and refreshes U1 material and nozzle profiles.
- Uses the highest required bed temperature across all active filaments in a mixed print.
- Blocks slicing when an active filament has a zero flow ratio and identifies the affected filaments.
- Adds runtime memory guards for very large 3MF projects, enables 3MF files larger than 2 GB, and parallelizes profile loading.
- Keeps Snapmaker login tokens refreshed and reduces privacy-sensitive Sentry reporting.
- Guards direct LAN printer requests and hardens local HTTP handling.
- Fixes missing-G-code loading, filament-sync matching, temperature-mixing validation, and several device and webview issues.
- Hardens the Windows release and localization scripts and improves Flatpak builds without precompiled headers.
- Fixes a process-tab crash caused by widget-only option rows.

## Important Notes

- Image mapping remains a surface-appearance technique. Detail and gamut are limited by layer height, nozzle size, sampling distance, safe displacement and extrusion-width limits, loaded filament colors, and their calibration quality.
- The 0.02 mm Ultra value is the horizontal image-sampling interval along the perimeter; it is not a request for a 0.02 mm print layer height.
- Variable-width modes change extrusion volume while keeping the layer's nominal Z unchanged. Adaptive Local-Z height modulation is the mode that intentionally changes Z during extrusion.
- The 0.75 mm default maximum outer-wall width is safer for common 0.4 mm nozzles, but printer, material, flow, speed, temperature, and cooling limits still require validation.
- Adaptive Local-Z emits simultaneous XYZ extrusion and should be used only with motion control and firmware that handle the generated non-planar moves correctly.
- The in-app FullSpectrum color-calibration workflow is not exposed in this release while its print and photo-registration process is still being refined.
- Projects using the new projection, processing, modulation, or adaptive Local-Z metadata should be kept in FullSpectrum v0.9.12 or newer.
- macOS builds from this fork remain unsigned and not notarized.
- Release downloads for this fork are available at https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases.
- Use at your own risk.
