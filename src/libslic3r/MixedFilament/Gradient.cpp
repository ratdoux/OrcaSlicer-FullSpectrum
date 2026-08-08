#include "Internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <sstream>

namespace Slic3r { namespace MixedFilamentInternal {

std::string normalize_gradient_component_ids(const std::string& components)
{
    std::string normalized;
    normalized.reserve(components.size());
    bool seen[10] = {false};
    for (const char c : components) {
        if (c < '1' || c > '9')
            continue;
        const int idx = c - '0';
        if (seen[idx])
            continue;
        seen[idx] = true;
        normalized.push_back(c);
    }
    return normalized;
}

std::vector<unsigned int> decode_gradient_component_ids(const std::string& components, size_t num_physical)
{
    std::vector<unsigned int> ids;
    if (components.empty() || num_physical == 0)
        return ids;

    bool seen[10] = {false};
    ids.reserve(components.size());
    for (const char c : components) {
        if (c < '1' || c > '9')
            continue;
        const unsigned int id = unsigned(c - '0');
        if (id == 0 || id > num_physical || seen[id])
            continue;
        seen[id] = true;
        ids.emplace_back(id);
    }
    return ids;
}

std::vector<int> parse_gradient_weight_tokens(const std::string& weights)
{
    std::vector<int> out;
    std::string      token;
    for (const char c : weights) {
        if (c >= '0' && c <= '9') {
            token.push_back(c);
            continue;
        }
        if (!token.empty()) {
            out.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        out.emplace_back(std::max(0, std::atoi(token.c_str())));
    return out;
}

std::vector<int> normalize_weight_vector_to_percent(const std::vector<int>& weights)
{
    std::vector<int> out(weights.size(), 0);
    if (weights.empty())
        return out;
    int sum = 0;
    for (const int w : weights)
        sum += std::max(0, w);
    if (sum <= 0)
        return out;

    std::vector<double> remainders(weights.size(), 0.);
    int                 assigned = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        const double exact = 100.0 * double(std::max(0, weights[i])) / double(sum);
        out[i]             = int(std::floor(exact));
        remainders[i]      = exact - double(out[i]);
        assigned += out[i];
    }
    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx = 0;
        double best_rem = -1.0;
        for (size_t i = 0; i < remainders.size(); ++i) {
            if (weights[i] <= 0)
                continue;
            if (remainders[i] > best_rem) {
                best_rem = remainders[i];
                best_idx = i;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }
    return out;
}

std::string normalize_gradient_component_weights(const std::string& weights, size_t expected_components)
{
    if (expected_components == 0)
        return std::string();
    std::vector<int> parsed = parse_gradient_weight_tokens(weights);
    if (parsed.size() != expected_components)
        return std::string();
    std::vector<int> normalized = normalize_weight_vector_to_percent(parsed);
    int              sum        = 0;
    for (const int v : normalized)
        sum += v;
    if (sum <= 0)
        return std::string();

    std::ostringstream ss;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (i > 0)
            ss << '/';
        ss << normalized[i];
    }
    return ss.str();
}

std::vector<int> decode_gradient_component_weights(const std::string& weights, size_t expected_components)
{
    if (expected_components == 0)
        return {};
    std::vector<int> parsed = parse_gradient_weight_tokens(weights);
    if (parsed.size() != expected_components)
        return {};
    std::vector<int> normalized = normalize_weight_vector_to_percent(parsed);
    int              sum        = 0;
    for (const int v : normalized)
        sum += v;
    return (sum > 0) ? normalized : std::vector<int>();
}

std::vector<unsigned int> build_weighted_gradient_sequence(const std::vector<unsigned int>& ids, const std::vector<int>& weights)
{
    if (ids.empty())
        return {};

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const int w = (i < weights.size()) ? std::max(0, weights[i]) : 0;
        if (w <= 0)
            continue;
        filtered_ids.emplace_back(ids[i]);
        counts.emplace_back(w);
    }
    if (filtered_ids.empty()) {
        filtered_ids = ids;
        counts.assign(ids.size(), 1);
    }

    int g = 0;
    for (const int c : counts)
        g = std::gcd(g, std::max(1, c));
    if (g > 1) {
        for (int& c : counts)
            c = std::max(1, c / g);
    }

    int           cycle       = std::accumulate(counts.begin(), counts.end(), 0);
    constexpr int k_max_cycle = 48;
    if (cycle > k_max_cycle) {
        const double scale = double(k_max_cycle) / double(cycle);
        for (int& c : counts)
            c = std::max(1, int(std::round(double(c) * scale)));
        cycle = std::accumulate(counts.begin(), counts.end(), 0);
        while (cycle > k_max_cycle) {
            auto it = std::max_element(counts.begin(), counts.end());
            if (it == counts.end() || *it <= 1)
                break;
            --(*it);
            --cycle;
        }
    }
    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx   = 0;
        double best_score = -1e9;
        for (size_t i = 0; i < counts.size(); ++i) {
            const double target = double((pos + 1) * counts[i]) / double(cycle);
            const double score  = target - double(emitted[i]);
            if (score > best_score) {
                best_score = score;
                best_idx   = i;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }
    return sequence;
}

std::vector<float> parse_gradient_stop_position_tokens(const std::string& positions)
{
    std::vector<float> out;
    std::stringstream stream(positions);
    std::string       token;
    while (std::getline(stream, token, '/')) {
        float value = 0.f;
        if (!parse_invariant_float(token, value))
            return {};
        out.emplace_back(value);
    }
    return out;
}

std::vector<float> normalize_gradient_stop_position_vector(const std::vector<float>& positions, size_t expected_stops)
{
    if (expected_stops < 3 || positions.size() != expected_stops)
        return {};

    std::vector<float> out;
    out.reserve(expected_stops);
    for (const float value : positions) {
        if (!std::isfinite(value))
            return {};
        out.emplace_back(std::clamp(value, 0.f, 1.f));
    }

    out.front() = 0.f;
    out.back()  = 1.f;

    float min_gap = 0.0001f;
    if (float(expected_stops - 1) * min_gap >= 1.f)
        min_gap = 0.f;

    for (size_t i = 1; i < out.size(); ++i)
        out[i] = std::max(out[i], out[i - 1] + min_gap);

    out.back() = 1.f;
    for (size_t i = out.size() - 1; i > 0; --i)
        out[i - 1] = std::min(out[i - 1], out[i] - min_gap);

    out.front() = 0.f;
    for (float& value : out)
        value = std::clamp(value, 0.f, 1.f);

    return out;
}

std::string normalize_gradient_stop_positions(const std::string& positions, size_t expected_stops)
{
    return legacy_gradient_positions_from_float_vector(
        normalize_gradient_stop_position_vector(parse_gradient_stop_position_tokens(positions), expected_stops));
}

std::vector<float> decode_gradient_stop_positions(const std::string& positions, size_t expected_stops)
{
    return normalize_gradient_stop_position_vector(parse_gradient_stop_position_tokens(positions), expected_stops);
}

std::string legacy_gradient_positions_from_float_vector(const std::vector<float>& positions)
{
    if (positions.empty())
        return std::string();

    std::ostringstream out;
    for (size_t i = 0; i < positions.size(); ++i) {
        if (i != 0)
            out << '/';
        out << format_invariant_decimal(std::clamp(positions[i], 0.f, 1.f), 4);
    }
    return out.str();
}

std::vector<int> equal_percent_vector(size_t count)
{
    std::vector<int> out(count, 0);
    if (count == 0)
        return out;

    const int base      = int(100 / count);
    int       remainder = int(100 % count);
    for (int& value : out) {
        value = base;
        if (remainder > 0) {
            ++value;
            --remainder;
        }
    }
    return out;
}

bool has_positive_sum(const std::vector<int>& values)
{
    int sum = 0;
    for (const int value : values)
        sum += std::max(0, value);
    return sum > 0;
}

std::vector<int> normalized_percent_vector_or_equal(const std::vector<int>& weights, size_t count)
{
    if (weights.size() != count)
        return equal_percent_vector(count);

    std::vector<int> normalized = normalize_weight_vector_to_percent(weights);
    if (!has_positive_sum(normalized))
        normalized = equal_percent_vector(count);
    return normalized;
}

std::optional<MixedFilamentWeightedBlend> mixed_filament_weighted_blend_from_legacy_row(const MixedFilamentLegacyRow& row,
                                                                                       size_t                        num_physical)
{
    const std::string normalized_ids = normalize_gradient_component_ids(row.gradient_component_ids);
    if (normalized_ids.size() < 3)
        return std::nullopt;

    const std::vector<int> parsed_weights = parse_gradient_weight_tokens(row.gradient_component_weights);
    const bool             use_parsed_weights = parsed_weights.size() == normalized_ids.size();
    std::vector<unsigned int> ids;
    std::vector<int>          weights;
    ids.reserve(normalized_ids.size() + 2);
    weights.reserve(normalized_ids.size() + 2);

    const auto is_valid_id = [num_physical](unsigned int id) {
        return id != 0 && (num_physical == 0 || id <= num_physical);
    };
    const auto parsed_weight_for = [&](unsigned int id) -> std::optional<int> {
        if (!use_parsed_weights)
            return std::nullopt;
        for (size_t i = 0; i < normalized_ids.size(); ++i)
            if (unsigned(normalized_ids[i] - '0') == id)
                return parsed_weights[i];
        return std::nullopt;
    };
    const auto add_id = [&](unsigned int id, int fallback_weight) {
        if (!is_valid_id(id) || std::find(ids.begin(), ids.end(), id) != ids.end())
            return;
        ids.emplace_back(id);
        weights.emplace_back(use_parsed_weights ? parsed_weight_for(id).value_or(fallback_weight) : 0);
    };

    const int pair_b_percent = std::clamp(row.mix_b_percent, 0, 100);
    add_id(row.component_a, 100 - pair_b_percent);
    add_id(row.component_b, pair_b_percent);
    for (const char c : normalized_ids)
        add_id(unsigned(c - '0'), 0);

    if (ids.size() < 3)
        return std::nullopt;

    weights = normalized_percent_vector_or_equal(weights, ids.size());

    MixedFilamentWeightedBlend out;
    out.components.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        out.components.push_back({{ids[i]}, weights[i]});

    return out;
}

std::string legacy_gradient_weights_from_percent_vector(const std::vector<int>& weights)
{
    std::ostringstream out;
    for (size_t i = 0; i < weights.size(); ++i) {
        if (i != 0)
            out << '/';
        out << std::max(0, weights[i]);
    }
    return out.str();
}

void apply_mixed_filament_blend_to_legacy_row(const MixedFilamentWeightedBlend& blend, MixedFilamentLegacyRow& row)
{
    std::string      ids;
    std::vector<int> weights;
    bool             seen[10] = {false};

    ids.reserve(blend.components.size());
    weights.reserve(blend.components.size());
    for (const MixedFilamentWeightedComponent& component : blend.components) {
        const unsigned int id = component.filament.id;
        if (id == 0 || id > 9 || seen[id])
            continue;
        seen[id] = true;
        ids.push_back(char('0' + id));
        weights.emplace_back(component.percent);
    }

    if (ids.size() < 3) {
        row.gradient_component_ids.clear();
        row.gradient_component_weights.clear();
        return;
    }

    row.gradient_component_ids     = ids;
    row.gradient_component_weights = legacy_gradient_weights_from_percent_vector(
        normalized_percent_vector_or_equal(weights, weights.size()));
}

}} // namespace Slic3r::MixedFilamentInternal
