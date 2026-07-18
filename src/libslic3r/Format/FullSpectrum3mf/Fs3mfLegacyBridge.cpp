#include "Fs3mfLegacyBridge.hpp"

#include "Fs3mfConstants.hpp"
#include "Fs3mfIds.hpp"

#include "../../MixedFilament.hpp"
#include "../../PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace Slic3r::FullSpectrum3mf {

namespace {

struct CanonicalPairRefs
{
    MixedFilamentPhysicalRef component_a;
    MixedFilamentPhysicalRef component_b;
};

std::vector<std::string> string_values(const DynamicPrintConfig &config, const std::string &key)
{
    if (const auto *opt = config.option<ConfigOptionStrings>(key); opt != nullptr)
        return opt->values;
    return {};
}

std::vector<double> double_values(const DynamicPrintConfig &config, const std::string &key)
{
    if (const auto *opt = config.option<ConfigOptionFloats>(key); opt != nullptr)
        return opt->values;
    return {};
}

std::string value_at(const std::vector<std::string> &values, size_t idx, const std::string &fallback = {})
{
    return idx < values.size() ? values[idx] : fallback;
}

std::string unique_id(const std::string &base_id, size_t one_based_index, std::unordered_set<std::string> &used_ids)
{
    if (used_ids.insert(base_id).second)
        return base_id;

    size_t attempt = 0;
    for (;;) {
        std::string candidate = base_id + "_" + std::to_string(one_based_index);
        if (attempt != 0)
            candidate += "_" + std::to_string(attempt);
        if (used_ids.insert(candidate).second)
            return candidate;
        ++attempt;
    }
}

double value_at(const std::vector<double> &values, size_t idx, double fallback)
{
    return idx < values.size() ? values[idx] : fallback;
}

unsigned int physical_index_from_ref(const std::string &ref, const std::vector<std::string> &physical_refs)
{
    auto it = std::find(physical_refs.begin(), physical_refs.end(), ref);
    if (it == physical_refs.end())
        return 0;
    return unsigned(std::distance(physical_refs.begin(), it) + 1);
}

std::string physical_ref_from_index(unsigned int index, const std::vector<std::string> &physical_refs)
{
    return index >= 1 && index <= physical_refs.size() ? physical_refs[index - 1] : std::string();
}

std::optional<ManualPattern> manual_pattern_from_definition(const MixedFilamentDefinition &definition,
                                                            const std::vector<std::string> &physical_refs)
{
    if (!definition.recipe.manual_pattern || definition.recipe.manual_pattern->groups.empty())
        return std::nullopt;

    ManualPattern out;
    const MixedFilamentPrimaryPairView pair = definition.recipe.blend.primary_pair_or(0, 0);
    const unsigned int component_a = pair.component_a.id;
    const unsigned int component_b = pair.component_b.id;
    for (const std::vector<MixedFilamentPhysicalRef> &group : definition.recipe.manual_pattern->groups) {
        std::vector<std::string> steps;
        steps.reserve(group.size());
        for (const MixedFilamentPhysicalRef &step : group) {
            if (step.id == component_a) {
                steps.emplace_back("component_a");
            } else if (step.id == component_b) {
                steps.emplace_back("component_b");
            } else {
                const std::string ref = physical_ref_from_index(step.id, physical_refs);
                if (!ref.empty())
                    steps.emplace_back("physical:" + ref);
            }
        }
        if (!steps.empty())
            out.groups.emplace_back(std::move(steps));
    }

    return out.groups.empty() ? std::nullopt : std::optional<ManualPattern>(std::move(out));
}

std::optional<Gradient> gradient_from_definition(const MixedFilamentDefinition &definition,
                                                 const std::vector<std::string> &physical_refs)
{
    const bool spatial_gradient = definition.behavior.gradient.enabled;
    const size_t minimum_components = spatial_gradient ? 2 : 3;
    if (definition.recipe.blend.components.size() < minimum_components ||
        (spatial_gradient && definition.behavior.distribution != MixedFilamentDistributionMode::LayerCycle))
        return std::nullopt;

    Gradient out;
    for (const MixedFilamentWeightedComponent &component : definition.recipe.blend.components) {
        const std::string ref = physical_ref_from_index(component.filament.id, physical_refs);
        if (ref.empty())
            continue;
        out.component_refs.emplace_back(ref);
        out.weights.emplace_back(std::max(0, component.percent));
    }

    if (out.component_refs.size() < minimum_components)
        return std::nullopt;
    if (out.weights.size() != out.component_refs.size())
        out.weights.assign(out.component_refs.size(), 1);
    out.enabled = spatial_gradient;
    if (spatial_gradient) {
        out.component_a_start = std::clamp(double(definition.behavior.gradient.component_a_start), 0.01, 0.99);
        out.component_a_end = std::clamp(double(definition.behavior.gradient.component_a_end), 0.01, 0.99);
        out.stop_positions.reserve(definition.behavior.gradient.stop_positions.size());
        for (const float position : definition.behavior.gradient.stop_positions)
            out.stop_positions.emplace_back(std::clamp(double(position), 0.0, 1.0));
    }
    return out;
}

std::optional<MixedFilamentManualPattern> definition_manual_pattern_from_canonical(
    const std::optional<ManualPattern> &manual_pattern,
    const CanonicalPairRefs            &pair,
    const std::vector<std::string>     &physical_refs)
{
    if (!manual_pattern || manual_pattern->groups.empty())
        return std::nullopt;

    MixedFilamentManualPattern out;
    for (const std::vector<std::string> &group : manual_pattern->groups) {
        std::vector<MixedFilamentPhysicalRef> refs;
        refs.reserve(group.size());
        for (const std::string &step : group) {
            if (step == "component_a") {
                refs.push_back(pair.component_a);
            } else if (step == "component_b") {
                refs.push_back(pair.component_b);
            } else if (step.rfind("physical:", 0) == 0) {
                const unsigned int index = physical_index_from_ref(step.substr(9), physical_refs);
                if (index != 0)
                    refs.push_back({ index });
            }
        }
        if (!refs.empty())
            out.groups.emplace_back(std::move(refs));
    }

    return out.groups.empty() ? std::nullopt : std::optional<MixedFilamentManualPattern>(std::move(out));
}

std::optional<MixedFilamentWeightedBlend> definition_gradient_from_canonical(
    const std::optional<Gradient>     &gradient,
    const CanonicalPairRefs           &pair,
    int                                component_b_percent,
    const std::vector<std::string>    &physical_refs)
{
    const size_t minimum_components = gradient && gradient->enabled ? 2 : 3;
    if (!gradient || gradient->component_refs.size() < minimum_components)
        return std::nullopt;

    MixedFilamentWeightedBlend out;
    std::unordered_set<unsigned int> seen;

    const bool use_weights = gradient->weights.size() == gradient->component_refs.size();
    const auto parsed_weight_for = [&](unsigned int index) -> std::optional<int> {
        if (!use_weights)
            return std::nullopt;
        for (size_t i = 0; i < gradient->component_refs.size(); ++i)
            if (physical_index_from_ref(gradient->component_refs[i], physical_refs) == index)
                return std::max(0, gradient->weights[i]);
        return std::nullopt;
    };
    const auto add_index = [&](unsigned int index, int fallback_weight) {
        if (index == 0 || !seen.insert(index).second)
            return;
        const int weight = use_weights ? parsed_weight_for(index).value_or(fallback_weight) : 0;
        out.components.push_back({ { index }, weight });
    };

    const int pair_b_percent = std::clamp(component_b_percent, 0, 100);
    add_index(pair.component_a.id, 100 - pair_b_percent);
    add_index(pair.component_b.id, pair_b_percent);
    for (const std::string &ref : gradient->component_refs)
        add_index(physical_index_from_ref(ref, physical_refs), 0);

    return out.components.size() < minimum_components ? std::nullopt : std::optional<MixedFilamentWeightedBlend>(std::move(out));
}

std::string distribution_mode_from_definition(MixedFilamentDistributionMode mode)
{
    return mode == MixedFilamentDistributionMode::LayerCycle ? "layer_cycle" : "simple";
}

MixedFilamentDistributionMode definition_distribution_mode(const std::string &mode, bool)
{
    if (mode == "layer_cycle" || mode == "height_weighted")
        return MixedFilamentDistributionMode::LayerCycle;
    return MixedFilamentDistributionMode::Simple;
}

} // namespace

