#include "../MixedFilament.hpp"
#include "Internal.hpp"

#include <algorithm>
#include <atomic>
#include <boost/log/trivial.hpp>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r {

namespace {

std::atomic_bool s_mixed_filament_auto_generate_enabled{true};
std::atomic<MixedFilamentColorEngine> s_mixed_filament_color_engine{MixedFilamentColorEngine::FilamentMixer};
std::atomic_bool s_mixed_filament_use_td_for_color_prediction{true};

} // namespace

using namespace MixedFilamentInternal;

namespace {

MixedFilamentWeightedBlend normalized_manager_blend(MixedFilamentWeightedBlend blend, size_t num_physical)
{
    auto clamp_component_id = [num_physical](unsigned int id) {
        if (num_physical == 0)
            return id;
        return std::max<unsigned int>(1, std::min<unsigned int>(id, unsigned(num_physical)));
    };

    MixedFilamentWeightedBlend out;
    out.components.reserve(blend.components.size());
    std::unordered_set<unsigned int> seen_ids;
    for (MixedFilamentWeightedComponent component : blend.components) {
        component.filament.id = clamp_component_id(component.filament.id);
        if (component.filament.id == 0 || !seen_ids.insert(component.filament.id).second)
            continue;
        out.components.emplace_back(component);
    }

    if (out.components.empty()) {
        out.components.push_back({{1}, 50});
        out.components.push_back({{num_physical >= 2 ? 2u : 1u}, 50});
    } else if (out.components.size() == 1) {
        unsigned int fallback_b = out.components.front().filament.id == 1 ? 2u : 1u;
        fallback_b = clamp_component_id(fallback_b);
        if (fallback_b == out.components.front().filament.id && num_physical >= 2)
            fallback_b = out.components.front().filament.id == unsigned(num_physical) ? unsigned(num_physical - 1) : unsigned(num_physical);
        out.components.front().percent = 50;
        out.components.push_back({{fallback_b}, 50});
    } else if (out.components.size() == 2) {
        const int pct_b = clamp_int(out.components[1].percent, 0, 100);
        out.components[0].percent = 100 - pct_b;
        out.components[1].percent = pct_b;
    } else {
        std::vector<int> weights;
        weights.reserve(out.components.size());
        for (const MixedFilamentWeightedComponent& component : out.components)
            weights.emplace_back(component.percent);
        weights = normalized_percent_vector_or_equal(weights, out.components.size());
        for (size_t i = 0; i < out.components.size(); ++i)
            out.components[i].percent = weights[i];
    }

    return out;
}

bool physical_ref_is_present(const MixedFilamentPhysicalRef& ref, unsigned int physical_id)
{
    return ref.id == physical_id;
}

void shift_physical_ref_after_deletion(MixedFilamentPhysicalRef& ref, unsigned int deleted_physical_id)
{
    if (ref.id > deleted_physical_id)
        --ref.id;
}

bool definition_uses_physical_filament(const MixedFilamentDefinition& definition, unsigned int physical_id)
{
    if (definition.recipe.manual_pattern) {
        for (const std::vector<MixedFilamentPhysicalRef>& group : definition.recipe.manual_pattern->groups)
            for (const MixedFilamentPhysicalRef& ref : group)
                if (physical_ref_is_present(ref, physical_id))
                    return true;
        return false;
    }

    for (const MixedFilamentWeightedComponent& component : definition.recipe.blend.components)
        if (physical_ref_is_present(component.filament, physical_id))
            return true;

    return false;
}

void shift_definition_after_physical_deletion(MixedFilamentDefinition& definition, unsigned int deleted_physical_id)
{
    if (definition.recipe.manual_pattern) {
        for (std::vector<MixedFilamentPhysicalRef>& group : definition.recipe.manual_pattern->groups)
            for (MixedFilamentPhysicalRef& ref : group)
                shift_physical_ref_after_deletion(ref, deleted_physical_id);
        return;
    }

    for (MixedFilamentWeightedComponent& component : definition.recipe.blend.components)
        shift_physical_ref_after_deletion(component.filament, deleted_physical_id);
}

MixedFilamentLegacyRow legacy_row_from_definition_for_manager(const MixedFilamentDefinition& definition)
{
    MixedFilamentLegacyRow row = mixed_filament_legacy_row_from_definition(definition);
    normalize_legacy_row(row);
    return row;
}

bool mixed_filament_references_exceed_physical(const std::string& gradient_component_ids,
                                               const std::string& manual_pattern,
                                               size_t             num_physical)
{
    for (unsigned int id : MixedFilamentManager::decode_gradient_component_ids(gradient_component_ids, 0))
        if (id > num_physical)
            return true;

    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(manual_pattern);
    for (const std::string& group : MixedFilamentManager::split_pattern_groups(normalized)) {
        for (const std::string& token : MixedFilamentManager::split_pattern_group_to_tokens(group, 0)) {
            if (token == "1" || token == "2")
                continue;
            char*               end = nullptr;
            const unsigned long id  = std::strtoul(token.c_str(), &end, 10);
            if (end != token.c_str() && *end == '\0' && id > num_physical)
                return true;
        }
    }
    return false;
}

} // namespace

uint64_t MixedFilamentManager::allocate_stable_id()
{
    const uint64_t stable_id = std::max<uint64_t>(1, m_next_stable_id);
    m_next_stable_id         = stable_id + 1;
    return stable_id;
}

uint64_t MixedFilamentManager::normalize_stable_id(uint64_t stable_id)
{
    if (stable_id == 0)
        return allocate_stable_id();
    if (stable_id >= m_next_stable_id)
        m_next_stable_id = stable_id + 1;
    return stable_id;
}

void MixedFilamentManager::set_auto_generate_enabled(bool enabled)
{
    s_mixed_filament_auto_generate_enabled.store(enabled, std::memory_order_relaxed);
}

bool MixedFilamentManager::auto_generate_enabled() { return s_mixed_filament_auto_generate_enabled.load(std::memory_order_relaxed); }

void MixedFilamentManager::set_color_engine(MixedFilamentColorEngine engine)
{
    s_mixed_filament_color_engine.store(engine, std::memory_order_relaxed);
}

MixedFilamentColorEngine MixedFilamentManager::color_engine()
{
    return s_mixed_filament_color_engine.load(std::memory_order_relaxed);
}

