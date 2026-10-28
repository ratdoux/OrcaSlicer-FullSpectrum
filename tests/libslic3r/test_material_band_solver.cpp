#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/MaterialBand/SlabSolver.hpp"
#include "material_band_fixtures.hpp"

#include <cmath>
#include <limits>
#include <set>

using namespace Slic3r;
using namespace Slic3r::MaterialBand;

namespace {
SlabProblem slab_fixture(const std::string& id)
{
    const auto     corpus = MaterialBandFixtures::corpus();
    nlohmann::json spec;
    for (const auto& candidate : corpus.at("fixtures"))
        if (candidate.at("id") == id)
            spec = candidate;
    REQUIRE_FALSE(spec.is_null());
    SlabProblem p;
    p.interface_id      = 101;
    p.revision          = {1, 2, 3, 4, 5, 6, 7};
    p.domain            = 100;
    p.bottom_mm         = .96;
    p.top_mm            = 1.2;
    const double length = spec.at("dimensions_mm").at(0), width = spec.at("dimensions_mm").at(1);
    p.target_volume_a_mm3  = length * width * .24 / 2.;
    p.discontinuous_target = spec.at("field") == "step";
    if (spec.at("boundary_policy") == "allow_transition") {
        p.boundary.policy                    = BoundaryPolicy::AllowTransition;
        p.boundary.transition_width_mm       = spec.at("transition_width_mm");
        p.boundary.transition_fraction_error = spec.at("transition_fraction_error");
    }
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
    return p;
}

const SlabSolution& solved(const SlabResult& result)
{
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure_name(failure->code) << ": " << failure->detail);
    const auto& solution = std::get<std::shared_ptr<const SlabSolution>>(result);
    REQUIRE(solution);
    return *solution;
}

void check_partition(const SlabProblem& problem, const SlabSolution& solution)
{
    double      a_volume = 0., b_volume = 0.;
    const auto& mesh = solution.interface.surface;
    for (const auto& triangle : mesh.triangles) {
        const Vec3d a = mesh.vertices[triangle[0]], b = mesh.vertices[triangle[1]], c = mesh.vertices[triangle[2]];
        const Vec3d normal = (b - a).cross(c - a);
        REQUIRE(normal.z() > 0.);
        const double area = normal.z() / 2.;
        CHECK(std::hypot(normal.x(), normal.y()) / normal.z() <= problem.process.max_interface_gradient + 1e-9);
        a_volume += area * ((a.z() + b.z() + c.z()) / 3. - problem.bottom_mm);
        b_volume += area * (problem.top_mm - (a.z() + b.z() + c.z()) / 3.);
        for (uint32_t index : triangle) {
            const double lower = mesh.vertices[index].z() - problem.bottom_mm;
            const double upper = problem.top_mm - mesh.vertices[index].z();
            CHECK(lower >= problem.process.min_active_thickness_mm - 1e-8);
            CHECK(upper >= problem.process.min_active_thickness_mm - 1e-8);
            CHECK(lower + upper == Approx(problem.top_mm - problem.bottom_mm).margin(1e-12));
            CHECK(std::abs(lower / (problem.top_mm - problem.bottom_mm) - problem.target_fraction_a[index]) <=
                  problem.local_fraction_error[index] + 1e-8);
        }
    }
    CHECK(a_volume == Approx(problem.target_volume_a_mm3).margin(1e-6));
    CHECK(a_volume == Approx(solution.volume_mm3[0]).margin(1e-8));
    CHECK(b_volume == Approx(solution.volume_mm3[1]).margin(1e-8));
    CHECK(solution.revision == problem.revision);
    CHECK(solution.interface.id == problem.interface_id);
}
} // namespace

TEST_CASE("Material band shared interfaces reproduce constant and linear slab oracles", "[material_band][unit]")
{
    const auto  id       = GENERATE("MB01", "MB03");
    auto        problem  = slab_fixture(id);
    const auto  result   = solve_slab(problem);
    const auto& solution = solved(result);
    check_partition(problem, solution);
    for (size_t i = 0; i < problem.vertices.size(); ++i)
        CHECK(solution.interface.surface.vertices[i].z() == Approx(problem.bottom_mm + .24 * problem.target_fraction_a[i]).margin(1e-10));
    CHECK(solution.optimization_steps == 1);
    CHECK(solution.projection_sweeps <= 2);
    CHECK(solution.interface.surface.vertices.size() == problem.vertices.size());
    CHECK(solution.interface.surface.triangles == problem.triangles);
}

