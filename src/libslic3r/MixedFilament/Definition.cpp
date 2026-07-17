#include "../MixedFilament.hpp"
#include "Internal.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace Slic3r {

using namespace MixedFilamentInternal;

std::optional<MixedFilamentPrimaryPairView> MixedFilamentWeightedBlend::primary_pair() const
{
    if (components.size() < 2)
        return std::nullopt;
    return MixedFilamentPrimaryPairView{components[0].filament, components[1].filament, std::clamp(components[1].percent, 0, 100)};
}

MixedFilamentPrimaryPairView MixedFilamentWeightedBlend::primary_pair_or(unsigned int component_a,
                                                                         unsigned int component_b,
                                                                         int          component_b_percent) const
{
    MixedFilamentPrimaryPairView pair =
        primary_pair().value_or(MixedFilamentPrimaryPairView{{component_a}, {component_b}, std::clamp(component_b_percent, 0, 100)});
    if (pair.component_a.id == 0)
        pair.component_a.id = component_a;
    if (pair.component_b.id == 0)
        pair.component_b.id = component_b;
    pair.component_b_percent = std::clamp(pair.component_b_percent, 0, 100);
    return pair;
}

std::vector<unsigned int> MixedFilamentWeightedBlend::component_ids(size_t num_physical) const
{
    std::vector<unsigned int> ids;
    ids.reserve(components.size());
    for (const MixedFilamentWeightedComponent& component : components) {
        const unsigned int id = component.filament.id;
        if (id >= 1 && (num_physical == 0 || id <= num_physical))
            ids.emplace_back(id);
    }
    return ids;
}

std::vector<int> MixedFilamentWeightedBlend::component_percents(size_t num_physical) const
{
    std::vector<int> percents;
    percents.reserve(components.size());
    for (const MixedFilamentWeightedComponent& component : components) {
        const unsigned int id = component.filament.id;
        if (id < 1 || (num_physical != 0 && id > num_physical))
            continue;
        percents.emplace_back(std::max(0, component.percent));
    }
    return percents;
}

namespace {

std::vector<float> decode_component_surface_offsets(const std::string &encoded, size_t expected_count)
{
    if (encoded.empty() || expected_count == 0)
        return {};

    std::vector<float> offsets;
    std::stringstream stream(encoded);
    std::string token;
    while (std::getline(stream, token, '/')) {
        try {
            size_t consumed = 0;
            const float value = std::stof(token, &consumed);
            if (consumed != token.size() || !std::isfinite(value))
                return {};
            offsets.emplace_back(clamp_surface_offset(value));
        } catch (...) {
            return {};
        }
    }
    return offsets.size() == expected_count ? offsets : std::vector<float>{};
}

std::string encode_component_surface_offsets(const std::vector<float> &offsets)
{
    std::ostringstream stream;
    for (size_t index = 0; index < offsets.size(); ++index) {
        if (index != 0)
            stream << '/';
        stream << format_surface_offset_token(clamp_surface_offset(offsets[index]));
    }
    return stream.str();
}

size_t expected_gradient_stop_count(size_t component_count, bool gradient_enabled)
{
    if (component_count >= 3)
        return 2 * component_count - 1;
    return gradient_enabled ? size_t(3) : size_t(0);
}

MixedFilamentWeightedBlend pair_blend_from_components(unsigned int component_a, unsigned int component_b, int component_b_percent)
{
    const int pct_b = std::clamp(component_b_percent, 0, 100);
    return {{{{component_a}, 100 - pct_b}, {{component_b}, pct_b}}};
}

MixedFilamentLegacyPair legacy_pair_from_blend(const MixedFilamentWeightedBlend& blend)
{
    const MixedFilamentPrimaryPairView pair = blend.primary_pair_or();
    return MixedFilamentLegacyPair{pair.component_a, pair.component_b};
}

MixedFilamentWeightedBlend aggregate_blend_from_manual_pattern(const MixedFilamentManualPattern& pattern,
                                                               const MixedFilamentWeightedBlend& fallback)
{
    std::vector<unsigned int> ordered_ids;
    std::vector<int>          counts;
    for (const std::vector<MixedFilamentPhysicalRef>& group : pattern.groups) {
        for (const MixedFilamentPhysicalRef& ref : group) {
            if (ref.id == 0)
                continue;
            auto it = std::find(ordered_ids.begin(), ordered_ids.end(), ref.id);
            if (it == ordered_ids.end()) {
                ordered_ids.emplace_back(ref.id);
                counts.emplace_back(1);
            } else {
                ++counts[size_t(std::distance(ordered_ids.begin(), it))];
            }
        }
    }

    if (ordered_ids.empty())
        return fallback;

    const auto count_for = [&](unsigned int id) {
        const auto it = std::find(ordered_ids.begin(), ordered_ids.end(), id);
        return it == ordered_ids.end() ? 0 : counts[size_t(std::distance(ordered_ids.begin(), it))];
    };

    std::vector<unsigned int> output_ids;
    std::vector<int>          output_counts;
    output_ids.reserve(ordered_ids.size() + 2);
    output_counts.reserve(ordered_ids.size() + 2);
    const auto add_id = [&](unsigned int id) {
        if (id == 0 || std::find(output_ids.begin(), output_ids.end(), id) != output_ids.end())
            return;
        output_ids.emplace_back(id);
        output_counts.emplace_back(count_for(id));
    };

    const MixedFilamentPrimaryPairView pair = fallback.primary_pair_or(0, 0);
    add_id(pair.component_a.id);
    add_id(pair.component_b.id);
    for (const unsigned int id : ordered_ids)
        add_id(id);

    const std::vector<int> percents = normalize_weight_vector_to_percent(output_counts);
    MixedFilamentWeightedBlend out;
    out.components.reserve(output_ids.size());
    for (size_t i = 0; i < output_ids.size(); ++i)
        out.components.push_back({{output_ids[i]}, percents[i]});
    return out;
}

} // namespace

