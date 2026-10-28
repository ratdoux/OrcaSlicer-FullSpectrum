#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/MaterialBand/SolidClipper.hpp"
#include "libslic3r/MaterialBand/ClippedPlanner.hpp"
#include "libslic3r/ClipperUtils.hpp"

#include <cmath>
#include <map>

using namespace Slic3r;
using namespace Slic3r::MaterialBand;

namespace {
// Independently triangulated regular polygon solid with optional coaxial void.
Mesh axial_solid(unsigned n, double bottom_radius, double top_radius, double inner_radius, double height, bool square = false)
{
    Mesh mesh;
    for (unsigned ring = 0; ring < (inner_radius > 0. ? 4u : 2u); ++ring)
        for (unsigned i = 0; i < n; ++i) {
            const double angle = 2. * PI * i / n, x = std::cos(angle), y = std::sin(angle);
            double       radius = ring < 2 ? (ring == 0 ? bottom_radius : top_radius) : inner_radius;
            if (square && ring < 2)
                radius /= std::max(std::abs(x), std::abs(y));
            mesh.vertices.emplace_back(radius * x, radius * y, ring % 2 ? height : 0.);
        }
    for (unsigned i = 0; i < n; ++i) {
        const unsigned j = (i + 1) % n;
        mesh.triangles.push_back({i, j, n + j});
        mesh.triangles.push_back({i, n + j, n + i});
        if (inner_radius > 0.) {
            mesh.triangles.push_back({2 * n + i, 3 * n + j, 2 * n + j});
            mesh.triangles.push_back({2 * n + i, 3 * n + i, 3 * n + j});
            mesh.triangles.push_back({i, 2 * n + j, j});
            mesh.triangles.push_back({i, 2 * n + i, 2 * n + j});
            mesh.triangles.push_back({n + i, n + j, 3 * n + j});
            mesh.triangles.push_back({n + i, 3 * n + j, 3 * n + i});
        } else if (i > 0 && i + 1 < n) {
            mesh.triangles.push_back({0, j, i});
            mesh.triangles.push_back({n, n + i, n + j});
        }
    }
    return mesh;
}

SlabProblem problem()
{
    SlabProblem p;
    p.revision     = {1, 2, 3, 4, 5, 6, 7};
    p.interface_id = 91;
    p.domain       = 12;
    p.macro_cycle  = 9;
    p.bottom_mm    = .96;
    p.top_mm       = 1.2;
    p.vertices     = {{-7., -7.}, {7., -7.}, {7., 7.}, {-7., 7.}};
    p.triangles    = {{0, 1, 2}, {0, 2, 3}};
    p.target_fraction_a.assign(4, .5);
    p.local_fraction_error.assign(4, .005);
    p.target_volume_a_mm3 = 14. * 14. * .24 / 2.;
    return p;
}

double signed_volume(const Mesh& mesh)
{
    double      result = 0.;
    const Vec3d origin = mesh.vertices.front();
    for (const auto& face : mesh.triangles)
        result += (mesh.vertices[face[0]] - origin).dot((mesh.vertices[face[1]] - origin).cross(mesh.vertices[face[2]] - origin)) / 6.;
    return result;
}
} // namespace

