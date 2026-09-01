#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "test_utils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfConstants.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfJson.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfLegacyBridge.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfReader.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfValidation.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfWriter.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MixedFilament/PerimeterModulation.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <map>
#include <utility>
#include <cstdint>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::FullSpectrum3mf;

namespace {

class ScopedCommaNumericLocale
{
public:
    ScopedCommaNumericLocale()
    {
        if (const char *current = std::setlocale(LC_NUMERIC, nullptr))
            m_original = current;

#ifdef _WIN32
        m_previous_thread_locale_mode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
#endif

        static constexpr const char *candidates[] = {
#ifdef _WIN32
            "German_Germany.1252",
            "German_Germany",
#else
            "de_DE.UTF-8",
            "de_DE.utf8",
            "de_DE",
#endif
        };

        for (const char *candidate : candidates) {
            if (std::setlocale(LC_NUMERIC, candidate) == nullptr)
                continue;
            const std::lconv *conventions = std::localeconv();
            if (conventions != nullptr && conventions->decimal_point != nullptr && conventions->decimal_point[0] == ',') {
                m_active = true;
                break;
            }
        }

        if (!m_active && !m_original.empty())
            std::setlocale(LC_NUMERIC, m_original.c_str());
    }

    ~ScopedCommaNumericLocale()
    {
        if (!m_original.empty())
            std::setlocale(LC_NUMERIC, m_original.c_str());
#ifdef _WIN32
        if (m_previous_thread_locale_mode != -1)
            _configthreadlocale(m_previous_thread_locale_mode);
#endif
    }

    bool active() const { return m_active; }

private:
    std::string m_original;
    bool        m_active = false;
#ifdef _WIN32
    int m_previous_thread_locale_mode = -1;
#endif
};

static std::vector<std::string> split_rows(const std::string &serialized)
{
    std::vector<std::string> rows;
    std::stringstream ss(serialized);
    std::string row;
    while (std::getline(ss, row, ';')) {
        if (!row.empty())
            rows.push_back(row);
    }
    return rows;
}

static std::string join_rows(const std::vector<std::string> &rows)
{
    std::ostringstream ss;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i != 0)
            ss << ';';
        ss << rows[i];
    }
    return ss.str();
}

static unsigned int virtual_id_for_stable_id(const std::vector<MixedFilamentLegacyRow> &mixed, size_t num_physical, uint64_t stable_id)
{
    unsigned int next_virtual_id = unsigned(num_physical + 1);
    for (const MixedFilamentLegacyRow &mf : mixed) {
        if (mf.deleted)
            continue;
        if (mf.stable_id == stable_id)
            return next_virtual_id;
        ++next_virtual_id;
    }
    return 0;
}

static void set_legacy_row(MixedFilamentManager &manager,
                           size_t                index,
                           const MixedFilamentLegacyRow  &row,
                           const std::vector<std::string> &colors)
{
    REQUIRE(manager.set_mixed_filament_legacy_row(index, row, colors.size(), colors));
}

struct MixedAutoGenerateGuard
{
    explicit MixedAutoGenerateGuard(bool enabled)
        : previous(MixedFilamentManager::auto_generate_enabled())
    {
        MixedFilamentManager::set_auto_generate_enabled(enabled);
    }

    ~MixedAutoGenerateGuard()
    {
        MixedFilamentManager::set_auto_generate_enabled(previous);
    }

    bool previous = true;
};

static PresetBundle make_bundle_with_filaments(const std::vector<std::string> &colors)
{
    PresetBundle bundle;
    bundle.filament_presets.assign(colors.size(), "Default Filament");
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = colors;
    bundle.project_config.option<ConfigOptionStrings>("filament_settings_id", true)->values.assign(colors.size(), "Default Filament");
    bundle.update_multi_material_filament_presets();
    return bundle;
}

static const PackagePartPlan *find_fullspectrum_part(const PackageWritePlan &plan, const std::string &path)
{
    auto it = std::find_if(plan.parts.begin(), plan.parts.end(), [&path](const PackagePartPlan &part) {
        return part.path == path;
    });
    return it == plan.parts.end() ? nullptr : &*it;
}

static std::map<std::string, std::string> fullspectrum_part_map(const PackageWritePlan &plan)
{
    std::map<std::string, std::string> parts;
    for (const PackagePartPlan &part : plan.parts)
        parts[package_path_to_zip_path(part.path)] = part.bytes;
    return parts;
}

static PackageModel make_assignment_package_model()
{
    PackageModel package;

    package.project.kind = KIND_PROJECT;
    package.project.schema_version = PROFILE_VERSION;
    package.project.project_id = "proj_assignment_test";
    package.project.display_name = "assignment import";
    package.project.legacy_projection_written = true;

    package.materials.kind = KIND_MATERIALS;
    package.materials.schema_version = PROFILE_VERSION;
    package.materials.physical_filaments.push_back({"fil_red", "Red", "PLA", 1.75, "#FF0000", 1});
    package.materials.physical_filaments.push_back({"fil_blue", "Blue", "PLA", 1.75, "#0000FF", 2});

    package.identity_map.kind = KIND_IDENTITY_MAP;
    package.identity_map.schema_version = PROFILE_VERSION;
    package.identity_map.model_object_bindings.push_back({10, "obj_a"});
    package.identity_map.volume_bindings.push_back({10, 77, "obj_a", "vol_a"});
    package.identity_map.material_bindings.push_back({1, "fil_red"});
    package.identity_map.material_bindings.push_back({2, "fil_blue"});

    package.assignments.kind = KIND_ASSIGNMENTS;
    package.assignments.schema_version = PROFILE_VERSION;

    Assignment assignment;
    assignment.id = "assign_vol_a";
    assignment.target.kind = "volume";
    assignment.target.stable_volume_id = "vol_a";
    assignment.material_ref = "fil_blue";
    package.assignments.assignments.push_back(assignment);
    package.assignments.paint_state_bindings.push_back({"vol_a", 3, "fil_blue"});

    package.manifest.kind = KIND_MANIFEST;
    package.manifest.schema_version = PROFILE_VERSION;
    package.manifest.document_class = "project";
    package.manifest.package_id = "pkg_assignment_test";
    package.manifest.required_features = {
        FEATURE_PROJECT_CORE,
        FEATURE_IDENTITY_MAP,
        FEATURE_MATERIALS_CORE,
        FEATURE_ASSIGNMENTS
    };
    package.manifest.optional_features = {FEATURE_LEGACY_PROJECTION};
    package.manifest.authoritative_sources = {
        {"project", PATH_PROJECT},
        {"identity_map", PATH_IDENTITY_MAP},
        {"materials", PATH_MATERIALS},
        {"assignments", PATH_ASSIGNMENTS}
    };
    package.manifest.legacy_projection.present = true;
    package.manifest.legacy_projection.derived_from = {PATH_PROJECT, PATH_MATERIALS, PATH_ASSIGNMENTS};
    package.manifest.legacy_projection.paths = {"/Metadata/project_settings.config"};

    return package;
}

static ArchiveImportState import_state_from_plan(const PackageWritePlan &plan)
{
    ArchiveImportState state;
    for (const PackagePartPlan &part : plan.parts)
        state.add_part(package_path_to_zip_path(part.path), part.bytes);
    return state;
}

} // namespace

TEST_CASE("Mixed filament project optical metadata survives presets without calibration values", "[MixedFilament]")
{
    PresetBundle bundle;
    const std::string selected_name = Preset::remove_suffix_modified(bundle.filaments.get_edited_preset().name);
    bundle.filament_presets.assign(2, selected_name);
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#0000FF"};
    bundle.project_config.option<ConfigOptionFloats>("filament_transmission_distance")->values = {0.8, 1.6};
    bundle.project_config.option<ConfigOptionStrings>("filament_full_spectrum_material_id")->values = {
        "fs_red_project", "fs_blue_project"};

    ConfigOptionFloats *preset_tds =
        bundle.filaments.get_edited_preset().config.option<ConfigOptionFloats>("filament_transmission_distance");
    ConfigOptionStrings *preset_material_ids =
        bundle.filaments.get_edited_preset().config.option<ConfigOptionStrings>("filament_full_spectrum_material_id");
    REQUIRE(preset_tds != nullptr);
    REQUIRE(preset_material_ids != nullptr);
    preset_tds->values = {0.0};
    preset_material_ids->values = {""};

    bundle.update_multi_material_filament_presets();

    CHECK(bundle.project_config.option<ConfigOptionFloats>("filament_transmission_distance")->values ==
          std::vector<double>{0.8, 1.6});
    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_full_spectrum_material_id")->values ==
          std::vector<std::string>{"fs_red_project", "fs_blue_project"});
}

TEST_CASE("Mixed filament selected preset optical metadata overrides project fallbacks", "[MixedFilament]")
{
    PresetBundle bundle;
    const std::string selected_name = Preset::remove_suffix_modified(bundle.filaments.get_edited_preset().name);
    bundle.filament_presets.assign(2, selected_name);
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#0000FF"};
    bundle.project_config.option<ConfigOptionFloats>("filament_transmission_distance")->values = {0.8, 1.6};
    bundle.project_config.option<ConfigOptionStrings>("filament_full_spectrum_material_id")->values = {
        "fs_red_project", "fs_blue_project"};

    ConfigOptionFloats *preset_tds =
        bundle.filaments.get_edited_preset().config.option<ConfigOptionFloats>("filament_transmission_distance");
    ConfigOptionStrings *preset_material_ids =
        bundle.filaments.get_edited_preset().config.option<ConfigOptionStrings>("filament_full_spectrum_material_id");
    REQUIRE(preset_tds != nullptr);
    REQUIRE(preset_material_ids != nullptr);
    preset_tds->values = {2.4};
    preset_material_ids->values = {"fs_selected_material"};

    bundle.update_multi_material_filament_presets();

    CHECK(bundle.project_config.option<ConfigOptionFloats>("filament_transmission_distance")->values ==
          std::vector<double>{2.4, 2.4});
    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_full_spectrum_material_id")->values ==
          std::vector<std::string>{"fs_selected_material", "fs_selected_material"});
}

TEST_CASE("Mixed filament optical metadata stays slot aligned across delete and add", "[MixedFilament]")
{
    PresetBundle bundle;
    const std::string selected_name = Preset::remove_suffix_modified(bundle.filaments.get_edited_preset().name);
    bundle.filament_presets.assign(3, selected_name);
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"};
    bundle.project_config.option<ConfigOptionFloats>("filament_transmission_distance")->values = {0.5, 1.5, 2.5};
    bundle.project_config.option<ConfigOptionStrings>("filament_full_spectrum_material_id")->values = {
        "fs_red", "fs_green", "fs_blue"};

    ConfigOptionFloats *preset_tds =
        bundle.filaments.get_edited_preset().config.option<ConfigOptionFloats>("filament_transmission_distance");
    ConfigOptionStrings *preset_material_ids =
        bundle.filaments.get_edited_preset().config.option<ConfigOptionStrings>("filament_full_spectrum_material_id");
    REQUIRE(preset_tds != nullptr);
    REQUIRE(preset_material_ids != nullptr);
    preset_tds->values = {0.0};
    preset_material_ids->values = {""};

    bundle.update_multi_material_filament_presets();
    bundle.update_num_filaments(1);

    CHECK(bundle.project_config.option<ConfigOptionFloats>("filament_transmission_distance")->values ==
          std::vector<double>{0.5, 2.5});
    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_full_spectrum_material_id")->values ==
          std::vector<std::string>{"fs_red", "fs_blue"});

    bundle.set_num_filaments(3);

    CHECK(bundle.project_config.option<ConfigOptionFloats>("filament_transmission_distance")->values ==
          std::vector<double>{0.5, 2.5, 0.0});
    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_full_spectrum_material_id")->values ==
          std::vector<std::string>{"fs_red", "fs_blue", ""});
}

TEST_CASE("Mixed filament remap follows stable row ids when same-pair rows reorder", "[MixedFilament]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#0000FF"};
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    auto mixed = mgr.mixed_filament_legacy_rows();
    REQUIRE(mixed.size() == 1);

    mixed[0].deleted = true;
    const auto colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.set_mixed_filament_legacy_rows(mixed, colors.size(), colors);
    mgr.add_custom_filament(1, 2, 25, colors);
    mgr.add_custom_filament(1, 2, 75, colors);

    const auto &old_mixed = mgr.mixed_filament_legacy_rows();
    REQUIRE(old_mixed.size() == 3);
    REQUIRE(!old_mixed[1].deleted);
    REQUIRE(!old_mixed[2].deleted);
    const uint64_t first_custom_id = old_mixed[1].stable_id;
    const uint64_t second_custom_id = old_mixed[2].stable_id;

    std::vector<std::string> rows = split_rows(mgr.serialize_custom_entries());
    REQUIRE(rows.size() == 3);
    std::swap(rows[1], rows[2]);

    auto *definitions = bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(definitions != nullptr);
    definitions->value = join_rows(rows);

    bundle.filament_presets.push_back(bundle.filament_presets.back());
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.push_back("#00FF00");
    bundle.update_multi_material_filament_presets(size_t(-1), 2);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 5);

    const auto &rebuilt = bundle.mixed_filaments.mixed_filament_legacy_rows();
    const unsigned int new_first_custom_virtual_id = virtual_id_for_stable_id(rebuilt, 3, first_custom_id);
    const unsigned int new_second_custom_virtual_id = virtual_id_for_stable_id(rebuilt, 3, second_custom_id);

    REQUIRE(new_first_custom_virtual_id != 0);
    REQUIRE(new_second_custom_virtual_id != 0);
    CHECK(remap[3] == new_first_custom_virtual_id);
    CHECK(remap[4] == new_second_custom_virtual_id);
}

TEST_CASE("Mixed filament remap keeps later painted colors stable when an earlier mixed row is deleted", "[MixedFilament]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    auto mixed = bundle.mixed_filaments.mixed_filament_legacy_rows();
    REQUIRE(mixed.size() >= 6);

    const uint64_t stable_id_6 = mixed[1].stable_id;
    const uint64_t stable_id_7 = mixed[2].stable_id;
    const uint64_t stable_id_8 = mixed[3].stable_id;

    const std::vector<MixedFilamentDefinition> old_mixed = bundle.mixed_filaments.mixed_filament_definitions(4);
    mixed[0].deleted = true;
    bundle.mixed_filaments.set_mixed_filament_legacy_rows(mixed, 4, bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values);

    bundle.update_mixed_filament_id_remap(old_mixed, 4, 4);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    const auto &updated_mixed = bundle.mixed_filaments.mixed_filament_legacy_rows();

    REQUIRE(remap.size() >= 11);
    CHECK(remap[6] == virtual_id_for_stable_id(updated_mixed, 4, stable_id_6));
    CHECK(remap[7] == virtual_id_for_stable_id(updated_mixed, 4, stable_id_7));
    CHECK(remap[8] == virtual_id_for_stable_id(updated_mixed, 4, stable_id_8));
}

TEST_CASE("Mixed filament remap shifts virtual ids on physical-count expansion with structurally identical rows", "[MixedFilament]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"F1", "F2"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00"};

    // One enabled mixed row after 2 physical filaments: virtual id 3.
    MixedFilament row;
    row.component_a = 1;
    row.component_b = 2;
    row.stable_id   = 4242;
    row.enabled     = true;
    row.custom      = true;
    std::vector<MixedFilament> old_mixed{row};
    const std::vector<MixedFilamentDefinition> old_definitions{
        mixed_filament_definition_from_legacy_row(row, 2)};
    bundle.mixed_filaments.mixed_filaments() = old_mixed;

    // Physical-count expansion 2 -> 4: the row keeps every field, so the two
    // lists are structurally identical (MixedFilament::operator== ignores
    // display_color) — yet the row's virtual id shifts from 3 (2 physical + 1)
    // to 5 (4 physical + 1) because virtual ids are positional. The remap must
    // still be built in this case (Sidebar runs it whenever the count changes,
    // not only when the row list differs).
    std::vector<MixedFilament> new_mixed{row};
    bundle.mixed_filaments.mixed_filaments() = new_mixed;
    REQUIRE(bundle.mixed_filaments.mixed_filaments() == old_mixed);

    bundle.update_mixed_filament_id_remap(old_definitions, 2, 4);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    REQUIRE(remap.size() >= 4);
    CHECK(remap[1] == 1);   // physical ids keep identity
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 5);   // mixed row vid 3 -> 5 (shifted past the 2 new physicals)
}

TEST_CASE("Mixed filament grouped manual patterns normalize and round-trip", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#0000FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("1/1/1/1/1/1/1/2, 1/1/1/2/1/1/1/1");
    REQUIRE(row.manual_pattern == "11111112,11121111");
    set_legacy_row(mgr, 0, row, colors);

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filament_legacy_rows().size() == 1);
    CHECK(loaded.mixed_filament_legacy_rows().front().manual_pattern == "11111112,11121111");
    CHECK(loaded.mixed_filament_legacy_rows().front().mix_b_percent == 13);
}

TEST_CASE("Mixed filament component surface offsets round-trip and apply independently", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.ratio_a = 1;
    row.ratio_b = 1;
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.01f;
    set_legacy_row(mgr, 0, row, colors);

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("xa0.02") != std::string::npos);
    CHECK(serialized.find("xb-0.01") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filament_legacy_rows().size() == 1);

    const MixedFilamentLegacyRow &loaded_row = loaded.mixed_filament_legacy_rows().front();
    CHECK(loaded_row.component_a_surface_offset == Approx(0.02f));
    CHECK(loaded_row.component_b_surface_offset == Approx(-0.01f));
    CHECK(loaded.component_surface_offset(3, 2, 0) == Approx(0.02f));
    CHECK(loaded.component_surface_offset(3, 2, 1) == Approx(-0.01f));
}

TEST_CASE("Mixed filament apparent mix percent follows the signed bias target", "[MixedFilament]")
{
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, 0.00f, 0.4f) == 50);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, 0.02f, 0.4f) == 45);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.02f, 0.00f, 0.4f) == 55);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, -0.02f, 0.00f, 0.4f) == 45);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, -0.02f, 0.4f) == 55);
}

TEST_CASE("Local-Z minimum sublayer height clamps direct nominal-layer ratios", "[MixedFilament][LocalZ]")
{
    const auto quarter_b = mixed_filament_local_z_pair_heights(0.20, 0.04, 25);
    CHECK(quarter_b.first == Approx(0.15));
    CHECK(quarter_b.second == Approx(0.05));

    const auto clamped_b = mixed_filament_local_z_pair_heights(0.20, 0.04, 10);
    CHECK(clamped_b.first == Approx(0.16));
    CHECK(clamped_b.second == Approx(0.04));

    const auto all_a = mixed_filament_local_z_pair_heights(0.20, 0.04, 0);
    CHECK(all_a.first == Approx(0.20));
    CHECK(all_a.second == Approx(0.0));

    const auto all_b = mixed_filament_local_z_pair_heights(0.20, 0.04, 100);
    CHECK(all_b.first == Approx(0.0));
    CHECK(all_b.second == Approx(0.20));
}

TEST_CASE("Local-Z upper height bound is retired as a legacy setting", "[MixedFilament][LocalZ]")
{
    CHECK(print_config_def.has("mixed_filament_height_lower_bound"));
    CHECK_FALSE(print_config_def.has("mixed_filament_height_upper_bound"));

    const ConfigOptionDef *minimum_def = print_config_def.get("mixed_filament_height_lower_bound");
    REQUIRE(minimum_def != nullptr);
    REQUIRE(bool(minimum_def->default_value));
    CHECK(static_cast<const ConfigOptionFloat *>(minimum_def->default_value.get())->value == Approx(0.06));

    std::string key   = "mixed_filament_height_upper_bound";
    std::string value = "0.24";
    PrintConfigDef::handle_legacy(key, value);
    CHECK(key.empty());
}

TEST_CASE("Gradient overlap settings migrate to a middle-filament window", "[MixedFilament][LocalZ][Gradient]")
{
    CHECK_FALSE(print_config_def.has("dithering_local_z_gradient_overlap_window"));
    const ConfigOptionDef *window_def = print_config_def.get("dithering_local_z_gradient_middle_filament_window");
    REQUIRE(window_def != nullptr);
    REQUIRE(bool(window_def->default_value));
    CHECK(static_cast<const ConfigOptionPercent *>(window_def->default_value.get())->value == Approx(22.0));
    CHECK(window_def->min == Approx(0.0));
    CHECK(window_def->max == Approx(100.0));

    std::string negative_key   = "dithering_local_z_gradient_overlap_window";
    std::string negative_value = "-35%";
    PrintConfigDef::handle_legacy(negative_key, negative_value);
    CHECK(negative_key == "dithering_local_z_gradient_middle_filament_window");
    CHECK(negative_value == "35%");

    std::string positive_key   = "dithering_local_z_gradient_overlap_window";
    std::string positive_value = "18%";
    PrintConfigDef::handle_legacy(positive_key, positive_value);
    CHECK(positive_key == "dithering_local_z_gradient_middle_filament_window");
    CHECK(positive_value == "18%");
}

TEST_CASE("Local-Z can preserve the first Full-domain layer", "[MixedFilament][LocalZ]")
{
    const ConfigOptionDef *option_def = print_config_def.get("dithering_local_z_preserve_first_layer");
    REQUIRE(option_def != nullptr);
    REQUIRE(bool(option_def->default_value));
    CHECK(static_cast<const ConfigOptionBool *>(option_def->default_value.get())->value);

    CHECK_FALSE(mixed_filament_local_z_should_subdivide_layer(0, true, true));
    CHECK(mixed_filament_local_z_should_subdivide_layer(1, true, true));
    CHECK(mixed_filament_local_z_should_subdivide_layer(0, false, true));
    CHECK(mixed_filament_local_z_should_subdivide_layer(0, true, false));
}

TEST_CASE("Gradients opt into Local-Z without enabling the global override", "[MixedFilament][LocalZ]")
{
    MixedFilamentDefinition ordinary;
    ordinary.recipe.blend.components = {{{1}, 50}, {{2}, 50}};

    MixedFilamentDefinition gradient = ordinary;
    gradient.behavior.gradient.enabled = true;

    CHECK_FALSE(mixed_filament_definition_uses_local_z(ordinary, false));
    CHECK(mixed_filament_definition_uses_local_z(ordinary, true));
    CHECK(mixed_filament_definition_uses_local_z(gradient, false));

    gradient.recipe.manual_pattern = MixedFilamentManualPattern{{{{1}}, {{2}}}};
    CHECK_FALSE(mixed_filament_definition_uses_local_z(gradient, false));

    gradient.recipe.manual_pattern.reset();
    gradient.visibility.tombstoned = true;
    CHECK_FALSE(mixed_filament_definition_uses_local_z(gradient, true));
}

TEST_CASE("Object-assigned Local-Z rows opt into full-domain scope", "[MixedFilament][LocalZ]")
{
    CHECK_FALSE(mixed_filament_local_z_uses_full_domain(false, false));
    CHECK(mixed_filament_local_z_uses_full_domain(false, true));
    CHECK(mixed_filament_local_z_uses_full_domain(true, false));
    CHECK(mixed_filament_local_z_uses_full_domain(true, true));
}

TEST_CASE("Local-Z planner leaves physical and inactive mixed paint at nominal height", "[MixedFilament][LocalZ]")
{
    CHECK_FALSE(mixed_filament_local_z_painted_override_uses_planner(false, false));
    CHECK_FALSE(mixed_filament_local_z_painted_override_uses_planner(false, true));
    CHECK_FALSE(mixed_filament_local_z_painted_override_uses_planner(true, false));
    CHECK(mixed_filament_local_z_painted_override_uses_planner(true, true));
}

TEST_CASE("Local-Z preview reports the effective direct thickness ratio", "[MixedFilament][LocalZ]")
{
    MixedFilamentLegacyRow row;
    row.component_a   = 1;
    row.component_b   = 2;
    row.mix_b_percent = 25;
    row.custom        = true;

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 2);
    MixedFilamentPreviewSettings settings;
    settings.nominal_layer_height = 0.20;
    settings.min_sublayer_height  = 0.04;
    settings.local_z_mode         = true;

    const std::vector<double> passes = mixed_filament_local_z_preview_pass_heights(0.20, 0.04, 0.0, 0.0, 25);
    REQUIRE(passes.size() == 2);
    CHECK(passes[0] == Approx(0.15));
    CHECK(passes[1] == Approx(0.05));
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(definition, settings) == 25);

    settings.min_sublayer_height = 0.06;
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(definition, settings) == 30);

    settings.local_z_mode = false;
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(definition, settings) == 25);
    CHECK(mixed_filament_supports_bias_apparent_color(definition, settings, true));

    MixedFilamentDefinition gradient = definition;
    gradient.behavior.gradient.enabled = true;
    settings.nominal_layer_height          = 0.08;
    settings.gradient_nominal_layer_height = 0.20;
    settings.preferred_a_height            = 0.03;
    settings.preferred_b_height            = 0.03;
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(gradient, settings) == 30);
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(gradient, settings, true));
}