std::vector<float> mixed_filament_component_surface_offsets(const MixedFilamentDefinition &definition)
{
    const size_t component_count = definition.recipe.blend.components.size();
    if (component_count == 0)
        return {};

    if (definition.behavior.surface_bias.component_offsets_mm.size() == component_count) {
        std::vector<float> offsets = definition.behavior.surface_bias.component_offsets_mm;
        for (float &offset : offsets)
            offset = clamp_surface_offset(offset);
        return offsets;
    }

    std::vector<float> offsets(component_count, definition.behavior.surface_bias.component_b_offset_mm);
    offsets.front() = definition.behavior.surface_bias.component_a_offset_mm;
    for (float &offset : offsets)
        offset = clamp_surface_offset(offset);
    return offsets;
}

void set_mixed_filament_component_surface_offsets(MixedFilamentDefinition &definition, const std::vector<float> &offsets_mm)
{
    const size_t component_count = definition.recipe.blend.components.size();
    std::vector<float> normalized(component_count, 0.f);
    for (size_t index = 0; index < std::min(component_count, offsets_mm.size()); ++index)
        normalized[index] = clamp_surface_offset(offsets_mm[index]);

    definition.behavior.surface_bias.component_offsets_mm = std::move(normalized);
    definition.behavior.surface_bias.component_a_offset_mm =
        definition.behavior.surface_bias.component_offsets_mm.empty() ? 0.f : definition.behavior.surface_bias.component_offsets_mm[0];
    definition.behavior.surface_bias.component_b_offset_mm =
        definition.behavior.surface_bias.component_offsets_mm.size() < 2 ? 0.f : definition.behavior.surface_bias.component_offsets_mm[1];
}

MixedFilamentDefinition mixed_filament_definition_from_legacy_row(const MixedFilamentLegacyRow& row, size_t num_physical)
{
    MixedFilamentDefinition definition;

    definition.identity.stable_id = row.stable_id;

    definition.source.kind        = row.custom ? MixedFilamentSourceKind::Custom : MixedFilamentSourceKind::AutoGenerated;
    definition.source.origin_auto = row.origin_auto;

    definition.visibility.tombstoned = row.deleted;

    const MixedFilamentWeightedBlend legacy_pair_blend = pair_blend_from_components(row.component_a, row.component_b, row.mix_b_percent);
    definition.recipe.manual_pattern =
        mixed_filament_manual_pattern_from_string(row.manual_pattern, row.component_a, row.component_b, num_physical);
    const std::optional<MixedFilamentWeightedBlend> legacy_weighted_blend =
        mixed_filament_weighted_blend_from_legacy_row(row, num_physical);

    if (definition.recipe.manual_pattern) {
        definition.recipe.kind = MixedFilamentRecipeKind::ManualPattern;
        definition.recipe.blend =
            legacy_weighted_blend.value_or(aggregate_blend_from_manual_pattern(*definition.recipe.manual_pattern, legacy_pair_blend));
    } else {
        definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
        definition.recipe.blend = legacy_weighted_blend.value_or(legacy_pair_blend);
    }

    definition.behavior.distribution =
        mixed_filament_distribution_from_legacy_mode(row.distribution_mode,
                                                     legacy_weighted_blend ? row.gradient_component_ids : std::string());
    definition.behavior.layer_cadence.component_a_layers   = row.ratio_a;
    definition.behavior.layer_cadence.component_b_layers   = row.ratio_b;
    definition.behavior.local_z.max_sublayers              = row.local_z_max_sublayers;
    definition.behavior.gradient.enabled                   = row.gradient_enabled;
    definition.behavior.gradient.component_a_start         = row.gradient_start;
    definition.behavior.gradient.component_a_end           = row.gradient_end;
    definition.behavior.gradient.stop_positions =
        decode_gradient_stop_positions(row.gradient_stop_positions,
                                       expected_gradient_stop_count(definition.recipe.blend.components.size(),
                                                                    definition.behavior.gradient.enabled));
    definition.behavior.surface_bias.component_a_offset_mm = row.component_a_surface_offset;
    definition.behavior.surface_bias.component_b_offset_mm = row.component_b_surface_offset;
    const std::vector<float> component_offsets =
        decode_component_surface_offsets(row.component_surface_offsets, definition.recipe.blend.components.size());
    if (!component_offsets.empty())
        set_mixed_filament_component_surface_offsets(definition, component_offsets);

    definition.presentation.display_color = row.display_color;

    return definition;
}

