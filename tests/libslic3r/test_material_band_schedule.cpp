#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/MaterialBand/Scheduler.hpp"
#include "libslic3r/MaterialBand/ClippedPlanner.hpp"
#include "libslic3r/ClipperUtils.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <sstream>
#include <tuple>

using namespace Slic3r;
using namespace Slic3r::MaterialBand;

namespace {
std::vector<BeadCell> rectangle(double x0, double x1, double y0, double y1, double z0, double z1)
{
    return {{{{Vec3d(x0, y0, z1), Vec3d(x1, y0, z1), Vec3d(x1, y1, z1)}}, {{z0, z0, z0}}},
            {{{Vec3d(x0, y0, z1), Vec3d(x1, y1, z1), Vec3d(x0, y1, z1)}}, {{z0, z0, z0}}}};
}
MachineEnvelope machine()
{
    MachineEnvelope m;
    m.sections       = {{0., .2, .01}, {.2, 2., .5}, {2., 10., 2.}};
    m.max_segment_mm = 3.;
    return m;
}
std::shared_ptr<const PathPlan> paths(double x, double y, size_t count = 2)
{
    PathPlan p;
    p.machine   = machine();
    p.revision  = {1, 2, 3, 4, 5, 6, 7};
    p.solid_min = Vec3d(x, y, 0.);
    p.solid_max = Vec3d(x + 2., y + .4, count * .12);
    for (size_t i = 0; i < count; ++i) {
        BandPath path;
        path.id               = i + 1;
        path.band             = {17, 9 + i / 2, {uint32_t(1 + i % 2)}, uint32_t(i % 2)};
        path.shared_interface = 51 + i / 2;
        path.vertices         = {Vec3d(x, y + .2, (i + 1) * .12), Vec3d(x + 2., y + .2, (i + 1) * .12)};
        PathSegment segment;
        segment.cells           = rectangle(x, x + 2., y, y + .4, i * .12, (i + 1) * .12);
        segment.volume_mm3      = 2. * .4 * .12;
        segment.width_mm        = .4;
        segment.height_start_mm = segment.height_end_mm = .12;
        segment.speed_mm_s                              = 10.;
        path.segments.push_back(segment);
        p.paths.push_back(path);
    }
    return std::make_shared<const PathPlan>(std::move(p));
}
ScheduleInput input()
{
    ScheduleInput in;
    in.revision         = {1, 2, 3, 4, 5, 6, 7};
    in.machine          = machine();
    in.objects          = {{1, paths(0., 0.)}};
    in.initial_position = Vec3d(-3., -3., 12.);
    return in;
}
const Schedule& accepted(const ScheduleResult& r)
{
    if (const auto* f = std::get_if<Failure>(&r))
        FAIL(failure_name(f->code) << ": " << f->detail);
    return *std::get<std::shared_ptr<const Schedule>>(r);
}
} // namespace

TEST_CASE("Material support contacts reuse established ordering chains", "[material_band][unit][schedule]")
{
    const bool separate_objects = GENERATE(false, true);
    const auto scenario         = GENERATE(std::string("supported"), std::string("gap"), std::string("reversed"));
    CAPTURE(separate_objects, scenario);
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    auto plan                       = std::make_shared<PathPlan>(*paths(0., 0., 3));
    plan->machine                   = in.machine;
    plan->ordered_region_paths      = true;
    for (size_t i = 0; i < 3; ++i) {
        auto&        path                = plan->paths[i];
        const double x0                  = i == 1 ? (scenario == "gap" ? 1.2 : 1.) : 0.;
        const double x1                  = i == 0 ? (scenario == "gap" ? .8 : 1.) : 2.;
        const double bottom              = i == 2 ? .12 : 0.;
        path.band                        = {17, 0, {uint32_t(i == 2 ? 2 : 1)}, uint32_t(i == 2 ? 1 : 0)};
        path.role                        = PathRole::DenseFill;
        path.vertices                    = {Vec3d(x0, .2, bottom + .12), Vec3d(x1, .2, bottom + .12)};
        path.segments.front().cells      = rectangle(x0, x1, 0., .4, bottom, bottom + .12);
        path.segments.front().volume_mm3 = (x1 - x0) * .4 * .12;
    }
    in.objects.clear();
    if (separate_objects) {
        for (size_t i = 0; i < 3; ++i) {
            auto part              = std::make_shared<PathPlan>(*plan);
            part->paths            = {plan->paths[i]};
            part->paths.front().id = 1;
            in.objects.push_back({i + 1, part});
        }
        in.dependencies = scenario == "reversed" ? std::vector<Dependency>{{{3, 1}, {1, 1}}, {{1, 1}, {2, 1}}} :
                                                   std::vector<Dependency>{{{1, 1}, {2, 1}}, {{2, 1}, {3, 1}}};
    } else {
        if (scenario == "reversed")
            std::rotate(plan->paths.begin(), plan->paths.begin() + 2, plan->paths.end());
        for (size_t i = 0; i < 3; ++i)
            plan->paths[i].id = i + 1;
        in.objects = {{1, plan}};
    }
    ScheduleLimits limits;
    // Two edges completely order these three paths. Contact between the
    // first floor patch and the upper bead must not consume a third edge.
    limits.max_edges  = 2;
    const auto result = schedule_paths(in, limits);
    if (scenario == "supported") {
        const auto& schedule = accepted(result);
        CHECK(schedule.order.size() == 3);
        CHECK(schedule.dependencies.size() == 2);
    } else {
        REQUIRE(std::holds_alternative<Failure>(result));
        // Removing redundant links must not admit a gap or reverse support.
        REQUIRE(std::holds_alternative<Failure>(schedule_paths(in)));
    }
}

TEST_CASE("Material grounded-wall caching stays bounded across a tall stack", "[material_band][unit][schedule]")
{
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    auto plan                       = std::make_shared<PathPlan>(*paths(0., 0., 512));
    plan->machine                   = in.machine;
    plan->ordered_region_paths      = true;
    for (auto& path : plan->paths)
        path.role = PathRole::Perimeter;
    in.objects          = {{1, plan}};
    in.initial_position = Vec3d(-3., -3., 64.);
    ScheduleLimits limits;
    limits.max_temporary_bytes = 1280 * 1024;
    const auto  result         = schedule_paths(in, limits);
    const auto& schedule       = accepted(result);
    CHECK(schedule.order.size() == 512);
}

TEST_CASE("Material planar contact indexing stays bounded under tessellation", "[material_band][unit][schedule]")
{
    const auto scenario = GENERATE(std::string("supported"), std::string("floor_gap"), std::string("overlap"));
    CAPTURE(scenario);
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    auto plan                       = std::make_shared<PathPlan>(*paths(-1., -1., 1));
    plan->machine                   = in.machine;
    plan->ordered_region_paths      = true;
    auto& floor                     = plan->paths.front();
    floor.role                      = PathRole::DenseFill;
    floor.vertices                  = {Vec3d(-1., 0., .12), Vec3d(1., 0., .12)};
    auto& base                      = floor.segments.front();
    base.cells.clear();
    base.width_mm   = 2.;
    base.volume_mm3 = 0.;
    for (size_t x = 0; x < 32; ++x)
        for (size_t y = 0; y < 32; ++y) {
            if (scenario == "floor_gap" && x == 16)
                continue;
            const auto part = rectangle(-1. + x / 16., -1. + (x + 1) / 16., -1. + y / 16., -1. + (y + 1) / 16., 0., .12);
            base.cells.insert(base.cells.end(), part.begin(), part.end());
            base.volume_mm3 += .12 / 256.;
        }
    const auto rotate = [](const Vec3d& p) { return Vec3d((p.x() - p.y()) / std::sqrt(2.), (p.x() + p.y()) / std::sqrt(2.), p.z()); };
    for (size_t row = 0; row < 16; ++row) {
        BandPath path;
        path.id        = row + 2;
        path.band      = {17, 9, {2}, 1};
        path.role      = PathRole::DenseFill;
        const double y = -.225 + .03 * row;
        path.vertices  = {rotate(Vec3d(-.85, y, .24)), rotate(Vec3d(.85, y, .24))};
        PathSegment segment;
        segment.cells = rectangle(-.85, .85, y - .01, y + .01, .12, .24);
        for (auto& cell : segment.cells)
            for (auto& point : cell.upper)
                point = rotate(point);
        segment.volume_mm3      = 1.7 * .02 * .12;
        segment.width_mm        = .02;
        segment.height_start_mm = segment.height_end_mm = .12;
        segment.speed_mm_s                              = 10.;
        if (scenario == "overlap" && row == 15) {
            segment.cells.push_back(segment.cells.front());
            segment.volume_mm3 *= 1.5;
        }
        path.segments.push_back(std::move(segment));
        plan->paths.push_back(std::move(path));
    }
    in.objects = {{1, plan}};
    ScheduleLimits limits;
    // The former box-spread index needed 277,712 visits for the unchanged
    // geometry. Keep substantial tessellation from exhausting query work.
    limits.max_index_visits = 250000;
    const auto result       = schedule_paths(in, limits);
    if (scenario == "supported") {
        const auto& schedule = accepted(result);
        INFO("Spatial node visits: " << schedule.index_visits);
        CHECK(schedule.order.size() == 17);
    } else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == (scenario == "floor_gap" ? "unsupported_bead_footprint" : "overlapping_deposition_cells"));
    }
}

