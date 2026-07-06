#include "FilamentHotBedNozzleRules.hpp"
#include "Config.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>

namespace Slic3r {

namespace pt = boost::property_tree;
namespace {

// Prefer user data dir (installed system profile) so rules can be patched without reinstalling the app.
static std::string filament_hot_bed_nozzles_json_path()
{
    namespace fs = boost::filesystem;
    const fs::path user_path = (fs::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR / PresetBundle::SM_BUNDLE / "filament" /
                                "filament_hot_bed_nozzles.json")
                                 .make_preferred();
    if (fs::exists(user_path))
        return user_path.string();
    return (fs::path(Slic3r::resources_dir()) / "profiles" / PresetBundle::SM_BUNDLE / "filament" / "filament_hot_bed_nozzles.json")
        .make_preferred()
        .string();
}

std::string to_upper_ascii(std::string s)
{
    for (char& c : s) {
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    }
    return s;
}

bool contains_token_ci(const std::string& text, const std::string& token)
{
    if (token.empty())
        return false;
    const std::string up_text  = to_upper_ascii(text);
    const std::string up_token = to_upper_ascii(token);
    return up_text.find(up_token) != std::string::npos;
}

bool match_any_rule_token_ci(const std::unordered_set<std::string>& tokens, const std::string& value)
{
    for (const std::string& token : tokens) {
        if (contains_token_ci(value, token))
            return true;
    }
    return false;
}

// Presets use optional " @Vendor / machine" suffixes (e.g. "Generic PLA @U1 0.2 nozzle"). Rules list the
// canonical name only; compare to the base name so "Generic PLA" does not match "Generic PLA Silk".
static std::string filament_preset_base_name_for_nozzle_rule(std::string preset_name)
{
    boost::algorithm::trim(preset_name);
    static constexpr const char* k_suffix_sep = " @";
    const std::string::size_type pos           = preset_name.find(k_suffix_sep);
    if (pos != std::string::npos) {
        preset_name.resize(pos);
        boost::algorithm::trim(preset_name);
    }
    return preset_name;
}

static bool nozzle_warning_rule_matches_filament_preset_ci(const std::string& token_raw, const std::string& filament_preset_name)
{
    std::string token = boost::algorithm::trim_copy(token_raw);
    if (token.empty())
        return false;
    const std::string base = filament_preset_base_name_for_nozzle_rule(filament_preset_name);
    return to_upper_ascii(base) == to_upper_ascii(token);
}

// Resolve filament_settings_id entry: may store preset display name or cloud setting_id / base_id.
static std::string resolve_filament_preset_full_name(const std::string& stored, const PresetCollection* filaments)
{
    if (stored.empty() || filaments == nullptr)
        return stored;
    if (const Preset* by_name = filaments->find_preset(stored, false))
        return by_name->name;
    for (const Preset& preset : filaments->get_presets()) {
        if (preset.type != Preset::TYPE_FILAMENT)
            continue;
        if (!preset.setting_id.empty() && preset.setting_id == stored)
            return preset.name;
        if (!preset.base_id.empty() && preset.base_id == stored)
            return preset.name;
        if (!preset.filament_id.empty() && preset.filament_id == stored)
            return preset.name;
    }
    return stored;
}

bool is_pla_type(const std::string& filament_type)
{
    return contains_token_ci(filament_type, "PLA");
}

bool is_tpu_type(const std::string& filament_type)
{
    return contains_token_ci(filament_type, "TPU");
}

static bool ptree_key_is_array_index(const std::string& key)
{
    return !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c); });
}

// JSON array elements are usually keyed "0","1",…; some ptree representations use an empty key per element.
static bool ptree_key_is_array_element(const std::string& key)
{
    return key.empty() || ptree_key_is_array_index(key);
}