void MixedFilamentManager::set_use_td_for_color_prediction(bool enabled)
{
    s_mixed_filament_use_td_for_color_prediction.store(enabled, std::memory_order_relaxed);
}

bool MixedFilamentManager::use_td_for_color_prediction()
{
    return s_mixed_filament_use_td_for_color_prediction.load(std::memory_order_relaxed);
}

MixedFilamentColorEngine MixedFilamentManager::color_engine_from_string(const std::string& value)
{
    if (value == "ks_pair_residual" || value == "fullspectrum_ks_pair_residual")
        return MixedFilamentColorEngine::FullSpectrumKSPairResidual;
    return MixedFilamentColorEngine::FilamentMixer;
}

const char* MixedFilamentManager::color_engine_to_string(MixedFilamentColorEngine engine)
{
    switch (engine) {
    case MixedFilamentColorEngine::FullSpectrumKSPairResidual:
        return "ks_pair_residual";
    case MixedFilamentColorEngine::FilamentMixer:
    default:
        return "filament_mixer";
    }
}

std::vector<unsigned int> MixedFilamentManager::decode_gradient_component_ids(const std::string& components, size_t num_physical)
{
    const size_t limit = num_physical == 0 ? kMaxPhysicalFilaments : std::min<size_t>(num_physical, kMaxPhysicalFilaments);
    return MixedFilamentInternal::decode_gradient_component_ids(components, limit);
}

std::string MixedFilamentManager::encode_gradient_component_ids(const std::vector<unsigned int>& component_ids)
{
    const bool extended = std::any_of(component_ids.begin(), component_ids.end(), [](unsigned int id) { return id > 9; });
    if (extended && component_ids.size() == 1 && component_ids.front() <= kMaxPhysicalFilaments)
        return "/" + std::to_string(component_ids.front());

    std::string encoded;
    std::unordered_set<unsigned int> seen;
    for (const unsigned int component_id : component_ids) {
        if (component_id == 0 || component_id > kMaxPhysicalFilaments || !seen.insert(component_id).second)
            continue;
        if (extended && !encoded.empty())
            encoded.push_back('/');
        if (extended)
            encoded += std::to_string(component_id);
        else
            encoded.push_back(char('0' + component_id));
    }
    return encoded;
}

std::vector<std::string> MixedFilamentManager::split_pattern_groups(const std::string& pattern)
{
    return split_manual_pattern_groups(pattern);
}

std::vector<std::string> MixedFilamentManager::split_pattern_group_to_tokens(const std::string& group, size_t num_physical)
{
    (void) num_physical;
    std::vector<std::string> tokens;
    tokens.reserve(group.size());
    for (const char c : group) {
        if (is_pattern_separator(c))
            continue;
        char token = 0;
        if (!decode_pattern_step(c, token))
            continue;
        tokens.emplace_back(1, token);
    }
    return tokens;
}

unsigned int MixedFilamentManager::physical_filament_from_token(const std::string&            token,
                                                                const MixedFilamentLegacyRow& row,
                                                                size_t                       num_physical)
{
    if (token.empty())
        return 0;
    char decoded = 0;
    if (token.size() != 1 || !decode_pattern_step(token.front(), decoded))
        return 0;

    const MixedFilamentLegacyPair pair{{row.component_a}, {row.component_b}};
    const unsigned int filament_id = physical_filament_from_legacy_pattern_token(decoded, pair);
    if (filament_id == 0)
        return 0;
    if (num_physical != 0 && filament_id > num_physical)
        return 0;
    return filament_id;
}

void MixedFilamentManager::auto_generate(const std::vector<std::string>& filament_colours)
{
    sync_mutable_legacy_cache_to_definitions(filament_colours.size());
    // Keep a copy of the old list so we can preserve user-modified ratios,
    // tombstones, and custom rows.
    std::vector<MixedFilamentDefinition> old = std::move(m_definitions);
    m_definitions.clear();
    invalidate_legacy_cache();

    const size_t n = filament_colours.size();

    std::vector<MixedFilamentDefinition> custom_definitions;
    custom_definitions.reserve(old.size());
    std::unordered_map<uint64_t, const MixedFilamentDefinition*> old_auto_definitions;
    old_auto_definitions.reserve(old.size());
    for (MixedFilamentDefinition& prev : old) {
        const MixedFilamentPrimaryPairView pair = prev.recipe.blend.primary_pair_or();
        const unsigned int component_a = pair.component_a.id;
        const unsigned int component_b = pair.component_b.id;
        if (prev.source.kind != MixedFilamentSourceKind::Custom) {
            old_auto_definitions.emplace(canonical_pair_key(component_a, component_b), &prev);
            continue;
        }
        if (component_a == 0 || component_b == 0 || component_a > n || component_b > n || component_a == component_b)
            continue;
        prev.identity.stable_id = normalize_stable_id(prev.identity.stable_id);
        custom_definitions.push_back(std::move(prev));
    }

    if (n < 2 || !auto_generate_enabled()) {
        for (MixedFilamentDefinition& definition : custom_definitions)
            m_definitions.push_back(std::move(definition));
        refresh_display_colors(filament_colours);
        return;
    }

    // Generate all C(N,2) pairwise combinations.
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            const unsigned int component_a = static_cast<unsigned int>(i + 1); // 1-based
            const unsigned int component_b = static_cast<unsigned int>(j + 1);

            MixedFilamentDefinition definition;
            definition.recipe.blend.components = {
                {{component_a}, 50},
                {{component_b}, 50}
            };
            definition.source.kind        = MixedFilamentSourceKind::AutoGenerated;
            definition.source.origin_auto = true;

            const auto it_prev = old_auto_definitions.find(canonical_pair_key(component_a, component_b));
            if (it_prev != old_auto_definitions.end()) {
                const MixedFilamentDefinition& prev = *it_prev->second;
                definition.visibility.tombstoned    = prev.visibility.tombstoned;
                definition.identity.stable_id       = prev.identity.stable_id;
            }
            definition.identity.stable_id = normalize_stable_id(definition.identity.stable_id);
            m_definitions.push_back(std::move(definition));
        }
    }

    for (MixedFilamentDefinition& definition : custom_definitions)
        m_definitions.push_back(std::move(definition));

    refresh_display_colors(filament_colours);
}

