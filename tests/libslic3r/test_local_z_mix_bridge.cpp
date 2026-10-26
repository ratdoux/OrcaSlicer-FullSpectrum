#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LocalZMixBridge.hpp"
#include "libslic3r/LocalZPathRouter.hpp"
#include "libslic3r/LocalZClipper.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <set>

using namespace Slic3r;

TEST_CASE("Captured Local-Z wall domains can be replayed without color partitioning", "[.][LocalZWallDomain]")
{
    const char* input = std::getenv("ORCA_BRIDGE_DOMAIN_INPUT");
    const char* output = std::getenv("ORCA_BRIDGE_DOMAIN_OUTPUT");
    REQUIRE(input);
    REQUIRE(output);
    std::ifstream stream(input);
    REQUIRE(stream.good());
    nlohmann::json document, result = nlohmann::json::array();
    stream >> document;
    auto config = DynamicPrintConfig::full_print_config();
    config.set("layer_height", .08);
    config.set("initial_layer_print_height", .2);
    config.set("wall_loops", 2);
    config.set("detect_thin_wall", false);
    config.set("resolution", .012);
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = PerimeterGeneratorType::Classic;
    config.set_key_value("line_width", new ConfigOptionFloatOrPercent(.42, false));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(.42, false));
    config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(.45, false));
    for (const auto& key : config.keys())
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    Model model;
    auto* object = model.add_object();
    object->add_volume(TriangleMesh(its_make_cube(100., 100., .6)));
    object->add_instance();
    Print print;
    print.apply(model, config);
    print.get_object(0)->slice();
    auto& layer = *print.get_object(0)->get_layer(1);
    for (const auto& item : document) {
        ExPolygons masks;
        for (const auto& rings : item) {
            ExPolygon polygon;
            for (size_t i = 0; i < rings.size(); ++i) {
                Polygon ring;
                for (const auto& point : rings[i])
                    ring.points.emplace_back(scale_(point[0].get<double>()), scale_(point[1].get<double>()));
                if (i == 0)
                    polygon.contour = std::move(ring);
                else
                    polygon.holes.push_back(std::move(ring));
            }
            masks.push_back(std::move(polygon));
        }
        auto domain = union_ex(to_polygons(masks), ClipperLib::pftPositive);
        if (const char* close = std::getenv("ORCA_BRIDGE_DOMAIN_CLOSE"))
            domain = closing_ex(domain, scale_(std::stod(close)));
        const auto paths = LocalZMixBridgePaths::build(layer, domain, 1);
        auto lines = nlohmann::json::array();
        for (const auto& path : paths->perimeters.as_polylines()) {
            auto points = nlohmann::json::array();
            for (const auto& point : path.points)
                points.push_back({unscale<double>(point.x()), unscale<double>(point.y())});
            lines.push_back(std::move(points));
        }
        result.push_back(std::move(lines));
    }
    std::ofstream exported(output);
    REQUIRE(exported.good());
    exported << result.dump();
}

namespace {
ExPolygons rectangle(double x0, double x1)
{
    return {ExPolygon(Points{{scale_(x0), scale_(-4.)}, {scale_(x1), scale_(-4.)}, {scale_(x1), scale_(4.)}, {scale_(x0), scale_(4.)}})};
}

TEST_CASE("Captured Local-Z cycle fitting can be replayed", "[.][LocalZCycleFit]")
{
    set_logging_level(5);
    const char* input  = std::getenv("ORCA_BRIDGE_CYCLE_INPUT");
    const char* output = std::getenv("ORCA_BRIDGE_CYCLE_OUTPUT");
    REQUIRE(input);
    REQUIRE(output);
    std::ifstream stream(input);
    REQUIRE(stream.good());
    const auto document = nlohmann::json::parse(stream);
    auto       polygons = [](const nlohmann::json& data) {
        ExPolygons result;
        for (const auto& rings : data) {
            ExPolygon polygon;
            for (size_t i = 0; i < rings.size(); ++i) {
                Polygon ring;
                for (const auto& point : rings[i])
                    ring.points.emplace_back(scale_(point[0].get<double>()), scale_(point[1].get<double>()));
                if (i == 0)
                    polygon.contour = std::move(ring);
                else
                    polygon.holes.push_back(std::move(ring));
            }
            result.push_back(std::move(polygon));
        }
        return result;
    };
    auto results = nlohmann::json::array();
    for (const auto& item : document) {
        const auto& zone   = item["zones"][0];
        auto        parent = LocalZMixBridge::build(item["lo"], item["hi"], .06, {.32, .32, .32, .32},
                                                    {{polygons(zone["masks"]), zone["heights"].get<std::vector<double>>(),
                                                      zone["tools"].get<std::vector<unsigned>>(), zone["source"]}},
                                                    {}, 1., .3, {1, 2, 3, 4})
                          .front();
        parent.follows_surface     = item["surface"];
        parent.boundary_tolerance  = .1;
        parent.lower_support_masks = union_ex(polygons(item["lower"]));
        std::vector<double> heights;
        for (const auto& layer : item["layers"]) {
            const auto masks = union_ex(intersection_ex(parent.zones.front().masks, union_ex(polygons(layer["masks"]))));
            parent.layer_footprints.push_back({layer["id"], masks, {masks}, layer["slice_z"], layer["height"]});
            heights.push_back(layer["height"]);
        }
        const bool parent_fits = parent.fit_contours(.1, nullptr, .4);
        const auto children    = parent.shorter_cycles(heights, {.32, .32, .32, .32}, {1, 2, 3, 4}, 1., {}, .4);
        results.push_back({{"lo", parent.z_lo}, {"parent_fits", parent_fits}, {"children", children.size()}});
    }
    std::ofstream exported(output);
    REQUIRE(exported.good());
    exported << results.dump(2);
}

TEST_CASE("Disconnected occurrences of the same recipes have independent bridge fields", "[LocalZ][MixBridge]")
{
    auto a = rectangle(-8., 0.);
    auto b = rectangle(0., 8.);
    append(a, rectangle(12., 20.));
    append(b, rectangle(20., 28.));
    auto bridges = LocalZMixBridge::build(.2, .4, .06, {.32, .32}, {{a, {.08, .12}, {1, 2}, 3}, {b, {.12, .08}, {1, 2}, 4}});
    REQUIRE(bridges.size() == 2);
    CHECK(intersection_ex(bridges[0].ownership(0), bridges[1].ownership(0)).empty());
    for (auto& bridge : bridges) {
        REQUIRE(bridge.zones.size() == 2);
        const auto owned = bridge.ownership(0);
        REQUIRE(owned.size() == 1);
        const bool remote          = owned.front().contains(Point::new_scale(16., 0.));
        bridge.follows_surface     = true;
        bridge.lower_support_masks = owned;
        bridge.layer_footprints    = {{1, owned, {}, .25, .1}, {2, remote ? ExPolygons{} : owned, {}, .35, .1}};
        // A disappearing remote fragment fails only its own contour fit.
        CHECK(bridge.fit_contours(.1) == !remote);
        if (!remote) {
            CHECK(bridge.interface_z(Point::new_scale(-4., 0.)) == Approx(.28).margin(1e-6));
            CHECK(bridge.interface_z(Point::new_scale(4., 0.)) == Approx(.32).margin(1e-6));
        }
    }
}

TEST_CASE("Fine local repeats can fit a surface that rejects their coarse parent", "[LocalZ][MixBridge]")
{
    auto bridge = LocalZMixBridge::build(.2, .6, .06, {.32, .32}, {{rectangle(-4.4, 4.4), {.132, .268}, {2, 1}, 3}}, {}, 1., .3, {1, 2})
                      .front();
    bridge.follows_surface     = true;
    bridge.boundary_tolerance  = .1;
    bridge.lower_support_masks = rectangle(-4., 4.);
    for (size_t i = 0; i < 5; ++i) {
        const double extent = 4.04 + .08 * i;
        const auto   masks  = rectangle(-extent, extent);
        bridge.layer_footprints.push_back({i + 1, masks, {masks}, .24 + .08 * i, .08});
    }
    CHECK_FALSE(bridge.fit_contours(.1));
    const auto fine = bridge.shorter_cycles(std::vector<double>(5, .08), {.32, .32}, {1, 2});
    REQUIRE(fine.size() == 2);
    for (const auto& child : fine)
        for (const auto& pass : child.passes)
            CHECK(pass.contour_fitted);
}

TEST_CASE("Same-material overlap retains printable nominal remainders", "[LocalZ][MixBridge]")
{
    const std::vector<ExPolygons> footprints{rectangle(0., 8.), rectangle(0., 7.)};
    const auto                    core = LocalZMixBridge::coalescing_core(footprints, {rectangle(0., 12.), rectangle(0., 12.)}, .45);
    REQUIRE_FALSE(core.empty());
    CHECK(get_extents(core).max.x() <= scale_(6.55));
    for (const auto& footprint : footprints) {
        const auto rest = diff_ex(footprint, core);
        REQUIRE_FALSE(rest.empty());
        CHECK_FALSE(offset_ex(rest, -scale_(.225)).empty());
        CHECK(intersection_ex(rest, core).empty());
        auto restored = rest;
        append(restored, core);
        CHECK(area(diff_ex(footprint, union_ex(restored))) < scale_(.00001) * double(scale_(.00001)));
        CHECK(area(diff_ex(core, footprint)) < scale_(.00001) * double(scale_(.00001)));
    }
    // A narrow run cannot create a stand-alone thick core or lose its ribbon.
    CHECK(LocalZMixBridge::coalescing_core({rectangle(0., .6), rectangle(0., .4)}, {rectangle(0., 12.), rectangle(0., 12.)}, .45).empty());
    const ExPolygons tail{
        ExPolygon(Points{Point::new_scale(3., -.05), Point::new_scale(7., -.05), Point::new_scale(7., .05), Point::new_scale(3., .05)})};
    auto lower = rectangle(-4., 4.), upper = rectangle(-3.5, 4.);
    append(lower, tail);
    append(upper, tail);
    lower = union_ex(lower);
    upper = union_ex(upper);
    // A nonempty erosion of the large body must not hide the narrow tail.
    CHECK(LocalZMixBridge::coalescing_core({lower, upper}, {rectangle(-5., 8.), rectangle(-5., 8.)}, .45).empty());
}

TEST_CASE("An unprintable fitted island cannot consume nominal ownership", "[LocalZ][MixBridge]")
{
    const auto masks           = rectangle(0., .3);
    auto       bridge          = LocalZMixBridge::build(.2, .4, .06, {.32, .32}, {{masks, {.1, .1}, {1, 2}, 3}}).front();
    bridge.follows_surface     = true;
    bridge.lower_support_masks = masks;
    bridge.layer_footprints    = {{1, masks, {}, .25, .1}, {2, masks, {}, .35, .1}};
    REQUIRE(bridge.fit_contours(.1));
    ExPolygons rejected;
    CHECK_FALSE(bridge.fit_contours(.1, &rejected, .4));
    CHECK(diff_ex(masks, rejected).empty());
    for (const auto& pass : bridge.passes)
        CHECK_FALSE(pass.contour_fitted);
}

TEST_CASE("A failed fine contour reports the footprint that must remain in fallback", "[LocalZ][MixBridge][LocalCadence]")
{
    const auto masks           = rectangle(0., .3);
    auto       parent          = LocalZMixBridge::build(.2, .6, .06, {.32, .32}, {{masks, {.132, .268}, {2, 1}, 7}}).front();
    parent.follows_surface     = true;
    parent.boundary_tolerance  = .1;
    parent.lower_support_masks = masks;
    for (size_t i = 0; i < 5; ++i)
        parent.layer_footprints.push_back({i + 1, masks, {}, .24 + .08 * i, .08});
    ExPolygons rejected;
    CHECK(parent.shorter_cycles(std::vector<double>(5, .08), {.32, .32}, {}, 1., {}, .4, &rejected).empty());
    CHECK(diff_ex(masks, rejected).empty());
    CHECK(diff_ex(rejected, masks).empty());
    CHECK(parent.z_lo == Approx(.2));
    CHECK(parent.z_hi == Approx(.6));
    for (const auto& pass : parent.passes)
        CHECK_FALSE(pass.contour_fitted);
}

TEST_CASE("A common-color retry can be pinned and repeated without optional colors", "[LocalZ][MixBridge]")
{
    std::vector<unsigned> preferred;
    SECTION("Anchor remains first without a preferred terminal color") {}
    SECTION("A preferred terminal anchor stays last across the retry") { preferred = {1, 2, 3}; }
    std::vector<LocalZMixBridge::Zone> zones{{rectangle(-4., 0.), {.28, .12}, {1, 2}, 5},
                                             {rectangle(0., 4.), {.16, .12, .12}, {1, 2, 3}, 6}};
    const auto                         cap    = std::vector<double>(3, .32);
    auto                               sparse = LocalZMixBridge::build(.2, .6, .06, cap, zones, {.08, .08, .08, .08, .08}, 1., .3);
    REQUIRE(sparse.size() == 1);
    REQUIRE_FALSE(sparse.front().extra_colors.empty());
    const auto foreign = rectangle(4., 5.);
    CHECK_FALSE(sparse.front().pin_foreign_boundary(foreign, .3));
    auto simple = LocalZMixBridge::build(.2, .6, .06, cap, zones, {.08, .08, .08, .08, .08}, 1., .3, preferred, false);
    REQUIRE(simple.size() == 1);
    auto& bridge = simple.front();
    CHECK_FALSE(bridge.shared_stack);
    CHECK(bridge.extra_colors.empty());
    REQUIRE(bridge.pin_foreign_boundary(foreign, .3));
    bridge.follows_surface     = true;
    bridge.boundary_tolerance  = .1;
    bridge.lower_support_masks = rectangle(-4., 4.);
    for (size_t i = 0; i < 5; ++i)
        bridge.layer_footprints.push_back({i + 1, rectangle(-4., 4.), {}, .24 + .08 * i, .08});
    auto repeats = bridge.shorter_cycles({.08, .08, .08, .08, .08}, cap, preferred, 1., foreign, .4);
    REQUIRE(repeats.size() == 2);
    for (const auto& child : repeats) {
        REQUIRE(child.extra_colors.empty());
        CHECK(child.terminal_anchor == !preferred.empty());
        if (!preferred.empty()) {
            CHECK(child.passes.front().filament_id != 1);
            CHECK(child.passes.back().filament_id == 1);
            CHECK(child.schedule_z(child.passes.back()) == Approx(child.z_hi));
        }
        CHECK(child.z_hi - child.z_lo == Approx(.2));
        auto joined = std::find_if(child.passes.begin(), child.passes.end(),
                                   [](const auto& pass) { return pass.filament_id == 1 && pass.zones.size() == 2; });
        REQUIRE(joined != child.passes.end());
        const auto index = size_t(joined - child.passes.begin());
        const auto left  = child.surfaces(Point::new_scale(-3., 0.), index);
        const auto right = child.surfaces(Point::new_scale(3., 0.), index);
        CHECK(left.second - left.first == Approx(.14));
        CHECK(right.second - right.first == Approx(.08));
        if (!preferred.empty())
            for (const auto& pass : child.passes) {
                const auto point = Point::new_scale(3.99, 0.);
                const auto masks = child.masks(pass);
                if (std::any_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); }))
                    CHECK(child.surfaces(point, &pass - child.passes.data()).second == Approx(child.schedule_z(pass)));
            }
        for (const auto& pass : child.passes)
            for (double x = -3.9; x < 4.; x += .05) {
                const auto point = Point::new_scale(x, 0.);
                const auto masks = child.masks(pass);
                if (std::none_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); }))
                    continue;
                const auto bounds = child.surfaces(point, &pass - child.passes.data());
                CHECK(bounds.second - bounds.first >= .06 - 1e-6);
                CHECK(bounds.second - bounds.first <= .32 + 1e-6);
                CHECK(bounds.second <= pass.ceiling + 1e-6);
            }
    }
    CHECK(repeats.front().schedule_z(repeats.front().passes.back()) < repeats.back().schedule_z(repeats.back().passes.front()) - 1e-6);
}

TEST_CASE("Local-Z bridge archive diagnosis", "[.][MixBridgeArchive]")
{
    const char* archive = std::getenv("ORCA_BRIDGE_ARCHIVE");
    REQUIRE(archive);
    set_logging_level(5);
    set_temporary_dir(boost::filesystem::path(archive).parent_path().string());
    set_resources_dir((boost::filesystem::current_path() / "resources").string());
    auto config = DynamicPrintConfig::full_print_config();
    for (const auto& key : config.keys())
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    En3mfType                 type = En3mfType::From_Other;
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    Semver                    file_version;
    Model                     model = Model::read_from_archive(archive, &config, &substitutions, type,
                                                               LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances, &plates,
                                                               &presets, &file_version);
    for (auto* plate : plates)
        delete plate;
    for (auto* preset : presets)
        delete preset;
    REQUIRE_FALSE(model.objects.empty());
    if (std::getenv("ORCA_BRIDGE_ARCHIVE_ENABLE_SML"))
        config.set("dithering_local_z_mode", true);
    if (std::getenv("ORCA_BRIDGE_ARCHIVE_MATERIAL_PLANNER"))
        config.option<ConfigOptionEnum<LocalZPlanner>>("dithering_local_z_planner")->value = LocalZPlanner::MaterialV1;
    if (std::getenv("ORCA_BRIDGE_ARCHIVE_FULL_DOMAIN")) {
        config.set("dithering_local_z_whole_objects", true);
        config.set("dithering_local_z_infill", true);
    }
    const bool planar_reference = std::getenv("ORCA_BRIDGE_ARCHIVE_PLANAR_REFERENCE") != nullptr;
    const bool material_planner = config.option<ConfigOptionEnum<LocalZPlanner>>("dithering_local_z_planner")->value == LocalZPlanner::MaterialV1;
    if (planar_reference) {
        config.set("dithering_local_z_mode", false);
        config.set("dithering_local_z_direct_multicolor", false);
        config.set("dithering_local_z_independent_layer_height", false);
    }
    for (const auto& key : config.keys())
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE_FALSE(print.objects().empty());
    for (auto* object : print.objects()) {
        object->slice();
        const auto& plan = object->local_z_sublayer_plan();
        std::cout << "BRIDGE_ARCHIVE object=" << object->model_object()->name << " layers=" << object->layer_count()
                  << " passes=" << plan.size()
                  << " bridge_passes=" << std::count_if(plan.begin(), plan.end(), [](const auto& pass) { return pass.mix_bridge && !pass.mix_bridge->planar_fallback; })
                  << " fallback_passes=" << std::count_if(plan.begin(), plan.end(), [](const auto& pass) { return pass.mix_bridge && pass.mix_bridge->planar_fallback; })
                  << '\n';
    }
    const char* output = std::getenv("ORCA_BRIDGE_ARCHIVE_OUTPUT");
    if (output || std::getenv("ORCA_BRIDGE_ARCHIVE_CHECK_PATHS")) {
        set_logging_level(2);
        std::cout << "BRIDGE_PROCESS_START" << std::endl;
        if (const char* path = std::getenv("ORCA_BRIDGE_ARCHIVE_DUMP")) {
            std::ofstream orphan_dump(std::string(path) + ".owners.jsonl");
            auto          polygons = [](const ExPolygons& masks) {
                auto result = nlohmann::json::array();
                for (const auto& mask : masks) {
                    auto rings = nlohmann::json::array();
                    for (const auto& ring : to_polygons(mask)) {
                        auto points = nlohmann::json::array();
                        for (const auto& point : ring.points)
                            points.push_back({point.x(), point.y()});
                        rings.push_back(std::move(points));
                    }
                    result.push_back(std::move(rings));
                }
                return result;
            };
            for (const auto* object : print.objects())
                for (const auto& plan : object->local_z_sublayer_plan()) {
                    if (!plan.mix_bridge)
                        continue;
                    const auto& bridge = *plan.mix_bridge;
                    const auto& pass   = bridge.passes[plan.mix_bridge_component];
                    const auto  masks  = bridge.masks(pass, plan.layer_id);
                    const auto* layer  = object->get_layer(int(plan.layer_id));
                    if (masks.empty() || std::any_of(layer->regions().begin(), layer->regions().end(), [&](const auto* region) {
                            return !intersection_ex(masks, to_expolygons(region->slices.surfaces)).empty();
                        }))
                        continue;
                    orphan_dump << nlohmann::json{{"layer", plan.layer_id},
                                                  {"z", layer->print_z},
                                                  {"lo", bridge.z_lo},
                                                  {"hi", bridge.z_hi},
                                                  {"band", bridge.nominal_band},
                                                  {"flat", bridge.planar_fallback},
                                                  {"surface", bridge.follows_surface},
                                                  {"tool", pass.filament_id},
                                                  {"owner_region", plan.mix_bridge_region_id},
                                                  {"masks", polygons(masks)},
                                                  {"owned", polygons(bridge.ownership(plan.layer_id))},
                                                  {"actual", polygons(layer->lslices)}}
                                << '\n';
                }
        }
        print.process();
        if (const char* dump_path = std::getenv("ORCA_BRIDGE_ARCHIVE_DUMP")) {
            std::ofstream dump(dump_path);
            auto          polygons = [](const ExPolygons& masks) {
                auto result = nlohmann::json::array();
                for (const auto& mask : masks) {
                    auto rings = nlohmann::json::array();
                    for (const auto& ring : to_polygons(mask)) {
                        auto points = nlohmann::json::array();
                        for (const auto& point : ring.points)
                            points.push_back({unscale<double>(point.x()), unscale<double>(point.y())});
                        rings.push_back(std::move(points));
                    }
                    result.push_back(std::move(rings));
                }
                return result;
            };
            for (const auto* object : print.objects()) {
                for (const auto* layer : object->layers()) {
                    auto regions = nlohmann::json::array();
                    for (const auto* region : layer->regions())
                        regions.push_back({{"tool", region->region().config().wall_filament.value},
                                           {"masks", polygons(to_expolygons(region->slices.surfaces))}});
                    dump << nlohmann::json{{"layer", layer->id()}, {"z", layer->print_z}, {"regions", regions}} << '\n';
                }
                for (const auto& plan : object->local_z_sublayer_plan()) {
                    if (!plan.mix_bridge)
                        continue;
                    const auto& bridge = *plan.mix_bridge;
                    const auto& pass   = bridge.passes[plan.mix_bridge_component];
                    auto        zones  = nlohmann::json::array();
                    for (const auto& zone : bridge.zones)
                        zones.push_back({{"source", zone.source_filament_id},
                                         {"masks", polygons(zone.masks)},
                                         {"heights", zone.heights},
                                         {"tools", zone.component_ids},
                                         {"pure", zone.preserve_nominal_layers}});
                    auto paths = nlohmann::json::array();
                    if (plan.mix_bridge_paths)
                        for (const auto& line : plan.mix_bridge_paths->perimeters.as_polylines()) {
                            auto points = nlohmann::json::array();
                            for (const auto& point : line.points) {
                                const auto [lo, hi] = bridge.surfaces(point, plan.mix_bridge_component);
                                points.push_back({unscale<double>(point.x()), unscale<double>(point.y()), lo, hi});
                            }
                            paths.push_back(std::move(points));
                        }
                    dump << nlohmann::json{{"layer", plan.layer_id},
                                           {"lo", bridge.z_lo},
                                           {"hi", bridge.z_hi},
                                           {"tool", pass.filament_id},
                                           {"band", bridge.nominal_band},
                                           {"flat", bridge.planar_fallback},
                                           {"surface", bridge.follows_surface},
                                           {"component", plan.mix_bridge_component},
                                           {"owned", polygons(bridge.ownership(plan.layer_id))},
                                           {"masks", polygons(bridge.masks(pass, plan.layer_id))},
                                           {"zones", zones},
                                           {"paths", paths}}
                         << '\n';
                }
            }
        }
        size_t island_paths = 0, crossing_islands = 0;
        for (const auto* object : print.objects())
            for (const auto& plan : object->local_z_sublayer_plan())
                if (plan.mix_bridge_paths) {
                    island_paths += plan.mix_bridge_paths->perimeters.entities.size();
                    const auto& bridge = *plan.mix_bridge;
                    const auto& pass   = bridge.passes[plan.mix_bridge_component];
                    if (pass.zones.size() < 2)
                        continue;
                    for (const auto& path : plan.mix_bridge_paths->perimeters.as_polylines()) {
                        if (path.points.empty() || path.points.front() != path.points.back())
                            continue;
                        std::set<unsigned> sources;
                        for (size_t zone : pass.zones) {
                            const auto clipped = intersection_pl(Polylines{path}, bridge.zones[zone].masks);
                            if (std::any_of(clipped.begin(), clipped.end(), [](const auto& part) { return part.length() > scale_(.05); }))
                                sources.insert(bridge.zones[zone].source_filament_id);
                        }
                        crossing_islands += sources.size() > 1;
                    }
                }
        std::cout << "BRIDGE_PATHS island_paths=" << island_paths << " closed_cross_recipe_paths=" << crossing_islands << std::endl;
        if (!planar_reference) {
            REQUIRE(island_paths > 0);
            if (!material_planner)
                REQUIRE(crossing_islands > 0);
        }
        if (!output)
            return;
        GCodeProcessorResult result;
        print.export_gcode(output, &result, nullptr);
        size_t sloped_moves = 0;
        for (size_t i = 1; i < result.moves.size(); ++i)
            if (result.moves[i].type == EMoveType::Extrude &&
                std::abs(result.moves[i].position.z() - result.moves[i - 1].position.z()) > 1e-6)
                ++sloped_moves;
        std::cout << "BRIDGE_EXPORT moves=" << result.moves.size() << " nonplanar_extrusions=" << sloped_moves << '\n';
        bool joined_material_cells = false;
        if (material_planner)
            for (const auto* object : print.objects())
                for (const auto& pass : object->local_z_sublayer_plan()) {
                    REQUIRE(pass.mix_bridge);
                    REQUIRE(pass.mix_bridge->material_planner);
                    joined_material_cells |= !pass.mix_bridge->nominal_band && !pass.mix_bridge->planar_fallback &&
                                             pass.mix_bridge->material_domains.size() > 1;
                }
        if (!planar_reference && (!material_planner || joined_material_cells))
            REQUIRE(sloped_moves > 0);
    }
}