TEST_CASE("Material explicit motion indexing retains earlier and static obstacles", "[material_band][unit][schedule]")
{
    const auto scenario = GENERATE(std::string("future"), std::string("earlier"), std::string("static"));
    CAPTURE(scenario);
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    in.initial_position             = Vec3d(-3., -3., 24.);
    const auto raised               = [&](double x, double y, double height, uint32_t material) {
        auto plan                  = std::make_shared<PathPlan>(*paths(x, y, 1));
        plan->machine              = in.machine;
        plan->ordered_region_paths = true;
        auto& path                 = plan->paths.front();
        path.band                  = {17, 0, {material}, material - 1};
        path.role                  = PathRole::DenseFill;
        for (auto& point : path.vertices)
            point.z() = height;
        auto& segment           = path.segments.front();
        segment.cells           = rectangle(x, x + 2., y, y + .4, 0., height);
        segment.volume_mm3      = 2. * .4 * height;
        segment.height_start_mm = segment.height_end_mm = height;
        segment.speed_mm_s                              = .1;
        return plan;
    };
    auto distant              = raised(100., 0., 20., 1);
    auto low                  = std::make_shared<PathPlan>();
    low->machine              = in.machine;
    low->ordered_region_paths = true;
    for (size_t row = 0; row < 16; ++row) {
        auto strip = raised(0., -.4 + .03 * row, .12, 1)->paths.front();
        strip.id   = row + 1;
        for (auto& point : strip.vertices)
            point.y() -= .19;
        auto& segment      = strip.segments.front();
        segment.cells      = rectangle(0., 2., -.4 + .03 * row, -.38 + .03 * row, 0., .12);
        segment.width_mm   = .02;
        segment.volume_mm3 = 2. * .02 * .12;
        low->paths.push_back(std::move(strip));
    }
    auto  nearby   = raised(0., 1., 5., 2);
    auto& detailed = nearby->paths.front().segments.front();
    detailed.cells.clear();
    for (size_t x = 0; x < 32; ++x)
        for (size_t y = 0; y < 16; ++y) {
            const auto part = rectangle(x / 16., (x + 1) / 16., 1. + y / 40., 1. + (y + 1) / 40., 0., 5.);
            detailed.cells.insert(detailed.cells.end(), part.begin(), part.end());
        }
    in.objects = {{1, distant}, {2, low}};
    std::vector<OperationRef> sequence{{1, 1}};
    if (scenario == "static")
        in.occupied = detailed.cells;
    else {
        in.objects.push_back({3, nearby});
        if (scenario == "earlier")
            sequence.push_back({3, 1});
    }
    for (size_t i = 0; i < low->paths.size(); ++i)
        sequence.push_back({2, i + 1});
    if (scenario == "future")
        sequence.push_back({3, 1});
    for (size_t i = 1; i < sequence.size(); ++i)
        in.dependencies.push_back({sequence[i - 1], sequence[i]});
    const auto result = schedule_paths(in);
    if (scenario == "future") {
        const auto& schedule = accepted(result);
        REQUIRE(schedule.order.size() == sequence.size());
        for (size_t i = 0; i < sequence.size(); ++i)
            CHECK(schedule.order[i].operation == sequence[i]);
    } else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail ==
              (scenario == "static" ? "deposition_hits_occupied_geometry" : "support_access_dependency_cycle"));
    }
}

TEST_CASE("Material band swept envelope detects interior and head collisions", "[material_band][unit][schedule]")
{
    const Vec3d           from(-3., 0., .12), to(3., 0., .12);
    const auto            obstacle = rectangle(-.1, .1, .7, .9, 0., 3.);
    const EnvelopeSection tip{0., .2, .01}, head{2., 10., 1.};
    for (const auto& cell : obstacle) {
        CHECK_FALSE(swept_section_intersects(from, from, head, cell));
        CHECK_FALSE(swept_section_intersects(to, to, head, cell));
        CHECK_FALSE(swept_section_intersects(from, to, tip, cell));
        CHECK(swept_section_intersects(from, to, head, cell));
        CHECK(swept_section_intersects(to, from, head, cell));
    }
    // Compare against analytic disk/rectangle separation for 64 translations.
    for (size_t i = 0; i < 64; ++i) {
        const double y    = .01 + i * .03;
        const auto   wall = rectangle(-.1, .1, y, y + .1, 0., 1.);
        bool         hit  = false;
        for (const auto& c : wall)
            hit |= swept_section_intersects(from, to, EnvelopeSection{0., 1., .5}, c);
        if (y < .5)
            CHECK(hit);
        if (y > .5 / std::cos(PI / 16.))
            CHECK_FALSE(hit);
    }
}

TEST_CASE("Material band narrow cells retain physical volume across integer rounding", "[material_band][unit][schedule]")
{
    auto  in             = input();
    auto  p              = std::make_shared<PathPlan>(*paths(0., 0., 1));
    auto& segment        = p->paths.front().segments.front();
    segment.cells        = rectangle(0., 2., .2, .2 + .3e-6, 0., .12);
    segment.volume_mm3   = 2. * .3e-6 * .12;
    in.objects           = {{1, p}};
    const auto  result   = schedule_paths(in);
    const auto& schedule = accepted(result);
    REQUIRE(schedule.order.size() == 1);
    CHECK(schedule.objects.front().paths->paths.front().segments.front().volume_mm3 == Approx(segment.volume_mm3));
    CHECK(schedule.objects.front().paths->paths.front().segments.front().cells.size() == 2);
}

TEST_CASE("Material band numerical support allowance cannot hide a missing strip", "[material_band][unit][schedule]")
{
    auto in = input();
    auto p  = std::make_shared<PathPlan>(*in.objects.front().paths);
    // A one-micrometre strip is still much smaller than a bead, but is real
    // missing support, not integer polygon rounding.
    p->paths.front().segments.front().cells      = rectangle(0., 2., 0., .399, 0., .12);
    p->paths.front().segments.front().volume_mm3 = 2. * .399 * .12;
    in.objects.front().paths                     = p;
    const auto result                            = schedule_paths(in);
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
}

TEST_CASE("Material painted region cap roundoff remains inside the declared contact tolerance", "[material_band][painted_regions][schedule]")
{
    const double gap   = GENERATE(1e-16, 5e-6, 2e-5);
    auto         in    = input();
    auto         p     = std::make_shared<PathPlan>(*in.objects.front().paths);
    auto&        upper = p->paths.back();
    for (auto& v : upper.vertices)
        v.z() += gap;
    for (auto& cell : upper.segments.front().cells)
        for (size_t i = 0; i < 3; ++i) {
            cell.lower_z[i] += gap;
            cell.upper[i].z() += gap;
        }
    in.objects.front().paths = p;
    const auto result        = schedule_paths(in);
    if (gap <= in.support_tolerance_mm)
        CHECK(std::holds_alternative<std::shared_ptr<const Schedule>>(result));
    else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
    }
}

TEST_CASE("Material painted support retains small exact contact intersections", "[material_band][painted_regions][schedule]")
{
    const bool missing = GENERATE(false, true);
    auto       in      = input();
    auto       p       = std::make_shared<PathPlan>(*in.objects.front().paths);
    p->paths.resize(1);
    auto& path                       = p->paths.front();
    path.vertices                    = {Vec3d(0., .0005, .24), Vec3d(.001, .0005, .24)};
    path.segments.front().cells      = rectangle(0., .001, 0., .001, .12, .24);
    path.segments.front().volume_mm3 = .001 * .001 * .12;
    path.segments.front().width_mm   = .001;
    in.objects.front().paths         = p;
    for (size_t i = 0; i < (missing ? 500u : 1000u); ++i) {
        const auto cells = rectangle(i * 1e-6, (i + 1) * 1e-6, 0., .001, 0., .12);
        in.occupied.insert(in.occupied.end(), cells.begin(), cells.end());
    }
    const auto result = schedule_paths(in);
    if (missing) {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
    } else {
        CHECK(accepted(result).order.size() == 1);
    }
}

