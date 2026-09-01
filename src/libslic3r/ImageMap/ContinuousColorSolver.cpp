// Portions adapted from OrcaSlicer-ImageMap ColorSolver.
// Copyright (C) 2026 sentientstardust
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ContinuousColorSolver.hpp"
#include "SimplePmCalibration.hpp"

#include "../FullSpectrumKSPairResidual.hpp"
#include "../MixedFilament.hpp"
#include "../filament_mixer_model.h"

#include <algorithm>
#include <array>
#include <atomic>
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
// 92548381056dbf72836b0a1bdc455f238218dbfb (sentientstardust, 2026).
constexpr float  OKLAB_MIN_L_WEIGHT             = 1.f;
constexpr float  OKLAB_MAX_AB_WEIGHT            = 4.f;
constexpr float  DARK_PENALTY                   = 4.f;
constexpr float  DARK_TOLERANCE                 = 0.04f;
constexpr size_t MAX_CANDIDATES                 = 250000;

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

std::array<float, 3> mix_filament_mixer(const std::vector<std::array<float, 3>>& colors, const std::vector<int>& weights)
{
    size_t first = 0;
    while (first < weights.size() && weights[first] <= 0)
        ++first;
    if (first >= colors.size())
        return {0.f, 0.f, 0.f};

    auto byte = [](float value) { return static_cast<unsigned char>(std::lround(clamp01(value) * 255.f)); };
    unsigned char r = byte(colors[first][0]);
    unsigned char g = byte(colors[first][1]);
    unsigned char b = byte(colors[first][2]);
    int accumulated = weights[first];
    for (size_t index = first + 1; index < colors.size() && index < weights.size(); ++index) {
        if (weights[index] <= 0)
            continue;
        const int total = accumulated + weights[index];
        filament_mixer::lerp(r, g, b, byte(colors[index][0]), byte(colors[index][1]), byte(colors[index][2]),
                             float(weights[index]) / float(total), &r, &g, &b);
        accumulated = total;
    }
    return {float(r) / 255.f, float(g) / 255.f, float(b) / 255.f};
}

std::array<float, 3> predict_candidate_color(const std::vector<ContinuousColorComponent>& components,
                                             const std::vector<std::array<float, 3>>&      component_colors,
                                             const std::vector<int>&                       units,
                                             ColorMixModel                                color_mix_model)
{
    std::vector<std::array<float, 3>> active_colors;
    std::vector<int>                  active_units;
    std::vector<FullSpectrumKSPairResidualColorInput> ks_inputs;
    active_colors.reserve(components.size());
    active_units.reserve(components.size());
    ks_inputs.reserve(components.size());
    for (size_t index = 0; index < components.size() && index < units.size(); ++index) {
        if (units[index] <= 0)
            continue;
        active_colors.emplace_back(component_colors[index]);
        active_units.emplace_back(units[index]);
        ks_inputs.push_back({components[index].color_hex, units[index], components[index].transmission_distance_mm,
                             components[index].material_id});
    }
    if (active_colors.empty())
        return {0.f, 0.f, 0.f};
    if (active_colors.size() == 1)
        return active_colors.front();

    switch (color_mix_model) {
    case ColorMixModel::FilamentMixer:
        return mix_filament_mixer(active_colors, active_units);
    case ColorMixModel::FullSpectrumKmKs:
    default:
        if (const std::optional<std::string> predicted = full_spectrum_ks_blend_color_multi(ks_inputs))
            return decode_rgb(*predicted);
        // Keep malformed legacy colours from invalidating the candidate
        // table without consulting the application's global display engine.
        return mix_filament_mixer(active_colors, active_units);
    }
}

} // namespace

struct ContinuousColorSolver::Impl
{
    static constexpr size_t QUANTIZED_LOOKUP_SIZE = 32u * 32u * 32u;

    std::vector<ContinuousColorComponent> components;
    std::vector<float>                    predicted_colors;
    std::vector<float>                    perceptual_coordinates;
    std::vector<float>                    weights;
    std::vector<KdNode>                   kd_nodes;
    ColorMixModel                         color_mix_model{ColorMixModel::FullSpectrumKmKs};
    bool                                  apply_saved_calibration{true};
    int                                   kd_root{-1};
    // Zero means unresolved; candidate indices are stored one-based.
    std::unique_ptr<std::atomic<uint32_t>[]> quantized_candidate_indices;

