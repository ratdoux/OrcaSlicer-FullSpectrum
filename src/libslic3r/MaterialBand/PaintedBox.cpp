#include "PaintedBox.hpp"
#include "ClippedPlanner.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace Slic3r::MaterialBand {
namespace {
struct Abort
{
    FailureCode code;
    const char* detail;
};
using Polygon = std::vector<Vec2d>;
Polygon clip(const Polygon& polygon, double x, bool above)
{
    Polygon result;
    if (polygon.empty())
        return result;
    Vec2d  a  = polygon.back();
    double da = (a.x() - x) * (above ? 1. : -1.);
    for (const auto& b : polygon) {
        const double db = (b.x() - x) * (above ? 1. : -1.);
        if ((da >= 0.) != (db >= 0.))
            result.push_back(a + (b - a) * da / (da - db));
        if (db >= 0.)
            result.push_back(b);
        a  = b;
        da = db;
    }
    return result;
}
double area(const Polygon& polygon)
{
    double result = 0.;
    for (size_t i = 2; i < polygon.size(); ++i) {
        const Vec2d a = polygon[i - 1] - polygon[0], b = polygon[i] - polygon[0];
        result += a.x() * b.y() - a.y() * b.x();
    }
    return std::abs(result) * .5;
}
Mesh box(const Vec3d& low, const Vec3d& high)
{
    Mesh mesh;
    for (double x : {low.x(), high.x()})
        for (const Vec2d yz : {Vec2d(low.y(), low.z()), Vec2d(high.y(), low.z()), Vec2d(high.y(), high.z()), Vec2d(low.y(), high.z())})
            mesh.vertices.emplace_back(x, yz.x(), yz.y());
    mesh.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}};
    for (uint32_t edge = 0; edge < 4; ++edge) {
        const uint32_t next = (edge + 1) % 4;
        mesh.triangles.push_back({edge, next, 4 + next});
        mesh.triangles.push_back({edge, 4 + next, 4 + edge});
    }
    return mesh;
}
} // namespace

