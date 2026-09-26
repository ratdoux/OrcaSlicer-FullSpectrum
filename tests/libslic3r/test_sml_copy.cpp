#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <boost/filesystem.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("Pasted objects retain SML when reusing sliced layers", "[LocalZ][Slice][SMLCopy]")
{
    const bool independent_heights = GENERATE(false, true);
    bool       paste_after_slicing = false;
    SECTION("Paste before slicing") {}
    SECTION("Paste after slicing and reuse cached layers") { paste_after_slicing = true; }

    const std::vector<std::string> colors{"#00FF00", "#FFFFFF", "#000000"};
    MixedFilamentManager           manager;
    manager.add_custom_filament(1, 2, 65, colors);
    manager.mixed_filaments().back().distribution_mode = int(MixedFilament::LayerCycle);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("layer_height", .12);
    config.set("initial_layer_print_height", .20);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("dithering_local_z_mode", true);
    config.set("dithering_local_z_whole_objects", false);
    config.set("dithering_local_z_direct_multicolor", independent_heights);
    config.set("dithering_local_z_independent_layer_height", independent_heights);
    config.option<ConfigOptionStrings>("filament_colour")->values  = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {.4, .4, .4};
    config.option<ConfigOptionFloats>("max_layer_height")->values  = {.3, .3, .3};
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                            "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));
    // Initialize enum serialization as preset loading does, for G-code export.
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() != coEnums)
            continue;
        auto* initialized = config.def()->get(key)->create_default_option();
        initialized->set(config.option(key));
        config.set_key_value(key, initialized);
    }

    Model model;
    auto* object = model.add_object();
    object->name = "sml-original";
    object->config.set("extruder", 3);
    auto*            volume = object->add_volume(make_cube(8., 8., 3.));
    TriangleSelector selector(volume->mesh());
    const auto&      mesh = volume->mesh().its;
    for (size_t facet = 0; facet < mesh.indices.size(); ++facet) {
        const auto& triangle = mesh.indices[facet];
        const float center_x = (mesh.vertices[triangle[0]].x() + mesh.vertices[triangle[1]].x() + mesh.vertices[triangle[2]].x()) / 3.f;
        if (center_x < 4.f)
            selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(4));
    }
    volume->mmu_segmentation_facets.set(selector);
    object->add_instance()->set_offset(Vec3d(20., 20., 0.));
    object->ensure_on_bed();

    Print print;
    print.set_status_silent();
    if (paste_after_slicing) {
        print.apply(model, config);
        print.process();
        REQUIRE_FALSE(print.get_object(0)->local_z_sublayer_plan().empty());
    }

    // Like clipboard paste, clone the painted mesh into a separate ModelObject.
    Model clipboard;
    auto* copied = clipboard.add_object(*object);
    auto* pasted = model.add_object(*copied);
    pasted->name = "sml-pasted";
    pasted->instances.front()->set_offset(Vec3d(40., 20., 0.));
    print.apply(model, config);
    print.process(nullptr, paste_after_slicing);
    REQUIRE(print.objects().size() == 2);
    auto* original_print = print.get_object(0);
    auto* pasted_print   = print.get_object(1);
    REQUIRE(pasted_print->get_shared_object() == original_print);
    REQUIRE_FALSE(original_print->local_z_sublayer_plan().empty());
    REQUIRE(pasted_print->local_z_intervals().size() == original_print->local_z_intervals().size());
    REQUIRE(pasted_print->local_z_sublayer_plan().size() == original_print->local_z_sublayer_plan().size());
    for (size_t i = 0; i < original_print->local_z_sublayer_plan().size(); ++i) {
        const auto& original = original_print->local_z_sublayer_plan()[i];
        const auto& copy     = pasted_print->local_z_sublayer_plan()[i];
        CHECK(copy.layer_id == original.layer_id);
        CHECK(copy.print_z == original.print_z);
        CHECK(copy.flow_height == original.flow_height);
        CHECK(copy.painted_masks_by_extruder == original.painted_masks_by_extruder);
    }

    const auto           output = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("sml-copy-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    const auto           path = print.export_gcode(output.string(), &result, nullptr);
    boost::filesystem::remove(path);
    // Verify that both objects actually emit the two mixed components at their
    // subdivided heights, rather than merely retaining paint or nominal layers.
    std::array<std::array<size_t, 2>, 2> subdivided_moves{};
    const double                         expected_heights[] = {.06, independent_heights ? .06 * 65 / 35 : .06};
    for (const auto& move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extruder_id >= 2 || move.position.z() < .5f || move.position.z() > 2.5f)
            continue;
        if (std::abs(move.height - expected_heights[move.extruder_id]) < 1e-5)
            ++subdivided_moves[move.position.x() < 34.f ? 0 : 1][move.extruder_id];
    }
    for (const auto& counts : subdivided_moves)
        for (size_t count : counts)
            CHECK(count > 0);

    // Detaching shared geometry must also invalidate its associated SML plan.
    pasted_print->clear_shared_object();
    CHECK(pasted_print->local_z_intervals().empty());
    CHECK(pasted_print->local_z_sublayer_plan().empty());
    REQUIRE_FALSE(original_print->local_z_sublayer_plan().empty());
    print.process();
    REQUIRE_FALSE(pasted_print->local_z_sublayer_plan().empty());

    config.set("dithering_local_z_mode", false);
    print.apply(model, config);
    print.process();
    for (const auto* printed : print.objects()) {
        CHECK(printed->local_z_intervals().empty());
        CHECK(printed->local_z_sublayer_plan().empty());
    }
}
