# FullSpectrum image-map architecture

## Purpose

Image maps are editable model data, not imported paint. The original surface
source (texture/UVs, vertex colours, or face colours) remains authoritative in
the model and in 3MF. Slicing derives printable regions or perimeter offsets
from that source for the current palette and print settings.

This deliberately separates four concerns:

1. **Model data** (`ImageMap::VolumeData`) owns validated, triangle-aligned
   sources, zones, texture assets, and stable palette references.
2. **Sampling** (`ImageMap::SurfaceSampler`) turns a point on a model volume
   into a source colour and palette entry. It has no UI, file-format, or G-code
   dependencies.
3. **Colour recipes** remain owned by `MixedFilamentManager` and the selected
   FullSpectrum colour engine. Image maps reference recipes by stable ID and do
   not duplicate colour-science logic.
4. **Rendering** consumes samples. Normal-mix rendering creates a transient
   segmentation snapshot during slicing. Perimeter V2 creates one modulated
   geometry envelope that is shared by perimeter and support/overhang logic.

## Ownership and lifetime

- `ModelVolume` owns `VolumeData`, because UVs and per-corner colours are tied
  to that volume's triangle topology.
- A topology fingerprint is stored and validated. Operations that replace
  topology invalidate the image map explicitly. Rigid transforms, scaling, and
  instance transforms preserve it.
- Texture bytes live in `TextureAsset`; external file paths are import
  provenance only and are never required after import.
- The canonical FullSpectrum 3MF extension stores a small image-map document
  plus content-addressed RGBA assets. Legacy MMU facet paint may be written as
  a compatibility projection, but it is never authoritative.
- Slicing constructs immutable samplers/snapshots. Derived facet trees and
  modulated polygons are caches scoped to one slice and are not written back to
  `ModelVolume`.

## Extension points

- New sources implement source sampling without changing palette resolution.
- New render modes consume `SurfaceSample`; they do not parse OBJ/3MF or call
  GUI code.
- Multiple zones are ordered by explicit priority. The current OBJ importer
  creates one zone, while later projection/text tools can add more.
- Palette entries carry both a stable mixed-filament reference and a physical
  fallback ID. Project filament reordering resolves stable IDs first.
- Calibration, dithering, continuous weight fields, and top-surface renderers
  belong in separate services built on the same sample contract.

## Invariants

- Source assets and triangle bindings must validate before attachment or load.
- Core image-map code must not depend on wxWidgets or import dialogs.
- File-format adapters may construct domain data but may not implement slicing.
- Renderers may not invent mixed-filament recipes; they resolve stored palette
  references through `MixedFilamentManager`.
- Perimeter displacement is bounded by nozzle/extrusion geometry, resampled at
  a finite spacing, corner-smoothed, and polygon-normalized before use.
- Support detection and perimeter generation consume the same V2 envelope, so
  an outward image-map displacement cannot become an unsupported surprise.

## V2 rendering pipeline

- `BoundaryModulation` is a pure geometry operator. A caller supplies signed
  displacement and smoothing-radius samples; the operator owns bounded
  resampling, circular smoothing, bidirectional slope limiting, narrow-feature
  clamping, corner-safe movement, and polygon normalization.
- `PerimeterEnvelopeRenderer` is the print-domain adapter. It snapshots volume
  transforms and `SurfaceSampler` instances, resolves stable palette references
  through `MixedFilamentManager`, and supplies the active layer's component
  offset to `BoundaryModulation`.
- The renderer runs after XY/elephant-foot compensation and before the final
  `Layer::make_slices()`. Contracted geometry clips every material region;
  outward strips are assigned back to the biased boundary region. The resulting
  region surfaces and `Layer::lslices` are then backed up as the untyped slice
  source used by wall, overhang, and support stages. Orca's separate
  elephant-foot-uncompensated first-layer envelope is rendered through the same
  adapter before it replaces `Layer::lslices`.
- The old G-code-time perimeter shifter remains only as a compatibility path
  for non-image-map mixed filaments. Filaments referenced by persistent V2
  palettes skip that path, preventing a second displacement without changing
  unrelated painted or whole-object mixed filaments.
