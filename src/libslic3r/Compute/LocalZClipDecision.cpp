#include "LocalZClipDecision.hpp"

#include <algorithm>
#include <cstdlib>

namespace Slic3r {
namespace {
bool eligible(const std::array<int32_t, 4>& p)
{
    return std::all_of(p.begin(), p.end(), [](int32_t v) { return v >= -(1 << 29) && v <= (1 << 29); });
}
int64_t orient(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py)
{
    return (int64_t(bx) - ax) * (int64_t(py) - ay) - (int64_t(by) - ay) * (int64_t(px) - ax);
}
bool same_side(int64_t a, int64_t b) { return (a > 0 && b > 0) || (a < 0 && b < 0); }
} // namespace

uint32_t local_z_segment_decision(const std::vector<ClipMaskEdge>& edges, const ClipSegmentQuery& query)
{
    const auto& p = query.endpoints;
    if (!eligible(p) || query.edge_count > (1u << 24) || query.edge_start > edges.size() ||
        query.edge_count > edges.size() - query.edge_start || (p[0] == p[2] && p[1] == p[3]))
        return 0;
    int winding = 0;
    for (size_t i = query.edge_start; i < size_t(query.edge_start) + query.edge_count; ++i) {
        const auto& e = edges[i];
        if (!eligible(e))
            return 0;
        const int64_t a = orient(e[0], e[1], e[2], e[3], p[0], p[1]);
        const int64_t b = orient(e[0], e[1], e[2], e[3], p[2], p[3]);
        if (std::max(p[0], p[2]) + 2 >= std::min(e[0], e[2]) && std::min(p[0], p[2]) - 2 <= std::max(e[0], e[2]) &&
            std::max(p[1], p[3]) + 2 >= std::min(e[1], e[3]) && std::min(p[1], p[3]) - 2 <= std::max(e[1], e[3])) {
            const int64_t c = orient(p[0], p[1], p[2], p[3], e[0], e[1]);
            const int64_t d = orient(p[0], p[1], p[2], p[3], e[2], e[3]);
            // Boundary crossings/touches and a two-coordinate rounding guard
            // always go to Clipper. No reconstructed intersection coordinates.
            const int64_t margin         = 2 * (std::abs(int64_t(e[2]) - e[0]) + std::abs(int64_t(e[3]) - e[1]));
            const int64_t subject_margin = 2 * (std::abs(int64_t(p[2]) - p[0]) + std::abs(int64_t(p[3]) - p[1]));
            if ((!same_side(a, b) && !same_side(c, d)) || std::abs(a) <= margin || std::abs(b) <= margin || std::abs(c) <= subject_margin ||
                std::abs(d) <= subject_margin)
                return 0;
        }
        if (e[1] <= p[1] && e[3] > p[1] && a > 0)
            ++winding;
        else if (e[1] > p[1] && e[3] <= p[1] && a < 0)
            --winding;
    }
    const bool empty = query.difference ? winding != 0 : winding == 0;
    return empty ? uint32_t(ClipDecision::Empty) : (query.classify_keep ? uint32_t(ClipDecision::KeepWhole) : 0);
}

uint32_t local_z_segment_proven_empty(const std::vector<ClipMaskEdge>& edges, const ClipSegmentQuery& query)
{
    return local_z_segment_decision(edges, query) == uint32_t(ClipDecision::Empty);
}

LocalZClipIndex::LocalZClipIndex(const std::vector<ClipMaskEdge>& edges, uint32_t start, uint32_t count) : m_start(start), m_count(count)
{
    if (count > (1u << 24) || start > edges.size() || count > edges.size() - start)
        return;
    // An ineligible edge anywhere in the range invalidates the original oracle,
    // even when its bounding box could otherwise be pruned.
    if (!std::all_of(edges.begin() + start, edges.begin() + start + count, eligible))
        return;
    m_edges.assign(edges.begin() + start, edges.begin() + start + count);
    m_nodes.reserve(count / 2 + 1);
    if (count)
        build(0, count);
    m_valid = true;
}

uint32_t LocalZClipIndex::build(uint32_t first, uint32_t end)
{
    const uint32_t index  = uint32_t(m_nodes.size());
    auto           bounds = m_edges[first];
    bounds                = {std::min(bounds[0], bounds[2]), std::min(bounds[1], bounds[3]), std::max(bounds[0], bounds[2]),
                             std::max(bounds[1], bounds[3])};
    for (uint32_t i = first + 1; i < end; ++i) {
        const auto& e = m_edges[i];
        bounds[0]     = std::min(bounds[0], std::min(e[0], e[2]));
        bounds[1]     = std::min(bounds[1], std::min(e[1], e[3]));
        bounds[2]     = std::max(bounds[2], std::max(e[0], e[2]));
        bounds[3]     = std::max(bounds[3], std::max(e[1], e[3]));
    }
    m_nodes.push_back({bounds, first, end, 0});
    if (end - first > 8) {
        const size_t   axis = bounds[2] - bounds[0] >= bounds[3] - bounds[1] ? 0 : 1;
        const uint32_t mid  = first + (end - first) / 2;
        std::nth_element(m_edges.begin() + first, m_edges.begin() + mid, m_edges.begin() + end,
                         [axis](const ClipMaskEdge& a, const ClipMaskEdge& b) {
                             return int64_t(a[axis]) + a[axis + 2] < int64_t(b[axis]) + b[axis + 2];
                         });
        build(first, mid);
        const uint32_t right = build(mid, end);
        m_nodes[index].right = right;
    }
    return index;
}

bool LocalZClipIndex::visit(uint32_t index, const ClipSegmentQuery& query, int& winding) const
{
    const auto& node     = m_nodes[index];
    const auto& p        = query.endpoints;
    const auto& box      = node.bounds;
    const bool  boundary = std::max(p[0], p[2]) + 2 >= box[0] && std::min(p[0], p[2]) - 2 <= box[2] && std::max(p[1], p[3]) + 2 >= box[1] &&
                          std::min(p[1], p[3]) - 2 <= box[3];
    const bool ray = p[1] >= box[1] && p[1] < box[3] && p[0] <= box[2];
    if (!boundary && !ray)
        return true;
    if (node.right)
        return visit(index + 1, query, winding) && visit(node.right, query, winding);
    for (uint32_t i = node.first; i < node.end; ++i) {
        const auto&   e = m_edges[i];
        const int64_t a = orient(e[0], e[1], e[2], e[3], p[0], p[1]);
        if (boundary && std::max(p[0], p[2]) + 2 >= std::min(e[0], e[2]) && std::min(p[0], p[2]) - 2 <= std::max(e[0], e[2]) &&
            std::max(p[1], p[3]) + 2 >= std::min(e[1], e[3]) && std::min(p[1], p[3]) - 2 <= std::max(e[1], e[3])) {
            const int64_t b              = orient(e[0], e[1], e[2], e[3], p[2], p[3]);
            const int64_t c              = orient(p[0], p[1], p[2], p[3], e[0], e[1]);
            const int64_t d              = orient(p[0], p[1], p[2], p[3], e[2], e[3]);
            const int64_t margin         = 2 * (std::abs(int64_t(e[2]) - e[0]) + std::abs(int64_t(e[3]) - e[1]));
            const int64_t subject_margin = 2 * (std::abs(int64_t(p[2]) - p[0]) + std::abs(int64_t(p[3]) - p[1]));
            if ((!same_side(a, b) && !same_side(c, d)) || std::abs(a) <= margin || std::abs(b) <= margin || std::abs(c) <= subject_margin ||
                std::abs(d) <= subject_margin)
                return false;
        }
        if (e[1] <= p[1] && e[3] > p[1] && a > 0)
            ++winding;
        else if (e[1] > p[1] && e[3] <= p[1] && a < 0)
            --winding;
    }
    return true;
}

uint32_t LocalZClipIndex::classify(const ClipSegmentQuery& query) const
{
    const auto& p = query.endpoints;
    if (!m_valid || query.edge_start != m_start || query.edge_count != m_count || !eligible(p) || (p[0] == p[2] && p[1] == p[3]))
        return 0;
    int winding = 0;
    if (!m_nodes.empty() && !visit(0, query, winding))
        return 0;
    const bool empty = query.difference ? winding != 0 : winding == 0;
    return empty ? uint32_t(ClipDecision::Empty) : (query.classify_keep ? uint32_t(ClipDecision::KeepWhole) : 0);
}

size_t LocalZClipIndex::memory_bytes() const { return m_edges.capacity() * sizeof(ClipMaskEdge) + m_nodes.capacity() * sizeof(Node); }

static_assert(sizeof(ClipMaskEdge) == 16 && sizeof(ClipSegmentQuery) == 32, "Shader buffer layout mismatch");

std::vector<ClipSegmentQuery> closed_polygon_empty_queries(const std::vector<ClipMaskEdge>& edges, size_t subject_edges, bool difference)
{
    if (!subject_edges || subject_edges >= edges.size() || edges.size() > (1u << 24))
        return {};
    std::vector<ClipSegmentQuery> queries;
    queries.reserve(edges.size());
    for (size_t i = 0; i < edges.size(); ++i) {
        const bool subject = i < subject_edges;
        queries.push_back({edges[i], uint32_t(subject ? subject_edges : 0),
                           uint32_t(subject ? edges.size() - subject_edges : subject_edges), uint32_t(subject && difference), 0});
    }
    return queries;
}

std::unique_ptr<VulkanLocalZClipDecision> VulkanLocalZClipDecision::create(const std::vector<ClipMaskEdge>& edges,
                                                                           const std::string&               filter,
                                                                           std::string&                     error)
{
    if (edges.size() > (1u << 24)) {
        error = "Oversized Local-Z edge buffer";
        return nullptr;
    }
    auto batch = VulkanBatch::create(VulkanBatch::Kernel::LocalZClipDecision, edges.data(), edges.size() * sizeof(ClipMaskEdge),
                                     uint32_t(edges.size()), 0, filter, error);
    if (!batch)
        return nullptr;
    auto result     = std::make_unique<VulkanLocalZClipDecision>();
    result->m_batch = std::move(batch);
    return result;
}
bool VulkanLocalZClipDecision::update_edges(const std::vector<ClipMaskEdge>& edges, std::string& error)
{
    if (edges.size() > (1u << 24)) {
        error = "Oversized Local-Z edge buffer";
        return false;
    }
    return m_batch->update_data(edges.data(), edges.size() * sizeof(ClipMaskEdge), uint32_t(edges.size()), error);
}

bool VulkanLocalZClipDecision::compute(const std::vector<ClipSegmentQuery>& queries, std::vector<uint32_t>& decisions, std::string& error)
{
    if (queries.size() > VulkanBatch::BATCH_CAPACITY) {
        decisions.clear();
        error = "Oversized Local-Z query batch";
        return false;
    }
    decisions.resize(queries.size());
    if (!m_batch->compute(queries.data(), uint32_t(queries.size()), decisions.data(), error)) {
        decisions.clear();
        return false;
    }
    for (size_t i = 0; i < decisions.size(); ++i) {
        if (decisions[i] <= (queries[i].classify_keep ? 2u : 1u))
            continue;
        decisions.clear();
        error = "Invalid Local-Z GPU decision";
        return false;
    }
    return true;
}

} // namespace Slic3r
