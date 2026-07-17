#include "../MixedFilament.hpp"
#include "../FullSpectrumKSPairResidual.hpp"
#include "Internal.hpp"
#include "../filament_mixer.h"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Slic3r {

using namespace MixedFilamentInternal;

namespace {

std::optional<double> physical_td_for_id(const MixedFilamentDisplayContext& context, unsigned int id)
{
    if (!MixedFilamentManager::use_td_for_color_prediction() || id == 0 || id > context.physical_tds.size())
        return std::nullopt;

    const double td = context.physical_tds[id - 1];
    if (!std::isfinite(td) || td <= EPSILON)
        return std::nullopt;
    return td;
}

std::optional<std::string> physical_material_id_for_id(const MixedFilamentDisplayContext& context, unsigned int id)
{
    if (id == 0 || id > context.physical_material_ids.size() || context.physical_material_ids[id - 1].empty())
        return std::nullopt;
    return context.physical_material_ids[id - 1];
}

std::vector<FullSpectrumKSPairResidualColorInput> full_spectrum_inputs_from_mixed_inputs(
    const std::vector<MixedFilamentColorInput>& color_percents)
{
    std::vector<FullSpectrumKSPairResidualColorInput> inputs;
    inputs.reserve(color_percents.size());
    const bool use_td = MixedFilamentManager::use_td_for_color_prediction();
    for (const MixedFilamentColorInput& input : color_percents)
        inputs.push_back({input.color_hex, input.percent, use_td ? input.td_mm : std::nullopt, input.material_id});
    return inputs;
}

double mixed_filament_definition_reference_nozzle_mm(const MixedFilamentDefinition &definition,
                                                      const std::vector<double>      &nozzle_diameters)
{
    double total = 0.0;
    size_t count = 0;
    for (const MixedFilamentWeightedComponent &component : definition.recipe.blend.components) {
        const unsigned int component_id = component.filament.id;
        if (component_id < 1 || component_id > nozzle_diameters.size())
            continue;
        total += std::max(0.05, nozzle_diameters[component_id - 1]);
        ++count;
    }
    return count > 0 ? total / double(count) : 0.4;
}

} // namespace

int mixed_filament_effective_local_z_preview_mix_b_percent(const MixedFilamentDefinition&     definition,
                                                           const MixedFilamentPreviewSettings& preview_settings)
{
    const int mix_b_percent = definition.recipe.blend.primary_pair_or().component_b_percent;
    if (!mixed_filament_definition_uses_local_z(definition, preview_settings.local_z_mode))
        return std::clamp(mix_b_percent, 0, 100);

    if (definition.recipe.manual_pattern)
        return std::clamp(mix_b_percent, 0, 100);

    if (definition.recipe.blend.components.size() >= 3)
        return std::clamp(mix_b_percent, 0, 100);

    const std::vector<double> pass_heights = mixed_filament_local_z_preview_pass_heights(preview_settings.nominal_layer_height,
                                                                                         preview_settings.min_sublayer_height,
                                                                                         preview_settings.preferred_a_height,
                                                                                         preview_settings.preferred_b_height,
                                                                                         mix_b_percent,
                                                                                         0);
    if (pass_heights.empty())
        return std::clamp(mix_b_percent, 0, 100);

    double expected_h_a = preview_settings.preferred_a_height;
    double expected_h_b = preview_settings.preferred_b_height;
    if (expected_h_a <= EPSILON && expected_h_b <= EPSILON) {
        const auto targets = mixed_filament_local_z_pair_heights(preview_settings.nominal_layer_height,
                                                                  preview_settings.min_sublayer_height,
                                                                  mix_b_percent);
        expected_h_a       = targets.first;
        expected_h_b       = targets.second;
    }

    auto choose_start_with_component_a = [](const std::vector<double>& passes, double local_expected_h_a, double local_expected_h_b) {
        double err_ab = 0.0;
        double err_ba = 0.0;
        for (size_t pass_i = 0; pass_i < passes.size(); ++pass_i) {
            const double expected_ab = (pass_i % 2) == 0 ? local_expected_h_a : local_expected_h_b;
            const double expected_ba = (pass_i % 2) == 0 ? local_expected_h_b : local_expected_h_a;
            err_ab += std::abs(passes[pass_i] - expected_ab);
            err_ba += std::abs(passes[pass_i] - expected_ba);
        }
        if (err_ab + 1e-6 < err_ba)
            return true;
        if (err_ba + 1e-6 < err_ab)
            return false;
        return local_expected_h_a >= local_expected_h_b;
    };

    const bool start_with_a = choose_start_with_component_a(pass_heights, expected_h_a, expected_h_b);
    double     total_a      = 0.0;
    double     total_b      = 0.0;
    for (size_t pass_i = 0; pass_i < pass_heights.size(); ++pass_i) {
        const bool even_pass = (pass_i % 2) == 0;
        const bool pass_is_a = even_pass ? start_with_a : !start_with_a;
        if (pass_is_a)
            total_a += pass_heights[pass_i];
        else
            total_b += pass_heights[pass_i];
    }

    const double total = total_a + total_b;
    if (total <= EPSILON)
        return std::clamp(mix_b_percent, 0, 100);
    return std::clamp(int(std::lround(100.0 * total_b / total)), 0, 100);
}

