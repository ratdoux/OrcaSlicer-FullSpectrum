#include "PaintedDomains.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <boost/polygon/voronoi.hpp>
#include "libslic3r/MultiMaterialSegmentation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>
#include <map>
#include <set>

namespace Slic3r::MaterialBand {
namespace {
struct Abort
{
    FailureCode code;
    std::string detail;
};
Point scaled(const Vec2d& p) { return Point(coord_t(std::llround(p.x() / SCALING_FACTOR)), coord_t(std::llround(p.y() / SCALING_FACTOR))); }
Vec2d mm(const Point& p) { return p.cast<double>() * SCALING_FACTOR; }
double area_mm2(const ExPolygons& polygons)
{
    double result = 0.;
    for (const auto& p : polygons)
        result += p.area() * SCALING_FACTOR * SCALING_FACTOR;
    return result;
}
Polygon projected(const std::vector<Vec3d>& vertices)
{
    Points points;
    for (const auto& p : vertices) {
        auto q = scaled(p.head<2>());
        if (points.empty() || points.back() != q)
            points.push_back(q);
    }
    if (points.size() > 1 && points.back() == points.front())
        points.pop_back();
    Polygon polygon(std::move(points));
    polygon.make_counter_clockwise();
    return polygon;
}
std::vector<Vec3d> clip_z(const std::vector<Vec3d>& vertices, double z, bool above)
{
    std::vector<Vec3d> result;
    if (vertices.empty())
        return result;
    Vec3d  a  = vertices.back();
    double da = (a.z() - z) * (above ? 1. : -1.);
    for (const auto& b : vertices) {
        const double db = (b.z() - z) * (above ? 1. : -1.);
        if ((da >= 0.) != (db >= 0.))
            result.push_back(a + (b - a) * (da / (da - db)));
        if (db >= 0.)
            result.push_back(b);
        a  = b;
        da = db;
    }
    return result;
}
struct Facet
{
    std::array<Vec3d, 3> points;
    size_t               region;
    double               low, high, normal_z;
};
struct PaintLine
{
    Vec2d  a, b;
    size_t region;
};
struct Layer
{
    std::shared_ptr<const PathPlan> geometry;
    std::vector<ExPolygons>         domains;
    double                          bottom, top;
    size_t                          geometry_bytes = 0;
};

std::vector<ExPolygons> partition(const ExPolygons&            outline,
                                  const std::vector<Facet>&    faces,
                                  const std::vector<size_t>&   candidates,
                                  size_t                       regions,
                                  double                       bottom,
                                  double                       top,
                                  double                       wall_width,
                                  double                       top_depth,
                                  double                       bottom_depth,
                                  const std::function<void()>& poll)
{
    const double           z = (bottom + top) / 2.;
    std::vector<PaintLine> lines;
    struct Cap
    {
        double  distance;
        size_t  region;
        Polygon polygon;
    };
    std::vector<Cap> caps;
    for (size_t index : candidates) {
        poll();
        const auto& face = faces[index];
        if (face.low <= z && face.high > z) {
            std::vector<Vec2d> points;
            for (size_t edge = 0; edge < 3; ++edge) {
                const auto& a = face.points[edge];
                const auto& b = face.points[(edge + 1) % 3];
                if ((a.z() <= z && b.z() > z) || (b.z() <= z && a.z() > z))
                    points.push_back((a + (b - a) * ((z - a.z()) / (b.z() - a.z()))).head<2>());
            }
            if (points.size() == 2 && (points[1] - points[0]).norm() > 1e-8)
                lines.push_back({points[0], points[1], face.region});
        }
        // Unpainted caps do not erase explicit side paint. Explicit top/bottom
        // paint extends through its configured shell depth, clipped to the solid.
        if (face.region == 0 || std::abs(face.normal_z) < 1e-12)
            continue;
        const bool   upward = face.normal_z > 0.;
        const double lo = upward ? z : z - bottom_depth, hi = upward ? z + top_depth : z;
        if (hi <= lo || face.high < lo || face.low > hi)
            continue;
        std::vector<Vec3d> triangle(face.points.begin(), face.points.end());
        auto               polygon = projected(clip_z(clip_z(triangle, lo, true), hi, false));
        if (polygon.points.size() >= 3 && polygon.area() > 0.)
            caps.push_back({upward ? std::max(0., face.low - z) : std::max(0., z - face.high), face.region, std::move(polygon)});
    }
    // Source facets may number in the hundreds of thousands. A section only
    // needs paint edges near its boundary, not all edge/contour pairs.
    constexpr double                                   grid_mm = 2.;
    std::map<std::pair<int, int>, std::vector<size_t>> line_grid;
    size_t                                             grid_references = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Vec2d lo = lines[i].a.cwiseMin(lines[i].b), hi = lines[i].a.cwiseMax(lines[i].b);
        for (int x = int(std::floor(lo.x() / grid_mm)); x <= int(std::floor(hi.x() / grid_mm)); ++x)
            for (int y = int(std::floor(lo.y() / grid_mm)); y <= int(std::floor(hi.y() / grid_mm)); ++y) {
                if (++grid_references > 2000000)
                    throw Abort{FailureCode::ResourceLimit, "paint_line_index_limit"};
                line_grid[{x, y}].push_back(i);
            }
    }
    std::vector<ColoredLines> colors;
    size_t                    work          = 0;
    const auto                color_contour = [&](const Polygon& contour) {
        ColoredLines painted;
        for (size_t edge = 0; edge < contour.points.size(); ++edge) {
            const Vec2d  a = mm(contour.points[edge]), b = mm(contour.points[(edge + 1) % contour.points.size()]);
            const Vec2d  delta  = b - a;
            const double length = delta.norm();
            if (length <= 1e-8)
                continue;
            const Vec2d      direction = delta / length;
            const double     reach     = std::max(.02, top - bottom);
            const Vec2d      query_lo = a.cwiseMin(b).array() - reach, query_hi = a.cwiseMax(b).array() + reach;
            std::set<size_t> nearby;
            for (int x = int(std::floor(query_lo.x() / grid_mm)); x <= int(std::floor(query_hi.x() / grid_mm)); ++x)
                for (int y = int(std::floor(query_lo.y() / grid_mm)); y <= int(std::floor(query_hi.y() / grid_mm)); ++y) {
                    const auto found = line_grid.find({x, y});
                    if (found != line_grid.end())
                        nearby.insert(found->second.begin(), found->second.end());
                }
            if (nearby.empty())
                throw Abort{FailureCode::UnsupportedTopology, "paint_boundary_without_source_edge"};
            std::vector<double> cuts{0., length};
            for (size_t index : nearby) {
                const auto& line = lines[index];
                if (++work % 1024 == 0)
                    poll();
                if (work > 20000000)
                    throw Abort{FailureCode::ResourceLimit, "paint_contour_projection_limit"};
                // Endpoints on parallel source edges carry leaf-facet splits.
                // Nonparallel nearby edges must not insert unrelated corner cuts.
                const Vec2d v = (line.b - line.a).normalized();
                if (std::abs(v.dot(direction)) < .999)
                    continue;
                for (const auto& p : {line.a, line.b}) {
                    const double along  = direction.dot(p - a);
                    const double across = std::abs(direction.x() * (p - a).y() - direction.y() * (p - a).x());
                    if (along > 1e-6 && along < length - 1e-6 && across <= std::max(.01, top - bottom))
                        cuts.push_back(along);
                }
            }
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double x, double y) { return std::abs(x - y) < 1e-6; }), cuts.end());
            const size_t first_edge_line = painted.size();
            for (size_t j = 1; j < cuts.size(); ++j) {
                const Vec2d middle  = a + direction * ((cuts[j - 1] + cuts[j]) / 2.);
                size_t      region  = 0;
                double      nearest = std::numeric_limits<double>::max();
                for (size_t index : nearby) {
                    const auto& line = lines[index];
                    if (++work % 1024 == 0)
                        poll();
                    if (work > 20000000)
                        throw Abort{FailureCode::ResourceLimit, "paint_contour_projection_limit"};
                    const Vec2d  v        = line.b - line.a;
                    const double t        = std::clamp(v.dot(middle - line.a) / v.squaredNorm(), 0., 1.);
                    const double distance = (middle - line.a - v * t).squaredNorm();
                    if (distance < nearest - 1e-14 || (std::abs(distance - nearest) <= 1e-14 && line.region < region)) {
                        nearest = distance;
                        region  = line.region;
                    }
                }
                const Point p = scaled(a + direction * cuts[j - 1]), q = scaled(a + direction * cuts[j]);
                if (p != q) {
                    if (painted.size() > first_edge_line && painted.back().color == int(region) && painted.back().line.b == p)
                        painted.back().line.b = q;
                    else
                        painted.push_back({Line(p, q), int(region), int(colors.size()), int(painted.size())});
                }
            }
        }
        colors.push_back(std::move(painted));
    };
    for (const auto& p : outline) {
        color_contour(p.contour);
        for (const auto& hole : p.holes)
            color_contour(hole);
    }
    auto       domains = segment_painted_layer(outline, colors, regions, true);
    const auto shell   = diff_ex(outline, offset_ex(outline, float(-wall_width / SCALING_FACTOR)));
    ExPolygons claimed;
    for (size_t r = 1; r < regions; ++r) {
        domains[r] = diff_ex(intersection_ex(domains[r], shell), claimed);
        claimed    = union_ex(claimed, domains[r]);
    }
    domains[0] = diff_ex(outline, claimed);
    // Closest painted cap wins deterministically where different height shells
    // overlap. At a shared facet edge the integer polygons remain complementary.
    std::stable_sort(caps.begin(), caps.end(),
                     [](const Cap& a, const Cap& b) { return std::tie(a.distance, a.region) < std::tie(b.distance, b.region); });
    ExPolygons                                         cap_claimed;
    std::vector<ExPolygons>                            cap_regions(regions);
    std::vector<ExPolygons>                            cap_parts;
    std::vector<size_t>                                cap_owners;
    std::vector<BoundingBox>                           cap_boxes;
    std::map<std::pair<int, int>, std::vector<size_t>> cap_grid;
    size_t                                             cap_references = 0;
    for (const auto& cap : caps) {
        poll();
        const BoundingBox box(cap.polygon.points);
        const Vec2d       lo = mm(box.min), hi = mm(box.max);
        const int         x0 = int(std::floor(lo.x() / grid_mm)), x1 = int(std::floor(hi.x() / grid_mm));
        const int         y0 = int(std::floor(lo.y() / grid_mm)), y1 = int(std::floor(hi.y() / grid_mm));
        std::set<size_t>  nearby;
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) {
                const auto found = cap_grid.find({x, y});
                if (found != cap_grid.end())
                    nearby.insert(found->second.begin(), found->second.end());
            }
        ExPolygons previous;
        for (size_t i : nearby)
            if (cap_owners[i] != cap.region && cap_boxes[i].overlap(box))
                previous.insert(previous.end(), cap_parts[i].begin(), cap_parts[i].end());
        // Priority subtraction commutes with the final outline intersection.
        // Clip once per region rather than once per source mesh triangle.
        auto part = diff_ex(ExPolygons{ExPolygon(cap.polygon)}, union_ex(previous));
        if (part.empty())
            continue;
        cap_regions[cap.region].insert(cap_regions[cap.region].end(), part.begin(), part.end());
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) {
                if (++cap_references > 2000000)
                    throw Abort{FailureCode::ResourceLimit, "paint_cap_index_limit"};
                cap_grid[{x, y}].push_back(cap_parts.size());
            }
        cap_parts.push_back(std::move(part));
        cap_owners.push_back(cap.region);
        cap_boxes.push_back(box);
    }
    for (auto& region : cap_regions) {
        region = intersection_ex(union_ex(region), outline);
        cap_claimed.insert(cap_claimed.end(), region.begin(), region.end());
    }
    cap_claimed = union_ex(cap_claimed);
    claimed.clear();
    double closure_rounding_area = 0.;
    for (size_t r = 0; r < regions; ++r) {
        if (!cap_claimed.empty())
            domains[r] = union_ex(diff_ex(domains[r], cap_claimed), cap_regions[r]);
        // Boolean subtraction of coincident angled edges can leave a region
        // consisting solely of lattice-width slivers. These cannot represent
        // a printable paint island; charge their area to closure below instead
        // of inventing a material stack for them.
        domains[r].erase(std::remove_if(domains[r].begin(), domains[r].end(),
                                        [](const ExPolygon& p) {
                                            double length = p.contour.length();
                                            for (const auto& hole : p.holes)
                                                length += hole.length();
                                            return p.area() <= 8. * length;
                                        }),
                         domains[r].end());
        double perimeter = 0.;
        for (const auto& polygon : domains[r]) {
            perimeter += polygon.contour.length();
            for (const auto& hole : polygon.holes)
                perimeter += hole.length();
        }
        // Independent integer Boolean intersections can differ by a lattice
        // quantum along a shared angled edge. Bound it by boundary length;
        // bead occupancy is still checked independently by the scheduler.
        const double rounding_area = 8. * perimeter * SCALING_FACTOR * SCALING_FACTOR;
        closure_rounding_area += rounding_area;
        if (area_mm2(intersection_ex(domains[r], claimed)) > std::max(1e-8, rounding_area))
            throw Abort{FailureCode::InvalidInput, "paint_domains_overlap"};
        claimed = union_ex(claimed, domains[r]);
    }
    const double closure_error = area_mm2(diff_ex(outline, claimed)) + area_mm2(diff_ex(claimed, outline));
    if (closure_error > std::max(1e-8, closure_rounding_area))
        throw Abort{FailureCode::MaterialToleranceExceeded, "paint_domains_do_not_close"};
    return domains;
}
} // namespace