TEST_CASE("Local-Z bridge autosave reproduction", "[.][MixBridgeRepro]")
{
    const char* project  = std::getenv("ORCA_BRIDGE_REPRO_PROJECT");
    const char* settings = std::getenv("ORCA_BRIDGE_REPRO_SETTINGS");
    const char* output   = std::getenv("ORCA_BRIDGE_REPRO_OUTPUT");
    REQUIRE(project);
    REQUIRE(settings);
    REQUIRE(output);
    if (std::getenv("ORCA_BRIDGE_REPRO_VERBOSE"))
        set_logging_level(4);
    Model model;
    auto  config = DynamicPrintConfig::full_print_config();
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    }
    std::ifstream        mesh_file(project);
    const auto           mesh = nlohmann::json::parse(mesh_file);
    indexed_triangle_set its;
    for (const auto& vertex : mesh.at("vertices"))
        its.vertices.emplace_back(vertex[0].get<float>(), vertex[1].get<float>(), vertex[2].get<float>());
    for (const auto& triangle : mesh.at("triangles"))
        its.indices.emplace_back(triangle[0].get<int>(), triangle[1].get<int>(), triangle[2].get<int>());
    auto* object = model.add_object();
    object->config.set("extruder", 1);
    auto* volume = object->add_volume(TriangleMesh(std::move(its)));
    for (size_t i = 0; i < mesh.at("paint").size(); ++i)
        volume->mmu_segmentation_facets.set_triangle_from_string(int(i), mesh["paint"][i].get<std::string>());
    object->add_instance()->set_offset(Vec3d(135.5, 136., 13.5));
    // This is the already resolved runtime configuration, not a preset import.
    // Read known options directly without running GUI preset migrations.
    std::ifstream             settings_file(settings);
    const auto                settings_json = nlohmann::json::parse(settings_file);
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    for (auto it = settings_json.begin(); it != settings_json.end(); ++it) {
        INFO("runtime option: " << it.key());
        const auto* definition = config.def()->get(it.key());
        if (!definition)
            continue;
        std::string value;
        if (it.value().is_string())
            value = it.value().get<std::string>();
        else if (it.value().is_array()) {
            const auto values = it.value().get<std::vector<std::string>>();
            if (definition->type == coStrings)
                value = ConfigOptionStrings(values).serialize();
            else if (definition->is_scalar() && definition->type != coPoint && definition->type != coPoint3 && !values.empty())
                value = values.front();
            else
                for (const auto& item : values) {
                    if (!value.empty())
                        value += ',';
                    value += item;
                }
        } else
            continue;
        config.set_deserialize(it.key(), value, substitutions);
    }
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    const auto process_start = std::chrono::steady_clock::now();
    print.process();
    const double process_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - process_start).count();
    size_t       bridges = 0, owned = 0;
    double       maximum_wall_ahead = 0.;
    for (const auto* object : print.objects()) {
        for (const auto& interval : object->local_z_intervals())
            owned += !interval.mix_bridges.empty();
        for (const auto& plan : object->local_z_sublayer_plan()) {
            bridges += bool(plan.mix_bridge);
            if (plan.mix_bridge)
                maximum_wall_ahead = std::max(maximum_wall_ahead, plan.mix_bridge->passes[plan.mix_bridge_component].ceiling -
                                                                      object->local_z_intervals()[plan.layer_id].z_hi);
        }
    }
    std::ofstream report(std::string(output) + ".txt");
    report << "bridge_passes=" << bridges << " owned_intervals=" << owned << '\n';
    report << "process_seconds=" << process_seconds << '\n';
    GCodeProcessorResult result;
    const auto           export_start = std::chrono::steady_clock::now();
    print.export_gcode(output, &result, nullptr);
    report << "export_seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - export_start).count() << '\n';
    report << "moves=" << result.moves.size() << '\n';
    std::set<int> preview_bands;
    for (const auto& move : result.moves)
        if (move.type == EMoveType::Extrude)
            preview_bands.insert(result.preview_layer_grid.index_at(move.position.z()));
    report << "nominal_preview=" << result.preview_layer_grid.enabled << " preview_bands=" << preview_bands.size() << '\n';
    if (result.preview_layer_grid.enabled) {
        GCodeProcessor reloaded;
        reloaded.process_file(output);
        const auto& imported = reloaded.get_result();
        REQUIRE(imported.preview_layer_grid.enabled);
        CHECK(imported.preview_layer_grid.height == Approx(result.preview_layer_grid.height));
        CHECK(imported.preview_layer_grid.first_height == Approx(result.preview_layer_grid.first_height));
        std::set<int> imported_bands;
        for (const auto& move : imported.moves)
            if (move.type == EMoveType::Extrude)
                imported_bands.insert(imported.preview_layer_grid.index_at(move.position.z()));
        CHECK(imported_bands == preview_bands);
        report << "reloaded_preview_bands=" << imported_bands.size() << '\n';
    }
    report << "maximum_wall_ahead_mm=" << maximum_wall_ahead << '\n';
    std::ofstream paths(std::string(output) + ".walls.csv");
    paths << "run,tool,x0,y0,z0,x1,y1,z1,height\n";
    size_t run = 0;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& move = result.moves[i];
        if (move.type != EMoveType::Extrude || move.extrusion_role != erExternalPerimeter) {
            ++run;
            continue;
        }
        const auto& previous = result.moves[i - 1];
        paths << run << ',' << unsigned(move.extruder_id) << ',' << previous.position.x() << ',' << previous.position.y() << ','
              << previous.position.z() << ',' << move.position.x() << ',' << move.position.y() << ',' << move.position.z() << ','
              << move.height << '\n';
    }
    std::ofstream all_paths(std::string(output) + ".paths.csv");
    all_paths << "run,role,tool,x0,y0,z0,x1,y1,z1,height\n";
    run = 0;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& move     = result.moves[i];
        const auto& previous = result.moves[i - 1];
        if (move.type != EMoveType::Extrude) {
            ++run;
            continue;
        }
        all_paths << run << ',' << unsigned(move.extrusion_role) << ',' << unsigned(move.extruder_id) << ',' << previous.position.x() << ','
                  << previous.position.y() << ',' << previous.position.z() << ',' << move.position.x() << ',' << move.position.y() << ','
                  << move.position.z() << ',' << move.height << '\n';
    }
    if (LocalZMixBridge::enabled_from_environment()) {
        CHECK(bridges > 0);
        CHECK(owned > 0);
    }
}

struct BridgeEnvironment
{
    std::optional<std::string> previous;
    explicit BridgeEnvironment(const char* value)
    {
        if (const char* old = std::getenv("ORCA_LOCAL_Z_MIX_BRIDGES"))
            previous = old;
        set(value);
    }
    ~BridgeEnvironment() { set(previous ? previous->c_str() : nullptr); }
    static void set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("ORCA_LOCAL_Z_MIX_BRIDGES", value ? value : "");
#else
        if (value)
            setenv("ORCA_LOCAL_Z_MIX_BRIDGES", value, 1);
        else
            unsetenv("ORCA_LOCAL_Z_MIX_BRIDGES");
#endif
    }
};
} // namespace

TEST_CASE("Mix bridge interfaces preserve bounded complementary stacks", "[LocalZ][MixBridge]")
{
    auto groups = LocalZMixBridge::build(1., 1.2, .06, {.3, .3},
                                         {{rectangle(-4., 0.), {.14, .06}, {1, 2}, 3}, {rectangle(0., 4.), {.06, .14}, {1, 2}, 4}});
    REQUIRE(groups.size() == 1);
    const auto&   bridge = groups.front();
    ExtrusionPath path(erExternalPerimeter, .09, .45f, .2f);
    path.polyline = Polyline(Points{{scale_(-3.), 0.}, {scale_(3.), 0.}});
    SECTION("Open wall") {}
    SECTION("Closed wall with two boundaries")
    {
        path.polyline.points = {{scale_(-3.), scale_(-2.)},
                                {scale_(3.), scale_(-2.)},
                                {scale_(3.), scale_(2.)},
                                {scale_(-3.), scale_(2.)},
                                {scale_(-3.), scale_(-2.)}};
    }
    const auto first  = bridge.modulate(path, 0, .7);
    const auto second = bridge.modulate(path, 1, .7);
    REQUIRE(first.size() > 10);
    REQUIRE(first.size() == second.size());
    CHECK(bridge.interface_z(Point(scale_(-2.), 0.)) == Approx(1.14));
    CHECK(bridge.interface_z(Point(scale_(2.), 0.)) == Approx(1.06));
    CHECK(bridge.interface_z(Point(0, 0)) == Approx(1.10));
    double total_volume = 0.;
    for (size_t i = 0; i < first.size(); ++i) {
        const auto&  a      = first[i];
        const auto&  b      = second[i];
        const double length = unscale<double>(a.polyline.length());
        REQUIRE(a.polyline.points == b.polyline.points);
        CHECK(a.height >= .06 - 1e-6);
        CHECK(a.height <= .14 + 1e-6);
        CHECK(b.height >= .06 - 1e-6);
        CHECK(b.height <= .14 + 1e-6);
        CHECK(a.height + b.height == Approx(.2));
        CHECK(b.absolute_z_begin == Approx(1.9));
        CHECK(b.absolute_z_end == Approx(1.9));
        CHECK(.5 * (a.absolute_z_begin + a.absolute_z_end) + b.height == Approx(1.9));
        CHECK(std::abs(a.absolute_z_end - a.absolute_z_begin) <= bridge.maximum_slope * length + 1e-6);
        total_volume += length * (a.mm3_per_mm + b.mm3_per_mm);
        if (i > 0) {
            CHECK(a.polyline.first_point() == first[i - 1].polyline.last_point());
            CHECK(a.absolute_z_begin == Approx(first[i - 1].absolute_z_end));
        }
    }
    CHECK(total_volume == Approx(unscale<double>(path.polyline.length()) * path.mm3_per_mm).epsilon(1e-6));
    std::reverse(path.polyline.points.begin(), path.polyline.points.end());
    const auto reversed = bridge.modulate(path, 0, .7);
    REQUIRE(reversed.size() == first.size());
    for (size_t i = 0; i < first.size(); ++i) {
        const auto& reverse = reversed[first.size() - 1 - i];
        CHECK(first[i].absolute_z_begin == Approx(reverse.absolute_z_end));
        CHECK(first[i].absolute_z_end == Approx(reverse.absolute_z_begin));
    }
    if (path.polyline.points.front() == path.polyline.points.back()) {
        path.polyline.points.pop_back();
        std::rotate(path.polyline.points.begin(), path.polyline.points.begin() + 1, path.polyline.points.end());
        path.polyline.points.push_back(path.polyline.points.front());
        const auto shifted = bridge.modulate(path, 0, .7);
        REQUIRE_FALSE(shifted.empty());
        CHECK(shifted.front().absolute_z_begin == Approx(shifted.back().absolute_z_end));
        for (const auto& segment : shifted) {
            CHECK(segment.absolute_z_begin == Approx(bridge.interface_z(segment.first_point()) + .7));
            CHECK(segment.absolute_z_end == Approx(bridge.interface_z(segment.last_point()) + .7));
        }
    }
}

TEST_CASE("A solid neighbor joins the final color without changing its nominal layers", "[LocalZ][MixBridge]")
{
    bool middle = false;
    SECTION("Solid is beside the touching mixtures") {}
    SECTION("Solid supplies the path between separated mixtures") { middle = true; }
    auto groups = LocalZMixBridge::build(.2, .44, .08, {.32, .32},
                                         {{rectangle(middle ? -4. : -8., middle ? 0. : -4.), {.24}, {1}, 1},
                                          {rectangle(middle ? -8. : -4., middle ? -4. : 0.), {.12, .12}, {1, 2}, 3},
                                          {rectangle(0., 4.), {.1584, .0816}, {1, 2}, 4}},
                                         {.08, .08, .08});
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE(bridge.zones.size() == 3);
    REQUIRE(bridge.passes.size() == 4);
    CHECK(bridge.passes.back().filament_id == 1);
    CHECK(bridge.passes.back().zones.size() == 3);
    const auto mixed_pass = std::find_if(bridge.passes.begin(), bridge.passes.end(), [](const auto& pass) { return pass.filament_id == 2; });
    REQUIRE(mixed_pass != bridge.passes.end());
    CHECK(mixed_pass->zones.size() == 2);
    const size_t mixed_index = size_t(mixed_pass - bridge.passes.begin());
    const auto solid = std::find_if(bridge.zones.begin(), bridge.zones.end(), [](const auto& zone) { return zone.preserve_nominal_layers; });
    REQUIRE(solid != bridge.zones.end());
    // Follow deposited passes in execution order, including the short pure
    // tongues. Every point must have one contiguous, non-overlapping stack.
    for (int x = -799; x < 400; ++x) {
        if (x == -400 || x == 0)
            continue;
        const Point point(scale_(x / 100.), 0.);
        double      previous = bridge.z_lo;
        for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
            const auto masks = bridge.masks(bridge.passes[pass]);
            if (!std::any_of(masks.begin(), masks.end(), [&](const ExPolygon& p) { return p.contains(point); }))
                continue;
            const auto surfaces = bridge.surfaces(point, pass);
            CHECK(surfaces.first == Approx(previous));
            CHECK(surfaces.second - surfaces.first >= (bridge.passes[pass].tongue >= 0 ? .01 : .08) - 1e-6);
            if (solid->masks.front().contains(point)) {
                CHECK(bridge.passes[pass].filament_id == 1);
                CHECK(surfaces.second - surfaces.first == Approx(.08));
            }
            previous = surfaces.second;
        }
        CHECK(previous == Approx(bridge.z_hi));
    }
}

TEST_CASE("Pure tongues lower the foreign corner and keep a supported stack", "[LocalZ][MixBridge]")
{
    double              height = .24;
    double              clearance = 0.;
    std::vector<double> nominal{.08, .08, .08};
    SECTION("Reported 61:39 and 36:64 mixtures") {}
    SECTION("Nozzle guard retains the entire tongue taper") { clearance = .4; }
    SECTION("Several nominal tongues")
    {
        height = .48;
        nominal.assign(6, .08);
    }
    const auto groups = LocalZMixBridge::build(0., height, .06, {.4, .4},
                                               {{rectangle(-8., -4.), {height}, {1}, 1},
                                                {rectangle(-4., 0.), {height * .61, height * .39}, {1, 2}, 3},
                                                {rectangle(0., 4.), {height * .36, height * .64}, {1, 2}, 4}},
                                               nominal, 1., clearance);
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE_FALSE(bridge.tongue_planes.empty());
    CHECK(bridge.interface_z(Point(scale_(-4.), 0.)) <= height * .39 + .00001);
    for (int x = -799; x < 400; ++x) {
        if (x == -400 || x == 0)
            continue;
        const Point point(scale_(x / 100.), 0.);
        double      previous = 0.;
        for (size_t p = 0; p < bridge.passes.size(); ++p) {
            const auto masks = bridge.masks(bridge.passes[p]);
            if (!std::any_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); }))
                continue;
            const auto [lower, upper] = bridge.surfaces(point, p);
            CHECK(lower == Approx(previous));
            CHECK(upper > lower);
            CHECK(upper <= bridge.passes[p].ceiling + 1e-6);
            CHECK(upper - lower <= .4 + 1e-6);
            if (x < -400) {
                CHECK(bridge.passes[p].filament_id == 1);
                CHECK(upper - lower == Approx(.08));
            }
            previous = upper;
        }
        CHECK(previous == Approx(height));
    }
}

TEST_CASE("A pure neighbor keeps every nominal bead when no join fits", "[LocalZ][MixBridge]")
{
    auto groups = LocalZMixBridge::build(.2, .6, .08, {.21, .21},
                                         {{rectangle(-8., -4.), {.4}, {1}, 1},
                                          {rectangle(-4., 0.), {.2, .2}, {1, 2}, 3},
                                          {rectangle(0., 4.), {.204, .196}, {1, 2}, 4}},
                                         {.12, .16, .12});
    REQUIRE(groups.size() == 1);
    CHECK(groups.front().zones.size() == 3);
    CHECK(groups.front().passes.size() == 5);
    CHECK(groups.front().source_filament_ids == std::vector<unsigned>{3, 4, 1});
    const auto& solid = groups.front().zones.back();
    CHECK(solid.preserve_nominal_layers);
    CHECK_FALSE(solid.join_terminal_pass);
    REQUIRE(groups.front().pin_foreign_boundary(solid.masks, .4));
    CHECK_FALSE(groups.front().advances_near(solid.masks, .4));
    size_t count = 0;
    for (size_t i = 0; i < groups.front().passes.size(); ++i) {
        const auto& pass = groups.front().passes[i];
        if (pass.flat_height <= 0.)
            continue;
        ++count;
        CHECK(pass.filament_id == 1);
        const std::vector<double> heights{.12, .16, .12};
        CHECK(pass.flat_height == Approx(heights[count - 1]));
        const auto surfaces = groups.front().surfaces(Point(scale_(-6.), 0.), i);
        CHECK(surfaces.second - surfaces.first == Approx(pass.flat_height));
    }
    CHECK(count == 3);
}

TEST_CASE("Pure boundaries keep a three-component bridge printable and continuous", "[LocalZ][MixBridge]")
{
    const auto groups = LocalZMixBridge::build(0., .32, .08, {.32, .32, .32},
                                               {{rectangle(-8., -4.), {.32}, {1}, 1},
                                                {rectangle(-4., 0.), {.10, .12, .10}, {1, 2, 3}, 4},
                                                {rectangle(0., 4.), {.09, .09, .14}, {1, 2, 3}, 5}},
                                               {.08, .08, .08, .08});
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE(bridge.zones.size() == 3);
    REQUIRE(bridge.passes.size() == 6);
    REQUIRE(bridge.passes.back().filament_id == 1);
    CHECK(bridge.passes.back().zones.size() == 3);
    for (int x = -800; x < 400; ++x) {
        const Point point(scale_(x / 100.), 0.);
        double      previous = 0.;
        for (size_t component = 0; component < 3; ++component) {
            const double top = component == 2 ? bridge.z_hi : bridge.interface_z(point, component);
            CHECK(top - previous >= .08 - 1e-9);
            CHECK(top - previous <= .16 + 1e-9);
            if (component < 2) {
                CHECK(std::abs(bridge.interface_z(Point(point.x() + scale_(.01), 0.), component) - top) <= .0012 + 1e-9);
                if (x < -400)
                    CHECK(top == Approx(component == 0 ? .08 : .24));
            }
            previous = top;
        }
    }
}

TEST_CASE("Cross-interval interfaces keep continuous Z and bead height", "[LocalZ][MixBridge]")
{
    bool reversed = false, vertex = false, repeated = false, pure = true;
    SECTION("Forward") {}
    SECTION("Reversed") { reversed = true; }
    SECTION("Crossing at an authored vertex") { vertex = true; }
    SECTION("Repeated crossings") { repeated = true; }
    SECTION("Without a pure neighbor") { pure = false; }
    std::vector<LocalZMixBridge::Zone> zones{{rectangle(-4., 0.), {.192, .128}, {1, 2}, 3}, {rectangle(0., 4.), {.1024, .2176}, {1, 2}, 4}};
    if (pure)
        zones.push_back({rectangle(-8., -4.), {.32}, {1}, 1});
    const auto groups = LocalZMixBridge::build(0., .32, .08, {.32, .32}, std::move(zones), {.08, .08, .08, .08});
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE(bridge.passes.size() == (pure ? 5 : 2));
    ExtrusionPath path(erExternalPerimeter, .036, .45f, .08f);
    path.polyline.points = {Point(scale_(-3.), 0.)};
    if (vertex)
        path.polyline.points.emplace_back(0., 0.);
    path.polyline.points.emplace_back(scale_(3.), 0.);
    if (repeated)
        path.polyline.points.emplace_back(scale_(-3.), 0.);
    if (reversed)
        std::reverse(path.polyline.points.begin(), path.polyline.points.end());
    double volume                = 0.;
    bool   crosses_nominal_plane = false;
    for (size_t index = 0; index < bridge.passes.size(); ++index) {
        const auto& pass = bridge.passes[index];
        if (pass.flat_height > 0.)
            continue;
        REQUIRE(pass.zones.size() >= 2);
        const auto segments = bridge.modulate(path, index, .1);
        REQUIRE(segments.size() > 10);
        Point  previous = path.first_point();
        double last_z   = segments.front().absolute_z_begin;
        for (const auto& segment : segments) {
            CHECK(segment.first_point() == previous);
            CHECK(segment.absolute_z_begin == Approx(last_z));
            const double length = unscale<double>(segment.polyline.length());
            CHECK(std::abs(segment.absolute_z_end - segment.absolute_z_begin) <= bridge.maximum_slope * length + 1e-6);
            CHECK(segment.height >= .08 - 1e-6);
            CHECK(segment.height <= .24 + 1e-6);
            CHECK(segment.absolute_z_begin >= bridge.schedule_z(pass) + .1 - 1e-6);
            volume += length * segment.mm3_per_mm;
            previous = segment.last_point();
            last_z   = segment.absolute_z_end;
        }
        CHECK(previous == path.last_point());
        const auto left  = bridge.surfaces(Point(scale_(-.00001), 0.), index);
        const auto right = bridge.surfaces(Point(scale_(.00001), 0.), index);
        CHECK(std::abs(left.first - right.first) <= .000003);
        CHECK(std::abs(left.second - right.second) <= .000003);
        const auto far_left  = bridge.surfaces(Point(scale_(-3.), 0.), index);
        const auto far_right = bridge.surfaces(Point(scale_(3.), 0.), index);
        crosses_nominal_plane |= (far_left.second < .16 && far_right.second > .16) || (far_right.second < .16 && far_left.second > .16);
    }
    CHECK(crosses_nominal_plane);
    CHECK(volume == Approx(6. * .32 * .45 * (repeated ? 2 : 1)));
    if (pure) {
        const size_t final  = bridge.passes.size() - 1;
        const auto   before = bridge.surfaces(Point(scale_(-4.00001), 0.), final);
        const auto   after  = bridge.surfaces(Point(scale_(-3.99999), 0.), final);
        CHECK(before.first == Approx(.24));
        CHECK(before.second - before.first == Approx(.08));
        CHECK(std::abs(before.first - after.first) <= .000002);
        CHECK(std::abs(before.second - after.second) <= .000002);
    }
}

TEST_CASE("Repeated same-material slots preserve volume and pure nominal layers", "[LocalZ][MixBridge]")
{
    unsigned pure_id = 1;
    SECTION("Pure cyan") {}
    SECTION("Pure magenta with a duplicated component") { pure_id = 2; }
    const auto groups = LocalZMixBridge::build(0., .48, .06, {.32, .32},
                                               {{rectangle(-8., -4.), {.48}, {pure_id}, pure_id},
                                                {rectangle(-4., 0.), {.24, .12, .12}, {1, 2, 2}, 3},
                                                {rectangle(0., 4.), {.0672, .2064, .2064}, {1, 2, 2}, 4}},
                                               {.08, .08, .08, .08, .08, .08});
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE(bridge.shared_stack);
    REQUIRE(bridge.passes.size() == 8);
    for (int x = -799; x < 400; ++x) {
        if (x == -400 || x == 0)
            continue;
        const Point                point(scale_(x / 100.), 0.);
        double                     previous = 0.;
        std::map<unsigned, double> volume;
        for (size_t i = 0; i < bridge.passes.size(); ++i) {
            const auto& pass  = bridge.passes[i];
            const auto  masks = bridge.masks(pass);
            if (!std::any_of(masks.begin(), masks.end(), [&](const auto& p) { return p.contains(point); }))
                continue;
            const auto [lo, hi] = bridge.surfaces(point, i);
            CHECK(lo == Approx(previous));
            const bool merging = std::any_of(bridge.split_runs.begin(), bridge.split_runs.end(), [&](const auto& run) {
                return pass.flat_height == 0. && pass.component >= run.first && pass.component < run.last;
            });
            CHECK(hi - lo >= (pass.tongue >= 0 || merging ? .01 : .06) - 1e-8);
            CHECK(hi - lo <= .32 + 1e-8);
            previous = hi;
            volume[pass.filament_id] += hi - lo;
            if (x < -400) {
                CHECK(pass.filament_id == pure_id);
                CHECK(hi - lo == Approx(.08));
            }
        }
        CHECK(previous == Approx(.48));
        if (x == 300) {
            CHECK(volume[1] == Approx(.0672));
            CHECK(volume[2] == Approx(.4128));
        }
        if (x == -300 && pure_id == 1) {
            size_t magenta_passes = 0;
            for (const auto& pass : bridge.passes) {
                const auto masks = bridge.masks(pass);
                magenta_passes += pass.filament_id == 2 &&
                                  std::any_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); });
            }
            CHECK(magenta_passes == 1);
            CHECK(volume[2] == Approx(.24));
        }
    }
}