TEST_CASE("Material band clipping partitions actual curved solids and holes", "[material_band][unit][clipping]")
{
    const std::string shape = GENERATE("cylinder", "annulus", "frustum", "hole_plate");
    CAPTURE(shape);
    const unsigned n       = 64;
    const bool     annulus = shape == "annulus", frustum = shape == "frustum", square = shape == "hole_plate";
    const double   r0 = shape == "cylinder" ? 5. : 6., r1 = frustum ? 4. : r0;
    const double   inner          = annulus ? 5. : square ? 2. : 0.;
    Mesh           mesh           = axial_solid(n, r0, r1, inner, 4.8, square);
    auto           p              = problem();
    const double   polygon_factor = n * std::sin(2. * PI / n) / 2.;
    const double   slope          = (r1 - r0) / 4.8;
    const auto     integral       = [&](double z) { return r0 * r0 * z + r0 * slope * z * z + slope * slope * z * z * z / 3.; };
    const double   expected       = square ? (144. - polygon_factor * inner * inner) * .24 :
                                             polygon_factor * (integral(p.top_mm) - integral(p.bottom_mm) - inner * inner * .24);
    const auto     solved         = solve_slab(p);
    REQUIRE(std::holds_alternative<std::shared_ptr<const SlabSolution>>(solved));
    StackPhase phase;
    phase.order              = {{{1}, {2}}};
    phase.first_active_cycle = 9;
    const auto clipped       = clip_bands(mesh, p, *std::get<std::shared_ptr<const SlabSolution>>(solved), phase, expected / 2.);
    if (const auto* failure = std::get_if<Failure>(&clipped))
        FAIL(failure_name(failure->code) << ": " << failure->detail);
    const auto& result = *std::get<std::shared_ptr<const MaterialBandComplex>>(clipped);
    CHECK(std::abs(result.domain_volume_mm3 - expected) < 1e-9);
    CHECK(std::abs(result.bands[0].volume_mm3 - expected / 2.) < expected * 1e-4);
    CHECK(std::abs(result.bands[1].volume_mm3 - expected / 2.) < expected * 1e-4);
    CHECK(result.bands[0].shared_interface == result.bands[1].shared_interface);
    REQUIRE_FALSE(result.shared_interface.surface.triangles.empty());
    for (const auto& band : result.bands) {
        CHECK(band.id.macro_cycle == 9);
        CHECK(std::abs(signed_volume(band.solid) - band.volume_mm3) < 1e-9);
        SolidQuery query(band.solid);
        if (inner > 0.) {
            CHECK_FALSE(query.contains({0., 0., 1.08}));
            CHECK(query.distance_mm({0., 0., 1.08}) > inner * .99);
        }
        for (const auto& vertex : band.solid.vertices) {
            CHECK(vertex.z() >= p.bottom_mm - 1e-10);
            CHECK(vertex.z() <= p.top_mm + 1e-10);
            const double radial = vertex.head<2>().norm();
            if (!square)
                CHECK(radial <= r0 + slope * vertex.z() + 1e-9);
            if (inner > 0.)
                CHECK(radial >= inner * std::cos(PI / n) - 1e-9);
        }
    }
    if (frustum) {
        CHECK(result.correction_iterations > 0);
        CHECK(result.interface_correction_mm < 0.);
        CHECK(std::abs(result.interface_correction_mm) <= .24 * .005);
    } else
        CHECK(std::abs(result.interface_correction_mm) < 1e-12);
}

TEST_CASE("Material band clipping fails atomically under bounded correction and cancellation", "[material_band][unit][clipping]")
{
    const int scenario = GENERATE(0, 1, 2, 3, 4, 5);
    CAPTURE(scenario);
    auto p     = problem();
    Mesh mesh  = axial_solid(32, 6., 4., 0., 4.8);
    auto solve = solve_slab(p);
    REQUIRE(std::holds_alternative<std::shared_ptr<const SlabSolution>>(solve));
    ClippingLimits limits;
    FailureCode    expected = FailureCode::NonConverged;
    if (scenario == 0)
        limits.max_correction_iterations = 0;
    if (scenario == 1) {
        limits.max_generated_elements = 5;
        expected                      = FailureCode::ResourceLimit;
    }
    if (scenario == 2) {
        mesh.triangles.pop_back();
        expected = FailureCode::InvalidInput;
    }
    if (scenario == 3)
        expected = FailureCode::Cancelled;
    if (scenario == 4)
        expected = FailureCode::StaleInput;
    if (scenario == 5) {
        limits.max_temporary_bytes = 100;
        expected                   = FailureCode::ResourceLimit;
    }
    StackPhase phase;
    phase.order       = {{{1}, {2}}};
    const auto result = clip_bands(
        mesh, p, *std::get<std::shared_ptr<const SlabSolution>>(solve), phase, 11., limits, [&] { return scenario == 3; },
        [&] { return scenario == 4; });
    REQUIRE(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == expected);
}