TEST_CASE("Imported model filament IDs remap physical overflow to virtual colors", "[MixedFilament][Import]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->add_instance();
    object->config.set("extruder", 5);
    ModelVolume *volume = object->add_volume(make_cube(1., 1., 1.));
    volume->config.set("extruder", 6);

    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder7);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    std::vector<unsigned int> remap(8, 0);
    for (unsigned int id = 1; id <= 4; ++id)
        remap[id] = id;
    remap[5] = 7;
    remap[6] = 6;
    remap[7] = 5;
    remap_model_filament_ids(model, remap, 7);

    CHECK(object->config.extruder() == 7);
    CHECK(volume->config.extruder() == 6);
    const std::vector<bool> &used_states = volume->mmu_segmentation_facets.get_data().used_states;
    REQUIRE(used_states.size() > 7);
    CHECK(used_states[5]);
    CHECK_FALSE(used_states[7]);
}

TEST_CASE("Mixed filament bias helper maps signed bias to a one-sided safe offset pair", "[MixedFilament]")
{
    const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.06f, 0.4f);
    CHECK_THAT(offset_a, WithinRel(0.0f, 0.001));
    CHECK_THAT(offset_b, WithinRel(0.06f, 0.001));

    CHECK_THAT(MixedFilamentManager::bias_ui_value_from_surface_offsets(offset_a, offset_b, 0.4f), WithinRel(0.06f, 0.001));

    CHECK_THAT(MixedFilamentManager::bias_ui_value_from_surface_offsets(0.02f, 0.0f, 0.4f), WithinRel(-0.02f, 0.001));
    CHECK_THAT(MixedFilamentManager::bias_ui_value_from_surface_offsets(-0.02f, 0.0f, 0.4f), WithinRel(0.02f, 0.001));

    const auto [negative_a, negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.06f, 0.4f);
    CHECK_THAT(negative_a, WithinRel(0.06f, 0.001));
    CHECK_THAT(negative_b, WithinRel(0.0f, 0.001));

    const auto [unclamped_a, unclamped_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.30f, 0.4f);
    CHECK_THAT(unclamped_a, WithinRel(0.0f, 0.001));
    CHECK_THAT(unclamped_b, WithinRel(0.30f, 0.001));

    const auto [unclamped_negative_a, unclamped_negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.30f, 0.4f);
    CHECK_THAT(unclamped_negative_a, WithinRel(0.30f, 0.001));
    CHECK_THAT(unclamped_negative_b, WithinRel(0.0f, 0.001));

    const auto [clamped_a, clamped_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.40f, 0.4f);
    CHECK_THAT(clamped_a, WithinRel(0.0f, 0.001));
    CHECK_THAT(clamped_b, WithinRel(0.35f, 0.001));

    const auto [clamped_negative_a, clamped_negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.40f, 0.4f);
    CHECK_THAT(clamped_negative_a, WithinRel(0.35f, 0.001));
    CHECK_THAT(clamped_negative_b, WithinRel(0.0f, 0.001));
}

TEST_CASE("Mixed filament component surface offsets follow the signed bias target across alternating layers", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern.clear();
    row.distribution_mode = int(MixedFilamentLegacyRow::Simple);
    row.ratio_a = 1;
    row.ratio_b = 1;
    set_legacy_row(mgr, 0, row, colors);

    {
        const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.05f, 0.4f);
        row.component_a_surface_offset = offset_a;
        row.component_b_surface_offset = offset_b;
        set_legacy_row(mgr, 0, row, colors);

        CHECK(mgr.component_surface_offset(3, 2, 0) == Approx(0.0f));
        CHECK(mgr.component_surface_offset(3, 2, 1) == Approx(0.05f));
        CHECK(mgr.component_surface_offset(3, 2, 2) == Approx(0.0f));
        CHECK(mgr.component_surface_offset(3, 2, 3) == Approx(0.05f));
    }

    {
        row.component_a_surface_offset = 0.05f;
        row.component_b_surface_offset = 0.0f;
        set_legacy_row(mgr, 0, row, colors);

        CHECK(mgr.component_surface_offset(3, 2, 0) == Approx(0.05f));
        CHECK(mgr.component_surface_offset(3, 2, 1) == Approx(0.0f));
        CHECK(mgr.component_surface_offset(3, 2, 2) == Approx(0.05f));
        CHECK(mgr.component_surface_offset(3, 2, 3) == Approx(0.0f));
    }

    {
        const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.05f, 0.4f);
        row.component_a_surface_offset = offset_a;
        row.component_b_surface_offset = offset_b;
        set_legacy_row(mgr, 0, row, colors);

        CHECK(mgr.component_surface_offset(3, 2, 0) == Approx(0.05f));
        CHECK(mgr.component_surface_offset(3, 2, 1) == Approx(0.0f));
        CHECK(mgr.component_surface_offset(3, 2, 2) == Approx(0.05f));
        CHECK(mgr.component_surface_offset(3, 2, 3) == Approx(0.0f));
    }
}

TEST_CASE("Multi-component mixed filament bias treats the first component against the remaining group", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00", "#0000FF"};
    MixedFilamentDefinition definition;
    definition.source.kind = MixedFilamentSourceKind::Custom;
    definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components = {{{1}, 40}, {{2}, 30}, {{3}, 30}};
    definition.behavior.distribution = MixedFilamentDistributionMode::Simple;

    MixedFilamentPreviewSettings preview_settings;
    CHECK(mixed_filament_supports_bias_apparent_color(definition, preview_settings, true));

    const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.04f, 0.4f);
    definition.behavior.surface_bias.component_a_offset_mm = offset_a;
    definition.behavior.surface_bias.component_b_offset_mm = offset_b;
    CHECK(MixedFilamentManager::apparent_component_percentages({40, 30, 30}, offset_a, offset_b, 0.4f) ==
          std::vector<int>{50, 25, 25});

    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions({definition}, colors);
    std::set<unsigned int> resolved_components;
    for (int layer_idx = 0; layer_idx < 20; ++layer_idx) {
        const unsigned int resolved = manager.resolve(4, 3, layer_idx);
        resolved_components.insert(resolved);
        CHECK(manager.component_surface_offset(4, 3, layer_idx) == Approx(resolved == 1 ? 0.0f : 0.04f));
    }
    CHECK(resolved_components == std::set<unsigned int>{1, 2, 3});

    const auto [negative_a, negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.04f, 0.4f);
    definition.behavior.surface_bias.component_a_offset_mm = negative_a;
    definition.behavior.surface_bias.component_b_offset_mm = negative_b;
    CHECK(MixedFilamentManager::apparent_component_percentages({40, 30, 30}, negative_a, negative_b, 0.4f) ==
          std::vector<int>{30, 35, 35});
    manager.set_mixed_filament_definitions({definition}, colors);
    for (int layer_idx = 0; layer_idx < 20; ++layer_idx) {
        const unsigned int resolved = manager.resolve(4, 3, layer_idx);
        CHECK(manager.component_surface_offset(4, 3, layer_idx) == Approx(resolved == 1 ? 0.04f : 0.0f));
    }
}

TEST_CASE("Multi-component mixed filament bias stores and applies every component independently", "[MixedFilament][FullSpectrum3mf]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00", "#0000FF"};
    const std::vector<std::string> refs = {"fil_red", "fil_yellow", "fil_blue"};

    MixedFilamentDefinition definition;
    definition.identity.stable_id = 6103;
    definition.source.kind = MixedFilamentSourceKind::Custom;
    definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components = {{{1}, 40}, {{2}, 30}, {{3}, 30}};
    definition.behavior.distribution = MixedFilamentDistributionMode::Simple;
    set_mixed_filament_component_surface_offsets(definition, {0.02f, 0.04f, -0.02f});

    CHECK(mixed_filament_component_surface_offsets(definition) == std::vector<float>{0.02f, 0.04f, -0.02f});
    CHECK(MixedFilamentManager::apparent_component_percentages(
              {40, 30, 30}, mixed_filament_component_surface_offsets(definition), 0.4f) ==
          std::vector<int>{38, 20, 42});

    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions({definition}, colors);
    std::set<unsigned int> resolved_components;
    for (int layer_idx = 0; layer_idx < 40; ++layer_idx) {
        const unsigned int resolved = manager.resolve(4, 3, layer_idx);
        resolved_components.insert(resolved);
        const float expected_offset = resolved == 1 ? 0.02f : (resolved == 2 ? 0.04f : -0.02f);
        CHECK(manager.component_surface_offset(4, 3, layer_idx) == Approx(expected_offset));
    }
    CHECK(resolved_components == std::set<unsigned int>{1, 2, 3});

    const std::string legacy = manager.serialize_custom_entries();
    CHECK(legacy.find(",xv0.02/0.04/-0.02") != std::string::npos);
    MixedFilamentManager legacy_rebuilt;
    legacy_rebuilt.load_custom_entries(legacy, colors);
    const std::vector<MixedFilamentDefinition> legacy_definitions = legacy_rebuilt.mixed_filament_definitions(colors.size());
    REQUIRE(legacy_definitions.size() == 1);
    CHECK(mixed_filament_component_surface_offsets(legacy_definitions.front()) == std::vector<float>{0.02f, 0.04f, -0.02f});

    const MixedFilaments canonical = mixed_filaments_from_manager(manager, refs);
    REQUIRE(canonical.virtual_filaments.size() == 1);
    REQUIRE(canonical.virtual_filaments.front().gradient);
    CHECK_FALSE(canonical.virtual_filaments.front().gradient->enabled);
    CHECK(canonical.virtual_filaments.front().distribution.mode == "simple");
    CHECK(canonical.virtual_filaments.front().surface_bias.component_refs == refs);
    REQUIRE(canonical.virtual_filaments.front().surface_bias.component_offsets_mm.size() == 3);
    CHECK(canonical.virtual_filaments.front().surface_bias.component_offsets_mm[0] == Approx(0.02));
    CHECK(canonical.virtual_filaments.front().surface_bias.component_offsets_mm[1] == Approx(0.04));
    CHECK(canonical.virtual_filaments.front().surface_bias.component_offsets_mm[2] == Approx(-0.02));

    const MixedFilaments parsed = parse_json<MixedFilaments>(serialize_json(canonical));
    const MixedFilamentManager canonical_rebuilt = manager_from_mixed_filaments(parsed, colors, refs);
    const std::vector<MixedFilamentDefinition> canonical_definitions = canonical_rebuilt.mixed_filament_definitions(colors.size());
    REQUIRE(canonical_definitions.size() == 1);
    CHECK(canonical_definitions.front().behavior.distribution == MixedFilamentDistributionMode::Simple);
    CHECK(canonical_definitions.front().recipe.blend.component_ids(colors.size()) == std::vector<unsigned int>{1, 2, 3});
    CHECK(mixed_filament_component_surface_offsets(canonical_definitions.front()) == std::vector<float>{0.02f, 0.04f, -0.02f});
}

TEST_CASE("Mixed filament gradients round-trip under a comma decimal locale", "[MixedFilament][Locale]")
{
    ScopedCommaNumericLocale locale;
    if (!locale.active()) {
        WARN("No German comma-decimal locale is installed; locale regression test skipped");
        return;
    }

    char localized_number[16] = {};
    std::snprintf(localized_number, sizeof(localized_number), "%.1f", 0.5);
    REQUIRE(std::string(localized_number) == "0,5");

    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentDefinition definition;
    definition.identity.stable_id                    = 6203;
    definition.source.kind                           = MixedFilamentSourceKind::Custom;
    definition.recipe.kind                           = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components               = {{{1}, 45}, {{2}, 30}, {{3}, 25}};
    definition.behavior.distribution                 = MixedFilamentDistributionMode::LayerCycle;
    definition.behavior.gradient.enabled             = true;
    definition.behavior.gradient.component_a_start   = 0.90f;
    definition.behavior.gradient.component_a_end     = 0.10f;
    definition.behavior.gradient.stop_positions      = {0.0f, 0.20f, 0.45f, 0.70f, 1.0f};
    set_mixed_filament_component_surface_offsets(definition, {0.02f, 0.04f, -0.02f});

    MixedFilamentManager manager;
    REQUIRE(manager.add_custom_filament_definition(definition, colors));

    const std::string serialized = manager.serialize_custom_entries();
    CHECK(serialized.find(",p0.0000/0.2000/0.4500/0.7000/1.0000") != std::string::npos);
    CHECK(serialized.find(",r1/0.9000/0.1000") != std::string::npos);
    CHECK(serialized.find(",xv0.02/0.04/-0.02") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    const std::vector<MixedFilamentDefinition> definitions = loaded.mixed_filament_definitions(colors.size());
    REQUIRE(definitions.size() == 1);

    const MixedFilamentDefinition &round_tripped = definitions.front();
    CHECK(round_tripped.behavior.gradient.enabled);
    CHECK(round_tripped.behavior.gradient.component_a_start == Approx(0.90f));
    CHECK(round_tripped.behavior.gradient.component_a_end == Approx(0.10f));
    REQUIRE(round_tripped.behavior.gradient.stop_positions.size() == 5);
    CHECK(round_tripped.behavior.gradient.stop_positions[1] == Approx(0.20f));
    CHECK(round_tripped.behavior.gradient.stop_positions[2] == Approx(0.45f));
    CHECK(round_tripped.behavior.gradient.stop_positions[3] == Approx(0.70f));
    CHECK(mixed_filament_component_surface_offsets(round_tripped) == std::vector<float>{0.02f, 0.04f, -0.02f});

    SECTION("hardcoded dot-decimal metadata loads independently of the writer")
    {
        const std::string dot_decimal_row =
            "1,2,1,1,50,0,g12,w50/50,m2,z2,xa+0.15,xb-0.1,xv+0.15/-0.1,d0,o0,u42,"
            "p0.0000/+0.5000/1.0000,cm3,r1/+0.8000/0.2000";

        MixedFilamentManager hardcoded;
        hardcoded.load_custom_entries(dot_decimal_row, colors);
        const std::vector<MixedFilamentLegacyRow> &rows = hardcoded.mixed_filament_legacy_rows();
        REQUIRE(rows.size() == 1);

        const MixedFilamentLegacyRow &row = rows.front();
        CHECK(row.gradient_enabled);
        CHECK(row.gradient_start == Approx(0.80f));
        CHECK(row.gradient_end == Approx(0.20f));
        CHECK(row.component_a_surface_offset == Approx(0.15f));
        CHECK(row.component_b_surface_offset == Approx(-0.10f));
        CHECK(row.gradient_stop_positions == "0.0000/0.5000/1.0000");

        const std::vector<MixedFilamentDefinition> hardcoded_definitions = hardcoded.mixed_filament_definitions(colors.size());
        REQUIRE(hardcoded_definitions.size() == 1);
        CHECK(mixed_filament_component_surface_offsets(hardcoded_definitions.front()) == std::vector<float>{0.15f, -0.10f});
    }

    SECTION("malformed and non-finite metadata is rejected without partial parsing")
    {
        const std::string malformed_row =
            "1,2,1,1,50,0,g12,w50/50,m2,z2,xa0.15garbage,xbinf,xv0.15/inf,d0,o0,u43,"
            "p0.0000/0.5000garbage/1.0000,cm3,r1/0.8000/0.2000";

        MixedFilamentManager malformed;
        malformed.load_custom_entries(malformed_row, colors);
        const std::vector<MixedFilamentLegacyRow> &rows = malformed.mixed_filament_legacy_rows();
        REQUIRE(rows.size() == 1);
        CHECK(rows.front().component_a_surface_offset == Approx(0.f));
        CHECK(rows.front().component_b_surface_offset == Approx(0.f));
        CHECK(rows.front().component_surface_offsets.empty());
        CHECK(rows.front().gradient_stop_positions.empty());

        const std::string malformed_gradient_row =
            "1,2,1,1,50,0,g12,w50/50,m2,z2,xa0.15,xb-0.1,d0,o0,u44,cm3,r1/0.8000garbage/0.2000";
        MixedFilamentManager malformed_gradient;
        malformed_gradient.load_custom_entries(malformed_gradient_row, colors);
        REQUIRE(malformed_gradient.mixed_filament_legacy_rows().size() == 1);
        CHECK_FALSE(malformed_gradient.mixed_filament_legacy_rows().front().gradient_enabled);

        const std::string non_finite_gradient_row =
            "1,2,1,1,50,0,g12,w50/50,m2,z2,xa0.15,xb-0.1,d0,o0,u45,cm3,r1/inf/0.2000";
        MixedFilamentManager non_finite_gradient;
        non_finite_gradient.load_custom_entries(non_finite_gradient_row, colors);
        REQUIRE(non_finite_gradient.mixed_filament_legacy_rows().size() == 1);
        CHECK_FALSE(non_finite_gradient.mixed_filament_legacy_rows().front().gradient_enabled);
    }
}

TEST_CASE("Surface-bias encoding reproduces requested apparent component percentages", "[MixedFilament][SurfaceBias]")
{
    SECTION("two components") {
        const std::vector<int> actual = {50, 50};
        const std::vector<int> target = {75, 25};
        const std::vector<float> offsets = mixed_filament_surface_offsets_for_apparent_percentages(actual, target, 0.4f);
        REQUIRE(offsets.size() == 2);
        CHECK(offsets[0] == Approx(-0.05f));
        CHECK(offsets[1] == Approx(0.05f));
        CHECK(MixedFilamentManager::apparent_component_percentages(actual, offsets, 0.4f) == target);
    }

    SECTION("four components") {
        const std::vector<int> actual = {25, 25, 25, 25};
        const std::vector<int> target = {55, 25, 15, 5};
        const std::vector<float> offsets = mixed_filament_surface_offsets_for_apparent_percentages(actual, target, 0.4f);
        REQUIRE(offsets.size() == 4);
        CHECK(MixedFilamentManager::apparent_component_percentages(actual, offsets, 0.4f) == target);
    }

    SECTION("fractional residual keeps the normal cadence")
    {
        const std::vector<int>   base    = {60, 40};
        const std::vector<float> offsets = mixed_filament_surface_offsets_for_apparent_weights(base, {59.4, 40.6}, 0.4f);
        REQUIRE(offsets.size() == 2);
        CHECK(offsets[0] == Approx(0.0012f));
        CHECK(offsets[1] == Approx(-0.0012f));
        CHECK(std::abs(offsets[0]) < 0.01f);
        CHECK(std::abs(offsets[1]) < 0.01f);
    }

    CHECK(mixed_filament_surface_offsets_for_apparent_percentages({50, 50}, {100}, 0.4f).empty());
}

TEST_CASE("Perimeter modulation moves only centerline points near the object boundary", "[MixedFilament][PerimeterModulation]")
{
    ExPolygon slice;
    slice.contour.points = {
        Point(scale_(0.0), scale_(0.0)),
        Point(scale_(10.0), scale_(0.0)),
        Point(scale_(10.0), scale_(10.0)),
        Point(scale_(0.0), scale_(10.0))
    };
    const ExPolygons layer_slices{slice};

    Polyline contracted({Point(scale_(0.2), scale_(1.0)), Point(scale_(0.2), scale_(9.0))});
    REQUIRE(apply_mixed_filament_perimeter_modulation(contracted, layer_slices, 0.1f, 0.2f, 0.6f));
    CHECK(unscale<double>(contracted.points.front().x()) == Approx(0.3).margin(1e-4));
    CHECK(unscale<double>(contracted.points.back().x()) == Approx(0.3).margin(1e-4));

    Polyline expanded({Point(scale_(0.2), scale_(1.0)), Point(scale_(0.2), scale_(9.0))});
    REQUIRE(apply_mixed_filament_perimeter_modulation(expanded, layer_slices, -0.1f, 0.2f, 0.6f));
    CHECK(unscale<double>(expanded.points.front().x()) == Approx(0.1).margin(1e-4));

    Polyline internal({Point(scale_(5.0), scale_(1.0)), Point(scale_(5.0), scale_(9.0))});
    CHECK_FALSE(apply_mixed_filament_perimeter_modulation(internal, layer_slices, -0.1f, 0.2f, 0.6f));
    CHECK(unscale<double>(internal.points.front().x()) == Approx(5.0).margin(1e-4));
}

TEST_CASE("Perimeter image-map definitions share one physical layer sequence and persist their application", "[MixedFilament][PerimeterModulation]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF", "#FFFF00", "#000000"};
    const std::vector<std::string> refs = {"fil_c", "fil_m", "fil_y", "fil_k"};

    auto make_definition = [](const std::vector<float> &offsets, const std::string &display_color) {
        MixedFilamentDefinition definition;
        definition.source.kind = MixedFilamentSourceKind::Custom;
        definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
        definition.recipe.blend.components = {{{1}, 25}, {{2}, 25}, {{3}, 25}, {{4}, 25}};
        definition.behavior.distribution = MixedFilamentDistributionMode::LayerCycle;
        definition.behavior.surface_bias.perimeter_modulation = true;
        set_mixed_filament_component_surface_offsets(definition, offsets);
        definition.presentation.display_color = display_color;
        return definition;
    };

    MixedFilamentManager manager;
    REQUIRE(manager.add_custom_filament_definition(make_definition({-0.08f, 0.02f, 0.03f, 0.03f}, "#35A8A8"), colors));
    REQUIRE(manager.add_custom_filament_definition(make_definition({0.04f, -0.08f, 0.02f, 0.02f}, "#A835A8"), colors));

    for (int layer_index = 0; layer_index < 16; ++layer_index)
        CHECK(manager.resolve(5, colors.size(), layer_index) == manager.resolve(6, colors.size(), layer_index));

    const std::string legacy = manager.serialize_custom_entries();
    CHECK(legacy.find(",xp1") != std::string::npos);
    MixedFilamentManager legacy_rebuilt;
    legacy_rebuilt.load_custom_entries(legacy, colors);
    const std::vector<MixedFilamentDefinition> legacy_definitions = legacy_rebuilt.mixed_filament_definitions(colors.size());
    REQUIRE(legacy_definitions.size() == 2);
    CHECK(legacy_definitions[0].behavior.surface_bias.perimeter_modulation);
    CHECK(legacy_definitions[1].behavior.surface_bias.perimeter_modulation);

    const MixedFilaments canonical = mixed_filaments_from_manager(manager, refs);
    REQUIRE(canonical.virtual_filaments.size() == 2);
    CHECK(canonical.virtual_filaments[0].surface_bias.application == "external_perimeter");
    const MixedFilaments parsed = parse_json<MixedFilaments>(serialize_json(canonical));
    const MixedFilamentManager canonical_rebuilt = manager_from_mixed_filaments(parsed, colors, refs);
    const std::vector<MixedFilamentDefinition> canonical_definitions = canonical_rebuilt.mixed_filament_definitions(colors.size());
    REQUIRE(canonical_definitions.size() == 2);
    CHECK(canonical_definitions[0].behavior.surface_bias.perimeter_modulation);
    CHECK(canonical_definitions[1].behavior.surface_bias.perimeter_modulation);
}

TEST_CASE("Perimeter-modulated recipes use strict equal layer cadence", "[MixedFilament][PerimeterModulation]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF", "#FFFF00", "#000000"};

    auto add_recipe = [&colors](MixedFilamentManager &manager, std::vector<MixedFilamentWeightedComponent> components) {
        MixedFilamentDefinition definition;
        definition.source.kind                                = MixedFilamentSourceKind::Custom;
        definition.recipe.kind                                = MixedFilamentRecipeKind::WeightedBlend;
        definition.recipe.blend.components                    = std::move(components);
        definition.behavior.distribution                      = MixedFilamentDistributionMode::LayerCycle;
        definition.behavior.surface_bias.perimeter_modulation = true;
        return manager.add_custom_filament_definition(std::move(definition), colors);
    };

    SECTION("pair is 1:1 even when exposure weights are unequal")
    {
        MixedFilamentManager manager;
        REQUIRE(add_recipe(manager, {{{1}, 85}, {{2}, 15}}));
        for (int layer = 0; layer < 8; ++layer) {
            CHECK(manager.resolve(5, colors.size(), layer) == unsigned(layer % 2 + 1));
            CHECK(manager.resolve_perimeter(5, colors.size(), layer, 0) == unsigned(layer % 2 + 1));
        }
    }

    SECTION("triple is 1:1:1")
    {
        MixedFilamentManager manager;
        REQUIRE(add_recipe(manager, {{{1}, 70}, {{2}, 20}, {{3}, 10}}));
        for (int layer = 0; layer < 12; ++layer) {
            CHECK(manager.resolve(5, colors.size(), layer) == unsigned(layer % 3 + 1));
            CHECK(manager.resolve_perimeter(5, colors.size(), layer, 1) == unsigned(layer % 3 + 1));
        }
    }

    SECTION("four-way is 1:1:1:1")
    {
        MixedFilamentManager manager;
        REQUIRE(add_recipe(manager, {{{1}, 55}, {{2}, 25}, {{3}, 15}, {{4}, 5}}));
        for (int layer = 0; layer < 16; ++layer) {
            CHECK(manager.resolve(5, colors.size(), layer) == unsigned(layer % 4 + 1));
            CHECK(manager.resolve_perimeter(5, colors.size(), layer, 2) == unsigned(layer % 4 + 1));
        }
    }
}