TEST_CASE("Same-color split runs merge locally and keep support and travel bounds", "[LocalZ][MixBridge]")
{
    std::vector<LocalZMixBridge::Zone> zones;
    double                             top           = .72;
    size_t                             expected_runs = 1, expected_passes = 4;
    SECTION("Three copies become two in the thinner zone")
    {
        zones = {{rectangle(-4., 0.), {.32, .4 / 3, .4 / 3, .4 / 3}, {1, 2, 2, 2}, 3},
                 {rectangle(0., 4.), {.06, .22, .22, .22}, {1, 2, 2, 2}, 4}};
    }
    SECTION("A run that fits everywhere becomes one bead")
    {
        top             = .4;
        expected_passes = 2;
        zones           = {{rectangle(-4., 0.), {.2, .1, .1}, {1, 2, 2}, 3}, {rectangle(0., 4.), {.16, .12, .12}, {1, 2, 2}, 4}};
    }
    SECTION("Infeasible cap retains the bounded uniform split")
    {
        top             = .48;
        expected_runs   = 0;
        expected_passes = 3;
        zones = {{rectangle(-4., 0.), {.168, .156, .156}, {1, 2, 2}, 3}, {rectangle(0., 4.), {.0672, .2064, .2064}, {1, 2, 2}, 4}};
    }
    const auto groups = LocalZMixBridge::build(0., top, .06, {.4, .32}, std::move(zones));
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    CHECK(bridge.split_runs.size() == expected_runs);
    CHECK(bridge.passes.size() == expected_passes);
    for (const auto& run : bridge.split_runs)
        for (size_t i = 1; i < run.masks.size(); ++i)
            CHECK(diff_ex(run.masks[i - 1], run.masks[i]).empty());
    for (int x = -399; x < 400; ++x) {
        if (x == 0)
            continue;
        const Point point(scale_(x / 100.), scale_(.137));
        double      previous = 0.;
        size_t      magenta  = 0;
        for (size_t i = 0; i < bridge.passes.size(); ++i) {
            const auto& pass  = bridge.passes[i];
            const auto  masks = bridge.masks(pass);
            if (!std::any_of(masks.begin(), masks.end(), [&](const auto& p) { return p.contains(point); }))
                continue;
            const auto [lo, hi] = bridge.surfaces(point, i);
            CHECK(lo == Approx(previous).margin(1e-8));
            CHECK(hi - lo >= .01 - 1e-8);
            CHECK(hi - lo <= (pass.filament_id == 1 ? .4 : .32) + 1e-8);
            CHECK(hi <= pass.ceiling + 1e-8);
            CHECK(hi >= bridge.schedule_z(pass) - 1e-8);
            previous = hi;
            magenta += pass.filament_id == 2;
        }
        CHECK(previous == Approx(top));
        if (x == -300)
            CHECK(magenta == (top == .4 ? 1 : 2));
        if (x == 300)
            CHECK(magenta == (top == .4 ? 1 : top == .72 ? 3 : 2));
    }
}

TEST_CASE("Bridge spatial indexes preserve fields on overlapping masks and holes", "[LocalZ][MixBridge]")
{
    auto masks = rectangle(-6., -1.);
    append(masks, rectangle(-3., 1.));
    masks[0].holes.push_back(
        Polygon(Points{{scale_(-5.), scale_(-1.)}, {scale_(-5.), scale_(1.)}, {scale_(-4.), scale_(1.)}, {scale_(-4.), scale_(-1.)}}));
    const auto groups = LocalZMixBridge::build(.2, .4, .06, {.32, .32},
                                               {{masks, {.14, .06}, {1, 2}, 3}, {rectangle(1., 6.), {.06, .14}, {1, 2}, 4}});
    REQUIRE(groups.size() == 1);
    const auto& indexed   = groups.front();
    auto        reference = indexed;
    reference.geometry.reset();
    for (int x = -70; x <= 70; ++x)
        for (int y = -45; y <= 45; ++y) {
            const Point point(scale_(x / 10.), scale_(y / 10.));
            CHECK(indexed.interface_z(point) == Approx(reference.interface_z(point)).margin(1e-6));
        }
}

TEST_CASE("Mix bridge masks keep real gaps and join touching recipes", "[LocalZ][MixBridge]")
{
    double gap = 0.;
    SECTION("Touching") {}
    SECTION("Disconnected") { gap = 1.; }
    ExPolygons masks = rectangle(-4., -gap);
    append(masks, rectangle(gap, 4.));
    masks = union_ex(masks);
    LocalZClipper    clipper(masks);
    LocalZPathRouter router;
    ExtrusionPath    path(erExternalPerimeter, .09, .45f, .2f);
    path.polyline = Polyline(Points{{scale_(-3.), 0.}, {scale_(3.), 0.}});
    std::vector<ExtrusionPath> paths;
    router.append(path, &clipper, nullptr, .2, paths);
    REQUIRE(paths.size() == (gap == 0. ? 1 : 2));
    double length = 0.;
    for (const auto& item : paths)
        length += unscale<double>(item.polyline.length());
    CHECK(length == Approx(6. - 2. * gap));
}

TEST_CASE("Mix bridge plateaus do not inflate straight wall move counts", "[LocalZ][MixBridge]")
{
    const auto    groups = LocalZMixBridge::build(0., .2, .06, {.3, .3}, {{rectangle(-4., 4.), {.1, .1}, {1, 2}, 3}});
    const auto&   bridge = groups.front();
    ExtrusionPath path(erExternalPerimeter, .09, .45f, .2f);
    path.polyline = Polyline(Points{{scale_(-3.), 0.}, {scale_(3.), 0.}});
    for (size_t component = 0; component < 2; ++component) {
        const auto segments = bridge.modulate(path, component, 0.);
        REQUIRE(segments.size() == 1);
        CHECK(segments.front().polyline.points == path.polyline.points);
        CHECK(segments.front().height == Approx(.1));
    }
}

TEST_CASE("Multi-component interfaces stay ordered at a three-zone junction", "[LocalZ][MixBridge]")
{
    ExPolygons upper{ExPolygon(Points{{0., 0.}, {scale_(4.), 0.}, {scale_(4.), scale_(4.)}, {0., scale_(4.)}})};
    ExPolygons lower{ExPolygon(Points{{0., scale_(-4.)}, {scale_(4.), scale_(-4.)}, {scale_(4.), 0.}, {0., 0.}})};
    const auto groups = LocalZMixBridge::build(1., 1.4, .06, {.2, .2, .2, .2},
                                               {{rectangle(-4., 0.), {.16, .08, .10, .06}, {1, 2, 3, 4}, 5},
                                                {upper, {.06, .15, .08, .11}, {1, 2, 3, 4}, 6},
                                                {lower, {.08, .06, .10, .16}, {1, 2, 3, 4}, 7}});
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE(bridge.shared_stack);
    REQUIRE(bridge.passes.size() == 4);
    for (int x = -20; x <= 20; ++x) {
        for (int y = -10; y <= 10; ++y) {
            const Point point(scale_(x * .1), scale_(y * .1));
            double      previous = bridge.z_lo;
            for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
                const auto surfaces = bridge.surfaces(point, pass);
                CHECK(surfaces.first == Approx(previous));
                CHECK(surfaces.second - surfaces.first >= .06 - 1e-9);
                CHECK(surfaces.second - surfaces.first <= .16 + 1e-9);
                const auto next = bridge.surfaces(Point(point.x() + scale_(.023), point.y() + scale_(.031)), pass);
                CHECK(std::abs(surfaces.second - next.second) <= bridge.maximum_slope * std::hypot(.023, .031) + 1e-9);
                previous = surfaces.second;
            }
            CHECK(previous == Approx(bridge.z_hi));
        }
    }
}

TEST_CASE("Unlike component sets share an anchor without thin residual beads", "[LocalZ][MixBridge]")
{
    // The C cap requires at least .07 mm of M on the two-color side.
    // The three-color side can raise its .06 mm M anchor to that boundary
    // target while retaining printable .065 mm C/Y residual beads.
    // Neither common color can carry the whole extra-free sparse stack,
    // so exercise the single-anchor fallback explicitly.
    const auto groups = LocalZMixBridge::build(0., .2, .06, {.13, .10, .3},
                                               {{rectangle(-4., 0.), {.13, .07}, {1, 2}, 4},
                                                {rectangle(0., 4.), {.07, .06, .07}, {1, 2, 3}, 5}});
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE_FALSE(bridge.shared_stack);
    REQUIRE(bridge.passes.size() == 4);
    REQUIRE(bridge.passes.front().filament_id == 2);
    CHECK(bridge.interface_z(Point(scale_(-3.), 0.)) == Approx(.07));
    CHECK(bridge.interface_z(Point(0, 0)) == Approx(.07));
    for (size_t zone = 0; zone < bridge.zones.size(); ++zone) {
        for (int step = 0; step <= 80; ++step) {
            const double x = (zone == 0 ? -4. : 0.) + step * .05;
            const Point  point(scale_(x), 0.);
            double       previous = 0.;
            double       volume   = 0.;
            for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
                const auto& band = bridge.passes[pass];
                if (std::find(band.zones.begin(), band.zones.end(), zone) == band.zones.end())
                    continue;
                const auto surfaces = bridge.surfaces(point, pass);
                CHECK(surfaces.first == Approx(previous));
                const double height = surfaces.second - surfaces.first;
                CHECK(height >= .06 - 1e-9);
                CHECK(height <= .3 + 1e-9);
                CHECK(surfaces.second <= band.ceiling + 1e-9);
                previous = surfaces.second;
                volume += height;
            }
            CHECK(previous == Approx(.2));
            CHECK(volume == Approx(.2));
        }
    }
    // Residual ownership has a butt joint, with no offset overlap and no
    // fictitious three-color material on the two-color side.
    ExtrusionPath path(erExternalPerimeter, .09, .45f, .2f);
    path.polyline = Polyline(Points{{scale_(-3.), 0.}, {scale_(3.), 0.}});
    LocalZPathRouter router;
    double           volume = 0.;
    for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
        ExPolygons masks;
        for (size_t zone : bridge.passes[pass].zones)
            append(masks, bridge.zones[zone].masks);
        masks = union_ex(masks);
        LocalZClipper              clipper(masks);
        std::vector<ExtrusionPath> clipped;
        router.append(path, &clipper, nullptr, .2, clipped);
        for (const auto& piece : clipped)
            for (const auto& segment : bridge.modulate(piece, pass, 0.))
                volume += unscale<double>(segment.polyline.length()) * segment.mm3_per_mm;
    }
    CHECK(volume == Approx(.09 * 6.).epsilon(1e-6));
}

TEST_CASE("Bridge groups require both contact and a shared printable filament", "[LocalZ][MixBridge]")
{
    auto zones = std::vector<LocalZMixBridge::Zone>{{rectangle(-8., -4.), {.13, .07}, {1, 2}, 5},
                                                    {rectangle(-4., 0.), {.07, .13}, {1, 2}, 6},
                                                    {rectangle(2., 6.), {.13, .07}, {3, 4}, 7},
                                                    {rectangle(6., 10.), {.07, .13}, {3, 4}, 8}};
    SECTION("Independent connected groups")
    {
        const auto groups = LocalZMixBridge::build(0., .2, .06, {.3, .3, .3, .3}, zones);
        REQUIRE(groups.size() == 2);
        CHECK(groups[0].source_filament_ids == std::vector<unsigned int>{5, 6});
        CHECK(groups[1].source_filament_ids == std::vector<unsigned int>{7, 8});
    }
    SECTION("Touching disjoint component sets")
    {
        zones.resize(2);
        zones[1].component_ids = {3, 4};
        const auto groups      = LocalZMixBridge::build(0., .2, .06, {.3, .3, .3, .3}, zones);
        REQUIRE(groups.size() == 2);
        CHECK(groups[0].zones.size() == 1);
        CHECK(groups[1].zones.size() == 1);
    }
    SECTION("A shared ID with incompatible height ranges")
    {
        zones.resize(2);
        zones[0].heights       = {.07, .13};
        zones[1].component_ids = {2, 3, 4};
        zones[1].heights       = {.06, .07, .07};
        // The left requires at least .13 mm of filament 2; the right needs
        // at least .12 mm remaining for its two other components.
        // The shared color also cannot carry a .2 mm sparse boundary bead.
        const auto groups = LocalZMixBridge::build(0., .2, .06, {.07, .19, .14, .14}, zones);
        REQUIRE(groups.size() == 2);
    }
}

TEST_CASE("Two three and four color recipes share supported material islands", "[LocalZ][MixBridge][SparseMixBridge]")
{
    bool pure = false;
    SECTION("Mixed regions") {}
    SECTION("Nominal pure cyan neighbor") { pure = true; }
    std::vector<LocalZMixBridge::Zone> zones{{rectangle(-12., 0.), {.20, .20}, {1, 2}, 5},
                                             {rectangle(0., 12.), {.16, .16, .08}, {1, 2, 3}, 6},
                                             {rectangle(12., 24.), {.12, .12, .08, .08}, {1, 2, 3, 4}, 7}};
    REQUIRE(LocalZMixBridge::sparse_stack_fits(zones, .06, {.32, .32, .32, .32}));
    if (pure)
        zones.push_back({rectangle(-24., -12.), {.4}, {1}, 1});
    const auto groups = LocalZMixBridge::build(1., 1.4, .06, {.32, .32, .32, .32}, zones, std::vector<double>(5, .08));
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE(bridge.shared_stack);
    REQUIRE(bridge.extra_colors.size() == 2);
    CHECK(bridge.source_filament_ids.size() == (pure ? 4 : 3));
    for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
        const auto& band  = bridge.passes[pass];
        const auto  masks = bridge.masks(band);
        CHECK_FALSE(masks.empty());
        if (band.extra_color >= 0) {
            const auto allowed = band.filament_id == 3 ? rectangle(0., 24.) : rectangle(12., 24.);
            CHECK(diff_ex(masks, allowed).empty());
        }
    }
    for (int step = -239; step < 480; ++step) {
        const Point                            point(scale_(step * .05), 0.);
        std::vector<std::pair<double, double>> stack;
        std::map<unsigned, double>             thickness;
        for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
            const auto& band  = bridge.passes[pass];
            const auto  masks = bridge.masks(band);
            if (!std::any_of(masks.begin(), masks.end(), [&](const ExPolygon& mask) { return mask.contains(point); }))
                continue;
            const auto   surfaces = bridge.surfaces(point, pass);
            const double h        = surfaces.second - surfaces.first;
            CAPTURE(step, pass, band.filament_id, h);
            CHECK(h >= (band.extra_color >= 0 || band.tongue >= 0 ? .009 : .06) - 1e-6);
            CHECK(h <= .32 + 1e-6);
            CHECK(surfaces.second <= band.ceiling + 1e-6);
            // Tongues wait for the previous pure nominal bead; their sloped
            // tip can be lower than that scheduling plane in the mixed area.
            if (band.tongue < 0)
                CHECK(surfaces.second >= bridge.schedule_z(band) - 1e-6);
            stack.push_back(surfaces);
            thickness[band.filament_id] += h;
        }
        std::sort(stack.begin(), stack.end());
        REQUIRE_FALSE(stack.empty());
        CHECK(stack.front().first == Approx(1.));
        for (size_t i = 1; i < stack.size(); ++i)
            CHECK(stack[i].first == Approx(stack[i - 1].second));
        CHECK(stack.back().second == Approx(1.4));
        if (step < 0)
            CHECK(thickness[3] + thickness[4] == 0.);
        if (step < 240)
            CHECK(thickness[4] == 0.);
        if (step == -120 || step == 120 || step == 360) {
            const size_t zone = step == -120 ? 0 : step == 120 ? 1 : 2;
            for (size_t i = 0; i < zones[zone].heights.size(); ++i)
                CHECK(thickness[zones[zone].component_ids[i]] == Approx(zones[zone].heights[i]).margin(1e-6));
        }
    }
    for (double x : {0., 12.}) {
        for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
            const auto& band = bridge.passes[pass];
            if (band.flat_height > 0. || band.extra_color >= 0)
                continue;
            REQUIRE(band.zones.size() >= 3);
            const auto left  = bridge.surfaces(Point(scale_(x - .0001), 0.), pass);
            const auto right = bridge.surfaces(Point(scale_(x + .0001), 0.), pass);
            CHECK(std::abs(left.first - right.first) <= bridge.maximum_slope * .0002 + 1e-8);
            CHECK(std::abs(left.second - right.second) <= bridge.maximum_slope * .0002 + 1e-8);
        }
    }
}

TEST_CASE("Compact extra-color ramps do not swell the other common color", "[LocalZ][MixBridge][SparseMixBridge]")
{
    bool four = false;
    SECTION("Equal cyan magenta yellow beside pure cyan") {}
    SECTION("Two extras share the slope budget") { four = true; }
    std::vector<LocalZMixBridge::Zone> zones{{rectangle(-12., 0.), {.16, .16}, {1, 2}, 5},
                                             {rectangle(0., 12.), {.1056, .1056, .1088}, {1, 2, 3}, 6},
                                             {rectangle(12., 24.), {.32}, {1}, 1}};
    if (four) {
        zones[1].heights       = {.08, .08, .08, .08};
        zones[1].component_ids = {1, 2, 3, 4};
    }
    const auto groups = LocalZMixBridge::build(0., .32, .06, {.32, .32, .32, .32}, zones, {.08, .08, .08, .08});
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE(bridge.extra_carrier == 1);
    REQUIRE(bridge.maximum_slope == .24);
    size_t magenta = bridge.passes.size(), terminal = bridge.passes.size();
    for (size_t pass = 0; pass < bridge.passes.size(); ++pass)
        if (bridge.passes[pass].filament_id == 2)
            magenta = pass;
        else if (bridge.passes[pass].filament_id == 1 && bridge.passes[pass].flat_height == 0. &&
                 bridge.passes[pass].component + 1 == bridge.zones.front().heights.size())
            terminal = pass;
    REQUIRE(magenta < bridge.passes.size());
    REQUIRE(terminal < bridge.passes.size());
    const double authored         = four ? .08 : .1056;
    double       previous_magenta = authored;
    for (int step = 800; step < 1200; ++step) {
        const Point  point(scale_(step * .01), 0.);
        const auto   m = bridge.surfaces(point, magenta);
        const double h = m.second - m.first;
        CHECK(h >= .08 - 1e-8);
        CHECK(h <= authored + 1e-8);
        CHECK(h <= previous_magenta + 1e-8);
        previous_magenta = h;
        const auto cyan  = bridge.surfaces(point, terminal);
        CHECK(cyan.second - cyan.first <= authored + bridge.tongue_tip_height + 1e-6);
        std::vector<std::pair<double, double>> stack;
        for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
            const auto masks = bridge.masks(bridge.passes[pass]);
            if (!std::any_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); }))
                continue;
            const auto surfaces = bridge.surfaces(point, pass);
            stack.push_back(surfaces);
            const Point next(point.x() + coord_t(scale_(.002)), point.y());
            const auto  other = bridge.surfaces(next, pass);
            // Contour tips use the existing explicit .01 mm exception. Away
            // from that contour, both bead surfaces obey the compact slope.
            if (std::abs((other.second - other.first) - (surfaces.second - surfaces.first)) < .005) {
                CHECK(std::abs(other.second - surfaces.second) <= .24 * .002 + 1e-7);
                CHECK(std::abs(other.first - surfaces.first) <= .24 * .002 + 1e-7);
            }
        }
        std::sort(stack.begin(), stack.end());
        REQUIRE_FALSE(stack.empty());
        CHECK(stack.front().first == Approx(0.));
        for (size_t i = 1; i < stack.size(); ++i)
            CHECK(stack[i].first == Approx(stack[i - 1].second));
        CHECK(stack.back().second == Approx(.32));
    }
    // A point 1 mm (three colors) / 1.4 mm (four) inside the mixed region is
    // already on the authored plateau: about half the former taper width.
    const Point plateau(scale_(12. - (four ? 1.4 : 1.)), 0.);
    for (size_t pass = 0; pass < bridge.passes.size(); ++pass)
        if (bridge.passes[pass].extra_color >= 0) {
            const auto surfaces = bridge.surfaces(plateau, pass);
            CHECK(surfaces.second - surfaces.first == Approx(four ? .08 : .1088).margin(1e-8));
        }
    // The incoming pure layers do not split the authored mixed plateaus or
    // extend to the separate CM/CMY boundary on the far side of this island.
    for (const auto& pass : bridge.passes)
        if (pass.tongue >= 0) {
            const auto masks = bridge.masks(pass);
            for (double x : {-1., 0., 1., 8.})
                CHECK_FALSE(std::any_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(Point(scale_(x), 0.)); }));
        }
}

TEST_CASE("Mix transition length scales ramps without changing recipe plateaus", "[LocalZ][MixBridge][SparseMixBridge]")
{
    const std::vector<LocalZMixBridge::Zone> zones{{rectangle(-12., 0.), {.16, .16}, {1, 2}, 5},
                                                   {rectangle(0., 12.), {.1056, .1056, .1088}, {1, 2, 3}, 6},
                                                   {rectangle(12., 24.), {.32}, {1}, 1}};
    for (double factor : {.5, 1., 2.}) {
        const auto groups = LocalZMixBridge::build(0., .32, .06, {.32, .32, .32}, zones, {.08, .08, .08, .08}, factor);
        REQUIRE(groups.size() == 1);
        const auto& bridge = groups.front();
        CHECK(bridge.maximum_slope == Approx(.24 / factor));
        for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
            if (bridge.passes[pass].extra_color < 0)
                continue;
            const auto ramp = bridge.surfaces(Point(scale_(12. - .5 * factor), 0.), pass);
            CHECK(ramp.second - ramp.first == Approx(.06).margin(1e-8));
            const auto plateau = bridge.surfaces(Point(scale_(6.), 0.), pass);
            CHECK(plateau.second - plateau.first == Approx(.1088).margin(1e-8));
        }
    }
    CHECK_THROWS(LocalZMixBridge::build(0., .32, .06, {.32, .32, .32}, zones, {}, 0.));
}

TEST_CASE("Extra colors do not duplicate a common bead that fits locally", "[LocalZ][MixBridge][SparseMixBridge]")
{
    bool pure = false;
    SECTION("Repeated carrier merges without a pure neighbor") {}
    SECTION("Repeated carrier supports incoming nominal layers") { pure = true; }
    std::vector<LocalZMixBridge::Zone> zones{{rectangle(-12., 0.), {.15, .225, .225}, {1, 2, 2}, 4},
                                             {rectangle(0., 12.), {.24, .12, .12, .12}, {1, 2, 2, 3}, 5}};
    if (pure)
        zones.push_back({rectangle(12., 24.), {.6}, {2}, 2});
    const auto groups = LocalZMixBridge::build(0., .6, .06, {.32, .32, .32}, zones,
                                               pure ? std::vector<double>(6, .1) : std::vector<double>{});
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE(bridge.extra_colors.size() == 1);
    REQUIRE(bridge.split_runs.size() == (pure ? 0 : 1));
    for (double x : {-8., 8.}) {
        const Point point(scale_(x), 0.);
        size_t      beads = 0;
        double      total = 0.;
        for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
            const auto& band = bridge.passes[pass];
            if (band.filament_id != 2)
                continue;
            const auto masks = bridge.masks(band);
            if (!std::any_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); }))
                continue;
            const auto   surfaces = bridge.surfaces(point, pass);
            const double h        = surfaces.second - surfaces.first;
            CHECK(h >= .06 - 1e-6);
            CHECK(h <= .32 + 1e-6);
            CHECK(surfaces.second <= band.ceiling + 1e-6);
            total += h;
            ++beads;
        }
        CHECK(beads == (x < 0. || pure ? 2 : 1));
        CHECK(total == Approx(x < 0. ? .45 : .24));
    }
    if (pure)
        for (int step = 400; step < 1400; ++step) {
            // Sample interiors; closed masks share the zero-area mixed/pure edge.
            const Point                            point(scale_(step * .01 + .005), 0.);
            std::vector<std::pair<double, double>> stack;
            for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
                const auto& band  = bridge.passes[pass];
                const auto  masks = bridge.masks(band);
                if (!std::any_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); }))
                    continue;
                const auto   surfaces = bridge.surfaces(point, pass);
                const double h        = surfaces.second - surfaces.first;
                CHECK(h >= (band.tongue >= 0 || band.extra_color >= 0 ? .009 : .06) - 1e-6);
                CHECK(h <= .32 + 1e-6);
                CHECK(surfaces.second <= band.ceiling + 1e-6);
                stack.push_back(surfaces);
            }
            std::sort(stack.begin(), stack.end());
            REQUIRE_FALSE(stack.empty());
            CHECK(stack.front().first == Approx(0.));
            for (size_t i = 1; i < stack.size(); ++i)
                CHECK(stack[i].first == Approx(stack[i - 1].second));
            CHECK(stack.back().second == Approx(.6));
        }
}