TEST_CASE("Material band clipped paths follow curved contours and preserve holes", "[material_band][unit][clipped_paths]")
{
    const std::string shape = GENERATE("cylinder", "annulus", "frustum", "hole_plate");
    CAPTURE(shape);
    const unsigned n       = 64;
    const bool     frustum = shape == "frustum", square = shape == "hole_plate";
    const double   r0 = shape == "cylinder" ? 5. : 6., r1 = frustum ? 4. : r0;
    const double   inner = shape == "annulus" ? 5. : square ? 2. : 0.;
    const Mesh     solid = axial_solid(n, r0, r1, inner, 4.8, square);
    auto           p     = problem();
    if (frustum) {
        p.top_mm                  = 1.08;
        p.process.macro_height_mm = .12;
    }
    const double height   = p.top_mm - p.bottom_mm;
    const Vec2d  gradient = frustum ? Vec2d::Zero() : Vec2d(.004, .002);
    for (size_t i = 0; i < p.vertices.size(); ++i)
        p.target_fraction_a[i] = .5 + gradient.dot(p.vertices[i]) / height;
    p.target_volume_a_mm3 = 196. * height / 2.;
    const auto solved     = solve_slab(p);
    REQUIRE(std::holds_alternative<std::shared_ptr<const SlabSolution>>(solved));
    const double factor = n * std::sin(2. * PI / n) / 2., slope = (r1 - r0) / 4.8;
    const auto   integral = [&](double z) { return r0 * r0 * z + r0 * slope * z * z + slope * slope * z * z * z / 3.; };
    const double expected = square ? (144. - factor * inner * inner) * height :
                                     factor * (integral(p.top_mm) - integral(p.bottom_mm) - inner * inner * height);
    StackPhase   phase;
    phase.order              = {{{1}, {2}}};
    phase.first_active_cycle = 9;
    const auto clipped       = clip_bands(solid, p, *std::get<std::shared_ptr<const SlabSolution>>(solved), phase, expected / 2.);
    if (const auto* failure = std::get_if<Failure>(&clipped))
        FAIL(failure_name(failure->code) << ": " << failure->detail);
    const auto&     complex = *std::get<std::shared_ptr<const MaterialBandComplex>>(clipped);
    MachineEnvelope machine;
    // Explicit virtual pointed tool for the narrow coverage cells. This is not
    // a qualified physical nozzle and is different from the box test envelope.
    machine.sections   = {{0., .2, .01}, {.2, 2., 1.}, {2., 10., 3.}};
    const auto planned = plan_clipped_cycle(complex, p.process, p.tolerances, machine);
    if (const auto* failure = std::get_if<Failure>(&planned))
        FAIL(failure_name(failure->code) << ": " << failure->detail);
    const auto& plan = *std::get<std::shared_ptr<const PathPlan>>(planned);
    REQUIRE_FALSE(plan.paths.empty());
    std::array<size_t, 2> perimeters{};
    std::array<double, 2> reconstructed{};
    using Cell = std::array<Vec2d, 3>;
    std::array<std::map<std::pair<int, int>, std::vector<Cell>>, 2> raster;
    const auto interface_z = [&](const Vec2d& xy) { return p.bottom_mm + height / 2. + gradient.dot(xy) + complex.interface_correction_mm; };
    const auto inside = [&](const Vec3d& point) {
        if (square) {
            if (std::abs(point.x()) > 6. + 1e-5 || std::abs(point.y()) > 6. + 1e-5)
                return false;
        } else
            for (unsigned i = 0; i < n; ++i) {
                const double angle = (i + .5) * 2. * PI / n;
                if (point.x() * std::cos(angle) + point.y() * std::sin(angle) > (r0 + slope * point.z()) * std::cos(PI / n) + 1e-5)
                    return false;
            }
        if (inner > 0.) {
            bool in_hole = true;
            for (unsigned i = 0; i < n; ++i) {
                const double angle = (i + .5) * 2. * PI / n;
                if (point.x() * std::cos(angle) + point.y() * std::sin(angle) >= inner * std::cos(PI / n) - 1e-5)
                    in_hole = false;
            }
            if (in_hole)
                return false;
        }
        return true;
    };
    for (const auto& path : plan.paths) {
        CHECK(path.band.macro_cycle == 9);
        CHECK(path.shared_interface == complex.shared_interface.id);
        const size_t slot = path.band.stack_slot;
        if (path.role == PathRole::Perimeter) {
            ++perimeters[slot];
            CHECK((path.vertices.front() - path.vertices.back()).norm() < 1e-7);
        }
        REQUIRE(path.vertices.size() == path.segments.size() + 1);
        for (size_t i = 0; i < path.segments.size(); ++i) {
            const auto& segment = path.segments[i];
            CHECK(inside(path.vertices[i]));
            CHECK(inside((path.vertices[i] + path.vertices[i + 1]) / 2.)); // no connector through a cavity
            double volume = 0.;
            for (const auto& t : segment.coverage_triangles) {
                Vec2d min = t[0], max = t[0];
                for (const auto& point : t) {
                    min = min.cwiseMin(point);
                    max = max.cwiseMax(point);
                }
                for (int y = int(std::floor(min.y() * 2.)); y <= int(std::floor(max.y() * 2.)); ++y)
                    for (int x = int(std::floor(min.x() * 2.)); x <= int(std::floor(max.x() * 2.)); ++x)
                        raster[slot][{x, y}].push_back(t);
                const Vec2d  a = t[1] - t[0], b = t[2] - t[0];
                const double triangle_area = (a.x() * b.y() - a.y() * b.x()) / 2.;
                double       thickness     = 0.;
                for (const auto& point : t) {
                    const double bottom = slot == 0 ? p.bottom_mm : interface_z(point);
                    const double top    = slot == 0 ? interface_z(point) : p.top_mm;
                    CHECK(inside({point.x(), point.y(), bottom}));
                    CHECK(inside({point.x(), point.y(), top}));
                    thickness += (top - bottom) / 3.;
                }
                volume += triangle_area * thickness;
            }
            CHECK(volume == Approx(segment.volume_mm3).margin(1e-7));
            reconstructed[slot] += volume;
        }
    }
    for (size_t slot = 0; slot < 2; ++slot) {
        CHECK(perimeters[slot] == (inner > 0. ? 2 : 1));
        CHECK(plan.coverage[slot].missing_mm3 <= expected / 2. * .01);
        CHECK(plan.coverage[slot].excess_mm3 <= p.tolerances.absolute_volume_mm3);
        CHECK(reconstructed[slot] == Approx(expected / 2.).epsilon(.01));
        double           missing = 0., excess = 0.;
        constexpr size_t resolution = 151;
        const double     pitch      = 2. * r0 / resolution;
        for (size_t y = 0; y < resolution; ++y)
            for (size_t x = 0; x < resolution; ++x) {
                const Vec2d point(-r0 + (x + .371) * pitch, -r0 + (y + .619) * pitch);
                double      radial  = -1e30;
                bool        in_hole = inner > 0.;
                for (unsigned edge = 0; edge < n; ++edge) {
                    const double angle    = (edge + .5) * 2. * PI / n;
                    const double distance = (point.x() * std::cos(angle) + point.y() * std::sin(angle)) / std::cos(PI / n);
                    radial                = std::max(radial, distance);
                    if (distance >= inner)
                        in_hole = false;
                }
                double solid_top = p.top_mm;
                if (frustum)
                    solid_top = std::min(solid_top, (radial - r0) / slope);
                else if ((!square && radial > r0) || (square && point.cwiseAbs().maxCoeff() > 6.))
                    solid_top = p.bottom_mm;
                if (in_hole)
                    solid_top = p.bottom_mm;
                const double g               = interface_z(point);
                const double expected_height = slot == 0 ? std::max(0., std::min(g, solid_top) - p.bottom_mm) :
                                                           std::max(0., solid_top - std::max(g, p.bottom_mm));
                size_t       count           = 0;
                const auto   found           = raster[slot].find({int(std::floor(point.x() * 2.)), int(std::floor(point.y() * 2.))});
                if (found != raster[slot].end())
                    for (const auto& tri : found->second) {
                        bool contained = true;
                        for (size_t edge = 0; edge < 3; ++edge) {
                            const Vec2d a = tri[(edge + 1) % 3] - tri[edge], b = point - tri[edge];
                            if (a.x() * b.y() - a.y() * b.x() < -1e-12)
                                contained = false;
                        }
                        count += contained;
                    }
                const double deposited_height = count * (slot == 0 ? g - p.bottom_mm : p.top_mm - g);
                missing += std::max(0., expected_height - deposited_height) * pitch * pitch;
                excess += std::max(0., deposited_height - expected_height) * pitch * pitch;
            }
        CAPTURE(missing, excess, expected);
        CHECK(missing <= expected / 2. * .01);
        CHECK(excess <= p.tolerances.absolute_volume_mm3);
    }
}

