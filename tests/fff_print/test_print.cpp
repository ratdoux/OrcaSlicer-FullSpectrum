#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ImageMap/VolumeData.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("Persistent image maps assign normal mixes and modulate the V2 slice envelope", "[PrintObject][ImageMap]")
{
    ImageMap::RenderMode render_mode;
    double               expected_size_mm;
    SECTION("normal mixed filaments are assigned without changing the envelope")
    {
        render_mode      = ImageMap::RenderMode::NormalMix;
        expected_size_mm = 20.0;
    }
    SECTION("V2 modulation changes the shared wall and support envelope")
    {
        render_mode      = ImageMap::RenderMode::PerimeterModulationV2;
        expected_size_mm = 19.6;
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
    CHECK(unscale<double>(first_layer_bounds.size().x()) == Approx(expected_size_mm).margin(0.04));
    CHECK(unscale<double>(first_layer_bounds.size().y()) == Approx(expected_size_mm).margin(0.04));

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
