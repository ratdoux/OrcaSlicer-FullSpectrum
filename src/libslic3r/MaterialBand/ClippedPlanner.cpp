#include "ClippedPlanner.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/Triangulation.hpp"
#include "libslic3r/Tesselate.hpp"
#include <Eigen/Cholesky>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <stdexcept>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace Slic3r::MaterialBand {
namespace {
struct Abort
{
    FailureCode code;
    std::string detail;
};
struct Plane
{
    Vec2d  origin = Vec2d::Zero(), gradient = Vec2d::Zero();
    double z = 0.;
    double at(const Vec2d& p) const { return z + gradient.dot(p - origin); }
};

Vec2d mm(const Point& p) { return p.cast<double>() * SCALING_FACTOR; }
Point scaled(const Vec2d& p) { return Point(coord_t(std::llround(p.x() / SCALING_FACTOR)), coord_t(std::llround(p.y() / SCALING_FACTOR))); }
Polygon polygon(const std::vector<Vec2d>& points)
{
    Points out;
    for (const auto& p : points)
        out.push_back(scaled(p));
    Polygon result(std::move(out));
    result.make_counter_clockwise();
    return result;
}
double area_mm2(const ExPolygons& polygons)
{
    double total = 0.;
    for (const auto& p : polygons)
        total += p.area() * SCALING_FACTOR * SCALING_FACTOR;
    return total;
}
std::vector<Vec2d> tessellate_contours(const Polygons& contours)
{
    if (contours.empty())
        return {};
    // GLU's even/odd winding rule evaluates all contours in one polygon.
    // Boolean output can contain nested islands or touching hole boundaries;
    // processing each reported island separately can fill the same area twice.
    ExPolygon all(contours.front());
    all.holes.assign(contours.begin() + 1, contours.end());
    return triangulate_expolygon_2d(all);
}
std::vector<std::array<Vec2d, 3>> triangles(const ExPolygons& polygons, bool general = false)
{
    const Points points    = to_points(polygons);
    double       perimeter = 0.;
    for (const auto& p : polygons) {
        perimeter += p.contour.length();
        for (const auto& h : p.holes)
            perimeter += h.length();
    }
    const double area_tolerance = std::max(1e-8, 8. * perimeter * SCALING_FACTOR * SCALING_FACTOR);
    const auto   covers_domain  = [&](const std::vector<std::array<Vec2d, 3>>& pieces) {
        Polygons footprints;
        double   summed = 0.;
        for (const auto& tri : pieces) {
            auto footprint = polygon({tri[0], tri[1], tri[2]});
            summed += footprint.area() * SCALING_FACTOR * SCALING_FACTOR;
            footprints.push_back(std::move(footprint));
        }
        const auto covered = union_ex(footprints);
        return std::abs(summed - area_mm2(covered)) <= area_tolerance && area_mm2(diff_ex(polygons, covered)) <= area_tolerance &&
               area_mm2(diff_ex(covered, polygons)) <= area_tolerance;
    };
    // Clipped islands may touch at a vertex. Use the overload that welds
    // duplicate coordinates and maps its indices back to the original points.
    std::vector<std::array<Vec2d, 3>> result;
    std::vector<std::array<Vec2d, 3>> candidates;
    if (!general)
        try {
            const auto indices = Triangulation::triangulate(polygons);
            candidates.reserve(indices.size());
            for (const auto& face : indices)
                candidates.push_back({mm(points.at(face[0])), mm(points.at(face[1])), mm(points.at(face[2]))});
            double area = 0.;
            for (const auto& t : candidates) {
                const Vec2d a = t[1] - t[0], b = t[2] - t[0];
                area += std::abs(a.x() * b.y() - a.y() * b.x()) / 2.;
            }
            if (std::abs(area - area_mm2(polygons)) > area_tolerance)
                throw std::runtime_error("constrained_coverage_area");
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            general = true;
        }
    if (general) {
        // Normalize all touching contours before tessellating them together.
        // Per-island tessellation can duplicate a nested cap; a large domain's
        // aggregate rounding budget can hide that locally significant overlap.
        const auto tessellated = tessellate_contours(union_(polygons));
        if (tessellated.size() % 3 != 0)
            throw Abort{FailureCode::InvalidInput, "coverage_tessellation"};
        candidates.clear();
        for (size_t i = 0; i < tessellated.size(); i += 3)
            candidates.push_back({tessellated[i], tessellated[i + 1], tessellated[i + 2]});
        double area = 0.;
        for (const auto& t : candidates) {
            const Vec2d a = t[1] - t[0], b = t[2] - t[0];
            area += std::abs(a.x() * b.y() - a.y() * b.x()) / 2.;
        }
        if (std::abs(area - area_mm2(polygons)) > area_tolerance || !covers_domain(candidates))
            throw Abort{FailureCode::MaterialToleranceExceeded,
                        "coverage_tessellation_footprint:expected=" + std::to_string(area_mm2(polygons)) +
                            ":actual=" + std::to_string(area) + ":tolerance=" + std::to_string(area_tolerance)};
    }
    result.reserve(candidates.size());
    for (auto triangle : candidates) {
        // The general tessellator may introduce intersection vertices between
        // grid points. Publish on the same grid used by occupancy predicates.
        for (auto& point : triangle)
            point = mm(scaled(point));
        const Vec2d a = triangle[1] - triangle[0], b = triangle[2] - triangle[0];
        // Boolean paint-edge intersections can leave lattice triangles whose
        // area is below the occupancy predicate's coordinate precision. Their
        // omitted volume remains included in the coverage budget.
        if (std::abs(a.x() * b.y() - a.y() * b.x()) <= 4. * SCALING_FACTOR * SCALING_FACTOR)
            continue;
        if (a.x() * b.y() - a.y() * b.x() < 0.)
            std::swap(triangle[1], triangle[2]);
        result.push_back(triangle);
    }
    return result;
}
void compact_planar_segment(PathSegment& segment, const Plane& lower, const Plane& upper, const Vec2d& origin)
{
    if (segment.cells.size() <= 8)
        return;
    Polygons footprints;
    footprints.reserve(segment.coverage_triangles.size());
    for (const auto& tri : segment.coverage_triangles)
        footprints.push_back(polygon({tri[0] - origin, tri[1] - origin, tri[2] - origin}));
    std::vector<std::array<Vec2d, 3>> rebuilt;
    try {
        rebuilt = triangles(union_ex(footprints), true);
    } catch (const Abort&) {
        return; // Keep the validated subdivision if normalization is inconclusive.
    }
    if (rebuilt.empty() || rebuilt.size() >= segment.cells.size())
        return;
    std::vector<BeadCell> cells;
    cells.reserve(rebuilt.size());
    double volume = 0.;
    for (auto& tri : rebuilt) {
        const Vec2d  a = tri[1] - tri[0], b = tri[2] - tri[0];
        const double area = .5 * (a.x() * b.y() - a.y() * b.x());
        BeadCell     cell;
        for (size_t i = 0; i < 3; ++i) {
            cell.upper[i]   = Vec3d(tri[i].x() + origin.x(), tri[i].y() + origin.y(), upper.at(tri[i]));
            cell.lower_z[i] = lower.at(tri[i]);
            volume += area * (cell.upper[i].z() - cell.lower_z[i]) / 3.;
            tri[i] += origin;
        }
        cells.push_back(std::move(cell));
    }
    // Cap refinement rounds subdivision vertices to the XY grid. Normalize
    // their union before scheduling, removing tiny duplicate wedges as well as
    // interior edges. Recompute the physical dose and charge its change to the
    // enclosing band's unchanged coverage and recipe checks.
    if (std::abs(volume - segment.volume_mm3) > std::max(1e-10, segment.volume_mm3 * 1e-4))
        return;
    segment.speed_mm_s         = std::floor(segment.speed_mm_s * std::min(1., segment.volume_mm3 / volume) * 60000.) / 60000.;
    segment.volume_mm3         = volume;
    segment.cells              = std::move(cells);
    segment.coverage_triangles = std::move(rebuilt);
}

Mesh prism(const ExPolygons& polygons, const Plane& lower, const Plane& upper, const Vec2d& origin)
{
    const Points points = to_points(polygons);
    const auto   faces  = Triangulation::triangulate(polygons, points);
    Mesh         result;
    for (const auto& p : points) {
        const Vec2d xy = mm(p);
        result.vertices.emplace_back(xy.x() + origin.x(), xy.y() + origin.y(), upper.at(xy));
    }
    for (const auto& p : points) {
        const Vec2d xy = mm(p);
        result.vertices.emplace_back(xy.x() + origin.x(), xy.y() + origin.y(), lower.at(xy));
    }
    const uint32_t                                                                              n = uint32_t(points.size());
    std::map<std::pair<uint32_t, uint32_t>, std::pair<unsigned, std::pair<uint32_t, uint32_t>>> edges;
    for (const auto& face : faces) {
        std::array<uint32_t, 3> f{{uint32_t(face[0]), uint32_t(face[1]), uint32_t(face[2])}};
        if ((result.vertices[f[1]] - result.vertices[f[0]]).cross(result.vertices[f[2]] - result.vertices[f[0]]).z() < 0.)
            std::swap(f[1], f[2]);
        result.triangles.push_back(f);
        result.triangles.push_back({f[2] + n, f[1] + n, f[0] + n});
        for (size_t i = 0; i < 3; ++i) {
            const auto a = f[i], b = f[(i + 1) % 3];
            auto&      edge = edges[{std::min(a, b), std::max(a, b)}];
            ++edge.first;
            edge.second = {a, b};
        }
    }
    for (const auto& entry : edges) {
        if (entry.second.first == 2)
            continue;
        if (entry.second.first != 1)
            throw Abort{FailureCode::UnsupportedTopology, "nonmanifold_coverage_domain"};
        const auto [a, b] = entry.second.second;
        result.triangles.push_back({a, a + n, b + n});
        result.triangles.push_back({a, b + n, b});
    }
    return result;
}

Plane affine(const Mesh& surface, const Vec2d& origin, double tolerance)
{
    if (surface.triangles.empty())
        throw Abort{FailureCode::InvalidInput, "empty_shared_interface"};
    size_t best    = 0;
    double largest = 0.;
    for (size_t i = 0; i < surface.triangles.size(); ++i) {
        const auto& f = surface.triangles[i];
        for (auto index : f)
            if (index >= surface.vertices.size())
                throw Abort{FailureCode::InvalidInput, "interface_vertex_index"};
        const double projected = (surface.vertices[f[1]] - surface.vertices[f[0]]).cross(surface.vertices[f[2]] - surface.vertices[f[0]]).z();
        if (projected > largest) {
            best    = i;
            largest = projected;
        }
    }
    const auto&  face   = surface.triangles[best];
    const Vec3d& p      = surface.vertices[face[0]];
    const Vec3d  normal = (surface.vertices[face[1]] - p).cross(surface.vertices[face[2]] - p);
    if (!normal.allFinite() || normal.z() <= 0.)
        throw Abort{FailureCode::InvalidInput, "invalid_affine_interface"};
    // Use the whole clipped cap, centered and scaled, rather than a narrow
    // Boolean sliver whose normal amplifies the 1 nm vertex conversion quantum.
    Vec3d center = Vec3d::Zero();
    for (const auto& vertex : surface.vertices)
        center += vertex - p;
    center       = p + center / double(surface.vertices.size());
    double scale = 0.;
    for (const auto& vertex : surface.vertices)
        scale = std::max(scale, (vertex - center).head<2>().norm());
    if (scale <= 0.)
        throw Abort{FailureCode::InvalidInput, "degenerate_affine_cap"};
    Eigen::Matrix3d matrix = Eigen::Matrix3d::Zero();
    Vec3d           rhs    = Vec3d::Zero();
    for (const auto& vertex : surface.vertices) {
        const Vec3d row((vertex.x() - center.x()) / scale, (vertex.y() - center.y()) / scale, 1.);
        matrix += row * row.transpose();
        rhs += row * (vertex.z() - center.z());
    }
    const Vec3d fit = matrix.ldlt().solve(rhs);
    if (!fit.allFinite())
        throw Abort{FailureCode::InvalidInput, "ill_conditioned_affine_cap"};
    Plane result{center.head<2>() - origin, fit.head<2>() / scale, center.z() + fit.z()};
    for (const auto& vertex : surface.vertices)
        if (!vertex.allFinite() || std::abs(vertex.z() - result.at(vertex.head<2>() - origin)) > tolerance)
            throw Abort{FailureCode::UnsupportedTopology, "affine_clipped_interface_required"};
    return result;
}

ExPolygons cap(const Mesh& solid, const Plane& upper, const Vec2d& origin, double tolerance)
{
    Polygons pieces;
    for (const auto& face : solid.triangles) {
        for (auto index : face)
            if (index >= solid.vertices.size())
                throw Abort{FailureCode::InvalidInput, "band_vertex_index"};
        const Vec3d a = solid.vertices[face[0]], b = solid.vertices[face[1]], c = solid.vertices[face[2]];
        if ((b - a).cross(c - a).z() <= 0.)
            continue;
        if (std::abs(a.z() - upper.at(a.head<2>() - origin)) <= tolerance &&
            std::abs(b.z() - upper.at(b.head<2>() - origin)) <= tolerance && std::abs(c.z() - upper.at(c.head<2>() - origin)) <= tolerance)
            pieces.push_back(polygon({a.head<2>() - origin, b.head<2>() - origin, c.head<2>() - origin}));
    }
    ExPolygons result = union_ex(pieces);
    // Closed Douglas-Peucker keeps its starting point. Start at a real corner
    // so an arbitrary collinear Boolean vertex is not retained at the seam.
    for (auto& piece : result) {
        std::vector<Polygon*> boundaries{&piece.contour};
        for (auto& hole : piece.holes)
            boundaries.push_back(&hole);
        for (auto* boundary : boundaries) {
            size_t       corner       = 0;
            double       largest_turn = 0.;
            const size_t n            = boundary->points.size();
            for (size_t i = 0; i < n; ++i) {
                const Vec2d  a    = mm(boundary->points[i]) - mm(boundary->points[(i + n - 1) % n]);
                const Vec2d  b    = mm(boundary->points[(i + 1) % n]) - mm(boundary->points[i]);
                const double turn = std::abs(a.x() * b.y() - a.y() * b.x()) / std::max(1e-20, a.norm() * b.norm());
                if (turn > largest_turn) {
                    corner       = i;
                    largest_turn = turn;
                }
            }
            std::rotate(boundary->points.begin(), boundary->points.begin() + corner, boundary->points.end());
        }
    }
    return expolygons_simplify(result, scale_(.000002));
}

void validate_constraints(const ProcessConstraints& process,
                          const NumericTolerances&  tolerances,
                          const MachineEnvelope&    machine,
                          const CoverageLimits&     coverage)
{
    if (machine.material_count < 2 || machine.material_count > MAX_MATERIALS)
        throw Abort{FailureCode::InvalidInput, "invalid_material_count"};
    for (size_t i = 0; i < machine.material_count; ++i)
        if (!std::isfinite(machine.filament_diameter_mm[i]) || machine.filament_diameter_mm[i] <= 0.)
            throw Abort{FailureCode::InvalidInput, "invalid_filament_diameter"};
    for (double value :
         {process.bead_width_mm, process.min_active_thickness_mm, process.max_active_thickness_mm, process.max_interface_gradient,
          tolerances.interface_mm, tolerances.path_approximation_mm, tolerances.absolute_volume_mm3, machine.clearance_mm,
          machine.travel_lift_mm, machine.max_volume_mm3_s, machine.max_z_mm_s, machine.deposition_mm_s, machine.max_segment_mm,
          machine.filament_diameter_mm[0], machine.filament_diameter_mm[1], coverage.minimum_bead_width_mm})
        if (!std::isfinite(value) || value <= 0.)
            throw Abort{FailureCode::InvalidInput, "invalid_clipped_path_constraint"};
    if (process.min_active_thickness_mm > process.max_active_thickness_mm || coverage.minimum_bead_width_mm > process.bead_width_mm ||
        coverage.minimum_bead_width_mm < .004 || coverage.max_refinement_depth > 32)
        throw Abort{FailureCode::UnsupportedProfile, "clipped_bead_or_refinement_limits"};
    if (!std::isfinite(coverage.relative_missing_volume) || coverage.relative_missing_volume < 0. || coverage.relative_missing_volume > .01)
        throw Abort{FailureCode::InvalidInput, "invalid_coverage_budget"};
    if (!std::isfinite(coverage.infill.angle_degrees) || coverage.perimeter_loops > 1000)
        throw Abort{FailureCode::UnsupportedProfile, "invalid_dense_infill_options"};
    if (machine.sections.empty())
        throw Abort{FailureCode::UnsupportedProfile, "complete_tool_envelope_required"};
    if (machine.sections.size() > 1024)
        throw Abort{FailureCode::ResourceLimit, "tool_envelope_section_limit"};
    double top = 0.;
    for (const auto& section : machine.sections) {
        if (!std::isfinite(section.bottom_mm) || !std::isfinite(section.top_mm) || !std::isfinite(section.radius_mm) ||
            section.bottom_mm != top || section.top_mm <= top || section.radius_mm <= 0.)
            throw Abort{FailureCode::UnsupportedProfile, "invalid_or_incomplete_tool_envelope"};
        top = section.top_mm;
    }
    if (machine.travel_lift_mm <= machine.clearance_mm)
        throw Abort{FailureCode::ClearanceFailure, "travel_clearance"};
}

void dump_partition_input(const PathPlan&                        geometry,
                          const PlanarMaterialBand&              part,
                          const ProcessConstraints&              process,
                          const std::vector<PlanarMaterialBand>* bands             = nullptr,
                          const ExPolygons*                      preceding         = nullptr,
                          double                                 contact_z         = 0.,
                          double                                 common_foundation = 0.)
{
    const char* directory = std::getenv("MATERIAL_BAND_GEOMETRY_DUMP");
    if (!directory || !geometry.layered_process)
        return;
    std::ofstream replay(std::string(directory) + "/partition-input.json");
    replay << std::setprecision(17);
    const auto vector = [&](const Vec3d& p) { replay << '[' << p.x() << ',' << p.y() << ',' << p.z() << ']'; };
    replay << "{\"minimum\":";
    vector(geometry.solid_min);
    replay << ",\"maximum\":";
    vector(geometry.solid_max);
    replay << ",\"process\":[" << process.bead_width_mm << ',' << process.min_active_thickness_mm << ',' << process.max_active_thickness_mm
           << ',' << process.macro_height_mm << ']';
    replay << ",\"machine\":[" << geometry.machine.material_count << ',' << geometry.machine.max_segment_mm << ','
           << geometry.machine.max_volume_mm3_s << ',' << geometry.machine.deposition_mm_s << ']';
    replay << ",\"band\":[" << part.id.domain << ',' << part.id.macro_cycle << ',' << part.id.material.value << ',' << part.bottom_mm << ','
           << part.top_mm << "],\"domain\":[";
    bool comma = false;
    for (const auto& polygon : part.footprint) {
        if (comma)
            replay << ',';
        comma = true;
        replay << '[';
        bool ring_comma = false;
        for (const auto& ring : to_polygons(polygon)) {
            if (ring_comma)
                replay << ',';
            ring_comma = true;
            replay << '[';
            for (size_t p = 0; p < ring.points.size(); ++p) {
                if (p)
                    replay << ',';
                replay << '[' << ring.points[p].x() << ',' << ring.points[p].y() << ']';
            }
            replay << ']';
        }
        replay << ']';
    }
    replay << "],\"segments\":[";
    comma = false;
    for (const auto& path : geometry.paths)
        for (size_t si = 0; si < path.segments.size(); ++si) {
            const auto& segment = path.segments[si];
            if (comma)
                replay << ',';
            comma = true;
            replay << "{\"a\":";
            vector(path.vertices[si]);
            replay << ",\"b\":";
            vector(path.vertices[si + 1]);
            replay << ",\"role\":" << int(path.role) << ",\"width\":" << segment.width_mm << ",\"volume\":" << segment.volume_mm3
                   << ",\"speed\":" << segment.speed_mm_s << ",\"triangles\":[";
            for (size_t t = 0; t < segment.coverage_triangles.size(); ++t) {
                if (t)
                    replay << ',';
                replay << '[';
                for (size_t p = 0; p < 3; ++p) {
                    if (p)
                        replay << ',';
                    const auto& xy = segment.coverage_triangles[t][p];
                    replay << '[' << xy.x() << ',' << xy.y() << ']';
                }
                replay << ']';
            }
            replay << "]}";
        }
    replay << ']';
    if (bands) {
        const auto polygons = [&](const ExPolygons& pieces) {
            replay << '[';
            for (size_t i = 0; i < pieces.size(); ++i) {
                replay << (i ? "," : "") << '[';
                const auto rings = to_polygons(pieces[i]);
                for (size_t r = 0; r < rings.size(); ++r) {
                    replay << (r ? "," : "") << '[';
                    for (size_t p = 0; p < rings[r].points.size(); ++p) {
                        const auto& point = rings[r].points[p];
                        replay << (p ? "," : "") << '[' << point.x() << ',' << point.y() << ']';
                    }
                    replay << ']';
                }
                replay << ']';
            }
            replay << ']';
        };
        replay << ",\"bands\":[";
        for (size_t i = 0; i < bands->size(); ++i) {
            const auto& b = (*bands)[i];
            replay << (i ? "," : "") << '[' << b.id.domain << ',' << b.id.macro_cycle << ',' << b.id.material.value << ','
                   << b.id.stack_slot << ',' << b.bottom_mm << ',' << b.top_mm << ',' << b.lower_interface << ',' << b.upper_interface
                   << ',';
            polygons(b.footprint);
            replay << ']';
        }
        replay << "],\"preceding\":";
        polygons(preceding ? *preceding : ExPolygons{});
        replay << ",\"contact_z\":" << contact_z << ",\"common_foundation\":" << common_foundation;
    }
    replay << "}\n";
}
struct Builder
{
    const ProcessConstraints&    process;
    const NumericTolerances&     tolerances;
    const MachineEnvelope&       machine;
    const PathLimits&            limits;
    const CoverageLimits&        coverage;
    const std::function<bool()>& cancelled;
    const std::function<bool()>& stale;
    Vec2d                        origin;
    PathPlan                     plan;
    size_t                       cells = 0, segments = 0, bytes = 0;
    double                       extent            = 0.;
    Eigen::Matrix2d              fill_rotation     = Eigen::Matrix2d::Identity();
    size_t                       fill_runs         = 0;
    double                       omitted_fill_area = 0.;
    bool                         general_coverage  = false;