void apply_mixed_filament_definition_to_legacy_row(const MixedFilamentDefinition& definition, MixedFilamentLegacyRow& row)
{
    const MixedFilamentLegacyPair pair = legacy_pair_from_blend(definition.recipe.blend);

    row.component_a                = pair.component_a.id;
    row.component_b                = pair.component_b.id;
    row.stable_id                  = definition.identity.stable_id;
    row.ratio_a                    = std::max(0, definition.behavior.layer_cadence.component_a_layers);
    row.ratio_b                    = std::max(0, definition.behavior.layer_cadence.component_b_layers);
    row.mix_b_percent              = definition.recipe.blend.primary_pair_or().component_b_percent;
    row.distribution_mode          = legacy_distribution_mode_from_mixed_filament_distribution(definition.behavior.distribution);
    row.local_z_max_sublayers      = std::max(0, definition.behavior.local_z.max_sublayers);
    row.gradient_enabled           = definition.behavior.gradient.enabled;
    row.gradient_start             = std::clamp(definition.behavior.gradient.component_a_start, 0.01f, 0.99f);
    row.gradient_end               = std::clamp(definition.behavior.gradient.component_a_end, 0.01f, 0.99f);
    row.gradient_stop_positions    = legacy_gradient_positions_from_float_vector(
        normalize_gradient_stop_position_vector(definition.behavior.gradient.stop_positions,
                                                expected_gradient_stop_count(definition.recipe.blend.components.size(),
                                                                             row.gradient_enabled)));
    if (std::abs(row.gradient_start - row.gradient_end) < MixedFilamentLegacyRow::k_min_gradient_difference)
        row.gradient_enabled = false;
    row.ui_mode                    = row.gradient_enabled ? 3 : -1;
    const std::vector<float> component_offsets = mixed_filament_component_surface_offsets(definition);
    row.component_a_surface_offset = component_offsets.empty() ? 0.f : component_offsets[0];
    row.component_b_surface_offset = component_offsets.size() < 2 ? 0.f : component_offsets[1];
    row.component_surface_offsets = component_offsets.size() >= 3 ? encode_component_surface_offsets(component_offsets) : std::string();
    row.deleted                    = definition.visibility.tombstoned;
    row.enabled                    = !row.deleted;
    row.custom        = definition.source.kind == MixedFilamentSourceKind::Custom;
    row.origin_auto   = definition.source.origin_auto;
    row.display_color = definition.presentation.display_color;

    row.manual_pattern.clear();
    row.gradient_component_ids.clear();
    row.gradient_component_weights.clear();

    if (definition.recipe.manual_pattern) {
        row.manual_pattern = legacy_manual_pattern_from_mixed_filament_pattern(*definition.recipe.manual_pattern, pair);
        if (!row.manual_pattern.empty())
            row.mix_b_percent = mix_percent_from_normalized_pattern(row.manual_pattern);
        if (definition.behavior.distribution == MixedFilamentDistributionMode::LayerCycle && definition.recipe.blend.components.size() >= 3)
            apply_mixed_filament_blend_to_legacy_row(definition.recipe.blend, row);
    } else if (definition.recipe.blend.components.size() >= 3) {
        apply_mixed_filament_blend_to_legacy_row(definition.recipe.blend, row);
    }
}

MixedFilamentLegacyRow mixed_filament_legacy_row_from_definition(const MixedFilamentDefinition& definition)
{
    MixedFilamentLegacyRow row;
    apply_mixed_filament_definition_to_legacy_row(definition, row);
    return row;
}

