#include <catch2/catch_test_macros.hpp>
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfJson.hpp"
#include "libslic3r/Format/FullSpectrum3mf/Fs3mfLegacyBridge.hpp"
#include <limits>

using namespace Slic3r;
using namespace Slic3r::FullSpectrum3mf;

TEST_CASE("Gradient solid-zone ramps thicken only the approaching filament", "[GradientPort][Gradient][GradientRamp]")
{
    MixedFilamentDefinition row;
    row.behavior.gradient.enabled        = true;
    row.recipe.blend.components          = {{{1}, 1}, {{2}, 1}, {{3}, 1}};
    row.behavior.gradient.stop_positions = {0.f, .25f, .5f, .75f, 1.f};
    row.behavior.gradient.solid_widths   = {0.f, .03f, 0.f};
    const auto sample                    = [&](double t, double maximum = .30) {
        return sample_mixed_gradient_local_z(row, 3, t, .03, .20, .06, {.30, maximum, .30});
    };
    const auto white_height = [](const MixedGradientLocalZSample& value) {
        return value.mix.component_a == 2 ? value.height_a : value.height_b;
    };
    for (double t : {.0, .2, .4, .6, .8, 1.0}) {
        const auto actual   = sample(t);
        const auto original = mixed_filament_local_z_pair_heights(.20, .06, actual.mix.mix_b_percent);
        CHECK(actual.height_a == Approx(original.first));
        CHECK(actual.height_b == Approx(original.second));
    }
    double last_white = .14;
    for (int step = 0; step <= 70; ++step) {
        const double t        = .4125 + .001 * step;
        const auto   entering = sample(t);
        const auto   leaving  = sample(1.0 - t);
        CHECK(entering.mix.component_b == 2);
        CHECK(leaving.mix.component_a == 2);
        CHECK(entering.height_a == Approx(.06));
        CHECK(leaving.height_b == Approx(.06));
        CHECK(white_height(entering) >= last_white - 1e-7);
        CHECK(white_height(entering) == Approx(white_height(leaving)).margin(1e-6));
        CHECK(white_height(entering) <= .28 + 1e-7);
        last_white = white_height(entering);
    }
    CHECK(last_white > .279);
    CHECK(white_height(sample(.5)) == Approx(.28));
    CHECK(sample(.5).height_b == 0.0);
    CHECK(white_height(sample(.4825, .24)) > .239);
    CHECK(white_height(sample(.5, .24)) == Approx(.24));
    // A zero-width color stop is not a solid zone.
    row.behavior.gradient.solid_widths[1] = 0.f;
    CHECK(white_height(sample(.5)) == Approx(.20));
    row.behavior.gradient.solid_widths[1] = .03f;
    row.behavior.gradient.enabled         = false;
    CHECK(white_height(sample(.5)) == Approx(.20));
}

TEST_CASE("Gradient ramp follows moved stops and per-filament printer limits", "[GradientPort][Gradient][GradientRamp]")
{
    MixedFilamentDefinition row;
    row.behavior.gradient.enabled        = true;
    row.recipe.blend.components          = {{{4}, 1}, {{3}, 1}, {{2}, 1}, {{1}, 1}};
    row.behavior.gradient.stop_positions = {0.f, .1f, .3f, .5f, .7f, .9f, 1.f};
    row.behavior.gradient.solid_widths   = {0.f, .04f, .08f, 0.f};
    auto first                           = sample_mixed_gradient_local_z(row, 4, .3, .03, .20, .06, {.3, .24, .27, .3});
    auto second                          = sample_mixed_gradient_local_z(row, 4, .7, .03, .20, .06, {.3, .24, .27, .3});
    CHECK(first.mix.component_a == 3);
    CHECK(first.height_a == Approx(.27));
    CHECK(second.mix.component_a == 2);
    CHECK(second.height_a == Approx(.24));
    // With a different cycle setting, the target remains nominal + .08.
    first = sample_mixed_gradient_local_z(row, 4, .3, .03, .12, .04, {.3, .3, .3, .3});
    CHECK(first.height_a == Approx(.20));
}

