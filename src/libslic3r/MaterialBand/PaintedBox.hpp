#ifndef slic3r_MaterialBand_PaintedBox_hpp_
#define slic3r_MaterialBand_PaintedBox_hpp_
#include "BoxPlanner.hpp"
namespace Slic3r::MaterialBand {
// Verified box, matching full-height paint stripes on opposing side walls.
// The wall domain is exactly perimeter_loops * bead_width; the base recipe
// independently owns the interior. Unsupported facet layouts fail atomically.
BoxPlanResult plan_painted_box(const KernelInput&           input,
                               const StackPhase&            phase,
                               const MachineEnvelope&       machine,
                               DomainId                     domain,
                               size_t                       perimeter_loops,
                               const PathLimits&            limits    = {},
                               const std::function<bool()>& cancelled = {},
                               const std::function<bool()>& stale     = {},
                               const DenseInfillProfile&    infill    = {});
} // namespace Slic3r::MaterialBand
#endif
