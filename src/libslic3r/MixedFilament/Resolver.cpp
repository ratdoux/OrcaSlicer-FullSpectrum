#include "../MixedFilament.hpp"
#include "Internal.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>

namespace Slic3r {

using namespace MixedFilamentInternal;

namespace {

unsigned int resolved_physical_ref(const MixedFilamentPhysicalRef& ref, size_t num_physical)
{
    return ref.id >= 1 && ref.id <= num_physical ? ref.id : 0;
}

} // namespace

unsigned int MixedFilamentManager::resolve(unsigned int filament_id,
                                           size_t       num_physical,
                                           int          layer_index,
                                           float        layer_print_z,
                                           float        layer_height,
                                           bool         force_height_weighted) const
{
    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0)
        return filament_id;

    const MixedFilamentDefinition& definition = m_definitions[size_t(mixed_idx)];
    const MixedFilamentPrimaryPairView pair = definition.recipe.blend.primary_pair_or(1, 2);
    const unsigned int component_a = pair.component_a.id;
    const unsigned int component_b = pair.component_b.id;

    // Manual pattern takes precedence when provided.
    if (definition.recipe.manual_pattern) {
        const std::vector<unsigned int> sequence = mixed_filament_manual_pattern_sequence(definition, num_physical);
        if (!sequence.empty()) {
            const int          pos      = safe_mod(layer_index, int(sequence.size()));
            const unsigned int resolved = sequence[size_t(pos)];
            if (resolved >= 1 && resolved <= num_physical)
                return resolved;
        }
        return component_a;
    }

    // Perimeter-modulated image-map recipes use their layer cadence only to
    // choose which physical material owns the visible wall. The recipe
    // weights control exposure (the perimeter inset / width), not how often a
    // material owns a layer. Keep every selected component exactly once in
    // the ownership cycle: pairs are 1:1, triples 1:1:1 and four-way recipes
    // 1:1:1:1.
    if (definition.behavior.surface_bias.perimeter_modulation) {
        const std::vector<unsigned int> components = definition.recipe.blend.component_ids(num_physical);
        if (!components.empty()) {
            const size_t pos = size_t(safe_mod(layer_index, int(components.size())));
            return components[pos];
        }
    }

    if (definition.recipe.blend.components.size() >= 3) {
        const std::vector<unsigned int> gradient_sequence = mixed_filament_weighted_blend_sequence(definition, num_physical);
        if (!gradient_sequence.empty()) {
            const size_t pos = size_t(safe_mod(layer_index, int(gradient_sequence.size())));
            return gradient_sequence[pos];
        }
    }

    // Height-weighted cadence can be forced by the local-Z planner. The
    // regular gradient height mode keeps historical behavior (custom rows).
    const bool use_height_weighted = force_height_weighted || (m_gradient_mode == 1 && definition.source.kind == MixedFilamentSourceKind::Custom);
    if (use_height_weighted) {
        float h_a = 0.f;
        float h_b = 0.f;
        compute_gradient_heights_from_mix(pair.component_b_percent,
                                          m_nominal_layer_height,
                                          m_min_sublayer_height,
                                          h_a,
                                          h_b);
        const float cycle_h  = std::max(0.01f, h_a + h_b);
        const float z_anchor = (layer_height > 1e-6f) ? std::max(0.f, layer_print_z - 0.5f * layer_height) : std::max(0.f, layer_print_z);
        float       phase    = std::fmod(z_anchor, cycle_h);
        if (phase < 0.f)
            phase += cycle_h;
        return (phase < h_a) ? component_a : component_b;
    }

    const int ratio_a = definition.behavior.layer_cadence.component_a_layers;
    const int ratio_b = definition.behavior.layer_cadence.component_b_layers;
    const int cycle = ratio_a + ratio_b;
    if (cycle <= 0)
        return component_a;

    if (m_gradient_mode == 0 && m_advanced_dithering && definition.source.kind == MixedFilamentSourceKind::Custom)
        return use_component_b_advanced_dither(layer_index, ratio_a, ratio_b) ? component_b : component_a;

    const int pos = safe_mod(layer_index, cycle);
    return (pos < ratio_a) ? component_a : component_b;
}

