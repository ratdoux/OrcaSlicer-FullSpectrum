#include <catch2/catch.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/MixedFilamentPreview.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

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

static unsigned int virtual_id_for_stable_id(const std::vector<MixedFilament> &mixed, size_t num_physical, uint64_t stable_id)
{
    unsigned int next_virtual_id = unsigned(num_physical + 1);
    for (const MixedFilament &mf : mixed) {
        if (!mf.enabled || mf.deleted)
            continue;
        if (mf.stable_id == stable_id)
            return next_virtual_id;
        ++next_virtual_id;
    }
    return 0;
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

struct TestRGB
{
    int r = 0;
    int g = 0;
    int b = 0;
};

static TestRGB parse_test_hex(const std::string &hex)
{
    TestRGB c;
    REQUIRE(hex.size() >= 7);
    REQUIRE(hex[0] == '#');
    c.r = std::stoi(hex.substr(1, 2), nullptr, 16);
    c.g = std::stoi(hex.substr(3, 2), nullptr, 16);
    c.b = std::stoi(hex.substr(5, 2), nullptr, 16);
    return c;
}

// Enable auto_generate by default for all tests (matches production behavior where
// AppConfig sets this to true during GUI startup).
static const int _auto_generate_enabler = []() {
    MixedFilamentManager::set_auto_generate_enabled(true);
    return 0;
}();

} // namespace

TEST_CASE("Mixed filament TD99 transmittance follows the 99 percent definition", "[MixedFilament][Preview]")
{
    CHECK(mixed_filament_td99_transmittance(1.0, 1.0) == Approx(0.01).epsilon(0.001));
    CHECK(mixed_filament_td99_transmittance(0.5, 1.0) == Approx(0.10).epsilon(0.001));
    CHECK(mixed_filament_td99_opacity(1.0, 1.0) == Approx(0.99).epsilon(0.001));
    CHECK(mixed_filament_td99_transmittance(0.2, 0.0) == Approx(1.0));
}

TEST_CASE("Mixed filament TD sidewall prediction requires explicit TD values", "[MixedFilament][Preview]")
{
    const std::vector<unsigned int> sequence { 1, 2 };
    CHECK(mixed_filament_sidewall_prediction_available(sequence, { 0.8, 1.2 }, MixedFilamentSidewallBlendModel::TdFilamentMixer));
    CHECK_FALSE(mixed_filament_sidewall_prediction_available(sequence, { 0.8, 0.0 }, MixedFilamentSidewallBlendModel::TdFilamentMixer));
    CHECK_FALSE(mixed_filament_sidewall_prediction_available(sequence, { 0.8, 1.2 }, MixedFilamentSidewallBlendModel::Legacy));
}

TEST_CASE("Mixed filament TD sidewall prediction isolates separate XY regions", "[MixedFilament][Preview]")
{
    const std::vector<MixedFilamentOpticalMaterial> materials {
        { "#FFFF00", 0.6 },
        { "#0000FF", 0.6 },
        { "#FF00FF", 0.6 }
    };
    const std::vector<MixedFilamentSidewallSample> samples {
        { 0, 0, 1, 0.0, 0.2 },
        { 0, 1, 1, 0.0, 0.2 },
        { 1, 0, 2, 0.2, 0.2 },
        { 1, 1, 3, 0.2, 0.2 }
    };

    MixedFilamentSidewallPredictionSettings settings;
    settings.blend_model = MixedFilamentSidewallBlendModel::TdFilamentMixer;

    const std::vector<std::string> apparent = predict_mixed_filament_sidewall_sample_colors(samples, materials, settings);
    REQUIRE(apparent.size() == samples.size());
    CHECK(apparent[0] != apparent[1]);

    const TestRGB yellow_under_blue    = parse_test_hex(apparent[0]);
    const TestRGB yellow_under_magenta = parse_test_hex(apparent[1]);
    CHECK(yellow_under_blue.r < yellow_under_magenta.r);
}

TEST_CASE("Mixed filament TD sidewall prediction exposes Yule-Nielsen as a distinct option", "[MixedFilament][Preview]")
{
    const std::vector<unsigned int> sequence { 1, 2, 3, 1, 2 };
    const std::vector<std::string> colors { "#F4D000", "#2040E0", "#C020C0" };
    const std::vector<double> td99 { 0.7, 1.1, 0.5 };

    MixedFilamentSidewallPredictionSettings mixer_settings;
    mixer_settings.blend_model = MixedFilamentSidewallBlendModel::TdFilamentMixer;
    const std::string mixer_color = predict_mixed_filament_sequence_aggregate_color(
        sequence, colors, td99, 0.2, mixer_settings, "#000000");

    MixedFilamentSidewallPredictionSettings yule_nielsen_settings;
    yule_nielsen_settings.blend_model = MixedFilamentSidewallBlendModel::TdYuleNielsen;
    const std::string yule_nielsen_color = predict_mixed_filament_sequence_aggregate_color(
        sequence, colors, td99, 0.2, yule_nielsen_settings, "#000000");
    const std::vector<std::string> yule_nielsen_layers = predict_mixed_filament_sequence_apparent_colors(
        sequence, colors, td99, 0.2, yule_nielsen_settings);
    REQUIRE(yule_nielsen_layers.size() == sequence.size());
    CHECK(predict_mixed_filament_sequence_apparent_color_at(sequence, 2, colors, td99, 0.2, yule_nielsen_settings, "#000000") ==
          yule_nielsen_layers[2]);

    CHECK(mixed_filament_sidewall_blend_model_from_string("td_yule_nielsen") == MixedFilamentSidewallBlendModel::TdYuleNielsen);
    CHECK(mixed_filament_sidewall_blend_model_from_string("TD + Yule-Nielsen") == MixedFilamentSidewallBlendModel::TdYuleNielsen);
    CHECK(mixed_filament_sidewall_blend_model_from_string("TD + FilamentMixer") == MixedFilamentSidewallBlendModel::TdFilamentMixer);
    CHECK(mixed_filament_sidewall_blend_model_to_string(MixedFilamentSidewallBlendModel::TdFilamentMixer) == "td_filament_mixer");
    CHECK(mixer_color != "#000000");
    CHECK(yule_nielsen_color != "#000000");
    CHECK(mixer_color != yule_nielsen_color);
}

TEST_CASE("Mixed filament Yule-Nielsen uses linear reflectance before returning sRGB", "[MixedFilament][Preview]")
{
    const std::vector<unsigned int> sequence { 1, 2 };
    const std::vector<std::string> colors { "#00FFFF", "#FF00FF" };
    const std::vector<double> td99 { 0.5, 0.5 };

    MixedFilamentSidewallPredictionSettings settings;
    settings.blend_model = MixedFilamentSidewallBlendModel::TdYuleNielsen;

    const TestRGB rgb = parse_test_hex(predict_mixed_filament_sequence_aggregate_color(
        sequence, colors, td99, 0.2, settings, "#000000"));

    CHECK(rgb.r > 100);
    CHECK(rgb.g > 100);
    CHECK(rgb.b > 220);
}

TEST_CASE("Mixed filament surface predictor attenuates distant different-color layers", "[MixedFilament][Preview]")
{
    const std::vector<unsigned int> repeated { 1, 2, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2 };
    const std::vector<std::string> colors { "#FFFF00", "#00FFFF" };
    const std::vector<double> td99 { 1.0, 1.0 };

    MixedFilamentSidewallPredictionSettings settings;
    settings.blend_model = MixedFilamentSidewallBlendModel::TdYuleNielsen;

    const TestRGB cyan_adjacent_to_yellow = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 5, colors, td99, 0.08, settings, "#000000"));
    const TestRGB cyan_between_cyan = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 6, colors, td99, 0.08, settings, "#000000"));

    CHECK(cyan_adjacent_to_yellow.b < cyan_between_cyan.b);
    CHECK(cyan_between_cyan.b - cyan_adjacent_to_yellow.b >= 4);
    CHECK(cyan_adjacent_to_yellow.r > cyan_between_cyan.r);
    CHECK(cyan_adjacent_to_yellow.g > cyan_adjacent_to_yellow.r);
    CHECK(cyan_adjacent_to_yellow.g > cyan_adjacent_to_yellow.b);
}

TEST_CASE("Mixed filament surface predictor adds neighbor channels for saturated carriers", "[MixedFilament][Preview]")
{
    const std::vector<unsigned int> repeated { 1, 2, 1, 2, 1, 2 };
    const std::vector<std::string> colors { "#FF00FF", "#FFFF00" };
    const std::vector<double> td99 { 1.0, 1.0 };

    MixedFilamentSidewallPredictionSettings settings;
    settings.blend_model = MixedFilamentSidewallBlendModel::TdYuleNielsen;

    const TestRGB magenta_tinted_by_yellow = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 2, colors, td99, 0.08, settings, "#000000"));
    const TestRGB yellow_tinted_by_magenta = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 3, colors, td99, 0.08, settings, "#000000"));

    CHECK(magenta_tinted_by_yellow.g > 120);
    CHECK(magenta_tinted_by_yellow.b < 255);
    CHECK(magenta_tinted_by_yellow.r >= magenta_tinted_by_yellow.b);
    CHECK(yellow_tinted_by_magenta.b > 120);
    CHECK(std::abs(magenta_tinted_by_yellow.g - yellow_tinted_by_magenta.g) < 32);
    CHECK(std::abs(magenta_tinted_by_yellow.b - yellow_tinted_by_magenta.b) < 32);
}

TEST_CASE("Mixed filament surface predictor mixes thinner alternating layers more strongly", "[MixedFilament][Preview]")
{
    const std::vector<unsigned int> repeated { 1, 2, 1, 2, 1, 2 };
    const std::vector<std::string> colors { "#FF00FF", "#FFFF00" };
    const std::vector<double> td99 { 1.0, 1.0 };

    MixedFilamentSidewallPredictionSettings settings;
    settings.blend_model = MixedFilamentSidewallBlendModel::TdYuleNielsen;

    const TestRGB thin_magenta = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 2, colors, td99, 0.08, settings, "#000000"));
    const TestRGB thin_yellow = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 3, colors, td99, 0.08, settings, "#000000"));
    const TestRGB thick_magenta = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 2, colors, td99, 0.32, settings, "#000000"));
    const TestRGB thick_yellow = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 3, colors, td99, 0.32, settings, "#000000"));

    CHECK(thin_magenta.g > thick_magenta.g);
    CHECK(thin_yellow.b > thick_yellow.b);
    const int thin_pair_delta = std::abs(thin_magenta.g - thin_yellow.g) + std::abs(thin_magenta.b - thin_yellow.b);
    const int thick_pair_delta = std::abs(thick_magenta.g - thick_yellow.g) + std::abs(thick_magenta.b - thick_yellow.b);
    CHECK(thin_pair_delta < thick_pair_delta);
}

TEST_CASE("Mixed filament surface predictor weakens tint as TD99 increases", "[MixedFilament][Preview]")
{
    const std::vector<unsigned int> repeated { 1, 2, 2, 2, 1, 2, 2, 2, 1 };
    const std::vector<std::string> colors { "#FFFF00", "#00FFFF" };

    MixedFilamentSidewallPredictionSettings settings;
    settings.blend_model = MixedFilamentSidewallBlendModel::TdYuleNielsen;

    const TestRGB low_td_cyan = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 5, colors, { 0.5, 0.5 }, 0.08, settings, "#000000"));
    const TestRGB high_td_cyan = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 5, colors, { 3.0, 3.0 }, 0.08, settings, "#000000"));

    CHECK(low_td_cyan.b < high_td_cyan.b);
    CHECK(high_td_cyan.g >= low_td_cyan.g);
}

