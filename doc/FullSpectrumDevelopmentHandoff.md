# FullSpectrum development handoff

This document is the self-contained engineering handoff for the FullSpectrum work completed through **2026-07-22**. It is intended to let another agent continue without access to the original chat.

## Snapshot

- Repository: `OrcaSlicer-FullSpectrum`
- Working branch: `test/imagemap-bias-port`
- Current HEAD: `85d4871e680f901771f3467a74fe78e02142ddbd`
- HEAD subject: `Use continuous color solving for V2 image maps`
- Worktree was clean before this handoff file was added.
- The branch is deliberately a test branch based on the prior FullSpectrum work.

The branch now contains four related bodies of work:

1. corrected and simplified Local-Z mixed-filament subdivision;
2. improved gradient routing and multi-component surface bias;
3. colored OBJ and excess-color 3MF import through generated mixed filaments;
4. persistent texture/vertex image maps, including an experimental one-physical-filament-change-per-layer perimeter-modulation renderer.

## Terminology and behavior

| Term | Meaning in this branch |
| --- | --- |
| Physical filament | A real input filament, numbered from 1 through the configured physical-filament count. |
| Mixed filament | A virtual filament recipe made from two or more physical filaments. It has a stable ID and a compatibility numeric ID. |
| Gradient | A virtual filament whose recipe varies along a domain. Gradients implicitly use Local-Z subdivision; users no longer have to enable the global override just to make a gradient work. |
| Local-Z / subdivision | Splits one nominal layer into smaller Z passes assigned to physical components. The split heights represent requested mixture proportions. |
| Full domain | Lets subdivision operate over the complete eligible object/volume domain instead of only an explicitly painted mixed region. A mixed filament assigned to an entire object or volume implicitly gets this behavior. |
| Normal Mix image map | Quantizes source colors to a finite palette of physical or generated mixed filaments and slices those regions normally. |
| Adaptive localized cycles | Quantizes source colors to choose a sparse KM/KS mixed-filament cadence per region, then continuously samples the original texture to modulate that cadence's perimeter exposure. It trades additional localized toolchanges for better color fidelity. |
| V2 / one filament per layer | Uses one shared physical-filament cadence and represents continuously sampled source colors by expanding or contracting the perimeter for the physical filament active on that layer. It performs no Local-Z subdivision and no line-width modulation. |
| Surface bias | A signed XY offset. Positive values contract/recess a component; negative values expand it outward. |

## 1. Local-Z and gradient work

### Minimum height was simplified

The old Local-Z lower/upper controls were reduced to one meaningful setting:

- config key: `mixed_filament_height_lower_bound`
- label: **Local-Z minimum sublayer height**
- default: **0.06 mm**
- hard configuration minimum: **0.01 mm**

The value is an absolute minimum height for each active sublayer, not a delta around the nominal layer height. For a two-component nominal layer of height `H`, the requested proportional heights are clamped so each active component receives at least the configured minimum. If `H` is too small to accommodate two minimum-height passes, the preview/slicing machinery must avoid manufacturing invalid tiny layers.

Example: at a nominal height of 0.20 mm with a 0.06 mm minimum, a requested 75/25 split would mathematically be 0.15/0.05 mm and is clamped to 0.14/0.06 mm. This means extreme requested percentages cannot be represented exactly at that nominal height. Solid 0/100 or 100/0 endpoints remain solid and should not be forced into two passes.

The core helpers are in `src/libslic3r/MixedFilament/Common.cpp`, with preview equivalents in `src/libslic3r/MixedFilament/Preview.cpp`.

### First layer protection

The boolean config key `dithering_local_z_preserve_first_layer` is exposed as **Keep first layer unsplit** and defaults to `true`. In full-domain subdivision it keeps object layer 0 at its nominal height, preventing a very thin first sublayer from compromising adhesion.

The policy helper is `mixed_filament_local_z_should_subdivide_layer(...)` in `src/libslic3r/MixedFilament/Common.cpp`.

