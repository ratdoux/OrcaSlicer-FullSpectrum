#include "material_band_fixtures.hpp"

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace MaterialBandFixtures {
using namespace Slic3r;

nlohmann::json corpus()
{
    std::ifstream stream(std::string(TEST_DATA_DIR) + "/material_band/fixtures.json");
    if (!stream)
        throw std::runtime_error("Cannot read material band fixture corpus");
    return nlohmann::json::parse(stream);
}

void enable_material_bands(Fixture& fixture)
{
    auto&                                    config = fixture.config;
    const std::map<std::string, std::string> values{{"local_z_engine", "material_bands"},
                                                    {"material_band_stationary_toolchange", "1"},
                                                    {"material_band_tool_sections", "0,0.2,0.01,0.2,2,0.5,2,10,2"},
                                                    {"gcode_flavor", "reprapfirmware"},
                                                    {"wall_generator", "classic"},
                                                    {"enable_arc_fitting", "0"},
                                                    {"enable_prime_tower", "1"},
                                                    {"prime_tower_width", "4.5"},
                                                    {"prime_volume", "1"},
                                                    {"wipe_tower_x", "50"},
                                                    {"wipe_tower_y", "50"},
                                                    {"flush_volumes_matrix", "0,1,1,0"},
                                                    {"flush_multiplier", "1"},
                                                    {"filament_minimal_purge_on_wipe_tower", "0,0"},
                                                    {"single_extruder_multi_material", "1"},
                                                    {"time_lapse_gcode", ""},
                                                    {"change_extrusion_role_gcode", ""},
                                                    {"printing_by_object_gcode", ""},
                                                    {"filament_start_gcode", ";"},
                                                    {"filament_end_gcode", ";"}};
    for (const auto& item : values)
        config.set_deserialize_strict(item.first, item.second);
    config.option<ConfigOptionStrings>("filament_start_gcode")->values = {"", ""};
    config.option<ConfigOptionStrings>("filament_end_gcode")->values   = {"", ""};
}

static DynamicPrintConfig make_config()
{
    auto config = DynamicPrintConfig::full_print_config();
    // Baseline profiles predate the opt-in engine. Keep their exact input keys;
    // production engine tests add the experimental settings explicitly.
    for (const auto& key : config.keys())
        if (key == "local_z_engine" || key.compare(0, 14, "material_band_") == 0)
            config.erase(key);
    // Production project metadata is not part of FullPrintConfig's static keys.
    config.apply(PresetBundle{}.project_config);
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    const auto data = corpus();
    for (const auto& item : data.at("production_overrides").items())
        config.set_deserialize_strict(item.key(), item.value().get<std::string>());
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {.4, .4};
    config.option<ConfigOptionFloats>("max_layer_height")->values  = {.24, .24};
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                            "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));
    // Match preset loading's enum initialization before G-code serialization.
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() != coEnums)
            continue;
        auto* initialized = config.def()->get(key)->create_default_option();
        initialized->set(config.option(key));
        config.set_key_value(key, initialized);
    }
    return config;
}

double target_fraction(const nlohmann::json& manifest, double x_mm)
{
    const double      a      = manifest.at("fraction_a").at(0);
    const double      b      = manifest.at("fraction_a").at(1);
    const double      length = manifest.at("dimensions_mm").at(0);
    const std::string field  = manifest.at("field");
    if (field == "linear")
        return a + (b - a) * x_mm / length;
    return field == "step" && x_mm >= length / 2. ? b : a;
}

double permitted_fraction(const nlohmann::json& manifest, double x_mm)
{
    if (manifest.at("boundary_policy") != "allow_transition")
        return target_fraction(manifest, x_mm);
    const double width  = manifest.at("transition_width_mm");
    const double length = manifest.at("dimensions_mm").at(0);
    const double a      = manifest.at("fraction_a").at(0);
    const double b      = manifest.at("fraction_a").at(1);
    return a + (b - a) * std::clamp((x_mm - (length - width) / 2.) / width, 0., 1.);
}

