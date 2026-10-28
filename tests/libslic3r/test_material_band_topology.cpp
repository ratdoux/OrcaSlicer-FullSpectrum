#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/MaterialBand/Topology.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Triangulation.hpp"

#include <algorithm>
#include <cmath>

using namespace Slic3r;
using namespace Slic3r::MaterialBand;

namespace {
DomainSection rectangle(double x, double y, uint64_t id = 1, uint64_t object = 42)
{
    DomainSection domain;
    domain.id                       = {object, id};
    domain.phase.order              = {{{1}, {2}}};
    domain.phase.first_active_cycle = 9;
    domain.footprint.vertices       = {{x, y, 0.}, {x + 2., y, 0.}, {x + 2., y + 2., 0.}, {x, y + 2., 0.}};
    domain.footprint.triangles      = {{0, 1, 2}, {0, 2, 3}};
    return domain;
}
DomainFrame frame(uint64_t cycle, std::vector<DomainSection> domains) { return {{1, 2, 3, 4, 5, 6, 7}, cycle, std::move(domains)}; }
DomainFrame sphere_section(double z, uint64_t cycle, uint64_t first_id)
{
    // Exact analytic cross sections of the two radius-3 spheres (after the
    // specified cut/translation), with a fixed 128-sided polygon approximation.
    const double radius = std::sqrt(9. - (z - 2.5) * (z - 2.5));
    Polygons     circles;
    for (double center : {-2., 2.}) {
        Points points;
        for (size_t i = 0; i < 128; ++i) {
            const double angle = 2. * PI * i / 128.;
            points.emplace_back(coord_t(std::llround((center + radius * std::cos(angle)) / SCALING_FACTOR)),
                                coord_t(std::llround(radius * std::sin(angle) / SCALING_FACTOR)));
        }
        circles.emplace_back(std::move(points));
    }
    auto       components = union_ex(circles);
    const auto minimum_x  = [](const ExPolygon& polygon) {
        coord_t result = polygon.contour.points.front().x();
        for (const auto& p : polygon.contour.points)
            result = std::min(result, p.x());
        return result;
    };
    std::sort(components.begin(), components.end(), [&](const auto& a, const auto& b) { return minimum_x(a) < minimum_x(b); });
    std::vector<DomainSection> domains;
    for (const auto& component : components) {
        DomainSection domain = rectangle(0., 0., first_id + domains.size());
        domain.footprint     = {};
        const Points points  = to_points(component);
        for (const auto& point : points)
            domain.footprint.vertices.emplace_back(point.x() * SCALING_FACTOR, point.y() * SCALING_FACTOR, z);
        for (const auto& triangle : Triangulation::triangulate(component)) {
            std::array<uint32_t, 3> face{{uint32_t(triangle[0]), uint32_t(triangle[1]), uint32_t(triangle[2])}};
            const auto&             v = domain.footprint.vertices;
            if ((v[face[1]] - v[face[0]]).cross(v[face[2]] - v[face[0]]).z() < 0.)
                std::swap(face[1], face[2]);
            domain.footprint.triangles.push_back(face);
        }
        domains.push_back(std::move(domain));
    }
    return frame(cycle, std::move(domains));
}
const TopologyTransition& accepted(const TopologyResult& result)
{
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure_name(failure->code) << ": " << failure->detail);
    return *std::get<std::shared_ptr<const TopologyTransition>>(result);
}
} // namespace

TEST_CASE("Material band topology records the analytic sphere merge and split", "[material_band][unit][topology]")
{
    const bool split  = GENERATE(false, true);
    const auto before = sphere_section(split ? 4.64 : .12, split ? 29 : 9, 101);
    const auto after  = sphere_section(split ? 4.88 : .36, split ? 30 : 10, 201);
    CHECK(before.domains.size() == (split ? 1 : 2));
    CHECK(after.domains.size() == (split ? 2 : 1));
    CHECK(2.5 - std::sqrt(5.) > .12);
    CHECK(2.5 - std::sqrt(5.) < .36);
    CHECK(2.5 + std::sqrt(5.) > 4.64);
    CHECK(2.5 + std::sqrt(5.) < 4.88);
    const auto  result     = link_domains(before, after, .45);
    const auto& transition = accepted(result);
    REQUIRE(transition.events.size() == 1);
    CHECK(transition.events.front().kind == (split ? TopologyEventKind::Split : TopologyEventKind::Merge));
    CHECK(transition.events.front().parents.size() == before.domains.size());
    CHECK(transition.events.front().children.size() == after.domains.size());
    REQUIRE(transition.connections.size() == 2);
    for (const auto& connection : transition.connections) {
        CHECK(connection.overlap_mm2 > connection.printable_overlap_mm2);
        CHECK(connection.printable_overlap_mm2 > 0.);
    }
}

