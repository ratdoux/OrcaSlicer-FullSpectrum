#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ImageMap/VolumeData.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <limits>

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("Persistent image maps assign normal mixes and modulate the V2 slice envelope", "[PrintObject][ImageMap]")
{
    ImageMap::RenderMode render_mode;
    double               expected_size_mm;
    double               expected_first_layer_size_mm;
    SECTION("normal mixed filaments are assigned without changing the envelope")
    {
        render_mode      = ImageMap::RenderMode::NormalMix;
        expected_size_mm = 20.0;
        expected_first_layer_size_mm = 20.0;
    }
    SECTION("V2 modulation changes the shared wall and support envelope")
    {
        render_mode      = ImageMap::RenderMode::PerimeterModulationV2;
        expected_size_mm = 19.8;
        expected_first_layer_size_mm = 20.2;
    }
    SECTION("adaptive localized cycles modulate their local cycle envelope")
    {
        render_mode                  = ImageMap::RenderMode::AdaptiveLocalizedCycles;
        expected_size_mm             = 19.8;
        expected_first_layer_size_mm = 20.2;
    }

    Model model;
    ModelObject *model_object = model.add_object();
    ModelVolume *volume = model_object->add_volume(Test::mesh(TestMesh::cube_20x20x20));
    model_object->add_instance();
    model_object->ensure_on_bed();

    MixedFilamentDefinition definition;
    definition.identity.stable_id = 424242;
    definition.source.kind = MixedFilamentSourceKind::Custom;
    definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components = {{{1}, 50}, {{2}, 50}};
    definition.behavior.distribution = MixedFilamentDistributionMode::LayerCycle;
    definition.behavior.surface_bias.perimeter_modulation = true;
    set_mixed_filament_component_surface_offsets(definition, {0.2f, 0.2f});
    definition.presentation.display_color = "#808000";
    MixedFilamentManager definitions;
    REQUIRE(definitions.add_custom_filament_definition(definition, {"#FF0000", "#00FF00"}));

    ImageMap::VolumeData image_map;
    image_map.topology_fingerprint = ImageMap::topology_fingerprint(volume->mesh());
    ImageMap::Zone zone;
    zone.stable_id = "image-map-zone";
    zone.render_mode = render_mode;
    zone.modulation_sample_spacing_mm = 0.25f;
    zone.corner_smoothing_radius_mm = 0.6f;
    zone.palette.push_back({RGBA{1.f, 0.f, 0.f, 1.f}, 424242, 3});
    image_map.zones.push_back(zone);
    for (size_t triangle_index = 0; triangle_index < volume->mesh().its.indices.size(); ++triangle_index) {
        ImageMap::TriangleBinding binding;
        binding.triangle_index = uint32_t(triangle_index);
        binding.source.kind = ImageMap::SourceKind::FaceColor;
        binding.source.corner_colors = {RGBA{1.f, 0.f, 0.f, 1.f}, RGBA{1.f, 0.f, 0.f, 1.f}, RGBA{1.f, 0.f, 0.f, 1.f}};
        image_map.triangle_bindings.push_back(binding);
    }
    REQUIRE(image_map.validate(volume->mesh()).valid);
    REQUIRE(volume->set_image_map_data(std::move(image_map)));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("elefant_foot_compensation", new ConfigOptionFloat(0.2));
    config.set_key_value("elefant_foot_compensation_layers", new ConfigOptionInt(1));
    config.set_key_value("mixed_filament_component_bias_enabled", new ConfigOptionBool(true));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));
    Print print;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    REQUIRE(print.objects().size() == 1);
    print.get_object(0)->slice();

    const PrintObject &print_object = *print.objects().front();
    REQUIRE(print_object.layer_count() > 2);
    const Layer *layer = print_object.get_layer(1);
    REQUIRE(layer != nullptr);
    REQUIRE_FALSE(layer->lslices.empty());
    const BoundingBox bounds = get_extents(layer->lslices);
    CHECK(unscale<double>(bounds.size().x()) == Approx(expected_size_mm).margin(0.04));
    CHECK(unscale<double>(bounds.size().y()) == Approx(expected_size_mm).margin(0.04));

    const Layer *first_layer = print_object.get_layer(0);
    REQUIRE(first_layer != nullptr);
    REQUIRE_FALSE(first_layer->lslices.empty());
    const BoundingBox first_layer_bounds = get_extents(first_layer->lslices);
    CHECK(unscale<double>(first_layer_bounds.size().x()) == Approx(expected_first_layer_size_mm).margin(0.04));
    CHECK(unscale<double>(first_layer_bounds.size().y()) == Approx(expected_first_layer_size_mm).margin(0.04));

    ExPolygons region_geometry;
    bool       mapped_to_mixed_filament = false;
    for (const LayerRegion *region : layer->regions()) {
        append(region_geometry, to_expolygons(region->slices.surfaces));
        mapped_to_mixed_filament |= !region->slices.empty() && region->region().config().wall_filament.value == 3;
    }
    CHECK(mapped_to_mixed_filament);
    REQUIRE_FALSE(region_geometry.empty());
    const BoundingBox region_bounds = get_extents(union_ex(std::move(region_geometry)));
    CHECK(unscale<double>(region_bounds.min.x()) == Approx(unscale<double>(bounds.min.x())).margin(0.002));
    CHECK(unscale<double>(region_bounds.min.y()) == Approx(unscale<double>(bounds.min.y())).margin(0.002));
    CHECK(unscale<double>(region_bounds.max.x()) == Approx(unscale<double>(bounds.max.x())).margin(0.002));
    CHECK(unscale<double>(region_bounds.max.y()) == Approx(unscale<double>(bounds.max.y())).margin(0.002));
}

