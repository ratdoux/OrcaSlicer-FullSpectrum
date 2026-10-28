#include "BoxPlanner.hpp"

#include "libslic3r/GCodeWriter.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <numeric>
#include <sstream>
#include <set>

namespace Slic3r::MaterialBand {
namespace {
using Footprint = std::vector<Vec2d>;

// Axis-aligned half-plane clipping of convex footprint cells, in mm.
Footprint clip(const Footprint& polygon, int axis, double bound, bool above)
{
    Footprint out;
    if (polygon.empty())
        return out;
    Vec2d  previous          = polygon.back();
    double previous_distance = (previous[axis] - bound) * (above ? 1. : -1.);
    for (const auto& current : polygon) {
        const double distance = (current[axis] - bound) * (above ? 1. : -1.);
        if ((distance >= 0.) != (previous_distance >= 0.))
            out.push_back(previous + (current - previous) * (previous_distance / (previous_distance - distance)));
        if (distance >= 0.)
            out.push_back(current);
        previous          = current;
        previous_distance = distance;
    }
    return out;
}

// Area and first X moment, with a local origin to retain translated precision.
std::pair<double, double> area_and_moment(const Footprint& polygon)
{
    if (polygon.size() < 3)
        return {0., 0.};
    const Vec2d origin     = polygon.front();
    double      twice_area = 0., moment = 0.;
    for (size_t i = 1; i + 1 < polygon.size(); ++i) {
        const Vec2d  a = polygon[i] - origin, b = polygon[i + 1] - origin;
        const double cross = a.x() * b.y() - a.y() * b.x();
        twice_area += cross;
        moment += cross * (a.x() + b.x()) / 6.;
    }
    const double area = twice_area / 2.;
    return {area, moment + origin.x() * area};
}

double field(const std::vector<std::pair<double, double>>& profile, double x)
{
    auto end = std::upper_bound(profile.begin(), profile.end(), x, [](double value, const auto& sample) { return value < sample.first; });
    if (end == profile.begin())
        return end->second;
    if (end == profile.end())
        return profile.back().second;
    const auto& left = *(end - 1);
    return left.second + (end->second - left.second) * (x - left.first) / (end->first - left.first);
}

double integrate(const Footprint& polygon, const std::vector<std::pair<double, double>>& profile)
{
    if (polygon.empty())
        return 0.;
    double minimum_x = polygon.front().x(), maximum_x = minimum_x;
    for (const auto& point : polygon) {
        minimum_x = std::min(minimum_x, point.x());
        maximum_x = std::max(maximum_x, point.x());
    }
    auto first = std::upper_bound(profile.begin(), profile.end(), minimum_x, [](double x, const auto& sample) { return x < sample.first; });
    const size_t first_interval = std::max(size_t(1), size_t(first - profile.begin()));
    double       volume         = 0.;
    for (size_t i = first_interval; i < profile.size() && profile[i - 1].first < maximum_x; ++i) {
        const auto clipped        = clip(clip(polygon, 0, profile[i - 1].first, true), 0, profile[i].first, false);
        const auto [area, moment] = area_and_moment(clipped);
        if (area <= 0.)
            continue;
        const double slope = (profile[i].second - profile[i - 1].second) / (profile[i].first - profile[i - 1].first);
        volume += area * profile[i - 1].second + slope * (moment - area * profile[i - 1].first);
    }
    return volume;
}

// Every surface triangle must be outward on an actual box face. Sum of face
// areas plus oriented manifold edges rules out caps/holes/internal walls.
bool verify_box(const Mesh& solid, double tolerance, Vec3d& minimum, Vec3d& maximum)
{
    if (solid.vertices.empty() || solid.triangles.empty())
        return false;
    minimum = maximum = solid.vertices.front();
    for (const auto& vertex : solid.vertices) {
        if (!vertex.allFinite())
            return false;
        minimum = minimum.cwiseMin(vertex);
        maximum = maximum.cwiseMax(vertex);
    }
    if (!(maximum - minimum).allFinite() || (maximum - minimum).minCoeff() <= tolerance)
        return false;
    std::map<std::pair<uint32_t, uint32_t>, std::pair<size_t, int>> edges;
    std::array<double, 6>                                           areas{};
    for (const auto& triangle : solid.triangles) {
        for (uint32_t index : triangle)
            if (index >= solid.vertices.size())
                return false;
        const Vec3d a = solid.vertices[triangle[0]], b = solid.vertices[triangle[1]], c = solid.vertices[triangle[2]];
        const Vec3d normal = (b - a).cross(c - a);
        if (!normal.allFinite() || !std::isfinite(normal.norm()) || normal.norm() == 0.)
            return false;
        bool found_face = false;
        for (int axis = 0; axis < 3; ++axis)
            for (int upper = 0; upper < 2; ++upper) {
                const double bound = upper ? maximum[axis] : minimum[axis];
                if (std::abs(a[axis] - bound) <= tolerance && std::abs(b[axis] - bound) <= tolerance &&
                    std::abs(c[axis] - bound) <= tolerance && normal[axis] * (upper ? 1. : -1.) > 0.) {
                    if (std::hypot(normal[(axis + 1) % 3], normal[(axis + 2) % 3]) > tolerance * normal.norm())
                        return false;
                    areas[2 * axis + upper] += normal.norm() / 2.;
                    found_face = true;
                }
            }
        if (!found_face)
            return false;
        for (size_t i = 0; i < 3; ++i) {
            const auto a_index = triangle[i], b_index = triangle[(i + 1) % 3];
            auto&      edge = edges[{std::min(a_index, b_index), std::max(a_index, b_index)}];
            ++edge.first;
            edge.second += a_index < b_index ? 1 : -1;
        }
    }
    for (const auto& edge : edges)
        if (edge.second.first != 2 || edge.second.second != 0)
            return false;
    const Vec3d size = maximum - minimum;
    for (int axis = 0; axis < 3; ++axis)
        for (int upper = 0; upper < 2; ++upper)
            if (std::abs(areas[2 * axis + upper] - size[(axis + 1) % 3] * size[(axis + 2) % 3]) >
                tolerance * (size[(axis + 1) % 3] + size[(axis + 2) % 3]))
                return false;
    return true;
}
bool verify_domain(const SlabProblem& cycle, const Vec3d& minimum, const Vec3d& maximum)
{
    std::map<std::pair<double, double>, size_t> coordinates;
    for (size_t i = 0; i < cycle.vertices.size(); ++i) {
        const auto& vertex = cycle.vertices[i];
        if (!vertex.allFinite() || vertex.x() < minimum.x() || vertex.x() > maximum.x() || vertex.y() < minimum.y() ||
            vertex.y() > maximum.y() || !coordinates.emplace(std::make_pair(vertex.x(), vertex.y()), i).second)
            return false;
    }
    struct Edge
    {
        size_t   count   = 0;
        int      balance = 0;
        uint32_t from = 0, to = 0;
    };
    std::map<std::pair<uint32_t, uint32_t>, Edge> edges;
    for (const auto& face : cycle.triangles) {
        for (uint32_t vertex : face)
            if (vertex >= cycle.vertices.size())
                return false;
        const Vec2d a = cycle.vertices[face[1]] - cycle.vertices[face[0]], b = cycle.vertices[face[2]] - cycle.vertices[face[0]];
        if (a.x() * b.y() - a.y() * b.x() <= 0.)
            return false;
        for (size_t i = 0; i < 3; ++i) {
            const auto from = face[i], to = face[(i + 1) % 3];
            auto&      edge = edges[{std::min(from, to), std::max(from, to)}];
            ++edge.count;
            edge.balance += from < to ? 1 : -1;
            edge.from = from;
            edge.to   = to;
        }
    }
    std::array<double, 4> boundary_lengths{};
    for (const auto& entry : edges) {
        const auto& edge = entry.second;
        if (edge.count == 2 && edge.balance == 0)
            continue;
        if (edge.count != 1)
            return false;
        const Vec2d a = cycle.vertices[edge.from], b = cycle.vertices[edge.to];
        if (a.y() == minimum.y() && b.y() == minimum.y() && b.x() > a.x())
            boundary_lengths[0] += b.x() - a.x();
        else if (a.x() == maximum.x() && b.x() == maximum.x() && b.y() > a.y())
            boundary_lengths[1] += b.y() - a.y();
        else if (a.y() == maximum.y() && b.y() == maximum.y() && b.x() < a.x())
            boundary_lengths[2] += a.x() - b.x();
        else if (a.x() == minimum.x() && b.x() == minimum.x() && b.y() < a.y())
            boundary_lengths[3] += a.y() - b.y();
        else
            return false;
    }
    for (size_t side = 0; side < 4; ++side)
        if (std::abs(boundary_lengths[side] - (maximum - minimum)[side % 2]) > cycle.tolerances.interface_mm)
            return false;
    return true;
}
} // namespace

BoxPlanResult plan_box(const Mesh&                  solid,
                       const SlabProblem&           cycle,
                       const StackPhase&            phase,
                       const MachineEnvelope&       machine,
                       const PathLimits&            limits,
                       const std::function<bool()>& cancelled,
                       const BoxPathOptions&        options)
{
    const auto fail = [&](FailureCode code, const char* detail) -> BoxPlanResult {
        return Failure{cycle.revision, code, detail, {}, {}, {}};
    };
    if (cancelled && cancelled())
        return fail(FailureCode::Cancelled, "cancelled_before_paths");
    for (double value : {cycle.tolerances.interface_mm, cycle.tolerances.macro_closure_mm, cycle.tolerances.path_approximation_mm})
        if (!std::isfinite(value) || value <= 0.)
            return fail(FailureCode::InvalidInput, "invalid_path_tolerance");
    if (phase.order[0].value < 1 || phase.order[0].value > 2 || phase.order[1].value < 1 || phase.order[1].value > 2 ||
        phase.order[0].value == phase.order[1].value || phase.slot != 0 || phase.cadence != std::array<uint32_t, 2>{{1, 1}})
        return fail(FailureCode::IncompatibleOrder, "unsupported_box_phase");
    if (solid.triangles.size() > limits.max_segments || solid.vertices.size() > limits.max_segments ||
        cycle.triangles.size() > limits.max_segments || cycle.vertices.size() > limits.max_segments ||
        machine.sections.size() > limits.max_temporary_bytes / sizeof(EnvelopeSection) ||
        solid.triangles.size() > limits.max_temporary_bytes / 200 || cycle.triangles.size() > limits.max_temporary_bytes / 200)
        return fail(FailureCode::ResourceLimit, "solid_validation_limit");
    BoxPathPlan plan;
    plan.revision = cycle.revision;
    plan.machine  = machine;
    if (!verify_box(solid, cycle.tolerances.interface_mm, plan.solid_min, plan.solid_max))
        return fail(FailureCode::UnsupportedTopology, "constant_box_solid_required");
    if (!verify_domain(cycle, plan.solid_min, plan.solid_max))
        return fail(FailureCode::InvalidInput, "nonconforming_box_domain");
    const Vec3d  size   = plan.solid_max - plan.solid_min;
    const double height = cycle.top_mm - cycle.bottom_mm, width = cycle.process.bead_width_mm;
    if (options.perimeter_loops == 0 || options.perimeter_loops > 1000)
        return fail(FailureCode::UnsupportedProfile, "invalid_perimeter_count");
    const double shell_width = options.perimeter_loops * width;
    if (!std::isfinite(height) || height <= 0. || !std::isfinite(width) || width <= 0. || size.x() <= 3. * width || size.y() <= 3. * width)
        return fail(FailureCode::InfeasibleThickness, "bead_does_not_fit_box");
    if (size.x() <= 2. * shell_width + width || size.y() <= 2. * shell_width + width)
        return fail(FailureCode::InfeasibleThickness, "wall_domain_does_not_fit_box");
    const double cycles_exact = size.z() / height;
    if (!std::isfinite(cycles_exact) || cycles_exact > double(limits.max_cycles) || cycles_exact > 1000000000. || cycles_exact < 1.)
        return fail(FailureCode::ResourceLimit, "macro_cycle_limit");
    const size_t cycles = size_t(std::llround(cycles_exact));
    if (phase.first_active_cycle > std::numeric_limits<uint64_t>::max() - cycles ||
        cycle.interface_id > std::numeric_limits<uint64_t>::max() - cycles)
        return fail(FailureCode::ResourceLimit, "cycle_identity_overflow");
    if (std::abs(cycles * height - size.z()) > cycle.tolerances.macro_closure_mm)
        return fail(FailureCode::UnsupportedTopology, "partial_macro_cycle");
    for (double value : {machine.clearance_mm, machine.travel_lift_mm, machine.max_volume_mm3_s, machine.max_z_mm_s,
                         machine.deposition_mm_s, machine.max_segment_mm, machine.filament_diameter_mm[0], machine.filament_diameter_mm[1]})
        if (!std::isfinite(value) || value <= 0.)
            return fail(FailureCode::UnsupportedProfile, "invalid_machine_limit");
    if (machine.sections.empty())
        return fail(FailureCode::UnsupportedProfile, "complete_tool_envelope_required");
    const double xyz_quantum    = GCodeFormatter::pow_10_inv[GCodeFormatter::XYZF_EXPORT_DIGITS];
    const double segment_budget = machine.max_segment_mm - 2. * std::sqrt(3.) * xyz_quantum;
    if (segment_budget <= 0.)
        return fail(FailureCode::UnsupportedProfile, "segment_limit_below_output_quantum");
    double envelope_top = 0.;
    for (const auto& section : machine.sections) {
        if (!std::isfinite(section.bottom_mm) || !std::isfinite(section.top_mm) || !std::isfinite(section.radius_mm) ||
            section.bottom_mm != envelope_top || section.top_mm <= section.bottom_mm || section.radius_mm <= 0.)
            return fail(FailureCode::UnsupportedProfile, "invalid_or_incomplete_tool_envelope");
        envelope_top = section.top_mm;
    }
    SolverLimits solver_limits;
    solver_limits.max_temporary_bytes = std::min(solver_limits.max_temporary_bytes, limits.max_temporary_bytes / 2);
    const auto solved                 = solve_slab(cycle, solver_limits, cancelled);
    if (const auto* failure = std::get_if<Failure>(&solved))
        return *failure;
    const auto&              solution = *std::get<std::shared_ptr<const SlabSolution>>(solved);
    std::map<double, double> samples;
    for (const auto& vertex : solution.interface.surface.vertices) {
        if (vertex.x() < plan.solid_min.x() || vertex.x() > plan.solid_max.x() || vertex.y() < plan.solid_min.y() ||
            vertex.y() > plan.solid_max.y())
            return fail(FailureCode::InvalidInput, "interface_outside_box");
        const auto inserted = samples.emplace(vertex.x(), vertex.z() - cycle.bottom_mm);
        if (!inserted.second && std::abs(inserted.first->second - (vertex.z() - cycle.bottom_mm)) > cycle.tolerances.interface_mm)
            return fail(FailureCode::UnsupportedTopology, "x_only_field_required");
    }
    double domain_area = 0.;
    for (const auto& face : solution.interface.surface.triangles) {
        const auto& vertices = solution.interface.surface.vertices;
        const Vec3d normal   = (vertices[face[1]] - vertices[face[0]]).cross(vertices[face[2]] - vertices[face[0]]);
        if (std::abs(normal.y() / normal.z()) > 1e-10)
            return fail(FailureCode::UnsupportedTopology, "x_only_field_required");
        domain_area += normal.z() / 2.;
    }
    if (std::abs(domain_area - size.x() * size.y()) > cycle.tolerances.absolute_volume_mm3 / height ||
        samples.begin()->first != plan.solid_min.x() || samples.rbegin()->first != plan.solid_max.x())
        return fail(FailureCode::InvalidInput, "interface_domain_mismatch");
    const std::vector<std::pair<double, double>> profile(samples.begin(), samples.end());
    const auto [minimum_height, maximum_height] = std::minmax_element(profile.begin(), profile.end(),
                                                                      [](const auto& a, const auto& b) { return a.second < b.second; });
    const double interior_width                 = size.y() - 2. * shell_width;
    const double rows_exact                     = std::ceil(interior_width / width);
    if (!std::isfinite(rows_exact) || rows_exact > double(limits.max_paths / 2) || rows_exact < 1.)
        return fail(FailureCode::ResourceLimit, "coverage_lane_limit");
    const size_t rows  = options.shell_only ? 0 : size_t(rows_exact);
    const double pitch = options.shell_only ? width : interior_width / rows;
    // The tip stays within the intended rectangular bead contact footprint.
    // Every larger envelope section clears the highest possible prior deposit
    // in the entire object. This is conservative, including intermediate motion.
    for (const auto& section : machine.sections)
        if (section.radius_mm > std::min(width, pitch) / 2. - machine.clearance_mm &&
            section.bottom_mm < maximum_height->second - minimum_height->second + machine.clearance_mm)
            return fail(FailureCode::ClearanceFailure, "tool_body_intersects_prior_band_bound");
    if (machine.travel_lift_mm <= machine.clearance_mm)
        return fail(FailureCode::ClearanceFailure, "travel_clearance");
    if (cycles > limits.max_paths / (2 * (rows + options.perimeter_loops)))
        return fail(FailureCode::ResourceLimit, "path_count_limit");
    const size_t interface_bytes = options.shell_only ? (profile.size() + 2) * (4 * sizeof(Vec3d) + 6 * sizeof(std::array<uint32_t, 3>)) +
                                                            sizeof(InterfacePatch) :
                                                        solution.interface.surface.vertices.size() * sizeof(Vec3d) +
                                                            solution.interface.surface.triangles.size() * sizeof(std::array<uint32_t, 3>) +
                                                            sizeof(InterfacePatch);
    if (interface_bytes > limits.max_temporary_bytes || cycles > limits.max_temporary_bytes / interface_bytes)
        return fail(FailureCode::ResourceLimit, "interface_output_memory_limit");
    size_t       segment_count = 0, temporary_bytes = cycles * interface_bytes;
    const size_t path_count = cycles * 2 * (rows + options.perimeter_loops);
    if (path_count > (limits.max_temporary_bytes - temporary_bytes) / (sizeof(BandPath) + sizeof(Vec3d) + 64))
        return fail(FailureCode::ResourceLimit, "path_output_memory_limit");
    temporary_bytes += path_count * (sizeof(BandPath) + sizeof(Vec3d) + 64);
    plan.paths.reserve(path_count);
    plan.interfaces.reserve(cycles);
    FailureCode  path_failure = FailureCode::ResourceLimit;
    const char*  path_detail  = "generated_path_limit";
    const double x0 = plan.solid_min.x(), x1 = plan.solid_max.x(), y0 = plan.solid_min.y(), y1 = plan.solid_max.y();
    const auto   append_line = [&](BandPath& path, const Vec2d& start, const Vec2d& end, const Footprint& footprint, double bead_width,
                                 double bottom, size_t slot) -> bool {
        const int    axis              = std::abs(end.x() - start.x()) > 1e-12 ? 0 : 1;
        const double length            = (end - start).norm();
        const double subdivision_count = std::ceil(
            length * std::sqrt(1. + cycle.process.max_interface_gradient * cycle.process.max_interface_gradient) / segment_budget);
        if (!std::isfinite(subdivision_count) || subdivision_count < 1. || subdivision_count > double(limits.max_segments - segment_count))
            return false;
        const size_t        subdivisions = size_t(subdivision_count);
        std::vector<double> breaks;
        const size_t        break_count = subdivisions + (axis == 0 ? profile.size() : 0) + 1;
        if (break_count > (limits.max_temporary_bytes - temporary_bytes) / sizeof(double))
            return false;
        breaks.reserve(break_count);
        for (size_t i = 0; i <= subdivisions; ++i)
            breaks.push_back(double(i) / subdivisions);
        if (axis == 0)
            for (const auto& sample : profile) {
                const double t = (sample.first - start.x()) / (end.x() - start.x());
                if (t > 0. && t < 1.)
                    breaks.push_back(t);
            }
        std::sort(breaks.begin(), breaks.end());
        breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());
        // Split paint leaves can differ by float mesh roundoff. Coalesce only
        // identical emitted positions, before volume/coverage and scheduling.
        // Cells still split at every field knot; the shape residual is checked
        // below against the actual resulting straight deposition segment.
        const auto emitted_at = [&](double t) {
            const Vec2d p = start + (end - start) * t;
            return Vec3d(p.x(), p.y(), bottom + (slot ? height : field(profile, p.x())))
                .unaryExpr([](double v) { return GCodeFormatter::quantize_xyzf(v); }).eval();
        };
        std::vector<double> printable_breaks;
        for (double t : breaks) {
            if (printable_breaks.empty() || emitted_at(t) != emitted_at(printable_breaks.back())) printable_breaks.push_back(t);
            else if (t == 1. && printable_breaks.size() > 1) printable_breaks.back() = t;
        }
        if (printable_breaks.size() < 2) {
            path_failure = FailureCode::UnsupportedProfile;
            path_detail = "segment_collapses_at_output_precision";
            return false;
        }
        breaks = std::move(printable_breaks);
        for (size_t i = 1; i < breaks.size(); ++i) {
            const Vec2d  a = start + (end - start) * breaks[i - 1], b = start + (end - start) * breaks[i];
            const double a_height = field(profile, a.x()), b_height = field(profile, b.x());
            const Vec3d  a_xyz(a.x(), a.y(), bottom + (slot ? height : a_height));
            const Vec3d  b_xyz(b.x(), b.y(), bottom + (slot ? height : b_height));
            if ((b_xyz - a_xyz).norm() < 1e-10)
                continue;
            if (path.vertices.empty())
                path.vertices.push_back(a_xyz);
            else if ((path.vertices.back() - a_xyz).norm() > 1e-8) {
                path_failure = FailureCode::InvalidInput;
                path_detail  = "disconnected_path_vertices";
                return false;
            }
            Footprint cell = footprint;
            if (i > 1)
                cell = clip(cell, axis, a[axis], end[axis] > start[axis]);
            if (i + 1 < breaks.size())
                cell = clip(cell, axis, b[axis], end[axis] < start[axis]);
            const double area       = area_and_moment(cell).first;
            double       cell_min_x = a.x(), cell_max_x = a.x();
            for (const auto& vertex : cell) {
                cell_min_x = std::min(cell_min_x, vertex.x());
                cell_max_x = std::max(cell_max_x, vertex.x());
            }
            std::vector<double> checks{cell_min_x, cell_max_x};
            auto                sample = std::upper_bound(profile.begin(), profile.end(), cell_min_x,
                                                            [](double x, const auto& entry) { return x < entry.first; });
            for (; sample != profile.end() && sample->first < cell_max_x; ++sample)
                checks.push_back(sample->first);
            for (double x : checks) {
                const double nearest_x = std::clamp(x, std::min(a.x(), b.x()), std::max(a.x(), b.x()));
                const double interpolated = axis == 0 ? a_height + (b_height - a_height) * (nearest_x - a.x()) / (b.x() - a.x()) : a_height;
                if (std::abs(field(profile, x) - interpolated) > cycle.tolerances.path_approximation_mm) {
                    path_failure = FailureCode::MaterialToleranceExceeded;
                    path_detail  = "bead_surface_approximation";
                    return false;
                }
            }
            const double a_volume = integrate(cell, profile);
            const double volume   = slot ? area * height - a_volume : a_volume;
            if (!std::isfinite(volume) || volume <= 0.) {
                path_failure = FailureCode::MaterialToleranceExceeded;
                path_detail  = "invalid_footprint_volume";
                return false;
            }
            const double length_xyz = (b_xyz - a_xyz).norm(), dz = std::abs(b_xyz.z() - a_xyz.z());
            double       speed         = std::min({machine.deposition_mm_s, machine.max_volume_mm3_s * length_xyz / volume,
                                     dz > 0. ? machine.max_z_mm_s * length_xyz / dz : machine.deposition_mm_s});
            const Vec3d  emitted_delta = b_xyz.unaryExpr([](double value) { return GCodeFormatter::quantize_xyzf(value); }) -
                                        a_xyz.unaryExpr([](double value) { return GCodeFormatter::quantize_xyzf(value); });
            const double emitted_length = emitted_delta.norm();
            if (emitted_length == 0.) {
                path_failure = FailureCode::UnsupportedProfile;
                path_detail  = "segment_collapses_at_output_precision";
                return false;
            }
            const double diameter          = machine.filament_diameter_mm[phase.order[slot].value - 1];
            const double e_rounding_volume = GCodeFormatter::pow_10_inv[GCodeFormatter::E_EXPORT_DIGITS] * PI * diameter * diameter / 4.;
            speed                          = std::min(speed, machine.max_volume_mm3_s * emitted_length / (volume + e_rounding_volume));
            if (emitted_delta.z() != 0.)
                speed = std::min(speed, machine.max_z_mm_s * emitted_length / std::abs(emitted_delta.z()));
            speed = std::floor(speed * 60. / xyz_quantum) * xyz_quantum / 60.;
            if (speed <= 0.) {
                path_failure = FailureCode::UnsupportedProfile;
                path_detail  = "feed_below_output_precision";
                return false;
            }
            std::vector<BeadCell> bead_cells;
            // A cell can straddle a field knot even when its centerline does
            // not (mitered corners). Split the volume at every knot first.
            for (size_t knot = 1; knot < profile.size(); ++knot) {
                Footprint piece = clip(clip(cell, 0, profile[knot - 1].first, true), 0, profile[knot].first, false);
                for (size_t v = 2; v < piece.size(); ++v) {
                    const std::array<Vec2d, 3> tri{{piece[0], piece[v - 1], piece[v]}};
                    const Vec2d                u = tri[1] - tri[0], w = tri[2] - tri[0];
                    if (u.x() * w.y() - u.y() * w.x() <= 1e-14)
                        continue;
                    if (bead_cells.size() >= (limits.max_temporary_bytes - temporary_bytes) / (2 * sizeof(BeadCell)))
                        return false;
                    BeadCell bead;
                    for (size_t j = 0; j < 3; ++j) {
                        const double g  = bottom + field(profile, tri[j].x());
                        bead.upper[j]   = Vec3d(tri[j].x(), tri[j].y(), slot ? bottom + height : g);
                        bead.lower_z[j] = slot ? g : bottom;
                    }
                    bead_cells.push_back(std::move(bead));
                }
            }
            const size_t added_bytes = 2 * (sizeof(PathSegment) + sizeof(Vec3d)) + cell.capacity() * sizeof(Vec2d) +
                                       bead_cells.capacity() * sizeof(BeadCell);
            if (++segment_count > limits.max_segments || added_bytes > limits.max_temporary_bytes - temporary_bytes)
                return false;
            temporary_bytes += added_bytes;
            path.vertices.push_back(b_xyz);
            path.segments.push_back({std::move(cell),
                                     volume,
                                     bead_width,
                                     slot ? height - a_height : a_height,
                                     slot ? height - b_height : b_height,
                                     speed,
                                     {},
                                     std::move(bead_cells)});
            plan.volume_mm3[slot] += volume;
        }
        return true;
    };
    for (size_t cycle_index = 0; cycle_index < cycles; ++cycle_index) {
        if (cancelled && cancelled())
            return fail(FailureCode::Cancelled, "cancelled_during_paths");
        const double   bottom = plan.solid_min.z() + cycle_index * height;
        InterfacePatch patch  = solution.interface;
        patch.id              = cycle.interface_id + cycle_index;
        if (patch.id < cycle.interface_id)
            return fail(FailureCode::ResourceLimit, "interface_identity_overflow");
        patch.macro_cycle = phase.first_active_cycle + cycle_index;
        for (auto& vertex : patch.surface.vertices)
            vertex.z() += bottom - cycle.bottom_mm;
        if (options.shell_only) {
            // Conforming rectangular strips with the interior explicitly absent.
            std::set<double> xs{x0, x0 + shell_width, x1 - shell_width, x1};
            for (const auto& sample : profile)
                xs.insert(sample.first);
            const std::vector<double>   knots(xs.begin(), xs.end());
            const std::array<double, 4> ys{{y0, y0 + shell_width, y1 - shell_width, y1}};
            patch.surface = {};
            for (double x : knots)
                for (double y : ys)
                    patch.surface.vertices.emplace_back(x, y, bottom + field(profile, x));
            for (size_t x = 1; x < knots.size(); ++x)
                for (uint32_t y = 0; y < 3; ++y) {
                    if (y == 1 && knots[x - 1] >= x0 + shell_width && knots[x] <= x1 - shell_width)
                        continue;
                    const uint32_t a = uint32_t((x - 1) * 4) + y, b = uint32_t(x * 4) + y;
                    patch.surface.triangles.push_back({a, b, b + 1});
                    patch.surface.triangles.push_back({a, b + 1, a + 1});
                }
        }
        plan.interfaces.push_back(std::move(patch));
        for (size_t slot = 0; slot < 2; ++slot) {
            const auto make_path = [&](PathRole role) {
                BandPath path;
                path.id               = plan.paths.size() + 1;
                path.band             = {cycle.domain, phase.first_active_cycle + cycle_index, phase.order[slot], uint32_t(slot)};
                path.shared_interface = plan.interfaces.back().id;
                path.role             = role;
                path.split_reason     = role == PathRole::Perimeter ? "closed_perimeter" : "coverage_lane_end";
                return path;
            };
            for (size_t ring = 0; ring < options.perimeter_loops; ++ring) {
                const double x0 = plan.solid_min.x() + ring * width, x1 = plan.solid_max.x() - ring * width;
                const double y0 = plan.solid_min.y() + ring * width, y1 = plan.solid_max.y() - ring * width;
                BandPath     perimeter = make_path(PathRole::Perimeter);
                const Vec2d  a(x0 + width / 2., y0 + width / 2.), b(x1 - width / 2., y0 + width / 2.);
                const Vec2d  c(x1 - width / 2., y1 - width / 2.), d(x0 + width / 2., y1 - width / 2.);
                if (!append_line(perimeter, a, b, {{x0, y0}, {x1, y0}, {x1 - width, y0 + width}, {x0 + width, y0 + width}}, width, bottom,
                                 slot) ||
                    !append_line(perimeter, b, c, {{x1, y0}, {x1, y1}, {x1 - width, y1 - width}, {x1 - width, y0 + width}}, width, bottom,
                                 slot) ||
                    !append_line(perimeter, c, d, {{x1, y1}, {x0, y1}, {x0 + width, y1 - width}, {x1 - width, y1 - width}}, width, bottom,
                                 slot) ||
                    !append_line(perimeter, d, a, {{x0, y1}, {x0, y0}, {x0 + width, y0 + width}, {x0 + width, y1 - width}}, width, bottom,
                                 slot))
                    return fail(path_failure, path_detail);
                plan.paths.push_back(std::move(perimeter));
            }
            for (size_t row = 0; row < rows; ++row) {
                if (cancelled && cancelled())
                    return fail(FailureCode::Cancelled, "cancelled_during_paths");
                const double lower_y = y0 + shell_width + row * pitch, upper_y = y0 + shell_width + (row + 1) * pitch;
                const Vec2d  start(x0 + shell_width + pitch / 2., (lower_y + upper_y) / 2.);
                const Vec2d  end(x1 - shell_width - pitch / 2., (lower_y + upper_y) / 2.);
                BandPath     path = make_path(PathRole::DenseFill);
                if (!append_line(path, start, end,
                                 {{x0 + shell_width, lower_y},
                                  {x1 - shell_width, lower_y},
                                  {x1 - shell_width, upper_y},
                                  {x0 + shell_width, upper_y}},
                                 pitch, bottom, slot))
                    return fail(path_failure, path_detail);
                plan.paths.push_back(std::move(path));
            }
        }
    }
    for (size_t slot = 0; slot < 2; ++slot) {
        double expected = cycles * solution.volume_mm3[slot];
        if (options.shell_only) {
            const double inner_a     = integrate({{x0 + shell_width, y0 + shell_width},
                                                  {x1 - shell_width, y0 + shell_width},
                                                  {x1 - shell_width, y1 - shell_width},
                                                  {x0 + shell_width, y1 - shell_width}},
                                                 profile);
            const double inner_total = (size.x() - 2. * shell_width) * (size.y() - 2. * shell_width) * height;
            expected -= cycles * (slot == 0 ? inner_a : inner_total - inner_a);
        }
        if (std::abs(plan.volume_mm3[slot] - expected) >
            std::max(cycle.tolerances.absolute_volume_mm3, cycle.tolerances.relative_volume * expected))
            return fail(FailureCode::MaterialToleranceExceeded, "path_footprint_volume");
        plan.coverage[slot].target_mm3    = expected;
        plan.coverage[slot].deposited_mm3 = plan.volume_mm3[slot];
    }
    if (cancelled && cancelled())
        return fail(FailureCode::Cancelled, "cancelled_before_publish");
    return std::make_shared<const BoxPathPlan>(std::move(plan));
}