TEST_CASE("Mixed filament auto generation can be disabled without dropping custom rows", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentManager enabled_mgr;
    enabled_mgr.auto_generate(colors);
    REQUIRE(enabled_mgr.mixed_filament_legacy_rows().size() == 3);
    const std::string serialized_auto_rows = enabled_mgr.serialize_custom_entries();

    MixedAutoGenerateGuard guard(false);

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    mgr.auto_generate(colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);
    CHECK(mgr.mixed_filament_legacy_rows().front().custom);
    CHECK(mgr.mixed_filament_legacy_rows().front().component_a == 1);
    CHECK(mgr.mixed_filament_legacy_rows().front().component_b == 2);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized_auto_rows, colors);
    CHECK(loaded.mixed_filament_legacy_rows().empty());
}

TEST_CASE("Mixed filament typed definition resolves legacy manual pattern tokens", "[MixedFilament]")
{
    MixedFilamentLegacyRow row;
    row.component_a = 4;
    row.component_b = 2;
    row.stable_id = 123;
    row.custom = true;
    row.origin_auto = false;
    row.mix_b_percent = 50;
    row.ratio_a = 3;
    row.ratio_b = 1;
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("1/2, 3/1");
    row.local_z_max_sublayers = 5;
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.01f;
    row.display_color = "#123456";

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 9);
    CHECK(definition.identity.stable_id == 123);
    CHECK(definition.source.kind == MixedFilamentSourceKind::Custom);
    CHECK(definition.recipe.kind == MixedFilamentRecipeKind::ManualPattern);
    REQUIRE(definition.recipe.blend.components.size() == 3);
    CHECK(definition.recipe.blend.components[0].filament.id == 4);
    CHECK(definition.recipe.blend.components[0].percent == 50);
    CHECK(definition.recipe.blend.components[1].filament.id == 2);
    CHECK(definition.recipe.blend.components[1].percent == 25);
    CHECK(definition.recipe.blend.components[2].filament.id == 3);
    CHECK(definition.recipe.blend.components[2].percent == 25);
    CHECK(definition.behavior.layer_cadence.component_a_layers == 3);
    CHECK(definition.behavior.layer_cadence.component_b_layers == 1);
    CHECK(definition.behavior.local_z.max_sublayers == 5);
    CHECK(definition.behavior.surface_bias.component_a_offset_mm == Approx(0.02f));
    CHECK(definition.behavior.surface_bias.component_b_offset_mm == Approx(-0.01f));
    CHECK(definition.presentation.display_color == "#123456");

    REQUIRE(definition.recipe.manual_pattern);
    REQUIRE(definition.recipe.manual_pattern->groups.size() == 2);
    REQUIRE(definition.recipe.manual_pattern->groups[0].size() == 2);
    REQUIRE(definition.recipe.manual_pattern->groups[1].size() == 2);
    CHECK(definition.recipe.manual_pattern->groups[0][0].id == 4);
    CHECK(definition.recipe.manual_pattern->groups[0][1].id == 2);
    CHECK(definition.recipe.manual_pattern->groups[1][0].id == 3);
    CHECK(definition.recipe.manual_pattern->groups[1][1].id == 4);

    CHECK(mixed_filament_manual_pattern_sequence(definition, 9) == std::vector<unsigned int>{4, 2, 3, 4});
    CHECK(mixed_filament_manual_pattern_preview_sequence(definition, 9, 2) == std::vector<unsigned int>{4, 3, 2, 4});

    const MixedFilamentLegacyRow rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.component_a == 4);
    CHECK(rebuilt.component_b == 2);
    CHECK(rebuilt.manual_pattern == "12,31");
    CHECK(rebuilt.mix_b_percent == 25);
    CHECK(rebuilt.gradient_component_ids.empty());
    CHECK(rebuilt.gradient_component_weights.empty());
}

TEST_CASE("Mixed filament manual pattern aggregate keeps legacy primary pair", "[MixedFilament]")
{
    MixedFilamentLegacyRow row;
    row.component_a = 1;
    row.component_b = 2;
    row.mix_b_percent = 50;
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("2,12");

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 2);
    REQUIRE(definition.recipe.manual_pattern);
    const MixedFilamentPrimaryPairView pair = definition.recipe.blend.primary_pair_or(0, 0);
    CHECK(pair.component_a.id == 1);
    CHECK(pair.component_b.id == 2);
    CHECK(definition.recipe.blend.component_ids(2) == std::vector<unsigned int>{1, 2});
    CHECK(definition.recipe.blend.component_percents(2) == std::vector<int>{33, 67});

    const MixedFilamentLegacyRow rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.component_a == 1);
    CHECK(rebuilt.component_b == 2);
    CHECK(rebuilt.manual_pattern == "2,12");
}

TEST_CASE("Mixed filament typed definition exposes weighted blend components and weights", "[MixedFilament]")
{
    MixedFilamentLegacyRow row;
    row.component_a = 1;
    row.component_b = 2;
    row.stable_id = 456;
    row.custom = true;
    row.gradient_component_ids = "312";
    row.gradient_component_weights = "50/25/25";
    row.distribution_mode = int(MixedFilamentLegacyRow::LayerCycle);

    const MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 9);
    CHECK(definition.recipe.kind == MixedFilamentRecipeKind::WeightedBlend);
    CHECK(definition.behavior.distribution == MixedFilamentDistributionMode::LayerCycle);

    REQUIRE(definition.recipe.blend.components.size() == 3);
    CHECK(definition.recipe.blend.components[0].filament.id == 1);
    CHECK(definition.recipe.blend.components[0].percent == 25);
    CHECK(definition.recipe.blend.components[1].filament.id == 2);
    CHECK(definition.recipe.blend.components[1].percent == 25);
    CHECK(definition.recipe.blend.components[2].filament.id == 3);
    CHECK(definition.recipe.blend.components[2].percent == 50);
    CHECK(definition.recipe.blend.component_ids(9) == std::vector<unsigned int>{1, 2, 3});
    CHECK(definition.recipe.blend.component_percents(9) == std::vector<int>{25, 25, 50});
    CHECK(definition.recipe.blend.component_ids(2) == std::vector<unsigned int>{1, 2});
    CHECK(definition.recipe.blend.component_percents(2) == std::vector<int>{25, 25});

    const MixedFilamentLegacyRow rebuilt = mixed_filament_legacy_row_from_definition(definition);
    CHECK(rebuilt.gradient_component_ids == "123");
    CHECK(rebuilt.gradient_component_weights == "25/25/50");
    CHECK(rebuilt.manual_pattern.empty());
}

TEST_CASE("Mixed filament legacy gradient decoding filters unavailable physical components", "[MixedFilament]")
{
    MixedFilamentLegacyRow row;
    row.component_a = 1;
    row.component_b = 2;
    row.mix_b_percent = 50;
    row.gradient_component_ids = "1235";
    row.gradient_component_weights = "10/20/30/40";
    row.distribution_mode = int(MixedFilamentLegacyRow::LayerCycle);

    MixedFilamentDefinition definition = mixed_filament_definition_from_legacy_row(row, 3);
    CHECK(definition.recipe.blend.component_ids(3) == std::vector<unsigned int>{1, 2, 3});
    CHECK(definition.recipe.blend.component_percents(3) == std::vector<int>{17, 33, 50});
    CHECK(definition.behavior.distribution == MixedFilamentDistributionMode::LayerCycle);

    row.gradient_component_ids = "125";
    row.gradient_component_weights = "10/20/70";
    definition = mixed_filament_definition_from_legacy_row(row, 2);
    CHECK(definition.recipe.blend.component_ids(2) == std::vector<unsigned int>{1, 2});
    CHECK(definition.behavior.distribution == MixedFilamentDistributionMode::LayerCycle);

    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("125");
    definition = mixed_filament_definition_from_legacy_row(row, 2);
    CHECK(mixed_filament_manual_pattern_sequence(definition, 2) == std::vector<unsigned int>{1, 2});
}

TEST_CASE("Mixed filament manager accepts typed definitions at the boundary", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentDefinition definition;
    definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend = MixedFilamentWeightedBlend{
        {
            {MixedFilamentPhysicalRef{1}, 50},
            {MixedFilamentPhysicalRef{2}, 25},
            {MixedFilamentPhysicalRef{3}, 25}
        }
    };
    definition.behavior.distribution = MixedFilamentDistributionMode::LayerCycle;

    MixedFilamentManager mgr;
    REQUIRE(mgr.add_custom_filament_definition(definition, colors));
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    const std::vector<MixedFilamentDefinition> definitions = mgr.mixed_filament_definitions(colors.size());
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().source.kind == MixedFilamentSourceKind::Custom);
    CHECK(definitions.front().identity.stable_id != 0);
    CHECK(definitions.front().recipe.blend.component_ids(colors.size()) == std::vector<unsigned int>{1, 2, 3});

    MixedFilamentDefinition edited = definitions.front();
    edited.recipe.blend.components = {
        {MixedFilamentPhysicalRef{1}, 35},
        {MixedFilamentPhysicalRef{2}, 65}
    };
    edited.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
    edited.behavior.distribution = MixedFilamentDistributionMode::Simple;
    REQUIRE(mgr.set_mixed_filament_definition(0, edited, colors));

    const auto roundtrip = mgr.mixed_filament_definition_from_id(4, colors.size());
    REQUIRE(roundtrip);
    CHECK(roundtrip->identity.stable_id == definitions.front().identity.stable_id);
    CHECK(roundtrip->recipe.kind == MixedFilamentRecipeKind::WeightedBlend);
    CHECK(roundtrip->recipe.blend.is_pair());
    CHECK(roundtrip->recipe.blend.primary_pair_or().component_b_percent == 65);
    CHECK(roundtrip->recipe.blend.component_ids(colors.size()) == std::vector<unsigned int>{1, 2});
}

TEST_CASE("Mixed filament perimeter resolver uses grouped manual patterns by inset", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    REQUIRE(row.manual_pattern == "12,21");
    set_legacy_row(mgr, 0, row, colors);

    const unsigned int mixed_filament_id = 3;
    CHECK(mgr.resolve(mixed_filament_id, 2, 0) == 1);
    CHECK(mgr.resolve(mixed_filament_id, 2, 1) == 2);

    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 0) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 1) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 1) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 3) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 3) == 1);

    const std::vector<unsigned int> ordered_layer0 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 0);
    const std::vector<unsigned int> ordered_layer1 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 1);
    REQUIRE(ordered_layer0.size() == 2);
    REQUIRE(ordered_layer1.size() == 2);
    CHECK(ordered_layer0[0] == 1);
    CHECK(ordered_layer0[1] == 2);
    CHECK(ordered_layer1[0] == 2);
    CHECK(ordered_layer1[1] == 1);
}

TEST_CASE("Grouped manual perimeter patterns keep grouped resolution on collapsed single-tool layers", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("2,12");
    REQUIRE(row.manual_pattern == "2,12");
    set_legacy_row(mgr, 0, row, colors);

    const unsigned int mixed_filament_id = 3;

    // The flattened row cadence resolves this layer to component A, but both
    // perimeter groups collapse onto physical filament 2. G-code generation
    // and tool ordering must keep using the grouped perimeter result here.
    CHECK(mgr.resolve(mixed_filament_id, 2, 1) == 1);

    const std::vector<unsigned int> ordered_layer1 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 1);
    REQUIRE(ordered_layer1.size() == 1);
    CHECK(ordered_layer1.front() == 2);

    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 0) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 1) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 2) == 2);
}

TEST_CASE("Grouped manual perimeter patterns resolve overlapping singleton inner groups", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,1");
    REQUIRE(row.manual_pattern == "12,1");
    set_legacy_row(mgr, 0, row, colors);

    const unsigned int mixed_filament_id = 3;

    const std::vector<unsigned int> ordered_layer0 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 0);
    const std::vector<unsigned int> ordered_layer1 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 1);

    REQUIRE(ordered_layer0.size() == 1);
    CHECK(ordered_layer0.front() == 1);
    REQUIRE(ordered_layer1.size() == 2);
    CHECK(ordered_layer1[0] == 2);
    CHECK(ordered_layer1[1] == 1);

    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 1) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 0) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 1) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 2, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 2, 1) == 1);
}

TEST_CASE("Grouped manual wall patterns make infill follow the innermost perimeter tool", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,1");
    REQUIRE(row.manual_pattern == "12,1");
    set_legacy_row(mgr, 0, row, colors);

    PrintRegionConfig region_config = static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults());
    region_config.wall_filament.value                  = 3;
    region_config.wall_loops.value                     = 2;
    region_config.enable_infill_filament_override.value = false;
    region_config.sparse_infill_density.value          = 15.;
    region_config.sparse_infill_filament.value         = 2;
    region_config.solid_infill_filament.value          = 3;

    PrintRegion region(region_config);

    LayerTools layer0(0.2);
    layer0.layer_index       = 0;
    layer0.object_layer_count = 6;
    layer0.layer_height      = 0.2;
    layer0.mixed_mgr         = &mgr;
    layer0.num_physical      = 2;

    LayerTools layer1(0.4);
    layer1.layer_index       = 1;
    layer1.object_layer_count = 6;
    layer1.layer_height      = 0.2;
    layer1.mixed_mgr         = &mgr;
    layer1.num_physical      = 2;

    CHECK(layer0.wall_filament(region) == 0);
    CHECK(layer1.wall_filament(region) == 1);
    CHECK(layer0.sparse_infill_filament(region) == 0);
    CHECK(layer1.sparse_infill_filament(region) == 0);
    CHECK(layer0.solid_infill_filament(region) == 0);
    CHECK(layer1.solid_infill_filament(region) == 0);

    region_config.enable_infill_filament_override.value = true;
    region_config.sparse_infill_filament.value          = 2;
    region_config.solid_infill_filament.value           = 2;
    PrintRegion overridden_region(region_config);

    CHECK(layer0.sparse_infill_filament(overridden_region) == 1);
    CHECK(layer1.sparse_infill_filament(overridden_region) == 1);
    CHECK(layer0.solid_infill_filament(overridden_region) == 1);
    CHECK(layer1.solid_infill_filament(overridden_region) == 1);
}

TEST_CASE("Mixed filament painted-region resolver collapses ordinary mixed rows to the active physical extruder", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.ratio_a = 1;
    row.ratio_b = 1;
    row.manual_pattern.clear();
    row.distribution_mode = int(MixedFilamentLegacyRow::Simple);
    set_legacy_row(mgr, 0, row, colors);

    CHECK(mgr.effective_painted_region_filament_id(3, 2, 0) == 1);
    CHECK(mgr.effective_painted_region_filament_id(3, 2, 1) == 2);
}

TEST_CASE("Mixed filament painted-region resolver preserves virtual channels for grouped patterns", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    set_legacy_row(mgr, 0, row, colors);
    CHECK(mgr.effective_painted_region_filament_id(3, 2, 0) == 3);
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.02f;
    set_legacy_row(mgr, 0, row, colors);
    CHECK(mgr.component_surface_offset(3, 2, 0) == Approx(0.0f));
}

TEST_CASE("Mixed filament loader normalizes retired distribution rows", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF", "#FFFF00"};

    MixedFilamentManager pair_mgr;
    pair_mgr.load_custom_entries("1,2,1,1,50,0,m1,u123", colors);
    REQUIRE(pair_mgr.mixed_filament_legacy_rows().size() == 1);
    CHECK(pair_mgr.mixed_filament_legacy_rows().front().distribution_mode == int(MixedFilamentLegacyRow::Simple));

    MixedFilamentManager gradient_mgr;
    gradient_mgr.load_custom_entries("1,2,1,1,50,0,g123,w50/25/25,m1,u456", colors);
    REQUIRE(gradient_mgr.mixed_filament_legacy_rows().size() == 1);
    CHECK(gradient_mgr.mixed_filament_legacy_rows().front().distribution_mode == int(MixedFilamentLegacyRow::LayerCycle));
}

TEST_CASE("ExtrusionPath copies preserve inset index", "[MixedFilament]")
{
    ExtrusionPath src(erPerimeter);
    src.inset_idx = 3;

    ExtrusionPath copied(src);
    CHECK(copied.inset_idx == 3);

    ExtrusionPath assigned(erExternalPerimeter);
    assigned.inset_idx = 0;
    assigned = src;
    CHECK(assigned.inset_idx == 3);
}

TEST_CASE("Extrusion loop and multipath entities preserve inset index", "[MixedFilament]")
{
    ExtrusionPath src(erPerimeter);
    src.inset_idx = 2;

    ExtrusionMultiPath multi_from_path(src);
    CHECK(multi_from_path.inset_idx == 2);

    ExtrusionMultiPath multi_copy(multi_from_path);
    CHECK(multi_copy.inset_idx == 2);

    ExtrusionMultiPath multi_assigned;
    multi_assigned.inset_idx = 0;
    multi_assigned = multi_from_path;
    CHECK(multi_assigned.inset_idx == 2);

    ExtrusionLoop loop_from_path(src);
    CHECK(loop_from_path.inset_idx == 2);

    ExtrusionLoop loop_copy(loop_from_path);
    CHECK(loop_copy.inset_idx == 2);
}

TEST_CASE("FullSpectrum mixed filaments round-trip canonical grouped pattern data", "[FullSpectrum3mf]")
{
    const std::vector<std::string> refs = {"fil_red", "fil_blue", "fil_green"};
    const std::vector<std::string> colors = {"#FF0000", "#0000FF", "#00FF00"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);

    MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
    row.stable_id = 4242;
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("1/2, 2/3");
    row.gradient_component_ids = "123";
    row.gradient_component_weights = "50/25/25";
    row.distribution_mode = int(MixedFilamentLegacyRow::LayerCycle);
    row.local_z_max_sublayers = 4;
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.01f;
    set_legacy_row(mgr, 0, row, colors);

    const MixedFilaments canonical = mixed_filaments_from_manager(mgr, refs);
    REQUIRE(canonical.virtual_filaments.size() == 1);
    CHECK(canonical.virtual_filaments.front().id == "mix_4242");
    REQUIRE(canonical.virtual_filaments.front().manual_pattern);
    CHECK(canonical.virtual_filaments.front().manual_pattern->groups[1][1] == "physical:fil_green");
    REQUIRE(canonical.virtual_filaments.front().gradient);
    CHECK(canonical.virtual_filaments.front().gradient->component_refs[2] == "fil_green");
    REQUIRE(canonical.virtual_filaments.front().local_z);
    CHECK(canonical.virtual_filaments.front().local_z->max_sublayers == 4);

    const MixedFilaments parsed = parse_json<MixedFilaments>(serialize_json(canonical));
    MixedFilamentManager rebuilt = manager_from_mixed_filaments(parsed, colors, refs);
    REQUIRE(rebuilt.mixed_filament_legacy_rows().size() == 1);
    CHECK(rebuilt.mixed_filament_legacy_rows().front().stable_id == 4242);
    CHECK(rebuilt.mixed_filament_legacy_rows().front().manual_pattern == "12,23");
    CHECK(rebuilt.mixed_filament_legacy_rows().front().gradient_component_ids == "123");
    CHECK(rebuilt.mixed_filament_legacy_rows().front().gradient_component_weights == "50/25/25");
    CHECK(rebuilt.mixed_filament_legacy_rows().front().component_a_surface_offset == Approx(0.02f));
    CHECK(rebuilt.mixed_filament_legacy_rows().front().component_b_surface_offset == Approx(-0.01f));
}

TEST_CASE("FullSpectrum validation checks canonical mixed component references", "[FullSpectrum3mf]")
{
    PackageModel model;
    model.materials.physical_filaments = {
        {"fil_red", "Red"},
        {"fil_blue", "Blue"},
        {"fil_green", "Green"}
    };

    VirtualFilament vf;
    vf.id = "mix_bad_refs";
    vf.visibility_state = "ghost";
    vf.source_kind = "maybe";
    vf.origin.kind = "triple";
    vf.origin.component_refs = {"fil_red", "fil_red"};
    vf.blend.type = "unknown";
    vf.blend.component_b_percent = 120;
    vf.distribution.mode = "sparkle";
    vf.gradient = Gradient{{"fil_red", "fil_missing", "fil_red"}, {50, 25, 25}};
    vf.manual_pattern = ManualPattern{{{"component_a", "physical:fil_missing"}}};
    vf.local_z = LocalZ{-1, "standard-pair-split"};

    MixedFilaments mixed;
    mixed.virtual_filaments.push_back(std::move(vf));
    model.mixed_filaments = std::move(mixed);

    const ValidationResult result = validate_package_model(model);
    CHECK_FALSE(result.valid);
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("gradient references missing physical filament fil_missing") != std::string::npos;
    }));
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("invalid visibility state ghost") != std::string::npos;
    }));
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("invalid source kind maybe") != std::string::npos;
    }));
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("invalid origin kind triple") != std::string::npos;
    }));
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("duplicate origin component ref fil_red") != std::string::npos;
    }));
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("invalid blend type unknown") != std::string::npos;
    }));
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("blend percent is outside 0..100") != std::string::npos;
    }));
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("invalid distribution mode sparkle") != std::string::npos;
    }));
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("gradient has duplicate component ref fil_red") != std::string::npos;
    }));
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("manual pattern has invalid step physical:fil_missing") != std::string::npos;
    }));
    CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string &error) {
        return error.find("Local-Z max_sublayers is negative") != std::string::npos;
    }));
}

TEST_CASE("FullSpectrum canonical gradient keeps origin pair as primary pair", "[FullSpectrum3mf]")
{
    const std::vector<std::string> refs = {"fil_red", "fil_blue", "fil_green"};
    const std::vector<std::string> colors = {"#FF0000", "#0000FF", "#00FF00"};

    VirtualFilament vf;
    vf.id = "mix_reordered_gradient";
    vf.source_kind = "custom";
    vf.origin.component_refs = {"fil_red", "fil_blue"};
    vf.blend.component_b_percent = 65;
    vf.gradient = Gradient{{"fil_green", "fil_red", "fil_blue"}, {50, 30, 20}};
    vf.distribution.mode = "layer_cycle";

    MixedFilaments mixed;
    mixed.virtual_filaments.push_back(std::move(vf));

    const MixedFilamentManager manager = manager_from_mixed_filaments(mixed, colors, refs);
    const std::vector<MixedFilamentDefinition> definitions = manager.mixed_filament_definitions(colors.size());
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().recipe.blend.component_ids(colors.size()) == std::vector<unsigned int>{1, 2, 3});
    CHECK(definitions.front().recipe.blend.component_percents(colors.size()) == std::vector<int>{30, 20, 50});
    const std::optional<MixedFilamentPrimaryPairView> pair = definitions.front().recipe.blend.primary_pair();
    REQUIRE(pair);
    CHECK(pair->component_a.id == 1);
    CHECK(pair->component_b.id == 2);
    CHECK(pair->component_b_percent == 20);
}

TEST_CASE("FullSpectrum canonical gradient falls back to equal weights when weights are malformed", "[FullSpectrum3mf]")
{
    const std::vector<std::string> refs = {"fil_red", "fil_blue", "fil_green"};
    const std::vector<std::string> colors = {"#FF0000", "#0000FF", "#00FF00"};

    VirtualFilament vf;
    vf.id = "mix_weights";
    vf.source_kind = "custom";
    vf.origin.component_refs = {"fil_red", "fil_blue"};
    vf.gradient = Gradient{{"fil_red", "fil_blue", "fil_green"}, {80, 20}};
    vf.distribution.mode = "layer_cycle";

    MixedFilaments mixed;
    mixed.virtual_filaments.push_back(std::move(vf));

    const MixedFilamentManager manager = manager_from_mixed_filaments(mixed, colors, refs);
    const std::vector<MixedFilamentDefinition> definitions = manager.mixed_filament_definitions(colors.size());
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().recipe.blend.component_ids(colors.size()) == std::vector<unsigned int>{1, 2, 3});
    CHECK(definitions.front().recipe.blend.component_percents(colors.size()) == std::vector<int>{34, 33, 33});
}