void MixedFilamentManager::remove_physical_filament(unsigned int deleted_filament_id)
{
    sync_mutable_legacy_cache_to_definitions(m_display_context.num_physical);
    if (deleted_filament_id == 0 || m_definitions.empty())
        return;

    std::vector<MixedFilamentDefinition> filtered;
    filtered.reserve(m_definitions.size());
    for (MixedFilamentDefinition definition : m_definitions) {
        if (definition_uses_physical_filament(definition, deleted_filament_id))
            continue;

        shift_definition_after_physical_deletion(definition, deleted_filament_id);
        filtered.emplace_back(std::move(definition));
    }

    m_definitions = std::move(filtered);
    invalidate_legacy_cache();
}

void MixedFilamentManager::add_custom_filament(unsigned int                    component_a,
                                               unsigned int                    component_b,
                                               int                             mix_b_percent,
                                               const std::vector<std::string>& filament_colours)
{
    MixedFilamentDefinition definition;
    const int pct_b = clamp_int(mix_b_percent, 0, 100);
    definition.recipe.blend.components = {
        {{component_a}, 100 - pct_b},
        {{component_b}, pct_b}
    };
    add_custom_filament_definition(std::move(definition), filament_colours);
}

bool MixedFilamentManager::add_custom_filament_definition(MixedFilamentDefinition         definition,
                                                          const std::vector<std::string>& filament_colours)
{
    sync_mutable_legacy_cache_to_definitions(filament_colours.size());
    const size_t n = filament_colours.size();
    if (n < 2)
        return false;

    definition.recipe.blend = normalized_manager_blend(std::move(definition.recipe.blend), n);

    const MixedFilamentPrimaryPairView pair = definition.recipe.blend.primary_pair_or();
    unsigned int component_a = pair.component_a.id;
    unsigned int component_b = pair.component_b.id;
    component_a              = std::max<unsigned int>(1, std::min<unsigned int>(component_a, unsigned(n)));
    component_b              = std::max<unsigned int>(1, std::min<unsigned int>(component_b, unsigned(n)));
    if (component_a == component_b)
        component_b = (component_a == 1) ? 2 : 1;

    definition.recipe.blend.components[0].filament.id = component_a;
    definition.recipe.blend.components[1].filament.id = component_b;
    definition.identity.stable_id                     = normalize_stable_id(definition.identity.stable_id);
    definition.visibility.tombstoned                   = false;
    definition.source.kind                            = MixedFilamentSourceKind::Custom;
    definition.source.origin_auto                     = false;
    m_definitions.push_back(std::move(definition));
    invalidate_legacy_cache();
    refresh_display_colors(filament_colours);
    return true;
}

void MixedFilamentManager::add_batch_custom_filaments(const std::vector<MixedFilamentBatchEntry>& entries,
                                                       const std::vector<std::string>&             filament_colours,
                                                       std::vector<unsigned int>*                  out_assigned_ids)
{
    sync_mutable_legacy_cache_to_definitions(filament_colours.size());
    if (out_assigned_ids != nullptr) {
        out_assigned_ids->clear();
        out_assigned_ids->reserve(entries.size());
    }

    const size_t num_physical = filament_colours.size();
    if (num_physical < 2) {
        if (out_assigned_ids != nullptr)
            out_assigned_ids->assign(entries.size(), 0u);
        return;
    }
    if (entries.empty())
        return;

    size_t current_total = total_filaments(num_physical);
    for (const MixedFilamentBatchEntry& entry : entries) {
        if (current_total >= MAXIMUM_FILAMENT_NUMBER) {
            if (out_assigned_ids != nullptr)
                out_assigned_ids->push_back(0u);
            continue;
        }

        unsigned int component_a = std::clamp(entry.component_a, 1u, static_cast<unsigned int>(num_physical));
        unsigned int component_b = std::clamp(entry.component_b, 1u, static_cast<unsigned int>(num_physical));
        if (component_a == component_b)
            component_b = component_a == 1 ? 2u : 1u;

        MixedFilamentLegacyRow row;
        row.component_a                 = component_a;
        row.component_b                 = component_b;
        row.stable_id                   = allocate_stable_id();
        row.mix_b_percent               = std::clamp(entry.mix_b_percent, 0, 100);
        row.ratio_a                     = 1;
        row.ratio_b                     = 1;
        row.manual_pattern              = normalize_manual_pattern(entry.manual_pattern);
        row.gradient_component_ids      = normalize_gradient_component_ids(entry.gradient_component_ids);
        row.gradient_component_weights  = entry.gradient_component_weights;
        row.distribution_mode           = entry.distribution_mode;
        row.gradient_enabled            = entry.gradient_enabled;
        row.gradient_start              = entry.gradient_start;
        row.gradient_end                = entry.gradient_end;
        row.display_color               = entry.display_color;
        row.enabled                     = true;
        row.deleted                     = false;
        row.custom                      = true;
        row.origin_auto                 = false;
        row.ui_mode                     = 2;

        if (row.gradient_enabled && std::abs(row.gradient_start - row.gradient_end) < MixedFilament::k_min_gradient_difference)
            row.gradient_enabled = false;
        if (mixed_filament_references_exceed_physical(row.gradient_component_ids, row.manual_pattern, num_physical)) {
            if (out_assigned_ids != nullptr)
                out_assigned_ids->push_back(0u);
            continue;
        }

        normalize_legacy_row(row);
        m_definitions.emplace_back(mixed_filament_definition_from_legacy_row(row, num_physical));
        if (out_assigned_ids != nullptr)
            out_assigned_ids->push_back(static_cast<unsigned int>(current_total + 1));
        ++current_total;
    }

    invalidate_legacy_cache();
    refresh_display_colors(filament_colours);
}