std::vector<ExPolygons> partition_painted_domains(const KernelInput&           input,
                                                  const ExPolygons&            outline,
                                                  double                       bottom,
                                                  double                       top,
                                                  double                       wall_width,
                                                  double                       top_depth,
                                                  double                       bottom_depth,
                                                  const std::function<void()>& poll)
{
    std::vector<Facet>  faces;
    std::vector<size_t> candidates;
    const double        z = (bottom + top) / 2.;
    for (size_t r = 0; r < input.regions.size(); ++r) {
        const auto& mesh = input.regions[r].painted_surface;
        for (const auto& triangle : mesh.triangles) {
            Facet face;
            face.region = r;
            for (size_t k = 0; k < 3; ++k)
                face.points[k] = mesh.vertices.at(triangle[k]);
            face.low  = std::min({face.points[0].z(), face.points[1].z(), face.points[2].z()});
            face.high = std::max({face.points[0].z(), face.points[1].z(), face.points[2].z()});
            if (face.high < z - bottom_depth || face.low > z + top_depth)
                continue;
            face.normal_z = (face.points[1] - face.points[0]).cross(face.points[2] - face.points[0]).z();
            candidates.push_back(faces.size());
            faces.push_back(face);
        }
        poll();
    }
    try {
        return partition(outline, faces, candidates, input.regions.size(), bottom, top, wall_width, top_depth, bottom_depth, poll);
    } catch (const Abort& error) {
        throw Failure{input.revision, error.code, error.detail, {}, {}, {}};
    }
}

