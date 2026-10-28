#ifndef slic3r_MaterialBand_SolidClipper_hpp_
#define slic3r_MaterialBand_SolidClipper_hpp_

#include "SlabSolver.hpp"

namespace Slic3r::MaterialBand {

struct ClippingLimits
{
    size_t max_input_faces           = 20000;
    size_t max_generated_elements    = 400000;
    size_t max_output_faces          = 100000;
    size_t max_correction_iterations = 24;
    size_t max_temporary_bytes       = 256 * 1024 * 1024;
};

struct ClippedBand
{
    BandId      id;
    InterfaceId shared_interface = 0;
    Mesh        solid;
    double      volume_mm3 = 0.;
};

struct MaterialBandComplex
{
    RevisionKey revision;
    // One clipped interface, owned once and referenced by both complements.
    InterfacePatch             shared_interface;
    std::array<ClippedBand, 2> bands;
    double                     domain_volume_mm3       = 0.;
    double                     interface_correction_mm = 0.;
    size_t                     correction_iterations   = 0;
    size_t                     generated_elements      = 0;
};

using ClippingResult = std::variant<std::shared_ptr<const MaterialBandComplex>, Failure>;

struct SolidCoverage
{
    double candidate_mm3 = 0.;
    double missing_mm3   = 0.;
    double excess_mm3    = 0.;
};
using SolidCoverageResult = std::variant<SolidCoverage, Failure>;

// Measures the complete candidate solid against the actual target, including
// cavities and intermediate geometry, rather than sampling path endpoints.
SolidCoverageResult measure_coverage(const Mesh&                  candidate,
                                     const Mesh&                  target,
                                     const RevisionKey&           revision,
                                     const ClippingLimits&        limits    = {},
                                     const std::function<bool()>& cancelled = {});

// Intersect the actual closed solid with the requested macro slab and shared
// height field. The field triangulation must contain the entire XY projection
// of the desired domain; an uncovered part is an error, never silently dropped.
// The supplied target is an independently integrated CLIPPED volume, not H*f.
// Optional uniform interface correction is bounded by every nodal recipe and
// thickness constraint. It changes neither slopes nor phase. Source mesh error
// remains separate from Boolean conversion and solver tolerances.
ClippingResult clip_bands(const Mesh&                  solid,
                          const SlabProblem&           problem,
                          const SlabSolution&          solution,
                          const StackPhase&            phase,
                          double                       target_volume_a_mm3,
                          const ClippingLimits&        limits    = {},
                          const std::function<bool()>& cancelled = {},
                          const std::function<bool()>& stale     = {},
                          std::optional<double> uniform_domain_fraction = {});

using SlabClipResult = std::variant<std::shared_ptr<const ClippedBand>, Failure>;
// Actual-solid macro-slab clipping for a genuine pure material domain.
SlabClipResult clip_pure_slab(const Mesh& solid, BandId id, const RevisionKey& revision, double bottom_mm, double top_mm,
                             const NumericTolerances& tolerances = {}, const ClippingLimits& limits = {},
                             const std::function<bool()>& cancelled = {}, const std::function<bool()>& stale = {});

// Immutable accelerated membership/distance queries on an accepted closed mesh.
// Construction validates closure, orientation and self-intersections; callers
// must not use a nearest-surface result as a substitute for inside/outside.
class SolidQuery
{
public:
    explicit SolidQuery(const Mesh& solid);
    bool   contains(const Vec3d& point) const;
    double distance_mm(const Vec3d& point) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> m_impl;
};

} // namespace Slic3r::MaterialBand
#endif