    size_t nearest_candidate(const RGBA& target_color) const
    {
        const std::array<float, 3> target       = oklab_from_srgb({target_color[0], target_color[1], target_color[2]});
        const std::array<float, 3> axis_weights = perceptual_axis_weights(target);
        size_t                     best_index   = size_t(-1);
        float                      best_error   = std::numeric_limits<float>::max();
        query(kd_root, target, axis_weights, best_index, best_error);
        return best_index;
    }

    size_t nearest_quantized_candidate(const RGBA& target_color) const
    {
        auto quantize = [](float channel) {
            return uint32_t(std::lround(std::clamp(channel, 0.f, 1.f) * 31.f));
        };
        const uint32_t red   = quantize(target_color[0]);
        const uint32_t green = quantize(target_color[1]);
        const uint32_t blue  = quantize(target_color[2]);
        const uint32_t key   = (red << 10) | (green << 5) | blue;
        if (!quantized_candidate_indices)
            return nearest_candidate(target_color);

        const uint32_t cached = quantized_candidate_indices[key].load(std::memory_order_acquire);
        if (cached != 0)
            return size_t(cached - 1);

        const RGBA quantized_target{float(red) / 31.f, float(green) / 31.f, float(blue) / 31.f, target_color[3]};
        const size_t candidate_index = nearest_candidate(quantized_target);
        if (candidate_index >= perceptual_coordinates.size() / 3 || candidate_index >= uint32_t(-1) - 1u)
            return candidate_index;

        const uint32_t encoded = uint32_t(candidate_index) + 1u;
        uint32_t       expected = 0;
        if (!quantized_candidate_indices[key].compare_exchange_strong(expected, encoded, std::memory_order_release,
                                                                       std::memory_order_acquire))
            return size_t(expected - 1u);
        return candidate_index;
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

ContinuousColorSolver::ContinuousColorSolver(std::vector<ContinuousColorComponent> components,
                                             ColorMixModel color_mix_model,
                                             bool apply_saved_calibration)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->components      = std::move(components);
    m_impl->color_mix_model = color_mix_model;
    m_impl->apply_saved_calibration = apply_saved_calibration;
    if (m_impl->components.size() < 2)
        return;

    const int    total_units = continuous_color_solver_total_units(m_impl->components.size());
    const size_t expected    = continuous_color_solver_candidate_count(m_impl->components.size(), total_units);
    if (expected == 0 || expected > MAX_CANDIDATES)
        return;
    m_impl->perceptual_coordinates.reserve(expected * 3);
    m_impl->predicted_colors.reserve(expected * 3);
    m_impl->weights.reserve(expected * m_impl->components.size());

    std::vector<std::array<float, 3>> component_colors;
    component_colors.reserve(m_impl->components.size());
    for (const ContinuousColorComponent& component : m_impl->components)
        component_colors.emplace_back(decode_rgb(component.color_hex));

    std::vector<int>                 units(m_impl->components.size(), 0);
    std::function<void(size_t, int)> enumerate = [&](size_t component_index, int remaining_units) {
        if (component_index + 1 == units.size()) {
            units[component_index] = remaining_units;
            const std::array<float, 3> raw_mixed =
                predict_candidate_color(m_impl->components, component_colors, units, m_impl->color_mix_model);
            std::vector<double> normalized_weights;
            normalized_weights.reserve(units.size());
            for (const int unit : units)
                normalized_weights.push_back(double(unit) / double(total_units));
            const RGBA calibrated = m_impl->apply_saved_calibration ?
                                        apply_simple_pm_calibration(m_impl->components, m_impl->color_mix_model,
                                                                    normalized_weights,
                                                                    RGBA{raw_mixed[0], raw_mixed[1], raw_mixed[2], 1.f}) :
                                        RGBA{raw_mixed[0], raw_mixed[1], raw_mixed[2], 1.f};
            const std::array<float, 3> mixed = {calibrated[0], calibrated[1], calibrated[2]};
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
    if (m_impl->kd_root >= 0) {
        m_impl->quantized_candidate_indices = std::make_unique<std::atomic<uint32_t>[]>(Impl::QUANTIZED_LOOKUP_SIZE);
        for (size_t index = 0; index < Impl::QUANTIZED_LOOKUP_SIZE; ++index)
            m_impl->quantized_candidate_indices[index].store(0, std::memory_order_relaxed);
    }
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

std::optional<ContinuousColorCandidate> ContinuousColorSolver::candidate(size_t candidate_index) const
{
    if (!valid() || candidate_index >= candidate_count())
        return std::nullopt;

    ContinuousColorCandidate candidate;
    candidate.weights.resize(component_count());
    const size_t weight_base = candidate_index * component_count();
    for (size_t component_index = 0; component_index < component_count(); ++component_index)
        candidate.weights[component_index] = m_impl->weights[weight_base + component_index];
    const size_t color_base = candidate_index * 3;
    candidate.predicted_color = RGBA{m_impl->predicted_colors[color_base], m_impl->predicted_colors[color_base + 1],
                                     m_impl->predicted_colors[color_base + 2], 1.f};
    return candidate;
}

std::vector<double> compact_modulation_weights(std::vector<double> weights)
{
    double maximum = 0.0;
    for (const double weight : weights)
        if (std::isfinite(weight))
            maximum = std::max(maximum, weight);
    if (maximum <= std::numeric_limits<double>::epsilon())
        return weights;

    for (double& weight : weights)
        weight = std::isfinite(weight) ? std::clamp(weight / maximum, 0.0, 1.0) : 0.0;
    return weights;
}

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

std::vector<double> ContinuousColorSolver::solve_quantized_5bit(const RGBA& target_color) const
{
    if (!valid())
        return {};
    const size_t best_index = m_impl->nearest_quantized_candidate(target_color);
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
    return compact_modulation_weights(solve(target_color));
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
    return predict_color(target_color);
}

std::optional<RGBA> ContinuousColorSolver::predict_weights(const std::vector<double>& weights) const
{
    if (!valid() || weights.size() != component_count())
        return std::nullopt;

    const int total_units = continuous_color_solver_total_units(component_count());
    double    sum         = 0.;
    for (double weight : weights)
        if (std::isfinite(weight) && weight > 0.)
            sum += weight;
    if (sum <= std::numeric_limits<double>::epsilon())
        return std::nullopt;

    std::vector<int>                       units(weights.size(), 0);
    std::vector<std::pair<double, size_t>> remainders;
    remainders.reserve(weights.size());
    int assigned = 0;
    for (size_t index = 0; index < weights.size(); ++index) {
        const double exact = double(total_units) * std::max(0., weights[index]) / sum;
        units[index]       = int(std::floor(exact));
        assigned += units[index];
        remainders.emplace_back(exact - double(units[index]), index);
    }
    std::stable_sort(remainders.begin(), remainders.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first != rhs.first ? lhs.first > rhs.first : lhs.second < rhs.second;
    });
    for (int remaining = total_units - assigned; remaining > 0; --remaining)
        ++units[remainders[size_t(total_units - assigned - remaining) % remainders.size()].second];

    std::vector<std::array<float, 3>> component_colors;
    component_colors.reserve(m_impl->components.size());
    for (const ContinuousColorComponent& component : m_impl->components)
        component_colors.emplace_back(decode_rgb(component.color_hex));
    const std::array<float, 3> predicted =
        predict_candidate_color(m_impl->components, component_colors, units, m_impl->color_mix_model);
    std::vector<double> normalized_weights(units.size(), 0.0);
    for (size_t index = 0; index < units.size(); ++index)
        normalized_weights[index] = double(units[index]) / double(total_units);
    const RGBA raw{predicted[0], predicted[1], predicted[2], 1.f};
    return m_impl->apply_saved_calibration ?
               apply_simple_pm_calibration(m_impl->components, m_impl->color_mix_model, normalized_weights, raw) :
               raw;
}

namespace {

float recipe_perceptual_error(const RGBA& target_color, const RGBA& predicted_color)
{
    const std::array<float, 3> target       = oklab_from_srgb({target_color[0], target_color[1], target_color[2]});
    const std::array<float, 3> predicted    = oklab_from_srgb({predicted_color[0], predicted_color[1], predicted_color[2]});
    const std::array<float, 3> axis_weights = perceptual_axis_weights(target);
    float                      error        = 0.f;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float delta = target[axis] - predicted[axis];
        error += axis_weights[axis] * delta * delta;
    }
    const float under_l = std::max(0.f, target[0] - predicted[0] - DARK_TOLERANCE);
    return error + DARK_PENALTY * chroma_factor(target) * under_l * under_l;
}

std::vector<size_t> build_recipe_layer_sequence(const std::vector<size_t>& component_indices)
{
    // Adaptive ownership is an equal cadence. The continuous solver weights
    // describe how strongly each selected bead is exposed by path modulation;
    // they must never be converted into repeated material layers (for example
    // C,C,C,M). Every selected physical filament owns exactly one layer in a
    // cycle, giving only 1:1, 1:1:1, or 1:1:1:1 multi-filament recipes.
    return component_indices;
}

std::vector<int> recipe_percents(const std::vector<double>& weights)
{
    if (weights.empty())
        return {};
    const double sum = std::accumulate(weights.begin(), weights.end(), 0.0);
    if (!(sum > 1e-12))
        return {};

    std::vector<int> percents(weights.size(), 0);
    std::vector<std::pair<double, size_t>> remainders;
    remainders.reserve(weights.size());
    int assigned = 0;
    for (size_t index = 0; index < weights.size(); ++index) {
        const double exact = 100.0 * std::max(0.0, weights[index]) / sum;
        percents[index]    = int(std::floor(exact));
        assigned += percents[index];
        remainders.emplace_back(exact - double(percents[index]), index);
    }
    std::stable_sort(remainders.begin(), remainders.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first != rhs.first ? lhs.first > rhs.first : lhs.second < rhs.second;
    });
    for (int remaining = 100 - assigned; remaining > 0; --remaining)
        ++percents[remainders[size_t(100 - assigned - remaining) % remainders.size()].second];
    return percents;
}

} // namespace

struct ContinuousColorRecipeSolver::Impl
{
    struct Subset
    {
        std::vector<size_t>                       component_indices;
        std::optional<RGBA>                       single_color;
        std::unique_ptr<ContinuousColorSolver> solver;
    };