TEST_CASE("Material band solid coverage reports excess and missing volume independently", "[material_band][unit][clipping]")
{
    // Analytic equal-volume cubes displaced by 3 mm: equal total dose hides
    // 36 mm^3 missing on one side and the same excess on the other.
    const Mesh target    = axial_solid(8, 6., 6., 0., 1., true);
    Mesh       candidate = target;
    for (auto& p : candidate.vertices)
        p.x() += 3.;
    const auto measured = measure_coverage(candidate, target, {});
    if (const auto* failure = std::get_if<Failure>(&measured))
        FAIL(failure_name(failure->code) << ": " << failure->detail);
    const auto& coverage = std::get<SolidCoverage>(measured);
    CHECK(coverage.candidate_mm3 == Approx(144.).margin(1e-8));
    CHECK(coverage.missing_mm3 == Approx(36.).margin(1e-8));
    CHECK(coverage.excess_mm3 == Approx(36.).margin(1e-8));
}

TEST_CASE("Material band pure domains contain no absent-material residue", "[material_band][unit][clipped_paths]")
{
    const unsigned physical = GENERATE(1, 2);
    ClippedBand    band;
    band.id         = {71, 13, {physical}, physical - 1};
    band.solid      = axial_solid(64, 6., 6., 5., .24);
    band.volume_mm3 = 64. * std::sin(2. * PI / 64.) / 2. * (36. - 25.) * .24;
    MachineEnvelope machine;
    machine.sections  = {{0., .2, .01}, {.2, 2., 1.}, {2., 10., 3.}};
    const auto result = plan_pure_cycle(band, {1, 2, 3, 4, 5, 6, 7}, {}, {}, machine);
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure_name(failure->code) << ": " << failure->detail);
    const auto& plan = *std::get<std::shared_ptr<const PathPlan>>(result);
    CHECK(plan.interfaces.empty());
    CHECK(plan.volume_mm3[2 - physical] == 0.);
    CHECK(plan.volume_mm3[physical - 1] == Approx(band.volume_mm3).epsilon(.01));
    for (const auto& path : plan.paths) {
        CHECK(path.band.material.value == physical);
        CHECK(path.band.macro_cycle == 13);
        CHECK(path.shared_interface == 0);
        for (const auto& segment : path.segments) {
            CHECK(segment.height_start_mm == Approx(.24));
            CHECK(segment.height_end_mm == Approx(.24));
        }
    }
}