// type: "all" | "undefine" | "hardened_steel" | "stainless_steel" | "brass" | ["brass","hardened_steel",...]
// Note: boost::ptree JSON leaves have no children, so node.empty() is true even when data() holds the string.
static void parse_nozzle_rule_type_field(const pt::ptree& rule_obj, FilamentHotBedNozzleRules::NozzleForbiddenBand& band)
{
    auto it = rule_obj.find("type");
    if (it == rule_obj.not_found()) {
        band.applies_to_all_nozzle_types = true;
        return;
    }
    const pt::ptree& tn = it->second;
    std::string      scalar = boost::trim_copy(tn.data());
    if (!scalar.empty()) {
        boost::algorithm::to_lower(scalar);
        if (scalar == "all") {
            band.applies_to_all_nozzle_types = true;
            return;
        }
        band.applies_to_all_nozzle_types = false;
        band.nozzle_types.insert(std::move(scalar));
        return;
    }
    // JSON array: children keyed "0", "1", …; values live in leaf .data() (same empty() quirk per element).
    band.applies_to_all_nozzle_types = false;
    for (const auto& ch : tn) {
        if (!ptree_key_is_array_element(ch.first))
            continue;
        std::string v = boost::trim_copy(ch.second.data());
        boost::algorithm::to_lower(v);
        if (!v.empty())
            band.nozzle_types.insert(std::move(v));
    }
    if (band.nozzle_types.empty())
        band.applies_to_all_nozzle_types = true;
}

static void parse_forbidden_from_array_tree(const pt::ptree& arr, FilamentHotBedNozzleRules::NozzleForbiddenBand& band)
{
    for (const auto& item : arr) {
        if (!ptree_key_is_array_element(item.first))
            continue;
        std::string v = boost::trim_copy(item.second.data());
        if (v.empty()) {
            try {
                v = item.second.get_value<std::string>();
            } catch (const pt::ptree_error&) {
                continue;
            }
        }
        boost::algorithm::trim(v);
        if (!v.empty())
            band.forbidden_substrings.insert(std::move(v));
    }
}

static void parse_nozzle_rule_forbidden(const pt::ptree& rule_obj, FilamentHotBedNozzleRules::NozzleForbiddenBand& band)
{
    auto it = rule_obj.find("forbidden");
    if (it == rule_obj.not_found())
        return;
    parse_forbidden_from_array_tree(it->second, band);
}

static void parse_warning_from_array_tree(const pt::ptree& arr, FilamentHotBedNozzleRules::NozzleForbiddenBand& band)
{
    for (const auto& item : arr) {
        if (!ptree_key_is_array_element(item.first))
            continue;
        std::string v = boost::trim_copy(item.second.data());
        if (v.empty()) {
            try {
                v = item.second.get_value<std::string>();
            } catch (const pt::ptree_error&) {
                continue;
            }
        }
        boost::algorithm::trim(v);
        if (!v.empty())
            band.warning_substrings.insert(std::move(v));
    }
}

static void parse_nozzle_rule_warning(const pt::ptree& rule_obj, FilamentHotBedNozzleRules::NozzleForbiddenBand& band)
{
    auto it = rule_obj.find("warning");
    if (it == rule_obj.not_found())
        return;
    parse_warning_from_array_tree(it->second, band);
}

static bool is_typed_nozzle_map_key(std::string k_lower)
{
    boost::algorithm::to_lower(k_lower);
    if (k_lower == "all")
        return true;
    return NozzleTypeStrToEumn.find(k_lower) != NozzleTypeStrToEumn.end();
}

static bool rule_obj_has_typed_nozzle_map(const pt::ptree& rule_obj)
{
    for (const auto& ch : rule_obj) {
        if (ptree_key_is_array_index(ch.first))
            continue;
        std::string k = boost::trim_copy(ch.first);
        boost::algorithm::to_lower(k);
        if (is_typed_nozzle_map_key(k))
            return true;
    }
    return false;
}

// Per-nozzle-diameter object: { "brass": [...], "undefine": { "forbidden": [...] }, "all": [...] }
static void append_bands_from_typed_nozzle_map(const std::string& rule_key, const pt::ptree& rule_obj,
    std::unordered_map<std::string, std::vector<FilamentHotBedNozzleRules::NozzleForbiddenBand>>& out_map)
{
    for (const auto& ch : rule_obj) {
        if (ptree_key_is_array_index(ch.first))
            continue;
        std::string k = boost::trim_copy(ch.first);
        boost::algorithm::to_lower(k);
        if (!is_typed_nozzle_map_key(k))
            continue;

        FilamentHotBedNozzleRules::NozzleForbiddenBand band;
        if (k == "all") {
            band.applies_to_all_nozzle_types = true;
        } else {
            band.applies_to_all_nozzle_types = false;
            band.nozzle_types.insert(k);
        }

        const pt::ptree& vnode         = ch.second;
        const bool       has_forbidden = vnode.find("forbidden") != vnode.not_found();
        const bool       has_warning   = vnode.find("warning") != vnode.not_found();
        if (has_forbidden)
            parse_nozzle_rule_forbidden(vnode, band);
        if (has_warning)
            parse_nozzle_rule_warning(vnode, band);
        if (!has_forbidden && !has_warning)
            parse_forbidden_from_array_tree(vnode, band);

        if (!band.forbidden_substrings.empty() || !band.warning_substrings.empty())
            out_map[rule_key].push_back(std::move(band));
    }
}