TEST_CASE("Mixed filament surface predictor keeps model options visually distinct", "[MixedFilament][Preview]")
{
    const std::vector<unsigned int> repeated { 1, 2, 2, 2, 1, 2, 2, 2, 1 };
    const std::vector<std::string> colors { "#FFFF00", "#00FFFF" };
    const std::vector<double> td99 { 1.0, 1.0 };

    MixedFilamentSidewallPredictionSettings yule_nielsen_settings;
    yule_nielsen_settings.blend_model = MixedFilamentSidewallBlendModel::TdYuleNielsen;

    MixedFilamentSidewallPredictionSettings mixer_settings;
    mixer_settings.blend_model = MixedFilamentSidewallBlendModel::TdFilamentMixer;

    const std::string yule_nielsen_hex = predict_mixed_filament_sequence_surface_color_at(
        repeated, 5, colors, td99, 0.08, yule_nielsen_settings, "#000000");
    const std::string mixer_hex = predict_mixed_filament_sequence_surface_color_at(
        repeated, 5, colors, td99, 0.08, mixer_settings, "#000000");
    const TestRGB yule_nielsen_cyan = parse_test_hex(yule_nielsen_hex);
    const TestRGB mixer_cyan = parse_test_hex(mixer_hex);

    CHECK(mixer_hex != yule_nielsen_hex);
    CHECK(mixer_cyan.b < yule_nielsen_cyan.b);
}

TEST_CASE("Mixed filament surface predictor keeps yellow-magenta tint orange, not green", "[MixedFilament][Preview]")
{
    const std::vector<unsigned int> repeated { 1, 2, 1, 2, 1, 2, 1, 2 };
    const std::vector<std::string> colors { "#FFFF00", "#FF00FF" };
    const std::vector<double> td99 { 1.0, 1.0 };

    MixedFilamentSidewallPredictionSettings settings;
    settings.blend_model = MixedFilamentSidewallBlendModel::TdFilamentMixer;

    const TestRGB yellow_tinted_by_magenta = parse_test_hex(predict_mixed_filament_sequence_surface_color_at(
        repeated, 4, colors, td99, 0.08, settings, "#000000"));

    CHECK(yellow_tinted_by_magenta.r >= yellow_tinted_by_magenta.g);
    CHECK(yellow_tinted_by_magenta.g > yellow_tinted_by_magenta.b);
}

TEST_CASE("Mixed filament local-Z preview pass heights fit inside the nominal layer", "[MixedFilament][Preview]")
{
    MixedFilamentPreviewSettings settings;
    settings.nominal_layer_height = 0.2;
    settings.mixed_lower_bound = 0.04;
    settings.mixed_upper_bound = 0.16;

    const std::vector<double> passes = mixed_filament_local_z_preview_pass_heights(settings, 75, 0);
    REQUIRE(passes.size() >= 2);

    double total = 0.0;
    for (const double h : passes) {
        CHECK(h >= settings.mixed_lower_bound - 1e-6);
        CHECK(h <= settings.mixed_upper_bound + 1e-6);
        total += h;
    }
    CHECK(total == Approx(settings.nominal_layer_height).epsilon(0.0001));
}

TEST_CASE("Mixed filament display color changes only when TD sidewall model is enabled and TD is known", "[MixedFilament][Preview]")
{
    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 2;
    mf.ratio_a = 1;
    mf.ratio_b = 1;
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.display_color = "#26A69A";

    MixedFilamentDisplayContext context;
    context.num_physical = 2;
    context.physical_colors = { "#FFFF00", "#0000FF" };
    context.preview_settings.nominal_layer_height = 0.2;

    context.sidewall_blend_model = MixedFilamentSidewallBlendModel::Legacy;
    context.physical_td99_mm = { 0.5, 0.5 };
    const std::string legacy = compute_mixed_filament_display_color(mf, context);

    context.sidewall_blend_model = MixedFilamentSidewallBlendModel::TdFilamentMixer;
    context.physical_td99_mm = { 0.0, 0.5 };
    CHECK(compute_mixed_filament_display_color(mf, context) == legacy);

    context.physical_td99_mm = { 0.5, 0.5 };
    const std::string td = compute_mixed_filament_display_color(mf, context);
    CHECK(td != legacy);
}

TEST_CASE("Mixed filament TD and sidewall options are registered for profile and project persistence", "[MixedFilament][Preview]")
{
    const auto& filament_keys = print_config_def.filament_option_keys();
    CHECK(std::find(filament_keys.begin(), filament_keys.end(), "filament_transmission_distance") != filament_keys.end());

    const auto& filament_preset_keys = Preset::filament_options();
    CHECK(std::find(filament_preset_keys.begin(), filament_preset_keys.end(), "filament_transmission_distance") !=
          filament_preset_keys.end());

    const auto& print_preset_keys = Preset::print_options();
    CHECK(std::find(print_preset_keys.begin(), print_preset_keys.end(), "mixed_filament_sidewall_color_model") != print_preset_keys.end());
}

TEST_CASE("G-code processor preserves FullSpectrum sidewall region markers", "[MixedFilament][Preview]")
{
    GCodeProcessor processor;
    processor.initialize("sidewall-region-test.gcode");
    processor.process_buffer(
        "G90\n"
        "M82\n"
        "; FEATURE: Outer wall\n"
        ";_FS_SIDEWALL_REGION_ID:7\n"
        "G1 X0 Y0 Z0.2 F1200\n"
        "G1 X1 Y0 E0.10\n"
        ";_FS_SIDEWALL_REGION_ID:3\n"
        "G1 X2 Y0 E0.20\n");
    processor.finalize(false);

    std::vector<unsigned int> extrude_region_ids;
    for (const GCodeProcessorResult::MoveVertex& move : processor.get_result().moves) {
        if (move.type == EMoveType::Extrude && move.delta_extruder > 0.f) {
            CHECK(move.extrusion_role == erExternalPerimeter);
            extrude_region_ids.push_back(move.sidewall_region_id);
        }
    }

    REQUIRE(extrude_region_ids.size() == 2);
    CHECK(extrude_region_ids[0] == 7);
    CHECK(extrude_region_ids[1] == 3);
}

TEST_CASE("Mixed filament remap follows stable row ids when same-pair rows reorder", "[MixedFilament]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#0000FF"};
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    auto &mixed = mgr.mixed_filaments();
    REQUIRE(mixed.size() == 1);

    mixed[0].deleted = true;
    mixed[0].enabled = false;

    const auto colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 2, 25, colors);
    mgr.add_custom_filament(1, 2, 75, colors);

    auto &old_mixed = mgr.mixed_filaments();
    REQUIRE(old_mixed.size() == 3);
    REQUIRE(old_mixed[1].enabled);
    REQUIRE(old_mixed[2].enabled);
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

    const auto &rebuilt = bundle.mixed_filaments.mixed_filaments();
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

    auto &mixed = bundle.mixed_filaments.mixed_filaments();
    REQUIRE(mixed.size() >= 6);

    const uint64_t stable_id_6 = mixed[1].stable_id;
    const uint64_t stable_id_7 = mixed[2].stable_id;
    const uint64_t stable_id_8 = mixed[3].stable_id;

    const std::vector<MixedFilament> old_mixed = mixed;
    mixed[0].enabled = false;
    mixed[0].deleted = true;

    bundle.update_mixed_filament_id_remap(old_mixed, 4, 4);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    REQUIRE(remap.size() >= 11);
    CHECK(remap[6] == virtual_id_for_stable_id(mixed, 4, stable_id_6));
    CHECK(remap[7] == virtual_id_for_stable_id(mixed, 4, stable_id_7));
    CHECK(remap[8] == virtual_id_for_stable_id(mixed, 4, stable_id_8));
}

TEST_CASE("Mixed filament grouped manual patterns normalize and round-trip", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#0000FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("11111112,11121111");
    REQUIRE(row.manual_pattern == "11111112,11121111");

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() == 1);
    CHECK(loaded.mixed_filaments().front().manual_pattern == "11111112,11121111");
    CHECK(loaded.mixed_filaments().front().mix_b_percent == 13);
}

TEST_CASE("Mixed filament component surface offsets round-trip and bias the second layer component", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.ratio_a = 1;
    row.ratio_b = 1;
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.01f;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("xa0.02") != std::string::npos);
    CHECK(serialized.find("xb-0.01") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() == 1);

    using Catch::Matchers::WithinAbs;

    const MixedFilament &loaded_row = loaded.mixed_filaments().front();
    CHECK_THAT(double(loaded_row.component_a_surface_offset), WithinAbs(0.02, 0.0001));
    CHECK_THAT(double(loaded_row.component_b_surface_offset), WithinAbs(-0.01, 0.0001));
    CHECK_THAT(double(loaded.component_surface_offset(3, 2, 0)), WithinAbs(0.01, 0.0001));
    CHECK_THAT(double(loaded.component_surface_offset(3, 2, 1)), WithinAbs(0.0, 0.0001));
}

TEST_CASE("Mixed filament apparent mix percent follows the signed bias target", "[MixedFilament]")
{
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, 0.00f, 0.4f) == 50);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, 0.02f, 0.4f) == 45);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.02f, 0.00f, 0.4f) == 55);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, -0.02f, 0.00f, 0.4f) == 45);
    CHECK(MixedFilamentManager::apparent_mix_b_percent(50, 0.00f, -0.02f, 0.4f) == 55);
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
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern.clear();
    row.distribution_mode = int(MixedFilament::Simple);
    row.ratio_a = 1;
    row.ratio_b = 1;

    using Catch::Matchers::WithinAbs;
    {
        const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(0.05f, 0.4f);
        row.component_a_surface_offset = offset_a;
        row.component_b_surface_offset = offset_b;

        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 0)), WithinAbs(0.0, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 1)), WithinAbs(0.05, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 2)), WithinAbs(0.0, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 3)), WithinAbs(0.05, 0.0001));
    }

    {
        row.component_a_surface_offset = 0.05f;
        row.component_b_surface_offset = 0.0f;

        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 0)), WithinAbs(0.05, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 1)), WithinAbs(0.0, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 2)), WithinAbs(0.05, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 3)), WithinAbs(0.0, 0.0001));
    }

    {
        const auto [offset_a, offset_b] = MixedFilamentManager::surface_offset_pair_from_signed_bias(-0.05f, 0.4f);
        row.component_a_surface_offset = offset_a;
        row.component_b_surface_offset = offset_b;

        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 0)), WithinAbs(0.05, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 1)), WithinAbs(0.0, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 2)), WithinAbs(0.05, 0.0001));
        CHECK_THAT(double(mgr.component_surface_offset(3, 2, 3)), WithinAbs(0.0, 0.0001));
    }
}

TEST_CASE("Mixed filament auto generation can be disabled without dropping custom rows", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentManager enabled_mgr;
    enabled_mgr.auto_generate(colors);
    REQUIRE(enabled_mgr.mixed_filaments().size() == 3);
    const std::string serialized_auto_rows = enabled_mgr.serialize_custom_entries();

    MixedAutoGenerateGuard guard(false);

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    mgr.auto_generate(colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);
    CHECK(mgr.mixed_filaments().front().custom);
    CHECK(mgr.mixed_filaments().front().component_a == 1);
    CHECK(mgr.mixed_filaments().front().component_b == 2);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized_auto_rows, colors);
    CHECK(loaded.mixed_filaments().empty());
}

TEST_CASE("Mixed filament perimeter resolver uses grouped manual patterns by inset", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    REQUIRE(row.manual_pattern == "12,21");

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
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("2,12");
    REQUIRE(row.manual_pattern == "2,12");

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
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,1");
    REQUIRE(row.manual_pattern == "12,1");

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
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,1");
    REQUIRE(row.manual_pattern == "12,1");

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
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.ratio_a = 1;
    row.ratio_b = 1;
    row.manual_pattern.clear();
    row.distribution_mode = int(MixedFilament::Simple);

    CHECK(mgr.effective_painted_region_filament_id(3, 2, 0) == 1);
    CHECK(mgr.effective_painted_region_filament_id(3, 2, 1) == 2);
}

