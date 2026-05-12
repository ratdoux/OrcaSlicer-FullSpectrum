# FullSpectrum 3MF Profile v1 Proposal

## Status

Draft proposal for FullSpectrum Profile v1, a vendor-neutral profile on top of 3MF / OPC.

This is not a description of the current Snapmaker Orca FullSpectrum implementation. It is a proposed interoperability profile intended to let slicers, printer manufacturers, CAD tools, and validation tools exchange FullSpectrum-capable `.3mf` files without depending on one application's private project format.

`v1` means the first stable compatibility contract. It does not mean temporary or incomplete. Additive changes can be added as later minor revisions or optional feature modules. Incompatible changes require a new major version or a new feature ID.

The profile has two layers:

- a portable standard layer that any conforming implementation can understand
- a vendor extension layer where manufacturers can add FullSpectrum-specific behavior without redefining the standard layer

## Core Idea

A FullSpectrum package must remain a valid ordinary 3MF package.

FullSpectrum data is additive. It adds portable project semantics, physical material identity, mixed and virtual filament recipes, assignments, paint-state bindings, and optional FullSpectrum extensions. It must not replace the base 3MF geometry document or make the package unreadable to non-FullSpectrum tools.

Vendor extensions may improve FullSpectrum-specific material behavior, calibration, or validation, but they must not silently change the meaning of standard FullSpectrum fields.

If a non-standard extension is required to interpret the project correctly, the package must either:

- declare that extension as required, so unsupported consumers fail closed
- provide a standards-compliant fallback that preserves the intended printable semantics

## Non-FullSpectrum Compatibility

A non-FullSpectrum slicer should still be able to open the file as a normal 3MF.

The baseline compatibility target is:

- geometry opens
- ordinary 3MF metadata remains valid
- ordinary physical filaments/materials that existed in the project remain ordinary physical filaments/materials
- FullSpectrum custom parts are ignored by non-FullSpectrum tools

The profile must not fake mixed or virtual filaments as physical filaments for non-FullSpectrum slicers.

For example, if a FullSpectrum project has:

- physical filament 1: red PLA
- physical filament 2: blue PLA
- virtual mixed filament: red/blue 50 percent

The non-FullSpectrum projection should expose only the real physical filaments, red PLA and blue PLA. It should not invent a third physical "purple" filament/tool. A non-FullSpectrum slicer may open the model, but it is not expected to correctly slice FullSpectrum mixed regions.

Round-tripping through non-FullSpectrum tools is not a compatibility guarantee. If a non-FullSpectrum tool saves the file and drops FullSpectrum parts, that is acceptable; such a tool cannot use the FullSpectrum data anyway. The only baseline promise for non-FullSpectrum tools is ordinary 3MF readability.

FullSpectrum-aware consumers have a stronger rule: when `Metadata/fullspectrum/manifest.json` is present, canonical FullSpectrum parts are authoritative. Legacy Snapmaker/Bambu-style files in the same package are compatibility projections only and must not override canonical materials, assignments, mixed-filament rows, or identity mappings.

## Why A New Profile

The current project package works, but it is not a clean industry interchange format:

- too much meaning is packed into ad hoc files and positional XML / CSV-like payloads
- `mixed_filament_definitions` is compact but not self-describing
- stable logical identity is mixed with transient export order
- project intent and derived slice data are not cleanly separated
- there is no package-level feature negotiation model
- vendor-specific behavior has no formal place to live

The proposed profile replaces that with:

- versioned JSON canonical parts
- stable IDs for semantic entities
- explicit material and assignment records
- conformance classes that let tools implement useful subsets
- standard feature modules for commonly shared behavior
- explicit required / optional feature negotiation
- extension rules for vendor-specific differences
- legacy projections only as compatibility output, not authority

## Relationship To 3MF

This profile deliberately builds on official 3MF and OPC mechanisms:

- 3MF documents are OPC/ZIP packages with parts and relationships.
- `3D/3dmodel.model` remains the geometry root.
- Custom data lives in custom package parts.
- Custom parts are discovered through relationships and the FullSpectrum manifest.
- Canonical FullSpectrum parts should be linked with `MustPreserve`.
- Consumers are expected to ignore unknown namespaces they do not support.
- Trust-sensitive workflows should reuse OPC / 3MF digital signatures.

Official references:

- 3MF specification index: https://3mf.io/spec/
- 3MF Core Specification: https://3mf.io/spec/core-v1-3-0/
- GitHub copy of the 3MF Core Specification: https://github.com/3MFConsortium/spec_core/blob/master/3MF%20Core%20Specification.md

## Design Principles

### 1. Portable Core Before Vendor Detail

The standard profile must define a portable semantic baseline.

A consumer that supports the relevant standard features should not need vendor-specific data to answer basic questions such as:

- what objects and instances exist
- which plate they are on
- which physical materials are available
- which virtual or mixed materials exist
- which objects, volumes, or stable regions are assigned to which material
- what standard blend or distribution behavior is requested
- how existing MMU paint-state numbers map to stable material references

### 2. Extensions Must Not Redefine Core Semantics

Vendor extensions may augment standard data, but must not silently reinterpret it.

For example:

- allowed: a vendor extension adds a preferred purge strategy for a standard mixed filament
- allowed: a vendor extension adds a calibrated mixing model while keeping the standard mixed-material fallback valid
- not allowed: a vendor extension makes `component_b_percent: 50` mean something other than a 50 percent B blend
- not allowed: a vendor extension changes material assignments without declaring a required extension or providing a standard fallback

### 3. Required Means Fail Closed

If ignoring a feature could produce wrong geometry, wrong material assignment, wrong mixed-material behavior, or invalid derived output, the feature must be declared required.

Consumers that do not understand an unknown required feature must not silently slice, print, or rewrite the affected semantics.

### 4. Optional Means Safe To Ignore

If a feature only improves FullSpectrum fidelity while the standard material semantics remain valid without it, it should be optional.

Consumers may ignore optional data and still use the standard data. Editors should preserve optional unknown parts when round-tripping if they claim preservation support.

### 5. Stable Identity Everywhere

All semantically meaningful entities need stable IDs:

- package
- project
- plates
- objects
- instances
- volumes
- paint or material regions
- physical materials / filaments
- virtual / mixed materials
- extension records when they affect reusable project state

No semantic identity may depend only on:

- array position
- plate index
- object export order
- visible-row order
- filename suffix

Array positions, export-order IDs, numeric extruder IDs, and visible-row indexes are still allowed as runtime handles. They are useful inside a slicer session and in legacy projections. They must be derived from canonical stable IDs when loading and must not be the only saved meaning in FullSpectrum standard parts.

For example, current legacy FullSpectrum files effectively use:

```text
1..N = physical filaments by current array position
N+1.. = visible mixed rows by current visible-row order
```

This is acceptable as a transient projection, but it is not stable project identity. If a physical filament is inserted, deleted, or reordered, or if a mixed row is hidden or moved, the same numeric value may refer to a different material. Profile v1 therefore stores assignments as stable material references such as `fil_...` and `mix_...`, then maps those references back to transient numeric IDs only after import.

