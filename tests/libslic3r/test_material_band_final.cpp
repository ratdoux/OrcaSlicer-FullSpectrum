#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/MaterialBand/FinalPrintPlan.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <map>
#include <set>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::MaterialBand;

namespace {
ScheduleInput candidates()
{
    ScheduleInput in;
    in.revision               = {1, 2, 3, 4, 5, 6, 7};
    in.initial_position       = Vec3d::Zero();
    in.machine.sections       = {{0., .2, .01}, {.2, 2., .5}, {2., 10., 2.}};
    in.machine.max_segment_mm = 3.;
    in.machine.shared_nozzle  = true;
    auto p                    = std::make_shared<PathPlan>();
    p->revision               = in.revision;
    p->machine                = in.machine;
    p->solid_min              = Vec3d(20., 20., 0.);
    p->solid_max              = Vec3d(22., 20.4, .48);
    for (size_t i = 0; i < 4; ++i) {
        const double low = i * .12, high = (i + 1) * .12;
        BandPath     path;
        path.id       = i + 1;
        path.band     = {17, 9 + i / 2, {uint32_t(i % 2 + 1)}, uint32_t(i % 2)};
        path.vertices = {Vec3d(20., 20.2, high), Vec3d(22., 20.2, high)};
        PathSegment s;
        s.width_mm        = .4;
        s.height_start_mm = s.height_end_mm = .12;
        s.speed_mm_s                        = 10.;
        s.volume_mm3                        = .096;
        s.cells = {{{{Vec3d(20., 20., high), Vec3d(22., 20., high), Vec3d(22., 20.4, high)}}, {{low, low, low}}},
                   {{{Vec3d(20., 20., high), Vec3d(22., 20.4, high), Vec3d(20., 20.4, high)}}, {{low, low, low}}}};
        path.segments.push_back(s);
        p->paths.push_back(path);
    }
    in.objects = {{1, p}};
    return in;
}
FinalizeProfile profile()
{
    FinalizeProfile p;
    p.purge.enabled    = true;
    p.purge.origin     = Vec2d(50., 50.);
    p.purge.width_mm   = 4.5;
    p.purge.prime_mm3  = {{.5, .5}};
    p.purge.volume_mm3 = {{{{0., 1.}}, {{1., 0.}}}};
    return p;
}
const FinalPrintPlan& accepted(const FinalizeResult& r)
{
    if (const auto* error = std::get_if<Failure>(&r))
        FAIL(failure_name(error->code) << ": " << error->detail);
    return *std::get<std::shared_ptr<const FinalPrintPlan>>(r);
}
} // namespace

TEST_CASE("Material band short planar travel uses the configured retract threshold", "[material_band][integration][final][layer_process]")
{
    const double threshold = GENERATE(1., 5.);
    auto         in        = candidates();
    auto         paths     = std::make_shared<PathPlan>(*in.objects.front().paths);
    paths->paths.resize(1);
    auto second = paths->paths.front();
    second.id   = 2;
    for (auto& vertex : second.vertices)
        vertex.x() += 4.;
    for (auto& cell : second.segments.front().cells)
        for (auto& point : cell.upper)
            point.x() += 4.;
    paths->paths.push_back(second);
    paths->solid_max                    = Vec3d(26., 20.4, .12);
    paths->layered_process              = true;
    paths->machine.coplanar_tip_contact = true;
    in.machine                          = paths->machine;
    in.objects                          = {{1, paths}};
    auto p                              = profile();
    p.purge.enabled                     = false;
    p.purge.prime_mm3                   = {};
    p.purge.volume_mm3                  = {};
    p.retract_min_travel_mm.fill(threshold);
    const auto  result   = finalize_print(in, p);
    const auto& plan     = accepted(result);
    size_t      retracts = 0;
    for (const auto& op : plan.operations)
        retracts += op.kind == OperationKind::Retract;
    // The final lift always retracts. Only the two-millimetre planar move
    // between these paths is subject to the configured distance threshold.
    CHECK(retracts == (threshold < 2. ? 2 : 1));
    CHECK(plan.object_volume_mm3[0] == Approx(.192));
}

