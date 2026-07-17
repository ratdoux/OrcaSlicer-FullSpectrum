# FullSpectrum 3MF Packaging Notes

## Overview

`SnorcaSlicer` does not save projects through the plain upstream `3mf.cpp` path. Full project saves and exports go through the Bambu-style `bbs_3mf` pipeline in `src/libslic3r/Format/bbs_3mf.cpp`, via:

- GUI save/export: `src/slic3r/GUI/Plater.cpp`
- CLI export: `src/Snapmaker_Orca.cpp`

The result is a ZIP-based `.3mf` archive with:

- standard 3MF scaffolding (`[Content_Types].xml`, `_rels/.rels`, `3D/...`)
- Bambu/Snapmaker metadata/config files under `Metadata/`
- optional `Auxiliaries/` assets
- FullSpectrum state stored as normal project config keys, not in a separate FullSpectrum-only file

## Typical Archive Layout

Common files written by the current exporter:

```text
[Content_Types].xml
_rels/.rels
3D/3dmodel.model

3D/_rels/3dmodel.model.rels              # when split-model export is enabled
3D/Objects/<name>_<id>.model             # when split-model export is enabled

Metadata/project_settings.config
Metadata/model_settings.config
Metadata/_rels/model_settings.config.rels
Metadata/slice_info.config
Metadata/layer_heights_profile.txt
Metadata/layer_config_ranges.xml
Metadata/brim_ear_points.txt
Metadata/custom_gcode_per_layer.xml
Metadata/cut_information.xml

Metadata/process_settings_<n>.config
Metadata/filament_settings_<n>.config
Metadata/machine_settings_<n>.config

Metadata/plate_<n>.gcode
Metadata/plate_<n>.gcode.md5
Metadata/plate_<n>.png
Metadata/plate_<n>_small.png
Metadata/plate_no_light_<n>.png
Metadata/top_<n>.png
Metadata/pick_<n>.png
Metadata/plate_<n>.json

Auxiliaries/...
```

Notes:

- CLI export explicitly uses `SplitModel|WithGcode|UseLoadedId|ShareMesh`, so split-model archives are the normal CLI shape.
- GUI export adds `Zip64` and whatever `SaveStrategy` flags the caller requested.
- `SkipModel`, `SkipAuxiliary`, and `WithGcode` change which optional payloads are included.

## What Lives In Each File

### `3D/3dmodel.model`

This is the main geometry document. It contains:

- model metadata such as title, origin, designer, description, license, copyright, app version
- `BambuStudio:3mfVersion`
- any extra CLI metadata pairs pushed through `model.md_name` / `model.md_value`
- the 3MF object/build graph

When split-model export is enabled, the root model references per-object `3D/Objects/*.model` files through `3D/_rels/3dmodel.model.rels`.

### `Metadata/project_settings.config`

This is the main project config payload. It is saved as JSON through `ConfigBase::save_to_json(...)`.

Header fields:

- `version`
- `name` = `project_settings`
- `from` = `project`

After that, it stores the full `DynamicPrintConfig` key/value set. This is where FullSpectrum-specific settings live.

### `Metadata/model_settings.config`

This is XML and carries structure that is not just raw config:

- per-object metadata and object-level config overrides
- per-volume metadata, matrices, source file/source object/source volume/source offsets
- emboss/text payload references
- mesh repair stats
- per-plate structure:
  - plate index
  - plate name
  - lock state
  - bed type
  - print sequence settings
  - spiral mode
  - gcode filename reference
  - thumbnail references
  - `plate_<n>.json` reference
  - object/instance membership for the plate
- assembly transform data

This is the file that binds the model, volumes, and plate layout together.

### `Metadata/_rels/model_settings.config.rels`

This is an XML relationships file pointing from `model_settings.config` to the per-plate G-code members:

- `Metadata/plate_1.gcode`
- `Metadata/plate_2.gcode`
- ...

### `Metadata/slice_info.config`

This is XML with per-plate sliced output metadata, including:

- plate index
- printer model id
- nozzle diameters
- timelapse mode
- predicted time
- predicted weight
- whether toolpaths go outside bounds
- whether support is used
- whether label objects are enabled
- object skip flags
- used filament summary per plate
- slice warnings

### `Metadata/plate_<n>.json`

This is a JSON sidecar built from `PlateBBoxData`. It stores:

- total first-layer bounding box
- per-object first-layer boxes and areas
- per-plate filament ids/colors
- sequential-print flag
- first extruder
- nozzle diameter
- bed type

### Embedded preset files