### 6. Additive Evolution

The profile should evolve by adding fields, parts, and feature modules.

Existing fields and enum values must not be reused with new meaning. Incompatible changes require a new major schema version or a new feature ID.

### 7. Intent Is Separate From Derivatives

Project intent and derived slice output should be distinct layers.

This profile's core is about portable project intent. Toolpaths and G-code are derived output and should not be part of the required project core.

### 8. FullSpectrum Scope Only

FullSpectrum Profile v1 does not model printer inventory or hardware loading state.

Out of scope:

- which physical spool is loaded
- how many grams remain on a spool
- which AMS, channel, bay, or slot a spool is in
- printer-specific slot mapping
- farm or inventory workflow state

Those concepts are not needed to preserve FullSpectrum project meaning. A receiver needs to know that `fil_red` is red PLA, that `mix_red_blue_50` blends red and blue, and that an object or painted state uses that material. It does not need the sender's spool inventory or machine loading state. If FullSpectrum itself ever uses inventory or loading data for local convenience, that data should live in local application state, not in the portable 3MF profile.

### 9. Human-Readable, Machine-Validatable

Canonical FullSpectrum data should be UTF-8 JSON with published JSON Schemas.

Opaque strings and position-dependent payloads may exist in legacy projections, but they must not be the canonical standard representation.

## Document Classes

Use explicit double-extension document classes:

- `.project.3mf`
  Authoring and editing project. Contains ordinary 3MF geometry plus FullSpectrum project intent.

FullSpectrum Profile v1 does not define standard `.build.3mf` or `.toolpath.3mf` semantics. Prepared builds, G-code, toolpaths, machine command streams, printer inventory, and hardware loading state may exist in existing slicer conventions, private application data, or future standards, but they are outside the FullSpectrum project core.

The manifest must declare the document class.

## Profile v1 Core

Profile v1 core is limited to FullSpectrum project semantics:

- ordinary 3MF geometry remains valid
- physical filament / material identity
- mixed and virtual filament recipes
- assignments to physical or mixed materials
- paint-state bindings for existing MMU painted facets
- stable IDs for semantic entities
- legacy migration from the current Snapmaker/Bambu-style projection
- feature and version negotiation

Spool inventory, AMS/channel/bay mappings, machine slot bindings, farm routing, private preset exchange, and build summaries are not part of FullSpectrum Profile v1 core.

### Minimal Portable Project

A minimal FullSpectrum Profile v1 project contains:

```text
[Content_Types].xml
_rels/.rels
3D/3dmodel.model

Metadata/fullspectrum/manifest.json
Metadata/fullspectrum/project.json
Metadata/fullspectrum/identity-map.json
Metadata/fullspectrum/materials.json
Metadata/fullspectrum/assignments.json
```

`plates.json` is required only when FullSpectrum assignments need stable plate or instance scope that cannot be expressed by the base 3MF instance list alone.

`mixed-filaments.json` is required only when `assignments.json` references virtual or mixed material IDs.

Local-Z data is optional only when it is needed to preserve existing FullSpectrum mixed-material behavior. It may be represented as row-level mixed-filament data or a separate `local-z` part.

### Baseline Conformance Classes

- `fs.project-core-reader.v1`
  Can discover the FullSpectrum manifest, read the core project graph, read stable identity bindings, read materials, read assignments, and read plate assignment scopes when present.
- `fs.project-core-editor.v1`
  Can edit the core project graph and preserve unknown optional standard parts and extension parts if it claims preservation support.
- `fs.assignment-reader.v1`
  Can resolve standard material assignment targets and material references.
- `fs.mixed-filament-reader.v1`
  Can read standard physical and virtual filament definitions, including pair blends, standard distribution modes, gradients, manual wall patterns, tombstones, and surface bias.
- `fs.mixed-filament-editor.v1`
  Can edit mixed filament data while preserving stable IDs and unknown optional fields.
- `fs.local-z-reader.v1`
  Can read optional standard Local-Z controls.
- `fs.extension-preserver.v1`
  Can preserve unknown `MustPreserve` FullSpectrum and vendor extension parts during round-trip edits.
- `fs.legacy-bridge.v1`
  Can read and/or write the legacy Snapmaker/Bambu-style projection.

### Standard Feature Modules

Core modules:

- `fs.project.core.v1`
- `fs.identity-map.v1`
- `fs.materials.core.v1`
- `fs.assignments.v1`

Conditional standard modules:

- `fs.plates.v1`
- `fs.mixed-filaments.v1`

Optional FullSpectrum modules:

- `fs.local-z.v1`
- `fs.legacy-projection.v1`

A file should only declare a feature module when it actually uses that module.

For example, a single-material model may not need `fs.mixed-filaments.v1`. A package containing assignments to virtual mixed filament IDs does need it.

## Required And Optional Features

The manifest divides features into:

- `required`
  Needed to interpret the package's semantics correctly.
- `optional`
  Useful enhancement data that can be ignored without changing core meaning.

Fallbacks should be declared where they apply, usually on an extension entry or optional part entry. Avoid global fallback lists that do not say which semantics are being approximated.

Reader behavior:

- unknown required feature: fail closed for workflows that depend on that feature
- unknown optional feature: ignore and preserve when possible
- unsupported optional vendor feature with a standard fallback: use the fallback
- unsupported required vendor feature with no fallback: do not slice, print, or rewrite affected semantics

## Compatibility Matrix

| Change | How The Profile Handles It | Effect On Older Consumers |
|---|---|---|
| Add optional field | Add JSON field; same major or minor version bump | Older readers ignore it; editors preserve it if possible |
| Add optional standard part | Add part and optional feature ID | Older readers ignore or preserve; standard data remains valid |
| Add required standard feature | Add required feature ID | Older readers fail closed for affected workflows |
| Add vendor behavior that changes printable semantics | Declare required or provide standard fallback | Older readers either use fallback or fail closed |
| Remove a feature | Deprecate, stop writing later, keep reading | Older files continue to load |
| Change meaning of existing field | Not allowed in place | Requires new field, new feature ID, or major version |
| Delete required field | Major version or explicit migration rule | Older readers may fail; newer readers migrate |

## Package Layout

Illustrative package layout:

```text
[Content_Types].xml
_rels/.rels
3D/3dmodel.model

Metadata/fullspectrum/manifest.json
Metadata/fullspectrum/project.json
Metadata/fullspectrum/identity-map.json
Metadata/fullspectrum/materials.json
Metadata/fullspectrum/assignments.json
Metadata/fullspectrum/plates.json
Metadata/fullspectrum/mixed-filaments.json

Metadata/fullspectrum/local-z.json                        # optional, when Local-Z is stored as a separate part

Metadata/extensions/<reverse-dns-vendor>/...

Metadata/...                                   # optional legacy projection files during migration
```

Notes:

- `3D/3dmodel.model` remains the standard 3MF geometry root.
- Standard FullSpectrum data lives under `Metadata/fullspectrum/`.
- Vendor data lives under `Metadata/extensions/<reverse-dns-vendor>/`.
- Legacy Snapmaker/Bambu-style files are derived compatibility projections only.
- Standard and extension parts that should survive editing should be linked with `MustPreserve`.
- Non-FullSpectrum projections must not invent fake physical filaments for virtual mixed filaments.

## Package Relationships

Top-level package relationships should include:

- StartPart relationship to `/3D/3dmodel.model`
- relationship to `/Metadata/fullspectrum/manifest.json`
- `MustPreserve` relationships for canonical FullSpectrum parts
- `MustPreserve` relationships for extension parts that should survive round trips

Illustrative standard relationship types:

- `https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-manifest`
- `https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-project`
- `https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-identity-map`
- `https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-materials`
- `https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-assignments`
- `https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-plates`
- `https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-mixed-filaments`
- `https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-local-z`

The domain is illustrative. A standardized profile should use a stable public namespace.

## Content Types

Illustrative content types:

- `application/vnd.fullspectrum.manifest+json`
- `application/vnd.fullspectrum.project+json`
- `application/vnd.fullspectrum.identity-map+json`
- `application/vnd.fullspectrum.materials+json`
- `application/vnd.fullspectrum.assignments+json`
- `application/vnd.fullspectrum.plates+json`
- `application/vnd.fullspectrum.mixed-filaments+json`
- `application/vnd.fullspectrum.local-z+json`

Vendor extension parts must use vendor-scoped content types.

## Canonical Parts

### `manifest.json`

The manifest is the entrypoint for FullSpectrum-aware consumers.

Responsibilities:

- declare document class
- declare profile version
- declare required and optional features
- enumerate canonical standard parts
- enumerate extension parts
- declare checksums and media types
- declare which parts are authoritative
- declare per-extension or per-part fallbacks
- declare legacy projections when present

Example mixed-material project:

```json
{
  "kind": "org.fullspectrum.package-manifest",
  "schema_version": "1.0.0",
  "document_class": "project",
  "package_id": "pkg_7f6a3313-2f6d-4e19-9c4a-5f5d8ec1a6d2",
  "features": {
    "required": [
      "fs.project.core.v1",
      "fs.identity-map.v1",
      "fs.materials.core.v1",
      "fs.assignments.v1",
      "fs.plates.v1",
      "fs.mixed-filaments.v1"
    ],
    "optional": [
      "fs.local-z.v1",
      "fs.legacy-projection.v1",
      "com.snapmaker.local-z-optimizer.v1"
    ]
  },
  "parts": [
    {
      "role": "project",
      "path": "/Metadata/fullspectrum/project.json",
      "content_type": "application/vnd.fullspectrum.project+json",
      "required": true,
      "checksum": {
        "algorithm": "sha256",
        "value": "..."
      }
    },
    {
      "role": "identity-map",
      "path": "/Metadata/fullspectrum/identity-map.json",
      "content_type": "application/vnd.fullspectrum.identity-map+json",
      "required": true,
      "checksum": {
        "algorithm": "sha256",
        "value": "..."
      }
    },
    {
      "role": "materials",
      "path": "/Metadata/fullspectrum/materials.json",
      "content_type": "application/vnd.fullspectrum.materials+json",
      "required": true,
      "checksum": {
        "algorithm": "sha256",
        "value": "..."
      }
    },
    {
      "role": "assignments",
      "path": "/Metadata/fullspectrum/assignments.json",
      "content_type": "application/vnd.fullspectrum.assignments+json",
      "required": true,
      "checksum": {
        "algorithm": "sha256",
        "value": "..."
      }
    },
    {
      "role": "plates",
      "path": "/Metadata/fullspectrum/plates.json",
      "content_type": "application/vnd.fullspectrum.plates+json",
      "required": true,
      "checksum": {
        "algorithm": "sha256",
        "value": "..."
      }
    },
    {
      "role": "mixed-filaments",
      "path": "/Metadata/fullspectrum/mixed-filaments.json",
      "content_type": "application/vnd.fullspectrum.mixed-filaments+json",
      "required": true,
      "checksum": {
        "algorithm": "sha256",
        "value": "..."
      }
    }
  ],
  "extensions": [
    {
      "feature": "com.snapmaker.local-z-optimizer.v1",
      "path": "/Metadata/extensions/com.snapmaker/local-z-optimizer.json",
      "content_type": "application/vnd.snapmaker.local-z-optimizer+json",
      "required": false,
      "fallback": {
        "feature": "fs.local-z.v1",
        "part": "/Metadata/fullspectrum/local-z.json",
        "lossiness": "none"
      }
    }
  ],
  "authoritative_sources": {
    "project": "/Metadata/fullspectrum/project.json",
    "identity_map": "/Metadata/fullspectrum/identity-map.json",
    "materials": "/Metadata/fullspectrum/materials.json",
    "assignments": "/Metadata/fullspectrum/assignments.json",
    "plates": "/Metadata/fullspectrum/plates.json",
    "mixed_filaments": "/Metadata/fullspectrum/mixed-filaments.json"
  },
  "legacy_projection": {
    "present": true,
    "non_fullspectrum_policy": "physical-filaments-only",
    "derived_from": [
      "/Metadata/fullspectrum/project.json",
      "/Metadata/fullspectrum/materials.json",
      "/Metadata/fullspectrum/assignments.json",
      "/Metadata/fullspectrum/mixed-filaments.json"
    ],
    "paths": [
      "/Metadata/project_settings.config",
      "/Metadata/model_settings.config",
      "/Metadata/slice_info.config"
    ]
  }
}
```

### `project.json`

Stores the small amount of package-level FullSpectrum project metadata that is not better represented by materials, assignments, mixed-filament recipes, or the identity map.

Suggested sections:

- `project`
- `compatibility`
- `feature_policy`
- `extensions`

`project.json` should not be a flat dump of private slicer config keys. It must not contain machine targets, printer profiles, generic process intent, slicing settings, UI state, inventory state, or prepared-build state.

Generic slicing/process settings are outside FullSpectrum Profile v1. A process-like control belongs in FullSpectrum standard data only when it directly defines FullSpectrum material deposition semantics. For example, multi-perimeter material patterns belong in `mixed-filaments.json`; layer height, supports, speeds, wall counts, infill, nozzle selection, printer choice, and quality presets do not.

### `identity-map.json`

Maps transient 3MF numeric IDs and export positions to stable package IDs.

Example:

```json
{
  "kind": "org.fullspectrum.identity-map",
  "schema_version": "1.0.0",
  "model_object_bindings": [
    {
      "model_object_id": 5,
      "stable_object_id": "obj_0d7aa2f6-a624-4d5b-a75d-0df6f4d2fdf4"
    }
  ],
  "volume_bindings": [
    {
      "model_object_id": 5,
      "model_volume_id": 2,
      "stable_volume_id": "vol_7b1d83db-3f1d-43ad-b1fc-6f8f72b6f6d0"
    }
  ],
  "instance_bindings": [
    {
      "source_build_item_index": 1,
      "stable_instance_id": "inst_99a26356-7b9e-4880-bb7c-f9b0f46f786e"
    }
  ],
  "region_bindings": [
    {
      "stable_region_id": "reg_3cf4b2a0-4a7e-41b1-b7cb-4658e63e7d7a",
      "scope": {
        "stable_volume_id": "vol_7b1d83db-3f1d-43ad-b1fc-6f8f72b6f6d0"
      }
    }
  ]
}
```

### `materials.json`

Stores portable material and physical filament definitions.

This part describes what the physical filament or material is. It must not encode where that material is loaded on a specific machine. Hardware slots, AMS channels, spool bays, extruder mapping, spool inventory, and remaining-spool estimates are out of scope for FullSpectrum Profile v1.

Required / common facts:

- stable physical filament IDs
- display names
- colors
- material family
- diameter
- vendor-neutral material tags
- optional vendor extension references

Example:

```json
{
  "kind": "org.fullspectrum.materials",
  "schema_version": "1.0.0",
  "physical_filaments": [
    {
      "id": "fil_5f3cfef5-40f6-4221-9c83-4bcfa25a7bd0",
      "display_name": "PLA Red",
      "material_family": "PLA",
      "diameter_mm": 1.75,
      "color": "#FF0000"
    }
  ]
}
```

### `assignments.json`

Stores standard material assignments.

Assignments bind stable project targets to material IDs. The assignment value is just an ID reference:

- `fil_...` for physical filament/material
- `mix_...` for virtual or mixed filament

Mixed material regions should not duplicate the mixed recipe. When stable logical paint regions exist, they should reference a stable region ID and assign that region to a material ID.

Example:

```json
{
  "kind": "org.fullspectrum.assignments",
  "schema_version": "1.0.0",
  "assignments": [
    {
      "id": "assign_a1",
      "target": {
        "kind": "volume",
        "stable_volume_id": "vol_7b1d83db-3f1d-43ad-b1fc-6f8f72b6f6d0"
      },
      "material_ref": "fil_5f3cfef5-40f6-4221-9c83-4bcfa25a7bd0"
    },
    {
      "id": "assign_a2",
      "target": {
        "kind": "paint_region",
        "stable_region_id": "reg_3cf4b2a0-4a7e-41b1-b7cb-4658e63e7d7a"
      },
      "material_ref": "mix_4d5d1f5b-98fe-4d13-b4db-96c3a58c0f15"
    }
  ],
  "paint_state_bindings": [
    {
      "scope": {
        "stable_volume_id": "vol_7b1d83db-3f1d-43ad-b1fc-6f8f72b6f6d0"
      },
      "paint_state": 5,
      "material_ref": "mix_4d5d1f5b-98fe-4d13-b4db-96c3a58c0f15"
    }
  ]
}
```

`paint_state_bindings` are for facet-level multi-material painting payloads, including the current Snapmaker/Bambu-style `paint_color` triangle attributes. The numeric `paint_state` is not stable identity. It is a local state number used by the referenced facet payload. The binding gives that local state a stable material meaning.

For Profile v1, producers should not create fake stable paint-region IDs from triangle array offsets alone. Existing facet-level painting data may remain encoded as facet or mesh-selection state, but the material meanings inside that payload must be recoverable through stable `fil_...` or `mix_...` references. Standard `paint_region` assignment targets should be used only when a region can be identified independently from triangle array position and transient material state numbers.

### `plates.json`

Stores only the plate information needed to scope FullSpectrum assignments in multi-plate projects:

- stable plate IDs
- object and instance membership
- optional plate-specific material assignment scope

Plate identity must use stable IDs, not `plate_1`, `plate_2`, or array position. Filenames may still contain UUIDs or friendly aliases.

Plate names, lock state, thumbnails, per-plate process overrides, plate-specific slicer settings, and presentation/UI state are outside FullSpectrum Profile v1.

## Standard Mixed Filaments

### Canonical Part: `mixed-filaments.json`

This replaces `mixed_filament_definitions` as the canonical source of truth for standard mixed and virtual filaments.

It should contain:

- virtual / mixed filament registry
- stable IDs for all virtual filament-like entities
- references to physical filaments from `materials.json`
- blend logic
- distribution logic
- grouped/manual wall patterns
- gradient component sets
- surface bias / expansion data
- tombstone state
- FullSpectrum-specific extension hooks that do not redefine standard semantics

Example:

```json
{
  "kind": "org.fullspectrum.mixed-filaments",
  "schema_version": "1.0.0",
  "virtual_filaments": [
    {
      "id": "mix_4d5d1f5b-98fe-4d13-b4db-96c3a58c0f15",
      "visibility_state": "active",
      "source_kind": "custom",
      "origin": {
        "kind": "pair",
        "component_refs": [
          "fil_5f3cfef5-40f6-4221-9c83-4bcfa25a7bd0",
          "fil_a2ac8cae-d81c-4e31-9b40-565819d881c7"
        ],
        "origin_auto_generated": false
      },
      "blend": {
        "type": "pair_ratio",
        "component_b_percent": 50
      },
      "distribution": {
        "mode": "simple"
      },
      "manual_pattern": null,
      "gradient": null,
      "surface_bias": {
        "component_a_offset_mm": 0.0,
        "component_b_offset_mm": 0.0
      }
    }
  ]
}
```

`origin.kind` is `pair`, and `origin.component_refs` must contain exactly two
distinct physical filaments.

### Standard Row State

The standard mixed row state should cover the semantics currently hidden inside the compact row blob:

| Current Concept | Standard Field |
|---|---|
| `u<stable_id>` | `id` |
| physical `a,b` slots | `origin.component_refs` |
| `deleted` tombstone | `visibility_state: "tombstoned"` |
| `custom` / `origin_auto` | `source_kind` and `origin.origin_auto_generated` |
| `mix` | `blend.component_b_percent` |
| `m<mode>` | `distribution.mode` |
| trailing manual pattern | `manual_pattern.groups` |
| `g<ids>` | `gradient.component_refs` |
| `w<weights>` | `gradient.weights` |
| `xa`, `xb` | `surface_bias.component_a_offset_mm`, `surface_bias.component_b_offset_mm` |

Local-Z row caps such as the current `z<max>` token belong to optional `fs.local-z.v1`, not the required mixed-filament core.

### Distribution Modes

Use string enums, not integers:

- `simple`
- `layer_cycle`
- `height_weighted`

Same-layer pointillisme is not part of FullSpectrum Profile v1. Implementations may keep reading legacy files that contain it, but standard writers should not emit it as a standard distribution mode.

Unknown distribution modes must be handled through feature negotiation.

If a future distribution mode can be ignored while preserving the same printable intent, it may be optional and provide a fallback mode. If ignoring it would change material assignment or slicing behavior, it must be required.

### Manual Pattern Representation

Do not persist grouped wall patterns as flattened comma tricks.

