#ifndef slic3r_MaterialBand_FinalPrintPlan_hpp_
#define slic3r_MaterialBand_FinalPrintPlan_hpp_

#include "Scheduler.hpp"

namespace Slic3r {
class GCodeWriter;
namespace MaterialBand {

enum class OperationKind {
    Initialize,
    Home,
    BedTemperature,
    NozzleTemperature,
    Fan,
    ToolChange,
    Retract,
    Recover,
    Travel,
    Deposit,
    U1Start,
    U1ToolChange,
    U1Layer,
    U1End
};
enum class MachineAdapter { StationaryRepRap, SnapmakerU1 };
enum class DepositionKind { Object, Prime, Purge, Brim };

struct PrintOperation
{
    uint64_t           id            = 0;
    OperationKind      kind          = OperationKind::Travel;
    DepositionKind     deposition    = DepositionKind::Object;
    uint64_t           logical_cycle = 0;
    OperationRef       source;
    BandId             band;
    InterfaceId        interface_id    = 0;
    InterfaceId        lower_interface = 0, upper_interface = 0;
    PathRole           role = PathRole::DenseFill;
    PhysicalMaterialId material;
    Vec3d              position   = Vec3d::Zero();
    double             volume_mm3 = 0., speed_mm_s = 0., width_mm = 0., height_start_mm = 0., height_end_mm = 0.;
    double             value = 0.; // temperature or fan percent for state operations
    bool               wait  = false;
};

struct PurgeProfile
{
    bool   enabled  = false;
    Vec2d  origin   = Vec2d(150., 120.);
    double width_mm = 20.;
    // Required minimum volumes. Extra dense tower structure is accounted as
    // purge, never as object deposition. Rows/columns are physical material IDs.
    std::array<std::array<double, MAX_MATERIALS>, MAX_MATERIALS> volume_mm3{};
    std::array<double, MAX_MATERIALS>                            prime_mm3{};
};

struct FinalizeProfile
{
    MachineAdapter                    adapter                 = MachineAdapter::StationaryRepRap;
    double                            bed_probe_offset_mm     = 0.;
    double                            initial_bed_temperature = 60.;
    int                               chamber_cooling_mode    = 1;
    std::array<double, MAX_MATERIALS> initial_nozzle_temperature{{200., 200., 200., 200.}};
    std::array<double, MAX_MATERIALS> initial_wall_speed_mm_s{{30., 30., 30., 30.}};
    std::array<double, MAX_MATERIALS> initial_infill_speed_mm_s{{30., 30., 30., 30.}};
    std::array<double, MAX_MATERIALS> material_fan_percent{};
    std::array<int, MAX_MATERIALS>    fan_disabled_cycles{};
    ProcessConstraints                process;
    NumericTolerances                 tolerances;
    PurgeProfile                      purge;
    Vec2d                             bed_min = Vec2d::Zero(), bed_max = Vec2d(256., 256.);
    double                            max_print_height_mm = 256.;
    bool                              home                = true;
    bool                              relative_extrusion  = true;
    std::array<double, MAX_MATERIALS> nozzle_temperature{{200., 200., 200., 200.}};
    double                            bed_temperature = 60.;
    double                            fan_percent     = 0.;
    std::array<double, MAX_MATERIALS> retract_mm{{1., 1., 1., 1.}};
    std::array<double, MAX_MATERIALS> retract_speed_mm_s{{30., 30., 30., 30.}};
    std::array<double, MAX_MATERIALS> retract_min_travel_mm{};
};

struct ToolTransition
{
    uint64_t           logical_cycle = 0;
    OperationRef       before_path;
    PhysicalMaterialId from, to; // from 0 means initial tool selection
    double             required_purge_mm3 = 0., deposited_purge_mm3 = 0.;
};

struct LogicalCycle
{
    uint64_t                               id       = 0;
    double                                 top_z_mm = 0.;
    std::vector<std::pair<size_t, size_t>> operation_ranges;
};

struct FinalPrintPlan
{
    RevisionKey                       revision;
    MachineEnvelope                   machine;
    FinalizeProfile                   profile;
    std::shared_ptr<const Schedule>   schedule;
    std::vector<PrintOperation>       operations;
    std::vector<ToolTransition>       transitions;
    std::vector<LogicalCycle>         cycles;
    std::array<double, MAX_MATERIALS> object_volume_mm3{}, purge_volume_mm3{}, brim_volume_mm3{};
};

struct FinalizeLimits
{
    ScheduleLimits schedule;
    PathLimits     paths;
    size_t         max_operations      = 2000000;
    size_t         max_purge_layers    = 10000;
    size_t         max_temporary_bytes = 512 * 1024 * 1024;
};
using FinalizeResult = std::variant<std::shared_ptr<const FinalPrintPlan>, Failure>;

// Finalizes object paths first, derives the exact tool transitions, constructs
// the required prime/purge columns, and validates the whole combined sequence.
// Every emitted position/material/state operation is represented in the result.
FinalizeResult finalize_print(const ScheduleInput&         input,
                              const FinalizeProfile&       profile,
                              const FinalizeLimits&        limits    = {},
                              const std::function<bool()>& cancelled = {},
                              const std::function<bool()>& stale     = {});

// Configures and uses the caller's production writer (including its statistics).
// It never creates, removes, reconnects or reorders geometry.
void emit_print(const FinalPrintPlan&                          plan,
                GCodeWriter&                                   writer,
                const std::function<void(const std::string&)>& emit,
                const std::function<bool()>&                   cancelled = {});

} // namespace MaterialBand
} // namespace Slic3r
#endif