    std::vector<ContinuousColorComponent> components;
    std::vector<Subset>                   subsets;
};

bool ContinuousColorRecipe::valid() const
{
    return !component_indices.empty() && component_indices.size() == component_percents.size() && !layer_sequence.empty();
}

std::optional<size_t> ContinuousColorRecipe::component_index_for_layer(size_t layer_index) const
{
    return valid() ? std::optional<size_t>(layer_sequence[layer_index % layer_sequence.size()]) : std::nullopt;
}

ContinuousColorRecipeSolver::ContinuousColorRecipeSolver(std::vector<ContinuousColorComponent> components,
                                                         size_t max_components,
                                                         ColorMixModel color_mix_model)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->components = std::move(components);
    max_components     = std::min({max_components, m_impl->components.size(), continuous_color_solver_max_component_count()});
    for (size_t subset_size = 1; subset_size <= max_components; ++subset_size) {
        std::vector<size_t> selection;
        std::function<void(size_t)> enumerate = [&](size_t begin) {
            if (selection.size() == subset_size) {
                Impl::Subset subset;
                subset.component_indices = selection;
                if (subset_size == 1) {
                    const std::array<float, 3> rgb = decode_rgb(m_impl->components[selection.front()].color_hex);
                    subset.single_color           = RGBA{rgb[0], rgb[1], rgb[2], 1.f};
                } else {
                    std::vector<ContinuousColorComponent> selected_components;
                    selected_components.reserve(selection.size());
                    for (const size_t index : selection)
                        selected_components.emplace_back(m_impl->components[index]);
                    subset.solver = std::make_unique<ContinuousColorSolver>(std::move(selected_components), color_mix_model);
                    if (!subset.solver->valid())
                        return;
                }
                m_impl->subsets.emplace_back(std::move(subset));
                return;
            }
            const size_t needed = subset_size - selection.size();
            for (size_t index = begin; index + needed <= m_impl->components.size(); ++index) {
                selection.emplace_back(index);
                enumerate(index + 1);
                selection.pop_back();
            }
        };
        enumerate(0);
    }
}