    ExPolygons rotate_fill(ExPolygons polygons, bool inverse) const
    {
        const Eigen::Matrix2d rotation = inverse ? fill_rotation.transpose().eval() : fill_rotation;
        for (auto& piece : polygons) {
            const auto rotate = [&](Polygon& boundary) {
                for (auto& point : boundary.points)
                    point = scaled(rotation * mm(point));
            };
            rotate(piece.contour);
            for (auto& hole : piece.holes)
                rotate(hole);
        }
        return polygons;
    }

    void check(size_t added = 0)
    {
        if (cancelled && cancelled())
            throw Abort{FailureCode::Cancelled, "cancelled_during_clipped_paths"};
        if (stale && stale())
            throw Abort{FailureCode::StaleInput, "revision_changed"};
        if (++cells > coverage.max_candidate_cells || added > limits.max_temporary_bytes - bytes)
            throw Abort{FailureCode::ResourceLimit, "clipped_path_work_limit"};
        bytes += added;
    }
    BandPath path(const ClippedBand& band, PathRole role, const char* reason)
    {
        if (plan.paths.size() >= limits.max_paths)
            throw Abort{FailureCode::ResourceLimit, "clipped_path_count_limit"};
        check(sizeof(BandPath) + 64);
        BandPath p;
        p.id               = plan.paths.size() + 1;
        p.band             = band.id;
        p.shared_interface = band.shared_interface;
        p.role             = role;
        p.split_reason     = reason;
        return p;
    }
    void append(BandPath&         path,
                const Vec2d&      start,
                const Vec2d&      end,
                const ExPolygons& footprint,
                const Plane&      lower,
                const Plane&      upper,
                double            width)
    {
        const Vec2d  delta  = end - start;
        const double length = delta.norm(), budget = machine.max_segment_mm - .004;
        if (length <= .001 || budget <= 0.)
            throw Abort{FailureCode::InfeasibleThickness, "coverage_run_below_output_precision"};
        const Vec2d  direction = delta / length, normal(-direction.y(), direction.x());
        const double exact     = std::ceil(length * std::sqrt(1. + upper.gradient.squaredNorm()) / budget);
        if (!std::isfinite(exact) || exact > double(limits.max_segments - segments))
            throw Abort{FailureCode::ResourceLimit, "clipped_segment_limit"};
        const size_t count = size_t(exact);
        for (size_t i = 0; i < count; ++i) {
            check();
            const Vec2d      a = start + delta * (double(i) / count), b = start + delta * (double(i + 1) / count);
            const Vec3d      from(a.x() + origin.x(), a.y() + origin.y(), upper.at(a));
            const Vec3d      to(b.x() + origin.x(), b.y() + origin.y(), upper.at(b));
            const Vec2d      left    = i == 0 ? start - direction * extent : a;
            const Vec2d      right   = i + 1 == count ? end + direction * extent : b;
            const ExPolygons clipped = intersection_ex(footprint, Polygons{polygon({left - normal * extent, right - normal * extent,
                                                                                    right + normal * extent, left + normal * extent})});
            auto             pieces  = triangles(clipped, general_coverage);
            double           volume  = 0.;
            for (const auto& tri : pieces) {
                const Vec2d  u = tri[1] - tri[0], v = tri[2] - tri[0];
                const double area = (u.x() * v.y() - u.y() * v.x()) / 2.;
                double       h    = 0.;
                for (const auto& p : tri) {
                    h += (upper.at(p) - lower.at(p)) / 3.;
                    const Vec2d nearest = a + direction * std::clamp(direction.dot(p - a), 0., (b - a).norm());
                    if (std::abs(upper.at(p) - upper.at(nearest)) > tolerances.path_approximation_mm)
                        throw Abort{FailureCode::MaterialToleranceExceeded, "clipped_bead_surface_approximation"};
                }
                volume += area * h;
            }
            if (!std::isfinite(volume) || volume <= 0.)
                throw Abort{FailureCode::InfeasibleThickness, "empty_coverage_segment"};
            const Vec3d emitted = to.unaryExpr([](double x) { return GCodeFormatter::quantize_xyzf(x); }) -
                                  from.unaryExpr([](double x) { return GCodeFormatter::quantize_xyzf(x); });
            const double distance = emitted.norm(), intended_distance = (to - from).norm();
            if (distance <= 0.)
                throw Abort{FailureCode::InfeasibleThickness, "coverage_segment_collapses_at_output_precision"};
            const double diameter  = machine.filament_diameter_mm.at(path.band.material.value - 1);
            const double e_quantum = 1e-5 * PI * diameter * diameter / 4.;
            if (volume < e_quantum)
                throw Abort{FailureCode::InfeasibleThickness, "coverage_dose_below_output_precision"};
            double speed = std::min({machine.deposition_mm_s, machine.max_volume_mm3_s * distance / (volume + e_quantum),
                                     machine.max_volume_mm3_s * intended_distance / volume});
            if (emitted.z() != 0.)
                speed = std::min(speed, machine.max_z_mm_s * distance / std::abs(emitted.z()));
            if (to.z() != from.z())
                speed = std::min(speed, machine.max_z_mm_s * intended_distance / std::abs(to.z() - from.z()));
            speed = std::floor(speed * 60000.) / 60000.;
            if (speed <= 0.)
                throw Abort{FailureCode::UnsupportedProfile, "coverage_feed_below_output_precision"};
            if (path.vertices.empty())
                path.vertices.push_back(from);
            else if ((path.vertices.back() - from).norm() > tolerances.interface_mm)
                throw Abort{FailureCode::InvalidInput, "disconnected_clipped_run"};
            check(2 * (sizeof(PathSegment) + sizeof(Vec3d)) + pieces.capacity() * (sizeof(std::array<Vec2d, 3>) + sizeof(BeadCell)));
            if (++segments > limits.max_segments)
                throw Abort{FailureCode::ResourceLimit, "clipped_segment_limit"};
            PathSegment segment;
            segment.cells.reserve(pieces.size());
            for (const auto& tri : pieces) {
                BeadCell cell;
                for (size_t j = 0; j < 3; ++j) {
                    cell.upper[j]   = Vec3d(tri[j].x() + origin.x(), tri[j].y() + origin.y(), upper.at(tri[j]));
                    cell.lower_z[j] = lower.at(tri[j]);
                }
                segment.cells.push_back(std::move(cell));
            }
            for (auto& tri : pieces)
                for (auto& p : tri)
                    p += origin;
            segment.coverage_triangles = std::move(pieces);
            segment.volume_mm3         = volume;
            segment.width_mm           = width;
            segment.height_start_mm    = upper.at(a) - lower.at(a);
            segment.height_end_mm      = upper.at(b) - lower.at(b);
            segment.speed_mm_s         = speed;
            path.segments.push_back(std::move(segment));
            path.vertices.push_back(to);
            plan.volume_mm3.at(path.band.stack_slot) += volume;
        }
    }