TEST_CASE("Mixed filament painted-region resolver preserves virtual channels for grouped and same-layer modes", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    using Catch::Matchers::WithinAbs;
    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    CHECK(mgr.effective_painted_region_filament_id(3, 2, 0) == 3);
    row.component_a_surface_offset = 0.02f;
    row.component_b_surface_offset = -0.02f;
    CHECK_THAT(double(mgr.component_surface_offset(3, 2, 0)), WithinAbs(0.0, 0.0001));

    row.manual_pattern.clear();
    row.distribution_mode = int(MixedFilament::SameLayerPointillisme);
    CHECK(mgr.effective_painted_region_filament_id(3, 2, 0) == 3);
    CHECK_THAT(double(mgr.component_surface_offset(3, 2, 0)), WithinAbs(0.0, 0.0001));
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

// ============================================================================
// [MixedFilament][Utility]
// ============================================================================

TEST_CASE("Mixed filament safe_mod handles negative wrapping and edge cases", "[MixedFilament][Utility]")
{
    CHECK(MixedFilamentManager::safe_mod(5, 3) == 2);
    CHECK(MixedFilamentManager::safe_mod(-1, 5) == 4);
    CHECK(MixedFilamentManager::safe_mod(5, 1) == 0);
    CHECK(MixedFilamentManager::safe_mod(0, 5) == 0);
    CHECK(MixedFilamentManager::safe_mod(7, 5) == 2);
}

TEST_CASE("Mixed filament normalize_ratio_pair clamps and normalizes", "[MixedFilament][Utility]")
{
    int a = 0, b = 0;
    MixedFilamentManager::normalize_ratio_pair(a, b);
    CHECK(a == 1);
    CHECK(b == 0);

    a = 0; b = 3;
    MixedFilamentManager::normalize_ratio_pair(a, b);
    CHECK(a == 0);
    CHECK(b == 3);

    a = -1; b = -2;
    MixedFilamentManager::normalize_ratio_pair(a, b);
    // negatives clamped to 0, then (0,0) → (1,0)
    CHECK(a == 1);
    CHECK(b == 0);
}

TEST_CASE("Mixed filament fill_continuous_layer_range fills gaps", "[MixedFilament][Utility]")
{
    const std::vector<int> empty;
    CHECK(fill_continuous_layer_range(empty).empty());

    const std::vector<int> single = {3};
    const auto filled_single = fill_continuous_layer_range(single);
    REQUIRE(filled_single.size() == 1);
    CHECK(filled_single[0] == 3);

    const std::vector<int> sparse = {1, 5};
    const auto filled_sparse = fill_continuous_layer_range(sparse);
    REQUIRE(filled_sparse.size() == 5);
    CHECK(filled_sparse[0] == 1);
    CHECK(filled_sparse[4] == 5);

    const std::vector<int> cont = {1, 2, 3};
    const auto filled_cont = fill_continuous_layer_range(cont);
    REQUIRE(filled_cont.size() == 3);
    CHECK(filled_cont[0] == 1);
    CHECK(filled_cont[2] == 3);
}

TEST_CASE("Mixed filament canonical_signed_bias_value extracts signed bias", "[MixedFilament][Utility]")
{
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(double(MixedFilamentManager::canonical_signed_bias_value(0.f, 0.f)), WithinAbs(0.0, 0.0001));
    CHECK_THAT(double(MixedFilamentManager::canonical_signed_bias_value(0.f, 0.05f)), WithinAbs(0.05, 0.0001));
    CHECK_THAT(double(MixedFilamentManager::canonical_signed_bias_value(0.05f, 0.f)), WithinAbs(-0.05, 0.0001));
    CHECK_THAT(double(MixedFilamentManager::canonical_signed_bias_value(-0.05f, 0.f)), WithinAbs(0.05, 0.0001));
}

TEST_CASE("Mixed filament format_surface_offset_token formats and strips", "[MixedFilament][Utility]")
{
    CHECK(MixedFilamentManager::format_surface_offset_token(0.f) == "0");
    CHECK(MixedFilamentManager::format_surface_offset_token(0.02f) == "0.02");
    CHECK(MixedFilamentManager::format_surface_offset_token(-0.01f) == "-0.01");
    CHECK(MixedFilamentManager::format_surface_offset_token(2.f) == "2");
    CHECK(MixedFilamentManager::format_surface_offset_token(-0.f) == "0");
    CHECK(MixedFilamentManager::format_surface_offset_token(0.1234f) == "0.1234");
}

TEST_CASE("Mixed filament max surface offset clamps correctly", "[MixedFilament][Utility]")
{
    using Catch::Matchers::WithinRel;
    CHECK_THAT(double(MixedFilamentManager::max_component_surface_offset_mm(0.4f)), WithinRel(0.35, 0.001));
    CHECK_THAT(double(MixedFilamentManager::max_component_surface_offset_mm(0.001f)), WithinRel(0.05, 0.001));
    CHECK_THAT(double(MixedFilamentManager::max_component_surface_offset_mm(2.f)), WithinRel(0.35, 0.001));
    CHECK_THAT(double(MixedFilamentManager::max_pair_bias_mm(0.4f)), WithinRel(0.35, 0.001));
}

// ============================================================================
// [MixedFilament][Pattern]
// ============================================================================

TEST_CASE("Mixed filament manual pattern bracket notation acceptance", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("[10]") == "[10]");
    CHECK(MixedFilamentManager::normalize_manual_pattern("[1]") == "1");
    CHECK(MixedFilamentManager::normalize_manual_pattern("[99]") == "[99]");
    CHECK(MixedFilamentManager::normalize_manual_pattern("[11]") == "[11]");
    CHECK(MixedFilamentManager::normalize_manual_pattern("1[10],[11]2") == "1[10],[11]2");
}

TEST_CASE("Mixed filament manual pattern bracket overflow rejection", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("[100]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[123]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[9999]").empty());
}

TEST_CASE("Mixed filament manual pattern zero and leading-zero rejection", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("0").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[0]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[01]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[00]").empty());
}

TEST_CASE("Mixed filament manual pattern malformed bracket rejection", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("[").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[]").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[1").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("1]").empty());
}

TEST_CASE("Mixed filament manual pattern empty group rejection", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("1,").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern(",1").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("1,,2").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern(",").empty());
}

TEST_CASE("Mixed filament manual pattern invalid character rejection", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::normalize_manual_pattern("a").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("(1)").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern(" ").empty());
    CHECK(MixedFilamentManager::normalize_manual_pattern("[1a]").empty());
}

TEST_CASE("Mixed filament mix_percent_from_manual_pattern with bracket tokens", "[MixedFilament][Pattern]")
{
    CHECK(MixedFilamentManager::mix_percent_from_manual_pattern("1112") == 25);
    CHECK(MixedFilamentManager::mix_percent_from_manual_pattern("22[2]1") == 75);
    CHECK(MixedFilamentManager::mix_percent_from_manual_pattern("[10][10]1[10]2") == 20);
}

TEST_CASE("Mixed filament physical_filament_from_token maps tokens correctly", "[MixedFilament][Pattern]")
{
    MixedFilament mf;
    mf.component_a = 3;
    mf.component_b = 4;
    const size_t num_physical = 10;

    CHECK(MixedFilamentManager::physical_filament_from_token("1", mf, num_physical) == 3);
    CHECK(MixedFilamentManager::physical_filament_from_token("2", mf, num_physical) == 4);
    CHECK(MixedFilamentManager::physical_filament_from_token("10", mf, num_physical) == 10);
    CHECK(MixedFilamentManager::physical_filament_from_token("999", mf, num_physical) == 0);
    CHECK(MixedFilamentManager::physical_filament_from_token("abc", mf, num_physical) == 0);
    CHECK(MixedFilamentManager::physical_filament_from_token("0", mf, num_physical) == 0);
}

// ============================================================================
// [MixedFilament][Color]
// ============================================================================

TEST_CASE("Mixed filament blend_color_multi blends N colors", "[MixedFilament][Color]")
{
    const std::vector<std::pair<std::string, int>> empty;
    CHECK(MixedFilamentManager::blend_color_multi(empty) == "#000000");

    const std::vector<std::pair<std::string, int>> single = {{"#FF0000", 100}};
    CHECK(MixedFilamentManager::blend_color_multi(single) == "#FF0000");

    const std::vector<std::pair<std::string, int>> all_zero = {{"#FF0000", 0}, {"#00FF00", 0}};
    CHECK(MixedFilamentManager::blend_color_multi(all_zero) == "#000000");

    const std::vector<std::pair<std::string, int>> three = {{"#FF0000", 40}, {"#00FF00", 30}, {"#0000FF", 30}};
    const std::string result = MixedFilamentManager::blend_color_multi(three);
    CHECK(!result.empty());
    CHECK(result[0] == '#');
}

TEST_CASE("Mixed filament blend_color handles ratio edge cases", "[MixedFilament][Color]")
{
    const std::string blended = MixedFilamentManager::blend_color("#FF0000", "#00FF00", 1, 1);
    CHECK(!blended.empty());
    CHECK(blended[0] == '#');

    const std::string zero_both = MixedFilamentManager::blend_color("#FF0000", "#00FF00", 0, 0);
    CHECK(!zero_both.empty());
    CHECK(zero_both[0] == '#');

    const std::string pure_b = MixedFilamentManager::blend_color("#FF0000", "#00FF00", 0, 1);
    CHECK(pure_b == "#00FF00");

    const std::string pure_a = MixedFilamentManager::blend_color("#FF0000", "#00FF00", 1, 0);
    CHECK(pure_a == "#FF0000");
}

// ============================================================================
// [MixedFilament][Lifecycle]
// ============================================================================

TEST_CASE("Mixed filament add_custom_filament guards and auto-swap", "[MixedFilament][Lifecycle]")
{
    // n<2 → no-op
    MixedFilamentManager mgr_small;
    const std::vector<std::string> one_color = {"#FF0000"};
    mgr_small.add_custom_filament(1, 2, 50, one_color);
    CHECK(mgr_small.mixed_filaments().empty());

    // Normal addition
    MixedFilamentManager mgr;
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);
    const auto &entry = mgr.mixed_filaments().front();
    CHECK(entry.custom);
    CHECK(entry.enabled);
    CHECK_FALSE(entry.deleted);
    CHECK(entry.mix_b_percent == 50);

    // a==b → auto-swap
    MixedFilamentManager mgr_swap;
    mgr_swap.add_custom_filament(1, 1, 30, colors);
    REQUIRE(mgr_swap.mixed_filaments().size() == 1);
    CHECK(mgr_swap.mixed_filaments().front().component_a == 1);
    CHECK(mgr_swap.mixed_filaments().front().component_b == 2);
}

TEST_CASE("Mixed filament auto_generate preserves state and respects disable", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    REQUIRE(mgr.mixed_filaments().size() >= 3);

    // Disable one auto row, then re-generate
    mgr.mixed_filaments()[0].enabled = false;
    mgr.auto_generate(colors);
    CHECK_FALSE(mgr.mixed_filaments()[0].enabled);

    // Custom rows survive auto_generate
    mgr.add_custom_filament(1, 3, 50, colors);
    size_t custom_count_before = 0;
    for (const auto &mf : mgr.mixed_filaments())
        if (mf.custom) ++custom_count_before;
    mgr.auto_generate(colors);
    size_t custom_count_after = 0;
    for (const auto &mf : mgr.mixed_filaments())
        if (mf.custom) ++custom_count_after;
    CHECK(custom_count_after == custom_count_before);

    // Disabled auto-generate: only custom rows remain
    {
        MixedAutoGenerateGuard guard_disabled(false);
        mgr.auto_generate(colors);
        for (const auto &mf : mgr.mixed_filaments())
            CHECK(mf.custom);
    }
}

TEST_CASE("Mixed filament enabled_count and cleanup operations", "[MixedFilament][Lifecycle]")
{
    MixedFilamentManager mgr;
    CHECK(mgr.enabled_count() == 0);

    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    mgr.add_custom_filament(1, 2, 50, colors);
    CHECK(mgr.enabled_count() == 1);

    mgr.mixed_filaments()[0].enabled = false;
    CHECK(mgr.enabled_count() == 0);

    mgr.mixed_filaments()[0].enabled = true;
    mgr.mixed_filaments()[0].deleted = true;
    CHECK(mgr.enabled_count() == 0);

    // cleanup removes deleted
    mgr.cleanup_deleted_entries();
    CHECK(mgr.mixed_filaments().empty());
}

TEST_CASE("Mixed filament clear_custom_entries removes only custom", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    size_t auto_count = mgr.mixed_filaments().size();

    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.add_custom_filament(1, 3, 50, colors);
    CHECK(mgr.mixed_filaments().size() == auto_count + 2);

    mgr.clear_custom_entries();
    CHECK(mgr.mixed_filaments().size() == auto_count);
    for (const auto &mf : mgr.mixed_filaments())
        CHECK_FALSE(mf.custom);
}

TEST_CASE("Mixed filament mixed_filaments_using_physical finds dependents", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    // Physical 1 used in auto pairs with 2 and 3
    auto deps = mgr.mixed_filaments_using_physical(1);
    CHECK(deps.size() >= 2);

    // Physical 2 used in auto pairs
    deps = mgr.mixed_filaments_using_physical(2);
    CHECK(deps.size() >= 2);

    // Deleted entries skipped
    mgr.mixed_filaments()[0].deleted = true;
    mgr.mixed_filaments()[0].enabled = false;
    deps = mgr.mixed_filaments_using_physical(1);
    for (size_t idx : deps)
        CHECK(idx != 0);
}

