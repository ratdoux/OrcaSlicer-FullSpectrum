#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <boost/filesystem.hpp>
#include <tbb/task_arena.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

using namespace Slic3r;

TEST_CASE("Sliced painted zones start and restart their own mixed pattern", "[SlicingSpeedPort][PaintedCadence][Slice]")
{
    const int workers        = GENERATE(1, 4);
    bool      manual_pattern = false;
    bool      collapse       = true;
    SECTION("Layer-ratio patterns") {}
    SECTION("Manual patterns") { manual_pattern = true; }
    SECTION("Disabled collapse retains virtual regions") { collapse = false; }

    const std::vector<std::string>       colors{"#FF0000", "#0000FF", "#888888"};
    std::vector<MixedFilamentDefinition> definitions(2);
    for (size_t i = 0; i < definitions.size(); ++i) {
        auto& definition                   = definitions[i];
        definition.identity.stable_id      = 9301 + i;
        definition.source.kind             = MixedFilamentSourceKind::Custom;
        definition.recipe.blend.components = {{{1}, 50}, {{2}, 50}};
        definition.behavior.distribution   = MixedFilamentDistributionMode::LayerCycle;
        definition.behavior.layer_cadence  = {1, 1};
    }
    if (manual_pattern)
        definitions[0].recipe.manual_pattern = MixedFilamentManualPattern{{{{1}, {2}, {2}}}};
    definitions[1].recipe.manual_pattern = MixedFilamentManualPattern{{{{2}, {1}, {1}}}};
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions(definitions, colors);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.option<ConfigOptionStrings>("filament_colour")->values  = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {.4, .4, .4};
    config.set("layer_height", .20);
    config.set("initial_layer_print_height", .20);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("mixed_filament_region_collapse", collapse);
    config.set("mixed_filament_gradient_mode", false);
    config.set("mixed_filament_advanced_dithering", false);
    config.set("mixed_filament_component_bias_enabled", false);
    config.set("dithering_local_z_mode", false);
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                            "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));

    struct Zone
    {
        double       x, z_begin, z_end;
        unsigned int channel;
    };
    // One channel has simultaneous disconnected components with different
    // starting heights, plus a component that disappears and reappears.
    const std::array<Zone, 5> zones{{{10., .6, 2.8, 4}, {20., 1., 2.8, 4}, {30., .2, 1., 4}, {30., 1.6, 3., 4}, {40., .8, 3., 5}}};
    Model                     model;
    auto*                     object = model.add_object();
    object->config.set("extruder", 3);
    object->add_volume(make_cube(4., 4., 4.)); // Physical anchor keeps all nominal layers present.
    for (const auto& zone : zones) {
        auto* volume = object->add_volume(make_cube(4., 4., zone.z_end - zone.z_begin));
        volume->translate(Vec3d(zone.x, 0., zone.z_begin));
        TriangleSelector selector(volume->mesh());
        for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
            selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(zone.channel));
        volume->mmu_segmentation_facets.set(selector);
    }
    object->add_instance();
    object->ensure_on_bed();
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    auto*           printed = print.get_object(0);
    tbb::task_arena arena(workers);
    arena.execute([&] { printed->slice(); });
    REQUIRE(printed->local_z_sublayer_plan().empty());
    REQUIRE(printed->layer_count() == 20);
    const std::vector<unsigned int> first_pattern = manual_pattern ? std::vector<unsigned int>{1, 2, 2} : std::vector<unsigned int>{1, 2};
    const std::vector<unsigned int> second_pattern{2, 1, 1};
    for (const auto& zone : zones) {
        size_t      local_layer = 0;
        const Point sample      = Point(scale_(zone.x + 2.), scale_(2.)) - printed->center_offset();
        for (const auto* layer : printed->layers()) {
            if (layer->slice_z < zone.z_begin || layer->slice_z > zone.z_end)
                continue;
            const auto&        pattern             = zone.channel == 4 ? first_pattern : second_pattern;
            const unsigned int expected            = collapse ? pattern[local_layer % pattern.size()] : zone.channel;
            size_t             containing_surfaces = 0;
            for (const auto* region : layer->regions()) {
                for (const auto& surface : region->slices.surfaces) {
                    if (!surface.expolygon.contains(sample))
                        continue;
                    ++containing_surfaces;
                    CAPTURE(workers, manual_pattern, collapse, zone.x, zone.z_begin, layer->slice_z, local_layer);
                    CHECK(region->region().config().wall_filament.value == expected);
                }
            }
            REQUIRE(containing_surfaces == 1);
            ++local_layer;
        }
        CHECK(local_layer == size_t(std::lround((zone.z_end - zone.z_begin) / .2)));
    }
}