These are JSON snapshots of embedded presets:

- `Metadata/process_settings_<n>.config`
- `Metadata/filament_settings_<n>.config`
- `Metadata/machine_settings_<n>.config`

The importer also still accepts older legacy `Metadata/print_setting_<n>.config` INI-style files for print presets.

### `Auxiliaries/`

This is a straight file copy of the model auxiliary temp directory into the archive. It is used for extra assets such as cover thumbnails and other sidecar data. Top-level thumbnail relationships may point here instead of defaulting to `Metadata/plate_1.png`.

## FullSpectrum-Specific Settings

FullSpectrum data is mostly not packaged as separate files. It is stored as ordinary config keys inside `Metadata/project_settings.config`.

The main keys currently defined in `src/libslic3r/PrintConfig.cpp` are:

- `mixed_color_layer_height_a`
- `mixed_color_layer_height_b`
- `mixed_filament_gradient_mode`
- `mixed_filament_height_lower_bound` (the minimum Local-Z sublayer height)
- `mixed_filament_advanced_dithering`
- `mixed_filament_pointillism_pixel_size`
- `mixed_filament_pointillism_line_gap`
- `mixed_filament_component_bias_enabled`
- `mixed_filament_surface_indentation`
- `mixed_filament_region_collapse`
- `mixed_filament_definitions`
- `dithering_z_step_size`
- `dithering_local_z_mode`
- `dithering_local_z_whole_objects`
- `dithering_local_z_preserve_first_layer`
- `dithering_local_z_direct_multicolor`
- `dithering_step_painted_zones_only`

In practice:

- `mixed_filament_definitions` is the important serialized state blob for virtual mixed rows
- the other keys are scalar toggles/bounds that control how those rows are interpreted during slicing

## `mixed_filament_definitions` Format

This is the main FullSpectrum project payload.

Inside `Metadata/project_settings.config`, it is stored as a single JSON string value, for example:

```json
{
    "mixed_filament_definitions": "1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u17;1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d1,o1,u4"
}
```

The string is not JSON-structured internally. It is a compact custom serialization handled by `MixedFilamentManager::serialize_custom_entries()` and `MixedFilamentManager::load_custom_entries()`.

### High-Level Structure

- rows are separated by `;`
- each row is a comma-separated token list
- the first fixed tokens define the basic pair
- later prefixed tokens add optional metadata
- any leftover trailing tokens are rejoined into the manual pattern

Current writer shape:

```text
a,b,enabled,custom,mix,pointillism,g<ids>,w<weights>,m<mode>,z<max>,xa<offsetA>,xb<offsetB>,d<deleted>,o<origin_auto>,u<stable_id>[,manual_pattern...]
```

### Fixed Leading Fields

- `a`
  1-based `component_a` physical filament id.
- `b`
  1-based `component_b` physical filament id.
- `enabled`
  `1` if the row is active, `0` if hidden/disabled.
- `custom`
  `1` for user-created rows, `0` for auto-generated pair rows.
- `mix`
  Blend percent of component B in `[0..100]`.
- `pointillism`
  Legacy compatibility flag. Current code still emits the slot, but modern save/load normalizes away the old pointillism path.

For custom rows, the distinction between `a` and `b` is meaningful:

- `mix` is “percent of B”
- manual-pattern token `1` means `component_a`
- manual-pattern token `2` means `component_b`

For auto rows, matching during reload is done by canonical pair identity, so `(1,3)` and `(3,1)` are treated as the same base pair.

### Prefixed Metadata Tokens

- `g<ids>`
  Compact gradient component id list. Example: `g123` means physical filaments `1,2,3`.
- `w<weights>`
  Gradient weights aligned with `g`. Example: `w50/25/25`.
- `m<mode>`
  Distribution mode integer.
- `z<max>`
  Local-Z max sublayers for this row.
- `xa<offsetA>`
  Surface offset for component A in mm.
- `xb<offsetB>`
  Surface offset for component B in mm.
- `d<deleted>`
  Tombstone flag. Used mainly so deleted auto rows stay deleted after regeneration.
- `o<origin_auto>`
  Remembers whether the row originally came from auto-generation even if it was later edited.
- `u<stable_id>`
  Persistent row identity.

### Distribution Mode Values

The enum in `MixedFilament.hpp` is:

- `0` = `LayerCycle`
- `1` = `SameLayerPointillisme`
- `2` = `Simple`

In persistence terms, there is an important nuance:

- mode `1` is not really treated as a normal long-term persisted mode anymore
- load/save paths normalize old pointillism rows instead of preserving them as a first-class serialized representation
- in practice, persisted rows today are mostly `m0` or `m2`