TEST_CASE("Local-Z simple multicolor mixes subdivide walls and infill with every component", "[PrintObject][MixedFilament][LocalZ]")
{
    MixedFilamentDefinition definition;
    definition.identity.stable_id          = 737373;
    definition.source.kind                 = MixedFilamentSourceKind::Custom;
    definition.recipe.kind                 = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components     = {{{1}, 50}, {{2}, 25}, {{3}, 25}};
    definition.behavior.distribution       = MixedFilamentDistributionMode::Simple;
    definition.presentation.display_color  = "#806040";

    MixedFilamentManager definitions;
    const std::vector<std::string> colors{"#FF0000", "#0000FF", "#FFFF00"};
    REQUIRE(definitions.add_custom_filament_definition(definition, colors));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    // G-code config serialization needs the enum key maps supplied by dynamic preset options.
    for (const std::string &key : config.keys()) {
        const ConfigOption *option = config.option(key);
        if (option->type() != coEnums)
            continue;
        ConfigOption *replacement = print_config_def.get(key)->create_default_option();
        replacement->set(option);
        config.set_key_value(key, replacement);
    }
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75, 1.75}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4}));
    config.set_key_value("wall_filament", new ConfigOptionInt(4));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(20.0));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));
    config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(0.04));
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_preserve_first_layer", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_infill", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_direct_multicolor", new ConfigOptionBool(false));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));

    Model model;
    ModelObject *model_object = model.add_object();
    model_object->add_volume(make_cube(20., 20., 4.));
    model_object->add_instance();
    model_object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    const boost::filesystem::path gcode_path =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("local-z-%%%%-%%%%-%%%%.gcode");
    GCodeProcessorResult processor_result;
    print.export_gcode(gcode_path.string(), &processor_result, nullptr);
    std::ifstream gcode_stream(gcode_path.string());
    const std::string gcode((std::istreambuf_iterator<char>(gcode_stream)), std::istreambuf_iterator<char>());
    gcode_stream.close();
    boost::filesystem::remove(gcode_path);

    REQUIRE(print.objects().size() == 1);
    const std::vector<SubLayerPlan> &plans = print.objects().front()->local_z_sublayer_plan();
    REQUIRE_FALSE(plans.empty());

    std::array<bool, 3> component_seen{false, false, false};
    for (const SubLayerPlan &plan : plans)
        for (size_t component_idx = 0;
             component_idx < component_seen.size() && component_idx < plan.painted_masks_by_extruder.size();
             ++component_idx)
            component_seen[component_idx] = component_seen[component_idx] ||
                                             !plan.painted_masks_by_extruder[component_idx].empty();
    CHECK(component_seen[0]);
    CHECK(component_seen[1]);
    CHECK(component_seen[2]);

    bool   local_z_infill_seen = false;
    size_t section_begin       = 0;
    while ((section_begin = gcode.find("; local-z phase-b path passes begin", section_begin)) != std::string::npos) {
        const size_t section_end = gcode.find("; local-z phase-b path passes end", section_begin);
        REQUIRE(section_end != std::string::npos);
        if (gcode.find("; infill", section_begin) < section_end) {
            local_z_infill_seen = true;
            break;
        }
        section_begin = section_end + 1;
    }
    CHECK(local_z_infill_seen);
}