Use explicit structure:

```json
{
  "manual_pattern": {
    "groups": [
      [
        "component_a",
        "component_a",
        "component_a",
        "component_a",
        "component_a",
        "component_a",
        "component_a",
        "component_b"
      ],
      [
        "component_a",
        "component_a",
        "component_a",
        "component_b",
        "component_a",
        "component_a",
        "component_a",
        "component_a"
      ]
    ]
  }
}
```

Allowed step tokens:

- `component_a`
- `component_b`
- `physical:<filament-id>`

### Gradient Representation

Do not encode gradients as `g123` and `w50/25/25`.

Use explicit component references and numeric weights:

```json
{
  "gradient": {
    "component_refs": [
      "fil_1",
      "fil_2",
      "fil_3"
    ],
    "weights": [50, 25, 25]
  }
}
```

`gradient.component_refs` must be distinct physical filament refs. When a package
also provides `origin.component_refs`, readers preserve that origin pair as the
first two in-memory weighted components and append the remaining gradient refs
after it.

### Tombstones

Deleted auto rows are important state and should remain first-class:

```json
{
  "visibility_state": "tombstoned"
}
```

Do not spread this meaning across unrelated booleans without a semantic wrapper.

### Optional Local-Z Feature

`fs.local-z.v1` is optional.

When present, it may add Local-Z controls to mixed filament rows or a separate Local-Z part. Example row-level payload:

```json
{
  "local_z": {
    "max_sublayers": 4,
    "strategy": "standard-pair-split"
  }
}
```

Consumers that do not support `fs.local-z.v1` may still open the project. They must not claim to correctly slice Local-Z-dependent assignments unless a safe standard fallback is declared.

## Extension Model

Vendor extensions are expected and useful. They are not a loophole for redefining the standard.

### Extension Locations

Large or reusable extension data should live in vendor-scoped parts:

```text
Metadata/extensions/com.vendorname/feature-name.json
```

Small annotations may live under an `extensions` object with reverse-DNS keys:

```json
{
  "extensions": {
    "com.vendorname": {
      "calibration_profile": "vendor-default"
    }
  }
}
```

### Extension Feature IDs

Vendor feature IDs must be reverse-DNS scoped:

- `com.snapmaker.local-z-optimizer.v1`
- `com.vendorname.mixing-calibration.v1`
- `org.example.fullspectrum-material-solver.v1`

Vendor feature IDs must not use the `fs.` prefix.

### Extension Rules

Vendor extensions:

- must not redefine standard fields
- must not change standard enum meanings
- must not require private data to interpret standard core fields
- must declare whether they are required or optional
- should provide a standard fallback when possible
- must use vendor-scoped relationship and content types
- should be attached with `MustPreserve` if round-trip preservation matters

### Portable Fallback Rule

Any extension that affects printable semantics must satisfy one of these:

- provide a standards-compliant fallback representation
- declare itself required

Examples:

- A vendor-specific mixing calibration model is optional if standard mixed-material data remains valid without it.
- A vendor-specific material assignment solver is required if no standard material assignment fallback exists.
- Vendor-specific machine command streams, inventory, farm routing, and build summaries are outside FullSpectrum Profile v1.

## Integrity And Provenance

Use standard OPC / 3MF mechanisms for trust-sensitive workflows.

Recommended rules:

- `manifest.json` checksums are advisory integrity hints for fast validation and caching.
- Package authenticity should rely on OPC digital signatures.
- Signatures should cover canonical FullSpectrum parts and any included required extension parts.
- Unsigned packages remain valid unless a consuming workflow explicitly requires signed input.

## Canonical Values

The standard should avoid ambiguity by defining canonical units and value syntax:

- lengths are millimeters
- angles are degrees
- percentages are `0..100` when a field name ends in `_percent`
- blend weights are nonnegative numbers; readers normalize when needed
- colors are sRGB `#RRGGBB`; alpha is allowed only when a field explicitly says so
- IDs are opaque ASCII strings, stable within the package
- UUID-like IDs are recommended, but consumers must not infer semantics from ID spelling
- enum values are lowercase snake case
- JSON numbers are used for numeric fields
- producers should preserve useful precision without excessive noise; around `1e-6 mm` is enough for project metadata, while mesh geometry precision remains governed by 3MF geometry data

## Versioning Rules

Every canonical FullSpectrum part must contain:

- `kind`
- `schema_version`

Versioning rules:

- patch version: clarifications or constraint tightening only
- minor version: additive fields, additive parts, additive optional features
- major version: incompatible semantic change

Reader rules:

- same major, newer minor: load and ignore unknown additive fields
- newer major on required part: fail closed for affected workflows
- newer major on optional part: preserve if possible, ignore otherwise

Feature IDs follow their own major versions. A feature ID with a new major version is a different compatibility contract.

## Unknown Data Rules

### Unknown Fields

JSON schema policy:

- top-level objects should allow unknown properties
- consumers must ignore unknown fields they do not understand
- editors should preserve unknown fields when rewriting parts they do not semantically normalize

### Unknown Parts

Canonical FullSpectrum and vendor extension parts that matter for round-trip editing should be attached with `MustPreserve`.

Unaware consumers may still drop unknown data. This is not considered a failure for non-FullSpectrum tools.

### Unknown Required Features

If a package declares an unknown required feature, a reader may still show geometry or metadata for inspection, but it must not silently perform workflows whose correctness depends on that feature.

For example:

- opening geometry for inspection may be acceptable
- slicing may not be acceptable
- printing may not be acceptable
- saving over the file may not be acceptable unless unknown parts are preserved unchanged

## Legacy And Non-FullSpectrum Compatibility Strategy

Adoption should use dual-read and a write-new-standard policy.

### Phase 1

Readers should support both:

- canonical FullSpectrum standard parts
- current legacy Snapmaker/Bambu projection

Read preference:

1. FullSpectrum standard parts
2. legacy projection fallback

Writers should always save canonical FullSpectrum standard parts.

During the migration window, writers may also save a safe derived legacy projection for older versions. This dual-write compatibility should be treated as a temporary release-window bridge, not as the long-term source of truth.

### Phase 2

Continue dual-read for old files indefinitely or for as long as practical.

Allow users or integrators to disable legacy write for clean standard packages.

Eventually, writers may stop emitting the legacy projection by default once enough releases have shipped with automatic migration support.

### Legacy Detection

A file should be treated as a legacy FullSpectrum file when:

- it does not contain `Metadata/fullspectrum/manifest.json`
- it contains known legacy FullSpectrum state such as `mixed_filament_definitions`
- it contains the current Snapmaker/Bambu-style metadata layout with FullSpectrum-specific project config keys

Legacy detection should be conservative. If a file has only ordinary non-FullSpectrum 3MF content, it should not be treated as a FullSpectrum migration candidate.

### Open-Time Migration Prompt

When a legacy FullSpectrum file is opened successfully, the application should tell the user that the file uses an older FullSpectrum project format and should be re-saved.

Suggested behavior:

- load the legacy file using the legacy importer
- convert the in-memory project state to the standard model
- mark the document as needing save
- show a non-destructive prompt explaining that saving will upgrade the package to FullSpectrum Profile v1

Suggested user-facing message:

```text
This project uses an older FullSpectrum 3MF format.

It has been opened using compatibility mode. Save it again to upgrade it to the current FullSpectrum 3MF standard.
```

The prompt should not block opening the file. The user should be able to inspect, slice, or save-as after conversion.

### Save-Time Migration

Saving from a FullSpectrum-aware application should always write the current FullSpectrum standard format.

If the source file was legacy:

- save should write canonical FullSpectrum parts
- save should preserve ordinary 3MF geometry
- save may write a safe legacy projection during the migration window
- save should not write misleading legacy data for features that cannot be safely represented

After save, the file should no longer be considered legacy by FullSpectrum-aware readers because the manifest and canonical parts are present.

### Migration Window

The project should ship the migration prompt and dual-read support for several releases.

Recommended release policy:

- Release N: introduce Profile v1 writer, dual-read, and prompt-on-open for legacy FullSpectrum files.
- Release N+1 / N+2: keep prompting and continue writing safe legacy projections by default.
- Later release: keep reading legacy files, but optionally stop writing legacy projections by default.

This gives users time to naturally migrate active projects simply by opening and saving them.

### Legacy Projection Rules

If legacy files are written:

- they must be marked as derived compatibility projections in `manifest.json`
- the canonical source of truth remains the FullSpectrum standard parts
- old consumers can keep reading the projection
- new consumers should not infer authority from whichever legacy file they parse first

### Non-FullSpectrum Projection Rules

The non-FullSpectrum projection should be conservative:

- keep valid ordinary 3MF geometry
- keep only real physical filaments/materials as physical filaments/materials
- do not represent mixed or virtual filaments as extra physical tools
- do not write misleading legacy data that suggests mixed regions are ordinary physical-filament regions
- allow non-FullSpectrum slicers to open the model even if they cannot correctly slice FullSpectrum features

### Lossy Legacy Projection

Some standard or vendor features may not have a safe legacy representation.

Exporter policy should be explicit:

- write a lossy legacy projection only when the loss is safe and disclosed
- warn when saving features that old consumers will ignore
- avoid writing legacy data that could cause old consumers to mis-slice silently
- prefer omitting unsafe legacy semantics over writing misleading legacy semantics

## Forward Compatibility Strategy

The profile is forward compatible because:

- every entity is an object with a stable ID
- every canonical part is self-described
- every canonical part is versioned
- feature requirements are explicit
- optional extension data can be ignored and preserved
- unknown fields cannot become accidental manual-pattern tokens
- new optional standard modules can be added without changing existing parsers

## Suggested Normative Rules

If this becomes a public industry profile, these are baseline requirements:

- consumers MUST treat `manifest.json` as the FullSpectrum entrypoint.
- producers MUST keep the package valid as ordinary 3MF geometry.
- producers MUST use stable IDs for semantic entities.
- producers MUST NOT encode semantic meaning solely by array position or filename suffix.
- producers MUST NOT use opaque CSV-like strings for canonical structured data.
- canonical FullSpectrum parts MUST be UTF-8 JSON.
- standard fields and enum values MUST NOT be redefined by vendor extensions.
- unknown optional fields SHOULD be ignored and preserved by conforming FullSpectrum editors.
- unknown required features MUST fail closed for affected workflows.
- vendor extensions that affect printable semantics MUST either be required or provide a standard fallback.
- non-FullSpectrum projections MUST NOT fake mixed or virtual filaments as physical filaments.
- FullSpectrum Profile v1 MUST NOT store spool inventory, AMS/channel/bay state, machine slot binding, farm routing, or private preset exchange as standard data.
- legacy files, when present, MUST be declared as derived projections.

## Standardization Path

To make this credible beyond one fork:

1. Publish the profile and JSON Schemas in a public repository.
2. Define a stable namespace and relationship/content-type registry.
3. Publish conformance classes and test requirements.
4. Publish standard feature modules separately from vendor extensions.
5. Publish test vectors:
   - minimal project
   - single-material project
   - multi-material physical-filament project
   - mixed filament project
   - grouped manual pattern project
   - multicolor gradient project
   - MMU painted facet project with paint-state bindings
   - optional Local-Z project
   - non-FullSpectrum projection with physical filaments only
   - vendor extension with standard fallback
   - vendor extension marked required
   - legacy dual-write package
6. Publish migration rules from the current Snapmaker/Bambu-like layout.
7. Add a reference validator.
8. Add a round-trip preservation test suite for conforming FullSpectrum editors.
9. Add a registry process for proposed standard feature modules.

## Bottom Line

The profile should be an industry contract, not just a cleaner private save format.

The core contract is:

- standard 3MF remains responsible for geometry, OPC packaging, relationships, and signatures
- FullSpectrum standard parts carry portable project semantics
- physical material identity is standard FullSpectrum data
- assignments are explicit ID references
- existing MMU painted facets use paint-state bindings to map local paint states to stable material references
- mixed filament recipes are standard FullSpectrum data, not fake physical filament tools
- non-FullSpectrum tools should still open the model and see real physical filaments only
- vendor extensions live in a clearly scoped extension layer
- required features fail closed
- optional features are safe to ignore
- printable vendor differences need either a standard fallback or an explicit required feature
- legacy Snapmaker/Bambu files remain compatibility projections during migration

That gives FullSpectrum a path from "works in our fork" to "can plausibly be implemented by slicers, printer vendors, and validators without private knowledge."

## Appendix: Mapping From Current Format

Current source:

- `Metadata/project_settings.config`
  Proposed destination: `project.json`, `materials.json`, `assignments.json`, and `mixed-filaments.json`.
- `mixed_filament_definitions`
  Proposed destination: `mixed-filaments.json`.
- current physical filament config keys
  Proposed destination: `materials.json`.
- current object, volume, and painting assignment data
  Proposed destination: `assignments.json` plus `identity-map.json`.
- `Metadata/model_settings.config`
  Proposed destination: `identity-map.json` plus `plates.json`, with geometry remaining in `3D/3dmodel.model`.
- `Metadata/slice_info.config`
  Outside the FullSpectrum Profile v1 core.
- `Metadata/process_settings_<n>.config`, `filament_settings_<n>.config`, `machine_settings_<n>.config`
  Outside the FullSpectrum Profile v1 core; keep in legacy projection if needed for current application compatibility.
- `Metadata/plate_<n>.gcode` and `Metadata/plate_<n>.gcode.md5`
  Outside the FullSpectrum project core. These may remain in legacy projections, vendor extensions, or future standard 3MF toolpath-related mechanisms.

## Appendix: Full Package Schema Skeleton

This is a no-data skeleton for the full FullSpectrum 3MF vNext package shape. Placeholder strings such as `"<package_id>"` are illustrative values; optional or conditional parts should be omitted when unused, along with their manifest entries, content-type overrides, and relationships.

