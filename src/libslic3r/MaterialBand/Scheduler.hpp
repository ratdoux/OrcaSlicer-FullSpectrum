#ifndef slic3r_MaterialBand_Scheduler_hpp_
#define slic3r_MaterialBand_Scheduler_hpp_

#include "BoxPlanner.hpp"

namespace Slic3r::MaterialBand {

struct OperationRef
{
    uint64_t object = 0, path = 0;
    bool     operator==(const OperationRef& b) const { return object == b.object && path == b.path; }
    bool     operator<(const OperationRef& b) const { return object < b.object || (object == b.object && path < b.path); }
};
enum class DependencyKind { Phase, Support, Access, Explicit };
struct Dependency
{
    OperationRef   before, after;
    DependencyKind kind = DependencyKind::Explicit;
};
struct ScheduleObject
{
    uint64_t                        object = 0;
    std::shared_ptr<const PathPlan> paths;
};
struct ScheduleInput
{
    RevisionKey                 revision;
    MachineEnvelope             machine;
    std::vector<ScheduleObject> objects;
    // Previously deposited material and fixtures, represented by their actual
    // closed cells. A caller must not substitute an object's bounding box.
    std::vector<BeadCell>   occupied;
    std::vector<Dependency> dependencies;
    Vec3d                   initial_position     = Vec3d(0., 0., 10.);
    double                  bed_z_mm             = 0.;
    double                  support_tolerance_mm = 1e-5;
    double                  contact_tolerance_mm = .01;
    // Construction preflight may reorder a single object's coplanar paths
    // within one material/domain pass. Final explicit sequences stay fixed.
    bool allow_planar_reordering = false;
};
struct ScheduledPath
{
    OperationRef operation;
    bool         requires_bridge = false;
    // Direct planar travel when clear, otherwise lift, horizontal move and
    // descent. Exact shared endpoints; zero-length moves are omitted.
    // Every move has passed swept clearance.
    std::vector<Vec3d> travel;
};
struct Schedule
{
    RevisionKey                 revision;
    MachineEnvelope             machine;
    Vec3d                       initial_position = Vec3d::Zero();
    std::vector<ScheduleObject> objects;
    std::vector<Dependency>     dependencies;
    std::vector<ScheduledPath>  order;
    std::vector<Vec3d>          final_travel;
    size_t                      candidate_pairs = 0, index_nodes = 0, index_visits = 0;
};
struct ScheduleLimits
{
    size_t max_paths           = 100000;
    size_t max_cells           = 1000000;
    size_t max_edges           = 1000000;
    size_t max_candidates      = 10000000;
    size_t max_index_visits    = 100000000;
    size_t max_index_nodes     = 4000000;
    size_t max_temporary_bytes = 512 * 1024 * 1024;
};
using ScheduleResult = std::variant<std::shared_ptr<const Schedule>, Failure>;

// Diagnostic only, requiring the declared initial position. Emits exactly the
// validated travel vertices and operation order. No homing, heating or purge.
std::string diagnostic_gcode(const Schedule& schedule, bool relative_extrusion = true, bool reset_each_path = false);

// Conservative fixed phase ordering per object, with geometry-derived support
// and access edges across objects. Cycles fail atomically; this version does
// not split already validated paths. Indexed local queries avoid all-pairs
// adjacency storage. This is NOT a FinalPrintPlan: purge and state operations
// must still be inserted and revalidated by production integration.
ScheduleResult schedule_paths(const ScheduleInput&         input,
                              const ScheduleLimits&        limits    = {},
                              const std::function<bool()>& cancelled = {},
                              const std::function<bool()>& stale     = {});

// Convex swept-volume test, exposed for analytic collision fixtures. Uses a
// circumscribed polygonal cylinder (conservative), not endpoint sampling.
bool swept_section_intersects(const Vec3d&           from,
                              const Vec3d&           to,
                              const EnvelopeSection& section,
                              const BeadCell&        cell,
                              double                 radial_margin_mm   = 0.,
                              double                 vertical_margin_mm = 0.);

} // namespace Slic3r::MaterialBand
#endif
