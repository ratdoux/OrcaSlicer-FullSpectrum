#ifndef slic3r_MaterialBand_ClippedPlanner_hpp_
#define slic3r_MaterialBand_ClippedPlanner_hpp_

#include "BoxPlanner.hpp"
#include "SolidClipper.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r::MaterialBand {

struct CoverageLimits
{
    double             relative_missing_volume = .01;
    double             minimum_bead_width_mm   = .04;
    size_t             max_candidate_cells     = 100000;
    size_t             max_refinement_depth    = 8;
    size_t             perimeter_loops         = 1;
    DenseInfillOptions infill;
};

struct PlanarMaterialBand
{
    BandId      id;
    ExPolygons  footprint;
    double      bottom_mm = 0., top_mm = 0.;
    InterfaceId lower_interface = 0, upper_interface = 0;
};

struct ContourRun
{
    Polyline line;
    double   width_mm = .45;
    PathRole role     = PathRole::DenseFill;
    // Optional owned fill region. Generator overlap may reach into walls, but
    // must not create floating solid skin in the neighboring sparse void.
    std::shared_ptr<const ExPolygons> coverage;
};

// Rotate closed wall contours onto actual preceding material before assigning
// bead ownership. This moves only the seam, preserves the contour, and leaves
// complete support/span validation to the scheduler.
size_t anchor_perimeter_seams(std::vector<ContourRun>& runs, const ExPolygons& preceding_deposition, const std::function<void()>& poll = {});

// Fit only sub-bead cap protrusions to the preceding layer's allowed overhang.
// Removed dose is charged to coverage; larger unsupported geometry is retained
// for the scheduler's bridge/support validation, never silently discarded.
BoxPlanResult fit_planar_overhang_caps(const PathPlan&              geometry,
                                       const ExPolygons&            preceding_deposition,
                                       double                       contact_z,
                                       const CoverageLimits&        coverage  = {},
                                       const PathLimits&            limits    = {},
                                       const std::function<bool()>& cancelled = {},
                                       const std::function<bool()>& stale     = {});

// Convert normal wall/fill centerlines into owned, nonoverlapping deposition
// cells, clipped to the layer outline. Missing sparse area stays intentional air.
BoxPlanResult plan_contour_cycle(const ExPolygons&              outline,
                                 const std::vector<ContourRun>& runs,
                                 const BandId&                  id,
                                 double                         bottom,
                                 double                         top,
                                 const RevisionKey&             revision,
                                 const ProcessConstraints&      process,
                                 const NumericTolerances&       tolerances,
                                 const MachineEnvelope&         machine,
                                 const PathLimits&              limits         = {},
                                 const std::function<bool()>&   cancelled      = {},
                                 const std::function<bool()>&   stale          = {},
                                 const ExPolygons&              required_solid = {});

// Partition an already qualified macro-cycle's walls and interior by owned
// painted domains. Walls follow the object's contour, not internal paint edges.
// An optional common first-material foundation keeps bridge runs continuous;
// its coverage is apportioned back to each original region for recipe checks.
// All remaining physical passes still satisfy the original thickness bounds.
BoxPlanResult plan_partitioned_cycle(const PathPlan&                        geometry,
                                     const std::vector<PlanarMaterialBand>& bands,
                                     const ProcessConstraints&              process,
                                     const NumericTolerances&               tolerances,
                                     const PathLimits&                      limits               = {},
                                     const CoverageLimits&                  coverage             = {},
                                     const std::function<bool()>&           cancelled            = {},
                                     const std::function<bool()>&           stale                = {},
                                     const ExPolygons&                      preceding_deposition = {},
                                     double                                 contact_z            = 0.,
                                     double                                 common_foundation_mm = 0.);

// A single macro cycle, with a globally affine shared interface. Perimeters
// follow the actual clipped upper contour (including holes), with bounded-width
// prismatic coverage cells for dense fill. This is a candidate PathPlan; combined
// scheduling/occupancy validation remains mandatory before production export.
BoxPlanResult plan_clipped_cycle(const MaterialBandComplex&   bands,
                                 const ProcessConstraints&    process,
                                 const NumericTolerances&     tolerances,
                                 const MachineEnvelope&       machine,
                                 const PathLimits&            limits    = {},
                                 const CoverageLimits&        coverage  = {},
                                 const std::function<bool()>& cancelled = {},
                                 const std::function<bool()>& stale     = {});

// A genuine single-material domain: no internal interface and no absent-material
// residue. The caller supplies the actual macro-slab solid and the material's
// existing phase/slot identity. It is never constructed as a near-zero mixture.
BoxPlanResult plan_pure_cycle(const ClippedBand&           band,
                              const RevisionKey&           revision,
                              const ProcessConstraints&    process,
                              const NumericTolerances&     tolerances,
                              const MachineEnvelope&       machine,
                              const PathLimits&            limits    = {},
                              const CoverageLimits&        coverage  = {},
                              const std::function<bool()>& cancelled = {},
                              const std::function<bool()>& stale     = {});

} // namespace Slic3r::MaterialBand
#endif