Materials materials_from_project_config(const DynamicPrintConfig &config)
{
    Materials materials;
    materials.kind = KIND_MATERIALS;
    materials.schema_version = PROFILE_VERSION;

    std::vector<std::string> colours = string_values(config, "filament_colour");
    if (colours.empty())
        colours = string_values(config, "default_filament_colour");
    const std::vector<std::string> preset_names = string_values(config, "filament_settings_id");
    const std::vector<std::string> filament_ids = string_values(config, "filament_ids");
    const std::vector<std::string> filament_types = string_values(config, "filament_type");
    const std::vector<double> diameters = double_values(config, "filament_diameter");

    size_t count = std::max({colours.size(), preset_names.size(), filament_ids.size(), diameters.size()});
    if (count == 0)
        count = 1;

    materials.physical_filaments.reserve(count);
    std::unordered_set<std::string> used_ids;
    for (size_t i = 0; i < count; ++i) {
        PhysicalFilament filament;
        filament.source_index = i + 1;
        filament.color = value_at(colours, i, "#FFFFFF");
        filament.display_name = value_at(preset_names, i, "Filament " + std::to_string(i + 1));
        filament.material_family = value_at(filament_types, i, {});
        filament.diameter_mm = value_at(diameters, i, 1.75);
        filament.id = unique_id(physical_filament_id_from_source(i + 1,
                                                                 value_at(filament_ids, i, {}),
                                                                 filament.display_name,
                                                                 filament.color),
                                i + 1,
                                used_ids);
        materials.physical_filaments.emplace_back(std::move(filament));
    }

    return materials;
}