### What Each Flag Really Means

- `custom=0` does not mean “unimportant row”
  Auto-generated rows are serialized too, because their enabled/deleted/stable-id state matters.
- `deleted=1` is stronger than `enabled=0`
  Deleted rows are treated as tombstoned and removed from active virtual-id enumeration.
- `origin_auto=1` means “this row belongs to an underlying auto pair”
  That prevents regeneration from resurrecting a deleted base pair as a brand-new row.
- `stable_id` is not the same thing as virtual filament id
  Virtual ids are rebuilt dynamically from enabled rows; `stable_id` is the durable identity that lets painted assignments survive reorder/regenerate.

### Virtual Filament Id Mapping

This is one of the most important behaviors to understand.

Suppose a printer has 3 physical filaments:

- physical ids are `1`, `2`, `3`
- mixed virtual ids start at `4`

But the virtual ids are assigned only by scanning rows that are:

- `enabled == true`
- `deleted == false`

So the effective virtual ids are position-dependent:

```text
physical: 1 2 3
mixed row A (enabled, not deleted) -> virtual id 4
mixed row B (disabled)             -> no virtual id
mixed row C (enabled, not deleted) -> virtual id 5
```

That is why `stable_id` exists:

- virtual id is ephemeral and depends on current enabled-row order
- `stable_id` is persistent and is used to remap old painted virtual assignments onto the rebuilt row list

### Concrete Row Examples

Simple custom pair:

```text
1,2,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u17
```

Meaning:

- custom row combining physical filaments 1 and 2
- enabled
- 50% B mix
- simple mode
- no Local-Z cap
- no bias offsets
- not deleted
- not from auto-generation
- stable id `17`

Auto-generated pair tombstoned by the user:

```text
1,3,0,0,50,0,g,w,m2,z0,xa0,xb0,d1,o1,u4
```

Meaning:

- auto row for pair `(1,3)`
- no longer active
- explicitly tombstoned
- still remembered as an auto-origin row
- stable id `4` retained so remap logic can reason about it

Grouped manual wall pattern:

```text
1,2,1,1,50,0,g,w,m2,z0,xa0.02,xb-0.01,d0,o0,u23,11111112,11121111
```

Meaning:

- custom pair `(1,2)`
- grouped manual pattern with two comma-separated wall groups
- component A gets `+0.02 mm` surface offset
- component B gets `-0.01 mm` surface offset

The key parser detail here is:

- the manual pattern itself may contain commas
- parser logic treats any unrecognized trailing tokens as pattern fragments
- those fragments are rejoined with commas during load

So a grouped pattern like `12,21` is not wrapped or escaped specially; it just rides at the end of the row.

### Manual Pattern Semantics

`manual_pattern` is normalized by `MixedFilamentManager::normalize_manual_pattern(...)`.

Accepted tokens:

- `1` = `component_a`
- `2` = `component_b`
- `3`..`9` = direct physical filament ids
- `A` / `a` aliases to `1`
- `B` / `b` aliases to `2`

Separators such as spaces, `/`, `_`, `-`, `|`, `:` and `;` are ignored during normalization.

Examples:

- `1/1/1/1/1/1/1/2, 1/1/1/2/1/1/1/1` normalizes to `11111112,11121111`
- `12,21` means grouped wall resolution
- `2,12` means the outer group is always component B, while the inner group alternates

Important behavior:

- a single group acts like a normal repeating cadence over layers
- multiple comma-separated groups are interpreted as grouped wall/perimeter groups
- when a manual pattern exists, `mix_b_percent` is recomputed from the normalized pattern on load

### Gradient Tokens

`g` and `w` matter when the row is being used as a multicolor gradient row rather than a simple A/B pair.

- `g123` means the row references physical filaments 1, 2, and 3
- `w50/25/25` means the intended relative weights are 50%, 25%, 25%

Normalization rules:

- duplicate ids are removed
- ids outside `1..9` are ignored
- weights are normalized to percentages summing to 100
- if weights are missing or invalid, code falls back to equal weighting

These gradient tokens only become active when:

- the row is not in `Simple` mode
- and the normalized gradient id list has at least 3 components

Otherwise, resolution falls back to the ordinary `component_a` / `component_b` pair logic.

### Surface Offset / Bias Tokens

`xa` and `xb` hold per-component surface offsets in mm.

Examples:

- `xa0.02`
- `xb-0.01`

These are used by the mixed-filament bias logic and apparent-color calculations.

