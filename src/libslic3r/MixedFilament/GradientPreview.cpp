#include "../MixedFilament.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace Slic3r {

std::vector<unsigned int> mixed_gradient_components(const MixedFilamentDefinition& entry, size_t num_physical)
{
    auto ids = entry.recipe.blend.component_ids(num_physical);
    if (ids.size() == 2 && entry.behavior.gradient.component_a_start < entry.behavior.gradient.component_a_end)
        std::reverse(ids.begin(), ids.end());
    return ids;
}

std::vector<float> mixed_gradient_stops(const MixedFilamentDefinition& entry, size_t num_physical)
{
    const auto   ids   = mixed_gradient_components(entry, num_physical);
    const size_t count = ids.size() >= 2 ? ids.size() * 2 - 1 : 0;
    if (count == 0)
        return {};
    auto stops = entry.behavior.gradient.stop_positions;
    if (stops.size() != count || std::any_of(stops.begin(), stops.end(), [](float x) { return !std::isfinite(x); })) {
        stops.resize(count);
        for (size_t i = 0; i < count; ++i)
            stops[i] = float(i) / float(count - 1);
        const auto   weights = entry.recipe.blend.component_percents(num_physical);
        const double total   = std::accumulate(weights.begin(), weights.end(), 0.0);
        if (ids.size() >= 3 && weights.size() == ids.size() && total > 0.0) {
            double boundary = 0.0;
            for (size_t i = 0; i + 1 < ids.size(); ++i) {
                boundary += weights[i] / total;
                stops[2 * i + 1] = float(boundary);
            }
            for (size_t i = 1; i + 1 < ids.size(); ++i)
                stops[2 * i] = .5f * (stops[2 * i - 1] + stops[2 * i + 1]);
        }
    }
    constexpr float gap = 0.0001f;
    stops.front()       = 0.f;
    stops.back()        = 1.f;
    for (size_t i = 1; i < count; ++i)
        stops[i] = std::max(std::clamp(stops[i], 0.f, 1.f), stops[i - 1] + gap);
    stops.back() = 1.f;
    for (size_t i = count - 1; i > 0; --i)
        stops[i - 1] = std::min(stops[i - 1], stops[i] - gap);
    stops.front() = 0.f;
    return stops;
}

std::vector<float> mixed_gradient_solid_half_widths(const MixedFilamentDefinition& entry, size_t num_physical, double fallback)
{
    const auto         stops = mixed_gradient_stops(entry, num_physical);
    const size_t       count = mixed_gradient_components(entry, num_physical).size();
    std::vector<float> widths(count, 0.f);
    if (!std::isfinite(fallback))
        fallback = 0.03;
    for (size_t i = 1; i + 1 < count; ++i) {
        const double requested = i < entry.behavior.gradient.solid_widths.size() && std::isfinite(entry.behavior.gradient.solid_widths[i]) ?
                                     entry.behavior.gradient.solid_widths[i] :
                                     fallback;
        widths[i] = float(std::max(0.0, std::min({std::clamp(requested, 0.0, 1.0) * 0.5, double(stops[2 * i] - stops[2 * i - 1]),
                                                  double(stops[2 * i + 1] - stops[2 * i])})));
    }
    return widths;
}

MixedGradientSample sample_mixed_gradient(const MixedFilamentDefinition& entry, size_t num_physical, double progress, double middle_window)
{
    const auto ids   = mixed_gradient_components(entry, num_physical);
    const auto stops = mixed_gradient_stops(entry, num_physical);
    if (ids.size() < 2 || stops.empty())
        return {};
    const double t = std::isfinite(progress) ? std::clamp(progress, 0.0, 1.0) : 0.0;
    if (t <= 0.0)
        return {ids.front(), ids.front(), 0};
    if (t >= 1.0)
        return {ids.back(), ids.back(), 0};
    const auto half_widths = mixed_gradient_solid_half_widths(entry, num_physical, middle_window);
    auto       half_width  = [&](size_t color) { return double(half_widths[color]); };
    for (size_t color = 1; color + 1 < ids.size(); ++color) {
        if (std::abs(t - stops[2 * color]) <= half_width(color))
            return {ids[color], ids[color], 0};
    }
    for (size_t i = 0; i + 1 < ids.size(); ++i) {
        const double lo = stops[2 * i] + half_width(i);
        const double hi = stops[2 * i + 2] - half_width(i + 1);
        if (t > hi && i + 2 < ids.size())
            continue;
        if (hi <= lo + 2e-8)
            return {ids[i + 1], ids[i + 1], 0};
        const double midpoint = std::clamp(double(stops[2 * i + 1]), lo + 1e-8, hi - 1e-8);
        const double ratio    = t <= midpoint ? 0.5 * (t - lo) / (midpoint - lo) : 0.5 + 0.5 * (t - midpoint) / (hi - midpoint);
        const int    percent  = int(std::lround(100.0 * std::clamp(ratio, 0.0, 1.0)));
        if (percent == 0)
            return {ids[i], ids[i], 0};
        if (percent == 100)
            return {ids[i + 1], ids[i + 1], 0};
        return {ids[i], ids[i + 1], percent};
    }
    return {ids.back(), ids.back(), 0};
}

