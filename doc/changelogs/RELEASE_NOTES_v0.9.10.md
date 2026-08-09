# Snapmaker Orca FullSpectrum v0.9.10

> **Experimental Image-Map Disclaimer:** Image-map features, including **Simple Perimeter Modulation** (one filament per layer) and **Adaptive Localized Cycles**, are experimental. Carefully inspect the sliced preview and validate with a safe test print before relying on their generated geometry, toolchanges, or G-code for production use.

Textured-model image mapping, calibrated mixed-color prediction, multi-filament gradients, a redesigned mixed-filament workflow, and the Snapmaker Orca 2.3.5 updates.

## Acknowledgements

- A huge and special thank you to **Xipit**, who designed and implemented the entire UI redesign shipped in this release. His work created the new physical- and mixed-filament sidebar, filament cards, dedicated Mix/Pattern/Gradient editor, accordion-based workflow, batch manager, recommendations, live previews, gradient controls, HSL color picker, contextual actions, and the many layout, styling, resizing, and flicker improvements that bring the new experience together.
- Special thanks to **Neotko** for his work on the advanced color-painting workflow: the 1x through 8x brush-precision controls, rectangle and polygon projection masks, **Surface paint only** shell confinement, and extra perimeters for painted zones.
- Special thanks to **sentientstardust** for his work on the [OrcaSlicer-ImageMap fork](https://gitlab.com/sentient_stardust/orcaslicer-imagemap), whose ColorSolver approach provided an important foundation for FullSpectrum's continuous image-map color solving.

## Quick Overview

- Adds persistent image maps for textured and vertex-colored OBJ models, with Normal Mix, one-filament-per-layer, and adaptive localized-cycle rendering modes.
- Adds continuous perimeter modulation that changes surface exposure without line-width modulation or Local-Z, with smoother color transitions, support-aware geometry, and UV seam repair.
- Adds selectable FilamentMixer and calibrated KM/K-S color prediction, per-filament transmission-distance and material calibration metadata, and spectral estimation for uncalibrated colors.
- Replaces the mixed-filament workflow with a dedicated Mix/Pattern/Gradient editor, HSL and exact-hex color input, live previews, recommendations, and batch management.
- Adds two-to-four-filament spatial gradients, independent gradient Local-Z cadence, direct multicolor Local-Z, and independent Local-Z layer heights.
- Adds independent surface-bias controls for every component, higher-precision color painting, surface-shell confinement, and extra perimeters in painted zones.
- Adds FullSpectrum 3MF persistence for typed mixed-filament definitions, multi-filament gradients, image-map sources, and embedded texture assets.
- Incorporates Snapmaker filament-sync v2, U1 nozzle/material profiles, top-cover support, device-control updates, wipe-tower improvements, and a broad set of stability and platform fixes.

## Image Maps And Textured Model Import

### Persistent OBJ Image Maps

- Textured OBJ files can now retain their original texture, UV coordinates, vertex colors, or face colors as editable model data instead of permanently baking the source into facet paint.
- The import dialog can use the OBJ/MTL texture, an explicitly selected replacement texture when UVs are available, or OBJ vertex colors.
- Texture assets are embedded in FullSpectrum 3MF projects, so the original external image is not required after saving.
- Image-map assignments, ordered zones, render settings, palettes, stable mixed-filament references, and topology fingerprints persist with the model volume.
- Image-map slicing data is regenerated from the authoritative source when needed and remains available across editor and preview view changes.
- Topology validation prevents an image map from being silently reused after incompatible mesh changes.

### Three Rendering Modes

- **Normal mixed filaments** quantizes the source into conventional printable material regions and maps each region to an existing physical filament or a generated mix.
- **One filament per layer - perimeter modulation** uses one shared physical-filament cadence and continuously samples the original source at the boundary, minimizing image-map-specific toolchanges.
- **Adaptive localized cycles - perimeter modulation** selects a sparse local filament cadence for each quantized region, then uses the original unquantized source to modulate surface exposure inside that region for improved local gamut at the cost of more toolchanges.
- Normal Mix supports automatic palettes up to 32 colors and user-requested palettes up to 256 regions, subject to the 255 printable filament-ID limit.
- Generated-mix choices are previewed from a dry run and committed from the same cached definitions, preventing the final assignment from drifting from the import preview.
- Excess colors in conventional 3MF files can now be mapped to generated mixed filaments instead of being dropped or assigned to nonexistent physical inputs.

### Perimeter Modulation And Preview Quality

- Perimeter modulation moves the printable boundary inward or outward to change the active filament's apparent surface contribution; it does not vary extrusion line width.
- The modulated envelope is shared by walls, overhangs, and support geometry so adjacent geometry follows the same boundary.
- Boundary displacement is resampled, smoothed, slope-limited, and clamped around narrow or acute features.
- A continuous modulation lookup interpolates nearby color solutions, reducing visible geometric bands from small changes in the source image.
- Small UV cracks across internal diagonals of coplanar textured surfaces are stitched while genuine UV-island seams remain intact.
- V2 and adaptive previews show attainable KM/K-S results rather than presenting the photographic source as directly printable output.
- Dense viewport previews use bounded levels of detail, report progress, support cancellation, and preserve generated slice previews across view changes.
- Image-map spectrum cards expose the attainable colors of shared or localized cadences; deleting the map card detaches the image map and restores the object's ordinary material appearance.

For a detailed comparison and limitations, see [FullSpectrum Image-Map Modes](../fullspectrum-image-map-modes.md).

## Mixed-Color Prediction And Calibration

- Adds a sidebar selector between the established **Mixer** preview engine and the new **KM/K-S** spectral engine.
- Adds optional transmission-distance correction for KM/K-S prediction.
- Adds per-filament **Transmission distance** and stable **FullSpectrum material ID** metadata and preserves them in filament presets.
- Uses measured spectral anchors and learned pair residuals for recognized calibrated materials.
- Uses an ICC polynomial reflectance estimate and plain KM/K-S mixing for valid uncalibrated colors instead of falling back directly to RGB interpolation.
- Applies the selected predictor consistently to mixed-filament rows, gradient previews, image-map matching, and attainable-color spectra.
- Adds exact hexadecimal color input alongside the updated filament color dialog and HSL picker.
- Refreshes physical and mixed palettes after filament-color edits and light/dark theme changes.
- Keeps the legacy FilamentMixer engine available for compatibility and comparison.

## Mixed-Filament Editor And Workflow

- Replaces the legacy mixed-filament editing flow with a dedicated dialog containing **Mix**, **Pattern**, and **Gradient** tabs.
- Adds collapsible material, ratio, color, recommendation, pattern, gradient, bias, and preview sections with reduced flicker and more reliable resizing.
- Supports manual-ratio and target-color workflows, live attainable-color previews, a minimum-weight control, and synchronized ratio sliders and text input.
- Adds HSL color selection, exact-hex entry, compact material swatches, collapsed-section previews, and updated color badges.
- Adds recommended 50/50, 34/66, and 33/33/34 mixes plus gradient recommendations.
- Adds a **Manage** dialog for batch-adding recommendations and reviewing or deleting active mixed filaments.
- Adds context and overflow menus, right-click access, current-row highlighting, list counts, and direct color-swatch editing.
- Warns before deleting physical filaments referenced by mixed definitions and refreshes dependent mixed colors when a physical color changes.
- Respects the automatic mixed-filament generation preference instead of recreating disabled automatic rows.
- Adds mixed-filament incompatibility detection, VHL conflict warnings, and a swatch-grid matching workflow.
- Refactors mixed-filament storage around typed weighted blends while keeping legacy row import/export compatibility, stable IDs, tombstones, grouped manual patterns, and physical/virtual remapping.

## Gradients, Local-Z, Bias, And Color Painting

### Multi-Filament Gradients

- Adds spatial gradients containing two to four ordered physical filaments with adjustable stops and live gradient previews.
- Multi-filament gradients transition through adjacent pairs and support a configurable overlap window around internal component joins.
- Gradients automatically enter the Local-Z subdivision path; users no longer have to enable the global **Subdivide all mix layers** option just to activate a gradient.
- Object- or volume-assigned gradients automatically receive the full-domain behavior they need, while painted gradients remain scoped to painted regions unless the global override is enabled.
- Adds an independent **Gradient Local-Z layer height**, decoupling the gradient's color cadence from the normal process layer height.
- Preserves component order, weights, endpoints, and stops in canonical FullSpectrum 3MF data and dual-writes a legacy compatibility projection.

### Local-Z Controls And Solvers

- Replaces the old lower/upper bounds with one **Local-Z minimum sublayer height** and keeps solid 0/100 endpoints solid.
- Adds **Keep first layer unsplit** to protect first-layer adhesion in full-domain subdivision.
- Adds **Apply subdivision to infill** for users who want mixed Local-Z passes inside eligible infill as well as walls.
- Adds an experimental direct multicolor solver for static recipes containing three or more physical filaments, allocating legal passes directly among all active components and carrying ratio error into later layers.
- Adds experimental independent Local-Z layer heights for static direct-multicolor recipes, allowing exact weighted cadences to cross normal process-layer boundaries while respecting each filament's maximum layer height.
- Keeps physical-color and inactive painted regions at their normal nominal height unless they explicitly participate in the selected Local-Z mode.
- Improves gradient seam handling, pass ordering, wipe-tower planning, and configuration invalidation for the new independent cadences.

### Bias And Painting

- Expands surface bias from one pair-level value to an independent signed XY offset for every component in two-color and multicolor recipes.
- Applies the same component-bias representation in previews, ordinary mixed-filament geometry, and perimeter-modulated image maps.
- Adds 1x through 8x brush precision plus rectangle and polygon projection tools to color painting.
- Adds **Surface paint only**, restricting facet-painted side regions to the calculated wall shell instead of allowing them to fill through the solid interior.
- Adds **Extra perimeters in painted zones** from 0 through 8, increasing colored-shell thickness only where the painted filament differs from the parent wall material.
- Prevents same-filament painted zones from producing overlapping duplicate walls.

## FullSpectrum 3MF And Project Compatibility

- Adds a canonical typed FullSpectrum 3MF representation for mixed-filament identities, weighted components, behavior, presentation, assignments, and tombstoned rows.
- Adds stable-reference-first reconstruction so filament reordering does not unnecessarily break mixed definitions or image-map palettes.
- Stores persistent image-map metadata in `/Metadata/fullspectrum/image-maps.json`, embedded RGBA assets under `/Metadata/fullspectrum/assets/`, and mixed definitions in `/Metadata/fullspectrum/mixed-filaments.json`.
- Dual-writes current multi-filament gradients into canonical data and a legacy compatibility projection.
- Keeps legacy compact `mixed_filament_definitions` rows readable and writable during the migration period.
- Makes mixed-filament numeric metadata locale invariant, fixing gradient and bias round trips on systems that use comma decimal separators.
- Improves 3MF persistence for mixed-filament edits, Local-Z settings, image-map source assignments, paint assignments, and generated virtual filaments.

## Snapmaker Devices, Profiles, And Online Features

- Incorporates the Snapmaker Orca v2.3.5-era device and slicer changes through upstream PR #584.
- Adds filament-sync v2 with machine-filament picking, automatic color/material mapping, plate previews, confirmation, scrolling/layout improvements, and follow-up sync fixes.
- Adds machine/filament compatibility rules, nozzle/plate/filament relationships, high-temperature filament metadata, and updated color/material libraries.
- Adds Snapmaker U1 presets for 0.2, 0.6, and 0.8 mm nozzles, matching process profiles, and expanded generic and Snapmaker material profiles.
- Adds U1-specific bed choices and updates profile/resource data for current filament and plate combinations.
- Adds top-cover detection, controls, UI state, notifications, slicing integration, filament-temperature handling, and corrected machine end G-code.
- Adds local printing, machine heartbeat monitoring, and purifier control to the device workflow.
- Adds client/device time synchronization and exposes the software version and local HTTP endpoint to the embedded web application.
- Updates the bundled Flutter/web application to v2.3.25 and refreshes model-station, device-control, guide, preset-binding, and profile resources.
- Improves resource/profile OTA migration and copying, source upgrades, module downloads, URL encoding, HTTP-port conflicts, and old-client/new-profile compatibility.
- Raises the declared minimum printer firmware from 1.0.0 to 1.5.0.

## Slicing, Wipe Tower, And G-code Fixes

- Adds the newer square/filleted wipe-tower path, wall-entry gaps, configurable outer-wall speed, and interface ironing-area support.
- Fixes wipe-tower size-limit handling, total calculations, material/tool ordering, filament-temperature mixing, missing configuration keys, and several top-cover interactions.
- Improves mixed-filament Local-Z integration throughout G-code generation, tool ordering, wipe-tower planning, painted-region rebuilding, and preview.
- Fixes first-layer honeycomb sparse-infill alignment.
- Fixes reported fuzzy-skin, infill-filament, and toolpath-routing regressions.
- Fixes generic support filament type resolution that incorrectly depended on Bambu-specific logic.
- Improves G-code model contours and keeps filament assignments synchronized through slicing and print preprocessing.

## UI, Stability, And Platform Work

- Refreshes the startup splash and FullSpectrum branding.
- Improves low-resolution macOS control layout and click targets.
- Reduces mixed-dialog flicker, preview flicker, stale callbacks, and invalid sidebar references during rebuilds.
- Fixes crashes around GL canvas destruction, plate deletion, selection clearing, network-test threading, filament synchronization, missing extruder data, and invalid `boost::any_cast` conversions.
- Reduces GUI stalls by optimizing texture compression and web-preset handling.
- Fixes MQTT connection handling, server-message backlog behavior, and excessive webview logging.
- Improves Windows packaging with running-app checks, old-version uninstall handling, URL-protocol registration, and consistent FullSpectrum shortcuts.
- Improves macOS and Linux builds, updates Flatpak to GNOME 49 with configurable low-memory parallelism, and adds AppImage dependency policy/auditing to avoid bundling host-specific runtime and graphics libraries.
- Updates CI, dependency caching, resource packaging, Sentry integration, and scheduled build automation.

## Important Notes

- Image mapping, perimeter modulation, direct multicolor Local-Z, independent Local-Z heights, and multi-filament gradients remain experimental and need model-specific slice-preview and printer validation.
- Perimeter modulation is a surface-appearance technique. Detail is limited by nozzle size, boundary sampling, safe displacement limits, the loaded physical colors, and their calibrated gamut.
- One-filament-per-layer mode minimizes image-map-specific toolchanges; other objects, supports, painted areas, and material assignments may still require additional toolchanges on the same layer.
- Adaptive localized cycles generally trade more toolchanges and virtual-filament slots for a better local color basis.
- Surface paint only does not provide strict confinement when beam interlocking is enabled, and very thin features may leave little or no unpainted interior.
- Projects using the new canonical gradients or persistent image maps should be kept in FullSpectrum v0.9.10 or newer; older builds may only see the compatibility projection or may ignore the new data.
- macOS builds from this fork remain unsigned and not notarized.
- Release downloads for this fork are available at https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases.
- Use at your own risk.