The current Snapmaker Orca implementation intentionally omits `plates.json`; BBS plate data remains the source for plate layout. It is still shown here because the profile schema reserves it as an optional package part.

### Package Layout

```text
[Content_Types].xml
_rels/.rels
3D/3dmodel.model
Metadata/fullspectrum/manifest.json
Metadata/fullspectrum/project.json
Metadata/fullspectrum/identity-map.json
Metadata/fullspectrum/materials.json
Metadata/fullspectrum/assignments.json
Metadata/fullspectrum/mixed-filaments.json
Metadata/fullspectrum/plates.json
Metadata/fullspectrum/local-z.json
Metadata/extensions/com.vendor/feature.json
Metadata/project_settings.config
Metadata/model_settings.config
Metadata/slice_info.config
```

### `[Content_Types].xml`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
  <Default Extension="config" ContentType="application/octet-stream"/>
  <Override PartName="/Metadata/fullspectrum/manifest.json" ContentType="application/vnd.fullspectrum.manifest+json"/>
  <Override PartName="/Metadata/fullspectrum/project.json" ContentType="application/vnd.fullspectrum.project+json"/>
  <Override PartName="/Metadata/fullspectrum/identity-map.json" ContentType="application/vnd.fullspectrum.identity-map+json"/>
  <Override PartName="/Metadata/fullspectrum/materials.json" ContentType="application/vnd.fullspectrum.materials+json"/>
  <Override PartName="/Metadata/fullspectrum/assignments.json" ContentType="application/vnd.fullspectrum.assignments+json"/>
  <Override PartName="/Metadata/fullspectrum/mixed-filaments.json" ContentType="application/vnd.fullspectrum.mixed-filaments+json"/>
  <Override PartName="/Metadata/fullspectrum/plates.json" ContentType="application/vnd.fullspectrum.plates+json"/>
  <Override PartName="/Metadata/fullspectrum/local-z.json" ContentType="application/vnd.fullspectrum.local-z+json"/>
  <Override PartName="/Metadata/extensions/com.vendor/feature.json" ContentType="application/vnd.vendor.feature+json"/>
</Types>
```

### `_rels/.rels`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel" Target="/3D/3dmodel.model"/>
  <Relationship Id="fs-manifest" Type="https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-manifest" Target="/Metadata/fullspectrum/manifest.json"/>
  <Relationship Id="fs-project" Type="https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-project" Target="/Metadata/fullspectrum/project.json"/>
  <Relationship Id="fs-identity-map" Type="https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-identity-map" Target="/Metadata/fullspectrum/identity-map.json"/>
  <Relationship Id="fs-materials" Type="https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-materials" Target="/Metadata/fullspectrum/materials.json"/>
  <Relationship Id="fs-assignments" Type="https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-assignments" Target="/Metadata/fullspectrum/assignments.json"/>
  <Relationship Id="fs-mixed-filaments" Type="https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-mixed-filaments" Target="/Metadata/fullspectrum/mixed-filaments.json"/>
  <Relationship Id="fs-plates" Type="https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-plates" Target="/Metadata/fullspectrum/plates.json"/>
  <Relationship Id="fs-local-z" Type="https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-local-z" Target="/Metadata/fullspectrum/local-z.json"/>
  <Relationship Id="fs-preserve-manifest" Type="http://schemas.openxmlformats.org/package/2006/relationships/mustpreserve" Target="/Metadata/fullspectrum/manifest.json"/>
  <Relationship Id="fs-preserve-project" Type="http://schemas.openxmlformats.org/package/2006/relationships/mustpreserve" Target="/Metadata/fullspectrum/project.json"/>
  <Relationship Id="fs-preserve-identity-map" Type="http://schemas.openxmlformats.org/package/2006/relationships/mustpreserve" Target="/Metadata/fullspectrum/identity-map.json"/>
  <Relationship Id="fs-preserve-materials" Type="http://schemas.openxmlformats.org/package/2006/relationships/mustpreserve" Target="/Metadata/fullspectrum/materials.json"/>
  <Relationship Id="fs-preserve-assignments" Type="http://schemas.openxmlformats.org/package/2006/relationships/mustpreserve" Target="/Metadata/fullspectrum/assignments.json"/>
  <Relationship Id="fs-preserve-mixed-filaments" Type="http://schemas.openxmlformats.org/package/2006/relationships/mustpreserve" Target="/Metadata/fullspectrum/mixed-filaments.json"/>
  <Relationship Id="fs-preserve-plates" Type="http://schemas.openxmlformats.org/package/2006/relationships/mustpreserve" Target="/Metadata/fullspectrum/plates.json"/>
  <Relationship Id="fs-preserve-local-z" Type="http://schemas.openxmlformats.org/package/2006/relationships/mustpreserve" Target="/Metadata/fullspectrum/local-z.json"/>
  <Relationship Id="vendor-feature" Type="https://schemas.vendor.example/3mf/relationships/feature" Target="/Metadata/extensions/com.vendor/feature.json"/>
  <Relationship Id="vendor-feature-preserve" Type="http://schemas.openxmlformats.org/package/2006/relationships/mustpreserve" Target="/Metadata/extensions/com.vendor/feature.json"/>
</Relationships>
```

### `manifest.json`