BoxPlanResult plan_painted_box(const KernelInput&           input,
                               const StackPhase&            phase,
                               const MachineEnvelope&       machine,
                               DomainId                     domain,
                               size_t                       perimeter_loops,
                               const PathLimits&            limits,
                               const std::function<bool()>& cancelled,
                               const std::function<bool()>& stale,
                               const DenseInfillProfile&    infill)
{
    const auto fail = [&](FailureCode code, const char* detail) -> BoxPlanResult {
        return Failure{input.revision, code, detail, {}, {}, {}};
    };
    const auto poll = [&] {
        if (cancelled && cancelled())
            throw Abort{FailureCode::Cancelled, "cancelled_during_painted_domain"};
        if (stale && stale())
            throw Abort{FailureCode::StaleInput, "revision_changed"};
    };
    try {
        poll();
        if (input.solid.vertices.empty() || input.regions.empty() || input.regions.front().id != 0 || machine.sections.empty())
            return fail(FailureCode::InvalidInput, "painted_box_input");
        if (domain >= (uint64_t(1) << 63) || perimeter_loops == 0 || perimeter_loops > 1000)
            return fail(FailureCode::ResourceLimit, "painted_domain_identity_or_wall_limit");
        Vec3d low = input.solid.vertices.front(), high = low;
        for (const auto& v : input.solid.vertices) {
            low  = low.cwiseMin(v);
            high = high.cwiseMax(v);
        }
        const double eps = input.tolerances.interface_mm, base_fraction = input.regions.front().fraction_a;
        const double wall = perimeter_loops * input.process.bead_width_mm;
        const Vec3d  size = high - low;
        if (!low.allFinite() || !high.allFinite() || size.minCoeff() <= 0. || std::abs(low.z()) > eps ||
            size.x() <= 2. * wall + input.process.bead_width_mm || size.y() <= 2. * wall + input.process.bead_width_mm)
            return fail(FailureCode::UnsupportedTopology, "painted_box_domain_bounds");
        struct Face
        {
            Polygon projected;
            double  fraction;
            int     side;
        };
        std::vector<Face>                   faces;
        std::vector<std::pair<int, double>> ends;
        std::set<double>                    stations{low.x(), high.x()};
        for (const auto& region : input.regions) {
            if (region.id != 0 && region.scope != DepositionScope::PaintedShell)
                return fail(FailureCode::UnsupportedProfile, "painted_box_requires_wall_scope");
            for (const auto& tri : region.painted_surface.triangles) {
                poll();
                const auto& vertices = region.painted_surface.vertices;
                for (auto i : tri)
                    if (i >= vertices.size())
                        return fail(FailureCode::InvalidInput, "paint_vertex_index");
                bool found = false;
                for (int axis = 0; axis < 3; ++axis)
                    for (int side = 0; side < 2; ++side) {
                        const double coordinate = side ? high[axis] : low[axis];
                        if (std::any_of(tri.begin(), tri.end(), [&](uint32_t i) { return std::abs(vertices[i][axis] - coordinate) > eps; }))
                            continue;
                        found = true;
                        if (axis == 2) {
                            if (std::abs(region.fraction_a - base_fraction) > 1e-12)
                                return fail(FailureCode::UnsupportedTopology, "top_or_bottom_paint_requires_domain_adapter");
                        } else if (axis == 0)
                            ends.emplace_back(side, region.fraction_a);
                        else {
                            Face face;
                            face.fraction = region.fraction_a;
                            face.side     = side;
                            for (uint32_t i : tri) {
                                // Axis-aligned rotations retain roundoff from
                                // sin/cos. Merge stations at the same declared
                                // XY quantum before constructing paint strips.
                                const double raw_x = vertices[i].x();
                                const double x     = std::abs(raw_x - low.x()) <= eps ?
                                                         low.x() :
                                                     std::abs(raw_x - high.x()) <= eps ?
                                                         high.x() :
                                                         low.x() + std::round((raw_x - low.x()) / SCALING_FACTOR) * SCALING_FACTOR;
                                stations.insert(x);
                                face.projected.emplace_back(x, vertices[i].z());
                            }
                            faces.push_back(std::move(face));
                        }
                    }
                if (!found)
                    return fail(FailureCode::UnsupportedTopology, "painted_domain_requires_box_faces");
            }
        }
        if (stations.size() > 4096 || faces.size() > limits.max_segments || (!faces.empty() && stations.size() > 2000000 / faces.size()))
            return fail(FailureCode::ResourceLimit, "paint_domain_candidate_limit");
        const std::vector<double> xs(stations.begin(), stations.end());
        std::vector<double>       fractions;
        for (size_t i = 1; i < xs.size(); ++i) {
            poll();
            double                fraction = -1.;
            std::array<double, 2> covered{};
            for (const auto& face : faces) {
                const double amount = area(clip(clip(face.projected, xs[i - 1], true), xs[i], false));
                if (amount <= 1e-12)
                    continue;
                covered[face.side] += amount;
                if (fraction >= 0. && std::abs(face.fraction - fraction) > 1e-12)
                    return fail(FailureCode::UnsupportedTopology, "paint_not_matching_full_height_stripes");
                fraction = face.fraction;
            }
            for (double covered_area : covered)
                if (std::abs(covered_area - (xs[i] - xs[i - 1]) * size.z()) > eps * (size.z() + size.x()))
                    return fail(FailureCode::UnsupportedTopology, "incomplete_painted_wall_coverage");
            if (fraction <= 0. || fraction >= 1.)
                return fail(FailureCode::UnsupportedTopology, "mixed_wall_termination_requires_adapter");
            fractions.push_back(fraction);
        }
        if (fractions.empty())
            return fail(FailureCode::InvalidInput, "empty_paint_stripes");
        for (const auto& end : ends)
            if (std::abs(end.second - (end.first ? fractions.back() : fractions.front())) > 1e-12)
                return fail(FailureCode::UnsupportedTopology, "end_wall_paint_mismatch");
        std::map<double, double> field{{xs.front(), fractions.front()}, {xs.back(), fractions.back()}};
        for (size_t i = 1; i < fractions.size(); ++i) {
            if (fractions[i - 1] == fractions[i])
                continue;
            if (input.boundary.policy == BoundaryPolicy::PreservePaintedEdge)
                return fail(FailureCode::UnsupportedTopology, "discontinuous_interface");
            const double width = std::min({input.boundary.transition_width_mm, xs[i] - xs[i - 1], xs[i + 1] - xs[i]});
            if (!std::isfinite(width) || width <= 0. ||
                .5 * std::abs(fractions[i] - fractions[i - 1]) > input.boundary.transition_fraction_error + 1e-12)
                return fail(FailureCode::MaterialToleranceExceeded, "paint_transition_budget");
            // Use the same local XY quantum as the support/coverage predicates.
            // Float paint leaves may otherwise create unrepresentable slivers
            // between identical neighboring plateau samples.
            const auto quantize = [&](double x) { return low.x() + std::round((x - low.x()) / SCALING_FACTOR) * SCALING_FACTOR; };
            field[quantize(xs[i] - width / 2.)] = fractions[i - 1];
            field[quantize(xs[i] + width / 2.)] = fractions[i];
        }
        SlabProblem problem;
        problem.revision     = input.revision;
        problem.domain       = domain;
        problem.interface_id = 1;
        problem.bottom_mm    = low.z();
        problem.top_mm       = low.z() + input.process.macro_height_mm;
        problem.process      = input.process;
        problem.tolerances   = input.tolerances;
        problem.boundary     = input.boundary;
        for (const auto& knot : field) {
            problem.vertices.emplace_back(knot.first, low.y());
            problem.vertices.emplace_back(knot.first, high.y());
            problem.target_fraction_a.insert(problem.target_fraction_a.end(), 2, knot.second);
            problem.local_fraction_error.insert(problem.local_fraction_error.end(), 2, 1e-9);
            double distance = size.x();
            for (size_t boundary = 1; boundary < fractions.size(); ++boundary)
                if (fractions[boundary - 1] != fractions[boundary])
                    distance = std::min(distance, std::abs(knot.first - xs[boundary]));
            problem.boundary_distance_mm.insert(problem.boundary_distance_mm.end(), 2, distance);
        }
        for (uint32_t i = 2; i < problem.vertices.size(); i += 2) {
            problem.triangles.push_back({i - 2, i, i + 1});
            problem.triangles.push_back({i - 2, i + 1, i - 1});
            problem.target_volume_a_mm3 += (problem.vertices[i].x() - problem.vertices[i - 2].x()) * size.y() *
                                           input.process.macro_height_mm *
                                           (problem.target_fraction_a[i] + problem.target_fraction_a[i - 2]) / 2.;
        }
        auto shell = plan_box(input.solid, problem, phase, machine, limits, cancelled, {perimeter_loops, true});
        if (const auto* error = std::get_if<Failure>(&shell))
            return *error;
        auto result = std::make_shared<PathPlan>(*std::get<std::shared_ptr<const PathPlan>>(shell));
        // Independent integration of the original painted step over the actual
        // rectangular frame. A symmetric blur cannot hide net shell dose error.
        double expected_a = 0.;
        for (size_t i = 1; i < xs.size(); ++i) {
            const double inner_x = std::max(0., std::min(xs[i], high.x() - wall) - std::max(xs[i - 1], low.x() + wall));
            expected_a += ((xs[i] - xs[i - 1]) * size.y() - inner_x * (size.y() - 2. * wall)) * size.z() * fractions[i - 1];
        }
        if (std::abs(result->volume_mm3[0] - expected_a) >
            std::max(input.tolerances.absolute_volume_mm3, input.tolerances.relative_volume * expected_a))
            return fail(FailureCode::MaterialToleranceExceeded, "painted_shell_material_budget");
        const Vec3d inner_low(low.x() + wall, low.y() + wall, low.z()), inner_high(high.x() - wall, high.y() - wall, high.z());
        size_t      generated_segments = 0, generated_bytes = 0;
        const auto  admit = [&](const PathPlan& plan) {
            for (const auto& path : plan.paths) {
                if (path.segments.size() > limits.max_segments - generated_segments)
                    throw Abort{FailureCode::ResourceLimit, "painted_domain_segment_limit"};
                generated_segments += path.segments.size();
                size_t bytes = sizeof(BandPath) + path.vertices.capacity() * sizeof(Vec3d) + path.segments.capacity() * sizeof(PathSegment);
                for (const auto& segment : path.segments)
                    bytes += segment.cells.capacity() * sizeof(BeadCell) + segment.footprint.capacity() * sizeof(Vec2d) +
                             segment.coverage_triangles.capacity() * sizeof(std::array<Vec2d, 3>);
                if (bytes > limits.max_temporary_bytes - generated_bytes)
                    throw Abort{FailureCode::ResourceLimit, "painted_domain_memory_limit"};
                generated_bytes += bytes;
            }
        };
        admit(*result);
        const auto append = [&](const PathPlan& interior) {
            if (interior.paths.size() > limits.max_paths - result->paths.size())
                throw Abort{FailureCode::ResourceLimit, "painted_domain_path_limit"};
            admit(interior);
            for (auto path : interior.paths) {
                path.id = result->paths.size() + 1;
                result->paths.push_back(std::move(path));
            }
            for (size_t slot = 0; slot < 2; ++slot) {
                result->volume_mm3[slot] += interior.volume_mm3[slot];
                result->coverage[slot].target_mm3 += interior.coverage[slot].target_mm3;
                result->coverage[slot].deposited_mm3 += interior.coverage[slot].deposited_mm3;
                result->coverage[slot].missing_mm3 += interior.coverage[slot].missing_mm3;
                result->coverage[slot].excess_mm3 += interior.coverage[slot].excess_mm3;
            }
        };
        if (base_fraction != 0. && base_fraction != 1.)
            return fail(FailureCode::UnsupportedProfile, "mixed_interior_requires_domain_adapter");
        const size_t   cycles = result->interfaces.size();
        CoverageLimits coverage;
        coverage.minimum_bead_width_mm = machine.coplanar_tip_contact ?
                                             .04 :
                                             std::max(.04, 2.1 * (machine.sections.front().radius_mm + machine.clearance_mm));
        for (size_t cycle = 0; cycle < cycles; ++cycle) {
            poll();
            coverage.infill = infill.cycle(cycle, cycles);
            // The painted wall already owns every configured perimeter. The
            // base-material core is infill, including its outermost track.
            coverage.perimeter_loops = 0;
            const double   bottom    = low.z() + cycle * input.process.macro_height_mm;
            const double   top       = bottom + input.process.macro_height_mm;
            const uint32_t slot      = base_fraction == 1. ? 0 : 1;
            ClippedBand    band;
            band.id         = {domain | (uint64_t(1) << 63), phase.first_active_cycle + cycle, phase.order[slot], slot};
            band.solid      = box(Vec3d(inner_low.x(), inner_low.y(), bottom), Vec3d(inner_high.x(), inner_high.y(), top));
            band.volume_mm3 = (inner_high.x() - inner_low.x()) * (inner_high.y() - inner_low.y()) * (top - bottom);
            auto core = plan_pure_cycle(band, input.revision, input.process, input.tolerances, machine, limits, coverage, cancelled, stale);
            if (const auto* error = std::get_if<Failure>(&core))
                return *error;
            append(*std::get<std::shared_ptr<const PathPlan>>(core));
        }
        std::stable_sort(result->paths.begin(), result->paths.end(), [](const BandPath& a, const BandPath& b) {
            return std::tie(a.band.macro_cycle, a.band.stack_slot) < std::tie(b.band.macro_cycle, b.band.stack_slot);
        });
        poll();
        return std::shared_ptr<const PathPlan>(std::move(result));
    } catch (const Abort& error) {
        return fail(error.code, error.detail);
    } catch (const std::bad_alloc&) {
        return fail(FailureCode::ResourceLimit, "painted_domain_allocation");
    }
}
} // namespace Slic3r::MaterialBand