TEST_CASE("Material band hard paint edges and permitted transitions have distinct outcomes", "[material_band][unit]")
{
    auto hard   = slab_fixture("MB02-edge");
    auto result = solve_slab(hard);
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == FailureCode::UnsupportedTopology);
    CHECK(std::get<Failure>(result).detail == "discontinuous_interface");
    const auto transition = slab_fixture("MB02-transition");
    result                = solve_slab(transition);
    const auto& solution  = solved(result);
    check_partition(transition, solution);
    for (size_t i = 0; i < transition.vertices.size(); ++i) {
        const double x        = transition.vertices[i].x();
        const double expected = x <= 5. ? .06 : x >= 7. ? .18 : .12;
        CHECK(solution.interface.surface.vertices[i].z() == Approx(transition.bottom_mm + expected).margin(1e-8));
    }
}

TEST_CASE("Material band constraints fail atomically without silently replacing recipes", "[material_band][unit]")
{
    auto              p = slab_fixture("MB01");
    SolverLimits      limits;
    FailureCode       expected = FailureCode::InvalidInput;
    const std::string scenario = GENERATE("Minimum printable thickness is infeasible", "Integrated dose cannot be achieved",
                                          "Pure endpoint needs a termination rule", "NaN target is invalid", "NaN geometry is invalid",
                                          "Bad triangle index is invalid", "Reversed triangles are invalid", "Vertex budget bounds work",
                                          "Memory budget counts generated constraints", "Zero iterations do not publish success",
                                          "Unbounded relative budget is invalid", "Domain volume overflow is invalid");
    CAPTURE(scenario);
    if (scenario == "Unbounded relative budget is invalid")
        p.tolerances.relative_volume = 1e308;
    if (scenario == "Domain volume overflow is invalid") {
        p.vertices  = {{0., 0.}, {1e154, 0.}, {0., 1e154}};
        p.triangles = {{0, 1, 2}};
        p.target_fraction_a.assign(3, .5);
        p.local_fraction_error.assign(3, 0.);
        p.top_mm                          = p.bottom_mm + 24.;
        p.process.min_active_thickness_mm = 1.;
        p.process.max_active_thickness_mm = 24.;
        p.target_volume_a_mm3             = 1e308;
    }
    if (scenario == "Minimum printable thickness is infeasible") {
        p.process.min_active_thickness_mm = .13;
        expected                          = FailureCode::InfeasibleThickness;
    }
    if (scenario == "Integrated dose cannot be achieved") {
        p.target_volume_a_mm3 *= 1.1;
        expected = FailureCode::MaterialToleranceExceeded;
    }
    if (scenario == "Pure endpoint needs a termination rule") {
        p.target_fraction_a[0] = 0.;
        expected               = FailureCode::UnsupportedTopology;
    }
    if (scenario == "NaN target is invalid") {
        p.target_fraction_a[0] = std::numeric_limits<double>::quiet_NaN();
    }
    if (scenario == "NaN geometry is invalid") {
        p.vertices[0].x() = std::numeric_limits<double>::infinity();
    }
    if (scenario == "Bad triangle index is invalid") {
        p.triangles[0][0] = 9999;
    }
    if (scenario == "Reversed triangles are invalid") {
        std::swap(p.triangles[0][0], p.triangles[0][1]);
    }
    if (scenario == "Vertex budget bounds work") {
        limits.max_vertices = 3;
        expected            = FailureCode::ResourceLimit;
    }
    if (scenario == "Memory budget counts generated constraints") {
        limits.max_temporary_bytes = 1024;
        expected                   = FailureCode::ResourceLimit;
    }
    if (scenario == "Zero iterations do not publish success") {
        limits.max_projection_sweeps = 0;
        expected                     = FailureCode::NonConverged;
    }
    const auto result = solve_slab(p, limits);
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == expected);
    CHECK(std::get<Failure>(result).revision == p.revision);
}