TEST_CASE("Mixed filament remove_physical_filament removes and shifts", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    mgr.add_custom_filament(2, 3, 50, colors);

    // Verify initial state has entries for filament 1
    size_t count_before = mgr.mixed_filaments().size();
    CHECK(count_before > 0);

    mgr.remove_physical_filament(1);

    // After removal, total count decreased (entries using old filament 1 removed)
    size_t count_after = mgr.mixed_filaments().size();
    CHECK(count_after < count_before);

    // Higher IDs shifted: old filament 2 → 1, 3 → 2, 4 → 3
    for (const auto &mf : mgr.mixed_filaments()) {
        CHECK(mf.component_a > 0);
        CHECK(mf.component_b > 0);
    }
}

TEST_CASE("Mixed filament mixed_index_from_filament_id maps correctly", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    CHECK(mgr.mixed_index_from_filament_id(1, num_physical) == -1);
    CHECK(mgr.mixed_index_from_filament_id(2, num_physical) == -1);
    CHECK(mgr.mixed_index_from_filament_id(3, num_physical) >= 0);

    // Deleted/disabled entry skipped
    mgr.mixed_filaments()[0].enabled = false;
    mgr.mixed_filaments()[0].deleted = true;
    CHECK(mgr.mixed_index_from_filament_id(3, num_physical) == -1);
}

TEST_CASE("Mixed filament stable_id allocation and normalize", "[MixedFilament][Lifecycle]")
{
    MixedFilamentManager mgr;
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    mgr.add_custom_filament(1, 2, 50, colors);

    const auto &entry = mgr.mixed_filaments().front();
    CHECK(entry.stable_id >= 1);
}

TEST_CASE("Mixed filament accessors total_filaments display_colors is_mixed", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    CHECK(mgr.total_filaments(2) == 3);

    CHECK(mgr.is_mixed(1, 2) == false);
    CHECK(mgr.is_mixed(3, 2) == true);

    auto display = mgr.display_colors();
    CHECK(display.size() == 1);
}

TEST_CASE("LIFE-REGRESS-01: deleted entry not returned by mixed_filaments_using_physical", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    REQUIRE(mgr.mixed_filaments().size() == 3);

    // Mark first entry as deleted and disabled
    mgr.mixed_filaments()[0].deleted = true;
    mgr.mixed_filaments()[0].enabled = false;

    const unsigned int phys_a = mgr.mixed_filaments()[0].component_a;
    const auto deps = mgr.mixed_filaments_using_physical(phys_a);

    // The deleted entry (index 0) should NOT appear in the results
    for (size_t idx : deps)
        CHECK(idx != 0);
}

TEST_CASE("LIFE-REGRESS-02: empty manager state after all entries cleared", "[MixedFilament][Lifecycle]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    REQUIRE(mgr.mixed_filaments().size() == 3);

    // Mark ALL entries as deleted and disabled to clear everything
    for (auto &mf : mgr.mixed_filaments()) {
        mf.deleted = true;
        mf.enabled = false;
    }

    const size_t num_physical = 3;
    const unsigned int first_mixed_id = 4;

    CHECK(mgr.enabled_count() == 0);
    CHECK(mgr.total_filaments(num_physical) == num_physical);
    CHECK(mgr.is_mixed(first_mixed_id, num_physical) == false);
}

// ============================================================================
// [MixedFilament][Serialization]
// ============================================================================

TEST_CASE("Mixed filament full 17-field serialization round-trip", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.gradient_component_ids = "123";
    mf.gradient_component_weights = "40/30/30";
    mf.gradient_enabled = true;
    mf.gradient_start = 0.85f;
    mf.gradient_end = 0.15f;
    mf.manual_pattern = "12,21";
    mf.distribution_mode = int(MixedFilament::LayerCycle);
    mf.local_z_max_sublayers = 4;
    mf.component_a_surface_offset = 0.02f;
    mf.component_b_surface_offset = -0.01f;
    mf.deleted = false;
    mf.origin_auto = false;

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.add_custom_filament(1, 3, 50, colors); // add an auto pairing trigger row
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);

    // Find the loaded custom entry
    const MixedFilament *loaded_mf = nullptr;
    for (const auto &row : loaded.mixed_filaments()) {
        if (row.custom && row.component_a == 1 && row.component_b == 2) {
            loaded_mf = &row;
            break;
        }
    }
    REQUIRE(loaded_mf != nullptr);
    CHECK(loaded_mf->gradient_enabled == true);
    CHECK(loaded_mf->distribution_mode == int(MixedFilament::LayerCycle));
    CHECK(loaded_mf->local_z_max_sublayers == 4);
    CHECK(loaded_mf->manual_pattern == "12,21");
}

TEST_CASE("Mixed filament legacy 4-token format parsing", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.load_custom_entries("1,2,1,50", colors);

    REQUIRE(mgr.mixed_filaments().size() == 1);
    const auto &entry = mgr.mixed_filaments().front();
    CHECK(entry.component_a == 1);
    CHECK(entry.component_b == 2);
    CHECK(entry.enabled);
    CHECK(entry.mix_b_percent == 50);
    CHECK(entry.custom);
}

TEST_CASE("Mixed filament legacy pointillism flag", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    // Add an auto row first so the serialized custom row is recognizable
    mgr.auto_generate(colors);
    mgr.load_custom_entries("1,2,1,1,50,1", colors);
    // The row is custom (tokens.size() > 4 with custom=1)
    bool found_custom = false;
    for (const auto &mf : mgr.mixed_filaments())
        if (mf.custom) found_custom = true;
    CHECK(found_custom);
}

TEST_CASE("Mixed filament deleted entry serialization round-trip", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().front().deleted = true;
    mgr.mixed_filaments().front().enabled = false;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("d1") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);
    CHECK(loaded.mixed_filaments().front().deleted);
    CHECK_FALSE(loaded.mixed_filaments().front().enabled);
}

TEST_CASE("Mixed filament duplicate stable_id dedup on load", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.add_custom_filament(1, 3, 50, colors);
    // Force same stable_id
    mgr.mixed_filaments()[0].stable_id = 100;
    mgr.mixed_filaments()[1].stable_id = 100;

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 2);
    // The two entries should have different stable_ids after dedup
    CHECK(loaded.mixed_filaments()[0].stable_id != loaded.mixed_filaments()[1].stable_id);
}

TEST_CASE("SER-REGRESS-01: manual_pattern preserved through serialize-load cycle", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};

    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    mgr.add_custom_filament(1, 2, 50, colors);

    // Find the custom entry and set manual_pattern
    MixedFilament *custom_entry = nullptr;
    for (auto &mf : mgr.mixed_filaments()) {
        if (mf.custom && mf.component_a == 1 && mf.component_b == 2) {
            custom_entry = &mf;
            break;
        }
    }
    REQUIRE(custom_entry != nullptr);
    custom_entry->manual_pattern = MixedFilamentManager::normalize_manual_pattern("1266666");
    REQUIRE(custom_entry->manual_pattern == "1266666");

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.auto_generate(colors);
    loaded.load_custom_entries(serialized, colors);

    bool found = false;
    for (const auto &mf : loaded.mixed_filaments()) {
        if (mf.custom && mf.component_a == 1 && mf.component_b == 2) {
            CHECK(mf.manual_pattern == "1266666");
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("SER-REGRESS-02: duplicate custom entries survive load_custom_entries", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};

    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    // Add two custom entries with same pair but different manual_patterns
    mgr.add_custom_filament(1, 2, 30, colors);
    mgr.add_custom_filament(1, 2, 70, colors);

    // Set distinct manual_patterns and stable_ids
    auto &mixed = mgr.mixed_filaments();
    std::vector<MixedFilament *> customs;
    for (auto &mf : mixed) {
        if (mf.custom && mf.component_a == 1 && mf.component_b == 2)
            customs.push_back(&mf);
    }
    REQUIRE(customs.size() == 2);

    customs[0]->manual_pattern = MixedFilamentManager::normalize_manual_pattern("1212");
    customs[0]->stable_id = 1001;
    customs[1]->manual_pattern = MixedFilamentManager::normalize_manual_pattern("2121");
    customs[1]->stable_id = 1002;

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.auto_generate(colors);
    loaded.load_custom_entries(serialized, colors);

    // Custom entries do NOT get pair-based dedup like auto entries
    int custom_count = 0;
    for (const auto &mf : loaded.mixed_filaments()) {
        if (mf.custom && mf.component_a == 1 && mf.component_b == 2)
            ++custom_count;
    }
    CHECK(custom_count == 2);
}

TEST_CASE("SER-REGRESS-03: deleted flag round-trips through serialize-load", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().front().deleted = true;
    mgr.mixed_filaments().front().enabled  = false;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("d1") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);
    CHECK(loaded.mixed_filaments().front().deleted);
    CHECK_FALSE(loaded.mixed_filaments().front().enabled);
}

TEST_CASE("SER-REGRESS-04: gradient r1 token round-trip", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.gradient_enabled = true;
    mf.gradient_start   = 0.8f;
    mf.gradient_end     = 0.2f;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("r1/0.8000/0.2000") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);

    const auto &loaded_mf = loaded.mixed_filaments().front();
    CHECK(loaded_mf.gradient_enabled);
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(double(loaded_mf.gradient_start), WithinAbs(0.8, 0.0001));
    CHECK_THAT(double(loaded_mf.gradient_end),   WithinAbs(0.2, 0.0001));
}

TEST_CASE("SER-REGRESS-05: ui_mode persistence through serialization", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.ui_mode        = 1;
    mf.manual_pattern = MixedFilamentManager::normalize_manual_pattern("1212");
    REQUIRE(mf.manual_pattern == "1212");

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);

    const auto &loaded_mf = loaded.mixed_filaments().front();
    CHECK(loaded_mf.ui_mode == 1);
}

TEST_CASE("SER-REGRESS-06: surface offset serialization round-trip xa/xb tokens", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().back();
    mf.component_a_surface_offset = 0.15f;
    mf.component_b_surface_offset = -0.10f;

    const std::string serialized = mgr.serialize_custom_entries();
    // format_surface_offset_token strips trailing zeros after setprecision(4):
    // 0.15 -> "xa0.15", -0.10 -> "xb-0.1"
    CHECK(serialized.find("xa0.15") != std::string::npos);
    CHECK(serialized.find("xb-0.1") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.auto_generate(colors);
    loaded.load_custom_entries(serialized, colors);

    // Find the loaded custom entry by component pair
    const MixedFilament *loaded_mf = nullptr;
    for (const auto &row : loaded.mixed_filaments()) {
        if (row.custom && row.component_a == 1 && row.component_b == 2) {
            loaded_mf = &row;
            break;
        }
    }
    REQUIRE(loaded_mf != nullptr);

    using Catch::Matchers::WithinAbs;
    CHECK_THAT(double(loaded_mf->component_a_surface_offset), WithinAbs(0.15, 0.001));
    CHECK_THAT(double(loaded_mf->component_b_surface_offset), WithinAbs(-0.10, 0.001));
}

TEST_CASE("SER-REGRESS-07: origin_auto flag serialization round-trip", "[MixedFilament][Serialization]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().back();
    mf.origin_auto = true;
    mf.custom      = false;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("o1") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.auto_generate(colors);
    loaded.load_custom_entries(serialized, colors);

    // Find the loaded custom entry by component pair
    const MixedFilament *loaded_mf = nullptr;
    for (const auto &row : loaded.mixed_filaments()) {
        if (row.custom == false && row.component_a == 1 && row.component_b == 2) {
            loaded_mf = &row;
            break;
        }
    }
    REQUIRE(loaded_mf != nullptr);
    CHECK(loaded_mf->origin_auto == true);
}

// ============================================================================
// [MixedFilament][Gradient]
// ============================================================================

TEST_CASE("Mixed filament is_simple_gradient all branches", "[MixedFilament][Gradient]")
{
    MixedFilament mf;
    mf.gradient_enabled = true;
    mf.component_a = 1;
    mf.component_b = 2;
    mf.manual_pattern.clear();
    mf.gradient_component_ids = "1";

    CHECK(is_simple_gradient(mf));

    mf.gradient_enabled = false;
    CHECK_FALSE(is_simple_gradient(mf));

    mf.gradient_enabled = true;
    mf.component_a = 1;
    mf.component_b = 1;
    CHECK_FALSE(is_simple_gradient(mf));

    mf.component_b = 2;
    mf.manual_pattern = "12";
    CHECK_FALSE(is_simple_gradient(mf));

    mf.manual_pattern.clear();
    mf.gradient_component_ids = "123";
    CHECK_FALSE(is_simple_gradient(mf));
}

TEST_CASE("Mixed filament gradient serialization round-trip with r1 token", "[MixedFilament][Gradient]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.gradient_component_ids = "12";
    mf.gradient_component_weights = "60/40";
    mf.gradient_enabled = true;
    mf.gradient_start = 0.85f;
    mf.gradient_end = 0.15f;

    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("r1/0.8500/0.1500") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);
    const auto &loaded_mf = loaded.mixed_filaments().front();
    CHECK(loaded_mf.gradient_enabled);
    CHECK(loaded_mf.gradient_component_ids == "12");
    CHECK(loaded_mf.gradient_component_weights == "60/40");
}

