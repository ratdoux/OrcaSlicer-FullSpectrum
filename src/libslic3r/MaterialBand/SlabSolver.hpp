#ifndef slic3r_MaterialBand_SlabSolver_hpp_
#define slic3r_MaterialBand_SlabSolver_hpp_

#include "Contracts.hpp"
#include <functional>

namespace Slic3r::MaterialBand {

// Numerical stage only: the geometry owner supplies a conforming, nonoverlapping
// triangulation of the intended deposition domain. This does not certify a solid
// or a printable path. Fractions and budgets are nodal piecewise-linear fields.
struct SlabProblem
{
    RevisionKey                          revision;
    InterfaceId                          interface_id = 0;
    DomainId                             domain       = 0;
    uint64_t                             macro_cycle  = 0;
    double                               bottom_mm    = 0.;
    double                               top_mm       = .24;
    std::vector<Vec2d>                   vertices;
    std::vector<std::array<uint32_t, 3>> triangles;
    std::vector<double>                  target_fraction_a;
    std::vector<double>                  local_fraction_error;
    // Signed XY distance to the intended paint boundary, supplied by its owner.
    // Required for AllowTransition; larger local error is confined to half the
    // declared transition width on either side. Empty for PreservePaintedEdge.
    std::vector<double> boundary_distance_mm;
    // Independently integrated recipe budget, not recomputed from blurred data.
    double             target_volume_a_mm3  = 0.;
    bool               discontinuous_target = false;
    BoundaryRule       boundary;
    ProcessConstraints process;
    NumericTolerances  tolerances;
};

struct SolverLimits
{
    size_t max_vertices           = 200000;
    size_t max_triangles          = 400000;
    size_t max_projection_sweeps  = 4000;
    size_t max_optimization_steps = 200;
    size_t max_temporary_bytes    = 128 * 1024 * 1024;
    double convergence_mm         = 1e-9;
    // Objective: half squared target error + weight/2 * sum(edge height jump^2).
    // Zero preserves feasible analytic targets. Smoothing is an explicit choice.
    double smoothness_weight = 0.;
};

struct SlabSolution
{
    RevisionKey           revision;
    InterfacePatch        interface;
    std::array<double, 2> volume_mm3{};
    double                max_gradient       = 0.;
    double                max_fraction_error = 0.;
    size_t                projection_sweeps  = 0;
    size_t                optimization_steps = 0;
};

using SlabResult = std::variant<std::shared_ptr<const SlabSolution>, Failure>;

// Deterministic projected-gradient/Dykstra solver; no added solver dependency.
// The conservative component bound |gx|, |gy| <= max_gradient/sqrt(2)
// guarantees the Euclidean triangle-gradient bound. It may reject a feasible
// nearly axis-aligned slope; it must never accept a slope over the norm limit.
SlabResult solve_slab(const SlabProblem&           problem,
                      const SolverLimits&          limits    = {},
                      const std::function<bool()>& cancelled = {},
                      const std::function<bool()>& stale     = {});

} // namespace Slic3r::MaterialBand
#endif
