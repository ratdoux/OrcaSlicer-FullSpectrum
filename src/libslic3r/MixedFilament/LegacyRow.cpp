#include "Internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace Slic3r { namespace MixedFilamentInternal {

bool parse_row_definition(const std::string& row,
                          unsigned int&      a,
                          unsigned int&      b,
                          uint64_t&          stable_id,
                          bool&              enabled,
                          bool&              custom,
                          bool&              origin_auto,
                          int&               mix_b_percent,
                          std::string&       gradient_component_ids,
                          std::string&       gradient_component_weights,
                          std::string&       gradient_stop_positions,
                          std::string&       manual_pattern,
                          int&               distribution_mode,
                          int&               local_z_max_sublayers,
                          float&             component_a_surface_offset,
                          float&             component_b_surface_offset,
                          std::string&       component_surface_offsets,
                          bool&              perimeter_modulation,
                          bool&              deleted,
                          bool&              gradient_enabled,
                          float&             gradient_start,
                          float&             gradient_end,
                          int&               ui_mode)
{
    auto trim_copy = [](const std::string& s) {
        size_t lo = 0;
        size_t hi = s.size();
        while (lo < hi && std::isspace(static_cast<unsigned char>(s[lo])))
            ++lo;
        while (hi > lo && std::isspace(static_cast<unsigned char>(s[hi - 1])))
            --hi;
        return s.substr(lo, hi - lo);
    };

    auto parse_int_token = [&trim_copy](const std::string& tok, int& out) {
        const std::string t = trim_copy(tok);
        if (t.empty())
            return false;
        try {
            size_t consumed = 0;
            int    v        = std::stoi(t, &consumed);
            if (consumed != t.size())
                return false;
            out = v;
            return true;
        } catch (...) {
            return false;
        }
    };

    auto parse_uint64_token = [&trim_copy](const std::string& tok, uint64_t& out) {
        const std::string t = trim_copy(tok);
        if (t.empty())
            return false;
        try {
            size_t                   consumed = 0;
            const unsigned long long v        = std::stoull(t, &consumed);
            if (consumed != t.size())
                return false;
            out = uint64_t(v);
            return true;
        } catch (...) {
            return false;
        }
    };

    auto parse_float_token = [&trim_copy](const std::string& tok, float& out) {
        const std::string t = trim_copy(tok);
        return parse_invariant_float(t, out);
    };

    std::vector<std::string> tokens;
    std::stringstream        ss(row);
    std::string              token;
    while (std::getline(ss, token, ','))
        tokens.emplace_back(trim_copy(token));

    if (tokens.size() < 4)
        return false;

    int values[5] = {0, 0, 1, 1, 50};
    if (tokens.size() == 4) {
        // Legacy: a,b,enabled,mix
        if (!parse_int_token(tokens[0], values[0]) || !parse_int_token(tokens[1], values[1]) || !parse_int_token(tokens[2], values[2]) ||
            !parse_int_token(tokens[3], values[4]))
            return false;
    } else {
        // Current legacy rows keep token 5 reserved for a retired same-layer flag.
        for (size_t i = 0; i < 5; ++i)
            if (!parse_int_token(tokens[i], values[i]))
                return false;
    }

    if (values[0] <= 0 || values[1] <= 0)
        return false;

    a             = unsigned(values[0]);
    b             = unsigned(values[1]);
    stable_id     = 0;
    enabled       = (values[2] != 0);
    custom        = (tokens.size() == 4) ? true : (values[3] != 0);
    origin_auto   = !custom;
    mix_b_percent = clamp_int(values[4], 0, 100);
    gradient_component_ids.clear();
    gradient_component_weights.clear();
    gradient_stop_positions.clear();
    manual_pattern.clear();
    distribution_mode          = int(MixedFilamentLegacyRow::Simple);
    local_z_max_sublayers      = 0;
    component_a_surface_offset = 0.f;
    component_b_surface_offset = 0.f;
    component_surface_offsets.clear();
    perimeter_modulation       = false;
    deleted                    = false;
    gradient_enabled           = false;
    gradient_start             = MixedFilamentLegacyRow::k_default_gradient_dominant;
    gradient_end               = MixedFilamentLegacyRow::k_default_gradient_minority;
    ui_mode                    = -1;

    auto is_metadata_token = [](const std::string& tok) {
        if (tok.empty())
            return true;
        const char first = char(std::tolower(static_cast<unsigned char>(tok[0])));
        if (first == 'g' || first == 'w' || first == 'p' || first == 'm' || first == 'z' || first == 'x' || first == 'd' || first == 'o' ||
            first == 'u' || first == 'r')
            return true;
        return tok.size() >= 2 && first == 'c' && std::tolower(static_cast<unsigned char>(tok[1])) == 'm';
    };

    size_t token_idx = 5;
    if (tokens.size() >= 6) {
        // Backward compatibility:
        // - old: token[5] is a retired same-layer flag ("0"/"1")
        // - old: token[5] is pattern ("12", "1212", ...)
        // - new: token[5] may be metadata token ("g..." / "m...")
        const std::string& compat_token = tokens[5];
        if (compat_token == "0" || compat_token == "1") {
            token_idx = 6;
        } else if (is_metadata_token(compat_token)) {
            token_idx = 5;
        } else {
            manual_pattern = compat_token;
            token_idx      = 6;
        }
    }

    std::vector<std::string> pattern_tokens;
    pattern_tokens.reserve(tokens.size() > token_idx ? tokens.size() - token_idx : 1);
    if (!manual_pattern.empty())
        pattern_tokens.push_back(manual_pattern);
    for (size_t i = token_idx; i < tokens.size(); ++i) {
        const std::string& tok = tokens[i];
        if (tok.empty())
            continue;
        if (tok[0] == 'g' || tok[0] == 'G') {
            gradient_component_ids = tok.substr(1);
            continue;
        }
        if (tok[0] == 'w' || tok[0] == 'W') {
            gradient_component_weights = tok.substr(1);
            continue;
        }
        if (tok[0] == 'p' || tok[0] == 'P') {
            gradient_stop_positions = tok.substr(1);
            continue;
        }
        if (tok[0] == 'm' || tok[0] == 'M') {
            int parsed_mode = distribution_mode;
            if (parse_int_token(tok.substr(1), parsed_mode))
                distribution_mode = clamp_int(parsed_mode, int(MixedFilamentLegacyRow::LayerCycle), int(MixedFilamentLegacyRow::Simple));
            continue;
        }
        if (tok[0] == 'z' || tok[0] == 'Z') {
            int parsed_max_sublayers = local_z_max_sublayers;
            if (parse_int_token(tok.substr(1), parsed_max_sublayers))
                local_z_max_sublayers = std::max(0, parsed_max_sublayers);
            continue;
        }
        if ((tok[0] == 'x' || tok[0] == 'X') && tok.size() >= 3) {
            const char component = char(std::tolower(static_cast<unsigned char>(tok[1])));
            if (component == 'p') {
                int parsed_perimeter_modulation = perimeter_modulation ? 1 : 0;
                if (parse_int_token(tok.substr(2), parsed_perimeter_modulation))
                    perimeter_modulation = parsed_perimeter_modulation != 0;
                continue;
            }
            if (component == 'v') {
                component_surface_offsets = tok.substr(2);
                continue;
            }
            if (component == 'a' || component == 'b') {
                float parsed_offset = component == 'a' ? component_a_surface_offset : component_b_surface_offset;
                if (parse_float_token(tok.substr(2), parsed_offset)) {
                    if (component == 'a')
                        component_a_surface_offset = clamp_surface_offset(parsed_offset);
                    else
                        component_b_surface_offset = clamp_surface_offset(parsed_offset);
                }
                continue;
            }
        }
        if (tok[0] == 'd' || tok[0] == 'D') {
            int parsed_deleted = deleted ? 1 : 0;
            if (parse_int_token(tok.substr(1), parsed_deleted))
                deleted = parsed_deleted != 0;
            continue;
        }
        if (tok[0] == 'o' || tok[0] == 'O') {
            int parsed_origin_auto = origin_auto ? 1 : 0;
            if (parse_int_token(tok.substr(1), parsed_origin_auto))
                origin_auto = parsed_origin_auto != 0;
            continue;
        }
        if (tok[0] == 'u' || tok[0] == 'U') {
            uint64_t parsed_stable_id = stable_id;
            if (parse_uint64_token(tok.substr(1), parsed_stable_id))
                stable_id = parsed_stable_id;
            continue;
        }
        if ((tok[0] == 'c' || tok[0] == 'C') && tok.size() >= 3 && (tok[1] == 'm' || tok[1] == 'M')) {
            int parsed_ui_mode = ui_mode;
            if (parse_int_token(tok.substr(2), parsed_ui_mode))
                ui_mode = clamp_int(parsed_ui_mode, -1, 3);
            continue;
        }
        if (tok[0] == 'r' || tok[0] == 'R') {
            const std::string body = tok.substr(1);
            const size_t      s1   = body.find('/');
            const size_t      s2   = s1 == std::string::npos ? std::string::npos : body.find('/', s1 + 1);
            if (s1 != std::string::npos && s2 != std::string::npos) {
                int   parsed_flag  = gradient_enabled ? 1 : 0;
                float parsed_start = gradient_start;
                float parsed_end   = gradient_end;
                if (parse_int_token(body.substr(0, s1), parsed_flag) &&
                    parse_float_token(body.substr(s1 + 1, s2 - s1 - 1), parsed_start) &&
                    parse_float_token(body.substr(s2 + 1), parsed_end)) {
                    gradient_enabled = parsed_flag != 0;
                    if (parsed_start > 0.f && parsed_start < 1.f)
                        gradient_start = parsed_start;
                    if (parsed_end > 0.f && parsed_end < 1.f)
                        gradient_end = parsed_end;
                }
            }
            continue;
        }
        pattern_tokens.push_back(tok);
    }

    if (!pattern_tokens.empty()) {
        std::ostringstream joined_pattern;
        for (size_t i = 0; i < pattern_tokens.size(); ++i) {
            if (i != 0)
                joined_pattern << ',';
            joined_pattern << pattern_tokens[i];
        }
        manual_pattern = joined_pattern.str();
    }

    distribution_mode = normalize_legacy_distribution_mode(distribution_mode, gradient_component_ids);
    if (gradient_enabled) {
        gradient_start = std::clamp(gradient_start, 0.01f, 0.99f);
        gradient_end   = std::clamp(gradient_end, 0.01f, 0.99f);
        if (std::abs(gradient_start - gradient_end) < MixedFilamentLegacyRow::k_min_gradient_difference)
            gradient_enabled = false;
    }
    return true;
}