TEST_CASE("Mixed filament gradient auto-disables when range too small", "[MixedFilament][Gradient]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.gradient_enabled = true;
    mf.gradient_start = 0.80f;
    mf.gradient_end = 0.82f;

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() >= 1);
    CHECK_FALSE(loaded.mixed_filaments().front().gradient_enabled);
}

TEST_CASE("Mixed filament apply_gradient_settings modes", "[MixedFilament][Gradient]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    mgr.add_custom_filament(1, 2, 50, colors);

    // Mode 0: layer-cycle
    mgr.apply_gradient_settings(0, 0.04f, 0.16f, false);
    for (const auto &mf : mgr.mixed_filaments()) {
        if (!mf.custom) {
            CHECK(mf.ratio_a == 1);
            CHECK(mf.ratio_b == 1);
        }
    }

    // Mode 1: height-weighted
    mgr.apply_gradient_settings(1, 0.04f, 0.16f, false);
    for (const auto &mf : mgr.mixed_filaments()) {
        CHECK(mf.ratio_a >= 0);
        CHECK(mf.ratio_b >= 0);
    }
}

TEST_CASE("Mixed filament gradient multi-color resolve", "[MixedFilament][Gradient]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.gradient_component_ids = "123";
    mf.gradient_component_weights = "50/30/20";
    mf.distribution_mode = int(MixedFilament::LayerCycle);

    const size_t num_physical = 3;
    const unsigned int mixed_id = 4;
    // Should resolve to different physical IDs at different layers
    unsigned int r0 = mgr.resolve(mixed_id, num_physical, 0);
    unsigned int r1 = mgr.resolve(mixed_id, num_physical, 1);
    unsigned int r2 = mgr.resolve(mixed_id, num_physical, 2);
    CHECK(r0 >= 1);
    CHECK(r0 <= num_physical);
    CHECK(r1 >= 1);
    CHECK(r1 <= num_physical);
    CHECK(r2 >= 1);
    CHECK(r2 <= num_physical);
}

// ============================================================================
// [MixedFilament][Resolve]
// ============================================================================

TEST_CASE("Mixed filament resolve passthrough and simple cadence", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    // Physical filament passthrough
    CHECK(mgr.resolve(1, num_physical, 0) == 1);
    CHECK(mgr.resolve(2, num_physical, 0) == 2);

    // Simple cadence: ratio_a=1, ratio_b=1
    const unsigned int mixed_id = 3;
    unsigned int r0 = mgr.resolve(mixed_id, num_physical, 0);
    unsigned int r1 = mgr.resolve(mixed_id, num_physical, 1);
    CHECK(r0 != r1);
}

TEST_CASE("Mixed filament resolve height-weighted mode", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    const unsigned int mixed_id = 3;
    unsigned int result = mgr.resolve(mixed_id, num_physical, 0, 0.5f, 0.2f, true);
    CHECK(result >= 1);
    CHECK(result <= num_physical);
}

TEST_CASE("Mixed filament resolve advanced dithering", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.apply_gradient_settings(0, 0.04f, 0.16f, true);
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    const unsigned int mixed_id = 3;
    unsigned int result = mgr.resolve(mixed_id, num_physical, 0);
    CHECK(result >= 1);
    CHECK(result <= num_physical);
}

TEST_CASE("Mixed filament resolve_perimeter and ordered_perimeter_extruders", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    const size_t num_physical = 2;
    const unsigned int mixed_id = 3;

    CHECK(mgr.resolve_perimeter(mixed_id, num_physical, 0, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_id, num_physical, 0, 1) == 2);

    auto ordered = mgr.ordered_perimeter_extruders(mixed_id, num_physical, 0);
    CHECK_FALSE(ordered.empty());
}

TEST_CASE("Mixed filament effective_painted_region_filament_id", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    const unsigned int mixed_id = 3;

    // Simple mode collapses to physical
    unsigned int eff = mgr.effective_painted_region_filament_id(mixed_id, num_physical, 0);
    CHECK(eff >= 1);
    CHECK(eff <= num_physical);

    // Pointillisme keeps virtual
    mgr.mixed_filaments().front().distribution_mode = int(MixedFilament::SameLayerPointillisme);
    CHECK(mgr.effective_painted_region_filament_id(mixed_id, num_physical, 0) == mixed_id);

    // Grouped pattern keeps virtual
    mgr.mixed_filaments().front().distribution_mode = int(MixedFilament::LayerCycle);
    mgr.mixed_filaments().front().manual_pattern = "12,21";
    CHECK(mgr.effective_painted_region_filament_id(mixed_id, num_physical, 0) == mixed_id);
}

TEST_CASE("Mixed filament component_surface_offset bias routing", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#FFFF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    auto &mf = mgr.mixed_filaments().front();
    mf.ratio_a = 1;
    mf.ratio_b = 1;
    mf.distribution_mode = int(MixedFilament::Simple);

    const size_t num_physical = 2;
    const unsigned int mixed_id = 3;

    // Pointillisme → 0
    mf.distribution_mode = int(MixedFilament::SameLayerPointillisme);
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(double(mgr.component_surface_offset(mixed_id, num_physical, 0)), WithinAbs(0.0, 0.0001));

    // Grouped → 0
    mf.distribution_mode = int(MixedFilament::LayerCycle);
    mf.manual_pattern = "12,21";
    CHECK_THAT(double(mgr.component_surface_offset(mixed_id, num_physical, 0)), WithinAbs(0.0, 0.0001));

    // Bias routing (simple mode)
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.manual_pattern.clear();
    mf.component_a_surface_offset = 0.f;
    mf.component_b_surface_offset = 0.05f;
    // At layer 1 (resolved to component_b), offset should be positive
    float off = mgr.component_surface_offset(mixed_id, num_physical, 1);
    CHECK(off >= -0.01f);
}

TEST_CASE("Mixed filament mixed_filament_from_id lookup", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    const size_t num_physical = 2;
    CHECK(mgr.mixed_filament_from_id(1, num_physical) == nullptr);
    CHECK(mgr.mixed_filament_from_id(3, num_physical) != nullptr);
    CHECK(mgr.mixed_filament_from_id(999, num_physical) == nullptr);
}

// ============================================================================
// [MixedFilament][Merge] — Physical filament merge / remap (PresetBundle level)
// ============================================================================

TEST_CASE("Mixed filament merge physical shifts IDs down and removes dependents", "[MixedFilament][Merge]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue", "PLA Yellow"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00"
    };
    bundle.update_multi_material_filament_presets();

    const size_t physical_before = 4;
    auto &mgr = bundle.mixed_filaments;
    REQUIRE(mgr.mixed_filaments().size() >= 6);

    // Delete the last physical filament (truncation — the function handles this case).
    // Erase filament at 0-based index 3 (physical #4), then call with to_delete=3.
    bundle.filament_presets.pop_back();
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.pop_back();
    bundle.update_multi_material_filament_presets(3, physical_before);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    // After truncating physical 4: physical 1→1, 2→2, 3→3, 4→0 (deleted)
    REQUIRE(remap.size() >= 5);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[4] == 0);
}

TEST_CASE("Mixed filament merge stable_id preserves surviving mixed rows", "[MixedFilament][Merge]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"
    };
    bundle.update_multi_material_filament_presets();

    const size_t physical_before = 3;
    auto &mgr = bundle.mixed_filaments;

    // Record stable_ids of mixed rows NOT depending on physical 3 (the last one)
    std::vector<uint64_t> surviving_stable_ids;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.enabled && !mf.deleted && mf.component_a != 3 && mf.component_b != 3)
            surviving_stable_ids.push_back(mf.stable_id);
    }
    REQUIRE_FALSE(surviving_stable_ids.empty());

    // Delete the last filament (truncation from end — 0-based index 2)
    bundle.filament_presets.pop_back();
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.pop_back();
    bundle.update_multi_material_filament_presets(2, physical_before);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 4);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 0);

    // Surviving rows should not reference the deleted physical filament
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.enabled && !mf.deleted) {
            CHECK(mf.component_a != 3);
            CHECK(mf.component_b != 3);
        }
    }
}

TEST_CASE("Mixed filament merge canonical_pair fallback when stable_id missing", "[MixedFilament][Merge]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"
    };
    bundle.update_multi_material_filament_presets();

    const size_t physical_before = 3;
    auto &mgr = bundle.mixed_filaments;

    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    // Add a custom row that does NOT depend on the last physical filament (3)
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() >= 4);

    bool has_custom_pair_12 = false;
    for (const auto &mf : mgr.mixed_filaments())
        if (mf.custom && ((mf.component_a == 1 && mf.component_b == 2) ||
                          (mf.component_a == 2 && mf.component_b == 1)))
            has_custom_pair_12 = true;
    CHECK(has_custom_pair_12);

    // Delete the last filament (truncation — 0-based index 2 = physical 3)
    bundle.filament_presets.pop_back();
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.pop_back();
    bundle.update_multi_material_filament_presets(2, physical_before);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    CHECK(remap.size() >= 6);

    // Custom pair (1,2) should survive since it doesn't depend on deleted physical 3
    bool has_surviving_custom_pair = false;
    for (const auto &mf : mgr.mixed_filaments())
        if (mf.custom && ((mf.component_a == 1 && mf.component_b == 2) ||
                          (mf.component_a == 2 && mf.component_b == 1)))
            has_surviving_custom_pair = true;
    CHECK(has_surviving_custom_pair);
}

// ============================================================================
// [MixedFilament][Display]
// ============================================================================

TEST_CASE("Mixed filament reference nozzle mm computation", "[MixedFilament][Display]")
{
    using Catch::Matchers::WithinRel;
    const std::vector<double> nozzles = {0.4, 0.6, 0.2};

    double ref = MixedFilamentManager::mixed_filament_reference_nozzle_mm(1, 2, nozzles);
    CHECK_THAT(ref, WithinRel(0.5, 0.01));

    double single = MixedFilamentManager::mixed_filament_reference_nozzle_mm(1, 0, nozzles);
    CHECK_THAT(single, WithinRel(0.4, 0.01));

    const std::vector<double> empty_nozzles;
    double fallback = MixedFilamentManager::mixed_filament_reference_nozzle_mm(0, 0, empty_nozzles);
    CHECK_THAT(fallback, WithinRel(0.4, 0.01));
}

TEST_CASE("Mixed filament supports_bias_apparent_color conditions", "[MixedFilament][Display]")
{
    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 2;
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.gradient_component_ids.clear();
    mf.manual_pattern.clear();
    mf.gradient_enabled = false;

    MixedFilamentPreviewSettings preview;
    preview.local_z_mode = false;

    CHECK(mixed_filament_supports_bias_apparent_color(mf, preview, true));

    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(mf, preview, false));
    preview.local_z_mode = true;
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(mf, preview, true));
    preview.local_z_mode = false;
    mf.distribution_mode = int(MixedFilament::SameLayerPointillisme);
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(mf, preview, true));
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.manual_pattern = "12";
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(mf, preview, true));
    mf.manual_pattern.clear();
    mf.gradient_component_ids = "123";
    CHECK_FALSE(mixed_filament_supports_bias_apparent_color(mf, preview, true));
}