TEST_CASE("Mixed color islands export across two three and four component recipes", "[LocalZ][MixBridge][SparseMixBridge][GCode]")
{
    BridgeEnvironment                    environment("1");
    const std::vector<std::string>       colors{"#00FFFF", "#FF00FF", "#FFFF00", "#000000"};
    std::vector<MixedFilamentDefinition> definitions(3);
    definitions[0].recipe.blend.components = {{{1}, 50}, {{2}, 50}};
    definitions[1].recipe.blend.components = {{{1}, 40}, {{2}, 40}, {{3}, 20}};
    definitions[2].recipe.blend.components = {{{4}, 20}, {{3}, 20}, {{2}, 30}, {{1}, 30}};
    double transition_percent              = 100.;
    SECTION("Different component sets") {}
    SECTION("Shorter transitions from the saved setting") { transition_percent = 50.; }
    SECTION("Shared three-color stack")
    {
        definitions[0].recipe.blend.components = {{{1}, 30}, {{2}, 40}, {{3}, 30}};
        definitions[2].recipe.blend.components = {{{3}, 25}, {{2}, 35}, {{1}, 40}};
    }
    SECTION("Shared four-color stack")
    {
        definitions[0].recipe.blend.components = {{{1}, 30}, {{2}, 30}, {{3}, 20}, {{4}, 20}};
        definitions[1].recipe.blend.components = {{{1}, 20}, {{2}, 20}, {{3}, 30}, {{4}, 30}};
    }
    const bool sparse = definitions[0].recipe.blend.components.size() == 2;
    for (size_t i = 0; i < definitions.size(); ++i) {
        definitions[i].identity.stable_id    = 9801 + i;
        definitions[i].source.kind           = MixedFilamentSourceKind::Custom;
        definitions[i].behavior.distribution = MixedFilamentDistributionMode::LayerCycle;
    }
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions(definitions, colors);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_deserialize_strict("dithering_local_z_mix_transition_length", std::to_string(transition_percent) + "%");
    config.set_num_filaments(4);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("mixed_filament_region_collapse", false);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("layer_height", .08);
    config.set("initial_layer_print_height", .2);
    config.set("wall_loops", 2);
    for (const auto* key : {"dithering_local_z_mode", "dithering_local_z_whole_objects", "dithering_local_z_preserve_first_layer",
                            "dithering_local_z_direct_multicolor", "dithering_local_z_independent_layer_height", "dithering_local_z_infill",
                            "enable_prime_tower"})
        config.set(key, true);
    config.set("single_extruder_multi_material", false);
    config.set("wipe_tower_no_sparse_layers", false);
    config.set("prime_volume", 15.);
    config.set("machine_start_gcode", "G90\nM83\n");
    config.option<ConfigOptionPercent>("sparse_infill_density")->value = 100.;
    config.option<ConfigOptionStrings>("filament_colour")->values      = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values.assign(4, 1.75);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values.assign(4, .4);
    config.option<ConfigOptionFloats>("max_layer_height")->values.assign(4, .32);
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(16, 100.);
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                            "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));
    for (const auto& key : config.keys())
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 1);
    for (int zone = -1; zone < 3; ++zone) {
        auto* volume = object->add_volume(make_cube(zone < 0 ? 4. : 12., 8., 1.8));
        volume->translate(Vec3d(zone < 0 ? -4. : 12. * zone, 0., 0.));
        TriangleSelector selector(volume->mesh());
        for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
            selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(zone < 0 ? 1 : 5 + zone));
        volume->mmu_segmentation_facets.set(selector);
    }
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    size_t extra_passes = 0, bridges = 0;
    for (const auto& plan : print.get_object(0)->local_z_sublayer_plan()) {
        if (!plan.mix_bridge || plan.layer_id == 0)
            continue;
        ++bridges;
        REQUIRE(plan.mix_bridge->shared_stack);
        REQUIRE(plan.mix_bridge->source_filament_ids.size() == 4);
        REQUIRE(plan.mix_bridge_paths);
        CHECK(plan.mix_bridge->maximum_slope == Approx((sparse ? .24 : .12) * 100. / transition_percent));
        const auto& pass = plan.mix_bridge->passes[plan.mix_bridge_component];
        if (pass.extra_color < 0)
            continue;
        ++extra_passes;
        const auto masks   = plan.mix_bridge->masks(pass);
        const auto edge    = diff_ex(masks, offset_ex(masks, -scale_(.45)));
        auto       covered = plan.mix_bridge_paths->perimeters.polygons_covered_by_width();
        append(covered, plan.mix_bridge_paths->fills.polygons_covered_by_width());
        CHECK(area(diff_ex(edge, union_ex(covered))) < .02 * area(edge) + scale_(.01) * scale_(.01));
    }
    REQUIRE(bridges > 0);
    CHECK((extra_passes > 0) == sparse);
    const auto           output = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("multicolor-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(output.string(), &result, nullptr);
    boost::filesystem::remove(output);
    std::array<bool, 4> joined{};
    double              run_min = 1e9, run_max = -1e9;
    unsigned            previous_tool = 99;
    size_t              pure_moves    = 0;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& move     = result.moves[i];
        const auto& previous = result.moves[i - 1];
        if (move.type != EMoveType::Extrude || move.extrusion_role != erExternalPerimeter || move.position.z() <= .201) {
            run_min = 1e9;
            run_max = -1e9;
            continue;
        }
        if (move.position.x() < 99. && previous.position.x() < 99.) {
            ++pure_moves;
            CHECK(move.extruder_id == 0);
            CHECK(move.height == Approx(.08).margin(1e-5));
            CHECK(move.position.z() == Approx(previous.position.z()).margin(1e-5));
        }
        if (std::abs(move.position.y() - 100.225) > .01 || std::abs(previous.position.y() - 100.225) > .01) {
            run_min = 1e9;
            run_max = -1e9;
            continue;
        }
        if (previous_tool != move.extruder_id) {
            run_min = 1e9;
            run_max = -1e9;
        }
        previous_tool         = move.extruder_id;
        run_min               = std::min({run_min, double(move.position.x()), double(previous.position.x())});
        run_max               = std::max({run_max, double(move.position.x()), double(previous.position.x())});
        const double boundary = sparse && move.extruder_id == 2 ? 124. : 112.;
        joined[move.extruder_id] |= run_min < boundary - 1. && run_max > boundary + 1.;
        if (sparse && move.extruder_id >= 2)
            CHECK(std::min(move.position.x(), previous.position.x()) >= (move.extruder_id == 2 ? 112. : 124.));
    }
    REQUIRE(pure_moves > 0);
    CHECK(joined[0]);
    CHECK(joined[1]);
    CHECK(joined[2]);
    if (definitions[0].recipe.blend.components.size() == 4)
        CHECK(joined[3]);
    if (transition_percent != 100.) {
        // Editing only the transition setting must rebuild the island fields.
        config.set_deserialize_strict("dithering_local_z_mix_transition_length", "100%");
        print.apply(model, config);
        print.process();
        for (const auto& plan : print.get_object(0)->local_z_sublayer_plan())
            if (plan.mix_bridge && plan.layer_id > 0)
                CHECK(plan.mix_bridge->maximum_slope == Approx(.24));
    }
}

TEST_CASE("Unlike stacks retain bounded residual islands across nominal planes", "[LocalZ][MixBridge][CurvedMixBridge]")
{
    auto groups = LocalZMixBridge::build(.2, .68, .06, {.32, .32, .32},
                                         {{rectangle(-8., 0.), {.31, .17}, {1, 2}, 4}, {rectangle(0., 8.), {.20, .28}, {1, 3}, 5}},
                                         {.08, .08, .08, .08, .08, .08});
    REQUIRE(groups.size() == 1);
    auto& bridge = groups.front();
    REQUIRE_FALSE(bridge.shared_stack);
    REQUIRE(bridge.pin_foreign_boundary(rectangle(-10., -8.), .3));
    CHECK_FALSE(bridge.advances_near(rectangle(-10., -8.), .3));
    CHECK(bridge.interface_z(Point::new_scale(-7.9, 0.)) == Approx(.4));
    CHECK(bridge.interface_z(Point::new_scale(-5., 0.)) == Approx(.51));
    for (double x = -7.; x <= 7.; x += .03125) {
        const Point point  = Point::new_scale(x, 0.);
        double      height = 0.;
        for (size_t pass = 0; pass < bridge.passes.size(); ++pass) {
            const auto masks = bridge.masks(bridge.passes[pass]);
            if (std::none_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); }))
                continue;
            const auto [bottom, top] = bridge.surfaces(point, pass);
            CHECK(top - bottom >= .06 - 1e-6);
            CHECK(top - bottom <= .32 + 1e-6);
            CHECK(bridge.schedule_z(bridge.passes[pass]) <= top + 1e-6);
            CHECK(bridge.passes[pass].ceiling >= top - 1e-6);
            height += top - bottom;
        }
        // The boundary belongs to both residual masks; sample either side.
        if (std::abs(x) > 1e-9)
            CHECK(height == Approx(.48));
    }
}

TEST_CASE("Fallback blocks carry rare colors without subminimum closing beads", "[LocalZ][MixBridge][CurvedMixBridge]")
{
    std::vector<double> weights{.60, .39, .01}, maximum{.32, .32, .32};
    double height = .24;
    SECTION("One percent component") {}
    SECTION("Four components") { weights = {.10, .15, .05, .70}; maximum.assign(4, .32); }
    SECTION("Narrow equal caps require three closing beads") { weights = {.65, .35}; maximum = {.09, .09}; height = .20; }
    SECTION("Different physical caps") { weights = {.1, .5, .4}; maximum = {.08, .12, .17}; height = .24; }
    std::vector<double> error, deposited(weights.size(), 0.);
    for (size_t cycle = 0; cycle < 1000; ++cycle) {
        const auto layers = LocalZMixBridge::allocate_flat_layers(weights, height, .06, maximum, error);
        double total = 0.;
        for (const auto& [id, h] : layers) {
            REQUIRE(id >= 1);
            REQUIRE(id <= weights.size());
            CHECK(h >= .06 - 1e-6);
            CHECK(h <= maximum[id - 1] + 1e-6);
            deposited[id - 1] += h;
            total += h;
        }
        CHECK(total == Approx(height).margin(1e-6));
        for (size_t i = 0; i < weights.size(); ++i)
            CHECK(std::abs(deposited[i] - (cycle + 1) * height * weights[i]) < .5);
    }
    for (size_t i = 0; i < weights.size(); ++i)
        CHECK(deposited[i] / (1000 * height) == Approx(weights[i]).margin(.002));
}

TEST_CASE("Pure nominal planes can pin a mixed island beside a curved fallback", "[LocalZ][MixBridge][CurvedMixBridge]")
{
    std::vector<LocalZMixBridge::Zone> zones{{rectangle(-8., 0.), {.064, .096}, {1, 2}, 5},
                                             {rectangle(0., 8.), {.0992, .0608}, {1, 2}, 6},
                                             {rectangle(8., 12.), {.16}, {1}, 1}};
    auto                               bridges = LocalZMixBridge::build(.2, .36, .06, {.32, .32}, zones, {.08, .08});
    REQUIRE(bridges.size() == 1);
    auto& bridge = bridges.front();
    REQUIRE(bridge.tongue_planes.empty());
    const auto before = bridge;
    REQUIRE(bridge.pin_foreign_boundary(rectangle(-10., -8.), .3));
    CHECK_FALSE(bridge.advances_near(rectangle(-10., -8.), .3));
    REQUIRE_FALSE(bridge.boundary_interfaces.empty());
    for (double x = -7.99; x < 11.99; x += .03125) {
        const auto point    = Point::new_scale(x, 0.);
        double     previous = .2;
        for (size_t i = 0; i < bridge.passes.size(); ++i) {
            const auto masks = bridge.masks(bridge.passes[i]);
            if (std::none_of(masks.begin(), masks.end(), [&](const auto& m) { return m.contains(point); }))
                continue;
            const auto [lo, hi] = bridge.surfaces(point, i);
            CHECK(lo == Approx(previous));
            CHECK(hi - lo >= .06 - 1e-6);
            CHECK(hi - lo <= .32 + 1e-6);
            CHECK(hi <= bridge.passes[i].ceiling + 1e-6);
            if (x > 8.) {
                CHECK(hi - lo == Approx(.08));
                CHECK(hi == Approx(before.surfaces(point, i).second));
            }
            if (x < -7.7)
                CHECK(hi == Approx(bridge.schedule_z(bridge.passes[i])));
            if (x > -6.5)
                CHECK(hi == Approx(before.surfaces(point, i).second));
            const auto next = bridge.surfaces(Point::new_scale(x + .001, 0.), i);
            CHECK(std::abs(next.second - hi) <= .001 * bridge.maximum_slope + 1e-7);
            previous = hi;
        }
        CHECK(previous == Approx(.36));
    }
}

TEST_CASE("Numerical residual ribbons cannot introduce pure-color transitions", "[LocalZ][MixBridge]")
{
    const auto pure  = rectangle(8., 12.);
    ExPolygons noisy = pure;
    append(noisy, rectangle(-4., -3.999998));
    append(noisy, rectangle(.5, .500004));
    const auto cleaned = LocalZMixBridge::clean_residual(noisy);
    CHECK(diff_ex(cleaned, pure).empty());
    CHECK(diff_ex(pure, cleaned).empty());
    CHECK_FALSE(LocalZMixBridge::clean_residual(rectangle(0., .1)).empty());
    std::vector<LocalZMixBridge::Zone> zones{{rectangle(-8., 0.), {.12, .12}, {1, 2}, 5},
                                             {rectangle(0., 8.), {.08, .16}, {1, 2}, 6},
                                             {noisy, {.24}, {1}, 1}};
    const auto                         actual = LocalZMixBridge::build(.2, .44, .06, {.32, .32}, zones, {.08, .08, .08});
    zones.back().masks                        = pure;
    const auto expected                       = LocalZMixBridge::build(.2, .44, .06, {.32, .32}, zones, {.08, .08, .08});
    REQUIRE(actual.size() == 1);
    for (double x = -7.9; x < 11.9; x += .1)
        CHECK(actual.front().interface_z(Point::new_scale(x, 0.)) == Approx(expected.front().interface_z(Point::new_scale(x, 0.))));
}

TEST_CASE("Shared mix interfaces meet flat fallback without changing their bead bounds", "[LocalZ][MixBridge][CurvedMixBridge]")
{
    auto bridges = LocalZMixBridge::build(.2, .44, .06, {.32, .32, .32},
                                          {{rectangle(-8., 0.), {.08, .10, .06}, {1, 2, 3}, 4},
                                           {rectangle(0., 8.), {.12, .06, .06}, {1, 2, 3}, 5}});
    REQUIRE(bridges.size() == 1);
    auto& bridge = bridges.front();
    REQUIRE(bridge.shared_stack);
    auto extended               = bridge;
    extended.boundary_tolerance = .1;
    extended.layer_footprints.push_back({1, rectangle(-8., 8.1), {rectangle(-8., 0.), rectangle(0., 8.1)}});
    CHECK(extended.advances_near(rectangle(8.35, 10.), .3));
    REQUIRE(bridge.pin_foreign_boundary(rectangle(-10., -8.), .3));
    for (size_t i = 0; i < bridge.passes.size(); ++i) {
        const auto boundary = bridge.surfaces(Point::new_scale(-7.9, 0.), i);
        CHECK(boundary.second == Approx(bridge.schedule_z(bridge.passes[i])).margin(1e-8));
    }
    CHECK_FALSE(bridge.advances_near(rectangle(-10., -8.), .3));
    for (double x = -7.99; x < 7.9; x += .03125) {
        double total = 0.;
        for (size_t i = 0; i < bridge.passes.size(); ++i) {
            const auto [lo, hi] = bridge.surfaces(Point::new_scale(x, 0.), i);
            CHECK(hi - lo >= .06 - 1e-7);
            CHECK(hi - lo <= .12 + 1e-7);
            const auto next = bridge.surfaces(Point::new_scale(x + .001, 0.), i);
            CHECK(std::abs(next.second - hi) <= .001 * bridge.maximum_slope + 1e-7);
            total += hi - lo;
        }
        CHECK(total == Approx(.24));
    }
    CHECK(bridge.surfaces(Point::new_scale(6., 0.), 0).second == Approx(.32));
}

TEST_CASE("Surface-following beads reject unsupported and disappearing geometry", "[LocalZ][MixBridge][CurvedMixBridge]")
{
    LocalZMixBridge::Zone left{rectangle(-4., 0.), {.12, .12}, {1, 2}, 3};
    LocalZMixBridge::Zone right{rectangle(0., 4.), {.08, .16}, {1, 2}, 4};
    bool                  ledge = false, disappearing = false, closing_hole = false;
    SECTION("A complete supported stack is accepted") {}
    SECTION("An outward step exactly at the cycle boundary is rejected") { ledge = true; }
    SECTION("A component that ends inside the first bead is not silently lost") { disappearing = true; }
    SECTION("A hole closing inside a bead retains nominal fallback") { closing_hole = true; }
    if (disappearing) {
        right.heights = {.12, .12};
        append(right.masks, rectangle(8., 10.));
    }
    auto bridges = LocalZMixBridge::build(.2, .44, .06, {.32, .32}, {left, right});
    if (disappearing) {
        REQUIRE(bridges.size() == 2);
        for (auto& island : bridges) {
            const auto owned           = island.ownership(size_t(-1));
            const bool ends            = owned.front().contains(Point::new_scale(9., 0.));
            island.follows_surface     = true;
            island.lower_support_masks = owned;
            for (size_t i = 0; i < 3; ++i)
                island.layer_footprints.push_back({i + 1, ends && i > 0 ? ExPolygons{} : owned, {}, .24 + .08 * i, .08});
            CHECK(island.fit_contours(.1) == !ends);
        }
        return;
    }
    REQUIRE(bridges.size() == 1);
    auto& bridge               = bridges.front();
    bridge.follows_surface     = true;
    bridge.lower_support_masks = ledge ? rectangle(-4., 2.) : bridge.ownership(size_t(-1));
    for (size_t i = 0; i < 3; ++i) {
        auto masks = rectangle(-4., 4.);
        if (disappearing && i == 0)
            append(masks, rectangle(8., 10.));
        if (closing_hole && i == 0) {
            const ExPolygons hole{ExPolygon(
                Points{Point::new_scale(-1., -1.), Point::new_scale(1., -1.), Point::new_scale(1., 1.), Point::new_scale(-1., 1.)})};
            masks = diff_ex(masks, hole);
        }
        bridge.layer_footprints.push_back({i + 1, std::move(masks), {}, .24 + .08 * i, .08});
    }
    INFO("ledge=" << ledge << " disappearing=" << disappearing << " closing_hole=" << closing_hole);
    ExPolygons rejected;
    CHECK(bridge.fit_contours(.1, &rejected) == !(ledge || disappearing || closing_hole));
    if (ledge || disappearing || closing_hole)
        REQUIRE_FALSE(rejected.empty());
    if (ledge)
        CHECK(get_extents(rejected).min.x() >= scale_(2.1));
    if (disappearing)
        CHECK(get_extents(rejected).min.x() > scale_(4.));
}

TEST_CASE("Unowned clipping debris is discarded but real islands still fail", "[LocalZ][MixBridge]")
{
    struct TestLayer : Layer
    {
        TestLayer() : Layer(1, nullptr, .08, 17.8, 17.76) {}
    } layer;
    const ExPolygons remnant{ExPolygon(Points{Point::new_scale(10., 10.), Point::new_scale(10.004, 10.), Point::new_scale(10., 10.0055)})};
    const auto       paths = LocalZMixBridgePaths::build(layer, remnant, 1);
    REQUIRE(paths);
    CHECK(paths->perimeters.empty());
    CHECK(paths->fills.empty());
    CHECK_THROWS_WITH(LocalZMixBridgePaths::build(layer, rectangle(0., 1.), 1),
                      "Local-Z island has no owning print region at Z=17.800000 area=8.000000");
}

TEST_CASE("Surface fitting retains fragmented contours and partial scan rows", "[LocalZ][MixBridge][CurvedMixBridge]")
{
    double shift = .02;
    SECTION("Partial last rows") {}
    SECTION("Horizontal boundary on a batched scanline") { shift = .025; }
    ExPolygons lower, upper;
    for (size_t row = 0; row < 20; ++row)
        for (size_t column = 0; column < 20; ++column) {
            const double x = .7 * column, y = .7 * row;
            lower.emplace_back(
                Points{Point::new_scale(x, y), Point::new_scale(x + .4, y), Point::new_scale(x + .4, y + .2), Point::new_scale(x, y + .2)});
            upper.push_back(lower.back());
            upper.back().translate(Point::new_scale(0., shift));
        }
    auto swept = lower;
    append(swept, upper);
    LocalZMixBridge bridge;
    bridge.z_lo            = .2;
    bridge.z_hi            = .36;
    bridge.minimum_height  = .06;
    bridge.planar_fallback = bridge.follows_surface = true;
    bridge.zones.push_back({union_ex(swept), {.16}, {1}, 2});
    bridge.passes.push_back({{0}, 0, 1, .36, .16});
    bridge.layer_footprints    = {{1, lower, {}, .24, .08}, {2, upper, {}, .32, .08}};
    bridge.lower_support_masks = lower;
    REQUIRE(bridge.fit_contours(.1));
    const auto contour = bridge.masks(bridge.passes.front());
    CHECK(contour.size() == 400);
    for (size_t row = 0; row < 20; ++row)
        for (size_t column = 0; column < 20; ++column) {
            const Point point = Point::new_scale(.7 * column + .2, .7 * row + .209);
            CHECK(std::any_of(contour.begin(), contour.end(), [&](const auto& polygon) { return polygon.contains(point); }));
        }
}

TEST_CASE("Local-Z color order survives appearing and disappearing pure neighbors", "[LocalZ][MixBridge]")
{
    unsigned previous = 0;
    for (unsigned neighbor : {1u, 0u, 2u, 0u, 1u}) {
        std::vector<LocalZMixBridge::Zone> zones{{rectangle(0., 4.), {.08, .08}, {1, 2}, 3}, {rectangle(4., 8.), {.10, .06}, {1, 2}, 4}};
        if (neighbor)
            zones.push_back({rectangle(-4., 0.), {.16}, {neighbor}, neighbor});
        const auto bridges = LocalZMixBridge::build(.2, .36, .06, {.32, .32}, zones, {.08, .08}, 1., 0., {1, 2});
        REQUIRE(bridges.size() == 1);
        const auto& bridge = bridges.front();
        for (const auto& zone : bridge.zones)
            if (!zone.preserve_nominal_layers)
                CHECK(zone.component_ids == std::vector<unsigned>{2, 1});
        std::vector<std::pair<double, unsigned>> stack;
        double                                   cyan  = 0.;
        const Point                              point = Point::new_scale(6., 0.);
        for (size_t i = 0; i < bridge.passes.size(); ++i) {
            const auto& pass  = bridge.passes[i];
            const auto  masks = bridge.masks(pass);
            if (std::none_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); }))
                continue;
            const auto [lo, hi] = bridge.surfaces(point, i);
            if (hi <= lo + 1e-9)
                continue;
            stack.emplace_back(lo, pass.filament_id);
            if (pass.filament_id == 1)
                cyan += hi - lo;
        }
        std::sort(stack.begin(), stack.end());
        REQUIRE(stack.size() == 2);
        CHECK(cyan == Approx(.10).margin(1e-6));
        CHECK(stack.front().second != previous);
        previous = stack.back().second;
    }
}

TEST_CASE("Sampled taper edges cannot extrude above their nominal ceiling", "[LocalZ][MixBridge]")
{
    LocalZMixBridge::Zone mixed{rectangle(0., 4.), {.07, .17}, {2, 1}, 3};
    LocalZMixBridge::Zone high{rectangle(4., 8.), {.17, .07}, {2, 1}, 4};
    LocalZMixBridge::Zone pure{rectangle(-4., 0.), {.24}, {1}, 1};
    const auto            bridges = LocalZMixBridge::build(.2, .44, .06, {.32, .32}, {mixed, high, pure}, {.08, .08, .08});
    REQUIRE(bridges.size() == 1);
    const auto& bridge = bridges.front();
    size_t      tips   = 0;
    for (size_t i = 0; i < bridge.passes.size(); ++i) {
        if (bridge.passes[i].tongue < 0)
            continue;
        ++tips;
        for (double x = -1.; x < 8.; x += .0037) {
            const auto [lo, hi] = bridge.surfaces(Point::new_scale(x, 0.), i);
            CHECK(hi >= lo);
            CHECK(hi <= bridge.passes[i].ceiling + 1e-8);
        }
        ExtrusionPath edge(erExternalPerimeter, .03, .4f, .08f);
        edge.polyline = Polyline{Point::new_scale(6., 0.), Point::new_scale(7., 0.)};
        CHECK(bridge.modulate(edge, i, 0.).empty());
    }
    REQUIRE(tips > 0);
    for (double x = -1.; x < 8.; x += .0037) {
        const Point                            point = Point::new_scale(x, 0.);
        std::vector<std::pair<double, double>> column;
        for (size_t i = 0; i < bridge.passes.size(); ++i) {
            const auto masks = bridge.masks(bridge.passes[i]);
            if (std::none_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); }))
                continue;
            const auto bounds = bridge.surfaces(point, i);
            if (bounds.second > bounds.first + 1e-9)
                column.push_back(bounds);
        }
        std::sort(column.begin(), column.end());
        double top = bridge.z_lo;
        for (const auto& [lo, hi] : column) {
            CHECK(lo == Approx(top).margin(1e-7));
            top = hi;
        }
        CHECK(top == Approx(bridge.z_hi).margin(1e-7));
    }
}

