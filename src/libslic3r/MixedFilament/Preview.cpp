#include "Internal.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Slic3r { namespace MixedFilamentInternal {

unsigned int decode_manual_pattern_preview_token(char token, unsigned int component_a, unsigned int component_b, size_t num_physical)
{
    unsigned int extruder_id = 0;
    if (token == '1')
        extruder_id = component_a;
    else if (token == '2')
        extruder_id = component_b;
    else if (token >= '3' && token <= '9')
        extruder_id = unsigned(token - '0');

    return (extruder_id >= 1 && extruder_id <= num_physical) ? extruder_id : 0;
}

std::vector<unsigned int> build_grouped_manual_pattern_preview_sequence(
    const std::string& pattern, unsigned int component_a, unsigned int component_b, size_t num_physical, size_t wall_loops)
{
    std::vector<unsigned int> sequence;
    if (num_physical == 0)
        return sequence;

    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(pattern);
    if (normalized.empty())
        return sequence;

    const std::vector<std::string> groups = split_manual_pattern_groups(normalized);
    if (groups.empty())
        return sequence;

    if (groups.size() == 1) {
        sequence.reserve(normalized.size());
        for (const char token : normalized) {
            const unsigned int extruder_id = decode_manual_pattern_preview_token(token, component_a, component_b, num_physical);
            if (extruder_id != 0)
                sequence.emplace_back(extruder_id);
        }
        return sequence;
    }

    constexpr size_t k_max_preview_cycle = 48;
    size_t           cycle               = 1;
    for (const std::string& group : groups) {
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
            const std::string& group = groups[std::min(wall_idx, groups.size() - 1)];
            if (group.empty())
                continue;
            const char         token       = group[layer_idx % group.size()];
            const unsigned int extruder_id = decode_manual_pattern_preview_token(token, component_a, component_b, num_physical);
            if (extruder_id != 0)
                sequence.emplace_back(extruder_id);
        }
    }

    return sequence;
}

std::pair<int, int> effective_pair_preview_ratios(int percent_b)
{
    const int mix_b   = std::clamp(percent_b, 0, 100);
    int       ratio_a = 1;
    int       ratio_b = 0;

    if (mix_b >= 100) {
        ratio_a = 0;
        ratio_b = 1;
    } else if (mix_b > 0) {
        const int  pct_b        = mix_b;
        const int  pct_a        = 100 - pct_b;
        const bool b_is_major   = pct_b >= pct_a;
        const int  major_pct    = b_is_major ? pct_b : pct_a;
        const int  minor_pct    = b_is_major ? pct_a : pct_b;
        const int  major_layers = std::max(1, int(std::lround(double(major_pct) / double(std::max(1, minor_pct)))));
        ratio_a                 = b_is_major ? 1 : major_layers;
        ratio_b                 = b_is_major ? major_layers : 1;
    }

    if (ratio_a > 0 && ratio_b > 0) {
        const int g = std::gcd(ratio_a, ratio_b);
        if (g > 1) {
            ratio_a /= g;
            ratio_b /= g;
        }
    }

    return {std::max(0, ratio_a), std::max(0, ratio_b)};
}

std::vector<unsigned int> build_effective_pair_preview_sequence(unsigned int component_a,
                                                                unsigned int component_b,
                                                                int          percent_b,
                                                                bool         limit_cycle)
{
    std::vector<unsigned int> sequence;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return sequence;

    auto [ratio_a, ratio_b]   = effective_pair_preview_ratios(percent_b);
    constexpr int k_max_cycle = 24;
    if (limit_cycle && ratio_a > 0 && ratio_b > 0 && ratio_a + ratio_b > k_max_cycle) {
        const double scale = double(k_max_cycle) / double(ratio_a + ratio_b);
        ratio_a            = std::max(1, int(std::round(double(ratio_a) * scale)));
        ratio_b            = std::max(1, int(std::round(double(ratio_b) * scale)));
    }
    if (ratio_a == 0 && ratio_b == 0)
        ratio_a = 1;

    const int cycle = std::max(1, ratio_a + ratio_b);
    sequence.reserve(size_t(cycle));
    for (int pos = 0; pos < cycle; ++pos) {
        const int b_before = (pos * ratio_b) / cycle;
        const int b_after  = ((pos + 1) * ratio_b) / cycle;
        sequence.emplace_back((b_after > b_before) ? component_b : component_a);
    }
    return sequence;
}