BoxPlanResult plan_painted_regions(const KernelInput&           input,
                                   const MachineEnvelope&       machine,
                                   DomainId                     domain,
                                   size_t                       perimeter_loops,
                                   const DenseInfillProfile&    infill,
                                   const PathLimits&            limits,
                                   const std::function<bool()>& cancelled,
                                   const std::function<bool()>& stale)
{
    try {
        const auto poll = [&] {
            if (cancelled && cancelled())
                throw Abort{FailureCode::Cancelled, "cancelled_during_painted_regions"};
            if (stale && stale())
                throw Abort{FailureCode::StaleInput, "revision_changed"};
        };
        poll();
        if (input.solid.vertices.empty() || input.regions.empty() || input.regions.front().id != 0 || input.material_count < 2 ||
            input.material_count > MAX_MATERIALS || input.material_count != machine.material_count || domain == 0 ||
            domain > std::numeric_limits<uint64_t>::max() / 512 || input.regions.size() > 256 || machine.sections.empty())
            throw Abort{FailureCode::InvalidInput, "painted_regions_input"};
        for (const auto& region : input.regions) {
            for (size_t i = 0; i < region.height_gradient.size(); ++i) {
                const auto& knot = region.height_gradient[i];
                if (!std::isfinite(knot.position) || knot.position < 0. || knot.position > 1. ||
                    (i && knot.position <= region.height_gradient[i - 1].position))
                    throw Abort{FailureCode::InvalidInput, "invalid_painted_gradient_curve"};
                double sum = 0.;
                for (size_t m = 0; m < MAX_MATERIALS; ++m) {
                    if (!std::isfinite(knot.fractions[m]) || knot.fractions[m] < 0. || knot.fractions[m] > 1. ||
                        (m >= input.material_count && knot.fractions[m] != 0.))
                        throw Abort{FailureCode::InvalidInput, "invalid_painted_gradient_fractions"};
                    sum += knot.fractions[m];
                }
                if (std::abs(sum - 1.) > 1e-10)
                    throw Abort{FailureCode::InvalidInput, "painted_gradient_fractions_do_not_close"};
            }
        }
        Vec3d low = input.solid.vertices.front(), high = low;
        for (const auto& p : input.solid.vertices) {
            low  = low.cwiseMin(p);
            high = high.cwiseMax(p);
        }
        const double height = input.process.macro_height_mm;
        if (!low.allFinite() || !high.allFinite() || std::abs(low.z()) > input.tolerances.interface_mm || !std::isfinite(height) ||
            height <= 0.)
            throw Abort{FailureCode::UnsupportedProfile, "painted_regions_height"};
        const double cycles = std::ceil((high.z() - low.z() - input.tolerances.interface_mm) / height);
        if (!std::isfinite(cycles) || cycles < 1. || cycles > double(limits.max_cycles) || cycles > 10000.)
            throw Abort{FailureCode::ResourceLimit, "painted_cycle_limit"};
        const size_t                     count = size_t(cycles);
        std::vector<Facet>               faces;
        std::vector<std::vector<size_t>> candidates(count);
        size_t                           references = 0;
        const double                     top_depth = infill.top_layers * height, bottom_depth = infill.bottom_layers * height;
        for (size_t r = 0; r < input.regions.size(); ++r) {
            const auto& paint = input.regions[r].painted_surface;
            for (const auto& triangle : paint.triangles) {
                poll();
                Facet face;
                face.region = r;
                for (size_t k = 0; k < 3; ++k)
                    face.points[k] = paint.vertices.at(triangle[k]);
                face.low           = std::min({face.points[0].z(), face.points[1].z(), face.points[2].z()});
                face.high          = std::max({face.points[0].z(), face.points[1].z(), face.points[2].z()});
                face.normal_z      = (face.points[1] - face.points[0]).cross(face.points[2] - face.points[0]).z();
                const size_t index = faces.size();
                faces.push_back(face);
                const int first = std::max(0, int(std::floor((face.low - top_depth - low.z()) / height)));
                const int last  = std::min(int(count) - 1, int(std::floor((face.high + bottom_depth - low.z()) / height)));
                for (int c = first; c <= last; ++c) {
                    if (++references > limits.max_temporary_bytes / 32)
                        throw Abort{FailureCode::ResourceLimit, "paint_facet_index_limit"};
                    candidates[c].push_back(index);
                }
            }
        }
        std::vector<Layer> layers;
        size_t             retained = references * sizeof(size_t) + faces.size() * sizeof(Facet);
        for (size_t c = 0; c < count; ++c) {
            poll();
            const double bottom = low.z() + c * height, top = std::min(high.z(), bottom + height);
            auto slab = clip_pure_slab(input.solid, {domain, c, {1}, 0}, input.revision, bottom, top, input.tolerances, {}, cancelled,
                                       stale);
            if (auto error = std::get_if<Failure>(&slab))
                return *error;
            const auto&    band = *std::get<std::shared_ptr<const ClippedBand>>(slab);
            CoverageLimits coverage;
            coverage.perimeter_loops       = perimeter_loops;
            coverage.infill                = infill.cycle(c, count);
            coverage.minimum_bead_width_mm = machine.coplanar_tip_contact ?
                                                 .04 :
                                                 std::max(.04, 2.1 * (machine.sections.front().radius_mm + machine.clearance_mm));
            auto paths = plan_pure_cycle(band, input.revision, input.process, input.tolerances, machine, limits, coverage, cancelled, stale);
            if (auto error = std::get_if<Failure>(&paths))
                return *error;
            Polygons cap;
            for (const auto& tri : band.solid.triangles) {
                const Vec3d a = band.solid.vertices[tri[0]], b = band.solid.vertices[tri[1]], d = band.solid.vertices[tri[2]];
                if (std::max({std::abs(a.z() - top), std::abs(b.z() - top), std::abs(d.z() - top)}) < 1e-8 && (b - a).cross(d - a).z() > 0.)
                    cap.push_back(projected({a, b, d}));
            }
            const auto outline = union_ex(cap);
            if (outline.empty())
                throw Abort{FailureCode::UnsupportedTopology, "painted_cycle_cap"};
            Layer layer{std::get<std::shared_ptr<const PathPlan>>(paths),
                        partition(outline, faces, candidates[c], input.regions.size(), bottom, top,
                                  perimeter_loops * input.process.bead_width_mm, top_depth, bottom_depth, poll),
                        bottom, top};
            for (const auto& path : layer.geometry->paths) {
                layer.geometry_bytes += sizeof(BandPath) + path.vertices.size() * sizeof(Vec3d);
                for (const auto& segment : path.segments)
                    layer.geometry_bytes += sizeof(PathSegment) + segment.cells.size() * (sizeof(BeadCell) + sizeof(std::array<Vec2d, 3>));
            }
            retained += layer.geometry_bytes;
            for (const auto& p : layer.domains)
                retained += to_points(p).size() * sizeof(Point);
            if (retained > limits.max_temporary_bytes)
                throw Abort{FailureCode::ResourceLimit, "painted_geometry_memory_limit"};
            layers.push_back(std::move(layer));
        }
        std::vector<std::vector<MaterialFractions>> fractions(input.regions.size(), std::vector<MaterialFractions>(count));
        MaterialFractions                           target{};
        for (size_t r = 0; r < input.regions.size(); ++r) {
            std::vector<StackDoseCycle> doses;
            std::vector<size_t>         cycles;
            for (size_t c = 0; c < count; ++c) {
                const auto&  layer  = layers[c];
                const double volume = area_mm2(layer.domains[r]) * (layer.top - layer.bottom);
                if (volume <= 0.)
                    continue;
                doses.push_back(
                    {sample_height_gradient(input.regions[r], ((layer.bottom + layer.top) / 2. - low.z()) / (high.z() - low.z())),
                     layer.top - layer.bottom, volume});
                cycles.push_back(c);
            }
            if (doses.empty())
                continue;
            auto result = solve_stack_doses(doses, input.material_count, input.process,
                                            input.regions[r].height_gradient.empty() ? 0. : input.gradient_fraction_error, input.tolerances,
                                            input.revision, {}, cancelled, stale);
            if (auto error = std::get_if<Failure>(&result))
                return *error;
            const auto& solution = *std::get<std::shared_ptr<const StackDoseSolution>>(result);
            for (size_t i = 0; i < cycles.size(); ++i)
                fractions[r][cycles[i]] = solution.fractions[i];
            for (size_t m = 0; m < MAX_MATERIALS; ++m)
                target[m] += solution.target_volume_mm3[m];
        }
        auto plan                  = std::make_shared<PathPlan>();
        plan->revision             = input.revision;
        plan->machine              = machine;
        plan->solid_min            = low;
        plan->solid_max            = high;
        plan->ordered_region_paths = true;
        size_t segments            = 0;
        for (size_t c = 0; c < count; ++c) {
            poll();
            std::vector<PlanarMaterialBand> bands;
            for (size_t r = 0; r < input.regions.size(); ++r) {
                double      z     = layers[c].bottom;
                InterfaceId lower = 0;
                for (size_t m = 0; m < input.material_count; ++m) {
                    const double f = fractions[r][c][m];
                    if (f == 0.)
                        continue;
                    const bool last = std::none_of(fractions[r][c].begin() + m + 1, fractions[r][c].end(), [](double x) { return x > 0.; });
                    const double      next  = last ? layers[c].top : z + (layers[c].top - layers[c].bottom) * f;
                    const InterfaceId upper = next < layers[c].top - input.tolerances.interface_mm ?
                                                  1 + (c * input.regions.size() + r) * MAX_MATERIALS + m :
                                                  0;
                    bands.push_back(
                        {{domain * 512 + r + 1, c, {uint32_t(m + 1)}, uint32_t(m)}, layers[c].domains[r], z, next, lower, upper});
                    z     = next;
                    lower = upper;
                }
            }
            CoverageLimits coverage;
            coverage.infill                = infill.cycle(c, count);
            coverage.minimum_bead_width_mm = machine.coplanar_tip_contact ?
                                                 .04 :
                                                 std::max(.04, 2.1 * (machine.sections.front().radius_mm + machine.clearance_mm));
            auto result = plan_partitioned_cycle(*layers[c].geometry, bands, input.process, input.tolerances, limits, coverage, cancelled,
                                                 stale);
            if (auto error = std::get_if<Failure>(&result))
                return *error;
            // This cycle's validated template is no longer needed after its
            // owned region paths are built. Release it before retaining output.
            retained -= layers[c].geometry_bytes;
            layers[c].geometry.reset();
            const auto& paths = *std::get<std::shared_ptr<const PathPlan>>(result);
            for (auto path : paths.paths) {
                segments += path.segments.size();
                retained += sizeof(BandPath) + path.vertices.size() * sizeof(Vec3d);
                for (const auto& s : path.segments)
                    retained += sizeof(PathSegment) + s.cells.size() * sizeof(BeadCell) +
                                s.coverage_triangles.size() * sizeof(std::array<Vec2d, 3>) + s.footprint.size() * sizeof(Vec2d);
                if (retained > limits.max_temporary_bytes)
                    throw Abort{FailureCode::ResourceLimit, "painted_paths_memory_limit"};
                if (segments > limits.max_segments || plan->paths.size() >= limits.max_paths)
                    throw Abort{FailureCode::ResourceLimit, "painted_path_limit"};
                plan->paths.push_back(std::move(path));
            }
            plan->interfaces.insert(plan->interfaces.end(), paths.interfaces.begin(), paths.interfaces.end());
            for (size_t m = 0; m < MAX_MATERIALS; ++m) {
                plan->volume_mm3[m] += paths.volume_mm3[m];
                plan->coverage[m].target_mm3 += paths.coverage[m].target_mm3;
                plan->coverage[m].deposited_mm3 += paths.coverage[m].deposited_mm3;
                plan->coverage[m].missing_mm3 += paths.coverage[m].missing_mm3;
                plan->coverage[m].excess_mm3 += paths.coverage[m].excess_mm3;
            }
        }
        for (size_t m = 0; m < input.material_count; ++m)
            if (std::abs(plan->volume_mm3[m] - target[m]) >
                std::max(input.tolerances.absolute_volume_mm3, input.tolerances.relative_volume * target[m]))
                throw Abort{FailureCode::MaterialToleranceExceeded, "painted_material_volume_budget"};
        std::stable_sort(plan->paths.begin(), plan->paths.end(), [](const BandPath& a, const BandPath& b) {
            return std::make_tuple(a.band.macro_cycle, std::llround(a.vertices.front().z() * 1e8), a.band.material.value, a.band.domain) <
                   std::make_tuple(b.band.macro_cycle, std::llround(b.vertices.front().z() * 1e8), b.band.material.value, b.band.domain);
        });
        // Restore continuous walls and infill runs split only for region clipping.
        std::vector<BandPath> joined;
        for (auto& path : plan->paths) {
            if (!joined.empty() && joined.back().band.domain == path.band.domain &&
                joined.back().band.macro_cycle == path.band.macro_cycle && joined.back().band.material.value == path.band.material.value &&
                joined.back().role == path.role && joined.back().lower_interface == path.lower_interface &&
                joined.back().upper_interface == path.upper_interface &&
                (joined.back().vertices.back() - path.vertices.front()).norm() < 1e-8) {
                auto& to = joined.back();
                to.vertices.insert(to.vertices.end(), path.vertices.begin() + 1, path.vertices.end());
                to.segments.insert(to.segments.end(), std::make_move_iterator(path.segments.begin()),
                                   std::make_move_iterator(path.segments.end()));
            } else
                joined.push_back(std::move(path));
        }
        plan->paths = std::move(joined);
        for (size_t i = 0; i < plan->paths.size(); ++i)
            plan->paths[i].id = i + 1;
        poll();
        return std::shared_ptr<const PathPlan>(std::move(plan));
    } catch (const Abort& error) {
        return Failure{input.revision, error.code, error.detail, {}, {}, {}};
    } catch (const std::bad_alloc&) {
        return Failure{input.revision, FailureCode::ResourceLimit, "painted_regions_allocation", {}, {}, {}};
    } catch (const std::exception& error) {
        return Failure{input.revision,
                       FailureCode::UnsupportedTopology,
                       std::string("painted_regions_geometry:") + error.what(),
                       {},
                       {},
                       {}};
    }
}
} // namespace Slic3r::MaterialBand