std::vector<unsigned int> MixedFilamentManager::build_mixed_deletion_painting_remap(
    size_t num_physical, size_t t2_total_filaments, const std::vector<unsigned int>& deleted_t2_vids)
{
    std::vector<unsigned int> remap(t2_total_filaments + 1, 0u);
    if (deleted_t2_vids.empty()) {
        for (size_t id = 1; id <= t2_total_filaments; ++id)
            remap[id] = static_cast<unsigned int>(id);
        return remap;
    }

    std::vector<unsigned int> sorted_deleted = deleted_t2_vids;
    std::sort(sorted_deleted.begin(), sorted_deleted.end());
    sorted_deleted.erase(std::unique(sorted_deleted.begin(), sorted_deleted.end()), sorted_deleted.end());

    for (size_t old_id = 1; old_id <= t2_total_filaments; ++old_id) {
        if (old_id <= num_physical) {
            remap[old_id] = static_cast<unsigned int>(old_id);
            continue;
        }

        const auto   it            = std::lower_bound(sorted_deleted.begin(), sorted_deleted.end(), static_cast<unsigned int>(old_id));
        const size_t deleted_below = static_cast<size_t>(it - sorted_deleted.begin());
        if (it != sorted_deleted.end() && *it == old_id)
            remap[old_id] = 0u;
        else if (deleted_below < old_id)
            remap[old_id] = static_cast<unsigned int>(old_id - deleted_below);
    }
    return remap;
}

void MixedFilamentManager::clear_custom_entries()
{
    m_definitions.erase(std::remove_if(m_definitions.begin(),
                                       m_definitions.end(),
                                       [](const MixedFilamentDefinition& definition) {
                                           return definition.source.kind == MixedFilamentSourceKind::Custom;
                                       }),
                        m_definitions.end());
    invalidate_legacy_cache();
}

std::string MixedFilamentManager::normalize_manual_pattern(const std::string& pattern)
{
    std::string normalized;
    normalized.reserve(pattern.size());
    bool current_group_has_steps = false;
    for (char c : pattern) {
        char step = '\0';
        if (decode_pattern_step(c, step)) {
            normalized.push_back(step);
            current_group_has_steps = true;
            continue;
        }
        if (c == ',') {
            if (!current_group_has_steps)
                return std::string();
            normalized.push_back(',');
            current_group_has_steps = false;
            continue;
        }
        if (is_pattern_separator(c))
            continue;
        // Unknown token => invalid pattern.
        return std::string();
    }
    if (!normalized.empty() && normalized.back() == ',')
        return std::string();
    return normalized;
}

int MixedFilamentManager::mix_percent_from_manual_pattern(const std::string& pattern)
{
    return mix_percent_from_normalized_pattern(normalize_manual_pattern(pattern));
}

void MixedFilamentManager::apply_gradient_settings(int   gradient_mode,
                                                   float nominal_layer_height,
                                                   float min_sublayer_height,
                                                   bool  advanced_dithering)
{
    m_gradient_mode        = (gradient_mode != 0) ? 1 : 0;
    m_nominal_layer_height = std::max(0.01f, nominal_layer_height);
    m_min_sublayer_height  = std::max(0.01f, min_sublayer_height);
    m_advanced_dithering   = advanced_dithering;

    for (MixedFilamentDefinition& definition : m_definitions) {
        if (definition.source.kind != MixedFilamentSourceKind::Custom) {
            definition.behavior.layer_cadence.component_a_layers = 1;
            definition.behavior.layer_cadence.component_b_layers = 1;
            continue;
        }
        const std::pair<int, int> ratios =
            gradient_ratios_from_mix(definition.recipe.blend.primary_pair_or().component_b_percent,
                                     m_gradient_mode,
                                     m_nominal_layer_height,
                                     m_min_sublayer_height);
        definition.behavior.layer_cadence.component_a_layers = ratios.first;
        definition.behavior.layer_cadence.component_b_layers = ratios.second;
    }
    invalidate_legacy_cache();
}

std::string MixedFilamentManager::serialize_custom_entries()
{
    sync_mutable_legacy_cache_to_definitions(m_display_context.num_physical);

    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    bool               first = true;
    for (MixedFilamentDefinition& definition : m_definitions) {
        if (!first)
            ss << ';';
        first = false;
        definition.identity.stable_id        = normalize_stable_id(definition.identity.stable_id);
        MixedFilamentLegacyRow mf                     = legacy_row_from_definition_for_manager(definition);
        const std::string normalized_ids     = normalize_gradient_component_ids(mf.gradient_component_ids);
        const size_t normalized_id_count = decode_gradient_component_ids(normalized_ids, 0).size();
        const std::string normalized_weights = normalize_gradient_component_weights(mf.gradient_component_weights, normalized_id_count);
        const size_t gradient_component_count = decode_gradient_component_ids(normalized_ids, m_display_context.num_physical).size();
        const size_t expected_gradient_stops = gradient_component_count >= 3 ? 2 * gradient_component_count - 1 :
            (mf.gradient_enabled ? size_t(3) : size_t(0));
        const std::string normalized_positions = normalize_gradient_stop_positions(mf.gradient_stop_positions, expected_gradient_stops);
        ss << mf.component_a << ',' << mf.component_b << ',' << (mf.deleted ? 0 : 1) << ',' << (mf.custom ? 1 : 0) << ','
            << clamp_int(mf.mix_b_percent, 0, 100) << ',' << 0 << ',' << 'g' << normalized_ids << ',' << 'w' << normalized_weights << ','
            << 'm' << clamp_int(mf.distribution_mode, int(MixedFilamentLegacyRow::LayerCycle), int(MixedFilamentLegacyRow::Simple)) << ',' << 'z'
            << std::max(0, mf.local_z_max_sublayers) << ',' << "xa" << format_surface_offset_token(mf.component_a_surface_offset) << ','
            << "xb" << format_surface_offset_token(mf.component_b_surface_offset);
        if (!mf.component_surface_offsets.empty())
            ss << ",xv" << mf.component_surface_offsets;
        if (mf.perimeter_modulation)
            ss << ",xp1";
        ss << ',' << 'd' << (mf.deleted ? 1 : 0) << ',' << 'o' << (mf.origin_auto ? 1 : 0) << ',' << 'u' << mf.stable_id;
        if (!normalized_positions.empty())
            ss << ",p" << normalized_positions;
        if (mf.ui_mode >= 0)
            ss << ",cm" << mf.ui_mode;
        if (mf.gradient_enabled)
            ss << ",r1/" << format_invariant_decimal(mf.gradient_start, 4) << '/'
               << format_invariant_decimal(mf.gradient_end, 4);
        const std::string normalized_pattern = normalize_manual_pattern(mf.manual_pattern);
        if (!normalized_pattern.empty())
            ss << ',' << normalized_pattern;
    }
    invalidate_legacy_cache();
    return ss.str();
}