```json
{
  "kind": "org.fullspectrum.package-manifest",
  "schema_version": "1.0.0",
  "document_class": "project",
  "package_id": "<package_id>",
  "features": {
    "required": [
      "fs.project.core.v1",
      "fs.identity-map.v1",
      "fs.materials.core.v1",
      "fs.assignments.v1"
    ],
    "optional": [
      "fs.mixed-filaments.v1",
      "fs.plates.v1",
      "fs.local-z.v1",
      "fs.legacy-projection.v1"
    ]
  },
  "parts": [
    {
      "role": "project",
      "path": "/Metadata/fullspectrum/project.json",
      "content_type": "application/vnd.fullspectrum.project+json",
      "required": true,
      "checksum": {
        "algorithm": "sha256",
        "value": "<sha256>"
      }
    },
    {
      "role": "identity-map",
      "path": "/Metadata/fullspectrum/identity-map.json",
      "content_type": "application/vnd.fullspectrum.identity-map+json",
      "required": true,
      "checksum": {
        "algorithm": "sha256",
        "value": "<sha256>"
      }
    },
    {
      "role": "materials",
      "path": "/Metadata/fullspectrum/materials.json",
      "content_type": "application/vnd.fullspectrum.materials+json",
      "required": true,
      "checksum": {
        "algorithm": "sha256",
        "value": "<sha256>"
      }
    },
    {
      "role": "assignments",
      "path": "/Metadata/fullspectrum/assignments.json",
      "content_type": "application/vnd.fullspectrum.assignments+json",
      "required": true,
      "checksum": {
        "algorithm": "sha256",
        "value": "<sha256>"
      }
    },
    {
      "role": "mixed-filaments",
      "path": "/Metadata/fullspectrum/mixed-filaments.json",
      "content_type": "application/vnd.fullspectrum.mixed-filaments+json",
      "required": false,
      "checksum": {
        "algorithm": "sha256",
        "value": "<sha256>"
      }
    },
    {
      "role": "plates",
      "path": "/Metadata/fullspectrum/plates.json",
      "content_type": "application/vnd.fullspectrum.plates+json",
      "required": false,
      "checksum": {
        "algorithm": "sha256",
        "value": "<sha256>"
      }
    },
    {
      "role": "local-z",
      "path": "/Metadata/fullspectrum/local-z.json",
      "content_type": "application/vnd.fullspectrum.local-z+json",
      "required": false,
      "checksum": {
        "algorithm": "sha256",
        "value": "<sha256>"
      }
    }
  ],
  "extensions": [
    {
      "feature": "<reverse.dns.vendor.feature.v1>",
      "path": "/Metadata/extensions/com.vendor/feature.json",
      "content_type": "application/vnd.vendor.feature+json",
      "required": false,
      "fallback": {
        "feature": "<fs.standard-fallback.v1>",
        "part": "/Metadata/fullspectrum/<fallback-part>.json",
        "lossiness": "<none|safe|lossy>"
      },
      "checksum": {
        "algorithm": "sha256",
        "value": "<sha256>"
      }
    }
  ],
  "authoritative_sources": {
    "project": "/Metadata/fullspectrum/project.json",
    "identity_map": "/Metadata/fullspectrum/identity-map.json",
    "materials": "/Metadata/fullspectrum/materials.json",
    "assignments": "/Metadata/fullspectrum/assignments.json",
    "mixed_filaments": "/Metadata/fullspectrum/mixed-filaments.json",
    "plates": "/Metadata/fullspectrum/plates.json",
    "local_z": "/Metadata/fullspectrum/local-z.json"
  },
  "legacy_projection": {
    "present": false,
    "non_fullspectrum_policy": "physical-filaments-only",
    "derived_from": [],
    "paths": []
  }
}
```

### `project.json`

```json
{
  "kind": "org.fullspectrum.project",
  "schema_version": "1.0.0",
  "project": {
    "id": "<project_id>",
    "display_name": "<display_name>"
  },
  "feature_policy": {
    "unknown_required_features": "fail_closed"
  },
  "compatibility": {
    "legacy_projection_written": false,
    "non_fullspectrum_policy": "physical-filaments-only"
  },
  "extensions": {}
}
```

### `identity-map.json`

```json
{
  "kind": "org.fullspectrum.identity-map",
  "schema_version": "1.0.0",
  "model_object_bindings": [
    {
      "model_object_id": 0,
      "stable_object_id": "<stable_object_id>"
    }
  ],
  "volume_bindings": [
    {
      "model_object_id": 0,
      "model_volume_id": 0,
      "stable_volume_id": "<stable_volume_id>"
    }
  ],
  "instance_bindings": [
    {
      "source_build_item_index": 0,
      "stable_instance_id": "<stable_instance_id>"
    }
  ],
  "region_bindings": [
    {
      "stable_region_id": "<stable_region_id>",
      "scope": {
        "stable_volume_id": "<stable_volume_id>"
      }
    }
  ]
}
```

### `materials.json`

```json
{
  "kind": "org.fullspectrum.materials",
  "schema_version": "1.0.0",
  "physical_filaments": [
    {
      "id": "<physical_filament_id>",
      "display_name": "<display_name>",
      "material_family": "<material_family>",
      "diameter_mm": 1.75,
      "color": "#RRGGBB",
      "extensions": {}
    }
  ]
}
```

### `assignments.json`

```json
{
  "kind": "org.fullspectrum.assignments",
  "schema_version": "1.0.0",
  "assignments": [
    {
      "id": "<assignment_id>",
      "target": {
        "kind": "volume",
        "stable_object_id": "<stable_object_id>",
        "stable_volume_id": "<stable_volume_id>",
        "stable_region_id": ""
      },
      "material_ref": "<physical_or_mixed_filament_id>"
    }
  ],
  "paint_state_bindings": [
    {
      "scope": {
        "stable_volume_id": "<stable_volume_id>"
      },
      "paint_state": 1,
      "material_ref": "<physical_or_mixed_filament_id>"
    }
  ]
}
```

### `mixed-filaments.json`

```json
{
  "kind": "org.fullspectrum.mixed-filaments",
  "schema_version": "1.0.0",
  "virtual_filaments": [
    {
      "id": "<mixed_filament_id>",
      "visibility_state": "<active|tombstoned>",
      "source_kind": "<auto|custom>",
      "origin": {
        "kind": "pair",
        "component_refs": [
          "<physical_filament_id>",
          "<physical_filament_id>"
        ],
        "origin_auto_generated": false
      },
      "blend": {
        "type": "pair_ratio",
        "component_b_percent": 50
      },
      "distribution": {
        "mode": "<simple|layer_cycle|height_weighted>"
      },
      "manual_pattern": {
        "groups": [
          [
            "component_a",
            "component_b"
          ]
        ]
      },
      "gradient": {
        "component_refs": [
          "<physical_filament_id>",
          "<physical_filament_id>",
          "<physical_filament_id>"
        ],
        "weights": [
          50,
          25,
          25
        ]
      },
      "surface_bias": {
        "component_a_offset_mm": 0.0,
        "component_b_offset_mm": 0.0
      },
      "local_z": {
        "max_sublayers": 0,
        "strategy": "standard-pair-split"
      },
      "extensions": {}
    }
  ]
}
```

### `plates.json`

```json
{
  "kind": "org.fullspectrum.plates",
  "schema_version": "1.0.0",
  "plates": [
    {
      "id": "<plate_id>",
      "source_plate_index": 0,
      "object_instances": [
        {
          "stable_object_id": "<stable_object_id>",
          "stable_instance_id": "<stable_instance_id>"
        }
      ],
      "assignment_scopes": []
    }
  ]
}
```

### `local-z.json`

```json
{
  "kind": "org.fullspectrum.local-z",
  "schema_version": "1.0.0",
  "rules": [
    {
      "id": "<local_z_rule_id>",
      "scope": {
        "stable_volume_id": "<stable_volume_id>"
      },
      "material_ref": "<physical_or_mixed_filament_id>",
      "z_range": {
        "min": 0.0,
        "max": 0.0
      }
    }
  ]
}
```

### Vendor Extension Part

```json
{
  "kind": "<reverse.dns.extension.kind>",
  "schema_version": "1.0.0",
  "feature": "<vendor.feature.v1>",
  "fallback": {
    "standard_parts": [
      "/Metadata/fullspectrum/materials.json",
      "/Metadata/fullspectrum/assignments.json"
    ]
  },
  "data": {}
}
```

## References

- 3MF Core Specification v1.3.0: https://3mf.io/spec/core-v1-3-0/
- 3MF Core Specification source: https://github.com/3MFConsortium/spec_core/blob/master/3MF%20Core%20Specification.md
- 3MF specification index: https://3mf.io/spec/