TEST_CASE("Moving eligible paint retains a continuous cycle owner", "[LocalZ][MixBridge][TemporalPaint]")
{
    BridgeEnvironment                    environment("1");
    const std::vector<std::string>       colors{"#FF00FF", "#00FFFF", "#FFFF00", "#FFFFFF"};
    std::vector<MixedFilamentDefinition> definitions(3);
    for (size_t i = 0; i < definitions.size(); ++i) {
        auto& recipe                 = definitions[i];
        recipe.identity.stable_id    = 9801 + i;
        recipe.source.kind           = MixedFilamentSourceKind::Custom;
        recipe.behavior.distribution = MixedFilamentDistributionMode::LayerCycle;
        if (i == 2)
            recipe.recipe.blend.components = {{{3}, 50}, {{4}, 50}};
        else
            recipe.recipe.blend.components = {{{1}, i == 0 ? 65 : 33}, {{2}, i == 0 ? 35 : 67}};
    }
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions(definitions, colors);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("mixed_filament_region_collapse", false);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("layer_height", .08);
    config.set("initial_layer_print_height", .2);
    config.set("wall_loops", 2);
    for (const auto* key : {"dithering_local_z_mode", "dithering_local_z_whole_objects", "dithering_local_z_preserve_first_layer",
                            "dithering_local_z_direct_multicolor", "dithering_local_z_independent_layer_height", "dithering_local_z_infill"})
        config.set(key, true);
    config.set("enable_prime_tower", false);
    config.set("single_extruder_multi_material", false);
    config.option<ConfigOptionStrings>("filament_colour")->values = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values.assign(4, 1.75);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values.assign(4, .4);
    config.option<ConfigOptionFloats>("max_layer_height")->values.assign(4, .32);
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(16, 100.);
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                            "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));
    for (const auto& key : config.keys())
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 1);
    const std::vector<double> cuts{0., .2, .28, 1.64};
    for (size_t part = 1; part < cuts.size(); ++part) {
        const double border = part == 2 ? 7.5 : 8.;
        for (size_t source = 0; source < 3; ++source) {
            auto     mesh  = make_cube(source == 2 ? 8. : 4., source == 2 ? 10. - border : border, cuts[part] - cuts[part - 1]);
            Matrix3d shear = Matrix3d::Identity();
            shear(0, 2)    = .2;
            mesh.transform(shear);
            auto* volume = object->add_volume(std::move(mesh));
            volume->translate(Vec3d((source == 1 ? 4. : 0.) + .2 * cuts[part - 1], source == 2 ? border : 0., cuts[part - 1]));
            TriangleSelector selector(volume->mesh());
            for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
                selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(5 + source));
            volume->mmu_segmentation_facets.set(selector);
        }
    }
    object->add_instance();
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    const auto* sliced           = print.get_object(0);
    const Point contact          = Point::new_scale(4.05, 7.75) - sliced->center_offset();
    bool        continuous_owner = false, third_recipe_retained = false;
    for (const auto& interval : sliced->local_z_intervals()) {
        if (interval.layer_id == 0)
            continue;
        ExPolygons actual, owned;
        for (const auto* region : sliced->get_layer(int(interval.layer_id))->regions())
            append(actual, to_expolygons(region->slices.surfaces));
        for (const auto& bridge : interval.mix_bridges) {
            const auto masks = bridge->ownership(interval.layer_id);
            CHECK(area(intersection_ex(owned, masks)) < scale_(.001) * double(scale_(.001)));
            append(owned, masks);
            if (interval.z_hi < .45 && !bridge->planar_fallback && bridge->source_filament_ids.size() > 1 &&
                std::any_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(contact); }))
                continuous_owner = true;
            for (const auto& zone : bridge->zones)
                third_recipe_retained |= zone.source_filament_id == 7 && !zone.masks.empty();
        }
        CHECK(area(diff_ex(union_ex(actual), union_ex(owned))) < scale_(.001) * double(scale_(.001)));
    }
    CHECK(continuous_owner);
    CHECK(third_recipe_retained);
}

TEST_CASE("Mix islands follow changing cross sections and close local fallback boundaries", "[LocalZ][MixBridge][CurvedMixBridge]")
{
    if (std::getenv("ORCA_BRIDGE_REPRO_VERBOSE"))
        set_logging_level(5);
    BridgeEnvironment environment("1");
    bool              ledge = false, remote = false, gradient = false, short_tail = false, hull = false, narrowing = false, steep = false;
    bool              painted_remote = false, stationary_noise = false;
    SECTION("Sloped painted walls") {}
    SECTION("An expanding hull retains each visible stripe on a complete outer wall") { hull = true; }
    SECTION("A narrowing hull retains each visible stripe on a complete outer wall") { hull = narrowing = true; }
    SECTION("A disappearing painted island leaves every outer hull cycle joined") { hull = painted_remote = true; }
    SECTION("A 45-degree expanding hull keeps one bead per color in every cycle") { hull = steep = true; }
    SECTION("A 45-degree narrowing hull keeps one bead per color in every cycle") { hull = steep = narrowing = true; }
    SECTION("Sub-resolution contour drift preserves stationary joined walls") { hull = stationary_noise = true; }
    SECTION("An unsupported ledge retains local fallback with joins below and above it") { ledge = true; }
    SECTION("A separate changing island does not veto the painted walls") { remote = true; }
    SECTION("Gradient recipes are sampled within each cycle") { gradient = true; }
    SECTION("An untileable final layer does not disable all complete cycles") { short_tail = true; }
    const double                         hull_height = steep ? 3.32 : 3.4;
    const double                         hull_slope  = (narrowing ? 1. : -1.) * (stationary_noise ? .0001 : steep ? 1. : .5);
    const std::vector<std::string>       colors{"#FF00FF", "#00FFFF"};
    std::vector<MixedFilamentDefinition> definitions(2);
    for (size_t i = 0; i < definitions.size(); ++i) {
        auto& recipe                   = definitions[i];
        recipe.identity.stable_id      = 9701 + i;
        recipe.source.kind             = MixedFilamentSourceKind::Custom;
        recipe.behavior.distribution   = MixedFilamentDistributionMode::LayerCycle;
        recipe.recipe.blend.components = {{{1}, i == 0 ? 65 : 33}, {{2}, i == 0 ? 35 : 67}};
        if (gradient) {
            recipe.behavior.gradient.enabled           = true;
            recipe.behavior.gradient.component_a_start = .35f;
            recipe.behavior.gradient.component_a_end   = .65f;
        }
    }
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions(definitions, colors);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("mixed_filament_region_collapse", false);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("layer_height", .08);
    config.set("initial_layer_print_height", .2);
    config.set("wall_loops", 2);
    for (const auto* key : {"dithering_local_z_mode", "dithering_local_z_whole_objects", "dithering_local_z_preserve_first_layer",
                            "dithering_local_z_direct_multicolor", "dithering_local_z_independent_layer_height", "dithering_local_z_infill"})
        config.set(key, true);
    config.set("enable_prime_tower", false);
    config.set("wipe_tower_no_sparse_layers", false);
    config.set("prime_volume", 15.);
    config.set("single_extruder_multi_material", false);
    config.set("machine_start_gcode", "G90\nM83\n");
    config.option<ConfigOptionPercent>("sparse_infill_density")->value = 100.;
    config.option<ConfigOptionStrings>("filament_colour")->values      = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values     = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values       = {.4, .4};
    config.option<ConfigOptionFloats>("max_layer_height")->values      = short_tail ? std::vector<double>{.17, .17} :
                                                                                      std::vector<double>{.32, .32};
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(4, 100.);
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                            "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));
    for (const auto& key : config.keys())
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 1);
    const std::vector<double> cuts = ledge ? std::vector<double>{0., .68, .92, 1.64} :
                                             std::vector<double>{0., hull       ? hull_height :
                                                                     short_tail ? 1.72 :
                                                                                  1.64};
    for (size_t half = 0; half < 2; ++half)
        for (size_t part = 1; part < cuts.size(); ++part) {
            auto mesh = make_cube(4., 8., cuts[part] - cuts[part - 1]);
            if (hull)
                for (auto& vertex : mesh.its.vertices)
                    vertex.y() += float(-hull_slope * vertex.z()) * (vertex.y() > 4. ? 1.f : -1.f);
            if (!hull && !ledge && !remote) {
                Matrix3d shear = Matrix3d::Identity();
                shear(0, 2)    = .2;
                mesh.transform(shear);
            }
            auto* volume = object->add_volume(std::move(mesh));
            volume->translate(Vec3d(4. * half, ledge && part == 2 ? 1. : 0., cuts[part - 1]));
            TriangleSelector selector(volume->mesh());
            for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
                selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(3 + half));
            volume->mmu_segmentation_facets.set(selector);
        }
    if (remote) {
        auto* volume = object->add_volume(make_cube(3., 3., .76));
        volume->translate(Vec3d(20., 0., 0.));
    }
    if (painted_remote) {
        auto* volume = object->add_volume(make_cube(1., 1., .36));
        volume->translate(Vec3d(20., 0., 0.));
        TriangleSelector selector(volume->mesh());
        for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
            selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(3));
        volume->mmu_segmentation_facets.set(selector);
    }
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    if (std::getenv("ORCA_BRIDGE_REPRO_VERBOSE"))
        print.set_status_callback([](const auto& status) { std::cout << "CURVED_STAGE " << status.text << std::endl; });
    print.process();
    const auto* sliced = print.get_object(0);
    size_t      joined = 0, following = 0;
    bool        below = false, above = false, fallback = false;
    for (const auto& interval : sliced->local_z_intervals()) {
        if (interval.layer_id == 0)
            continue;
        fallback |= interval.mix_bridges.empty();
        ExPolygons actual, owned, ordinary;
        for (const auto* region : sliced->get_layer(int(interval.layer_id))->regions())
            append(actual, to_expolygons(region->slices.surfaces));
        for (const auto& bridge : interval.mix_bridges)
            append(owned, bridge->ownership(interval.layer_id));
        const auto remainder = diff_ex(union_ex(actual), union_ex(owned));
        fallback |= area(remainder) > scale_(.001) * scale_(.001);
        if (ledge || short_tail) {
            const double sample_z = (interval.z_lo + interval.z_hi) * .5;
            for (const auto& plan : sliced->local_z_sublayer_plan())
                if (!plan.mix_bridge && plan.z_lo < sample_z && plan.z_hi >= sample_z)
                    for (const auto& masks : plan.painted_masks_by_extruder)
                        append(ordinary, masks);
            CHECK(area(diff_ex(remainder, union_ex(ordinary))) < scale_(.001) * scale_(.001));
        }
        for (const auto& bridge : interval.mix_bridges) {
            if (bridge->planar_fallback) {
                fallback = true;
                continue;
            }
            ++joined;
            if (stationary_noise && bridge->z_lo > .5)
                CHECK_FALSE(bridge->follows_surface);
            below |= bridge->z_hi < .69;
            above |= bridge->z_lo > .91;
            following += !bridge->layer_footprints.empty();
        }
        for (size_t i = 0; i < interval.mix_bridges.size(); ++i)
            for (size_t j = i + 1; j < interval.mix_bridges.size(); ++j)
                if (std::max(interval.mix_bridges[i]->z_lo, interval.mix_bridges[j]->z_lo) <
                    std::min(interval.mix_bridges[i]->z_hi, interval.mix_bridges[j]->z_hi) - 1e-6)
                    CHECK(area(intersection_ex(interval.mix_bridges[i]->ownership(interval.layer_id),
                                               interval.mix_bridges[j]->ownership(interval.layer_id))) < scale_(.001) * scale_(.001));
        if (!remote)
            CHECK(area(diff_ex(union_ex(actual), union_ex(owned))) < scale_(.001) * scale_(.001));
        // A continuous owned region may span successive bridge cycles. Close
        // the independent cadence only when ownership actually hands over.
        const auto& previous = sliced->local_z_intervals()[interval.layer_id - 1];
        if (previous.mix_bridges.empty() != interval.mix_bridges.empty())
            for (const auto& plan : sliced->local_z_sublayer_plan())
                if (!plan.mix_bridge && plan.dependency_group > 0 && plan.dependency_group <= 2)
                    CHECK_FALSE((plan.z_lo < interval.z_lo - 1e-6 && plan.z_hi > interval.z_lo + 1e-6));
    }
    REQUIRE(joined > 0);
    if (short_tail) {
        CHECK(fallback);
        REQUIRE_FALSE(sliced->local_z_intervals()[1].mix_bridges.empty());
        const auto& tail = sliced->local_z_intervals().back();
        CHECK(std::all_of(tail.mix_bridges.begin(), tail.mix_bridges.end(), [](const auto& bridge) { return bridge->planar_fallback; }));
        for (double x : {2., 6.}) {
            const Point                            point = Point::new_scale(x, 4.) - sliced->center_offset();
            std::vector<std::pair<double, double>> cover;
            for (const auto& plan : sliced->local_z_sublayer_plan()) {
                if (plan.layer_id == 0)
                    continue;
                const auto contains = [&](const auto& masks) {
                    return std::any_of(masks.begin(), masks.end(), [&](const auto& mask) { return mask.contains(point); });
                };
                if (plan.mix_bridge) {
                    if (!plan.mix_bridge->planar_fallback)
                        CHECK(plan.mix_bridge->z_hi <= tail.z_lo + 1e-6);
                    const auto& pass = plan.mix_bridge->passes[plan.mix_bridge_component];
                    if (contains(plan.mix_bridge->masks(pass, plan.layer_id)))
                        cover.push_back(plan.mix_bridge->surfaces(point, plan.mix_bridge_component));
                } else if (std::any_of(plan.painted_masks_by_extruder.begin(), plan.painted_masks_by_extruder.end(), contains)) {
                    // Ordinary plans retain their source masks; path generation
                    // subtracts the island ownership at each nominal interval.
                    for (const auto& interval : sliced->local_z_intervals()) {
                        const double lo = std::max(plan.z_lo, interval.z_lo), hi = std::min(plan.z_hi, interval.z_hi);
                        if (hi > lo + 1e-6 &&
                            std::none_of(interval.mix_bridges.begin(), interval.mix_bridges.end(),
                                         [&](const auto& bridge) { return contains(bridge->ownership(interval.layer_id)); }))
                            cover.emplace_back(lo, hi);
                    }
                }
            }
            std::sort(cover.begin(), cover.end());
            double top = sliced->local_z_intervals().front().z_hi;
            for (const auto& [lo, hi] : cover) {
                CHECK(lo == Approx(top).margin(1e-6));
                CHECK(hi > lo);
                top = hi;
            }
            CHECK(top == Approx(tail.z_hi).margin(1e-6));
        }
    }
    if (ledge) {
        CHECK(below);
        CHECK(above);
        CHECK(fallback);
    } else if (!remote)
        CHECK(following > 0);
    for (const auto& plan : sliced->local_z_sublayer_plan()) {
        if (!plan.mix_bridge || plan.layer_id == 0)
            continue;
        const auto& bridge = *plan.mix_bridge;
        REQUIRE(plan.mix_bridge_paths);
        const auto& pass   = bridge.passes[plan.mix_bridge_component];
        const auto  own    = bridge.ownership(plan.layer_id);
        if (!bridge.planar_fallback)
            CHECK_FALSE(plan.mix_bridge_paths->perimeters.empty());
        else if (!bridge.nominal_band && plan.mix_bridge_paths->perimeters.empty())
            CHECK(offset_ex(own, -scale_(.2)).empty()); // Sub-nozzle slivers cannot carry a separate perimeter.
        ExPolygons  actual;
        for (const auto* region : sliced->get_layer(int(plan.layer_id))->regions())
            append(actual, to_expolygons(region->slices.surfaces));
        CHECK(area(diff_ex(own, union_ex(actual))) < scale_(.001) * scale_(.001));
        for (const auto& footprint : bridge.layer_footprints) {
            if (bridge.follows_surface)
                continue;
            CHECK(area(diff_ex(own, footprint.masks)) < scale_(.001) * scale_(.001));
            CHECK(area(diff_ex(footprint.masks, own)) < scale_(.001) * scale_(.001));
        }
        if (!remote && !bridge.follows_surface) {
            // Moving portions of the exterior have a separate nominal owner;
            // their coverage is checked across all owners above and in G-code.
            CHECK(area(diff_ex(own, plan.painted_masks_by_extruder[pass.filament_id - 1])) < scale_(.001) * scale_(.001));
        }
    }
    const auto           output = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("curved-mix-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(output.string(), &result, nullptr);
    boost::filesystem::remove(output);
    if (hull) {
        // Every shared cycle must reach the visible shell. An occasional joined
        // loop in the interior cannot stand in for continuity at paint boundaries.
        std::set<std::pair<double, double>> shell_cycles;
        for (const auto& plan : sliced->local_z_sublayer_plan())
            if (plan.mix_bridge && !plan.mix_bridge->planar_fallback && plan.layer_id > 0)
                shell_cycles.emplace(plan.mix_bridge->z_lo, plan.mix_bridge->z_hi);
        REQUIRE_FALSE(shell_cycles.empty());
        for (const auto& [lo, hi] : shell_cycles) {
            std::array<bool, 2> crossed{};
            double              left = 1e9, right = -1e9;
            unsigned            active_tool = 99;
            for (size_t i = 1; i < result.moves.size(); ++i) {
                const auto&  move     = result.moves[i];
                const auto&  previous = result.moves[i - 1];
                const double mid_z    = move.position.z() - .5 * move.height;
                const double front    = 100. + hull_slope * mid_z;
                if (move.type != EMoveType::Extrude ||
                    (move.extrusion_role != erExternalPerimeter && move.extrusion_role != erOverhangPerimeter) ||
                    move.position.z() <= lo + 1e-5 || move.position.z() > hi + 1e-5 ||
                    std::abs(move.position.y() - .5 * move.width - front) > .03 || active_tool != move.extruder_id) {
                    left        = 1e9;
                    right       = -1e9;
                    active_tool = move.extruder_id;
                    continue;
                }
                left  = std::min({left, double(previous.position.x()), double(move.position.x())});
                right = std::max({right, double(previous.position.x()), double(move.position.x())});
                if (active_tool < 2)
                    crossed[active_tool] |= left < 103. && right > 106.;
            }
            INFO("Visible shared cycle " << lo << " .. " << hi);
            CHECK(crossed[0]);
            CHECK(crossed[1]);
        }
        for (const auto [x, expected_magenta] : {std::pair{102.5, .35}, std::pair{105.5, .67}}) {
            size_t samples = 0, magenta = 0;
            for (const auto* layer : sliced->layers()) {
                if (layer->id() == 0)
                    continue;
                const double z          = layer->print_z - .5 * layer->height;
                double       front      = 1e9;
                double       bead_mid_z = 0.;
                unsigned     front_tool = 99;
                for (size_t i = 1; i < result.moves.size(); ++i) {
                    const auto& move     = result.moves[i];
                    const auto& previous = result.moves[i - 1];
                    if (move.type != EMoveType::Extrude ||
                        (move.extrusion_role != erExternalPerimeter && move.extrusion_role != erOverhangPerimeter) ||
                        std::abs(move.position.x() - previous.position.x()) < 1e-6 ||
                        x < std::min(move.position.x(), previous.position.x()) || x > std::max(move.position.x(), previous.position.x()))
                        continue;
                    const double t   = (x - previous.position.x()) / (move.position.x() - previous.position.x());
                    const double top = previous.position.z() + t * (move.position.z() - previous.position.z());
                    if (top < z || top - move.height > z)
                        continue;
                    const double y = previous.position.y() + t * (move.position.y() - previous.position.y()) - .5 * move.width;
                    if (y < front) {
                        front      = y;
                        front_tool = move.extruder_id;
                        bead_mid_z = top - .5 * move.height;
                    }
                }
                INFO("Hull wall at x=" << x << " z=" << z);
                REQUIRE(front_tool < 2);
                CHECK(front == Approx(100. + hull_slope * bead_mid_z).margin(.015));
                CHECK(std::abs(front - (100. + hull_slope * z)) <= .105);
                ++samples;
                magenta += front_tool == 1;
            }
            CHECK(double(magenta) / samples == Approx(expected_magenta).margin(.06));
        }
        // Read the actual exterior bead stack, including separate same-color
        // moves. Each plateau must close without gaps/overlap, and two adjacent
        // beads may not repeat a tool when their combined height fits the cap.
        for (const double x : {102.5, 105.5}) {
            struct Bead
            {
                double   lo, hi;
                unsigned tool;
            };
            std::vector<Bead> beads;
            for (size_t i = 1; i < result.moves.size(); ++i) {
                const auto& move     = result.moves[i];
                const auto& previous = result.moves[i - 1];
                if (move.type != EMoveType::Extrude ||
                    (move.extrusion_role != erExternalPerimeter && move.extrusion_role != erOverhangPerimeter) ||
                    move.position.z() <= .201 || std::abs(move.position.x() - previous.position.x()) < 1e-6 ||
                    x < std::min(move.position.x(), previous.position.x()) || x >= std::max(move.position.x(), previous.position.x()))
                    continue;
                const double t  = (x - previous.position.x()) / (move.position.x() - previous.position.x());
                const double hi = previous.position.z() + t * (move.position.z() - previous.position.z());
                const double lo = hi - move.height;
                const double y  = previous.position.y() + t * (move.position.y() - previous.position.y()) - .5 * move.width;
                if (std::abs(y - (100. + hull_slope * .5 * (lo + hi))) < .02)
                    beads.push_back({lo, hi, move.extruder_id});
            }
            std::sort(beads.begin(), beads.end(), [](const auto& a, const auto& b) { return a.lo < b.lo; });
            REQUIRE(beads.size() == 2 * shell_cycles.size());
            double top = .2, color_height = 0.;
            for (size_t i = 0; i < beads.size(); ++i) {
                const auto& bead = beads[i];
                INFO("Exterior stack x=" << x << " bottom=" << bead.lo << " tool=" << bead.tool);
                CHECK(bead.lo == Approx(top).margin(.001));
                CHECK(bead.hi - bead.lo >= .06 - .001);
                CHECK(bead.hi - bead.lo <= .32 + .001);
                if (i > 0 && bead.hi - beads[i - 1].lo <= .32 - .001)
                    CHECK(bead.tool != beads[i - 1].tool);
                color_height += bead.tool == 1 ? bead.hi - bead.lo : 0.;
                top = bead.hi;
            }
            CHECK(top == Approx(hull_height).margin(.001));
            CHECK(color_height / (top - .2) == Approx(x < 104. ? .35 : .67).margin(.01));
        }
        // The side is stationary while the front slopes. Probe the end of the
        // reserved shell, where clipping an old path leaves a crescent-shaped
        // gap between its endpoint and the inset island wall.
        for (const auto* layer : sliced->layers()) {
            if (layer->id() == 0)
                continue;
            const double z         = layer->slice_z;
            const double y         = 100. + hull_slope * z + .3;
            double       uncovered = 1e9;
            for (size_t i = 1; i < result.moves.size(); ++i) {
                const auto& move     = result.moves[i];
                const auto& previous = result.moves[i - 1];
                if (move.type != EMoveType::Extrude)
                    continue;
                const Vec2d a = previous.position.head<2>().cast<double>();
                const Vec2d d = (move.position - previous.position).head<2>().cast<double>();
                if (d.squaredNorm() < 1e-12)
                    continue;
                const Vec2d  probe(100.02, y);
                const double t   = std::clamp((probe - a).dot(d) / d.squaredNorm(), 0., 1.);
                const double top = previous.position.z() + t * (move.position.z() - previous.position.z());
                if (top >= z && top - move.height <= z)
                    uncovered = std::min(uncovered, (probe - a - t * d).norm() - .5 * move.width);
            }
            INFO("Shell endpoint at y=" << y << " z=" << z);
            CHECK(uncovered <= .04);
        }
    }
    std::array<bool, 2> crossing{};
    double              run_min = 1e9, run_max = -1e9;
    unsigned            tool = 99;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& move     = result.moves[i];
        const auto& previous = result.moves[i - 1];
        if (move.type != EMoveType::Extrude || (move.extrusion_role != erExternalPerimeter && move.extrusion_role != erOverhangPerimeter) ||
            move.position.z() <= .201 || std::abs(move.position.y() - previous.position.y()) > 1e-5) {
            run_min = 1e9;
            run_max = -1e9;
            continue;
        }
        if (tool != move.extruder_id) {
            run_min = 1e9;
            run_max = -1e9;
        }
        tool    = move.extruder_id;
        run_min = std::min({run_min, double(move.position.x()), double(previous.position.x())});
        run_max = std::max({run_max, double(move.position.x()), double(previous.position.x())});
        crossing[tool] |= run_min < 103. && run_max > 106.;
    }
    if (!hull) {
        CHECK(crossing[0]);
        CHECK(crossing[1]);
    }
}