TEST_CASE("Material band schedule owns support phase and all travel geometry", "[material_band][unit][schedule]")
{
    auto in = input();
    in.objects.push_back({2, paths(8., 0., 4)});
    const auto  result   = schedule_paths(in);
    const auto& schedule = accepted(result);
    REQUIRE(schedule.order.size() == 6);
    std::map<OperationRef, size_t> position;
    Vec3d                          previous = in.initial_position;
    for (size_t i = 0; i < schedule.order.size(); ++i) {
        const auto& item         = schedule.order[i];
        position[item.operation] = i;
        REQUIRE(item.travel.size() >= 2);
        CHECK((item.travel.front() - previous).norm() < 1e-12);
        const auto& p = in.objects[item.operation.object - 1].paths->paths[item.operation.path - 1];
        CHECK((item.travel.back() - p.vertices.front()).norm() < 1e-12);
        previous = p.vertices.back();
    }
    for (const auto& edge : schedule.dependencies)
        CHECK(position.at(edge.before) < position.at(edge.after));
    REQUIRE(schedule.final_travel.size() == 2);
    CHECK((schedule.final_travel.front() - previous).norm() < 1e-12);
    // Insertion order does not select the tool or object sequence.
    std::reverse(in.objects.begin(), in.objects.end());
    const auto  reordered = schedule_paths(in);
    const auto& canonical = accepted(reordered);
    for (size_t i = 0; i < schedule.order.size(); ++i)
        CHECK(schedule.order[i].operation == canonical.order[i].operation);
}

TEST_CASE("Material layered travel checks direct moves and lifts around obstacles", "[material_band][unit][schedule][layer_process]")
{
    const int scenario = GENERATE(0, 1, 2, 3);
    auto      in       = input();
    auto      p        = std::make_shared<PathPlan>(*paths(0., 0., 1));
    p->paths.push_back(paths(8., 0., 1)->paths.front());
    p->paths.back().id              = 2;
    p->solid_max.x()                = 10.;
    p->layered_process              = scenario != 3;
    p->machine.coplanar_tip_contact = true;
    if (scenario == 1)
        in.occupied = rectangle(4., 4.1, .1, .3, 0., 1.);
    if (scenario == 2) {
        p->paths.back().band.material.value = 2;
        p->paths.back().band.stack_slot     = 1;
    }
    in.machine           = p->machine;
    in.objects           = {{1, p}};
    const auto  result   = schedule_paths(in);
    const auto& schedule = accepted(result);
    REQUIRE(schedule.order.size() == 2);
    const auto& travel = schedule.order.back().travel;
    REQUIRE(travel.size() >= 2);
    CHECK((travel.front() - p->paths.front().vertices.back()).norm() < 1e-12);
    CHECK((travel.back() - p->paths.back().vertices.front()).norm() < 1e-12);
    if (scenario == 0 || scenario == 3) {
        REQUIRE(travel.size() == 2);
        CHECK(travel.front().z() == Approx(.12));
        CHECK(travel.back().z() == Approx(.12));
    } else {
        CHECK(travel.size() > 2);
        CHECK(travel[1].z() > (scenario == 1 ? 1. : .12));
    }
}

TEST_CASE("Material band schedule fails atomically for support access cycles and stale work", "[material_band][unit][schedule]")
{
    const int      scenario = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    auto           in       = input();
    ScheduleLimits limits;
    FailureCode    expected = FailureCode::InvalidInput;
    std::string    detail;
    if (scenario == 0) {
        auto p = std::make_shared<PathPlan>(*in.objects[0].paths);
        p->paths.erase(p->paths.begin());
        in.objects[0].paths = p;
        expected            = FailureCode::InfeasibleThickness;
        detail              = "unsupported_bead_footprint";
    } else if (scenario == 1) {
        in.occupied = rectangle(.8, 1.2, .8, 1., 0., 3.);
        expected    = FailureCode::ClearanceFailure;
        detail      = "deposition_hits_occupied_geometry";
    } else if (scenario == 2) {
        in.objects      = {{1, paths(0., 0., 1)}, {2, paths(8., 0., 1)}, {3, paths(16., 0., 1)}};
        in.dependencies = {{{1, 1}, {2, 1}}, {{2, 1}, {3, 1}}, {{3, 1}, {1, 1}}};
        expected        = FailureCode::IncompatibleOrder;
        detail          = "support_access_dependency_cycle";
    } else if (scenario == 3) {
        limits.max_edges = 0;
        expected         = FailureCode::ResourceLimit;
    } else if (scenario == 4) {
        limits.max_candidates = 0;
        expected              = FailureCode::ResourceLimit;
    } else if (scenario == 5) {
        limits.max_temporary_bytes = 10;
        expected                   = FailureCode::ResourceLimit;
    } else if (scenario == 6) {
        expected = FailureCode::Cancelled;
    } else if (scenario == 7) {
        expected = FailureCode::StaleInput;
    } else {
        in.initial_position = Vec3d(1., 4., 2.5);
        // Initial lift passes through a high shelf even though both endpoints
        // and all object deposition paths are clear.
        in.occupied = rectangle(.9, 1.1, 3.9, 4.1, 4., 4.1);
        expected    = FailureCode::ClearanceFailure;
        detail      = "travel_hits_occupied_geometry";
    }
    size_t     polls = 0;
    const auto result =
        schedule_paths(in, limits, [&] { return scenario == 6 && ++polls >= 3; }, [&] { return scenario == 7 && ++polls >= 3; });
    REQUIRE(std::holds_alternative<Failure>(result));
    const auto& failure = std::get<Failure>(result);
    CHECK(failure.code == expected);
    if (!detail.empty())
        CHECK(failure.detail == detail);
}

TEST_CASE("Material band schedule derives access before tall neighboring material", "[material_band][unit][schedule]")
{
    auto in = input();
    // One tall, independently supported candidate would block the other
    // object's head. It must be deposited last, regardless of object ID.
    auto  tall        = std::make_shared<PathPlan>(*paths(0., 1., 1));
    auto& p           = tall->paths[0];
    p.vertices[0].z() = p.vertices[1].z() = 3.;
    p.segments[0].cells                   = rectangle(0., 2., 1., 1.4, 0., 3.);
    p.segments[0].volume_mm3              = 2.4;
    p.segments[0].speed_mm_s              = 5.;
    p.segments[0].height_start_mm = p.segments[0].height_end_mm = 3.;
    in.objects                                                  = {{1, tall}, {2, paths(0., 0., 1)}};
    const auto  result                                          = schedule_paths(in);
    const auto& s                                               = accepted(result);
    REQUIRE(s.order.size() == 2);
    CHECK(s.order[0].operation.object == 2);
    CHECK(s.order[1].operation.object == 1);
    CHECK(std::any_of(s.dependencies.begin(), s.dependencies.end(), [](const auto& e) { return e.kind == DependencyKind::Access; }));
}

TEST_CASE("Material band scheduled export preserves every validated move and dose", "[material_band][integration][schedule]")
{
    const bool relative      = GENERATE(false, true);
    const bool resets        = GENERATE(false, true);
    const bool shared        = GENERATE(false, true);
    auto       in            = input();
    auto       p             = std::make_shared<PathPlan>(*paths(0., 0., 4));
    p->machine.shared_nozzle = in.machine.shared_nozzle = shared;
    in.objects                                          = {{1, p}};
    const auto         result                           = schedule_paths(in);
    const auto&        schedule                         = accepted(result);
    std::vector<Vec3d> expected;
    for (const auto& item : schedule.order) {
        expected.insert(expected.end(), item.travel.begin() + 1, item.travel.end());
        const auto& path = p->paths.at(item.operation.path - 1);
        expected.insert(expected.end(), path.vertices.begin() + 1, path.vertices.end());
    }
    expected.insert(expected.end(), schedule.final_travel.begin() + 1, schedule.final_travel.end());
    std::istringstream    stream(diagnostic_gcode(schedule, relative, resets));
    Vec3d                 position = schedule.initial_position;
    double                e = 0., feed = 0.;
    std::array<double, 2> dose{}, debt{};
    size_t                tool = 0, move = 0, deposition_count = 0, cycle_records = 0, bead_records = 0;
    bool                  relative_mode = false, depositing = false;
    for (std::string line; std::getline(stream, line);) {
        if (line == ";MB_OPERATION TRAVEL")
            depositing = false;
        if (line == ";MB_OPERATION DEPOSITION")
            depositing = true;
        if (line.find(";MB_CYCLE ") == 0)
            ++cycle_records;
        if (line.find(";MB_BEAD ") == 0)
            ++bead_records;
        if (line.empty() || line[0] == ';')
            continue;
        std::istringstream tokens(line.substr(0, line.find(';')));
        std::string        command, word;
        tokens >> command;
        std::map<char, double> args;
        while (tokens >> word)
            if (word.size() > 1)
                args[word[0]] = std::stod(word.substr(1));
        if (command == "M82")
            relative_mode = false;
        if (command == "M83")
            relative_mode = true;
        if (command[0] == 'T')
            tool = size_t(std::stoul(command.substr(1)));
        if (command == "G92" && args.count('E'))
            e = args['E'];
        if (command != "G0" && command != "G1")
            continue;
        if (args.count('F'))
            feed = args['F'] / 60.;
        const Vec3d from = position;
        for (int axis = 0; axis < 3; ++axis)
            if (args.count("XYZ"[axis]))
                position[axis] = args["XYZ"[axis]];
        const double length = (position - from).norm();
        if (length > 1e-9) {
            REQUIRE(move < expected.size());
            CHECK((position - expected[move++]).norm() <= .001);
            CHECK(std::abs(position.z() - from.z()) / length * feed <= in.machine.max_z_mm_s + 1e-7);
        }
        if (args.count('E')) {
            const double delta = relative_mode ? args['E'] : args['E'] - e;
            e                  = relative_mode ? e + args['E'] : args['E'];
            auto& retracted    = debt[shared ? 0 : tool];
            if (depositing && delta > 0.) {
                CHECK(std::abs(retracted) < .00002);
                dose[tool] += delta * PI * 1.75 * 1.75 / 4.;
                ++deposition_count;
            } else {
                retracted -= delta;
                CHECK(retracted >= -.00002);
            }
        }
    }
    CHECK(move == expected.size());
    CHECK(deposition_count == 4);
    CHECK(cycle_records == 4);
    CHECK(bead_records == 4);
    CHECK(dose[0] == Approx(.192).margin(2. * 1e-5 * PI * 1.75 * 1.75 / 4.));
    CHECK(dose[1] == Approx(.192).margin(2. * 1e-5 * PI * 1.75 * 1.75 / 4.));
}