TEST_CASE("FullSpectrum writer emits core package parts and mixed assignments", "[FullSpectrum3mf]")
{
    PresetBundle bundle = make_bundle_with_filaments({"#FF0000", "#0000FF"});

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 25, {"#FF0000", "#0000FF"});
    REQUIRE(mgr.mixed_filament_legacy_rows().size() == 1);
    {
        MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
        row.stable_id = 9001;
        set_legacy_row(mgr, 0, row, {"#FF0000", "#0000FF"});
    }
    bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions")->value = mgr.serialize_custom_entries();

    GeometryBindingInput geometry;
    geometry.project_name = "canonical writer test";
    geometry.objects.push_back({10, "obj_a"});
    geometry.volumes.push_back({10, 11, "obj_a", "vol_a", 3, {3}});

    const PackageWritePlan plan = build_write_plan(bundle.project_config, geometry, true);

    REQUIRE(find_fullspectrum_part(plan, PATH_MANIFEST) != nullptr);
    REQUIRE(find_fullspectrum_part(plan, PATH_PROJECT) != nullptr);
    REQUIRE(find_fullspectrum_part(plan, PATH_IDENTITY_MAP) != nullptr);
    REQUIRE(find_fullspectrum_part(plan, PATH_MATERIALS) != nullptr);
    REQUIRE(find_fullspectrum_part(plan, PATH_ASSIGNMENTS) != nullptr);
    REQUIRE(find_fullspectrum_part(plan, PATH_MIXED_FILAMENTS) != nullptr);

    const Manifest manifest = parse_json<Manifest>(find_fullspectrum_part(plan, PATH_MANIFEST)->bytes);
    CHECK(std::find(manifest.required_features.begin(),
                    manifest.required_features.end(),
                    std::string(FEATURE_MIXED_FILAMENTS)) != manifest.required_features.end());
    CHECK(manifest.legacy_projection.present);

    const Assignments assignments = parse_json<Assignments>(find_fullspectrum_part(plan, PATH_ASSIGNMENTS)->bytes);
    REQUIRE(assignments.assignments.size() == 1);
    CHECK(assignments.assignments.front().material_ref == "mix_9001");
    REQUIRE(assignments.paint_state_bindings.size() == 1);
    CHECK(assignments.paint_state_bindings.front().material_ref == "mix_9001");
}

TEST_CASE("FullSpectrum image maps persist authoritative sources and binary texture assets", "[FullSpectrum3mf][ImageMap]")
{
    PresetBundle bundle = make_bundle_with_filaments({"#FF0000", "#0000FF"});

    Model source_model;
    ModelObject *source_object = source_model.add_object();
    source_object->add_instance();
    ModelVolume *source_volume = source_object->add_volume(make_cube(1., 1., 1.));

    ImageMap::VolumeData data;
    data.topology_fingerprint = ImageMap::topology_fingerprint(source_volume->mesh());
    data.texture_assets.push_back({"source-texture", "checker", 1, 1, {12, 34, 56, 255}});
    ImageMap::Zone zone;
    zone.stable_id   = "zone-main";
    zone.display_name = "Main image";
    zone.render_mode = ImageMap::RenderMode::PerimeterModulationV2;
    zone.synchronize_whole_object_cadence = true;
    zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 0, 1});
    data.zones.push_back(zone);
    ImageMap::Zone adaptive_zone = zone;
    adaptive_zone.stable_id      = "zone-adaptive";
    adaptive_zone.display_name   = "Adaptive local cycles";
    adaptive_zone.render_mode    = ImageMap::RenderMode::AdaptiveLocalizedCycles;
    adaptive_zone.adaptive_modulation_mode = ImageMap::AdaptiveModulationMode::LocalZHeight;
    adaptive_zone.synchronize_whole_object_cadence = false;
    data.zones.push_back(std::move(adaptive_zone));
    ImageMap::TriangleBinding binding;
    binding.triangle_index                   = 0;
    binding.zone_index                       = 0;
    binding.source.kind                      = ImageMap::SourceKind::Texture;
    binding.source.texture_asset_index       = 0;
    binding.source.uvs                       = {Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(0.f, 1.f)};
    data.triangle_bindings.push_back(binding);
    REQUIRE(source_volume->set_image_map_data(std::move(data)));

    GeometryBindingInput geometry;
    geometry.project_name = "image map persistence";
    geometry.objects.push_back({10, "obj_image"});
    VolumeBindingInput volume_input;
    volume_input.model_object_id = 10;
    volume_input.model_volume_id = 11;
    volume_input.stable_object_id = "obj_image";
    volume_input.stable_volume_id = "vol_image";
    volume_input.extruder_id      = 1;
    volume_input.image_map_data   = source_volume->image_map_data();
    geometry.volumes.push_back(volume_input);

    const PackageWritePlan plan = build_write_plan(bundle.project_config, geometry, true);
    const PackagePartPlan *image_maps_part = find_fullspectrum_part(plan, PATH_IMAGE_MAPS);
    REQUIRE(image_maps_part != nullptr);
    const ImageMaps canonical = parse_json<ImageMaps>(image_maps_part->bytes);
    REQUIRE(canonical.volumes.size() == 1);
    REQUIRE(canonical.texture_assets.size() == 1);
    REQUIRE(canonical.volumes.front().zones.size() == 2);
    CHECK(canonical.volumes.front().stable_volume_id == "vol_image");
    CHECK(canonical.volumes.front().zones.front().render_mode == "perimeter_modulation_v2");
    CHECK(canonical.volumes.front().zones.front().adaptive_modulation_mode == "perimeter");
    CHECK(canonical.volumes.front().zones.front().synchronize_whole_object_cadence);
    CHECK(canonical.volumes.front().zones[1].render_mode == "adaptive_localized_cycles");
    CHECK(canonical.volumes.front().zones[1].adaptive_modulation_mode == "local_z_height");
    const PackagePartPlan *asset_part = find_fullspectrum_part(plan, canonical.texture_assets.front().path);
    REQUIRE(asset_part != nullptr);
    CHECK(std::vector<uint8_t>(asset_part->bytes.begin(), asset_part->bytes.end()) == std::vector<uint8_t>{12, 34, 56, 255});

    Model target_model;
    ModelObject *target_object = target_model.add_object();
    target_object->add_instance();
    ModelVolume *target_volume = target_object->add_volume(make_cube(1., 1., 1.));
    CanonicalBindingContext context;
    context.model_objects_by_3mf_id[10] = target_object;
    context.model_volumes_by_3mf_id[11] = target_volume;
    DynamicPrintConfig imported_config = bundle.project_config;
    ArchiveImportState state = import_state_from_plan(plan);
    std::string warning;
    REQUIRE(state.apply_to_model_and_config(target_model, imported_config, context, &warning));
    CHECK(warning.empty());
    REQUIRE(target_volume->has_image_map_data());
    const auto imported = target_volume->image_map_data();
    REQUIRE(imported->texture_assets.size() == 1);
    REQUIRE(imported->zones.size() == 2);
    CHECK(imported->texture_assets.front().rgba == std::vector<uint8_t>{12, 34, 56, 255});
    CHECK(imported->zones.front().render_mode == ImageMap::RenderMode::PerimeterModulationV2);
    CHECK(imported->zones.front().synchronize_whole_object_cadence);
    CHECK(imported->zones[1].render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles);
    CHECK(imported->zones.front().adaptive_modulation_mode == ImageMap::AdaptiveModulationMode::Perimeter);
    CHECK(imported->zones[1].adaptive_modulation_mode == ImageMap::AdaptiveModulationMode::LocalZHeight);
    CHECK(imported->triangle_bindings.front().source.texture_asset_index == 0);
}

TEST_CASE("FullSpectrum writer dual-writes spatial multi-filament gradients", "[FullSpectrum3mf]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle = make_bundle_with_filaments(colors);

    MixedFilamentDefinition definition;
    definition.identity.stable_id = 9301;
    definition.source.kind = MixedFilamentSourceKind::Custom;
    definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components = {
        {{1}, 45},
        {{2}, 30},
        {{3}, 25}
    };
    definition.behavior.distribution = MixedFilamentDistributionMode::LayerCycle;
    definition.behavior.gradient.enabled = true;
    definition.behavior.gradient.component_a_start = 0.90f;
    definition.behavior.gradient.component_a_end = 0.10f;
    definition.behavior.gradient.stop_positions = {0.0f, 0.20f, 0.45f, 0.70f, 1.0f};
    bundle.mixed_filaments.add_custom_filament_definition(definition, colors);

    bundle.sync_mixed_filament_definitions_to_project_config();
    const std::string legacy = bundle.project_config.opt_string("mixed_filament_definitions");
    CHECK(legacy.find("g123") != std::string::npos);
    CHECK(legacy.find("w45/30/25") != std::string::npos);
    CHECK(legacy.find(",p") != std::string::npos);
    CHECK(legacy.find(",r1/0.9000/0.1000") != std::string::npos);

    const PackageWritePlan plan = build_write_plan(bundle.project_config, {}, true);
    const Manifest manifest = parse_json<Manifest>(find_fullspectrum_part(plan, PATH_MANIFEST)->bytes);
    CHECK(std::find(manifest.required_features.begin(),
                    manifest.required_features.end(),
                    std::string(FEATURE_MIXED_GRADIENT)) != manifest.required_features.end());

    const MixedFilaments canonical = parse_json<MixedFilaments>(find_fullspectrum_part(plan, PATH_MIXED_FILAMENTS)->bytes);
    REQUIRE(canonical.virtual_filaments.size() == 1);
    REQUIRE(canonical.virtual_filaments.front().gradient);
    const Gradient &gradient = *canonical.virtual_filaments.front().gradient;
    CHECK(gradient.component_refs.size() == 3);
    CHECK(gradient.weights == std::vector<int>{45, 30, 25});
    CHECK(gradient.enabled);
    CHECK(gradient.component_a_start == Approx(0.90));
    CHECK(gradient.component_a_end == Approx(0.10));
    REQUIRE(gradient.stop_positions.size() == 5);
    CHECK(gradient.stop_positions[2] == Approx(0.45));

    DynamicPrintConfig imported_config = bundle.project_config;
    imported_config.option<ConfigOptionString>("mixed_filament_definitions")->value.clear();
    REQUIRE(apply_canonical_mixed_filaments_to_config(fullspectrum_part_map(plan), imported_config));

    MixedFilamentManager imported_manager;
    imported_manager.load_custom_entries(imported_config.opt_string("mixed_filament_definitions"), colors);
    const std::vector<MixedFilamentDefinition> imported = imported_manager.mixed_filament_definitions(colors.size());
    REQUIRE(imported.size() == 1);
    CHECK(imported.front().behavior.gradient.enabled);
    CHECK(imported.front().behavior.gradient.component_a_start == Approx(0.90f));
    CHECK(imported.front().behavior.gradient.component_a_end == Approx(0.10f));
    REQUIRE(imported.front().behavior.gradient.stop_positions.size() == 5);
    CHECK(imported.front().behavior.gradient.stop_positions[3] == Approx(0.70f));
    CHECK(imported.front().recipe.blend.component_ids(colors.size()) == std::vector<unsigned int>{1, 2, 3});
    CHECK(imported.front().recipe.blend.component_percents(colors.size()) == std::vector<int>{45, 30, 25});
}

TEST_CASE("FullSpectrum writer keeps material ids unique for duplicate filament sources", "[FullSpectrum3mf]")
{
    PresetBundle bundle = make_bundle_with_filaments({"#FF0000", "#0000FF", "#00FF00"});
    bundle.project_config.option<ConfigOptionStrings>("filament_ids", true)->values = {"shared_spool", "shared_spool", "shared_spool"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#0000FF", "#00FF00"});
    mgr.add_custom_filament(1, 3, 50, {"#FF0000", "#0000FF", "#00FF00"});
    {
        std::vector<MixedFilamentLegacyRow> rows = mgr.mixed_filament_legacy_rows();
        rows[0].stable_id = 1234;
        rows[1].stable_id = 1234;
        mgr.set_mixed_filament_legacy_rows(rows, 3, {"#FF0000", "#0000FF", "#00FF00"});
    }
    bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions")->value = mgr.serialize_custom_entries();

    const PackageWritePlan plan = build_write_plan(bundle.project_config, {}, true);

    const Materials materials = parse_json<Materials>(find_fullspectrum_part(plan, PATH_MATERIALS)->bytes);
    std::set<std::string> material_ids;
    for (const PhysicalFilament &filament : materials.physical_filaments)
        CHECK(material_ids.insert(filament.id).second);

    const MixedFilaments mixed = parse_json<MixedFilaments>(find_fullspectrum_part(plan, PATH_MIXED_FILAMENTS)->bytes);
    for (const VirtualFilament &filament : mixed.virtual_filaments)
        CHECK(material_ids.insert(filament.id).second);
}

TEST_CASE("FullSpectrum canonical mixed rows override legacy mixed definitions on import", "[FullSpectrum3mf]")
{
    PresetBundle bundle = make_bundle_with_filaments({"#FF0000", "#0000FF"});

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 25, {"#FF0000", "#0000FF"});
    {
        MixedFilamentLegacyRow row = mgr.mixed_filament_legacy_rows().front();
        row.stable_id = 777;
        set_legacy_row(mgr, 0, row, {"#FF0000", "#0000FF"});
    }
    bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions")->value = mgr.serialize_custom_entries();

    const PackageWritePlan plan = build_write_plan(bundle.project_config, {}, true);
    std::map<std::string, std::string> parts = fullspectrum_part_map(plan);

    bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions")->value = "1,2,1,1,75,0,u1";
    REQUIRE(apply_canonical_mixed_filaments_to_config(parts, bundle.project_config));

    const std::string upgraded = bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions")->value;
    CHECK(upgraded.find("u777") != std::string::npos);
    CHECK(upgraded.find(",25,") != std::string::npos);
    CHECK(upgraded.find(",75,") == std::string::npos);
}

TEST_CASE("FullSpectrum reader blocks unknown required features", "[FullSpectrum3mf]")
{
    PresetBundle bundle = make_bundle_with_filaments({"#FF0000", "#0000FF"});

    Manifest manifest;
    manifest.kind = KIND_MANIFEST;
    manifest.schema_version = PROFILE_VERSION;
    manifest.document_class = "project";
    manifest.package_id = "pkg_future";
    manifest.required_features = {FEATURE_PROJECT_CORE, "fs.future.required.v9"};

    std::map<std::string, std::string> parts;
    parts[package_path_to_zip_path(PATH_MANIFEST)] = serialize_json(manifest);

    std::string warning;
    CHECK_FALSE(apply_canonical_mixed_filaments_to_config(parts, bundle.project_config, &warning));
    CHECK(warning.find("fs.future.required.v9") != std::string::npos);
}

TEST_CASE("FullSpectrum reader rejects canonical parts with failed checksums", "[FullSpectrum3mf]")
{
    PresetBundle bundle = make_bundle_with_filaments({"#FF0000", "#0000FF"});
    const PackageWritePlan plan = build_write_plan(bundle.project_config, {}, true);

    std::map<std::string, std::string> parts = fullspectrum_part_map(plan);
    parts[package_path_to_zip_path(PATH_MATERIALS)] += "\n";

    std::string warning;
    CHECK_FALSE(apply_canonical_mixed_filaments_to_config(parts, bundle.project_config, &warning));
    CHECK(warning.find("checksum mismatch") != std::string::npos);
}

TEST_CASE("FullSpectrum reader applies canonical volume and paint assignments", "[FullSpectrum3mf]")
{
    PresetBundle bundle = make_bundle_with_filaments({"#FF0000", "#0000FF"});
    DynamicPrintConfig config = bundle.project_config;

    Model model;
    ModelObject *object = model.add_object();
    object->add_instance();
    ModelVolume *volume = object->add_volume(make_cube(1., 1., 1.));

    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder3);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    const PackageWritePlan plan = build_write_plan(make_assignment_package_model());
    ArchiveImportState state = import_state_from_plan(plan);

    CanonicalBindingContext context;
    context.model_objects_by_3mf_id[10] = object;
    context.model_volumes_by_3mf_id[77] = volume;

    std::string warning;
    REQUIRE(state.apply_to_model_and_config(model, config, context, &warning));
    CHECK(warning.empty());
    CHECK(volume->extruder_id() == 2);

    const std::vector<size_t> painted = volume->get_extruders_from_multi_material_painting();
    REQUIRE(painted.size() == 1);
    CHECK(painted.front() == 1);

    REQUIRE(model.model_info);
    const std::string object_key = std::string(MODEL_METADATA_STABLE_OBJECT_PREFIX) + std::to_string(object->id().id);
    const std::string volume_key = std::string(MODEL_METADATA_STABLE_VOLUME_PREFIX) + std::to_string(volume->id().id);
    CHECK(model.model_info->metadata_items[object_key] == "obj_a");
    CHECK(model.model_info->metadata_items[volume_key] == "vol_a");
    CHECK(model.model_info->metadata_items[MODEL_METADATA_STATUS] == "canonical-loaded");
}

TEST_CASE("FullSpectrum reader preserves optional extension parts for later re-export", "[FullSpectrum3mf]")
{
    PresetBundle bundle = make_bundle_with_filaments({"#FF0000", "#0000FF"});

    GeometryBindingInput geometry;
    geometry.project_name = "extension preservation";
    geometry.preserved_parts.push_back(PreservedPart{
        "/Metadata/extensions/com.example/solver.json",
        "application/vnd.example.solver+json",
        "solver-extension",
        "{\"solver\":\"keep-me\"}",
        false,
        true
    });

    const PackageWritePlan plan = build_write_plan(bundle.project_config, geometry, true);
    REQUIRE(find_fullspectrum_part(plan, "/Metadata/extensions/com.example/solver.json") != nullptr);

    ArchiveImportState state = import_state_from_plan(plan);
    Model model;
    DynamicPrintConfig config = bundle.project_config;
    std::string warning;
    REQUIRE(state.apply_to_model_and_config(model, config, {}, &warning));
    CHECK(warning.empty());

    const std::vector<PreservedPart> preserved = preserved_parts_from_model(model);
    REQUIRE(preserved.size() == 1);
    CHECK(preserved.front().path == "/Metadata/extensions/com.example/solver.json");
    CHECK(preserved.front().bytes == "{\"solver\":\"keep-me\"}");

    GeometryBindingInput rewrite_geometry;
    rewrite_geometry.project_name = "extension preservation";
    rewrite_geometry.preserved_parts = preserved;
    const PackageWritePlan rewritten = build_write_plan(config, rewrite_geometry, true);

    const PackagePartPlan *extension = find_fullspectrum_part(rewritten, "/Metadata/extensions/com.example/solver.json");
    REQUIRE(extension != nullptr);
    CHECK(extension->bytes == "{\"solver\":\"keep-me\"}");
    CHECK(extension->must_preserve);
}

// --------------------------------------------------------------------------
// compute_redundant_filaments
// --------------------------------------------------------------------------

namespace {

/// Build a MixedFilamentManager seeded with `n` physical colours and an
/// optional list of custom mixed rows.  Returns the manager so callers can
/// inspect its mixed_filaments() vector.
static MixedFilamentManager build_manager(size_t n,
                                          const std::vector<MixedFilament> &extra_rows = {})
{
    std::vector<std::string> colours(n, "#FF0000");
    for (size_t i = 1; i < n; ++i)
        colours[i] = "#0000FF"; // placeholder
    // Disable auto-generate so m_mixed stays exactly what we build.
    MixedAutoGenerateGuard guard(false);
    MixedFilamentManager    mgr;
    // The constructor doesn't need colours; add_custom adds mixed rows.
    // We don't need add_batch — just push rows directly via the mutable accessor.
    for (const MixedFilament &row : extra_rows) {
        MixedFilament mf = row;
        mf.enabled       = true;
        mf.deleted       = false;
        mf.custom        = true;
        mf.origin_auto   = false;
        // stable_id is left 0: these tests never serialize (load_custom_entries
        // is what assigns/validates it), and compute_redundant_filaments ignores
        // it.  Tests that need stable_id (e.g. the differential oracle) set it
        // explicitly after building.
        mgr.mixed_filaments().push_back(std::move(mf));
    }
    return mgr;
}

/// Helper to build a minimal enabled mixed row for cascade tests.
static MixedFilament make_row(unsigned int a, unsigned int b,
                              const std::string &gradient_ids = {},
                              const std::string &manual_pattern_str = {})
{
    MixedFilament mf;
    mf.component_a            = a;
    mf.component_b            = b;
    mf.enabled                = true;
    mf.deleted                = false;
    mf.gradient_component_ids = gradient_ids;
    mf.manual_pattern         = manual_pattern_str;
    mf.stable_id              = 0;
    mf.custom                 = true;
    return mf;
}

} // namespace

TEST_CASE("compute_redundant_filaments physical-only", "[MixedFilament][redundant_set]")
{
    // 4 physicals, keep {1,3} → redundant {4,2}
    auto mgr = build_manager(4);
    auto red = compute_redundant_filaments(4, {1, 3}, {}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.empty());
    REQUIRE(red.redundant_physical.size() == 2);
    CHECK(red.redundant_physical[0] == 4); // descending
    CHECK(red.redundant_physical[1] == 2);
    CHECK(red.new_num_physical == 2);
}

TEST_CASE("compute_redundant_filaments mixed-only", "[MixedFilament][redundant_set]")
{
    // 4 physicals, 1 mixed row (v5), keep all physicals but not the mixed
    auto mgr = build_manager(4, {make_row(1, 2)});
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.empty());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5); // virtual id = 4+1
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("remove_physical_filament preserves stable_id on survivors", "[MixedFilament][redundant_set]")
{
    // Regression guard for batch-match cleanup (C1 fix): cleanup marks
    // redundant mixed rows by stable_id AFTER physical deletion rebuilds
    // m_mixed, so it relies on remove_physical_filament keeping each
    // survivor's stable_id unchanged (it edits components/pattern only).
    //   row A (1,2) stable_id=100 — references physical 2 -> DROPPED
    //   row B (1,3) stable_id=200 — survives; component 3 > 2 -> renumbered to 2
    auto mgr = build_manager(4, {make_row(1, 2), make_row(1, 3)});
    auto &rows = mgr.mixed_filaments();
    REQUIRE(rows.size() == 2);
    rows[0].stable_id = 100;
    rows[1].stable_id = 200;

    mgr.remove_physical_filament(2); // delete physical 2 (1-based)

    REQUIRE(rows.size() == 1);           // A dropped; B survives the rebuild
    CHECK(rows[0].stable_id == 200);     // identity preserved through rebuild
    CHECK(rows[0].component_a == 1);
    CHECK(rows[0].component_b == 2);     // 3 > deleted 2 -> decremented to 2
}

