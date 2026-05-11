# FullSpectrum 3MF vNext Technical Implementation

## Status

Implementation guide for the rewrite described in `doc/fullspectrum-3mf-vnext-schema.md`.

This document is intentionally code-facing. The schema proposal defines the portable FullSpectrum Profile v1 contract; this document describes how Snapmaker Orca FullSpectrum should implement that contract in the current codebase while preserving existing project compatibility.

Related documents:

- `doc/fullspectrum-3mf-vnext-schema.md`
- `doc/fullspectrum-3mf-packaging.md`

## Current Implementation Snapshot

This document tracks the implementation in:

```text
src/libslic3r/Format/FullSpectrum3mf/
```

Implemented in the current code:

- Writes canonical FullSpectrum sidecar parts under `Metadata/fullspectrum/`.
- Writes `[Content_Types].xml` overrides for canonical FullSpectrum JSON parts.
- Writes package-level relationships for canonical parts and `MustPreserve`.
- Generates `manifest.json`, `project.json`, `identity-map.json`, `materials.json`, `assignments.json`, and `mixed-filaments.json`.
- Computes SHA-256 checksums for manifest-listed required canonical parts.
- Reads canonical parts through `ArchiveImportState`.
- Treats canonical mixed rows as authority over legacy `mixed_filament_definitions`.
- Applies canonical volume material assignments and MMU paint-state bindings back to current runtime numeric filament IDs.
- Preserves stable object and volume IDs through `ModelInfo::metadata_items`.
- Preserves optional unknown JSON extension parts under `Metadata/extensions/` by storing their bytes on `ModelInfo::metadata_items`.
- De-dupes generated canonical material IDs when multiple filament slots share the same source ID or preset identity.
- Keeps FullSpectrum sidecar write failure non-fatal: a canonical export problem is logged, but normal BBS 3MF saving continues.
- Adds focused coverage in `tests/libslic3r/test_mixed_filament.cpp`.

Intentionally not implemented in the current pass:

- `plates.json` and plate-scoped material assignments.
- GUI migration prompts and GUI blocking for unsupported required features.
- Public JSON Schema validation.
- OPC digital signatures.
- Stable paint-region records beyond current MMU paint-state bindings.
- A separate `Fs3mfPackage.*` module.

## Implementation Goals

- Write canonical FullSpectrum Profile v1 parts under `Metadata/fullspectrum/`.
- Keep every saved package valid as ordinary 3MF.
- Keep reading current Bambu/Snapmaker-style FullSpectrum project files.
- Treat canonical FullSpectrum JSON as the source of truth whenever it is present.
- Emit legacy files only as derived compatibility projections during the migration window.
- Preserve unknown optional FullSpectrum and vendor extension parts when round-tripping.
- Keep FullSpectrum vNext limited to physical material identity, mixed/virtual recipes, assignments, paint-state bindings, stable IDs, legacy migration, and feature/version negotiation.
- Replace `mixed_filament_definitions` as authority without breaking existing painted virtual-filament assignments.

## Non-Goals For First Implementation

- Do not standardize G-code or toolpath semantics.
- Do not move all private slicer settings into the portable profile.
- Do not require a public JSON Schema validator at runtime before the serializer is stable.
- Do not rewrite the whole 3MF exporter. Extend the existing BBS 3MF pipeline first, then extract pieces when the shape is proven.
- Do not invent physical filament entries for mixed or virtual filaments in the non-FullSpectrum projection.
- Do not implement `plates.json` or plate-scoped material assignments in the first shipping pass. BBS plate metadata remains in the legacy projection.
- Do not model spool inventory, remaining spool amount, AMS/channel/bay state, machine slot binding, farm routing, private preset exchange, or build summaries as FullSpectrum vNext standard data.

## Current Code Anchors

The existing package flow is centered in:

- `src/libslic3r/Format/bbs_3mf.cpp`
- `src/libslic3r/Format/bbs_3mf.hpp`
- `src/libslic3r/MixedFilament.cpp`
- `src/libslic3r/MixedFilament.hpp`
- `src/libslic3r/PrintConfig.cpp`
- `src/libslic3r/Config.cpp`
- `src/slic3r/GUI/Plater.cpp`
- `src/Snapmaker_Orca.cpp`

Current exporter responsibilities in `bbs_3mf.cpp`:

- `_add_content_types_file_to_archive(...)` writes `[Content_Types].xml`.
- `_add_fullspectrum_parts_to_archive(...)` builds and writes canonical FullSpectrum sidecar parts.
- `_add_project_config_file_to_archive(...)` writes `Metadata/project_settings.config`.
- `_add_project_embedded_presets_to_archive(...)` writes embedded preset JSON files.
- `_add_model_config_file_to_archive(...)` writes `Metadata/model_settings.config` and plate topology.
- `_add_slice_info_config_file_to_archive(...)` writes `Metadata/slice_info.config`.
- `_add_relationships_file_to_archive(...)` writes `_rels/.rels`, model config relationships, and cached FullSpectrum relationships.