std::vector<std::string> physical_filament_refs(const Materials &materials)
{
    std::vector<std::string> refs;
    refs.reserve(materials.physical_filaments.size());
    for (const PhysicalFilament &filament : materials.physical_filaments)
        refs.emplace_back(filament.id);
    return refs;
}

MixedFilaments mixed_filaments_from_manager(const MixedFilamentManager    &manager,
                                            const std::vector<std::string> &physical_refs)
{
    MixedFilaments mixed;
    mixed.kind = KIND_MIXED_FILAMENTS;
    mixed.schema_version = PROFILE_VERSION;

    std::unordered_set<std::string> used_ids(physical_refs.begin(), physical_refs.end());
    size_t row_idx = 0;
    for (const MixedFilamentDefinition &definition : manager.mixed_filament_definitions(physical_refs.size())) {
        ++row_idx;
        const MixedFilamentPrimaryPairView pair = definition.recipe.blend.primary_pair_or(0, 0);
        const unsigned int component_a = pair.component_a.id;
        const unsigned int component_b = pair.component_b.id;
        if (component_a == 0 || component_b == 0 ||
            component_a > physical_refs.size() || component_b > physical_refs.size())
            continue;

        VirtualFilament vf;
        vf.legacy_stable_id = definition.identity.stable_id;
        vf.id = unique_id(mixed_filament_id_from_legacy_stable_id(definition.identity.stable_id,
                                                                  std::to_string(component_a) + "|" +
                                                                  std::to_string(component_b) + "|" +
                                                                  std::to_string(pair.component_b_percent)),
                          row_idx,
                          used_ids);
        vf.visibility_state = definition.visibility.tombstoned ? "tombstoned" : "active";
        vf.source_kind = definition.source.kind == MixedFilamentSourceKind::Custom ? "custom" : "auto";
        vf.origin.component_refs = {physical_refs[component_a - 1], physical_refs[component_b - 1]};
        vf.origin.origin_auto_generated = definition.source.origin_auto;
        vf.blend.component_b_percent = pair.component_b_percent;
        vf.distribution.mode = distribution_mode_from_definition(definition.behavior.distribution);
        vf.manual_pattern = manual_pattern_from_definition(definition, physical_refs);
        vf.gradient = gradient_from_definition(definition, physical_refs);
        const std::vector<float> component_offsets = mixed_filament_component_surface_offsets(definition);
        for (size_t component_idx = 0;
             component_idx < definition.recipe.blend.components.size() && component_idx < component_offsets.size();
             ++component_idx) {
            const std::string component_ref =
                physical_ref_from_index(definition.recipe.blend.components[component_idx].filament.id, physical_refs);
            if (component_ref.empty())
                continue;
            vf.surface_bias.component_refs.emplace_back(component_ref);
            vf.surface_bias.component_offsets_mm.emplace_back(component_offsets[component_idx]);
        }
        vf.surface_bias.component_a_offset_mm = component_offsets.empty() ? 0.0 : component_offsets[0];
        vf.surface_bias.component_b_offset_mm = component_offsets.size() < 2 ? 0.0 : component_offsets[1];
        vf.surface_bias.application = definition.behavior.surface_bias.perimeter_modulation ? "external_perimeter" : "region_mask";
        if (definition.behavior.local_z.max_sublayers > 0)
            vf.local_z = LocalZ{definition.behavior.local_z.max_sublayers, "standard-pair-split"};
        mixed.virtual_filaments.emplace_back(std::move(vf));
    }

    return mixed;
}