### Implicit gradient routing

The global controls are now overrides, not prerequisites:

- `dithering_local_z_mode`, shown as **Subdivide all mix layers**, extends subdivision to ordinary non-gradient mixed filaments.
- Gradients use the Local-Z subdivision path automatically even when that override is off.
- `dithering_local_z_whole_objects`, shown as **Full domain for all mixes**, extends full-domain handling to other eligible mixed wall regions.
- A gradient/mixed filament assigned as an entire object or volume color automatically uses the full domain it needs.

This implicit behavior must stay scoped. A gradient chosen as the object color must not accidentally make separately painted physical-filament or ordinary mixed-filament zones use Local-Z. Physical painted zones and ordinary mixed painted zones should behave alike unless the user explicitly enables the global overrides.

The relevant policy functions are declared in `src/libslic3r/MixedFilament.hpp`, implemented in `src/libslic3r/MixedFilament/Common.cpp`, and consumed mainly by `PrintApply.cpp`, `PrintObject.cpp`, and `PrintObjectSlice.cpp`.

### Two-color ratio behavior

The Local-Z ratio is now based on physical sublayer height. A requested ratio is first applied directly to the nominal layer height and then constrained by the absolute minimum above. This addresses the behavior reported in FullSpectrum issue #92, where a 25/75 request could require a very different UI slider value to obtain the intended printed height ratio.

## 2. Mixed-filament bias

Mixed-filament surface bias was generalized from a single “one component versus all others” value to independent per-component offsets.

Completed behavior:

- two-component and three-or-more-component recipes are supported;
- each component can be expanded or contracted independently;
- normalization and legacy compatibility are handled by the mixed-filament definition/resolver layer;
- the dialog preview changes stripe thickness in proportion to the requested physical offset;
- preview scaling is bounded so a value such as 0.33 mm does not make one layer appear orders of magnitude thinner than another;
- the same offset representation is reused by V2 perimeter image mapping.

Important sign convention:

- negative offset = expand that active filament at the surface;
- positive offset = contract/recess it;
- more desired apparent contribution therefore normally produces a more negative/outward offset for the active component.

Important files:

- `src/libslic3r/MixedFilament/Definition.cpp`
- `src/libslic3r/MixedFilament/Display.cpp`
- `src/libslic3r/MixedFilament/Resolver.cpp`
- `src/slic3r/GUI/MixedFilamentDialog.cpp`
- `src/slic3r/GUI/MFDPreviewAccordion.cpp`
- `tests/libslic3r/test_mixed_filament.cpp`

The print setting `mixed_filament_component_bias_enabled` gates surface-bias application. The persistent V2 renderer currently also requires this setting to be enabled in `PerimeterEnvelopeRenderer::create()`.

## 3. Mixed-filament and gradient persistence in 3MF

Multi-filament gradients are dual-written:

- the canonical FullSpectrum representation is stored in `/Metadata/fullspectrum/mixed-filaments.json`;
- a legacy compatibility projection is also written for older FullSpectrum readers.

Stable mixed-filament IDs are authoritative. Numeric filament IDs remain fallbacks for legacy files and project remapping. New code should resolve stable IDs first and must preserve them when physical or virtual filament ordering changes.

The format code lives under `src/libslic3r/Format/FullSpectrum3mf/`. Supporting design notes already exist in:

- `doc/fullspectrum-3mf-packaging.md`
- `doc/fullspectrum-3mf-vnext-schema.md`
- `doc/fullspectrum-3mf-vnext-implementation.md`

## 4. Colored OBJ and excess-color 3MF import

### Colored OBJ import

OBJ import now offers a color/import dialog instead of blindly collapsing source colors. The user can:

