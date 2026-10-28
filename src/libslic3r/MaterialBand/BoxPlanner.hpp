#ifndef slic3r_MaterialBand_BoxPlanner_hpp_
#define slic3r_MaterialBand_BoxPlanner_hpp_

#include "SlabSolver.hpp"

namespace Slic3r::MaterialBand {

enum class PathRole { Perimeter, DenseFill, SparseFill, Brim, Bridge };

enum class DenseInfillPattern { Monotonic, Rectilinear, Concentric };

struct DenseInfillOptions
{
    DenseInfillPattern pattern             = DenseInfillPattern::Monotonic;
    double             angle_degrees       = 0.;
    bool               alternate_direction = false;
};

struct DenseInfillProfile
{
    DenseInfillOptions internal, top, bottom;
    size_t             top_layers = 0, bottom_layers = 0;

    DenseInfillOptions cycle(size_t index, size_t count) const
    {
        auto result = index < bottom_layers ? bottom : (count - index <= top_layers ? top : internal);
        if (result.alternate_direction && index % 2)
            result.angle_degrees += 90.;
        result.alternate_direction = false;
        return result;
    }
};

// Conservative cylindrical sections of the COMPLETE configured tool, including
// the head. Sections must cover [0, tool_top] without gaps. The tip's intended
// deposition contact zone is limited to the current bead footprint.
struct EnvelopeSection
{
    double bottom_mm = 0.;
    double top_mm    = 0.;
    double radius_mm = 0.;
};

struct MachineEnvelope
{
    // Conservative U1 cylinder can touch a coplanar surface at its bottom;
    // it must never intersect or rise above the nozzle plane outside the bead.
    bool                              coplanar_tip_contact = false;
    std::vector<EnvelopeSection>      sections;
    bool                              shared_nozzle    = false; // Diagnostic E/retraction state; purge is not modeled here.
    double                            clearance_mm     = .001;
    double                            travel_lift_mm   = .4;
    double                            max_volume_mm3_s = 8.;
    double                            max_z_mm_s       = 5.;
    double                            deposition_mm_s  = 30.;
    double                            max_segment_mm   = .5;
    std::array<double, MAX_MATERIALS> filament_diameter_mm{{1.75, 1.75, 1.75, 1.75}};
    size_t                            material_count = 2;
};

// A convex triangular vertical column. Both faces are affine and share XY.
// These owned cells are the occupancy/support authority for the path stage.
struct BeadCell
{
    std::array<Vec3d, 3>  upper;
    std::array<double, 3> lower_z;
};

struct PathSegment
{
    // Vertical rectangular footprint convention. This footprint, not XYZ arc
    // length times an implicit legacy flow, defines the intended volume.
    std::vector<Vec2d> footprint;
    double             volume_mm3      = 0.;
    double             width_mm        = 0.;
    double             height_start_mm = 0.;
    double             height_end_mm   = 0.;
    double             speed_mm_s      = 0.;
    // General clipped footprints can be nonconvex or contain holes. When this
    // is populated it is a disjoint triangulation, replacing `footprint`.
    std::vector<std::array<Vec2d, 3>> coverage_triangles;
    std::vector<BeadCell>             cells;
};

struct BandPath
{
    uint64_t    id = 0;
    BandId      band;
    InterfaceId shared_interface = 0;
    PathRole    role             = PathRole::DenseFill;
    std::string split_reason;
    // Consecutive segments share exactly one vertex in this vector.
    std::vector<Vec3d>       vertices;
    std::vector<PathSegment> segments;
    InterfaceId              lower_interface = 0, upper_interface = 0;
    // Planar FDM support contracts. Zero keeps the exact full-footprint rule.
    double overhang_mm = 0., bridge_span_mm = 0.;
};

struct CoverageDiagnostic
{
    double target_mm3    = 0.;
    double deposited_mm3 = 0.;
    double missing_mm3   = 0.;
    double excess_mm3    = 0.;
};

struct PathPlan
{
    RevisionKey revision;
    // Region stacks may have different interface heights. Their planner supplies
    // a validated height order instead of the uniform global material phase.
    bool                        ordered_region_paths = false;
    bool                        layered_process      = false;
    double                      intentional_void_mm3 = 0.;
    Vec3d                       solid_min, solid_max;
    MachineEnvelope             machine;
    std::vector<InterfacePatch> interfaces;
    // Conservative cycle/phase/coverage order. Every path depends on the
    // preceding path, so support barriers use linear, not all-pairs, storage.
    std::vector<BandPath>                         paths;
    std::array<double, MAX_MATERIALS>             volume_mm3{}; // stack slots, not sorted physical IDs
    std::array<CoverageDiagnostic, MAX_MATERIALS> coverage;
};
using BoxPathPlan = PathPlan;

struct PathLimits
{
    size_t max_cycles          = 10000;
    size_t max_paths           = 100000;
    size_t max_segments        = 1000000;
    size_t max_temporary_bytes = 256 * 1024 * 1024;
};

using BoxPlanResult = std::variant<std::shared_ptr<const BoxPathPlan>, Failure>;

struct BoxPathOptions
{
    size_t perimeter_loops = 1;
    // Restrict both bands and their shared interface to the wall domain. The
    // caller must separately plan the unchanged base-material interior.
    bool shell_only = false;
};

// Isolated-box diagnostic stage. The entire mesh is checked against six
// consistently oriented closed faces; a bounding box or sampled slices alone
// cannot qualify it. The slab domain must cover that box, and its field must
// vary only along X. Other solids/scopes require later geometry stages.
// This result is NOT a FinalPrintPlan and is not a production export route.
BoxPlanResult plan_box(const Mesh&                  solid,
                       const SlabProblem&           cycle,
                       const StackPhase&            phase,
                       const MachineEnvelope&       machine,
                       const PathLimits&            limits    = {},
                       const std::function<bool()>& cancelled = {},
                       const BoxPathOptions&        options   = {});

// Uses the production writer and authoritative volume contract for independent
// reconstruction tests. Travel is above the occupied macro cycle before XY
// motion. Opaque custom G-code, purge and coexisting objects are absent here.
std::string diagnostic_gcode(const BoxPathPlan& plan, bool relative_extrusion = true, bool reset_each_path = false);

} // namespace Slic3r::MaterialBand
#endif