std::optional<MixedFilamentManualPattern> mixed_filament_manual_pattern_from_string(const std::string& pattern,
                                                                                    unsigned int       component_a,
                                                                                    unsigned int       component_b,
                                                                                    size_t             num_physical)
{
    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(pattern);
    if (normalized.empty())
        return std::nullopt;

    const MixedFilamentLegacyPair pair{{component_a}, {component_b}};
    MixedFilamentManualPattern    out;
    for (const std::string& group : split_manual_pattern_groups(normalized)) {
        std::vector<MixedFilamentPhysicalRef> refs;
        refs.reserve(group.size());
        for (const char token : group) {
            const unsigned int id = physical_filament_from_legacy_pattern_token(token, pair);
            if (id >= 1 && (num_physical == 0 || id <= num_physical))
                refs.push_back({id});
        }
        if (!refs.empty())
            out.groups.emplace_back(std::move(refs));
    }

    return out.groups.empty() ? std::nullopt : std::optional<MixedFilamentManualPattern>(std::move(out));
}

std::string mixed_filament_manual_pattern_string(const MixedFilamentDefinition& definition)
{
    if (!definition.recipe.manual_pattern)
        return {};

    return legacy_manual_pattern_from_mixed_filament_pattern(*definition.recipe.manual_pattern,
                                                            legacy_pair_from_blend(definition.recipe.blend));
}

static bool physical_ref_is_valid(const MixedFilamentPhysicalRef& ref, size_t num_physical)
{
    return ref.id >= 1 && (num_physical == 0 || ref.id <= num_physical);
}

std::vector<unsigned int> mixed_filament_manual_pattern_sequence(const MixedFilamentDefinition& definition, size_t num_physical)
{
    std::vector<unsigned int> sequence;
    if (!definition.recipe.manual_pattern)
        return sequence;

    for (const std::vector<MixedFilamentPhysicalRef>& group : definition.recipe.manual_pattern->groups) {
        sequence.reserve(sequence.size() + group.size());
        for (const MixedFilamentPhysicalRef& ref : group)
            if (physical_ref_is_valid(ref, num_physical))
                sequence.emplace_back(ref.id);
    }
    return sequence;
}

std::vector<unsigned int> mixed_filament_manual_pattern_preview_sequence(const MixedFilamentDefinition& definition,
                                                                         size_t                         num_physical,
                                                                         size_t                         wall_loops)
{
    std::vector<unsigned int> sequence;
    if (!definition.recipe.manual_pattern || num_physical == 0)
        return sequence;

    const auto& groups = definition.recipe.manual_pattern->groups;
    if (groups.empty())
        return sequence;

    if (groups.size() == 1) {
        sequence.reserve(groups.front().size());
        for (const MixedFilamentPhysicalRef& ref : groups.front())
            if (physical_ref_is_valid(ref, num_physical))
                sequence.emplace_back(ref.id);
        return sequence;
    }

    constexpr size_t k_max_preview_cycle = 48;
    size_t           cycle               = 1;
    for (const std::vector<MixedFilamentPhysicalRef>& group : groups) {
        if (group.empty())
            continue;
        cycle = std::lcm(cycle, group.size());
        if (cycle >= k_max_preview_cycle) {
            cycle = k_max_preview_cycle;
            break;
        }
    }

    const size_t preview_wall_loops = std::max<size_t>(1, wall_loops == 0 ? groups.size() : wall_loops);
    sequence.reserve(preview_wall_loops * cycle);
    for (size_t layer_idx = 0; layer_idx < cycle; ++layer_idx) {
        for (size_t wall_idx = 0; wall_idx < preview_wall_loops; ++wall_idx) {
            const auto& group = groups[std::min(wall_idx, groups.size() - 1)];
            if (group.empty())
                continue;
            const MixedFilamentPhysicalRef& ref = group[layer_idx % group.size()];
            if (physical_ref_is_valid(ref, num_physical))
                sequence.emplace_back(ref.id);
        }
    }

    return sequence;
}

std::vector<unsigned int> mixed_filament_weighted_blend_sequence(const MixedFilamentDefinition& definition, size_t num_physical)
{
    const std::vector<unsigned int> ids = definition.recipe.blend.component_ids(num_physical);
    if (ids.empty())
        return {};

    std::vector<int> weights = definition.recipe.blend.component_percents(num_physical);
    if (weights.size() != ids.size())
        weights.assign(ids.size(), 1);
    return build_weighted_gradient_sequence(ids, weights);
}

// ---------------------------------------------------------------------------
// MixedFilamentManager
// ---------------------------------------------------------------------------

} // namespace Slic3r