TEST_CASE("Material band final plan owns purge sequence and emitted motion", "[material_band][integration][final]")
{
    const bool relative  = GENERATE(false, true);
    auto       in        = candidates();
    auto       p         = profile();
    p.relative_extrusion = relative;
    const auto  result   = finalize_print(in, p);
    const auto& plan     = accepted(result);
    REQUIRE(plan.transitions.size() == 4);
    REQUIRE(plan.cycles.size() == 2);
    CHECK(plan.cycles[0].id == 9);
    CHECK(plan.cycles[1].id == 10);
    for (const auto& transition : plan.transitions)
        CHECK(transition.deposited_purge_mm3 >= transition.required_purge_mm3 - 1e-6);
    CHECK(plan.object_volume_mm3[0] == Approx(.192));
    CHECK(plan.object_volume_mm3[1] == Approx(.192));
    CHECK(plan.purge_volume_mm3[0] >= 1.5);
    CHECK(plan.purge_volume_mm3[1] >= 2.5);
    GCodeWriter writer;
    writer.apply_print_config(static_cast<const PrintConfig&>(FullPrintConfig::defaults()));
    writer.config.gcode_flavor.value = gcfRepRapFirmware;
    std::string text;
    emit_print(plan, writer, [&](const auto& s) { text += s; });
    struct Move
    {
        Vec3d    to;
        double   volume;
        uint32_t material;
    };
    std::vector<Move> moves;
    for (const auto& op : plan.operations)
        if (op.kind == OperationKind::Travel || op.kind == OperationKind::Deposit)
            moves.push_back({op.position, op.kind == OperationKind::Deposit ? op.volume_mm3 : 0., op.material.value});
    std::istringstream    stream(text);
    Vec3d                 position = Vec3d::Zero();
    double                e        = 0.;
    size_t                index    = 0;
    uint32_t              tool     = 0;
    bool                  rel      = false;
    std::array<double, 2> object{}, purge{};
    bool                  depositing = false, is_object = false;
    for (std::string line; std::getline(stream, line);) {
        if (line.find(";MB_OPERATION ") == 0) {
            depositing = line == ";MB_OPERATION DEPOSITION" || line == ";MB_OPERATION PRIME" || line == ";MB_OPERATION PURGE";
            is_object  = line == ";MB_OPERATION DEPOSITION";
        }
        if (line.empty() || line[0] == ';')
            continue;
        std::istringstream tokens(line);
        std::string        command, word;
        tokens >> command;
        std::map<char, double> args;
        while (tokens >> word) {
            if (word[0] == ';')
                break;
            if (word.size() > 1)
                args[word[0]] = std::stod(word.substr(1));
        }
        if (command == "M82")
            rel = false;
        if (command == "M83")
            rel = true;
        if (command[0] == 'T')
            tool = uint32_t(std::stoul(command.substr(1)));
        if (command == "G92" && args.count('E'))
            e = args['E'];
        if (command == "G28")
            position = Vec3d::Zero();
        if (command != "G0" && command != "G1")
            continue;
        const Vec3d old = position;
        for (int axis = 0; axis < 3; ++axis)
            if (args.count("XYZ"[axis]))
                position[axis] = args["XYZ"[axis]];
        double volume = 0.;
        if (args.count('E')) {
            const double delta = rel ? args['E'] : args['E'] - e;
            e                  = rel ? e + args['E'] : args['E'];
            if (depositing && delta > 0.) {
                volume = delta * PI * 1.75 * 1.75 / 4.;
                (is_object ? object : purge)[tool] += volume;
            }
        }
        if ((position - old).norm() > 1e-8) {
            REQUIRE(index < moves.size());
            const auto& expected = moves[index++];
            CHECK((position - expected.to).norm() < .001);
            CHECK(volume == Approx(expected.volume).margin(1e-5 * PI * 1.75 * 1.75 / 4. + 1e-9));
            if (volume > 0.)
                CHECK(tool + 1 == expected.material);
        }
    }
    CHECK(index == moves.size());
    for (size_t physical = 0; physical < 2; ++physical) {
        CHECK(object[physical] == Approx(plan.object_volume_mm3[physical]).margin(.0001));
        CHECK(purge[physical] == Approx(plan.purge_volume_mm3[physical]).margin(.005));
    }
}