static void append_nozzle_bands_from_json(const std::string& rule_key, const pt::ptree& rule_obj,
    std::unordered_map<std::string, std::vector<FilamentHotBedNozzleRules::NozzleForbiddenBand>>& out_map)
{
    const bool has_top_forbidden = rule_obj.find("forbidden") != rule_obj.not_found();
    const bool has_top_type      = rule_obj.find("type") != rule_obj.not_found();
    const bool has_top_warning   = rule_obj.find("warning") != rule_obj.not_found();

    // 顶层 type/forbidden/warning（旧格式）可与分键（brass / undefine / …）并存，不要提前 return，否则分键不会被加载。
    if (has_top_forbidden || has_top_type || has_top_warning) {
        FilamentHotBedNozzleRules::NozzleForbiddenBand band;
        parse_nozzle_rule_type_field(rule_obj, band);
        parse_nozzle_rule_forbidden(rule_obj, band);
        parse_nozzle_rule_warning(rule_obj, band);
        if (!band.forbidden_substrings.empty() || !band.warning_substrings.empty())
            out_map[rule_key].push_back(std::move(band));
    }

    if (rule_obj_has_typed_nozzle_map(rule_obj)) {
        append_bands_from_typed_nozzle_map(rule_key, rule_obj, out_map);
        return;
    }

    for (const auto& ch : rule_obj) {
        if (!ptree_key_is_array_index(ch.first))
            continue;
        const pt::ptree& el = ch.second;
        if (el.find("forbidden") == el.not_found() && el.find("type") == el.not_found() && el.find("warning") == el.not_found())
            continue;
        FilamentHotBedNozzleRules::NozzleForbiddenBand band;
        parse_nozzle_rule_type_field(el, band);
        parse_nozzle_rule_forbidden(el, band);
        parse_nozzle_rule_warning(el, band);
        if (!band.forbidden_substrings.empty() || !band.warning_substrings.empty())
            out_map[rule_key].push_back(std::move(band));
    }
}
} // namespace

FilamentHotBedNozzleRules& FilamentHotBedNozzleRules::singleton()
{
    static FilamentHotBedNozzleRules inst;
    return inst;
}

std::string bed_type_to_filament_rule_key(BedType bed_type)
{
    switch (bed_type) {
    case btPEI:  return "btPEI";
    case btGESP: return "btGESP";
    default:     return "";
    }
}

std::string nozzle_diameter_to_filament_rule_key(double nozzle_diameter_mm)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << nozzle_diameter_mm;
    std::string out = ss.str();
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out + "mm";
}

void FilamentHotBedNozzleRules::ensure_loaded()
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    if (m_loaded)
        return;
    // std::scoped_lock has no unlock(); load() takes the same recursive_mutex (nested lock is OK).
    load();
}

void FilamentHotBedNozzleRules::load()
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);

    m_bed_support_filament_types.clear();
    m_bed_warning_filament_types.clear();
    m_nozzle_forbidden_bands.clear();
    m_loaded = false;

    const std::string file_path = filament_hot_bed_nozzles_json_path();
    if (!boost::filesystem::exists(file_path)) {
        BOOST_LOG_TRIVIAL(warning) << "filament_hot_bed_nozzles.json not found: " << file_path;
        return;
    }

    try {
        pt::ptree root;
        pt::read_json(file_path, root);

        for (const auto& kv : root) {
            const std::string rule_key = boost::trim_copy(kv.first);
            const auto&       rule_obj = kv.second;

            if (boost::algorithm::ends_with(rule_key, "mm")) {
                append_nozzle_bands_from_json(rule_key, rule_obj, m_nozzle_forbidden_bands);
                continue;
            }

            auto& support_set = m_bed_support_filament_types[rule_key];
            auto& warning_set = m_bed_warning_filament_types[rule_key];

            for (const auto& child : rule_obj) {
                if (child.first == "support") {
                    for (const auto& item : child.second)
                        support_set.insert(item.second.get_value<std::string>());
                } else if (child.first == "warning") {
                    for (const auto& item : child.second)
                        warning_set.insert(item.second.get_value<std::string>());
                }
            }
        }
        m_loaded = true;
        BOOST_LOG_TRIVIAL(info) << "Loaded filament/hotbed/nozzle rules from " << file_path;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to parse filament_hot_bed_nozzles.json: " << e.what();
    }
}

