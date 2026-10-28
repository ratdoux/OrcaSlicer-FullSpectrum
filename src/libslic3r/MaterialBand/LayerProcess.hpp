#ifndef slic3r_MaterialBand_LayerProcess_hpp_
#define slic3r_MaterialBand_LayerProcess_hpp_

#include "PaintedDomains.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::MaterialBand {

struct LayerProcessProfile
{
    double             nominal_height_mm = .2, initial_height_mm = .2;
    double             density              = .15;
    InfillPattern      sparse_pattern       = ipGyroid;
    double             sparse_angle_degrees = 45.;
    double             brim_width_mm = 0., brim_gap_mm = .1;
    bool               outer_brim = true, inner_brim = false;
    size_t             wall_loops   = 2;
    double             top_shell_mm = .8, bottom_shell_mm = .6;
    DenseInfillProfile dense;
};

struct ProcessCycle
{
    double bottom_mm = 0., top_mm = 0.;
};
using ProcessCyclesResult = std::variant<std::vector<ProcessCycle>, Failure>;

// Nominal layers provide a resolution grid. Material cycles may cover several
// grid layers so every active material is printable. The initial cycle retains
// the requested first-layer height; no minimum thickness is silently reduced.
ProcessCyclesResult plan_process_cycles(const KernelInput&           input,
                                        const LayerProcessProfile&   profile,
                                        const PathLimits&            limits    = {},
                                        const std::function<bool()>& cancelled = {});

// Planar process geometry is built directly from the owned source mesh. Its
// explicit wall/fill footprints define deposition; sparse voids are excluded
// from both occupancy and recipe dose. No legacy gradient or Local-Z state.
BoxPlanResult plan_layer_process(const KernelInput&           input,
                                 const MachineEnvelope&       machine,
                                 DomainId                     domain,
                                 const LayerProcessProfile&   profile,
                                 const PathLimits&            limits    = {},
                                 const std::function<bool()>& cancelled = {},
                                 const std::function<bool()>& stale     = {});

} // namespace Slic3r::MaterialBand
#endif