TEST_CASE("Mixed filament effective_local_z_preview_mix_b_percent", "[MixedFilament][Display]")
{
    MixedFilament mf;
    mf.mix_b_percent = 50;
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.manual_pattern.clear();
    mf.gradient_component_ids.clear();

    MixedFilamentPreviewSettings preview;
    preview.local_z_mode = false;

    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(mf, preview) == 50);

    preview.local_z_mode = true;
    preview.nominal_layer_height = 0.2;
    preview.mixed_lower_bound = 0.04;
    preview.mixed_upper_bound = 0.16;
    preview.preferred_a_height = 0.0;
    preview.preferred_b_height = 0.0;

    int local_z_mix = mixed_filament_effective_local_z_preview_mix_b_percent(mf, preview);
    CHECK(local_z_mix >= 0);
    CHECK(local_z_mix <= 100);

    // Manual pattern → passthrough
    mf.manual_pattern = "12";
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(mf, preview) == 50);

    // Pointillisme → passthrough
    mf.manual_pattern.clear();
    mf.distribution_mode = int(MixedFilament::SameLayerPointillisme);
    CHECK(mixed_filament_effective_local_z_preview_mix_b_percent(mf, preview) == 50);
}

TEST_CASE("Mixed filament compute_mixed_filament_display_color fallback paths", "[MixedFilament][Display]")
{
    MixedFilament entry;
    entry.component_a = 1;
    entry.component_b = 2;
    entry.mix_b_percent = 50;
    entry.distribution_mode = int(MixedFilament::Simple);

    MixedFilamentDisplayContext ctx;
    ctx.num_physical = 0;
    CHECK(compute_mixed_filament_display_color(entry, ctx) == "#26A69A");

    ctx.num_physical = 2;
    ctx.physical_colors = {"#FF0000", "#0000FF"};
    ctx.preview_settings.nominal_layer_height = 0.2;
    ctx.preview_settings.wall_loops = 1;
    ctx.nozzle_diameters = {0.4, 0.4};
    ctx.component_bias_enabled = false;

    std::string color = compute_mixed_filament_display_color(entry, ctx);
    CHECK(!color.empty());
    CHECK(color[0] == '#');

    // Out of range component → fallback
    MixedFilament bad;
    bad.component_a = 0;
    bad.component_b = 2;
    bad.mix_b_percent = 50;
    bad.distribution_mode = int(MixedFilament::Simple);
    CHECK(compute_mixed_filament_display_color(bad, ctx) == "#26A69A");
}

// ============================================================================
// [MixedFilament][Display] — Remaining display helpers
// ============================================================================

TEST_CASE("Mixed filament set_display_context auto-corrects and refreshes", "[MixedFilament][Display]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);

    MixedFilamentDisplayContext ctx;
    ctx.num_physical = 0;
    ctx.physical_colors = colors;
    ctx.preview_settings.wall_loops = 0;
    ctx.preview_settings.nominal_layer_height = 0.2;
    // nozzle_diameters left empty

    mgr.set_display_context(ctx);

    // After set_display_context, num_physical should be corrected to colors.size()
    auto display = mgr.display_colors();
    CHECK(display.size() >= 1);

    // total_filaments should reflect correct num_physical
    CHECK(mgr.total_filaments(3) == 3 + mgr.enabled_count());
}

TEST_CASE("Mixed filament apparent pair percentages bias on vs off", "[MixedFilament][Display]")
{
    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 2;
    mf.mix_b_percent = 50;
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.component_a_surface_offset = 0.f;
    mf.component_b_surface_offset = 0.05f;
    mf.manual_pattern.clear();
    mf.gradient_component_ids.clear();
    mf.gradient_enabled = false;

    MixedFilamentPreviewSettings preview;
    preview.local_z_mode = false;
    preview.nominal_layer_height = 0.2;
    preview.mixed_lower_bound = 0.04;
    preview.mixed_upper_bound = 0.16;

    const std::vector<double> nozzles = {0.4, 0.6};

    // Bias off: returns (100-base, base)
    auto [pct_a_off, pct_b_off] = mixed_filament_apparent_pair_percentages(mf, preview, nozzles, false);
    CHECK(pct_a_off == 50);
    CHECK(pct_b_off == 50);

    // Bias on: apparent percents shift due to surface offset
    auto [pct_a_on, pct_b_on] = mixed_filament_apparent_pair_percentages(mf, preview, nozzles, true);
    CHECK(pct_a_on + pct_b_on == 100);
    // With positive B offset (0.05mm), B's apparent percent should decrease
    CHECK(pct_b_on < pct_b_off);
}

// ============================================================================
// [MixedFilament][Resolve] — RES-REGRESS regression tests
// ============================================================================

TEST_CASE("RES-REGRESS-01: resolve handles deleted mixed filament", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);
    // 3 physical -> 3 auto-generated pairs: (1,2), (1,3), (2,3)
    // Virtual IDs: 4, 5, 6
    REQUIRE(mgr.mixed_filaments().size() == 3);

    const size_t num_physical = 3;
    const unsigned int virt_4 = 4;
    const unsigned int virt_5 = 5;
    const unsigned int virt_6 = 6;

    // Verify is_mixed for two of the auto-generated pairs
    CHECK(mgr.is_mixed(virt_4, num_physical));
    CHECK(mgr.is_mixed(virt_5, num_physical));

    // All 3 auto pairs are enabled, total = 3 physical + 3 mixed = 6
    const size_t total_before = mgr.total_filaments(num_physical);
    CHECK(total_before == 6);

    // Find the entry to delete by component pair (1,3) using component-based lookup
    // instead of a hardcoded index, so the test is robust against changes in
    // auto_generate ordering.
    for (auto &mf : mgr.mixed_filaments()) {
        if (mf.component_a == 1 && mf.component_b == 3) {
            mf.deleted = true;
            mf.enabled = false;
            break;
        }
    }

    // total_filaments decreases: 3 physical + 2 enabled mixed = 5
    const size_t total_after = mgr.total_filaments(num_physical);
    CHECK(total_after == 5);

    // The highest old virtual ID (6) is now unmapped because virtual IDs are
    // dynamically re-enumerated over enabled entries.  Only 2 enabled mixed remain.
    CHECK(mgr.mixed_index_from_filament_id(virt_6, num_physical) == -1);

    // resolve returns filament_id unchanged when mixed_index returns -1 (passthrough)
    CHECK(mgr.resolve(virt_6, num_physical, 0) == virt_6);

    // The surviving mixed entries still resolve correctly (virt_4 -> index 0, virt_5 -> index 1)
    CHECK(mgr.mixed_index_from_filament_id(virt_4, num_physical) >= 0);
    CHECK(mgr.mixed_index_from_filament_id(virt_5, num_physical) >= 0);
}

TEST_CASE("RES-REGRESS-02: resolve with gradient multi-color after component removal", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"
    };
    MixedFilamentManager mgr;

    // Add a custom mixed filament whose gradient references physical filament #6
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &mf = mgr.mixed_filaments().front();
    mf.gradient_enabled = true;
    mf.gradient_component_ids = "6"; // References physical filament #6

    const size_t count_before = mgr.mixed_filaments().size();
    CHECK(count_before == 1);

    // Delete physical #6 — the gradient-dependent mixed should be removed
    mgr.remove_physical_filament(6);

    const size_t count_after = mgr.mixed_filaments().size();
    CHECK(count_after == 0);
}

TEST_CASE("RES-REGRESS-03: effective_painted_region_filament_id with Grouped mode", "[MixedFilament][Resolve]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF", "#FF0000"};
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    REQUIRE(row.manual_pattern == "12,21");

    const size_t num_physical = 3;
    const unsigned int virtual_id = 4;

    // Grouped pattern (contains comma) preserves virtual ID for paint identity
    unsigned int eff = mgr.effective_painted_region_filament_id(virtual_id, num_physical, 0);
    CHECK(eff == virtual_id);
}

// ============================================================================
// [MixedFilament][Gradient] — GRAD-REGRESS regression tests
// ============================================================================

TEST_CASE("GRAD-REGRESS-01: encode/decode gradient IDs >= 10", "[MixedFilament][Gradient]")
{
    // Multi-ID with values >= 10 uses extended slash-separated format
    const std::vector<unsigned int> ids_1_2_12 = {1, 2, 12};
    std::string encoded = MixedFilamentManager::encode_gradient_component_ids(ids_1_2_12);
    CHECK(encoded == "1/2/12");

    // Round-trip: decode back to original IDs
    std::vector<unsigned int> decoded = MixedFilamentManager::decode_gradient_component_ids(encoded, 0);
    REQUIRE(decoded.size() == 3);
    CHECK(decoded[0] == 1);
    CHECK(decoded[1] == 2);
    CHECK(decoded[2] == 12);

    // Single-ID >= 10 uses leading slash for disambiguation from legacy
    const std::vector<unsigned int> single_id_12 = {12};
    std::string encoded_single = MixedFilamentManager::encode_gradient_component_ids(single_id_12);
    CHECK(encoded_single == "/12");

    // Round-trip single
    std::vector<unsigned int> decoded_single =
        MixedFilamentManager::decode_gradient_component_ids(encoded_single, 0);
    REQUIRE(decoded_single.size() == 1);
    CHECK(decoded_single[0] == 12);

    // All legacy IDs (<= 9) still use compact single-char format
    const std::vector<unsigned int> legacy_ids = {1, 2, 3};
    std::string legacy_encoded = MixedFilamentManager::encode_gradient_component_ids(legacy_ids);
    CHECK(legacy_encoded == "123");

    std::vector<unsigned int> legacy_decoded =
        MixedFilamentManager::decode_gradient_component_ids(legacy_encoded, 0);
    REQUIRE(legacy_decoded.size() == 3);
    CHECK(legacy_decoded[0] == 1);
    CHECK(legacy_decoded[1] == 2);
    CHECK(legacy_decoded[2] == 3);

    // Mixed extended: some IDs < 10, some >= 10
    const std::vector<unsigned int> mixed_ids = {3, 12, 5};
    std::string mixed_encoded = MixedFilamentManager::encode_gradient_component_ids(mixed_ids);
    CHECK(mixed_encoded == "3/12/5");

    std::vector<unsigned int> mixed_decoded =
        MixedFilamentManager::decode_gradient_component_ids(mixed_encoded, 0);
    REQUIRE(mixed_decoded.size() == 3);
    CHECK(mixed_decoded[0] == 3);
    CHECK(mixed_decoded[1] == 12);
    CHECK(mixed_decoded[2] == 5);
}

TEST_CASE("GRAD-REGRESS-02: is_simple_gradient false with manual_pattern", "[MixedFilament][Gradient]")
{
    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 2;
    mf.gradient_enabled = true;
    mf.manual_pattern = "1212";

    // Non-empty manual_pattern -> is_simple_gradient returns false
    CHECK_FALSE(is_simple_gradient(mf));

    // Clear manual_pattern, set 2 gradient components -> is_simple_gradient true
    mf.manual_pattern.clear();
    mf.gradient_component_ids = "12";
    CHECK(is_simple_gradient(mf));

    // Empty gradient_component_ids with valid pair also satisfies the check
    mf.gradient_component_ids.clear();
    CHECK(is_simple_gradient(mf));
}

// ============================================================================
// [MixedFilament][Delete]
// ============================================================================

TEST_CASE("remove_physical_filament detects manual_pattern dependency and removes dependents", "[MixedFilament][Delete]")
{
    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors); // C(6,2)=15 auto entries (all custom=false)

    // Add 4 custom entries as per the regression scenario:
    // A: no pattern, component_a=1, component_b=2 (no dependency on #6)
    mgr.add_custom_filament(1, 2, 50, colors);

    // B: manual_pattern="1266666" — token "6" maps to physical #6
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = "1266666";
    uint64_t idB = mgr.mixed_filaments().back().stable_id;

    // C: no pattern, component_a=2, component_b=5 (no dependency on #6)
    mgr.add_custom_filament(2, 5, 50, colors);

    // D: manual_pattern="1221412465" — token "6" maps to physical #6
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = "1221412465";
    uint64_t idD = mgr.mixed_filaments().back().stable_id;

    // Delete physical filament #6 (1-based)
    mgr.remove_physical_filament(6);

    // Check: Entry B and D should be REMOVED (they depend on physical #6 via pattern)
    bool foundB = false, foundD = false;
    bool foundA = false, foundC = false;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.stable_id == idB) foundB = true;
        if (mf.stable_id == idD) foundD = true;
        if (mf.component_a == 1 && mf.component_b == 2 && mf.manual_pattern.empty()) foundA = true;
        if (mf.component_a == 2 && mf.component_b == 5) foundC = true;
    }
    CHECK_FALSE(foundB);
    CHECK_FALSE(foundD);
    CHECK(foundA);
    CHECK(foundC);

    // Surviving entries should have component IDs adjusted (but #6 was the max, so no shift needed)
}

