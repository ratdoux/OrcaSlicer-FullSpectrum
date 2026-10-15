#ifndef slic3r_Compute_SeamVisibility_hpp_
#define slic3r_Compute_SeamVisibility_hpp_

#include "VulkanBatch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

// Explicit std430 layout. Nodes use the CPU tree's binary-heap order. A zero
// normal[3] marks an empty node; position[3] holds the sample's visibility.
struct SeamVisibilityNode
{
    std::array<float, 4> position;
    std::array<float, 4> normal;
};
using SeamVisibilityQuery = std::array<float, 4>;

using SeamVisibilityTiming = VulkanBatchTiming;

// Optional experimental backend. No Vulkan dependency in this interface.
// Each instance owns its device, queue and bounded scratch buffers. Calls on
// one instance must be serialized. An unavailable backend returns a reason.
class VulkanSeamVisibility
{
public:
    static constexpr uint32_t                    BATCH_CAPACITY = 16384;
    static std::unique_ptr<VulkanSeamVisibility> create(const std::vector<SeamVisibilityNode>& nodes,
                                                        float                                  radius,
                                                        const std::string&                     device_filter,
                                                        std::string&                           error);
    ~VulkanSeamVisibility();
    bool                        compute(const std::vector<SeamVisibilityQuery>& queries, std::vector<float>& result, std::string& error);
    const std::string&          device_name() const;
    const SeamVisibilityTiming& timing() const;

private:
    struct Impl;
    explicit VulkanSeamVisibility(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r
#endif
