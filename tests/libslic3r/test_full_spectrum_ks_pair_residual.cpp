#include <catch2/catch.hpp>

#include "libslic3r/FullSpectrumICCPolynomialEstimator.hpp"
#include "libslic3r/FullSpectrumKSPairResidual.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

using namespace Slic3r;

namespace {

struct MixedColorSettingsGuard
{
    MixedColorSettingsGuard(MixedFilamentColorEngine engine, bool use_td)
        : previous_engine(MixedFilamentManager::color_engine())
        , previous_use_td(MixedFilamentManager::use_td_for_color_prediction())
    {
        MixedFilamentManager::set_color_engine(engine);
        MixedFilamentManager::set_use_td_for_color_prediction(use_td);
    }

    ~MixedColorSettingsGuard()
    {
        MixedFilamentManager::set_color_engine(previous_engine);
        MixedFilamentManager::set_use_td_for_color_prediction(previous_use_td);
    }

    MixedFilamentColorEngine previous_engine;
    bool                     previous_use_td;
};

struct MaterialCase
{
    const char *hex;
    double      td_mm;
};

struct PairCase
{
    MaterialCase a;
    MaterialCase b;
};

constexpr std::array<MaterialCase, 16> MEASURED_MATERIALS {{
    {"#008BB3", 6.4}, {"#AD4A76", 5.0}, {"#EBBE00", 9.7}, {"#7B7F80", 6.8},
    {"#00A0CC", 5.8}, {"#C34C7E", 5.2}, {"#FFB717", 4.5}, {"#E4E5E1", 6.1},
    {"#0091B8", 6.4}, {"#C64D7A", 5.0}, {"#FFB81B", 4.5}, {"#494340", 9.5},
    {"#B93C41", 4.8}, {"#0050A3", 5.2}, {"#E9BF00", 9.7}, {"#E3E4E0", 6.1},
}};

constexpr std::array<PairCase, 4> NATIVE_TD_PAIRS {{
    {{"#008BB3", 6.4}, {"#AD4A76", 5.0}},
    {{"#00A0CC", 5.8}, {"#C34C7E", 5.2}},
    {{"#0091B8", 6.4}, {"#C64D7A", 5.0}},
    {{"#B93C41", 4.8}, {"#0050A3", 5.2}},
}};

constexpr const char *CYAN_MATERIAL_ID =
    "panchroma_g6_8y9_7m5_0c6_4_sc185_lh0p08_sd10p8_008bb3_ad4a76_ebbe00_7b7f80_20260612_192835_266"
    "__slot_1__td_6.4";
constexpr const char *MAGENTA_MATERIAL_ID =
    "panchroma_g6_8y9_7m5_0c6_4_sc185_lh0p08_sd10p8_008bb3_ad4a76_ebbe00_7b7f80_20260612_192835_266"
    "__slot_2__td_5";
constexpr const char *WHITE_MATERIAL_ID =
    "panchromasnapseed_w6_1_c5_8_m5_2_y4_5_sc46_lh0p08_sd10_00a0cc_c34c7e_ffb717_e4e5e1_20260625_164811_523"
    "__slot_4__td_6.1";

bool is_rgb_hex(const std::string &value)
{
    if (value.size() != 7 || value.front() != '#')
        return false;
    for (size_t i = 1; i < value.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(value[i])))
            return false;
    }
    return true;
}

int rgb_hex_channel(const std::string &value, size_t channel)
{
    REQUIRE(is_rgb_hex(value));
    REQUIRE(channel < 3);

    const auto nibble = [](char ch) {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        return std::toupper(static_cast<unsigned char>(ch)) - 'A' + 10;
    };
    return 16 * nibble(value[1 + 2 * channel]) + nibble(value[2 + 2 * channel]);
}

} // namespace

