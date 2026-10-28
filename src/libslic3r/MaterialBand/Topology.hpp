#ifndef slic3r_MaterialBand_Topology_hpp_
#define slic3r_MaterialBand_Topology_hpp_

#include "Contracts.hpp"
#include <functional>

namespace Slic3r::MaterialBand {

struct DomainRef
{
    uint64_t object = 0;
    DomainId domain = 0;
    bool     operator==(const DomainRef& other) const { return object == other.object && domain == other.domain; }
    bool     operator<(const DomainRef& other) const { return object < other.object || (object == other.object && domain < other.domain); }
};

struct DomainSection
{
    DomainRef  id;
    StackPhase phase;
    // Physical IDs 1 and 2: bit 0 = material 1, bit 1 = material 2. A pure
    // domain has exactly one bit; absence is never encoded as a small dose.
    uint32_t active_materials = 3;
    Mesh     footprint; // Conforming XY triangulation; vertex Z is provenance only.
};

struct DomainFrame
{
    RevisionKey                revision;
    uint64_t                   macro_cycle = 0;
    std::vector<DomainSection> domains;
};

enum class TopologyEventKind { Birth, Termination, Split, Merge, MaterialBirth, MaterialTermination };

struct TopologyEvent
{
    TopologyEventKind                 kind;
    uint64_t                          macro_cycle = 0;
    std::vector<DomainRef>            parents, children;
    std::optional<PhysicalMaterialId> material;
};

struct DomainConnection
{
    DomainRef parent, child;
    double    overlap_mm2           = 0.;
    double    printable_overlap_mm2 = 0.;
};

struct TopologyTransition
{
    RevisionKey                   revision;
    uint64_t                      macro_cycle = 0;
    std::vector<DomainConnection> connections;
    std::vector<TopologyEvent>    events;
    size_t                        candidate_pairs = 0;
    size_t                        grid_references = 0;
};

struct TopologyLimits
{
    size_t max_domains         = 10000;
    size_t max_faces           = 100000;
    size_t max_candidates      = 100000;
    size_t max_grid_references = 500000;
    size_t max_temporary_bytes = 128 * 1024 * 1024;
};

using TopologyResult = std::variant<std::shared_ptr<const TopologyTransition>, Failure>;

// Links adjacent logical cycles by positive, bead-clear overlap. Inputs must
// already describe connected domains. Matching IDs or contour indices alone
// never establishes continuity; separate objects never link. Supplied phase
// origins are preserved, and incompatible merges fail atomically. These are
// topology/phase relations, not a substitute for 3D support and tool clearance.
TopologyResult link_domains(const DomainFrame&           before,
                            const DomainFrame&           after,
                            double                       bead_width_mm,
                            const TopologyLimits&        limits    = {},
                            const std::function<bool()>& cancelled = {},
                            const std::function<bool()>& stale     = {});

} // namespace Slic3r::MaterialBand
#endif