MixedFilamentManager manager_from_mixed_filaments(const MixedFilaments          &mixed_filaments,
                                                  const std::vector<std::string> &filament_colours,
                                                  const std::vector<std::string> &physical_refs)
{
    MixedFilamentManager manager;
    std::vector<MixedFilamentDefinition> definitions;
    definitions.reserve(mixed_filaments.virtual_filaments.size());

    for (const VirtualFilament &vf : mixed_filaments.virtual_filaments) {
        if (vf.origin.component_refs.size() != 2)
            continue;

        const unsigned int a = physical_index_from_ref(vf.origin.component_refs[0], physical_refs);
        const unsigned int b = physical_index_from_ref(vf.origin.component_refs[1], physical_refs);
        if (a == 0 || b == 0)
            continue;

        MixedFilamentDefinition definition;
        definition.recipe.blend.components = {
            {{a}, 100 - std::clamp(vf.blend.component_b_percent, 0, 100)},
            {{b}, std::clamp(vf.blend.component_b_percent, 0, 100)}
        };
        definition.identity.stable_id = vf.legacy_stable_id;
        if (definition.identity.stable_id == 0 && vf.id.rfind("mix_", 0) == 0) {
            try {
                definition.identity.stable_id = std::stoull(vf.id.substr(4));
            } catch (...) {
                definition.identity.stable_id = 0;
            }
        }
        definition.visibility.tombstoned = vf.visibility_state == "tombstoned";
        definition.source.kind = vf.source_kind == "custom" ? MixedFilamentSourceKind::Custom : MixedFilamentSourceKind::AutoGenerated;
        definition.source.origin_auto = vf.origin.origin_auto_generated;
        const CanonicalPairRefs pair{{a}, {b}};
        definition.recipe.manual_pattern =
            definition_manual_pattern_from_canonical(vf.manual_pattern, pair, physical_refs);
        const std::optional<MixedFilamentWeightedBlend> gradient =
            definition_gradient_from_canonical(vf.gradient, pair, vf.blend.component_b_percent, physical_refs);
        if (definition.recipe.manual_pattern) {
            definition.recipe.kind = MixedFilamentRecipeKind::ManualPattern;
            if (gradient)
                definition.recipe.blend = *gradient;
        } else {
            definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
            if (gradient)
                definition.recipe.blend = *gradient;
        }
        definition.behavior.distribution = definition_distribution_mode(vf.distribution.mode, gradient.has_value());
        if (vf.gradient) {
            definition.behavior.gradient.enabled = vf.gradient->enabled;
            definition.behavior.gradient.component_a_start = float(std::clamp(vf.gradient->component_a_start, 0.01, 0.99));
            definition.behavior.gradient.component_a_end = float(std::clamp(vf.gradient->component_a_end, 0.01, 0.99));
            definition.behavior.gradient.stop_positions.clear();
            definition.behavior.gradient.stop_positions.reserve(vf.gradient->stop_positions.size());
            for (const double position : vf.gradient->stop_positions)
                definition.behavior.gradient.stop_positions.emplace_back(float(std::clamp(position, 0.0, 1.0)));
        }
        definition.behavior.local_z.max_sublayers = vf.local_z ? std::max(0, vf.local_z->max_sublayers) : 0;
        definition.behavior.surface_bias.component_a_offset_mm = float(vf.surface_bias.component_a_offset_mm);
        definition.behavior.surface_bias.component_b_offset_mm = float(vf.surface_bias.component_b_offset_mm);
        definition.behavior.surface_bias.perimeter_modulation = vf.surface_bias.application == "external_perimeter";
        if (!vf.surface_bias.component_refs.empty() &&
            vf.surface_bias.component_refs.size() == vf.surface_bias.component_offsets_mm.size()) {
            std::vector<float> component_offsets(definition.recipe.blend.components.size(), 0.f);
            bool complete = true;
            for (size_t component_idx = 0; component_idx < definition.recipe.blend.components.size(); ++component_idx) {
                const std::string component_ref =
                    physical_ref_from_index(definition.recipe.blend.components[component_idx].filament.id, physical_refs);
                const auto ref_it = std::find(vf.surface_bias.component_refs.begin(),
                                              vf.surface_bias.component_refs.end(),
                                              component_ref);
                if (ref_it == vf.surface_bias.component_refs.end()) {
                    complete = false;
                    break;
                }
                const size_t offset_idx = size_t(std::distance(vf.surface_bias.component_refs.begin(), ref_it));
                component_offsets[component_idx] = float(vf.surface_bias.component_offsets_mm[offset_idx]);
            }
            if (complete)
                set_mixed_filament_component_surface_offsets(definition, component_offsets);
        }
        definitions.emplace_back(std::move(definition));
    }

    manager.set_mixed_filament_definitions(std::move(definitions), filament_colours);
    manager.set_display_context(MixedFilamentDisplayContext{filament_colours.size(), filament_colours});
    return manager;
}

std::string legacy_rows_from_mixed_filaments(const MixedFilaments          &mixed_filaments,
                                             const std::vector<std::string> &physical_refs)
{
    std::vector<std::string> colours(physical_refs.size(), "#FFFFFF");
    MixedFilamentManager manager = manager_from_mixed_filaments(mixed_filaments, colours, physical_refs);
    return manager.serialize_custom_entries();
}

} // namespace Slic3r::FullSpectrum3mf