// Ported from 707c6e1853. FullSpectrum already has the scheduling fix;
// retain its end-to-end check while parallelizing wipe-tower eligibility.
TEST_CASE("Local-Z prime towers emit every planned layer and extrusion", "[SlicingSpeedPort][LocalZ][GCode][WipeTower]")
{
    double layer_height = .20;
    bool   gradient     = false;
    SECTION("Independent mixes at standard layer height") {}
    SECTION("Independent mixes with single-pass intervals") { layer_height = .08; }
    SECTION("Gradients with single-pass intervals")
    {
        layer_height = .08;
        gradient     = true;
    }

    const std::vector<std::string>       colors{"#00C3FF", "#F54399", "#FAE727", "#9199A4"};
    MixedFilamentManager                 manager;
    std::vector<MixedFilamentDefinition> rows(2);
    rows[0].recipe.blend.components = {{{1}, 35}, {{2}, 65}};
    rows[1].recipe.blend.components = {{{1}, 13}, {{2}, 18}, {{3}, 69}};
    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i].identity.stable_id        = 9101 + i;
        rows[i].source.kind               = MixedFilamentSourceKind::Custom;
        rows[i].behavior.distribution     = MixedFilamentDistributionMode::LayerCycle;
        rows[i].behavior.gradient.enabled = gradient;
    }
    manager.set_mixed_filament_definitions(rows, colors);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("layer_height", layer_height);
    config.set("initial_layer_print_height", .20);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("dithering_local_z_mode", true);
    config.set("dithering_local_z_infill", true);
    config.set("dithering_local_z_direct_multicolor", true);
    config.set("dithering_local_z_independent_layer_height", true);
    config.set("enable_prime_tower", true);
    config.set("wipe_tower_no_sparse_layers", false);
    config.set("single_extruder_multi_material", false);
    config.set("prime_volume", 15.);
    config.set("machine_start_gcode", "G90\nM83\n");
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = {.4, .4, .4, .4};
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                            "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));
    config.option<ConfigOptionStrings>("filament_colour")->values  = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("max_layer_height")->values  = {.30, .30, .30, .30};
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(16, 100.);
    // Production G-code export requires the enum dictionaries used by preset loading.
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() != coEnums)
            continue;
        auto* option = config.def()->get(key)->create_default_option();
        option->set(config.option(key));
        config.set_key_value(key, option);
    }

    Model model;
    auto* object = model.add_object();
    object->name = "prime-tower-mixed-pass-regression";
    object->config.set("extruder", 4);
    // The 0.08 mm sections cross the 64-layer preparation batch boundary.
    auto*            volume = object->add_volume(make_cube(8., 8., 6.));
    TriangleSelector selector(volume->mesh());
    const auto&      mesh = volume->mesh().its;
    for (size_t facet = 0; facet < mesh.indices.size(); ++facet) {
        const auto& triangle = mesh.indices[facet];
        const float center_x = (mesh.vertices[triangle[0]].x() + mesh.vertices[triangle[1]].x() + mesh.vertices[triangle[2]].x()) / 3.f;
        selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(center_x < 4.f ? 5 : 6));
    }
    volume->mmu_segmentation_facets.set(selector);
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();

    Print print;
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    const auto& tower = print.wipe_tower_data();
    REQUIRE(tower.tool_changes.size() > 10);
    REQUIRE(tower.local_z_tool_changes.size() == tower.tool_changes.size());
    if (layer_height < .1) {
        const auto& intervals = print.get_object(0)->local_z_intervals();
        REQUIRE(std::any_of(intervals.begin(), intervals.end(), [](const LocalZInterval& interval) {
            return interval.independent_layer_height && interval.sublayer_count == 1;
        }));
    }

    struct TemporaryGCode
    {
        boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tower-%%%%-%%%%.gcode");
        ~TemporaryGCode()
        {
            boost::system::error_code ec;
            boost::filesystem::remove(path, ec);
        }
    } temporary;
    GCodeProcessorResult result;
    print.export_gcode(temporary.path.string(), &result, nullptr);

    // Compare the actual exported moves with every nominal and Local-Z tower
    // section. Checking height alone misses skipped walls on partly printed layers.
    auto                  z_key = [](double z) { return int(std::lround(z * 1000.)); };
    std::map<int, double> actual_lengths;
    for (size_t idx = 1; idx < result.moves.size(); ++idx) {
        const auto& move = result.moves[idx];
        if (move.type == EMoveType::Extrude && move.extrusion_role == erWipeTower)
            actual_lengths[z_key(move.position.z())] += (move.position - result.moves[idx - 1].position).head<2>().norm();
    }
    for (size_t layer_idx = 0; layer_idx < tower.tool_changes.size(); ++layer_idx) {
        const auto& nominal = tower.tool_changes[layer_idx];
        REQUIRE_FALSE(nominal.empty());
        const auto& local_z         = tower.local_z_tool_changes[layer_idx];
        double      expected_length = 0.;
        for (const auto* changes : {&nominal, &local_z}) {
            for (const auto& change : *changes) {
                for (size_t idx = 1; idx < change.extrusions.size(); ++idx) {
                    if (change.extrusions[idx].width > 0.f)
                        expected_length += (change.extrusions[idx].pos - change.extrusions[idx - 1].pos).norm();
                }
            }
        }
        CAPTURE(layer_idx, nominal.front().print_z, expected_length);
        REQUIRE(expected_length > 0.);
        const auto actual = actual_lengths.find(z_key(nominal.front().print_z));
        REQUIRE(actual != actual_lengths.end());
        // Tower coordinates are rounded to 0.001 mm when exported.
        CHECK(actual->second == Approx(expected_length).margin(.5));
    }
}