ContinuousColorRecipeSolver::~ContinuousColorRecipeSolver()                                              = default;
ContinuousColorRecipeSolver::ContinuousColorRecipeSolver(ContinuousColorRecipeSolver&&) noexcept          = default;
ContinuousColorRecipeSolver& ContinuousColorRecipeSolver::operator=(ContinuousColorRecipeSolver&&) noexcept = default;

bool ContinuousColorRecipeSolver::valid() const { return m_impl && !m_impl->subsets.empty(); }

ContinuousColorRecipe ContinuousColorRecipeSolver::solve(const RGBA& target_color, int minimum_component_percent) const
{
    ContinuousColorRecipe result;
    if (!valid())
        return result;

    minimum_component_percent = std::clamp(minimum_component_percent, 0, 49);
    struct Candidate
    {
        const Impl::Subset* subset{nullptr};
        std::vector<double> weights;
        RGBA                predicted{1.f, 1.f, 1.f, 1.f};
        float               error{std::numeric_limits<float>::infinity()};
    };
    std::vector<std::optional<Candidate>> best_by_size(m_impl->components.size() + 1);
    float best_error = std::numeric_limits<float>::infinity();
    for (const Impl::Subset& subset : m_impl->subsets) {
        Candidate candidate;
        candidate.subset = &subset;
        if (subset.single_color) {
            candidate.weights   = {1.0};
            candidate.predicted = *subset.single_color;
        } else {
            candidate.weights = subset.solver->solve(target_color);
            const std::optional<RGBA> predicted = subset.solver->predict_color(target_color);
            if (candidate.weights.size() != subset.component_indices.size() || !predicted)
                continue;
            candidate.predicted = *predicted;
        }
        if (candidate.weights.size() > 1 &&
            std::any_of(candidate.weights.begin(), candidate.weights.end(), [minimum_component_percent](double weight) {
                return weight * 100.0 + 1e-6 < double(minimum_component_percent);
            }))
            continue;
        candidate.error = recipe_perceptual_error(target_color, candidate.predicted);
        best_error      = std::min(best_error, candidate.error);
        const size_t size = subset.component_indices.size();
        if (size >= best_by_size.size())
            continue;
        if (!best_by_size[size] || candidate.error < best_by_size[size]->error)
            best_by_size[size] = std::move(candidate);
    }
    if (!std::isfinite(best_error))
        return result;

    // Roughly 3.5 Oklab units. This accepts a smaller recipe only when its
    // visual result is effectively indistinguishable from the best pair or
    // ternary solution available to this physical palette.
    constexpr float perceptual_slack = 0.00125f;
    const Candidate* selected = nullptr;
    for (size_t size = 1; size < best_by_size.size(); ++size) {
        if (best_by_size[size] && best_by_size[size]->error <= best_error + perceptual_slack) {
            selected = &*best_by_size[size];
            break;
        }
    }
    if (selected == nullptr) {
        for (size_t size = 1; size < best_by_size.size(); ++size)
            if (best_by_size[size] && (selected == nullptr || best_by_size[size]->error < selected->error))
                selected = &*best_by_size[size];
    }
    if (selected == nullptr)
        return result;

    result.component_indices  = selected->subset->component_indices;
    result.component_percents = recipe_percents(selected->weights);
    result.layer_sequence     = build_recipe_layer_sequence(result.component_indices);
    result.predicted_color    = selected->predicted;
    result.perceptual_error   = selected->error;
    return result;
}