TEST_CASE("Painted mix bridges export continuous walls and retain the base interior", "[LocalZ][MixBridge][GCode]")
{
    bool   wide_ratios = false, wide_tail = false;
    bool   enabled          = true;
    bool   compatible       = true;
    double minimum          = .06;
    double nominal          = .2;
    size_t passes_per_layer = 2;
    bool   different_pair = false, three_components = false, different_count = false, unrelated = false, disconnected = false;
    bool   multiple_groups = false, changing_connectivity = false;
    bool   foreign_contact = false, different_wall_settings = false, independent = true, tower_enabled = true;
    bool   fine_cycles = false, intermittent_paint = false, asymmetric_pure = false, cross_interval = false, coarse_cross_interval = false;
    bool   latest_ratios = false, foreign_physical = false, base_contact = false;
    SECTION("Enabled") {}
    SECTION("Fine whole-object layers with collapse off join mixtures and a solid neighbor")
    {
        fine_cycles = true;
        nominal = minimum = .08;
    }
    SECTION("Unpainted contact decides the phase ahead of a distant pure body")
    {
        fine_cycles = base_contact = true;
        nominal = minimum = .08;
    }
    SECTION("Short paint interruptions retain complete shared cycles")
    {
        fine_cycles = intermittent_paint = true;
        nominal = minimum = .08;
    }
    SECTION("Asymmetric mixtures leave every pure-color bead at nominal height")
    {
        fine_cycles = asymmetric_pure = true;
        nominal = minimum = .08;
    }
    SECTION("Latest 65:35 and 33:67 ratios taper into pure nominal walls")
    {
        fine_cycles = latest_ratios = true;
        nominal                     = .08;
        minimum                     = .06;
    }
    SECTION("Cross-interval ratios keep uninterrupted non-planar walls")
    {
        fine_cycles = cross_interval = true;
        nominal = minimum = .08;
    }
    SECTION("The reported 0.20 mm nominal layers keep non-planar joins")
    {
        fine_cycles = cross_interval = coarse_cross_interval = true;
        nominal                                              = .2;
        minimum                                              = .08;
    }
    SECTION("Six adjacent recipes join when the dominant material needs two beads")
    {
        fine_cycles = wide_ratios = true;
        nominal                   = .08;
        minimum                   = .06;
    }
    SECTION("Wide recipes spread closing height across mergeable cycles")
    {
        fine_cycles = wide_ratios = wide_tail = true;
        nominal                               = .08;
        minimum                               = .06;
    }
    SECTION("Disabled") { enabled = false; }
    SECTION("Unprintable ratio falls back")
    {
        minimum    = .17;
        compatible = false;
    }
    SECTION("Different physical sets share their common filament")
    {
        different_pair   = true;
        passes_per_layer = 3;
    }
    SECTION("Three physical components share a complete stack")
    {
        three_components = true;
        nominal          = .3;
        passes_per_layer = 3;
    }
    SECTION("Different component counts share every common color")
    {
        different_count  = true;
        nominal          = .3;
        passes_per_layer = 3;
    }
    SECTION("An unrelated ineligible recipe keeps its independent cadence") { unrelated = true; }
    SECTION("Two connected groups share walls independently")
    {
        multiple_groups  = true;
        passes_per_layer = 4;
    }
    SECTION("A selected recipe closes cycles after its neighbor ends") { changing_connectivity = true; }
    SECTION("An adjacent ineligible cadence keeps the touching group flat")
    {
        foreign_contact = true;
        compatible      = false;
    }
    SECTION("An adjacent physical color outside the recipes prevents an advancing island")
    {
        foreign_physical = true;
        compatible       = false;
    }
    SECTION("Different wall settings preserve the original planner")
    {
        different_wall_settings = true;
        compatible              = false;
    }
    SECTION("Fixed nominal cadences can coexist with bridges")
    {
        unrelated   = true;
        independent = false;
    }
    SECTION("Shared paths export without a prime tower")
    {
        different_pair   = true;
        passes_per_layer = 3;
        tower_enabled    = false;
    }
    SECTION("Disconnected recipes do not form a bridge")
    {
        disconnected = true;
        compatible   = false;
    }
    BridgeEnvironment                    environment(enabled ? "1" : "0");
    const std::vector<std::string>       colors{"#FF00FF", "#00FFFF", "#FFFF00"};
    std::vector<MixedFilamentDefinition> definitions(wide_ratios ? 6 : multiple_groups ? 4 : (unrelated || foreign_contact) ? 3 : 2);
    definitions[0].recipe.blend.components = {{{1}, 65}, {{2}, 35}};
    // Reversed authored order still refers to the same physical stack.
    definitions[1].recipe.blend.components = {{{2}, 65}, {{1}, 35}};
    if (fine_cycles) {
        definitions[0].recipe.blend.components = {{{1}, 50}, {{2}, 50}};
        definitions[1].recipe.blend.components = {{{1}, 51}, {{2}, 49}};
    }
    if (asymmetric_pure) {
        definitions[0].recipe.blend.components = {{{1}, 66}, {{2}, 34}};
        definitions[1].recipe.blend.components = {{{1}, 41}, {{2}, 59}};
    }
    if (cross_interval) {
        definitions[0].recipe.blend.components = {{{1}, 60}, {{2}, 40}};
        definitions[1].recipe.blend.components = {{{1}, 32}, {{2}, 68}};
    }
    if (latest_ratios) {
        definitions[0].recipe.blend.components = {{{1}, 65}, {{2}, 35}};
        definitions[1].recipe.blend.components = {{{1}, 33}, {{2}, 67}};
    }
    if (different_pair)
        definitions[1].recipe.blend.components = {{{2}, 65}, {{3}, 35}};
    if (three_components) {
        definitions[0].recipe.blend.components = {{{1}, 50}, {{2}, 25}, {{3}, 25}};
        definitions[1].recipe.blend.components = {{{3}, 25}, {{2}, 50}, {{1}, 25}};
    }
    if (different_count) {
        definitions[0].recipe.blend.components = {{{1}, 50}, {{2}, 50}};
        definitions[1].recipe.blend.components = {{{1}, 30}, {{2}, 30}, {{3}, 40}};
    }
    if (unrelated || foreign_contact)
        definitions[2].recipe.blend.components = {{{1}, 90}, {{3}, 10}};
    if (multiple_groups) {
        definitions[2].recipe.blend.components = {{{2}, 65}, {{3}, 35}};
        definitions[3].recipe.blend.components = {{{2}, 35}, {{3}, 65}};
    }
    if (wide_ratios) {
        const std::vector<int> weights{50, 67, 75, 80, 83, 86};
        for (size_t i = 0; i < weights.size(); ++i)
            definitions[i].recipe.blend.components = {{{1}, 100 - weights[i]}, {{2}, weights[i]}};
    }
    for (size_t i = 0; i < definitions.size(); ++i) {
        definitions[i].identity.stable_id    = 9501 + i;
        definitions[i].source.kind           = MixedFilamentSourceKind::Custom;
        definitions[i].behavior.distribution = MixedFilamentDistributionMode::LayerCycle;
    }
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions(definitions, colors);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("mixed_filament_region_collapse", !fine_cycles);
    config.set("mixed_filament_height_lower_bound", minimum);
    config.set("layer_height", nominal);
    config.set("initial_layer_print_height", fine_cycles ? .2 : nominal);
    config.set("wall_loops", 2);
    config.set("dithering_local_z_mode", true);
    config.set("dithering_local_z_whole_objects", fine_cycles);
    config.set("dithering_local_z_preserve_first_layer", true);
    config.set("dithering_local_z_direct_multicolor", true);
    config.set("dithering_local_z_independent_layer_height", independent);
    config.set("dithering_local_z_infill", true);
    config.set("enable_prime_tower", tower_enabled);
    config.set("wipe_tower_no_sparse_layers", false);
    config.set("single_extruder_multi_material", false);
    config.set("prime_volume", 15.);
    config.set("machine_start_gcode", "G90\nM83\n");
    config.option<ConfigOptionStrings>("filament_colour")->values  = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {.4, .4, .4};
    config.option<ConfigOptionFloats>("max_layer_height")->values  = wide_ratios ? std::vector<double>{.32, .32, .32} :
                                                                                   std::vector<double>{.3, .3, .3};
    // This taller fixture reaches sparse interior layers. Its deposited-area
    // checks require solid fill, as in the shorter overlapping-skin fixtures.
    if (wide_tail)
        config.option<ConfigOptionPercent>("sparse_infill_density")->value = 100.;
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(9, 100.);
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                            "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    }
    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 3);
    // Two explicit halves keep the paint boundary vertical and give both
    // recipes substantial top skin. Painting a coarse cube by triangle
    // centroid creates diagonal, wall-width slivers instead.
    for (int half = 0; half < int(definitions.size()); ++half) {
        const double              top  = fine_cycles ? (wide_ratios           ? (wide_tail ? 3.4 : 1.64) :
                                                        coarse_cross_interval ? 1. :
                                                        asymmetric_pure       ? .92 :
                                                                                .84) :
                                                       (changing_connectivity && half == 1 ? 2 : 5) * nominal;
        const double              gap  = half == 0 ? .28 : .52;
        const std::vector<double> cuts = intermittent_paint ? std::vector<double>{0., gap, gap + .08, top} : std::vector<double>{0., top};
        for (size_t part = 1; part < cuts.size(); ++part) {
            auto* volume = object->add_volume(make_cube(4., 8., cuts[part] - cuts[part - 1]));
            volume->translate(Vec3d(wide_ratios ? 4. * half :
                                    half >= 2   ? (foreign_contact ? 8. : 20.) + 4. * (half - 2) :
                                                  (disconnected ? 6. : 4.) * half,
                                    0., cuts[part - 1]));
            if (different_wall_settings && half == 1)
                volume->config.set("wall_loops", 3);
            TriangleSelector selector(volume->mesh());
            for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
                selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(intermittent_paint && part == 2 ? 1 : 4 + half));
            volume->mmu_segmentation_facets.set(selector);
        }
    }
    if (foreign_physical) {
        auto* volume = object->add_volume(make_cube(4., 8., 5 * nominal));
        volume->translate(Vec3d(8., 0., 0.));
        TriangleSelector selector(volume->mesh());
        for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
            selector.set_facet(int(facet), EnforcerBlockerType(3));
        volume->mmu_segmentation_facets.set(selector);
    }
    if (fine_cycles) {
        auto* volume = object->add_volume(make_cube(4., 8.,
                                                    wide_ratios           ? (wide_tail ? 3.4 : 1.64) :
                                                    coarse_cross_interval ? 1. :
                                                    asymmetric_pure       ? .92 :
                                                                            .84));
        volume->translate(Vec3d(-4., 0., 0.));
        if (base_contact) {
            volume->config.set("extruder", 2);
            auto* remote = object->add_volume(make_cube(20., 20., .84));
            remote->translate(Vec3d(20., 20., 0.));
            remote->config.set("extruder", 1);
        } else {
            TriangleSelector selector(volume->mesh());
            for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
                selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(1));
            volume->mmu_segmentation_facets.set(selector);
        }
    }
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    const auto& plans         = print.get_object(0)->local_z_sublayer_plan();
    size_t      bridge_passes = 0;
    for (const auto& plan : plans) {
        if (!plan.mix_bridge)
            continue;
        bridge_passes += plan.layer_id > 0;
        if (plan.layer_id == 0) {
            CHECK(plan.mix_bridge->z_lo == Approx(0.));
            CHECK(plan.mix_bridge->z_hi == Approx(nominal));
            for (const auto& zone : plan.mix_bridge->zones) {
                REQUIRE(zone.heights.size() == 1);
                CHECK(zone.heights.front() == Approx(nominal));
            }
            continue;
        }
        CHECK_FALSE(plan.external_perimeters_only);
        REQUIRE(plan.mix_bridge_paths);
        CHECK_FALSE(plan.mix_bridge_paths->perimeters.empty());
        std::function<size_t(const ExtrusionEntity&)> closed_loops = [&](const ExtrusionEntity& entity) -> size_t {
            if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
                CHECK(loop->first_point() == loop->last_point());
                return 1;
            }
            size_t count = 0;
            if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity))
                for (const auto* child : collection->entities)
                    count += closed_loops(*child);
            return count;
        };
        CHECK(closed_loops(plan.mix_bridge_paths->perimeters) > 0);
        const auto& pass           = plan.mix_bridge->passes[plan.mix_bridge_component];
        const bool  optional_split = std::any_of(plan.mix_bridge->split_runs.begin(), plan.mix_bridge->split_runs.end(),
                                                 [&](const auto& run) {
                                                    return pass.flat_height == 0. && pass.component >= run.first &&
                                                           pass.component < run.last;
                                                });
        if (optional_split) {
            const auto masks     = plan.mix_bridge->masks(pass);
            const auto edge      = diff_ex(masks, offset_ex(masks, -scale_(.45)));
            auto       deposited = plan.mix_bridge_paths->perimeters.polygons_covered_by_width();
            append(deposited, plan.mix_bridge_paths->fills.polygons_covered_by_width());
            CHECK(area(diff_ex(edge, union_ex(deposited))) <= .02 * area(edge) + scale_(.01) * scale_(.01));
        }
        if (pass.tongue >= 0) {
            ExPolygons pure;
            for (const auto& zone : plan.mix_bridge->zones)
                if (zone.join_terminal_pass)
                    append(pure, zone.masks);
            const auto bite      = diff_ex(plan.mix_bridge->masks(pass), pure);
            auto       deposited = plan.mix_bridge_paths->perimeters.polygons_covered_by_width();
            append(deposited, plan.mix_bridge_paths->fills.polygons_covered_by_width());
            // Boundary tessellation may leave tiny corners, but no missing band.
            CHECK(area(diff_ex(bite, union_ex(deposited))) <= .05 * area(bite) + scale_(.01) * scale_(.01));
        }

        CHECK(plan.dependency_order == plan.mix_bridge_component);
        CHECK(plan.mix_bridge->z_hi == Approx(plan.z_hi));
        CHECK(plan.mix_bridge->z_lo == Approx(plan.z_lo));
        const auto& band = plan.mix_bridge->passes[plan.mix_bridge_component];
        CHECK(plan.print_z == Approx(plan.mix_bridge->schedule_z(band)));
        const auto& interval = print.get_object(0)->local_z_intervals()[plan.layer_id];
        CHECK(plan.print_z > interval.z_lo - 1e-6);
        CHECK(plan.print_z <= interval.z_hi + 1e-6);
        if (multiple_groups)
            CHECK(plan.mix_bridge->source_filament_ids ==
                  (plan.mix_bridge->source_filament_ids.front() == 4 ? std::vector<unsigned int>{4, 5} : std::vector<unsigned int>{6, 7}));
        if (changing_connectivity)
            CHECK(plan.mix_bridge->zones.size() == (plan.layer_id == 1 ? 2 : 1));
    }
    const size_t expected_bridges = fine_cycles ? (wide_ratios           ? (wide_tail ? 52 : 24) :
                                                   coarse_cross_interval ? 6 :
                                                   cross_interval        ? 10 :
                                                   asymmetric_pure       ? 12 :
                                                                           10) :
                                                  4 * passes_per_layer;
    REQUIRE(bridge_passes == (enabled && compatible ? expected_bridges : 0));
    if (fine_cycles) {
        for (const auto& interval : print.get_object(0)->local_z_intervals()) {
            if (interval.layer_id == 0) {
                CHECK(interval.mix_bridges.empty());
                continue;
            }
            REQUIRE(interval.mix_bridges.size() == 1);
            const auto& bridge = *interval.mix_bridges.front();
            CHECK(bridge.zones.size() == (wide_ratios ? 7 : 3));
            if (base_contact) {
                CHECK(std::any_of(bridge.zones.begin(), bridge.zones.end(),
                                  [](const auto& zone) { return zone.source_filament_id == 2 && zone.join_terminal_pass; }));
                for (const auto& zone : bridge.zones)
                    if (!zone.preserve_nominal_layers)
                        CHECK(zone.component_ids.back() == 2);
            }
            if (wide_tail) {
                CHECK(bridge.z_hi - bridge.z_lo <= .56 + 1e-6);
                REQUIRE(bridge.split_runs.size() == 1);
                CHECK(bridge.split_runs.front().cap >= .24);
            }
            CHECK(bridge.passes.size() == (wide_ratios ? (wide_tail ? size_t(std::lround((bridge.z_hi - bridge.z_lo) / .08)) + 2 : 8) :
                                           coarse_cross_interval               ? 3 :
                                           cross_interval                      ? 5 :
                                           bridge.z_hi < .5 || asymmetric_pure ? 4 :
                                                                                 6));
        }
    }
    std::map<std::pair<size_t, size_t>, std::pair<size_t, double>> previous_bridge_pass;
    for (const auto& plan : plans) {
        if (!plan.mix_bridge)
            continue;
        const auto key      = std::make_pair(plan.layer_id, plan.dependency_group);
        const auto previous = previous_bridge_pass.find(key);
        if (previous != previous_bridge_pass.end()) {
            CHECK(plan.dependency_order > previous->second.first);
            CHECK(plan.print_z >= previous->second.second - 1e-6);
        }
        CHECK(plan.mix_bridge->passes[plan.mix_bridge_component].ceiling >= plan.print_z - 1e-6);
        previous_bridge_pass[key] = {plan.dependency_order, plan.print_z};
    }
    for (const auto& plan : plans)
        if (plan.split_interval)
            for (size_t tool = 0; tool < plan.painted_masks_by_extruder.size(); ++tool)
                if (!plan.painted_masks_by_extruder[tool].empty()) {
                    const auto& registered = print.tool_ordering().all_extruders();
                    CHECK(std::find(registered.begin(), registered.end(), unsigned(tool)) != registered.end());
                }
    if (unrelated || (enabled && !compatible)) {
        BridgeEnvironment disabled("0");
        Print             reference;
        reference.is_BBL_printer() = false;
        reference.set_status_silent();
        reference.apply(model, config);
        reference.process();
        const auto fallback = [unrelated](const std::vector<SubLayerPlan>& source) {
            std::vector<const SubLayerPlan*> result;
            for (const auto& plan : source)
                if (!unrelated || (!plan.mix_bridge && plan.dependency_group == 3))
                    result.push_back(&plan);
            return result;
        };
        const auto actual   = fallback(plans);
        const auto expected = fallback(reference.get_object(0)->local_z_sublayer_plan());
        REQUIRE_FALSE(actual.empty());
        REQUIRE(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            CHECK(actual[i]->print_z == expected[i]->print_z);
            CHECK(actual[i]->z_lo == expected[i]->z_lo);
            CHECK(actual[i]->z_hi == expected[i]->z_hi);
            CHECK(actual[i]->flow_height == expected[i]->flow_height);
            CHECK(actual[i]->dependency_order == expected[i]->dependency_order);
            CHECK(actual[i]->painted_masks_by_extruder == expected[i]->painted_masks_by_extruder);
        }
    }
    if (enabled && compatible) {
        // A shared perimeter must not absorb the other recipe's visible top
        // skin, infill ownership, or slices used for recipe phase tracking.
        for (const Layer* layer : print.get_object(0)->layers()) {
            if (layer->id() == 0)
                continue;
            for (int source_filament = 4; source_filament < int(4 + definitions.size()); ++source_filament) {
                if (unrelated && source_filament == 6)
                    continue;
                if (changing_connectivity && layer->id() > 1 && source_filament == 5)
                    continue;
                double     slice_area = 0., fill_area = 0.;
                ExPolygons source_fill;
                for (const LayerRegion* region : layer->regions()) {
                    if (region->region().config().wall_filament.value == source_filament) {
                        slice_area += area(to_expolygons(region->slices.surfaces));
                        fill_area += area(to_expolygons(region->fill_surfaces.surfaces));
                        append(source_fill, to_expolygons(region->fill_surfaces.surfaces));
                    }
                }
                // Fill.cpp may combine generated paths when the recipes
                // resolve to the same physical fill tool on this layer. The
                // source surfaces must survive and actually receive fill.
                Polylines fill_paths;
                for (const LayerRegion* region : layer->regions())
                    region->fills.collect_polylines(fill_paths);
                CAPTURE(layer->id(), source_filament, slice_area, fill_area);
                if (intermittent_paint && slice_area == 0.)
                    continue;
                CHECK(slice_area > 0.);
                CHECK(fill_area > 0.);
                CHECK_FALSE(intersection_pl(fill_paths, source_fill).empty());
            }
        }
    }

    struct TemporaryGCode
    {
        boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("bridge-%%%%-%%%%.gcode");
        ~TemporaryGCode()
        {
            boost::system::error_code ec;
            boost::filesystem::remove(path, ec);
        }
    } temporary;
    GCodeProcessorResult result;
    print.export_gcode(temporary.path.string(), &result, nullptr);
    bool                  continuous_ramp = false;
    bool                  interior        = false;
    std::map<int, double> tower_lengths;
    double                run_length = 0., run_min_z = 1e9, run_max_z = -1e9;
    size_t                pure_moves  = 0;
    double                front_min_x = 1e9, front_max_x = -1e9;
    bool                  joined_final_color = false;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& move = result.moves[i];
        if (move.type == EMoveType::Extrude && move.extrusion_role == erExternalPerimeter && move.extruder_id == 1 &&
            move.position.z() > .201 && std::abs(move.position.y() - 100.225) < .01 &&
            std::abs(result.moves[i - 1].position.y() - 100.225) < .01) {
            front_min_x = std::min({front_min_x, double(move.position.x()), double(result.moves[i - 1].position.x())});
            front_max_x = std::max({front_max_x, double(move.position.x()), double(result.moves[i - 1].position.x())});
            joined_final_color |= front_min_x < 103.9 && front_max_x > 104.1;
        } else {
            front_min_x = 1e9;
            front_max_x = -1e9;
        }
        if (move.type == EMoveType::Extrude && move.extrusion_role == erWipeTower)
            tower_lengths[int(std::lround(move.position.z() * 1000.))] += (move.position - result.moves[i - 1].position).head<2>().norm();
        if (move.type == EMoveType::Extrude && move.position.z() > nominal + .01 && move.extrusion_role == erSolidInfill)
            interior = true;
        if (move.type != EMoveType::Extrude || move.extrusion_role != erExternalPerimeter || move.position.z() < nominal + .01) {
            run_length = 0.;
            run_min_z  = 1e9;
            run_max_z  = -1e9;
            continue;
        }
        const double length = (move.position - result.moves[i - 1].position).head<2>().norm();
        if (fine_cycles && move.position.x() < 99. && result.moves[i - 1].position.x() < 99. && move.position.z() > .201) {
            ++pure_moves;
            CHECK(move.extruder_id == (base_contact ? 1 : 0));
            CHECK(move.height == Approx(nominal).margin(1e-5));
            CHECK(move.position.z() == Approx(result.moves[i - 1].position.z()).margin(1e-5));
            CHECK((move.position.z() - .2) / nominal == Approx(std::round((move.position.z() - .2) / nominal)).margin(.001));
        }
        run_length += length;
        run_min_z = std::min(run_min_z, double(move.position.z()));
        run_max_z = std::max(run_max_z, double(move.position.z()));
        if (enabled && compatible && !unrelated && length > 1e-6)
            CHECK(std::abs(move.position.z() - result.moves[i - 1].position.z()) <= .12 * length + .002);
        continuous_ramp |= run_length > 5. && run_max_z - run_min_z > (fine_cycles ? .001 : .05);
    }
    CHECK(interior);
    if (fine_cycles)
        CHECK(pure_moves > 0);
    CHECK(continuous_ramp == (enabled && compatible));
    if (cross_interval)
        CHECK(joined_final_color);
    const auto& tower = print.wipe_tower_data();
    REQUIRE(tower.tool_changes.size() == tower.local_z_tool_changes.size());
    for (size_t layer = 0; layer < tower.tool_changes.size(); ++layer) {
        REQUIRE_FALSE(tower.tool_changes[layer].empty());
        double expected_length = 0.;
        for (const auto* changes : {&tower.tool_changes[layer], &tower.local_z_tool_changes[layer]})
            for (const auto& change : *changes)
                for (size_t i = 1; i < change.extrusions.size(); ++i)
                    if (change.extrusions[i].width > 0.f)
                        expected_length += (change.extrusions[i].pos - change.extrusions[i - 1].pos).norm();
        const int z_key = int(std::lround(tower.tool_changes[layer].front().print_z * 1000.));
        CHECK(tower_lengths[z_key] == Approx(expected_length).margin(.5));
    }
}

TEST_CASE("Rejected sparse joins rebuild the complete authored recipe", "[LocalZ][MixBridge][SparseMixBridge]")
{
    std::vector<LocalZMixBridge::Zone> zones{{rectangle(-4., 0.), {.1, .1}, {1, 2}, 4}, {rectangle(0., 4.), {.06, .06, .08}, {1, 2, 3}, 5}};
    auto                               joined = LocalZMixBridge::build(.2, .4, .06, {.32, .32, .32}, zones, {.2});
    REQUIRE(joined.size() == 1);
    REQUIRE(joined.front().extra_colors.size() == 1);
    REQUIRE(joined.front().authored_zones.size() == zones.size());
    for (const auto& recipe : joined.front().authored_zones) {
        auto fallback = LocalZMixBridge::build(.2, .4, .06, {.32, .32, .32}, {recipe});
        REQUIRE(fallback.size() == 1);
        for (size_t i = 0; i < recipe.component_ids.size(); ++i) {
            double height = 0.;
            for (const auto& pass : fallback.front().passes)
                if (pass.filament_id == recipe.component_ids[i]) {
                    const auto z = fallback.front().surfaces(Point(scale_(recipe.source_filament_id == 4 ? -2. : 2.), 0.),
                                                             size_t(&pass - fallback.front().passes.data()));
                    height += z.second - z.first;
                }
            CHECK(height == Approx(recipe.heights[i]));
        }
    }
}

