#include "Scheduler.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <iostream>
#include <fstream>
#include <iomanip>

namespace Slic3r::MaterialBand {
namespace {
constexpr size_t SIDES  = 16;
constexpr size_t STATIC = std::numeric_limits<size_t>::max();
ExPolygons       precise_support_offset(const ExPolygons& polygons, double distance)
{
    // The general offset helper decimates edges by 0.5% of its radius.
    // Support certification must preserve small curved-wall corners instead.
    ClipperLib::ClipperOffset offset;
    offset.MiterLimit         = DefaultMiterLimit;
    offset.ShortestEdgeLength = 0.;
    for (const auto& polygon : polygons) {
        offset.AddPath(polygon.contour.points, jtMiter, ClipperLib::etClosedPolygon);
        for (const auto& hole : polygon.holes)
            offset.AddPath(hole.points, jtMiter, ClipperLib::etClosedPolygon);
    }
    ClipperLib::Paths expanded;
    offset.Execute(expanded, distance);
    Polygons result;
    for (const auto& path : expanded)
        result.emplace_back(Points(path.begin(), path.end()));
    return union_ex(result);
}
struct Abort
{
    FailureCode code;
    const char* detail;
};
using Corners = std::array<Vec3d, 6>;
Corners corners(const BeadCell& cell)
{
    Corners p;
    for (size_t i = 0; i < 3; ++i) {
        p[i]     = cell.upper[i];
        p[i + 3] = Vec3d(p[i].x(), p[i].y(), cell.lower_z[i]);
    }
    return p;
}
struct Bounds
{
    Vec3d lo = Vec3d::Constant(std::numeric_limits<double>::max()), hi = -lo;
    void  add(const Vec3d& p)
    {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    bool overlaps(const Bounds& b) const { return (lo.array() <= b.hi.array()).all() && (b.lo.array() <= hi.array()).all(); }
};
double area_mm2(const ExPolygons& p)
{
    double result = 0.;
    for (const auto& poly : p)
        result += poly.area() * SCALING_FACTOR * SCALING_FACTOR;
    return result;
}
Polygon footprint(const BeadCell& c, const Vec2d& origin)
{
    Points points;
    for (const auto& p : c.upper) {
        const Vec2d xy = p.head<2>() - origin;
        points.emplace_back(coord_t(std::llround(xy.x() / SCALING_FACTOR)), coord_t(std::llround(xy.y() / SCALING_FACTOR)));
    }
    Polygon polygon(points);
    if (polygon.area() <= 0.) {
        // A positive floating-point face can collapse or reverse when its
        // width is below one integer quantum. Keep the physical 3D cell and
        // conservatively enclose its rounded footprint for polygon predicates.
        // The extra strip is bounded by the existing coordinate-rounding
        // allowance; neither the deposited volume nor swept 3D cell changes.
        Points enclosure;
        for (const auto& point : points)
            for (coord_t dx : {-1, 1})
                for (coord_t dy : {-1, 1})
                    enclosure.emplace_back(point.x() + dx, point.y() + dy);
        polygon = Geometry::convex_hull(std::move(enclosure));
    }
    return polygon;
}
double face_z(const BeadCell& c, const Vec2d& xy, bool upper)
{
    const Vec3d a = upper ? c.upper[0] : Vec3d(c.upper[0].x(), c.upper[0].y(), c.lower_z[0]);
    const Vec3d b = upper ? c.upper[1] : Vec3d(c.upper[1].x(), c.upper[1].y(), c.lower_z[1]);
    const Vec3d d = upper ? c.upper[2] : Vec3d(c.upper[2].x(), c.upper[2].y(), c.lower_z[2]);
    const Vec3d n = (b - a).cross(d - a);
    return a.z() - n.head<2>().dot(xy - a.head<2>()) / n.z();
}
bool same_machine(const MachineEnvelope& a, const MachineEnvelope& b)
{
    if (a.coplanar_tip_contact != b.coplanar_tip_contact || a.material_count != b.material_count ||
        a.sections.size() != b.sections.size() || a.clearance_mm != b.clearance_mm || a.travel_lift_mm != b.travel_lift_mm ||
        a.max_volume_mm3_s != b.max_volume_mm3_s || a.max_z_mm_s != b.max_z_mm_s || a.max_segment_mm != b.max_segment_mm ||
        a.deposition_mm_s != b.deposition_mm_s || a.filament_diameter_mm != b.filament_diameter_mm || a.shared_nozzle != b.shared_nozzle)
        return false;
    for (size_t i = 0; i < a.sections.size(); ++i)
        if (a.sections[i].bottom_mm != b.sections[i].bottom_mm || a.sections[i].top_mm != b.sections[i].top_mm ||
            a.sections[i].radius_mm != b.sections[i].radius_mm)
            return false;
    return true;
}
} // namespace

bool swept_section_intersects(const Vec3d&           from,
                              const Vec3d&           to,
                              const EnvelopeSection& section,
                              const BeadCell&        cell,
                              double                 radial_margin_mm,
                              double                 vertical_margin_mm)
{
    // The sweep is the Minkowski sum of a vertical polygonal prism and the
    // complete motion segment. Its edges are prism edges or the motion vector;
    // its face normals are prism normals or prism-edge x motion. SAT adds all
    // cell-edge x sweep-edge axes. Thus an interior collision cannot be missed.
    const Corners                bead = corners(cell);
    std::array<Vec3d, 4 * SIDES> sweep;
    std::array<Vec3d, SIDES + 2> tool_edges;
    const double                 radius = (section.radius_mm + radial_margin_mm) / std::cos(PI / SIDES);
    const Vec3d                  move   = to - from;
    for (size_t i = 0; i < SIDES; ++i) {
        const double angle = (2. * i + 1.) * PI / SIDES;
        const Vec3d  xy(radius * std::cos(angle), radius * std::sin(angle), 0.);
        sweep[i]             = from + xy + Vec3d(0., 0., section.bottom_mm - vertical_margin_mm);
        sweep[i + SIDES]     = from + xy + Vec3d(0., 0., section.top_mm + vertical_margin_mm);
        sweep[i + 2 * SIDES] = sweep[i] + move;
        sweep[i + 3 * SIDES] = sweep[i + SIDES] + move;
    }
    Bounds aabb, bbb;
    for (const auto& p : bead)
        aabb.add(p);
    for (const auto& p : sweep)
        bbb.add(p);
    if (!aabb.overlaps(bbb))
        return false;
    // Translate projections to the moving tip to retain precision far from 0.
    const auto separated = [&](const Vec3d& axis) {
        const double length = axis.norm();
        if (length < 1e-12)
            return false;
        const Vec3d n    = axis / length;
        double      amin = std::numeric_limits<double>::max(), amax = -amin, bmin = amin, bmax = -amin;
        for (const auto& p : bead) {
            const double d = (p - from).dot(n);
            amin           = std::min(amin, d);
            amax           = std::max(amax, d);
        }
        for (const auto& p : sweep) {
            const double d = (p - from).dot(n);
            bmin           = std::min(bmin, d);
            bmax           = std::max(bmax, d);
        }
        // Boundary contact has zero occupied volume and is allowed. The
        // configured margins cover arithmetic/output error for noncontact.
        return amax <= bmin + 1e-10 || bmax <= amin + 1e-10;
    };
    std::array<Vec3d, 7> bead_edges;
    for (size_t i = 0; i < 3; ++i) {
        bead_edges[i]     = bead[(i + 1) % 3] - bead[i];
        bead_edges[i + 3] = bead[(i + 1) % 3 + 3] - bead[i + 3];
        if (separated(bead_edges[i].cross(Vec3d::UnitZ())))
            return false;
    }
    bead_edges[6] = Vec3d::UnitZ();
    if (separated(bead_edges[0].cross(bead_edges[1])) || separated(bead_edges[3].cross(bead_edges[4])) || separated(Vec3d::UnitZ()))
        return false;
    for (size_t i = 0; i < SIDES; ++i)
        tool_edges[i] = sweep[(i + 1) % SIDES] - sweep[i];
    tool_edges[SIDES]     = Vec3d::UnitZ();
    tool_edges[SIDES + 1] = move;
    for (const auto& edge : tool_edges) {
        if (separated(edge.cross(Vec3d::UnitZ())) || separated(edge.cross(move)))
            return false;
        for (const auto& be : bead_edges)
            if (separated(be.cross(edge)))
                return false;
    }
    return true;
}

ScheduleResult schedule_paths(const ScheduleInput&         input,
                              const ScheduleLimits&        limits,
                              const std::function<bool()>& cancelled,
                              const std::function<bool()>& stale)
{
    std::optional<BandId> responsible;
    std::optional<Vec3d>  witness;
    const auto            fail = [&](FailureCode code, const char* detail) -> Failure {
        return Failure{input.revision, code, detail, responsible, {}, witness};
    };
    try {
        const auto poll = [&] {
            if (cancelled && cancelled())
                throw Abort{FailureCode::Cancelled, "cancelled_during_schedule"};
            if (stale && stale())
                throw Abort{FailureCode::StaleInput, "revision_changed"};
        };
        poll();
        const auto& machine = input.machine;
        if (machine.material_count < 2 || machine.material_count > MAX_MATERIALS)
            return fail(FailureCode::InvalidInput, "invalid_material_count");
        for (size_t i = 0; i < machine.material_count; ++i)
            if (!std::isfinite(machine.filament_diameter_mm[i]) || machine.filament_diameter_mm[i] <= 0.)
                return fail(FailureCode::InvalidInput, "invalid_schedule_filament");
        if (!input.initial_position.allFinite() || !std::isfinite(input.bed_z_mm) || !std::isfinite(input.support_tolerance_mm) ||
            input.support_tolerance_mm <= 0. || input.support_tolerance_mm > .001 || !std::isfinite(input.contact_tolerance_mm) ||
            input.contact_tolerance_mm < 0. || input.contact_tolerance_mm > .01)
            return fail(FailureCode::InvalidInput, "invalid_schedule_constraints");
        for (double v : {machine.clearance_mm, machine.travel_lift_mm, machine.max_z_mm_s, machine.max_volume_mm3_s, machine.max_segment_mm,
                         machine.deposition_mm_s, machine.filament_diameter_mm[0], machine.filament_diameter_mm[1]})
            if (!std::isfinite(v) || v <= 0.)
                return fail(FailureCode::InvalidInput, "invalid_schedule_machine");
        if (machine.sections.empty() || machine.sections.size() > 1024 || machine.travel_lift_mm <= machine.clearance_mm + .002)
            return fail(FailureCode::UnsupportedProfile, "complete_schedule_envelope_required");
        double top = 0., radius = 0.;
        for (const auto& s : machine.sections) {
            if (!std::isfinite(s.top_mm) || !std::isfinite(s.radius_mm) || s.bottom_mm != top || s.top_mm <= top || s.radius_mm <= 0.)
                return fail(FailureCode::UnsupportedProfile, "invalid_schedule_envelope");
            top    = s.top_mm;
            radius = std::max(radius, s.radius_mm);
        }
        if (top > 100000. || radius > 100000. || std::abs(input.initial_position.z() - input.bed_z_mm) > 100000.)
            return fail(FailureCode::ResourceLimit, "schedule_coordinate_range");
        struct Node
        {
            OperationRef    id;
            const BandPath* path;
            ExPolygons      footprint;
            bool            planar;
            double          bottom, top;
        };
        struct Cell
        {
            const BeadCell* bead;
            size_t          owner, segment;
            Bounds          bounds;
            Polygon         xy;
        };
        std::vector<Node>                                                            nodes;
        std::vector<Cell>                                                            cells;
        std::map<OperationRef, size_t>                                               ids;
        std::set<uint64_t>                                                           objects;
        std::set<uint64_t>                                                           ordered_region_objects;
        std::map<std::pair<uint64_t, DomainId>, std::array<uint32_t, MAX_MATERIALS>> materials_by_slot;
        Schedule                                                                     result;
        result.revision  = input.revision;
        size_t     bytes = 0;
        const auto admit = [&](size_t amount) {
            if (amount > limits.max_temporary_bytes - bytes)
                throw Abort{FailureCode::ResourceLimit, "schedule_memory_limit"};
            bytes += amount;
        };
        Vec2d origin = input.initial_position.head<2>();
        // Local coordinates are used for all integer polygon operations.
        const auto add_cell = [&](const BeadCell& bead, size_t owner, size_t segment) {
            if (cells.size() >= limits.max_cells)
                throw Abort{FailureCode::ResourceLimit, "schedule_cell_limit"};
            admit(2 * sizeof(Cell) + 256);
            Bounds b;
            for (size_t k = 0; k < 3; ++k) {
                if (!bead.upper[k].allFinite() || !std::isfinite(bead.lower_z[k]) || bead.lower_z[k] >= bead.upper[k].z() ||
                    bead.upper[k].z() - input.bed_z_mm > 100000. || bead.lower_z[k] < input.bed_z_mm - input.support_tolerance_mm ||
                    (bead.upper[k].head<2>() - origin).cwiseAbs().maxCoeff() > 100000.)
                    throw Abort{FailureCode::InvalidInput, "invalid_occupied_cell"};
                b.add(bead.upper[k]);
                b.add(Vec3d(bead.upper[k].x(), bead.upper[k].y(), bead.lower_z[k]));
            }
            const Vec3d n = (bead.upper[1] - bead.upper[0]).cross(bead.upper[2] - bead.upper[0]);
            if (n.z() <= 1e-12)
                throw Abort{FailureCode::InvalidInput, "nonpositive_occupied_cell"};
            auto xy = footprint(bead, origin);
            if (xy.area() <= 0.)
                throw Abort{FailureCode::InvalidInput, "cell_below_coordinate_precision"};
            cells.push_back({&bead, owner, segment, b, std::move(xy)});
        };
        for (const auto& object : input.objects) {
            poll();
            if (!object.paths || object.object == 0 || !objects.insert(object.object).second ||
                !same_machine(object.paths->machine, machine))
                return fail(FailureCode::InvalidInput, "schedule_object_or_machine_mismatch");
            if (object.paths->ordered_region_paths)
                ordered_region_objects.insert(object.object);
            for (const auto& path : object.paths->paths) {
                responsible = path.band;
                if (nodes.size() >= limits.max_paths)
                    return fail(FailureCode::ResourceLimit, "schedule_path_limit");
                const OperationRef id{object.object, path.id};
                const size_t       owner = nodes.size();
                if (!ids.emplace(id, owner).second || path.id == 0 || path.vertices.size() != path.segments.size() + 1 ||
                    path.segments.empty() || path.band.domain == 0 || path.band.material.value < 1 ||
                    path.band.material.value > machine.material_count || path.band.stack_slot >= machine.material_count)
                    return fail(FailureCode::InvalidInput, "invalid_schedule_path");
                if (!std::isfinite(path.overhang_mm) || path.overhang_mm < 0. || path.overhang_mm > .5 ||
                    !std::isfinite(path.bridge_span_mm) || path.bridge_span_mm < 0. || path.bridge_span_mm > 20.)
                    return fail(FailureCode::InvalidInput, "invalid_path_support_contract");
                auto& material_slots = materials_by_slot[{object.object, path.band.domain}];
                auto& material       = material_slots[path.band.stack_slot];
                if (material != 0 && material != path.band.material.value)
                    return fail(FailureCode::IncompatibleOrder, "schedule_material_phase_conflict");
                for (size_t slot = 0; slot < machine.material_count; ++slot)
                    if (slot != path.band.stack_slot && material_slots[slot] == path.band.material.value)
                        return fail(FailureCode::IncompatibleOrder, "schedule_material_phase_conflict");
                material = path.band.material.value;
                admit(2 * sizeof(Node) + 256);
                Polygons     polygons;
                bool         planar = true;
                const double bottom = path.segments.front().cells.empty() ? 0. : path.segments.front().cells.front().lower_z.front();
                const double top    = path.vertices.front().z();
                for (size_t j = 0; j < path.segments.size(); ++j) {
                    const auto& segment = path.segments[j];
                    if (!path.vertices[j].allFinite() || !path.vertices[j + 1].allFinite() || segment.cells.empty() ||
                        (path.vertices[j + 1] - path.vertices[j]).norm() <= 1e-10 || !std::isfinite(segment.volume_mm3) ||
                        !std::isfinite(segment.speed_mm_s) || segment.volume_mm3 <= 0. || segment.speed_mm_s <= 0.)
                        return fail(FailureCode::InvalidInput, "invalid_schedule_segment");
                    const Vec3d  delta  = path.vertices[j + 1] - path.vertices[j];
                    const double length = delta.norm();
                    if (length > machine.max_segment_mm + 1e-9 || segment.speed_mm_s > machine.deposition_mm_s + 1e-9 ||
                        segment.speed_mm_s * segment.volume_mm3 / length > machine.max_volume_mm3_s + 1e-9 ||
                        segment.speed_mm_s * std::abs(delta.z()) / length > machine.max_z_mm_s + 1e-9) {
                        if (std::getenv("MATERIAL_BAND_TRACE"))
                            std::clog << std::setprecision(17) << "Material motion limit path=" << path.id << " segment=" << j
                                      << " length=" << length << '/' << machine.max_segment_mm << " speed=" << segment.speed_mm_s << '/'
                                      << machine.deposition_mm_s << " flow=" << segment.speed_mm_s * segment.volume_mm3 / length << '/'
                                      << machine.max_volume_mm3_s << std::endl;
                        return fail(FailureCode::UnsupportedProfile, "schedule_motion_or_flow_limit");
                    }
                    for (double value : {segment.width_mm, segment.height_start_mm, segment.height_end_mm})
                        if (!std::isfinite(value) || value <= 0.)
                            return fail(FailureCode::InvalidInput, "invalid_schedule_bead_dimensions");
                    double volume = 0.;
                    for (const auto& c : segment.cells) {
                        add_cell(c, owner, j);
                        for (size_t k = 0; k < 3; ++k)
                            planar &= std::abs(c.lower_z[k] - bottom) <= 1e-12 && std::abs(c.upper[k].z() - top) <= 1e-12;
                        polygons.push_back(cells.back().xy);
                        const double a = (c.upper[1] - c.upper[0]).cross(c.upper[2] - c.upper[0]).z() / 2.;
                        for (size_t k = 0; k < 3; ++k)
                            volume += a * (c.upper[k].z() - c.lower_z[k]) / 3.;
                    }
                    if (!std::isfinite(volume) || std::abs(volume - segment.volume_mm3) > std::max(1e-8, segment.volume_mm3 * 1e-6))
                        return fail(FailureCode::MaterialToleranceExceeded, "schedule_cell_dose_mismatch");
                }
                nodes.push_back({id, &path, union_ex(polygons), planar, bottom, top});
            }
        }
        responsible.reset();
        for (const auto& cell : input.occupied)
            add_cell(cell, STATIC, 0);
        // A balanced 3D index separates logical cycles at the same XY location.
        // Split on centroid spread: long bead boxes must not repeatedly choose
        // an axis on which every candidate has the same center. Weight Z only
        // for balancing thin layers; all stored bounds and queries stay in mm.
        using Tree = AABBTreeIndirect::Tree<3, double>;
        struct Source
        {
            size_t                   index;
            Tree::BoundingBox        box;
            Tree::VectorType         center;
            size_t                   idx() const { return index; }
            const Tree::BoundingBox& bbox() const { return box; }
            const Tree::VectorType&  centroid() const { return center; }
        };
        if (cells.size() > limits.max_index_nodes / 4)
            return fail(FailureCode::ResourceLimit, "schedule_index_node_limit");
        if (cells.size() > (limits.max_temporary_bytes - bytes) / (4 * sizeof(Tree::Node) + sizeof(Source)))
            return fail(FailureCode::ResourceLimit, "schedule_index_memory_limit");
        admit(cells.size() * (4 * sizeof(Tree::Node) + sizeof(Source)));
        std::vector<Source> source;
        source.reserve(cells.size());
        for (size_t i = 0; i < cells.size(); ++i)
            source.push_back({i, Tree::BoundingBox(cells[i].bounds.lo, cells[i].bounds.hi), (cells[i].bounds.lo + cells[i].bounds.hi) / 2.});
        std::vector<Tree::Node> tree;
        if (!source.empty())
            tree.resize(next_highest_power_of_2(source.size()) * 2 - 1);
        const auto build_index = [&](const auto& self, size_t index, size_t first, size_t last) -> void {
            poll();
            if (last - first == 1) {
                tree[index].set(source[first]);
                return;
            }
            Vec3d low = source[first].center, high = low;
            for (size_t i = first + 1; i < last; ++i) {
                low  = low.cwiseMin(source[i].center);
                high = high.cwiseMax(source[i].center);
            }
            Vec3d spread = high - low;
            spread.z() *= 16.;
            int axis = 0;
            spread.maxCoeff(&axis);
            const size_t middle = first + (last - first) / 2;
            std::nth_element(source.begin() + first, source.begin() + middle, source.begin() + last, [&](const Source& a, const Source& b) {
                return std::tie(a.center[axis], a.index) < std::tie(b.center[axis], b.index);
            });
            const size_t left = Tree::left_child_idx(index), right = Tree::right_child_idx(index);
            self(self, left, first, middle);
            self(self, right, middle, last);
            tree[index].idx  = Tree::inner;
            tree[index].bbox = tree[left].bbox;
            tree[index].bbox.extend(tree[right].bbox);
        };
        if (!source.empty())
            build_index(build_index, 0, 0, source.size());
        std::vector<Source>().swap(source);
        bytes -= cells.size() * sizeof(Source);
        result.index_nodes = tree.size();
        // Most planar contact queries need one complete owner footprint, not
        // every triangle used to represent it. Mark uniform-owner subtrees so
        // an already admitted or ineligible owner can be skipped as a group.
        // The original cell tree remains authoritative for overlap and motion.
        constexpr uint32_t STATIC_OWNER = std::numeric_limits<uint32_t>::max();
        constexpr uint32_t MIXED_OWNER  = STATIC_OWNER - 1;
        if (nodes.size() >= MIXED_OWNER)
            return fail(FailureCode::ResourceLimit, "schedule_owner_index_limit");
        admit(tree.size() * sizeof(uint32_t));
        std::vector<uint32_t> subtree_owner(tree.size(), MIXED_OWNER);
        for (size_t i = subtree_owner.size(); i-- > 0;) {
            const auto& node = tree[i];
            if (!node.is_valid())
                continue;
            if (!node.is_inner()) {
                const size_t owner = cells[node.idx].owner;
                subtree_owner[i]   = owner == STATIC ? STATIC_OWNER : uint32_t(owner);
            } else {
                const auto left = subtree_owner[Tree::left_child_idx(i)], right = subtree_owner[Tree::right_child_idx(i)];
                if (left == right)
                    subtree_owner[i] = left;
            }
            if (i % 4096 == 0)
                poll();
        }
        poll();
        std::vector<uint32_t> first_rank;
        const auto query_pruned = [&](const Bounds& bounds, const auto& eligible, const auto& exclude_owner, size_t rank_limit = STATIC) {
            std::vector<size_t> found;
            if ((bounds.hi.array() < bounds.lo.array()).any())
                return found;
            const Tree::BoundingBox box(bounds.lo, bounds.hi);
            const auto              visit = [&](const auto& self, size_t index) -> void {
                if (++result.index_visits > limits.max_index_visits)
                    throw Abort{FailureCode::ResourceLimit, "schedule_index_work_limit"};
                const auto& node = tree[index];
                if (!node.is_valid() || !box.intersects(node.bbox))
                    return;
                if (rank_limit != STATIC && first_rank[index] >= rank_limit)
                    return;
                const uint32_t owner = subtree_owner[index];
                if (owner != MIXED_OWNER && owner != STATIC_OWNER && exclude_owner(size_t(owner)))
                    return;
                if (node.is_inner()) {
                    self(self, Tree::left_child_idx(index));
                    self(self, Tree::right_child_idx(index));
                } else if (eligible(cells[node.idx])) {
                    if (found.size() >= (limits.max_temporary_bytes - bytes) / (2 * sizeof(size_t)))
                        throw Abort{FailureCode::ResourceLimit, "schedule_query_memory_limit"};
                    found.push_back(node.idx);
                }
            };
            if (!tree.empty())
                visit(visit, 0);
            std::sort(found.begin(), found.end());
            if (found.size() > limits.max_candidates - result.candidate_pairs)
                throw Abort{FailureCode::ResourceLimit, "schedule_candidate_limit"};
            result.candidate_pairs += found.size();
            return found;
        };
        const auto query = [&](const Bounds& bounds, const auto& eligible) {
            return query_pruned(bounds, eligible, [](size_t) { return false; });
        };
        std::vector<std::set<size_t>> outgoing(nodes.size());
        std::vector<size_t>           indegree(nodes.size(), 0);
        std::vector<size_t>           phase_rank(nodes.size(), 0);
        std::vector<size_t>           explicit_rank(nodes.size(), STATIC);
        bool                          total_order = false;
        const bool                    flexible    = input.allow_planar_reordering;
        if (flexible && (!machine.coplanar_tip_contact || input.objects.size() != 1 || !input.dependencies.empty() ||
                         std::any_of(nodes.begin(), nodes.end(), [](const Node& node) { return !node.planar; })))
            return fail(FailureCode::InvalidInput, "planar_reordering_requires_single_coplanar_object");
        std::vector<size_t> flexible_order;
        std::vector<bool>   support_validated(nodes.size(), false);
        admit(nodes.size() * (sizeof(ExPolygons) + sizeof(size_t) + 1));
        std::vector<ExPolygons>       grounded_wall_faces(nodes.size());
        std::vector<size_t>           grounded_wall_bytes(nodes.size(), 0);
        std::vector<bool>             floor_classified(nodes.size(), false);
        std::map<double, size_t>      remaining_heights;
        std::multimap<double, size_t> grounded_at_height;
        constexpr size_t              HEIGHT_ENTRY_BYTES = 128;
        for (const auto& node : nodes) {
            const auto inserted = remaining_heights.emplace(node.top, 0);
            if (inserted.second)
                admit(HEIGHT_ENTRY_BYTES);
            ++inserted.first->second;
        }
        const auto retain_grounded_wall = [&](size_t owner) {
            const auto& faces = grounded_wall_faces[owner];
            if (faces.empty())
                return;
            const size_t retained = to_points(faces).size() * sizeof(Point) * 2 + faces.size() * sizeof(ExPolygon) * 2;
            admit(retained + HEIGHT_ENTRY_BYTES);
            grounded_wall_bytes[owner] = retained;
            grounded_at_height.emplace(nodes[owner].top, owner);
        };
        const auto complete_support = [&](size_t owner) {
            support_validated[owner] = true;
            const double top         = nodes[owner].top;
            auto         remaining   = remaining_heights.find(top);
            if (--remaining->second != 0)
                return;
            remaining_heights.erase(remaining);
            bytes -= HEIGHT_ENTRY_BYTES;
            // Qualified side anchors are only consulted by coplanar paths.
            // Keep their faces until every possible target at that height has
            // been checked, including deferred paths and later tower/object
            // returns. Actual bead cells and floor footprints remain intact.
            // A wider lookup window conservatively covers tolerance rounding.
            const double reach = 2. * input.support_tolerance_mm + 1e-8;
            for (auto anchor = grounded_at_height.lower_bound(top - reach);
                 anchor != grounded_at_height.end() && anchor->first <= top + reach;) {
                const auto future = remaining_heights.lower_bound(anchor->first - reach);
                if (future != remaining_heights.end() && future->first <= anchor->first + reach) {
                    ++anchor;
                    continue;
                }
                const size_t id = anchor->second;
                ExPolygons().swap(grounded_wall_faces[id]);
                bytes -= grounded_wall_bytes[id] + HEIGHT_ENTRY_BYTES;
                grounded_wall_bytes[id] = 0;
                anchor                  = grounded_at_height.erase(anchor);
            }
        };
        std::vector<double>        prior_top(nodes.size(), input.bed_z_mm);
        std::map<uint64_t, double> object_top;
        double                     occupied_top = input.bed_z_mm;
        for (const auto& c : cells) {
            if (c.owner == STATIC)
                occupied_top = std::max(occupied_top, c.bounds.hi.z());
            else {
                prior_top[c.owner] = std::max(prior_top[c.owner], c.bounds.hi.z());
                auto inserted      = object_top.emplace(nodes[c.owner].id.object, c.bounds.hi.z());
                if (!inserted.second)
                    inserted.first->second = std::max(inserted.first->second, c.bounds.hi.z());
            }
        }
        const auto edge = [&](size_t a, size_t b, DependencyKind kind) {
            if (a == b)
                throw Abort{FailureCode::IncompatibleOrder, "self_dependency"};
            // Cell contacts can establish millions of links already implied
            // by the object's phase chain or the final explicit sequence.
            // Keep validating every contact, but retain only dependencies
            // that add an ordering constraint. Backward links still expose
            // contradictions; construction preflight has no fixed chain yet.
            if ((kind == DependencyKind::Support || kind == DependencyKind::Access) && !flexible &&
                ((total_order && explicit_rank[a] < explicit_rank[b]) ||
                 (nodes[a].id.object == nodes[b].id.object && phase_rank[a] < phase_rank[b])))
                return;
            if (outgoing[a].find(b) != outgoing[a].end())
                return;
            if (result.dependencies.size() >= limits.max_edges)
                throw Abort{FailureCode::ResourceLimit, "schedule_edge_limit"};
            admit(sizeof(Dependency) * 2 + 96);
            outgoing[a].insert(b);
            ++indegree[b];
            result.dependencies.push_back({nodes[a].id, nodes[b].id, kind});
        };
        for (uint64_t object : objects) {
            std::vector<size_t> order;
            for (size_t i = 0; i < nodes.size(); ++i)
                if (nodes[i].id.object == object)
                    order.push_back(i);
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                if (ordered_region_objects.count(object))
                    return nodes[a].id.path < nodes[b].id.path;
                const auto& x = nodes[a].path->band;
                const auto& y = nodes[b].path->band;
                return std::tie(x.macro_cycle, x.stack_slot, x.domain, nodes[a].id.path) <
                       std::tie(y.macro_cycle, y.stack_slot, y.domain, nodes[b].id.path);
            });
            for (size_t i = 0; i < order.size(); ++i) {
                phase_rank[order[i]] = i;
                if (i > 0)
                    prior_top[order[i]] = std::max(prior_top[order[i]], prior_top[order[i - 1]]);
            }
            double other_top = occupied_top;
            for (const auto& entry : object_top)
                if (entry.first != object)
                    other_top = std::max(other_top, entry.second);
            for (size_t i : order)
                prior_top[i] = std::max(prior_top[i], other_top);
            if (flexible)
                flexible_order = std::move(order);
            else
                for (size_t i = 1; i < order.size(); ++i)
                    edge(order[i - 1], order[i], DependencyKind::Phase);
        }
        for (const auto& e : input.dependencies) {
            const auto a = ids.find(e.before), b = ids.find(e.after);
            if (a == ids.end() || b == ids.end())
                return fail(FailureCode::InvalidInput, "unknown_operation_dependency");
            edge(a->second, b->second, e.kind);
        }
        // Finalization supplies an explicit total sequence containing the
        // object and prime tower. Future cells in that chain cannot obstruct
        // this operation. Recognize the chain structurally; a mere topological
        // sort would not prove ordering between otherwise independent paths.
        if (machine.coplanar_tip_contact && !nodes.empty() && input.dependencies.size() + 1 == nodes.size()) {
            std::vector<size_t> next(nodes.size(), STATIC), previous(nodes.size(), STATIC);
            bool                chain = true;
            for (const auto& dependency : input.dependencies) {
                const size_t a = ids.at(dependency.before), b = ids.at(dependency.after);
                if (next[a] != STATIC || previous[b] != STATIC) {
                    chain = false;
                    break;
                }
                next[a]     = b;
                previous[b] = a;
            }
            size_t first = STATIC;
            for (size_t i = 0; i < nodes.size(); ++i)
                if (previous[i] == STATIC) {
                    if (first != STATIC)
                        chain = false;
                    first = i;
                }
            size_t count = 0;
            for (size_t i = first; chain && i != STATIC && explicit_rank[i] == STATIC; i = next[i])
                explicit_rank[i] = count++;
            total_order = chain && count == nodes.size();
            if (total_order) {
                std::vector<double> prefix_top(nodes.size(), occupied_top);
                for (const auto& cell : cells)
                    if (cell.owner != STATIC)
                        prefix_top[explicit_rank[cell.owner]] = std::max(prefix_top[explicit_rank[cell.owner]], cell.bounds.hi.z());
                for (size_t i = 1; i < prefix_top.size(); ++i)
                    prefix_top[i] = std::max(prefix_top[i], prefix_top[i - 1]);
                for (size_t i = 0; i < nodes.size(); ++i)
                    prior_top[i] = prefix_top[explicit_rank[i]];
                // The explicit chain proves which paths can be occupied at a
                // motion. Static geometry has rank zero and is never skipped.
                // Only motion queries use this bound; support and overlap
                // checks still include future cells to expose contradictions.
                admit(tree.size() * sizeof(uint32_t));
                first_rank.assign(tree.size(), STATIC_OWNER);
                for (size_t i = tree.size(); i-- > 0;) {
                    if (!tree[i].is_valid())
                        continue;
                    if (tree[i].is_inner())
                        first_rank[i] = std::min(first_rank[Tree::left_child_idx(i)], first_rank[Tree::right_child_idx(i)]);
                    else {
                        const size_t owner = cells[tree[i].idx].owner;
                        first_rank[i]      = owner == STATIC ? 0 : uint32_t(explicit_rank[owner] + 1);
                    }
                    if (i % 4096 == 0)
                        poll();
                }
            }
        }
        // A layered process may declare bounded bridging. Verify the entire
        // run against actual preceding contact faces, including both anchors;
        // splitting a long bridge into small segments cannot evade the span.
        std::vector<int> bridge_valid(nodes.size(), -1);
        const auto       earlier_coplanar = [&](size_t owner, size_t other) {
            if (!machine.coplanar_tip_contact || other == STATIC || other == owner || !nodes[owner].planar)
                return false;
            const auto& anchor = nodes[other];
            // A wall may bond to independently grounded sections of an earlier
            // inner wall. Bridged and side-supported sections cannot relay
            // support farther outward, even when the rest of that run is sound.
            if (nodes[owner].path->role == PathRole::Perimeter &&
                (anchor.path->role != PathRole::Perimeter || !support_validated[other] || grounded_wall_faces[other].empty()))
                return false;
            if (!anchor.planar || std::abs(anchor.top - nodes[owner].top) > input.support_tolerance_mm ||
                anchor.bottom > nodes[owner].bottom + input.support_tolerance_mm)
                return false;
            if (flexible)
                return support_validated[other] && anchor.id.object == nodes[owner].id.object;
            return total_order ? explicit_rank[other] < explicit_rank[owner] :
                                       anchor.id.object == nodes[owner].id.object && phase_rank[other] < phase_rank[owner];
        };
        const auto anchored_bridge = [&](size_t owner, const ExPolygons* ordinary_support) {
            if (bridge_valid[owner] >= 0)
                return bridge_valid[owner] != 0;
            bridge_valid[owner]       = 0;
            const auto& path          = *nodes[owner].path;
            const auto  reject_bridge = [&](const char* reason) {
                if (std::getenv("MATERIAL_BAND_TRACE"))
                    std::clog << "Bridge rejected " << path.id << ' ' << reason << " role=" << int(path.role)
                              << " endpoints=" << path.vertices.front().transpose() << " -> " << path.vertices.back().transpose()
                              << " vertices=" << path.vertices.size() << " source=" << path.split_reason << std::endl;
                return false;
            };
            if (path.bridge_span_mm == 0.)
                return false;
            const double bottom = path.segments.front().cells.front().lower_z.front();
            Bounds       bounds;
            Points       points;
            for (const auto& p : path.vertices) {
                bounds.add(Vec3d(p.x(), p.y(), bottom));
                points.emplace_back(scale_(p.x() - origin.x()), scale_(p.y() - origin.y()));
            }
            for (const auto& s : path.segments)
                for (const auto& bead : s.cells)
                    for (size_t i = 0; i < 3; ++i) {
                        if (std::abs(bead.lower_z[i] - bottom) > input.support_tolerance_mm)
                            return false;
                        bounds.add(Vec3d(bead.upper[i].x(), bead.upper[i].y(), bottom));
                    }
            // Search every finite cap that end_contact measures. Centerlines
            // expanded only by the overhang allowance can miss real side
            // anchors, especially with zero overhang or a rotated end cap.
            // This changes the spatial query, not the qualified contact area.
            for (const auto& [end, adjacent, width] :
                 {std::make_tuple(size_t(0), size_t(1), path.segments.front().width_mm),
                  std::make_tuple(path.vertices.size() - 1, path.vertices.size() - 2, path.segments.back().width_mm)}) {
                const Vec2d center = path.vertices[end].head<2>();
                const Vec2d along  = (path.vertices[adjacent] - path.vertices[end]).head<2>().normalized() * (width / 2.);
                const Vec2d across(-along.y(), along.x());
                for (const Vec2d& p : std::array<Vec2d, 4>{
                         {center - along - across, center + along - across, center + along + across, center - along + across}})
                    bounds.add(Vec3d(p.x(), p.y(), bottom));
            }
            bounds.lo.array() -= path.overhang_mm;
            bounds.hi.array() += path.overhang_mm;
            bounds.lo.z()                   = bottom - input.support_tolerance_mm;
            const bool coplanar_anchors     = machine.coplanar_tip_contact && nodes[owner].planar;
            bounds.hi.z()                   = (coplanar_anchors ? nodes[owner].top : bottom) + input.support_tolerance_mm;
            const auto       earlier_anchor = [&](const Cell& other) { return earlier_coplanar(owner, other.owner); };
            Polygons         supports, side_supports;
            std::set<size_t> anchors;
            for (size_t j : query(bounds, [&](const Cell& other) {
                     return other.owner != owner &&
                            (std::abs(other.bounds.hi.z() - bottom) <= input.support_tolerance_mm || earlier_anchor(other));
                 })) {
                const auto& other  = cells[j];
                bool        planar = true;
                for (const auto& p : other.bead->upper)
                    planar &= std::abs(p.z() - bottom) <= input.support_tolerance_mm;
                if (!planar && !earlier_anchor(other))
                    continue;
                const bool lateral = earlier_anchor(other);
                const auto faces   = lateral && path.role == PathRole::Perimeter ?
                                         to_polygons(intersection_ex(Polygons{other.xy}, grounded_wall_faces[other.owner])) :
                                         Polygons{other.xy};
                if (faces.empty())
                    continue;
                supports.insert(supports.end(), faces.begin(), faces.end());
                if (lateral)
                    side_supports.insert(side_supports.end(), faces.begin(), faces.end());
                if (other.owner != STATIC)
                    anchors.insert(other.owner);
            }
            if (supports.empty())
                return reject_bridge("no_supports");
            // Anchors need actual lower-face material or an earlier, separately
            // validated deposition run covering the bridge's full height.
            // These supply lateral contact at a normal infill/perimeter joint
            // or where adjacent bridge regions meet. Future or floating paths
            // and the expanded overhang allowance cannot supply this contact.
            const auto contact = union_ex(supports);
            Polyline   line(std::move(points));
            const bool closed = line.points.front() == line.points.back();
            // Owned coplanar beads meet along a vertical side face, without
            // overlapping in XY. Measure that real bond inside each finite end
            // cap separately from contact underneath the cap. Proximity and
            // contact elsewhere along the run cannot anchor an endpoint.
            const auto shared_sides = side_supports.empty() ?
                                          Polylines{} :
                                          intersection_pl(to_polylines(nodes[owner].footprint), offset_ex(union_ex(side_supports), 2.f));
            const auto end_contact  = [&](size_t end, size_t adjacent, double width) {
                const Vec2d center    = line.points[end].cast<double>() * SCALING_FACTOR;
                const Vec2d direction = (line.points[adjacent] - line.points[end]).cast<double>().normalized();
                const Vec2d along     = direction * (width / 2.), across(-along.y(), along.x());
                Points      corners;
                for (const Vec2d& point : std::array<Vec2d, 4>{
                         {center - along - across, center + along - across, center + along + across, center - along + across}})
                    corners.emplace_back(scale_(point.x()), scale_(point.y()));
                const Polygons cap{Polygon(std::move(corners))};
                const double   area        = area_mm2(intersection_ex(cap, contact));
                double         side_length = 0.;
                for (const auto& side : intersection_pl(shared_sides, cap))
                    side_length += side.length() * SCALING_FACTOR;
                const bool side_anchor = side_length + 8. * SCALING_FACTOR >= .5 * width;
                return std::make_pair(area + 8. * width * SCALING_FACTOR >= .5 * width * width || side_anchor,
                                      side_anchor ? .5 * width : 0.);
            };
            // A closed contour still starts at a concrete machine position.
            // Closure cannot supply an anchor before any filament is printed.
            const auto start_contact  = end_contact(0, 1, path.segments.front().width_mm);
            const auto finish_contact = end_contact(line.points.size() - 1, line.points.size() - 2, path.segments.back().width_mm);
            double     unsupported = 0., seam_gap = 0.;
            if (path.role == PathRole::Perimeter && ordinary_support && !side_supports.empty()) {
                // A side-attached wall may bridge a short interruption between
                // independently grounded inner-wall sections. The already
                // certified ordinary support is not itself a bridge gap.
                // Project all unsupported bead area, including off-center caps,
                // onto path arclength; segment subdivision cannot shorten it.
                std::vector<std::pair<double, double>> intervals;
                double                                 position = 0.;
                for (size_t s = 0; s < path.segments.size(); ++s) {
                    poll();
                    const Vec2d  start     = path.vertices[s].head<2>() - origin.head<2>();
                    const Vec2d  delta     = (path.vertices[s + 1] - path.vertices[s]).head<2>();
                    const double length    = delta.norm();
                    const Vec2d  direction = delta / length;
                    for (const auto& bead : path.segments[s].cells) {
                        const Polygon face    = footprint(bead, origin);
                        const auto    missing = diff_ex(Polygons{face}, *ordinary_support);
                        if (area_mm2(missing) <= std::max(1e-9, 8. * face.length() * SCALING_FACTOR * SCALING_FACTOR))
                            continue;
                        for (const auto& piece : missing) {
                            double first = length, last = 0.;
                            for (const auto& point : piece.contour.points) {
                                const double along = direction.dot(point.cast<double>() * SCALING_FACTOR - start);
                                first              = std::min(first, along);
                                last               = std::max(last, along);
                            }
                            if (intervals.size() >= (limits.max_temporary_bytes - bytes) / (4 * sizeof(intervals.front())))
                                throw Abort{FailureCode::ResourceLimit, "bridge_span_memory_limit"};
                            intervals.emplace_back(position + std::max(0., first), position + std::min(length, last));
                        }
                    }
                    position += length;
                }
                std::sort(intervals.begin(), intervals.end());
                std::vector<std::pair<double, double>> gaps;
                for (const auto& interval : intervals) {
                    if (interval.second <= interval.first)
                        continue;
                    if (!gaps.empty() && interval.first <= gaps.back().second + 2. * SCALING_FACTOR)
                        gaps.back().second = std::max(gaps.back().second, interval.second);
                    else
                        gaps.push_back(interval);
                }
                for (const auto& gap : gaps) {
                    const double length = gap.second - gap.first;
                    unsupported += length;
                    if (length > path.bridge_span_mm + 1e-6)
                        return reject_bridge("long_side_supported_gap");
                    const bool at_start = gap.first <= 2. * SCALING_FACTOR;
                    const bool at_end   = gap.second >= position - 2. * SCALING_FACTOR;
                    if (!closed && ((at_start && start_contact.second == 0.) || (at_end && finish_contact.second == 0.)))
                        return reject_bridge("unanchored_endpoint");
                    if (closed && (at_start || at_end))
                        seam_gap += length;
                }
            } else
                for (const auto& gap : diff_pl(Polylines{line}, contact)) {
                    const double length = gap.length() * SCALING_FACTOR;
                    unsupported += length;
                    if (length > path.bridge_span_mm + 1e-6)
                        return reject_bridge("long_gap");
                    const bool at_start = (gap.points.front() - line.points.front()).cast<double>().norm() <= 2. ||
                                          (gap.points.back() - line.points.front()).cast<double>().norm() <= 2.;
                    const bool at_end = (gap.points.front() - line.points.back()).cast<double>().norm() <= 2. ||
                                        (gap.points.back() - line.points.back()).cast<double>().norm() <= 2.;
                    if (!closed && ((at_start && start_contact.second == 0.) || (at_end && finish_contact.second == 0.)))
                        return reject_bridge("unanchored_endpoint");
                    if (closed && (at_start || at_end))
                        seam_gap += length;
                }
            // A short cap can have less than one bead width of total motion.
            // In that case require all its arclength to be anchored, rather
            // than an impossible length greater than the complete run.
            // A clipped run can taper. Each finite cap contributes half its
            // own width; requiring the first width at both ends incorrectly
            // rejects a separately qualified, narrower finishing anchor.
            const double anchor_length = std::min(line.length() * SCALING_FACTOR,
                                                  .5 * (path.segments.front().width_mm + path.segments.back().width_mm));
            if (!start_contact.first || !finish_contact.first)
                return reject_bridge("insufficient_end_cap_contact");
            if (seam_gap > path.bridge_span_mm + 1e-6 ||
                line.length() * SCALING_FACTOR - unsupported + start_contact.second + finish_contact.second + 1e-6 < anchor_length)
                return reject_bridge("insufficient_anchor_or_long_seam");
            for (size_t anchor : anchors)
                edge(anchor, owner, DependencyKind::Support);
            bridge_valid[owner] = 1;
            return true;
        };
        // Every lower-face point must be on the bed or on a preceding upper
        // face. Intersections are clipped in XY and affine heights compared at
        // all polygon vertices, establishing the bound throughout each face.
        size_t     checked_support_cells = 0;
        size_t     cached_support_owner  = STATIC;
        ExPolygons cached_support;
        ExPolygons cached_direct_floor;
        const auto validate_support = [&](const Cell& cell) -> std::optional<Failure> {
            if (cell.owner == STATIC)
                return {};
            poll();
            if (std::getenv("MATERIAL_BAND_TRACE") && ++checked_support_cells % 100000 == 0)
                std::clog << "Material support cells=" << checked_support_cells << '/' << cells.size()
                          << " candidates=" << result.candidate_pairs << " index_visits=" << result.index_visits << " bytes=" << bytes
                          << std::endl;
            responsible = nodes[cell.owner].path->band;
            witness     = (cell.bead->upper[0] + cell.bead->upper[1] + cell.bead->upper[2]) / 3.;
            bool bed    = true;
            for (double z : cell.bead->lower_z)
                bed &= std::abs(z - input.bed_z_mm) <= input.support_tolerance_mm;
            const auto&  node           = nodes[cell.owner];
            const bool   planar_support = node.planar;
            const double overhang       = node.path->overhang_mm;
            if (bed && node.path->role == PathRole::Perimeter && !floor_classified[cell.owner]) {
                grounded_wall_faces[cell.owner] = node.footprint;
                floor_classified[cell.owner]    = true;
                retain_grounded_wall(cell.owner);
            }
            if (!bed && planar_support && cached_support_owner != cell.owner) {
                // All cells in a planar run share one lower face. Query its
                // actual preceding contact faces once, then check every cell
                // against that same owned coverage. Tiny cap subdivisions must
                // not repeat the full expanded spatial query thousands of times.
                const BoundingBox xy_bounds(to_points(node.footprint));
                Bounds            contact_bounds;
                contact_bounds.lo = Vec3d(unscale<double>(xy_bounds.min.x()) + origin.x() - overhang,
                                          unscale<double>(xy_bounds.min.y()) + origin.y() - overhang,
                                          node.bottom - input.support_tolerance_mm);
                contact_bounds.hi = Vec3d(unscale<double>(xy_bounds.max.x()) + origin.x() + overhang,
                                          unscale<double>(xy_bounds.max.y()) + origin.y() + overhang,
                                          (machine.coplanar_tip_contact ? node.top : node.bottom) + input.support_tolerance_mm);
                std::set<size_t>           planar_owners, lateral_owners;
                std::map<size_t, Polygons> other_faces;
                for (size_t j : query_pruned(
                         contact_bounds,
                         [&](const Cell& other) {
                             if (other.owner == cell.owner)
                                 return false;
                             if (earlier_coplanar(cell.owner, other.owner))
                                 return lateral_owners.insert(other.owner).second;
                             for (const auto& p : other.bead->upper)
                                 if (std::abs(p.z() - node.bottom) > input.support_tolerance_mm)
                                     return false;
                             // Planar contact below uses the owner's complete
                             // footprint once. Its triangulation must not multiply
                             // the query allocation or narrow-phase work budget.
                             return other.owner == STATIC || !nodes[other.owner].planar || planar_owners.insert(other.owner).second;
                         },
                         [&](size_t owner) {
                             if (owner == cell.owner || planar_owners.count(owner) || lateral_owners.count(owner))
                                 return true;
                             // Planarity is certified within 1e-12 above. Retain
                             // that margin at the contact tolerance boundary.
                             return nodes[owner].planar && !earlier_coplanar(cell.owner, owner) &&
                                    std::abs(nodes[owner].top - node.bottom) > input.support_tolerance_mm + 1e-12;
                         })) {
                    const auto& other = cells[j];
                    if (other.owner == STATIC || !nodes[other.owner].planar)
                        other_faces[other.owner].push_back(other.xy);
                }
                cached_support.clear();
                cached_direct_floor.clear();
                const auto add_contact = [&](size_t owner, const ExPolygons& faces, double reach, bool floor = false) {
                    auto contact = intersection_ex(node.footprint, faces);
                    if (floor && node.path->role == PathRole::Perimeter) {
                        cached_direct_floor.insert(cached_direct_floor.end(), contact.begin(), contact.end());
                    }
                    if (reach > 0.) {
                        // Offset simplification can erase a narrow cap even
                        // for a positive radius. Actual preceding material
                        // remains support regardless of that approximation.
                        auto expanded = intersection_ex(node.footprint, offset_ex(faces, float(scale_(reach))));
                        contact.insert(contact.end(), std::make_move_iterator(expanded.begin()), std::make_move_iterator(expanded.end()));
                    }
                    if (contact.empty())
                        return;
                    if (owner != STATIC)
                        edge(owner, cell.owner, DependencyKind::Support);
                    cached_support.insert(cached_support.end(), std::make_move_iterator(contact.begin()),
                                          std::make_move_iterator(contact.end()));
                    size_t points = 0;
                    for (const auto* polygons : {&cached_support, &cached_direct_floor})
                        for (const auto& polygon : *polygons) {
                            points += polygon.contour.points.size();
                            for (const auto& hole : polygon.holes)
                                points += hole.points.size();
                        }
                    if (points * sizeof(Point) * 4 + (cached_support.size() + cached_direct_floor.size()) * sizeof(ExPolygon) * 4 >
                        limits.max_temporary_bytes - bytes)
                        throw Abort{FailureCode::ResourceLimit, "schedule_support_cache_limit"};
                };
                for (size_t owner : planar_owners)
                    add_contact(owner, nodes[owner].footprint, overhang, true);
                for (const auto& [owner, faces] : other_faces)
                    add_contact(owner, union_ex(faces), overhang, true);
                double width = node.path->segments.front().width_mm;
                for (const auto& segment : node.path->segments)
                    width = std::min(width, segment.width_mm);
                cached_direct_floor = union_ex(cached_direct_floor);
                if (node.path->role == PathRole::Perimeter && !floor_classified[cell.owner]) {
                    const auto floor_allowance = precise_support_offset(cached_direct_floor, scale_(std::min(overhang, width / 2.)) + 8.);
                    ExPolygons grounded;
                    for (const auto& segment : node.path->segments) {
                        Polygons pieces;
                        for (const auto& bead : segment.cells)
                            pieces.push_back(footprint(bead, origin));
                        const auto face      = union_ex(pieces);
                        double     perimeter = 0.;
                        for (const auto& polygon : to_polygons(face))
                            perimeter += polygon.length() * SCALING_FACTOR;
                        // Classify each bounded motion segment using its own
                        // actual floor contact. A narrow seam between floor
                        // beads need not cover the centerline, but at least
                        // half the footprint must be supported directly and
                        // all of it must fit the ordinary half-bead allowance.
                        const double floor_contact = area_mm2(intersection_ex(face, cached_direct_floor));
                        const auto   outside_floor = diff_ex(face, floor_allowance);
                        if (floor_contact + 8. * SCALING_FACTOR * perimeter >= .5 * area_mm2(face) && outside_floor.empty())
                            grounded.insert(grounded.end(), face.begin(), face.end());
                    }
                    grounded_wall_faces[cell.owner] = union_ex(grounded);
                    floor_classified[cell.owner]    = true;
                    retain_grounded_wall(cell.owner);
                }
                // Integer unions may split one wall at point contacts or a
                // rounded edge. Group only those numerically touching pieces
                // for the contact test, then clip back to the unchanged bead
                // footprint. This creates no deposition across a real gap.
                const bool wall_contact       = node.path->role == PathRole::Perimeter;
                const auto contact_components = wall_contact && !lateral_owners.empty() ? precise_support_offset(node.footprint, 2.) :
                                                                                          node.footprint;
                for (size_t owner : lateral_owners) {
                    // A narrow gap lane can attach continuously to an earlier
                    // full-height bead. Require an actual shared side, not just
                    // proximity, and bound the extension by both this lane's
                    // square-cap diagonal and the declared overhang. A fitted
                    // corner can be farther from a side than its transverse
                    // width. Each connected piece
                    // must supply its own contact; an isolated cap cannot borrow
                    // another piece's attachment.
                    const auto& side_faces = node.path->role == PathRole::Perimeter ? grounded_wall_faces[owner] : nodes[owner].footprint;
                    const auto  touching   = offset_ex(side_faces, 2.f); // Two XY quanta for shared-edge rounding.
                    for (const auto& connected : contact_components) {
                        const auto component      = wall_contact ? intersection_ex(node.footprint, ExPolygons{connected}) :
                                                                   ExPolygons{connected};
                        double     contact_length = 0.;
                        for (const auto& line : intersection_pl(to_polylines(component), touching))
                            contact_length += line.length() * SCALING_FACTOR;
                        if (contact_length + 4. * SCALING_FACTOR < width)
                            continue;
                        const double reach    = node.path->role == PathRole::Perimeter ? std::sqrt(2.) * width :
                                                                                         std::min(overhang, std::sqrt(2.) * width);
                        const auto   expanded = node.path->role == PathRole::Perimeter ? precise_support_offset(side_faces, scale_(reach)) :
                                                                                         offset_ex(side_faces, float(scale_(reach)));
                        const auto   attached = intersection_ex(component, expanded);
                        add_contact(owner, attached, 0.);
                    }
                }
                cached_support       = union_ex(cached_support);
                cached_support_owner = cell.owner;
            }
            Polygons supported;
            // Match the broad phase to the contact predicate below. Arithmetic
            // closure can put a preceding cap one ulp below this lower face.
            auto support_bounds = cell.bounds;
            if (planar_support) {
                // Lower-face contact is already cached. This query checks only
                // positive-volume intersections, including within the same run.
                support_bounds.lo.z() += input.support_tolerance_mm;
                support_bounds.hi.z() -= input.support_tolerance_mm;
            } else {
                support_bounds.lo.head<2>().array() -= overhang;
                support_bounds.hi.head<2>().array() += overhang;
                support_bounds.lo.z() -= input.support_tolerance_mm;
                support_bounds.hi.z() += input.support_tolerance_mm;
            }
            const auto support_candidates = query(support_bounds, [&](const Cell& other) {
                // Edge-only XY contact has no support or overlap area.
                // Exclude it before charging the narrow-phase budget;
                // dense tower strips otherwise revisit adjacent rows.
                if (&other == &cell)
                    return false;
                // The support margin applies only to a lower-face contact.
                // Same-height neighbors and this path's own cells need an
                // actual XY intersection to overlap. Applying the expanded
                // margin to them makes detailed caps grow quadratically.
                bool         expanded_contact = !planar_support && !bed && overhang > 0. && other.owner != cell.owner;
                const double bottom           = cell.bead->lower_z.front();
                for (size_t k = 0; expanded_contact && k < 3; ++k)
                    expanded_contact = std::abs(cell.bead->lower_z[k] - bottom) <= input.support_tolerance_mm &&
                                       std::abs(other.bead->upper[k].z() - bottom) <= input.support_tolerance_mm;
                if (!expanded_contact &&
                    (other.bounds.lo.z() >= cell.bounds.hi.z() - input.support_tolerance_mm ||
                     std::min(cell.bounds.hi.x(), other.bounds.hi.x()) <= std::max(cell.bounds.lo.x(), other.bounds.lo.x()) ||
                     std::min(cell.bounds.hi.y(), other.bounds.hi.y()) <= std::max(cell.bounds.lo.y(), other.bounds.lo.y()) ||
                     !Geometry::convex_polygons_intersect(cell.xy, other.xy)))
                    return false;
                return std::min(support_bounds.hi.x(), other.bounds.hi.x()) > std::max(support_bounds.lo.x(), other.bounds.lo.x()) &&
                       std::min(support_bounds.hi.y(), other.bounds.hi.y()) > std::max(support_bounds.lo.y(), other.bounds.lo.y());
            });
            for (size_t j : support_candidates) {
                const auto& other = cells[j];
                if (&other == &cell || !support_bounds.overlaps(other.bounds))
                    continue;
                if (!planar_support && !bed && overhang > 0. && other.owner != cell.owner) {
                    bool         planar_contact = true;
                    const double bottom         = cell.bead->lower_z.front();
                    for (size_t k = 0; k < 3; ++k)
                        planar_contact &= std::abs(cell.bead->lower_z[k] - bottom) <= input.support_tolerance_mm &&
                                          std::abs(other.bead->upper[k].z() - bottom) <= input.support_tolerance_mm;
                    if (planar_contact) {
                        auto reach = intersection_ex(Polygons{cell.xy}, offset_ex(Polygons{other.xy}, float(scale_(overhang))));
                        if (!reach.empty()) {
                            auto polygons = to_polygons(reach);
                            supported.insert(supported.end(), polygons.begin(), polygons.end());
                            if (other.owner != STATIC)
                                edge(other.owner, cell.owner, DependencyKind::Support);
                        }
                    }
                }
                auto overlap = intersection_ex(Polygons{cell.xy}, Polygons{other.xy});
                // Independent triangulations share integer polygon boundaries.
                // Intersection rounding can produce a strip at most one XY
                // quantum wide; this is not a second physical deposit.
                const double xy_rounding_area = 2. * SCALING_FACTOR * std::min(cell.xy.length(), other.xy.length()) * SCALING_FACTOR;
                const double overlap_area     = area_mm2(overlap);
                if (overlap_area <= 0.)
                    continue;
                const bool numerical_overlap = overlap_area <= std::max(1e-12, xy_rounding_area);
                bool       match = true, above = true, below = true;
                for (const auto& p : to_points(overlap)) {
                    const Vec2d  xy  = p.cast<double>() * SCALING_FACTOR + origin;
                    const double gap = face_z(*cell.bead, xy, false) - face_z(*other.bead, xy, true);
                    match &= std::abs(gap) <= input.support_tolerance_mm;
                    above &= gap >= -input.support_tolerance_mm;
                    below &= face_z(*other.bead, xy, false) - face_z(*cell.bead, xy, true) >= -input.support_tolerance_mm;
                }
                if (!above && !below && !numerical_overlap) {
                    if (std::getenv("MATERIAL_BAND_TRACE"))
                        std::clog << "Material overlap paths=" << nodes[cell.owner].path->id << ','
                                  << (other.owner == STATIC ? 0 : nodes[other.owner].path->id) << " area=" << overlap_area
                                  << " rounding=" << xy_rounding_area << std::endl;
                    if (const char* directory = std::getenv("MATERIAL_BAND_GEOMETRY_DUMP")) {
                        std::ofstream dump(std::string(directory) + "/schedule-overlap.tsv");
                        dump << std::setprecision(17) << "cell\tobject\tpath\tsegment\tdomain\tmaterial\tx\ty\tbottom\ttop\n";
                        size_t index = 0;
                        for (const auto* item : {&cell, &other}) {
                            const auto* node = item->owner == STATIC ? nullptr : &nodes[item->owner];
                            for (size_t k = 0; k < 3; ++k)
                                dump << index << '\t' << (node ? node->id.object : 0) << '\t' << (node ? node->id.path : 0) << '\t'
                                     << item->segment << '\t' << (node ? node->path->band.domain : 0) << '\t'
                                     << (node ? node->path->band.material.value : 0) << '\t' << item->bead->upper[k].x() << '\t'
                                     << item->bead->upper[k].y() << '\t' << item->bead->lower_z[k] << '\t' << item->bead->upper[k].z()
                                     << '\n';
                            ++index;
                        }
                    }
                    return fail(FailureCode::MaterialToleranceExceeded, "overlapping_deposition_cells");
                }
                if (planar_support || bed || !match || other.owner == cell.owner)
                    continue;
                if (other.owner != STATIC)
                    edge(other.owner, cell.owner, DependencyKind::Support);
                const auto polygons = to_polygons(overlap);
                supported.insert(supported.end(), polygons.begin(), polygons.end());
            }
            // Independent rotated scan frames introduce two coordinate
            // roundings per layer, followed by clipping and triangulation.
            // Bound that numerical boundary uncertainty by perimeter times
            // eight integer XY quanta (8 nm), never a bead-width allowance.
            const auto   cell_support     = planar_support ? ExPolygons{} : union_ex(supported);
            const auto&  support_union    = planar_support ? cached_support : cell_support;
            const double unsupported_area = bed ? 0. : area_mm2(diff_ex(Polygons{cell.xy}, support_union));
            const double rounding_area    = 8. * cell.xy.length() * SCALING_FACTOR * SCALING_FACTOR;
            if (!bed && unsupported_area > std::max(1e-9, rounding_area) &&
                !anchored_bridge(cell.owner, planar_support ? &cached_support : nullptr)) {
                if (std::getenv("MATERIAL_BAND_TRACE"))
                    std::clog << "Material unsupported path=" << nodes[cell.owner].path->id << " area=" << unsupported_area
                              << " rounding=" << rounding_area << " bottom=" << cell.bead->lower_z.front()
                              << " witness=" << witness->transpose() << std::endl;
                return fail(FailureCode::InfeasibleThickness, "unsupported_bead_footprint");
            }
            return {};
        };
        const size_t support_work_bytes = nodes.size() * (sizeof(std::pair<size_t, size_t>) + 5 * sizeof(size_t));
        {
            admit(support_work_bytes);
            std::vector<std::pair<size_t, size_t>> ranges(nodes.size(), {STATIC, STATIC});
            for (size_t c = 0; c < cells.size(); ++c)
                if (cells[c].owner != STATIC) {
                    auto& range = ranges[cells[c].owner];
                    if (range.first == STATIC)
                        range.first = c;
                    range.second = c + 1;
                }
            if (!flexible) {
                std::vector<size_t> support_order(nodes.size());
                std::iota(support_order.begin(), support_order.end(), 0);
                std::sort(support_order.begin(), support_order.end(), [&](size_t a, size_t b) {
                    return total_order ? explicit_rank[a] < explicit_rank[b] :
                                         std::tie(nodes[a].id.object, phase_rank[a]) < std::tie(nodes[b].id.object, phase_rank[b]);
                });
                for (size_t owner : support_order) {
                    for (size_t c = ranges[owner].first; c < ranges[owner].second; ++c)
                        if (const auto failure = validate_support(cells[c]))
                            return *failure;
                    complete_support(owner);
                }
            } else {
                std::vector<size_t> completed;
                const auto          same_pass = [&](size_t a, size_t b) {
                    const auto& x      = nodes[a];
                    const auto& y      = nodes[b];
                    const auto& a_band = x.path->band;
                    const auto& b_band = y.path->band;
                    return std::tie(a_band.domain, a_band.macro_cycle, a_band.material.value, a_band.stack_slot) ==
                               std::tie(b_band.domain, b_band.macro_cycle, b_band.material.value, b_band.stack_slot) &&
                           std::abs(x.bottom - y.bottom) <= input.support_tolerance_mm &&
                           std::abs(x.top - y.top) <= input.support_tolerance_mm;
                };
                for (size_t first = 0; first < flexible_order.size();) {
                    size_t last = first + 1;
                    while (last < flexible_order.size() && same_pass(flexible_order[first], flexible_order[last]))
                        ++last;
                    std::vector<size_t> pending(flexible_order.begin() + first, flexible_order.begin() + last);
                    while (!pending.empty()) {
                        poll();
                        std::vector<size_t>    deferred;
                        std::optional<Failure> first_failure;
                        for (size_t owner : pending) {
                            bridge_valid[owner]  = -1;
                            cached_support_owner = STATIC;
                            std::optional<Failure> failure;
                            for (size_t c = ranges[owner].first; c < ranges[owner].second; ++c)
                                if ((failure = validate_support(cells[c])))
                                    break;
                            if (failure) {
                                if (failure->detail != "unsupported_bead_footprint")
                                    return *failure;
                                if (!first_failure)
                                    first_failure = failure;
                                deferred.push_back(owner);
                            } else {
                                complete_support(owner);
                                completed.push_back(owner);
                            }
                        }
                        // No floating group may bootstrap itself. An anchor must
                        // pass its own full cell checks before supplying contact.
                        if (deferred.size() == pending.size())
                            return *first_failure;
                        pending = std::move(deferred);
                    }
                    first = last;
                }
                for (size_t i = 0; i < completed.size(); ++i) {
                    phase_rank[completed[i]] = i;
                    if (i)
                        edge(completed[i - 1], completed[i], DependencyKind::Phase);
                }
            }
        }
        bytes -= support_work_bytes;
        if (std::getenv("MATERIAL_BAND_TRACE"))
            std::clog << "Material support complete paths=" << nodes.size() << " index_visits=" << result.index_visits << std::endl;
        const auto contact = [&](const Node& node, const Vec3d& from, const Vec3d& to, const Cell& occupied) {
            // Contact is limited to this deposition run's declared footprint
            // and a bounded vertical surface approximation. It never exempts
            // a head section or a neighboring tall object.
            const Vec2d  delta = to.head<2>() - from.head<2>();
            Points       capsule;
            const double radius = (machine.sections.front().radius_mm + machine.clearance_mm + .001) / std::cos(PI / SIDES);
            for (const auto& end : {from, to})
                for (size_t j = 0; j < SIDES; ++j) {
                    const double angle = (2. * j + 1.) * PI / SIDES;
                    const Vec2d  xy    = end.head<2>() - origin + radius * Vec2d(std::cos(angle), std::sin(angle));
                    capsule.emplace_back(coord_t(std::llround(xy.x() / SCALING_FACTOR)), coord_t(std::llround(xy.y() / SCALING_FACTOR)));
                }
            const auto         overlap = intersection_ex(Polygons{occupied.xy}, Polygons{Geometry::convex_hull(std::move(capsule))});
            std::vector<Vec2d> checks;
            for (const auto& poly : overlap) {
                const auto& points = poly.contour.points;
                for (size_t j = 0; j < points.size(); ++j) {
                    const Vec2d a = points[j].cast<double>() * SCALING_FACTOR + origin;
                    const Vec2d b = points[(j + 1) % points.size()].cast<double>() * SCALING_FACTOR + origin;
                    checks.push_back(a);
                    if (delta.squaredNorm() < 1e-20)
                        continue;
                    const double ta = (a - from.head<2>()).dot(delta) / delta.squaredNorm();
                    const double tb = (b - from.head<2>()).dot(delta) / delta.squaredNorm();
                    // The clamped height is affine on each of three strips.
                    // Include vertices created by those strip boundaries.
                    for (double boundary : {0., 1.})
                        if ((ta < boundary && tb > boundary) || (tb < boundary && ta > boundary))
                            checks.push_back(a + (b - a) * ((boundary - ta) / (tb - ta)));
                }
            }
            bool coplanar = machine.coplanar_tip_contact;
            for (const auto& xy : checks) {
                const double t = delta.squaredNorm() > 1e-20 ? std::clamp((xy - from.head<2>()).dot(delta) / delta.squaredNorm(), 0., 1.) :
                                                               0.;
                const double difference = face_z(*occupied.bead, xy, true) - (from.z() + t * (to.z() - from.z()));
                coplanar                = coplanar && difference <= 1e-8;
                if (difference > input.contact_tolerance_mm)
                    return false;
            }
            return coplanar || area_mm2(diff_ex(overlap, node.footprint)) <= 1e-10;
        };
        const auto motion_bounds = [&](const Vec3d& a, const Vec3d& b) {
            Bounds box;
            box.add(a);
            box.add(b);
            const double r = (radius + machine.clearance_mm + .001) / std::cos(PI / SIDES);
            box.lo -= Vec3d(r, r, .001);
            box.hi += Vec3d(r, r, top + machine.clearance_mm + .001);
            return box;
        };
        const auto motion_candidates = [&](const Vec3d& a, const Vec3d& b, double occupied_height, const auto& eligible,
                                           size_t rank_limit = STATIC) {
            std::vector<size_t> found;
            // Keep the narrow nozzle and the elevated wide head in separate
            // broad-phase queries. A single enclosing box assigns the head's
            // radius to the nozzle tip and revisits almost every dense cell.
            // The exact swept-section tests below remain unchanged.
            for (const auto& section : machine.sections) {
                Bounds bounds;
                bounds.add(a);
                bounds.add(b);
                const double r = (section.radius_mm + machine.clearance_mm + .001) / std::cos(PI / SIDES);
                bounds.lo += Vec3d(-r, -r, section.bottom_mm - machine.clearance_mm - .001);
                bounds.hi += Vec3d(r, r, section.top_mm + machine.clearance_mm + .001);
                bounds.hi.z() = std::min(bounds.hi.z(), occupied_height);
                if (machine.coplanar_tip_contact)
                    bounds.lo.z() = std::max(bounds.lo.z(), std::min(a.z(), b.z()) + 1e-8);
                if (bounds.lo.z() > bounds.hi.z())
                    continue;
                auto section_cells = query_pruned(bounds, eligible, [](size_t) { return false; }, rank_limit);
                if (section_cells.size() > (limits.max_temporary_bytes - bytes) / (2 * sizeof(size_t)) - found.size())
                    throw Abort{FailureCode::ResourceLimit, "schedule_query_memory_limit"};
                found.insert(found.end(), section_cells.begin(), section_cells.end());
            }
            std::sort(found.begin(), found.end());
            found.erase(std::unique(found.begin(), found.end()), found.end());
            return found;
        };
        // A collision with another future path adds an access edge, while a
        // collision with an earlier segment of this run cannot be reordered.
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (std::getenv("MATERIAL_BAND_TRACE") && i % 100000 == 0)
                std::clog << "Material access paths=" << i << '/' << nodes.size() << " index_visits=" << result.index_visits << std::endl;
            const auto& node = nodes[i];
            responsible      = node.path->band;
            for (size_t segment = 0; segment < node.path->segments.size(); ++segment) {
                poll();
                const Vec3d from = node.path->vertices[segment], to = node.path->vertices[segment + 1];
                auto        bounds = motion_bounds(from, to);
                // Future phases of this object cannot be occupied yet. Bound
                // the query by the highest eligible deposit, not the head top.
                bounds.hi.z() = std::min(bounds.hi.z(), prior_top[i]);
                for (size_t j : motion_candidates(
                         from, to, prior_top[i],
                         [&](const Cell& c) {
                             if (c.owner == STATIC)
                                 return true;
                             if (c.owner == i)
                                 return c.segment <= segment;
                             if (total_order)
                                 return explicit_rank[c.owner] < explicit_rank[i];
                             // The fixed phase chain already ensures this path precedes
                             // future material of the same object. No redundant access
                             // edge or collision calculation is needed for that pair.
                             return nodes[c.owner].id.object != node.id.object || phase_rank[c.owner] < phase_rank[i];
                         },
                         total_order ? explicit_rank[i] + 2 : STATIC)) {
                    const auto& c = cells[j];
                    if (!bounds.overlaps(c.bounds) || (c.owner == i && c.segment > segment))
                        continue;
                    if (machine.coplanar_tip_contact && c.bounds.hi.z() <= std::min(from.z(), to.z()) + 1e-8)
                        continue; // Entire occupied cell is below the flat cylinder bottom.
                    for (const auto& section : machine.sections) {
                        const double vertical = section.bottom_mm == 0. ? .001 : machine.clearance_mm + .001;
                        if (c.owner == i && c.segment == segment && section.bottom_mm == 0.)
                            continue; // Newly deposited material is the intended tip contact.
                        if (!swept_section_intersects(from, to, section, *c.bead, machine.clearance_mm + .001, vertical))
                            continue;
                        if (section.bottom_mm == 0. && contact(node, from, to, c))
                            continue;
                        if (c.owner == STATIC)
                            return fail(FailureCode::ClearanceFailure, "deposition_hits_occupied_geometry");
                        if (c.owner == i)
                            return fail(FailureCode::ClearanceFailure, "deposition_self_access_conflict");
                        edge(i, c.owner, DependencyKind::Access);
                        witness = from;
                    }
                }
            }
        }
        if (std::getenv("MATERIAL_BAND_TRACE"))
            std::clog << "Material access complete index_visits=" << result.index_visits << std::endl;
        // Among eligible operations prefer the earliest logical cycle/slot.
        // This keeps independent objects on a common macro-cycle frontier and
        // avoids needless A/B/A switches solely because of object IDs.
        using ReadyKey       = std::tuple<uint64_t, uint32_t, OperationRef, size_t>;
        const auto ready_key = [&](size_t i) {
            return ReadyKey{nodes[i].path->band.macro_cycle, nodes[i].path->band.stack_slot, nodes[i].id, i};
        };
        std::set<ReadyKey> ready;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (indegree[i] == 0)
                ready.insert(ready_key(i));
        std::vector<size_t> order;
        while (!ready.empty()) {
            const size_t i = std::get<3>(*ready.begin());
            ready.erase(ready.begin());
            order.push_back(i);
            for (size_t j : outgoing[i])
                if (--indegree[j] == 0)
                    ready.insert(ready_key(j));
        }
        if (order.size() != nodes.size())
            return fail(FailureCode::IncompatibleOrder, "support_access_dependency_cycle");
        // The graph is frozen in result.dependencies. Its mutable adjacency
        // sets are no longer needed while constructing validated travel.
        std::vector<std::set<size_t>>().swap(outgoing);
        bytes -= result.dependencies.size() * 96;
        std::vector<bool> deposited(nodes.size(), false);
        double            safe_z = input.bed_z_mm;
        for (const auto& c : cells)
            safe_z = std::max(safe_z, c.bounds.hi.z());
        safe_z += machine.travel_lift_mm;
        Vec3d      position          = input.initial_position;
        size_t     travel_rank_limit = STATIC;
        const auto travel            = [&](const Vec3d& destination, std::vector<Vec3d>& path, const Node* touching = nullptr) {
            if ((destination - position).norm() <= 1e-12)
                return;
            poll();
            auto bounds   = motion_bounds(position, destination);
            bounds.hi.z() = std::min(bounds.hi.z(), occupied_top);
            for (size_t j : motion_candidates(
                     position, destination, occupied_top, [&](const Cell& c) { return c.owner == STATIC || deposited[c.owner]; },
                     travel_rank_limit)) {
                const auto& c = cells[j];
                if (!bounds.overlaps(c.bounds) || (c.owner != STATIC && !deposited[c.owner]))
                    continue;
                if (machine.coplanar_tip_contact && c.bounds.hi.z() <= std::min(position.z(), destination.z()) + 1e-8)
                    continue;
                for (const auto& section : machine.sections) {
                    // The tip can touch an already deposited upper surface at
                    // lift/descent endpoints, but may not enter its volume.
                    const double vertical = section.bottom_mm == 0. ? .001 : machine.clearance_mm + .001;
                    if (!swept_section_intersects(position, destination, section, *c.bead, machine.clearance_mm + .001, vertical))
                        continue;
                    if (touching && section.bottom_mm == 0. && (position.head<2>() - destination.head<2>()).norm() < 1e-12) {
                        const Vec3d endpoint = position.z() < destination.z() ? position : destination;
                        if (contact(*touching, endpoint, endpoint, c))
                            continue;
                    }
                    throw Abort{FailureCode::ClearanceFailure, "travel_hits_occupied_geometry"};
                }
            }
            if (path.empty())
                path.push_back(position);
            path.push_back(destination);
            position = destination;
        };
        const Node* previous = nullptr;
        for (size_t i : order) {
            travel_rank_limit = total_order ? explicit_rank[i] + 1 : STATIC;
            if (std::getenv("MATERIAL_BAND_TRACE") && result.order.size() % 100000 == 0)
                std::clog << "Material travel paths=" << result.order.size() << '/' << nodes.size()
                          << " index_visits=" << result.index_visits << std::endl;
            responsible = nodes[i].path->band;
            ScheduledPath item;
            item.operation       = nodes[i].id;
            item.requires_bridge = bridge_valid[i] == 1;
            const Vec3d start    = nodes[i].path->vertices.front();
            bool        direct   = false;
            if (previous && machine.coplanar_tip_contact && previous->path->band.material.value == nodes[i].path->band.material.value &&
                std::abs(position.z() - start.z()) <= 1e-12) {
                try {
                    // Keep short planar moves at the current height, but check
                    // the same swept head envelope as the lifted route. travel
                    // changes position only after every collision check passes.
                    travel(start, item.travel, &nodes[i]);
                    direct = true;
                } catch (const Abort& e) {
                    if (e.code != FailureCode::ClearanceFailure)
                        throw;
                }
            }
            if (!direct) {
                travel(Vec3d(position.x(), position.y(), std::max(position.z(), safe_z)), item.travel, previous);
                travel(Vec3d(start.x(), start.y(), position.z()), item.travel);
                travel(start, item.travel, &nodes[i]);
            }
            position     = nodes[i].path->vertices.back();
            deposited[i] = true;
            for (const auto& segment : nodes[i].path->segments)
                for (const auto& c : segment.cells)
                    for (const auto& p : c.upper)
                        occupied_top = std::max(occupied_top, p.z());
            previous = &nodes[i];
            admit(sizeof(ScheduledPath) * 2 + 8 * sizeof(Vec3d));
            result.order.push_back(std::move(item));
        }
        travel_rank_limit = STATIC;
        travel(Vec3d(position.x(), position.y(), std::max(position.z(), safe_z)), result.final_travel, previous);
        if (std::getenv("MATERIAL_BAND_TRACE"))
            std::clog << "Material travel complete index_visits=" << result.index_visits << std::endl;
        poll();
        result.objects          = input.objects;
        result.machine          = input.machine;
        result.initial_position = input.initial_position;
        return std::make_shared<const Schedule>(std::move(result));
    } catch (const Abort& e) {
        return fail(e.code, e.detail);
    } catch (const std::bad_alloc&) {
        return fail(FailureCode::ResourceLimit, "schedule_allocation");
    } catch (const std::exception&) {
        return fail(FailureCode::InvalidInput, "schedule_geometry_failure");
    }
}
} // namespace Slic3r::MaterialBand