unsigned int MixedFilamentManager::resolve_perimeter(unsigned int filament_id,
                                                     size_t       num_physical,
                                                     int          layer_index,
                                                     int          perimeter_index,
                                                     float        layer_print_z,
                                                     float        layer_height,
                                                     bool         force_height_weighted) const
{
    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0)
        return filament_id;

    const MixedFilamentDefinition& definition = m_definitions[size_t(mixed_idx)];
    if (definition.recipe.manual_pattern && !definition.recipe.manual_pattern->groups.empty()) {
        const std::vector<std::vector<MixedFilamentPhysicalRef>>& groups = definition.recipe.manual_pattern->groups;
        const size_t group_idx = size_t(std::max(0, perimeter_index));
        const std::vector<MixedFilamentPhysicalRef>& group = groups[std::min(group_idx, groups.size() - 1)];
        if (!group.empty()) {
            const int          pos      = safe_mod(layer_index, int(group.size()));
            const unsigned int resolved = resolved_physical_ref(group[size_t(pos)], num_physical);
            if (resolved >= 1 && resolved <= num_physical)
                return resolved;
        }
    }

    return resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height, force_height_weighted);
}

unsigned int MixedFilamentManager::effective_painted_region_filament_id(unsigned int filament_id,
                                                                        size_t       num_physical,
                                                                        int          layer_index,
                                                                        float        layer_print_z,
                                                                        float        layer_height,
                                                                        float        layer_height_a,
                                                                        float        layer_height_b,
                                                                        float        base_layer_height) const
{
    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0)
        return filament_id;

    const MixedFilamentDefinition& definition = m_definitions[size_t(mixed_idx)];

    if (definition.recipe.manual_pattern && definition.recipe.manual_pattern->groups.size() > 1)
        return filament_id;

    const bool is_custom_mixed = definition.source.kind == MixedFilamentSourceKind::Custom;
    if (!is_custom_mixed && (layer_height_a > 0.f || layer_height_b > 0.f)) {
        const float safe_base = std::max<float>(0.01f, base_layer_height);
        const int   ratio_a   = std::max(1, int(std::lround((layer_height_a > 0.f ? layer_height_a : safe_base) / safe_base)));
        const int   ratio_b   = std::max(1, int(std::lround((layer_height_b > 0.f ? layer_height_b : safe_base) / safe_base)));
        const int   cycle     = ratio_a + ratio_b;

        if (cycle > 0) {
            const int pos = safe_mod(layer_index, cycle);
            const MixedFilamentPrimaryPairView pair = definition.recipe.blend.primary_pair_or(1, 2);
            return pos < ratio_a ? pair.component_a.id : pair.component_b.id;
        }
    }

    return resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height);
}

float MixedFilamentManager::component_surface_offset(unsigned int filament_id,
                                                     size_t       num_physical,
                                                     int          layer_index,
                                                     float        layer_print_z,
                                                     float        layer_height,
                                                     bool         force_height_weighted) const
{
    const std::optional<MixedFilamentDefinition> definition_opt = mixed_filament_definition_from_id(filament_id, num_physical);
    if (!definition_opt)
        return 0.f;

    const MixedFilamentDefinition& definition = *definition_opt;
    if (definition.recipe.manual_pattern && definition.recipe.manual_pattern->groups.size() > 1)
        return 0.f;

    const unsigned int resolved = resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height, force_height_weighted);
    const std::vector<float> component_offsets = mixed_filament_component_surface_offsets(definition);
    for (size_t component_idx = 0;
         component_idx < definition.recipe.blend.components.size() && component_idx < component_offsets.size();
         ++component_idx) {
        if (definition.recipe.blend.components[component_idx].filament.id == resolved)
            return component_offsets[component_idx];
    }
    return 0.f;
}

std::vector<unsigned int> MixedFilamentManager::ordered_perimeter_extruders(unsigned int filament_id,
                                                                            size_t       num_physical,
                                                                            int          layer_index,
                                                                            float        layer_print_z,
                                                                            float        layer_height,
                                                                            bool         force_height_weighted) const
{
    std::vector<unsigned int> ordered;

    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0) {
        ordered.emplace_back(filament_id);
        return ordered;
    }

    const MixedFilamentDefinition& definition = m_definitions[size_t(mixed_idx)];
    if (definition.recipe.manual_pattern && !definition.recipe.manual_pattern->groups.empty()) {
        ordered.reserve(definition.recipe.manual_pattern->groups.size());
        for (size_t group_idx = 0; group_idx < definition.recipe.manual_pattern->groups.size(); ++group_idx) {
            const unsigned int resolved = resolve_perimeter(filament_id, num_physical, layer_index, int(group_idx), layer_print_z,
                                                            layer_height, force_height_weighted);
            if (resolved < 1 || resolved > num_physical)
                continue;
            if (std::find(ordered.begin(), ordered.end(), resolved) == ordered.end())
                ordered.emplace_back(resolved);
        }
        if (!ordered.empty())
            return ordered;
    }

    ordered.emplace_back(resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height, force_height_weighted));
    return ordered;
}

} // namespace Slic3r
