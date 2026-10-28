#ifndef slic3r_MaterialBand_InputAdapter_hpp_
#define slic3r_MaterialBand_InputAdapter_hpp_

#include "Contracts.hpp"
#include <functional>

namespace Slic3r {
class ModelObject;
class DynamicPrintConfig;
} // namespace Slic3r

namespace Slic3r::MaterialBand {

struct InputContext
{
    RevisionKey revision;
    std::string source_revision;
    // Supplied by the ordered connected-domain cadence owner, not inferred
    // from absolute layer zero or from a displayed color.
    StackPhase         phase;
    DepositionScope    painted_scope = DepositionScope::PaintedShell;
    BoundaryRule       boundary;
    ProcessConstraints process;
    NumericTolerances  tolerances;
    size_t             max_triangles = 400000;
};

using InputResult = std::variant<std::shared_ptr<const KernelInput>, Failure>;

// Owns a snapshot in object coordinates (volume transformations applied).
// Scope/phase must be resolved by the production caller. No LayerRegion, GUI,
// writer state or borrowed painting buffers survive this call. Supported input
// is one model-part volume and two to four physical materials; unsupported intent is
// reported rather than projected over a whole solid by assumption.
InputResult snapshot_input(const ModelObject&           object,
                           const DynamicPrintConfig&    config,
                           const InputContext&          context,
                           const std::function<bool()>& cancelled = {});

} // namespace Slic3r::MaterialBand
#endif