- choose a quantized color count;
- inspect and remap the quantized colors;
- prefer physical matches;
- generate mixed-filament matches;
- open the explicit **Image map...** / **Image source...** flow;
- use a detected OBJ/MTL texture, choose another texture file, or use OBJ vertex colors;
- choose **Normal mixed filaments**, **One filament per layer - perimeter modulation**, or **Adaptive localized cycles - perimeter modulation**.

Normal mixed-filament recipes are deliberately preferred before bias is introduced. Bias is used for residual color accuracy and transitions, not as the only mechanism. The earlier prototype behavior—many equal 1/2/3/4 recipes distinguished only by bias—should not return.

OBJ parsing/import code:

- `src/libslic3r/Format/OBJ.cpp` and `OBJ.hpp`
- `src/libslic3r/Format/objparser.cpp`
- `src/libslic3r/Format/ImportedTexture.cpp` and `.hpp`
- `src/libslic3r/Format/OBJImageMap.cpp` and `.hpp`
- `src/libslic3r/Model.cpp`
- `src/slic3r/GUI/ObjColorDialog.cpp` and `.hpp`
- `src/slic3r/GUI/MixedColorMatchHelpers.cpp` and `.hpp`

### 3MF files with more colors than physical inputs

When importing a conventionally colored 3MF that references more colors than the configured physical filament count, the extra colors are mapped to generated mixed filaments instead of being discarded or forced onto nonexistent physical inputs. The import/remapping entry points are in `Model.cpp`, `Model.hpp`, and `slic3r/GUI/Plater.cpp`.

## 5. Persistent image-map architecture

The central design decision is that an image map is model data, not permanent facet paint.

`ModelVolume` owns an immutable/shared `ImageMap::VolumeData` containing:

- embedded RGBA texture assets;
- per-triangle texture UVs, vertex colors, or face colors;
- ordered zones with render settings;
- palette entries with stable mixed-filament IDs and fallback filament IDs;
- a topology fingerprint.

The source texture bytes are embedded, so an external image file is import provenance only and is not required after import. Replacing the volume mesh clears the data; operations that preserve the exact triangle topology may retain it after fingerprint validation.

The code is separated into maintainable layers:

1. `ImageMap/VolumeData.*` owns the serializable, validated domain model.
2. `ImageMap/Sampling.*` samples raw source RGBA and a palette entry at a surface point.
3. `ImageMap/FacetRasterizer.*` generates transient palette-based facet data for Normal Mix and viewport display.
4. `MixedFilamentManager` owns recipes and color prediction; image-map code does not duplicate the FullSpectrum color engine.
5. `ImageMap/BoundaryModulation.*` is a geometry-only signed-boundary-displacement operator.
6. `ImageMap/PerimeterEnvelopeRenderer.*` connects sampled colors, layer cadence, continuous weights, and print geometry.
7. `Format/FullSpectrum3mf/*` serializes/deserializes the persistent domain.

See `doc/FullSpectrumImageMapArchitecture.md` for the smaller architecture contract that accompanied the implementation.

### 3MF representation

Persistent image maps are stored in the FullSpectrum 3MF extension as:

- `/Metadata/fullspectrum/image-maps.json`
- content-addressed RGBA8 assets below `/Metadata/fullspectrum/assets/`
- the corresponding package content types and relationships;
- mixed recipes in `/Metadata/fullspectrum/mixed-filaments.json`;
- legacy facet-paint data where a compatibility projection is needed.

The schema identifier is currently `fs.image-maps.v1`; in-memory `VolumeData` is schema version 1.

For a texture with tens of thousands of colors, the original pixels are not expanded into tens of thousands of virtual filament definitions or stored solver-weight records. The texture asset remains the compact color source. The palette is a UI/Normal-Mix/fallback representation.

## 6. The three image-map render modes

### Normal Mix

Normal Mix intentionally remains palette-quantized:

1. source texture/vertex colors are sampled and quantized during import;
2. each palette target is matched to a physical filament or a generated mixed-filament recipe;
3. the palette/stable IDs are attached to the persistent map;
4. transient facet segmentation is derived for viewport display and slicing;
5. the resulting mapped regions use normal mixed-filament behavior.

