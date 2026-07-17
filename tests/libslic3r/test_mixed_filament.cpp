#include <catch2/catch.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfConstants.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfJson.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfLegacyBridge.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfReader.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfValidation.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfWriter.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::FullSpectrum3mf;

namespace {

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
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(gradient, settings) == 30);
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(gradient, settings, true));
}

TEST_CASE("Mixed filament bias helper maps signed bias to a one-sided safe offset pair", "[MixedFilament]")
{
    const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.06f, 0.4f);
    CHECK(offset_a == Approx(0.0f));
    CHECK(offset_b == Approx(0.06f));

    CHECK(MixedFilamentManager::bias_ui_value_from_surface_offsets(offset_a, offset_b, 0.4f) == Approx(0.06f));

    CHECK(MixedFilamentManager::bias_ui_value_from_surface_offsets(0.02f, 0.0f, 0.4f) == Approx(-0.02f));
    CHECK(MixedFilamentManager::bias_ui_value_from_surface_offsets(-0.02f, 0.0f, 0.4f) == Approx(0.02f));

    const auto [negative_a, negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.06f, 0.4f);
    CHECK(negative_a == Approx(0.06f));
    CHECK(negative_b == Approx(0.0f));

    const auto [unclamped_a, unclamped_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.30f, 0.4f);
    CHECK(unclamped_a == Approx(0.0f));
    CHECK(unclamped_b == Approx(0.30f));

    const auto [unclamped_negative_a, unclamped_negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.30f, 0.4f);
    CHECK(unclamped_negative_a == Approx(0.30f));
    CHECK(unclamped_negative_b == Approx(0.0f));

    const auto [clamped_a, clamped_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.40f, 0.4f);
    CHECK(clamped_a == Approx(0.0f));
    CHECK(clamped_b == Approx(0.35f));

    const auto [clamped_negative_a, clamped_negative_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.40f, 0.4f);
    CHECK(clamped_negative_a == Approx(0.35f));
    CHECK(clamped_negative_b == Approx(0.0f));
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