bool mixed_filament_supports_bias_apparent_color(const MixedFilamentDefinition&      definition,
                                                 const MixedFilamentPreviewSettings& preview_settings,
                                                 bool                                bias_mode_enabled)
{
    if (!bias_mode_enabled)
        return false;
    if (mixed_filament_definition_uses_local_z(definition, preview_settings.local_z_mode))
        return false;
    if (definition.recipe.kind != MixedFilamentRecipeKind::WeightedBlend || definition.recipe.blend.components.size() < 2)
        return false;
    const unsigned int component_a = definition.recipe.blend.components.front().filament.id;
    if (component_a < 1)
        return false;
    return std::any_of(definition.recipe.blend.components.begin() + 1,
                       definition.recipe.blend.components.end(),
                       [component_a](const MixedFilamentWeightedComponent &component) {
                           return component.filament.id >= 1 && component.filament.id != component_a;
                       });
}

std::pair<int, int> mixed_filament_apparent_pair_percentages(const MixedFilamentDefinition&      definition,
                                                             const MixedFilamentPreviewSettings& preview_settings,
                                                             const std::vector<double>&          nozzle_diameters,
                                                             bool                                bias_mode_enabled)
{
    std::vector<int> base_percents = normalize_weight_vector_to_percent(definition.recipe.blend.component_percents());
    if (base_percents.size() == 2) {
        const int base_b = mixed_filament_effective_local_z_preview_mix_b_percent(definition, preview_settings);
        base_percents = {100 - base_b, base_b};
    }
    const int base_b = base_percents.empty() ? 0 : std::accumulate(base_percents.begin() + 1, base_percents.end(), 0);
    if (!mixed_filament_supports_bias_apparent_color(definition, preview_settings, bias_mode_enabled))
        return {100 - base_b, base_b};

    const double reference_nozzle_mm = mixed_filament_definition_reference_nozzle_mm(definition, nozzle_diameters);
    const std::vector<int> apparent = MixedFilamentManager::apparent_component_percentages(
        base_percents, mixed_filament_component_surface_offsets(definition), float(reference_nozzle_mm));
    const int apparent_b = apparent.empty() ? base_b : std::accumulate(apparent.begin() + 1, apparent.end(), 0);
    return {100 - apparent_b, apparent_b};
}

