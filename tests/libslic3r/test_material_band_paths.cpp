#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/MaterialBand/BoxPlanner.hpp"
#include "libslic3r/MaterialBand/InputAdapter.hpp"
#include "libslic3r/MaterialBand/Scheduler.hpp"
#include "material_band_fixtures.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace Slic3r;
using namespace Slic3r::MaterialBand;

namespace {
struct BoxFixture
{
    Mesh            solid;
    SlabProblem     cycle;
    StackPhase      phase;
    MachineEnvelope machine;
};

BoxFixture box_fixture(const std::string& id)
{
    const auto   fixture = MaterialBandFixtures::make_fixture(id);
    BoxFixture   result;
    InputContext context;
    context.phase.order     = {{{1}, {2}}};
    const auto input_result = snapshot_input(*fixture.model.objects.front(), fixture.config, context);
    REQUIRE(std::holds_alternative<std::shared_ptr<const KernelInput>>(input_result));
    result.solid           = std::get<std::shared_ptr<const KernelInput>>(input_result)->solid;
    auto& p                = result.cycle;
    p.interface_id         = 101;
    p.domain               = 10;
    p.revision             = {1, 1, 1, 1, 1, 1, 1};
    const auto&  spec      = fixture.manifest;
    const double width     = spec.at("dimensions_mm").at(1);
    p.target_volume_a_mm3  = 12. * width * .24 / 2.;
    p.discontinuous_target = spec.at("field") == "step";
    if (spec.at("boundary_policy") == "allow_transition") {
        p.boundary.policy                    = BoundaryPolicy::AllowTransition;
        p.boundary.transition_width_mm       = 2.;
        p.boundary.transition_fraction_error = .25;
    }
    // The diagnostic explicitly requests a whole-solid analytic field. The
    // separate production fixture's shell paint is not projected into infill.
    for (double x : {0., 5., 6., 7., 12.})
        for (double y : {0., width}) {
            p.vertices.emplace_back(x, y);
            p.target_fraction_a.push_back(MaterialBandFixtures::target_fraction(spec, x));
            p.local_fraction_error.push_back(p.boundary.policy == BoundaryPolicy::AllowTransition && x == 6. ? .25 : 0.);
            if (p.boundary.policy == BoundaryPolicy::AllowTransition)
                p.boundary_distance_mm.push_back(x - 6.);
        }
    for (uint32_t i = 0; i < 4; ++i) {
        p.triangles.push_back({2 * i, 2 * i + 2, 2 * i + 3});
        p.triangles.push_back({2 * i, 2 * i + 3, 2 * i + 1});
    }
    result.phase.order              = {{{1}, {2}}};
    result.phase.first_active_cycle = 9;
    result.machine.sections         = {{0., .2, .2}, {.2, 2., 1.}, {2., 10., 3.}};
    return result;
}

const BoxPathPlan& accepted(const BoxPlanResult& result)
{
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure_name(failure->code) << ": " << failure->detail);
    const auto& plan = std::get<std::shared_ptr<const BoxPathPlan>>(result);
    REQUIRE(plan);
    return *plan;
}

bool contains(const std::vector<Vec2d>& polygon, const Vec2d& point)
{
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Vec2d edge = polygon[(i + 1) % polygon.size()] - polygon[i], offset = point - polygon[i];
        if (edge.x() * offset.y() - edge.y() * offset.x() < -1e-9)
            return false;
    }
    return !polygon.empty();
}

struct ParsedMove
{
    Vec3d        xyz;
    unsigned int tool;
    double       filament;
    double       length_mm;
    double       speed_mm_s;
};
std::vector<ParsedMove> parse_deposition(const std::string& gcode, const MachineEnvelope& machine, size_t& retracts, size_t& recoveries)
{
    std::vector<ParsedMove> moves;
    std::istringstream      input(gcode);
    Vec3d                   xyz      = Vec3d::Zero();
    double                  e        = 0.;
    double                  feed     = 0.;
    bool                    relative = false, deposition = false;
    unsigned int            tool = 0;
    std::array<double, 2>   retracted{};
    for (std::string line; std::getline(input, line);) {
        if (line == ";MB_OPERATION DEPOSITION")
            deposition = true;
        if (line == ";MB_OPERATION TRAVEL")
            deposition = false;
        if (line.empty() || line[0] == ';')
            continue;
        std::istringstream tokens(line.substr(0, line.find(';')));
        std::string        command, token;
        tokens >> command;
        if (command == "M83")
            relative = true;
        if (command == "M82")
            relative = false;
        if (command[0] == 'T')
            tool = unsigned(std::stoul(command.substr(1)));
        std::map<char, double> args;
        while (tokens >> token)
            if (token.size() > 1)
                args[token[0]] = std::stod(token.substr(1));
        if (command == "G92") {
            if (args.count('E'))
                e = args['E'];
            continue;
        }
        if (command != "G0" && command != "G1")
            continue;
        if (args.count('F'))
            feed = args['F'];
        const Vec3d previous = xyz;
        for (int axis = 0; axis < 3; ++axis)
            if (args.count("XYZ"[axis]))
                xyz[axis] = args["XYZ"[axis]];
        const double length = (xyz - previous).norm();
        if (length > 0.)
            CHECK(std::abs(xyz.z() - previous.z()) / length * feed / 60. <= machine.max_z_mm_s + 1e-6);
        if (args.count('E')) {
            const double delta        = relative ? args['E'] : args['E'] - e;
            const size_t retract_axis = machine.shared_nozzle ? 0 : tool;
            if (!deposition && delta < 0.) {
                retracted[retract_axis] -= delta;
                ++retracts;
            } else if (!deposition && delta > 0.) {
                CHECK(delta == Approx(retracted[retract_axis]).margin(.00002));
                retracted[retract_axis] -= delta;
                ++recoveries;
            } else if (deposition) {
                CHECK(std::abs(retracted[retract_axis]) < .00002);
            }
            e = relative ? e + args['E'] : args['E'];
            if (deposition && delta > 0.)
                moves.push_back({xyz, tool, delta, length, feed / 60.});
        }
    }
    return moves;
}
} // namespace

