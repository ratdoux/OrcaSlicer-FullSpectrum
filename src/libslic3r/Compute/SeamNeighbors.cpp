#include "SeamNeighbors.hpp"
#include <algorithm>
#include <cmath>
namespace Slic3r {
static_assert(sizeof(SeamNeighborNode) == 32 && sizeof(SeamNeighborQuery) == 16 && sizeof(SeamNeighborResult) == 1032, "Shader layout");
bool VulkanSeamNeighbors::supports_query(const SeamNeighborQuery& q)
{
    return q[3] > 0 && std::isfinite(q[3] * q[3]) && std::all_of(q.begin(), q.end(), [](float v) { return std::isfinite(v); });
}
std::unique_ptr<VulkanSeamNeighbors> VulkanSeamNeighbors::create(size_t capacity, const std::string& device, std::string& error)
{
    if (!capacity || capacity > (1u << 20)) {
        error = "Invalid seam neighborhood capacity";
        return nullptr;
    }
    std::vector<SeamNeighborNode> nodes(capacity);
    auto batch = VulkanBatch::create(VulkanBatch::Kernel::SeamNeighbors, nodes.data(), nodes.size() * sizeof(nodes[0]),
                                     uint32_t(nodes.size()), 0, device, error);
    if (!batch)
        return nullptr;
    auto result     = std::make_unique<VulkanSeamNeighbors>();
    result->m_batch = std::move(batch);
    return result;
}
bool VulkanSeamNeighbors::update(const std::vector<SeamNeighborNode>& nodes, std::string& error)
{
    return m_batch->update_data(nodes.data(), nodes.size() * sizeof(nodes[0]), uint32_t(nodes.size()), error);
}
bool VulkanSeamNeighbors::compute(const std::vector<SeamNeighborQuery>& queries,
                                  std::vector<SeamNeighborResult>&      results,
                                  std::string&                          error)
{
    if (queries.size() > VulkanBatch::BATCH_CAPACITY) {
        error = "Oversized seam neighbor batch";
        results.clear();
        return false;
    }
    for (const auto& q : queries)
        if (!supports_query(q)) {
            error = "Invalid seam neighbor query";
            results.clear();
            return false;
        }
    results.resize(queries.size());
    if (!m_batch->compute(queries.data(), uint32_t(queries.size()), results.data(), error)) {
        results.clear();
        return false;
    }
    for (const auto& r : results)
        if (r.count > 256 || r.fallback > 3) {
            error = "Invalid seam neighbor result";
            results.clear();
            return false;
        }
    return true;
}
} // namespace Slic3r