    void fill(
        const ExPolygons& domain, double low, double high, const ClippedBand& band, const Plane& lower, const Plane& upper, size_t depth)
    {
        check();
        const auto stripe = intersection_ex(domain, Polygons{polygon({{-extent, low}, {extent, low}, {extent, high}, {-extent, high}})});
        if (stripe.empty())
            return;
        const double    middle = (low + high) / 2.;
        const Polylines lines  = intersection_pl(Polylines{Polyline(Points{scaled({-extent, middle}), scaled({extent, middle})})}, stripe);
        std::vector<std::pair<double, double>> intervals;
        for (const auto& line : lines) {
            if (line.points.size() < 2)
                continue;
            const double x0 = mm(line.points.front()).x(), x1 = mm(line.points.back()).x();
            intervals.emplace_back(std::min(x0, x1), std::max(x0, x1));
        }
        std::sort(intervals.begin(), intervals.end());
        bool                    refine = intervals.empty();
        std::vector<ExPolygons> regions;
        std::vector<bool>       incomplete;
        for (size_t i = 0; i < intervals.size(); ++i) {
            const double left   = i == 0 ? -extent : (intervals[i - 1].second + intervals[i].first) / 2.;
            const double right  = i + 1 == intervals.size() ? extent : (intervals[i].second + intervals[i + 1].first) / 2.;
            auto         region = intersection_ex(stripe, Polygons{polygon({{left, low}, {right, low}, {right, high}, {left, high}})});
            bool         does_not_fit = false;
            for (const auto& p : to_points(region)) {
                const double x = mm(p).x();
                if (x < intervals[i].first - process.bead_width_mm / 2. || x > intervals[i].second + process.bead_width_mm / 2.)
                    does_not_fit = true;
            }
            if (intervals[i].second - intervals[i].first <= .01)
                does_not_fit = true;
            refine = refine || does_not_fit;
            incomplete.push_back(does_not_fit);
            regions.push_back(std::move(region));
        }
        if (refine) {
            if (depth < coverage.max_refinement_depth && (high - low) / 2. >= coverage.minimum_bead_width_mm) {
                fill(domain, low, middle, band, lower, upper, depth + 1);
                fill(domain, middle, high, band, lower, upper, depth + 1);
                return;
            }
            if (intervals.empty()) {
                omitted_fill_area += area_mm2(stripe);
                return;
            }
        }
        for (size_t i = 0; i < intervals.size(); ++i) {
            // A tiny disconnected cap must not suppress the printable interval
            // beside it when refinement reaches the minimum bead width.
            if (incomplete[i]) {
                omitted_fill_area += area_mm2(regions[i]);
                continue;
            }
            if (area_mm2(regions[i]) / (intervals[i].second - intervals[i].first) < coverage.minimum_bead_width_mm) {
                omitted_fill_area += area_mm2(regions[i]);
                continue;
            }
            // The U1 stock cylinder bounds the entire head; its radius is not
            // a nozzle diameter. Its flat-bottom contact is checked by the
            // scheduler, including neighboring objects and earlier beads.
            const double tip_inset = ((machine.coplanar_tip_contact ? 0. : machine.sections.front().radius_mm) + machine.clearance_mm +
                                      .002) /
                                     std::cos(PI / 16.);
            const auto center_domain = offset_ex(regions[i], float(-tip_inset / SCALING_FACTOR));
            const auto centers       = intersection_pl(Polylines{Polyline(
                                                     Points{scaled({intervals[i].first, middle}), scaled({intervals[i].second, middle})})},
                                                       center_domain);
            if (centers.size() != 1 || centers.front().points.size() < 2) {
                omitted_fill_area += area_mm2(regions[i]);
                continue; // Omission is charged to the missing-volume budget below.
            }
            const double center_a = mm(centers.front().points.front()).x(), center_b = mm(centers.front().points.back()).x();
            // The vertical bead cell includes the end caps. Keep the nozzle
            // center inside those caps instead of placing its disk across an
            // already deposited perimeter at the lane boundary.
            const double cap_inset = std::min((high - low) / 2., (intervals[i].second - intervals[i].first) / 4.);
            const double start     = std::max(intervals[i].first + cap_inset, std::min(center_a, center_b));
            const double end       = std::min(intervals[i].second - cap_inset, std::max(center_a, center_b));
            if (end - start <= .001) {
                omitted_fill_area += area_mm2(regions[i]);
                continue;
            }
            Vec2d       a = fill_rotation * Vec2d(start, middle), b = fill_rotation * Vec2d(end, middle);
            const Vec3d emitted_a = Vec3d(a.x() + origin.x(), a.y() + origin.y(), upper.at(a)).unaryExpr([](double x) {
                return GCodeFormatter::quantize_xyzf(x);
            });
            const Vec3d emitted_b = Vec3d(b.x() + origin.x(), b.y() + origin.y(), upper.at(b)).unaryExpr([](double x) {
                return GCodeFormatter::quantize_xyzf(x);
            });
            if ((emitted_b - emitted_a).squaredNorm() == 0.) {
                // A short diagonal can lie inside one output-coordinate cell
                // even when its length exceeds one coordinate quantum. Leave
                // its area for adjacent bead caps or the explicit gap budget.
                omitted_fill_area += area_mm2(regions[i]);
                continue;
            }
            auto run = path(band, PathRole::DenseFill, intervals.size() > 1 ? "void_separates_coverage_lanes" : "coverage_lane_end");
            if (coverage.infill.pattern == DenseInfillPattern::Rectilinear && fill_runs % 2)
                std::swap(a, b);
            ++fill_runs;
            const double previous_volume = plan.volume_mm3.at(band.id.stack_slot);
            try {
                append(run, a, b, rotate_fill(regions[i], false), lower, upper, high - low);
            } catch (const Abort& error) {
                if (!plan.layered_process || error.code != FailureCode::InfeasibleThickness ||
                    (error.detail != "coverage_dose_below_output_precision" &&
                     error.detail != "coverage_segment_collapses_at_output_precision" && error.detail != "empty_coverage_segment"))
                    throw;
                // The run is still private. Roll back any earlier segments in
                // it and leave the whole footprint for cap repair/coverage
                // accounting, instead of publishing a zero-E motion.
                plan.volume_mm3.at(band.id.stack_slot) = previous_volume;
                omitted_fill_area += area_mm2(regions[i]);
                continue;
            }
            plan.paths.push_back(std::move(run));
        }
    }

    void close_infill_gaps(const ExPolygons&  domain,
                           size_t             first_path,
                           const ClippedBand& band,
                           const Plane&       lower,
                           const Plane&       upper,
                           bool               include_walls = false)
    {
        Polygons occupied;
        for (size_t pi = first_path; pi < plan.paths.size(); ++pi)
            for (const auto& segment : plan.paths[pi].segments)
                for (const auto& tri : segment.coverage_triangles)
                    occupied.push_back(polygon({tri[0] - origin, tri[1] - origin, tri[2] - origin}));
        // A residual outside every existing bead's allowed end/side cap
        // cannot be repaired by subdivision. Exclude it from the refinement
        // queue, but retain it in the final missing-coverage accounting.
        Polygons     reachable_caps;
        const double cap_radius = process.bead_width_mm / 2. + 8. * SCALING_FACTOR;
        for (size_t pi = first_path; pi < plan.paths.size(); ++pi) {
            const auto& path = plan.paths[pi];
            if (!include_walls && path.role != PathRole::DenseFill)
                continue;
            for (size_t si = 0; si < path.segments.size(); ++si) {
                const Vec2d a = path.vertices[si].head<2>() - origin, b = path.vertices[si + 1].head<2>() - origin;
                const Vec2d direction = (b - a).normalized(), normal(-direction.y(), direction.x());
                reachable_caps.push_back(polygon({a - cap_radius * (direction + normal), b + cap_radius * (direction - normal),
                                                  b + cap_radius * (direction + normal), a - cap_radius * (direction - normal)}));
            }
        }
        // Never filter individual residual contours before triangulation:
        // a negative-area component may encode a hole around existing walls.
        const auto missing = diff_ex(intersection_ex(domain, union_(reachable_caps)), union_(occupied));
        struct Gap
        {
            std::array<Vec2d, 3> triangle;
            size_t               depth;
        };
        std::vector<Gap> pending;
        const auto       gap_triangles = triangles(missing, true);
        check(gap_triangles.capacity() * (sizeof(std::array<Vec2d, 3>) + sizeof(Gap)));
        for (const auto& triangle : gap_triangles) {
            const Vec2d  a = triangle[1] - triangle[0], b = triangle[2] - triangle[0];
            const double area      = .5 * std::abs(a.x() * b.y() - a.y() * b.x());
            const double perimeter = a.norm() + b.norm() + (triangle[2] - triangle[1]).norm();
            if (area <= std::max(1e-10, 8. * perimeter * SCALING_FACTOR))
                continue;
            pending.push_back({triangle, 0});
        }
        if (pending.empty())
            return;
        // A cap that fits a segment's bounded-width rectangle also has its
        // centroid in that rectangle's bounding box. Index those boxes so gap
        // fitting stays local even on a large painted layer.
        using GridKey    = std::pair<int64_t, int64_t>;
        using SegmentRef = std::pair<size_t, size_t>;
        std::map<GridKey, std::vector<SegmentRef>> nearby;
        std::map<SegmentRef, ExPolygons>           cap_regions;
        const double                               pitch    = std::max(process.bead_width_mm, machine.max_segment_mm);
        const auto                                 grid_key = [&](const Vec2d& p) -> GridKey {
            return {int64_t(std::floor(p.x() / pitch)), int64_t(std::floor(p.y() / pitch))};
        };
        for (size_t pi = first_path; pi < plan.paths.size(); ++pi) {
            const auto& path = plan.paths[pi];
            if (!include_walls && path.role != PathRole::DenseFill)
                continue;
            for (size_t si = 0; si < path.segments.size(); ++si) {
                const Vec2d a = path.vertices[si].head<2>() - origin, b = path.vertices[si + 1].head<2>() - origin;
                const Vec2d margin = Vec2d::Constant(process.bead_width_mm);
                const auto  low = grid_key(a.cwiseMin(b) - margin), high = grid_key(a.cwiseMax(b) + margin);
                for (int64_t x = low.first; x <= high.first; ++x)
                    for (int64_t y = low.second; y <= high.second; ++y) {
                        check(sizeof(SegmentRef) + 64);
                        nearby[{x, y}].emplace_back(pi, si);
                    }
            }
        }
        size_t scans = 0;
        while (!pending.empty()) {
            check();
            const auto gap = pending.back();
            pending.pop_back();
            auto triangle = gap.triangle;
            for (auto& point : triangle)
                point = mm(scaled(point));
            const Vec2d  u = triangle[1] - triangle[0], v = triangle[2] - triangle[0];
            const double area = .5 * std::abs(u.x() * v.y() - u.y() * v.x());
            if (area < 1e-10)
                continue;
            if (u.x() * v.y() - u.y() * v.x() < 0.)
                std::swap(triangle[1], triangle[2]);
            PathSegment* best = nullptr;
            SegmentRef   best_ref{};
            bool         disconnected_fit = false;
            double       best_width = 0., best_score = std::numeric_limits<double>::max();
            double       best_distance = 0., best_emitted_distance = 0.;
            const auto   candidates = nearby.find(grid_key((triangle[0] + triangle[1] + triangle[2]) / 3.));
            if (candidates != nearby.end()) {
                for (const auto& [pi, si] : candidates->second) {
                    auto& path = plan.paths[pi];
                    if (++scans % 64 == 0)
                        check();
                    const Vec2d  a = path.vertices[si].head<2>() - origin, b = path.vertices[si + 1].head<2>() - origin;
                    const double length     = (b - a).norm();
                    const Vec2d  direction  = (b - a) / length;
                    double       half_width = 0., score = 0.;
                    bool         fits = true;
                    for (const auto& point : triangle) {
                        const Vec2d  delta  = point - a;
                        const double along  = direction.dot(delta);
                        const double across = std::abs(direction.x() * delta.y() - direction.y() * delta.x());
                        // Only enlarge the rectangular end/side cap inside the
                        // configured bead width. This cannot bridge a cavity or
                        // authorize material outside the clipped solid.
                        const double cap_margin = process.bead_width_mm / 2. + 8. * SCALING_FACTOR;
                        if (across > cap_margin || along < -cap_margin || along > length + cap_margin) {
                            fits = false;
                            break;
                        }
                        half_width          = std::max(half_width, across);
                        const Vec2d nearest = a + direction * std::clamp(along, 0., length);
                        if (std::abs(upper.at(point) - upper.at(nearest)) > tolerances.path_approximation_mm) {
                            fits = false;
                            break;
                        }
                        score += (point - nearest).squaredNorm();
                    }
                    if (!fits || score >= best_score)
                        continue;
                    const SegmentRef ref{pi, si};
                    if (plan.layered_process) {
                        auto connected = cap_regions.find(ref);
                        if (connected == cap_regions.end()) {
                            Polygons pieces;
                            for (const auto& existing : path.segments[si].coverage_triangles)
                                pieces.push_back(polygon({existing[0] - origin, existing[1] - origin, existing[2] - origin}));
                            auto region = union_ex(pieces);
                            check(to_points(region).size() * sizeof(Point) * 4 + region.size() * sizeof(ExPolygon) * 4 + 128);
                            connected = cap_regions.emplace(ref, std::move(region)).first;
                        }
                        // A width-bounded triangle is not necessarily a bead cap:
                        // it may be an isolated island across an already occupied
                        // lane. Require a shared edge with this actual segment.
                        // Remaining islands go through the later residual fill.
                        double contact = 0.;
                        for (const auto& line : intersection_pl(to_polylines(Polygons{polygon({triangle[0], triangle[1], triangle[2]})}),
                                                                offset_ex(connected->second, 2.f)))
                            contact += line.length() * SCALING_FACTOR;
                        if (contact <= 8. * SCALING_FACTOR) {
                            disconnected_fit = true;
                            continue;
                        }
                    }
                    best                  = &path.segments[si];
                    best_ref              = ref;
                    best_width            = std::max(best->width_mm, 2. * half_width);
                    best_score            = score;
                    best_distance         = (path.vertices[si + 1] - path.vertices[si]).norm();
                    best_emitted_distance = (path.vertices[si + 1].unaryExpr([](double x) { return GCodeFormatter::quantize_xyzf(x); }) -
                                             path.vertices[si].unaryExpr([](double x) {
                                                 return GCodeFormatter::quantize_xyzf(x);
                                             })).norm();
                }
            }
            if (best) {
                if (plan.layered_process) {
                    auto& region = cap_regions.at(best_ref);
                    region       = union_ex(region, ExPolygons{ExPolygon(polygon({triangle[0], triangle[1], triangle[2]}))});
                }
                check(sizeof(BeadCell) + sizeof(std::array<Vec2d, 3>));
                BeadCell cell;
                auto     world  = triangle;
                double   height = 0.;
                for (size_t i = 0; i < 3; ++i) {
                    world[i] += origin;
                    cell.upper[i]   = Vec3d(world[i].x(), world[i].y(), upper.at(triangle[i]));
                    cell.lower_z[i] = lower.at(triangle[i]);
                    height += (cell.upper[i].z() - cell.lower_z[i]) / 3.;
                }
                const double volume = area * height;
                best->coverage_triangles.push_back(world);
                best->cells.push_back(cell);
                best->volume_mm3 += volume;
                best->width_mm         = best_width;
                const double diameter  = machine.filament_diameter_mm.at(band.id.material.value - 1);
                const double e_quantum = 1e-5 * PI * diameter * diameter / 4.;
                best->speed_mm_s       = std::floor(std::min({best->speed_mm_s, machine.max_volume_mm3_s * best_distance / best->volume_mm3,
                                                              machine.max_volume_mm3_s * best_emitted_distance / (best->volume_mm3 + e_quantum)}) *
                                                    60000.) /
                                   60000.;
                if (best->speed_mm_s <= 0.)
                    throw Abort{FailureCode::UnsupportedProfile, "coverage_feed_below_output_precision"};
                plan.volume_mm3.at(band.id.stack_slot) += volume;
            } else if (!disconnected_fit && gap.depth < coverage.max_refinement_depth) {
                size_t edge = 0;
                for (size_t i = 1; i < 3; ++i)
                    if ((triangle[(i + 1) % 3] - triangle[i]).squaredNorm() > (triangle[(edge + 1) % 3] - triangle[edge]).squaredNorm())
                        edge = i;
                const size_t next = (edge + 1) % 3, last = (edge + 2) % 3;
                const Vec2d  middle = (triangle[edge] + triangle[next]) / 2.;
                pending.push_back({{triangle[edge], middle, triangle[last]}, gap.depth + 1});
                pending.push_back({{middle, triangle[next], triangle[last]}, gap.depth + 1});
            }
            // Unmanufacturable caps remain explicit missing volume, and the
            // scheduler still requires support under every later deposited cell.
        }
    }