TEST_CASE("Fallback color debt follows volume when footprints change", "[LocalZ][MixBridge][CurvedMixBridge]")
{
    std::vector<double> error, deposited(2, 0.);
    double              total = 0.;
    for (size_t cycle = 0; cycle < 1000; ++cycle) {
        const double footprint = cycle % 2 ? 5. : 95.;
        for (const auto& [id, height] : LocalZMixBridge::allocate_flat_layers({.5, .5}, .08, .06, {.12, .12}, error, footprint)) {
            CHECK(height >= .06 - 1e-6);
            CHECK(height <= .12 + 1e-6);
            deposited[id - 1] += height * footprint;
        }
        total += .08 * footprint;
        for (size_t i = 0; i < error.size(); ++i)
            CHECK(error[i] == Approx(total * .5 - deposited[i]).margin(1e-7));
    }
    CHECK(deposited[0] / total == Approx(.5).margin(.002));
    CHECK(deposited[1] / total == Approx(.5).margin(.002));
    CHECK_THROWS(LocalZMixBridge::allocate_flat_layers({.5, .5}, .2, .06, {.12, .12}, error, 0.));
}

TEST_CASE("Nominal bands align with exact stacks without losing volume debt", "[LocalZ][MixBridge][FallbackCadence]")
{
    std::vector<std::pair<unsigned, double>> beads{{1, .08}, {1, .08}, {3, .08}, {1, .08}, {3, .08}};
    const auto original = beads;
    const std::vector<double> areas{25., 30., 35., 20., 15.};
    std::vector<double> error{.2, -.1, -.1};
    const auto old_error = error;
    REQUIRE(LocalZMixBridge::align_flat_layers(beads, areas, {3, 1}, {.32, .32, .32}, error));
    const std::vector<std::pair<unsigned, double>> expected{{3, .08}, {3, .08}, {1, .08}, {1, .08}, {1, .08}};
    CHECK(beads == expected);
    std::vector<double> before(3, 0.), after(3, 0.);
    for (size_t i = 0; i < beads.size(); ++i) {
        before[original[i].first - 1] += areas[i] * original[i].second;
        after[beads[i].first - 1] += areas[i] * beads[i].second;
    }
    for (size_t tool = 0; tool < error.size(); ++tool)
        CHECK(error[tool] == Approx(old_error[tool] + before[tool] - after[tool]).margin(1e-9));

    SECTION("A moved tool cannot exceed its cap") {
        beads = {{1, .08}, {2, .2}};
        const auto unchanged = beads;
        const auto unchanged_error = error;
        REQUIRE_FALSE(LocalZMixBridge::align_flat_layers(beads, {1., 2.}, {2, 1}, {.1, .32, .32}, error));
        CHECK(beads == unchanged);
        CHECK(error == unchanged_error);
    }
    SECTION("Interleaved exact stacks keep their existing cadence") {
        REQUIRE_FALSE(LocalZMixBridge::align_flat_layers(beads, areas, {1, 3, 1}, {.32, .32, .32}, error));
        CHECK(beads == expected);
    }
}

TEST_CASE("Aligned nominal bands retain long-run color volume on changing footprints", "[LocalZ][MixBridge][FallbackCadence]")
{
    bool changing = false, growing = false;
    SECTION("Constant footprint") {}
    SECTION("Strongly varying footprint") { changing = true; }
    SECTION("A steadily growing footprint") { growing = true; }
    const std::vector<double> weights{.66, .17, .17}, maximum{.32, .32, .32};
    std::vector<double> error, deposited(3, 0.);
    double volume = 0.;
    for (size_t cycle = 0; cycle < 1000; ++cycle) {
        std::vector<std::pair<unsigned, double>> beads;
        std::vector<double> areas;
        for (size_t layer = 0; layer < 5; ++layer) {
            const double footprint = growing ? 1. + .02 * (5 * cycle + layer) : changing ? ((layer + cycle) % 2 ? 5. : 95.) : 10.;
            const auto allocated = LocalZMixBridge::allocate_flat_layers(weights, .08, .06, maximum, error, footprint);
            REQUIRE(allocated.size() == 1);
            beads.push_back(allocated.front());
            areas.push_back(footprint);
            volume += footprint * .08;
        }
        REQUIRE(LocalZMixBridge::align_flat_layers(beads, areas, {2, 3, 1}, maximum, error));
        size_t previous_rank = 0;
        for (size_t i = 0; i < beads.size(); ++i) {
            const auto [tool, height] = beads[i];
            const size_t rank = tool == 1 ? 2 : tool - 2;
            CHECK(rank >= previous_rank);
            previous_rank = rank;
            CHECK(height == Approx(.08));
            deposited[tool - 1] += areas[i] * height;
        }
        for (size_t tool = 0; tool < error.size(); ++tool)
            CHECK(error[tool] == Approx(volume * weights[tool] - deposited[tool]).margin(1e-7));
    }
    for (size_t tool = 0; tool < error.size(); ++tool)
        CHECK(deposited[tool] / volume == Approx(weights[tool]).margin(.004));
}

TEST_CASE("Local recipes repeat at their own printable minimum", "[LocalZ][MixBridge][LocalCadence]")
{
    const std::vector<std::pair<unsigned, double>> recipe{{1, .16}, {2, .24}};
    const auto                                     repeated = LocalZMixBridge::repeat_flat_layers(recipe, .06, {.32, .32});
    REQUIRE(repeated.size() == 4);
    for (size_t i = 0; i < repeated.size(); ++i) {
        CHECK(repeated[i].first == recipe[i % 2].first);
        CHECK(repeated[i].second == Approx(recipe[i % 2].second * .5));
    }
    const std::vector<std::pair<unsigned, double>> rare{{1, .06}, {2, .28}, {3, .06}};
    CHECK(LocalZMixBridge::repeat_flat_layers(rare, .06, {.32, .32, .32}) == rare);
    CHECK(LocalZMixBridge::repeat_flat_layers(recipe, .08, {.32, .32}) == repeated);
    CHECK(LocalZMixBridge::repeat_flat_layers({{1, .32}}, .06, {.32}).size() == 1);
    const std::vector<std::pair<unsigned, double>> same_ends{{1, .12}, {2, .16}, {1, .12}};
    CHECK(LocalZMixBridge::repeat_flat_layers(same_ends, .06, {.32, .32}) == same_ends);
    CHECK_THROWS(LocalZMixBridge::repeat_flat_layers(recipe, 0., {.32, .32}));
    CHECK_THROWS(LocalZMixBridge::repeat_flat_layers(recipe, .06, {.32, .12}));
}

TEST_CASE("Repeated nominal cadence matches physical heights and preserves color debt", "[LocalZ][MixBridge][LocalCadence]")
{
    const std::vector<std::pair<unsigned, double>> cadence{{1, .08}, {2, .12}, {1, .08}, {2, .12}};
    std::vector<std::pair<unsigned, double>>       beads{{1, .08}, {1, .08}, {2, .08}, {2, .08}, {2, .08}};
    std::vector<double>                            starts{0., .08, .16, .24, .32}, areas{10., 20., 30., 40., 50.}, error{.2, -.2};
    const auto                                     original       = beads;
    const auto                                     previous_error = error;
    auto                                           agreement      = [&](const auto& candidate) {
        double result = 0.;
        for (size_t i = 0; i < candidate.size(); ++i) {
            double bottom = 0.;
            for (const auto& [tool, height] : cadence) {
                if (tool == candidate[i].first)
                    result += areas[i] *
                              std::max(0., std::min(starts[i] + candidate[i].second, bottom + height) - std::max(starts[i], bottom));
                bottom += height;
            }
        }
        return result;
    };
    double best = 0.;
    do {
        best = std::max(best, agreement(beads));
    } while (std::next_permutation(beads.begin(), beads.end()));
    beads = original;
    REQUIRE(LocalZMixBridge::align_flat_layers_to_cadence(beads, areas, starts, cadence, {.32, .32}, error));
    CHECK(agreement(beads) == Approx(best));
    CHECK(beads.front().first == 1);
    CHECK(beads.back().first == 2);
    for (unsigned tool : {1u, 2u}) {
        double change = 0.;
        for (size_t i = 0; i < beads.size(); ++i) {
            CHECK(beads[i].second == original[i].second);
            change += areas[i] * beads[i].second * (int(original[i].first == tool) - int(beads[i].first == tool));
        }
        CHECK(error[tool - 1] == Approx(previous_error[tool - 1] + change));
    }
    SECTION("Missing nominal slots retain their real Z positions")
    {
        beads = {{2, .08}, {1, .08}};
        REQUIRE(LocalZMixBridge::align_flat_layers_to_cadence(beads, {1., 1.}, {0., .32}, cadence, {.32, .32}, error));
        CHECK(beads[0].first == 1);
        CHECK(beads[1].first == 2);
    }
    SECTION("An impossible cap leaves both colors and debt untouched")
    {
        const auto before = beads;
        const auto debt   = error;
        CHECK_FALSE(LocalZMixBridge::align_flat_layers_to_cadence(beads, areas, starts, cadence, {.04, .32}, error));
        CHECK(beads == before);
        CHECK(error == debt);
    }
}

TEST_CASE("Repeated fallback cadence carries volume on moving footprints", "[LocalZ][MixBridge][LocalCadence]")
{
    const std::vector<double> weights{.4, .6}, maximum{.32, .32}, starts{0., .08, .16, .24, .32};
    const auto                cadence = LocalZMixBridge::repeat_flat_layers({{1, .16}, {2, .24}}, .06, maximum);
    std::vector<double>       error, deposited(2, 0.);
    double                    volume = 0.;
    for (size_t cycle = 0; cycle < 1000; ++cycle) {
        std::vector<std::pair<unsigned, double>> beads;
        std::vector<double>                      areas;
        for (size_t layer = 0; layer < 5; ++layer) {
            const double footprint = (cycle + layer) % 2 ? 5. : 95.;
            const auto   allocated = LocalZMixBridge::allocate_flat_layers(weights, .08, .06, maximum, error, footprint);
            REQUIRE(allocated.size() == 1);
            beads.push_back(allocated.front());
            areas.push_back(footprint);
            volume += footprint * .08;
        }
        REQUIRE(LocalZMixBridge::align_flat_layers_to_cadence(beads, areas, starts, cadence, maximum, error));
        for (size_t i = 0; i < beads.size(); ++i)
            deposited[beads[i].first - 1] += areas[i] * beads[i].second;
        for (size_t tool = 0; tool < error.size(); ++tool)
            CHECK(error[tool] == Approx(volume * weights[tool] - deposited[tool]).margin(1e-7));
    }
    for (size_t tool = 0; tool < error.size(); ++tool)
        CHECK(deposited[tool] / volume == Approx(weights[tool]).margin(.004));
}

TEST_CASE("Connected stacks choose shorter cycles without a distant rare recipe", "[LocalZ][MixBridge][LocalCadence]")
{
    const std::vector<double> maximum{.32, .32, .32};
    const auto                groups = LocalZMixBridge::build(.2, .6, .06, maximum,
                                                              {{rectangle(-8., -4.), {.16, .24}, {1, 2}, 4},
                                                               {rectangle(-4., 0.), {.2, .2}, {1, 2}, 5},
                                                               {rectangle(4., 8.), {.06, .28, .06}, {1, 2, 3}, 6}},
                                                              {.08, .08, .08, .08, .08});
    REQUIRE(groups.size() == 2);
    for (const auto& group : groups) {
        const auto local = group.shorter_cycles({.08, .08, .08, .08, .08}, maximum, {});
        if (group.source_filament_ids.size() == 1) {
            CHECK(local.empty());
            continue;
        }
        REQUIRE(local.size() == 2);
        CHECK(local.front().z_lo == Approx(.2));
        CHECK(local.back().z_hi == Approx(.6));
        CHECK(local.front().z_hi == Approx(local.back().z_lo));
        for (const auto& cycle : local) {
            CHECK(cycle.z_hi - cycle.z_lo <= .24 + 1e-6);
            CHECK(cycle.source_filament_ids == group.source_filament_ids);
            CHECK(area(diff_ex(cycle.ownership(size_t(-1)), group.ownership(size_t(-1)))) == Approx(0.));
            for (const auto& zone : cycle.zones)
                for (double height : zone.heights)
                    CHECK(height >= .06 - 1e-6);
        }
        for (size_t zone = 0; zone < group.zones.size(); ++zone)
            for (size_t component = 0; component < group.zones[zone].heights.size(); ++component) {
                double height = 0.;
                for (const auto& cycle : local)
                    height += cycle.zones[zone].heights[component];
                CHECK(height == Approx(group.zones[zone].heights[component]));
            }
        auto pinned                = group;
        pinned.boundary_interfaces = {.2, .36, .6};
        CHECK(pinned.shorter_cycles({.08, .08, .08, .08, .08}, maximum, {}).empty());
    }
}

TEST_CASE("Shorter surface cycles validate all children before replacing the parent", "[LocalZ][MixBridge][LocalCadence]")
{
    auto groups = LocalZMixBridge::build(.2, 1., .06, {.5, .5},
                                         {{rectangle(-4., 0.), {.4, .4}, {1, 2}, 3}, {rectangle(0., 4.), {.4, .4}, {1, 2}, 4}},
                                         {.2, .2, .2, .2});
    REQUIRE(groups.size() == 1);
    auto& group               = groups.front();
    group.follows_surface     = true;
    group.boundary_tolerance  = .1;
    group.lower_support_masks = rectangle(-4., 4.);
    for (size_t i = 0; i < 4; ++i)
        group.layer_footprints.push_back({i + 1, rectangle(-4. + .01 * i, 4. - .01 * i), {}, .3 + .2 * i, .2});
    REQUIRE(group.fit_contours(.1));
    SECTION("Off-plane children retain the complete contour sampling context")
    {
        const auto local = group.shorter_cycles({.2, .2, .2, .2}, {.5, .5}, {});
        REQUIRE(local.size() == 6);
        for (const auto& cycle : local) {
            CHECK(cycle.layer_footprints.size() == 4);
            CHECK(cycle.z_hi - cycle.z_lo == Approx(.8 / 6));
            for (const auto& pass : cycle.passes)
                CHECK(pass.contour_fitted);
        }
    }
    SECTION("Failure in a later child discards the entire replacement")
    {
        group.layer_footprints[2].masks.clear();
        CHECK(group.shorter_cycles({.2, .2, .2, .2}, {.5, .5}, {}).empty());
        CHECK(group.z_lo == Approx(.2));
        CHECK(group.z_hi == Approx(1.));
        CHECK(group.layer_footprints.size() == 4);
    }
}

TEST_CASE("Pinned 33-67 stacks repeat across nominal planes exactly once", "[LocalZ][MixBridge][LocalCadence]")
{
    const std::vector<double> maximum{.32, .32};
    auto groups = LocalZMixBridge::build(.2, .6, .06, maximum, {{rectangle(-4., 0.), {.132, .268}, {2, 1}, 7}}, {.08, .08, .08, .08, .08},
                                         1., .4, {1, 2});
    REQUIRE(groups.size() == 1);
    auto&      parent    = groups.front();
    const auto neighbors = rectangle(0., 4.);
    REQUIRE(parent.pin_foreign_boundary(neighbors, .4));
    parent.follows_surface     = true;
    parent.boundary_tolerance  = .1;
    parent.lower_support_masks = rectangle(-4., 0.);
    for (size_t i = 0; i < 5; ++i)
        parent.layer_footprints.push_back({i + 1, rectangle(-4., 0.), {}, .24 + .08 * i, .08});
    REQUIRE(parent.fit_contours(.1));
    CHECK(parent.shorter_cycles({.08, .08, .08, .08, .08}, maximum, {1, 2}).empty());
    const auto children = parent.shorter_cycles({.08, .08, .08, .08, .08}, maximum, {1, 2}, 1., neighbors);
    REQUIRE(children.size() == 2);
    double              previous_key = .2;
    std::vector<size_t> owners(5, 0);
    for (size_t child_index = 0; child_index < children.size(); ++child_index) {
        const auto& child = children[child_index];
        CHECK(child.z_lo == Approx(.2 + .2 * child_index));
        CHECK(child.z_hi == Approx(.4 + .2 * child_index));
        CHECK(child.skin_range() == parent.skin_range());
        CHECK(child.boundary_anchor > child.z_lo);
        CHECK(child.shares_cadence(.2, .6, 2, {2, 1}));
        CHECK_FALSE(child.shares_cadence(.2, .6, 1, {2, 1}));
        CHECK_FALSE(child.shares_cadence(.2, .6, 2, {1, 2}));
        CHECK_FALSE(child.shares_cadence(.21, .61, 2, {2, 1}));
        REQUIRE(child.zones.front().heights.size() == 2);
        CHECK(child.zones.front().heights[0] == Approx(.066));
        CHECK(child.zones.front().heights[1] == Approx(.134));
        for (size_t i = 0; i < 5; ++i) {
            const double lo = .2 + .08 * i, hi = lo + .08;
            if (lo < child.z_hi - 1e-6 && hi > child.z_lo + 1e-6) {
                ++owners[i];
                CHECK(area(diff_ex(parent.ownership(i + 1), child.ownership(i + 1))) == Approx(0.));
            }
        }
        for (const auto& pass : child.passes) {
            REQUIRE(pass.contour_fitted);
            const double key = child.schedule_z(pass);
            CHECK(key > previous_key + .06 - 1e-6);
            previous_key   = key;
            size_t emitted = 0;
            for (size_t i = 0; i < 5; ++i) {
                const double lo = .2 + .08 * i, hi = lo + .08;
                if (lo < child.z_hi - 1e-6 && hi > child.z_lo + 1e-6 && key > lo + 1e-6 && key <= hi + 1e-6)
                    ++emitted;
            }
            CHECK(emitted == 1);
        }
    }
    CHECK(owners == std::vector<size_t>{1, 1, 2, 1, 1});
    CHECK_FALSE(parent.shares_cadence(.2, .6, 2, {2, 1}));
    CHECK(LocalZMixBridge::repeat_flat_layers({{2, .132}, {1, .268}}, .06, maximum, 1).size() == 2);
}

TEST_CASE("Foreign flat repeats can interleave with pinned joined planes", "[LocalZ][MixBridge][LocalCadence]")
{
    auto groups = LocalZMixBridge::build(.2, .6, .06, {.32, .32, .32},
                                         {{rectangle(-8., -4.), {.2, .2}, {2, 1}, 6}, {rectangle(-4., 0.), {.132, .268}, {2, 1}, 7}}, {},
                                         1., .4, {1, 2, 3});
    REQUIRE(groups.size() == 1);
    const auto children = groups.front().shorter_cycles({.08, .08, .08, .08, .08}, {.32, .32, .32}, {1, 2, 3}, 1., rectangle(0., 4.));
    REQUIRE(children.size() == 2);
    std::vector<std::pair<double, double>> events;
    for (const auto& child : children) {
        REQUIRE(child.boundary_anchor > 0.);
        for (size_t i = 0; i < child.passes.size(); ++i) {
            const double key = child.schedule_z(child.passes[i]);
            for (double x : {-.001, -.1, -.39}) {
                const double top = child.surfaces(Point::new_scale(x, 0.), i).second;
                CHECK(top == Approx(key));
                events.emplace_back(key, top);
            }
        }
    }
    // A different recipe repeats three times while the joined group repeats
    // twice. Its flat tops are scheduling keys, independent of material order.
    double     z       = .2;
    const auto foreign = LocalZMixBridge::repeat_flat_layers({{3, .2}, {1, .2}}, .06, {.32, .32, .32});
    REQUIRE(foreign.size() == 6);
    for (const auto& [tool, height] : foreign) {
        z += height;
        events.emplace_back(z, z);
    }
    std::sort(events.begin(), events.end());
    double deposited = .2;
    for (const auto& [key, top] : events) {
        CHECK(top >= deposited - 1e-6);
        deposited = std::max(deposited, top);
    }
}