std::string blend_display_color_from_sequence(const std::vector<std::string>&  colors,
                                              const std::vector<double>&       tds,
                                              const std::vector<std::string>&  material_ids,
                                              size_t                           num_physical,
                                              const std::vector<unsigned int>& sequence,
                                              const std::string&               fallback)
{
    if (colors.empty() || sequence.empty() || num_physical == 0)
        return fallback;

    std::vector<size_t> counts(num_physical + 1, size_t(0));
    size_t              total = 0;
    for (const unsigned int id : sequence) {
        if (id == 0 || id > num_physical)
            continue;
        ++counts[id];
        ++total;
    }
    if (total == 0)
        return fallback;

    std::vector<MixedFilamentColorInput> color_percents;
    color_percents.reserve(num_physical);
    for (size_t id = 1; id <= num_physical; ++id) {
        if (counts[id] == 0 || id > colors.size())
            continue;
        std::optional<double> td;
        if (MixedFilamentManager::use_td_for_color_prediction() && id <= tds.size() && std::isfinite(tds[id - 1]) &&
            tds[id - 1] > EPSILON)
            td = tds[id - 1];
        std::optional<std::string> material_id;
        if (id <= material_ids.size() && !material_ids[id - 1].empty())
            material_id = material_ids[id - 1];
        color_percents.push_back({colors[id - 1], int(counts[id]), td, material_id});
    }
    if (color_percents.empty())
        return fallback;

    if (color_percents.size() == 1)
        return color_percents.front().color_hex;

    return MixedFilamentManager::blend_color_multi(color_percents);
}

}} // namespace Slic3r::MixedFilamentInternal

namespace Slic3r {

std::vector<double> mixed_filament_local_z_preview_pass_heights(double nominal_layer_height,
                                                                 double min_sublayer_height,
                                                                 double preferred_a_height,
                                                                 double preferred_b_height,
                                                                 int    mix_b_percent,
                                                                 int    max_sublayers_limit)
{
    if (nominal_layer_height <= EPSILON)
        return {};

    const double height  = nominal_layer_height;
    const double minimum = std::max(0.01, min_sublayer_height);
    const double maximum = std::max(minimum, height);
    const size_t pass_limit = max_sublayers_limit >= 2 ? size_t(max_sublayers_limit) : size_t(0);

    auto fit_to_height = [height, minimum, maximum](std::vector<double>& passes) {
        if (passes.empty())
            return false;

        double delta = height - std::accumulate(passes.begin(), passes.end(), 0.0);
        if (delta > EPSILON) {
            for (double& pass : passes) {
                const double take = std::min(maximum - pass, delta);
                if (take > 0.0) {
                    pass += take;
                    delta -= take;
                }
                if (delta <= EPSILON)
                    break;
            }
        } else if (delta < -EPSILON) {
            for (auto it = passes.rbegin(); it != passes.rend() && delta < -EPSILON; ++it) {
                const double take = std::min(*it - minimum, -delta);
                if (take > 0.0) {
                    *it -= take;
                    delta += take;
                }
            }
        }

        return std::abs(delta) <= 1e-6 &&
               std::all_of(passes.begin(), passes.end(), [minimum, maximum](double pass) {
                   return pass >= minimum - 1e-6 && pass <= maximum + 1e-6;
               });
    };

    auto build_pair = [height, minimum](double target_a, double target_b) {
        if (target_a <= EPSILON || target_b <= EPSILON || height < 2.0 * minimum - EPSILON)
            return std::vector<double>{height};

        const double ratio_a = target_a / (target_a + target_b);
        const double height_a = std::clamp(height * ratio_a, minimum, height - minimum);
        return std::vector<double>{height_a, height - height_a};
    };

    if (preferred_a_height > EPSILON || preferred_b_height > EPSILON) {
        std::vector<double> cadence;
        if (preferred_a_height > EPSILON)
            cadence.push_back(std::clamp(preferred_a_height, minimum, maximum));
        if (preferred_b_height > EPSILON)
            cadence.push_back(std::clamp(preferred_b_height, minimum, maximum));

        std::vector<double> passes;
        double used = 0.0;
        size_t index = 0;
        while (!cadence.empty() && used + cadence[index] < height - EPSILON) {
            passes.push_back(cadence[index]);
            used += cadence[index];
            index = (index + 1) % cadence.size();
        }
        if (height - used > EPSILON)
            passes.push_back(height - used);

        if (fit_to_height(passes) && (pass_limit == 0 || passes.size() <= pass_limit))
            return passes;
        if (preferred_a_height > EPSILON && preferred_b_height > EPSILON)
            return build_pair(preferred_a_height, preferred_b_height);
        return height >= 2.0 * minimum - EPSILON ? std::vector<double>{0.5 * height, 0.5 * height} : std::vector<double>{height};
    }

    const auto targets = mixed_filament_local_z_pair_heights(height, minimum, mix_b_percent);
    return build_pair(targets.first, targets.second);
}

} // namespace Slic3r