    // Validate a complete ring before publishing any of it. The same disjoint
    // miter cells serve configured walls and concentric dense infill; only the
    // path role differs. A narrow remainder is filled by bounded-width lanes.
    bool ring(ExPolygons& domain, const ClippedBand& band, const Plane& lower, const Plane& upper, PathRole role)
    {
        struct Ring
        {
            std::vector<Vec2d>      middle;
            std::vector<ExPolygons> cells;
        };
        std::vector<Ring> rings;
        Polygons          shell_cells;
        double            shell_area = 0.;
        for (const auto& piece : domain) {
            Polygons boundaries{piece.contour};
            boundaries.insert(boundaries.end(), piece.holes.begin(), piece.holes.end());
            for (const auto& boundary : boundaries) {
                check(boundary.points.size() * 128);
                const size_t       n = boundary.points.size();
                std::vector<Vec2d> original, inset;
                Ring               ring;
                for (const auto& point : boundary.points)
                    original.push_back(mm(point));
                double orientation = 0.;
                for (size_t i = 0; i < n; ++i) {
                    const Vec2d  a = original[i] - original[(i + n - 1) % n], b = original[(i + 1) % n] - original[i];
                    const double cross = a.x() * b.y() - a.y() * b.x();
                    if (cross != 0.) {
                        if (orientation * cross < -1e-10)
                            throw Abort{FailureCode::UnsupportedTopology, "convex_boundary_contours_required"};
                        orientation = cross;
                    }
                    if (a.norm() == 0. || b.norm() == 0.)
                        throw Abort{FailureCode::InvalidInput, "duplicate_contour_vertex"};
                    const Vec2d  u = a.normalized(), v = b.normalized();
                    const double denominator = 1. + u.dot(v);
                    if (denominator < .01)
                        return false;
                    const Vec2d displacement = process.bead_width_mm * Vec2d(-u.y() - v.y(), u.x() + v.x()) / denominator;
                    inset.push_back(original[i] + displacement);
                    ring.middle.push_back(original[i] + displacement / 2.);
                }
                for (size_t i = 0; i < n; ++i) {
                    const size_t     j      = (i + 1) % n;
                    Polygon          cell   = polygon({original[i], original[j], inset[j], inset[i]});
                    const ExPolygons inside = intersection_ex(Polygons{cell}, domain);
                    if ((ring.middle[i] - ring.middle[j]).norm() <= .001 || cell.area() <= 0. ||
                        std::abs(cell.area() * SCALING_FACTOR * SCALING_FACTOR - area_mm2(inside)) >
                            tolerances.interface_mm * process.bead_width_mm)
                        return false;
                    shell_area += area_mm2(inside);
                    shell_cells.push_back(std::move(cell));
                    ring.cells.push_back(inside);
                }
                rings.push_back(std::move(ring));
            }
        }
        const ExPolygons shell = union_ex(shell_cells);
        if (shell_area - area_mm2(shell) > tolerances.interface_mm * std::max(1., extent))
            return false;
        const ExPolygons remainder = diff_ex(domain, shell);
        if (shell_area < tolerances.interface_mm || area_mm2(remainder) < tolerances.interface_mm)
            return false;
        for (const auto& ring : rings) {
            auto run = path(band, role, role == PathRole::Perimeter ? "closed_clipped_contour" : "concentric_dense_infill");
            for (size_t i = 0; i < ring.middle.size(); ++i)
                append(run, ring.middle[i], ring.middle[(i + 1) % ring.middle.size()], ring.cells[i], lower, upper, process.bead_width_mm);
            plan.paths.push_back(std::move(run));
        }
        domain = remainder;
        return true;
    }

    void band(const ClippedBand& band, const Plane& lower, const Plane& upper)
    {
        const size_t first_path = plan.paths.size();
        omitted_fill_area       = 0.;
        fill_runs               = 0;
        const ExPolygons domain = cap(band.solid, upper, origin, tolerances.interface_mm);
        if (domain.empty())
            throw Abort{FailureCode::UnsupportedTopology, "no_supported_upper_cap"};
        check(to_points(domain).size() * 200);
        double min_z = std::numeric_limits<double>::max(), max_z = -min_z;
        double ymin = min_z, ymax = max_z;
        for (const auto& point : to_points(domain)) {
            const Vec2d  p = mm(point);
            const double h = upper.at(p) - lower.at(p);
            if (h < process.min_active_thickness_mm - tolerances.interface_mm ||
                h > process.max_active_thickness_mm + tolerances.interface_mm)
                throw Abort{FailureCode::InfeasibleThickness, "clipped_active_thickness"};
            min_z = std::min(min_z, upper.at(p));
            max_z = std::max(max_z, upper.at(p));
            ymin  = std::min(ymin, p.y());
            ymax  = std::max(ymax, p.y());
        }
        for (const auto& section : machine.sections)
            if (!(machine.coplanar_tip_contact && section.bottom_mm == 0. && max_z - min_z <= 1e-8) &&
                section.radius_mm > coverage.minimum_bead_width_mm / 2. - machine.clearance_mm &&
                section.bottom_mm < max_z - min_z + machine.clearance_mm)
                throw Abort{FailureCode::ClearanceFailure, "clipped_tool_body_above_prior_band"};
        // A downward vertical ray cannot exit through a side face whose outward
        // normal has nonnegative Z. Verify EVERY actual side triangle, not just
        // cross sections. This certifies the upper-cap columns for inward tapers
        // and vertical holes without a near-coplanar Boolean explosion.
        const SolidQuery validated_source(band.solid);
        double           side_rounding_bound = 0., cap_residual = 0.;
        for (const auto& face : band.solid.triangles) {
            check();
            const Vec3d a = band.solid.vertices[face[0]], b = band.solid.vertices[face[1]], c = band.solid.vertices[face[2]];
            double      upper_error = 0., lower_error = 0.;
            for (const auto& p : {a, b, c}) {
                upper_error = std::max(upper_error, std::abs(p.z() - upper.at(p.head<2>() - origin)));
                lower_error = std::max(lower_error, std::abs(p.z() - lower.at(p.head<2>() - origin)));
            }
            if (std::min(upper_error, lower_error) <= 1e-8) {
                cap_residual = std::max(cap_residual, std::min(upper_error, lower_error));
                continue;
            }
            const Vec3d normal = (b - a).cross(c - a);
            if (normal.z() < 0.) {
                const double drift = -normal.z() / normal.head<2>().norm() * process.max_active_thickness_mm;
                if (!std::isfinite(drift) || drift > tolerances.interface_mm)
                    throw Abort{FailureCode::UnsupportedTopology, "nonmonotone_clipped_boundary"};
                side_rounding_bound += -normal.z() / 2. * process.max_active_thickness_mm;
            }
        }
        const Mesh  candidate        = prism(domain, lower, upper, origin);
        double      candidate_volume = 0., perimeter = 0.;
        const Vec3d reference = candidate.vertices.front();
        for (const auto& face : candidate.triangles)
            candidate_volume += (candidate.vertices[face[0]] - reference)
                                    .dot((candidate.vertices[face[1]] - reference).cross(candidate.vertices[face[2]] - reference)) /
                                6.;
        for (const auto& piece : domain) {
            Polygons boundaries{piece.contour};
            boundaries.insert(boundaries.end(), piece.holes.begin(), piece.holes.end());
            for (const auto& boundary : boundaries)
                for (size_t i = 0; i < boundary.points.size(); ++i)
                    perimeter += (mm(boundary.points[(i + 1) % boundary.points.size()]) - mm(boundary.points[i])).norm();
        }
        const double rounding_bound = side_rounding_bound + perimeter * .000004 * process.max_active_thickness_mm +
                                      2. * cap_residual * area_mm2(domain);
        const SolidCoverage solid_coverage{candidate_volume, std::max(0., band.volume_mm3 - candidate_volume + rounding_bound),
                                           rounding_bound};
        // Integer boundary rounding scales with contour length. Use the same
        // absolute-or-relative volume budget as the material dose checks so a
        // larger vertical box is not rejected solely for having more perimeter.
        const double excess_budget = std::max(tolerances.absolute_volume_mm3, tolerances.relative_volume * band.volume_mm3);
        if (solid_coverage.excess_mm3 > excess_budget)
            throw Abort{FailureCode::UnsupportedTopology, "bead_columns_leave_actual_solid"};
        ExPolygons interior = domain;
        for (size_t wall = 0; wall < coverage.perimeter_loops; ++wall)
            if (!ring(interior, band, lower, upper, PathRole::Perimeter))
                throw Abort{FailureCode::InfeasibleThickness, "perimeter_bead_does_not_fit"};
        if (coverage.infill.pattern == DenseInfillPattern::Concentric)
            while (ring(interior, band, lower, upper, PathRole::DenseFill))
                check();
        double angle = coverage.infill.angle_degrees;
        if (coverage.infill.alternate_direction && band.id.macro_cycle % 2)
            angle += 90.;
        const double radians = std::remainder(angle, 180.) * PI / 180.;
        fill_rotation        = Eigen::Rotation2Dd(radians).toRotationMatrix();
        interior             = rotate_fill(std::move(interior), true);
        ymin                 = std::numeric_limits<double>::max();
        ymax                 = -ymin;
        for (const auto& point : to_points(interior)) {
            ymin = std::min(ymin, mm(point).y());
            ymax = std::max(ymax, mm(point).y());
        }
        const double rows = std::ceil((ymax - ymin) / process.bead_width_mm);
        if (!std::isfinite(rows) || rows > double(limits.max_paths) || rows > 1000000.)
            throw Abort{FailureCode::ResourceLimit, "coverage_lane_limit"};
        const double pitch = (ymax - ymin) / rows;
        for (size_t row = 0; row < size_t(rows); ++row)
            fill(interior, ymin + row * pitch, ymin + (row + 1) * pitch, band, lower, upper, 0);
        if (omitted_fill_area > 1e-10)
            close_infill_gaps(domain, first_path, band, lower, upper);
        const double       deposited = plan.volume_mm3.at(band.id.stack_slot);
        const double       omitted   = solid_coverage.candidate_mm3 - deposited;
        CoverageDiagnostic diagnostic{band.volume_mm3, deposited, solid_coverage.missing_mm3 + std::max(0., omitted),
                                      solid_coverage.excess_mm3 + std::max(0., -omitted)};
        if (diagnostic.excess_mm3 > excess_budget ||
            diagnostic.missing_mm3 > std::max(tolerances.absolute_volume_mm3, coverage.relative_missing_volume * band.volume_mm3)) {
            throw Abort{FailureCode::MaterialToleranceExceeded, "clipped_path_coverage_budget"};
        }
        plan.coverage.at(band.id.stack_slot) = diagnostic;
    }
};
} // namespace