constexpr int RETIRED_SAME_LAYER_DISTRIBUTION_MODE = 1;

int normalize_legacy_distribution_mode(int distribution_mode, const std::string& gradient_component_ids)
{
    const int clamped_mode = clamp_int(distribution_mode, int(MixedFilamentLegacyRow::LayerCycle), int(MixedFilamentLegacyRow::Simple));
    if (clamped_mode != RETIRED_SAME_LAYER_DISTRIBUTION_MODE)
        return clamped_mode;

    const size_t gradient_count = decode_gradient_component_ids(gradient_component_ids, 9).size();
    return gradient_count >= 3 ? int(MixedFilamentLegacyRow::LayerCycle) : int(MixedFilamentLegacyRow::Simple);
}

void normalize_legacy_row(MixedFilamentLegacyRow& mf)
{
    mf.distribution_mode = normalize_legacy_distribution_mode(mf.distribution_mode, mf.gradient_component_ids);
    const size_t gradient_component_count = decode_gradient_component_ids(mf.gradient_component_ids, 0).size();
    const size_t expected_stops = gradient_component_count >= 3 ? 2 * gradient_component_count - 1 :
        (mf.gradient_enabled ? size_t(3) : size_t(0));
    mf.gradient_stop_positions = normalize_gradient_stop_positions(mf.gradient_stop_positions, expected_stops);
}

MixedFilamentDistributionMode mixed_filament_distribution_from_legacy_mode(int distribution_mode, const std::string& gradient_component_ids)
{
    switch (normalize_legacy_distribution_mode(distribution_mode, gradient_component_ids)) {
    case int(MixedFilamentLegacyRow::LayerCycle): return MixedFilamentDistributionMode::LayerCycle;
    case int(MixedFilamentLegacyRow::Simple):
    default: return MixedFilamentDistributionMode::Simple;
    }
}

int legacy_distribution_mode_from_mixed_filament_distribution(MixedFilamentDistributionMode distribution)
{
    switch (distribution) {
    case MixedFilamentDistributionMode::LayerCycle: return int(MixedFilamentLegacyRow::LayerCycle);
    case MixedFilamentDistributionMode::Simple:
    default: return int(MixedFilamentLegacyRow::Simple);
    }
}

}} // namespace Slic3r::MixedFilamentInternal
