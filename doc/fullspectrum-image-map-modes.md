# FullSpectrum Image-Map Modes

This document explains OBJ texture and vertex-color image mapping and its three rendering modes:

1. **Normal mixed filaments**
2. **One filament per layer - perimeter modulation**
3. **Adaptive localized cycles - perimeter modulation**

## Persistent image-map model

An image map is stored as persistent model data rather than destructively baked into permanent MMU facet paint.

Each mapped model volume may retain:

- embedded RGBA texture assets;
- triangle UV coordinates;
- per-vertex or per-face colors;
- ordered image-map zones;
- palette entries and stable mixed-filament references;
- render settings;
- a topology fingerprint.

The original texture bytes are embedded in the FullSpectrum 3MF package. The external source file is import provenance and is not required after the project is saved.

The topology fingerprint prevents a map from being applied to incompatible geometry. Mesh operations that replace or materially change triangle topology may detach or invalidate the map.

Persistent FullSpectrum data is stored in:

```text
/Metadata/fullspectrum/image-maps.json
/Metadata/fullspectrum/assets/
```

Generated mixed-filament recipes are stored in:

```text
/Metadata/fullspectrum/mixed-filaments.json
```

## Source selection

The OBJ import flow can use:

- a texture referenced by the OBJ/MTL;
- another explicitly selected texture, provided the OBJ has usable UV coordinates;
- OBJ vertex colors;
- face colors where available.

Texture lookup uses the triangle UVs and bilinear image sampling. Vertex colors use barycentric interpolation across each triangle. Texture alpha is composited over the triangle's underlying color.

## Mode comparison

| Mode | Purpose of quantization | Filament cadence | Source used for final surface detail | Expected toolchanges |
| --- | --- | --- | --- | --- |
| Normal Mix | Defines the final printable regions | Per palette region | Quantized palette | Depends on adjacent regions |
| One filament per layer | UI and compatibility fallback | One shared cadence | Original continuously sampled source | Lowest of the three image-map modes |
| Adaptive localized cycles | Selects a sparse local cadence | Per localized region | Original source, solved within the local cadence | Higher, because regions may use different tools on one layer |

## Normal Mix: OBJ texture quantization

Normal Mix intentionally converts the image into a finite set of printable material regions.

### Import process

1. The source texture or vertex colors are sampled.
2. K-means quantization produces the requested palette.
3. Training is bounded for large sources so import remains tractable.
4. Every source sample is still classified, including samples not used for training.
5. Each palette color is matched to a physical filament or a generated mixed-filament recipe.
6. The palette and stable mixed-filament references are attached to the persistent image map.

Automatic quantization uses at most 32 colors. The user may request up to 256 palette regions. Above 32, the importer displays a reminder that the additional regions may not produce exact printable colors because several regions may reuse or approximate the available physical and mixed-filament recipes.

For normal mixed-filament mapping, the **Standard** table lets each quantized color use either an **Existing Filament** or a **Generated Mix**. Generated-mix swatches come from a non-mutating dry run against the loaded physical filaments, existing derived mixes, and the active color engine, so they show predicted attainable colors rather than simply repeating the requested source colors. Recipe details remain available in the swatch tooltip, while numeric color-difference values are intentionally omitted from the table. Choosing **OK** commits the exact cached definitions after verifying that the mixed-filament list has not changed, so the final assignment cannot drift from the import preview.

Palette size is independent of the number of distinct filament IDs. The facet annotation namespace contains 256 states; state 0 is reserved, so physical and mixed filaments may use IDs 1 through 255. Multiple palette entries may resolve to the same filament ID.

### Slicing process

At slice time, the slicer creates a transient adaptively subdivided facet projection. Every sampled leaf is assigned to the nearest palette entry, and the resulting regions are processed as conventional physical or mixed materials.

The transient facets are a derived cache/projection. They do not replace the persistent texture source.

### Consequences

- The palette is the final color resolution for this mode.
- More palette colors can preserve more source variation.
- More palette colors may require more virtual filament definitions and consume the available printable color slots.
- Region boundaries may introduce toolchanges on the same layer.
- **Surface paint only** can restrict the resulting regions to the visible wall shell.

Normal Mix is appropriate when the desired result should behave like ordinary multi-material segmentation.

## One filament per layer: perimeter modulation

This mode uses one shared physical-filament cadence for the complete image map.

### Shared cadence

The generated cadence:

- contains every configured physical filament;
- gives them positive, approximately equal base weights;
- uses normal nominal-layer cadence;
- enables perimeter modulation;
- does not use Local-Z.

Every V2 palette entry points to the same shared mixed-filament definition. Consequently, the image-map regions resolve to the same physical component on a given layer.

“One filament per layer” means one physical component is active for this image-map cadence on that layer. Other objects, supports, unrelated painting, or other material assignments may still cause additional toolchanges in the complete print.

### Continuous color solution

The quantized import palette is not the source of the final V2 slicing weights.

For every sampled boundary point, the slicer:

1. samples the original RGBA texture or interpolated vertex color;
2. finds the closest attainable physical-filament weight vector;
3. resolves the physical component active on the current layer;
4. compares the desired apparent weights with the shared cadence;
5. converts the difference into an inward or outward perimeter displacement;
6. clamps the displacement to a nozzle-derived safety limit.

The color solver predicts mixtures through the configured FullSpectrum KM/K-S color engine and searches them in Oklab space. It uses a dense but finite weight grid; “continuous” means that every continuously sampled source color is solved independently of the import palette, not that the optimizer has infinite precision.

