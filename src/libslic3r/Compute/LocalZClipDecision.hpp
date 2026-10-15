#ifndef slic3r_Compute_LocalZClipDecision_hpp_
#define slic3r_Compute_LocalZClipDecision_hpp_

#include "VulkanBatch.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace Slic3r {

// Bounded exact integer predicates. Coordinates outside +/- 2^29 are not
// eligible: this bound keeps differences and cross products within int64.
// A decision only proves an empty result; all other work stays with Clipper.
using ClipMaskEdge = std::array<int32_t, 4>;
struct ClipSegmentQuery
{
    std::array<int32_t, 4> endpoints;
    uint32_t               edge_start = 0, edge_count = 0, difference = 0, reserved = 0;
};
uint32_t local_z_segment_proven_empty(const std::vector<ClipMaskEdge>& edges, const ClipSegmentQuery& query);
// Complete closed rings, subject first and then clip. All decisions must be 1.
std::vector<ClipSegmentQuery> closed_polygon_empty_queries(const std::vector<ClipMaskEdge>& edges, size_t subject_edges, bool difference);

// Experimental conservative classifier, not a replacement for polygon clipping.
class VulkanLocalZClipDecision
{
public:
    static std::unique_ptr<VulkanLocalZClipDecision> create(const std::vector<ClipMaskEdge>& edges,
                                                            const std::string&               filter,
                                                            std::string&                     error);
    bool                     compute(const std::vector<ClipSegmentQuery>& queries, std::vector<uint32_t>& decisions, std::string& error);
    bool                     update_edges(const std::vector<ClipMaskEdge>& edges, std::string& error);
    const std::string&       device_name() const { return m_batch->device_name(); }
    const VulkanBatchTiming& timing() const { return m_batch->timing(); }

private:
    std::unique_ptr<VulkanBatch> m_batch;
};

} // namespace Slic3r
#endif