TEST_CASE("ICC polynomial spectrum estimator matches pinned profile anchors",
          "[MixedFilament][Color][FullSpectrumKS][ICC]")
{
    struct Anchor
    {
        const char *hex;
        double      r400;
        double      r550;
        double      r700;
    };

    constexpr std::array<Anchor, 3> anchors {{
        {"#808080", 0.199341226, 0.211860790, 0.197229721},
        {"#123456", 0.077179592, 0.023264583, 0.051173693},
        {"#FEDCBA", 0.366944434, 0.722001847, 0.888527066},
    }};

    for (const Anchor &anchor : anchors) {
        CAPTURE(anchor.hex);
        const std::optional<FullSpectrumICCPolynomialEstimator::Spectrum> spectrum =
            FullSpectrumICCPolynomialEstimator::estimate_reflectance_from_srgb_hex(anchor.hex);
        REQUIRE(spectrum.has_value());
        CHECK((*spectrum)[0] == Approx(anchor.r400).margin(1e-6));
        CHECK((*spectrum)[15] == Approx(anchor.r550).margin(1e-6));
        CHECK((*spectrum)[30] == Approx(anchor.r700).margin(1e-6));
    }

    CHECK_FALSE(FullSpectrumICCPolynomialEstimator::estimate_reflectance_from_srgb_hex("#12345").has_value());
    CHECK_FALSE(FullSpectrumICCPolynomialEstimator::estimate_reflectance_from_srgb_hex("123456").has_value());
    CHECK_FALSE(FullSpectrumICCPolynomialEstimator::estimate_reflectance_from_srgb_hex("#12345G").has_value());
}

TEST_CASE("ICC fallback keeps red shades red and neighboring deep reds continuous",
          "[MixedFilament][Color][FullSpectrumKS][ICC]")
{
    const std::array<const char *, 4> source_colors {{"#C91218", "#C91418", "#C91618", "#C91718"}};
    std::optional<std::string> previous;

    for (const char *source : source_colors) {
        CAPTURE(source);
        const std::optional<std::string> estimated = full_spectrum_ks_blend_color(source, source, 50, 50);
        REQUIRE(estimated.has_value());

        const int red   = rgb_hex_channel(*estimated, 0);
        const int green = rgb_hex_channel(*estimated, 1);
        const int blue  = rgb_hex_channel(*estimated, 2);
        CHECK(red > 150);
        CHECK(green < 50);
        CHECK(blue < 50);

        if (previous) {
            CHECK(std::abs(red - rgb_hex_channel(*previous, 0)) <= 4);
            CHECK(std::abs(green - rgb_hex_channel(*previous, 1)) <= 4);
            CHECK(std::abs(blue - rgb_hex_channel(*previous, 2)) <= 4);
        }
        previous = estimated;
    }

    const std::optional<std::string> pale_red = full_spectrum_ks_blend_color("#F5ADAD", "#F5ADAD", 50, 50);
    REQUIRE(pale_red.has_value());
    const int pale_red_channel   = rgb_hex_channel(*pale_red, 0);
    const int pale_green_channel = rgb_hex_channel(*pale_red, 1);
    const int pale_blue_channel  = rgb_hex_channel(*pale_red, 2);
    CHECK(pale_red_channel > pale_green_channel + 50);
    CHECK(std::abs(pale_green_channel - pale_blue_channel) <= 4);
}

TEST_CASE("Removed red alias stays on the continuous ICC path",
          "[MixedFilament][Color][FullSpectrumKS][ICC][Continuity]")
{
    constexpr std::array<const char *, 3> reds {{"#C91718", "#C91818", "#C91918"}};
    std::optional<std::string> previous;

    for (const char *red_input : reds) {
        CAPTURE(red_input);
        CHECK_FALSE(full_spectrum_ks_profile_matches_color(red_input));
        CHECK_FALSE(full_spectrum_ks_profile_td_mm_for_color(red_input).has_value());

        const std::optional<std::string> output = full_spectrum_ks_blend_color_multi(
            std::vector<std::pair<std::string, int>> {
                {red_input, 40}, {"#0050A3", 20}, {"#E9BF00", 20}, {"#E3E4E0", 20}
            });
        REQUIRE(output.has_value());

        const int red   = rgb_hex_channel(*output, 0);
        const int green = rgb_hex_channel(*output, 1);
        const int blue  = rgb_hex_channel(*output, 2);
        CHECK(red > green + 50);
        CHECK(red > blue + 50);

        if (previous) {
            CHECK(std::abs(red - rgb_hex_channel(*previous, 0)) <= 4);
            CHECK(std::abs(green - rgb_hex_channel(*previous, 1)) <= 4);
            CHECK(std::abs(blue - rgb_hex_channel(*previous, 2)) <= 4);
        }
        previous = output;
    }
}