This mode can require many generated virtual filaments when a large palette is requested. It is appropriate when spatial regions should be printed as conventional physical/mixed materials.

### Adaptive localized cycles / `AdaptiveLocalizedCycles`

This mode deliberately accepts more toolchanges than V2 in exchange for local color fidelity:

1. source colors are quantized into the user-selected palette;
2. each palette color is matched with the existing KM/KS predictor and sparse recipe search;
3. every generated definition uses a normal mixed-filament layer cadence and enables perimeter modulation;
4. transient image-map segmentation assigns those mixed definitions to their corresponding surface regions, so different regions may resolve to different physical filaments on the same nominal layer;
5. the perimeter envelope renderer samples the original texture at each boundary point, compares the desired apparent KM/KS weights with that region's base recipe, and offsets the currently active component's perimeter;
6. no Local-Z subdivision is used.

The result is a finite set of localized cadences, not a unique cadence for every source pixel. Increasing the quantized-color count gives the slicer more base recipes at the cost of additional definitions and potentially more toolchanges. Perimeter displacement still uses full-resolution source samples within each palette region, so the palette selects the local filament cycle rather than becoming the final color resolution. The viewport uses the optical source-color preview, while the sidebar exposes the generated mixed definitions that actually drive the regional cadences.

The persistent 3MF render-mode value is `adaptive_localized_cycles`. The map render mode selects adaptive behavior; its referenced definitions use `behavior.surface_bias.perimeter_modulation` and must not opt into Local-Z.

### One filament per layer / `PerimeterModulationV2`

Only this mode uses the continuous, full-resolution source-color solver.

Its invariants are:

- one shared cadence determines which physical filament is active on a layer;
- all physical components must appear with positive, approximately equal base cadence weights;
- `behavior.surface_bias.perimeter_modulation` must be true on that cadence definition;
- source color is sampled along the model boundary during slicing;
- the active filament's surface share is adjusted by moving the perimeter, not by varying line width;
- no Local-Z subdivision is used;
- support/overhang geometry consumes the same modulated envelope as perimeter generation;
- the old G-code-time bias shifter skips persistent V2 palette filaments, preventing double displacement.

The imported quantized palette remains as compact fallback metadata, but every V2 palette entry now points to the same shared cadence definition. It is **not** the source of slicing weights when the continuous solver and cadence resolve successfully. Consequently, 50,000 distinct image colors are not reduced to palette colors for V2 slicing: each sampled raw RGBA value is independently matched to a physical-filament weight vector at slice time.

The V2 viewport now renders a transient adaptively subdivided mesh with colors sampled directly from the persistent texture or vertex-color source. It no longer previews the quantized fallback palette. In the mixed-filament sidebar, the implementation cadence is hidden and the image map is represented by one non-editable spectrum card named after its object. Deleting that card detaches the persistent image map, restores the object's ordinary base-filament appearance, and removes cadence definitions that are no longer referenced elsewhere.

## 7. Continuous V2 color solver

`src/libslic3r/ImageMap/ContinuousColorSolver.*` adapts the ColorSolver approach from `sentient_stardust/orcaslicer-imagemap`, commit `1ff08f86146450141cf18af14af884ebcaa68092`. The source retains the AGPL-3.0-or-later attribution header.

Construction:

1. collect each configured physical filament's sRGB hex value, transmission distance, and FullSpectrum material ID;
2. enumerate dense integer compositions of a fixed total number of units;
3. predict every candidate mixture through `MixedFilamentManager::blend_color_multi()`, so the active FullSpectrum K/S or fallback predictor remains authoritative;
4. convert predicted colors to Oklab;
5. build a read-only KD tree.

Candidate resolution follows the upstream component-count policy:

- 2–4 physical filaments: 40 units, or 2.5% increments;
- 5 physical filaments: 24 units;
- 6 physical filaments: 20 units;
- more than 6: 12 units.