TEST_CASE("Material band schedule rejects duplicate volume and bounds spatial work", "[material_band][unit][schedule]")
{
    SECTION("A finely triangulated path does not query itself for overhang support")
    {
        auto  in         = input();
        auto  p          = std::make_shared<PathPlan>(*paths(0., 0., 1));
        auto& path       = p->paths.front();
        path.overhang_mm = .2;
        auto& segment    = path.segments.front();
        segment.cells.clear();
        constexpr size_t count = 256;
        for (size_t i = 0; i < count; ++i) {
            auto cells = rectangle(2. * i / count, 2. * (i + 1) / count, 0., .4, 0., .12);
            segment.cells.insert(segment.cells.end(), cells.begin(), cells.end());
        }
        in.objects = {{1, p}};
        ScheduleLimits limits;
        limits.max_candidates = 4 * count;
        const auto result     = schedule_paths(in, limits);
        CHECK(accepted(result).candidate_pairs < 4 * count);
    }
    SECTION("Planar support work is shared across a run's cell subdivisions")
    {
        auto             in    = input();
        auto             p     = std::make_shared<PathPlan>(*paths(0., 0., 2));
        constexpr size_t count = 256;
        for (size_t layer = 0; layer < p->paths.size(); ++layer) {
            auto& path       = p->paths[layer];
            path.overhang_mm = .2;
            auto& segment    = path.segments.front();
            segment.cells.clear();
            for (size_t i = 0; i < count; ++i) {
                auto cells = rectangle(2. * i / count, 2. * (i + 1) / count, 0., .4, layer * .12, (layer + 1) * .12);
                segment.cells.insert(segment.cells.end(), cells.begin(), cells.end());
            }
        }
        in.objects = {{1, p}};
        ScheduleLimits limits;
        limits.max_candidates = 16 * count;
        const auto result     = schedule_paths(in, limits);
        CHECK(accepted(result).candidate_pairs < 16 * count);
    }
    SECTION("Overlapping cells inside one path are still rejected")
    {
        auto  in      = input();
        auto  p       = std::make_shared<PathPlan>(*paths(0., 0., 1));
        auto& segment = p->paths.front().segments.front();
        segment.cells.push_back(segment.cells.front());
        segment.volume_mm3 *= 1.5;
        in.objects        = {{1, p}};
        const auto result = schedule_paths(in);
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "overlapping_deposition_cells");
    }
    SECTION("Duplicate object volume is not hidden by matching total dose")
    {
        auto in = input();
        in.objects.push_back({2, in.objects[0].paths});
        const auto result = schedule_paths(in);
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "overlapping_deposition_cells");
    }
    SECTION("Distant objects retain local candidate growth")
    {
        for (size_t count : {16, 64, 256}) {
            auto in = input();
            in.objects.clear();
            for (size_t i = 0; i < count; ++i)
                in.objects.push_back({i + 1, paths(i * 8., 0., 2)});
            const auto  result = schedule_paths(in);
            const auto& s      = accepted(result);
            CHECK(s.order.size() == 2 * count);
            CHECK(s.candidate_pairs < 100 * count);
            CHECK(s.index_nodes < 16 * count);
        }
    }
    SECTION("Index visits are bounded independently of candidate count")
    {
        auto           in = input();
        ScheduleLimits limits;
        limits.max_index_visits = 0;
        const auto result       = schedule_paths(in, limits);
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "schedule_index_work_limit");
    }
}

TEST_CASE("Material band U1 flat clearance retains raised obstacle collisions", "[material_band][u1][schedule]")
{
    const double height             = GENERATE(.12, .120001, .2, .24);
    auto         in                 = input();
    in.machine.sections             = {{0., 140., 72.5}};
    in.machine.coplanar_tip_contact = true;
    in.machine.travel_lift_mm       = 2.;
    auto p                          = std::make_shared<PathPlan>(*in.objects.front().paths);
    p->machine                      = in.machine;
    in.objects.front().paths        = p;
    in.occupied                     = rectangle(5., 6., 0., 1., 0., height);
    const auto result               = schedule_paths(in);
    if (height == .12) {
        CHECK(accepted(result).order.size() == 2);
    } else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).code == FailureCode::ClearanceFailure);
    }
}

TEST_CASE("Material band U1 total sequence preserves cross object access constraints", "[material_band][u1][schedule]")
{
    const bool blocked              = GENERATE(false, true);
    auto       in                   = input();
    in.machine.sections             = {{0., 140., 72.5}};
    in.machine.coplanar_tip_contact = true;
    in.machine.travel_lift_mm       = 2.;
    auto a                          = std::make_shared<PathPlan>(*paths(0., 0., 1));
    auto b                          = std::make_shared<PathPlan>(*paths(5., 0., 2));
    a->machine = b->machine = in.machine;
    in.objects              = {{1, a}, {2, b}};
    in.dependencies         = blocked ? std::vector<Dependency>{{{2, 1}, {2, 2}}, {{2, 2}, {1, 1}}} :
                                        std::vector<Dependency>{{{1, 1}, {2, 1}}, {{2, 1}, {2, 2}}};
    const auto result       = schedule_paths(in);
    if (blocked) {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).code == FailureCode::IncompatibleOrder);
    } else
        CHECK(accepted(result).order.size() == 3);
}

TEST_CASE("Material layer bridges require two anchors and bound the whole unsupported span", "[material_band][layer_process][schedule]")
{
    const std::string scenario = GENERATE("anchored", "one_anchor", "short_anchor", "too_long", "no_contract");
    const double      length   = scenario == "too_long" ? 24. : 12.;
    auto              in       = input();
    auto              plan     = std::make_shared<PathPlan>(*paths(0., 0., 1));
    auto&             path     = plan->paths.front();
    path.vertices.clear();
    path.segments.clear();
    path.overhang_mm    = .2;
    path.bridge_span_mm = scenario == "no_contract" ? 0. : 20.;
    for (double x = 0.; x <= length; x += 2.)
        path.vertices.emplace_back(x, .2, .24);
    for (double x = 0.; x < length; x += 2.) {
        PathSegment segment;
        segment.cells           = rectangle(x, x + 2., 0., .4, .12, .24);
        segment.volume_mm3      = 2. * .4 * .12;
        segment.width_mm        = .4;
        segment.height_start_mm = segment.height_end_mm = .12;
        segment.speed_mm_s                              = 10.;
        path.segments.push_back(segment);
    }
    in.objects  = {{1, plan}};
    in.occupied = rectangle(0., scenario == "short_anchor" ? .1 : 1., 0., .4, 0., .12);
    if (scenario != "one_anchor") {
        const auto end = rectangle(length - 1., length, 0., .4, 0., .12);
        in.occupied.insert(in.occupied.end(), end.begin(), end.end());
    }
    const auto result = schedule_paths(in);
    if (scenario == "anchored") {
        CHECK(accepted(result).order.size() == 1);
    } else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
    }
}