TEST_CASE("KM/K-S profile exposes the embedded four-profile database", "[MixedFilament][Color][FullSpectrumKS]")
{
    CHECK(std::string(full_spectrum_ks_profile_id()) ==
          "fullspectrum_material_database_4profiles_lh0p08_sce_black_d65_10_20260701");
    CHECK(full_spectrum_ks_profile_material_count() == 16);
    CHECK(full_spectrum_ks_profile_pair_count() == 24);
    CHECK(std::string(full_spectrum_ks_profile_specular_mode()) == "SCE");
    CHECK(std::string(full_spectrum_ks_profile_backing_condition()) == "black_backing");
}

TEST_CASE("KM/K-S profile recognizes all measured materials and their native TD", "[MixedFilament][Color][FullSpectrumKS]")
{
    for (const MaterialCase &material : MEASURED_MATERIALS) {
        CAPTURE(material.hex, material.td_mm);
        CHECK(full_spectrum_ks_profile_matches_color(material.hex));

        const std::optional<double> td = full_spectrum_ks_profile_td_mm_for_color(material.hex);
        REQUIRE(td.has_value());
        CHECK(*td == Approx(material.td_mm));
    }

    CHECK_FALSE(full_spectrum_ks_profile_matches_color("#123456"));
    CHECK_FALSE(full_spectrum_ks_profile_td_mm_for_color("#123456").has_value());
}

TEST_CASE("Native profile TD is neutral for every measured material family", "[MixedFilament][Color][FullSpectrumKS][TD]")
{
    for (const PairCase &pair : NATIVE_TD_PAIRS) {
        CAPTURE(pair.a.hex, pair.a.td_mm, pair.b.hex, pair.b.td_mm);

        const std::optional<std::string> without_td =
            full_spectrum_ks_blend_color(pair.a.hex, pair.b.hex, 63, 37);
        const std::optional<std::string> at_native_td =
            full_spectrum_ks_blend_color(pair.a.hex, pair.b.hex, 63, 37, pair.a.td_mm, pair.b.td_mm);

        REQUIRE(without_td.has_value());
        REQUIRE(at_native_td.has_value());
        CHECK(*at_native_td == *without_td);
    }
}

TEST_CASE("Selected KM/K-S engine applies and globally disables TD correction", "[MixedFilament][Color][FullSpectrumKS][TD]")
{
    MixedColorSettingsGuard guard(MixedFilamentColorEngine::FullSpectrumKSPairResidual, true);

    const std::string without_td = MixedFilamentManager::blend_color("#008BB3", "#AD4A76", 50, 50);
    const std::string native_td  = MixedFilamentManager::blend_color("#008BB3", "#AD4A76", 50, 50, 6.4, 5.0);
    const std::string changed_td = MixedFilamentManager::blend_color("#008BB3", "#AD4A76", 50, 50, 3.2, 5.0);

    CHECK(native_td == without_td);
    CHECK(changed_td != without_td);

    MixedFilamentManager::set_use_td_for_color_prediction(false);
    CHECK(MixedFilamentManager::blend_color("#008BB3", "#AD4A76", 50, 50, 3.2, 5.0) == without_td);
}