void MixedFilamentManager::load_custom_entries(const std::string& serialized, const std::vector<std::string>& filament_colours)
{
    const size_t n = filament_colours.size();
    if (serialized.empty() || n < 2) {
        BOOST_LOG_TRIVIAL(debug) << "MixedFilamentManager::load_custom_entries skipped"
                                 << ", serialized_empty=" << (serialized.empty() ? 1 : 0) << ", physical_count=" << n;
        return;
    }

    size_t parsed_rows   = 0;
    size_t loaded_rows   = 0;
    size_t updated_auto  = 0;
    size_t appended_auto = 0;
    size_t skipped_rows  = 0;

    const std::vector<MixedFilamentLegacyRow>& current_rows = mixed_filament_legacy_rows();
    std::vector<const MixedFilamentLegacyRow*> auto_rows_in_order;
    auto_rows_in_order.reserve(current_rows.size());
    std::unordered_map<uint64_t, const MixedFilamentLegacyRow*> auto_rows_by_pair;
    auto_rows_by_pair.reserve(current_rows.size());
    for (const MixedFilamentLegacyRow& mf : current_rows) {
        if (!mf.custom) {
            auto_rows_in_order.push_back(&mf);
            auto_rows_by_pair.emplace(canonical_pair_key(mf.component_a, mf.component_b), &mf);
        }
    }

    std::vector<MixedFilamentLegacyRow> rebuilt;
    rebuilt.reserve(current_rows.size() + 8);
    std::unordered_set<uint64_t> consumed_auto_pairs;
    consumed_auto_pairs.reserve(auto_rows_by_pair.size());
    std::unordered_set<uint64_t> used_stable_ids;
    used_stable_ids.reserve(current_rows.size() + 8);
    auto dedupe_stable_id = [this, &used_stable_ids](uint64_t stable_id) {
        stable_id = normalize_stable_id(stable_id);
        if (used_stable_ids.insert(stable_id).second)
            return stable_id;
        uint64_t replacement = allocate_stable_id();
        used_stable_ids.insert(replacement);
        return replacement;
    };

    std::stringstream all(serialized);
    std::string       row;
    while (std::getline(all, row, ';')) {
        if (row.empty())
            continue;
        ++parsed_rows;
        unsigned int a           = 0;
        unsigned int b           = 0;
        uint64_t     stable_id   = 0;
        bool         enabled     = true;
        bool         custom      = true;
        bool         origin_auto = false;
        int          mix         = 50;
        std::string  gradient_component_ids;
        std::string  gradient_component_weights;
        std::string  gradient_stop_positions;
        std::string  manual_pattern;
        int          distribution_mode          = int(MixedFilamentLegacyRow::Simple);
        int          local_z_max_sublayers      = 0;
        float        component_a_surface_offset = 0.f;
        float        component_b_surface_offset = 0.f;
        std::string  component_surface_offsets;
        bool         perimeter_modulation        = false;
        bool         deleted                    = false;
        bool         gradient_enabled           = false;
        float        gradient_start             = MixedFilamentLegacyRow::k_default_gradient_dominant;
        float        gradient_end               = MixedFilamentLegacyRow::k_default_gradient_minority;
        int          ui_mode                    = -1;
        if (!parse_row_definition(row, a, b, stable_id, enabled, custom, origin_auto, mix, gradient_component_ids,
                                   gradient_component_weights, gradient_stop_positions, manual_pattern, distribution_mode, local_z_max_sublayers,
                                   component_a_surface_offset, component_b_surface_offset, component_surface_offsets, perimeter_modulation, deleted, gradient_enabled,
                                   gradient_start, gradient_end, ui_mode)) {
            ++skipped_rows;
            BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries invalid row format: " << row;
            continue;
        }
        if (a == 0 || b == 0 || a > n || b > n || a == b) {
            ++skipped_rows;
            BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries row rejected"
                                       << ", row=" << row << ", a=" << a << ", b=" << b << ", physical_count=" << n;
            continue;
        }

        if (!custom) {
            const uint64_t key = canonical_pair_key(a, b);
            if (consumed_auto_pairs.count(key) != 0) {
                ++skipped_rows;
                BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries duplicate auto row"
                                           << ", row=" << row << ", a=" << std::min(a, b) << ", b=" << std::max(a, b);
                continue;
            }

            auto it_auto = auto_rows_by_pair.find(key);
            if (it_auto == auto_rows_by_pair.end()) {
                ++skipped_rows;
                BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries auto row missing after regenerate"
                                           << ", row=" << row << ", a=" << std::min(a, b) << ", b=" << std::max(a, b);
                continue;
            }

            MixedFilamentLegacyRow mf              = *it_auto->second;
            mf.component_a                = std::min(a, b);
            mf.component_b                = std::max(a, b);
            mf.stable_id                  = dedupe_stable_id(stable_id != 0 ? stable_id : mf.stable_id);
            (void) enabled;
            mf.enabled                    = !deleted;
            mf.gradient_component_ids     = normalize_gradient_component_ids(gradient_component_ids);
            mf.gradient_component_weights = normalize_gradient_component_weights(
                gradient_component_weights, decode_gradient_component_ids(mf.gradient_component_ids, 0).size());
            mf.manual_pattern             = normalize_manual_pattern(manual_pattern);
            mf.distribution_mode          = clamp_int(distribution_mode, int(MixedFilamentLegacyRow::LayerCycle), int(MixedFilamentLegacyRow::Simple));
            mf.local_z_max_sublayers      = std::max(0, local_z_max_sublayers);
            mf.component_a_surface_offset = clamp_surface_offset(component_a_surface_offset);
            mf.component_b_surface_offset = clamp_surface_offset(component_b_surface_offset);
            mf.component_surface_offsets  = component_surface_offsets;
            mf.perimeter_modulation       = perimeter_modulation;
            mf.gradient_enabled           = gradient_enabled;
            mf.gradient_start             = gradient_start;
            mf.gradient_end               = gradient_end;
            {
                const size_t gradient_component_count = decode_gradient_component_ids(mf.gradient_component_ids, n).size();
                const size_t expected_stops = gradient_component_count >= 3 ? 2 * gradient_component_count - 1 :
                    (mf.gradient_enabled ? size_t(3) : size_t(0));
                mf.gradient_stop_positions = normalize_gradient_stop_positions(gradient_stop_positions, expected_stops);
            }
            mf.ui_mode                    = ui_mode;
            mf.mix_b_percent              = mf.manual_pattern.empty() ? mix : mix_percent_from_normalized_pattern(mf.manual_pattern);
            mf.deleted                    = deleted;
            mf.enabled                    = !mf.deleted;
            mf.custom      = false;
            mf.origin_auto = true;
            normalize_legacy_row(mf);

            rebuilt.push_back(std::move(mf));
            consumed_auto_pairs.insert(key);
            ++updated_auto;
            continue;
        }

        MixedFilamentLegacyRow mf;
        mf.component_a                = a;
        mf.component_b                = b;
        mf.stable_id                  = dedupe_stable_id(stable_id);
        mf.mix_b_percent              = mix;
        mf.ratio_a                    = 1;
        mf.ratio_b                    = 1;
        mf.gradient_component_ids     = normalize_gradient_component_ids(gradient_component_ids);
        mf.gradient_component_weights = normalize_gradient_component_weights(
            gradient_component_weights, decode_gradient_component_ids(mf.gradient_component_ids, 0).size());
        mf.manual_pattern             = normalize_manual_pattern(manual_pattern);
        mf.distribution_mode          = clamp_int(distribution_mode, int(MixedFilamentLegacyRow::LayerCycle), int(MixedFilamentLegacyRow::Simple));
        mf.local_z_max_sublayers      = std::max(0, local_z_max_sublayers);
        mf.component_a_surface_offset = clamp_surface_offset(component_a_surface_offset);
        mf.component_b_surface_offset = clamp_surface_offset(component_b_surface_offset);
        mf.component_surface_offsets  = component_surface_offsets;
        mf.perimeter_modulation       = perimeter_modulation;
        mf.gradient_enabled           = gradient_enabled;
        mf.gradient_start             = gradient_start;
        mf.gradient_end               = gradient_end;
        {
            const size_t gradient_component_count = decode_gradient_component_ids(mf.gradient_component_ids, n).size();
            const size_t expected_stops = gradient_component_count >= 3 ? 2 * gradient_component_count - 1 :
                (mf.gradient_enabled ? size_t(3) : size_t(0));
            mf.gradient_stop_positions = normalize_gradient_stop_positions(gradient_stop_positions, expected_stops);
        }
        mf.ui_mode                    = ui_mode;
        if (!mf.manual_pattern.empty())
            mf.mix_b_percent = mix_percent_from_normalized_pattern(mf.manual_pattern);
        mf.deleted = deleted;
        (void) enabled;
        mf.enabled = !mf.deleted;
        mf.custom      = custom;
        mf.origin_auto = origin_auto;
        normalize_legacy_row(mf);
        rebuilt.push_back(std::move(mf));
        ++loaded_rows;
    }

    // Keep any newly generated auto rows that were not present in serialized
    // definitions and append them at the end to preserve existing virtual IDs.
    for (const MixedFilamentLegacyRow* auto_mf_ptr : auto_rows_in_order) {
        if (auto_mf_ptr == nullptr)
            continue;
        const uint64_t key = canonical_pair_key(auto_mf_ptr->component_a, auto_mf_ptr->component_b);
        if (consumed_auto_pairs.count(key) != 0)
            continue;
        MixedFilamentLegacyRow      mf = *auto_mf_ptr;
        const unsigned int lo = std::min(mf.component_a, mf.component_b);
        const unsigned int hi = std::max(mf.component_a, mf.component_b);
        mf.component_a        = lo;
        mf.component_b        = hi;
        mf.stable_id          = dedupe_stable_id(mf.stable_id);
        mf.custom             = false;
        mf.origin_auto        = true;
        rebuilt.push_back(std::move(mf));
        ++appended_auto;
    }

    m_definitions.clear();
    m_definitions.reserve(rebuilt.size());
    for (const MixedFilamentLegacyRow& mf : rebuilt)
        m_definitions.emplace_back(mixed_filament_definition_from_legacy_row(mf, n));
    invalidate_legacy_cache();
    refresh_display_colors(filament_colours);
    BOOST_LOG_TRIVIAL(info) << "MixedFilamentManager::load_custom_entries"
                            << ", physical_count=" << n << ", parsed_rows=" << parsed_rows << ", loaded_rows=" << loaded_rows
                            << ", updated_auto_rows=" << updated_auto << ", appended_auto_rows=" << appended_auto
                            << ", skipped_rows=" << skipped_rows << ", mixed_total=" << m_definitions.size();
}