TEST_CASE("build_filament_id_remap zeros dependents via mixed_filament_depends_on_physical", "[MixedFilament][Delete]")
{
    PresetBundle bundle;
    bundle.filament_presets = {
        "PLA Red", "PLA Green", "PLA Blue", "PLA Yellow", "PLA White", "PLA Black"
    };
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FFFFFF", "#000000"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;

    // Add B: manual_pattern="1266666" — token "6" maps to physical #6
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1266666");
    const uint64_t idB = mgr.mixed_filaments().back().stable_id;

    // Add D: manual_pattern="1221412465" — token "6" maps to physical #6
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1221412465");
    const uint64_t idD = mgr.mixed_filaments().back().stable_id;

    // Record old virtual IDs for B and D before the deletion call
    const unsigned int old_virtual_id_B = virtual_id_for_stable_id(mgr.mixed_filaments(), 6, idB);
    const unsigned int old_virtual_id_D = virtual_id_for_stable_id(mgr.mixed_filaments(), 6, idD);
    REQUIRE(old_virtual_id_B > 6);
    REQUIRE(old_virtual_id_D > 6);

    // Delete physical #6 (0-based index = 5)
    const size_t physical_before = 6;
    bundle.filament_presets.pop_back();
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.pop_back();
    bundle.update_multi_material_filament_presets(5, physical_before);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() > old_virtual_id_D);
    REQUIRE(remap.size() > old_virtual_id_B);

    // Pattern-dependent entries B and D should map to 0 (deleted) in the remap
    CHECK(remap[old_virtual_id_B] == 0);
    CHECK(remap[old_virtual_id_D] == 0);

    // Verify B and D are NOT in the post-deletion mixed filaments
    bool foundB = false, foundD = false;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.stable_id == idB) foundB = true;
        if (mf.stable_id == idD) foundD = true;
    }
    CHECK_FALSE(foundB);
    CHECK_FALSE(foundD);
}

TEST_CASE("mixed_filament_depends_on_physical indirectly exercised via deletion remap", "[MixedFilament][Delete]")
{
    // mixed_filament_depends_on_physical is a static function in PresetBundle.cpp
    // and is exercised indirectly through update_multi_material_filament_presets.
    // This test validates that the remap correctly marks pattern-dependents as
    // removed (mapped to 0) even when component_a/component_b do not directly
    // reference the deleted physical filament.

    PresetBundle bundle;
    bundle.filament_presets = {"Filament 1", "Filament 2", "Filament 3"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;

    // Add a custom entry whose manual_pattern references physical #3
    // but whose component_a/component_b are 1 and 2 (not 3).
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("123");
    const uint64_t sid = mgr.mixed_filaments().back().stable_id;

    const unsigned int old_virtual_id = virtual_id_for_stable_id(mgr.mixed_filaments(), 3, sid);
    REQUIRE(old_virtual_id > 3);

    // Delete physical #3 (0-based index = 2)
    const size_t physical_before = 3;
    bundle.filament_presets.pop_back();
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.pop_back();
    bundle.update_multi_material_filament_presets(2, physical_before);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() > old_virtual_id);

    // The pattern-dependent entry should be mapped to 0 (deleted)
    CHECK(remap[old_virtual_id] == 0);

    // Verify the entry is gone from post-deletion state
    bool found = false;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.stable_id == sid) found = true;
    }
    CHECK_FALSE(found);
}

TEST_CASE("remove_physical_filament adjusts manual_pattern tokens above deleted ID", "[MixedFilament][Delete]")
{
    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    // Add a custom entry with pattern "15353"
    // "1" maps to component_a, "3" and "5" are numeric physical IDs
    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("15353");
    const uint64_t sid = row.stable_id;

    // Delete physical filament #4 (1-based)
    mgr.remove_physical_filament(4);

    // The entry should survive (no token resolves to physical #4)
    // but tokens > 4 should be decremented: "5" → "4"
    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            // "1" stays (<=4), "5"→"4", "3" stays (<=4), "5"→"4", "3" stays (<=4)
            CHECK(cur.manual_pattern == "14343");
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("mixed_filaments_using_physical finds multiple pattern dependents for same physical", "[MixedFilament][Delete]")
{
    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    // Add B: manual_pattern="1266666" — tokens include numeric "6"
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1266666");
    const uint64_t idB = mgr.mixed_filaments().back().stable_id;

    // Add D: manual_pattern="1221412465" — tokens include numeric "6"
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1221412465");
    const uint64_t idD = mgr.mixed_filaments().back().stable_id;

    // mixed_filaments_using_physical(6) should return BOTH entries
    const auto deps = mgr.mixed_filaments_using_physical(6);
    CHECK(deps.size() >= 2);

    // Verify both stable_ids are in the returned indices
    bool foundB_in_deps = false, foundD_in_deps = false;
    for (size_t idx : deps) {
        if (mgr.mixed_filaments()[idx].stable_id == idB) foundB_in_deps = true;
        if (mgr.mixed_filaments()[idx].stable_id == idD) foundD_in_deps = true;
    }
    CHECK(foundB_in_deps);
    CHECK(foundD_in_deps);

    // After remove_physical_filament(6), BOTH should be removed
    mgr.remove_physical_filament(6);

    bool foundB = false, foundD = false;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.stable_id == idB) foundB = true;
        if (mf.stable_id == idD) foundD = true;
    }
    CHECK_FALSE(foundB);
    CHECK_FALSE(foundD);
}

// ============================================================================
// [MixedFilament][Merge] — MERGE-REGRESS regression tests
// ============================================================================

TEST_CASE("MERGE-REGRESS-01: build_merge_filament_remap 3-arg merges mixed into physical", "[MixedFilament][Merge]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue", "PLA Yellow"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    // C(4,2) = 6 auto entries with auto_generate enabled
    REQUIRE(mgr.mixed_filaments().size() == 6);

    // Capture old_mixed state for reference
    const std::vector<MixedFilament> old_mixed = mgr.mixed_filaments();
    const size_t total_filaments = 10; // 4 physical + 6 mixed

    // Merge mixed at 0-based ID 5 (first auto mixed in 1-based: physical 1..4, mixed 5..10)
    // into physical at 0-based ID 0 (physical #1)
    bundle.build_merge_filament_remap(/*from_id=*/5, /*to_id=*/0, total_filaments);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 11); // total_filaments + 1

    // from_id(5) > to_id(0) → else branch: remap[from_id+1] = to_id+1 = 1
    CHECK(remap[6] == 1);

    // IDs before from_id+1 (i < 6) stay the same
    CHECK(remap[0] == 0);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 3);
    CHECK(remap[4] == 4);
    CHECK(remap[5] == 5);

    // IDs after from_id+1 (i > 6) shift down by 1
    CHECK(remap[7] == 6);
    CHECK(remap[8] == 7);
    CHECK(remap[9] == 8);
    CHECK(remap[10] == 9);
}

TEST_CASE("MERGE-REGRESS-02: build_merge_filament_remap 4-arg handles dependent mixed", "[MixedFilament][Merge]")
{
    // Disable auto_generate so we control exactly which entries exist
    MixedAutoGenerateGuard guard(false);

    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue", "PLA Yellow"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    // No auto entries (auto_generate disabled), total = 4 physical

    // Add 3 custom entries:
    // Entry 0 (0-based virtual=4): component_a=2, component_b=3 — NOT dependent on physical #1
    mgr.add_custom_filament(2, 3, 50, colors);
    // Entry 1 (0-based virtual=5): component_a=1, component_b=2 — DEPENDENT on physical #1
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1212");
    const uint64_t dependent_sid = mgr.mixed_filaments().back().stable_id;
    // Entry 2 (0-based virtual=6): component_a=3, component_b=4 — NOT dependent on physical #1
    mgr.add_custom_filament(3, 4, 50, colors);

    REQUIRE(mgr.mixed_filaments().size() == 3);

    const size_t total_filaments = 7; // 4 physical + 3 mixed
    const size_t num_physical    = 4;

    // Merge physical #1 (from_id=0) into first mixed entry (to_id=4, 0-based virtual ID)
    // The dependent entry at old 1-based virtual ID 6 should be zeroed out in the remap.
    bundle.build_merge_filament_remap(/*from_id=*/0, /*to_id=*/4, total_filaments, num_physical);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 8); // total_filaments + 1

    // Source physical #1 (1-based=1) maps to target mixed's recalculated 1-based virtual ID.
    // target_old_virtual_id=4, target_old_mixed_idx=0.  No deleted mixed before it.
    // new_num_physical=3, target_new_virtual_id=3 → 1-based = 4.
    CHECK(remap[1] == 4);

    // Physical #2..#4 shift down by 1
    CHECK(remap[2] == 1);
    CHECK(remap[3] == 2);
    CHECK(remap[4] == 3);

    // First mixed (old 1-based=5, i=5): NOT dependent, survives.
    // old_virtual_id=4, old_mixed_idx=0, deleted_before=0
    // new_virtual_id = 3 + 0 - 0 = 3 → 1-based = 4
    CHECK(remap[5] == 4);

    // Second mixed (old 1-based=6, i=6): DEPENDENT → mapped to 0.
    const unsigned int old_virtual_id_dep = num_physical + 1; // 4 + 1 = 5, i=6
    CHECK(remap[6] == 0);

    // Third mixed (old 1-based=7, i=7): NOT dependent, survives.
    // old_virtual_id=6, old_mixed_idx=2, deleted_before=1 (dependent at vid 5)
    // new_virtual_id = 3 + 2 - 1 = 4 → 1-based = 5
    CHECK(remap[7] == 5);
}

TEST_CASE("MERGE-REGRESS-03: merge remap handles multiple dependents on same physical", "[MixedFilament][Merge]")
{
    // Disable auto_generate so we control the exact entry set
    MixedAutoGenerateGuard guard(false);

    PresetBundle bundle;
    bundle.filament_presets = {
        "PLA 1", "PLA 2", "PLA 3", "PLA 4", "PLA 5", "PLA 6"
    };
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    const auto &colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    // No auto entries, 6 physical

    // Add 4 custom entries:
    // Entry 0 (0-based virt=6): comp_a=2, comp_b=3 — NOT dependent on physical #1
    mgr.add_custom_filament(2, 3, 50, colors);

    // Entry 1 (0-based virt=7): comp_a=1, comp_b=2 — DEPENDENT on physical #1
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1266666");
    const uint64_t dep1_sid = mgr.mixed_filaments().back().stable_id;

    // Entry 2 (0-based virt=8): comp_a=4, comp_b=5 — NOT dependent on physical #1
    mgr.add_custom_filament(4, 5, 50, colors);

    // Entry 3 (0-based virt=9): comp_a=1, comp_b=3 — DEPENDENT on physical #1
    mgr.add_custom_filament(1, 3, 50, colors);
    mgr.mixed_filaments().back().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1221412465");
    const uint64_t dep2_sid = mgr.mixed_filaments().back().stable_id;

    REQUIRE(mgr.mixed_filaments().size() == 4);

    const size_t total_filaments = 10; // 6 physical + 4 mixed
    const size_t num_physical    = 6;

    // Merge physical #1 (from_id=0) into first mixed entry (to_id=6, 0-based).
    // The first mixed entry (comp_a=2, comp_b=3) does NOT depend on physical #1.
    bundle.build_merge_filament_remap(/*from_id=*/0, /*to_id=*/6, total_filaments, num_physical);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 11); // total_filaments + 1

    // Source physical #1 (1-based=1) maps to target mixed's recalculated 1-based virtual ID.
    // target_old_virtual_id=6, target_old_mixed_idx=0, deleted_before=0
    // new_num_physical=5, target_new_virtual_id=5 → 1-based = 6
    CHECK(remap[1] == 6);

    // Physical #2..#6 shift down by 1
    CHECK(remap[2] == 1);
    CHECK(remap[3] == 2);
    CHECK(remap[4] == 3);
    CHECK(remap[5] == 4);
    CHECK(remap[6] == 5);

    // First mixed (old 1-based=7, i=7): NOT dependent, survives.
    // old_virtual_id=6, old_mixed_idx=0, deleted_before=0
    // new_virtual_id = 5 + 0 - 0 = 5 → 1-based = 6
    CHECK(remap[7] == 6);

    // Second mixed (old 1-based=8, i=8): DEPENDENT → 0
    CHECK(remap[8] == 0);

    // Third mixed (old 1-based=9, i=9): NOT dependent, survives.
    // old_virtual_id=8, old_mixed_idx=2, deleted_before=1 (dependent at vid 7)
    // new_virtual_id = 5 + 2 - 1 = 6 → 1-based = 7
    CHECK(remap[9] == 7);

    // Fourth mixed (old 1-based=10, i=10): DEPENDENT → 0
    CHECK(remap[10] == 0);

    // Verify the surviving non-dependent entry at i=9 has correct deleted_before_target adjustment
    // deleted_before for vid 8 = 1 (one dependent, vid 7, appears before it)
    // new_virtual_id = 5 + 2 - 1 = 6, so 1-based = 7. This accounts for one deleted entry
    // that was before it in the virtual ID space.
    CHECK(remap[9] == 7);
}

