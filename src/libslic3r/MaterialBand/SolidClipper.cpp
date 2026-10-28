#include "SolidClipper.hpp"

#undef PI
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Polygon_mesh_processing/measure.h>
#include <CGAL/Polygon_mesh_processing/orientation.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Side_of_triangle_mesh.h>
#include <CGAL/AABB_face_graph_triangle_primitive.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_tree.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <new>
#include <stdexcept>
#include <tuple>

namespace Slic3r::MaterialBand {
namespace {
using Kernel    = CGAL::Exact_predicates_exact_constructions_kernel;
using Surface   = CGAL::Surface_mesh<Kernel::Point_3>;
using Primitive = CGAL::AABB_face_graph_triangle_primitive<Surface>;
using Tree      = CGAL::AABB_tree<CGAL::AABB_traits<Kernel, Primitive>>;
namespace PMP   = CGAL::Polygon_mesh_processing;

struct Abort
{
    FailureCode code;
    const char* detail;
};

Surface convert(const Mesh& input)
{
    Surface result;
    for (const auto& vertex : input.vertices) {
        if (!vertex.allFinite())
            throw Abort{FailureCode::InvalidInput, "nonfinite_solid_vertex"};
        result.add_vertex({vertex.x(), vertex.y(), vertex.z()});
    }
    for (const auto& face : input.triangles) {
        for (uint32_t index : face)
            if (index >= input.vertices.size())
                throw Abort{FailureCode::InvalidInput, "solid_vertex_index"};
        if ((input.vertices[face[1]] - input.vertices[face[0]]).cross(input.vertices[face[2]] - input.vertices[face[0]]).squaredNorm() == 0.)
            throw Abort{FailureCode::InvalidInput, "degenerate_solid_face"};
        if (result.add_face(Surface::Vertex_index(face[0]), Surface::Vertex_index(face[1]), Surface::Vertex_index(face[2])) ==
            Surface::null_face())
            throw Abort{FailureCode::InvalidInput, "nonmanifold_or_unoriented_solid"};
    }
    if (result.is_empty() || !CGAL::is_closed(result) || PMP::does_self_intersect(result) || !PMP::does_bound_a_volume(result) ||
        CGAL::to_double(PMP::volume(result)) <= 0.)
        throw Abort{FailureCode::InvalidInput, "invalid_closed_solid"};
    return result;
}

Mesh convert(const Surface& input, size_t max_faces, const Vec3d& origin, double quantum)
{
    if (input.number_of_faces() > max_faces || input.number_of_vertices() > max_faces * 3)
        throw Abort{FailureCode::ResourceLimit, "clipped_output_limit"};
    Mesh                                       result;
    std::map<Surface::Vertex_index, uint32_t>  indices;
    std::map<std::array<int64_t, 3>, uint32_t> welded;
    for (const auto vertex : input.vertices()) {
        const auto& p = input.point(vertex);
        const Vec3d point(CGAL::to_double(p.x()), CGAL::to_double(p.y()), CGAL::to_double(p.z()));
        const Vec3d grid = (point - origin) / quantum;
        if (!grid.allFinite() || grid.cwiseAbs().maxCoeff() > 1e15)
            throw Abort{FailureCode::InvalidInput, "clipped_coordinate_range"};
        const std::array<int64_t, 3> key{{std::llround(grid.x()), std::llround(grid.y()), std::llround(grid.z())}};
        auto                         found = welded.find(key);
        if (found == welded.end()) {
            // Equal exact intersections can straddle adjacent rounding cells.
            // A same-cell lookup alone retains folded nanometer triangles on
            // repeatedly clipped coplanar caps. Never merge farther than two
            // quanta from the retained vertex; no transitive clustering.
            double nearest = 4. * quantum * quantum;
            for (int x = -1; x <= 1; ++x)
                for (int y = -1; y <= 1; ++y)
                    for (int z = -1; z <= 1; ++z) {
                        const auto candidate = welded.find({{key[0] + x, key[1] + y, key[2] + z}});
                        if (candidate == welded.end())
                            continue;
                        const double distance = (point - result.vertices[candidate->second]).squaredNorm();
                        if (distance <= nearest) {
                            found   = candidate;
                            nearest = distance;
                        }
                    }
        }
        if (found == welded.end()) {
            found = welded.emplace(key, uint32_t(result.vertices.size())).first;
            result.vertices.push_back(origin + quantum * Vec3d(double(key[0]), double(key[1]), double(key[2])));
        }
        indices.emplace(vertex, found->second);
    }
    for (const auto face : input.faces()) {
        std::array<uint32_t, 3> triangle{};
        size_t                  i = 0;
        for (auto vertex : CGAL::vertices_around_face(input.halfedge(face), input)) {
            if (i == 3)
                throw Abort{FailureCode::InvalidInput, "nontriangular_boolean_output"};
            triangle[i++] = indices.at(vertex);
        }
        if (i != 3)
            throw Abort{FailureCode::InvalidInput, "nontriangular_boolean_output"};
        if (triangle[0] != triangle[1] && triangle[1] != triangle[2] && triangle[0] != triangle[2] &&
            (result.vertices[triangle[1]] - result.vertices[triangle[0]])
                    .cross(result.vertices[triangle[2]] - result.vertices[triangle[0]])
                    .squaredNorm() > 0.)
            result.triangles.push_back(triangle);
    }
    return result;
}

struct Work
{
    size_t                       elements = 0, limit = 0;
    const std::function<bool()>& cancelled;
    const std::function<bool()>& stale;
    void                         check()
    {
        if (cancelled && cancelled())
            throw Abort{FailureCode::Cancelled, "cancelled_during_clipping"};
        if (stale && stale())
            throw Abort{FailureCode::StaleInput, "revision_changed"};
        if (++elements > limit)
            throw Abort{FailureCode::ResourceLimit, "boolean_refinement_limit"};
    }
};
struct Visitor : PMP::Corefinement::Default_visitor<Surface>
{
    Work* work;
    explicit Visitor(Work& value) : work(&value) {}
    void before_subface_created(Surface&) { work->check(); }
    void before_face_copy(Surface::Face_index, const Surface&, Surface&) { work->check(); }
    void before_vertex_copy(Surface::Vertex_index, const Surface&, Surface&) { work->check(); }
    void new_vertex_added(size_t, Surface::Vertex_index, const Surface&) { work->check(); }
    void intersection_point_detected(
        size_t, int, Surface::Halfedge_index, Surface::Halfedge_index, const Surface&, const Surface&, bool, bool)
    {
        work->check();
    }
};

Mesh column(const Mesh& upper, double bottom)
{
    Mesh           result;
    const uint32_t n = uint32_t(upper.vertices.size());
    result.vertices  = upper.vertices;
    for (const auto& p : upper.vertices)
        result.vertices.emplace_back(p.x(), p.y(), bottom);
    std::map<std::pair<uint32_t, uint32_t>, std::pair<size_t, std::pair<uint32_t, uint32_t>>> edges;
    for (const auto& face : upper.triangles) {
        const Vec3d normal = (upper.vertices[face[1]] - upper.vertices[face[0]]).cross(upper.vertices[face[2]] - upper.vertices[face[0]]);
        if (normal.z() <= 0.)
            throw Abort{FailureCode::InvalidInput, "invalid_interface_orientation"};
        result.triangles.push_back(face);
        result.triangles.push_back({face[2] + n, face[1] + n, face[0] + n});
        for (size_t i = 0; i < 3; ++i) {
            const auto a = face[i], b = face[(i + 1) % 3];
            auto&      edge = edges[{std::min(a, b), std::max(a, b)}];
            ++edge.first;
            edge.second = {a, b};
        }
    }
    for (const auto& entry : edges) {
        if (entry.second.first == 2)
            continue;
        if (entry.second.first != 1)
            throw Abort{FailureCode::InvalidInput, "nonmanifold_interface"};
        const auto [a, b] = entry.second.second;
        result.triangles.push_back({a, a + n, b + n});
        result.triangles.push_back({a, b + n, b});
    }
    return result;
}

Surface intersection(Surface a, Surface b, Work& work)
{
    work.check();
    Surface result;
    if (!PMP::corefine_and_compute_intersection(a, b, result, CGAL::parameters::visitor(Visitor(work))))
        throw Abort{FailureCode::UnsupportedTopology, "nonmanifold_clipped_intersection"};
    return result;
}

double volume(const Surface& mesh) { return mesh.is_empty() ? 0. : CGAL::to_double(PMP::volume(mesh)); }

Surface open_surface(const Mesh& field)
{
    Surface surface;
    for (const auto& point : field.vertices)
        surface.add_vertex({point.x(), point.y(), point.z()});
    for (const auto& face : field.triangles)
        surface.add_face(Surface::Vertex_index(face[0]), Surface::Vertex_index(face[1]), Surface::Vertex_index(face[2]));
    return surface;
}
} // namespace

ClippingResult clip_bands(const Mesh&                  solid,
                          const SlabProblem&           p,
                          const SlabSolution&          solution,
                          const StackPhase&            phase,
                          double                       target,
                          const ClippingLimits&        limits,
                          const std::function<bool()>& cancelled,
                          const std::function<bool()>& stale,
                          std::optional<double>        uniform_domain_fraction)
{
    const auto failure = [&](FailureCode code, const char* detail) -> ClippingResult {
        return Failure{p.revision, code, detail, {}, {}, {}};
    };
    try {
        if (solid.triangles.size() > limits.max_input_faces || p.triangles.size() > limits.max_input_faces ||
            solid.vertices.size() > limits.max_input_faces * 3 || p.vertices.size() > limits.max_input_faces * 3)
            return failure(FailureCode::ResourceLimit, "clipping_input_limit");
        if (solution.revision != p.revision || solution.interface.id != p.interface_id || solution.interface.domain != p.domain ||
            solution.interface.macro_cycle != p.macro_cycle || solution.interface.surface.vertices.size() != p.vertices.size() ||
            solution.interface.surface.triangles != p.triangles || p.local_fraction_error.size() != p.vertices.size() ||
            p.target_fraction_a.size() != p.vertices.size())
            return failure(FailureCode::StaleInput, "interface_input_mismatch");
        for (const auto& face : solution.interface.surface.triangles)
            for (auto vertex : face)
                if (vertex >= p.vertices.size())
                    return failure(FailureCode::InvalidInput, "interface_vertex_index");
        for (const auto& point : solution.interface.surface.vertices)
            if (!point.allFinite())
                return failure(FailureCode::InvalidInput, "nonfinite_interface");
        if (!std::isfinite(target) || target <= 0. || !std::isfinite(p.bottom_mm) || !std::isfinite(p.top_mm) || p.top_mm <= p.bottom_mm)
            return failure(FailureCode::InvalidInput, "invalid_clipped_target");
        for (double value : {p.tolerances.interface_mm, p.tolerances.absolute_volume_mm3, p.process.min_active_thickness_mm,
                             p.process.max_active_thickness_mm})
            if (!std::isfinite(value) || value <= 0.)
                return failure(FailureCode::InvalidInput, "invalid_clipping_constraint");
        if (!std::isfinite(p.tolerances.relative_volume) || p.tolerances.relative_volume < 0. ||
            p.process.min_active_thickness_mm > p.process.max_active_thickness_mm)
            return failure(FailureCode::InvalidInput, "invalid_clipping_constraint");
        if (phase.order[0].value == 0 || phase.order[1].value == 0 || phase.order[0].value == phase.order[1].value ||
            phase.cadence != std::array<uint32_t, 2>{{1, 1}} || phase.slot != 0 || p.macro_cycle < phase.first_active_cycle)
            return failure(FailureCode::IncompatibleOrder, "unsupported_clipping_phase");
        const size_t input_elements = solid.triangles.size() + solid.vertices.size() + 8 * (p.triangles.size() + p.vertices.size());
        if (input_elements > limits.max_temporary_bytes / 2048)
            return failure(FailureCode::ResourceLimit, "clipping_memory_admission");
        Work work{0, std::min(limits.max_generated_elements, limits.max_temporary_bytes / 2048 - input_elements), cancelled, stale};
        work.check();
        Surface source = convert(solid);
        Mesh    flat   = solution.interface.surface;
        for (auto& point : flat.vertices)
            point.z() = p.top_mm;
        Surface      slab  = convert(column(flat, p.bottom_mm));
        Surface      macro = intersection(source, slab, work);
        const double total = volume(macro);
        if (uniform_domain_fraction) {
            if (!std::isfinite(*uniform_domain_fraction) || *uniform_domain_fraction <= 0. || *uniform_domain_fraction >= 1.)
                return failure(FailureCode::InvalidInput, "invalid_uniform_clipped_fraction");
            target = total * *uniform_domain_fraction;
        }
        if (total <= 0. || target >= total)
            return failure(FailureCode::InfeasibleThickness, "empty_clipped_band");
        // Verify the field domain does not trim source material within this slab.
        Vec3d minimum = solid.vertices.front(), maximum = minimum;
        for (const auto& point : solid.vertices) {
            minimum = minimum.cwiseMin(point);
            maximum = maximum.cwiseMax(point);
        }
        Mesh bounds;
        bounds.vertices             = {{minimum.x() - 1., minimum.y() - 1., p.top_mm},
                                       {maximum.x() + 1., minimum.y() - 1., p.top_mm},
                                       {maximum.x() + 1., maximum.y() + 1., p.top_mm},
                                       {minimum.x() - 1., maximum.y() + 1., p.top_mm}};
        bounds.triangles            = {{0, 1, 2}, {0, 2, 3}};
        const double complete_total = volume(intersection(source, convert(column(bounds, p.bottom_mm)), work));
        const double tolerance      = std::max(p.tolerances.absolute_volume_mm3, p.tolerances.relative_volume * total);
        if (!std::isfinite(tolerance) || tolerance <= 0. || std::abs(total - complete_total) > tolerance)
            return failure(FailureCode::InvalidInput, "interface_does_not_cover_solid");

        const double height = p.top_mm - p.bottom_mm;
        double       lower = -height, upper = height;
        for (size_t i = 0; i < p.vertices.size(); ++i) {
            const auto& point = solution.interface.surface.vertices[i];
            if (point.head<2>() != p.vertices[i])
                return failure(FailureCode::StaleInput, "interface_coordinates_changed");
            const double h = point.z() - p.bottom_mm, fraction = p.target_fraction_a[i], error = p.local_fraction_error[i];
            if (!std::isfinite(h) || !std::isfinite(fraction) || !std::isfinite(error) || error < 0.)
                return failure(FailureCode::InvalidInput, "invalid_correction_constraint");
            lower = std::max({lower, p.process.min_active_thickness_mm - h, height - p.process.max_active_thickness_mm - h,
                              height * (fraction - error) - h});
            upper = std::min({upper, p.process.max_active_thickness_mm - h, height - p.process.min_active_thickness_mm - h,
                              height * (fraction + error) - h});
        }
        if (lower > upper + p.tolerances.interface_mm)
            return failure(FailureCode::InfeasibleThickness, "clipped_correction_bounds");
        double              offset = std::clamp(0., lower, std::max(lower, upper));
        MaterialBandComplex result;
        Surface             band_a, band_b;
        Mesh                field;
        bool                converged = false;
        for (size_t iteration = 0; iteration <= limits.max_correction_iterations; ++iteration) {
            work.check();
            field = solution.interface.surface;
            for (auto& point : field.vertices)
                point.z() += offset;
            Surface a = macro, cut = convert(column(field, p.bottom_mm));
            band_a.clear();
            band_b.clear();
            std::array<boost::optional<Surface*>, 4> outputs;
            outputs[PMP::Corefinement::INTERSECTION]  = &band_a;
            outputs[PMP::Corefinement::TM1_MINUS_TM2] = &band_b;
            const auto valid = PMP::corefine_and_compute_boolean_operations(a, cut, outputs, CGAL::parameters::visitor(Visitor(work)),
                                                                            CGAL::parameters::all_default(),
                                                                            std::make_tuple(CGAL::parameters::all_default(),
                                                                                            CGAL::parameters::all_default(),
                                                                                            CGAL::parameters::all_default(),
                                                                                            CGAL::parameters::all_default()));
            if (!valid[PMP::Corefinement::INTERSECTION] || !valid[PMP::Corefinement::TM1_MINUS_TM2])
                return failure(FailureCode::UnsupportedTopology, "nonmanifold_band_partition");
            result.correction_iterations = iteration;
            const double measured        = volume(band_a);
            if (std::abs(measured - target) <= tolerance) {
                converged = true;
                break;
            }
            if (measured < target)
                lower = offset;
            else
                upper = offset;
            if (upper - lower <= 1e-12)
                break;
            offset = (lower + upper) / 2.;
        }
        if (!converged)
            return failure(FailureCode::NonConverged, "clipped_material_budget");
        const double a_volume = volume(band_a), b_volume = volume(band_b);
        if (a_volume <= 0. || b_volume <= 0. || std::abs(a_volume + b_volume - total) > tolerance)
            return failure(FailureCode::MaterialToleranceExceeded, "clipped_partition_volume");
        result.revision                = p.revision;
        result.domain_volume_mm3       = total;
        result.interface_correction_mm = offset;
        result.generated_elements      = work.elements;
        // Exact intersections can produce vertices separated by less than one
        // representable double. Weld on a common local grid, then independently
        // revalidate the converted closed surfaces and their volumes.
        const double quantum = std::min(1e-9, p.tolerances.interface_mm / 100.);
        result.bands[0]      = {{p.domain, p.macro_cycle, phase.order[0], 0},
                                p.interface_id,
                                convert(band_a, limits.max_output_faces, minimum, quantum),
                                a_volume};
        result.bands[1]      = {{p.domain, p.macro_cycle, phase.order[1], 1},
                                p.interface_id,
                                convert(band_b, limits.max_output_faces, minimum, quantum),
                                b_volume};
        for (const auto& band : result.bands) {
            try {
                if (std::abs(volume(convert(band.solid)) - band.volume_mm3) > tolerance)
                    return failure(FailureCode::MaterialToleranceExceeded, "clipped_output_rounding_volume");
            } catch (const Abort&) {
                return failure(FailureCode::UnsupportedTopology, "clipped_output_rounding_topology");
            }
        }
        result.shared_interface             = {p.interface_id, p.domain, p.macro_cycle, {}};
        auto&                        shared = result.shared_interface.surface;
        const auto&                  mesh   = result.bands[0].solid;
        std::map<uint32_t, uint32_t> shared_vertices;
        const Surface                field_surface = open_surface(field);
        Tree                         field_tree(faces(field_surface).first, faces(field_surface).second, field_surface);
        field_tree.accelerate_distance_queries();
        const double squared_tolerance = p.tolerances.interface_mm * p.tolerances.interface_mm;
        // Extract actual clipped interface faces from the first complement. The
        // two Boolean outputs were built together from the same intersection.
        for (const auto& face : mesh.triangles) {
            work.check();
            if ((mesh.vertices[face[1]] - mesh.vertices[face[0]]).cross(mesh.vertices[face[2]] - mesh.vertices[face[0]]).z() <= 0.)
                continue;
            bool on = true;
            for (auto index : face) {
                const auto& point = mesh.vertices[index];
                on                = on &&
                     CGAL::to_double(field_tree.squared_distance(Kernel::Point_3(point.x(), point.y(), point.z()))) <= squared_tolerance;
            }
            if (on) {
                std::array<uint32_t, 3> triangle;
                for (size_t i = 0; i < 3; ++i) {
                    const auto inserted = shared_vertices.emplace(face[i], uint32_t(shared.vertices.size()));
                    if (inserted.second)
                        shared.vertices.push_back(mesh.vertices[face[i]]);
                    triangle[i] = inserted.first->second;
                }
                shared.triangles.push_back(triangle);
            }
        }
        if (shared.triangles.empty())
            return failure(FailureCode::UnsupportedTopology, "empty_clipped_interface");
        work.check();
        result.generated_elements = work.elements;
        return std::make_shared<const MaterialBandComplex>(std::move(result));
    } catch (const Abort& abort) {
        return failure(abort.code, abort.detail);
    } catch (const std::bad_alloc&) {
        return failure(FailureCode::ResourceLimit, "clipping_allocation");
    } catch (const std::exception&) {
        return failure(FailureCode::UnsupportedTopology, "boolean_geometry_failure");
    }
}

struct SolidQuery::Impl
{
    Surface                                      mesh;
    Tree                                         tree;
    CGAL::Side_of_triangle_mesh<Surface, Kernel> side;
    explicit Impl(const Mesh& input) : mesh(convert(input)), tree(faces(mesh).first, faces(mesh).second, mesh), side(tree)
    {
        tree.accelerate_distance_queries();
    }
};

SlabClipResult clip_pure_slab(const Mesh&                  solid,
                              BandId                       id,
                              const RevisionKey&           revision,
                              double                       bottom_mm,
                              double                       top_mm,
                              const NumericTolerances&     tolerances,
                              const ClippingLimits&        limits,
                              const std::function<bool()>& cancelled,
                              const std::function<bool()>& stale)
{
    const auto fail = [&](FailureCode code, const char* detail) -> SlabClipResult { return Failure{revision, code, detail, id, {}, {}}; };
    try {
        if (solid.vertices.empty() || solid.triangles.empty() || !std::isfinite(bottom_mm) || !std::isfinite(top_mm) ||
            top_mm <= bottom_mm || !std::isfinite(tolerances.interface_mm) || tolerances.interface_mm <= 0. || id.material.value < 1 ||
            id.material.value > MAX_MATERIALS)
            return fail(FailureCode::InvalidInput, "invalid_pure_slab");
        if (solid.triangles.size() > limits.max_input_faces || solid.vertices.size() > 3 * limits.max_input_faces ||
            solid.triangles.size() + solid.vertices.size() > limits.max_temporary_bytes / 2048)
            return fail(FailureCode::ResourceLimit, "pure_slab_admission");
        const size_t input_elements = solid.triangles.size() + solid.vertices.size();
        Work         work{0, std::min(limits.max_generated_elements, limits.max_temporary_bytes / 2048 - input_elements), cancelled, stale};
        work.check();
        Vec3d low = solid.vertices.front(), high = low;
        for (const auto& p : solid.vertices) {
            if (!p.allFinite())
                return fail(FailureCode::InvalidInput, "nonfinite_pure_solid");
            low  = low.cwiseMin(p);
            high = high.cwiseMax(p);
        }
        Mesh cap;
        cap.vertices   = {{low.x() - 1., low.y() - 1., top_mm},
                          {high.x() + 1., low.y() - 1., top_mm},
                          {high.x() + 1., high.y() + 1., top_mm},
                          {low.x() - 1., high.y() + 1., top_mm}};
        cap.triangles  = {{0, 1, 2}, {0, 2, 3}};
        Surface source = convert(solid), slab = convert(column(cap, bottom_mm));
        // Float source meshes commonly place a subdivision vertex a few nm
        // from an intended macro/band plane. Exact clipping preserves that
        // sliver, but conversion back to the nanometer grid can fold it. Snap
        // only this bounded numerical neighborhood, after validating the
        // original solid, and verify both topology and the volume change.
        const double original_volume = volume(source);
        const double plane_quantum   = std::min(1e-8, tolerances.interface_mm / 100.);
        bool         conditioned     = false;
        for (const auto vertex : source.vertices()) {
            const auto   p     = source.point(vertex);
            const double z     = CGAL::to_double(p.z());
            const double plane = std::abs(z - bottom_mm) <= std::abs(z - top_mm) ? bottom_mm : top_mm;
            if (z != plane && std::abs(z - plane) <= plane_quantum) {
                source.point(vertex) = Kernel::Point_3(p.x(), p.y(), plane);
                conditioned          = true;
            }
        }
        if (conditioned) {
            if (PMP::does_self_intersect(source) || !PMP::does_bound_a_volume(source))
                return fail(FailureCode::UnsupportedTopology, "pure_slab_plane_conditioning_topology");
            if (std::abs(volume(source) - original_volume) >
                std::max(tolerances.absolute_volume_mm3, tolerances.relative_volume * original_volume))
                return fail(FailureCode::MaterialToleranceExceeded, "pure_slab_plane_conditioning_volume");
        }
        Surface      clipped = intersection(source, slab, work);
        const double dose    = volume(clipped);
        if (!std::isfinite(dose) || dose <= 0.)
            return fail(FailureCode::UnsupportedTopology, "empty_pure_slab");
        ClippedBand   result{id, 0, convert(clipped, limits.max_output_faces, low, std::min(1e-9, tolerances.interface_mm / 100.)), dose};
        const Surface checked = convert(result.solid);
        if (std::abs(volume(checked) - dose) > std::max(tolerances.absolute_volume_mm3, tolerances.relative_volume * dose))
            return fail(FailureCode::MaterialToleranceExceeded, "pure_slab_rounding_volume");
        work.check();
        return std::make_shared<const ClippedBand>(std::move(result));
    } catch (const Abort& error) {
        return fail(error.code, error.detail);
    } catch (const std::bad_alloc&) {
        return fail(FailureCode::ResourceLimit, "pure_slab_allocation");
    } catch (const std::exception&) {
        return fail(FailureCode::UnsupportedTopology, "pure_slab_geometry");
    }
}

SolidCoverageResult measure_coverage(const Mesh&                  candidate,
                                     const Mesh&                  target,
                                     const RevisionKey&           revision,
                                     const ClippingLimits&        limits,
                                     const std::function<bool()>& cancelled)
{
    const auto fail = [&](FailureCode code, const char* detail) -> SolidCoverageResult {
        return Failure{revision, code, detail, {}, {}, {}};
    };
    try {
        const size_t count = candidate.triangles.size() + candidate.vertices.size() + target.triangles.size() + target.vertices.size();
        if (candidate.triangles.size() > limits.max_input_faces || target.triangles.size() > limits.max_input_faces ||
            count > limits.max_temporary_bytes / 2048)
            return fail(FailureCode::ResourceLimit, "coverage_boolean_admission");
        const std::function<bool()> stale;
        Work work{0, std::min(limits.max_generated_elements, limits.max_temporary_bytes / 2048 - count), cancelled, stale};
        work.check();
        Surface      a = convert(candidate), b = convert(target);
        const double a_volume = volume(a), b_volume = volume(b);
        const double common = volume(intersection(std::move(a), std::move(b), work));
        work.check();
        return SolidCoverage{a_volume, std::max(0., b_volume - common), std::max(0., a_volume - common)};
    } catch (const Abort& abort) {
        return fail(abort.code, abort.detail);
    } catch (const std::bad_alloc&) {
        return fail(FailureCode::ResourceLimit, "coverage_allocation");
    } catch (const std::exception&) {
        return fail(FailureCode::UnsupportedTopology, "coverage_boolean_failure");
    }
}

SolidQuery::SolidQuery(const Mesh& solid)
{
    try {
        m_impl = std::make_shared<const Impl>(solid);
    } catch (const Abort& abort) {
        throw std::invalid_argument(abort.detail);
    }
}
bool SolidQuery::contains(const Vec3d& point) const
{
    if (!point.allFinite())
        return false;
    return m_impl->side({point.x(), point.y(), point.z()}) != CGAL::ON_UNBOUNDED_SIDE;
}
double SolidQuery::distance_mm(const Vec3d& point) const
{
    if (!point.allFinite())
        throw std::invalid_argument("nonfinite_distance_query");
    return std::sqrt(CGAL::to_double(m_impl->tree.squared_distance({point.x(), point.y(), point.z()})));
}
} // namespace Slic3r::MaterialBand
