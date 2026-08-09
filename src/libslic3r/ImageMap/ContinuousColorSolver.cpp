// Portions adapted from OrcaSlicer-ImageMap ColorSolver.
// Copyright (C) 2026 sentientstardust
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ContinuousColorSolver.hpp"

#include "../FullSpectrumKSPairResidual.hpp"
#include "../MixedFilament.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <utility>

namespace Slic3r::ImageMap {
namespace {

// Candidate enumeration, Oklab weighting and KD lookup are adapted from
// OrcaSlicer-ImageMap's AGPLv3 ColorSolver at commit
// 1ff08f86146450141cf18af14af884ebcaa68092 (sentientstardust, 2026).
constexpr float  OKLAB_MIN_L_WEIGHT             = 1.f;
constexpr float  OKLAB_MAX_AB_WEIGHT            = 4.f;
constexpr float  DARK_PENALTY                   = 4.f;
constexpr float  DARK_TOLERANCE                 = 0.04f;
constexpr size_t MAX_CANDIDATES                 = 250000;
constexpr int    MODULATION_LUT_CELLS           = 8;
constexpr float  MODULATION_MIN_VARIANCE        = 0.0004f;
constexpr float  MODULATION_OUT_OF_GAMUT_FACTOR = 0.4f;

float clamp01(float value) { return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f; }

float srgb_to_linear(float value)
{
    const float x = clamp01(value);
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

std::array<float, 3> oklab_from_srgb(const std::array<float, 3>& rgb)
{
    const float r = srgb_to_linear(rgb[0]);
    const float g = srgb_to_linear(rgb[1]);
    const float b = srgb_to_linear(rgb[2]);

    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);

    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s, 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

std::array<float, 3> perceptual_axis_weights(const std::array<float, 3>& target)
{
    const float chroma        = std::hypot(target[1], target[2]);
    const float chroma_factor = std::clamp((chroma - 0.015f) / 0.13f, 0.f, 1.f);
    return {std::max(OKLAB_MIN_L_WEIGHT, 1.f + (0.25f - 1.f) * chroma_factor),
            std::min(OKLAB_MAX_AB_WEIGHT, 1.25f + (8.f - 1.25f) * chroma_factor),
            std::min(OKLAB_MAX_AB_WEIGHT, 1.25f + (8.f - 1.25f) * chroma_factor)};
}

float chroma_factor(const std::array<float, 3>& target)
{
    return std::clamp((std::hypot(target[1], target[2]) - 0.015f) / 0.13f, 0.f, 1.f);
}

struct KdNode
{
    uint32_t candidate_index{0};
    int      left{-1};
    int      right{-1};
    uint8_t  axis{0};
};

int build_kd_tree(const std::vector<float>& coordinates,
                  std::vector<KdNode>&      nodes,
                  std::vector<uint32_t>&    indices,
                  size_t                    begin,
                  size_t                    end,
                  uint8_t                   axis)
{
    if (begin >= end)
        return -1;

    const size_t mid = begin + (end - begin) / 2;
    std::nth_element(indices.begin() + begin, indices.begin() + mid, indices.begin() + end,
                     [&coordinates, axis](uint32_t lhs, uint32_t rhs) {
                         return coordinates[size_t(lhs) * 3 + axis] < coordinates[size_t(rhs) * 3 + axis];
                     });

    const int node_index = int(nodes.size());
    nodes.push_back({indices[mid], -1, -1, axis});
    const uint8_t next_axis         = uint8_t((axis + 1) % 3);
    const int     left              = build_kd_tree(coordinates, nodes, indices, begin, mid, next_axis);
    const int     right             = build_kd_tree(coordinates, nodes, indices, mid + 1, end, next_axis);
    nodes[size_t(node_index)].left  = left;
    nodes[size_t(node_index)].right = right;
    return node_index;
}

int build_kd_tree(const std::vector<float>& coordinates, std::vector<KdNode>& nodes)
{
    const size_t          count = coordinates.size() / 3;
    std::vector<uint32_t> indices(count);
    std::iota(indices.begin(), indices.end(), uint32_t(0));
    nodes.clear();
    nodes.reserve(count);
    return build_kd_tree(coordinates, nodes, indices, 0, count, 0);
}

std::array<float, 3> decode_rgb(const std::string& color_hex)
{
    ColorRGB color;
    if (!decode_color(color_hex, color))
        return {0.f, 0.f, 0.f};
    return {color.r(), color.g(), color.b()};
}

} // namespace

struct ContinuousColorSolver::Impl
{
    std::vector<ContinuousColorComponent> components;
    std::vector<float>                    predicted_colors;
    std::vector<float>                    perceptual_coordinates;
    std::vector<float>                    weights;
    std::vector<float>                    modulation_lut_weights;
    std::vector<float>                    modulation_lut_colors;
    std::vector<KdNode>                   kd_nodes;
    int                                   kd_root{-1};