std::vector<size_t> select_continuous_color_components(const std::vector<ContinuousColorComponent>& components,
                                                       const std::vector<RGBA>&                     representative_colors,
                                                       size_t                                       requested_count,
                                                       ColorMixModel                                color_mix_model)
{
    if (components.empty())
        return {};
    if (components.size() == 1)
        return {0};

    const size_t max_supported = std::min(components.size(), continuous_color_solver_max_component_count());
    const bool automatic = requested_count == 0;
    requested_count      = automatic ? max_supported : std::clamp(requested_count, size_t(2), max_supported);

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

    // A shared ImageMap cadence must retain neutral/black/white components
    // even when their maximum fitted weight falls below the color-match
    // threshold. Removing one changes the printable gamut: a neutral texture
    // background then turns into visible CMY/RGB layer stripes. Explicit
    // component counts still rank the palette by source relevance below.
    if (automatic) {
        std::sort(pool.begin(), pool.end());
        return pool;
    }

    std::vector<ContinuousColorComponent> solver_components;
    solver_components.reserve(pool.size());
    for (const size_t component_index : pool)
        solver_components.emplace_back(components[component_index]);

    ContinuousColorSolver solver(std::move(solver_components), color_mix_model);
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

    std::vector<size_t> selected;
    selected.reserve(requested_count);
    for (size_t rank = 0; rank < requested_count; ++rank)
        selected.emplace_back(pool[ranked[rank]]);
    std::sort(selected.begin(), selected.end());
    return selected;
}

} // namespace Slic3r::ImageMap
