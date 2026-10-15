#include "SeamVisibility.hpp"
#include <algorithm>
#include <cmath>

namespace Slic3r {
struct VulkanSeamVisibility::Impl
{
    std::unique_ptr<VulkanBatch> batch;
};
static_assert(sizeof(SeamVisibilityNode) == 32 && sizeof(SeamVisibilityQuery) == 16, "Shader buffer layout mismatch");
VulkanSeamVisibility::VulkanSeamVisibility(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
VulkanSeamVisibility::~VulkanSeamVisibility() = default;
std::unique_ptr<VulkanSeamVisibility> VulkanSeamVisibility::create(const std::vector<SeamVisibilityNode>& nodes,
                                                                   float                                  radius,
                                                                   const std::string&                     filter,
                                                                   std::string&                           error)
{
    if (nodes.size() > (1u << 24)) {
        error = "Oversized visibility tree";
        return nullptr;
    }
    auto batch = VulkanBatch::create(VulkanBatch::Kernel::SeamVisibility, nodes.data(), nodes.size() * sizeof(SeamVisibilityNode),
                                     uint32_t(nodes.size()), radius, filter, error);
    if (!batch)
        return nullptr;
    auto impl   = std::make_unique<Impl>();
    impl->batch = std::move(batch);
    return std::unique_ptr<VulkanSeamVisibility>(new VulkanSeamVisibility(std::move(impl)));
}
bool VulkanSeamVisibility::compute(const std::vector<SeamVisibilityQuery>& queries, std::vector<float>& result, std::string& error)
{
    if (queries.size() > BATCH_CAPACITY) {
        result.clear();
        error = "Seam query batch exceeds bounded GPU capacity";
        return false;
    }
    result.resize(queries.size());
    if (!m_impl->batch->compute(queries.data(), uint32_t(queries.size()), result.data(), error)) {
        result.clear();
        return false;
    }
    if (std::any_of(result.begin(), result.end(), [](float v) { return !std::isfinite(v); })) {
        result.clear();
        error = "Non-finite Vulkan seam visibility result";
        return false;
    }
    return true;
}
const std::string&          VulkanSeamVisibility::device_name() const { return m_impl->batch->device_name(); }
const SeamVisibilityTiming& VulkanSeamVisibility::timing() const { return m_impl->batch->timing(); }
} // namespace Slic3r