    size_t nearest_candidate(const RGBA& target_color) const
    {
        const std::array<float, 3> target       = oklab_from_srgb({target_color[0], target_color[1], target_color[2]});
        const std::array<float, 3> axis_weights = perceptual_axis_weights(target);
        size_t                     best_index   = size_t(-1);
        float                      best_error   = std::numeric_limits<float>::max();
        query(kd_root, target, axis_weights, best_index, best_error);
        return best_index;
    }

    void append_smooth_projection(const RGBA& target_color)
    {
        const std::array<float, 3> target       = oklab_from_srgb({target_color[0], target_color[1], target_color[2]});
        const std::array<float, 3> axis_weights = perceptual_axis_weights(target);
        size_t                     best_index   = size_t(-1);
        float                      best_error   = std::numeric_limits<float>::max();
        query(kd_root, target, axis_weights, best_index, best_error);
        if (best_index >= perceptual_coordinates.size() / 3)
            return;

        const float variance = std::max(1e-7f, MODULATION_MIN_VARIANCE + std::max(0.f, best_error) * MODULATION_OUT_OF_GAMUT_FACTOR);
        std::vector<double>   projected_weights(components.size(), 0.0);
        std::array<double, 3> projected_color{0.0, 0.0, 0.0};
        double                total = 0.0;
        for (size_t candidate_index = 0; candidate_index < perceptual_coordinates.size() / 3; ++candidate_index) {
            const float exponent = -(candidate_error(candidate_index, target, axis_weights) - best_error) / variance;
            if (exponent < -16.f)
                continue;
            const double contribution = std::exp(double(exponent));
            const size_t weight_base  = candidate_index * components.size();
            for (size_t component_index = 0; component_index < components.size(); ++component_index)
                projected_weights[component_index] += contribution * double(weights[weight_base + component_index]);
            const size_t color_base = candidate_index * 3;
            for (size_t channel = 0; channel < 3; ++channel)
                projected_color[channel] += contribution * double(predicted_colors[color_base + channel]);
            total += contribution;
        }
        if (total <= std::numeric_limits<double>::epsilon())
            return;
        for (const double weight : projected_weights)
            modulation_lut_weights.emplace_back(float(weight / total));
        for (const double channel : projected_color)
            modulation_lut_colors.emplace_back(float(channel / total));
    }

    void build_modulation_lut()
    {
        const size_t side        = size_t(MODULATION_LUT_CELLS + 1);
        const size_t point_count = side * side * side;
        modulation_lut_weights.clear();
        modulation_lut_colors.clear();
        modulation_lut_weights.reserve(point_count * components.size());
        modulation_lut_colors.reserve(point_count * 3);
        for (int red = 0; red <= MODULATION_LUT_CELLS; ++red)
            for (int green = 0; green <= MODULATION_LUT_CELLS; ++green)
                for (int blue = 0; blue <= MODULATION_LUT_CELLS; ++blue)
                    append_smooth_projection(RGBA{float(red) / float(MODULATION_LUT_CELLS), float(green) / float(MODULATION_LUT_CELLS),
                                                  float(blue) / float(MODULATION_LUT_CELLS), 1.f});
        if (modulation_lut_weights.size() != point_count * components.size() || modulation_lut_colors.size() != point_count * 3) {
            modulation_lut_weights.clear();
            modulation_lut_colors.clear();
        }
    }