bool FilamentHotBedNozzleRules::is_loaded() const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    return m_loaded;
}
bool FilamentHotBedNozzleRules::is_bed_filament_tips(const std::string& bed_key, const std::string& filament_type) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_bed_warning_filament_types.find(bed_key);
    if (it == m_bed_warning_filament_types.end())
        return false;

    bool res = match_any_rule_token_ci(it->second, filament_type);

    return res;
}


bool FilamentHotBedNozzleRules::is_bed_filament_supported(const std::string& bed_key, const std::string& filament_type) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_bed_support_filament_types.find(bed_key);
    if (it == m_bed_support_filament_types.end())
        return true;
    if (it->second.empty())
        return true;
    if (match_any_rule_token_ci(it->second, filament_type))
        return true;
    // Warning-tier materials are still "supported" (separate API reports warning).
    auto itw = m_bed_warning_filament_types.find(bed_key);
    bool res = false;
    res = itw != m_bed_warning_filament_types.end() && match_any_rule_token_ci(itw->second, filament_type);
    return res;
}

bool FilamentHotBedNozzleRules::is_bed_filament_warning(const std::string& bed_key, const std::string& filament_type) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_bed_warning_filament_types.find(bed_key);
    bool res = false;
    res =  (it != m_bed_warning_filament_types.end() && match_any_rule_token_ci(it->second, filament_type));
    return res;
}

bool FilamentHotBedNozzleRules::is_nozzle_filament_forbidden(const std::string& nozzle_key, const std::string& filament_preset_name,
                                                             NozzleType nozzle_type) const
{
    (void)nozzle_key;
    (void)filament_preset_name;
    (void)nozzle_type;
    // JSON "forbidden" for nozzle+filament is intentionally not applied; use warning lists only.
    return false;
}

bool FilamentHotBedNozzleRules::is_nozzle_filament_warning(const std::string& nozzle_key, const std::string& filament_preset_name,
                                                           NozzleType nozzle_type) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_nozzle_forbidden_bands.find(nozzle_key);
    if (it == m_nozzle_forbidden_bands.end())
        return false;

    std::string cur_type = "undefine";
    auto        nit      = NozzleTypeEumnToStr.find(nozzle_type);
    if (nit != NozzleTypeEumnToStr.end())
        cur_type = nit->second;

    for (const NozzleForbiddenBand& band : it->second) {
        if (!band.applies_to_all_nozzle_types) {
            if (band.nozzle_types.find(cur_type) == band.nozzle_types.end())
                continue;
        }
        for (const std::string& token : band.warning_substrings) {
            if (nozzle_warning_rule_matches_filament_preset_ci(token, filament_preset_name))
                return true;
        }
    }
    return false;
}

static std::string nozzle_diameter_mm_display(double nozzle_diameter_mm)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << nozzle_diameter_mm;
    std::string out = ss.str();
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out;
}

