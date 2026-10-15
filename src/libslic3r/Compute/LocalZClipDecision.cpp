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

uint32_t local_z_segment_proven_empty(const std::vector<ClipMaskEdge>& edges, const ClipSegmentQuery& query)
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
    return query.difference ? winding != 0 : winding == 0;
}

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
    if (std::any_of(decisions.begin(), decisions.end(), [](uint32_t value) { return value > 1; })) {
        decisions.clear();
        error = "Invalid Local-Z GPU decision";
        return false;
    }
    return true;
}

} // namespace Slic3r