BoxPlanResult plan_clipped_cycle(const MaterialBandComplex&   bands,
                                 const ProcessConstraints&    process,
                                 const NumericTolerances&     tolerances,
                                 const MachineEnvelope&       machine,
                                 const PathLimits&            limits,
                                 const CoverageLimits&        coverage,
                                 const std::function<bool()>& cancelled,
                                 const std::function<bool()>& stale)
{
    const auto fail = [&](FailureCode code, const std::string& detail) -> BoxPlanResult {
        return Failure{bands.revision, code, detail, {}, {}, {}};
    };
    try {
        validate_constraints(process, tolerances, machine, coverage);
        Builder builder{process, tolerances, machine, limits, coverage, cancelled, stale};
        builder.check();
        auto& plan     = builder.plan;
        plan.revision  = bands.revision;
        plan.machine   = machine;
        plan.solid_min = Vec3d::Constant(std::numeric_limits<double>::max());
        plan.solid_max = -plan.solid_min;
        for (size_t slot = 0; slot < 2; ++slot) {
            const auto& band = bands.bands[slot];
            if (band.id.stack_slot != slot || band.id.material.value < 1 || band.id.material.value > 2 ||
                band.shared_interface != bands.shared_interface.id || band.id.domain != bands.shared_interface.domain ||
                band.id.macro_cycle != bands.shared_interface.macro_cycle || band.solid.vertices.empty() || band.solid.triangles.empty() ||
                !std::isfinite(band.volume_mm3) || band.volume_mm3 <= 0.)
                return fail(FailureCode::InvalidInput, "clipped_band_identity");
            builder.check(band.solid.vertices.size() * sizeof(Vec3d) + band.solid.triangles.size() * 200);
            for (const auto& point : band.solid.vertices) {
                if (!point.allFinite())
                    return fail(FailureCode::InvalidInput, "nonfinite_clipped_solid");
                plan.solid_min = plan.solid_min.cwiseMin(point);
                plan.solid_max = plan.solid_max.cwiseMax(point);
            }
        }
        if (bands.bands[0].id.material.value == bands.bands[1].id.material.value)
            return fail(FailureCode::IncompatibleOrder, "duplicate_physical_material");
        builder.origin = plan.solid_min.head<2>();
        // The scan frame rotates independently of the model. Its coordinates
        // can reach the bounding-box diagonal, not just its longest side.
        builder.extent = (plan.solid_max - plan.solid_min).head<2>().norm() + 2. * process.bead_width_mm + 1.;
        if (!std::isfinite(builder.extent) || builder.extent > 100000.)
            return fail(FailureCode::ResourceLimit, "clipped_coordinate_range");
        const Plane shared = affine(bands.shared_interface.surface, builder.origin, tolerances.interface_mm);
        if (shared.gradient.norm() > process.max_interface_gradient + tolerances.interface_mm)
            return fail(FailureCode::InfeasibleThickness, "clipped_interface_gradient");
        plan.interfaces.push_back(bands.shared_interface);
        builder.band(bands.bands[0], Plane{Vec2d::Zero(), Vec2d::Zero(), plan.solid_min.z()}, shared);
        builder.band(bands.bands[1], shared, Plane{Vec2d::Zero(), Vec2d::Zero(), plan.solid_max.z()});
        builder.check();
        return std::make_shared<const PathPlan>(std::move(plan));
    } catch (const Abort& abort) {
        return fail(abort.code, abort.detail);
    } catch (const std::bad_alloc&) {
        return fail(FailureCode::ResourceLimit, "clipped_paths_allocation");
    } catch (const std::exception&) {
        return fail(FailureCode::UnsupportedTopology, "clipped_path_geometry_failure");
    }
}

BoxPlanResult plan_pure_cycle(const ClippedBand&           band,
                              const RevisionKey&           revision,
                              const ProcessConstraints&    process,
                              const NumericTolerances&     tolerances,
                              const MachineEnvelope&       machine,
                              const PathLimits&            limits,
                              const CoverageLimits&        coverage,
                              const std::function<bool()>& cancelled,
                              const std::function<bool()>& stale)
{
    const auto fail = [&](FailureCode code, const std::string& detail) -> BoxPlanResult {
        return Failure{revision, code, detail, {}, {}, {}};
    };
    try {
        validate_constraints(process, tolerances, machine, coverage);
        if (band.shared_interface != 0 || band.id.material.value < 1 || band.id.material.value > machine.material_count ||
            band.id.stack_slot >= machine.material_count || band.solid.vertices.empty() || band.solid.triangles.empty() ||
            !std::isfinite(band.volume_mm3) || band.volume_mm3 <= 0.)
            return fail(FailureCode::InvalidInput, "pure_band_identity_or_geometry");
        Builder builder{process, tolerances, machine, limits, coverage, cancelled, stale};
        builder.check(band.solid.vertices.size() * sizeof(Vec3d) + band.solid.triangles.size() * 200);
        auto& plan     = builder.plan;
        plan.revision  = revision;
        plan.machine   = machine;
        plan.solid_min = plan.solid_max = band.solid.vertices.front();
        for (const auto& point : band.solid.vertices) {
            if (!point.allFinite())
                return fail(FailureCode::InvalidInput, "nonfinite_pure_solid");
            plan.solid_min = plan.solid_min.cwiseMin(point);
            plan.solid_max = plan.solid_max.cwiseMax(point);
        }
        builder.origin = plan.solid_min.head<2>();
        builder.extent = (plan.solid_max - plan.solid_min).head<2>().norm() + 2. * process.bead_width_mm + 1.;
        if (!std::isfinite(builder.extent) || builder.extent > 100000.)
            return fail(FailureCode::ResourceLimit, "clipped_coordinate_range");
        builder.band(band, Plane{Vec2d::Zero(), Vec2d::Zero(), plan.solid_min.z()}, Plane{Vec2d::Zero(), Vec2d::Zero(), plan.solid_max.z()});
        builder.check();
        return std::make_shared<const PathPlan>(std::move(plan));
    } catch (const Abort& abort) {
        return fail(abort.code, abort.detail);
    } catch (const std::bad_alloc&) {
        return fail(FailureCode::ResourceLimit, "pure_paths_allocation");
    } catch (const std::exception&) {
        return fail(FailureCode::UnsupportedTopology, "pure_path_geometry_failure");
    }
}
size_t anchor_perimeter_seams(std::vector<ContourRun>& runs, const ExPolygons& preceding_deposition, const std::function<void()>& poll)
{
    if (preceding_deposition.empty())
        return 0;
    size_t changed = 0;
    for (auto& run : runs) {
        if (poll)
            poll();
        auto& points = run.line.points;
        if (run.role != PathRole::Perimeter || points.size() < 4 || points.front() != points.back() || !std::isfinite(run.width_mm) ||
            run.width_mm <= 0.)
            continue;
        const size_t count = points.size() - 1;
        BoundingBox  bounds(points);
        bounds.offset(scale_(run.width_mm));
        const auto floor = intersection_ex(preceding_deposition, Polygons{bounds.polygon()});
        if (floor.empty())
            continue;
        const auto contact = [&](size_t vertex, size_t adjacent) {
            const Vec2d center = mm(points[vertex]);
            const Vec2d delta  = mm(points[adjacent]) - center;
            if (delta.squaredNorm() < 1e-20)
                return 0.;
            const Vec2d along = delta.normalized() * (run.width_mm / 2.);
            const Vec2d across(-along.y(), along.x());
            return area_mm2(intersection_ex(Polygons{polygon({center - along - across, center + along - across, center + along + across,
                                                              center - along + across})},
                                            floor));
        };
        const auto contact_score = [&](size_t first) {
            const size_t previous = (first + count - 1) % count;
            // The final short closing segment can be wholly owned by the
            // first cap. Its preceding endpoint must also remain grounded.
            double score = std::min(contact(first, (first + 1) % count), contact(first, previous));
            if ((mm(points[first]) - mm(points[previous])).norm() <= run.width_mm)
                score = std::min(score, contact(previous, (previous + count - 1) % count));
            return score;
        };
        size_t       best         = 0;
        double       best_contact = contact_score(0);
        const double preferred    = .75 * run.width_mm * run.width_mm;
        if (best_contact >= preferred)
            continue;
        for (size_t first = 1; first < count; ++first) {
            if (poll)
                poll();
            const double score = contact_score(first);
            if (score > best_contact + 8. * run.width_mm * SCALING_FACTOR) {
                best         = first;
                best_contact = score;
            }
            if (best_contact >= preferred)
                break;
        }
        // Prefer generous contact, but a narrow supported arc can still meet
        // the scheduler's unchanged half-cap anchoring contract.
        if (best != 0 && best_contact >= .5 * run.width_mm * run.width_mm) {
            points.pop_back();
            std::rotate(points.begin(), points.begin() + best, points.end());
            points.push_back(points.front());
            ++changed;
        }
    }
    return changed;
}

BoxPlanResult fit_planar_overhang_caps(const PathPlan&              geometry,
                                       const ExPolygons&            preceding_deposition,
                                       double                       contact_z,
                                       const CoverageLimits&        coverage,
                                       const PathLimits&            limits,
                                       const std::function<bool()>& cancelled,
                                       const std::function<bool()>& stale)
{
    try {
        auto plan = std::make_shared<PathPlan>(geometry);
        if (preceding_deposition.empty())
            return std::shared_ptr<const PathPlan>(std::move(plan));
        size_t                       retained = sizeof(PathPlan);
        std::map<double, ExPolygons> support_cache;
        using Tile = std::tuple<double, int64_t, int64_t, int64_t, int64_t>;
        std::map<Tile, ExPolygons> local_support_cache;
        for (auto& path : plan->paths) {
            if (path.overhang_mm <= 0.)
                continue;
            auto found = support_cache.find(path.overhang_mm);
            if (found == support_cache.end()) {
                auto support = offset_ex(preceding_deposition, float(scale_(path.overhang_mm)));
                support      = union_ex(preceding_deposition, support);
                found        = support_cache.emplace(path.overhang_mm, std::move(support)).first;
            }
            const auto& layer_support = found->second;
            for (auto& segment : path.segments) {
                if (cancelled && cancelled())
                    throw Abort{FailureCode::Cancelled, "cap_fit_cancelled"};
                if (stale && stale())
                    throw Abort{FailureCode::StaleInput, "cap_fit_stale"};
                retained += sizeof(PathSegment) + segment.cells.size() * (sizeof(BeadCell) + 6 * sizeof(Point));
                if (retained > limits.max_temporary_bytes)
                    throw Abort{FailureCode::ResourceLimit, "cap_fit_memory_limit"};
                if (segment.cells.empty())
                    continue;
                const double bottom = segment.cells.front().lower_z.front(), top = segment.cells.front().upper.front().z();
                if (std::abs(bottom - contact_z) > 1e-10)
                    continue;
                bool     planar = true;
                Polygons pieces;
                for (const auto& cell : segment.cells) {
                    Points points;
                    for (size_t i = 0; i < 3; ++i) {
                        planar &= std::abs(cell.lower_z[i] - bottom) < 1e-10 && std::abs(cell.upper[i].z() - top) < 1e-10;
                        points.push_back(scaled(cell.upper[i].head<2>()));
                    }
                    pieces.emplace_back(std::move(points));
                }
                if (!planar)
                    continue;
                const auto footprint = union_ex(pieces);
                if (footprint.empty())
                    continue;
                const BoundingBox bounds(to_points(footprint));
                constexpr double  tile_mm = 8.;
                const int64_t     x0      = int64_t(std::floor(unscale<double>(bounds.min.x()) / tile_mm));
                const int64_t     y0      = int64_t(std::floor(unscale<double>(bounds.min.y()) / tile_mm));
                const int64_t     x1      = int64_t(std::floor(unscale<double>(bounds.max.x()) / tile_mm));
                const int64_t     y1      = int64_t(std::floor(unscale<double>(bounds.max.y()) / tile_mm));
                const Tile        tile{path.overhang_mm, x0, y0, x1, y1};
                auto              local = local_support_cache.find(tile);
                if (local == local_support_cache.end()) {
                    // Boolean queries need only this segment's tiles. Include
                    // a full bead margin so the later cap-depth check cannot
                    // mistake a tile boundary for a physical support boundary.
                    const double margin = 2. * path.overhang_mm;
                    auto         region = intersection_ex(layer_support,
                                                          Polygons{polygon({Vec2d(x0 * tile_mm - margin, y0 * tile_mm - margin),
                                                                            Vec2d((x1 + 1) * tile_mm + margin, y0 * tile_mm - margin),
                                                                            Vec2d((x1 + 1) * tile_mm + margin, (y1 + 1) * tile_mm + margin),
                                                                            Vec2d(x0 * tile_mm - margin, (y1 + 1) * tile_mm + margin)})});
                    retained += to_points(region).size() * sizeof(Point) * 4 + region.size() * sizeof(ExPolygon) * 4 + 128;
                    if (retained > limits.max_temporary_bytes)
                        throw Abort{FailureCode::ResourceLimit, "cap_fit_support_cache_limit"};
                    local = local_support_cache.emplace(tile, std::move(region)).first;
                }
                const auto&  support      = local->second;
                const auto   excess       = diff_ex(footprint, support);
                const double removed_area = area_mm2(excess);
                if (removed_area <= 1e-12 || removed_area > coverage.relative_missing_volume * area_mm2(footprint))
                    continue;
                // Keep the fitted edge within one quarter of this bead's width,
                // as well as the independent one-percent segment-volume bound.
                // This cannot trim away a bridge or a floating island.
                if (!diff_ex(excess, offset_ex(support, float(scale_(segment.width_mm / 4.)))).empty())
                    continue;
                const auto fitted  = intersection_ex(footprint, support);
                auto       rebuilt = triangles(fitted, true);
                if (rebuilt.empty())
                    continue;
                std::vector<BeadCell> cells;
                double                volume = 0.;
                for (const auto& tri : rebuilt) {
                    const Vec2d a = tri[1] - tri[0], b = tri[2] - tri[0];
                    volume += .5 * (a.x() * b.y() - a.y() * b.x()) * (top - bottom);
                    BeadCell cell;
                    for (size_t i = 0; i < 3; ++i) {
                        cell.upper[i]   = Vec3d(tri[i].x(), tri[i].y(), top);
                        cell.lower_z[i] = bottom;
                    }
                    cells.push_back(std::move(cell));
                }
                const double removed = segment.volume_mm3 - volume;
                if (removed <= 0. || removed > coverage.relative_missing_volume * segment.volume_mm3)
                    continue;
                segment.volume_mm3         = volume;
                segment.cells              = std::move(cells);
                segment.coverage_triangles = std::move(rebuilt);
                segment.footprint.clear();
                plan->volume_mm3[path.band.stack_slot] -= removed;
                auto& diagnostic = plan->coverage[path.band.stack_slot];
                diagnostic.deposited_mm3 -= removed;
                diagnostic.missing_mm3 += removed;
            }
        }
        return std::shared_ptr<const PathPlan>(std::move(plan));
    } catch (const Abort& error) {
        return Failure{geometry.revision, error.code, error.detail, {}, {}, {}};
    } catch (const std::bad_alloc&) {
        return Failure{geometry.revision, FailureCode::ResourceLimit, "cap_fit_allocation", {}, {}, {}};
    }
}