TEST_CASE("Material layer painted cap repair keeps deposition cells disjoint", "[material_band][layer_process][clipped_paths]")
{
    const double            amplitude = GENERATE(.3, .012);
    const auto              point     = [](double x, double y) { return Point(scale_(x), scale_(y)); };
    const ExPolygons        outline{ExPolygon(Polygon(Points{point(0., 0.), point(8., 0.), point(8., 4.), point(0., 4.)}))};
    std::vector<ContourRun> runs;
    for (size_t row = 0; row < 10; ++row) {
        const double y = .225 + row * .4;
        runs.push_back({Polyline(Points{point(.225, y), point(7.775, y + .08 * std::sin(row))}), .45, PathRole::DenseFill});
    }
    MachineEnvelope machine;
    machine.sections    = {{0., 10., .01}};
    const auto geometry = plan_contour_cycle(outline, runs, {1, 0, {1}, 0}, 0., .24, {}, {}, {}, machine);
    if (const auto* failure = std::get_if<Failure>(&geometry))
        FAIL(failure->detail);
    const auto& source = *std::get<std::shared_ptr<const PathPlan>>(geometry);
    Polygons    deposited;
    for (const auto& path : source.paths)
        for (const auto& segment : path.segments)
            for (const auto& cell : segment.cells) {
                Points points;
                for (const auto& p : cell.upper)
                    points.push_back(point(p.x(), p.y()));
                deposited.emplace_back(std::move(points));
            }
    const auto occupied = union_ex(deposited);
    Points     boundary{point(0., 0.)};
    for (size_t i = 0; i <= 64; ++i) {
        const double y = i / 16.;
        boundary.push_back(point(4. + amplitude * std::sin(5. * y), y));
    }
    boundary.push_back(point(0., 4.));
    const auto                            left  = intersection_ex(occupied, ExPolygons{ExPolygon(Polygon(std::move(boundary)))});
    const auto                            right = diff_ex(occupied, left);
    const std::vector<PlanarMaterialBand> bands{{{2, 0, {1}, 0}, left, 0., .12, 0, 1},
                                                {{2, 0, {2}, 1}, left, .12, .24, 1, 0},
                                                {{3, 0, {1}, 0}, right, 0., .144, 0, 2},
                                                {{3, 0, {2}, 1}, right, .144, .24, 2, 0}};
    const auto                            result = plan_partitioned_cycle(source, bands, {}, {});
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure->detail);
    const auto& plan = *std::get<std::shared_ptr<const PathPlan>>(result);
    for (const auto& band : bands) {
        Polygons triangles;
        double   summed_area = 0.;
        for (const auto& path : plan.paths)
            if (path.band.domain == band.id.domain && path.band.material.value == band.id.material.value)
                for (const auto& segment : path.segments)
                    for (const auto& cell : segment.cells) {
                        Points points;
                        for (const auto& p : cell.upper)
                            points.push_back(point(p.x(), p.y()));
                        Polygon triangle(std::move(points));
                        summed_area += triangle.area() * SCALING_FACTOR * SCALING_FACTOR;
                        triangles.push_back(std::move(triangle));
                    }
        const auto merged      = union_ex(triangles);
        double     merged_area = 0., excess = 0.;
        for (const auto& polygon : merged)
            merged_area += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
        for (const auto& polygon : diff_ex(merged, band.footprint))
            excess += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
        CHECK(summed_area - merged_area < 1e-4);
        CHECK(excess < 1e-4);
    }
}