TEST_CASE("Independent direct multicolor Local-Z preserves ratios within printer height limits",
          "[PrintObject][MixedFilament][LocalZ]")
{
    MixedFilamentDefinition definition;
    definition.identity.stable_id         = 747474;
    definition.source.kind                = MixedFilamentSourceKind::Custom;
    definition.recipe.kind                = MixedFilamentRecipeKind::WeightedBlend;
    definition.recipe.blend.components    = {{{1}, 20}, {{2}, 20}, {{3}, 60}};
    definition.behavior.distribution      = MixedFilamentDistributionMode::Simple;
    definition.presentation.display_color = "#808040";

    MixedFilamentDefinition two_component_definition;
    two_component_definition.identity.stable_id         = 747475;
    two_component_definition.source.kind                = MixedFilamentSourceKind::Custom;
    two_component_definition.recipe.kind                = MixedFilamentRecipeKind::WeightedBlend;
    two_component_definition.recipe.blend.components    = {{{1}, 25}, {{3}, 75}};
    two_component_definition.behavior.distribution      = MixedFilamentDistributionMode::Simple;
    two_component_definition.presentation.display_color = "#BF4000";

    MixedFilamentDefinition extreme_ratio_definition;
    extreme_ratio_definition.identity.stable_id         = 747476;
    extreme_ratio_definition.source.kind                = MixedFilamentSourceKind::Custom;
    extreme_ratio_definition.recipe.kind                = MixedFilamentRecipeKind::WeightedBlend;
    extreme_ratio_definition.recipe.blend.components    = {{{1}, 3}, {{3}, 97}};
    extreme_ratio_definition.behavior.distribution      = MixedFilamentDistributionMode::Simple;
    extreme_ratio_definition.presentation.display_color = "#F7E000";

    MixedFilamentManager definitions;
    const std::vector<std::string> colors{"#FF0000", "#0000FF", "#FFFF00"};
    REQUIRE(definitions.add_custom_filament_definition(definition, colors));
    REQUIRE(definitions.add_custom_filament_definition(two_component_definition, colors));
    REQUIRE(definitions.add_custom_filament_definition(extreme_ratio_definition, colors));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    for (const std::string &key : config.keys()) {
        const ConfigOption *option = config.option(key);
        if (option->type() != coEnums)
            continue;
        ConfigOption *replacement = print_config_def.get(key)->create_default_option();
        replacement->set(option);
        config.set_key_value(key, replacement);
    }
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75, 1.75}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4}));
    config.set_key_value("max_layer_height", new ConfigOptionFloats({0.2, 0.2, 0.2}));
    config.set_key_value("line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("initial_layer_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("sparse_infill_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("internal_solid_infill_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("top_surface_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("wall_filament", new ConfigOptionInt(4));
    config.set_key_value("enable_infill_filament_override", new ConfigOptionBool(true));
    config.set_key_value("sparse_infill_filament", new ConfigOptionInt(2));
    config.set_key_value("solid_infill_filament", new ConfigOptionInt(3));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.08));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.08));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(20.0));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));
    config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(0.04));
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_preserve_first_layer", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_infill", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_direct_multicolor", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_independent_layer_height", new ConfigOptionBool(true));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("flush_volumes_matrix",
                         new ConfigOptionFloats({0.0, 100.0, 100.0,
                                                 100.0, 0.0, 100.0,
                                                 100.0, 100.0, 0.0}));
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats({100.0, 100.0, 100.0, 100.0, 100.0, 100.0}));

    Model model;
    ModelObject *model_object = model.add_object();
    ModelVolume *model_volume = model_object->add_volume(make_cube(20., 20., 4.0));
    TriangleSelector selector(model_volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder2);
    for (int facet_idx = 2; facet_idx <= 7; ++facet_idx)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder5);
    selector.set_facet(8, EnforcerBlockerType::Extruder6);
    selector.set_facet(9, EnforcerBlockerType::Extruder6);
    REQUIRE(model_volume->mmu_segmentation_facets.set(selector));
    model_object->add_instance();
    model_object->ensure_on_bed();

    Print print;
    print.is_BBL_printer() = false;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.validate();
    REQUIRE(print.has_wipe_tower());
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 1);
    const PrintObject &print_object = *print.objects().front();
    const std::vector<LocalZInterval> &intervals = print_object.local_z_intervals();
    const std::vector<SubLayerPlan> &plans = print_object.local_z_sublayer_plan();
    REQUIRE(intervals.size() >= 3);
    REQUIRE(plans.size() >= 3);
    CHECK(intervals[0].independent_layer_height);
    CHECK(intervals[1].independent_layer_height);
    CHECK(intervals[2].independent_layer_height);
    CHECK_FALSE(intervals[1].managed_masks.empty());

    std::vector<unsigned int> component_sequence;
    std::vector<double>       height_sequence;
    std::vector<double>       print_z_sequence;
    for (const SubLayerPlan &plan : plans) {
        if (plan.dependency_group != 1)
            continue;
        for (size_t component_idx = 0; component_idx < plan.painted_masks_by_extruder.size(); ++component_idx) {
            if (plan.painted_masks_by_extruder[component_idx].empty())
                continue;
            component_sequence.push_back(unsigned(component_idx + 1));
            height_sequence.push_back(plan.flow_height);
            print_z_sequence.push_back(plan.print_z);
            break;
        }
    }
    REQUIRE(component_sequence.size() >= 3);
    REQUIRE(height_sequence.size() >= 3);
    REQUIRE(print_z_sequence.size() >= 3);
    CHECK(component_sequence[0] == 1);
    CHECK(component_sequence[1] == 2);
    CHECK(component_sequence[2] == 3);
    CHECK(height_sequence[0] == Approx(0.04).margin(1e-6));
    CHECK(height_sequence[1] == Approx(0.04).margin(1e-6));
    CHECK(height_sequence[2] == Approx(0.12).margin(1e-6));
    CHECK(print_z_sequence[0] == Approx(0.04).margin(1e-6));
    CHECK(print_z_sequence[1] == Approx(0.08).margin(1e-6));
    CHECK(print_z_sequence[2] == Approx(0.20).margin(1e-6));

    std::vector<unsigned int> two_component_sequence;
    std::vector<double>       two_component_heights;
    for (const SubLayerPlan &plan : plans) {
        if (plan.dependency_group != 2)
            continue;
        for (size_t component_idx = 0; component_idx < plan.painted_masks_by_extruder.size(); ++component_idx) {
            if (plan.painted_masks_by_extruder[component_idx].empty())
                continue;
            two_component_sequence.push_back(unsigned(component_idx + 1));
            two_component_heights.push_back(plan.flow_height);
            break;
        }
    }
    REQUIRE(two_component_sequence.size() >= 2);
    REQUIRE(two_component_heights.size() >= 2);
    CHECK(two_component_sequence[0] == 1);
    CHECK(two_component_sequence[1] == 3);
    CHECK(two_component_heights[0] == Approx(0.04).margin(1e-6));
    CHECK(two_component_heights[1] == Approx(0.12).margin(1e-6));

    std::vector<unsigned int> extreme_component_sequence;
    std::vector<double>       extreme_height_sequence;
    for (const SubLayerPlan &plan : plans) {
        if (plan.dependency_group != 3)
            continue;
        for (size_t component_idx = 0; component_idx < plan.painted_masks_by_extruder.size(); ++component_idx) {
            if (plan.painted_masks_by_extruder[component_idx].empty())
                continue;
            extreme_component_sequence.push_back(unsigned(component_idx + 1));
            extreme_height_sequence.push_back(plan.flow_height);
            break;
        }
    }
    REQUIRE(extreme_component_sequence.size() >= 9);
    REQUIRE(extreme_height_sequence.size() == extreme_component_sequence.size());
    CHECK(extreme_component_sequence.front() == 1);
    CHECK(extreme_height_sequence.front() == Approx(0.04).margin(1e-6));

    const auto next_small_component =
        std::find(extreme_component_sequence.begin() + 1, extreme_component_sequence.end(), 1);
    REQUIRE(next_small_component != extreme_component_sequence.end());
    const size_t next_small_component_idx =
        size_t(std::distance(extreme_component_sequence.begin(), next_small_component));
    CHECK(next_small_component_idx == 8);

    const double expected_dominant_total_height = 0.04 * 97.0 / 3.0;
    double       dominant_total_height          = 0.0;
    for (size_t pass_idx = 1; pass_idx < next_small_component_idx; ++pass_idx) {
        CHECK(extreme_component_sequence[pass_idx] == 3);
        CHECK(extreme_height_sequence[pass_idx] <= 0.2 + EPSILON);
        CHECK(extreme_height_sequence[pass_idx] ==
              Approx(expected_dominant_total_height / 7.0).margin(1e-6));
        dominant_total_height += extreme_height_sequence[pass_idx];
    }
    CHECK(dominant_total_height == Approx(expected_dominant_total_height).margin(1e-6));

    const boost::filesystem::path gcode_path =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("local-z-independent-%%%%-%%%%.gcode");
    GCodeProcessorResult processor_result;
    print.export_gcode(gcode_path.string(), &processor_result, nullptr);
    std::ifstream gcode_stream(gcode_path.string());
    const std::string gcode((std::istreambuf_iterator<char>(gcode_stream)), std::istreambuf_iterator<char>());
    gcode_stream.close();
    boost::filesystem::remove(gcode_path);
    const WipeTowerData &wipe_tower_data = print.wipe_tower_data();
    REQUIRE_FALSE(wipe_tower_data.tool_changes.empty());
    REQUIRE(wipe_tower_data.local_z_tool_changes.size() >= 2);
    bool planned_single_pass_interval = false;
    for (const std::vector<WipeTower::ToolChangeResult> &layer_toolchanges : wipe_tower_data.local_z_tool_changes) {
        for (const WipeTower::ToolChangeResult &toolchange : layer_toolchanges) {
            for (const LocalZInterval &interval : intervals) {
                if (interval.sublayer_count != 1 || interval.layer_id >= print_object.layers().size())
                    continue;
                if (std::abs(toolchange.print_z - float(print_object.layers()[interval.layer_id]->print_z)) <= float(EPSILON)) {
                    planned_single_pass_interval = true;
                    break;
                }
            }
            if (planned_single_pass_interval)
                break;
        }
        if (planned_single_pass_interval)
            break;
    }
    CHECK(planned_single_pass_interval);
    CHECK(gcode.find("; local-z phase-b path passes begin") != std::string::npos);

    std::array<bool, 3> exact_ratio_height_seen{false, false, false};
    const std::array<double, 3> exact_ratio_heights{0.04, 0.04, 0.12};
    for (const GCodeProcessorResult::MoveVertex &move : processor_result.moves) {
        if (move.type != EMoveType::Extrude || move.extruder_id >= exact_ratio_height_seen.size())
            continue;
        if (std::abs(double(move.height) - exact_ratio_heights[move.extruder_id]) <= 1e-5)
            exact_ratio_height_seen[move.extruder_id] = true;
    }
    CHECK(exact_ratio_height_seen[0]);
    CHECK(exact_ratio_height_seen[1]);
    CHECK(exact_ratio_height_seen[2]);

    size_t section_begin = 0;
    while ((section_begin = gcode.find("; local-z phase-b path passes begin", section_begin)) != std::string::npos) {
        const size_t section_end = gcode.find("; local-z phase-b path passes end", section_begin);
        REQUIRE(section_end != std::string::npos);

        double previous_pass_z = -std::numeric_limits<double>::infinity();
        size_t line_begin = section_begin;
        while (line_begin < section_end) {
            const size_t line_end = std::min(gcode.find('\n', line_begin), section_end);
            const std::string line = gcode.substr(line_begin, line_end - line_begin);
            if (line.find("Local-Z path pass") != std::string::npos) {
                const size_t z_pos = line.find(" Z");
                REQUIRE(z_pos != std::string::npos);
                const double pass_z = std::stod(line.substr(z_pos + 2));
                CHECK(pass_z + EPSILON >= previous_pass_z);
                previous_pass_z = pass_z;
            }
            line_begin = line_end + 1;
        }
        section_begin = section_end + 1;
    }
}