The name “continuous” means the solver handles each continuously sampled source color without first mapping it to the import palette. The returned weights still come from this finite dense candidate grid; it is not an analytic infinite-precision optimizer.

Candidate count is the stars-and-bars combination count `C(units + components - 1, components - 1)`. Construction is rejected above 250,000 candidates to prevent pathological memory/runtime growth. The solver is built once per `PerimeterEnvelopeRenderer` before the TBB layer loop and queried read-only from worker threads.

For each boundary sample, the solver:

1. samples the original `SurfaceSample.color` using bilinear texture interpolation or barycentric vertex-color interpolation;
2. finds the nearest predicted candidate using upstream-style Oklab axis weighting and a dark-color penalty;
3. resolves the cadence's active physical component for the current layer;
4. converts the desired apparent weights to component surface offsets with `mixed_filament_surface_offsets_for_apparent_weights(...)`;
5. applies the active component's signed offset, clamped to the nozzle-derived maximum.

If the continuous cadence cannot be found—primarily for older V2 projects—the renderer falls back to the palette entry's pre-baked mixed-filament surface offset.

## 8. Boundary and support integration

`BoundaryModulation` performs bounded resampling, circular corner smoothing, bidirectional slope limiting, narrow-feature clamping, corner-safe motion, and polygon normalization.

`PerimeterEnvelopeRenderer` runs after XY/elephant-foot compensation and before final `Layer::make_slices()`. It:

- contracts each material region against the modulated envelope;
- assigns outward expansion strips back to the most appropriate region;
- updates `Layer::lslices` and the uncompensated first-layer envelope through the same renderer.

This ordering is important. The modulated envelope becomes the geometry seen by wall, overhang, and support stages, avoiding an outward G-code-only shift that support generation never saw.

## 9. Critical invariants and common traps

1. Do not bake persistent image maps destructively into legacy MMU facet paint. Derived facets are transient caches/projections.
2. Do not use quantized palette colors as V2 or Adaptive solver input. Use raw `SurfaceSample.color`; Adaptive uses the palette entry only to select its local mixed cadence.
3. Do not make Normal Mix continuous implicitly. Its semantics are palette-based region assignment.
4. Do not use line-width modulation for V2. The requested technique is perimeter geometry modulation.
5. Do not add Local-Z subdivision to either perimeter-modulation mode. V2 uses one shared cadence; Adaptive uses region-specific nominal-layer cadences and accepts the resulting localized toolchanges.
6. Keep `BoundaryModulation` free of UI, OBJ, 3MF, and color-science dependencies.
7. Keep color prediction in `MixedFilamentManager`; do not implement a second RGB averaging path in image-map code.
8. Stable mixed-filament IDs are primary. Numeric IDs are compatibility fallbacks.
9. Validate topology fingerprints before attaching or using a persistent map.
10. Keep implicit gradient/full-domain/adaptive behavior scoped to the relevant definition or object assignment. Painted physical and ordinary mixed zones must not be pulled into that machinery accidentally.
11. Preserve first-layer protection and the 0.06 mm Local-Z default.
12. Preserve upstream attribution when changing adapted solver logic.
13. Do not modify `deps/` or `deps_src/` as part of this feature unless explicitly mirroring an upstream version.

## 10. File map