Candidate construction is capped at 250,000 mixtures to prevent excessive memory and preprocessing time.

### Geometry modulation

The active component's apparent surface contribution changes by moving the perimeter:

- a negative offset expands the component outward;
- a positive offset contracts or recesses it.

This is not line-width modulation.

The envelope is modified after XY and elephant-foot compensation but before final wall and support geometry are generated. Walls, overhangs, and supports therefore see the same modulated boundary.

### Requirements and limitations

- `mixed_filament_component_bias_enabled` must be enabled.
- The shared cadence definition must resolve correctly and have perimeter modulation enabled.
- The effect is primarily a surface-appearance technique; it does not reproduce the image throughout the model interior.
- Fine texture detail is limited by boundary sampling, nozzle size, displacement limits, and the physical color gamut.
- The first layer may show the opposite apparent size change from later layers because elephant-foot compensation and active-filament displacement are both applied.

## Adaptive localized cycles: perimeter modulation

Adaptive mode trades more localized toolchanges for a better local physical-filament basis.

### Region and cadence selection

1. The source is quantized into localized color regions.
2. Each region is matched with the existing FullSpectrum KM/K-S recipe search.
3. The result is a sparse cadence containing the physical filaments useful for that region.
4. Regions with sufficiently similar needs may share an existing cadence.
5. Regions already close to a physical filament may use that filament directly.

The user-selected color count controls the number of localized source regions, not necessarily the number of unique generated mixed filaments. Several regions may share one cadence.

The import UI previews:

- the number of adaptive regions;
- the unique mixed-filament cycles;
- directly used physical filaments;
- the attainable KM/K-S color spectrum of each cycle.

Out-of-gamut colors are projected to the closest color attainable by the selected physical components.

### Slicing and modulation

Transient palette segmentation selects the cadence that owns each surface region. Different regions may resolve to different physical components on the same nominal layer, which can introduce localized toolchanges.

Within each region, the perimeter renderer:

1. samples the original unquantized source color;
2. uses a solver restricted to that cadence's physical components;
3. resolves the component active for that local cadence and layer;
4. computes the required surface exposure;
5. moves the perimeter inward or outward.

The quantized palette therefore selects the local physical basis. It is not the final surface color resolution.

Adaptive mode does not use Local-Z subdivision.

### Tradeoffs

Compared with one-filament-per-layer mode:

- it can represent local colors using a more suitable subset of physical filaments;
- it may achieve better local gamut and exposure control;
- it can use several tools on the same nominal layer;
- it consumes more virtual-filament slots;
- it has more complicated purge and toolchange behavior.

Reducing the adaptive region count can help if the generated cycles exceed the 255 printable filament IDs or if a smaller set of regions gives a more stable material match.

## Shared safety and fallback behavior

- Boundary displacement is resampled, smoothed, slope-limited, and clamped around narrow or acute features.
- Persistent maps are validated against their topology fingerprints.
- Stable mixed-filament IDs are resolved before numeric fallback IDs.
- Older V2 projects whose continuous cadence cannot be resolved may fall back to stored palette surface offsets.
- The G-code-time legacy bias shifter skips persistent perimeter-modulated image-map filaments to avoid applying the displacement twice.

## Choosing a mode

Use **Normal mixed filaments** when:

- discrete material regions are acceptable or desired;
- the texture should behave like conventional color painting;
- palette-limited output is acceptable.

Use **One filament per layer - perimeter modulation** when:

- minimizing image-map toolchanges is the priority;
- the model's surface can represent color through controlled perimeter exposure;
- every physical filament can participate in one shared cadence.

Use **Adaptive localized cycles - perimeter modulation** when:

- better local color fidelity is worth additional toolchanges;
- different areas benefit from different subsets of physical filaments;
- a finite set of reusable local cadences is acceptable.

## Validation status

Automated tests cover:

- bounded quantization and full classification;
- OBJ texture and UV import;
- persistent source retention;
- raw source sampling independent of the V2 palette;
- continuous solver behavior;
- transient facet rasterization;
- bounded, corner-safe boundary modulation;
- print-level Normal Mix, V2, and adaptive envelope integration;
- FullSpectrum image-map and texture-asset persistence.

Before release, representative textured models should still be checked in preview and generated G-code. The current tests do not directly assert the total toolchange count for a complete V2 or multi-region adaptive print.

## Implementation references

- [`Format/OBJImageMap.cpp`](../src/libslic3r/Format/OBJImageMap.cpp)
- [`ImageMap/VolumeData.cpp`](../src/libslic3r/ImageMap/VolumeData.cpp)
- [`ImageMap/Sampling.cpp`](../src/libslic3r/ImageMap/Sampling.cpp)
- [`ImageMap/FacetRasterizer.cpp`](../src/libslic3r/ImageMap/FacetRasterizer.cpp)
- [`ImageMap/ContinuousColorSolver.cpp`](../src/libslic3r/ImageMap/ContinuousColorSolver.cpp)
- [`ImageMap/BoundaryModulation.cpp`](../src/libslic3r/ImageMap/BoundaryModulation.cpp)
- [`ImageMap/PerimeterEnvelopeRenderer.cpp`](../src/libslic3r/ImageMap/PerimeterEnvelopeRenderer.cpp)
- [`slic3r/GUI/ObjColorDialog.cpp`](../src/slic3r/GUI/ObjColorDialog.cpp)
- [`tests/libslic3r/test_obj_image_map.cpp`](../tests/libslic3r/test_obj_image_map.cpp)