TEST_CASE("Material side-supported wall bridges bound every unsupported bead interval", "[material_band][layer_process][schedule]")
{
    const std::string scenario      = GENERATE("anchored", "too_long", "missing_start", "floating", "no_contract");
    auto              in            = input();
    in.machine.coplanar_tip_contact = true;
    auto plan                       = std::make_shared<PathPlan>();
    plan->machine                   = in.machine;
    plan->revision                  = in.revision;
    plan->ordered_region_paths = plan->layered_process = true;
    const auto wall                                    = [&](double first, double last, double y, bool bridge) {
        BandPath p;
        p.id               = plan->paths.size() + 1;
        p.band             = {17, 0, {1}, 0};
        p.role             = PathRole::Perimeter;
        p.overhang_mm      = .2;
        p.bridge_span_mm   = bridge && scenario != "no_contract" ? 20. : 0.;
        const size_t count = size_t(std::ceil((last - first) / 2.));
        for (size_t i = 0; i <= count; ++i)
            p.vertices.emplace_back(first + (last - first) * i / count, y + .2, .24);
        for (size_t i = 0; i < count; ++i) {
            const double x0 = p.vertices[i].x(), x1 = p.vertices[i + 1].x();
            PathSegment  segment;
            segment.cells           = rectangle(x0, x1, y, y + .4, .12, .24);
            segment.volume_mm3      = (x1 - x0) * .4 * .12;
            segment.width_mm        = .4;
            segment.height_start_mm = segment.height_end_mm = .12;
            segment.speed_mm_s                              = 10.;
            p.segments.push_back(std::move(segment));
        }
        plan->paths.push_back(std::move(p));
    };
    if (scenario != "missing_start")
        wall(0., scenario == "too_long" ? 3. : 8., 0., false);
    wall(scenario == "too_long" ? 27. : 12., 30., 0., false);
    wall(0., 30., .4, true);
    in.objects = {{1, plan}};
    if (scenario != "floating")
        in.occupied = rectangle(0., 30., 0., .4, 0., .12);
    const auto result = schedule_paths(in);
    if (scenario == "anchored")
        CHECK(accepted(result).order.size() == 3);
    else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
    }
}

TEST_CASE("Material short fill caps require their full centerline and both finite anchors", "[material_band][layer_process][schedule]")
{
    const bool missing_end = GENERATE(false, true);
    auto       in          = input();
    auto       plan        = std::make_shared<PathPlan>(*paths(0., 0., 1));
    auto&      path        = plan->paths.front();
    path.vertices          = {Vec3d(0., .2, .24), Vec3d(.3, .2, .24)};
    path.bridge_span_mm    = 20.;
    auto& segment          = path.segments.front();
    segment.cells          = rectangle(0., .3, 0., .4, .12, .24);
    segment.volume_mm3     = .3 * .4 * .12;
    in.occupied            = rectangle(-.2, missing_end ? .15 : .5, .1, .3, 0., .12);
    in.objects             = {{1, plan}};
    const auto result      = schedule_paths(in);
    if (missing_end) {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
    } else
        CHECK(accepted(result).order.size() == 1);
}

TEST_CASE("Material cap fitting trims only bounded protrusions and accounts for removed dose", "[material_band][layer_process][schedule]")
{
    const double tip      = GENERATE(.01, .03, .05, .12);
    const double tip_base = tip == .12 ? .01 : .4;
    auto         in       = input();
    auto         plan     = std::make_shared<PathPlan>(*paths(0., 0., 1));
    auto&        path     = plan->paths.front();
    path.vertices         = {Vec3d(0., .2, .24), Vec3d(1.8, .2, .24)};
    path.overhang_mm      = .2;
    auto& segment         = path.segments.front();
    segment.cells         = rectangle(0., 2., 0., .4, .12, .24);
    segment.cells.push_back({{{Vec3d(2., 0., .24), Vec3d(2. + tip, tip_base / 2., .24), Vec3d(2., tip_base, .24)}}, {{.12, .12, .12}}});
    segment.volume_mm3 += tip * tip_base / 2. * .12;
    plan->volume_mm3[0] = segment.volume_mm3;
    plan->coverage[0]   = {segment.volume_mm3, segment.volume_mm3, 0., 0.};
    in.occupied         = rectangle(-.2, 1.8, 0., .4, 0., .12);
    in.objects          = {{1, plan}};
    REQUIRE(std::holds_alternative<Failure>(schedule_paths(in)));
    const ExPolygons support{ExPolygon(Polygon(Points{Point(scale_(-.2), scale_(0.)), Point(scale_(1.8), scale_(0.)),
                                                      Point(scale_(1.8), scale_(.4)), Point(scale_(-.2), scale_(.4))}))};
    const auto       result = fit_planar_overhang_caps(*plan, support, .12);
    REQUIRE(std::holds_alternative<std::shared_ptr<const PathPlan>>(result));
    const auto fitted = std::get<std::shared_ptr<const PathPlan>>(result);
    in.objects        = {{1, fitted}};
    if (tip <= .03) {
        CHECK(accepted(schedule_paths(in)).order.size() == 1);
        CHECK(fitted->volume_mm3[0] == Approx(.096));
        CHECK(fitted->coverage[0].missing_mm3 == Approx(tip * .2 * .12));
        CHECK(fitted->coverage[0].target_mm3 == Approx(plan->volume_mm3[0]));
        CHECK(fitted->paths.front().vertices == path.vertices);
    } else {
        CHECK(fitted->volume_mm3[0] == plan->volume_mm3[0]);
        CHECK(std::holds_alternative<Failure>(schedule_paths(in)));
    }
    const auto other_height = fit_planar_overhang_caps(*plan, support, 0.);
    CHECK(std::get<std::shared_ptr<const PathPlan>>(other_height)->volume_mm3[0] == plan->volume_mm3[0]);
}

TEST_CASE("Material optional cap fitting preserves each painted foundation dose", "[material_band][layer_process][clipped_paths]")
{
    const bool repair_corners = GENERATE(false, true);
    CAPTURE(repair_corners);
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    PathPlan geometry;
    geometry.machine         = in.machine;
    geometry.layered_process = true;
    geometry.solid_min       = Vec3d(0., 0., .12);
    geometry.solid_max       = Vec3d(2.01, .4, .44);
    BandPath source;
    source.id       = 1;
    source.band     = {1, 0, {1}, 0};
    source.role     = PathRole::DenseFill;
    source.vertices = {Vec3d(0., .2, .44), Vec3d(1.8, .2, .44)};
    PathSegment segment;
    segment.cells = rectangle(0., 2., 0., .4, .12, .44);
    segment.cells.push_back({{{Vec3d(2., 0., .44), Vec3d(2.01, .2, .44), Vec3d(2., .4, .44)}}, {{.12, .12, .12}}});
    segment.volume_mm3      = .802 * .32;
    segment.width_mm        = .4;
    segment.height_start_mm = segment.height_end_mm = .32;
    segment.speed_mm_s                              = 10.;
    for (const auto& cell : segment.cells)
        segment.coverage_triangles.push_back({cell.upper[0].head<2>(), cell.upper[1].head<2>(), cell.upper[2].head<2>()});
    source.segments.push_back(segment);
    geometry.paths.push_back(source);
    const auto region = [](double x0, double x1) {
        return ExPolygons{
            ExPolygon(Polygon({Point::new_scale(x0, 0.), Point::new_scale(x1, 0.), Point::new_scale(x1, .4), Point::new_scale(x0, .4)}))};
    };
    ExPolygons painted{ExPolygon(Polygon({Point::new_scale(1.9, 0.), Point::new_scale(2., 0.), Point::new_scale(2.01, .2),
                                          Point::new_scale(2., .4), Point::new_scale(1.9, .4)}))};
    if (repair_corners)
        painted = region(1.9, 2.01);
    const std::vector<PlanarMaterialBand> bands{{{1, 0, {1}, 0}, region(0., 1.9), .12, .44, 0, 0},
                                                {{2, 0, {1}, 0}, painted, .12, .28, 0, 1},
                                                {{2, 0, {2}, 1}, painted, .28, .44, 1, 0}};
    ProcessConstraints                    process;
    process.bead_width_mm           = .4;
    process.min_active_thickness_mm = .06;
    const auto planned              = plan_partitioned_cycle(geometry, bands, process, {}, {}, {}, {}, {}, region(-.2, 1.8), .12, .06);
    if (repair_corners) {
        // The source has a gap concentrated in the small painted region.
        // Its intended foundation dose must not be reduced to hide that gap.
        REQUIRE(std::holds_alternative<Failure>(planned));
        const auto& failure = std::get<Failure>(planned);
        REQUIRE(failure.band.has_value());
        CHECK(failure.band->domain == 2);
        CHECK(failure.band->material.value == 1);
        CHECK(failure.detail.find("painted_fitted_cap_coverage_budget") == 0);
        return;
    }
    if (const auto* failure = std::get_if<Failure>(&planned))
        FAIL(failure->detail);
    REQUIRE(std::holds_alternative<std::shared_ptr<const PathPlan>>(planned));
    const auto&  plan         = *std::get<std::shared_ptr<const PathPlan>>(planned);
    const double painted_area = repair_corners ? .044 : .042;
    CHECK(plan.volume_mm3[0] == Approx(.76 * .32 + painted_area * .16).epsilon(1e-4));
    CHECK(plan.volume_mm3[1] == Approx(painted_area * .16).epsilon(1e-4));
    const auto fitted = fit_planar_overhang_caps(plan, region(-.2, 1.8), .12);
    REQUIRE(std::holds_alternative<std::shared_ptr<const PathPlan>>(fitted));
    CHECK(plan.volume_mm3[0] - std::get<std::shared_ptr<const PathPlan>>(fitted)->volume_mm3[0] > painted_area * .16 * .01);
    // Keeping coverage does not certify support: a floating copy still fails.
    in.objects             = {{1, std::get<std::shared_ptr<const PathPlan>>(planned)}};
    const auto unsupported = schedule_paths(in);
    REQUIRE(std::holds_alternative<Failure>(unsupported));
    CHECK(std::get<Failure>(unsupported).detail == "unsupported_bead_footprint");
}