TEST_CASE("Material band slab candidates pass combined support and travel scheduling", "[material_band][integration][schedule]")
{
    const std::string id      = GENERATE("MB01", "MB02-transition", "MB03");
    auto              fixture = box_fixture(id);
    // Explicit virtual tip for the combined swept-clearance fixture. The
    // original wide diagnostic-only tip is not qualified on a falling ramp.
    fixture.machine.sections.front().radius_mm = .1;
    const auto    result                       = plan_box(fixture.solid, fixture.cycle, fixture.phase, fixture.machine);
    const auto&   candidate                    = accepted(result);
    ScheduleInput input;
    input.revision         = candidate.revision;
    input.machine          = candidate.machine;
    input.objects          = {{1, std::get<std::shared_ptr<const PathPlan>>(result)}};
    input.initial_position = Vec3d(-5., -5., candidate.solid_max.z() + 12.);
    input.bed_z_mm         = candidate.solid_min.z();
    const auto scheduled   = schedule_paths(input);
    if (const auto* failure = std::get_if<Failure>(&scheduled))
        FAIL(id << ": " << failure_name(failure->code) << ": " << failure->detail);
    const auto& schedule = *std::get<std::shared_ptr<const Schedule>>(scheduled);
    CHECK(schedule.order.size() == candidate.paths.size());
    CHECK(schedule.candidate_pairs > candidate.paths.size());
    CHECK_FALSE(schedule.final_travel.empty());
}

TEST_CASE("Material band box paths close complementary volumes and cross transitions continuously", "[material_band][unit][paths]")
{
    const auto  id      = GENERATE("MB01", "MB02-transition", "MB03");
    const auto  fixture = box_fixture(id);
    const auto  result  = plan_box(fixture.solid, fixture.cycle, fixture.phase, fixture.machine);
    const auto& plan    = accepted(result);
    REQUIRE(plan.interfaces.size() == 10);
    const double width = std::string(id) == "MB01" ? 12. : 8.;
    CHECK(plan.volume_mm3[0] == Approx(12. * width * 2.4 / 2.).margin(1e-5));
    CHECK(plan.volume_mm3[1] == Approx(plan.volume_mm3[0]).margin(1e-5));
    size_t   previous_slot    = 0;
    uint64_t previous_cycle   = 9;
    bool     checked_crossing = false;
    for (const auto& path : plan.paths) {
        REQUIRE(path.vertices.size() == path.segments.size() + 1);
        CHECK(path.shared_interface == 101 + path.band.macro_cycle - 9);
        CHECK(path.band.macro_cycle >= previous_cycle);
        if (path.band.macro_cycle == previous_cycle)
            CHECK(path.band.stack_slot >= previous_slot);
        previous_cycle   = path.band.macro_cycle;
        previous_slot    = path.band.stack_slot;
        size_t crossings = 0;
        for (size_t i = 0; i < path.segments.size(); ++i) {
            const auto&  segment = path.segments[i];
            const Vec3d  delta   = path.vertices[i + 1] - path.vertices[i];
            const double seconds = delta.norm() / segment.speed_mm_s;
            CHECK(segment.volume_mm3 > 0.);
            CHECK(delta.norm() <= fixture.machine.max_segment_mm + 1e-8);
            CHECK(segment.volume_mm3 / seconds <= fixture.machine.max_volume_mm3_s + 1e-8);
            CHECK(std::abs(delta.z()) / seconds <= fixture.machine.max_z_mm_s + 1e-8);
            if (path.vertices[i].x() < 6. && path.vertices[i + 1].x() >= 6.)
                ++crossings;
        }
        if (!checked_crossing && path.role == PathRole::DenseFill && path.band.stack_slot == 0) {
            CHECK(crossings == 1);
            CHECK(path.vertices.front().x() < 5.);
            CHECK(path.vertices.back().x() > 7.);
            checked_crossing = true;
        }
    }
    CHECK(checked_crossing);
    // Independent membership sampling reports missing and excess coverage
    // separately; matching total E alone could conceal a spatial defect.
    size_t missing = 0, excess = 0;
    for (size_t y = 0; y < 41; ++y)
        for (size_t x = 0; x < 61; ++x) {
            const Vec2d point(12. * (x + .37) / 61., width * (y + .43) / 41.);
            size_t      count = 0;
            for (const auto& path : plan.paths) {
                if (path.band.macro_cycle != 9 || path.band.stack_slot != 0)
                    continue;
                for (const auto& segment : path.segments)
                    count += contains(segment.footprint, point);
            }
            missing += count == 0;
            excess += count > 1;
        }
    CHECK(missing == 0);
    CHECK(excess == 0);
}