    std::vector<double> interpolate_modulation_lut(const RGBA& target_color, const std::vector<float>& table, size_t value_count) const
    {
        const size_t side = size_t(MODULATION_LUT_CELLS + 1);
        if (table.size() != side * side * side * value_count)
            return {};

        std::array<int, 3>   low{};
        std::array<float, 3> fraction{};
        for (size_t channel = 0; channel < 3; ++channel) {
            const float scaled = clamp01(target_color[channel]) * float(MODULATION_LUT_CELLS);
            low[channel]       = std::min(int(std::floor(scaled)), MODULATION_LUT_CELLS - 1);
            fraction[channel]  = scaled - float(low[channel]);
        }

        std::vector<double> result(value_count, 0.0);
        for (int corner = 0; corner < 8; ++corner) {
            std::array<size_t, 3> coordinate{};
            double                coefficient = 1.0;
            for (size_t channel = 0; channel < 3; ++channel) {
                const bool high     = (corner & (1 << channel)) != 0;
                coordinate[channel] = size_t(low[channel] + (high ? 1 : 0));
                coefficient *= high ? double(fraction[channel]) : double(1.f - fraction[channel]);
            }
            const size_t base = ((coordinate[0] * side + coordinate[1]) * side + coordinate[2]) * value_count;
            for (size_t value_index = 0; value_index < value_count; ++value_index)
                result[value_index] += coefficient * double(table[base + value_index]);
        }
        return result;
    }

    float candidate_error(size_t candidate_index, const std::array<float, 3>& target, const std::array<float, 3>& axis_weights) const
    {
        const size_t coordinate_index = candidate_index * 3;
        const float  dl               = perceptual_coordinates[coordinate_index] - target[0];
        const float  da               = perceptual_coordinates[coordinate_index + 1] - target[1];
        const float  db               = perceptual_coordinates[coordinate_index + 2] - target[2];
        float        error            = axis_weights[0] * dl * dl + axis_weights[1] * da * da + axis_weights[2] * db * db;
        const float  under_l          = std::max(0.f, target[0] - perceptual_coordinates[coordinate_index] - DARK_TOLERANCE);
        error += DARK_PENALTY * chroma_factor(target) * under_l * under_l;
        return error;
    }

    void query(int                         node_index,
               const std::array<float, 3>& target,
               const std::array<float, 3>& axis_weights,
               size_t&                     best_index,
               float&                      best_error) const
    {
        if (node_index < 0 || size_t(node_index) >= kd_nodes.size())
            return;
        const KdNode& node  = kd_nodes[size_t(node_index)];
        const float   error = candidate_error(node.candidate_index, target, axis_weights);
        if (error < best_error) {
            best_error = error;
            best_index = node.candidate_index;
        }

        const size_t coordinate_index = size_t(node.candidate_index) * 3;
        const size_t axis             = std::min<size_t>(node.axis, 2);
        const float  split_delta      = target[axis] - perceptual_coordinates[coordinate_index + axis];
        const int    near_node        = split_delta <= 0.f ? node.left : node.right;
        const int    far_node         = split_delta <= 0.f ? node.right : node.left;
        query(near_node, target, axis_weights, best_index, best_error);
        if (axis_weights[axis] * split_delta * split_delta <= best_error)
            query(far_node, target, axis_weights, best_index, best_error);
    }
};

int continuous_color_solver_total_units(size_t component_count)
{
    return component_count <= 4 ? 40 : (component_count == 5 ? 24 : (component_count == 6 ? 20 : 12));
}

size_t continuous_color_solver_candidate_count(size_t component_count, int total_units)
{
    if (component_count == 0)
        return 0;
    if (total_units <= 0)
        total_units = continuous_color_solver_total_units(component_count);

    const size_t n      = size_t(total_units) + component_count - 1;
    size_t       k      = std::min(component_count - 1, n - (component_count - 1));
    size_t       result = 1;
    for (size_t index = 1; index <= k; ++index) {
        if (result > std::numeric_limits<size_t>::max() / (n - k + index))
            return 0;
        result = (result * (n - k + index)) / index;
    }
    return result;
}

size_t continuous_color_solver_max_component_count()
{
    size_t component_count = 2;
    for (;;) {
        const size_t next_count = continuous_color_solver_candidate_count(component_count + 1);
        if (next_count == 0 || next_count > MAX_CANDIDATES)
            break;
        ++component_count;
    }
    return component_count;
}

ContinuousColorSolver::ContinuousColorSolver(std::vector<ContinuousColorComponent> components, bool prepare_modulation)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->components = std::move(components);
    if (m_impl->components.size() < 2)
        return;

    const int    total_units = continuous_color_solver_total_units(m_impl->components.size());
    const size_t expected    = continuous_color_solver_candidate_count(m_impl->components.size(), total_units);
    if (expected == 0 || expected > MAX_CANDIDATES)
        return;
    m_impl->perceptual_coordinates.reserve(expected * 3);
    m_impl->predicted_colors.reserve(expected * 3);
    m_impl->weights.reserve(expected * m_impl->components.size());

