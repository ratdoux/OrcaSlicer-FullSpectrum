#ifndef slic3r_Compute_SeamNeighbors_hpp_
#define slic3r_Compute_SeamNeighbors_hpp_
#include "VulkanBatch.hpp"
#include <array>
#include <vector>
namespace Slic3r {
struct SeamNeighborNode
{
    std::array<float, 4>    position{};
    std::array<uint32_t, 4> meta{{UINT32_MAX, 0, 0, 0}};
};
using SeamNeighborQuery = std::array<float, 4>; // XYZ, actual radius
struct SeamNeighborResult
{
    uint32_t                  count = 0, fallback = 0;
    std::array<uint32_t, 256> indices{};
};
class VulkanSeamNeighbors
{
public:
    static bool                                 supports_query(const SeamNeighborQuery& query);
    static std::unique_ptr<VulkanSeamNeighbors> create(size_t capacity, const std::string& device, std::string& error);
    bool                                        update(const std::vector<SeamNeighborNode>& nodes, std::string& error);
    bool compute(const std::vector<SeamNeighborQuery>& queries, std::vector<SeamNeighborResult>& results, std::string& error);
    const VulkanBatchTiming& timing() const { return m_batch->timing(); }
    const std::string&       device_name() const { return m_batch->device_name(); }

private:
    std::unique_ptr<VulkanBatch> m_batch;
};
} // namespace Slic3r
#endif
