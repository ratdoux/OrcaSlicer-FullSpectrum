#include "Internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace Slic3r { namespace MixedFilamentInternal {

bool is_pattern_separator(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '-' || c == '_' || c == '|' || c == ':' || c == ';' || c == ',';
}

bool decode_pattern_step(char c, char& out)
{
    if (c >= '1' && c <= '9') {
        out = c;
        return true;
    }
    switch (std::tolower(static_cast<unsigned char>(c))) {
    case 'a': out = '1'; return true;
    case 'b': out = '2'; return true;
    default: return false;
    }
}

std::vector<std::string> split_manual_pattern_groups(const std::string& pattern)
{
    std::vector<std::string> groups;
    if (pattern.empty())
        return groups;

    std::string current;
    for (const char c : pattern) {
        if (c == ',') {
            if (!current.empty()) {
                groups.emplace_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        groups.emplace_back(std::move(current));
    return groups;
}

std::string flatten_manual_pattern_groups(const std::string& pattern)
{
    std::string flattened;
    flattened.reserve(pattern.size());
    for (const char c : pattern)
        if (c != ',')
            flattened.push_back(c);
    return flattened;
}

int mix_percent_from_normalized_pattern(const std::string& pattern)
{
    const std::vector<std::string> groups = split_manual_pattern_groups(pattern);
    if (groups.empty())
        return 50;

    // For grouped patterns, blend preview is the average of each perimeter
    // group's own cadence. This keeps simple outer/inner patterns like
    // "12,21" at 50/50 and "11111112,11121111" at 12.5%.
    double blend_b = 0.0;
    for (const std::string& group : groups) {
        if (group.empty())
            continue;
        const int count_b = int(std::count(group.begin(), group.end(), '2'));
        blend_b += double(count_b) / double(group.size());
    }
    return clamp_int(int(std::lround(100.0 * blend_b / double(groups.size()))), 0, 100);
}

unsigned int physical_filament_from_legacy_pattern_token(char token, const MixedFilamentLegacyPair& pair)
{
    if (token == '1')
        return pair.component_a.id;
    if (token == '2')
        return pair.component_b.id;
    if (token >= '3' && token <= '9')
        return unsigned(token - '0');
    return 0;
}

std::string legacy_manual_pattern_from_mixed_filament_pattern(const MixedFilamentManualPattern& pattern, const MixedFilamentLegacyPair& pair)
{
    if (pattern.groups.empty())
        return {};

    std::ostringstream out;
    for (size_t group_idx = 0; group_idx < pattern.groups.size(); ++group_idx) {
        if (group_idx != 0)
            out << ',';

        const std::vector<MixedFilamentPhysicalRef>& group = pattern.groups[group_idx];
        if (group.empty())
            return {};

        for (const MixedFilamentPhysicalRef& ref : group) {
            if (ref.id == pair.component_a.id) {
                out << '1';
            } else if (ref.id == pair.component_b.id) {
                out << '2';
            } else if (ref.id >= 1 && ref.id <= 9) {
                out << char('0' + ref.id);
            } else {
                return {};
            }
        }
    }

    return MixedFilamentManager::normalize_manual_pattern(out.str());
}

}} // namespace Slic3r::MixedFilamentInternal
