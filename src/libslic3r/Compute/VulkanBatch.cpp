#include "VulkanBatch.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef ORCA_ENABLE_VULKAN_COMPUTE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "seam_visibility_spirv.hpp"
#include "local_z_clip_decision_spirv.hpp"
#include "seam_neighbors_spirv.hpp"
#endif

namespace Slic3r {

#ifdef ORCA_ENABLE_VULKAN_COMPUTE
namespace {
using Clock = std::chrono::steady_clock;
double elapsed_ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(result) + ")");
}

// clang-format off
#define INSTANCE_FUNCTIONS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceQueueFamilyProperties) X(EnumerateDeviceExtensionProperties) X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
    X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) \
    X(InvalidateMappedMemoryRanges) X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdCopyBuffer) X(CmdPipelineBarrier) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(QueueSubmit)
// clang-format on

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}

std::string lowercase(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = char(c + ('a' - 'A'));
    return text;
}

struct SharedInstance
{
    VkInstance            instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy  = nullptr;
    ~SharedInstance()
    {
        if (instance && destroy)
            destroy(instance, nullptr);
    }
};
struct InstanceStorage
{
    std::mutex                      mutex;
    std::shared_ptr<SharedInstance> owner;
};
InstanceStorage& instance_storage()
{
    static InstanceStorage storage;
    return storage;
}
std::shared_ptr<SharedInstance> shared_instance(PFN_vkGetInstanceProcAddr get_instance_proc)
{
    auto&                       storage = instance_storage();
    std::lock_guard<std::mutex> lock(storage.mutex);
    if (!storage.owner)
        storage.owner = [get_instance_proc] {
            auto        owner                = std::make_shared<SharedInstance>();
            VkInstance& instance             = owner->instance;
            auto        enumerate_extensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                get_instance_proc(nullptr, "vkEnumerateInstanceExtensionProperties"));
            auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc(nullptr, "vkCreateInstance"));
            if (!enumerate_extensions || !create_instance)
                throw std::runtime_error("Incomplete Vulkan loader");
            uint32_t count = 0;
            check(enumerate_extensions(nullptr, &count, nullptr), "vkEnumerateInstanceExtensionProperties");
            std::vector<VkExtensionProperties> extensions(count);
            check(enumerate_extensions(nullptr, &count, extensions.data()), "vkEnumerateInstanceExtensionProperties");
            const char*       portability = "VK_KHR_portability_enumeration";
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.pApplicationName = "Orca bounded compute experiments";
            app.apiVersion       = VK_API_VERSION_1_0;
            VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            instance_info.pApplicationInfo = &app;
            if (has_extension(extensions, portability)) {
                instance_info.flags                   = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
                instance_info.enabledExtensionCount   = 1;
                instance_info.ppEnabledExtensionNames = &portability;
            }
            check(create_instance(&instance_info, nullptr, &instance), "vkCreateInstance");

            owner->destroy = reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc(instance, "vkDestroyInstance"));
            if (!owner->destroy)
                throw std::runtime_error("Missing vkDestroyInstance");
            return owner;
        }();
    return storage.owner;
}
} // namespace

struct VulkanBatch::Impl
{
    struct Buffer
    {
        VkBuffer       buffer   = VK_NULL_HANDLE;
        VkDeviceMemory memory   = VK_NULL_HANDLE;
        void*          mapped   = nullptr;
        bool           coherent = false;
    } nodes_buffer, queries_buffer, results_buffer, staging;