| Area | Primary files |
| --- | --- |
| Mixed definitions and behavior | `src/libslic3r/MixedFilament.hpp`, `src/libslic3r/MixedFilament/*` |
| Local-Z configuration | `src/libslic3r/PrintConfig.cpp`, `PrintConfig.hpp`, `PrintObjectSlice.cpp` |
| Gradient/region routing | `src/libslic3r/PrintApply.cpp`, `PrintObject.cpp`, `PrintObjectSlice.cpp` |
| Bias UI | `src/slic3r/GUI/MixedFilamentDialog.*`, `MFDPreviewAccordion.*` |
| OBJ parsing and texture import | `src/libslic3r/Format/OBJ.*`, `OBJImageMap.*`, `ImportedTexture.*`, `objparser.cpp` |
| OBJ import UI and color matching | `src/slic3r/GUI/ObjColorDialog.*`, `MixedColorMatchHelpers.*`, `Plater.cpp` |
| Persistent image-map model | `src/libslic3r/ImageMap/VolumeData.*`, `src/libslic3r/Model.*` |
| Surface sampling | `src/libslic3r/ImageMap/Sampling.*` |
| Palette rasterization | `src/libslic3r/ImageMap/FacetRasterizer.*` |
| Adaptive localized recipe generation | `src/slic3r/GUI/MixedColorMatchHelpers.*`, `src/libslic3r/MixedFilament/*` |
| Continuous V2 solve | `src/libslic3r/ImageMap/ContinuousColorSolver.*` |
| V2 geometry | `src/libslic3r/ImageMap/BoundaryModulation.*`, `PerimeterEnvelopeRenderer.*` |
| Slice integration | `src/libslic3r/PrintApply.cpp`, `PrintObjectSlice.cpp`, `GCode.cpp` |
| Viewport preview | `src/slic3r/GUI/3DScene.cpp`, `3DScene.hpp` |
| FullSpectrum 3MF | `src/libslic3r/Format/FullSpectrum3mf/*`, `src/libslic3r/Format/bbs_3mf.cpp` |
| Tests | `tests/libslic3r/test_mixed_filament.cpp`, `test_obj_image_map.cpp`, `test_3mf.cpp`, `tests/fff_print/test_print.cpp` |

## 11. Commit map

These are the feature commits on this line, oldest to newest:

| Commit | Purpose |
| --- | --- |
| `3eaca5dda7` | Simplify Local-Z subdivision controls, default minimum, first-layer policy, and scoped automatic gradient/full-domain routing. |
| `b95573413e` | Add independent component bias controls and corrected previews for multi-component mixes. |
| `b7cb5e3ca2` | Dual-write multi-filament gradients to canonical and legacy FullSpectrum 3MF data. |
| `7daa102723` | Import quantized OBJ colors as generated mixed filaments. |
| `1530653885` | Map excess 3MF colors to generated mixed filaments. |
| `450588fa44` | Initial OBJ texture/image-map prototype using surface bias. |
| `8ec18aa488` | Prefer ordinary color recipes before applying bias for OBJ targets. |
| `d5b73a07eb` | Add one-change-per-layer image mapping using perimeter modulation. |
| `a4842b0096` | Add explicit image-map button/source selection, external texture choice, and vertex-color choice. |
| `942c5e6a72` | Introduce the persistent image-map model domain. |
| `30fe2dbd1d` | Persist image-map sources/assets in FullSpectrum 3MF. |
| `153ff923ac` | Retain original OBJ image-map sources after import. |
| `22f80666be` | Sample persistent maps during slicing. |
| `69a90f0d31` | Add corner-smoothed, support-aware V2 perimeter envelope modulation. |
| `4e3ad69859` | Fix persistent map assignment, painted-extruder discovery, and viewport preview. |
| `85d4871e68` | Use the upstream-style continuous solver for raw V2 source colors. |

## 12. Tests and build notes

Focused tests that passed at current HEAD:

```powershell
build_tests\tests\libslic3r\Release\libslic3r_tests.exe "[ImageMap]" --reporter compact
build_tests\tests\fff_print\Release\fff_print_tests.exe "[PrintObject][ImageMap]" --reporter compact
```

Results:

- `[ImageMap]`: 6 test cases, 78 assertions;
- `[PrintObject][ImageMap]`: 1 test case, 38 assertions.