TEST_CASE("Material solid fill overlap cannot occupy neighboring sparse air", "[material_band][layer_process][clipped_paths]")
{
    const bool                    crossing = GENERATE(false, true);
    const auto                    point    = [](double x, double y) { return Point(scale_(x), scale_(y)); };
    const ExPolygons              outline{ExPolygon(Polygon(Points{point(0., 0.), point(4., 0.), point(4., 4.), point(0., 4.)}))};
    const ExPolygons              skin{ExPolygon(Polygon(Points{point(1., 0.), point(1.2, 0.), point(1.2, 4.), point(1., 4.)}))};
    auto                          allowed = std::make_shared<const ExPolygons>(skin);
    const Points                  motion  = crossing ? Points{point(.5, 2.), point(1.7, 2.)} : Points{point(1.1, 0.), point(1.1, 4.)};
    const std::vector<ContourRun> runs{{Polyline(motion), .45, PathRole::DenseFill, allowed}};
    MachineEnvelope               machine;
    machine.sections  = {{0., 10., .01}};
    const auto result = plan_contour_cycle(outline, runs, {1, 0, {1}, 0}, 0., .24, {}, {}, {}, machine, {}, {}, {}, skin);
    if (const auto* failure = std::get_if<Failure>(&result))
        FAIL(failure->detail);
    const auto& plan = *std::get<std::shared_ptr<const PathPlan>>(result);
    CHECK(plan.volume_mm3[0] == Approx(.2 * 4. * .24).epsilon(.01));
    Polygons deposited;
    for (const auto& path : plan.paths)
        for (const auto& segment : path.segments)
            for (const auto& cell : segment.cells) {
                Points points;
                for (const auto& p : cell.upper)
                    points.push_back(point(p.x(), p.y()));
                deposited.emplace_back(std::move(points));
            }
    CHECK(diff_ex(deposited, skin).empty());
    double missing = 0.;
    for (const auto& polygon : diff_ex(skin, deposited))
        missing += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
    CHECK(missing < .008);
    const auto tolerance = offset_ex(skin, float(scale_(.001)));
    for (const auto& path : plan.paths)
        for (const auto& p : path.vertices)
            CHECK(std::any_of(tolerance.begin(), tolerance.end(),
                              [&](const ExPolygon& region) { return region.contains(point(p.x(), p.y())); }));
}
