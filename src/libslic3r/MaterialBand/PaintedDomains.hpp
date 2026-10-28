#ifndef slic3r_MaterialBand_PaintedDomains_hpp_
#define slic3r_MaterialBand_PaintedDomains_hpp_
#include "StackPlanner.hpp"

namespace Slic3r::MaterialBand {

// Shared paint-domain geometry for the exact-solid and planar process planners.
// The supplied outline is an actual mesh section, not a bounding rectangle.
std::vector<ExPolygons> partition_painted_domains(const KernelInput&           input,
                                                  const ExPolygons&            outline,
                                                  double                       bottom,
                                                  double                       top,
                                                  double                       wall_width,
                                                  double                       top_depth,
                                                  double                       bottom_depth,
                                                  const std::function<void()>& poll);

// Owned leaf-facet paint -> disjoint shell/cap domains, with independent base
// interior. Geometric contour segmentation is shared; gradient evaluation,
// material stacks, paths and execution remain owned by this kernel.
BoxPlanResult plan_painted_regions(const KernelInput&           input,
                                   const MachineEnvelope&       machine,
                                   DomainId                     domain,
                                   size_t                       perimeter_loops,
                                   const DenseInfillProfile&    infill,
                                   const PathLimits&            limits    = {},
                                   const std::function<bool()>& cancelled = {},
                                   const std::function<bool()>& stale     = {});
} // namespace Slic3r::MaterialBand
#endif
