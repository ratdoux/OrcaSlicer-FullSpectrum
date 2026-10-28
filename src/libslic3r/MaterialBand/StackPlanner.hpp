#ifndef slic3r_MaterialBand_StackPlanner_hpp_
#define slic3r_MaterialBand_StackPlanner_hpp_

#include "ClippedPlanner.hpp"

namespace Slic3r::MaterialBand {

struct StackDoseCycle
{
    MaterialFractions target{};
    double            height_mm = 0., volume_mm3 = 0.;
};
struct StackDoseSolution
{
    std::vector<MaterialFractions> fractions;
    MaterialFractions              target_volume_mm3{}, volume_mm3{};
    double                         maximum_fraction_error = 0.;
    size_t                         projection_sweeps      = 0;
};
using StackDoseResult = std::variant<std::shared_ptr<const StackDoseSolution>, Failure>;

// Fixed physical order, with genuinely absent bands. Enumerate bounded active
// sets, then jointly project cycle closure and every material's global dose.
// No epsilon material is added to pure gradient stops. Infeasible local/global
// budgets fail rather than silently changing the gradient's material usage.
StackDoseResult solve_stack_doses(const std::vector<StackDoseCycle>& cycles,
                                  size_t                             materials,
                                  const ProcessConstraints&          process,
                                  double                             fraction_error,
                                  const NumericTolerances&           tolerances,
                                  const RevisionKey&                 revision,
                                  const SolverLimits&                limits    = {},
                                  const std::function<bool()>&       cancelled = {},
                                  const std::function<bool()>&       stale     = {});

MaterialFractions sample_height_gradient(const RegionIntent& intent, double progress);

BoxPlanResult plan_material_stack(const KernelInput&           input,
                                  const MachineEnvelope&       machine,
                                  DomainId                     domain,
                                  size_t                       perimeter_loops,
                                  const DenseInfillProfile&    infill,
                                  const PathLimits&            limits    = {},
                                  const std::function<bool()>& cancelled = {},
                                  const std::function<bool()>& stale     = {});
} // namespace Slic3r::MaterialBand
#endif
