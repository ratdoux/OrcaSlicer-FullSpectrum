#include "Topology.hpp"

#include "libslic3r/ClipperUtils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace Slic3r::MaterialBand {
namespace {
struct Abort
{
    FailureCode code;
    const char* detail;
};
struct Prepared
{
    const DomainSection* source;
    ExPolygons           footprint;
    Vec2d                minimum, maximum;
    size_t               points = 0;
};
double area_mm2(const ExPolygons& polygons)
{
    double sum = 0.;
    for (const auto& p : polygons)
        sum += p.area() * SCALING_FACTOR * SCALING_FACTOR;
    return sum;
}
bool compatible(const StackPhase& a, const StackPhase& b)
{
    return a.order[0].value == b.order[0].value && a.order[1].value == b.order[1].value && a.cadence == b.cadence &&
           a.first_active_cycle == b.first_active_cycle && a.slot == b.slot;
}
} // namespace

TopologyResult link_domains(const DomainFrame&           before,
                            const DomainFrame&           after,
                            double                       bead_width_mm,
                            const TopologyLimits&        limits,
                            const std::function<bool()>& cancelled,
                            const std::function<bool()>& stale)
{
    const auto fail = [&](FailureCode code, const char* detail) -> TopologyResult {
        return Failure{after.revision, code, detail, {}, {}, {}};
    };
    try {
        const auto poll = [&] {
            if (cancelled && cancelled())
                throw Abort{FailureCode::Cancelled, "cancelled_during_topology"};
            if (stale && stale())
                throw Abort{FailureCode::StaleInput, "revision_changed"};
        };
        poll();
        if (before.revision != after.revision)
            return fail(FailureCode::StaleInput, "topology_revision_mismatch");
        if (before.macro_cycle == std::numeric_limits<uint64_t>::max() || after.macro_cycle != before.macro_cycle + 1)
            return fail(FailureCode::InvalidInput, "nonadjacent_topology_cycles");
        if (!std::isfinite(bead_width_mm) || bead_width_mm < .004)
            return fail(FailureCode::InvalidInput, "invalid_topology_bead_width");
        if (before.domains.size() > limits.max_domains || after.domains.size() > limits.max_domains)
            return fail(FailureCode::ResourceLimit, "topology_domain_limit");
        Vec2d  origin     = Vec2d::Constant(std::numeric_limits<double>::max());
        size_t face_count = 0, bytes = 0;
        for (const auto* frame : {&before, &after}) {
            std::set<DomainRef> ids;
            for (const auto& domain : frame->domains) {
                poll();
                if (!ids.insert(domain.id).second || domain.id.object == 0 || domain.id.domain == 0 || domain.footprint.triangles.empty())
                    return fail(FailureCode::InvalidInput, "duplicate_or_empty_topology_domain");
                if (domain.active_materials == 0 || domain.active_materials > 3)
                    return fail(FailureCode::UnsupportedProfile, "two_material_topology_required");
                const auto& phase = domain.phase;
                if (phase.order[0].value < 1 || phase.order[0].value > 2 || phase.order[1].value < 1 || phase.order[1].value > 2 ||
                    phase.order[0].value == phase.order[1].value || phase.cadence != std::array<uint32_t, 2>{{1, 1}} || phase.slot != 0 ||
                    phase.first_active_cycle > frame->macro_cycle)
                    return fail(FailureCode::IncompatibleOrder, "unsupported_topology_phase");
                if (domain.footprint.triangles.size() > limits.max_faces - face_count)
                    return fail(FailureCode::ResourceLimit, "topology_face_limit");
                face_count += domain.footprint.triangles.size();
                if (domain.footprint.vertices.size() > limits.max_temporary_bytes / 128 ||
                    domain.footprint.triangles.size() > limits.max_temporary_bytes / 512)
                    return fail(FailureCode::ResourceLimit, "topology_memory_limit");
                const size_t added = domain.footprint.vertices.size() * 128 + domain.footprint.triangles.size() * 512;
                if (added > limits.max_temporary_bytes - bytes)
                    return fail(FailureCode::ResourceLimit, "topology_memory_limit");
                bytes += added;
                for (const auto& p : domain.footprint.vertices) {
                    if (!p.allFinite())
                        return fail(FailureCode::InvalidInput, "nonfinite_topology_vertex");
                    origin = origin.cwiseMin(p.head<2>());
                }
            }
        }
        const auto prepare = [&](const DomainFrame& frame) {
            std::vector<Prepared> result;
            for (const auto& domain : frame.domains) {
                poll();
                Polygons triangles;
                Vec2d    minimum = Vec2d::Constant(std::numeric_limits<double>::max()), maximum = -minimum;
                double   source_area = 0.;
                for (const auto& face : domain.footprint.triangles) {
                    Points points;
                    for (uint32_t index : face) {
                        if (index >= domain.footprint.vertices.size())
                            throw Abort{FailureCode::InvalidInput, "topology_vertex_index"};
                        const Vec2d xy = domain.footprint.vertices[index].head<2>() - origin;
                        if (xy.cwiseAbs().maxCoeff() > 100000.)
                            throw Abort{FailureCode::ResourceLimit, "topology_coordinate_range"};
                        minimum = minimum.cwiseMin(xy);
                        maximum = maximum.cwiseMax(xy);
                        points.emplace_back(coord_t(std::llround(xy.x() / SCALING_FACTOR)), coord_t(std::llround(xy.y() / SCALING_FACTOR)));
                    }
                    Polygon polygon(std::move(points));
                    if (polygon.area() <= 0.)
                        throw Abort{FailureCode::InvalidInput, "nonpositive_topology_triangle"};
                    source_area += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
                    triangles.push_back(std::move(polygon));
                }
                ExPolygons footprint = union_ex(triangles);
                if (footprint.size() != 1)
                    throw Abort{FailureCode::InvalidInput, "topology_domain_not_connected"};
                if (std::abs(source_area - area_mm2(footprint)) > 1e-7 * std::max(1., source_area))
                    throw Abort{FailureCode::InvalidInput, "overlapping_topology_triangles"};
                const size_t points = to_points(footprint).size();
                result.push_back({&domain, std::move(footprint), minimum, maximum, points});
            }
            std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.source->id < b.source->id; });
            return result;
        };
        const auto         old = prepare(before), next = prepare(after);
        TopologyTransition result;
        result.revision    = after.revision;
        result.macro_cycle = after.macro_cycle;
        using Key          = std::tuple<uint64_t, int64_t, int64_t>;
        std::map<Key, std::vector<size_t>> grid;
        const double                       pitch = std::max(1., 4. * bead_width_mm);
        const auto                         bins  = [&](const Prepared& p, const auto& visit) {
            const int64_t x0 = int64_t(std::floor(p.minimum.x() / pitch)), x1 = int64_t(std::floor(p.maximum.x() / pitch));
            const int64_t y0 = int64_t(std::floor(p.minimum.y() / pitch)), y1 = int64_t(std::floor(p.maximum.y() / pitch));
            const double  count = double(x1 - x0 + 1) * double(y1 - y0 + 1);
            if (count > double(limits.max_grid_references))
                throw Abort{FailureCode::ResourceLimit, "topology_grid_limit"};
            for (int64_t x = x0; x <= x1; ++x)
                for (int64_t y = y0; y <= y1; ++y)
                    visit(Key{p.source->id.object, x, y});
        };
        const auto admit_grid = [&] {
            if (++result.grid_references > limits.max_grid_references || 128 > limits.max_temporary_bytes - bytes)
                throw Abort{FailureCode::ResourceLimit, "topology_grid_limit"};
            bytes += 128;
        };
        for (size_t i = 0; i < next.size(); ++i)
            bins(next[i], [&](const Key& key) {
                admit_grid();
                grid[key].push_back(i);
            });
        std::vector<std::vector<size_t>> children(old.size()), parents(next.size());
        for (size_t i = 0; i < old.size(); ++i) {
            poll();
            if (next.size() > (limits.max_temporary_bytes - bytes) / 64)
                return fail(FailureCode::ResourceLimit, "topology_candidate_memory_limit");
            std::set<size_t> candidates;
            bins(old[i], [&](const Key& key) {
                admit_grid();
                const auto found = grid.find(key);
                if (found != grid.end())
                    candidates.insert(found->second.begin(), found->second.end());
            });
            for (size_t j : candidates) {
                poll();
                if (++result.candidate_pairs > limits.max_candidates)
                    return fail(FailureCode::ResourceLimit, "topology_candidate_limit");
                if ((old[i].maximum.array() < next[j].minimum.array()).any() || (next[j].maximum.array() < old[i].minimum.array()).any())
                    continue;
                // Bound potential intersection output as well as the returned
                // vertices; a small input-domain count does not bound clipping.
                if (old[i].points != 0 && next[j].points > (limits.max_temporary_bytes - bytes) / 64 / old[i].points)
                    return fail(FailureCode::ResourceLimit, "topology_boolean_admission");
                const ExPolygons overlap = intersection_ex(old[i].footprint, next[j].footprint);
                if (to_points(overlap).size() > limits.max_faces)
                    return fail(FailureCode::ResourceLimit, "topology_boolean_output_limit");
                const double overlap_area = area_mm2(overlap);
                if (overlap_area <= 1e-8)
                    continue;
                const ExPolygons printable      = offset_ex(overlap, -float(scale_(bead_width_mm / 2.)), ClipperLib::jtMiter);
                const double     printable_area = area_mm2(printable);
                if (printable_area <= 1e-8)
                    return fail(FailureCode::InfeasibleThickness, "nonprintable_domain_overlap");
                if (!compatible(old[i].source->phase, next[j].source->phase))
                    return fail(FailureCode::IncompatibleOrder, "topology_merge_or_phase_conflict");
                // Account for graph adjacency and both possible material event
                // records, including their owned parent/child vectors.
                if (1024 > limits.max_temporary_bytes - bytes)
                    return fail(FailureCode::ResourceLimit, "topology_graph_memory_limit");
                bytes += 1024;
                children[i].push_back(j);
                parents[j].push_back(i);
                result.connections.push_back({old[i].source->id, next[j].source->id, overlap_area, printable_area});
                for (uint32_t material = 1; material <= 2; ++material) {
                    const uint32_t bit        = 1u << (material - 1);
                    const bool     was_active = (old[i].source->active_materials & bit) != 0;
                    const bool     now_active = (next[j].source->active_materials & bit) != 0;
                    if (was_active != now_active)
                        result.events.push_back({now_active ? TopologyEventKind::MaterialBirth : TopologyEventKind::MaterialTermination,
                                                 after.macro_cycle,
                                                 {old[i].source->id},
                                                 {next[j].source->id},
                                                 PhysicalMaterialId{material}});
                }
            }
        }
        for (size_t i = 0; i < old.size(); ++i) {
            if (children[i].size() == 1)
                continue;
            TopologyEvent event{children[i].empty() ? TopologyEventKind::Termination : TopologyEventKind::Split,
                                after.macro_cycle,
                                {old[i].source->id},
                                {},
                                {}};
            for (size_t j : children[i])
                event.children.push_back(next[j].source->id);
            result.events.push_back(std::move(event));
        }
        for (size_t j = 0; j < next.size(); ++j) {
            if (parents[j].size() == 1)
                continue;
            TopologyEvent event{parents[j].empty() ? TopologyEventKind::Birth : TopologyEventKind::Merge,
                                after.macro_cycle,
                                {},
                                {next[j].source->id},
                                {}};
            for (size_t i : parents[j])
                event.parents.push_back(old[i].source->id);
            result.events.push_back(std::move(event));
        }
        poll();
        return std::make_shared<const TopologyTransition>(std::move(result));
    } catch (const Abort& abort) {
        return fail(abort.code, abort.detail);
    } catch (const std::bad_alloc&) {
        return fail(FailureCode::ResourceLimit, "topology_allocation");
    } catch (const std::exception&) {
        return fail(FailureCode::InvalidInput, "topology_geometry_failure");
    }
}
} // namespace Slic3r::MaterialBand