Fixture make_fixture(const std::string& id)
{
    Fixture    fixture;
    const auto data = corpus();
    for (const auto& candidate : data.at("fixtures")) {
        if (candidate.at("id") == id) {
            fixture.manifest = candidate;
            break;
        }
    }
    if (fixture.manifest.is_null())
        throw std::runtime_error("Unknown material band fixture: " + id);
    fixture.config             = make_config();
    const auto&         spec   = fixture.manifest;
    const double        length = spec.at("dimensions_mm").at(0);
    const double        width  = spec.at("dimensions_mm").at(1);
    const double        height = spec.at("dimensions_mm").at(2);
    const size_t        strips = spec.at("mesh_strips");
    std::vector<double> stations{0.};
    for (size_t i = 1; i < strips; ++i)
        stations.push_back(spec.at("field") == "linear" ? length * (double(i) - .5) / (strips - 1) : length * i / strips);
    stations.push_back(length);

    indexed_triangle_set mesh;
    for (double x : stations)
        for (const Vec2d yz : {Vec2d(0., 0.), Vec2d(width, 0.), Vec2d(width, height), Vec2d(0., height)})
            mesh.vertices.emplace_back(float(x), float(yz.x()), float(yz.y()));
    // Each ring has four vertices. Shared ring indices make a closed conforming
    // mesh; no coincident internal box faces are used to fake a painted object.
    mesh.indices.emplace_back(0, 2, 1);
    mesh.indices.emplace_back(0, 3, 2);
    const int last = int(4 * strips);
    mesh.indices.emplace_back(last, last + 1, last + 2);
    mesh.indices.emplace_back(last, last + 2, last + 3);
    for (size_t strip = 0; strip < strips; ++strip) {
        const int a = int(4 * strip);
        const int b = a + 4;
        for (int edge = 0; edge < 4; ++edge) {
            const int next = (edge + 1) % 4;
            mesh.indices.emplace_back(a + edge, a + next, b + next);
            mesh.indices.emplace_back(a + edge, b + next, b + edge);
        }
    }

    std::vector<MixedFilamentDefinition> recipes(strips);
    for (size_t i = 0; i < strips; ++i) {
        auto& recipe                   = recipes[i];
        recipe.identity.stable_id      = 10001 + i;
        recipe.source.kind             = MixedFilamentSourceKind::Custom;
        const int a                    = spec.at("field") == "linear" ? 25 + int(i) :
                                                                        int(std::lround(100. * target_fraction(spec, (stations[i] + stations[i + 1]) / 2.)));
        recipe.recipe.blend.components = {{{1}, a}, {{2}, 100 - a}};
        recipe.behavior.distribution   = MixedFilamentDistributionMode::LayerCycle;
        recipe.behavior.layer_cadence  = {1, 1};
    }
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions(recipes, {"#FF0000", "#0000FF"});
    fixture.config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    auto* object = fixture.model.add_object();
    object->name = id;
    object->config.set("extruder", id == "MB01" ? 3 : 1);
    auto*            volume = object->add_volume(TriangleMesh(std::move(mesh)), ModelVolumeType::MODEL_PART, false);
    TriangleSelector selector(volume->mesh());
    const auto&      its = volume->mesh().its;
    for (size_t facet = 0; facet < its.indices.size(); ++facet) {
        const auto& indices = its.indices[facet];
        const Vec3f center  = (its.vertices[indices[0]] + its.vertices[indices[1]] + its.vertices[indices[2]]) / 3.f;
        // Only vertical faces carry shell paint. The top/bottom and unpainted
        // interior retain physical A for MB02/MB03.
        const bool vertical = facet < 4 || ((facet - 4) % 8) / 2 == 1 || ((facet - 4) % 8) / 2 == 3;
        if (id != "MB01" && !vertical)
            continue;
        size_t stripe = std::min(strips - 1, size_t(std::upper_bound(stations.begin(), stations.end(), center.x()) - stations.begin() - 1));
        selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(3 + stripe));
    }
    volume->mmu_segmentation_facets.set(selector);
    object->add_instance()->set_offset(Vec3d(20., 20., 0.));
    object->ensure_on_bed();
    return fixture;
}