std::string compute_mixed_filament_display_color(const MixedFilamentDefinition& definition, const MixedFilamentDisplayContext& context)
{
    constexpr const char* fallback = "#26A69A";
    if (context.num_physical == 0 || context.physical_colors.empty())
        return fallback;

    const MixedFilamentPrimaryPairView pair = definition.recipe.blend.primary_pair_or(0, 0);
    const unsigned int component_a = pair.component_a.id;
    const unsigned int component_b = pair.component_b.id;

    const bool supports_bias =
        mixed_filament_supports_bias_apparent_color(definition, context.preview_settings, context.component_bias_enabled);
    if (supports_bias && definition.recipe.blend.components.size() >= 3) {
        const std::vector<unsigned int> component_ids = definition.recipe.blend.component_ids(context.num_physical);
        const std::vector<int> apparent_percents = MixedFilamentManager::apparent_component_percentages(
            definition.recipe.blend.component_percents(context.num_physical),
            mixed_filament_component_surface_offsets(definition),
            float(mixed_filament_definition_reference_nozzle_mm(definition, context.nozzle_diameters)));
        std::vector<MixedFilamentColorInput> inputs;
        inputs.reserve(std::min(component_ids.size(), apparent_percents.size()));
        for (size_t component_idx = 0;
             component_idx < component_ids.size() && component_idx < apparent_percents.size();
             ++component_idx) {
            const unsigned int component_id = component_ids[component_idx];
            if (component_id < 1 || component_id > context.num_physical || component_id > context.physical_colors.size() ||
                apparent_percents[component_idx] <= 0)
                continue;
            inputs.push_back({context.physical_colors[component_id - 1],
                              apparent_percents[component_idx],
                              physical_td_for_id(context, component_id),
                              physical_material_id_for_id(context, component_id)});
        }
        if (!inputs.empty())
            return MixedFilamentManager::blend_color_multi(inputs);
    }

    if (supports_bias && definition.recipe.blend.components.size() == 2 &&
        component_a >= 1 && component_b >= 1 && component_a <= context.num_physical &&
        component_b <= context.num_physical && component_a <= context.physical_colors.size() &&
        component_b <= context.physical_colors.size()) {
        const auto [apparent_pct_a, apparent_pct_b] = mixed_filament_apparent_pair_percentages(definition, context.preview_settings,
                                                                                               context.nozzle_diameters,
                                                                                               context.component_bias_enabled);
        return MixedFilamentManager::blend_color(context.physical_colors[component_a - 1],
                                                 context.physical_colors[component_b - 1], apparent_pct_a, apparent_pct_b,
                                                 physical_td_for_id(context, component_a), physical_td_for_id(context, component_b),
                                                 physical_material_id_for_id(context, component_a),
                                                 physical_material_id_for_id(context, component_b));
    }

    if (definition.recipe.manual_pattern) {
        const std::vector<unsigned int> sequence = mixed_filament_manual_pattern_preview_sequence(definition, context.num_physical,
                                                                                                  context.preview_settings.wall_loops);
        if (!sequence.empty())
            return blend_display_color_from_sequence(
                context.physical_colors, context.physical_tds, context.physical_material_ids, context.num_physical, sequence, fallback);
    }

    const std::vector<unsigned int> blend_ids = definition.recipe.blend.component_ids(context.num_physical);
    if (blend_ids.size() >= 3) {
        const std::vector<unsigned int> sequence = mixed_filament_weighted_blend_sequence(definition, context.num_physical);
        if (!sequence.empty())
            return blend_display_color_from_sequence(
                context.physical_colors, context.physical_tds, context.physical_material_ids, context.num_physical, sequence, fallback);
    }

    const int effective_mix_b                     = mixed_filament_effective_local_z_preview_mix_b_percent(definition, context.preview_settings);
    const std::vector<unsigned int> pair_sequence = build_effective_pair_preview_sequence(component_a, component_b,
                                                                                          effective_mix_b, false);
    if (!pair_sequence.empty())
        return blend_display_color_from_sequence(
            context.physical_colors, context.physical_tds, context.physical_material_ids, context.num_physical, pair_sequence, fallback);

    if (component_a == 0 || component_b == 0 || component_a > context.num_physical ||
        component_b > context.num_physical || component_a > context.physical_colors.size() ||
        component_b > context.physical_colors.size()) {
        return fallback;
    }

    const int mix_b = std::clamp(pair.component_b_percent, 0, 100);
    return MixedFilamentManager::blend_color(context.physical_colors[component_a - 1], context.physical_colors[component_b - 1],
                                             100 - mix_b, mix_b, physical_td_for_id(context, component_a),
                                             physical_td_for_id(context, component_b), physical_material_id_for_id(context, component_a),
                                             physical_material_id_for_id(context, component_b));
}

std::string compute_mixed_filament_display_color(const MixedFilamentLegacyRow& row, const MixedFilamentDisplayContext& context)
{
    return compute_mixed_filament_display_color(mixed_filament_definition_from_legacy_row(row, context.num_physical), context);
}

std::string MixedFilamentManager::blend_color_multi(const std::vector<std::pair<std::string, int>>& color_percents)
{
    std::vector<MixedFilamentColorInput> inputs;
    inputs.reserve(color_percents.size());
    for (const auto& [hex, percent] : color_percents)
        inputs.push_back({hex, percent, std::nullopt, std::nullopt});
    return blend_color_multi(inputs);
}

