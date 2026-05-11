# Mixed Filament Refactor

This document describes the current mixed filament model after the refactor.
Use `doc/fullspectrum-3mf-vnext-schema.md` as the canonical package schema reference;
`doc/fullspectrum-3mf-packaging.md` describes the older compact project-settings layout.

## Goals

- Make the in-memory data model explain the feature instead of the legacy encoding.
- Keep compatibility with existing `mixed_filament_definitions` rows and older 3MF projects.
- Give UI and 3MF code a typed object to work with.
- Represent two-filament blends and multi-filament blends through one weighted blend shape.
- Keep exact manual/perimeter patterns separate from the aggregate blend summary.
- Remove runtime dependency on the old `enabled` bit.

## Main Boundary

There is one canonical in-memory shape and one legacy boundary shape:

```cpp
struct MixedFilamentLegacyRow
{
    unsigned int component_a;
    unsigned int component_b;
    uint64_t stable_id;
    int ratio_a;
    int ratio_b;
    int mix_b_percent;
    std::string manual_pattern;
    std::string gradient_component_ids;
    std::string gradient_component_weights;
    int distribution_mode;
    int local_z_max_sublayers;
    float component_a_surface_offset;
    float component_b_surface_offset;
    bool enabled;      // legacy column only
    bool deleted;
    bool custom;
    bool origin_auto;
    std::string display_color;
};
```

`MixedFilamentLegacyRow` is the compact compatibility row. It is still flat and
pair-first because existing project settings and legacy 3MF projections need that
layout. The manager does not own this as its source of truth anymore; it rebuilds
legacy rows from typed definitions on demand.

The typed domain view is:

```cpp
struct MixedFilamentDefinition
{
    MixedFilamentIdentity     identity;
    MixedFilamentSource       source;
    MixedFilamentVisibility   visibility;
    MixedFilamentRecipe       recipe;
    MixedFilamentBehavior     behavior;
    MixedFilamentPresentation presentation;
};
```

This is the source of truth for the manager, UI, package import/export, and
higher-level feature logic.

## Recipe Shape

The recipe now has one aggregate blend representation:

```cpp
struct MixedFilamentWeightedComponent
{
    MixedFilamentPhysicalRef filament;
    int                      percent;
};

struct MixedFilamentWeightedBlend
{
    std::vector<MixedFilamentWeightedComponent> components;
};

struct MixedFilamentManualPattern
{
    std::vector<std::vector<MixedFilamentPhysicalRef>> groups;
};

struct MixedFilamentRecipe
{
    MixedFilamentWeightedBlend blend;
    std::optional<MixedFilamentManualPattern> manual_pattern;
    MixedFilamentRecipeKind kind;
};
```

`blend.components` are physical filament references, not display colors. A normal
two-filament mix is just a two-component weighted blend. Three or more components
represent the explicit multi-color blend path.

The first two blend components remain the primary A/B pair when compatibility or
runtime pair behavior needs one. That keeps legacy tokens, Local-Z pair splitting,
surface bias, and vNext `origin.component_refs` deterministic.

Manual patterns are separate because they are not just percentages. They preserve
exact grouped/perimeter sequencing:

```cpp
recipe.kind = MixedFilamentRecipeKind::ManualPattern;
recipe.manual_pattern = {
    {
        { {1}, {2}, {1}, {2} },
        { {2}, {1}, {2}, {1} }
    }
};
```

For manual patterns, `recipe.blend` remains populated as an aggregate summary.
That summary is used by UI cards, display color, legacy compatibility, Local-Z
defaults, and canonical origin selection. The exact print sequence still comes
from `manual_pattern.groups`.

## Behavior Shape

Behavior that used to be scattered across the legacy row is grouped here:

```cpp
struct MixedFilamentBehavior
{
    MixedFilamentDistributionMode distribution;
    MixedFilamentLayerCadence     layer_cadence;
    MixedFilamentLocalZBehavior   local_z;
    MixedFilamentSurfaceBias      surface_bias;
};
```

- `distribution` is `Simple` or `LayerCycle`.
- `layer_cadence` is the old ratio A/B cadence.
- `local_z.max_sublayers` is the row-level Local-Z cap.
- `surface_bias` carries the component A/B XY offsets.

The old legacy value `1` is still accepted by the loader but normalized before
runtime logic sees it.

## Visibility

There is no active runtime `enabled` state anymore. The old `enabled` legacy
column is kept so older rows keep their layout, but non-deleted rows are treated
as available.

The meaningful state is:

```cpp
struct MixedFilamentVisibility
{
    bool tombstoned;
};
```

`tombstoned` maps to legacy `deleted` and canonical vNext `visibility_state:
"tombstoned"`. It means "remember this row as hidden so regeneration does not
bring it back."

## File Layout

Mixed filament code now lives under `src/libslic3r/MixedFilament/`:

| File | Responsibility |
|---|---|
| `Common.cpp` | shared color, numeric, cadence, and key helpers |
| `LegacyRow.cpp` | compact legacy-row parsing and normalization |
| `Definition.cpp` | legacy-row-to-definition and definition-to-legacy-row adapters |
| `Pattern.cpp` | manual pattern parsing and legacy token conversion |
| `Gradient.cpp` | legacy weighted component and weight helpers |
| `Display.cpp` | preview/display color behavior |
| `Preview.cpp` | preview sequence and Local-Z preview helpers |
| `Resolver.cpp` | runtime virtual-to-physical filament resolution |
| `Manager.cpp` | mixed filament list ownership, generation, and mutation |
| `Internal.hpp` | private helper contracts, including legacy-row-only adapters |

`src/libslic3r/MixedFilament.hpp` is the public model and manager API. The
manager stores `std::vector<MixedFilamentDefinition>`. Its
`mixed_filament_legacy_rows()` method returns a read-only legacy-row snapshot for
persistence and compatibility tests.

## Data Boundaries

`MixedFilamentDefinition` is the canonical in-memory object.

- Legacy rows are imported into definitions and exported from definitions for
  `mixed_filament_definitions` compatibility.
- vNext package JSON is imported into definitions and exported from definitions
  for `Metadata/fullspectrum/mixed-filaments.json`.
- The manager owns definitions, not legacy rows.

## Compatibility Mapping

Legacy rows remain supported:

| Legacy Row Field | Typed Definition Field |
|---|---|
| `component_a`, `component_b` | first two `recipe.blend.components` |
| `stable_id` | `identity.stable_id` |
| `deleted` | `visibility.tombstoned` |
| `custom`, `origin_auto` | `source.kind`, `source.origin_auto` |
| `mix_b_percent` | second blend component percent |
| `manual_pattern` | `recipe.manual_pattern.groups` |
| `gradient_component_ids` | 3+ `recipe.blend.components[].filament` |
| `gradient_component_weights` | 3+ `recipe.blend.components[].percent` |
| `distribution_mode` | `behavior.distribution` |
| `ratio_a`, `ratio_b` | `behavior.layer_cadence` |
| `local_z_max_sublayers` | `behavior.local_z.max_sublayers` |
| `component_a_surface_offset`, `component_b_surface_offset` | `behavior.surface_bias` |
| `display_color` | `presentation.display_color` |

The canonical vNext bridge maps the typed definition to
`Metadata/fullspectrum/mixed-filaments.json`. It still serializes the explicit
3+ multi-color component set as canonical `gradient` data because that is the
current vNext package schema. Internally, both pairs and multi-color recipes are
read through the same `MixedFilamentWeightedBlend` aggregate.

## Developer Rules

- Prefer `MixedFilamentDefinition` for UI, package code, and feature logic.
- Treat `MixedFilamentLegacyRow` as a persistence and compatibility adapter
  shape. There is no `MixedFilament` alias anymore.
- Do not mutate manager-owned legacy rows; there are none. Use
  `set_mixed_filament_definition(...)`, `set_mixed_filament_definitions(...)`,
  or the explicit legacy-row adapter setters when crossing old boundaries.
- Use `mixed_filament_blend_component_ids(...)` and
  `mixed_filament_blend_component_weights(...)` when code wants the weighted
  component list.
- Check `definition.recipe.blend.components.size() >= 3` when code specifically
  needs the explicit multi-color path.
- Use `definition.recipe.manual_pattern` for exact grouped/perimeter sequencing.
- Do not use `enabled` for behavior. Use `visibility.tombstoned`.
- Keep legacy-row naming inside compatibility adapters only; outside that layer,
  talk about typed definitions and weighted blends.

## Examples

Two-filament mix:

```cpp
definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
definition.recipe.blend.components = {
    { {1}, 35 },
    { {2}, 65 }
};
definition.behavior.distribution = MixedFilamentDistributionMode::Simple;
```

Three-filament weighted blend:

```cpp
definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
definition.recipe.blend.components = {
    { {1}, 50 },
    { {2}, 25 },
    { {3}, 25 }
};
definition.behavior.distribution = MixedFilamentDistributionMode::LayerCycle;
```

Grouped manual/perimeter pattern:

```cpp
definition.recipe.kind = MixedFilamentRecipeKind::ManualPattern;
definition.recipe.manual_pattern = MixedFilamentManualPattern{
    {
        { {1}, {1}, {1}, {2} },
        { {2}, {1}, {1}, {1} }
    }
};
definition.recipe.blend.components = {
    { {1}, 75 },
    { {2}, 25 }
};
```

## What Is Cleaner Now

- Pair blends are no longer a separate public recipe type.
- The confusing public `MixedFilamentPair` helper is gone; pair conversion is an
  internal legacy-token detail.
- Public helpers use "blend" language instead of "gradient" language.
- Visibility is named as visibility, while the legacy deleted behavior remains
  explicit as `tombstoned`.
- `MixedFilamentManager` now owns typed definitions. Legacy rows are derived
  snapshots, not the backing store.
- The large mixed filament implementation is split by responsibility.

## Remaining Intentional Weirdness

The compact legacy row is still weird because compatibility is weird. It carries
`component_a`, `component_b`, `manual_pattern`, `gradient_component_ids`, and
`gradient_component_weights` in one flat record. That is acceptable as long as it
stays behind adapters.

Canonical vNext still distinguishes `blend` and `gradient` in the package JSON.
That is a package-compatibility contract. The internal object model is free to
offer the simpler aggregate weighted blend view on top of it.