Standard project build flow:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Snapmaker_Orca --config Release --parallel
cmake --build build --target tests --config Release --parallel
ctest --test-dir build --output-on-failure
```

Windows-specific observation: unrestricted MSVC `/MP` over-spawned during the large GUI build and produced `C1083` permission-denied and `D8040` compiler-process errors. A conservative workaround is:

```powershell
$env:CL='/MP1'
cmake --build build-dev-release --target Snapmaker_Orca --config Release --parallel 1
```

`/MP4` may be a reasonable faster bounded alternative. The modified `libslic3r` and focused test executables compiled cleanly, but a complete GUI Release build/link did not finish within the repeated ten-minute command windows used during development. It still needs a final uninterrupted build confirmation.

## 13. Manual verification still recommended

Use a real UV-textured OBJ and verify all three modes independently:

1. Click **Image map...** during OBJ color import.
2. Verify the detected MTL texture can be used.
3. Choose a different external texture and verify it replaces the detected source.
4. Verify vertex colors can be selected where present.
5. In Normal Mix, confirm multiple generated mixed filaments map visibly onto the model and appear in the sliced regions.
6. In Adaptive Localized Cycles, confirm palette regions receive sparse KM/KS recipes and that differently colored regions can use different physical filaments on the same nominal layer.
7. Inspect Adaptive geometry and G-code to confirm perimeter displacement follows the original texture, its extra toolchanges are localized, and it creates no Local-Z sublayers.
8. In V2, enable mixed-filament bias, confirm the shared cadence uses all physical filaments, and inspect sliced geometry at several layers.
9. Confirm there is no Local-Z layer subdivision in V2 and no second G-code-time displacement.
10. Confirm outward V2 modulation is reflected in support/overhang behavior.
11. Save and reopen the project; verify texture colors, UVs, render mode, stable palette IDs, and generated recipes survive the 3MF round trip.
12. Inspect generated G-code/tool changes to verify the one-filament-per-layer cadence is retained in V2.

## 14. Known limitations and good next steps

Highest-value follow-up work:

1. Finish a full GUI Release build/link and perform the manual checks above.
2. Improve V2 viewport rendering so it previews the original texture/continuous apparent-color solve instead of the quantized import palette.
3. Add a spatial regression test using a gradient texture, proving that opposite boundary points on the same layer receive different continuous offsets.
4. Add an end-to-end 3MF round-trip test combining embedded texture data, continuous V2 solving, and stable-ID remapping.
5. Benchmark solver construction for four or more physical filaments with the full K/S predictor.
6. Consider a shared solver cache keyed by physical colors, transmission distances, material IDs, and color-engine/calibration state when multiple mapped objects use the same inputs.
7. If solver resolution becomes user-configurable, persist an explicit solver algorithm/version/settings record for reproducibility. Current resolution is an implementation detail.
8. Only after viewport rendering is independent of the import palette, consider replacing the many V2 palette proxy recipes with a single shared cadence definition. Doing this now would make the current viewport look uniform.

## 15. How to resume safely

Before editing:

```powershell
git status --short
git branch --show-current
git log --oneline -20
```

Then:

1. preserve any user changes in a dirty worktree;
2. read `doc/FullSpectrumImageMapArchitecture.md` and the relevant files from the map above;
3. decide explicitly whether a change targets Normal Mix, Adaptive Localized Cycles, V2, or more than one mode;
4. maintain the separation between persistent sources, sampling, color solving, and geometry rendering;
5. add focused deterministic tests before doing a full GUI build;
6. run `clang-format` only on files actually changed;
7. keep each logically separate task in its own concise commit if the user asks for commits.

## References used during the design

- FullSpectrum issue #92: <https://github.com/ratdoux/OrcaSlicer-FullSpectrum/issues/92>
- OrcaSlicer-ImageMap: <https://gitlab.com/sentient_stardust/orcaslicer-imagemap>
- Color3DP paper: <https://cdfg.mit.edu/assets/files/Color3DP_compressed.pdf>
- Layered color mixing paper: <https://arxiv.org/pdf/1805.01375>