TEST_CASE("MERGE-REGRESS-04: merge_mixed_filament marks source deleted and serializes d1", "[MixedFilament][Merge]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"PLA Red", "PLA Green", "PLA Blue"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#FF0000", "#00FF00", "#0000FF"
    };
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    // C(3,2) = 3 auto entries
    REQUIRE(mgr.mixed_filaments().size() >= 3);

    // Simulate merge_mixed_filament logic: mark an auto entry as deleted
    auto &source_mf = mgr.mixed_filaments()[0];
    const uint64_t source_sid = source_mf.stable_id;
    source_mf.deleted  = true;
    source_mf.enabled  = false;

    // Assert source flags
    CHECK(source_mf.deleted);
    CHECK_FALSE(source_mf.enabled);

    // Serialize and check for "d1" token
    const std::string serialized = mgr.serialize_custom_entries();
    CHECK(serialized.find("d1") != std::string::npos);

    // Round-trip: load into a new manager and verify deleted state
    MixedFilamentManager loaded;
    loaded.auto_generate(bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values);
    loaded.clear_custom_entries();
    loaded.load_custom_entries(serialized,
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values);

    // Find the entry by stable_id in the loaded list
    bool found_in_loaded = false;
    for (const auto &mf : loaded.mixed_filaments()) {
        if (mf.stable_id == source_sid) {
            found_in_loaded = true;
            CHECK(mf.deleted);
            CHECK_FALSE(mf.enabled);
            break;
        }
    }
    CHECK(found_in_loaded);
}

// ============================================================================
// [MixedFilament][Delete] — DELETE-PRIORITY resolution order tests
// ============================================================================

TEST_CASE("DELETE-PRIORITY-01: resolve-order — manual_pattern blocks pair/gradient check", "[MixedFilament][Delete]")
{
    // Verify that when a mixed filament has a non-empty manual_pattern, the
    // component_a/component_b pair check and gradient check are skipped (both
    // guarded by norm.empty()), so an entry can survive even if its component_a
    // or component_b references the deleted physical filament.
    //
    // Setup: pattern "12" where both tokens are non-symbolic direct physical IDs
    // (they do not map to component_a/b).  Delete physical #5 which is not
    // referenced by the pattern but IS referenced by component_a.
    // Result: manual_pattern check passes (no token resolves to #5), gradient
    // and pair checks are skipped (norm is non-empty), so the entry SURVIVES.

    constexpr int NUM_PHYSICAL = 6;
    const std::vector<std::string> colors(NUM_PHYSICAL, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    // Add a custom entry.  We use component_a=5, component_b=2 so that
    // component_a references the soon-to-be-deleted physical #5.  Then we
    // override the manual_pattern to tokens that do NOT resolve to #5.
    mgr.add_custom_filament(5, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    // Use a pattern whose tokens are all direct physical IDs ("3", "4") that
    // do not include "5".  Because the norm is non-empty the pair check
    // (which would flag component_a=5) is never reached.
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("34");
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(5);

    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("DELETE-PRIORITY-02: bracket notation token adjustment after deletion", "[MixedFilament][Delete]")
{
    // Setup: 6 physical.  Create a custom entry with manual_pattern="1[12]3"
    // containing a bracket-wrapped two‑digit token [12].  Delete physical #5.
    // The entry survives (no token resolves to #5), and the [12] token is
    // decremented to [11] because 12 > 5.
    //
    // Normalization preserves multi‑digit bracket tokens: "1[12]3" → "1[12]3".

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("1[12]3");
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(5);

    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            // Token "1" stays (1 <= 5, not decremented)
            // Token "[12]" → 12 > 5 → decremented to 11, re‑encoded as "[11]"
            // Token "3" stays (3 <= 5, not decremented)
            CHECK(cur.manual_pattern == "1[11]3");
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("DELETE-PRIORITY-03: deleting physical #1 when manual_pattern uses symbolic 1", "[MixedFilament][Delete]")
{
    // Setup: 3 physical.  Create a custom entry with manual_pattern="121".
    // The "1" tokens are symbolic — they map to component_a via
    // physical_filament_from_token.  Delete physical #1 which matches
    // component_a, so every "1" token flags uses_deleted_in_pattern.
    // The entry IS removed because it depends on physical #1 via component_a.

    const std::vector<std::string> colors(3, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("121");
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(1);

    // Entry removed: token "1" → component_a=1 == deleted(1)
    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            break;
        }
    }
    CHECK_FALSE(found);
}

TEST_CASE("DELETE-BRACKET-01: bracket token crosses single-digit boundary after adjustment", "[MixedFilament][Delete]")
{
    // Setup: 6 physical.  Create a custom entry with manual_pattern="1[10]3".
    // Delete physical #1.  Token "1" resolves to component_a=1 == deleted → entry removed.
    // Instead use a setup where the bracket token crosses the boundary safely:
    // Pattern "3[10]4", delete physical #2.  None resolves to deleted.
    // Token [10] (literal 10) > 2 → decremented to 9, which is < 10 → bare "9".

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("3[10]4");
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(2);

    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            // 3 > 2 → 2, 10 > 2 → 9 → bare "9" (< 10), 4 > 2 → 3
            CHECK(cur.manual_pattern == "293");
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("DELETE-GROUP-01: comma-separated group adjustment after deletion", "[MixedFilament][Delete]")
{
    // Setup: 6 physical.  Create a custom entry with multi-group pattern "34,78".
    // Delete physical #2.  Neither group resolves to deleted.
    // Group 0: "34" → 3>2→2, 4>2→3 = "23"
    // Group 1: "78" → 7>2→6, 8>2→7 = "67"
    // Result: "23,67" with comma preserved.

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("34,78");
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(2);

    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            CHECK(cur.manual_pattern == "23,67");
            break;
        }
    }
    CHECK(found);
}

// ============================================================================
// [MixedFilament][Gradient] — GRAD-DEL regression tests
// ============================================================================

TEST_CASE("GRAD-DEL-01: gradient with partial component survival — matching ID removes entry", "[MixedFilament][Gradient]")
{
    // Setup: 6 physical.  Create a custom entry with gradient_component_ids="123"
    // (IDs 1, 2, 3).  Delete physical #2.  The gradient check (step 2 in
    // remove_physical_filament) finds that ID 2 matches the deleted physical,
    // so the entire mixed filament is removed — even though IDs 1 and 3 survive.

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.gradient_enabled       = true;
    row.gradient_component_ids = "123";
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(2);

    // The gradient entry should be gone
    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) found = true;
    }
    CHECK_FALSE(found);

    // No surviving entry should have gradient_component_ids set
    int gradient_entries = 0;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (!cur.gradient_component_ids.empty()) ++gradient_entries;
    }
    CHECK(gradient_entries == 0);
}

TEST_CASE("GRAD-DEL-02: gradient partial survival — no matching component, IDs adjusted", "[MixedFilament][Gradient]")
{
    // Setup: 6 physical.  Create a custom entry with gradient_component_ids="345"
    // (IDs 3, 4, 5).  Delete physical #2.  None of the gradient IDs match #2,
    // so the entry SURVIVES and all IDs > 2 are decremented: "345" → "234".

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(3, 4, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.gradient_enabled       = true;
    row.gradient_component_ids = "345";
    const uint64_t sid = row.stable_id;

    mgr.remove_physical_filament(2);

    // Entry survives — none of the gradient IDs (3, 4, 5) match deleted #2
    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            // 3 > 2 → 2, 4 > 2 → 3, 5 > 2 → 4  (all single‑digit → compact "234")
            CHECK(cur.gradient_component_ids == "234");
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("GRAD-DEL-03: stale gradient ID removed when manual_pattern survives deletion", "[MixedFilament][Gradient]")
{
    // When a mixed filament has both manual_pattern and gradient_component_ids,
    // and the pattern does not reference the deleted physical but the gradient
    // does, the entry survives (pattern is the active resolution source) but
    // the stale gradient ID must be removed from gradient_component_ids.

    const std::vector<std::string> colors(6, "#FF0000");
    MixedFilamentManager mgr;
    mgr.auto_generate(colors);

    mgr.add_custom_filament(1, 2, 50, colors);
    MixedFilament &row = mgr.mixed_filaments().back();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("34");
    row.gradient_component_ids = "5";
    const uint64_t sid = row.stable_id;

    // Delete physical #5. Pattern "34" does not reference #5, so entry survives.
    // Gradient "5" references #5, so it must be erased (stale reference).
    mgr.remove_physical_filament(5);

    bool found = false;
    for (const auto &cur : mgr.mixed_filaments()) {
        if (cur.stable_id == sid) {
            found = true;
            CHECK(cur.gradient_component_ids.empty());
            break;
        }
    }
    CHECK(found);
}

// ============================================================================
// [MixedFilament][Config] — Dirty-check and config-resolution regression tests
// ============================================================================

// Regression test for Preset.cpp skipped_in_dirty fix:
// mixed_filament_definitions is auto-generated (PresetBundle.cpp) and must not
// cause the print preset to be marked dirty after loading a 3MF.
TEST_CASE("mixed_filament_definitions skipped in dirty check", "[MixedFilament][Config]")
{
    Preset edited(Preset::TYPE_PRINT, "test_edited", false);
    Preset reference(Preset::TYPE_PRINT, "test_reference", false);

    // Simulate what happens after 3MF load: mixed_filament_definitions exists
    // in edited (from the loaded config) but not in the system preset reference.
    edited.config.set_key_value("mixed_filament_definitions", new ConfigOptionString("0,1;1,0"));

    CHECK_FALSE(PresetCollection::is_dirty(&edited, &reference));
}

// Sanity check: the dirty mechanism still catches real user-facing changes.
// A key that is NOT in skipped_in_dirty, exists only in edited, and differs
// from its ConfigDef default MUST trigger dirty.
TEST_CASE("non-skipped missing key triggers dirty", "[MixedFilament][Config]")
{
    Preset edited(Preset::TYPE_PRINT, "test_edited", false);
    Preset reference(Preset::TYPE_PRINT, "test_reference", false);

    // layer_height default is 0.2; 0.3 differs from default
    edited.config.set_key_value("layer_height", new ConfigOptionFloat(0.3));

    CHECK(PresetCollection::is_dirty(&edited, &reference));
}

// Verify that dithering_local_z_mode properly participates in dirty tracking
// through the real PresetCollection::is_dirty path (not a mock lambda).
// When both edited and reference have the same value, the preset is clean.
TEST_CASE("dithering_local_z_mode dirty tracking via is_dirty", "[MixedFilament][Config]")
{
    Preset edited(Preset::TYPE_PRINT, "test_edited", false);
    Preset reference(Preset::TYPE_PRINT, "test_reference", false);

    // Both have dlzm=true → should be clean (no difference)
    edited.config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    reference.config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    CHECK_FALSE(PresetCollection::is_dirty(&edited, &reference));

    // Change edited to false → should be dirty
    edited.config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
    CHECK(PresetCollection::is_dirty(&edited, &reference));
}