TEST_CASE("Material bridges require earlier supported full-height lateral anchors", "[material_band][layer_process][schedule]")
{
    const std::string scenario      = GENERATE("anchored", "bridge_anchor", "dense_anchor", "missing_wall", "late_walls", "floating_walls",
                                               "shallow_anchor");
    auto              in            = input();
    in.machine.coplanar_tip_contact = true;
    auto plan                       = std::make_shared<PathPlan>();
    plan->machine                   = in.machine;
    plan->ordered_region_paths = plan->layered_process = true;
    const auto wall                                    = [&](double x, uint64_t id) {
        BandPath p;
        p.id   = id;
        p.band = {17, 0, {1}, 0};
        p.role = scenario == "dense_anchor" ? PathRole::DenseFill : PathRole::Perimeter;
        if (scenario == "bridge_anchor") {
            p.role           = PathRole::Bridge;
            p.bridge_span_mm = 20.;
        }
        p.overhang_mm = .2;
        p.vertices    = {Vec3d(x + .2, -.2, .24), Vec3d(x + .2, .6, .24)};
        PathSegment  s;
        const double bottom = scenario == "shallow_anchor" ? .18 : .12;
        s.cells             = rectangle(x, x + .4, -.2, .6, bottom, .24);
        s.volume_mm3        = .4 * .8 * (.24 - bottom);
        s.width_mm          = .4;
        s.height_start_mm = s.height_end_mm = .24 - bottom;
        s.speed_mm_s                        = 10.;
        p.segments.push_back(std::move(s));
        return p;
    };
    plan->paths.push_back(wall(-.4, scenario == "late_walls" ? 2 : 1));
    if (scenario != "missing_wall")
        plan->paths.push_back(wall(12., scenario == "late_walls" ? 3 : 2));
    BandPath bridge;
    bridge.id             = scenario == "late_walls" ? 1 : 3;
    bridge.band           = {17, 0, {1}, 0};
    bridge.role           = PathRole::Bridge;
    bridge.overhang_mm    = .2;
    bridge.bridge_span_mm = 20.;
    for (size_t i = 0; i <= 6; ++i)
        bridge.vertices.emplace_back(-.2 + 12.4 * i / 6., .2, .24);
    for (size_t i = 0; i < 6; ++i) {
        PathSegment s;
        s.cells           = rectangle(2. * i, 2. * (i + 1), 0., .4, .12, .24);
        s.volume_mm3      = 2. * .4 * .12;
        s.width_mm        = .4;
        s.height_start_mm = s.height_end_mm = .12;
        s.speed_mm_s                        = 10.;
        bridge.segments.push_back(std::move(s));
    }
    plan->paths.push_back(std::move(bridge));
    in.objects = {{1, plan}};
    if (scenario != "floating_walls") {
        in.occupied      = scenario == "shallow_anchor" ? rectangle(-.4, 0., -.2, .6, 0., .18) : rectangle(-.6, -.2, -.2, .6, 0., .12);
        const auto right = scenario == "shallow_anchor" ? rectangle(12., 12.4, -.2, .6, 0., .18) : rectangle(12.2, 12.6, -.2, .6, 0., .12);
        in.occupied.insert(in.occupied.end(), right.begin(), right.end());
    }
    const auto result = schedule_paths(in);
    if (scenario == "anchored" || scenario == "bridge_anchor" || scenario == "dense_anchor")
        CHECK(accepted(result).order.size() == 3);
    else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
    }
}

TEST_CASE("Material painted bridge foundation preserves local recipes and printable passes", "[material_band][layer_process][schedule]")
{
    const size_t      count          = GENERATE(2u, 3u, 4u);
    const std::string scenario       = GENERATE("normal", "thin", "whole_first");
    const bool        thin_remainder = scenario == "thin";
    const auto        region         = [](double x0, double x1, double y0, double y1) {
        return ExPolygons{
            ExPolygon(Polygon({Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)}))};
    };
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    in.machine.material_count       = count;
    const auto         outline = region(0., 12., 0., 1.2), painted = region(5., 7., .4, .8);
    ProcessConstraints process;
    process.bead_width_mm           = .4;
    process.min_active_thickness_mm = .06;
    process.max_active_thickness_mm = .24;
    std::vector<ContourRun> runs;
    for (double y : {.2, .6, 1.})
        runs.push_back({Polyline(Points{Point::new_scale(0., y), Point::new_scale(12., y)}), .4, PathRole::Bridge});
    const double height         = thin_remainder ? .16 : scenario == "whole_first" ? (count == 2 ? .12 : .24) : count == 2 ? .32 : .4;
    const double first_fraction = count == 2 ? .5 : scenario == "whole_first" ? .25 : .4;
    const auto   source         = plan_contour_cycle(outline, runs, {1, 0, {1}, 0}, .12, .12 + height, {}, process, {}, in.machine);
    REQUIRE(std::holds_alternative<std::shared_ptr<const PathPlan>>(source));
    std::vector<PlanarMaterialBand> bands{{{1, 0, {1}, 0}, diff_ex(outline, painted), .12, .12 + height, 0, 0}};
    double                          bottom = .12;
    for (size_t m = 0; m < count; ++m) {
        const double top = m + 1 == count ? .12 + height :
                                            bottom + height * (m == 0 ? first_fraction : (1. - first_fraction) / (count - 1));
        bands.push_back({{2, 0, {uint32_t(m + 1)}, uint32_t(m)}, painted, bottom, top, m, m + 1 == count ? 0 : m + 1});
        bottom = top;
    }
    const auto result = plan_partitioned_cycle(*std::get<std::shared_ptr<const PathPlan>>(source), bands, process, {}, {}, {}, {}, {}, {},
                                               0., .06);
    if (thin_remainder) {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "bridge_foundation_remainder_too_thin");
        return;
    }
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure->detail);
    auto plan = std::make_shared<PathPlan>(*std::get<std::shared_ptr<const PathPlan>>(result));
    if (scenario == "whole_first")
        CHECK(std::any_of(plan->interfaces.begin(), plan->interfaces.end(), [](const InterfacePatch& patch) {
            return patch.id == 1 && patch.domain == 2 && !patch.surface.triangles.empty();
        }));
    std::stable_sort(plan->paths.begin(), plan->paths.end(), [](const BandPath& a, const BandPath& b) {
        return std::make_tuple(a.vertices.front().z(), a.band.material.value) <
               std::make_tuple(b.vertices.front().z(), b.band.material.value);
    });
    double first = 0., second = 0.;
    for (size_t i = 0; i < plan->paths.size(); ++i) {
        auto& path = plan->paths[i];
        path.id    = i + 1;
        for (const auto& segment : path.segments) {
            CHECK(segment.height_start_mm >= .06 - 1e-9);
            CHECK(segment.height_start_mm <= .24 + 1e-9);
            for (const auto& cell : segment.cells) {
                Points points;
                for (const auto& p : cell.upper)
                    points.push_back(Point::new_scale(p.x(), p.y()));
                for (const auto& piece : intersection_ex(Polygons{Polygon(points)}, painted)) {
                    const double volume = piece.area() * SCALING_FACTOR * SCALING_FACTOR * (cell.upper[0].z() - cell.lower_z[0]);
                    (path.band.material.value == 1 ? first : second) += volume;
                }
            }
        }
    }
    CHECK(first == Approx(.8 * height * first_fraction).epsilon(.01));
    CHECK(second == Approx(.8 * height * (1. - first_fraction)).epsilon(.01));
    CHECK(std::accumulate(plan->volume_mm3.begin(), plan->volume_mm3.end(), 0.) == Approx(12. * 1.2 * height).epsilon(.01));
    in.occupied    = rectangle(-.2, 1., -.2, 1.4, 0., .12);
    const auto end = rectangle(11., 12.2, -.2, 1.4, 0., .12);
    in.occupied.insert(in.occupied.end(), end.begin(), end.end());
    in.objects = {{1, plan}};
    CHECK(accepted(schedule_paths(in)).order.size() == plan->paths.size());
    const std::vector<PlanarMaterialBand> incompatible{{{1, 0, {1}, 0}, diff_ex(outline, painted), .12, .12 + height, 0, 0},
                                                       {{2, 0, {2}, 1}, painted, .12, .12 + height, 0, 0}};
    const auto rejected = plan_partitioned_cycle(*std::get<std::shared_ptr<const PathPlan>>(source), incompatible, process, {}, {}, {}, {},
                                                 {}, {}, 0., .06);
    REQUIRE(std::holds_alternative<Failure>(rejected));
    CHECK(std::get<Failure>(rejected).detail == "bridge_foundation_requires_common_first_material");
}