TEST_CASE("Material band finalization rejects inaccessible purge and stale revisions atomically", "[material_band][unit][final]")
{
    const int      scenario = GENERATE(0, 1, 2, 3, 4, 5);
    auto           in       = candidates();
    auto           p        = profile();
    FinalizeLimits limits;
    if (scenario == 0)
        p.purge.enabled = false;
    if (scenario == 1)
        p.purge.origin = Vec2d(20., 20.);
    if (scenario == 2)
        p.purge.origin = Vec2d(255., 255.);
    if (scenario == 3)
        limits.max_operations = 10;
    size_t     polls = 0;
    const auto result =
        finalize_print(in, p, limits, [&] { return scenario == 4 && ++polls > 10; }, [&] { return scenario == 5 && ++polls > 10; });
    REQUIRE(std::holds_alternative<Failure>(result));
    const auto& failure = std::get<Failure>(result);
    if (scenario == 0)
        CHECK(failure.detail == "prime_or_purge_tower_required");
    if (scenario == 1)
        CHECK(failure.code == FailureCode::MaterialToleranceExceeded);
    if (scenario == 2)
        CHECK(failure.detail == "purge_tower_outside_bed");
    if (scenario == 3)
        CHECK(failure.code == FailureCode::ResourceLimit);
    if (scenario == 4)
        CHECK(failure.code == FailureCode::Cancelled);
    if (scenario == 5)
        CHECK(failure.code == FailureCode::StaleInput);
}

TEST_CASE("Material band U1 primes repeated same height changes into separate tower strips", "[material_band][final][u1][tower_slots]")
{
    const bool relative             = GENERATE(false, true);
    auto       in                   = candidates();
    in.machine.shared_nozzle        = false;
    in.machine.coplanar_tip_contact = true;
    in.machine.travel_lift_mm       = 2.;
    in.machine.sections             = {{0., 140., 72.5}};
    in.initial_position             = Vec3d(5., 5., 2.24);
    auto paths                      = std::make_shared<PathPlan>(*in.objects.front().paths);
    paths->machine                  = in.machine;
    paths->ordered_region_paths     = true;
    paths->solid_max                = Vec3d(22., 23.4, .12);
    for (size_t i = 0; i < paths->paths.size(); ++i) {
        auto& path            = paths->paths[i];
        path.band.macro_cycle = 0;
        for (auto& p : path.vertices) {
            p.y() += double(i);
            p.z() = .12;
        }
        for (auto& segment : path.segments)
            for (auto& cell : segment.cells) {
                for (auto& p : cell.upper) {
                    p.y() += double(i);
                    p.z() = .12;
                }
                cell.lower_z = {{0., 0., 0.}};
            }
    }
    in.objects.front().paths = paths;
    auto p                   = profile();
    p.adapter                = MachineAdapter::SnapmakerU1;
    p.home                   = false;
    p.relative_extrusion     = relative;
    p.purge.origin           = Vec2d(220., 220.);
    p.purge.width_mm         = 10.;
    p.purge.prime_mm3        = {{1., 1.}};
    const auto  result       = finalize_print(in, p);
    const auto& plan         = accepted(result);
    REQUIRE(plan.transitions.size() == 4);
    REQUIRE(plan.schedule->objects.size() == 2);
    for (const auto& transition : plan.transitions) {
        CHECK(transition.required_purge_mm3 == Approx(1.));
        CHECK(transition.deposited_purge_mm3 >= 1. - 1e-6);
    }
    std::set<unsigned> tower_materials;
    for (const auto& path : plan.schedule->objects.back().paths->paths) {
        tower_materials.insert(path.band.material.value);
        for (const auto& point : path.vertices)
            CHECK(point.z() == Approx(.24));
    }
    CHECK(tower_materials.size() == 2);
    GCodeWriter writer;
    writer.apply_print_config(static_cast<const PrintConfig&>(FullPrintConfig::defaults()));
    writer.config.gcode_flavor.value = gcfKlipper;
    std::string gcode;
    emit_print(plan, writer, [&](const std::string& text) { gcode += text; });
    CHECK(gcode.find(";TYPE:Wipe tower") != std::string::npos);
    CHECK(gcode.find("MB_U1_BIN") == std::string::npos);
    CHECK(gcode.find("BIN_PURGE") == std::string::npos);
    auto disabled          = p;
    disabled.purge.enabled = false;
    disabled.purge.prime_mm3.fill(0.);
    for (auto& row : disabled.purge.volume_mm3)
        row.fill(0.);
    const auto without_tower = finalize_print(in, disabled);
    CHECK(accepted(without_tower).schedule->objects.size() == 1);
    auto invalid             = p;
    invalid.purge.width_mm   = 0.;
    const auto invalid_tower = finalize_print(in, invalid);
    REQUIRE(std::holds_alternative<Failure>(invalid_tower));
    CHECK(std::get<Failure>(invalid_tower).detail == "invalid_purge_footprint");
}