TEST_CASE("compute_redundant_filaments cascade component-a", "[MixedFilament][redundant_set]")
{
    // 4 physical, keep {1,2,3} (drop 4). Mixed row component_a=4 → cascade
    auto mgr = build_manager(4, {make_row(4, 1)});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 4);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments cascade component-b", "[MixedFilament][redundant_set]")
{
    // 4 physical, keep {1,2,3}. Mixed row component_b=4 → cascade
    auto mgr = build_manager(4, {make_row(1, 4)});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments cascade gradient-ids", "[MixedFilament][redundant_set]")
{
    // 4 physical, keep {1,2,3}. Mixed row gradient_component_ids="14" → cascade
    auto mgr = build_manager(4, {make_row(1, 2, "14")});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments survivor-floor", "[MixedFilament][redundant_set]")
{
    // Empty kept set → survivor floor keeps only filament 1
    auto mgr = build_manager(4);
    auto red = compute_redundant_filaments(4, {}, {}, mgr.mixed_filaments());
    CHECK(red.new_num_physical == 1);
    REQUIRE(red.redundant_physical.size() == 3);
    CHECK(red.redundant_physical[0] == 4); // descending
    CHECK(red.redundant_physical[1] == 3);
    CHECK(red.redundant_physical[2] == 2);
}

TEST_CASE("compute_redundant_filaments keep-all", "[MixedFilament][redundant_set]")
{
    // 3 physical + 1 mixed, all kept
    auto mgr = build_manager(3, {make_row(1, 2)});
    auto red = compute_redundant_filaments(3, {1,2,3}, {4}, mgr.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    CHECK(red.redundant_mixed.empty());
}

TEST_CASE("compute_redundant_filaments empty-mixed", "[MixedFilament][redundant_set]")
{
    // No mixed rows → no crash
    auto mgr = build_manager(2);
    auto red = compute_redundant_filaments(2, {1}, {}, mgr.mixed_filaments());
    CHECK(red.redundant_physical.size() == 1);
    CHECK(red.redundant_mixed.empty());
}

TEST_CASE("compute_redundant_filaments out-of-range ids", "[MixedFilament][redundant_set]")
{
    // kept_physical_ids={99} → filtered to empty → floor → filament 1 only
    auto mgr = build_manager(4);
    auto red = compute_redundant_filaments(4, {99}, {999}, mgr.mixed_filaments());
    CHECK(red.new_num_physical == 1);
    REQUIRE(red.redundant_physical.size() == 3);
    CHECK(red.redundant_physical[0] == 4); // descending
    CHECK(red.redundant_physical[1] == 3);
    CHECK(red.redundant_physical[2] == 2);
    CHECK(red.redundant_mixed.empty());
}

TEST_CASE("compute_redundant_filaments modern gradient-id format", "[MixedFilament][redundant_set]")
{
    // "/" separated multi-digit IDs
    auto mgr = build_manager(12, {make_row(1, 2, "1/12/3")});
    auto red = compute_redundant_filaments(12, {1,2,3,4,5,6,7,8,9,10,11}, {13}, mgr.mixed_filaments());
    // Physical 12 is not kept → gradient contains 12 → cascade
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 12);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("add_batch assigned_ids matches virtual-id enumeration", "[MixedFilament][batch_match]")
{
    // 4 physicals, no pre-existing mixed rows.
    std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    MixedAutoGenerateGuard    guard(false);
    MixedFilamentManager      mgr;

    std::vector<MixedFilamentBatchEntry> entries(2);
    entries[0].component_a   = 1;
    entries[0].component_b   = 2;
    entries[0].mix_b_percent = 50;
    entries[1].component_a   = 3;
    entries[1].component_b   = 4;
    entries[1].mix_b_percent = 30;

    std::vector<unsigned int> assigned_ids;
    mgr.add_batch_custom_filaments(entries, colors, &assigned_ids);

    REQUIRE(assigned_ids.size() == 2);
    CHECK(assigned_ids[0] == 5);
    CHECK(assigned_ids[1] == 6);

    // kept_mixed = assigned_ids → both rows kept
    auto red = compute_redundant_filaments(4, {1,2,3,4}, assigned_ids, mgr.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    CHECK(red.redundant_mixed.empty());

    // With only one in kept_mixed, the other is redundant
    auto red2 = compute_redundant_filaments(4, {1,2,3,4}, {assigned_ids[0]}, mgr.mixed_filaments());
    CHECK(red2.redundant_mixed.size() == 1);
    CHECK(red2.redundant_mixed[0] == assigned_ids[1]);
}

TEST_CASE("auto_generate shifts virtual ids before add_batch", "[MixedFilament][batch_match]")
{
    // Reproduces the second-match paint-loss bug:
    //   T0: dialog computes target_filament_id assuming 4 phys + 0 mixed → v5.
    //   T1: set_num_filaments → auto_generate inserts C(4,2)=6 auto rows at v5-v10.
    //   T2: add_batch_custom_filaments assigns actual ids starting from v11.
    //   kept_mixed uses the stale dialog target (v5) → batch row (v11) becomes
    //   redundant → deleted → painting lost.
    std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};

    // T0: dialog-time state
    size_t num_phys_dialog    = 4;
    size_t existing_mixed_cnt = 0;
    unsigned int dialog_target = unsigned(num_phys_dialog + existing_mixed_cnt + 1);
    CHECK(dialog_target == 5);

    // T1: auto_generate
    MixedAutoGenerateGuard guard(true);
    MixedFilamentManager   mgr;
    mgr.auto_generate(colors);
    CHECK(mgr.enabled_count() == 6);

    // T2: add_batch
    std::vector<MixedFilamentBatchEntry> entries(1);
    entries[0].component_a   = 1;
    entries[0].component_b   = 2;
    entries[0].mix_b_percent = 50;

    std::vector<unsigned int> assigned_ids;
    mgr.add_batch_custom_filaments(entries, colors, &assigned_ids);

    REQUIRE(assigned_ids.size() == 1);
    unsigned int actual_assigned = assigned_ids[0];
    CHECK(actual_assigned == 11);

    // Dialog target ≠ actual assigned
    CHECK(dialog_target != actual_assigned);

    // BUG: kept_mixed with stale dialog_target ({5}) marks the actual batch row
    // (v11) as redundant, along with the other 5 auto rows not in the kept set.
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {dialog_target}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 6);                // v6-v11 all redundant
    CHECK(red.redundant_mixed[5] == actual_assigned);        // last one = the batch row

    // Fix: kept_mixed with assigned_ids ({11}) keeps the batch row.
    // The 6 auto rows (v5-v10) not in kept_mixed remain redundant.
    auto red_ok = compute_redundant_filaments(4, {1,2,3,4}, assigned_ids, mgr.mixed_filaments());
    REQUIRE(red_ok.redundant_mixed.size() == 6);
    // v5-v10 are auto rows, none are the batch row
    for (unsigned int id : red_ok.redundant_mixed)
        CHECK(id != actual_assigned);
}

TEST_CASE("compute_redundant_filaments manual-pattern cascade legacy", "[MixedFilament][redundant_set]")
{
    // mixed a=1,b=2, manual_pattern="13". Token '3'=literal physical 3.
    // Keep {1,2,4}, delete 3 → manual_pattern refs deleted physical → cascade.
    auto mgr = build_manager(4, {make_row(1, 2, {}, "13")});
    auto red = compute_redundant_filaments(4, {1,2,4}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 3);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments manual-pattern cascade multi-digit-token", "[MixedFilament][redundant_set]")
{
    // Mixed row with manual_pattern containing a literal direct physical id
    // that is NOT kept.  Same semantics as the modern "/" format but using
    // the legacy encoding path which is what normalize_manual_pattern produces
    // (it drops '/' characters, converting "1/11/2" → "1[11]2" style; the
    // multi-digit normalization is covered by the existing remove_physical_filament
    // test in the source).  Here we test: "14" pattern where token '4' is a
    // literal physical id not in the kept set.
    auto mgr = build_manager(4, {make_row(1, 2, {}, "14")});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 4);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments manual-pattern token-1-resolves-to-component-a", "[MixedFilament][redundant_set]")
{
    // mixed a=3,b=4, manual_pattern="1". Token '1'→component_a(=3).
    // Keep {1,2,3,4} (3 kept) → no cascade.
    auto mgr = build_manager(4, {make_row(3, 4, {}, "1")});
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {5}, mgr.mixed_filaments());
    CHECK(red.redundant_mixed.empty());
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("compute_redundant_filaments manual-pattern token-2-resolves-to-component-b", "[MixedFilament][redundant_set]")
{
    // mixed a=1,b=4, manual_pattern="2". Token '2'→component_b(=4).
    // Keep {1,2,3}, delete 4 → cascade via component_b, not just via pattern.
    auto mgr = build_manager(4, {make_row(1, 4, {}, "2")});
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments both-components-cascade", "[MixedFilament][redundant_set]")
{
    // mixed a=3,b=4. Keep {1,2}, delete 3 AND 4 → both cascade.
    auto mgr = build_manager(4, {make_row(3, 4)});
    auto red = compute_redundant_filaments(4, {1,2}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 2);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments all-mixed-deleted", "[MixedFilament][redundant_set]")
{
    // 4 phys + 2 mixed. keep_phys={1,2,3,4}, kept_mixed empty.
    // Both mixed are explicit redundant (not in kept set).
    auto mgr = build_manager(4, {make_row(1, 2), make_row(3, 4)});
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {}, mgr.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    REQUIRE(red.redundant_mixed.size() == 2);
    CHECK(red.redundant_mixed[0] == 5);
    CHECK(red.redundant_mixed[1] == 6);
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("compute_redundant_filaments num-physical-2", "[MixedFilament][redundant_set]")
{
    // Smallest meaningful physical count. 2 phys + 1 mixed.
    auto mgr = build_manager(2, {make_row(1, 2)});
    auto red = compute_redundant_filaments(2, {1}, {3}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 2);
    // mixed row component_b=2 → deleted physical → cascade (not just kept)
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.redundant_mixed[0] == 3);
    CHECK(red.cascade_mixed_count == 1);
}

TEST_CASE("compute_redundant_filaments num-physical-0", "[MixedFilament][redundant_set]")
{
    // No physical filaments is a degenerate input: the survivor floor does not
    // apply (nothing to survive) and nothing can be redundant.  Production code
    // never reaches this state (cleanup_unused_filaments_after_batch_match
    // guards on filament_presets.empty()), but this pure helper is exported and
    // test-visible, so the contract must hold.  Before the early-return, the
    // mixed-enumeration loop below would run with virtual_id = num_physical + 1
    // = 1 and emit bogus redundant_mixed entries for any stray rows — this test
    // pins the fixed, well-defined behaviour (empty result, no floor).
    std::vector<MixedFilament> stray{make_row(1, 2)};
    auto red = compute_redundant_filaments(0, {}, {}, stray);
    REQUIRE(red.redundant_physical.empty());
    REQUIRE(red.redundant_mixed.empty());
    CHECK(red.cascade_mixed_count == 0);
}

TEST_CASE("compute_redundant_filaments deleted-rows-skipped", "[MixedFilament][redundant_set]")
{
    // 4 phys. Push a deleted mixed row — compute must skip it.
    // NOTE: build_manager overrides enabled/deleted on extra_rows, so we push
    // the deleted row directly into the manager's mutable list after creation.
    MixedFilament del_row = make_row(1, 2);
    del_row.enabled = false;
    del_row.deleted = true;
    auto mgr = build_manager(4, {make_row(3, 4)});   // v5 = enabled row (3,4)
    mgr.mixed_filaments().insert(mgr.mixed_filaments().begin(), del_row); // pushed as-is at front
    // Now: [deleted(1,2), enabled(3,4)].  virtual_id=5 for the deleted row is
    // skipped (continue); the enabled row also gets virtual_id=5, which is in
    // kept_mixed={5} → kept.  deleted/disabled rows do NOT consume a virtual-ID slot.
    auto red = compute_redundant_filaments(4, {1,2,3,4}, {5}, mgr.mixed_filaments());
    CHECK(red.redundant_mixed.empty());
}

// ===========================================================================
// Differential oracle: compute_redundant_filaments  vs  remove_physical_filament
// ---------------------------------------------------------------------------
// compute_redundant_filaments is a PREDICTION of which mixed rows the batch-match
// cleanup should drop. The cleanup then deletes physicals via delete_filament,
// which routes through remove_physical_filament -- the AUTHORITATIVE runtime
// cascade. If the two disagree, cleanup marks the wrong rows deleted (by
// stable_id): a live, correctly-resolving row is killed, its virtual id is freed
// and re-aliased to the next survivor, and painted regions silently render as a
// different valid color (the R3 hazard). B1 is exactly such a disagreement.
//
// These tests encode the invariant as a machine-checked differential assertion:
//   for every mixed row the match decided to KEEP (kept_mixed),
//     compute flags it redundant  <=>  it does NOT survive the cascade of
//     remove_physical_filament calls.
// The oracle is remove_physical_filament itself -- no hand-written expected
// values -- so the tests cannot pass-for-the-wrong-reason by mirroring the
// author's mental model (which is how B1 slipped through: every existing
// manual_pattern test kept component_a in the kept set, so the disagree shape
// was never constructed, and the expected ids were written from the same flawed
// model that produced the bug).
// ===========================================================================

namespace {

// Asserts the differential invariant (see comment above) for the kept_mixed rows.
// `rows` must be enabled & non-deleted; each is tagged with a unique 1-based
// stable_id so survival can be tracked across remove_physical_filament's rebuild.
void expect_kept_mixed_matches_runtime(
    size_t                           num_physical,
    const std::vector<unsigned int> &kept_physical,
    const std::vector<unsigned int> &kept_mixed,
    std::vector<MixedFilament>       rows)
{
    for (size_t i = 0; i < rows.size(); ++i)
        rows[i].stable_id = static_cast<uint64_t>(i + 1);

    // (1) compute's prediction.
    auto red = compute_redundant_filaments(num_physical, kept_physical, kept_mixed, rows);
    const std::set<unsigned int> compute_redundant(red.redundant_mixed.begin(),
                                                    red.redundant_mixed.end());

    // (2) ground truth: replay the cleanup's physical-deletion cascade.
    //     Descending order is required -- cleanup iterates red.redundant_physical
    //     (descending) and remove_physical_filament renumbers components > id,
    //     so deleting in ascending order would invalidate the higher ids still
    //     pending deletion.
    // Iterate red.redundant_physical directly: it is the exact descending, survivor-
    // floor-respecting set the real cleanup deletes (compute force-keeps phys 1 when
    // nothing is kept, so phys 1 is never in redundant_physical and the cleanup never
    // deletes it). Recomputing the delete set from kept_physical instead would delete
    // phys 1 when the floor fires, diverging from the real cleanup and making the
    // oracle compare compute against the wrong ground truth.
    auto mgr = build_manager(num_physical, rows);
    for (unsigned int pid : red.redundant_physical)
        mgr.remove_physical_filament(pid);

    std::set<uint64_t> survivor_sids;
    for (const MixedFilament &mf : mgr.mixed_filaments())
        if (mf.stable_id != 0)
            survivor_sids.insert(mf.stable_id);

    // (3) invariant: a KEPT row is flagged redundant by compute  <=>  it did NOT
    //     survive the runtime cascade. Checking only kept_mixed rows is intentional:
    //     rows outside the kept set are dropped by match policy, not by physical
    //     dependency, so remove_physical_filament has no opinion on them.
    for (unsigned int vid : kept_mixed) {
        if (vid <= num_physical) continue;           // not a virtual id
        const size_t k = vid - num_physical - 1;      // 0-based enabled-row index
        if (k >= rows.size()) continue;
        const uint64_t sid = rows[k].stable_id;
        if (sid == 0) continue;

        const bool compute_says_redundant = compute_redundant.count(vid) > 0;
        const bool survived               = survivor_sids.count(sid) > 0;
        INFO("vid=" << vid << " stable_id=" << sid
             << " compute_redundant=" << (compute_says_redundant ? "yes" : "no")
             << " survived=" << (survived ? "yes" : "no")
             << " (invariant expects: redundant == !survived)");
        CHECK(compute_says_redundant != survived);
    }
}

} // namespace

TEST_CASE("B1: kept manual_pattern row whose component_a is dropped must survive (differential)", "[MixedFilament][redundant_set][differential]")
{
    // THE B1 ROOT. a=3 (dropped), b=2, manual_pattern "2" -> component_b=2 (kept).
    // The pattern does NOT reference component_a. At runtime resolve() uses the
    // pattern, so the row works; remove_physical_filament gates component_a behind
    // norm.empty() and PRESERVES the row. But compute checks component_a
    // unconditionally -> cascade -> the cleanup would mark this live row deleted.
    // RED until the component_a check is gated behind norm.empty() (aligned with
    // remove_physical_filament), at which point it turns GREEN.
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "2") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5}, rows);  // drop physical 3
}

TEST_CASE("B1 control: pattern references dropped component_b -> both drop (differential)", "[MixedFilament][redundant_set][differential]")
{
    // Mirror of B1 via component_b: a=1, b=4 (dropped), pattern "2" -> component_b=4.
    // The pattern genuinely references the dropped physical, so both compute
    // (token resolves to 4, not kept) and remove_physical (token "2"->4==deleted)
    // drop the row. GREEN -- documents that the b-path is consistent and that the
    // divergence is component_a only.
    std::vector<MixedFilament> rows{ make_row(1, 4, {}, "2") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 3}, {5}, rows);  // drop physical 4
}

TEST_CASE("B1 control: pattern '1' references dropped component_a -> both drop (differential)", "[MixedFilament][redundant_set][differential]")
{
    // a=3 (dropped), pattern "1" -> component_a=3 (genuinely references the dropped
    // physical). Both compute (token resolves to 3, not kept) and remove_physical
    // (physical_filament_from_token("1")->3==deleted) drop the row. GREEN.
    // Distinguishes "pattern USES deleted component_a" (agree) from B1 "pattern
    // does NOT use component_a but it is dropped" (disagree).
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "1") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5}, rows);  // drop physical 3
}

TEST_CASE("B1 variant: multi-token pattern not touching component_a, a dropped (differential)", "[MixedFilament][redundant_set][differential]")
{
    // a=3 (dropped). pattern "2,4": token '2'->component_b=2 (kept), token '4'->literal 4 (kept).
    // Neither token references physical 3, so remove_physical PRESERVES; compute
    // over-cascades on component_a=3. RED (B1). Confirms B1 is not specific to a
    // single-token pattern.
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "2,4") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5}, rows);  // drop physical 3
}

TEST_CASE("B1 with two kept rows: only the over-cascaded one disagrees (differential)", "[MixedFilament][redundant_set][differential]")
{
    // v5: a=3 dropped, pattern "2"->b=2 kept   -> B1 over-cascade (compute drops, runtime keeps).
    // v6: a=3 dropped, pattern "1"->a=3 dropped -> both agree (drop).
    // The oracle flags exactly the v5 disagreement. RED (B1). Shows B1 can coexist
    // with a correctly-cascaded sibling row.
    std::vector<MixedFilament> rows{ make_row(3, 2, {}, "2"), make_row(3, 2, {}, "1") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 4}, {5, 6}, rows);  // drop physical 3
}

TEST_CASE("B1 control: no pattern, component_a dropped -> both drop via norm-empty branch (differential)", "[MixedFilament][redundant_set][differential]")
{
    // No manual_pattern (norm empty). a=4 (dropped). Both compute (component_a check
    // in the norm-empty branch) and remove_physical (pair check, norm empty) drop
    // the row. GREEN -- confirms the divergence lives ONLY in the norm-non-empty
    // branch (i.e. only when a manual_pattern is present).
    std::vector<MixedFilament> rows{ make_row(4, 1) };
    expect_kept_mixed_matches_runtime(4, {1, 2, 3}, {5}, rows);  // drop physical 4
}

TEST_CASE("m1: compute uses num_physical bound, remove_physical uses kMax=64 (differential)", "[MixedFilament][redundant_set][differential][!shouldfail]")
{
    // KNOWN DIVERGENCE (m1) -- tagged [!shouldfail]: this test is EXPECTED to FAIL.
    // It documents that compute_redundant_filaments bounds pattern/gradient literal
    // tokens by `num_physical`, while remove_physical_filament bounds them by
    // kMaxPhysicalFilaments (64) and uses an ==deleted criterion. For an
    // out-of-range literal token they disagree:
    //   pattern "[12]" (literal), num_physical=4, drop physical 4:
    //     compute:         slashified -> pid 12 > num_physical 4 -> cascade.
    //     remove_physical: physical_filament_from_token("12", mf, 64) -> 12 != 4 -> PRESERVE.
    // UNREACHABLE in product flows: a literal token > num_physical cannot exist in
    // the live mixed list -- references_exceed_physical rejects it at EVERY write
    // path: load_custom_entries AND add_batch_custom_filaments (the batch-match
    // confirm flow's own entry point) both call it, and add_custom_filament /
    // auto_generate never set a manual_pattern/gradient at all.  The
    // clear_custom_entries + load_custom_entries cycle inside
    // update_multi_material_filament_presets (run on every filament-count change,
    // incl. set_num_filaments -> to_delete=-1, which skips remove_physical_filament)
    // re-validates with the new colour count and drops it. So this guards no
    // user-hittable bug today; it is a regression guard for the validation perimeter
    // (if that perimeter is ever weakened, this state becomes reachable and m1 turns
    // into a live wrong-delete). If m1 is ever fixed (bounds unified) this test will
    // UNEXPECTEDLY SUCCEED and the [!shouldfail] tag will flag it red -- at that point
    // drop the tag. Do NOT relax the oracle instead.
    std::vector<MixedFilament> rows{ make_row(1, 2, {}, "[12]") };
    expect_kept_mixed_matches_runtime(4, {1, 2, 3}, {5}, rows);  // drop physical 4
}


TEST_CASE("compute_redundant_filaments gradient-and-manual-pattern-both-kept", "[MixedFilament][redundant_set]")
{
    // mixed with gradient="13" and manual_pattern="4". Keep {1,2,3} → delete 4.
    // manual_pattern refs deleted id 4 → cascade, even though gradient ids are kept.
    auto mgr = build_manager(4, {make_row(1, 2, "13", "4")});
    // gradient "13": physicals 1 and 3 → both kept {1,3} ✓
    // manual_pattern "4": literal physical 4 → not in {1,2,3} → cascade
    auto red = compute_redundant_filaments(4, {1,2,3}, {5}, mgr.mixed_filaments());
    REQUIRE(red.redundant_physical.size() == 1);
    CHECK(red.redundant_physical[0] == 4);
    REQUIRE(red.redundant_mixed.size() == 1);
    CHECK(red.cascade_mixed_count == 1);
}

// ===========================================================================
// Batch-match confirm-flow mechanism guards (RV1 / RV2 in the gap register)
// ===========================================================================

TEST_CASE("display_color follows the physical palette: color-keyed matching across a palette change misses", "[MixedFilament][batch_match]")
{
    // RV1 mechanism. WHY the confirm handler matches existing custom mixed rows
    // by IDENTITY (dialog-time vid -> stable_id -> current row) instead of by
    // display color: in recommended mode the handler rewrites filament_colour
    // and calls set_num_filaments BEFORE the in-place block, and
    // update_multi_material_filament_presets -> auto_generate ends in
    // refresh_display_colors on BOTH exits (auto-generate pref on or off), so
    // display_color is already NEW-palette based when that block runs. This
    // test pins the underlying reason: the same recipe yields a different
    // display_color under a different palette, so an equality lookup keyed on
    // the dialog-time color (the rejected design) would silently never find the
    // row. The identity lookup the code actually uses is immune to this.
    MixedAutoGenerateGuard guard(false);
    auto mgr = build_manager(2, {make_row(1, 2)});
    REQUIRE(mgr.mixed_filaments().size() == 1);

    const std::vector<std::string> palette_old = {"#FF0000", "#0000FF"}; // dialog-time
    const std::vector<std::string> palette_new = {"#FFFF00", "#00FF00"}; // post-rewrite

    mgr.refresh_display_colors(palette_old);
    const std::string dialog_time_color = mgr.mixed_filaments()[0].display_color;
    REQUIRE(!dialog_time_color.empty());

    // Control: refresh is deterministic — same palette, same display_color.
    mgr.refresh_display_colors(palette_old);
    CHECK(mgr.mixed_filaments()[0].display_color == dialog_time_color);

    // Palette change (what recommended mode does before the in-place block).
    mgr.refresh_display_colors(palette_new);
    const std::string confirm_time_color = mgr.mixed_filaments()[0].display_color;
    REQUIRE(!confirm_time_color.empty());

    // The equality key the in-place block relies on no longer holds.
    CHECK(confirm_time_color != dialog_time_color);
}

TEST_CASE("physical growth shifts mixed virtual ids in the remap: dialog-time source ids go stale", "[MixedFilament][batch_match]")
{
    // RV2 mechanism. The batch dialog captures source_extruder_ids = {slot+1}
    // at dialog-open. In recommended mode with < 4 physicals the confirm
    // handler then grows the physical count (set_num_filaments) and Plater::
    // on_filaments_change consumes the remap built there and applies it to
    // painted facets BEFORE apply_batch_match_to_model runs. This test pins
    // the shift itself: after 3 -> 4 growth the mixed row's old virtual id (4)
    // maps to 5, so no facet carries the dialog-time id anymore and a facet
    // remap keyed on it (apply's extruder_remap) targets what is now physical
    // slot 4 instead of the moved mixed painting.
    MixedAutoGenerateGuard guard(false);

    PresetBundle bundle;
    bundle.filament_presets = {"Filament 1", "Filament 2", "Filament 3"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 2, 50, colors);
    const uint64_t sid = mgr.mixed_filaments().back().stable_id;
    REQUIRE(sid != 0);

    // Persist the custom row the way the product does after every mixed edit —
    // the non-deleting umfp path below reloads customs from this config string.
    bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions", true)->value =
        mgr.serialize_custom_entries();

    const unsigned int dialog_time_vid = virtual_id_for_stable_id(mgr.mixed_filaments(), 3, sid);
    REQUIRE(dialog_time_vid == 4);
    (void)bundle.consume_last_filament_id_remap(); // discard setup remap

    // Grow 3 -> 4 exactly like the confirm handler: write the new palette
    // first, then set_num_filaments (passes the true old count to the remap).
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#00FFFF", "#FF00FF", "#FFFF00", "#00FF00"
    };
    bundle.set_num_filaments(4u, std::vector<std::string>{});

    const unsigned int post_growth_vid = virtual_id_for_stable_id(mgr.mixed_filaments(), 4, sid);
    REQUIRE(post_growth_vid == 5);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() > dialog_time_vid);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[dialog_time_vid] == post_growth_vid); // painted facets move 4 -> 5
    CHECK(dialog_time_vid != post_growth_vid);        // the dialog-time key is stale
}

// ===========================================================================
// add_batch_custom_filaments branch coverage (out_assigned_ids contract)
// ===========================================================================