Current importer responsibilities in `bbs_3mf.cpp`:

- `_BBS_3MF_Importer::_load_model_from_file(...)` iterates ZIP members.
- `_extract_fullspectrum_json_from_archive(...)` captures canonical and preservable extension JSON.
- `_apply_fullspectrum_canonical_config(...)` applies canonical data after geometry/config import.
- `_extract_project_config_from_archive(...)` reads `Metadata/project_settings.config`.
- `_extract_print_config_from_archive(...)` reads embedded config files.
- model, volume, plate, slice info, and auxiliary payloads are parsed from the current Bambu/Snapmaker metadata files.

Current mixed-filament state:

- `MixedFilamentManager::serialize_custom_entries()`
- `MixedFilamentManager::load_custom_entries(...)`
- `MixedFilamentManager::apply_gradient_settings(...)`
- `MixedFilament::stable_id`
- `PresetBundle::update_mixed_filament_id_remap(...)`

The existing tests in `tests/libslic3r/test_mixed_filament.cpp` already cover important migration behavior: stable row ID remapping, grouped manual patterns, gradient/bias behavior, and virtual ID rebuilds.

## Current Module Layout

The current implementation uses a small FullSpectrum module beside the BBS 3MF format code:

```text
src/libslic3r/Format/FullSpectrum3mf/
  Fs3mfConstants.cpp
  Fs3mfConstants.hpp
  Fs3mfIds.cpp
  Fs3mfIds.hpp
  Fs3mfJson.cpp
  Fs3mfJson.hpp
  Fs3mfLegacyBridge.cpp
  Fs3mfLegacyBridge.hpp
  Fs3mfReader.cpp
  Fs3mfReader.hpp
  Fs3mfTypes.hpp
  Fs3mfValidation.cpp
  Fs3mfValidation.hpp
  Fs3mfWriter.cpp
  Fs3mfWriter.hpp
```

There is no `Fs3mfPackage.*` file in the current implementation. Package planning lives in `Fs3mfWriter.*`; archive import state and manifest-driven reading live in `Fs3mfReader.*`.

Responsibilities:

- `Fs3mfConstants.*`
  Paths, content types, relationship types, feature IDs, profile versions, package path helpers, and preservable-path classification.
- `Fs3mfTypes.hpp`
  Plain C++ data structures for manifest, project, identity map, materials, assignments, mixed filaments, paint-state bindings, preserved parts, package write plans, and feature negotiation.
- `Fs3mfJson.*`
  `nlohmann::json` serializers and parsers for canonical parts.
- `Fs3mfIds.*`
  Stable ID generation and legacy ID conversion helpers.
- `Fs3mfReader.*`
  Canonical package reader, feature negotiation, checksum verification, extension preservation, and post-import application to `Model` / `DynamicPrintConfig`.
- `Fs3mfWriter.*`
  Canonical package writer and part generation.
- `Fs3mfLegacyBridge.*`
  Conversion between current in-memory state and the Profile v1 canonical model.
- `Fs3mfValidation.*`
  Lightweight semantic validation used by tests and by safe import/export checks.

`bbs_3mf.cpp` should remain the integration point for ZIP writing and legacy projection writing until the new module is stable.

## In-Memory Model

Introduce an aggregate that represents canonical FullSpectrum intent independent of the archive layout:

```cpp
namespace Slic3r::FullSpectrum3mf {

struct PackageModel {
    Manifest manifest;
    Project project;
    IdentityMap identity_map;
    Materials materials;
    Assignments assignments;
    std::optional<MixedFilaments> mixed_filaments;
    std::vector<PreservedPart> preserved_parts;
};

} // namespace Slic3r::FullSpectrum3mf
```

This object is the bridge between:

- archive parts
- `Model`
- `DynamicPrintConfig`
- `PresetBundle`
- `MixedFilamentManager`

Plate state is not part of the current `PackageModel`. BBS plate topology remains in existing legacy files such as `Metadata/model_settings.config` and `Metadata/slice_info.config`.

The package model should never store transient virtual filament numbers as authority. Transient numeric IDs are import/export projections only.

The package model should not contain spool inventory, AMS/channel/bay state, machine slot binding, farm routing, private preset exchange, build-summary data, generic process intent, machine targets, printer profiles, or UI state. If the application uses any of that for local convenience, keep it in local/session/account state outside the portable FullSpectrum package. Existing preset and slice-info files may still be written as legacy application projections, but they are not FullSpectrum vNext standard parts.

## Stable IDs

Profile v1 requires stable IDs for semantic entities. The implementation should use string IDs in canonical JSON even when current in-memory state uses integers.

Recommended prefixes:

- `pkg_` for package IDs
- `proj_` for project IDs
- `plate_` for plates
- `obj_` for objects
- `inst_` for instances
- `vol_` for volumes
- `reg_` for paint/material regions
- `fil_` for physical filaments
- `mix_` for mixed or virtual filaments

ID generation rules:

- Preserve IDs from canonical files on load.
- Allocate new IDs only for newly created semantic entities.
- For legacy migration, generate IDs once and write them to `identity-map.json`.
- For mixed-filament rows, map current `MixedFilament::stable_id` to a canonical `mix_...` ID and preserve that mapping.
- If two generated IDs collide, keep the first ID and append a deterministic numeric suffix to later IDs, for example `fil_gfsl991`, `fil_gfsl991_2`, `fil_gfsl991_3`. This commonly happens when multiple runtime filament slots share the same source spool or preset identity.
- Do not derive meaning from ID spelling after parsing.

Temporary legacy bridge rule:

- Until `MixedFilament` can carry a canonical string ID directly, use an ID map that binds `uint64_t stable_id` to a canonical `mix_...` string.
- Existing `u<stable_id>` values remain useful for migration and for preserving painted assignment remaps, but they must not appear as the only identity in canonical JSON.

### Positional IDs And Current Legacy State

The current implementation still uses positional numeric IDs as the working material handles:

```text
1..N = physical filaments by current config array position
N+1.. = visible mixed rows by current visible-row order
```

Those IDs are fine inside one slicer session and in legacy compatibility files. They are not stable saved identity. Adding or deleting a physical filament changes where mixed rows start, and disabling or reordering mixed rows changes which virtual material a numeric ID points to. The vNext importer should therefore treat numeric filament IDs as runtime handles derived from canonical `fil_...` and `mix_...` references.

The legacy implementation is a hybrid: `MixedFilament::stable_id` and the `u<stable_id>` token give mixed rows a persistent identity, but object, volume, and painted material assignments still reference numeric filament IDs. The canonical format should move those assignment references to stable material IDs first, then rebuild numeric IDs for existing slicer code after load.

### MMU Paint Data

MMU means multi-material unit / multi-material printing. In this codebase, MMU painting is the feature where users paint mesh facets with material states. FullSpectrum already supports painted mixed-material regions through the current legacy path: triangle attributes such as `paint_color` are loaded into `ModelVolume::mmu_segmentation_facets`, and those facet states ultimately refer to numeric filament IDs.

The first vNext implementation should keep that existing facet paint payload instead of redesigning MMU painting. What changes is the authority for what a numeric paint state means:

```text
existing facet payload: triangle/facet uses paint state 5
canonical assignment data: paint state 5 on volume vol_... means material_ref mix_...
runtime import: material_ref mix_... is mapped back to the correct current numeric filament ID
```

Do not model current painted facet offsets as stable `paint_region` records unless the implementation has real stable region handles. Standard `paint_region` assignments should be emitted only when a region can be identified independently from triangle array position and transient material state numbers.

## Package Detection

Import should classify a package in this order:

1. Canonical FullSpectrum Profile v1 package
   `Metadata/fullspectrum/manifest.json` is present.
2. Legacy FullSpectrum package
   Manifest is absent and legacy FullSpectrum state such as `mixed_filament_definitions` is present.
3. Ordinary 3MF or BBS 3MF package
   No FullSpectrum profile and no conservative legacy FullSpectrum marker.

Canonical packages should be read through the FullSpectrum reader first. Legacy files should be read through the existing importer and then converted to `PackageModel` in memory.

Packages without a FullSpectrum manifest should continue through the existing ordinary 3MF/BBS path. FullSpectrum packages must still contain normal 3MF geometry so non-FullSpectrum slicers can open them and ignore `Metadata/fullspectrum/` and `Metadata/extensions/`. Those slicers are not expected to preserve or correctly slice FullSpectrum-only semantics.

## Feature Negotiation

Implement feature negotiation before applying canonical data to slicer state.

Reader behavior:

- Unknown required feature: load geometry for inspection if possible, but do not silently slice, print, save over, or rewrite affected FullSpectrum semantics.
- Unknown optional feature: ignore for behavior and preserve the part if possible.
- Known optional feature with unsupported details: use a declared fallback or preserve without claiming support.
- Required vendor feature without fallback: mark the project as blocked for correctness-sensitive workflows.

Expose the result as a small status object:

```cpp
struct FeatureNegotiationResult {
    bool can_edit = true;
    bool can_slice = true;
    bool can_print = true;
    bool should_preserve_only = false;
    std::vector<std::string> unsupported_required_features;
    std::vector<std::string> unsupported_optional_features;
    std::vector<std::string> warnings;
};
```

The GUI can use this object to show a targeted warning instead of failing with a generic import error.

Current status:

- The reader negotiates known required and optional feature IDs.
- Unknown required features prevent canonical FullSpectrum data from being applied.
- The importer still loads ordinary 3MF geometry through the existing path when possible.
- GUI-level blocking prompts for unsupported required features are not implemented yet.