std::string MixedFilamentManager::blend_color_multi(const std::vector<MixedFilamentColorInput>& color_percents)
{
    if (color_percents.empty())
        return "#000000";
    if (color_percents.size() == 1)
        return color_percents.front().color_hex;

    if (color_engine() == MixedFilamentColorEngine::FullSpectrumKSPairResidual) {
        if (const auto calibrated = full_spectrum_ks_blend_color_multi(full_spectrum_inputs_from_mixed_inputs(color_percents)))
            return *calibrated;
    }

    struct WeightedColor
    {
        RGB color;
        int pct;
    };
    std::vector<WeightedColor> colors;
    colors.reserve(color_percents.size());

    int total_pct = 0;
    for (const MixedFilamentColorInput& input : color_percents) {
        const int pct = input.percent;
        if (pct <= 0)
            continue;
        colors.push_back({parse_hex_color(input.color_hex), pct});
        total_pct += pct;
    }
    if (colors.empty() || total_pct <= 0)
        return "#000000";

    unsigned char r               = static_cast<unsigned char>(colors.front().color.r);
    unsigned char g               = static_cast<unsigned char>(colors.front().color.g);
    unsigned char b               = static_cast<unsigned char>(colors.front().color.b);
    int           accumulated_pct = colors.front().pct;

    for (size_t i = 1; i < colors.size(); ++i) {
        const auto& next      = colors[i];
        const int   new_total = accumulated_pct + next.pct;
        if (new_total <= 0)
            continue;
        const float t = static_cast<float>(next.pct) / static_cast<float>(new_total);
        filament_mixer_lerp(r, g, b, static_cast<unsigned char>(next.color.r), static_cast<unsigned char>(next.color.g),
                            static_cast<unsigned char>(next.color.b), t, &r, &g, &b);
        accumulated_pct = new_total;
    }

    return rgb_to_hex({int(r), int(g), int(b)});
}