TEST_CASE("add_batch at the filament cap emits per-entry zero ids instead of truncating", "[MixedFilament][batch_match]")
{
    // The confirm handler aligns assigned_ids to mappings by construction
    // order, so the contract is STRICT: one id per input entry, 0 = dropped.
    // The cap path must therefore continue (pushing 0s), never break early.
    MixedAutoGenerateGuard guard(false);
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    MixedFilamentManager mgr;
    // Fill to one slot below the cap: 4 physical + (kMax - 5) mixed = kMax - 1.
    const size_t kMax = MAXIMUM_FILAMENT_NUMBER;
    for (size_t i = 0; i < kMax - 5; ++i)
        mgr.mixed_filaments().push_back(make_row(1, 2));
    REQUIRE(mgr.total_filaments(4) == kMax - 1);

    std::vector<MixedFilamentBatchEntry> entries(3);
    for (auto &e : entries) {
        e.component_a   = 1;
        e.component_b   = 3;
        e.mix_b_percent = 40;
    }

    std::vector<unsigned int> assigned;
    mgr.add_batch_custom_filaments(entries, colors, &assigned);

    REQUIRE(assigned.size() == 3);          // strict 1:1 with entries
    CHECK(assigned[0] == kMax);             // last free slot
    CHECK(assigned[1] == 0);                // dropped at cap
    CHECK(assigned[2] == 0);                // still one zero PER entry
    CHECK(mgr.total_filaments(4) == kMax);
}

TEST_CASE("add_batch clamps out-of-range components and falls back on a==b instead of dropping", "[MixedFilament][batch_match]")
{
    MixedAutoGenerateGuard guard(false);
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;

    std::vector<MixedFilamentBatchEntry> entries(3);
    entries[0].component_a = 2;  entries[0].component_b = 2;  // a==b       -> b = 1
    entries[1].component_a = 1;  entries[1].component_b = 1;  // a==b, a==1 -> b = 2
    entries[2].component_a = 99; entries[2].component_b = 1;  // a clamped to n=3

    std::vector<unsigned int> assigned;
    mgr.add_batch_custom_filaments(entries, colors, &assigned);

    REQUIRE(assigned.size() == 3);
    CHECK(assigned[0] == 4);
    CHECK(assigned[1] == 5);
    CHECK(assigned[2] == 6);
    REQUIRE(mgr.mixed_filaments().size() == 3);
    CHECK(mgr.mixed_filaments()[0].component_a == 2);
    CHECK(mgr.mixed_filaments()[0].component_b == 1);
    CHECK(mgr.mixed_filaments()[1].component_a == 1);
    CHECK(mgr.mixed_filaments()[1].component_b == 2);
    CHECK(mgr.mixed_filaments()[2].component_a == 3);
    CHECK(mgr.mixed_filaments()[2].component_b == 1);
}

TEST_CASE("add_batch guards: <2 colours yields all-zero ids, empty entries yield empty ids", "[MixedFilament][batch_match]")
{
    MixedAutoGenerateGuard guard(false);
    MixedFilamentManager mgr;

    std::vector<MixedFilamentBatchEntry> entries(2);
    entries[0].component_a = 1; entries[0].component_b = 2;
    entries[1].component_a = 1; entries[1].component_b = 2;

    std::vector<unsigned int> assigned = {77u}; // stale content must be overwritten
    mgr.add_batch_custom_filaments(entries, {"#FF0000"}, &assigned); // n = 1
    REQUIRE(assigned.size() == 2);              // still 1:1 with entries
    CHECK(assigned[0] == 0);
    CHECK(assigned[1] == 0);
    CHECK(mgr.mixed_filaments().empty());

    std::vector<unsigned int> assigned2 = {77u};
    mgr.add_batch_custom_filaments({}, {"#FF0000", "#00FF00"}, &assigned2);
    CHECK(assigned2.empty());                   // 0 entries -> 0 ids
    CHECK(mgr.mixed_filaments().empty());
}

TEST_CASE("serialize/load round-trip preserves custom-row stable_id", "[MixedFilament][Serialization]")
{
    // The batch-match cleanup marks redundant mixed rows by stable_id AFTER
    // physical deletions that round-trip the list through serialize -> clear ->
    // load (update_multi_material_filament_presets).  Identity surviving that
    // round-trip is the load-bearing assumption; pin it explicitly.
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedAutoGenerateGuard guard(false);
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.add_custom_filament(2, 3, 30, colors);
    REQUIRE(mgr.mixed_filaments().size() == 2);
    const uint64_t sid0 = mgr.mixed_filaments()[0].stable_id;
    const uint64_t sid1 = mgr.mixed_filaments()[1].stable_id;
    REQUIRE(sid0 != 0);
    REQUIRE(sid1 != 0);
    REQUIRE(sid0 != sid1);

    const std::string serialized = mgr.serialize_custom_entries();
    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);

    bool found0 = false;
    bool found1 = false;
    for (const MixedFilament &mf : loaded.mixed_filaments()) {
        if (mf.stable_id == sid0) {
            found0 = true;
            CHECK(mf.component_a == 1);
            CHECK(mf.component_b == 2);
        }
        if (mf.stable_id == sid1) {
            found1 = true;
            CHECK(mf.component_a == 2);
            CHECK(mf.component_b == 3);
        }
    }
    CHECK(found0);
    CHECK(found1);
}

TEST_CASE("add_batch rejects entries whose gradient/pattern reference a filament > n (m1 perimeter)", "[MixedFilament][batch_match]")
{
    // Regression guard for the major-1 fix. compute_redundant_filaments bounds
    // pattern/gradient literal tokens by num_physical, while remove_physical_
    // filament bounds them by kMaxPhysicalFilaments=64 (the m1 divergence,
    // documented in MixedFilament.hpp and pinned by the [!shouldfail] "m1"
    // differential test). For m1 to stay UNREACHABLE the invariant "no live row
    // references a literal physical id > n" must hold at EVERY write path, not
    // just load_custom_entries. add_batch_custom_filaments is the batch-match
    // confirm flow's own entry point, so it must reject out-of-range references
    // the same way load_custom_entries does. Without this guard a recipe
    // generator change that ever emitted a literal token > n would turn m1 into a
    // live silent-wrong-color bug (cleanup marks the wrong rows deleted).
    MixedAutoGenerateGuard guard(false);
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}; // n = 4
    MixedFilamentManager mgr;

    std::vector<MixedFilamentBatchEntry> entries(3);
    // e0: valid (a,b in range, no gradient) -> assigned a real id
    entries[0].component_a = 1;
    entries[0].component_b = 2;
    // e1: gradient references physical 12 > n=4 -> REJECTED (assigned 0)
    entries[1].component_a = 1;
    entries[1].component_b = 2;
    entries[1].gradient_component_ids = "1/12/3";
    // e2: manual_pattern literal token '5' > n=4 -> REJECTED (assigned 0)
    entries[2].component_a = 1;
    entries[2].component_b = 2;
    entries[2].manual_pattern = "15"; // '1' symbolic, '5' literal > 4

    std::vector<unsigned int> assigned;
    mgr.add_batch_custom_filaments(entries, colors, &assigned);

    REQUIRE(assigned.size() == 3);              // strict 1:1 with entries
    CHECK(assigned[0] == 5);                    // only e0 created (4 phys + 1)
    CHECK(assigned[1] == 0u);                   // e1 rejected
    CHECK(assigned[2] == 0u);                   // e2 rejected
    REQUIRE(mgr.mixed_filaments().size() == 1); // only e0's row stored
    CHECK(mgr.mixed_filaments()[0].component_a == 1);
    CHECK(mgr.mixed_filaments()[0].component_b == 2);

    // The one stored row is clean (no out-of-range references) and, since it is
    // the only enabled mixed row at v5, compute_redundant_filaments sees a list
    // whose every token is in range -- the m1 state never enters it.
    auto red = compute_redundant_filaments(4, {1, 2, 3, 4}, {5}, mgr.mixed_filaments());
    CHECK(red.redundant_mixed.empty());
}

// --------------------------------------------------------------------------
// [shrink] — set_num_filaments tail-truncation path
//
// These tests pin the remap behaviour when the physical filament count is
// REDUCED via set_num_filaments(N) with N < current. This is the "compact"
// path the batch-match confirm flow uses (mirroring recommended mode's
// set_num_filaments call): it truncates the tail rather than deleting an
// arbitrary middle slot, so surviving physical ids keep their identity and
// the truncated tail maps to 0 (NONE). auto_generate is disabled via
// MixedAutoGenerateGuard to isolate the pure remap-table behaviour from any
// mixed-list rebuild side effects.
// --------------------------------------------------------------------------

TEST_CASE("shrink: set_num_filaments remap physical tail to 0", "[MixedFilament][shrink]")
{
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament",
                               "Default Filament", "Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"};
    bundle.update_multi_material_filament_presets();

    // Shrink 6 -> 2 (tail truncation, same call shape as recommended mode).
    bundle.set_num_filaments(2, std::vector<std::string>{});
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    // Remap covers old_total + 1 (6 physical + 0 mixed + 1). Index 0 is the
    // NONE sink and is always present.
    REQUIRE(remap.size() == 7);

    // Surviving head keeps identity.
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    // Truncated tail maps to 0 (NONE) — NOT identity. This is what makes
    // tail-truncation safe for the compact flow: painting referencing a
    // deleted physical is cleanly redirected to NONE.
    CHECK(remap[3] == 0);
    CHECK(remap[4] == 0);
    CHECK(remap[5] == 0);
    CHECK(remap[6] == 0);
}

TEST_CASE("shrink: tail-truncate keeps surviving ids identity, mid-delete shifts", "[MixedFilament][shrink]")
{
    MixedAutoGenerateGuard guard(false);

    // --- Scenario A: tail-truncate 6 -> 4 --------------------------------
    {
        PresetBundle bundle;
        bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament",
                                   "Default Filament", "Default Filament", "Default Filament"};
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
            {"#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"};
        bundle.update_multi_material_filament_presets();

        bundle.set_num_filaments(4, std::vector<std::string>{});
        const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

        REQUIRE(remap.size() >= 5);
        // All survivors keep identity — no shifting because nothing was
        // removed from the middle.
        CHECK(remap[1] == 1);
        CHECK(remap[2] == 2);
        CHECK(remap[3] == 3);
        CHECK(remap[4] == 4);
    }

    // --- Scenario B: mid-delete via update_num_filaments(index 2) --------
    // Deleting the 3rd physical (0-based index 2) shifts every higher id
    // down by one — the opposite of tail-truncation.
    {
        PresetBundle bundle;
        bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament",
                                   "Default Filament", "Default Filament", "Default Filament"};
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
            {"#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"};
        bundle.update_multi_material_filament_presets();

        bundle.update_num_filaments(2); // remove 0-based index 2 → 1-based id 3
        const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

        REQUIRE(remap.size() >= 6);
        // Deleted slot maps to 0.
        CHECK(remap[3] == 0);
        // Every id above the deleted slot shifts down by one.
        CHECK(remap[4] == 3);
        CHECK(remap[5] == 4);
        CHECK(remap[6] == 5);
        // Ids below the deleted slot are unaffected.
        CHECK(remap[1] == 1);
        CHECK(remap[2] == 2);
    }
}

TEST_CASE("shrink: redundant_physical empty after tail-truncate to kept count", "[MixedFilament][shrink]")
{
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    // Compact: keep only the first 2 physicals.
    bundle.set_num_filaments(2, std::vector<std::string>{});
    bundle.consume_last_filament_id_remap(); // drain, not needed here

    // After compaction the physical count is already the kept count, so
    // compute_redundant_filaments must report NO redundant physicals.
    // This is the precondition for the cleanup loop being a no-op in the
    // compact flow — if this fails, the deletion loop would still run and
    // the optimisation would not hold.
    auto red = compute_redundant_filaments(2, {1, 2}, {}, bundle.mixed_filaments.mixed_filaments());
    CHECK(red.redundant_physical.empty());
    CHECK(red.new_num_physical == 2);
}

// ===========================================================================
// Non-contiguous manual-mode subset — composite remap divergence
//
// Characterization tests for the "partial colour corruption" bug that fires
// when a manual-mode batch match selects a NON-contiguous subset of the
// physical palette, e.g. [2,6,8,10] out of 10 physical filaments.
//
// Root cause (verified by reading the production code, not inferred):
//   cleanup_unused_filaments_after_batch_match (Plater.cpp:8354-8446) deletes
//   the unselected physical slots via middle-deletion repacking, which compacts
//   the survivors into the low ids (C2->1, C6->2, C8->3, C10->4). It then builds
//   ONE composite painting remap via update_mixed_filament_id_remap(old, 10, 4)
//   (Plater.cpp:8425). That call routes into build_filament_id_remap with
//   deleting_filament=false (PresetBundle.cpp:3774), whose PHYSICAL branch
//   (PresetBundle.cpp:3824-3834) only does tail-truncation when
//   deleting_filament=false:
//       old_id <= new_num -> mapped = old_id (identity)
//       else              -> mapped = 0       (truncated tail)
//   Tail-truncation is only correct when the surviving set is exactly {1..N}
//   (the documented invariant at Plater.cpp:8360-8366). recommended mode
//   satisfies it because set_num_filaments rewrites the palette head-first;
//   manual mode with an arbitrary subset like [2,6,8,10] does NOT, and the
//   invariant is silently violated with no runtime check on the kept-set shape.
//
// Result for [2,6,8,10]:
//   - paint on C6/C8/C10 (old ids 6/8/10, SURVIVORS) -> remapped to 0 (LOST)
//   - paint on C1/C3/C4 (old ids 1/3/4, DELETED)     -> remapped to 1/3/4
//     which now hold C2/C8/C10 (WRONG COLOUR)
//   mixed virtual ids survive correctly (stable_id path), so the corruption
//   is PARTIAL — only unmigrated physical-source painting is affected, which
//   matches the reported "部分颜色错乱" symptom.
//
// These tests pin the behaviour at TWO layers:
//   Layer 1: compute_redundant_filaments (pure fn, upstream input — CORRECT,
//            just pinned so the remap tests have deterministic inputs).
//   Layer 2: update_mixed_filament_id_remap batch path (the bug itself).
//
// For the bug layer we use the "double test" pattern (consistent with the m1
// test above): one TEST_CASE pins the CURRENT (wrong) output (green), and a
// sibling tagged [!shouldfail] pins the EXPECTED (correct) output (red). When
// the bug is fixed the [!shouldfail] case will "unexpectedly succeed" and CI
// will flag it — at that point drop the tag. Do NOT relax the oracle instead.
// ===========================================================================

TEST_CASE("compute_redundant_filaments non-contiguous kept subset [2,6,8,10]", "[MixedFilament][redundant_set]")
{
    // Layer 1: pin the upstream deterministic input that the cleanup loop feeds
    // into the composite remap. This function's output is CORRECT for a
    // non-contiguous kept set; it just produces the {9,7,5,4,3,1} descending
    // deletion list that the (buggy) batch remap then misinterprets.
    auto mgr = build_manager(10, {});
    auto red = compute_redundant_filaments(10, {2, 6, 8, 10}, {}, mgr.mixed_filaments());

    REQUIRE(red.redundant_physical.size() == 6);
    // Descending order — cleanup's batched-delete path requires this (and
    // asserts it at runtime, Plater.cpp:8372-8383).
    CHECK(red.redundant_physical[0] == 9);
    CHECK(red.redundant_physical[1] == 7);
    CHECK(red.redundant_physical[2] == 5);
    CHECK(red.redundant_physical[3] == 4);
    CHECK(red.redundant_physical[4] == 3);
    CHECK(red.redundant_physical[5] == 1);
    CHECK(red.new_num_physical == 4);
}

// Helper for the batch-remap layer: build a 10-physical PresetBundle with
// auto-generate disabled (so m_mixed stays empty and the remap under test is
// the PURE physical branch of build_filament_id_remap), snapshot it, and run
// update_mixed_filament_id_remap(old, 10, 4) — the exact call shape
// cleanup_unused_filaments_after_batch_match makes at Plater.cpp:8442 for a
// 4-physical manual selection out of 10.
//
// `kept_physical_ids` (default empty) mirrors the cleanup call site's new
// parameter: empty = original tail-truncation behaviour (backward compat),
// non-empty = kept-aware mapping (the fix for non-contiguous manual subsets).
static std::vector<unsigned int> build_batch_remap_for_kept(size_t num_physical, size_t new_num_physical,
                                                            const std::vector<unsigned int> &kept_physical_ids = {})
{
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets.assign(num_physical, "Default Filament");
    {
        std::vector<std::string> colours;
        colours.reserve(num_physical);
        for (size_t i = 0; i < num_physical; ++i)
            colours.emplace_back("#FF0000");
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = std::move(colours);
    }
    bundle.update_multi_material_filament_presets();

    const std::vector<MixedFilament> old_mixed = bundle.mixed_filaments.mixed_filaments();
    // Direct batch call — mirrors Plater.cpp:8442 (deleting_filament=false),
    // forwarding kept_physical_ids as the cleanup call site now does.
    bundle.update_mixed_filament_id_remap(old_mixed, num_physical, new_num_physical,
                                          size_t(-1), kept_physical_ids);
    return bundle.consume_last_filament_id_remap();
}

TEST_CASE("batch_remap contiguous head [1,2,3,4] keeps identity (recommended-mode parity)", "[MixedFilament][batch_remap]")
{
    // Parity / non-regression guard: when the surviving physical set IS the
    // contiguous head {1..new_num}, the tail-truncation branch is correct and
    // survivors keep identity. This is exactly the recommended-mode situation
    // (set_num_filaments rewrites the palette head-first), so a future fix to
    // the [2,6,8,10] bug must NOT break this case.
    const auto remap = build_batch_remap_for_kept(10, 4);

    // size == old_total + 1 == 10 physical + 0 mixed + 1 (NONE sink at [0]).
    REQUIRE(remap.size() == 11);
    CHECK(remap[0] == 0); // NONE sink, untouched.
    // Surviving head keeps identity.
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[4] == 4);
    // Truncated tail maps to 0 (NONE) — correct for the contiguous-head case.
    CHECK(remap[5] == 0);
    CHECK(remap[6] == 0);
    CHECK(remap[7] == 0);
    CHECK(remap[8] == 0);
    CHECK(remap[9] == 0);
    CHECK(remap[10] == 0);
}

TEST_CASE("batch_remap non-contiguous kept subset [2,6,8,10] produces tail-truncation (CURRENT bug)", "[MixedFilament][batch_remap]")
{
    // Pins the CURRENT (incorrect) output of build_filament_id_remap's physical
    // branch for a non-contiguous kept subset. The batch path only does
    // tail-truncation when deleting_filament=false (PresetBundle.cpp:3830-3831),
    // so it ignores WHICH physicals survived and maps solely by id <= new_num.
    // For kept={2,6,8,10} -> new_num=4 that yields the table below, which is
    // WRONG (survivors 6/8/10 are lost; deleted 1/3/4 are kept as identity).
    // Kept green so a refactor that changes this surface is caught; the
    // expected-correct oracle lives in the [!shouldfail] sibling below.
    const auto remap = build_batch_remap_for_kept(10, 4);
    REQUIRE(remap.size() == 11);

    // --- DELETED slots wrongly kept as identity (the "wrong colour" half) ---
    CHECK(remap[1] == 1); // C1 deleted, yet maps to id 1 (now C2)
    CHECK(remap[3] == 3); // C3 deleted, yet maps to id 3 (now C8)
    CHECK(remap[4] == 4); // C4 deleted, yet maps to id 4 (now C10)
    // --- SURVIVOR C2 maps to id 2 (now C6) instead of new id 1 ---
    CHECK(remap[2] == 2);
    // --- SURVIVORS C6/C8/C10 (old ids 6/8/10) wrongly truncated to NONE ---
    CHECK(remap[5] == 0); // C5 deleted -> 0 (correct by accident)
    CHECK(remap[6] == 0); // C6 SURVIVED -> 0 (LOST)  <- bug
    CHECK(remap[7] == 0); // C7 deleted -> 0 (correct by accident)
    CHECK(remap[8] == 0); // C8 SURVIVED -> 0 (LOST)  <- bug
    CHECK(remap[9] == 0); // C9 deleted -> 0 (correct by accident)
    CHECK(remap[10] == 0); // C10 SURVIVED -> 0 (LOST) <- bug
}

TEST_CASE("batch_remap non-contiguous kept subset [2,6,8,10] maps survivors correctly when kept is supplied (FIXED)", "[MixedFilament][batch_remap]")
{
    // Kept-aware fix: when the caller supplies kept_physical_ids, the batch
    // remap maps each survivor by its position in the kept set instead of
    // tail-truncation. For [2,6,8,10] -> new ids 1/2/3/4 (sorted survivors):
    //   old id 1 (C1, deleted)   -> 0
    //   old id 2 (C2, survivor)  -> 1
    //   old id 3 (C3, deleted)   -> 0
    //   old id 4 (C4, deleted)   -> 0
    //   old id 5 (C5, deleted)   -> 0
    //   old id 6 (C6, survivor)  -> 2
    //   old id 7 (C7, deleted)   -> 0
    //   old id 8 (C8, survivor)  -> 3
    //   old id 9 (C9, deleted)   -> 0
    //   old id 10 (C10, survivor)-> 4
    // This was the [!shouldfail] oracle for the tail-truncation bug; the
    // kept-aware branch in build_filament_id_remap now makes it pass, so the
    // tag is dropped and this becomes the regression guard for the fix.
    const auto remap = build_batch_remap_for_kept(10, 4, {2, 6, 8, 10});
    REQUIRE(remap.size() == 11);

    CHECK(remap[1] == 0);  // C1 deleted
    CHECK(remap[2] == 1);  // C2 -> new id 1
    CHECK(remap[3] == 0);  // C3 deleted
    CHECK(remap[4] == 0);  // C4 deleted
    CHECK(remap[5] == 0);  // C5 deleted
    CHECK(remap[6] == 2);  // C6 -> new id 2
    CHECK(remap[7] == 0);  // C7 deleted
    CHECK(remap[8] == 3);  // C8 -> new id 3
    CHECK(remap[9] == 0);  // C9 deleted
    CHECK(remap[10] == 4); // C10 -> new id 4
}

// ---------------------------------------------------------------------------
// Kept-set SHAPE matrix for the batch physical branch. The branch
// (PresetBundle.cpp:3824-3834, deleting_filament=false) emits
//   old_id <= new_num -> old_id (identity)
//   old_id >  new_num -> 0
// REGARDLESS of which physicals survived — it only sees `new_num`. So the
// output is correct iff the kept set happens to be {1..new_num} (case A/F) and
// is the SAME bug for every other shape. The three cases below cover the
// distinct FAILURE SIGNATURES:
//   C  {5,6,7,8}  — contiguous but NOT head: every survivor > new_num, so all
//                   survivors are truncated to 0 (LOST) while deleted head ids
//                   1..4 are kept as identity (WRONG COLOUR).
//   D  {1,3,5}    — interspersed: survivors straddle new_num, so id 1 happens
//                   to be right, id 3 is wrongly kept (a deleted id held as
//                   identity), and id 2 (deleted) is wrongly held as 2. Most
//                   insidious shape because PART of the output is coincidentally
//                   correct.
//   F  {1..10}    — keep-all: the only other correct shape besides the
//                   contiguous head. Non-regression guard that a fix must not
//                   break.
// ---------------------------------------------------------------------------

TEST_CASE("batch_remap contiguous-non-head {5,6,7,8} truncates survivors (CURRENT bug)", "[MixedFilament][batch_remap]")
{
    // kept = {5,6,7,8}, new_num = 4. Physical branch output is the SAME as the
    // [2,6,8,10] case — [1,2,3,4,0,0,0,0,0,0] — proving the output does not
    // depend on WHICH ids survived, only on new_num. Here ids 5-8 (survivors)
    // all exceed new_num=4 and are truncated to 0; ids 1-4 (deleted) are held
    // as identity.
    const auto remap = build_batch_remap_for_kept(10, 4);
    REQUIRE(remap.size() == 11);
    // Deleted head wrongly held as identity (would point at C5/C6/C7/C8 after repack).
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[4] == 4);
    // Survivors 5/6/7/8 all truncated to NONE.
    CHECK(remap[5] == 0);
    CHECK(remap[6] == 0);
    CHECK(remap[7] == 0);
    CHECK(remap[8] == 0);
    // Deleted tail, correct by accident.
    CHECK(remap[9] == 0);
    CHECK(remap[10] == 0);
}

TEST_CASE("batch_remap contiguous-non-head {5,6,7,8} maps survivors correctly when kept is supplied (FIXED)", "[MixedFilament][batch_remap]")
{
    // Kept-aware fix: {5,6,7,8} survivors -> new ids 1/2/3/4.
    const auto remap = build_batch_remap_for_kept(10, 4, {5, 6, 7, 8});
    REQUIRE(remap.size() == 11);
    CHECK(remap[1] == 0); // C1 deleted
    CHECK(remap[2] == 0); // C2 deleted
    CHECK(remap[3] == 0); // C3 deleted
    CHECK(remap[4] == 0); // C4 deleted
    CHECK(remap[5] == 1); // C5 -> new id 1
    CHECK(remap[6] == 2); // C6 -> new id 2
    CHECK(remap[7] == 3); // C7 -> new id 3
    CHECK(remap[8] == 4); // C8 -> new id 4
    CHECK(remap[9] == 0); // C9 deleted
    CHECK(remap[10] == 0); // C10 deleted
}