TEST_CASE("Unknown valid colors use spectral estimates and invalid input falls back", "[MixedFilament][Color][FullSpectrumKS]")
{
    MixedColorSettingsGuard guard(MixedFilamentColorEngine::FullSpectrumKSPairResidual, true);

    const std::optional<std::string> estimated = full_spectrum_ks_blend_color("#123456", "#FEDCBA", 40, 60);
    REQUIRE(estimated.has_value());
    CHECK(is_rgb_hex(*estimated));
    CHECK(MixedFilamentManager::blend_color("#123456", "#FEDCBA", 40, 60) == *estimated);

    const std::string ks_invalid_fallback = MixedFilamentManager::blend_color("#not-a-color", "#FEDCBA", 40, 60);
    MixedFilamentManager::set_color_engine(MixedFilamentColorEngine::FilamentMixer);
    const std::string legacy_invalid_result = MixedFilamentManager::blend_color("#not-a-color", "#FEDCBA", 40, 60);
    CHECK(ks_invalid_fallback == legacy_invalid_result);
    CHECK(is_rgb_hex(ks_invalid_fallback));
}

TEST_CASE("Alternate display colors are not material aliases", "[MixedFilament][Color][FullSpectrumKS][Identity]")
{
    constexpr std::array<const char *, 8> removed_aliases {{
        "#0091B3", "#AE537F", "#C8AA0F", "#868787",
        "#FFFFFF", "#000000", "#0000FF", "#C91818"
    }};
    for (const char *color : removed_aliases) {
        CAPTURE(color);
        CHECK_FALSE(full_spectrum_ks_profile_matches_color(color));
        CHECK_FALSE(full_spectrum_ks_profile_td_mm_for_color(color).has_value());
    }
}

TEST_CASE("Generic UI colors require stable material identity for calibrated anchors",
          "[MixedFilament][Color][FullSpectrumKS][Identity]")
{
    for (const char *generic : {"#FFFFFF", "#000000", "#0000FF"}) {
        CAPTURE(generic);
        CHECK_FALSE(full_spectrum_ks_profile_matches_color(generic));
        CHECK_FALSE(full_spectrum_ks_profile_td_mm_for_color(generic).has_value());
    }

    const std::vector<FullSpectrumKSPairResidualColorInput> measured_inputs {
        {"#E4E5E1", 50, 6.1, std::string(WHITE_MATERIAL_ID)},
        {"#AD4A76", 50, 5.0, std::string(MAGENTA_MATERIAL_ID)},
    };
    const std::vector<FullSpectrumKSPairResidualColorInput> identified_display_inputs {
        {"#FFFFFF", 50, 6.1, std::string(WHITE_MATERIAL_ID)},
        {"#AD4A76", 50, 5.0, std::string(MAGENTA_MATERIAL_ID)},
    };
    const std::vector<FullSpectrumKSPairResidualColorInput> uncalibrated_display_inputs {
        {"#FFFFFF", 50, 6.1, std::nullopt},
        {"#AD4A76", 50, 5.0, std::nullopt},
    };

    const std::optional<std::string> measured = full_spectrum_ks_blend_color_multi(measured_inputs);
    const std::optional<std::string> identified = full_spectrum_ks_blend_color_multi(identified_display_inputs);
    const std::optional<std::string> inferred = full_spectrum_ks_blend_color_multi(uncalibrated_display_inputs);
    REQUIRE(measured.has_value());
    REQUIRE(identified.has_value());
    REQUIRE(inferred.has_value());
    CHECK(*identified == *measured);
    CHECK(*inferred != *measured);
    CHECK(*identified == "#BD648D");
    CHECK(*inferred == "#C0638E");
}