TEST_CASE("Material band diagnostic writer reconstructs XYZ and material E in both extrusion modes", "[material_band][integration][paths]")
{
    const bool relative              = GENERATE(false, true);
    const bool resets                = GENERATE(false, true);
    auto       fixture               = box_fixture("MB02-transition");
    fixture.machine.shared_nozzle    = GENERATE(false, true);
    fixture.machine.max_volume_mm3_s = .5;
    fixture.machine.max_z_mm_s       = .2;
    const auto  result               = plan_box(fixture.solid, fixture.cycle, fixture.phase, fixture.machine);
    const auto& plan                 = accepted(result);
    const auto  gcode                = diagnostic_gcode(plan, relative, resets);
    size_t      retracts = 0, recoveries = 0;
    const auto  moves = parse_deposition(gcode, fixture.machine, retracts, recoveries);
    CHECK(retracts > 0);
    CHECK(recoveries > 0);
    size_t                index = 0;
    std::array<double, 2> volume{};
    std::array<size_t, 2> move_counts{};
    for (const auto& path : plan.paths)
        for (size_t i = 0; i < path.segments.size(); ++i) {
            REQUIRE(index < moves.size());
            const auto& actual = moves[index++];
            REQUIRE(actual.tool == path.band.material.value - 1);
            CHECK((actual.xyz - path.vertices[i + 1]).cwiseAbs().maxCoeff() <= .001);
            const double diameter = fixture.machine.filament_diameter_mm[actual.tool];
            REQUIRE(actual.length_mm > 0.);
            CHECK(actual.length_mm <= fixture.machine.max_segment_mm + 1e-9);
            CHECK(actual.filament * PI * diameter * diameter / 4. / actual.length_mm * actual.speed_mm_s <=
                  fixture.machine.max_volume_mm3_s + 1e-6);
            volume[actual.tool] += actual.filament * PI * diameter * diameter / 4.;
            ++move_counts[actual.tool];
        }
    REQUIRE(index == moves.size());
    for (size_t tool = 0; tool < 2; ++tool) {
        const double diameter       = fixture.machine.filament_diameter_mm[tool];
        const double rounding_bound = move_counts[tool] * .00001 * PI * diameter * diameter / 4.;
        CHECK(volume[tool] == Approx(12. * 8. * 2.4 / 2.).margin(rounding_bound));
    }
}

TEST_CASE("Material band box planner rejects invalid geometry, blocked heads and exhausted budgets", "[material_band][unit][paths]")
{
    auto        fixture = box_fixture("MB03");
    PathLimits  limits;
    FailureCode expected = FailureCode::UnsupportedTopology;
    const int   scenario = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    CAPTURE(scenario);
    if (scenario == 0)
        fixture.solid.vertices[0].x() += .5; // Same bbox, unsupported actual geometry.
    if (scenario == 1)
        fixture.solid.triangles.pop_back(); // Open solid.
    if (scenario == 2) {
        fixture.machine.sections = {{0., .01, .2}, {.01, 10., 3.}};
        expected                 = FailureCode::ClearanceFailure; // Tip fits, wider body does not.
    }
    if (scenario == 3) {
        limits.max_segments = 500;
        expected            = FailureCode::ResourceLimit;
    }
    if (scenario == 4) {
        limits.max_temporary_bytes = 100;
        expected                   = FailureCode::ResourceLimit;
    }
    if (scenario == 5) {
        fixture.phase.slot = 1;
        expected           = FailureCode::IncompatibleOrder;
    }
    if (scenario == 6) {
        // Same summed area, but an overlapping triangle replaces a missing one.
        fixture.cycle.triangles.back() = fixture.cycle.triangles.front();
        expected                       = FailureCode::InvalidInput;
    }
    if (scenario == 7) {
        fixture.machine.max_segment_mm = .0001;
        expected                       = FailureCode::UnsupportedProfile;
    }
    const auto result = plan_box(fixture.solid, fixture.cycle, fixture.phase, fixture.machine, limits);
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == expected);
}