    std::vector<int>                 units(m_impl->components.size(), 0);
    std::function<void(size_t, int)> enumerate = [&](size_t component_index, int remaining_units) {
        if (component_index + 1 == units.size()) {
            units[component_index] = remaining_units;
            std::vector<FullSpectrumKSPairResidualColorInput> ks_inputs;
            std::vector<MixedFilamentColorInput>              fallback_inputs;
            ks_inputs.reserve(units.size());
            fallback_inputs.reserve(units.size());
            for (size_t index = 0; index < units.size(); ++index) {
                if (units[index] <= 0)
                    continue;
                const ContinuousColorComponent& component = m_impl->components[index];
                ks_inputs.push_back({component.color_hex, units[index], component.transmission_distance_mm, component.material_id});
                fallback_inputs.push_back({component.color_hex, units[index], component.transmission_distance_mm, component.material_id});
            }
            std::string mixed_hex;
            if (ks_inputs.size() == 1) {
                mixed_hex = ks_inputs.front().color_hex;
            } else if (const std::optional<std::string> predicted = full_spectrum_ks_blend_color_multi(ks_inputs)) {
                mixed_hex = *predicted;
            } else {
                // Retain a usable candidate table for malformed legacy colors.
                // Valid physical colors always take the KM/K-S path above.
                mixed_hex = MixedFilamentManager::blend_color_multi(fallback_inputs);
            }
            const std::array<float, 3> mixed      = decode_rgb(mixed_hex);
            const std::array<float, 3> perceptual = oklab_from_srgb(mixed);
            m_impl->predicted_colors.insert(m_impl->predicted_colors.end(), mixed.begin(), mixed.end());
            m_impl->perceptual_coordinates.insert(m_impl->perceptual_coordinates.end(), perceptual.begin(), perceptual.end());
            for (const int unit : units)
                m_impl->weights.push_back(float(unit) / float(total_units));
            return;
        }
        for (int unit = 0; unit <= remaining_units; ++unit) {
            units[component_index] = unit;
            enumerate(component_index + 1, remaining_units - unit);
        }
    };
    enumerate(0, total_units);
    m_impl->kd_root = build_kd_tree(m_impl->perceptual_coordinates, m_impl->kd_nodes);
    if (prepare_modulation)
        m_impl->build_modulation_lut();
}

ContinuousColorSolver::~ContinuousColorSolver()                                           = default;
ContinuousColorSolver::ContinuousColorSolver(ContinuousColorSolver&&) noexcept            = default;
ContinuousColorSolver& ContinuousColorSolver::operator=(ContinuousColorSolver&&) noexcept = default;

bool ContinuousColorSolver::valid() const
{
    return m_impl && m_impl->components.size() >= 2 && m_impl->kd_root >= 0 && m_impl->predicted_colors.size() == candidate_count() * 3 &&
           m_impl->weights.size() == candidate_count() * component_count();
}

size_t ContinuousColorSolver::component_count() const { return m_impl ? m_impl->components.size() : 0; }

size_t ContinuousColorSolver::candidate_count() const { return m_impl ? m_impl->perceptual_coordinates.size() / 3 : 0; }

std::vector<double> ContinuousColorSolver::solve(const RGBA& target_color) const
{
    if (!valid())
        return {};
    const size_t best_index = m_impl->nearest_candidate(target_color);
    if (best_index >= candidate_count())
        return {};

    std::vector<double> result(component_count());
    const size_t        base = best_index * component_count();
    for (size_t index = 0; index < result.size(); ++index)
        result[index] = m_impl->weights[base + index];
    return result;
}

std::vector<double> ContinuousColorSolver::solve_modulation(const RGBA& target_color) const
{
    if (!valid())
        return {};
    return m_impl->interpolate_modulation_lut(target_color, m_impl->modulation_lut_weights, component_count());
}

std::optional<RGBA> ContinuousColorSolver::predict_color(const RGBA& target_color) const
{
    if (!valid())
        return std::nullopt;
    const size_t best_index = m_impl->nearest_candidate(target_color);
    if (best_index >= candidate_count())
        return std::nullopt;

    const size_t base = best_index * 3;
    return RGBA{m_impl->predicted_colors[base], m_impl->predicted_colors[base + 1], m_impl->predicted_colors[base + 2], target_color[3]};
}

std::optional<RGBA> ContinuousColorSolver::predict_modulation_color(const RGBA& target_color) const
{
    if (!valid())
        return std::nullopt;
    const std::vector<double> projected = m_impl->interpolate_modulation_lut(target_color, m_impl->modulation_lut_colors, 3);
    if (projected.size() != 3)
        return std::nullopt;
    return RGBA{float(projected[0]), float(projected[1]), float(projected[2]), target_color[3]};
}

std::vector<size_t> select_continuous_color_components(const std::vector<ContinuousColorComponent>& components,
                                                       const std::vector<RGBA>&                     representative_colors,
                                                       size_t                                       requested_count,
                                                       double                                       minimum_component_weight)
{
    if (components.empty())
        return {};
    if (components.size() == 1)
        return {0};

    const size_t max_supported = std::min(components.size(), continuous_color_solver_max_component_count());
    requested_count            = requested_count == 0 ? 0 : std::clamp(requested_count, size_t(2), max_supported);

    // Very large physical palettes cannot be enumerated directly. Keep the
    // components closest to at least one representative source color, then run
    // the same continuous solver used by the perimeter renderer on that pool.
    std::vector<size_t> pool(components.size());
    std::iota(pool.begin(), pool.end(), size_t(0));
    if (pool.size() > max_supported) {
        std::vector<std::array<float, 3>> targets;
        targets.reserve(representative_colors.size());
        for (const RGBA& color : representative_colors)
            targets.emplace_back(oklab_from_srgb({color[0], color[1], color[2]}));

        auto relevance = [&](size_t component_index) {
            const std::array<float, 3> component = oklab_from_srgb(decode_rgb(components[component_index].color_hex));
            float                      best      = std::numeric_limits<float>::max();
            for (const std::array<float, 3>& target : targets) {
                const float dl = component[0] - target[0];
                const float da = component[1] - target[1];
                const float db = component[2] - target[2];
                best           = std::min(best, dl * dl + da * da + db * db);
            }
            return best;
        };
        std::stable_sort(pool.begin(), pool.end(), [&](size_t lhs, size_t rhs) {
            const float lhs_relevance = relevance(lhs);
            const float rhs_relevance = relevance(rhs);
            return lhs_relevance != rhs_relevance ? lhs_relevance < rhs_relevance : lhs < rhs;
        });
        pool.resize(max_supported);
    }

    std::vector<ContinuousColorComponent> solver_components;
    solver_components.reserve(pool.size());
    for (const size_t component_index : pool)
        solver_components.emplace_back(components[component_index]);

    ContinuousColorSolver solver(std::move(solver_components));
    if (!solver.valid()) {
        pool.resize(requested_count == 0 ? std::min<size_t>(2, pool.size()) : std::min(requested_count, pool.size()));
        std::sort(pool.begin(), pool.end());
        return pool;
    }

    std::vector<double> maximum_weights(pool.size(), 0.0);
    std::vector<double> accumulated_weights(pool.size(), 0.0);
    for (const RGBA& target : representative_colors) {
        const std::vector<double> weights = solver.solve(target);
        for (size_t index = 0; index < weights.size() && index < pool.size(); ++index) {
            maximum_weights[index] = std::max(maximum_weights[index], weights[index]);
            accumulated_weights[index] += weights[index];
        }
    }

    std::vector<size_t> ranked(pool.size());
    std::iota(ranked.begin(), ranked.end(), size_t(0));
    std::stable_sort(ranked.begin(), ranked.end(), [&](size_t lhs, size_t rhs) {
        if (std::abs(maximum_weights[lhs] - maximum_weights[rhs]) > 1e-9)
            return maximum_weights[lhs] > maximum_weights[rhs];
        if (std::abs(accumulated_weights[lhs] - accumulated_weights[rhs]) > 1e-9)
            return accumulated_weights[lhs] > accumulated_weights[rhs];
        return pool[lhs] < pool[rhs];
    });

    size_t selected_count = requested_count;
    if (selected_count == 0) {
        const double threshold = std::clamp(minimum_component_weight, 0.01, 0.5);
        selected_count         = size_t(
            std::count_if(ranked.begin(), ranked.end(), [&](size_t index) { return maximum_weights[index] + 1e-9 >= threshold; }));
        selected_count = std::clamp(selected_count, size_t(2), pool.size());
    }

    std::vector<size_t> selected;
    selected.reserve(selected_count);
    for (size_t rank = 0; rank < selected_count; ++rank)
        selected.emplace_back(pool[ranked[rank]]);
    std::sort(selected.begin(), selected.end());
    return selected;
}

} // namespace Slic3r::ImageMap