## Canonical Writer Flow

The save path should build canonical parts from the current in-memory project before writing legacy projections.

Treat the following as logical generation order, not necessarily ZIP member order. The exporter should first build a complete package plan containing every part, content type, relationship, checksum, preserved unknown part, and legacy projection decision. Then it can write `[Content_Types].xml`, canonical parts, legacy parts, and relationship parts from that plan. This avoids fragile save code where a JSON part is added but its content type, relationship, checksum, or `MustPreserve` link is forgotten.

Suggested package-plan shape:

```cpp
struct PackagePartPlan {
    std::string path;
    std::string content_type;
    std::string role;
    std::string bytes;
    bool required = false;
    bool must_preserve = false;
};

struct PackageRelationshipPlan {
    std::string target;
    std::string type;
    std::string source; // empty means package root
};

struct PackageWritePlan {
    std::vector<PackagePartPlan> parts;
    std::vector<PreservedPart> preserved_parts;
    std::vector<PackageRelationshipPlan> relationships;
    std::vector<std::string> required_features;
    std::vector<std::string> optional_features;
};
```

Current exporter integration:

1. `save_model_to_file(...)` reads preserved FullSpectrum extension bytes from `ModelInfo::metadata_items`.
2. `_add_content_types_file_to_archive(...)` writes standard BBS content types plus FullSpectrum content-type overrides and preserved-part overrides.
3. The normal BBS exporter writes geometry and legacy metadata.
4. `_add_fullspectrum_parts_to_archive(...)` builds a `GeometryBindingInput` from the BBS object/volume export maps.
5. `Fs3mfWriter::build_write_plan(...)` builds canonical JSON parts, checksums, relationships, and preserved part entries.
6. `_add_fullspectrum_parts_to_archive(...)` writes the generated FullSpectrum parts into the ZIP and caches relationship XML for `_rels/.rels`.
7. `_add_relationships_file_to_archive(...)` appends the FullSpectrum relationships.

If `build_write_plan(...)` throws or a FullSpectrum part cannot be added, the error is logged and normal BBS 3MF saving continues. The FullSpectrum sidecar is authoritative when present and valid, but its writer must not make ordinary project saving fail.

Target architecture for a later cleanup:

1. Build or refresh `PackageModel` from `Model`, `DynamicPrintConfig`, and `MixedFilamentManager`.
2. Validate the package model for required core invariants.
3. Serialize canonical JSON parts.
4. Compute part checksums for `manifest.json`.
5. Write canonical parts under `Metadata/fullspectrum/`.
6. Write preserved unknown FullSpectrum and vendor extension parts.
7. Write the ordinary 3MF geometry and current BBS metadata files.
8. If legacy write is enabled, write the derived legacy projection and declare it in `manifest.json`.
9. Write `[Content_Types].xml` with FullSpectrum JSON content types.
10. Write relationships, including manifest and `MustPreserve` links.

`manifest.json` should be the last canonical JSON part generated because it references the checksums and paths of other parts.

Integration points:

- Extend `_add_content_types_file_to_archive(...)` to include FullSpectrum overrides.
- Add `_add_fullspectrum_parts_to_archive(...)` near the current project-config write block.
- Extend `_add_relationships_file_to_archive(...)` or add a FullSpectrum-aware relationship builder that accepts additional relationships.
- Keep existing BBS files during the migration window, but mark them as derived legacy projection in `manifest.json`.
- Prefer generating the package plan before `_add_content_types_file_to_archive(...)`. The current implementation still writes content types before the FullSpectrum write plan exists, so it uses static FullSpectrum overrides plus preserved-part overrides collected at save start.

## Canonical Reader Flow

Current importer integration:

1. The existing BBS importer iterates ZIP members.
2. `ArchiveImportState::accepts_part(...)` captures `Metadata/fullspectrum/*.json` and preservable `Metadata/extensions/...` JSON parts.
3. The existing importer loads ordinary 3MF geometry, BBS metadata, and legacy project config.
4. Import-time bindings record the loaded `ModelObject*` and `ModelVolume*` for their 3MF object/volume IDs.
5. After geometry/config load, `ArchiveImportState::apply_to_model_and_config(...)` parses the manifest, verifies checksums, negotiates features, validates canonical part kind/schema version, and applies canonical data.
6. Canonical mixed rows overwrite legacy `mixed_filament_definitions`.
7. Canonical volume assignments set volume `extruder` config.
8. Canonical paint-state bindings remap `ModelVolume::mmu_segmentation_facets` states to the current runtime filament IDs.
9. Stable object and volume IDs are stored on `ModelInfo::metadata_items`.
10. Optional unknown extension parts listed in the manifest are preserved as serialized `PreservedPart` metadata on `ModelInfo::metadata_items`.

Recommended target import sequence for a later cleanup:

1. Open ZIP and build a member index.
2. Locate and parse `Metadata/fullspectrum/manifest.json`.
3. Validate manifest version, document class, required core parts, content types, and required features.
4. Read required canonical parts from the manifest, not from hardcoded assumptions alone.
5. Verify checksums when present.
6. Parse canonical JSON into `PackageModel`.
7. Preserve unknown optional parts and extension parts that should survive round-trip editing.
8. Load ordinary 3MF geometry through the existing model importer.
9. Apply `identity-map.json` to bind stable IDs to loaded model objects, volumes, instances, and regions.
10. Apply materials and assignments to `DynamicPrintConfig` and model/volume/paint state.
11. Rebuild `MixedFilamentManager` from `mixed-filaments.json`.
12. Apply optional Local-Z FullSpectrum data when present.
13. Mark the loaded project as canonical FullSpectrum Profile v1.

The reader must not let legacy `mixed_filament_definitions` override canonical `mixed-filaments.json` when the manifest is present.

The safest canonical read path is:

- use the existing importer for geometry and ordinary 3MF data
- suppress or ignore legacy FullSpectrum config fields while canonical data is being applied
- apply canonical materials, assignments, and mixed rows after geometry has stable object/volume bindings

If the existing importer cannot cleanly suppress legacy FullSpectrum fields, add a canonical post-import correction pass that overwrites any legacy-derived mixed rows and remaps numeric material states from canonical stable IDs.

## Canonical Part Mapping

### `manifest.json`

Implementation source:

- generated by `Fs3mfWriter`
- read first by `Fs3mfReader`

Required fields to enforce:

- `kind`
- `schema_version`
- `document_class`
- `package_id`
- `features.required`
- `parts`
- `authoritative_sources`

Implementation notes:

- The manifest path should be discovered through package relationships when possible, but `Metadata/fullspectrum/manifest.json` should remain the fallback path.
- Checksums are advisory for fast integrity checks; they do not replace package signatures.
- The manifest should declare legacy projection files only when they are actually written.

### `project.json`

Implementation source:

- minimal FullSpectrum project metadata from `Model`
- compatibility and feature policy from save options

Do not dump every config key into `project.json`. Generic slicer process settings, machine targets, printer profiles, UI state, private preset data, and build/session state must remain outside the FullSpectrum vNext standard.

Process-like controls belong in FullSpectrum standard parts only when they directly define mixed-material deposition semantics. For example, multi-perimeter material patterns belong in `mixed-filaments.json`; layer height, supports, speeds, wall counts, infill, nozzle selection, printer choice, and quality presets do not.

### `identity-map.json`

Implementation source:

- object IDs from the current BBS object export map
- volume IDs from the current volume/object mapping
- runtime material bindings from project filament config arrays
- mixed-filament runtime bindings from visible canonical mixed row order

Implementation notes:

- The current implementation writes object, volume, physical material, and mixed-material bindings.
- It does not write instance, plate, or stable paint-region bindings yet.
- If stable paint-region IDs are not available for a painted region feature, declare the relevant extension as required or preserve it as unsupported. Do not pretend region identity is stable when it is only an array offset.
- Store the identity map before writing assignments so assignment targets can reference stable IDs only.

### `materials.json`

Implementation source:

- physical filament count from project filament config arrays such as `filament_colour`, `filament_settings_id`, and `filament_ids`
- material family/type from filament config where available
- diameter from filament configuration
- display names from user-visible filament names where available

Implementation notes:

- Only physical filaments go in `physical_filaments`.
- Multiple runtime filament slots may share the same source `filament_ids` value or preset name. The writer must de-dupe canonical `fil_...` IDs while preserving `source_index` so assignments still map back to the correct runtime slot.
- Spool inventory, AMS/channel/bay state, machine slot bindings, and remaining-spool amounts do not belong in FullSpectrum vNext standard parts.
- Virtual and mixed rows are written to `mixed-filaments.json`.

### `assignments.json`

Implementation source:

- object and volume material assignments from model and volume configs
- paint/material region assignments from the current painting data structures
- imported canonical assignment records when no edit has touched them

Implementation notes:

- Assignment values must reference stable `fil_...` or `mix_...` IDs.
- The bridge to current slicer state maps stable material refs back to transient 1-based filament IDs after load.
- If a non-FullSpectrum projection is written, mixed assignments must not be exported as fake physical materials.
- For current MMU painted facets, preserve the existing facet payload and write canonical paint-state bindings that map each local numeric paint state to a stable material ref. Do not require stable `paint_region` IDs for this first implementation.

### `plates.json`

Current status:

- Not implemented in the first shipping pass.
- The product decision for now is to avoid plate-scoped material assignments because different material meanings per plate would add complexity and confusion.
- Existing BBS plate data remains in the legacy projection files.
- FullSpectrum canonical assignments currently target volumes and MMU paint states, not plates.

Possible future implementation source:

- `PlateDataPtrs`
- object/instance membership in `PlateData::objects_and_instances`
- plate-specific material assignment scopes if the product later needs them

Implementation notes:

- Plate IDs must be stable and separate from `plate_index`.
- Filenames may still use `plate_<n>` for legacy projection files, but canonical references should use stable plate IDs.
- Plate names, lock state, thumbnails, per-plate process overrides, plate-specific slicer settings, and presentation/UI state are not FullSpectrum vNext standard data.

### `mixed-filaments.json`

Implementation source:

- `MixedFilamentManager::mixed_filaments()`
- legacy `mixed_filament_definitions` only during migration reads

Mapping from current fields:

| Current State | Canonical Field |
|---|---|
| `MixedFilament::stable_id` | `virtual_filaments[].id` through ID map |
| `filament_id` / `a` | `origin.component_refs[0]` |
| `filament_id_b` / `b` | `origin.component_refs[1]` |
| `deleted` | `visibility_state` |
| `custom` | `source_kind` |
| `origin_auto_generated` | `origin.origin_auto_generated` |
| `mix_b_percent` | `blend.component_b_percent` |
| `distribution_mode` | `distribution.mode` |
| `manual_pattern` | `manual_pattern.groups` |
| `gradient_component_ids` | `gradient.component_refs` |
| `gradient_component_weights` | `gradient.weights` |
| `component_a_surface_offset` | `surface_bias.component_a_offset_mm` |
| `component_b_surface_offset` | `surface_bias.component_b_offset_mm` |
| Local-Z max sublayers | optional `local_z.max_sublayers` |

Distribution mode conversion:

| Current Mode | Canonical Value | Notes |
|---|---|---|
| `LayerCycle` | `layer_cycle` | Standard v1 mode |
| `Simple` | `simple` | Standard v1 mode |
| retired storage value `1` | none | Read only for old compact rows; normalize before writing vnext data |

Manual pattern conversion:

- Split current grouped pattern on commas.
- Convert `1` to `component_a`.
- Convert `2` to `component_b`.
- Convert `3`..`9` to `physical:<filament-id>` after resolving the physical filament stable ID.
- Preserve empty or invalid patterns as `null` after normalization.

Gradient conversion:

- Decode current compact component IDs into physical filament stable refs.
- Decode and normalize weights.
- Omit `gradient` when fewer than three valid components remain.

Private preset files and slice/build summaries are outside FullSpectrum vNext standard data. During the migration window they may remain in the legacy BBS projection for current application compatibility, but canonical FullSpectrum readers must not depend on them for FullSpectrum material semantics.

## Legacy Bridge

The bridge has two directions.

### Legacy To Canonical

Used when opening old files:

1. Read the package through the current BBS importer.
2. Detect `mixed_filament_definitions` and other FullSpectrum config keys.
3. Build `PackageModel` from loaded `Model`, `DynamicPrintConfig`, and `MixedFilamentManager`.
4. Mark the document as legacy-upgraded in memory.
5. Prompt the user that saving will upgrade the project.

The prompt should not block inspection, slicing, or save-as.

### Canonical To Legacy

Used during the migration window:

1. Treat canonical `PackageModel` as authority.
2. Generate `mixed_filament_definitions` from canonical mixed rows through the bridge.
3. Generate current BBS config files from current in-memory config.
4. Omit legacy data that would cause old consumers to misinterpret mixed materials.
5. Declare the legacy projection in `manifest.json`.

This write direction is compatibility output only. Readers must prefer canonical parts when both are present.

There are two different compatibility outputs:

- **Legacy FullSpectrum projection**
  Temporary bridge for older FullSpectrum builds. It may include `mixed_filament_definitions` so users can share or roll back during the first migration releases.
- **Non-FullSpectrum projection**
  Ordinary 3MF view for slicers that do not know FullSpectrum. It should expose geometry and real physical filaments only. It must not invent mixed or virtual filaments as extra physical tools.

Recommended policy:

- Release N: write canonical parts and a safe legacy FullSpectrum projection by default.
- Release N+1 / N+2: keep reading legacy files and continue dual-write unless a user/build option disables it.
- Later: keep legacy reading, but allow or default to canonical-only writes once old FullSpectrum versions are expected to have updated.

## Unknown Part Preservation

Canonical editors should preserve:

- unknown `Metadata/fullspectrum/*.json` parts listed in the manifest as optional
- unknown `Metadata/extensions/<reverse-dns-vendor>/...` parts listed in the manifest
- relationship entries marked `MustPreserve`
- extension payloads under known objects when the implementation does not normalize them

Implementation approach:

1. During canonical import, record original ZIP member path, content type, bytes, relationship target, and feature ID for preserved parts.
2. Keep preserved bytes in project-attached state.
3. On save, write preserved parts unchanged unless the user edited the owning feature.
4. If a preserved part has a checksum in the old manifest, recompute and write the new checksum in the new manifest.