TEST_CASE("Material bridge endpoints can bond to finite full-height side faces", "[material_band][layer_process][schedule]")
{
    const std::string scenario      = GENERATE("attached", "tapered", "no_overhang", "one_anchor", "gap", "short_side", "late", "floating",
                                               "shallow", "too_long");
    auto              in            = input();
    in.machine.coplanar_tip_contact = true;
    auto plan                       = std::make_shared<PathPlan>();
    plan->machine                   = in.machine;
    plan->ordered_region_paths = plan->layered_process = true;
    const double length                                = scenario == "too_long" ? 24. : 12.;
    const double bottom                                = scenario == "shallow" ? .18 : .12;
    for (size_t end = 0; end < (scenario == "one_anchor" ? 1 : 2); ++end) {
        const double side_y = scenario == "gap" ? -.01 : scenario == "tapered" && end == 1 ? .05 : 0.;
        const double x0     = end == 0 ? -.2 : length - .8;
        const double x1     = end == 0 ? (scenario == "short_side" ? .05 : .8) : length + .2;
        BandPath     anchor;
        anchor.id       = scenario == "late" ? end + 2 : end + 1;
        anchor.band     = {17, 0, {1}, 0};
        anchor.role     = PathRole::Perimeter;
        anchor.vertices = {Vec3d(x0, side_y - .2, .24), Vec3d(x1, side_y - .2, .24)};
        PathSegment s;
        s.cells           = rectangle(x0, x1, side_y - .4, side_y, bottom, .24);
        s.volume_mm3      = (x1 - x0) * .4 * (.24 - bottom);
        s.width_mm        = .4;
        s.height_start_mm = s.height_end_mm = .24 - bottom;
        s.speed_mm_s                        = 10.;
        anchor.segments.push_back(std::move(s));
        plan->paths.push_back(std::move(anchor));
        if (scenario != "floating") {
            const auto base = rectangle(x0, x1, side_y - .4, side_y, 0., bottom);
            in.occupied.insert(in.occupied.end(), base.begin(), base.end());
        }
    }
    BandPath bridge;
    bridge.id             = scenario == "late" ? 1 : 3;
    bridge.band           = {17, 0, {1}, 0};
    bridge.role           = PathRole::Bridge;
    bridge.overhang_mm    = scenario == "no_overhang" ? 0. : .2;
    bridge.bridge_span_mm = 20.;
    for (double x = 0.; x <= length; x += 2.)
        bridge.vertices.emplace_back(x, .2, .24);
    for (double x = 0.; x < length; x += 2.) {
        PathSegment  s;
        const double width = scenario == "tapered" && x + 2. == length ? .3 : .4;
        s.cells            = rectangle(x, x + 2., .2 - width / 2., .2 + width / 2., .12, .24);
        s.volume_mm3       = 2. * width * .12;
        s.width_mm         = width;
        s.height_start_mm = s.height_end_mm = .12;
        s.speed_mm_s                        = 10.;
        bridge.segments.push_back(std::move(s));
    }
    plan->paths.push_back(std::move(bridge));
    in.objects        = {{1, plan}};
    const auto result = schedule_paths(in);
    if (scenario == "attached" || scenario == "tapered" || scenario == "no_overhang")
        CHECK(accepted(result).order.size() == 3);
    else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
    }
}

TEST_CASE("Material outer walls use only grounded sections of an earlier inner wall", "[material_band][layer_process][schedule]")
{
    const std::string scenario = GENERATE("reorder", "inner_first", "open", "floor_margin", "floor_slot", "floor_gap", "late", "gap",
                                          "floating", "chain");
    CAPTURE(scenario);
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    in.allow_planar_reordering      = scenario != "late" && scenario != "inner_first";
    ProcessConstraints process;
    process.bead_width_mm = .4;
    const ExPolygons outline{
        ExPolygon(Polygon({Point::new_scale(0., 0.), Point::new_scale(12., 0.), Point::new_scale(12., 12.), Point::new_scale(0., 12.)}))};
    std::vector<ContourRun> runs;
    for (size_t wall = 0; wall < (scenario == "chain" ? 3 : 2); ++wall) {
        const double low = .2 + .4 * wall + (scenario == "gap" && wall ? .01 : 0.), high = 12. - low;
        runs.push_back({Polyline(Points{Point::new_scale(low, low), Point::new_scale(high, low), Point::new_scale(high, high),
                                        Point::new_scale(low, high), Point::new_scale(low, low)}),
                        .4, PathRole::Perimeter});
    }
    if (scenario == "open")
        runs.front().line.points.pop_back();
    const auto source = plan_contour_cycle(outline, runs, {17, 0, {1}, 0}, .12, .24, {}, process, {}, in.machine);
    if (const auto* failure = std::get_if<Failure>(&source))
        FAIL(failure->detail);
    auto plan = std::make_shared<PathPlan>(*std::get<std::shared_ptr<const PathPlan>>(source));
    REQUIRE(plan->paths.size() == runs.size());
    if (scenario != "floating")
        for (const auto& segment : plan->paths.back().segments)
            for (auto cell : segment.cells) {
                for (size_t k = 0; k < 3; ++k) {
                    cell.upper[k].z() = .12;
                    cell.lower_z[k]   = 0.;
                }
                in.occupied.push_back(std::move(cell));
            }
    if (scenario == "inner_first")
        std::reverse(plan->paths.begin(), plan->paths.end());
    if (scenario == "floor_margin" || scenario == "floor_slot" || scenario == "floor_gap") {
        in.occupied.clear();
        const double margin = scenario == "floor_margin" ? .05 : 0.;
        const auto   base   = [&](double x0, double x1, double y0, double y1) {
            const auto cells = rectangle(x0, x1, y0, y1, 0., .12);
            in.occupied.insert(in.occupied.end(), cells.begin(), cells.end());
        };
        base(.4 + margin, 6., .4 + margin, .8 - margin);
        base(scenario == "floor_gap" ? 6.5 : scenario == "floor_slot" ? 6.01 : 6., 11.6 - margin, .4 + margin, .8 - margin);
        base(.4 + margin, 11.6 - margin, 11.2 + margin, 11.6 - margin);
        base(.4 + margin, .8 - margin, .8 - margin, 11.2 + margin);
        base(11.2 + margin, 11.6 - margin, .8 - margin, 11.2 + margin);
    }
    plan->ordered_region_paths = plan->layered_process = true;
    for (size_t i = 0; i < plan->paths.size(); ++i) {
        plan->paths[i].id             = i + 1;
        plan->paths[i].overhang_mm    = i == 1 && (scenario == "floor_margin" || scenario == "floor_slot" || scenario == "floor_gap") ? .2 :
                                                                                                                                        0.;
        plan->paths[i].bridge_span_mm = 20.;
    }
    in.objects        = {{1, plan}};
    const auto result = schedule_paths(in);
    if (scenario == "reorder" || scenario == "inner_first" || scenario == "open" || scenario == "floor_margin" || scenario == "floor_slot") {
        const auto& schedule = accepted(result);
        REQUIRE(schedule.order.size() == 2);
        CHECK(schedule.order.front().operation.path == (scenario == "inner_first" ? 1 : 2));
    } else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
    }
}

TEST_CASE("Material wall point contacts retain attachment without bridging detached caps", "[material_band][layer_process][schedule]")
{
    const double gap = GENERATE(0., .001);
    CAPTURE(gap);
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    auto plan                       = std::make_shared<PathPlan>();
    plan->machine                   = in.machine;
    plan->ordered_region_paths = plan->layered_process = true;
    in.occupied                                        = rectangle(0., 8., 0., .4, 0., .12);
    for (size_t lane = 0; lane < 2; ++lane) {
        BandPath path;
        path.id          = lane + 1;
        path.band        = {17, 0, {1}, 0};
        path.role        = PathRole::Perimeter;
        const double low = lane ? .4 : 0., high = lane ? .55 : .4;
        for (double x = 0.; x <= 8.; x += 2.)
            path.vertices.emplace_back(x, (low + high) / 2., .24);
        for (double x = 0.; x < 8.; x += 2.) {
            PathSegment segment;
            segment.cells           = rectangle(x, x + 2., low, high, .12, .24);
            segment.volume_mm3      = 2. * (high - low) * .12;
            segment.width_mm        = high - low;
            segment.height_start_mm = segment.height_end_mm = .12;
            segment.speed_mm_s                              = 10.;
            path.segments.push_back(std::move(segment));
        }
        if (lane) {
            BeadCell cap;
            cap.upper   = {Vec3d(4., .55 + gap, .24), Vec3d(4.05, .59 + gap, .24), Vec3d(3.95, .59 + gap, .24)};
            cap.lower_z = {.12, .12, .12};
            path.segments[1].cells.push_back(cap);
            path.segments[1].volume_mm3 += .5 * .1 * .04 * .12;
        }
        plan->paths.push_back(std::move(path));
    }
    in.objects        = {{1, plan}};
    const auto result = schedule_paths(in);
    if (gap == 0.)
        CHECK(accepted(result).order.size() == 2);
    else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
    }
}