int MixedFilamentManager::mixed_index_from_filament_id(unsigned int filament_id, size_t num_physical) const
{
    if (filament_id <= num_physical)
        return -1;

    const size_t visible_virtual_idx = size_t(filament_id - num_physical - 1);
    size_t       visible_seen        = 0;
    for (size_t i = 0; i < m_definitions.size(); ++i) {
        if (m_definitions[i].visibility.tombstoned)
            continue;
        if (visible_seen == visible_virtual_idx)
            return int(i);
        ++visible_seen;
    }
    return -1;
}

std::optional<MixedFilamentDefinition> MixedFilamentManager::mixed_filament_definition_from_id(unsigned int filament_id,
                                                                                               size_t       num_physical) const
{
    const int idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (idx < 0 || size_t(idx) >= m_definitions.size())
        return std::nullopt;
    return m_definitions[size_t(idx)];
}

std::optional<unsigned int> MixedFilamentManager::filament_id_from_stable_id(MixedFilamentStableId stable_id,
                                                                              size_t                num_physical) const
{
    if (stable_id == 0)
        return std::nullopt;
    unsigned int filament_id = unsigned(num_physical + 1);
    for (const MixedFilamentDefinition &definition : m_definitions) {
        if (definition.visibility.tombstoned)
            continue;
        if (definition.identity.stable_id == stable_id)
            return filament_id;
        ++filament_id;
    }
    return std::nullopt;
}

std::vector<MixedFilamentDefinition> MixedFilamentManager::mixed_filament_definitions(size_t num_physical) const
{
    if (m_legacy_cache_mutable_borrowed && !m_legacy_cache_dirty) {
        std::vector<MixedFilamentDefinition> definitions;
        definitions.reserve(m_legacy_cache.size());
        for (const MixedFilamentLegacyRow& row : m_legacy_cache)
            definitions.emplace_back(mixed_filament_definition_from_legacy_row(row, num_physical));
        return definitions;
    }
    return m_definitions;
}

