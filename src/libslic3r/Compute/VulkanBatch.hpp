#ifndef slic3r_Compute_VulkanBatch_hpp_
#define slic3r_Compute_VulkanBatch_hpp_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Slic3r {
struct VulkanBatchTiming
{
    double setup_ms             = 0;
    double upload_ms            = 0;
    double dispatch_readback_ms = 0;
    size_t queries              = 0;
};

// Internal shared backend for bounded compute experiments: immutable data,
// query batches, and fixed-stride results per kernel. Calls must be serialized.
class VulkanBatch
{
public:
    enum class Kernel { SeamVisibility, LocalZClipDecision, SeamNeighbors };
    static constexpr uint32_t           BATCH_CAPACITY = 16384;
    static std::unique_ptr<VulkanBatch> create(Kernel             kernel,
                                               const void*        data,
                                               size_t             bytes,
                                               uint32_t           count,
                                               float              parameter,
                                               const std::string& device_filter,
                                               std::string&       error);
    ~VulkanBatch();
    // Build capability only; does not load a driver or probe devices.
    static bool compiled();
    // Call after application jobs have joined, before unloading the host DLL.
    // Avoid Vulkan/driver teardown from Windows DLL_PROCESS_DETACH.
    static void              shutdown();
    bool                     compute(const void* queries, uint32_t count, void* results, std::string& error);
    bool                     update_data(const void* data, size_t bytes, uint32_t count, std::string& error);
    const std::string&       device_name() const;
    const VulkanBatchTiming& timing() const;

private:
    struct Impl;
    explicit VulkanBatch(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace Slic3r
#endif