TEST_CASE("batch_remap interspersed {1,3,5} mixes coincidentally-correct and wrong (CURRENT bug)", "[MixedFilament][batch_remap]")
{
    // kept = {1,3,5}, new_num = 3. Output [1,2,3,0,...]: id 1 is correct by
    // coincidence (kept AND == new_num range); id 3 (kept) is wrongly held as
    // 3 (should shift to 2); id 2 (deleted) is wrongly held as 2 (should be 0).
    const auto remap = build_batch_remap_for_kept(10, 3);
    REQUIRE(remap.size() == 11);
    CHECK(remap[1] == 1);  // C1 survivor, coincidentally correct (new id 1)
    CHECK(remap[2] == 2);  // C2 DELETED, wrongly held as 2 (would be C3 after repack)
    CHECK(remap[3] == 3);  // C3 survivor, wrongly held as 3 (should be new id 2)
    CHECK(remap[4] == 0);  // C4 deleted
    CHECK(remap[5] == 0);  // C5 SURVIVOR, truncated to NONE (should be new id 3)
    CHECK(remap[6] == 0);  // C6 deleted
    CHECK(remap[7] == 0);  // C7 deleted
    CHECK(remap[8] == 0);  // C8 deleted
    CHECK(remap[9] == 0);  // C9 deleted
    CHECK(remap[10] == 0); // C10 deleted
}

TEST_CASE("batch_remap interspersed {1,3,5} maps survivors correctly when kept is supplied (FIXED)", "[MixedFilament][batch_remap]")
{
    // Kept-aware fix: {1,3,5} survivors -> new ids 1/2/3.
    const auto remap = build_batch_remap_for_kept(10, 3, {1, 3, 5});
    REQUIRE(remap.size() == 11);
    CHECK(remap[1] == 1);  // C1 -> new id 1
    CHECK(remap[2] == 0);  // C2 deleted
    CHECK(remap[3] == 2);  // C3 -> new id 2
    CHECK(remap[4] == 0);  // C4 deleted
    CHECK(remap[5] == 3);  // C5 -> new id 3
    CHECK(remap[6] == 0);  // C6 deleted
    CHECK(remap[7] == 0);  // C7 deleted
    CHECK(remap[8] == 0);  // C8 deleted
    CHECK(remap[9] == 0);  // C9 deleted
    CHECK(remap[10] == 0); // C10 deleted
}

TEST_CASE("batch_remap keep-all {1..10} is identity (non-regression)", "[MixedFilament][batch_remap]")
{
    // The other correct shape besides the contiguous head: nothing deleted, so
    // new_num == old_num and every id keeps identity. A fix to the non-contiguous
    // bug must leave this case untouched.
    const auto remap = build_batch_remap_for_kept(10, 10);
    REQUIRE(remap.size() == 11);
    CHECK(remap[0] == 0); // NONE sink
    for (unsigned int i = 1; i <= 10; ++i)
        CHECK(remap[i] == i);
}

// ===========================================================================
// Batch-remap MIXED branch (deleting_filament=false)
//
// In the batch path the mixed branch (PresetBundle.cpp:3856-3918) skips the
// deletion-specific zeroing (3874/3877, both gated on deleting_filament or
// deleted_1based) and the component shift (3894-3898, gated on
// deleting_filament). It relies on EITHER:
//   (a) stable_id match (3884-3892) — new side's mixed_filaments() carries
//       the same stable_id -> old virtual id maps to the new virtual id; OR
//   (b) canonical_pair fallback (3893-3914) — old side's (component_a,component_b)
//       looked up in a map keyed by the NEW side's (component_a,component_b).
//
// In real cleanup (Plater.cpp:8425) the call is made AFTER the delete_filament
// loop, so the bundle's live mixed_filaments() has already been renumbered by
// remove_physical_filament (component_a/b decremented past each deleted id,
// MixedFilament.cpp:1864-1870), while old_mixed passed in is the PRE-deletion
// snapshot. This means:
//   - stable_id path: correct — stable_id is an identity key, renumber-proof.
//   - pair fallback path: the OLD key uses pre-deletion component ids while
//     the NEW map uses post-deletion renumbered ids -> the keys NEVER match
//     for any pair that straddled a deleted id -> fallback returns 0 (NONE),
//     silently dropping the mixed row's painting.
//
// The pair-fallback bug is effectively UNREACHABLE in product flows today:
// every mixed row gets a non-zero stable_id at creation (add_custom_filament)
// and survives serialize/load, so path (a) always fires first. The pair
// fallback only runs for stable_id==0 rows, which cannot exist in a live
// bundle (load_custom_entries re-validates and assigns). This mirrors the m1
// test's "UNREACHABLE but guards the validation perimeter" rationale: if the
// stable_id allocation is ever weakened, this state becomes reachable and
// turns into silent painting loss. Pinned as a known bug.
// ===========================================================================

TEST_CASE("batch_remap mixed stable_id survives non-contiguous physical delete (correct)", "[MixedFilament][batch_remap]")
{
    // 4 physicals, one mixed row {component 1,3, stable_id=S}. Simulate the
    // cleanup sequence for kept={1,3,4} (delete physical 2):
    //   - snapshot old_mixed = [{1,3,S}]
    //   - remove_physical_filament(2) renumbers the live row to {1,2,S}
    //     (component_b 3 -> 2 because 3 > deleted 2; component_a 1 unchanged)
    //   - update_mixed_filament_id_remap(old_mixed, 4, 3)
    // stable_id matches -> old virtual id 5 maps to new virtual id 4. CORRECT.
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets = {"F1", "F2", "F3", "F4"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 3, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    REQUIRE(row.stable_id != 0);
    const uint64_t sid = row.stable_id;

    // Snapshot BEFORE simulating the physical deletion.
    const std::vector<MixedFilament> old_mixed = mgr.mixed_filaments();

    // Simulate remove_physical_filament(2): renumber the live row's component_b
    // (3 -> 2). We mutate the live bundle directly rather than calling
    // remove_physical_filament so the test isolates the remap-table behaviour
    // from the manager's cascade/erase logic (mirrors how the [shrink] tests
    // call set_num_filaments to isolate the pure remap path).
    mgr.mixed_filaments().back().component_b = 2;

    bundle.update_mixed_filament_id_remap(old_mixed, 4, 3);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    // old_total = 4 physical + 1 mixed = 5, +1 for [0] sink -> size 6.
    REQUIRE(remap.size() == 6);
    // Physical branch (tail-truncation): old ids 1..3 identity, id 4 -> 0.
    // (Physical-branch bug for non-head kept sets is covered by the matrix
    //  above; here we focus on the mixed slot at index 5.)
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[4] == 0);
    // Mixed virtual id 5 -> 4 via stable_id match (the new side's single row
    // sits at virtual id 4 = new_num 3 + 1). This is the CORRECT outcome.
    const unsigned int new_vid = virtual_id_for_stable_id(mgr.mixed_filaments(), 3, sid);
    REQUIRE(new_vid == 4);
    CHECK(remap[5] == 4);
}

TEST_CASE("batch_remap mixed pair-fallback (stable_id=0) straddles a deleted physical (CURRENT bug)", "[MixedFilament][batch_remap]")
{
    // Same setup as the stable_id test, but the mixed row has stable_id=0,
    // forcing the pair-fallback path. old_mixed key = canonical(1,3); the
    // renumbered live row's key = canonical(1,2). The keys do not match, so
    // the fallback returns 0 (NONE) — the mixed row's painting is silently
    // dropped. UNREACHABLE in product flows (every live row has a non-zero
    // stable_id), but pinned as a boundary guard for the validation perimeter.
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets = {"F1", "F2", "F3", "F4"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 3, 50, colors);
    // Force the fallback path by zeroing the stable_id (simulates a row that
    // bypassed allocation — cannot exist in a live bundle today).
    mgr.mixed_filaments().back().stable_id = 0;

    const std::vector<MixedFilament> old_mixed = mgr.mixed_filaments();
    // Simulate remove_physical_filament(2): component_b 3 -> 2.
    mgr.mixed_filaments().back().component_b = 2;

    bundle.update_mixed_filament_id_remap(old_mixed, 4, 3);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    REQUIRE(remap.size() == 6);
    // Mixed virtual id 5 -> 0 (NONE): pair key canonical(1,3) not found in the
    // new map keyed by canonical(1,2) because the old side's components are NOT
    // shifted in the batch path (3894 is gated on deleting_filament). This is
    // the bug: painting on this mixed row is dropped.
    CHECK(remap[5] == 0);
}

TEST_CASE("batch_remap mixed pair-fallback (stable_id=0) should match renumbered pair (KNOWN bug)", "[MixedFilament][batch_remap][!shouldfail]")
{
    // Expected-correct oracle for the case above: the pair fallback ought to
    // find the renumbered row. Since old (1,3) and new (1,2) describe the same
    // physical spools after the id-2 deletion, a fallback that applied the same
    // shift the batch path skips (3894-3898) would compute key canonical(1,2)
    // and hit the new map. It currently does not. When the batch mixed branch
    // is taught to honour the actual deletion set (or cleanup stops relying on
    // this path), this test will unexpectedly succeed — drop the tag then.
    MixedAutoGenerateGuard guard(false);
    PresetBundle bundle;
    bundle.filament_presets = {"F1", "F2", "F3", "F4"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 3, 50, colors);
    mgr.mixed_filaments().back().stable_id = 0;

    const std::vector<MixedFilament> old_mixed = mgr.mixed_filaments();
    mgr.mixed_filaments().back().component_b = 2;

    bundle.update_mixed_filament_id_remap(old_mixed, 4, 3);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    REQUIRE(remap.size() == 6);
    // The single live mixed row sits at new virtual id 4 (= new_num 3 + 1).
    CHECK(remap[5] == 4);
}

// ===========================================================================
// Manual-mode painting-loss reproduction (apply "target==src skip" + cleanup
// tail-truncation)
//
// Root-cause chain for the "partial colour corruption" symptom in manual mode
// when the user selects a non-contiguous physical subset like [2,6,8,10]:
//
//   (1) apply_batch_match_to_model (MixedColorMatchHelpers.cpp:1829-1835) builds
//       extruder_remap[src] = target ONLY when target != src. After
//       need_manual_remap the target for a pure recipe pointing at a SELECTED
//       physical (e.g. C6) is the SAME global id as the source (6), so the
//       entry is skipped -> the painting is NOT migrated, it stays on the
//       original global slot (extruder 6).
//   (2) cleanup_unused_filaments_after_batch_match then deletes the unselected
//       physical slots {1,3,4,5,7,9} and builds ONE composite painting remap
//       via build_filament_id_remap(deleting_filament=false) (PresetBundle.cpp:
//       3824-3834), whose physical branch only does tail-truncation
//       (old_id <= new_num -> identity, else -> 0). For survivors packed into
//       low ids {2->1,6->2,8->3,10->4} it maps the ORIGINAL global id 6 -> 0
//       (6 > new_num 4), so the painting still sitting on slot 6 is dropped.
//
// The two tests below reproduce each step with the PURE, test-visible pieces:
//   A. The "target==src skip" rule (inline re-implementation of the
//      extruder_remap build loop) — pure data, no wxGetApp.
//   B. The painting end-state: construct a ModelVolume painted on extruder 6,
//      apply the cleanup batch state_map (tail-truncation), call the pure
//      ModelVolume::remap_extruder_ids, and assert the painting is lost.
//      This uses only libslic3r APIs (Model/TriangleSelector), no wxGetApp.
//
// If either assertion EVER fails to reproduce the loss, the root-cause chain
// above is wrong and must be re-investigated — do NOT relax these oracles.
// ===========================================================================

// Minimal POD mirror of ColorMappingEntry's two fields used by apply's
// extruder_remap build. The real ColorMappingEntry lives in the GUI header
// (MixedColorMatchHelpers.hpp, with wxColour members) which the test binary
// cannot link, so we reproduce only the two fields the build loop reads.
struct ApplyMappingStub {
    std::vector<unsigned int> source_extruder_ids;
    unsigned int              target_filament_id = 0;
};

// Inline re-implementation of apply_batch_match_to_model's extruder_remap build
// (MixedColorMatchHelpers.cpp:1829-1835). Kept byte-faithful to the production
// loop so a change there surfaces here.
static std::unordered_map<int, unsigned int> build_apply_extruder_remap(
    const std::vector<ApplyMappingStub> &mappings)
{
    std::unordered_map<int, unsigned int> extruder_remap;
    for (const auto &mapping : mappings) {
        for (unsigned int src_eid : mapping.source_extruder_ids) {
            if (mapping.target_filament_id != src_eid)
                extruder_remap[static_cast<int>(src_eid)] = mapping.target_filament_id;
        }
    }
    return extruder_remap;
}

TEST_CASE("manual-mode apply skips selected-physical painting (target==src)", "[MixedFilament][batch_apply]")
{
    // Manual subset [2,6,8,10]. A model color painted on extruder 6 (C6, which
    // the user selected) is matched as a pure recipe -> after need_manual_remap
    // target_filament_id == 6 (same global id as the source). apply's
    // extruder_remap build SKIPS it (target==src), so the painting is NOT
    // migrated. Compare with recommended, where the target is a subset id
    // (CMYG 1-4) that differs from the global source -> the painting IS moved.
    ApplyMappingStub manual_pure;
    manual_pure.source_extruder_ids = {6};   // painting on global C6
    manual_pure.target_filament_id  = 6;     // pure recipe -> global C6 after remap
    const auto manual_remap = build_apply_extruder_remap({manual_pure});
    // Manual: painting stays put — NOT in the apply remap table.
    CHECK(manual_remap.find(6) == manual_remap.end());
    CHECK(manual_remap.empty());

    // Recommended: same source 6, but target is a subset id (3) that differs.
    ApplyMappingStub recom_target;
    recom_target.source_extruder_ids = {6};
    recom_target.target_filament_id  = 3;     // CMYG subset id
    const auto recom_remap = build_apply_extruder_remap({recom_target});
    // Recommended: painting IS migrated (6 -> 3).
    REQUIRE(recom_remap.count(6) == 1);
    CHECK(recom_remap.at(6) == 3);
}

TEST_CASE("manual-mode painting on a selected physical is lost after cleanup tail-truncation", "[MixedFilament][batch_apply]")
{
    // Reproduce the consequence of the two-step chain above, using only the
    // pure libslic3r painting API (ModelVolume::remap_extruder_ids). A facet
    // painted on extruder 6 (a selected physical that survived apply unmoved)
    // is then run through cleanup's batch state_map (tail-truncation for
    // new_num=4: old ids 1..4 keep identity, ids > 4 -> 0/NONE), which drops it.
    Model model;
    ModelObject *object = model.add_object();
    object->name = "manual-painting-loss.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();

    // Paint facet 0 on extruder 6 (simulating C6, a user-selected physical).
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(6));
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    // Sanity: facet 0 is painted on extruder 6 before remap.
    REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(6)));

    // cleanup's batch state_map: tail-truncation for new_num_physical=4.
    // Mirrors build_filament_id_remap(deleting_filament=false) at
    // PresetBundle.cpp:3824-3834 (old_id <= new_num -> identity, else -> 0),
    // then Plater.cpp:8429-8437 (mapped==0 -> NONE).
    EnforcerBlockerStateMap state_map;
    for (size_t i = 0; i < state_map.size(); ++i)
        state_map[i] = EnforcerBlockerType(i);
    constexpr size_t new_num_physical = 4;
    for (size_t i = 1; i < state_map.size(); ++i)
        if (i > new_num_physical)
            state_map[i] = EnforcerBlockerType::NONE;

    // Apply the composite remap exactly as cleanup does (Plater.cpp:8445).
    // total_filaments = new_num_physical + 0 mixed (no mixed in this scenario).
    volume->remap_extruder_ids(new_num_physical, state_map);

    // CURRENT (buggy) outcome: the painting on extruder 6 is LOST.
    // build_filament_id_remap's tail-truncation maps old id 6 -> NONE (6 >
    // new_num 4), and FacetsAnnotation::deserialize treats NONE as "unpainted",
    // so the facet data is dropped entirely (mmu_segmentation_facets becomes
    // empty). The correct outcome would be that the painting follows its
    // physical (C6 -> new survivor slot 2), but the batch remap has no notion
    // of which physicals survived — it only knows new_num, not the kept set.
    CHECK(volume->mmu_segmentation_facets.empty());                                       // painting data dropped (the bug)
    CHECK(!volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(2)));  // NOT remapped to survivor slot 2
}

TEST_CASE("manual-mode painting on a selected physical survives with kept-aware state_map (FIXED)", "[MixedFilament][batch_apply]")
{
    // The kept-aware fix's end-to-end painting evidence: with the state_map the
    // fix produces (6 -> 2, the survivor's new slot, instead of 6 -> NONE),
    // the painting on extruder 6 is PRESERVED on survivor slot 2. This pairs
    // with the bug-reproduction test above (which used the old tail-truncation
    // state_map and showed the painting lost) to give before/after evidence
    // per the fix-verification harness. The state_map here mirrors what
    // build_filament_id_remap now emits for kept_physical_ids={2,6,8,10}
    // (see the "batch_remap ... FIXED" tests for the remap table itself).
    Model model;
    ModelObject *object = model.add_object();
    object->name = "manual-painting-survives.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(6)); // painting on selected C6
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(6)));

    // kept-aware state_map: survivors map to their new packed slots.
    // For kept={2,6,8,10}: 2->1, 6->2, 8->3, 10->4; others (incl. 1,3,4,5,7,9) -> NONE.
    EnforcerBlockerStateMap state_map;
    for (size_t i = 0; i < state_map.size(); ++i)
        state_map[i] = EnforcerBlockerType::NONE;
    state_map[2]  = EnforcerBlockerType(1);
    state_map[6]  = EnforcerBlockerType(2);
    state_map[8]  = EnforcerBlockerType(3);
    state_map[10] = EnforcerBlockerType(4);

    constexpr size_t new_num_physical = 4;
    volume->remap_extruder_ids(new_num_physical, state_map);

    // FIXED outcome: painting migrated from old slot 6 to new survivor slot 2.
    CHECK(!volume->mmu_segmentation_facets.empty());                                      // painting preserved
    CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(2)));   // now on survivor slot 2
    CHECK(!volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(6)));  // gone from old slot 6
}

// ===========================================================================
// Modifier & layer-config remap (apply_batch_match_to_model Level-2 + Level-3)
//
// apply_batch_match_to_model lives in the GUI layer (depends on wxGetApp()) so
// the test binary cannot link it. These tests mirror its Level-2 (volume/object
// config extruder) and Level-3 (layer_config_ranges) passes over the REAL
// libslic3r Model API — the same approach the manual-mode tests above use for
// the triangle-painting pass. Keep the mirror byte-faithful to the production
// loop (MixedColorMatchHelpers.cpp); a change there must surface here.
//
// Level-2 reads the object's extruder ONCE before iterating volumes (a
// snapshot). ModelVolume::extruder_id() otherwise falls back to the OBJECT
// config, which this loop rewrites mid-iteration — so re-reading it for a later
// inheriting volume would resolve a different id than the first, making two
// volumes that share one source diverge. The first test below pins that real
// libslic3r contract; the rest pin the apply end-state.
// ===========================================================================

// Mirror of apply_batch_match_to_model's Level-2 (config) + Level-3 (layer)
// passes (MixedColorMatchHelpers.cpp). Covers ONLY those two passes — the
// Level-1 triangle-painting pass is exercised by the manual-mode tests above.
static void apply_config_layer_remap_mirror(
    Model&                                        model,
    const std::unordered_map<int, unsigned int>&  extruder_remap)
{
    for (ModelObject* mo : model.objects) {
        const ConfigOption* obj_opt      = mo->config.option("extruder");
        const int           orig_obj_eid = (obj_opt ? obj_opt->getInt() : 0);
        auto                obj_it       = extruder_remap.find(orig_obj_eid);
        const bool          obj_remap    = (orig_obj_eid > 0 && obj_it != extruder_remap.end());
        bool                object_extruder_written = false;
        for (ModelVolume* mv : mo->volumes) {
            const ModelVolumeType vt = mv->type();
            const bool is_part      = (vt == ModelVolumeType::MODEL_PART);
            const bool is_modifier  = (vt == ModelVolumeType::PARAMETER_MODIFIER);
            if (!is_part && !is_modifier) continue;

            const ConfigOption* vol_opt = mv->config.option("extruder");
            if (vol_opt && vol_opt->getInt() > 0) {
                auto it = extruder_remap.find(vol_opt->getInt());
                if (it != extruder_remap.end())
                    mv->config.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(it->second)));
            } else if (obj_remap && !object_extruder_written) {
                mo->config.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(obj_it->second)));
                object_extruder_written = true;
            }
        }

        for (auto& lr : mo->layer_config_ranges) {
            ModelConfig&         lcfg = lr.second;
            const ConfigOption*  lopt = lcfg.option("extruder");
            if (!lopt) continue;
            const int old_eid = lopt->getInt();
            if (old_eid <= 0) continue;
            auto lit = extruder_remap.find(old_eid);
            if (lit == extruder_remap.end()) continue;
            lcfg.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(lit->second)));
        }
    }
}