std::string MixedFilamentManager::blend_color(const std::string& color_a, const std::string& color_b, int ratio_a, int ratio_b)
{
    return blend_color(color_a, color_b, ratio_a, ratio_b, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
}

std::string MixedFilamentManager::blend_color(const std::string&           color_a,
                                              const std::string&           color_b,
                                              int                          ratio_a,
                                              int                          ratio_b,
                                              const std::optional<double>& td_a_mm,
                                              const std::optional<double>& td_b_mm)
{
    return blend_color(color_a, color_b, ratio_a, ratio_b, td_a_mm, td_b_mm, std::nullopt, std::nullopt);
}

std::string MixedFilamentManager::blend_color(const std::string&                color_a,
                                              const std::string&                color_b,
                                              int                               ratio_a,
                                              int                               ratio_b,
                                              const std::optional<double>&      td_a_mm,
                                              const std::optional<double>&      td_b_mm,
                                              const std::optional<std::string>& material_id_a,
                                              const std::optional<std::string>& material_id_b)
{
    const std::optional<double> active_td_a = use_td_for_color_prediction() ? td_a_mm : std::nullopt;
    const std::optional<double> active_td_b = use_td_for_color_prediction() ? td_b_mm : std::nullopt;
    if (color_engine() == MixedFilamentColorEngine::FullSpectrumKSPairResidual) {
        if (const auto calibrated = full_spectrum_ks_blend_color_multi({
                {color_a, std::max(0, ratio_a), active_td_a, material_id_a},
                {color_b, std::max(0, ratio_b), active_td_b, material_id_b}
            }))
            return *calibrated;
    }

    const int   safe_a = std::max(0, ratio_a);
    const int   safe_b = std::max(0, ratio_b);
    const int   total  = safe_a + safe_b;
    const float t      = (total > 0) ? (static_cast<float>(safe_b) / static_cast<float>(total)) : 0.5f;

    const RGB rgb_a = parse_hex_color(color_a);
    const RGB rgb_b = parse_hex_color(color_b);

    unsigned char out_r = static_cast<unsigned char>(rgb_a.r);
    unsigned char out_g = static_cast<unsigned char>(rgb_a.g);
    unsigned char out_b = static_cast<unsigned char>(rgb_a.b);
    filament_mixer_lerp(static_cast<unsigned char>(rgb_a.r), static_cast<unsigned char>(rgb_a.g), static_cast<unsigned char>(rgb_a.b),
                        static_cast<unsigned char>(rgb_b.r), static_cast<unsigned char>(rgb_b.g), static_cast<unsigned char>(rgb_b.b), t,
                        &out_r, &out_g, &out_b);

    return rgb_to_hex({int(out_r), int(out_g), int(out_b)});
}

float MixedFilamentManager::max_component_surface_offset_mm(float reference_width_mm)
{
    const float safe_reference = std::max(0.05f, std::abs(reference_width_mm));
    return std::clamp(safe_reference, 0.01f, 0.35f);
}

float MixedFilamentManager::max_pair_bias_mm(float reference_width_mm) { return max_component_surface_offset_mm(reference_width_mm); }

std::pair<float, float> MixedFilamentManager::surface_offset_pair_from_signed_bias(float bias_mm, float reference_width_mm)
{
    const float clamped_bias = std::clamp(bias_mm, -max_pair_bias_mm(reference_width_mm), max_pair_bias_mm(reference_width_mm));
    if (clamped_bias > EPSILON)
        return std::make_pair(0.f, clamped_bias);
    if (clamped_bias < -EPSILON)
        return std::make_pair(-clamped_bias, 0.f);
    return std::make_pair(0.f, 0.f);
}

float MixedFilamentManager::bias_ui_value_from_surface_offsets(float component_a_surface_offset,
                                                               float component_b_surface_offset,
                                                               float reference_width_mm)
{
    return std::clamp(canonical_signed_bias_value(component_a_surface_offset, component_b_surface_offset),
                      -max_pair_bias_mm(reference_width_mm), max_pair_bias_mm(reference_width_mm));
}

int MixedFilamentManager::apparent_mix_b_percent(int   mix_b_percent,
                                                 float component_a_surface_offset,
                                                 float component_b_surface_offset,
                                                 float reference_width_mm)
{
    const float safe_reference = std::max(0.05f, std::abs(reference_width_mm));
    const float shift_pct      = -100.f *
                            std::clamp(canonical_signed_bias_value(component_a_surface_offset, component_b_surface_offset),
                                       -max_pair_bias_mm(reference_width_mm), max_pair_bias_mm(reference_width_mm)) /
                            safe_reference;
    return clamp_int(int(std::lround(float(clamp_int(mix_b_percent, 0, 100)) + shift_pct)), 0, 100);
}

std::vector<int> MixedFilamentManager::apparent_component_percentages(const std::vector<int> &component_percents,
                                                                       float component_a_surface_offset,
                                                                       float component_b_surface_offset,
                                                                       float reference_width_mm)
{
    std::vector<float> component_offsets(component_percents.size(), component_b_surface_offset);
    if (!component_offsets.empty())
        component_offsets.front() = component_a_surface_offset;
    return apparent_component_percentages(component_percents, component_offsets, reference_width_mm);
}

std::vector<int> MixedFilamentManager::apparent_component_percentages(const std::vector<int>   &component_percents,
                                                                       const std::vector<float> &component_surface_offsets,
                                                                       float                    reference_width_mm)
{
    const std::vector<int> normalized = normalize_weight_vector_to_percent(component_percents);
    if (normalized.size() < 2)
        return normalized;
    if (component_surface_offsets.size() != normalized.size())
        return normalized;

    const double safe_reference = std::max(0.05, double(std::abs(reference_width_mm)));
    const double component_scale = double(normalized.size()) / double(normalized.size() - 1);
    std::vector<double> adjustments(normalized.size(), 0.0);
    double mean_adjustment = 0.0;
    for (size_t component_idx = 0; component_idx < normalized.size(); ++component_idx) {
        adjustments[component_idx] = -100.0 * double(component_surface_offsets[component_idx]) / safe_reference;
        mean_adjustment += adjustments[component_idx];
    }
    mean_adjustment /= double(normalized.size());

    std::vector<double> apparent_exact(normalized.size(), 0.0);
    double apparent_sum = 0.0;
    for (size_t component_idx = 0; component_idx < normalized.size(); ++component_idx) {
        apparent_exact[component_idx] = std::max(0.0,
            double(normalized[component_idx]) + (adjustments[component_idx] - mean_adjustment) * component_scale);
        apparent_sum += apparent_exact[component_idx];
    }
    if (apparent_sum <= EPSILON)
        return normalized;

    std::vector<int> apparent(normalized.size(), 0);
    std::vector<double> remainders(normalized.size(), 0.0);
    int assigned = 0;
    for (size_t component_idx = 0; component_idx < normalized.size(); ++component_idx) {
        const double exact_percent = 100.0 * apparent_exact[component_idx] / apparent_sum;
        apparent[component_idx] = int(std::floor(exact_percent));
        remainders[component_idx] = exact_percent - double(apparent[component_idx]);
        assigned += apparent[component_idx];
    }

    for (int missing = 100 - assigned; missing > 0; --missing) {
        size_t best_idx = 0;
        for (size_t component_idx = 1; component_idx < remainders.size(); ++component_idx)
            if (remainders[component_idx] > remainders[best_idx])
                best_idx = component_idx;
        ++apparent[best_idx];
        remainders[best_idx] = -1.0;
    }
    return apparent;
}

} // namespace Slic3r