std::vector<size_t> MixedFilamentManager::mixed_filaments_using_physical(unsigned int physical_filament_id) const
{
    std::vector<size_t> indices;
    if (physical_filament_id == 0)
        return indices;

    for (size_t i = 0; i < m_definitions.size(); ++i) {
        const MixedFilamentDefinition& definition = m_definitions[i];
        if (definition.visibility.tombstoned)
            continue;
        if (definition_uses_physical_filament(definition, physical_filament_id))
            indices.emplace_back(i);
    }

    return indices;
}

void MixedFilamentManager::expand_virtual_extruder_ids(std::vector<int>& filament_ids, size_t num_physical) const
{
    std::vector<int> expanded;
    for (const int filament_id : filament_ids) {
        if (filament_id <= 0)
            continue;

        const int mixed_idx = mixed_index_from_filament_id(static_cast<unsigned int>(filament_id), num_physical);
        if (mixed_idx < 0 || size_t(mixed_idx) >= m_definitions.size())
            continue;

        const MixedFilamentDefinition& definition = m_definitions[size_t(mixed_idx)];
        std::vector<unsigned int> component_ids;
        if (definition.recipe.manual_pattern)
            component_ids = mixed_filament_manual_pattern_sequence(definition, num_physical);
        else
            component_ids = definition.recipe.blend.component_ids(num_physical);

        for (const unsigned int component_id : component_ids)
            if (component_id >= 1 && component_id <= num_physical)
                expanded.emplace_back(static_cast<int>(component_id));
    }

    filament_ids.insert(filament_ids.end(), expanded.begin(), expanded.end());
}

bool MixedFilamentManager::set_mixed_filament_definition(size_t                          index,
                                                         const MixedFilamentDefinition&  definition,
                                                         const std::vector<std::string>& filament_colours)
{
    if (index >= m_definitions.size())
        return false;

    MixedFilamentDefinition normalized = definition;
    normalized.recipe.blend            = normalized_manager_blend(std::move(normalized.recipe.blend), filament_colours.size());
    normalized.identity.stable_id      = normalize_stable_id(normalized.identity.stable_id);
    m_definitions[index]               = std::move(normalized);
    invalidate_legacy_cache();
    if (!filament_colours.empty())
        refresh_display_colors(filament_colours);
    return true;
}

void MixedFilamentManager::set_mixed_filament_definitions(std::vector<MixedFilamentDefinition> definitions,
                                                          const std::vector<std::string>&      filament_colours)
{
    m_definitions = std::move(definitions);
    for (MixedFilamentDefinition& definition : m_definitions) {
        definition.recipe.blend       = normalized_manager_blend(std::move(definition.recipe.blend), filament_colours.size());
        definition.identity.stable_id = normalize_stable_id(definition.identity.stable_id);
    }
    invalidate_legacy_cache();
    if (!filament_colours.empty())
        refresh_display_colors(filament_colours);
}

bool MixedFilamentManager::set_mixed_filament_legacy_row(size_t                          index,
                                                         const MixedFilamentLegacyRow&            row,
                                                         size_t                          num_physical,
                                                         const std::vector<std::string>& filament_colours)
{
    return set_mixed_filament_definition(index, mixed_filament_definition_from_legacy_row(row, num_physical), filament_colours);
}

void MixedFilamentManager::set_mixed_filament_legacy_rows(const std::vector<MixedFilamentLegacyRow>&      rows,
                                                          size_t                                 num_physical,
                                                          const std::vector<std::string>&        filament_colours)
{
    std::vector<MixedFilamentDefinition> definitions;
    definitions.reserve(rows.size());
    for (const MixedFilamentLegacyRow& row : rows)
        definitions.emplace_back(mixed_filament_definition_from_legacy_row(row, num_physical));
    set_mixed_filament_definitions(std::move(definitions), filament_colours);
}

void MixedFilamentManager::refresh_display_colors(const std::vector<std::string>& filament_colours)
{
    sync_mutable_legacy_cache_to_definitions(filament_colours.size());
    MixedFilamentDisplayContext context = m_display_context;
    context.num_physical                = filament_colours.size();
    context.physical_colors             = filament_colours;
    if (context.preview_settings.wall_loops == 0)
        context.preview_settings.wall_loops = 1;
    if (context.nozzle_diameters.size() < context.num_physical)
        context.nozzle_diameters.resize(context.num_physical, 0.4);
    if (context.physical_tds.size() < context.num_physical)
        context.physical_tds.resize(context.num_physical, 0.0);

    for (MixedFilamentDefinition& definition : m_definitions) {
        definition.presentation.display_color = compute_mixed_filament_display_color(definition, context);
    }
    invalidate_legacy_cache();
}

size_t MixedFilamentManager::visible_count() const
{
    size_t count = 0;
    if (m_legacy_cache_mutable_borrowed && !m_legacy_cache_dirty) {
        for (const MixedFilamentLegacyRow& row : m_legacy_cache)
            if (!row.deleted)
                ++count;
        return count;
    }
    for (const MixedFilamentDefinition& definition : m_definitions)
        if (!definition.visibility.tombstoned)
            ++count;
    return count;
}

std::vector<std::string> MixedFilamentManager::display_colors() const
{
    std::vector<std::string> colors;
    if (m_legacy_cache_mutable_borrowed && !m_legacy_cache_dirty) {
        for (const MixedFilamentLegacyRow& row : m_legacy_cache)
            if (!row.deleted)
                colors.push_back(row.display_color);
        return colors;
    }
    for (const MixedFilamentDefinition& definition : m_definitions)
        if (!definition.visibility.tombstoned)
            colors.push_back(definition.presentation.display_color);
    return colors;
}

const std::vector<MixedFilamentLegacyRow>& MixedFilamentManager::mixed_filament_legacy_rows() const
{
    rebuild_legacy_cache();
    return m_legacy_cache;
}

std::vector<MixedFilamentLegacyRow>& MixedFilamentManager::mixed_filaments()
{
    rebuild_legacy_cache();
    m_legacy_cache_mutable_borrowed = true;
    return m_legacy_cache;
}

const std::vector<MixedFilamentLegacyRow>& MixedFilamentManager::mixed_filaments() const
{
    return mixed_filament_legacy_rows();
}