TEST_CASE("Joined islands retain modifiers, bounded strips and incremental infill", "[LocalZ][MixBridge][CurvedMixBridge]")
{
    BridgeEnvironment environment("1");
    bool bounded = false, toggle = false, initially_enabled = true, travel = false, hop = false, disabled = false, repeat = false, skins = false;
    SECTION("Infill modifiers") {}
    SECTION("Nominal bands split at physical height caps") { bounded = true; }
    SECTION("Infill enabled incrementally")
    {
        toggle            = true;
        initially_enabled = false;
    }
    SECTION("Infill disabled incrementally") { toggle = true; }
    SECTION("Travel clearance without retraction or Z hop") { travel = true; }
    SECTION("Travel clearance with spiral hop and wipe")
    {
        travel = true;
        hop    = true;
    }
    SECTION("Saved project switch disables joining") { disabled = true; }
    SECTION("Repeated slices retain the same island paths") { repeat = true; }
    SECTION("Only exterior beads use top skin settings") { skins = true; }
    const std::vector<std::string>       colors{"#FF00FF", "#00FFFF"};
    std::vector<MixedFilamentDefinition> definitions(2);
    for (size_t i = 0; i < definitions.size(); ++i) {
        auto& recipe                   = definitions[i];
        recipe.identity.stable_id      = 9701 + i;
        recipe.source.kind             = MixedFilamentSourceKind::Custom;
        recipe.behavior.distribution   = MixedFilamentDistributionMode::LayerCycle;
        recipe.recipe.blend.components = {{{1}, i == 0 ? 65 : 33}, {{2}, i == 0 ? 35 : 67}};
    }
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions(definitions, colors);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("mixed_filament_region_collapse", false);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("layer_height", bounded ? .2 : .08);
    config.set("initial_layer_print_height", .2);
    config.set("wall_loops", 2);
    for (const auto* key : {"dithering_local_z_mode", "dithering_local_z_whole_objects", "dithering_local_z_preserve_first_layer",
                            "dithering_local_z_direct_multicolor", "dithering_local_z_independent_layer_height", "dithering_local_z_infill"})
        config.set(key, true);
    config.set("enable_prime_tower", false);
    config.set("wipe_tower_no_sparse_layers", false);
    config.set("prime_volume", 15.);
    config.set("single_extruder_multi_material", false);
    config.set("machine_start_gcode", "G90\nM83\n");
    config.option<ConfigOptionPercent>("sparse_infill_density")->value = 100.;
    config.option<ConfigOptionStrings>("filament_colour")->values      = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values     = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values       = {.4, .4};
    config.option<ConfigOptionFloats>("max_layer_height")->values = bounded ? std::vector<double>{.12, .12} : std::vector<double>{.32, .32};
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(4, 100.);
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                            "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));
    for (const auto& key : config.keys())
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    config.set("top_shell_layers", skins ? 3 : 0);
    config.set("bottom_shell_layers", skins ? 3 : 0);
    config.option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value = ipRectilinear;
    config.set("dithering_local_z_infill", initially_enabled);
    config.set("dithering_local_z_mix_bridges", !disabled);
    if (travel) {
        config.set("gcode_comments", true);
        config.set("reduce_crossing_wall", hop);
        config.option<ConfigOptionFloats>("z_hop")->values                     = {hop ? .4 : 0., hop ? .4 : 0.};
        config.option<ConfigOptionFloats>("retraction_minimum_travel")->values = {hop ? 0. : 1000., hop ? 0. : 1000.};
        config.option<ConfigOptionBools>("wipe")->values                       = {hop, hop};
        config.option<ConfigOptionEnumsGeneric>("z_hop_types")->values         = {int(ZHopType::zhtSpiral), int(ZHopType::zhtSpiral)};
    }
    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 1);
    const std::vector<double> cuts{0., 1.68};
    for (size_t half = 0; half < 2; ++half)
        for (size_t part = 1; part < cuts.size(); ++part) {
            auto mesh = make_cube(4., 8., cuts[part] - cuts[part - 1]);
            if (true) {
                Matrix3d shear = Matrix3d::Identity();
                shear(0, 2)    = .2;
                mesh.transform(shear);
            }
            auto* volume = object->add_volume(std::move(mesh));
            volume->translate(Vec3d(4. * half, 0., cuts[part - 1]));
            volume->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(half == 0 ? 15. : 85.));
            TriangleSelector selector(volume->mesh());
            for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
                selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(3 + half));
            volume->mmu_segmentation_facets.set(selector);
        }
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    print.process();

    if (disabled) {
        for (const auto& plan : print.get_object(0)->local_z_sublayer_plan())
            CHECK_FALSE(plan.mix_bridge);
        return;
    }
    auto fills = [](const Print& job) {
        size_t count = 0;
        for (const auto* object : job.objects())
            for (const auto& plan : object->local_z_sublayer_plan())
                if (plan.mix_bridge_paths)
                    count += plan.mix_bridge_paths->fills.entities.size();
        return count;
    };
    if (toggle) {
        CHECK((fills(print) > 0) == initially_enabled);
        config = print.full_print_config();
        config.set("dithering_local_z_infill", !initially_enabled);
        print.apply(model, config);
        print.process();
        Print fresh;
        fresh.set_plate_origin(Vec3d::Zero());
        fresh.is_BBL_printer() = false;
        fresh.set_status_silent();
        fresh.apply(model, config);
        fresh.process();
        CHECK(fills(print) == fills(fresh));
        CHECK((fills(print) > 0) == !initially_enabled);
        return;
    }
    auto*                                                 sliced = print.get_object(0);
    std::map<const LocalZMixBridge*, std::vector<size_t>> emissions;
    for (const auto& interval : sliced->local_z_intervals())
        for (const auto& bridge : interval.mix_bridges)
            emissions.try_emplace(bridge.get(), bridge->passes.size(), 0);
    size_t      bands = 0, following = 0, joined = 0, top_paths = 0;
    for (const auto& plan : sliced->local_z_sublayer_plan()) {
        if (!plan.mix_bridge)
            continue;
        const auto& bridge = *plan.mix_bridge;
        const auto& pass   = bridge.passes[plan.mix_bridge_component];
        ++emissions.at(&bridge).at(plan.mix_bridge_component);
        if (bridge.follows_surface) {
            ++following;
            for (const auto& line : plan.mix_bridge_paths->perimeters.as_polylines())
                for (const auto& point : line.points) {
                    const auto [lo, hi] = bridge.surfaces(point, plan.mix_bridge_component);
                    CHECK(hi - lo >= .06 - 1e-6);
                    CHECK(hi - lo <= (bounded ? .12 : .32) + 1e-6);
                }
        }
        if (bridge.nominal_band) {
            ++bands;
            CHECK(pass.flat_height >= .06 - 1e-6);
            CHECK(pass.flat_height <= (bounded ? .12 : .32) + 1e-6);
        }
        if (!plan.mix_bridge_paths || bridge.planar_fallback || pass.zones.size() < 2)
            continue;
        const auto& paths = *plan.mix_bridge_paths;
        REQUIRE(paths.fill_region_ids.size() == paths.fills.entities.size());
        std::function<void(const ExtrusionEntity&)> check_roles = [&](const ExtrusionEntity& entity) {
            if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
                for (const auto* child : collection->entities)
                    check_roles(*child);
            } else if (entity.role() == erTopSolidInfill) {
                ++top_paths;
                CHECK(pass.ceiling == Approx(bridge.z_hi));
                CHECK(bridge.z_hi == Approx(sliced->layers().back()->print_z));
            }
        };
        check_roles(paths.fills);
        double lengths[2] = {0., 0.};
        for (size_t i = 0; i < paths.fills.entities.size(); ++i) {
            const auto*  region  = sliced->get_layer(int(plan.layer_id))->regions().at(paths.fill_region_ids[i]);
            const double density = region->region().config().sparse_infill_density.value;
            REQUIRE((density == 15. || density == 85.));
            const auto lines = paths.fills.entities[i]->as_polylines();
            for (const auto& line : lines)
                lengths[density == 15. ? 0 : 1] += unscale<double>(line.length());
            auto owner_config                = region->region().config();
            owner_config.wall_filament.value = owner_config.sparse_infill_filament.value = owner_config.solid_infill_filament.value = int(
                pass.filament_id);
            ExPolygons owner_masks;
            for (const auto* candidate : sliced->get_layer(int(plan.layer_id))->regions()) {
                auto candidate_config                            = candidate->region().config();
                candidate_config.wall_filament.value             = candidate_config.sparse_infill_filament.value =
                    candidate_config.solid_infill_filament.value = int(pass.filament_id);
                if (candidate_config == owner_config)
                    append(owner_masks, to_expolygons(candidate->slices.surfaces));
            }
            CHECK(diff_pl(lines, offset_ex(union_ex(owner_masks), scale_(.001))).empty());
        }
        if (lengths[0] > 0. && lengths[1] > 0.) {
            ++joined;
            if (!skins)
                CHECK(lengths[1] > 2. * lengths[0]);
        }
    }
    REQUIRE(bands + following > 0);
    for (const auto& [bridge, counts] : emissions)
        for (size_t count : counts)
            CHECK(count == 1);
    if (!bounded)
        REQUIRE(joined > 0);
    if (skins) {
        REQUIRE(top_paths > 0);
        // Force a repeat through the final roof envelope independently of the
        // global cadence chooser, which selects a shorter block for this fixture.
        auto&      roof = *sliced->layers().back();
        ExPolygons masks;
        for (const auto* region : roof.regions())
            append(masks, to_expolygons(region->slices.surfaces));
        auto parents = LocalZMixBridge::build(roof.print_z - .4, roof.print_z, .06, {.32, .32},
                                              {{union_ex(masks), {.132, .268}, {2, 1}, 3}});
        REQUIRE(parents.size() == 1);
        const auto children = parents.front().shorter_cycles({.08, .08, .08, .08, .08}, {.32, .32}, {});
        REQUIRE(children.size() == 2);
        for (size_t i = 0; i < children.size(); ++i) {
            const auto& child    = children[i];
            const auto  surfaces = LocalZMixBridgeFillSurfaces::build(sliced->layers(), child);
            REQUIRE_FALSE(surfaces.tops.empty());
            const size_t last   = child.passes.size() - 1;
            const auto   paths  = LocalZMixBridgePaths::build(roof, child.masks(child.passes[last]), child.passes[last].filament_id);
            const auto   filled = LocalZMixBridgePaths::with_fills(*paths, roof, child, last, surfaces, nullptr, nullptr, nullptr);
            size_t       tops = 0, solids = 0;
            std::function<void(const ExtrusionEntity&)> count_roles = [&](const ExtrusionEntity& entity) {
                if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity))
                    for (const auto* item : collection->entities)
                        count_roles(*item);
                else {
                    tops += entity.role() == erTopSolidInfill;
                    solids += entity.role() == erSolidInfill;
                }
            };
            count_roles(filled->fills);
            if (i == 0) {
                CHECK(tops == 0);
                CHECK(solids > 0);
            } else
                CHECK(tops > 0);
        }
    }
    if (repeat) {
        auto fingerprint = [](const Print& job) {
            std::vector<double> result;
            for (const auto& plan : job.get_object(0)->local_z_sublayer_plan()) {
                if (!plan.mix_bridge_paths)
                    continue;
                const auto& paths = *plan.mix_bridge_paths;
                result.insert(result.end(), {plan.print_z, double(plan.mix_bridge_component), double(paths.perimeters.entities.size()),
                                             double(paths.fills.entities.size())});
                for (const auto* collection : {&paths.perimeters, &paths.fills}) {
                    const auto lines = collection->as_polylines();
                    result.push_back(double(lines.size()));
                    double length = 0.;
                    for (const auto& line : lines)
                        length += line.length();
                    result.push_back(length);
                }
            }
            return result;
        };
        Print fresh;
        fresh.set_plate_origin(Vec3d::Zero());
        fresh.is_BBL_printer() = false;
        fresh.set_status_silent();
        fresh.apply(model, config);
        fresh.process();
        CHECK(fingerprint(print) == fingerprint(fresh));
    }
    if (travel) {
        struct TemporaryGCode
        {
            boost::filesystem::path path = boost::filesystem::temp_directory_path() /
                                           boost::filesystem::unique_path("bridge-travel-%%%%.gcode");
            ~TemporaryGCode()
            {
                boost::system::error_code ec;
                boost::filesystem::remove(path, ec);
            }
        } temporary;
        GCodeProcessorResult result;
        print.export_gcode(temporary.path.string(), &result, nullptr);
        // Replay deposited centerlines on a 0.1 mm XY grid. Check the interior
        // of each travel; endpoints are validated by the stack/support tests.
        // This detects travel through an earlier bead, not nozzle-body contact.
        std::map<std::pair<int, int>, double> deposited;
        size_t                                tested = 0, descents = 0;
        for (size_t i = 1; i < result.moves.size(); ++i) {
            const auto&  move = result.moves[i];
            const Vec3d  a = result.moves[i - 1].position.cast<double>(), b = move.position.cast<double>();
            const double length = (b - a).head<2>().norm();
            if (length < 1e-5 && move.type == EMoveType::Travel && b.z() < a.z() - .001) {
                const auto key   = std::make_pair(int(std::lround(b.x() * 10)), int(std::lround(b.y() * 10)));
                const auto found = deposited.find(key);
                if (found != deposited.end()) {
                    ++descents;
                    CAPTURE(move.gcode_id, b.z(), found->second);
                    CHECK(b.z() >= found->second - .015);
                }
            }
            if (length < 1e-5 || move.extrusion_role == erCustom)
                continue;
            const bool extrude = move.type == EMoveType::Extrude && move.extrusion_role != erWipeTower;
            if (!extrude && move.type != EMoveType::Travel)
                continue;
            const size_t steps = size_t(std::ceil(length / .025));
            for (size_t step = 0; step <= steps; ++step) {
                const double t     = double(step) / steps;
                const Vec3d  point = a + t * (b - a);
                const auto   key   = std::make_pair(int(std::lround(point.x() * 10)), int(std::lround(point.y() * 10)));
                if (extrude) {
                    deposited[key] = std::max(deposited[key], point.z());
                } else if (t * length > .5 && (1. - t) * length > .5) {
                    const auto found = deposited.find(key);
                    if (found != deposited.end()) {
                        ++tested;
                        CAPTURE(move.gcode_id, point.z(), found->second);
                        CHECK(point.z() >= found->second - .015);
                    }
                }
            }
        }
        REQUIRE(tested > 10);
        REQUIRE(descents > 0);
    }
}

TEST_CASE("Local-Z ownership preserves touching holes across merged recipes", "[LocalZ][MixBridge][TouchingHole]")
{
    // Reduced from three Tyranid fallback masks. A rounded hole edge crosses
    // its contour by less than one integer unit. An unrelated polygon changes
    // Clipper's sweep ordering, exposing the invalid negative-winding region.
    std::ifstream stream(std::string(TEST_DATA_DIR) + "/local_z/touching_ownership.json");
    REQUIRE(stream.good());
    nlohmann::json document;
    stream >> document;
    LocalZMixBridge bridge;
    for (const auto& rings : document) {
        ExPolygon polygon;
        for (size_t i = 0; i < rings.size(); ++i) {
            Polygon ring;
            for (const auto& point : rings[i])
                ring.points.emplace_back(point[0].get<coord_t>(), point[1].get<coord_t>());
            if (i == 0)
                polygon.contour = std::move(ring);
            else
                polygon.holes.push_back(std::move(ring));
        }
        bridge.zones.push_back({{polygon}, {.08}, {1}, unsigned(bridge.zones.size() + 2)});
    }
    bridge.passes.push_back({{0, 1, 2}, 0, 1, .28, .08});
    SECTION("Original recipe order") {}
    SECTION("Reversed recipe order") { std::reverse(bridge.zones.begin(), bridge.zones.end()); }
    const Point hole(-11158500, 1271000);
    for (const auto& zone : bridge.zones)
        REQUIRE_FALSE(zone.masks.front().contains(hole));
    for (const auto& masks : {bridge.ownership(0), bridge.masks(bridge.passes.front()), bridge.masks(bridge.passes.front(), 0)}) {
        CHECK_FALSE(std::any_of(masks.begin(), masks.end(), [&](const auto& polygon) { return polygon.contains(hole); }));
        for (const Point point : {Point(15123431, -456190), Point(-11628570, 2121300), Point(-11246548, 456275)})
            CHECK(std::any_of(masks.begin(), masks.end(), [&](const auto& polygon) { return polygon.contains(point); }));
        CHECK(area(masks) * SCALING_FACTOR * SCALING_FACTOR == Approx(16.791036).margin(.00001));
    }
}

TEST_CASE("Trimmed fallback cores return unprintable strips to nominal coverage", "[LocalZ][MixBridge][CurvedMixBridge]")
{
    const auto domain = rectangle(-4., 4.);
    auto support = domain;
    ExPolygons shell;
    SECTION("Support leaves only a narrow strip") { support = rectangle(3.8, 4.); }
    SECTION("A moving shell leaves only a narrow strip") { shell = rectangle(-4., 3.8); }
    SECTION("A core fits the nozzle but not the configured wall") { support = rectangle(3.59, 4.); }
    SECTION("A printable neighbor is retained") {
        support = rectangle(3.8, 4.);
        append(support, rectangle(-4., -2.));
    }
    const auto core = LocalZMixBridge::fallback_core(domain, support, shell, .42);
    const auto band = diff_ex(domain, core);
    const Point strip = Point::new_scale(3.9, 0.);
    CHECK(std::none_of(core.begin(), core.end(), [&](const auto& polygon) { return polygon.contains(strip); }));
    CHECK(std::any_of(band.begin(), band.end(), [&](const auto& polygon) { return polygon.contains(strip); }));
    CHECK(intersection_ex(core, band).empty());
    auto combined = core;
    append(combined, band);
    CHECK(area(union_ex(combined)) == Approx(area(domain)).margin(1.));
    if (support.size() > 1) {
        const Point retained = Point::new_scale(-3., 0.);
        CHECK(std::any_of(core.begin(), core.end(), [&](const auto& polygon) { return polygon.contains(retained); }));
    } else {
        CHECK(core.empty());
    }
}

TEST_CASE("Nominal color patches share printable walls without duplicate boundary paths", "[LocalZ][MixBridge][NominalWallPartition]")
{
    bool boundary = false, rounded_hole = false, rounding_crack = false, shifted_owner = false;
    SECTION("A sub-nozzle stripe crosses the exterior") {}
    SECTION("A paint boundary coincides with the wall centerline") { boundary = true; }
    SECTION("A rounded mask cannot emit a loop inside its hole") { rounded_hole = true; }
    SECTION("Reassigning colors cannot split a common wall along a rounding crack") { rounding_crack = true; }
    SECTION("A fitted bead uses its nominal owner when its midpoint contour misses the scheduling slice") { shifted_owner = true; }
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set("layer_height", .08);
    config.set("initial_layer_print_height", .2);
    config.set("wall_loops", 2);
    config.set("detect_thin_wall", false);
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = PerimeterGeneratorType::Classic;
    config.option<ConfigOptionFloats>("nozzle_diameter")->values                     = {.4, .4};
    for (const auto* key : {"line_width", "outer_wall_line_width", "inner_wall_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.4, false));
    if (rounded_hole || rounding_crack) {
        config.set("resolution", .012);
        config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(.42, false));
        config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(.45, false));
    }
    for (const auto& key : config.keys())
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    Model model;
    auto* object = model.add_object();
    object->add_volume(TriangleMesh(its_make_cube(rounding_crack ? 100. : 8., rounding_crack ? 100. : 8., .6)));
    object->add_instance();
    Print print;
    print.apply(model, config);
    print.get_object(0)->slice();
    auto& layer = *print.get_object(0)->get_layer(1);
    if (shifted_owner) {
        // Two narrow sections of a leaning feature, separated by less than
        // the permitted exterior fit error. The midpoint bead still belongs
        // to the same physical region as the nominal scheduling footprint.
        const auto owner = rectangle(-.08, -.02), fitted = rectangle(.02, .08);
        layer.regions().front()->slices.set(owner, stInternal);
        CHECK_THROWS(LocalZMixBridgePaths::build(layer, fitted, 1));
        CHECK_NOTHROW(LocalZMixBridgePaths::build(layer, fitted, 1, 0));
        CHECK_THROWS(LocalZMixBridgePaths::build(layer, fitted, 1, layer.regions().size()));
        layer.regions().front()->slices.clear();
        CHECK_NOTHROW(LocalZMixBridgePaths::build(layer, fitted, 1, 0));
        const auto interior = rectangle(-3., 3.);
        const auto paths    = LocalZMixBridgePaths::build(layer, interior, 1, 0);
        REQUIRE_FALSE(paths->perimeters.empty());
        REQUIRE_FALSE(paths->fill_masks.empty());
        LocalZMixBridge flat;
        flat.z_lo = layer.bottom_z();
        flat.z_hi = layer.print_z;
        flat.zones.push_back({interior, {layer.height}, {1}, 1});
        flat.passes.push_back({{0}, 0, 1, layer.print_z, layer.height});
        const auto filled = LocalZMixBridgePaths::with_fills(*paths, layer, flat, 0, {}, nullptr, nullptr, nullptr);
        CHECK_FALSE(filled->fills.empty());
        return;
    }
    if (rounding_crack) {
        std::ifstream stream(std::string(TEST_DATA_DIR) + "/local_z/nominal_partition_rounding.json");
        REQUIRE(stream.good());
        const auto          document = nlohmann::json::parse(stream);
        const Point         point(scale_(document["point"][0].get<double>()), scale_(document["point"][1].get<double>()));
        std::vector<double> distances;
        for (const auto& partition : document["partitions"]) {
            std::vector<ExPolygons> masks;
            for (const auto& owner : partition) {
                ExPolygons polygons;
                for (const auto& rings : owner) {
                    ExPolygon polygon;
                    for (size_t i = 0; i < rings.size(); ++i) {
                        Polygon ring;
                        for (const auto& vertex : rings[i])
                            ring.points.emplace_back(scale_(vertex[0].get<double>()), scale_(vertex[1].get<double>()));
                        if (i == 0)
                            polygon.contour = std::move(ring);
                        else
                            polygon.holes.push_back(std::move(ring));
                    }
                    polygons.push_back(std::move(polygon));
                }
                masks.push_back(std::move(polygons));
            }
            const auto paths    = LocalZMixBridgePaths::build_partitioned(layer, masks, std::vector<unsigned>(masks.size(), 1));
            double     distance = 100.;
            for (size_t owner = 0; owner < paths.size(); ++owner) {
                const auto lines = paths[owner]->perimeters.as_polylines();
                for (const auto& path : lines)
                    distance = std::min(distance, unscale<double>(point.distance_to(point.projection_onto(path))));
                const auto escaped = diff_pl(lines, offset_ex(masks[owner], scale_(.00001)));
                CHECK(std::none_of(escaped.begin(), escaped.end(), [](const auto& path) { return path.length() > scale_(.0001); }));
            }
            CHECK(distance < .4);
            distances.push_back(distance);
        }
        REQUIRE(distances.size() == 2);
        CHECK(distances[0] == Approx(distances[1]).margin(.001));
        return;
    }
    if (rounded_hole) {
        std::ifstream stream(std::string(TEST_DATA_DIR) + "/local_z/escaped_nominal_paths.json");
        REQUIRE(stream.good());
        nlohmann::json document;
        stream >> document;
        ExPolygons masks;
        for (const auto& rings : document) {
            ExPolygon polygon;
            for (size_t i = 0; i < rings.size(); ++i) {
                Polygon ring;
                for (const auto& point : rings[i])
                    ring.points.emplace_back(point[0].get<coord_t>(), point[1].get<coord_t>());
                if (i == 0)
                    polygon.contour = std::move(ring);
                else
                    polygon.holes.push_back(std::move(ring));
            }
            masks.push_back(std::move(polygon));
        }
        const auto paths = LocalZMixBridgePaths::build(layer, masks, 1);
        REQUIRE_FALSE(paths->perimeters.empty());
        const auto escaped = diff_pl(paths->perimeters.as_polylines(), offset_ex(masks, scale_(.005)));
        CHECK(std::none_of(escaped.begin(), escaped.end(), [](const auto& path) { return path.length() > scale_(.01); }));
        return;
    }
    const auto domain = rectangle(-4., 4.);
    const auto common = LocalZMixBridgePaths::build(layer, domain, 1);
    auto patch = boundary ? rectangle(-4., unscale<double>(get_extents(common->perimeters.as_polylines()).min.x())) : rectangle(-.1, .1);
    const auto remainder = diff_ex(domain, patch);
    const auto isolated  = LocalZMixBridgePaths::build(layer, patch, 2);
    REQUIRE(isolated->perimeters.empty());
    const auto split = LocalZMixBridgePaths::build_partitioned(layer, {patch, remainder}, {2, 1});
    REQUIRE(split.size() == 2);
    // Export expects root children to be island collections, containing
    // directly extrudable paths/loops rather than another collection wrapper.
    for (const auto& paths : split)
        for (const auto* entity : paths->perimeters.entities) {
            const auto* island = dynamic_cast<const ExtrusionEntityCollection*>(entity);
            REQUIRE(island);
            for (const auto* child : island->entities)
                CHECK(dynamic_cast<const ExtrusionEntityCollection*>(child) == nullptr);
        }
    if (!boundary)
        REQUIRE_FALSE(split[0]->perimeters.empty());
    const auto length = [](const ExtrusionEntityCollection& paths) {
        double total = 0.;
        for (const auto& path : paths.as_polylines())
            total += path.length();
        return total;
    };
    CHECK(length(split[0]->perimeters) + length(split[1]->perimeters) == Approx(length(common->perimeters)).margin(scale_(.00001)));
    CHECK(split[0]->perimeters.total_volume() + split[1]->perimeters.total_volume() ==
          Approx(common->perimeters.total_volume()).margin(.000001));
    ExPolygons filled = split[0]->fill_masks;
    append(filled, split[1]->fill_masks);
    CHECK(area(union_ex(filled)) == Approx(area(common->fill_masks)).margin(scale_(.00001) * double(scale_(.00001))));
    CHECK(intersection_ex(split[0]->fill_masks, split[1]->fill_masks).empty());
}

TEST_CASE("Material planner exports minimum stacks and same-material ramps", "[LocalZ][LocalZMaterialPlanner][GCode]")
{
    const std::vector<std::string> colors{"#00FFFF", "#FF00FF", "#FFFF00", "#FFFFFF"};
    std::vector<MixedFilamentDefinition> definitions(2);
    definitions[0].recipe.blend.components = {{{1}, 40}, {{2}, 60}};
    definitions[1].recipe.blend.components = {{{1}, 60}, {{2}, 40}};
    for (size_t i = 0; i < definitions.size(); ++i) {
        definitions[i].identity.stable_id = 9901 + i;
        definitions[i].source.kind = MixedFilamentSourceKind::Custom;
        definitions[i].behavior.distribution = MixedFilamentDistributionMode::LayerCycle;
    }
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions(definitions, colors);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set("mixed_filament_region_collapse", false);
    config.set("mixed_filament_height_lower_bound", .06);
    config.set("layer_height", .08);
    config.set("initial_layer_print_height", .2);
    config.set("wall_loops", 2);
    config.option<ConfigOptionEnum<LocalZPlanner>>("dithering_local_z_planner")->value = LocalZPlanner::MaterialV1;
    for (const auto* key : {"dithering_local_z_mode", "dithering_local_z_whole_objects", "dithering_local_z_preserve_first_layer",
                           "dithering_local_z_direct_multicolor", "dithering_local_z_independent_layer_height", "dithering_local_z_infill"})
        config.set(key, true);
    config.set("enable_prime_tower", false);
    config.set("single_extruder_multi_material", false);
    config.set("machine_start_gcode", "G90\nM83\n");
    config.option<ConfigOptionStrings>("filament_colour")->values = colors;
    config.option<ConfigOptionFloats>("filament_diameter")->values.assign(4, 1.75);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values.assign(4, .4);
    config.option<ConfigOptionFloats>("min_layer_height")->values.assign(4, .06);
    config.option<ConfigOptionFloats>("max_layer_height")->values.assign(4, .32);
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(16, 100.);
    for (const auto* key : {"line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                           "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width"})
        config.set_key_value(key, new ConfigOptionFloatOrPercent(.45, false));
    for (const auto& key : config.keys())
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    Model model;
    auto* object = model.add_object();
    object->config.set("extruder", 1);
    for (int zone = 0; zone < 2; ++zone) {
        auto* volume = object->add_volume(make_cube(12., 8., 1.4));
        volume->translate(Vec3d(12. * zone, 0., 0.));
        TriangleSelector selector(volume->mesh());
        for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
            selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(5 + zone));
        volume->mmu_segmentation_facets.set(selector);
    }
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    size_t joined = 0;
    for (const auto& pass : print.get_object(0)->local_z_sublayer_plan()) {
        REQUIRE(pass.mix_bridge);
        REQUIRE(pass.mix_bridge->material_planner);
        REQUIRE(pass.mix_bridge_paths);
        CHECK_FALSE(pass.mix_bridge->planar_fallback);
        CHECK(pass.mix_bridge->z_hi - pass.mix_bridge->z_lo == Approx(.15).margin(1e-6));
        joined += pass.mix_bridge->material_domains.size() == 2;
    }
    REQUIRE(joined > 0);
    const char* artifact = std::getenv("ORCA_MATERIAL_FIXTURE_OUTPUT");
    const auto output = artifact ? boost::filesystem::path(artifact) :
                                  boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("material-localz-%%%%-%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(output.string(), &result, nullptr);
    size_t slopes = 0;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& move = result.moves[i];
        if (move.type != EMoveType::Extrude || move.extrusion_role != erExternalPerimeter || move.position.z() <= .201)
            continue;
        CHECK(move.height >= .06 - .001);
        CHECK(move.height <= .09 + .001);
        slopes += std::abs(move.position.z() - result.moves[i - 1].position.z()) > .001;
    }
    CHECK(slopes > 0);
    if (!artifact)
        boost::filesystem::remove(output);
}