TEST_CASE("Stable material identity takes precedence over the display color",
          "[MixedFilament][Color][FullSpectrumKS][Identity]")
{
    const std::vector<FullSpectrumKSPairResidualColorInput> measured_inputs {
        {"#008BB3", 55, 6.4, std::nullopt},
        {"#AD4A76", 45, 5.0, std::nullopt},
    };
    const std::vector<FullSpectrumKSPairResidualColorInput> identified_inputs {
        {"#123456", 55, 6.4, std::string(CYAN_MATERIAL_ID)},
        {"#FEDCBA", 45, 5.0, std::string(MAGENTA_MATERIAL_ID)},
    };
    const std::vector<FullSpectrumKSPairResidualColorInput> stale_identity_inputs {
        {"#008BB3", 55, 6.4, std::string("retired-material-id")},
        {"#AD4A76", 45, 5.0, std::string(MAGENTA_MATERIAL_ID)},
    };
    const std::vector<FullSpectrumKSPairResidualColorInput> estimated_inputs {
        {"#123456", 55, 6.4, std::nullopt},
        {"#FEDCBA", 45, 5.0, std::nullopt},
    };

    const std::optional<std::string> measured = full_spectrum_ks_blend_color_multi(measured_inputs);
    const std::optional<std::string> identified = full_spectrum_ks_blend_color_multi(identified_inputs);
    const std::optional<std::string> stale_identity = full_spectrum_ks_blend_color_multi(stale_identity_inputs);
    const std::optional<std::string> estimated = full_spectrum_ks_blend_color_multi(estimated_inputs);
    REQUIRE(measured.has_value());
    REQUIRE(identified.has_value());
    REQUIRE(stale_identity.has_value());
    REQUIRE(estimated.has_value());
    CHECK(*identified == *measured);
    CHECK(*stale_identity != *measured);
    CHECK(*estimated != *measured);
    CHECK(*identified == "#5B5686");
    CHECK(*stale_identity == "#67628A");
    CHECK(*estimated == "#144E6D");
}

TEST_CASE("Inferred material TD accepts local measurements and rejects distant ones",
          "[MixedFilament][Color][FullSpectrumKS][TD]")
{
    const auto blend_with_cyan_td = [](double td_mm, const std::optional<std::string> &material_id) {
        return full_spectrum_ks_blend_color_multi(std::vector<FullSpectrumKSPairResidualColorInput> {
            {"#008BB3", 50, td_mm, material_id},
            {"#AD4A76", 50, 5.0, std::string(MAGENTA_MATERIAL_ID)},
        });
    };

    // Cyan's native TD is 6.4 mm, so its inferred acceptance radius is 1.6 mm.
    const std::optional<std::string> accepted_inference = blend_with_cyan_td(8.0, std::nullopt);
    const std::optional<std::string> accepted_identity = blend_with_cyan_td(8.0, std::string(CYAN_MATERIAL_ID));
    const std::optional<std::string> rejected_inference = blend_with_cyan_td(8.1, std::nullopt);
    const std::optional<std::string> rejected_identity = blend_with_cyan_td(8.1, std::string(CYAN_MATERIAL_ID));
    REQUIRE(accepted_inference.has_value());
    REQUIRE(accepted_identity.has_value());
    REQUIRE(rejected_inference.has_value());
    REQUIRE(rejected_identity.has_value());
    CHECK(*accepted_inference == *accepted_identity);
    CHECK(*rejected_inference != *rejected_identity);
    CHECK(*accepted_inference == "#645382");
    CHECK(*rejected_inference == "#6F5C85");
    CHECK(*rejected_identity == "#655382");
}

TEST_CASE("Full-spectrum colors require exactly hash-prefixed RRGGBB input",
          "[MixedFilament][Color][FullSpectrumKS]")
{
    CHECK_FALSE(full_spectrum_ks_profile_matches_color("#008BB3garbage"));
    CHECK_FALSE(full_spectrum_ks_profile_td_mm_for_color("#008BB3garbage").has_value());
    CHECK_FALSE(full_spectrum_ks_blend_color("#008BB3garbage", "#AD4A76", 50, 50).has_value());
    CHECK_FALSE(full_spectrum_ks_blend_color("#008BB", "#AD4A76", 50, 50).has_value());
}