BoxPlanResult plan_contour_cycle(const ExPolygons&              outline,
                                 const std::vector<ContourRun>& runs,
                                 const BandId&                  id,
                                 double                         bottom,
                                 double                         top,
                                 const RevisionKey&             revision,
                                 const ProcessConstraints&      process,
                                 const NumericTolerances&       tolerances,
                                 const MachineEnvelope&         machine,
                                 const PathLimits&              limits,
                                 const std::function<bool()>&   cancelled,
                                 const std::function<bool()>&   stale,
                                 const ExPolygons&              required_solid)
{
    try {
        CoverageLimits coverage;
        coverage.max_candidate_cells   = limits.max_segments * 4;
        coverage.minimum_bead_width_mm = .04;
        validate_constraints(process, tolerances, machine, coverage);
        if (id.domain == 0 || id.material.value < 1 || id.material.value > machine.material_count ||
            id.stack_slot != id.material.value - 1 || !std::isfinite(bottom) || !std::isfinite(top))
            throw Abort{FailureCode::InvalidInput, "invalid_contour_layer_identity"};
        Builder builder{process, tolerances, machine, limits, coverage, cancelled, stale};
        builder.general_coverage = true;
        auto& plan               = builder.plan;
        plan.revision            = revision;
        plan.machine             = machine;
        plan.layered_process     = true;
        if (outline.empty() || top <= bottom)
            throw Abort{FailureCode::InvalidInput, "empty_contour_layer"};
        const BoundingBox bounds(to_points(outline));
        builder.origin           = mm(bounds.min);
        builder.extent           = (mm(bounds.max) - builder.origin).norm() + 2. * process.bead_width_mm + 1.;
        plan.solid_min           = Vec3d(builder.origin.x(), builder.origin.y(), bottom);
        plan.solid_max           = Vec3d(mm(bounds.max).x(), mm(bounds.max).y(), top);
        ExPolygons local_outline = outline;
        for (auto& p : local_outline) {
            for (auto& q : p.contour.points)
                q -= bounds.min;
            for (auto& h : p.holes)
                for (auto& q : h.points)
                    q -= bounds.min;
        }
        ExPolygons                                         occupied;
        std::vector<ExPolygons>                            occupied_parts;
        std::map<std::pair<int, int>, std::vector<size_t>> occupied_grid;
        size_t                                             grid_references = 0;
        ClippedBand                                        band;
        band.id = id;
        const Plane                             lower{Vec2d::Zero(), Vec2d::Zero(), bottom}, upper{Vec2d::Zero(), Vec2d::Zero(), top};
        std::map<const ExPolygons*, ExPolygons> fill_regions;
        for (const auto& source : runs) {
            builder.check();
            if (!std::isfinite(source.width_mm) || source.width_mm <= 0. || source.width_mm > process.bead_width_mm + 1e-9)
                throw Abort{FailureCode::InvalidInput, "invalid_contour_width"};
            BandPath          run     = builder.path(band, source.role, "owned_planar_process");
            const ExPolygons* allowed = &local_outline;
            if (source.coverage) {
                auto found = fill_regions.find(source.coverage.get());
                if (found == fill_regions.end()) {
                    auto region = *source.coverage;
                    for (auto& component : region) {
                        for (auto& point : component.contour.points)
                            point -= bounds.min;
                        for (auto& hole : component.holes)
                            for (auto& point : hole.points)
                                point -= bounds.min;
                    }
                    region = intersection_ex(region, local_outline);
                    builder.check(to_points(region).size() * sizeof(Point) * 4 + region.size() * sizeof(ExPolygon) * 4);
                    found = fill_regions.emplace(source.coverage.get(), std::move(region)).first;
                }
                allowed = &found->second;
            }
            for (size_t i = 1; i < source.line.points.size(); ++i) {
                const Vec2d a = mm(source.line.points[i - 1] - bounds.min), b = mm(source.line.points[i] - bounds.min);
                if ((b - a).norm() < .003)
                    continue;
                const auto stroke = intersection_ex(offset(Polyline{Points{scaled(a), scaled(b)}}, float(scale_(source.width_mm / 2.)),
                                                           ClipperLib::jtMiter, 3., ClipperLib::etOpenSquare),
                                                    *allowed);
                if (stroke.empty())
                    continue;
                const BoundingBox box(to_points(stroke));
                constexpr double  grid_mm = 2.;
                const Vec2d       lo = mm(box.min), hi = mm(box.max);
                const int         x0 = int(std::floor(lo.x() / grid_mm)), x1 = int(std::floor(hi.x() / grid_mm));
                const int         y0 = int(std::floor(lo.y() / grid_mm)), y1 = int(std::floor(hi.y() / grid_mm));
                std::set<size_t>  nearby;
                for (int x = x0; x <= x1; ++x)
                    for (int y = y0; y <= y1; ++y) {
                        const auto found = occupied_grid.find({x, y});
                        if (found != occupied_grid.end())
                            nearby.insert(found->second.begin(), found->second.end());
                    }
                ExPolygons previous;
                for (size_t index : nearby)
                    previous.insert(previous.end(), occupied_parts[index].begin(), occupied_parts[index].end());
                auto available = diff_ex(stroke, union_ex(previous));
                if (area_mm2(available) * (top - bottom) < 3e-5)
                    continue;
                // The fill generator may deliberately extend into a wall.
                // Deposit only the remaining centerline intervals, rather than
                // spreading their dose over a long already-occupied prefix.
                const Vec2d                            direction = (b - a).normalized(), normal(-direction.y(), direction.x());
                std::vector<std::pair<double, double>> intervals;
                for (const auto& component : available) {
                    double first = (b - a).norm(), last = 0.;
                    for (const auto& point : component.contour.points) {
                        const double along = direction.dot(mm(point) - a);
                        first              = std::min(first, along);
                        last               = std::max(last, along);
                    }
                    intervals.emplace_back(std::max(0., first), std::min((b - a).norm(), last));
                }
                std::sort(intervals.begin(), intervals.end());
                // A lane beside a wall may have its center inside the wall,
                // while the remaining half of its bead still fills a skin.
                // Project that footprint along the motion instead of requiring
                // the center itself to intersect the unoccupied footprint.
                std::vector<std::pair<double, double>> merged;
                for (const auto& interval : intervals) {
                    if (!merged.empty() && interval.first <= merged.back().second + SCALING_FACTOR)
                        merged.back().second = std::max(merged.back().second, interval.second);
                    else
                        merged.push_back(interval);
                }
                intervals = std::move(merged);
                Polygons deposited;
                // Keep a connected source run continuous across the caps
                // already owned by its preceding segments. Only the external
                // ends of open infill runs may drop an occupied prefix/suffix.
                const bool trim_motion = (source.role != PathRole::Perimeter && source.line.points.front() != source.line.points.back() &&
                                          !intervals.empty() &&
                                          ((run.vertices.empty() && intervals.front().first > .001) ||
                                           (i + 1 == source.line.points.size() && intervals.back().second < (b - a).norm() - .001))) ||
                                         (source.coverage && !diff_pl(Polylines{Polyline{Points{scaled(a), scaled(b)}}}, *allowed).empty());
                for (size_t attempt = trim_motion ? 1 : 0; attempt <= intervals.size(); ++attempt) {
                    const size_t k        = attempt == 0 ? 0 : attempt - 1;
                    const auto   interval = attempt == 0 ? std::make_pair(0., (b - a).norm()) : intervals[k];
                    if (interval.second - interval.first < .003)
                        continue;
                    const double lo   = attempt == 0 || k == 0 ? -builder.extent : (intervals[k - 1].second + interval.first) / 2.;
                    const double hi   = attempt == 0 || k + 1 == intervals.size() ? builder.extent :
                                                                                    (interval.second + intervals[k + 1].first) / 2.;
                    const Vec2d  left = a + direction * lo, right = a + direction * hi;
                    const auto   piece = intersection_ex(available,
                                                         Polygons{polygon({left - normal * builder.extent, right - normal * builder.extent,
                                                                           right + normal * builder.extent, left + normal * builder.extent})});
                    if (area_mm2(piece) * (top - bottom) < 3e-5)
                        continue;
                    auto         candidate       = builder.path(band, source.role, "owned_planar_process");
                    const double previous_volume = plan.volume_mm3[id.stack_slot];
                    try {
                        builder.append(candidate, a + direction * interval.first, a + direction * interval.second, piece, lower, upper,
                                       source.width_mm);
                    } catch (const Abort& error) {
                        if (error.code != FailureCode::InfeasibleThickness ||
                            (error.detail != "coverage_dose_below_output_precision" &&
                             error.detail != "coverage_segment_collapses_at_output_precision" && error.detail != "empty_coverage_segment"))
                            throw;
                        plan.volume_mm3[id.stack_slot] = previous_volume;
                        continue;
                    }
                    for (const auto& segment : candidate.segments)
                        for (const auto& triangle : segment.coverage_triangles)
                            deposited.push_back(
                                polygon({triangle[0] - builder.origin, triangle[1] - builder.origin, triangle[2] - builder.origin}));
                    if (!run.vertices.empty() && (run.vertices.back() - candidate.vertices.front()).norm() > 1e-8) {
                        plan.paths.push_back(std::move(run));
                        run = builder.path(band, source.role, "planar_coverage_split");
                    }
                    if (run.vertices.empty())
                        run.vertices.push_back(candidate.vertices.front());
                    run.vertices.insert(run.vertices.end(), candidate.vertices.begin() + 1, candidate.vertices.end());
                    run.segments.insert(run.segments.end(), std::make_move_iterator(candidate.segments.begin()),
                                        std::make_move_iterator(candidate.segments.end()));
                    if (attempt == 0)
                        break;
                }
                if (deposited.empty())
                    continue;
                available     = union_ex(deposited);
                size_t points = 0;
                for (const auto& polygon : available) {
                    points += polygon.contour.points.size();
                    for (const auto& hole : polygon.holes)
                        points += hole.points.size();
                }
                builder.check(2 * points * sizeof(Point) + available.size() * 2 * sizeof(ExPolygon));
                for (int x = x0; x <= x1; ++x)
                    for (int y = y0; y <= y1; ++y) {
                        if (++grid_references > limits.max_segments * 32)
                            throw Abort{FailureCode::ResourceLimit, "contour_occupancy_index_limit"};
                        occupied_grid[{x, y}].push_back(occupied_parts.size());
                    }
                builder.check(size_t(x1 - x0 + 1) * size_t(y1 - y0 + 1) * 64);
                occupied.insert(occupied.end(), available.begin(), available.end());
                occupied_parts.push_back(std::move(available));
            }
            if (!run.segments.empty())
                plan.paths.push_back(std::move(run));
        }
        if (!required_solid.empty()) {
            auto required = required_solid;
            for (auto& component : required) {
                for (auto& point : component.contour.points)
                    point -= bounds.min;
                for (auto& hole : component.holes)
                    for (auto& point : hole.points)
                        point -= bounds.min;
            }
            required = intersection_ex(required, local_outline);
            builder.close_infill_gaps(required, 0, band, lower, upper, true);
            const auto deposited = [&] {
                Polygons pieces;
                for (const auto& path : plan.paths)
                    for (const auto& segment : path.segments)
                        for (const auto& triangle : segment.coverage_triangles)
                            pieces.push_back(
                                polygon({triangle[0] - builder.origin, triangle[1] - builder.origin, triangle[2] - builder.origin}));
                return union_ex(pieces);
            };
            const auto missing = diff_ex(required, deposited());
            if (!missing.empty()) {
                const BoundingBox missing_bounds(to_points(missing));
                for (double y = mm(missing_bounds.min).y(); y < mm(missing_bounds.max).y(); y += process.bead_width_mm)
                    builder.fill(missing, y, std::min(y + process.bead_width_mm, mm(missing_bounds.max).y()), band, lower, upper, 0);
                builder.close_infill_gaps(required, 0, band, lower, upper, true);
            }
            occupied = deposited();
        } else
            occupied = union_ex(occupied);
        for (auto& path : plan.paths)
            for (auto& segment : path.segments) {
                builder.check();
                const double previous_volume = segment.volume_mm3;
                compact_planar_segment(segment, lower, upper, builder.origin);
                plan.volume_mm3[id.stack_slot] += segment.volume_mm3 - previous_volume;
            }
        plan.coverage[id.stack_slot].target_mm3    = area_mm2(occupied) * (top - bottom);
        plan.coverage[id.stack_slot].deposited_mm3 = plan.volume_mm3[id.stack_slot];
        plan.intentional_void_mm3                  = std::max(0., (area_mm2(local_outline) - area_mm2(occupied)) * (top - bottom));
        // Solid gap repair is generated after the source runs, but supplies
        // real side anchors at sparse-fill endpoints. Deposit those solid
        // lanes first, preserving the relative order of all other runs.
        std::stable_sort(plan.paths.begin(), plan.paths.end(), [](const BandPath& a, const BandPath& b) {
            return (a.role == PathRole::SparseFill) < (b.role == PathRole::SparseFill);
        });
        for (size_t i = 0; i < plan.paths.size(); ++i)
            plan.paths[i].id = i + 1;
        builder.check();
        return std::make_shared<const PathPlan>(std::move(plan));
    } catch (const Abort& error) {
        return Failure{revision, error.code, error.detail, id, {}, {}};
    } catch (const std::bad_alloc&) {
        return Failure{revision, FailureCode::ResourceLimit, "contour_geometry_allocation", id, {}, {}};
    } catch (const std::exception& error) {
        return Failure{revision, FailureCode::InvalidInput, std::string("contour_geometry:") + error.what(), id, {}, {}};
    }
}

