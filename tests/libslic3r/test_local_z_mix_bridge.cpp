#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LocalZMixBridge.hpp"
#include "libslic3r/LocalZPathRouter.hpp"
#include "libslic3r/LocalZClipper.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <boost/filesystem.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <map>
#include <optional>

using namespace Slic3r;

namespace {
ExPolygons rectangle(double x0, double x1)
{
    return {ExPolygon(Points{{scale_(x0), scale_(-4.)}, {scale_(x1), scale_(-4.)}, {scale_(x1), scale_(4.)}, {scale_(x0), scale_(4.)}})};
}

TEST_CASE("Local-Z bridge autosave reproduction", "[.][MixBridgeRepro]")
{
    const char* project  = std::getenv("ORCA_BRIDGE_REPRO_PROJECT");
    const char* settings = std::getenv("ORCA_BRIDGE_REPRO_SETTINGS");
    const char* output   = std::getenv("ORCA_BRIDGE_REPRO_OUTPUT");
    REQUIRE(project);
    REQUIRE(settings);
    REQUIRE(output);
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
    print.is_BBL_printer() = false;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    size_t bridges = 0, owned = 0;
    double maximum_wall_ahead = 0.;
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
    GCodeProcessorResult result;
    print.export_gcode(output, &result, nullptr);
    report << "moves=" << result.moves.size() << '\n';
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
    std::vector<double> nominal{.08, .08, .08};
    SECTION("Reported 61:39 and 36:64 mixtures") {}
    SECTION("Several nominal tongues")
    {
        height = .48;
        nominal.assign(6, .08);
    }
    const auto groups = LocalZMixBridge::build(0., height, .06, {.4, .4},
                                               {{rectangle(-8., -4.), {height}, {1}, 1},
                                                {rectangle(-4., 0.), {height * .61, height * .39}, {1, 2}, 3},
                                                {rectangle(0., 4.), {height * .36, height * .64}, {1, 2}, 4}},
                                               nominal);
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
    const auto groups = LocalZMixBridge::build(.2, .6, .08, {.21, .21},
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
    // The three-color side puts a strict .07 mm cap on the common C bead.
    // The other side retains its .13 mm C plateau, with a continuous ramp
    // confined to that side near the boundary.
    const auto groups = LocalZMixBridge::build(0., .2, .06, {.3, .3, .3},
                                               {{rectangle(-4., 0.), {.13, .07}, {1, 2}, 4},
                                                {rectangle(0., 4.), {.07, .06, .07}, {1, 2, 3}, 5}});
    REQUIRE(groups.size() == 1);
    const auto& bridge = groups.front();
    REQUIRE_FALSE(bridge.shared_stack);
    REQUIRE(bridge.passes.size() == 4);
    REQUIRE(bridge.passes.front().filament_id == 1);
    CHECK(bridge.interface_z(Point(scale_(-3.), 0.)) == Approx(.13));
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
        const auto groups = LocalZMixBridge::build(0., .2, .06, {.07, .3, .14, .14}, zones);
        REQUIRE(groups.size() == 2);
    }
}

TEST_CASE("Painted mix bridges export continuous walls and retain the base interior", "[LocalZ][MixBridge][GCode]")
{
    bool   enabled          = true;
    bool   compatible       = true;
    double minimum          = .06;
    double nominal          = .2;
    size_t passes_per_layer = 2;
    bool   different_pair = false, three_components = false, different_count = false, unrelated = false, disconnected = false;
    bool   multiple_groups = false, changing_connectivity = false;
    bool   foreign_contact = false, different_wall_settings = false, independent = true, tower_enabled = true;
    bool   fine_cycles = false, intermittent_paint = false, asymmetric_pure = false, cross_interval = false, coarse_cross_interval = false;
    bool   latest_ratios = false;
    SECTION("Enabled") {}
    SECTION("Fine whole-object layers with collapse off join mixtures and a solid neighbor")
    {
        fine_cycles = true;
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
    SECTION("Different component counts share a supported bottom pass")
    {
        different_count  = true;
        nominal          = .3;
        passes_per_layer = 4;
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
    std::vector<MixedFilamentDefinition> definitions(multiple_groups ? 4 : (unrelated || foreign_contact) ? 3 : 2);
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
    config.option<ConfigOptionFloats>("max_layer_height")->values  = {.3, .3, .3};
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
        const double              top  = fine_cycles ? (coarse_cross_interval ? 1. :
                                                        asymmetric_pure       ? .92 :
                                                                                .84) :
                                                       (changing_connectivity && half == 1 ? 2 : 5) * nominal;
        const double              gap  = half == 0 ? .28 : .52;
        const std::vector<double> cuts = intermittent_paint ? std::vector<double>{0., gap, gap + .08, top} : std::vector<double>{0., top};
        for (size_t part = 1; part < cuts.size(); ++part) {
            auto* volume = object->add_volume(make_cube(4., 8., cuts[part] - cuts[part - 1]));
            volume->translate(
                Vec3d(half >= 2 ? (foreign_contact ? 8. : 20.) + 4. * (half - 2) : (disconnected ? 6. : 4.) * half, 0., cuts[part - 1]));
            if (different_wall_settings && half == 1)
                volume->config.set("wall_loops", 3);
            TriangleSelector selector(volume->mesh());
            for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
                selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(intermittent_paint && part == 2 ? 1 : 4 + half));
            volume->mmu_segmentation_facets.set(selector);
        }
    }
    if (fine_cycles) {
        auto* volume = object->add_volume(make_cube(4., 8., coarse_cross_interval ? 1. : asymmetric_pure ? .92 : .84));
        volume->translate(Vec3d(-4., 0., 0.));
        TriangleSelector selector(volume->mesh());
        for (size_t facet = 0; facet < volume->mesh().its.indices.size(); ++facet)
            selector.set_facet(int(facet), static_cast<EnforcerBlockerType>(1));
        volume->mmu_segmentation_facets.set(selector);
    }
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    Print print;
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
        const auto& pass = plan.mix_bridge->passes[plan.mix_bridge_component];
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
    const size_t expected_bridges = fine_cycles ? (coarse_cross_interval ? 6 :
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
            CHECK(bridge.zones.size() == 3);
            CHECK(bridge.passes.size() == (coarse_cross_interval ? 3 : cross_interval ? 5 : bridge.z_hi < .5 || asymmetric_pure ? 4 : 6));
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
            CHECK(move.extruder_id == 0);
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