TEST_CASE("Mixed color engine names round-trip and preserve legacy aliases", "[MixedFilament][Color][FullSpectrumKS]")
{
    MixedColorSettingsGuard guard(MixedFilamentColorEngine::FilamentMixer, true);

    CHECK(MixedFilamentManager::color_engine_from_string("filament_mixer") == MixedFilamentColorEngine::FilamentMixer);
    CHECK(MixedFilamentManager::color_engine_from_string("unknown") == MixedFilamentColorEngine::FilamentMixer);
    CHECK(MixedFilamentManager::color_engine_from_string("ks_pair_residual") ==
          MixedFilamentColorEngine::FullSpectrumKSPairResidual);
    CHECK(MixedFilamentManager::color_engine_from_string("fullspectrum_ks_pair_residual") ==
          MixedFilamentColorEngine::FullSpectrumKSPairResidual);
    CHECK(std::string(MixedFilamentManager::color_engine_to_string(MixedFilamentColorEngine::FilamentMixer)) ==
          "filament_mixer");
    CHECK(std::string(MixedFilamentManager::color_engine_to_string(MixedFilamentColorEngine::FullSpectrumKSPairResidual)) ==
          "ks_pair_residual");

    MixedFilamentManager::set_color_engine(MixedFilamentColorEngine::FullSpectrumKSPairResidual);
    CHECK(MixedFilamentManager::color_engine() == MixedFilamentColorEngine::FullSpectrumKSPairResidual);
    MixedFilamentManager::set_color_engine(MixedFilamentColorEngine::FilamentMixer);
    CHECK(MixedFilamentManager::color_engine() == MixedFilamentColorEngine::FilamentMixer);
}

TEST_CASE("Display context routes physical TDs into KM/K-S prediction", "[MixedFilament][Color][FullSpectrumKS][TD]")
{
    MixedColorSettingsGuard guard(MixedFilamentColorEngine::FullSpectrumKSPairResidual, true);

    MixedFilamentLegacyRow row;
    row.component_a  = 1;
    row.component_b  = 2;
    row.mix_b_percent = 50;

    MixedFilamentDisplayContext context;
    context.num_physical    = 2;
    context.physical_colors = {"#008BB3", "#AD4A76"};
    context.nozzle_diameters = {0.4, 0.4};
    context.physical_tds     = {3.2, 5.0};

    const std::string routed_td = compute_mixed_filament_display_color(row, context);
    CHECK(routed_td == MixedFilamentManager::blend_color("#008BB3", "#AD4A76", 50, 50, 3.2, 5.0));

    context.physical_tds.clear();
    const std::string without_td = compute_mixed_filament_display_color(row, context);
    CHECK(routed_td != without_td);

    context.physical_tds = {3.2, 5.0};
    MixedFilamentManager::set_use_td_for_color_prediction(false);
    CHECK(compute_mixed_filament_display_color(row, context) == without_td);
}

TEST_CASE("Display context routes stable material IDs into KM/K-S prediction",
          "[MixedFilament][Color][FullSpectrumKS][Identity]")
{
    MixedColorSettingsGuard guard(MixedFilamentColorEngine::FullSpectrumKSPairResidual, true);

    MixedFilamentLegacyRow row;
    row.component_a   = 1;
    row.component_b   = 2;
    row.mix_b_percent = 50;

    MixedFilamentDisplayContext context;
    context.num_physical          = 2;
    context.physical_colors       = {"#FFFFFF", "#AD4A76"};
    context.nozzle_diameters      = {0.4, 0.4};
    context.physical_tds          = {6.1, 5.0};
    context.physical_material_ids = {WHITE_MATERIAL_ID, MAGENTA_MATERIAL_ID};

    const std::string identified = compute_mixed_filament_display_color(row, context);
    CHECK(identified == "#BD648D");

    context.physical_material_ids.clear();
    const std::string inferred = compute_mixed_filament_display_color(row, context);
    CHECK(inferred == "#C0638E");
    CHECK(inferred != identified);
}