SCENARIO("PrintObject: Perimeter generation", "[PrintObject]") {
    GIVEN("20mm cube and default config") {
        WHEN("make_perimeters() is called")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, { { "fill_density", 0 } });
			const PrintObject &object = *print.objects().front();
			THEN("67 layers exist in the model") {
                REQUIRE(object.layers().size() == 66);
            }
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.entities.size() == 1);
            }
            THEN("Every layer in region 0 has 3 paths in its perimeters list.") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.items_count() == 3);
            }
        }
    }
}

SCENARIO("Print: Skirt generation", "[Print]") {
    GIVEN("20mm cube and default config") {
        WHEN("Skirts is set to 2 loops")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
            	{ "skirt_height", 	1 },
        		{ "skirt_distance", 1 },
        		{ "skirts", 		2 }
            });
            THEN("Skirt Extrusion collection has 2 loops in it") {
                REQUIRE(print.skirt().items_count() == 2);
                REQUIRE(print.skirt().flatten().entities.size() == 2);
            }
        }
    }
}

SCENARIO("Print: Changing number of solid surfaces does not cause all surfaces to become internal.", "[Print]") {
    GIVEN("sliced 20mm cube and config with top_solid_surfaces = 2 and bottom_solid_surfaces = 1") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_deserialize_strict({
			{ "top_solid_layers",		2 },
			{ "bottom_solid_layers",	1 },
			{ "layer_height",			0.25 }, // get a known number of layers
			{ "first_layer_height",		0.25 }
			});
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, config);
        // Precondition: Ensure that the model has 2 solid top layers (39, 38)
        // and one solid bottom layer (0).
		auto test_is_solid_infill = [&print](size_t obj_id, size_t layer_id) {
		    const Layer &layer = *(print.objects().at(obj_id)->get_layer((int)layer_id));
		    // iterate over all of the regions in the layer
		    for (const LayerRegion *region : layer.regions()) {
		        // for each region, iterate over the fill surfaces
		        for (const Surface &surface : region->fill_surfaces.surfaces)
		            CHECK(surface.is_solid());
		    }
		};
        print.process();
        test_is_solid_infill(0,  0); // should be solid
        test_is_solid_infill(0, 79); // should be solid
        test_is_solid_infill(0, 78); // should be solid
        WHEN("Model is re-sliced with top_solid_layers == 3") {
			config.set("top_solid_layers", 3);
			print.apply(model, config);
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(0, 79);
                test_is_solid_infill(0, 78);
                test_is_solid_infill(0, 77);
            }
        }
    }
}

SCENARIO("Print: Brim generation", "[Print]") {
    GIVEN("20mm cube and default config, 1mm first layer width") {
        WHEN("Brim is set to 3mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "first_layer_extrusion_width", 	1 },
	        	{ "brim_width", 					3 }
	        });
            THEN("Brim Extrusion collection has 3 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 3);
            }
        }
        WHEN("Brim is set to 6mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "first_layer_extrusion_width", 	1 },
	        	{ "brim_width", 					6 }
	        });
            THEN("Brim Extrusion collection has 6 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 6);
            }
        }
        WHEN("Brim is set to 6mm, extrusion width 0.5mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "first_layer_extrusion_width", 	1 },
	        	{ "brim_width", 					6 },
	        	{ "first_layer_extrusion_width", 	0.5 }
	        });
			print.process();
            THEN("Brim Extrusion collection has 12 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 14);
            }
        }
    }
}