std::string diagnostic_gcode(const BoxPathPlan& plan, bool relative_extrusion, bool reset_each_path)
{
    GCodeWriter writer;
    writer.apply_print_config(static_cast<const PrintConfig&>(FullPrintConfig::defaults()));
    writer.config.gcode_flavor.value             = gcfRepRapFirmware;
    writer.config.use_relative_e_distances.value = relative_extrusion;
    writer.config.filament_diameter.values.assign(plan.machine.filament_diameter_mm.begin(), plan.machine.filament_diameter_mm.end());
    writer.config.retraction_length.values             = {1., 1.};
    writer.config.retraction_speed.values              = {30., 30.};
    writer.config.deretraction_speed.values            = {30., 30.};
    writer.config.retract_restart_extra.values         = {0., 0.};
    writer.config.use_firmware_retraction.value        = false;
    writer.config.single_extruder_multi_material.value = plan.machine.shared_nozzle;
    writer.config.travel_speed_z.value                 = plan.machine.max_z_mm_s;
    writer.set_extruders({0, 1});
    writer.set_is_first_layer(false);
    std::string output = "; MaterialBand diagnostic only: isolated virtual profile; not a production FinalPrintPlan\nG90\n";
    output += relative_extrusion ? "M83\n" : "M82\n";
    for (const auto& path : plan.paths) {
        output += ";MB_OPERATION TRAVEL\n";
        if (writer.extruder() != nullptr)
            output += writer.retract();
        // The safe plane is above the complete solid, so intermediate travel
        // and subsequent tool changes cannot collide with any prior cycle.
        const double safe_z = plan.solid_max.z() + plan.machine.travel_lift_mm;
        output += writer.travel_to_z(safe_z, "validated safe plane", true);
        output += writer.set_extruder(path.band.material.value - 1);
        if (reset_each_path)
            output += writer.reset_e(true);
        output += writer.travel_to_xy(path.vertices.front().head<2>());
        output += writer.travel_to_z(path.vertices.front().z(), {}, true);
        output += writer.unretract();
        output += ";MB_CYCLE " + std::to_string(path.band.macro_cycle) + " INTERFACE " + std::to_string(path.shared_interface) + " PATH " +
                  std::to_string(path.id) + " MATERIAL " + std::to_string(path.band.material.value) + "\n;MB_OPERATION DEPOSITION\n";
        const double diameter      = plan.machine.filament_diameter_mm[path.band.material.value - 1];
        const double filament_area = PI * diameter * diameter / 4.;
        for (size_t i = 0; i < path.segments.size(); ++i) {
            output += writer.set_speed(path.segments[i].speed_mm_s * 60.);
            output += writer.extrude_to_xyz(path.vertices[i + 1], path.segments[i].volume_mm3 / filament_area);
        }
    }
    output += ";MB_OPERATION TRAVEL\n";
    output += writer.travel_to_z(plan.solid_max.z() + plan.machine.travel_lift_mm, {}, true);
    return output;
}

} // namespace Slic3r::MaterialBand