BoxPlanResult plan_partitioned_cycle(const PathPlan&                        geometry,
                                     const std::vector<PlanarMaterialBand>& bands,
                                     const ProcessConstraints&              process,
                                     const NumericTolerances&               tolerances,
                                     const PathLimits&                      limits,
                                     const CoverageLimits&                  coverage,
                                     const std::function<bool()>&           cancelled,
                                     const std::function<bool()>&           stale,
                                     const ExPolygons&                      preceding_deposition,
                                     double                                 contact_z,
                                     double                                 common_foundation_mm)
{
    std::optional<BandId> active_band;
    try {
        validate_constraints(process, tolerances, geometry.machine, coverage);
        if (!std::isfinite(common_foundation_mm) || common_foundation_mm < 0.)
            throw Abort{FailureCode::InvalidInput, "invalid_common_foundation_height"};
        Builder builder{process, tolerances, geometry.machine, limits, coverage, cancelled, stale};
        builder.origin            = geometry.solid_min.head<2>();
        builder.extent            = (geometry.solid_max - geometry.solid_min).head<2>().norm() + process.bead_width_mm * 2. + 1.;
        auto& plan                = builder.plan;
        plan.revision             = geometry.revision;
        plan.machine              = geometry.machine;
        plan.solid_min            = geometry.solid_min;
        plan.solid_max            = geometry.solid_max;
        plan.ordered_region_paths = true;
        plan.layered_process      = geometry.layered_process;
        plan.intentional_void_mm3 = geometry.intentional_void_mm3;
        builder.general_coverage  = true;
        const auto local          = [&](const ExPolygons& source) {
            ExPolygons  result = source;
            const Point shift  = scaled(builder.origin);
            for (auto& p : result) {
                for (auto& q : p.contour.points)
                    q -= shift;
                for (auto& h : p.holes)
                    for (auto& q : h.points)
                        q -= shift;
            }
            return result;
        };
        struct SourceSegment
        {
            Vec2d              a, b;
            ExPolygons         footprint;
            double             width;
            PathRole           role;
            const PathSegment* segment;
        };
        std::vector<SourceSegment> source_segments;
        Polygons                   source_polygons;
        for (const auto& path : geometry.paths) {
            for (size_t i = 0; i < path.segments.size(); ++i) {
                Polygons footprint;
                for (const auto& tri : path.segments[i].coverage_triangles)
                    footprint.push_back(polygon({tri[0] - builder.origin, tri[1] - builder.origin, tri[2] - builder.origin}));
                auto merged = union_ex(footprint);
                auto polys  = to_polygons(merged);
                source_polygons.insert(source_polygons.end(), polys.begin(), polys.end());
                source_segments.push_back({path.vertices[i].head<2>() - builder.origin, path.vertices[i + 1].head<2>() - builder.origin,
                                           std::move(merged), path.segments[i].width_mm, path.role, &path.segments[i]});
            }
        }
        const auto                      all_source = union_ex(source_polygons);
        std::vector<PlanarMaterialBand> phases;
        DomainId                        foundation_domain   = 0;
        uint32_t                        foundation_material = 0;
        if (common_foundation_mm > 0.) {
            if (!geometry.layered_process || common_foundation_mm < process.min_active_thickness_mm - tolerances.interface_mm ||
                common_foundation_mm > process.max_active_thickness_mm + tolerances.interface_mm || bands.empty())
                throw Abort{FailureCode::InfeasibleThickness, "invalid_common_bridge_foundation"};
            foundation_domain = (bands.front().id.domain / 512) * 512 + 510;
            ExPolygons foundation_area;
            for (const auto& part : bands) {
                if (part.id.domain == foundation_domain)
                    throw Abort{FailureCode::InvalidInput, "reserved_foundation_domain"};
                if (std::abs(part.bottom_mm - geometry.solid_min.z()) > tolerances.interface_mm)
                    continue;
                if (foundation_material && foundation_material != part.id.material.value)
                    throw Abort{FailureCode::IncompatibleOrder, "bridge_foundation_requires_common_first_material"};
                foundation_material = part.id.material.value;
                foundation_area.insert(foundation_area.end(), part.footprint.begin(), part.footprint.end());
            }
            if (!foundation_material)
                throw Abort{FailureCode::InvalidInput, "missing_bridge_foundation_regions"};
            phases.push_back({{foundation_domain, bands.front().id.macro_cycle, {foundation_material}, foundation_material - 1},
                              union_ex(foundation_area),
                              geometry.solid_min.z(),
                              geometry.solid_min.z() + common_foundation_mm,
                              0,
                              0});
        }
        // A shared first-material pass preserves continuous bridge lanes across
        // paint provenance boundaries. Subsequent passes retain each region's
        // exact material volume. Splits never lower the printable thickness.
        for (auto part : bands) {
            if (common_foundation_mm > 0. && std::abs(part.bottom_mm - geometry.solid_min.z()) <= tolerances.interface_mm)
                part.bottom_mm += common_foundation_mm;
            const double height = part.top_mm - part.bottom_mm;
            if (!std::isfinite(height) || height < 0.)
                throw Abort{FailureCode::InfeasibleThickness, "bridge_foundation_remainder_too_thin"};
            if (common_foundation_mm > 0. && std::abs(height) <= tolerances.interface_mm) {
                // The common pass may consume a region's complete first
                // material. Retain its shared interface with the next material
                // even though it no longer needs a separate extrusion run.
                if (part.upper_interface) {
                    InterfacePatch patch{part.upper_interface, part.id.domain, part.id.macro_cycle, {}};
                    for (const auto& tri : triangles(local(part.footprint), true)) {
                        builder.check(3 * sizeof(Vec3d) + sizeof(std::array<uint32_t, 3>));
                        const uint32_t index = uint32_t(patch.surface.vertices.size());
                        for (const auto& p : tri)
                            patch.surface.vertices.emplace_back(p.x() + builder.origin.x(), p.y() + builder.origin.y(), part.top_mm);
                        patch.surface.triangles.push_back({index, index + 1, index + 2});
                    }
                    plan.interfaces.push_back(std::move(patch));
                }
                continue;
            }
            const size_t count = common_foundation_mm > 0. ?
                                     size_t(std::max(1., std::ceil(height / process.max_active_thickness_mm - 1e-9))) :
                                     1;
            if (common_foundation_mm > 0. && count > 3)
                throw Abort{FailureCode::ResourceLimit, "bridge_material_pass_limit"};
            if (count > limits.max_paths - std::min(phases.size(), limits.max_paths))
                throw Abort{FailureCode::ResourceLimit, "painted_phase_count_limit"};
            if (height / count < process.min_active_thickness_mm - tolerances.interface_mm)
                throw Abort{FailureCode::InfeasibleThickness, "bridge_foundation_remainder_too_thin"};
            for (size_t k = 0; k < count; ++k) {
                auto pass            = part;
                pass.bottom_mm       = part.bottom_mm + height * k / count;
                pass.top_mm          = k + 1 == count ? part.top_mm : part.bottom_mm + height * (k + 1) / count;
                pass.lower_interface = k == 0 ? part.lower_interface : 0;
                pass.upper_interface = k + 1 == count ? part.upper_interface : 0;
                builder.check(sizeof(PlanarMaterialBand) + to_points(pass.footprint).size() * sizeof(Point) * 2);
                phases.push_back(std::move(pass));
            }
        }
        using DomainCycle = std::pair<DomainId, uint64_t>;
        std::map<DomainCycle, MaterialFractions> domain_targets, domain_deposited;
        for (const auto& part : phases) {
            active_band = part.id;
            builder.check();
            if (const char* capture = std::getenv("MATERIAL_BAND_CAPTURE_PARTITION"))
                if (std::string(capture) == std::to_string(part.id.macro_cycle) + ":" + std::to_string(part.id.domain) + ":" +
                                                std::to_string(part.id.material.value))
                    dump_partition_input(geometry, part, process);
            const auto domain = local(part.footprint);
            if (domain.empty())
                continue;
            const double height = part.top_mm - part.bottom_mm;
            if (part.id.material.value < 1 || part.id.material.value > geometry.machine.material_count ||
                part.id.stack_slot != part.id.material.value - 1 || height < process.min_active_thickness_mm - tolerances.interface_mm ||
                height > process.max_active_thickness_mm + tolerances.interface_mm)
                throw Abort{FailureCode::InfeasibleThickness, "painted_band_thickness_or_identity"};
            const Plane lower{Vec2d::Zero(), Vec2d::Zero(), part.bottom_mm}, upper{Vec2d::Zero(), Vec2d::Zero(), part.top_mm};
            ClippedBand band;
            band.id                      = part.id;
            const size_t first           = plan.paths.size();
            const double previous_volume = plan.volume_mm3[part.id.stack_slot];
            const auto   fill_area       = [&](const ExPolygons& area, double angle, PathRole role) {
                if (area.empty())
                    return;
                builder.fill_rotation = Eigen::Rotation2Dd(angle).toRotationMatrix();
                const auto rotated    = builder.rotate_fill(area, true);
                double     low = std::numeric_limits<double>::max(), high = -low;
                for (const auto& point : to_points(rotated)) {
                    low  = std::min(low, mm(point).y());
                    high = std::max(high, mm(point).y());
                }
                if (high <= low || area_mm2(rotated) <= 1e-12)
                    return;
                const size_t rows = size_t(std::ceil((high - low) / process.bead_width_mm));
                if (rows > limits.max_paths)
                    throw Abort{FailureCode::ResourceLimit, "painted_fill_row_limit"};
                const size_t begin = plan.paths.size();
                for (size_t row = 0; row < rows; ++row)
                    builder.fill(rotated, low + (high - low) * row / rows, low + (high - low) * (row + 1) / rows, band, lower, upper, 0);
                for (size_t i = begin; i < plan.paths.size(); ++i) {
                    plan.paths[i].role         = role;
                    plan.paths[i].split_reason = role == PathRole::Perimeter ? "paint_boundary_on_object_wall" : "painted_region_infill";
                }
            };
            // Preserve the validated whole-object infill as well as its walls.
            // Re-filling a thin curved interior discards its fitted bead caps.
            // Only segments crossed by a paint boundary need new centerlines.
            for (const auto& source : source_segments) {
                builder.check();
                const auto cut = intersection_ex(source.footprint, domain);
                if (cut.empty())
                    continue;
                double source_perimeter = 0.;
                for (const auto& polygon : source.footprint) {
                    source_perimeter += polygon.contour.length();
                    for (const auto& hole : polygon.holes)
                        source_perimeter += hole.length();
                }
                const double source_rounding = 8. * source_perimeter * SCALING_FACTOR * SCALING_FACTOR;
                const auto   outside         = diff_ex(source.footprint, domain);
                if (area_mm2(outside) <= std::max(1e-12, source_rounding)) {
                    // The source segment already has validated centerlines,
                    // fitted caps and bounded length. Do not split it again
                    // just because floating-point length rounds above a limit.
                    auto segment = *source.segment;
                    segment.volume_mm3 *= height / (geometry.solid_max.z() - geometry.solid_min.z());
                    segment.height_start_mm = segment.height_end_mm = height;
                    for (auto& cell : segment.cells)
                        for (size_t i = 0; i < 3; ++i) {
                            cell.upper[i].z() = part.top_mm;
                            cell.lower_z[i]   = part.bottom_mm;
                        }
                    if (!outside.empty()) {
                        // Retain the source motion, but clip its fitted caps
                        // exactly at the paint boundary. A small *area* alone
                        // does not imply boundary roundoff: a detached cap can
                        // lie entirely inside the neighboring material region.
                        segment.cells.clear();
                        segment.coverage_triangles.clear();
                        segment.volume_mm3 = 0.;
                        for (const auto& triangle : triangles(cut, true)) {
                            const Vec2d a = triangle[1] - triangle[0], b = triangle[2] - triangle[0];
                            segment.volume_mm3 += .5 * (a.x() * b.y() - a.y() * b.x()) * height;
                            BeadCell             cell;
                            std::array<Vec2d, 3> world;
                            for (size_t i = 0; i < 3; ++i) {
                                world[i]        = triangle[i] + builder.origin;
                                cell.upper[i]   = Vec3d(world[i].x(), world[i].y(), part.top_mm);
                                cell.lower_z[i] = part.bottom_mm;
                            }
                            segment.cells.push_back(std::move(cell));
                            segment.coverage_triangles.push_back(std::move(world));
                        }
                    }
                    builder.check(sizeof(PathSegment) + segment.cells.size() * sizeof(BeadCell) +
                                  segment.coverage_triangles.size() * sizeof(std::array<Vec2d, 3>));
                    if (++builder.segments > limits.max_segments)
                        throw Abort{FailureCode::ResourceLimit, "painted_segment_limit"};
                    const Vec3d  a(source.a.x() + builder.origin.x(), source.a.y() + builder.origin.y(), part.top_mm);
                    const Vec3d  b(source.b.x() + builder.origin.x(), source.b.y() + builder.origin.y(), part.top_mm);
                    const double diameter  = geometry.machine.filament_diameter_mm[part.id.material.value - 1];
                    const double e_quantum = 1e-5 * PI * diameter * diameter / 4.;
                    if (segment.cells.empty() || segment.volume_mm3 < e_quantum)
                        continue; // Leave sub-output-precision caps for the bounded gap repair.
                    const double emitted_length = (b.unaryExpr([](double x) { return GCodeFormatter::quantize_xyzf(x); }) -
                                                   a.unaryExpr([](double x) {
                                                       return GCodeFormatter::quantize_xyzf(x);
                                                   })).norm();
                    segment.speed_mm_s =
                        std::floor(std::min({segment.speed_mm_s, geometry.machine.max_volume_mm3_s * (b - a).norm() / segment.volume_mm3,
                                             geometry.machine.max_volume_mm3_s * emitted_length / (segment.volume_mm3 + e_quantum)}) *
                                   60000.) /
                        60000.;
                    if (segment.speed_mm_s <= 0.)
                        throw Abort{FailureCode::UnsupportedProfile, "coverage_feed_below_output_precision"};
                    if (plan.paths.size() == first || plan.paths.back().role != source.role ||
                        (plan.paths.back().vertices.back() - a).norm() >= 1e-8) {
                        auto run = builder.path(band, source.role, "painted_object_path");
                        run.vertices.push_back(a);
                        plan.paths.push_back(std::move(run));
                    }
                    plan.volume_mm3[part.id.stack_slot] += segment.volume_mm3;
                    plan.paths.back().vertices.push_back(b);
                    plan.paths.back().segments.push_back(std::move(segment));
                } else {
                    const auto d = source.b - source.a;
                    fill_area(cut, std::atan2(d.y(), d.x()), source.role);
                }
            }
            const double after_sources = plan.volume_mm3[part.id.stack_slot] - previous_volume;
            const auto   interior      = diff_ex(domain, all_source);
            fill_area(interior, std::remainder(coverage.infill.angle_degrees, 180.) * PI / 180., PathRole::DenseFill);
            const double after_interior = plan.volume_mm3[part.id.stack_slot] - previous_volume;
            builder.close_infill_gaps(domain, first, band, lower, upper, true);
            const double after_caps = plan.volume_mm3[part.id.stack_slot] - previous_volume;
            const double target     = area_mm2(domain) * height;
            // A paint edge can leave several adjacent fragments of the source
            // segments too short to print independently. Reunite the remaining
            // area and fit lanes along each connected fragment's longest edge.
            // Every added footprint is clipped to still-unoccupied domain area.
            for (size_t attempt = 0; attempt < 2; ++attempt) {
                Polygons occupied;
                for (size_t i = first; i < plan.paths.size(); ++i)
                    for (const auto& s : plan.paths[i].segments)
                        for (const auto& t : s.coverage_triangles)
                            occupied.push_back(polygon({t[0] - builder.origin, t[1] - builder.origin, t[2] - builder.origin}));
                const auto residual = diff_ex(domain, union_(occupied));
                Polygons   residual_triangles;
                for (const auto& tri : triangles(residual, true))
                    residual_triangles.push_back(polygon({tri[0], tri[1], tri[2]}));
                const auto gaps           = union_ex(residual_triangles);
                bool       actionable_gap = false;
                for (const auto& gap : gaps) {
                    double perimeter = gap.contour.length();
                    for (const auto& hole : gap.holes)
                        perimeter += hole.length();
                    if (gap.area() <= 8. * perimeter)
                        continue;
                    actionable_gap   = true;
                    Vec2d  direction = Vec2d::UnitX();
                    double longest   = 0.;
                    for (const auto& edge : gap.contour.lines()) {
                        const Vec2d d = mm(edge.b) - mm(edge.a);
                        if (d.squaredNorm() > longest) {
                            longest   = d.squaredNorm();
                            direction = d;
                        }
                    }
                    fill_area(ExPolygons{gap}, std::atan2(direction.y(), direction.x()) + attempt * PI / 2., PathRole::DenseFill);
                }
                if (!actionable_gap)
                    break;
                builder.close_infill_gaps(domain, first, band, lower, upper, true);
            }
            if (geometry.layered_process)
                for (size_t i = first; i < plan.paths.size(); ++i)
                    for (auto& segment : plan.paths[i].segments) {
                        builder.check();
                        const double previous_segment_volume = segment.volume_mm3;
                        compact_planar_segment(segment, lower, upper, builder.origin);
                        plan.volume_mm3[part.id.stack_slot] += segment.volume_mm3 - previous_segment_volume;
                    }
            const double      deposited = plan.volume_mm3[part.id.stack_slot] - previous_volume;
            const DomainCycle domain_cycle{part.id.domain, part.id.macro_cycle};
            if (domain_targets.find(domain_cycle) == domain_targets.end())
                builder.check(256);
            domain_targets[domain_cycle][part.id.stack_slot] += target;
            domain_deposited[domain_cycle][part.id.stack_slot] += deposited;
            // Keep omitted bead coverage separate from the tighter analytic
            // band-volume budget. Check the local material fractions below so
            // a global total cannot hide a shifted painted recipe.
            const double missing_budget = geometry.layered_process ? coverage.relative_missing_volume : tolerances.relative_volume;
            if (target - deposited > std::max(tolerances.absolute_volume_mm3, missing_budget * target) ||
                deposited - target > std::max(tolerances.absolute_volume_mm3, tolerances.relative_volume * target)) {
                if (const char* directory = std::getenv("MATERIAL_BAND_GEOMETRY_DUMP")) {
                    std::clog << "Partition dose sources=" << after_sources << " interior=" << after_interior << " caps=" << after_caps
                              << " final=" << deposited << " target=" << target << std::endl;
                    std::ofstream cells_file(std::string(directory) + "/partition-" + std::to_string(part.id.macro_cycle) + "-" +
                                             std::to_string(part.id.domain) + "-" + std::to_string(part.id.material.value) + ".cells.tsv");
                    cells_file << std::setprecision(17);
                    for (size_t i = first; i < plan.paths.size(); ++i)
                        for (size_t j = 0; j < plan.paths[i].segments.size(); ++j)
                            for (const auto& t : plan.paths[i].segments[j].coverage_triangles) {
                                cells_file << i << '\t' << j << '\t' << int(plan.paths[i].role) << '\t' << plan.paths[i].split_reason;
                                for (const auto& p : t)
                                    cells_file << '\t' << p.x() - builder.origin.x() << '\t' << p.y() - builder.origin.y();
                                cells_file << '\n';
                            }
                    Polygons occupied;
                    for (size_t i = first; i < plan.paths.size(); ++i)
                        for (const auto& segment : plan.paths[i].segments)
                            for (const auto& t : segment.coverage_triangles)
                                occupied.push_back(polygon({t[0] - builder.origin, t[1] - builder.origin, t[2] - builder.origin}));
                    const auto    gaps = diff_ex(domain, union_ex(occupied));
                    std::ofstream svg(std::string(directory) + "/partition-" + std::to_string(part.id.macro_cycle) + "-" +
                                      std::to_string(part.id.domain) + "-" + std::to_string(part.id.material.value) + ".svg");
                    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='1200' height='1200' viewBox='-1 -1 " << builder.extent + 2 << ' '
                        << builder.extent + 2 << "'>\n";
                    for (const auto& group : {std::make_pair(&domain, "#dddddd"), std::make_pair(&gaps, "#e02020")})
                        for (const auto& p : *group.first) {
                            svg << "<path fill='" << group.second << "' fill-rule='evenodd' d='";
                            for (const auto& boundary : to_polygons(p)) {
                                svg << "M";
                                for (const auto& point : boundary.points)
                                    svg << mm(point).x() << ',' << mm(point).y() << ' ';
                                svg << "z ";
                            }
                            svg << "'/>\n";
                        }
                    for (size_t i = first; i < plan.paths.size(); ++i) {
                        svg << "<polyline fill='none' stroke='#2255a0' stroke-width='.01' points='";
                        for (const auto& p : plan.paths[i].vertices)
                            svg << p.x() - builder.origin.x() << ',' << p.y() - builder.origin.y() << ' ';
                        svg << "'/>\n";
                    }
                    svg << "</svg>\n";
                }
                throw Abort{FailureCode::MaterialToleranceExceeded,
                            "painted_region_path_volume:target=" + std::to_string(target) + ":deposited=" + std::to_string(deposited) +
                                ":cycle=" + std::to_string(part.id.macro_cycle) + ":domain=" + std::to_string(part.id.domain) +
                                ":material=" + std::to_string(part.id.material.value)};
            }
            auto& diagnostic = plan.coverage[part.id.stack_slot];
            diagnostic.target_mm3 += target;
            diagnostic.deposited_mm3 += deposited;
            diagnostic.missing_mm3 += std::max(0., target - deposited);
            diagnostic.excess_mm3 += std::max(0., deposited - target);
            for (size_t i = first; i < plan.paths.size(); ++i) {
                if (geometry.layered_process) {
                    plan.paths[i].overhang_mm    = process.bead_width_mm * .5;
                    plan.paths[i].bridge_span_mm = 20.;
                }
                plan.paths[i].lower_interface  = part.lower_interface;
                plan.paths[i].upper_interface  = part.upper_interface;
                plan.paths[i].shared_interface = part.upper_interface ? part.upper_interface : part.lower_interface;
            }
            if (part.upper_interface) {
                InterfacePatch patch{part.upper_interface, part.id.domain, part.id.macro_cycle, {}};
                for (const auto& tri : triangles(domain, true)) {
                    const uint32_t index = uint32_t(patch.surface.vertices.size());
                    for (const auto& p : tri)
                        patch.surface.vertices.emplace_back(p.x() + builder.origin.x(), p.y() + builder.origin.y(), part.top_mm);
                    patch.surface.triangles.push_back({index, index + 1, index + 2});
                }
                plan.interfaces.push_back(std::move(patch));
            }
        }
        if (common_foundation_mm > 0.) {
            for (const auto& part : bands) {
                builder.check();
                if (std::abs(part.bottom_mm - geometry.solid_min.z()) > tolerances.interface_mm)
                    continue;
                const auto region = local(part.footprint);
                // Every phase targets the same intended painted region. Gap
                // repair can restore coverage absent from the source paths;
                // intersecting just the foundation with those source paths
                // would undercount its target while retaining its deposition.
                domain_targets[{part.id.domain, part.id.macro_cycle}][foundation_material - 1] += area_mm2(region) * common_foundation_mm;
            }
        }
        const auto add_foundation_dose = [&](const PathPlan& candidate, std::map<DomainCycle, MaterialFractions>& deposited) {
            if (common_foundation_mm <= 0.)
                return;
            Polygons foundation_cells;
            for (const auto& path : candidate.paths)
                if (path.band.domain == foundation_domain)
                    for (const auto& segment : path.segments)
                        for (const auto& cell : segment.cells) {
                            Points points;
                            for (const auto& p : cell.upper)
                                points.push_back(scaled(p.head<2>() - builder.origin));
                            foundation_cells.emplace_back(std::move(points));
                        }
            const auto foundation_covered = union_ex(foundation_cells);
            for (const auto& part : bands) {
                builder.check();
                if (std::abs(part.bottom_mm - geometry.solid_min.z()) > tolerances.interface_mm)
                    continue;
                const DomainCycle key{part.id.domain, part.id.macro_cycle};
                const auto        region = local(part.footprint);
                deposited[key][foundation_material - 1] += area_mm2(intersection_ex(region, foundation_covered)) * common_foundation_mm;
            }
        };
        const auto dose_failure = [&](const std::map<DomainCycle, MaterialFractions>& doses) -> std::optional<Failure> {
            for (const auto& [key, intended] : domain_targets) {
                const auto&  actual    = doses.at(key);
                const double target    = std::accumulate(intended.begin(), intended.end(), 0.);
                const double deposited = std::accumulate(actual.begin(), actual.end(), 0.);
                if (target <= tolerances.absolute_volume_mm3)
                    continue;
                for (size_t m = 0; m < MAX_MATERIALS; ++m) {
                    const BandId responsible{key.first, key.second, {uint32_t(m + 1)}, uint32_t(m)};
                    if (intended[m] - actual[m] > std::max(tolerances.absolute_volume_mm3, intended[m] * coverage.relative_missing_volume) ||
                        actual[m] - intended[m] > std::max(tolerances.absolute_volume_mm3, intended[m] * tolerances.relative_volume))
                        return Failure{geometry.revision,
                                       FailureCode::MaterialToleranceExceeded,
                                       "painted_fitted_cap_coverage_budget:domain=" + std::to_string(key.first) +
                                           ":cycle=" + std::to_string(key.second) + ":material=" + std::to_string(m + 1) +
                                           ":target=" + std::to_string(intended[m]) + ":deposited=" + std::to_string(actual[m]),
                                       responsible,
                                       {},
                                       {}};
                    if (deposited <= 0. || std::abs(intended[m] / target - actual[m] / deposited) > tolerances.plateau_fraction)
                        return Failure{geometry.revision,
                                       FailureCode::MaterialToleranceExceeded,
                                       "painted_recipe_fraction_budget:domain=" + std::to_string(key.first) +
                                           ":cycle=" + std::to_string(key.second) + ":material=" + std::to_string(m + 1),
                                       responsible,
                                       {},
                                       {}};
                }
            }
            return {};
        };
        add_foundation_dose(plan, domain_deposited);
        auto failure = dose_failure(domain_deposited);
        if (!preceding_deposition.empty()) {
            auto fitted = fit_planar_overhang_caps(plan, preceding_deposition, contact_z, coverage, limits, cancelled, stale);
            if (auto* error = std::get_if<Failure>(&fitted))
                return *error;
            const auto&                              candidate = *std::get<std::shared_ptr<const PathPlan>>(fitted);
            std::map<DomainCycle, MaterialFractions> fitted_dose;
            for (const auto& [key, values] : domain_targets)
                fitted_dose[key] = {};
            for (const auto& path : candidate.paths)
                for (const auto& segment : path.segments)
                    fitted_dose[{path.band.domain, path.band.macro_cycle}][path.band.stack_slot] += segment.volume_mm3;
            add_foundation_dose(candidate, fitted_dose);
            // Cap fitting is optional. Its per-segment allowance cannot spend
            // a painted region's volume or recipe budget twice. Keep the
            // original coverage when fitting exceeds either budget; support
            // scheduling must still validate the selected, unchanged geometry.
            if (!dose_failure(fitted_dose)) {
                plan = candidate;
                failure.reset();
            }
        }
        if (failure) {
            active_band = failure->band;
            throw Abort{failure->code, failure->detail};
        }
        // Partitioning and fitted caps can change the actual preceding contact
        // from the source-template estimate. A complete closed wall can move
        // its start without rebuilding or changing any owned deposition cell.
        if (!preceding_deposition.empty()) {
            for (auto& path : plan.paths) {
                builder.check();
                if (path.role != PathRole::Perimeter || path.vertices.size() < 4 ||
                    (path.vertices.front() - path.vertices.back()).norm() > 1e-10 || path.segments.empty())
                    continue;
                const double width  = path.segments.front().width_mm;
                bool         planar = true;
                for (const auto& segment : path.segments) {
                    planar &= std::abs(segment.width_mm - width) <= 1e-10;
                    for (const auto& cell : segment.cells)
                        for (size_t i = 0; i < 3; ++i)
                            planar &= std::abs(cell.lower_z[i] - contact_z) <= 1e-10 &&
                                      std::abs(cell.upper[i].z() - path.vertices.front().z()) <= 1e-10;
                }
                if (!planar)
                    continue;
                Points points;
                for (const auto& vertex : path.vertices)
                    points.push_back(scaled(vertex.head<2>()));
                const Points            original = points;
                std::vector<ContourRun> runs{{Polyline(std::move(points)), width, PathRole::Perimeter}};
                if (!anchor_perimeter_seams(runs, preceding_deposition, [&] { builder.check(); }))
                    continue;
                const auto first = std::find(original.begin(), original.end() - 1, runs.front().line.points.front()) - original.begin();
                std::rotate(path.vertices.begin(), path.vertices.begin() + first, path.vertices.end() - 1);
                path.vertices.back() = path.vertices.front();
                std::rotate(path.segments.begin(), path.segments.begin() + first, path.segments.end());
            }
        }
        return std::make_shared<const PathPlan>(std::move(plan));
    } catch (const Abort& error) {
        if (active_band)
            for (const auto& part : bands)
                if (part.id.domain == active_band->domain && part.id.macro_cycle == active_band->macro_cycle &&
                    part.id.material.value == active_band->material.value) {
                    try {
                        dump_partition_input(geometry, part, process, &bands, &preceding_deposition, contact_z, common_foundation_mm);
                    } catch (const std::exception&) {
                        // Diagnostics must not replace the original rejection.
                    }
                    break;
                }
        return Failure{geometry.revision, error.code, error.detail, active_band, {}, {}};
    } catch (const std::bad_alloc&) {
        return Failure{geometry.revision, FailureCode::ResourceLimit, "painted_paths_allocation", {}, {}, {}};
    } catch (const std::exception& error) {
        return Failure{geometry.revision,
                       FailureCode::UnsupportedTopology,
                       std::string("painted_path_geometry:") + error.what(),
                       {},
                       {},
                       {}};
    }
}
} // namespace Slic3r::MaterialBand