TEST_CASE("Material band paths preserve dose and phase under translation and material permutation", "[material_band][unit][paths]")
{
    auto fixture = box_fixture("MB01");
    fixture.cycle.target_fraction_a.assign(fixture.cycle.vertices.size(), .35);
    fixture.cycle.target_volume_a_mm3 = 12. * 12. * .24 * .35;
    const auto  original_result       = plan_box(fixture.solid, fixture.cycle, fixture.phase, fixture.machine);
    const auto& original              = accepted(original_result);
    const Vec3d translation(1000000., -1000000., 1000.);
    for (auto& point : fixture.solid.vertices)
        point += translation;
    for (auto& point : fixture.cycle.vertices)
        point += translation.head<2>();
    fixture.cycle.bottom_mm += translation.z();
    fixture.cycle.top_mm += translation.z();
    std::reverse(fixture.cycle.triangles.begin(), fixture.cycle.triangles.end());
    std::swap(fixture.phase.order[0], fixture.phase.order[1]);
    const auto  translated_result = plan_box(fixture.solid, fixture.cycle, fixture.phase, fixture.machine);
    const auto& translated        = accepted(translated_result);
    REQUIRE(original.paths.size() == translated.paths.size());
    for (size_t i = 0; i < original.paths.size(); ++i) {
        const auto& a = original.paths[i];
        const auto& b = translated.paths[i];
        REQUIRE(a.vertices.size() == b.vertices.size());
        CHECK(a.band.macro_cycle == b.band.macro_cycle);
        CHECK(a.band.material.value + b.band.material.value == 3);
        for (size_t j = 0; j < a.vertices.size(); ++j)
            CHECK((b.vertices[j] - translation - a.vertices[j]).norm() < 1e-8);
    }
    CHECK(translated.volume_mm3[0] == Approx(12. * 12. * 2.4 * .35).margin(1e-5));
    CHECK(translated.volume_mm3[1] == Approx(12. * 12. * 2.4 * .65).margin(1e-5));
}

TEST_CASE("Material band diagnostic artifacts", "[.][material_band][diagnostic]")
{
    const char* directory = std::getenv("MATERIAL_BAND_DIAGNOSTIC_DIR");
    REQUIRE(directory != nullptr);
    const std::string id      = GENERATE("MB01", "MB02-transition", "MB03");
    const auto        fixture = box_fixture(id);
    const auto        result  = plan_box(fixture.solid, fixture.cycle, fixture.phase, fixture.machine);
    const auto&       plan    = accepted(result);
    const auto        output  = std::filesystem::path(directory) / id;
    std::filesystem::create_directories(output);
    nlohmann::json data{{"schema_version", 1},
                        {"fixture", id},
                        {"engine_used", "material_bands"},
                        {"stage", "isolated_box_diagnostic"},
                        {"production_accepted", false},
                        {"volume_mm3_by_stack_slot", plan.volume_mm3},
                        {"logical_cycles", plan.interfaces.size()},
                        {"paths", nlohmann::json::array()},
                        {"interfaces", nlohmann::json::array()}};
    for (const auto& interface : plan.interfaces) {
        nlohmann::json vertices = nlohmann::json::array();
        for (const auto& p : interface.surface.vertices)
            vertices.push_back({p.x(), p.y(), p.z()});
        data["interfaces"].push_back({{"id", interface.id},
                                      {"macro_cycle", interface.macro_cycle},
                                      {"vertices_mm", vertices},
                                      {"triangles", interface.surface.triangles}});
    }
    for (const auto& path : plan.paths) {
        nlohmann::json vertices = nlohmann::json::array(), volumes = nlohmann::json::array();
        for (const auto& p : path.vertices)
            vertices.push_back({p.x(), p.y(), p.z()});
        for (const auto& segment : path.segments)
            volumes.push_back(segment.volume_mm3);
        data["paths"].push_back({{"id", path.id},
                                 {"macro_cycle", path.band.macro_cycle},
                                 {"physical_material", path.band.material.value},
                                 {"interface", path.shared_interface},
                                 {"split_reason", path.split_reason},
                                 {"vertices_mm", vertices},
                                 {"segment_volume_mm3", volumes}});
    }
    std::ofstream json_file(output / "plan.json"), gcode_file(output / "diagnostic.gcode");
    json_file << data.dump(2) << '\n';
    gcode_file << diagnostic_gcode(plan);
    REQUIRE(json_file.good());
    REQUIRE(gcode_file.good());
}