TEST_CASE("ModelVolume::extruder_id falls back to live object config (snapshot rationale)", "[MixedFilament][batch_apply]")
{
    // Pins the libslic3r contract that makes apply_batch_match_to_model pre-read
    // the object extruder: a volume with no own "extruder" resolves
    // extruder_id() through the OBJECT config, and that resolution is LIVE —
    // rewriting the object config changes what a later inheriting volume
    // resolves. Without a pre-loop snapshot, two inheriting volumes that share
    // one source would resolve different targets once the loop writes the object
    // config mid-iteration. Pure libslic3r — no mirror.
    Model model;
    ModelObject *obj  = model.add_object();
    ModelVolume  *part = obj->add_volume(make_cube(20., 20., 20.));
    ModelVolume  *mod  = obj->add_volume(make_cube(20., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    obj->config.set_key_value("extruder", new ConfigOptionInt(5));

    // Neither volume owns an extruder → both inherit the object's 5.
    REQUIRE(part->extruder_id() == 5);
    REQUIRE(mod->extruder_id()  == 5);

    // Simulate the apply loop writing the object extruder for the FIRST
    // inheriting volume (5 -> 7). The second inheriting volume now resolves 7
    // — proving extruder_id() reads the CURRENT object config, not a snapshot.
    obj->config.set_key_value("extruder", new ConfigOptionInt(7));
    REQUIRE(part->extruder_id() == 7);
    REQUIRE(mod->extruder_id()  == 7);
}

TEST_CASE("apply config remap: inheriting modifier follows object consistently", "[MixedFilament][batch_apply]")
{
    // The modifier-inclusion fix's core contract: a modifier that inherits the
    // object's extruder must follow the SAME target as the part that inherits
    // it, so their colours stay the same hue. remap {5 -> 7}; both volumes
    // inherit object=5, so both must end on 7 — no divergence, regardless of
    // mo->volumes ordering.
    Model model;
    ModelObject *obj  = model.add_object();
    ModelVolume  *part = obj->add_volume(make_cube(20., 20., 20.));
    ModelVolume  *mod  = obj->add_volume(make_cube(20., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    obj->config.set_key_value("extruder", new ConfigOptionInt(5));

    std::unordered_map<int, unsigned int> remap{{5, 7u}};
    apply_config_layer_remap_mirror(model, remap);

    // Object written once to 7; both inheriting volumes resolve 7.
    REQUIRE(obj->config.opt_int("extruder") == 7);
    REQUIRE(part->extruder_id() == 7);
    REQUIRE(mod->extruder_id()  == 7);
    // Neither volume gained its own config entry — they still inherit.
    REQUIRE_FALSE(part->config.has("extruder"));
    REQUIRE_FALSE(mod->config.has("extruder"));
}

TEST_CASE("apply config remap: modifier with own extruder is remapped alone", "[MixedFilament][batch_apply]")
{
    // A modifier that carries its own extruder (user picked a colour in the
    // object list) is remapped on its OWN config only; the object and any other
    // volume are untouched.
    Model model;
    ModelObject *obj  = model.add_object();
    ModelVolume  *part = obj->add_volume(make_cube(20., 20., 20.));
    ModelVolume  *mod  = obj->add_volume(make_cube(20., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    obj->config.set_key_value("extruder",  new ConfigOptionInt(1)); // object default (not remapped)
    part->config.set_key_value("extruder", new ConfigOptionInt(3));
    mod->config.set_key_value("extruder",  new ConfigOptionInt(4));

    std::unordered_map<int, unsigned int> remap{{3, 7u}, {4, 8u}};
    apply_config_layer_remap_mirror(model, remap);

    REQUIRE(part->extruder_id() == 7);
    REQUIRE(mod->extruder_id()  == 8);
    REQUIRE(obj->config.opt_int("extruder") == 1); // object untouched
}

TEST_CASE("apply layer remap: hit is remapped, miss is left untouched", "[MixedFilament][batch_apply]")
{
    // Level-3 (layer_config_ranges): a height range whose extruder is in the
    // remap follows the match; one whose extruder is NOT in the remap stays put
    // (so a layer on an unchanged slot is not reset to default by cleanup).
    Model model;
    ModelObject *obj = model.add_object();
    obj->add_volume(make_cube(20., 20., 20.));
    obj->layer_config_ranges[t_layer_height_range{0.0, 1.0}].set_key_value("extruder", new ConfigOptionInt(5));
    obj->layer_config_ranges[t_layer_height_range{1.0, 2.0}].set_key_value("extruder", new ConfigOptionInt(9));

    std::unordered_map<int, unsigned int> remap{{5, 7u}}; // 9 absent → miss
    apply_config_layer_remap_mirror(model, remap);

    REQUIRE(obj->layer_config_ranges.at(t_layer_height_range{0.0, 1.0}).opt_int("extruder") == 7);
    REQUIRE(obj->layer_config_ranges.at(t_layer_height_range{1.0, 2.0}).opt_int("extruder") == 9);
}

TEST_CASE("manual-mode apply migrates unselected-physical painting off its slot", "[MixedFilament][batch_apply]")
{
    // Validates the precondition for the kept-aware cleanup fix (fix-verification
    // side-effects item). The fix maps unselected physical ids -> 0 (NONE) in
    // cleanup's batch state_map. That is only safe if apply has ALREADY moved
    // the painting off those ids — otherwise mapping to 0 drops residual data.
    //
    // Setup mirrors manual subset [2,6,8,10]: facet A painted on extruder 6
    // (SELECTED, apply skips it — target==src), facet B painted on extruder 3
    // (UNSELECTED, matched to C6 so target=6 != src=3, apply migrates it).
    // After apply's state_map (6->6 identity, 3->6 migrate), extruder 3 must be
    // EMPTY (its painting moved to 6). This is what makes kept-aware mapping
    // 3 -> 0 safe: nothing is left on 3 to lose.
    Model model;
    ModelObject *object = model.add_object();
    object->name = "apply-migrate-unselected.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(6)); // facet A: selected physical C6
    selector.set_facet(1, EnforcerBlockerType(3)); // facet B: unselected physical C3
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(6)));
    REQUIRE(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(3)));

    // apply_batch_match_to_model's state_map for this scenario:
    //   C6 selected, pure recipe, target==src -> identity (6->6)
    //   C3 unselected, matched to C6, target=6 != src=3 -> migrate (3->6)
    // (Mirrors MixedColorMatchHelpers.cpp:1849-1861 state_map construction.)
    EnforcerBlockerStateMap apply_state_map;
    for (size_t i = 0; i < apply_state_map.size(); ++i)
        apply_state_map[i] = EnforcerBlockerType(i);
    apply_state_map[3] = EnforcerBlockerType(6); // C3 painting migrated to C6
    // total_filaments: pre-cleanup palette still has all 10 physicals here.
    constexpr size_t pre_cleanup_total = 10;
    volume->remap_extruder_ids(pre_cleanup_total, apply_state_map);

    // After apply: C3 is EMPTY (its painting moved to 6). C6 holds both facets.
    CHECK(!volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(3))); // migrated away
    CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(6)));  // both facets now on 6

    // THEN cleanup's kept-aware state_map is safe to map 3 -> 0 (nothing left on
    // 3 to drop) and 6 -> 2 (survivor's new slot). This is the fix's guarantee.
}

// ===========================================================================
// rebuild_match_thumb_cache color-substitution rule (the "After Match" preview).
//
// Production function: MixedFilamentBatchDialog::rebuild_match_thumb_cache
// (MixedFilamentBatchDialog.cpp:567-601). It builds the preview color vector
// m_match_colors that the After-Match thumbnail renders against, by applying
// each match mapping onto a base vector of [physical filament_colour ...][mixed
// display_color ...] indexed by extruder_id-1.
//
// Why this is pinned separately from apply_batch_match_to_model:
//   The preview runs BEFORE the model is modified (no confirm yet), so the
//   render pipeline still indexes m_match_colors by the ORIGINAL extruder id
//   (GLVolume::simple_render, 3DScene.cpp:573 reads extruder_colors[idx-1]
//   for painted facets; render_match_thumb_for_plate:636 sets
//   vol->color = m_match_colors[vol->extruder_id-1] for unpainted geometry).
//   Therefore the preview must substitute the SOURCE slots with the matched
//   color so that, visually, it matches what apply_batch_match_to_model
//   (MixedColorMatchHelpers.cpp:1829-1835, src->target id remap) produces once
//   confirmed. The bug this pins: the production loop used to match slots by
//   wxColour == source_color and `break` on the first hit, so when two extruder
//   slots share one source color (e.g. two cubes both painted red but on
//   different extruder ids) only the first slot was substituted -> the second
//   cube rendered as the original model. The fix iterates ALL
//   source_extruder_ids per mapping with no break.
//
// Same POD-stub discipline as the [batch_apply] block above (the real
// ColorMappingEntry lives in the GUI header with wxColour members the test
// binary cannot link), byte-faithful to the fixed production loop so a
// regression surfaces here. Colors use packed RGB (0xRRGGBB) to avoid wx.
// ===========================================================================

struct PreviewMappingStub {
    std::vector<unsigned int> source_extruder_ids; // mirrors ColorMappingEntry::source_extruder_ids
    unsigned int              target_filament_id = 0;
    uint32_t                  matched_rgb = 0;      // packed RGB (mirrors matched_color)
};

// Inline re-implementation of rebuild_match_thumb_cache's color-substitution
// loop (MixedFilamentBatchDialog.cpp:567-601), FIXED rule. For each mapping,
// substitute EVERY source extruder slot (index = src-1) with the matched color,
// no break. An empty source_extruder_ids is a natural no-op (the loop body never
// runs); a defensive target==0 skip is kept but is a dead branch in practice —
// assign_batch_virtual_filament_ids (MixedColorMatchHelpers.cpp:1510-1524)
// always assigns a non-zero target. Kept byte-faithful to the (fixed) production
// loop so a regression surfaces here.
static std::vector<uint32_t> build_match_preview_colors(
    const std::vector<uint32_t>&           base_colors, // initial [physical...][virtual...], index=extruder_id-1
    const std::vector<PreviewMappingStub>& mappings)
{
    std::vector<uint32_t> out = base_colors;
    for (const auto& mapping : mappings) {
        if (mapping.target_filament_id == 0) continue; // dead branch (target always non-zero); kept defensively
        for (unsigned int src_eid : mapping.source_extruder_ids) {
            if (src_eid == 0) continue;
            const size_t idx = static_cast<size_t>(src_eid - 1);
            if (idx >= out.size()) out.resize(idx + 1, 0x80808080u); // pad gray (ensure_slot)
            out[idx] = mapping.matched_rgb;
        }
    }
    return out;
}

TEST_CASE("preview colors: same color on multiple extruder slots all get substituted", "[MixedFilament][batch_preview]")
{
    // Root-cause reproduction for the reported "two cubes, one rendered matched,
    // one rendered as the original model" bug. Two cubes are both painted red,
    // but red occupies extruder slots 2 and 5 (different extruder ids sharing one
    // source color). The match maps red -> purple and carries
    // source_extruder_ids = {2, 5}. The preview MUST substitute BOTH source
    // slots; the old production loop matched by wxColour == source_color and
    // `break`-ed on the first hit (slot 2), leaving slot 5 as red -> cube B kept
    // its original color. (MixedFilamentBatchDialog.cpp:567-601.)
    constexpr uint32_t RED    = 0xFF0000;
    constexpr uint32_t PURPLE = 0x800080;
    constexpr uint32_t OTHER  = 0x123456; // unrelated slot color, must be untouched
    // base_colors indexed by extruder_id-1: slot1=OTHER, slot2=RED, slot3=OTHER,
    // slot4=OTHER, slot5=RED.
    const std::vector<uint32_t> base = {OTHER, RED, OTHER, OTHER, RED};

    PreviewMappingStub m;
    m.source_extruder_ids = {2, 5}; // both cubes' red
    m.target_filament_id  = 6;      // virtual slot for the purple mix
    m.matched_rgb         = PURPLE;

    const auto out = build_match_preview_colors(base, {m});

    // BOTH source slots substituted (the fix). Old code would leave out[4]==RED.
    REQUIRE(out.size() >= 5);
    CHECK(out[1] == PURPLE); // slot 2 (cube A) -> matched
    CHECK(out[4] == PURPLE); // slot 5 (cube B) -> matched (was the bug: stayed RED)
    // Unrelated slots untouched.
    CHECK(out[0] == OTHER);
    CHECK(out[2] == OTHER);
    CHECK(out[3] == OTHER);
}

TEST_CASE("preview colors: single source extruder substitutes exactly one slot", "[MixedFilament][batch_preview]")
{
    // Regression baseline: the common single-extruder case substitutes exactly
    // one slot and leaves everything else untouched. Guards against an over-broad
    // substitution fix that would repaint unrelated slots.
    constexpr uint32_t RED  = 0xFF0000;
    constexpr uint32_t BLUE = 0x0000FF;
    constexpr uint32_t GRN  = 0x00FF00;
    const std::vector<uint32_t> base = {RED, BLUE, GRN};

    PreviewMappingStub m;
    m.source_extruder_ids = {1};
    m.target_filament_id  = 4;
    m.matched_rgb         = 0x111111;

    const auto out = build_match_preview_colors(base, {m});
    REQUIRE(out.size() == 3);
    CHECK(out[0] == 0x111111); // slot 1 substituted
    CHECK(out[1] == BLUE);     // untouched
    CHECK(out[2] == GRN);      // untouched
}

TEST_CASE("preview colors: empty source_extruder_ids substitutes nothing", "[MixedFilament][batch_preview]")
{
    // A mapping with empty source_extruder_ids must leave every slot untouched
    // (the loop body never runs). This is the real no-op condition the loop
    // relies on: assign_batch_virtual_filament_ids always assigns a non-zero
    // target_filament_id, so the target==0 skip is a dead branch in practice and
    // cannot guard an empty-source mapping. Guards against a regression that
    // would substitute a stale matched_rgb onto an unrelated slot when the source
    // list is empty (defensive: shouldn't happen in normal match output).
    constexpr uint32_t RED = 0xFF0000;
    const std::vector<uint32_t> base = {RED, 0x00FF00};

    PreviewMappingStub m;
    m.source_extruder_ids = {};      // empty -> no-op
    m.target_filament_id  = 4;       // non-zero (the normal case)
    m.matched_rgb         = 0x222222;

    const auto out = build_match_preview_colors(base, {m});
    REQUIRE(out.size() == 2);
    CHECK(out[0] == RED);       // untouched
    CHECK(out[1] == 0x00FF00);  // untouched
}

TEST_CASE("preview colors: virtual slot beyond base vector is padded gray", "[MixedFilament][batch_preview]")
{
    // A source extruder id pointing past the base vector (a mixed-filament
    // virtual slot whose display_color isn't in the initial m_match_colors) must
    // resize-and-pad so the render pipeline's extruder_colors[idx-1] read stays
    // in range. Mirrors the ensure_slot resize in the production loop.
    const std::vector<uint32_t> base = {0xFF0000}; // only 1 physical slot

    PreviewMappingStub m;
    m.source_extruder_ids = {4}; // virtual slot 4, beyond base size
    m.target_filament_id  = 4;
    m.matched_rgb         = 0x333333;

    const auto out = build_match_preview_colors(base, {m});
    REQUIRE(out.size() == 4);              // grown to hold index 3
    CHECK(out[0] == 0xFF0000);             // existing slot untouched
    CHECK(out[3] == 0x333333);             // new slot substituted
    // Padded holes (indices 1,2) are the gray fill, not 0.
    CHECK(out[1] == 0x80808080u);
    CHECK(out[2] == 0x80808080u);
}

// ============================================================================
// [MixedFilament][deletion_remap] — build_mixed_deletion_painting_remap
//
// Regression coverage for the batch-match cleanup bug where deleting redundant
// mixed rows (after duplicate-recipe merge) re-enumerates the virtual IDs of
// the remaining rows, but the model's painted facets kept the old (pre-deletion)
// IDs, so they resolved to the wrong mixed filament — the last merged slot
// appeared to fall back to color #1.
//
// The fix extracts the T2(pre-delete)→T3(post-delete) painting remap into a
// pure, library-testable function. These are SPEC tests (the math is defined
// independently of any runtime), not characterization tests, so a hand-written
// expected value is a valid oracle here (differential-oracle harness §5).
//
// Mapping rule under test, for each old_vid in T2 space:
//   old_vid ∈ deleted_vids               -> 0   (NONE; the row itself)
//   old_vid <= num_physical              -> old_vid (physical slots are identity)
//   else                                 -> old_vid - count(deleted_vids < old_vid)
// Return vector is sized t2_total_filaments + 1 (1-based; index 0 is unused/0).
// ===========================================================================

TEST_CASE("build_mixed_deletion_painting_remap: single middle delete shifts tail down by one", "[MixedFilament][deletion_remap]")
{
    // The reported scenario: 4 physicals, mixed rows v5..v14. After duplicate-
    // recipe merge, v9 is redundant and gets deleted. Every vid > 9 must shift
    // down by one; v9 itself maps to NONE.
    const size_t num_physical = 4;
    const size_t t2_total     = 14; // 4 physical + 10 mixed (v5..v14)
    const std::vector<unsigned int> deleted = {9};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[0] == 0u); // unused index
    // Physical slots: identity.
    CHECK(remap[1] == 1u);
    CHECK(remap[2] == 2u);
    CHECK(remap[3] == 3u);
    CHECK(remap[4] == 4u);
    // Mixed slots before the deleted one: identity.
    CHECK(remap[5] == 5u);
    CHECK(remap[6] == 6u);
    CHECK(remap[7] == 7u);
    CHECK(remap[8] == 8u);
    // Deleted slot itself -> NONE.
    CHECK(remap[9] == 0u);
    // Mixed slots after the deleted one: shift down by one.
    CHECK(remap[10] == 9u);
    CHECK(remap[11] == 10u);
    CHECK(remap[12] == 11u);
    CHECK(remap[13] == 12u);
    CHECK(remap[14] == 13u);
}

TEST_CASE("build_mixed_deletion_painting_remap: multiple deletes accumulate offset", "[MixedFilament][deletion_remap]")
{
    // Delete v7 and v9 out of v5..v14. Each survivor's new id subtracts the
    // number of deleted vids strictly less than it.
    const size_t num_physical = 4;
    const size_t t2_total     = 14;
    const std::vector<unsigned int> deleted = {7, 9};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[0] == 0u);
    // Physical + pre-first-delete mixed: identity.
    CHECK(remap[5] == 5u);
    CHECK(remap[6] == 6u);
    // Deleted.
    CHECK(remap[7] == 0u);
    // v8: one deleted vid (7) below it -> 8-1 = 7.
    CHECK(remap[8] == 7u);
    // Deleted.
    CHECK(remap[9] == 0u);
    // v10..v14: two deleted vids (7,9) below each -> subtract 2.
    CHECK(remap[10] == 8u);
    CHECK(remap[11] == 9u);
    CHECK(remap[12] == 10u);
    CHECK(remap[13] == 11u);
    CHECK(remap[14] == 12u);
}

TEST_CASE("build_mixed_deletion_painting_remap: physical slots never move", "[MixedFilament][deletion_remap]")
{
    // Even with mixed deletes, the physical range [1..num_physical] is identity.
    const size_t num_physical = 4;
    const size_t t2_total     = 8;
    const std::vector<unsigned int> deleted = {5, 6};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[1] == 1u);
    CHECK(remap[2] == 2u);
    CHECK(remap[3] == 3u);
    CHECK(remap[4] == 4u);
}

TEST_CASE("build_mixed_deletion_painting_remap: deleted vids map to NONE", "[MixedFilament][deletion_remap]")
{
    const size_t num_physical = 4;
    const size_t t2_total     = 10;
    const std::vector<unsigned int> deleted = {6, 8};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    CHECK(remap[6] == 0u);
    CHECK(remap[8] == 0u);
}

TEST_CASE("build_mixed_deletion_painting_remap: delete all mixed leaves only physicals", "[MixedFilament][deletion_remap]")
{
    // Deleting every mixed row: the survivors are the physicals alone, all
    // unchanged; every mixed vid maps to NONE.
    const size_t num_physical = 4;
    const size_t t2_total     = 7; // 4 physical + 3 mixed (v5,v6,v7)
    const std::vector<unsigned int> deleted = {5, 6, 7};

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[1] == 1u);
    CHECK(remap[2] == 2u);
    CHECK(remap[3] == 3u);
    CHECK(remap[4] == 4u);
    CHECK(remap[5] == 0u);
    CHECK(remap[6] == 0u);
    CHECK(remap[7] == 0u);
}

TEST_CASE("build_mixed_deletion_painting_remap: empty delete list is identity (short-circuit)", "[MixedFilament][deletion_remap]")
{
    // No deletes -> every vid maps to itself. This is the no-op path cleanup
    // must take without running a remap pass over the whole model.
    const size_t num_physical = 4;
    const size_t t2_total     = 10;
    const std::vector<unsigned int> deleted;

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    for (size_t i = 1; i <= t2_total; ++i)
        CHECK(remap[i] == static_cast<unsigned int>(i));
}

TEST_CASE("build_mixed_deletion_painting_remap: unsorted delete input is tolerated", "[MixedFilament][deletion_remap]")
{
    // Robustness: caller may supply vids in any order; the function must sort
    // internally so the offset count is correct. Same expectation as the
    // ordered {7,9} case.
    const size_t num_physical = 4;
    const size_t t2_total     = 14;
    const std::vector<unsigned int> deleted = {9, 7}; // descending input

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[7] == 0u);
    CHECK(remap[8] == 7u); // one delete below
    CHECK(remap[9] == 0u);
    CHECK(remap[10] == 8u); // two deletes below
    CHECK(remap[14] == 12u);
}

TEST_CASE("build_mixed_deletion_painting_remap: duplicate ids in delete list dedupe (offset not inflated)", "[MixedFilament][deletion_remap]")
{
    // Robustness: a caller that collects the same vid twice (e.g. from
    // overlapping reference scans) must not double-count it — the function
    // sorts + uniques internally, so {9,9,7} is equivalent to {7,9}.
    // The batch-match cleanup reuses this table to drive both painting and
    // config-level extruder remaps, so an inflated offset would shift every
    // survivor below the duplicate onto the wrong row.
    const size_t num_physical = 4;
    const size_t t2_total     = 14;
    const std::vector<unsigned int> deleted = {9, 9, 7}; // duplicate 9

    const auto remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, t2_total, deleted);

    REQUIRE(remap.size() == t2_total + 1);
    CHECK(remap[7] == 0u);
    CHECK(remap[8] == 7u); // one delete below (7 only; 9 is above)
    CHECK(remap[9] == 0u);
    CHECK(remap[10] == 8u); // two deletes below (7, 9) — NOT three
    CHECK(remap[14] == 12u);
}

// ============================================================================
// [MixedFilament][config_extruder_remap] — cascade gap: config "extruder"
// references are adjusted per-deletion (naive), while the virtual-ID space also
// contracts by the cascade-removed mixed rows. This test pins the CORRECT
// cascade-aware result and currently FAILS (CURRENT BUG) — it reproduces the
// batch-match physical-deletion cascade under-shift in pure libslic3r terms.
//
// Scenario: 4 physicals {1,2,3,4}, mixed rows A(1,2)=v5, B(2,3)=v6, C(3,4)=v7.
// Batch match keeps {1,3,4} → physical 2 is deleted → A and B cascade-removed
// (both reference physical 2); C survives and its recipe (3,4) renumbers to
// (2,3). An object pinned to C holds config "extruder" = 7.
//
//   CORRECT: C's new virtual id = new_num_physical(3) + position(1) = 4.
//   ACTUAL : GUI_ObjectList.cpp:857-969 (driven per deletion by
//            Plater::on_filaments_delete, Plater.cpp:21253) subtracts exactly 1
//            per deleted physical ("if extruder > deleted_id then -1") → 7 → 6.
//            The cascade-removed rows ahead of C are never accounted for, so
//            the config lands on a renumbered survivor (6) or — when out of
//            range — is reset to default by update_objects_list_filament_column
//            (GUI_ObjectList.cpp:694-700). The painting path IS cascade-aware
//            (kept-aware composite remap, PresetBundle.cpp:3850-3854); the
//            config path is not.
//
//   HIDDEN ([.]): this test intentionally FAILS (6 != 4). The `actual` side is
//   a hand-rolled simulation of the naive decrement INSIDE the test, not a call
//   into production — so fixing the production cascade config remap will NOT
//   flip this test green, and [!shouldfail] would never fire its "unexpectedly
//   succeeded" signal. It is documentation, not a regression sentinel.
//
//   PRE-EXISTING: the naive per-deletion config decrement (GUI_ObjectList.cpp:
//   857-969) predates this PR; the cascade-aware config remap is tracked as a
//   follow-up (see Plater.cpp remap_config_extruder — it currently skips
//   out-of-range config references silently). When the follow-up lands, rewrite
//   the `actual` side to assert the production result == 4 and remove the [.]
//   tag.
// ============================================================================
TEST_CASE("config_extruder cascade: per-deletion decrement under-counts cascade rows (CURRENT BUG)",
          "[MixedFilament][config_extruder_remap][.]")
{
    // --- Correct side: real libslic3r cascade + production kept-aware remap ---
    MixedAutoGenerateGuard guard(false); // keep add_custom_filament from auto-generating gradient rows
    PresetBundle bundle;
    bundle.filament_presets = {"F1", "F2", "F3", "F4"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values =
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();
    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 2, 50, colors); // A → v5
    mgr.add_custom_filament(2, 3, 50, colors); // B → v6
    mgr.add_custom_filament(3, 4, 50, colors); // C → v7
    REQUIRE(mgr.enabled_count() == 3);

    const std::vector<MixedFilament> old_mixed = mgr.mixed_filaments(); // T2 snapshot

    // Delete physical 2 (kept {1,3,4} → new_num_physical 3). Real cascade.
    mgr.remove_physical_filament(2);
    REQUIRE(mgr.enabled_count() == 1); // A, B cascaded away; C survives

    // Production kept-aware remap — the oracle the PAINTING path trusts:
    // old vid 7 (C) must map to 4.
    bundle.update_mixed_filament_id_remap(old_mixed, 4, 3, size_t(-1), {1, 3, 4});
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() > 7);
    CHECK(remap[7] == 4u); // oracle sanity: C → 4

    // --- Actual side: GUI_ObjectList naive decrement for the single deletion ---
    const int config_ref_before = 7; // object pinned to C (old vid)
    int actual = config_ref_before;
    if (actual == 2)
        actual = -1; // == deleted id → replace (batch cleanup passes -1)
    else if (actual > 2)
        actual -= 1; // per-deletion "> deleted_id → -1"

    // The naive adjustment must land on the row the config actually points at.
    // It does not (6 != 4) — the config keeps a stale virtual id. This CHECK
    // FAILS until the config path is made cascade-aware (currently only
    // painting gets the kept-aware composite remap).
    CHECK(actual == static_cast<int>(remap[7]));
}

// ============================================================================
// [MixedFilament][FilamentColor] — dual-color (multi-colour) slot primary
// derivation. The batch-match dialog derives a dual-color slot's effective
// match colour as the first valid token of filament_multi_colors (the app-wide
// PrimaryColor rule used by PresetBundle's sync), falling back to the raw
// filament_colour value when no valid token exists. These cases pin the
// libslic3r primitives the dialog's slot_match_color wrapper relies on.
// ============================================================================
TEST_CASE("Dual-color multi string primary is the first valid colour", "[MixedFilament][FilamentColor]")
{
    const auto parts = SplitFilamentMultiColors("#AABBCC|#112233");
    REQUIRE(parts.size() == 2);
    CHECK(FilamentColor::FromColors(parts, FilamentColorMode::Segment).PrimaryColor("#26A69A") == "#AABBCC");
}

TEST_CASE("Dual-color primary drops invalid tokens and falls back on empty", "[MixedFilament][FilamentColor]")
{
    const auto parts = SplitFilamentMultiColors("#AABBCC|not-a-color|#112233");
    REQUIRE(parts.size() == 2); // invalid token whitelisted away
    CHECK(FilamentColor::FromColors(parts, FilamentColorMode::Segment).PrimaryColor() == "#AABBCC");
    CHECK(FilamentColor::FromColors({}, FilamentColorMode::Segment).PrimaryColor("#26A69A") == "#26A69A");
}