TEST_CASE("Material band gradient constraints examine faces rather than only edges", "[material_band][unit]")
{
    SlabProblem p;
    p.interface_id = 102;
    p.vertices     = {{0., 0.}, {1., 0.}, {0., 1.}};
    p.triangles    = {{{0, 1, 2}}};
    // Every edge slope is <= .08, but the face gradient norm is sqrt(2)*.08.
    p.target_fraction_a    = {.25, .25 + .08 / .24, .25 + .08 / .24};
    p.local_fraction_error = {0., 0., 0.};
    p.target_volume_a_mm3  = .5 * (.06 + .14 + .14) / 3.;
    SolverLimits limits;
    limits.max_projection_sweeps = 200;
    const auto result            = solve_slab(p, limits);
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == FailureCode::NonConverged);
}

TEST_CASE("Material band cancellation and revision changes cannot publish an interface", "[material_band][unit]")
{
    const auto p         = slab_fixture("MB02-transition");
    int        polls     = 0;
    const auto cancelled = solve_slab(p, {}, [&] { return ++polls == 3; });
    REQUIRE(std::holds_alternative<Failure>(cancelled));
    CHECK(std::get<Failure>(cancelled).code == FailureCode::Cancelled);
    const auto stale = solve_slab(p, {}, {}, [] { return true; });
    REQUIRE(std::holds_alternative<Failure>(stale));
    CHECK(std::get<Failure>(stale).code == FailureCode::StaleInput);
}

TEST_CASE("Material band slab solutions survive large coordinate translations", "[material_band][unit]")
{
    auto       p        = slab_fixture("MB03");
    const auto original = solve_slab(p);
    for (auto& vertex : p.vertices)
        vertex += Vec2d(1000000., -1000000.);
    p.bottom_mm += 1000.;
    p.top_mm += 1000.;
    const auto  translated = solve_slab(p);
    const auto& a          = solved(original);
    const auto& b          = solved(translated);
    CHECK(a.volume_mm3[0] == Approx(b.volume_mm3[0]).margin(1e-8));
    for (size_t i = 0; i < p.vertices.size(); ++i)
        CHECK(a.interface.surface.vertices[i].z() == Approx(b.interface.surface.vertices[i].z() - 1000.).margin(1e-9));
}

TEST_CASE("Material band constrained smoothing reduces the declared objective without changing dose", "[material_band][unit]")
{
    auto p                        = slab_fixture("MB01");
    p.tolerances.plateau_fraction = .1; // Explicit budget for this numerical objective test.
    for (size_t i = 0; i < p.vertices.size(); ++i) {
        p.target_fraction_a[i]    = i % 2 ? .53 : .47;
        p.local_fraction_error[i] = .1;
    }
    SolverLimits limits;
    limits.smoothness_weight = .1;
    const auto  unsmoothed   = solve_slab(p);
    const auto  smoothed     = solve_slab(p, limits);
    const auto& first        = solved(unsmoothed);
    const auto& second       = solved(smoothed);
    check_partition(p, second);
    std::set<std::pair<size_t, size_t>> edges;
    for (const auto& face : p.triangles)
        for (size_t i = 0; i < 3; ++i)
            edges.emplace(std::min(face[i], face[(i + 1) % 3]), std::max(face[i], face[(i + 1) % 3]));
    const auto objective = [&](const SlabSolution& solution) {
        double      value    = 0.;
        const auto& vertices = solution.interface.surface.vertices;
        for (size_t i = 0; i < vertices.size(); ++i)
            value += .5 * std::pow(vertices[i].z() - p.bottom_mm - .24 * p.target_fraction_a[i], 2);
        for (const auto& edge : edges)
            value += .5 * limits.smoothness_weight * std::pow(vertices[edge.first].z() - vertices[edge.second].z(), 2);
        return value;
    };
    CHECK(objective(second) < objective(first));
    CHECK(first.volume_mm3[0] == Approx(second.volume_mm3[0]).margin(1e-8));
    CHECK(second.optimization_steps > 1);
}

TEST_CASE("Material band transition error stays inside its declared neighborhood", "[material_band][unit]")
{
    auto      p        = slab_fixture("MB02-transition");
    const int scenario = GENERATE(0, 1, 2);
    if (scenario == 0)
        p.local_fraction_error.front() = .25; // x=0 lies outside [5,7].
    if (scenario == 1)
        p.local_fraction_error[4] = .26; // x=6 exceeds the allowed local transition error.
    if (scenario == 2)
        p.boundary_distance_mm.clear();
    const auto result = solve_slab(p);
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == FailureCode::InvalidInput);
}