TEST_CASE("Material band topology preserves pure-material absence and phase", "[material_band][unit][topology]")
{
    auto before                            = frame(12, {rectangle(0., 0.)});
    auto after                             = frame(13, {rectangle(0., 0., 99)});
    after.domains.front().active_materials = 1;
    auto        result                     = link_domains(before, after, .45);
    const auto& termination                = accepted(result);
    REQUIRE(termination.events.size() == 1);
    CHECK(termination.events.front().kind == TopologyEventKind::MaterialTermination);
    REQUIRE(termination.events.front().material);
    CHECK(termination.events.front().material->value == 2);
    before                                 = after;
    after.macro_cycle                      = 14;
    after.domains.front().active_materials = 3;
    result                                 = link_domains(before, after, .45);
    const auto& birth                      = accepted(result);
    REQUIRE(birth.events.size() == 1);
    CHECK(birth.events.front().kind == TopologyEventKind::MaterialBirth);
    CHECK(before.domains.front().phase.first_active_cycle == 9);
    CHECK(after.domains.front().phase.first_active_cycle == 9);
    after.domains.front().phase.first_active_cycle = 0;
    result                                         = link_domains(before, after, .45);
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == FailureCode::IncompatibleOrder);
}

TEST_CASE("Material band topology rejects false joins and bounded-work violations", "[material_band][unit][topology]")
{
    const int scenario = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    CAPTURE(scenario);
    auto           before = frame(9, {rectangle(0., 0.)});
    auto           after  = frame(10, {rectangle(0., 0.)});
    TopologyLimits limits;
    FailureCode    expected = FailureCode::InvalidInput;
    if (scenario == 0)
        after.domains.front() = rectangle(0., 0., 1, 43); // distinct objects, same tools/coordinates
    if (scenario == 1)
        after.domains.front() = rectangle(2., 0.); // touching is not positive overlap
    if (scenario == 2) {
        after.domains.front() = rectangle(1.9, 0.);
        expected              = FailureCode::InfeasibleThickness;
    }
    if (scenario == 3) {
        std::swap(after.domains.front().phase.order[0], after.domains.front().phase.order[1]);
        expected = FailureCode::IncompatibleOrder;
    }
    if (scenario == 4) {
        limits.max_candidates = 0;
        expected              = FailureCode::ResourceLimit;
    }
    if (scenario == 5) {
        limits.max_grid_references = 1;
        expected                   = FailureCode::ResourceLimit;
    }
    if (scenario == 6)
        expected = FailureCode::Cancelled;
    if (scenario == 7)
        expected = FailureCode::StaleInput;
    const auto result = link_domains(before, after, .45, limits, [&] { return scenario == 6; }, [&] { return scenario == 7; });
    if (scenario < 2) {
        const auto& transition = accepted(result);
        CHECK(transition.connections.empty());
        REQUIRE(transition.events.size() == 2);
        CHECK(transition.events[0].kind == TopologyEventKind::Termination);
        CHECK(transition.events[1].kind == TopologyEventKind::Birth);
    } else {
        REQUIRE(std::holds_alternative<Failure>(result));
        CHECK(std::get<Failure>(result).code == expected);
    }
}

TEST_CASE("Material band topology candidate growth stays local and insertion order is immaterial", "[material_band][unit][topology]")
{
    const size_t               count = GENERATE(32, 128, 512);
    std::vector<DomainSection> domains;
    for (size_t i = 0; i < count; ++i)
        domains.push_back(rectangle(4. * i, 0., i + 1));
    const auto before = frame(9, domains);
    std::reverse(domains.begin(), domains.end());
    const auto  after      = frame(10, std::move(domains));
    const auto  result     = link_domains(before, after, .45);
    const auto& transition = accepted(result);
    CHECK(transition.events.empty());
    REQUIRE(transition.connections.size() == count);
    CHECK(transition.candidate_pairs <= 3 * count);
    CHECK(transition.grid_references <= 20 * count);
    for (size_t i = 0; i < count; ++i) {
        CHECK(transition.connections[i].parent.domain == i + 1);
        CHECK(transition.connections[i].child.domain == i + 1);
    }
}