TEST_CASE("Material source infill trims motion over an already deposited prefix", "[material_band][layer_process][clipped_paths]")
{
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    ProcessConstraints process;
    process.bead_width_mm = .4;
    const ExPolygons outline{
        ExPolygon(Polygon({Point::new_scale(0., 0.), Point::new_scale(8., 0.), Point::new_scale(8., .4), Point::new_scale(0., .4)}))};
    const std::vector<ContourRun> runs{{Polyline(Points{Point::new_scale(.2, .2), Point::new_scale(4., .2)}), .4, PathRole::DenseFill},
                                       {Polyline(Points{Point::new_scale(.2, .2), Point::new_scale(7.8, .2)}), .4, PathRole::SparseFill}};
    const auto                    result = plan_contour_cycle(outline, runs, {17, 0, {1}, 0}, 0., .2, {}, process, {}, in.machine, {});
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure->detail);
    const auto plan   = std::get<std::shared_ptr<const PathPlan>>(result);
    size_t     sparse = 0;
    for (const auto& path : plan->paths)
        if (path.role == PathRole::SparseFill) {
            ++sparse;
            CHECK(path.vertices.front().x() >= 4.2 - 1e-6);
        }
    CHECK(sparse > 0);
    CHECK(std::abs(plan->volume_mm3[0] - .64) < 1e-6);
    in.objects = {{1, plan}};
    CHECK(accepted(schedule_paths(in)).order.size() == plan->paths.size());
}

TEST_CASE("Material closed dense contours retain their seam motion", "[material_band][layer_process][clipped_paths]")
{
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    ProcessConstraints process;
    process.bead_width_mm = .4;
    const ExPolygons outline{
        ExPolygon(Polygon({Point::new_scale(0., 0.), Point::new_scale(4., 0.), Point::new_scale(4., 4.), Point::new_scale(0., 4.)}))};
    const std::vector<ContourRun> runs{{Polyline(Points{Point::new_scale(.2, .2), Point::new_scale(3.8, .2), Point::new_scale(3.8, 3.8),
                                                        Point::new_scale(.2, 3.8), Point::new_scale(.2, .2)}),
                                        .4, PathRole::DenseFill}};
    const auto                    result = plan_contour_cycle(outline, runs, {17, 0, {1}, 0}, 0., .2, {}, process, {}, in.machine, {});
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure->detail);
    const auto plan = std::get<std::shared_ptr<const PathPlan>>(result);
    REQUIRE(plan->paths.size() == 1);
    CHECK((plan->paths.front().vertices.front() - plan->paths.front().vertices.back()).norm() < 1e-10);
    CHECK(std::abs(plan->volume_mm3[0] - 1.152) < 1e-6);
}

TEST_CASE("Material solid gap repair anchors sparse source runs before deposition", "[material_band][layer_process][schedule]")
{
    auto in                         = input();
    in.machine.coplanar_tip_contact = true;
    ProcessConstraints process;
    process.bead_width_mm = .4;
    const auto polygon    = [](double x0, double x1, double y0, double y1) {
        return ExPolygon(Polygon({Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)}));
    };
    const ExPolygons              outline{polygon(0., 12., -.4, .4)};
    const ExPolygons              solid{polygon(0., 1., -.4, 0.), polygon(11., 12., -.4, 0.)};
    const std::vector<ContourRun> runs{{Polyline(Points{Point::new_scale(0., .2), Point::new_scale(12., .2)}), .4, PathRole::SparseFill}};
    const BandId                  band{17, 0, {1}, 0};
    const auto source = plan_contour_cycle(outline, runs, band, .12, .24, {}, process, {}, in.machine, {}, {}, {}, solid);
    if (const auto* failure = std::get_if<Failure>(&source))
        FAIL(failure->detail);
    const auto& geometry = *std::get<std::shared_ptr<const PathPlan>>(source);
    Polygons    pieces;
    for (const auto& path : geometry.paths)
        for (const auto& segment : path.segments)
            for (const auto& triangle : segment.coverage_triangles)
                pieces.emplace_back(Points{Point::new_scale(triangle[0].x(), triangle[0].y()),
                                           Point::new_scale(triangle[1].x(), triangle[1].y()),
                                           Point::new_scale(triangle[2].x(), triangle[2].y())});
    const auto partition = plan_partitioned_cycle(geometry, {{band, union_ex(pieces), .12, .24, 0, 0}}, process, {}, {});
    if (const auto* failure = std::get_if<Failure>(&partition))
        FAIL(failure->detail);
    auto plan      = std::make_shared<PathPlan>(*std::get<std::shared_ptr<const PathPlan>>(partition));
    in.objects     = {{1, plan}};
    in.occupied    = rectangle(0., 1., -.4, 0., 0., .12);
    const auto end = rectangle(11., 12., -.4, 0., 0., .12);
    in.occupied.insert(in.occupied.end(), end.begin(), end.end());
    CHECK(accepted(schedule_paths(in)).order.size() == plan->paths.size());

    // Same geometry and dose, but delaying the solid end anchors must fail.
    std::stable_sort(plan->paths.begin(), plan->paths.end(), [](const BandPath& a, const BandPath& b) {
        return (a.role == PathRole::SparseFill) > (b.role == PathRole::SparseFill);
    });
    for (size_t i = 0; i < plan->paths.size(); ++i)
        plan->paths[i].id = i + 1;
    const auto late = schedule_paths(in);
    REQUIRE(std::holds_alternative<Failure>(late));
    CHECK(std::get<Failure>(late).detail == "unsupported_bead_footprint");
    in.allow_planar_reordering = true;
    CHECK(accepted(schedule_paths(in)).order.size() == plan->paths.size());
    in.occupied.clear();
    const auto floating = schedule_paths(in);
    REQUIRE(std::holds_alternative<Failure>(floating));
    CHECK(std::get<Failure>(floating).detail == "unsupported_bead_footprint");
}

TEST_CASE("Material narrow gap lanes require continuous earlier full-height side contact", "[material_band][layer_process][schedule]")
{
    const std::string scenario      = GENERATE("attached", "gap", "late", "floating", "shallow", "wide", "wide_cap", "noncoplanar_machine");
    auto              in            = input();
    in.machine.coplanar_tip_contact = scenario != "noncoplanar_machine";
    auto plan                       = std::make_shared<PathPlan>();
    plan->machine                   = in.machine;
    plan->ordered_region_paths = plan->layered_process = true;
    const double anchor_bottom                         = scenario == "shallow" ? .18 : .12;
    const double y                                     = scenario == "gap" ? .41 : .4;
    const double width                                 = scenario == "wide" ? .3 : scenario == "wide_cap" ? .08 : .15;
    for (size_t lane = 0; lane < 2; ++lane) {
        BandPath p;
        p.id                = scenario == "late" ? 2 - lane : 1 + lane;
        p.band              = {17, 0, {1}, 0};
        p.role              = lane == 0 ? PathRole::Bridge : PathRole::DenseFill;
        p.overhang_mm       = .2;
        p.bridge_span_mm    = lane == 0 ? 20. : 0.;
        const double bottom = lane == 0 ? anchor_bottom : .12;
        const double low_y = lane == 0 ? 0. : y, high_y = lane == 0 ? .4 : y + width;
        for (double x = 0.; x <= 8.; x += 2.)
            p.vertices.emplace_back(x, (low_y + high_y) / 2., .24);
        for (double x = 0.; x < 8.; x += 2.) {
            PathSegment s;
            s.cells           = rectangle(x, x + 2., low_y, high_y, bottom, .24);
            s.volume_mm3      = 2. * (high_y - low_y) * (.24 - bottom);
            s.width_mm        = lane == 1 && scenario == "wide_cap" ? .05 : high_y - low_y;
            s.height_start_mm = s.height_end_mm = .24 - bottom;
            s.speed_mm_s                        = 10.;
            p.segments.push_back(std::move(s));
        }
        plan->paths.push_back(std::move(p));
    }
    in.objects = {{1, plan}};
    if (scenario != "floating") {
        in.occupied    = rectangle(0., 1., 0., .4, 0., anchor_bottom);
        const auto end = rectangle(7., 8., 0., .4, 0., anchor_bottom);
        in.occupied.insert(in.occupied.end(), end.begin(), end.end());
    }
    const auto result = schedule_paths(in);
    if (scenario == "attached")
        CHECK(accepted(result).order.size() == 2);
    else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).detail == "unsupported_bead_footprint");
    }
}