Behavior worth noting:

- values are clamped during load
- grouped wall patterns and same-layer pointillisme paths ignore component surface offsets for effective region ownership
- UI bias helpers often convert a single signed bias choice into a safe one-sided `(xa, xb)` pair

### How Load Actually Rebuilds Rows

Load is not just “deserialize rows into a vector”.

The actual sequence is:

1. Build the current auto-generated row set from physical filament colors.
2. Parse each serialized row.
3. For `custom=0` rows:
   Update the matching existing auto pair if it exists.
4. For `custom=1` rows:
   Append a new custom row directly.
5. Append any newly generated auto rows that were not present in the serialized blob.

This has several consequences:

- serialized auto rows do not create brand-new auto pairs from scratch
- if auto-generation is disabled, serialized auto rows may effectively disappear on load because there is no prebuilt auto row set to update
- custom rows are the only rows that are always reconstructed directly from the blob

### Reordering, Deleting, and Remapping

`stable_id` is the field that makes reorder-safe persistence work.

Example:

- project A has a custom row with virtual id `4` and stable id `17`
- later the row order changes, or an earlier row is deleted
- after reload, that same logical row might now be virtual id `5`

The code does not try to preserve the old numeric virtual id. Instead it:

- finds the row by `stable_id`
- computes its new active virtual id from the rebuilt enabled-row order
- remaps painted references onto the new virtual id

That is the reason the serializer stores both:

- transient active ordering information implicitly, by row order
- durable identity explicitly, through `u<stable_id>`

### Compatibility Quirks

The loader still accepts older historical row shapes:

- older short rows with only `a,b,enabled,mix`
- older rows where token 5 or 6 held a pointillism flag or legacy pattern payload

Current save logic always writes the full modern token sequence, but the parser remains permissive so older FullSpectrum projects still have a chance to load.

### Practical Summary

`mixed_filament_definitions` is doing four jobs at once:

- storing the visible mixed-row list
- storing row identity across regeneration via `stable_id`
- storing behavior knobs per row such as manual patterns, gradient ids/weights, Local-Z caps, and bias offsets
- storing enough tombstone state for auto-generated pairs so project evolution does not silently resurrect deleted rows

## How The Mixed Data Is Rebuilt On Load

On load, the importer:

1. Reads `Metadata/project_settings.config` JSON into `DynamicPrintConfig`.
2. Determines physical filament count from config keys such as `filament_colour`, `filament_settings_id`, `filament_ids`, or `nozzle_diameter`.
3. Rebuilds auto-generated mixed rows from physical filament colors.
4. Re-applies `mixed_filament_definitions` on top through `MixedFilamentManager::load_custom_entries(...)`.
5. Re-applies gradient/local-z settings through `MixedFilamentManager::apply_gradient_settings(...)`.

That means the persisted payload is:

- physical filament base state from normal project config
- mixed row state from `mixed_filament_definitions`

## Package Details Worth Noting

- The top-level `_rels/.rels` always points to `3D/3dmodel.model`.
- It also exposes cover-thumbnail relationships, defaulting to `Metadata/plate_1.png` and `Metadata/plate_1_small.png` unless auxiliary thumbnail assets are available.
- `Metadata/print_profile.config` still exists as a constant in code, but the current exporter does not write it. The active path is `project_settings.config` plus embedded preset JSON files.
- G-code members are paired with `Metadata/plate_<n>.gcode.md5`.
- `Metadata/plate_<n>.json` is currently used for first-layer bbox/pattern metadata, not for the mixed-row definitions themselves.

## Bottom Line

The key packaging takeaway is:

- geometry and plate layout live in `3D/...` and `Metadata/model_settings.config`
- slice outputs live in `Metadata/slice_info.config`, `Metadata/plate_<n>.gcode*`, and the image sidecars
- FullSpectrum-specific behavior is mostly persisted in `Metadata/project_settings.config`
- the most important FullSpectrum payload is the compact `mixed_filament_definitions` string, which preserves virtual filament rows, stable ids, grouped/manual patterns, gradient component sets, Local-Z limits, and bias offsets

## Relevant Code

- `src/libslic3r/Format/bbs_3mf.cpp`
- `src/libslic3r/Format/bbs_3mf.hpp`
- `src/libslic3r/MixedFilament.cpp`
- `src/libslic3r/MixedFilament.hpp`
- `src/libslic3r/PrintConfig.cpp`
- `src/libslic3r/Config.cpp`
- `src/slic3r/GUI/Plater.cpp`
- `src/Snapmaker_Orca.cpp`