    void*                     library           = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
#define DECLARE_FUNCTION(name) PFN_vk##name name = nullptr;
    INSTANCE_FUNCTIONS(DECLARE_FUNCTION)
    DEVICE_FUNCTIONS(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION
    std::shared_ptr<SharedInstance>  instance_owner;
    VkInstance                       instance          = VK_NULL_HANDLE;
    VkPhysicalDevice                 physical          = VK_NULL_HANDLE;
    VkDevice                         device            = VK_NULL_HANDLE;
    VkQueue                          queue             = VK_NULL_HANDLE;
    VkDescriptorSetLayout            descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool                 descriptor_pool   = VK_NULL_HANDLE;
    VkDescriptorSet                  descriptor_set    = VK_NULL_HANDLE;
    VkPipelineLayout                 pipeline_layout   = VK_NULL_HANDLE;
    VkShaderModule                   shader            = VK_NULL_HANDLE;
    VkPipeline                       pipeline          = VK_NULL_HANDLE;
    VkCommandPool                    command_pool      = VK_NULL_HANDLE;
    VkCommandBuffer                  command           = VK_NULL_HANDLE;
    VkFence                          fence             = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    std::string                      name;
    VulkanBatchTiming                timing;
    uint32_t                         node_count          = 0;
    float                            radius              = 0;
    VkDeviceSize                     READBACK_OFFSET     = 0;
    uint32_t                         query_stride        = 0;
    uint32_t                         result_stride       = 4;
    size_t                           node_capacity_bytes = 0;
    Kernel                           kernel              = Kernel::SeamVisibility;

    ~Impl()
    {
        if (device && DeviceWaitIdle)
            DeviceWaitIdle(device);
        if (fence)
            DestroyFence(device, fence, nullptr);
        if (command_pool)
            DestroyCommandPool(device, command_pool, nullptr);
        if (pipeline)
            DestroyPipeline(device, pipeline, nullptr);
        if (shader)
            DestroyShaderModule(device, shader, nullptr);
        if (pipeline_layout)
            DestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (descriptor_pool)
            DestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (descriptor_layout)
            DestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
        for (Buffer* b : {&staging, &results_buffer, &queries_buffer, &nodes_buffer}) {
            if (b->mapped)
                UnmapMemory(device, b->memory);
            if (b->buffer)
                DestroyBuffer(device, b->buffer, nullptr);
            if (b->memory)
                FreeMemory(device, b->memory, nullptr);
        }
        if (device && DestroyDevice)
            DestroyDevice(device, nullptr);
    }

    void open_library()
    {
#ifdef _WIN32
        // Retain one loader reference for the process, as with a normal linked
        // Vulkan application. Repeated DLL unload/reload can race driver worker
        // teardown. All Vulkan instances/devices/buffers still have RAII owners.
        static HMODULE retained_loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        library                        = retained_loader;
        if (library)
            get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                GetProcAddress(static_cast<HMODULE>(library), "vkGetInstanceProcAddr"));
#else
        static void* retained_loader = []() -> void* {
#ifdef __APPLE__
            const char* names[] = {"@rpath/libvulkan.1.dylib", "libvulkan.1.dylib", "@rpath/libMoltenVK.dylib", "libMoltenVK.dylib"};
#else
            const char* names[] = {"libvulkan.so.1"};
#endif
            for (const char* candidate : names)
                if (void* handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL))
                    return handle;
            return nullptr;
        }();
        library = retained_loader;
        if (library)
            get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
#endif
        if (!get_instance_proc)
            throw std::runtime_error("Vulkan loader unavailable");
    }

    void make_buffer(Buffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, bool host)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size        = bytes;
        info.usage       = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(CreateBuffer(device, &info, nullptr, &buffer.buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        GetBufferMemoryRequirements(device, buffer.buffer, &requirements);
        int selected = -1, best = -1;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            const auto flags    = memory_properties.memoryTypes[i].propertyFlags;
            const auto required = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            if (!(requirements.memoryTypeBits & (1u << i)) || !(flags & required))
                continue;
            const int score = host ? ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? 2 : 0) +
                                      (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? 1 : 0)) :
                                     0;
            if (score > best) {
                best     = score;
                selected = int(i);
            }
        }
        if (selected < 0)
            throw std::runtime_error("No compatible Vulkan buffer memory type");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize  = requirements.size;
        allocation.memoryTypeIndex = uint32_t(selected);
        check(AllocateMemory(device, &allocation, nullptr, &buffer.memory), "vkAllocateMemory");
        check(BindBufferMemory(device, buffer.buffer, buffer.memory, 0), "vkBindBufferMemory");
        buffer.coherent = (memory_properties.memoryTypes[selected].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (host)
            check(MapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped), "vkMapMemory");
    }

    void host_visibility(bool flush)
    {
        if (staging.coherent)
            return;
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = staging.memory;
        range.size   = VK_WHOLE_SIZE;
        check(flush ? FlushMappedMemoryRanges(device, 1, &range) : InvalidateMappedMemoryRanges(device, 1, &range),
              "Vulkan host memory visibility");
    }

    void begin()
    {
        check(ResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(BeginCommandBuffer(command, &info), "vkBeginCommandBuffer");
    }

    void barrier(VkPipelineStageFlags from_stage, VkPipelineStageFlags to_stage, VkAccessFlags from, VkAccessFlags to)
    {
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memory.srcAccessMask = from;
        memory.dstAccessMask = to;
        CmdPipelineBarrier(command, from_stage, to_stage, 0, 1, &memory, 0, nullptr, 0, nullptr);
    }

    void submit()
    {
        check(EndCommandBuffer(command), "vkEndCommandBuffer");
        check(ResetFences(device, 1, &fence), "vkResetFences");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers    = &command;
        check(QueueSubmit(queue, 1, &info, fence), "vkQueueSubmit");
        check(WaitForFences(device, 1, &fence, VK_TRUE, 30000000000ULL), "vkWaitForFences");
    }

    void initialize(
        Kernel selected_kernel, const void* nodes, size_t node_bytes, uint32_t count_nodes, float input_radius, const std::string& filter)
    {
        const auto start         = Clock::now();
        kernel                   = selected_kernel;
        const bool   clipping    = kernel == Kernel::LocalZClipDecision;
        const bool   neighbors   = kernel == Kernel::SeamNeighbors;
        const size_t node_stride = clipping ? 16 : 32;
        if (!nodes || count_nodes == 0 || count_nodes > (1u << 24) || node_bytes != size_t(count_nodes) * node_stride ||
            (!clipping && !neighbors && (!(input_radius > 0) || !std::isfinite(input_radius))))
            throw std::runtime_error("Invalid or oversized Vulkan immutable data");
        query_stride        = clipping ? 32 : 16;
        result_stride       = neighbors ? (256 + 2) * sizeof(uint32_t) : sizeof(uint32_t);
        node_capacity_bytes = node_bytes;
        READBACK_OFFSET     = BATCH_CAPACITY * query_stride;
        node_count          = count_nodes;
        radius              = input_radius;
        open_library();
        instance_owner                           = shared_instance(get_instance_proc);
        instance                                 = instance_owner->instance;
        uint32_t                           count = 0;
        std::vector<VkExtensionProperties> extensions;
#define LOAD_INSTANCE(name) \
    name = reinterpret_cast<PFN_vk##name>(get_instance_proc(instance, "vk" #name)); \
    if (!name) \
        throw std::runtime_error("Missing vk" #name);
        INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
        check(EnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        check(EnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        int      best_score = -1;
        uint32_t family     = 0;
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            GetPhysicalDeviceProperties(candidate, &properties);
            VkPhysicalDeviceFeatures features{};
            GetPhysicalDeviceFeatures(candidate, &features);
            if (clipping && !features.shaderInt64)
                continue;
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
                continue;
            if (!filter.empty() && lowercase(properties.deviceName).find(lowercase(filter)) == std::string::npos)
                continue;
            if (properties.limits.maxStorageBufferRange <
                    std::max<VkDeviceSize>({node_bytes, READBACK_OFFSET, size_t(BATCH_CAPACITY) * result_stride}) ||
                properties.limits.maxComputeWorkGroupInvocations < 64 || properties.limits.maxComputeWorkGroupSize[0] < 64 ||
                properties.limits.maxComputeWorkGroupCount[0] < BATCH_CAPACITY / 64)
                continue;
            uint32_t family_count = 0;
            GetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            GetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
            for (uint32_t i = 0; i < family_count; ++i) {
                if (families[i].queueCount == 0 || !(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
                    continue;
                int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > best_score) {
                    physical   = candidate;
                    name       = properties.deviceName;
                    family     = i;
                    best_score = score;
                }
                break;
            }
        }
        if (!physical)
            throw std::runtime_error("No eligible Vulkan GPU matching device filter: " + filter);
        GetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        check(EnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
        extensions.resize(count);
        check(EnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()), "vkEnumerateDeviceExtensionProperties");
        const char*             subset   = "VK_KHR_portability_subset";
        float                   priority = 1;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount       = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos    = &queue_info;
        VkPhysicalDeviceFeatures enabled_features{};
        enabled_features.shaderInt64 = clipping ? VK_TRUE : VK_FALSE;
        device_info.pEnabledFeatures = &enabled_features;
        if (has_extension(extensions, subset)) {
            device_info.enabledExtensionCount   = 1;
            device_info.ppEnabledExtensionNames = &subset;
        }
        check(CreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
#define LOAD_DEVICE(name) \
    name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name)); \
    if (!name) \
        throw std::runtime_error("Missing vk" #name);
        DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
        GetDeviceQueue(device, family, 0, &queue);
        make_buffer(nodes_buffer, node_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        make_buffer(queries_buffer, READBACK_OFFSET, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        make_buffer(results_buffer, BATCH_CAPACITY * result_stride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    false);
        make_buffer(staging, std::max<VkDeviceSize>(node_bytes, READBACK_OFFSET + BATCH_CAPACITY * result_stride),
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout_info.bindingCount = 3;
        layout_info.pBindings    = bindings;
        check(CreateDescriptorSetLayout(device, &layout_info, nullptr, &descriptor_layout), "vkCreateDescriptorSetLayout");
        VkPushConstantRange        push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pipeline_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_info.setLayoutCount         = 1;
        pipeline_info.pSetLayouts            = &descriptor_layout;
        pipeline_info.pushConstantRangeCount = 1;
        pipeline_info.pPushConstantRanges    = &push;
        check(CreatePipelineLayout(device, &pipeline_info, nullptr, &pipeline_layout), "vkCreatePipelineLayout");
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = clipping  ? sizeof(LOCAL_Z_CLIP_DECISION_SPIRV) :
                               neighbors ? sizeof(SEAM_NEIGHBORS_SPIRV) :
                                           sizeof(SEAM_VISIBILITY_SPIRV);
        shader_info.pCode    = clipping ? LOCAL_Z_CLIP_DECISION_SPIRV : neighbors ? SEAM_NEIGHBORS_SPIRV : SEAM_VISIBILITY_SPIRV;
        check(CreateShaderModule(device, &shader_info, nullptr, &shader), "vkCreateShaderModule");
        VkComputePipelineCreateInfo compute_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute_info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute_info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        compute_info.stage.module = shader;
        compute_info.stage.pName  = "main";
        compute_info.layout       = pipeline_layout;
        check(CreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute_info, nullptr, &pipeline), "vkCreateComputePipelines");
        VkDescriptorPoolSize       pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets       = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes    = &pool_size;
        check(CreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_info.descriptorPool     = descriptor_pool;
        set_info.descriptorSetCount = 1;
        set_info.pSetLayouts        = &descriptor_layout;
        check(AllocateDescriptorSets(device, &set_info, &descriptor_set), "vkAllocateDescriptorSets");
        Buffer*                storage[] = {&nodes_buffer, &queries_buffer, &results_buffer};
        VkDescriptorBufferInfo buffer_info[3]{};
        VkWriteDescriptorSet   writes[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            buffer_info[i]            = {storage[i]->buffer, 0, VK_WHOLE_SIZE};
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = descriptor_set;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo     = &buffer_info[i];
        }
        UpdateDescriptorSets(device, 3, writes, 0, nullptr);
        VkCommandPoolCreateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        command_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_info.queueFamilyIndex = family;
        check(CreateCommandPool(device, &command_info, nullptr, &command_pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo command_allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_allocate.commandPool        = command_pool;
        command_allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocate.commandBufferCount = 1;
        check(AllocateCommandBuffers(device, &command_allocate, &command), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(CreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence");
        std::memcpy(staging.mapped, nodes, size_t(node_bytes));
        host_visibility(true);
        begin();
        VkBufferCopy copy{0, 0, node_bytes};
        CmdCopyBuffer(command, staging.buffer, nodes_buffer.buffer, 1, &copy);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        submit();
        timing.setup_ms = elapsed_ms(start);
    }

    void update_data(const void* data, size_t bytes, uint32_t count)
    {
        const size_t stride = kernel == Kernel::LocalZClipDecision ? 16 : 32;
        if (!data || count == 0 || bytes > node_capacity_bytes || bytes != size_t(count) * stride)
            throw std::runtime_error("Invalid or oversized Vulkan immutable-data update");
        const auto start = Clock::now();
        std::memcpy(staging.mapped, data, bytes);
        host_visibility(true);
        begin();
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferCopy copy{0, 0, bytes};
        CmdCopyBuffer(command, staging.buffer, nodes_buffer.buffer, 1, &copy);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        submit();
        node_count = count;
        timing.upload_ms += elapsed_ms(start);
    }

    void compute(const void* queries, uint32_t count, void* result)
    {
        if (count == 0)
            return;
        if (!queries || !result || count > BATCH_CAPACITY)
            throw std::runtime_error("Invalid or oversized Vulkan query batch");
        const auto         start        = Clock::now();
        const VkDeviceSize query_bytes  = size_t(count) * query_stride;
        const VkDeviceSize output_bytes = size_t(count) * result_stride;
        std::memcpy(staging.mapped, queries, size_t(query_bytes));
        host_visibility(true);
        begin();
        VkBufferCopy upload{0, 0, query_bytes};
        CmdCopyBuffer(command, staging.buffer, queries_buffer.buffer, 1, &upload);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
        struct
        {
            uint32_t count, nodes;
            float    radius, epsilon;
        } params{count, node_count, radius, 1e-4f};
        CmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        CmdDispatch(command, (params.count + 63) / 64, 1, 1);
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy download{0, READBACK_OFFSET, output_bytes};
        CmdCopyBuffer(command, results_buffer.buffer, staging.buffer, 1, &download);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        submit();
        host_visibility(false);
        std::memcpy(result, static_cast<char*>(staging.mapped) + READBACK_OFFSET, size_t(output_bytes));
        timing.dispatch_readback_ms += elapsed_ms(start);
        timing.queries += count;
    }
};
#else
struct VulkanBatch::Impl
{
    std::string       name;
    VulkanBatchTiming timing;
};
#endif

VulkanBatch::VulkanBatch(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
VulkanBatch::~VulkanBatch() = default;

bool VulkanBatch::compiled()
{
#ifdef ORCA_ENABLE_VULKAN_COMPUTE
    return true;
#else
    return false;
#endif
}

void VulkanBatch::shutdown()
{
#ifdef ORCA_ENABLE_VULKAN_COMPUTE
    auto&                       storage = instance_storage();
    std::lock_guard<std::mutex> lock(storage.mutex);
    storage.owner.reset();
#endif
}

std::unique_ptr<VulkanBatch> VulkanBatch::create(
    Kernel kernel, const void* data, size_t bytes, uint32_t count, float parameter, const std::string& filter, std::string& error)
{
    error.clear();
#ifdef ORCA_ENABLE_VULKAN_COMPUTE
    try {
        auto impl = std::make_unique<Impl>();
        impl->initialize(kernel, data, bytes, count, parameter, filter);
        return std::unique_ptr<VulkanBatch>(new VulkanBatch(std::move(impl)));
    } catch (const std::exception& e) {
        error = e.what();
    }
#else
    (void) kernel;
    (void) data;
    (void) bytes;
    (void) count;
    (void) parameter;
    (void) filter;
    error = "Vulkan compute was disabled at build time";
#endif
    return nullptr;
}
bool VulkanBatch::compute(const void* queries, uint32_t count, void* results, std::string& error)
{
    error.clear();
#ifdef ORCA_ENABLE_VULKAN_COMPUTE
    try {
        m_impl->compute(queries, count, results);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
    }
#else
    (void) queries;
    (void) count;
    (void) results;
    error = "Vulkan compute was disabled at build time";
#endif
    return false;
}
bool VulkanBatch::update_data(const void* data, size_t bytes, uint32_t count, std::string& error)
{
    error.clear();
#ifdef ORCA_ENABLE_VULKAN_COMPUTE
    try {
        m_impl->update_data(data, bytes, count);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
    }
#else
    (void) data;
    (void) bytes;
    (void) count;
    error = "Vulkan compute was disabled at build time";
#endif
    return false;
}
const std::string&       VulkanBatch::device_name() const { return m_impl->name; }
const VulkanBatchTiming& VulkanBatch::timing() const { return m_impl->timing; }
} // namespace Slic3r
