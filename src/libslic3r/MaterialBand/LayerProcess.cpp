#include "LayerProcess.hpp"
#include "Scheduler.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Surface.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <iostream>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <fstream>
#include <iomanip>

namespace Slic3r::MaterialBand {
namespace {
struct Abort
{
    FailureCode code;
    std::string detail;
};
void capture_cycle_paths(const PathPlan& plan, size_t cycle, bool last, bool failed = false)
{
    const char* selected  = std::getenv("MATERIAL_BAND_CAPTURE_CYCLE");
    const char* directory = std::getenv("MATERIAL_BAND_GEOMETRY_DUMP");
    const bool  full      = std::getenv("MATERIAL_BAND_CAPTURE_FULL") != nullptr;
    if (!selected || !directory ||
        (!failed && ("," + std::string(selected) + ",").find("," + std::to_string(cycle) + ",") == std::string::npos &&
         !(last && ("," + std::string(selected) + ",").find(",last,") != std::string::npos)) ||
        plan.paths.size() > (full ? 500000 : 50000))
        return;
    size_t cells = 0;
    for (const auto& path : plan.paths)
        for (const auto& segment : path.segments)
            cells += segment.cells.size();
    if (cells > (full ? 4000000 : 500000))
        return;
    const std::string filename = failed ? "cycle-" + std::to_string(cycle) + "-failed-paths.json" :
                                 std::string(selected).find(',') == std::string::npos ? "cycle-paths.json" :
                                                                                        "cycle-" + std::to_string(cycle) + "-paths.json";
    std::ofstream     out(std::string(directory) + "/" + filename);
    out << std::setprecision(17);
    const auto  vector = [&](const Vec3d& p) { out << '[' << p.x() << ',' << p.y() << ',' << p.z() << ']'; };
    const auto& m      = plan.machine;
    out << "{\"diagnostic_full\":" << (full ? "true" : "false") << ",\"machine\":[" << m.material_count << ',' << m.clearance_mm << ','
        << m.travel_lift_mm << ',' << m.max_z_mm_s << ',' << m.max_volume_mm3_s << ',' << m.max_segment_mm << ',' << m.deposition_mm_s
        << ',' << m.shared_nozzle << ',' << m.coplanar_tip_contact << "],\"diameters\":[";
    for (size_t i = 0; i < MAX_MATERIALS; ++i)
        out << (i ? "," : "") << m.filament_diameter_mm[i];
    out << "],\"sections\":[";
    for (size_t i = 0; i < m.sections.size(); ++i) {
        const auto& s = m.sections[i];
        out << (i ? "," : "") << '[' << s.bottom_mm << ',' << s.top_mm << ',' << s.radius_mm << ']';
    }
    out << "],\"minimum\":";
    vector(plan.solid_min);
    out << ",\"maximum\":";
    vector(plan.solid_max);
    out << ",\"paths\":[";
    for (size_t i = 0; i < plan.paths.size(); ++i) {
        const auto& p = plan.paths[i];
        out << (i ? "," : "") << "{\"band\":[" << p.band.domain << ',' << p.band.macro_cycle << ',' << p.band.material.value << ','
            << p.band.stack_slot << "],\"role\":" << int(p.role) << ",\"overhang\":" << p.overhang_mm << ",\"bridge\":" << p.bridge_span_mm
            << ",\"reason\":" << std::quoted(p.split_reason) << ",\"vertices\":[";
        for (size_t j = 0; j < p.vertices.size(); ++j) {
            if (j)
                out << ',';
            vector(p.vertices[j]);
        }
        out << "],\"segments\":[";
        for (size_t j = 0; j < p.segments.size(); ++j) {
            const auto& s = p.segments[j];
            out << (j ? "," : "") << "{\"values\":[" << s.volume_mm3 << ',' << s.speed_mm_s << ',' << s.width_mm << ',' << s.height_start_mm
                << ',' << s.height_end_mm << "],\"cells\":[";
            for (size_t k = 0; k < s.cells.size(); ++k) {
                const auto& c = s.cells[k];
                out << (k ? "," : "") << '[';
                for (size_t v = 0; v < 3; ++v)
                    out << (v ? "," : "") << c.upper[v].x() << ',' << c.upper[v].y() << ',' << c.upper[v].z() << ',' << c.lower_z[v];
                out << ']';
            }
            out << "]}";
        }
        out << "]}";
    }
    out << "]}\n";
}
double area_mm2(const ExPolygons& polys)
{
    double a = 0.;
    for (const auto& p : polys)
        a += p.area() * SCALING_FACTOR * SCALING_FACTOR;
    return a;
}
InfillPattern dense_pattern(const DenseInfillOptions& options)
{
    switch (options.pattern) {
    case DenseInfillPattern::Rectilinear: return ipRectilinear;
    case DenseInfillPattern::Concentric: return ipConcentric;
    default: return ipMonotonic;
    }
}
ExPolygons deposition(const PathPlan& plan, std::optional<double> contact_z = {})
{
    Polygons polygons;
    for (const auto& path : plan.paths)
        for (const auto& segment : path.segments)
            for (const auto& cell : segment.cells) {
                if (contact_z &&
                    std::any_of(cell.upper.begin(), cell.upper.end(), [&](const Vec3d& p) { return std::abs(p.z() - *contact_z) > 1e-10; }))
                    continue;
                Points points;
                for (const auto& p : cell.upper)
                    points.emplace_back(scale_(p.x()), scale_(p.y()));
                polygons.emplace_back(std::move(points));
            }
    return union_ex(polygons);
}
void contours(const ExPolygons& polygons, double width, PathRole role, std::vector<ContourRun>& runs)
{
    for (const auto& p : polygons) {
        const auto add = [&](const Polygon& boundary) {
            if (boundary.points.size() < 3)
                return;
            Points points = boundary.points;
            points.push_back(points.front());
            Polyline line(std::move(points));
            line.simplify(scale_(.005));
            runs.push_back({std::move(line), width, role});
        };
        add(p.contour);
        for (const auto& hole : p.holes)
            add(hole);
    }
}
void fill(const ExPolygons&                 polygons,
          InfillPattern                     pattern,
          double                            density,
          double                            width,
          double                            z,
          double                            height,
          size_t                            cycle,
          double                            angle,
          PathRole                          role,
          std::vector<ContourRun>&          runs,
          std::shared_ptr<const ExPolygons> coverage = {})
{
    if (polygons.empty() || density <= 0.)
        return;
    if (pattern == ipAdaptiveCubic || pattern == ipSupportCubic || pattern == ipLightning)
        throw Abort{FailureCode::UnsupportedProfile, "layer_infill_requires_volume_preprocessing"};
    std::unique_ptr<Fill> generator(Fill::new_from_type(pattern));
    if (!generator)
        throw Abort{FailureCode::UnsupportedProfile, "unknown_layer_infill"};
    generator->layer_id = cycle;
    generator->z        = z;
    generator->spacing  = width;
    // Rectilinear-family generators require their inner offset to be negative;
    // overlap above half a spacing silently produces no inner contours. Expand
    // the input explicitly, then use the normal half-spacing inset. This also
    // gives sparse/bridge endpoints their intended wall-anchor overlap.
    generator->overlap = 0.;
    generator->angle   = float(angle * PI / 180.);
    generator->set_bounding_box(BoundingBox(to_points(polygons)));
    FillParams parameters;
    parameters.density           = float(density);
    parameters.pattern           = pattern;
    parameters.resolution        = .0125;
    parameters.anchor_length     = 2. * width;
    parameters.anchor_length_max = 5. * width;
    if (role == PathRole::Bridge)
        parameters.anchor_length = parameters.anchor_length_max = 0.;
    parameters.flow         = Flow(float(width), float(height), .4f);
    parameters.layer_height = height;
    parameters.monotonic    = pattern == ipMonotonic;
    for (const auto& polygon : offset_ex(polygons, float(scale_(width + .01)))) {
        Surface surface(density >= .999 ? stInternalSolid : stInternal, polygon);
        auto    lines = generator->fill_surface(&surface, parameters);
        for (auto& line : lines) {
            line.simplify(scale_(.005));
            if (line.points.size() > 1)
                runs.push_back({std::move(line), width, role, coverage});
        }
    }
}
// Extend a bridge lane through interior air to real anchors, without crossing
// an object boundary or cavity. The scheduler independently verifies the final
// cap contact and complete unsupported span against actual deposition cells.
bool anchor_bridge_run(ContourRun& run, const ExPolygons& anchors, const ExPolygons& anchor_core, const ExPolygons& center_domain)
{
    bool anchored = true;
    for (size_t end : {size_t(0), run.line.points.size() - 1}) {
        const size_t adjacent = end == 0 ? 1 : end - 1;
        const Point  original = run.line.points[end];
        if (std::any_of(anchor_core.begin(), anchor_core.end(), [&](const ExPolygon& p) { return p.contains(original); }))
            continue;
        const Vec2d a     = original.cast<double>() * SCALING_FACTOR;
        const Vec2d delta = (original - run.line.points[adjacent]).cast<double>();
        if (delta.squaredNorm() == 0.) {
            anchored = false;
            continue;
        }
        const Vec2d direction = delta.normalized();
        if (std::any_of(anchors.begin(), anchors.end(), [&](const ExPolygon& p) { return p.contains(original); })) {
            const Vec2d along = direction * (run.width_mm / 2.), across(-along.y(), along.x());
            Points      corners;
            for (const Vec2d& p : std::array<Vec2d, 4>{{a - along - across, a + along - across, a + along + across, a - along + across}})
                corners.emplace_back(scale_(p.x()), scale_(p.y()));
            if (area_mm2(intersection_ex(Polygons{Polygon(std::move(corners))}, anchors)) >= .5 * run.width_mm * run.width_mm)
                continue;
        }
        const Vec2d    b = a + direction * 20.;
        const Polyline ray{Points{original, Point(scale_(b.x()), scale_(b.y()))}};
        double         reachable = 0.;
        for (const auto& piece : intersection_pl(Polylines{ray}, center_domain)) {
            double low = 20., high = 0.;
            for (const auto& point : piece.points) {
                const double t = direction.dot(point.cast<double>() * SCALING_FACTOR - a);
                low            = std::min(low, t);
                high           = std::max(high, t);
            }
            if (low < 4. * SCALING_FACTOR)
                reachable = std::max(reachable, high);
        }
        double nearest = 21.;
        for (const auto& piece : intersection_pl(Polylines{ray}, anchor_core)) {
            double low = 20., high = 0.;
            for (const auto& point : piece.points) {
                const double t = direction.dot(point.cast<double>() * SCALING_FACTOR - a);
                low            = std::min(low, t);
                high           = std::max(high, t);
            }
            high = std::min(high, reachable);
            if (high > low + 4. * SCALING_FACTOR)
                nearest = std::min(nearest, low + std::min(run.width_mm / 2., (high - low) / 2.));
        }
        if (nearest > 20.) {
            anchored = false;
            continue;
        }
        const Vec2d target   = a + direction * nearest;
        run.line.points[end] = Point(scale_(target.x()), scale_(target.y()));
    }
    // Endpoint contact alone cannot qualify a direction. A diagonal may
    // exceed the span limit while an axis-aligned lane across the same roof
    // remains printable. Continue direction selection in that case; the
    // scheduler still checks the final owned and painted deposition.
    for (const auto& gap : diff_pl(Polylines{run.line}, anchors))
        if (gap.length() * SCALING_FACTOR > 20. + 1e-6)
            anchored = false;
    return anchored;
}
size_t bytes(const PathPlan& plan)
{
    size_t result = sizeof(PathPlan) + plan.paths.capacity() * sizeof(BandPath) + plan.interfaces.capacity() * sizeof(InterfacePatch);
    for (const auto& path : plan.paths) {
        result += path.vertices.capacity() * sizeof(Vec3d) + path.segments.capacity() * sizeof(PathSegment) + path.split_reason.capacity();
        for (const auto& s : path.segments)
            result += s.cells.capacity() * sizeof(BeadCell) + s.footprint.capacity() * sizeof(Vec2d) +
                      s.coverage_triangles.capacity() * sizeof(std::array<Vec2d, 3>);
    }
    for (const auto& patch : plan.interfaces)
        result += patch.surface.vertices.capacity() * sizeof(Vec3d) + patch.surface.triangles.capacity() * sizeof(std::array<uint32_t, 3>);
    return result;
}
size_t bytes(const ExPolygons& polygons)
{
    size_t result = polygons.capacity() * sizeof(ExPolygon);
    for (const auto& piece : polygons) {
        result += piece.contour.points.capacity() * sizeof(Point) + piece.holes.capacity() * sizeof(Polygon);
        for (const auto& hole : piece.holes)
            result += hole.points.capacity() * sizeof(Point);
    }
    return result;
}
} // namespace

ProcessCyclesResult plan_process_cycles(const KernelInput&           input,
                                        const LayerProcessProfile&   profile,
                                        const PathLimits&            limits,
                                        const std::function<bool()>& cancelled)
{
    const auto fail = [&](const char* message) -> ProcessCyclesResult {
        return Failure{input.revision, FailureCode::InfeasibleThickness, message, {}, {}, {}};
    };
    if (input.solid.vertices.empty() || !std::isfinite(profile.nominal_height_mm) || profile.nominal_height_mm <= 0. ||
        !std::isfinite(profile.initial_height_mm) || profile.initial_height_mm <= 0. ||
        !std::isfinite(input.process.min_active_thickness_mm) || input.process.min_active_thickness_mm <= 0. ||
        !std::isfinite(input.process.max_active_thickness_mm) ||
        input.process.max_active_thickness_mm < input.process.min_active_thickness_mm)
        return fail("invalid_layer_process_heights");
    if (cancelled && cancelled())
        return Failure{input.revision, FailureCode::Cancelled, "cancelled_during_cycles", {}, {}, {}};
    double low = input.solid.vertices.front().z(), high = low;
    for (const auto& p : input.solid.vertices) {
        low  = std::min(low, p.z());
        high = std::max(high, p.z());
    }
    if (std::abs(low) > input.tolerances.interface_mm || high <= low)
        return fail("layer_process_requires_bed_origin");
    double minimum = input.process.min_active_thickness_mm, maximum = input.process.max_active_thickness_mm;
    for (const auto& region : input.regions) {
        const auto examine = [&](const MaterialFractions& fractions) {
            const double error = region.height_gradient.empty() ? 0. : input.gradient_fraction_error;
            for (double f : fractions)
                if (f > error + 1e-12) {
                    minimum = std::max(minimum, input.process.min_active_thickness_mm / std::min(1., f + error));
                    maximum = std::min(maximum, input.process.max_active_thickness_mm / (f - error));
                }
        };
        if (region.height_gradient.empty())
            examine(region.fractions);
        else
            for (size_t i = 0; i <= 128; ++i)
                examine(sample_height_gradient(region, double(i) / 128.));
    }
    if (minimum > maximum + input.tolerances.interface_mm)
        return fail("material_cycle_cannot_meet_thickness_bounds");
    const double first = std::min(high, profile.initial_height_mm);
    if (first < minimum - input.tolerances.interface_mm || first > maximum + input.tolerances.interface_mm)
        return fail("first_layer_cannot_fit_active_materials");
    std::vector<ProcessCycle> cycles{{0., first}};
    const double              remaining = high - first;
    if (remaining <= input.tolerances.interface_mm)
        return cycles;
    const double desired = std::min(maximum, std::max(profile.nominal_height_mm,
                                                      std::ceil(minimum / profile.nominal_height_mm - 1e-9) * profile.nominal_height_mm));
    if (!std::isfinite(high) || remaining / minimum > 10000000.)
        return Failure{input.revision, FailureCode::ResourceLimit, "process_cycle_limit", {}, {}, {}};
    const size_t least = size_t(std::ceil(remaining / maximum - 1e-8));
    const size_t most  = size_t(std::floor(remaining / minimum + 1e-8));
    if (most < least || most == 0)
        return fail("last_material_cycle_too_thin");
    const size_t count = std::clamp(size_t(std::max(1., std::floor(remaining / desired))), least, most);
    if (count + 1 > limits.max_cycles)
        return Failure{input.revision, FailureCode::ResourceLimit, "process_cycle_limit", {}, {}, {}};
    for (size_t c = 0; c < count; ++c) {
        if (cancelled && cancelled())
            return Failure{input.revision, FailureCode::Cancelled, "cancelled_during_cycles", {}, {}, {}};
        cycles.push_back({first + remaining * c / count, first + remaining * (c + 1) / count});
    }
    return cycles;
}

static BoxPlanResult plan_layer_process_impl(const KernelInput&               input,
                                             const MachineEnvelope&           machine,
                                             DomainId                         domain,
                                             const LayerProcessProfile&       profile,
                                             const PathLimits&                limits,
                                             const std::function<bool()>&     cancelled,
                                             const std::function<bool()>&     stale,
                                             const std::vector<ProcessCycle>* refined_cycles,
                                             size_t                           refinement)
{
    try {
        const auto poll = [&] {
            if (cancelled && cancelled())
                throw Abort{FailureCode::Cancelled, "cancelled_during_layer_process"};
            if (stale && stale())
                throw Abort{FailureCode::StaleInput, "revision_changed"};
        };
        poll();
        if (input.regions.empty() || input.regions.size() > 256 || input.regions.front().id != 0 || input.material_count < 2 ||
            input.material_count > MAX_MATERIALS || input.material_count != machine.material_count || domain == 0 ||
            domain > std::numeric_limits<uint64_t>::max() / 512)
            throw Abort{FailureCode::InvalidInput, "invalid_layer_process_input"};
        if (!std::isfinite(profile.density) || profile.density < 0. || profile.density > 1. ||
            !std::isfinite(profile.sparse_angle_degrees) || !std::isfinite(profile.brim_width_mm) || profile.brim_width_mm < 0. ||
            profile.brim_width_mm > 100. || !std::isfinite(profile.brim_gap_mm) || profile.brim_gap_mm < 0. || profile.brim_gap_mm > 100. ||
            !std::isfinite(profile.top_shell_mm) || profile.top_shell_mm < 0. || !std::isfinite(profile.bottom_shell_mm) ||
            profile.bottom_shell_mm < 0. || profile.wall_loops == 0 || profile.wall_loops > 1000)
            throw Abort{FailureCode::InvalidInput, "invalid_layer_process_profile"};
        const auto cycles_result = refined_cycles ? ProcessCyclesResult(*refined_cycles) :
                                                    plan_process_cycles(input, profile, limits, cancelled);
        if (const auto* f = std::get_if<Failure>(&cycles_result))
            return *f;
        const auto& cycles = std::get<std::vector<ProcessCycle>>(cycles_result);
        if (input.solid.triangles.size() > 2000000 || input.solid.vertices.size() > 2000000)
            throw Abort{FailureCode::ResourceLimit, "section_mesh_limit"};
        indexed_triangle_set mesh;
        mesh.vertices.reserve(input.solid.vertices.size());
        mesh.indices.reserve(input.solid.triangles.size());
        for (const auto& p : input.solid.vertices) {
            if (!p.allFinite() || p.cwiseAbs().maxCoeff() > 100000.)
                throw Abort{FailureCode::InvalidInput, "invalid_layer_process_vertex"};
            mesh.vertices.push_back(p.cast<float>());
        }
        for (const auto& t : input.solid.triangles) {
            for (uint32_t index : t)
                if (index >= mesh.vertices.size())
                    throw Abort{FailureCode::InvalidInput, "invalid_layer_process_triangle"};
            mesh.indices.emplace_back(int(t[0]), int(t[1]), int(t[2]));
        }
        std::vector<float> zs;
        for (const auto& c : cycles)
            zs.push_back(float((c.bottom_mm + c.top_mm) / 2.));
        MeshSlicingParamsEx slicing;
        slicing.resolution = .005;
        // No morphological closing: preserve cavities and separate components.
        auto       outlines = slice_mesh_ex(mesh, zs, slicing, poll);
        const bool trace    = std::getenv("MATERIAL_BAND_TRACE") != nullptr;
        struct Layer
        {
            std::shared_ptr<const PathPlan> paths;
            std::vector<ExPolygons>         domains;
        };
        std::vector<Layer>  layers;
        std::vector<double> foundations(cycles.size(), 0.), required_heights(cycles.size(), 0.);
        std::vector<bool>   support_preflight(cycles.size(), false);
        size_t              retained = mesh.vertices.capacity() * sizeof(mesh.vertices.front()) +
                          mesh.indices.capacity() * sizeof(mesh.indices.front()) + outlines.capacity() * sizeof(ExPolygons);
        for (const auto& outline : outlines)
            retained += bytes(outline);
        if (retained > limits.max_temporary_bytes)
            throw Abort{FailureCode::ResourceLimit, "layer_section_memory_limit"};
        MaterialFractions source_target{};
        const double      width = input.process.bead_width_mm;
        ExPolygons        preceding_deposition;
        for (size_t c = 0; c < cycles.size(); ++c) {
            poll();
            if (trace)
                std::clog << "Material layer geometry " << c << '/' << cycles.size() << std::endl;
            const auto started = std::chrono::steady_clock::now();
            const auto stage   = [&](const char* name) {
                if (trace)
                    std::clog << name << ' ' << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
                              << std::endl;
            };
            if (outlines[c].empty())
                throw Abort{FailureCode::UnsupportedTopology, "empty_source_section"};
            auto core  = offset_ex(outlines[c], float(-scale_(profile.wall_loops * width)));
            auto above = outlines[c], below = outlines[c];
            for (size_t k = c + 1; profile.top_shell_mm > 0. && !above.empty(); ++k) {
                if (k == cycles.size()) {
                    above.clear();
                    break;
                }
                above = intersection_ex(above, outlines[k]);
                if (cycles[k].top_mm - cycles[c].top_mm >= profile.top_shell_mm)
                    break;
            }
            for (size_t k = c; profile.bottom_shell_mm > 0. && !below.empty();) {
                if (k == 0) {
                    below.clear();
                    break;
                }
                --k;
                below = intersection_ex(below, outlines[k]);
                if (cycles[c].bottom_mm - cycles[k].bottom_mm >= profile.bottom_shell_mm)
                    break;
            }
            const auto              top_solid    = intersection_ex(core, diff_ex(outlines[c], above));
            const auto              bottom_solid = diff_ex(intersection_ex(core, diff_ex(outlines[c], below)), top_solid);
            auto                    solid        = union_ex(top_solid, bottom_solid);
            auto                    sparse       = diff_ex(core, solid);
            std::vector<ContourRun> runs;
            for (size_t wall = 0; wall < profile.wall_loops; ++wall)
                contours(offset_ex(outlines[c], float(-scale_((wall + .5) * width))), width, PathRole::Perimeter, runs);
            const double height     = cycles[c].top_mm - cycles[c].bottom_mm;
            const auto   dense_fill = [&](const ExPolygons& area, const DenseInfillOptions& options) {
                ExPolygons supported, bridges;
                const auto supported_area = c == 0 ? ExPolygons{} : offset_ex(preceding_deposition, float(scale_(width / 2.)));
                for (const auto& component : area) {
                    const bool bridge = c != 0 && area_mm2(diff_ex(ExPolygons{component}, supported_area)) >
                                                      std::max(1e-6, area_mm2(ExPolygons{component}) * 1e-4);
                    (bridge ? bridges : supported).push_back(component);
                }
                fill(supported, dense_pattern(options), 1., width, cycles[c].top_mm, height, c,
                       options.angle_degrees + (options.alternate_direction && c % 2 ? 90. : 0.), PathRole::DenseFill, runs,
                       std::make_shared<const ExPolygons>(union_ex(supported, diff_ex(outlines[c], core))));
                // The first solid skin over sparse fill needs anchored bridge
                // lanes. Following supported skins retain the selected pattern.
                if (!bridges.empty()) {
                    const auto anchors       = union_ex(preceding_deposition, diff_ex(outlines[c], core));
                    const auto anchor_core   = offset_ex(anchors, float(-scale_(width / 4.)));
                    const auto center_domain = offset_ex(outlines[c], float(-scale_(width / 2.)));
                    for (const auto& component : bridges) {
                        std::vector<ContourRun>   best;
                        std::pair<size_t, double> best_score{std::numeric_limits<size_t>::max(), 0.};
                        for (double angle : {profile.sparse_angle_degrees, 0., 45., 90., 135.}) {
                            std::vector<ContourRun> candidate;
                            fill(ExPolygons{component}, ipRectilinear, 1., width, cycles[c].top_mm, height, c, angle, PathRole::Bridge,
                                   candidate);
                            if (candidate.empty())
                                continue;
                            std::pair<size_t, double> score{};
                            for (auto& run : candidate) {
                                if (!anchor_bridge_run(run, anchors, anchor_core, center_domain))
                                    ++score.first;
                                score.second += run.line.length() * SCALING_FACTOR;
                            }
                            if (score < best_score) {
                                best_score = score;
                                best       = std::move(candidate);
                            }
                            // Keep a valid configured direction. A shorter
                            // unpainted bridge can acquire worse endpoints when
                            // it is later divided by material boundaries.
                            if (best_score.first == 0)
                                break;
                        }
                        runs.insert(runs.end(), std::make_move_iterator(best.begin()), std::make_move_iterator(best.end()));
                    }
                }
            };
            dense_fill(top_solid, profile.dense.top);
            dense_fill(bottom_solid, profile.dense.bottom);
            if (profile.density == 1.)
                dense_fill(sparse, profile.dense.internal);
            else
                fill(sparse, profile.sparse_pattern, profile.density, width, cycles[c].top_mm, height, c, profile.sparse_angle_degrees,
                     PathRole::SparseFill, runs);
            stage("contours");
            if (trace && c < 10) {
                std::array<size_t, 5> counts{};
                for (const auto& run : runs)
                    ++counts[size_t(run.role)];
                std::clog << "Material source areas cycle=" << c << " core=" << area_mm2(core) << " solid=" << area_mm2(solid)
                          << " sparse=" << area_mm2(sparse) << " runs=";
                for (size_t n : counts)
                    std::clog << n << ',';
                std::clog << std::endl;
            }
            anchor_perimeter_seams(runs, preceding_deposition, poll);
            auto geometry = plan_contour_cycle(outlines[c], runs, {domain, c, {1}, 0}, cycles[c].bottom_mm, cycles[c].top_mm,
                                               input.revision, input.process, input.tolerances, machine, limits, cancelled, stale,
                                               profile.density == 1. ? core : solid);
            if (auto* f = std::get_if<Failure>(&geometry))
                return *f;
            auto paths = std::get<std::shared_ptr<const PathPlan>>(geometry);
            stage("beads");
            const auto occupied = deposition(*paths);
            if (area_mm2(diff_ex(solid, occupied)) > std::max(1e-6, area_mm2(solid) * .01)) {
                if (trace)
                    std::clog << "Material solid missing cycle=" << c << " area=" << area_mm2(diff_ex(solid, occupied))
                              << " target=" << area_mm2(solid) << std::endl;
                if (const char* directory = std::getenv("MATERIAL_BAND_GEOMETRY_DUMP")) {
                    std::ofstream dump(std::string(directory) + "/source-coverage.json");
                    dump << std::setprecision(17) << "{\"cycle\":" << c;
                    const auto write = [&](const char* key, const ExPolygons& polygons) {
                        dump << ",\"" << key << "\":[";
                        for (size_t p = 0; p < polygons.size(); ++p) {
                            dump << (p ? "," : "") << '[';
                            const auto ring = [&](const Polygon& poly) {
                                dump << '[';
                                for (size_t q = 0; q < poly.points.size(); ++q)
                                    dump << (q ? "," : "") << '[' << unscale<double>(poly.points[q].x()) << ','
                                         << unscale<double>(poly.points[q].y()) << ']';
                                dump << ']';
                            };
                            ring(polygons[p].contour);
                            for (const auto& hole : polygons[p].holes) {
                                dump << ',';
                                ring(hole);
                            }
                            dump << ']';
                        }
                        dump << ']';
                    };
                    write("outline", outlines[c]);
                    write("solid", solid);
                    write("occupied", occupied);
                    dump << ",\"runs\":[";
                    for (size_t r = 0; r < runs.size(); ++r) {
                        dump << (r ? "," : "") << "{\"role\":" << int(runs[r].role) << ",\"points\":[";
                        for (size_t p = 0; p < runs[r].line.points.size(); ++p)
                            dump << (p ? "," : "") << '[' << unscale<double>(runs[r].line.points[p].x()) << ','
                                 << unscale<double>(runs[r].line.points[p].y()) << ']';
                        dump << "]}";
                    }
                    dump << "]}";
                }
                throw Abort{FailureCode::MaterialToleranceExceeded, "layer_solid_infill_coverage:cycle=" + std::to_string(c)};
            }
            if (trace && c < 10)
                std::clog << "Material source occupancy cycle=" << c << " area=" << area_mm2(occupied) << " paths=" << paths->paths.size()
                          << std::endl;
            auto domains = partition_painted_domains(input, outlines[c], cycles[c].bottom_mm, cycles[c].top_mm, profile.wall_loops * width,
                                                     profile.top_shell_mm, profile.bottom_shell_mm, poll);
            stage("paint");
            if (c != 0)
                support_preflight[c] = area_mm2(diff_ex(occupied, offset_ex(preceding_deposition, float(scale_(width / 2.))))) > 1e-6;
            if (c != 0 && input.regions.size() > 1) {
                Polygons bridge_cells;
                for (const auto& path : paths->paths)
                    if (path.role == PathRole::Bridge)
                        for (const auto& segment : path.segments)
                            for (const auto& cell : segment.cells) {
                                Points points;
                                for (const auto& p : cell.upper)
                                    points.emplace_back(scale_(p.x()), scale_(p.y()));
                                bridge_cells.emplace_back(std::move(points));
                            }
                const auto bridges = union_ex(bridge_cells);
                // Paint can also divide a bridging wall or a curved sparse
                // lane. Their roles remain Perimeter/SparseFill, so looking
                // only for solid bridge infill misses these broken sequences.
                size_t              bridge_regions = 0, active_regions = 0, common_material = MAX_MATERIALS;
                bool                compatible = true;
                std::vector<double> first_heights;
                for (size_t r = 0; r < domains.size(); ++r) {
                    const auto active = intersection_ex(domains[r], occupied);
                    if (area_mm2(active) <= 1e-9)
                        continue;
                    ++active_regions;
                    bridge_regions += area_mm2(intersection_ex(active, bridges)) > 1e-6;
                    const auto recipe = sample_height_gradient(input.regions[r],
                                                               (cycles[c].bottom_mm + cycles[c].top_mm) / (2. * cycles.back().top_mm));
                    const auto first  = std::find_if(recipe.begin(), recipe.end(), [](double f) { return f > 0.; });
                    if (first == recipe.end())
                        continue;
                    const size_t material = size_t(first - recipe.begin());
                    if (common_material == MAX_MATERIALS)
                        common_material = material;
                    compatible &= material == common_material;
                    first_heights.push_back(*first * height);
                }
                if (compatible && !first_heights.empty() &&
                    (bridge_regions > 1 || (support_preflight[c] && active_regions > 1) ||
                     height > input.process.max_active_thickness_mm + input.tolerances.interface_mm)) {
                    const double minimum  = input.process.min_active_thickness_mm;
                    const double smallest = *std::min_element(first_heights.begin(), first_heights.end());
                    for (double foundation : {smallest, minimum}) {
                        bool fits = foundation >= minimum - input.tolerances.interface_mm &&
                                    foundation <= input.process.max_active_thickness_mm + input.tolerances.interface_mm;
                        for (double first : first_heights) {
                            const double remaining = first - foundation;
                            fits &= std::abs(remaining) <= input.tolerances.interface_mm ||
                                    remaining >= minimum - input.tolerances.interface_mm;
                        }
                        if (fits) {
                            foundations[c] = foundation;
                            break;
                        }
                    }
                    if (foundations[c] == 0.)
                        required_heights[c] = 2. * minimum * height / smallest;
                }
            }
            preceding_deposition = occupied;
            for (size_t r = 0; r < domains.size(); ++r) {
                const double volume    = area_mm2(intersection_ex(domains[r], occupied)) * height;
                const auto   fractions = sample_height_gradient(input.regions[r],
                                                                (cycles[c].bottom_mm + cycles[c].top_mm) / (2. * cycles.back().top_mm));
                for (size_t m = 0; m < MAX_MATERIALS; ++m)
                    source_target[m] += volume * fractions[m];
            }
            for (auto& d : domains)
                d = intersection_ex(d, occupied);
            retained += bytes(*paths);
            for (const auto& d : domains)
                retained += bytes(d);
            retained += domains.capacity() * sizeof(ExPolygons);
            if (retained > limits.max_temporary_bytes)
                throw Abort{FailureCode::ResourceLimit, "layer_template_memory_limit"};
            layers.push_back({std::move(paths), std::move(domains)});
        }
        if (const char* directory = std::getenv("MATERIAL_BAND_GEOMETRY_DUMP")) {
            std::ofstream dump(std::string(directory) + "/process-cycles-" + std::to_string(refinement) + ".json");
            dump << std::setprecision(17) << '[';
            for (size_t c = 0; c < cycles.size(); ++c)
                dump << (c ? "," : "") << '[' << cycles[c].bottom_mm << ',' << cycles[c].top_mm << ',' << foundations[c] << ','
                     << required_heights[c] << ']';
            dump << "]\n";
        }
        const auto refine_cycles = [&](const std::vector<double>& requests) -> BoxPlanResult {
            if (refinement >= 4)
                throw Abort{FailureCode::InfeasibleThickness, "bridge_cycle_refinement_limit"};
            std::vector<ProcessCycle> merged;
            for (size_t c = 0; c < cycles.size();) {
                ProcessCycle cycle    = cycles[c];
                double       required = requests[c];
                ++c;
                while (cycle.top_mm - cycle.bottom_mm < required - input.tolerances.interface_mm && c < cycles.size()) {
                    cycle.top_mm = cycles[c].top_mm;
                    required     = std::max(required, requests[c]);
                    ++c;
                }
                if (cycle.top_mm - cycle.bottom_mm < required - input.tolerances.interface_mm ||
                    // A new bridge can meet an already merged neighbor. Bound
                    // the aggregate cycle separately from each physical pass:
                    // at most three maximum-height remainders plus the shared
                    // first pass, whose thickness is deducted from that total.
                    cycle.top_mm - cycle.bottom_mm > 3. * input.process.max_active_thickness_mm + input.tolerances.interface_mm)
                    throw Abort{FailureCode::InfeasibleThickness,
                                "bridge_cycle_cannot_fit_common_foundation:bottom=" + std::to_string(cycle.bottom_mm) +
                                    ":top=" + std::to_string(cycle.top_mm) + ":required=" + std::to_string(required)};
                merged.push_back(cycle);
            }
            if (trace)
                std::clog << "Material bridge cycle refinement " << cycles.size() << " -> " << merged.size() << std::endl;
            // Discard construction caches before rebuilding the refined mesh
            // sections. The first layer and all material targets remain owned
            // by this kernel; no legacy gradient state participates.
            layers.clear();
            outlines.clear();
            mesh.vertices.clear();
            mesh.indices.clear();
            return plan_layer_process_impl(input, machine, domain, profile, limits, cancelled, stale, &merged, refinement + 1);
        };
        std::vector<std::vector<MaterialFractions>> fractions(input.regions.size(), std::vector<MaterialFractions>(cycles.size()));
        MaterialFractions                           target{};
        auto                                        dose_process = input.process;
        // A refined cycle can contain several passes of one material. The dose
        // solve constrains their aggregate; partitioning checks every actual
        // deposition pass against the original physical thickness limits.
        for (const auto& cycle : cycles)
            dose_process.max_active_thickness_mm = std::max(dose_process.max_active_thickness_mm, cycle.top_mm - cycle.bottom_mm);
        for (size_t r = 0; r < input.regions.size(); ++r) {
            std::vector<StackDoseCycle> doses;
            std::vector<size_t>         active;
            for (size_t c = 0; c < cycles.size(); ++c) {
                const double height = cycles[c].top_mm - cycles[c].bottom_mm;
                const double volume = area_mm2(layers[c].domains[r]) * height;
                if (volume <= 1e-9)
                    continue;
                doses.push_back(
                    {sample_height_gradient(input.regions[r], (cycles[c].bottom_mm + cycles[c].top_mm) / (2. * cycles.back().top_mm)),
                     height, volume});
                active.push_back(c);
            }
            if (doses.empty())
                continue;
            auto solved = solve_stack_doses(doses, input.material_count, dose_process,
                                            input.regions[r].height_gradient.empty() ? 0. : input.gradient_fraction_error, input.tolerances,
                                            input.revision, {}, cancelled, stale);
            if (auto* f = std::get_if<Failure>(&solved))
                return *f;
            const auto& solution = *std::get<std::shared_ptr<const StackDoseSolution>>(solved);
            for (size_t i = 0; i < active.size(); ++i)
                fractions[r][active[i]] = solution.fractions[i];
            for (size_t m = 0; m < MAX_MATERIALS; ++m)
                target[m] += solution.target_volume_mm3[m];
        }
        auto plan                  = std::make_shared<PathPlan>();
        plan->revision             = input.revision;
        plan->machine              = machine;
        plan->ordered_region_paths = plan->layered_process = true;
        plan->solid_min                                    = layers.front().paths->solid_min;
        plan->solid_max                                    = layers.back().paths->solid_max;
        MaterialFractions missing_volume{}, excess_volume{};
        size_t            generated_segments = 0, generated_cells = 0;
        const auto        append = [&](const PathPlan& source) {
            if (source.paths.size() > limits.max_paths - plan->paths.size())
                throw Abort{FailureCode::ResourceLimit, "layer_path_count_limit"};
            for (const auto& path : source.paths) {
                if (path.segments.size() > limits.max_segments - generated_segments)
                    throw Abort{FailureCode::ResourceLimit, "layer_segment_count_limit"};
                generated_segments += path.segments.size();
                for (const auto& segment : path.segments)
                    generated_cells += segment.cells.size();
            }
            plan->solid_min = plan->solid_min.cwiseMin(source.solid_min);
            plan->solid_max = plan->solid_max.cwiseMax(source.solid_max);
            // XY triangles are a construction cache for clipping and cap repair.
            // Completed geometry owns the same footprint in its 3D bead cells;
            // scheduling and export use those cells. Release the duplicate cache
            // before retaining hundreds of completed cycles.
            PathPlan completed = source;
            for (auto& path : completed.paths)
                for (auto& segment : path.segments)
                    std::vector<std::array<Vec2d, 3>>().swap(segment.coverage_triangles);
            retained += bytes(completed);
            if (retained > limits.max_temporary_bytes)
                throw Abort{FailureCode::ResourceLimit, "layer_path_memory_limit"};
            plan->paths.insert(plan->paths.end(), std::make_move_iterator(completed.paths.begin()),
                                      std::make_move_iterator(completed.paths.end()));
            plan->interfaces.insert(plan->interfaces.end(), std::make_move_iterator(completed.interfaces.begin()),
                                           std::make_move_iterator(completed.interfaces.end()));
            for (size_t m = 0; m < MAX_MATERIALS; ++m)
                plan->volume_mm3[m] += source.volume_mm3[m];
            plan->intentional_void_mm3 += source.intentional_void_mm3;
        };
        preceding_deposition.clear();
        std::vector<BeadCell> preceding_cells;
        std::vector<double>   refinement_requests(cycles.size(), 0.);
        for (size_t c = 0; c < cycles.size(); ++c) {
            poll();
            if (trace)
                std::clog << "Material layer partition " << c << '/' << cycles.size() << std::endl;
            std::vector<PlanarMaterialBand> bands;
            for (size_t r = 0; r < input.regions.size(); ++r) {
                double      z     = cycles[c].bottom_mm;
                InterfaceId lower = 0;
                for (size_t m = 0; m < input.material_count; ++m) {
                    const double f = fractions[r][c][m];
                    if (f == 0.)
                        continue;
                    const bool last = std::none_of(fractions[r][c].begin() + m + 1, fractions[r][c].end(), [](double x) { return x > 0.; });
                    const double      top   = last ? cycles[c].top_mm : z + (cycles[c].top_mm - cycles[c].bottom_mm) * f;
                    const InterfaceId upper = last ? 0 : 1 + (c * input.regions.size() + r) * MAX_MATERIALS + m;
                    bands.push_back({{domain * 512 + r + 1, c, {uint32_t(m + 1)}, uint32_t(m)}, layers[c].domains[r], z, top, lower, upper});
                    z     = top;
                    lower = upper;
                }
            }
            CoverageLimits coverage;
            coverage.minimum_bead_width_mm = .04;
            coverage.max_refinement_depth  = 8;
            coverage.max_candidate_cells   = limits.max_segments * 4;
            const bool refined             = cycles[c].top_mm - cycles[c].bottom_mm >
                                 input.process.max_active_thickness_mm + input.tolerances.interface_mm;
            auto result = plan_partitioned_cycle(*layers[c].paths, bands, input.process, input.tolerances, limits, coverage, cancelled,
                                                 stale, preceding_deposition, cycles[c].bottom_mm, refined ? foundations[c] : 0.);
            if (auto* f = std::get_if<Failure>(&result))
                return *f;
            if (support_preflight[c] || foundations[c] > 0. || required_heights[c] > 0.) {
                // Preserve a manufacturable painted sequence. A common pass
                // or a larger cycle is requested only after the actual bead
                // scheduler rejects that sequence, not from paint adjacency.
                const auto support_failure = [&](BoxPlanResult& candidate) -> std::optional<Failure> {
                    auto ordered = std::make_shared<PathPlan>(*std::get<std::shared_ptr<const PathPlan>>(candidate));
                    std::stable_sort(ordered->paths.begin(), ordered->paths.end(), [](const BandPath& a, const BandPath& b) {
                        return std::make_tuple(std::llround(a.vertices.front().z() * 1e8), a.band.material.value, a.band.domain) <
                               std::make_tuple(std::llround(b.vertices.front().z() * 1e8), b.band.material.value, b.band.domain);
                    });
                    for (size_t i = 0; i < ordered->paths.size(); ++i)
                        ordered->paths[i].id = i + 1;
                    ScheduleInput check;
                    check.revision                = input.revision;
                    check.machine                 = machine;
                    check.objects                 = {{1, ordered}};
                    check.occupied                = preceding_cells;
                    check.allow_planar_reordering = true;
                    check.initial_position        = Vec3d(.5, 1., cycles.back().top_mm + 2.);
                    ScheduleLimits check_limits;
                    check_limits.max_candidates   = 100000000;
                    check_limits.max_index_visits = 500000000;
                    auto checked                  = schedule_paths(check, check_limits, cancelled, stale);
                    if (const auto* failure = std::get_if<Failure>(&checked))
                        return *failure;
                    const auto&           schedule = *std::get<std::shared_ptr<const Schedule>>(checked);
                    std::vector<BandPath> reordered;
                    reordered.reserve(ordered->paths.size());
                    for (const auto& item : schedule.order) {
                        reordered.push_back(std::move(ordered->paths.at(item.operation.path - 1)));
                        reordered.back().id = reordered.size();
                    }
                    ordered->paths = std::move(reordered);
                    candidate      = std::shared_ptr<const PathPlan>(std::move(ordered));
                    return {};
                };
                auto failure = support_failure(result);
                if (failure && failure->detail == "unsupported_bead_footprint" && !refined && foundations[c] > 0.) {
                    result = plan_partitioned_cycle(*layers[c].paths, bands, input.process, input.tolerances, limits, coverage, cancelled,
                                                    stale, preceding_deposition, cycles[c].bottom_mm, foundations[c]);
                    if (auto* error = std::get_if<Failure>(&result))
                        return *error;
                    failure = support_failure(result);
                }
                if (failure) {
                    if (failure->detail == "unsupported_bead_footprint" && !refined && required_heights[c] > 0.)
                        refinement_requests[c] = required_heights[c];
                    else {
                        if (std::getenv("MATERIAL_BAND_CAPTURE_CYCLE")) {
                            PathPlan    failed    = *plan;
                            const auto& candidate = *std::get<std::shared_ptr<const PathPlan>>(result);
                            failed.paths.insert(failed.paths.end(), candidate.paths.begin(), candidate.paths.end());
                            failed.solid_max = failed.solid_max.cwiseMax(candidate.solid_max);
                            capture_cycle_paths(failed, c, false, true);
                        }
                        return *failure;
                    }
                }
                if (trace)
                    std::clog << "Material bridge preflight cycle=" << c << " refinement=" << refinement_requests[c]
                              << " accepted=" << !failure << std::endl;
            }
            retained -= bytes(*layers[c].paths);
            layers[c].paths.reset();
            for (const auto& d : layers[c].domains)
                retained -= bytes(d);
            retained -= layers[c].domains.capacity() * sizeof(ExPolygons);
            std::vector<ExPolygons>().swap(layers[c].domains);
            const auto& completed = *std::get<std::shared_ptr<const PathPlan>>(result);
            preceding_deposition  = deposition(completed, cycles[c].top_mm);
            preceding_cells.clear();
            for (const auto& path : completed.paths)
                for (const auto& segment : path.segments)
                    for (const auto& cell : segment.cells)
                        if (std::abs(cell.upper.front().z() - cycles[c].top_mm) <= input.tolerances.interface_mm)
                            preceding_cells.push_back(cell);
            for (size_t m = 0; m < MAX_MATERIALS; ++m) {
                missing_volume[m] += completed.coverage[m].missing_mm3;
                excess_volume[m] += completed.coverage[m].excess_mm3;
            }
            append(completed);
            capture_cycle_paths(*plan, c, c + 1 == cycles.size());
            if (trace && (c % 25 == 0 || c + 1 == cycles.size()))
                std::clog << "Material layer retained paths=" << plan->paths.size() << " segments=" << generated_segments
                          << " cells=" << generated_cells << " bytes=" << retained << std::endl;
        }
        if (std::any_of(refinement_requests.begin(), refinement_requests.end(), [](double h) { return h > 0.; })) {
            plan.reset();
            preceding_cells.clear();
            return refine_cycles(refinement_requests);
        }
        // Band integration and manufactured path coverage are different oracles.
        // Keep analytic recipe conservation at 1e-4 relative; apply the declared
        // path-coverage budget to omissions, without allowing compensating excess.
        const double missing_budget  = CoverageLimits{}.relative_missing_volume;
        const double intended_total  = std::accumulate(source_target.begin(), source_target.end(), 0.);
        const double deposited_total = std::accumulate(plan->volume_mm3.begin(), plan->volume_mm3.end(), 0.);
        for (size_t m = 0; m < MAX_MATERIALS; ++m) {
            auto& coverage         = plan->coverage[m];
            coverage.target_mm3    = source_target[m];
            coverage.deposited_mm3 = plan->volume_mm3[m];
            coverage.missing_mm3   = missing_volume[m];
            coverage.excess_mm3    = excess_volume[m];
            if (trace)
                std::clog << "Material layer dose material=" << m + 1 << " requested=" << source_target[m] << " band=" << target[m]
                          << " deposited=" << plan->volume_mm3[m] << " missing=" << coverage.missing_mm3
                          << " excess=" << coverage.excess_mm3 << std::endl;
            if (std::abs(target[m] - source_target[m]) >
                std::max(input.tolerances.absolute_volume_mm3, source_target[m] * input.tolerances.relative_volume))
                throw Abort{FailureCode::MaterialToleranceExceeded, "paint_projection_recipe_volume_budget"};
            if (coverage.missing_mm3 > std::max(input.tolerances.absolute_volume_mm3, source_target[m] * missing_budget) ||
                coverage.excess_mm3 > std::max(input.tolerances.absolute_volume_mm3, source_target[m] * input.tolerances.relative_volume))
                throw Abort{FailureCode::MaterialToleranceExceeded, "layer_material_path_coverage_budget"};
            if (intended_total > input.tolerances.absolute_volume_mm3 &&
                (deposited_total <= 0. ||
                 std::abs(source_target[m] / intended_total - plan->volume_mm3[m] / deposited_total) > input.tolerances.plateau_fraction))
                throw Abort{FailureCode::MaterialToleranceExceeded, "layer_material_fraction_budget"};
        }
        if (profile.brim_width_mm > 0.) {
            ExPolygons brim_area, outer, holes;
            for (const auto& p : outlines.front()) {
                outer.emplace_back(p.contour);
                for (auto h : p.holes) {
                    h.make_counter_clockwise();
                    holes.emplace_back(std::move(h));
                }
            }
            std::vector<ContourRun> runs;
            const size_t            rings = size_t(std::ceil(profile.brim_width_mm / width));
            const double            pitch = profile.brim_width_mm / rings;
            if (profile.outer_brim) {
                brim_area = diff_ex(offset_ex(outer, float(scale_(profile.brim_gap_mm + profile.brim_width_mm))),
                                    offset_ex(outer, float(scale_(profile.brim_gap_mm))));
                for (size_t i = 0; i < rings; ++i)
                    contours(offset_ex(outer, float(scale_(profile.brim_gap_mm + (i + .5) * pitch))), pitch, PathRole::Brim, runs);
            }
            if (profile.inner_brim) {
                brim_area = union_ex(brim_area, diff_ex(offset_ex(holes, float(-scale_(profile.brim_gap_mm))),
                                                        offset_ex(holes, float(-scale_(profile.brim_gap_mm + profile.brim_width_mm)))));
                for (size_t i = 0; i < rings; ++i)
                    contours(offset_ex(holes, float(-scale_(profile.brim_gap_mm + (i + .5) * pitch))), pitch, PathRole::Brim, runs);
            }
            brim_area = diff_ex(brim_area, offset_ex(outlines.front(), float(scale_(profile.brim_gap_mm))));
            const size_t physical = size_t(std::max_element(input.regions.front().fractions.begin(), input.regions.front().fractions.end()) -
                                           input.regions.front().fractions.begin());
            if (!brim_area.empty()) {
                auto brim = plan_contour_cycle(brim_area, runs, {domain * 512 + 511, 0, {uint32_t(physical + 1)}, uint32_t(physical)}, 0.,
                                               cycles.front().top_mm, input.revision, input.process, input.tolerances, machine, limits,
                                               cancelled, stale);
                if (auto* f = std::get_if<Failure>(&brim))
                    return *f;
                append(*std::get<std::shared_ptr<const PathPlan>>(brim));
            }
        }
        std::stable_sort(plan->paths.begin(), plan->paths.end(), [](const BandPath& a, const BandPath& b) {
            return std::make_tuple(a.band.macro_cycle, std::llround(a.vertices.front().z() * 1e8), a.band.material.value, a.band.domain) <
                   std::make_tuple(b.band.macro_cycle, std::llround(b.vertices.front().z() * 1e8), b.band.material.value, b.band.domain);
        });
        if (plan->paths.size() > limits.max_paths)
            throw Abort{FailureCode::ResourceLimit, "layer_path_count_limit"};
        for (size_t i = 0; i < plan->paths.size(); ++i)
            plan->paths[i].id = i + 1;
        poll();
        return std::shared_ptr<const PathPlan>(std::move(plan));
    } catch (const Failure& error) {
        return error;
    } catch (const Abort& error) {
        return Failure{input.revision, error.code, error.detail, {}, {}, {}};
    } catch (const std::bad_alloc&) {
        return Failure{input.revision, FailureCode::ResourceLimit, "layer_process_allocation", {}, {}, {}};
    } catch (const std::exception& error) {
        return Failure{input.revision, FailureCode::InvalidInput, std::string("layer_process:") + error.what(), {}, {}, {}};
    }
}

BoxPlanResult plan_layer_process(const KernelInput&           input,
                                 const MachineEnvelope&       machine,
                                 DomainId                     domain,
                                 const LayerProcessProfile&   profile,
                                 const PathLimits&            limits,
                                 const std::function<bool()>& cancelled,
                                 const std::function<bool()>& stale)
{
    return plan_layer_process_impl(input, machine, domain, profile, limits, cancelled, stale, nullptr, 0);
}
} // namespace Slic3r::MaterialBand