Fixture make_axial_fixture(const std::string& shape, unsigned n, double height)
{
    auto  fixture = make_fixture("MB01");
    auto* object  = fixture.model.objects.front();
    object->delete_volume(0);
    const bool           hole   = shape == "annulus" || shape == "hole_plate";
    const bool           square = shape == "hole_plate";
    const double         inner  = shape == "annulus" ? 5. : 2.;
    indexed_triangle_set mesh;
    for (unsigned ring = 0; ring < (hole ? 4u : 2u); ++ring)
        for (unsigned i = 0; i < n; ++i) {
            const double angle = 2. * PI * i / n, x = std::cos(angle), y = std::sin(angle);
            double radius = ring >= 2 ? inner : shape == "cylinder" ? 5. : shape == "frustum" && ring == 1 ? 6. - height * (2. / 4.8) : 6.;
            if (square && ring < 2)
                radius /= std::max(std::abs(x), std::abs(y));
            mesh.vertices.emplace_back(float(radius * x), float(radius * y), float(ring % 2 ? height : 0.));
        }
    for (unsigned i = 0; i < n; ++i) {
        const unsigned j = (i + 1) % n;
        mesh.indices.emplace_back(i, j, n + j);
        mesh.indices.emplace_back(i, n + j, n + i);
        if (hole) {
            mesh.indices.emplace_back(2 * n + i, 3 * n + j, 2 * n + j);
            mesh.indices.emplace_back(2 * n + i, 3 * n + i, 3 * n + j);
            mesh.indices.emplace_back(i, 2 * n + j, j);
            mesh.indices.emplace_back(i, 2 * n + i, 2 * n + j);
            mesh.indices.emplace_back(n + i, n + j, 3 * n + j);
            mesh.indices.emplace_back(n + i, 3 * n + j, 3 * n + i);
        } else if (i > 0 && i + 1 < n) {
            mesh.indices.emplace_back(0, j, i);
            mesh.indices.emplace_back(n, n + i, n + j);
        }
    }
    object->add_volume(TriangleMesh(std::move(mesh)), ModelVolumeType::MODEL_PART, false);
    fixture.config.set("wall_loops", 1);
    fixture.manifest["shape"]                 = shape;
    fixture.manifest["resolution"]            = n;
    fixture.manifest["source_chord_error_mm"] = 6. * (1. - std::cos(PI / n));
    return fixture;
}

Fixture make_feature_off_fixture(const std::string& id)
{
    if (id == "OFF-solid" || id == "OFF-painted") {
        auto fixture                        = make_fixture(id == "OFF-solid" ? "MB01" : "MB02-edge");
        fixture.model.objects.front()->name = id;
        if (id == "OFF-solid") {
            fixture.model.objects.front()->config.set("extruder", 1);
            fixture.model.objects.front()->volumes.front()->mmu_segmentation_facets.reset();
        } else {
            fixture.config.set("dithering_local_z_mode", true);
        }
        return fixture;
    }
    if (id != "OFF-tapered")
        throw std::runtime_error("Unknown feature-off fixture: " + id);
    Fixture fixture;
    fixture.config  = make_config();
    const auto data = corpus();
    for (const auto& spec : data.at("feature_off_cases"))
        if (spec.at("id") == id)
            fixture.manifest = spec;
    const int            sides         = fixture.manifest.at("angular_segments");
    const double         radius_bottom = fixture.manifest.at("radius_bottom_mm");
    const double         radius_top    = fixture.manifest.at("radius_top_mm");
    const double         height        = fixture.manifest.at("height_mm");
    indexed_triangle_set mesh;
    for (int ring = 0; ring < 2; ++ring)
        for (int i = 0; i < sides; ++i) {
            const double angle  = 2. * PI * i / sides;
            const double radius = ring ? radius_top : radius_bottom;
            mesh.vertices.emplace_back(float(radius * std::cos(angle)), float(radius * std::sin(angle)), float(ring * height));
        }
    for (int i = 0; i < sides; ++i) {
        const int next = (i + 1) % sides;
        mesh.indices.emplace_back(i, next, next + sides);
        mesh.indices.emplace_back(i, next + sides, i + sides);
    }
    for (int i = 1; i < sides - 1; ++i) {
        mesh.indices.emplace_back(0, i + 1, i);
        mesh.indices.emplace_back(sides, sides + i, sides + i + 1);
    }
    auto* object = fixture.model.add_object();
    object->name = id;
    object->config.set("extruder", 1);
    object->add_volume(TriangleMesh(std::move(mesh)));
    object->add_instance()->set_offset(Vec3d(40., 40., 0.));
    object->ensure_on_bed();
    return fixture;
}

} // namespace MaterialBandFixtures