MixedFilamentLegacyRow* MixedFilamentManager::mixed_filament_from_id(unsigned int filament_id, size_t num_physical)
{
    const int idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (idx < 0)
        return nullptr;
    std::vector<MixedFilamentLegacyRow>& rows = mixed_filaments();
    return size_t(idx) < rows.size() ? &rows[size_t(idx)] : nullptr;
}

const MixedFilamentLegacyRow* MixedFilamentManager::mixed_filament_from_id(unsigned int filament_id, size_t num_physical) const
{
    const int idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (idx < 0)
        return nullptr;
    const std::vector<MixedFilamentLegacyRow>& rows = mixed_filament_legacy_rows();
    return size_t(idx) < rows.size() ? &rows[size_t(idx)] : nullptr;
}

size_t MixedFilamentManager::mixed_filament_count() const
{
    if (m_legacy_cache_mutable_borrowed && !m_legacy_cache_dirty)
        return m_legacy_cache.size();
    return m_definitions.size();
}

void MixedFilamentManager::invalidate_legacy_cache() const
{
    m_legacy_cache_dirty = true;
    if (m_legacy_cache_mutable_borrowed)
        rebuild_legacy_cache();
}

void MixedFilamentManager::rebuild_legacy_cache() const
{
    if (!m_legacy_cache_dirty)
        return;

    m_legacy_cache.clear();
    m_legacy_cache.reserve(m_definitions.size());
    for (const MixedFilamentDefinition& definition : m_definitions)
        m_legacy_cache.emplace_back(legacy_row_from_definition_for_manager(definition));
    m_legacy_cache_dirty = false;
}

void MixedFilamentManager::sync_mutable_legacy_cache_to_definitions(size_t num_physical)
{
    if (!m_legacy_cache_mutable_borrowed)
        return;
    if (m_legacy_cache_dirty) {
        m_legacy_cache_mutable_borrowed = false;
        return;
    }

    std::vector<MixedFilamentDefinition> definitions;
    definitions.reserve(m_legacy_cache.size());
    for (const MixedFilamentLegacyRow& row : m_legacy_cache) {
        MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, num_physical);
        definition.recipe.blend            = normalized_manager_blend(std::move(definition.recipe.blend), num_physical);
        definition.identity.stable_id      = normalize_stable_id(definition.identity.stable_id);
        definitions.emplace_back(std::move(definition));
    }

    m_definitions        = std::move(definitions);
    m_legacy_cache_dirty = true;
    rebuild_legacy_cache();
}

void MixedFilamentManager::set_display_context(const MixedFilamentDisplayContext& context)
{
    m_display_context = context;
    if (m_display_context.num_physical == 0 || m_display_context.num_physical < m_display_context.physical_colors.size())
        m_display_context.num_physical = m_display_context.physical_colors.size();
    if (m_display_context.preview_settings.wall_loops == 0)
        m_display_context.preview_settings.wall_loops = 1;
    if (m_display_context.nozzle_diameters.size() < m_display_context.num_physical)
        m_display_context.nozzle_diameters.resize(m_display_context.num_physical, 0.4);
    if (m_display_context.physical_tds.size() < m_display_context.num_physical)
        m_display_context.physical_tds.resize(m_display_context.num_physical, 0.0);
    if (!m_display_context.physical_colors.empty())
        refresh_display_colors(m_display_context.physical_colors);
}

RedundantFilamentSet compute_redundant_filaments(size_t                            num_physical,
                                                 const std::vector<unsigned int>&   kept_physical_ids,
                                                 const std::vector<unsigned int>&   kept_mixed_ids,
                                                 const std::vector<MixedFilament>& mixed_filaments)
{
    if (num_physical == 0)
        return {};

    RedundantFilamentSet result;
    std::vector<bool>    physical_kept(num_physical + 1, false);
    for (unsigned int id : kept_physical_ids)
        if (id >= 1 && id <= num_physical)
            physical_kept[id] = true;

    size_t kept_count = 0;
    for (size_t id = 1; id <= num_physical; ++id)
        if (physical_kept[id])
            ++kept_count;
    if (kept_count == 0) {
        physical_kept[1] = true;
        kept_count       = 1;
    }
    result.new_num_physical = kept_count;

    for (size_t id = num_physical; id >= 1; --id)
        if (!physical_kept[id])
            result.redundant_physical.push_back(static_cast<unsigned int>(id));

    std::unordered_set<unsigned int> kept_mixed;
    for (unsigned int id : kept_mixed_ids)
        if (id > num_physical)
            kept_mixed.insert(id);

    unsigned int virtual_id = static_cast<unsigned int>(num_physical + 1);
    for (const MixedFilament& mixed : mixed_filaments) {
        if (!mixed.enabled || mixed.deleted)
            continue;

        const bool retained = kept_mixed.count(virtual_id) != 0;
        bool       cascade  = false;
        if (retained) {
            const std::string pattern = MixedFilamentManager::normalize_manual_pattern(mixed.manual_pattern);
            if (!pattern.empty()) {
                for (const std::string& group : MixedFilamentManager::split_pattern_groups(pattern)) {
                    for (const std::string& token : MixedFilamentManager::split_pattern_group_to_tokens(group, 0)) {
                        const unsigned int physical_id = MixedFilamentManager::physical_filament_from_token(token, mixed, num_physical);
                        if (physical_id < 1 || physical_id > num_physical || !physical_kept[physical_id]) {
                            cascade = true;
                            break;
                        }
                    }
                    if (cascade)
                        break;
                }
            } else if (mixed.component_a < 1 || mixed.component_a > num_physical || !physical_kept[mixed.component_a]
                       || mixed.component_b < 1 || mixed.component_b > num_physical || !physical_kept[mixed.component_b]) {
                cascade = true;
            } else {
                for (unsigned int physical_id :
                     MixedFilamentManager::decode_gradient_component_ids(mixed.gradient_component_ids, 0)) {
                    if (physical_id < 1 || physical_id > num_physical || !physical_kept[physical_id]) {
                        cascade = true;
                        break;
                    }
                }
            }
        }

        if (!retained || cascade) {
            result.redundant_mixed.push_back(virtual_id);
            if (cascade)
                ++result.cascade_mixed_count;
        }
        ++virtual_id;
    }

    return result;
}

} // namespace Slic3r