Current implementation:

- Preserves manifest-listed optional JSON parts under `Metadata/extensions/`.
- Stores preserved bytes as serialized `std::vector<PreservedPart>` in `ModelInfo::metadata_items` under `FullSpectrum3mf:PreservedParts`.
- Re-emits preserved parts on save, including their content type, role, required flag, `MustPreserve` relationship, and recomputed checksum in the new manifest.
- Skips duplicate preserved paths and never lets preserved parts overwrite core FullSpectrum parts.
- Does not yet preserve arbitrary unknown relationship entries that are not represented as manifest-listed extension parts.

Example:

```text
Metadata/extensions/com.example/full-spectrum-material-solver.json
```

If this optional extension is listed in the manifest and the current application does not understand it, a no-op open/save should copy the original bytes and relationship back into the new package. If the application drops it, it becomes destructive for newer FullSpectrum packages or vendor-authored packages even when the user did not edit that feature.

## Validation Rules

Start with semantic validation in C++ tests, then add JSON Schema validation as schemas stabilize.

Required writer checks:

- all required parts have `kind` and `schema_version`
- every manifest part path exists in the ZIP
- every required feature has a known writer implementation
- every assignment target resolves to an identity-map entry
- every assignment material ref resolves to a physical or mixed material
- every mixed row component ref resolves to a physical filament
- no mixed or virtual filament is emitted as a physical filament
- every visible mixed row has a stable ID
- generated canonical material IDs are unique after de-duping duplicate source IDs
- every declared checksum matches the serialized bytes
- FullSpectrum sidecar failures do not abort ordinary BBS 3MF saving

Required reader checks:

- unknown required features block affected workflows
- malformed required JSON parts fail the canonical FullSpectrum import
- missing required parts fail the canonical FullSpectrum import
- required part checksums fail the canonical FullSpectrum import
- optional malformed parts warn and preserve when possible
- legacy projection never overrides canonical parts

## Testing Plan

Currently implemented in `tests/libslic3r/test_mixed_filament.cpp`:

- Canonical grouped manual pattern data round-trips through `mixed-filaments.json`.
- Writer emits core package parts and mixed assignments.
- Writer keeps material IDs unique when duplicate filament sources are present.
- Canonical mixed rows override conflicting legacy `mixed_filament_definitions`.
- Unknown required features block canonical import.
- Required canonical part checksum mismatch blocks canonical import.
- Reader applies canonical volume and paint assignments.
- Reader preserves optional extension parts and writer re-emits them.

Unit tests:

- `Fs3mfIds` preserves and allocates stable IDs.
- `Fs3mfJson` round-trips each canonical part.
- `Fs3mfValidation` rejects dangling assignments and unresolved mixed components.
- `Fs3mfLegacyBridge` converts `mixed_filament_definitions` rows to canonical `mixed-filaments.json`.
- `Fs3mfLegacyBridge` converts canonical mixed rows back to the current legacy row string.
- Manual pattern groups round-trip through canonical JSON.
- Gradient component refs and weights round-trip through canonical JSON.
- Tombstoned auto rows remain tombstoned after canonical round-trip.
- Surface bias offsets remain in millimeters and preserve sign.
- Local-Z row caps are emitted only when `fs.local-z.v1` is declared.

Integration tests:

- Save a minimal project and assert all required canonical parts exist.
- Save a physical multi-material project and assert canonical material IDs remain unique when source IDs repeat.
- Save a mixed-filament project and assert `mixed-filaments.json` is authoritative.
- Open a legacy project, save it, and assert canonical parts are written.
- Open a dual-write project and assert canonical data wins over conflicting legacy data.
- Round-trip unknown optional vendor extension parts unchanged.
- Verify `[Content_Types].xml` includes FullSpectrum content types.
- Verify `_rels/.rels` links the manifest and preservable parts.
- Verify non-FullSpectrum projection exposes only real physical filaments.

Existing tests to extend:

- `tests/libslic3r/test_mixed_filament.cpp`

Potential new tests:

```text
tests/libslic3r/test_fullspectrum_3mf_ids.cpp
tests/libslic3r/test_fullspectrum_3mf_json.cpp
tests/libslic3r/test_fullspectrum_3mf_legacy_bridge.cpp
tests/libslic3r/test_fullspectrum_3mf_validation.cpp
tests/libslic3r/test_fullspectrum_3mf_package.cpp
```

Suggested fixtures:

```text
tests/data/fullspectrum_3mf/
  minimal_project.project.3mf
  physical_multimaterial.project.3mf
  mixed_pair.project.3mf
  grouped_manual_pattern.project.3mf
  gradient.project.3mf
  local_z_optional.project.3mf
  vendor_optional_preserve.project.3mf
  vendor_required_blocked.project.3mf
  legacy_mixed_definitions.3mf
  dual_write_canonical_wins.project.3mf
```