TEST_CASE("Sliced gradients ramp on both sides of the solid zone within printer limits", "[GradientPort][LocalZ][Slice][GradientRamp]")
{
    double white_limit = .30;
    SECTION("Normal nozzle limit") {}
    SECTION("Lower white nozzle limit") { white_limit = .24; }
    const std::vector<std::string> colors{"#00FF00", "#FFFFFF", "#FFFF00"};
    MixedFilamentManager           manager;
    MixedFilamentDefinition        row;
    row.identity.stable_id               = 8123;
    row.source.kind                      = MixedFilamentSourceKind::Custom;
    row.recipe.blend.components          = {{{1}, 1}, {{2}, 1}, {{3}, 1}};
    row.behavior.gradient.enabled        = true;
    row.behavior.gradient.stop_positions = {0.f, .25f, .5f, .75f, 1.f};
    row.behavior.gradient.solid_widths   = {0.f, .03f, 0.f};
    manager.set_mixed_filament_definitions({row}, colors);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("layer_height", .20);
    config.set("initial_layer_print_height", .20);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("dithering_local_z_gradient_layer_height", .20);
    config.set("dithering_local_z_preserve_first_layer", true);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = {.4, .4, .4};
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                            "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));

    config.option<ConfigOptionStrings>("filament_colour")->values  = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("max_layer_height")->values  = {.30, white_limit, .30};
    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 4);
    object->add_volume(make_cube(8., 8., 20.));
    object->add_instance();
    object->ensure_on_bed();
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    auto* printed = print.get_object(0);
    printed->slice();
    REQUIRE_FALSE(printed->local_z_sublayer_plan().empty());
    bool         ramp_before = false, ramp_after = false, solid_target = false;
    const double target = std::min(.28, white_limit);
    for (const auto& pass : printed->local_z_sublayer_plan()) {
        REQUIRE(pass.flow_height > 0.0);
        CHECK(pass.z_hi <= 20.0 + 1e-5);
        if (pass.layer_id == 0) {
            CHECK(pass.flow_height == Approx(.20));
            continue;
        }
        REQUIRE(pass.painted_masks_by_extruder.size() >= 3);
        for (size_t component = 0; component < 3; ++component) {
            if (pass.painted_masks_by_extruder[component].empty())
                continue;
            CHECK(pass.flow_height <= (component == 1 ? white_limit : .30) + 1e-6);
            if (component == 1) {
                const double mid = .5 * (pass.z_lo + pass.z_hi);
                if (mid < 9.8 && pass.flow_height > .20)
                    ramp_before = true;
                if (mid > 10.4 && pass.flow_height > .20)
                    ramp_after = true;
                if (mid > 9.8 && mid < 10.4 && std::abs(pass.flow_height - target) < 1e-6)
                    solid_target = true;
            }
        }
    }
    CHECK(ramp_before);
    CHECK(ramp_after);
    CHECK(solid_target);
}

TEST_CASE("Gradient widths survive both legacy and Full Spectrum project serialization", "[GradientPort][MixedFilament][FullSpectrum3mf]")
{
    const std::vector<std::string> colors{"#00FF00", "#FFFFFF", "#FFFF00"};
    const std::vector<std::string> refs{"fil_green", "fil_white", "fil_yellow"};
    MixedFilamentDefinition        definition;
    definition.identity.stable_id               = 8123;
    definition.source.kind                      = MixedFilamentSourceKind::Custom;
    definition.recipe.blend.components          = {{{1}, 20}, {{2}, 30}, {{3}, 50}};
    definition.behavior.gradient.enabled        = true;
    definition.behavior.gradient.stop_positions = {0.f, .1f, .3f, .6f, 1.f};
    definition.behavior.gradient.solid_widths   = {0.f, .12f, 0.f};
    SECTION("Older recipes keep the global width fallback") { definition.behavior.gradient.solid_widths.clear(); }
    SECTION("Explicit widths") {}
    definition.behavior.distribution = MixedFilamentDistributionMode::LayerCycle;
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions({definition}, colors);
    MixedFilamentManager legacy;
    legacy.load_custom_entries(manager.serialize_custom_entries(), colors);
    const auto canonical = mixed_filaments_from_manager(manager, refs);
    const auto parsed    = parse_json<MixedFilaments>(serialize_json(canonical));
    const auto restored  = manager_from_mixed_filaments(parsed, colors, refs);
    for (const auto* rebuilt : {static_cast<const MixedFilamentManager*>(&legacy), &restored}) {
        const auto definitions = rebuilt->mixed_filament_definitions(colors.size());
        REQUIRE(definitions.size() == 1);
        const auto& actual = definitions.front();
        CHECK(actual.behavior.gradient.solid_widths == definition.behavior.gradient.solid_widths);
        CHECK(actual.behavior.gradient.stop_positions == definition.behavior.gradient.stop_positions);
        const auto widths = mixed_gradient_solid_half_widths(actual, 3, .08);
        REQUIRE(widths.size() == 3);
        CHECK(widths[1] == Approx(definition.behavior.gradient.solid_widths.empty() ? .04 : .06));
        const auto center = sample_mixed_gradient(actual, 3, .3, .08);
        CHECK(center.component_a == 2);
        CHECK(center.component_b == 2);
    }
}

TEST_CASE("Gradient widths are clamped to the neighboring midpoints", "[GradientPort][MixedFilament]")
{
    MixedFilamentDefinition definition;
    definition.behavior.gradient.enabled        = true;
    definition.recipe.blend.components          = {{{1}, 1}, {{2}, 1}, {{3}, 1}};
    definition.behavior.gradient.stop_positions = {0.f, .29f, .3f, .6f, 1.f};
    definition.behavior.gradient.solid_widths   = {0.f, 1.f, 0.f};
    CHECK(mixed_gradient_solid_half_widths(definition, 3)[1] == Approx(.01));
    definition.behavior.gradient.solid_widths[1] = 0.f;
    CHECK(mixed_gradient_solid_half_widths(definition, 3)[1] == 0.f);
    definition.behavior.gradient.solid_widths[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(mixed_gradient_solid_half_widths(definition, 3)[1] == Approx(.01));
    definition.recipe.blend.components.resize(2);
    definition.behavior.gradient.stop_positions    = {0.f, .5f, 1.f};
    definition.behavior.gradient.component_a_start = .01f;
    definition.behavior.gradient.component_a_end   = .99f;
    CHECK(sample_mixed_gradient(definition, 2, 0., .03).component_a == 2);
    CHECK(sample_mixed_gradient(definition, 2, 1., .03).component_a == 1);
}
