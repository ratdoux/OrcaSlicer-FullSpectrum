#ifndef slic3r_Compute_LocalZClipDecision_hpp_
#define slic3r_Compute_LocalZClipDecision_hpp_

#include "VulkanBatch.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace Slic3r {

// Bounded exact integer predicates. Coordinates outside +/- 2^29 are not
// eligible: this bound keeps differences and cross products within int64.
// KeepWhole proves geometric containment only, not Clipper's output order or
// normalization. Exact clipping callers must not use it to bypass clipping;
// the Local-Z path router may use it under its geometric-ownership contract.
enum class ClipDecision : uint32_t { Unknown = 0, Empty = 1, KeepWhole = 2 };
using ClipMaskEdge = std::array<int32_t, 4>;
struct ClipSegmentQuery
{
    std::array<int32_t, 4> endpoints;
    uint32_t               edge_start = 0, edge_count = 0, difference = 0, classify_keep = 0;
};
// classify_keep=0 preserves the original binary protocol for all existing callers.
uint32_t local_z_segment_decision(const std::vector<ClipMaskEdge>& edges, const ClipSegmentQuery& query);
uint32_t local_z_segment_proven_empty(const std::vector<ClipMaskEdge>& edges, const ClipSegmentQuery& query);

// Immutable, owned per-mask BVH. Queries inspect nearby edges for boundary
// ambiguity and edges intersecting the positive-X winding ray. No float math.
class LocalZClipIndex
{
public:
    LocalZClipIndex(const std::vector<ClipMaskEdge>& edges, uint32_t start, uint32_t count);
    uint32_t classify(const ClipSegmentQuery& query) const;
    size_t   memory_bytes() const;

private:
    struct Node
    {
        std::array<int32_t, 4> bounds;
        uint32_t               first, end, right = 0;
    };
    uint32_t                  build(uint32_t first, uint32_t end);
    bool                      visit(uint32_t node, const ClipSegmentQuery& query, int& winding) const;
    std::vector<ClipMaskEdge> m_edges;
    std::vector<Node>         m_nodes;
    uint32_t                  m_start, m_count;
    bool                      m_valid = false;
};
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