bool FilamentHotBedNozzleRules::evaluate_nozzle_filament_mismatch_detail(const PrintConfig& cfg,
                                                                         const std::vector<unsigned int>& used_filament_indices,
                                                                         const PresetBundle* preset_bundle,
                                                                         NozzleFilamentRuleMismatch&      out) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    out = NozzleFilamentRuleMismatch{};
    if (!m_loaded || used_filament_indices.empty())
        return false;

    const ConfigOptionStrings* filament_settings_id = cfg.option<ConfigOptionStrings>("filament_settings_id");
    const PresetCollection*    filament_collection  = preset_bundle != nullptr ? &preset_bundle->filaments : nullptr;

    for (unsigned int fid : used_filament_indices) {
        std::string        nozzle_key_fid;
        unsigned int       nd_idx = 0;
        double             cur_mm = 0.;
        if (!cfg.nozzle_diameter.empty()) {
            nd_idx         = std::min(fid, unsigned(cfg.nozzle_diameter.size() - 1));
            cur_mm         = cfg.nozzle_diameter.get_at(nd_idx);
            nozzle_key_fid = nozzle_diameter_to_filament_rule_key(cur_mm);
        }
        if (nozzle_key_fid.empty())
            continue;

        std::string stored;
        if (preset_bundle != nullptr && fid < preset_bundle->filament_presets.size()) {
            stored = preset_bundle->filament_presets[fid];
            boost::algorithm::trim(stored);
        }
        if (stored.empty() && filament_settings_id != nullptr && fid < filament_settings_id->values.size())
            stored = filament_settings_id->get_at(fid);
        boost::algorithm::trim(stored);

        std::string normalized = stored;
        if (preset_bundle != nullptr && !normalized.empty()) {
            const std::string            trimmed = Preset::remove_suffix_modified(normalized);
            const std::string&           by_alias = preset_bundle->get_preset_name_by_alias(Preset::TYPE_FILAMENT, trimmed);
            normalized.assign(by_alias);
        }

        const std::string preset_name = resolve_filament_preset_full_name(normalized, filament_collection);
        if (!is_nozzle_filament_warning(nozzle_key_fid, preset_name, cfg.nozzle_type.value))
            continue;

        out.has_mismatch           = true;
        out.nozzle_diameter_mm     = nozzle_diameter_mm_display(cur_mm);
        auto nit                   = NozzleTypeEumnToStr.find(cfg.nozzle_type.value);
        out.nozzle_type_key        = (nit != NozzleTypeEumnToStr.end()) ? nit->second : std::string("undefine");
        out.filament_preset_name   = preset_name;
        return true;
    }

    return false;
}

std::string FilamentHotBedNozzleRules::evaluate_nozzle_filament_mismatch(const PrintConfig& cfg,
                                                                          const std::vector<unsigned int>& used_filament_indices,
                                                                          const PresetBundle* preset_bundle) const
{
    NozzleFilamentRuleMismatch d;
    if (!evaluate_nozzle_filament_mismatch_detail(cfg, used_filament_indices, preset_bundle, d) || !d.has_mismatch)
        return "";
    return d.filament_preset_name.empty() ? "nozzle filament preset warning" : d.filament_preset_name;
}

bool FilamentHotBedNozzleRules::evaluate_graphic_effect_bed_filament_mismatch(const PrintConfig& cfg, const std::vector<unsigned int>& used_filament_indices) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_loaded || used_filament_indices.empty())
        return false;
    if (static_cast<BedType>(cfg.curr_bed_type) != btGESP)
        return false;
    if (cfg.filament_type.empty())
        return false;

    const std::string bed_key = kBedKey_GESP;
    for (unsigned int fid : used_filament_indices) {
        const std::string ftype = cfg.filament_type.get_at(fid);
        if (ftype.empty())
            continue;
        if (!is_bed_filament_supported(bed_key, ftype))
            return true;
        if (is_bed_filament_warning(bed_key, ftype))
            return true;
    }
    return false;
}
bool FilamentHotBedNozzleRules::evaluate_pei_bed_filament_mismatch_not_pla(const PrintConfig&               cfg,
                                                                   const std::vector<unsigned int>& used_filament_indices) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_loaded || used_filament_indices.empty())
        return false;
    if (static_cast<BedType>(cfg.curr_bed_type) != btPEI)
        return false;
    if (cfg.filament_type.empty())
        return false;

    const std::string bed_key = kBedKey_PEI;
    bool res = false;//default not pla and need to show tips
    for (unsigned int fid : used_filament_indices) {
        const std::string ftype = cfg.filament_type.get_at(fid);
        if (ftype.empty())
            continue;
        // not_pla means every non-PLA type should be checked, including TPU.
        bool checkRes = is_pla_type(ftype);
        if (!checkRes)
        {
            res = !checkRes;
            break;
        }
    }
    return res;
}

bool FilamentHotBedNozzleRules::evaluate_pei_bed_filament_mismatch_tpu(const PrintConfig&               cfg,
                                                                       const std::vector<unsigned int>& used_filament_indices) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_loaded || used_filament_indices.empty())
        return false;
    if (static_cast<BedType>(cfg.curr_bed_type) != btPEI)
        return false;
    if (cfg.filament_type.empty())
        return false;

    const std::string bed_key = kBedKey_PEI;
    for (unsigned int fid : used_filament_indices) {
        const std::string ftype = cfg.filament_type.get_at(fid);
        if (ftype.empty())
            continue;

        if (!is_tpu_type(ftype))
            continue;
        // TPU on PEI: dedicated warning channel.
        if (is_bed_filament_tips(bed_key, ftype))
            return true;
    }
    return false;
}

} // namespace Slic3r
