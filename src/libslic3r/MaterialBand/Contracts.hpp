#ifndef slic3r_MaterialBand_Contracts_hpp_
#define slic3r_MaterialBand_Contracts_hpp_

#include "libslic3r/Point.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Slic3r::MaterialBand {

constexpr size_t MAX_MATERIALS = 4;
using MaterialFractions        = std::array<double, MAX_MATERIALS>;

struct GradientKnot
{
    double            position = 0.;
    MaterialFractions fractions{}; // physical IDs minus one
};

// All coordinates and thicknesses at this boundary are mm; volumes are mm^3.
// Physical IDs are 1-based, matching the production recipe model. A virtual
// recipe ID is provenance and must never be used as a physical material ID.
struct PhysicalMaterialId
{
    uint32_t value = 0;
};

using RegionId    = uint64_t;
using DomainId    = uint64_t;
using InterfaceId = uint64_t;
using RecipeId    = uint64_t;

struct RevisionKey
{
    uint64_t object   = 0;
    uint64_t geometry = 0;
    uint64_t paint    = 0;
    uint64_t recipe   = 0;
    uint64_t process  = 0;
    uint64_t machine  = 0;
    // Distinguishes retries, cancellation and edit/revert publication attempts.
    uint64_t generation = 0;

    bool operator==(const RevisionKey& other) const;
    bool operator!=(const RevisionKey& other) const { return !(*this == other); }
};

enum class BoundaryPolicy { PreservePaintedEdge, AllowTransition };
enum class DepositionScope { PaintedShell, Infill, WholeSolid };

struct NumericTolerances
{
    double interface_mm          = 1e-5;
    double macro_closure_mm      = 1e-3;
    double path_approximation_mm = 1e-2;
    double plateau_fraction      = .005;
    double relative_volume       = 1e-4;
    double absolute_volume_mm3   = 1e-4;
};

struct BoundaryRule
{
    BoundaryPolicy policy              = BoundaryPolicy::PreservePaintedEdge;
    double         transition_width_mm = 0.;
    // Separate from plateau and integrated-volume budgets. Zero permits no blur.
    double transition_fraction_error = 0.;
};

struct ProcessConstraints
{
    double macro_height_mm         = .24;
    double bead_width_mm           = .45;
    double min_active_thickness_mm = .04;
    double max_active_thickness_mm = .24;
    // Bound on the norm of the actual triangle gradient, not just edge slopes.
    double max_interface_gradient = .10;
};

// The adapter must preserve the connected domain's actual phase origin. Zero
// here is a value, not an instruction to restart at the object's first layer.
struct StackPhase
{
    std::array<PhysicalMaterialId, 2> order;
    std::array<uint32_t, 2>           cadence{{1, 1}};
    uint64_t                          first_active_cycle = 0;
    uint32_t                          slot               = 0;
};

struct Mesh
{
    std::vector<Vec3d>                   vertices;
    std::vector<std::array<uint32_t, 3>> triangles;
};

struct RegionIntent
{
    RegionId          id     = 0;
    DomainId          domain = 0;
    RecipeId          recipe = 0;
    DepositionScope   scope  = DepositionScope::PaintedShell;
    StackPhase        phase;
    double            fraction_a = .5;
    MaterialFractions fractions{};
    // Owned normalized height curve decoded from the existing gradient controls.
    // An empty curve denotes the constant blend above.
    std::vector<GradientKnot> height_gradient;
    // An owned snapshot of painted leaf facets, including triangle splits.
    // This is surface provenance, NOT a license to fill its XY projection.
    Mesh painted_surface;
};

struct KernelInput
{
    RevisionKey               revision;
    std::string               source_revision;
    Mesh                      solid;
    std::vector<RegionIntent> regions;
    ProcessConstraints        process;
    NumericTolerances         tolerances;
    BoundaryRule              boundary;
    size_t                    material_count             = 2;
    double                    gradient_fraction_error    = 0.;
    size_t                    discarded_degenerate_faces = 0;
};

struct BandId
{
    DomainId           domain      = 0;
    uint64_t           macro_cycle = 0;
    PhysicalMaterialId material;
    uint32_t           stack_slot = 0;
};

struct InterfacePatch
{
    InterfaceId id          = 0;
    DomainId    domain      = 0;
    uint64_t    macro_cycle = 0;
    Mesh        surface;
};

// Adjacent bands refer to the SAME patch ID, not separately fitted copies.
// No band/path/operation framework is introduced before its milestone.
struct InterfaceComplex
{
    RevisionKey                 revision;
    std::vector<InterfacePatch> patches;
};

enum class FailureCode {
    UnsupportedTopology,
    InfeasibleThickness,
    IncompatibleOrder,
    MaterialToleranceExceeded,
    ClearanceFailure,
    UnsupportedProfile,
    NonConverged,
    ResourceLimit,
    InvalidInput,
    Cancelled,
    StaleInput
};

struct Failure
{
    RevisionKey             revision;
    FailureCode             code = FailureCode::InvalidInput;
    std::string             detail;
    std::optional<BandId>   band;
    std::optional<RegionId> region;
    std::optional<Vec3d>    witness_mm;
};

// A failed stage contains no partially accepted output. A successful interface
// solve is NOT an accepted print plan. Object-level fallback is decided only
// after all required stages have validated the same revision.
using InterfaceResult = std::variant<std::shared_ptr<const InterfaceComplex>, Failure>;

const char* failure_name(FailureCode code);

} // namespace Slic3r::MaterialBand

#endif