MixedGradientLocalZSample sample_mixed_gradient_local_z(const MixedFilamentDefinition& entry,
                                                        size_t                         num_physical,
                                                        double                         progress,
                                                        double                         middle_window,
                                                        double                         nominal_height,
                                                        double                         minimum_height,
                                                        const std::vector<double>&     max_layer_heights)
{
    MixedGradientLocalZSample result;
    result.mix           = sample_mixed_gradient(entry, num_physical, progress, middle_window);
    const double minimum = std::max(0.01, minimum_height);
    const double nominal = std::max(nominal_height, 2.0 * minimum);
    const auto   heights = mixed_filament_local_z_pair_heights(nominal, minimum, result.mix.mix_b_percent);
    result.height_a      = heights.first;
    result.height_b      = heights.second;
    if (!entry.behavior.gradient.enabled || !std::isfinite(progress))
        return result;

    const auto ids    = mixed_gradient_components(entry, num_physical);
    const auto stops  = mixed_gradient_stops(entry, num_physical);
    const auto widths = mixed_gradient_solid_half_widths(entry, num_physical, middle_window);
    // Ease the solid-zone filament up to nominal + 0.08 mm over the adjacent
    // 15% of each transition. This is a pass-height target, not a cycle target.
    constexpr double ramp_fraction = 0.15;
    for (size_t i = 1; i + 1 < ids.size(); ++i) {
        if (widths[i] <= 0.f)
            continue;
        const double lo         = stops[2 * i] - widths[i];
        const double hi         = stops[2 * i] + widths[i];
        const double left_span  = lo - (stops[2 * i - 2] + widths[i - 1]);
        const double right_span = (stops[2 * i + 2] - widths[i + 1]) - hi;
        // A moved midpoint must not make us thicken the still-minority color.
        const double left_ramp  = std::min(ramp_fraction * left_span, lo - stops[2 * i - 1]);
        const double right_ramp = std::min(ramp_fraction * right_span, stops[2 * i + 1] - hi);
        double       strength   = 1.0;
        if (progress < lo)
            strength = left_ramp > 0.0 ? 1.0 - (lo - progress) / left_ramp : 0.0;
        else if (progress > hi)
            strength = right_ramp > 0.0 ? 1.0 - (progress - hi) / right_ramp : 0.0;
        strength = std::clamp(strength, 0.0, 1.0);
        strength = strength * strength * (3.0 - 2.0 * strength);
        if (strength <= 0.0)
            continue;
        double* dominant = result.mix.component_a == ids[i] ? &result.height_a :
                           result.mix.component_b == ids[i] ? &result.height_b :
                                                              nullptr;
        if (dominant == nullptr || *dominant <= 0.0)
            continue;
        const double configured_max = ids[i] <= max_layer_heights.size() ? max_layer_heights[ids[i] - 1] : 0.30;
        const double maximum        = std::isfinite(configured_max) && configured_max > 0.0 ? configured_max : 0.30;
        const double target         = std::min(nominal + 0.08, maximum);
        // Preserve the fading pass and never shrink an existing pass here.
        // The planner splits any pre-existing oversized passes as usual.
        *dominant += strength * std::max(0.0, target - *dominant);
        break;
    }
    return result;
}

} // namespace Slic3r