## Migration Phases

### Phase 0: Foundations

- Add constants and canonical C++ types.
- Add JSON serializers and parser tests.
- Add semantic validators.
- Add ID generation and ID-map tests.
- Add legacy mixed-row conversion tests.

### Phase 1: Dual Read

- Detect canonical manifest.
- Parse manifest and canonical parts.
- Apply canonical mixed-filament rows to `MixedFilamentManager`.
- Keep existing legacy importer as fallback.
- Block unsupported required features with clear status.

### Phase 2: Dual Write

- Write canonical `Metadata/fullspectrum/*.json` parts on save.
- Keep writing existing BBS projection files.
- Mark legacy projection as derived in `manifest.json`.
- Add FullSpectrum content types and relationships.
- Preserve unknown optional parts loaded from canonical packages.

### Phase 3: Canonical Authority

- Make canonical parts authoritative whenever manifest is present.
- Ensure legacy fields cannot override canonical mixed rows, assignments, or identity maps.
- Add GUI prompt for legacy-opened projects.
- Add command-line reporting for legacy migration and unsupported required features.

### Phase 4: Compatibility Hardening

- Add conformance fixtures and archive-level tests.
- Add optional JSON Schema validation in test tooling.
- Add round-trip preservation tests for vendor extensions.
- Add non-FullSpectrum projection tests.

### Later Phase: Reduce Legacy Writes

- Keep legacy reading.
- Add user or build option to disable legacy projection writes.
- Stop writing unsafe legacy projections by default once migration is mature.

## User Interface Touchpoints

GUI import:

- Show a non-blocking migration prompt when a legacy FullSpectrum file is opened.
- Show a blocking warning when unsupported required FullSpectrum features affect slicing or printing.
- Show optional-feature warnings only when they affect visible fidelity or editing.

GUI save:

- Save canonical FullSpectrum parts by default.
- During migration, optionally keep legacy projection output.
- Warn if saving would drop unsupported required extension semantics.

CLI import/export:

- Report unsupported required features as a specific failure reason.
- Report legacy migration in logs.
- Prefer canonical profile data when slicing canonical packages.

## Open Implementation Decisions

- Whether preserved extension package-state should stay on `ModelInfo::metadata_items` or move to a typed project/package state object.
- Whether `MixedFilament` should gain a canonical string ID or keep using `uint64_t stable_id` behind an ID map.
- How to expose stable paint-region IDs from the existing painting data structures.
- Whether future implementations should promote MMU painted facets into stable `paint_region` records. The first implementation should keep the existing facet payload and add canonical paint-state bindings.
- Whether first runtime validation should use only C++ semantic checks or also a bundled JSON Schema validator.
- Whether and when to implement `plates.json`. The current product decision is no plate-scoped material assignments.
- Whether FullSpectrum writer failure should remain non-fatal forever or become a user-visible warning once the sidecar is considered mature.
- Exact stable namespace and relationship URLs for public standardization.

## Acceptance Criteria For The Rewrite

The first complete implementation is acceptable when:

- A new mixed-filament project saves with canonical `manifest.json`, `project.json`, `identity-map.json`, `materials.json`, `assignments.json`, and `mixed-filaments.json`.
- The same project can be reopened with canonical parts as authority.
- Existing legacy FullSpectrum projects still open.
- Saving a legacy project upgrades it to canonical FullSpectrum Profile v1.
- Painted assignments to mixed rows survive row reordering, deletion, and physical filament count changes.
- Unknown optional extension parts survive a no-op open/save round trip.
- Unknown required features block slicing/printing instead of being silently ignored.
- Non-FullSpectrum tools can still see ordinary 3MF geometry and real physical filaments only.
- The current mixed-filament unit tests continue to pass, and new canonical round-trip tests cover the replacement JSON parts.

Current implementation status against those criteria:

- Canonical parts are written and visible in saved `.3mf` packages.
- Canonical mixed rows, volume assignments, and paint-state bindings are applied on load when the manifest is present and valid.
- Existing legacy BBS projection files are still written for compatibility.
- Unknown required features block canonical application; GUI workflow blocking is not implemented yet.
- Optional extension JSON parts under `Metadata/extensions/` can survive no-op open/save.
- `plates.json` is intentionally out of scope for this pass.

## Bottom Line

The rewrite should be implemented as a canonical project-intent layer on top of the existing 3MF package writer, not as a replacement for 3MF geometry or a dump of private slicer config.

The implementation path is:

1. Define canonical C++ types and JSON serializers.
2. Convert current project state into those types.
3. Read canonical parts before legacy projection files.
4. Write canonical parts on every save.
5. Keep legacy projection files only as derived compatibility output.
6. Preserve unknown optional data and fail closed on unknown required semantics.

That gives the project a practical migration path from the current compact `mixed_filament_definitions` payload to a vendor-neutral FullSpectrum Profile v1 package.
