// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "compiler.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vulkan/vk_layer.h>

#if defined(_WIN32)
#define VEL_EXPORT extern "C" __declspec(dllexport)
#else
#define VEL_EXPORT extern "C" __attribute__((visibility("default")))
#endif

VEL_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char *);
VEL_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char *);

namespace {
constexpr char kName[] = "VK_LAYER_HANYANG_ray_tracing_fallback";
constexpr VkShaderStageFlags kRT = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                                   VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                                   VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;
constexpr VkExtensionProperties kExtensions[] = {
    {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_SPEC_VERSION},
    {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, VK_KHR_RAY_TRACING_PIPELINE_SPEC_VERSION},
    {VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION}};
std::recursive_mutex mutex;
struct Instance {
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_GetPhysicalDeviceProcAddr gpdpa;
};
std::unordered_map<VkInstance, Instance> instances;
std::unordered_map<VkPhysicalDevice, VkInstance> physicals;
struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
};
struct AS {
    VkAccelerationStructureCreateInfoKHR info{};
    uint64_t address{};
    Buffer descriptor;
};
struct Pipeline {
    VkPipeline compute{};
    VkPipelineLayout layout{};
    VkDescriptorSetLayout runtime{};
    uint32_t sets{}, groups{};
};
struct Layout {
    std::vector<VkDescriptorSetLayout> sets;
    std::vector<VkPushConstantRange> push;
};
struct Bind {
    VkPipelineLayout layout;
    uint32_t first;
    std::vector<VkDescriptorSet> sets;
    std::vector<uint32_t> offsets;
};
struct Push {
    VkPipelineLayout layout;
    VkShaderStageFlags stages;
    uint32_t offset;
    std::vector<uint8_t> bytes;
};
struct Command {
    VkPipeline compute{}, rt{};
    std::vector<Bind> compute_binds, rt_binds;
    std::vector<Push> compute_push, rt_push;
    std::vector<Buffer> buffers;
    std::vector<VkDescriptorPool> pools;
    bool error = false;
};
struct Device {
    VkDevice handle{};
    VkPhysicalDevice physical{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::array<VkPipeline, static_cast<size_t>(vel::rt::BuildPass::Count)> builders{};
    VkPipelineLayout build_layout{};
    std::unordered_map<VkAccelerationStructureKHR, std::unique_ptr<AS>> structures;
    std::unordered_map<VkShaderModule, std::unique_ptr<std::vector<uint32_t>>> modules;
    std::unordered_map<VkPipeline, std::unique_ptr<Pipeline>> pipelines;
    std::unordered_map<VkPipelineLayout, Layout> layouts;
    std::unordered_map<VkCommandBuffer, Command> commands;
    std::unordered_map<VkCommandBuffer, VkCommandPool> command_pools;
    std::unordered_map<VkDeferredOperationKHR, std::unique_ptr<uint32_t>> deferred;
    template <class F>
    F fn(const char *n) {
        return reinterpret_cast<F>(gdpa(handle, n));
    }
};
std::unordered_map<VkDevice, std::unique_ptr<Device>> devices;
std::unordered_map<VkCommandBuffer, VkDevice> command_devices;
std::unordered_map<VkQueue, VkDevice> queues;
#define D_FN(d, name) (d).fn<PFN_vk##name>("vk" #name)
#define LOCK std::lock_guard<std::recursive_mutex> lock(mutex)
Device &D(VkDevice d) { return *devices.at(d); }
Device &C(VkCommandBuffer cb) { return D(command_devices.at(cb)); }
void Error(Command &c, const char *message) {
    c.error = true;
    std::fprintf(stderr, "VEL RT fallback: %s\n", message);
}
void Check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("Vulkan operation failed: " + std::to_string(r));
}
VkShaderStageFlags Stages(VkShaderStageFlags s) { return (s & ~kRT) | ((s & kRT) ? VK_SHADER_STAGE_COMPUTE_BIT : 0); }
VkPipelineStageFlags2 Stages2(VkPipelineStageFlags2 s) {
    auto rt = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    return (s & ~rt) | ((s & rt) ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : 0);
}
VkAccessFlags2 Access2(VkAccessFlags2 s) {
    auto out = s & ~(VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    if (s & VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR) out |= VK_ACCESS_2_SHADER_READ_BIT;
    if (s & VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR) out |= VK_ACCESS_2_SHADER_WRITE_BIT;
    return out;
}
bool Emulated(const char *n) {
    for (auto &e : kExtensions)
        if (!std::strcmp(n, e.extensionName)) return true;
    return false;
}
bool UnsupportedRT(const char *n) {
    return std::strstr(n, "AccelerationStructure") || std::strstr(n, "RayTracing") || std::strstr(n, "TraceRays") ||
           std::strstr(n, "DeferredOperation");
}
bool Supported(VkPhysicalDevice p) {
    auto &i = instances.at(physicals.at(p));
    VkPhysicalDeviceVulkan12Features v{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(i.gipa(i.handle, "vkGetPhysicalDeviceFeatures2"));
    if (!query) return false;
    query(p, &f);
    VkPhysicalDeviceProperties properties;
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(i.gipa(i.handle, "vkGetPhysicalDeviceProperties"))(p, &properties);
    return f.features.shaderInt64 && f.features.shaderInt16 && v.bufferDeviceAddress && v11.storageBuffer16BitAccess &&
           properties.limits.maxComputeWorkGroupInvocations >= 1024 && properties.limits.maxComputeWorkGroupSize[0] >= 1024 &&
           properties.limits.maxComputeSharedMemorySize >= 16384;
}
template <class T>
VkResult Enumerate(const std::vector<T> &src, uint32_t *n, T *dst) {
    if (!dst) {
        *n = uint32_t(src.size());
        return VK_SUCCESS;
    }
    uint32_t count = std::min(*n, uint32_t(src.size()));
    std::copy_n(src.begin(), count, dst);
    *n = count;
    return count < src.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
uint64_t Address(Device &d, VkBuffer b) {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = b;
    return D_FN(d, GetBufferDeviceAddress)(d.handle, &info);
}
void Free(Device &d, Buffer b) {
    if (b.buffer) D_FN(d, DestroyBuffer)(d.handle, b.buffer, nullptr);
    if (b.memory) D_FN(d, FreeMemory)(d.handle, b.memory, nullptr);
}
Buffer Upload(Device &d, const void *data, size_t size) {
    Buffer out;
    try {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        Check(D_FN(d, CreateBuffer)(d.handle, &ci, nullptr, &out.buffer));
        VkMemoryRequirements req;
        D_FN(d, GetBufferMemoryRequirements)(d.handle, out.buffer, &req);
        uint32_t type = 0;
        for (; type < d.memory.memoryTypeCount; type++)
            if ((req.memoryTypeBits & (1u << type)) &&
                (d.memory.memoryTypes[type].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                break;
        if (type == d.memory.memoryTypeCount) throw std::runtime_error("No coherent host memory type");
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        Check(D_FN(d, AllocateMemory)(d.handle, &ai, nullptr, &out.memory));
        Check(D_FN(d, BindBufferMemory)(d.handle, out.buffer, out.memory, 0));
        void *ptr;
        Check(D_FN(d, MapMemory)(d.handle, out.memory, 0, size, 0, &ptr));
        std::memcpy(ptr, data, size);
        D_FN(d, UnmapMemory)(d.handle, out.memory);
    } catch (...) {
        Free(d, out);
        throw;
    }
    return out;
}
VkPipeline Compute(Device &d, VkPipelineLayout layout, const std::vector<uint32_t> &code) {
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = code.size() * 4;
    sm.pCode = code.data();
    VkShaderModule module;
    Check(D_FN(d, CreateShaderModule)(d.handle, &sm, nullptr, &module));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.layout = layout;
    ci.stage = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    VkPipeline p;
    auto result = D_FN(d, CreateComputePipelines)(d.handle, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    D_FN(d, DestroyShaderModule)(d.handle, module, nullptr);
    Check(result);
    return p;
}
void Clear(Device &d, Command &c) {
    for (auto b : c.buffers) Free(d, b);
    for (auto p : c.pools) D_FN(d, DestroyDescriptorPool)(d.handle, p, nullptr);
    c = Command{};
}
void Replay(Device &d, VkCommandBuffer cb, const std::vector<Bind> &binds, const std::vector<Push> &pushes,
            VkPipelineLayout override_layout = VK_NULL_HANDLE) {
    for (auto &b : binds) D_FN(d, CmdBindDescriptorSets)
    (cb, VK_PIPELINE_BIND_POINT_COMPUTE, override_layout ? override_layout : b.layout, b.first, uint32_t(b.sets.size()),
     b.sets.data(), uint32_t(b.offsets.size()), b.offsets.data());
    for (auto &p : pushes) D_FN(d, CmdPushConstants)
    (cb, override_layout ? override_layout : p.layout, VK_SHADER_STAGE_COMPUTE_BIT, p.offset, uint32_t(p.bytes.size()),
     p.bytes.data());
}
void Restore(Device &d, VkCommandBuffer cb) {
    auto &c = d.commands.at(cb);
    if (c.compute) D_FN(d, CmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.compute);
    Replay(d, cb, c.compute_binds, c.compute_push);
}
template <class T>
T *Find(void *chain, VkStructureType type) {
    for (auto p = static_cast<VkBaseOutStructure *>(chain); p; p = p->pNext)
        if (p->sType == type) return reinterpret_cast<T *>(p);
    return nullptr;
}
}  // namespace

VEL_EXPORT VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                VkInstance *out) {
    LOCK;
    if (!ci->pApplicationInfo || ci->pApplicationInfo->apiVersion < VK_API_VERSION_1_2) return VK_ERROR_INCOMPATIBLE_DRIVER;
    auto p = Find<VkLayerInstanceCreateInfo>(const_cast<void *>(ci->pNext), VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    while (p && p->function != VK_LAYER_LINK_INFO)
        p = Find<VkLayerInstanceCreateInfo>(const_cast<void *>(p->pNext), VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!p) return VK_ERROR_INITIALIZATION_FAILED;
    auto link = p->u.pLayerInfo;
    auto next = link->pfnNextGetInstanceProcAddr;
    auto gpdpa = link->pfnNextGetPhysicalDeviceProcAddr;
    p->u.pLayerInfo = link->pNext;
    auto f = reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE, "vkCreateInstance"));
    auto result = f(ci, alloc, out);
    if (result == VK_SUCCESS) instances.emplace(*out, Instance{*out, next, gpdpa});
    return result;
}
VEL_EXPORT void VKAPI_CALL vkDestroyInstance(VkInstance i, const VkAllocationCallbacks *a) {
    LOCK;
    auto data = instances.at(i);
    for (auto it = physicals.begin(); it != physicals.end();)
        if (it->second == i)
            it = physicals.erase(it);
        else
            ++it;
    reinterpret_cast<PFN_vkDestroyInstance>(data.gipa(i, "vkDestroyInstance"))(i, a);
    instances.erase(i);
}
VEL_EXPORT VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance i, uint32_t *n, VkPhysicalDevice *p) {
    LOCK;
    auto &data = instances.at(i);
    auto r = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(data.gipa(i, "vkEnumeratePhysicalDevices"))(i, n, p);
    if (p && (r == VK_SUCCESS || r == VK_INCOMPLETE))
        for (uint32_t j = 0; j < *n; j++) physicals[p[j]] = i;
    return r;
}
VEL_EXPORT VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(VkInstance i, uint32_t *n, VkPhysicalDeviceGroupProperties *p) {
    LOCK;
    auto &data = instances.at(i);
    auto f = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(data.gipa(i, "vkEnumeratePhysicalDeviceGroups"));
    auto r = f(i, n, p);
    if (p && (r == VK_SUCCESS || r == VK_INCOMPLETE))
        for (uint32_t j = 0; j < *n; j++)
            for (uint32_t k = 0; k < p[j].physicalDeviceCount; k++) physicals[p[j].physicalDevices[k]] = i;
    return r;
}
VEL_EXPORT VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p, const char *layer, uint32_t *n,
                                                                    VkExtensionProperties *dst) {
    LOCK;
    if (layer && std::strcmp(layer, kName) == 0)
        return Enumerate(std::vector<VkExtensionProperties>(std::begin(kExtensions), std::end(kExtensions)), n, dst);
    auto &i = instances.at(physicals.at(p));
    auto f = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(i.gipa(i.handle, "vkEnumerateDeviceExtensionProperties"));
    if (layer) return f(p, layer, n, dst);
    uint32_t count = 0;
    auto r = f(p, nullptr, &count, nullptr);
    if (r != VK_SUCCESS) return r;
    std::vector<VkExtensionProperties> props(count);
    r = f(p, nullptr, &count, props.data());
    if (r != VK_SUCCESS) return r;
    props.resize(count);
    if (Supported(p))
        for (auto &e : kExtensions)
            if (std::none_of(props.begin(), props.end(),
                             [&](auto &a) { return std::strcmp(a.extensionName, e.extensionName) == 0; }))
                props.push_back(e);
    return Enumerate(props, n, dst);
}
VEL_EXPORT void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *f) {
    LOCK;
    auto &i = instances.at(physicals.at(p));
    reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(i.gipa(i.handle, "vkGetPhysicalDeviceFeatures2"))(p, f);
    if (auto a = Find<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(
            f->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR)) {
        auto next = a->pNext;
        *a = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, next};
        a->accelerationStructure = Supported(p);
    }
    if (auto a = Find<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(
            f->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR)) {
        auto next = a->pNext;
        *a = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, next};
        a->rayTracingPipeline = Supported(p);
    }
}
VEL_EXPORT void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *f) {
    LOCK;
    auto &i = instances.at(physicals.at(p));
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(i.gipa(i.handle, "vkGetPhysicalDeviceProperties2"))(p, f);
    if (auto a = Find<VkPhysicalDeviceRayTracingPipelinePropertiesKHR>(
            f->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR)) {
        auto next = a->pNext;
        *a = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR, next};
        a->shaderGroupHandleSize = 32;
        a->shaderGroupHandleAlignment = 32;
        a->shaderGroupBaseAlignment = 64;
        a->maxRayRecursionDepth = 1;
        a->maxShaderGroupStride = 4096;
        a->maxRayDispatchInvocationCount = 1u << 24;
        a->maxRayHitAttributeSize = 8;
    }
    if (auto a = Find<VkPhysicalDeviceAccelerationStructurePropertiesKHR>(
            f->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR)) {
        auto next = a->pNext;
        *a = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR, next};
        a->maxGeometryCount = vel::rt::kMaxASPrimitives;
        a->maxPrimitiveCount = 1u << 20;
        a->maxInstanceCount = 1u << 20;
        a->minAccelerationStructureScratchOffsetAlignment = 256;
        a->maxPerStageDescriptorAccelerationStructures = 1;
        a->maxDescriptorSetAccelerationStructures = 1;
    }
}

VEL_EXPORT VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *info,
                                              const VkAllocationCallbacks *alloc, VkDevice *out) {
    LOCK;
    try {
        if (!Supported(p)) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (auto a = Find<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(
                const_cast<void *>(info->pNext), VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR))
            if (a->accelerationStructureCaptureReplay || a->accelerationStructureIndirectBuild ||
                a->accelerationStructureHostCommands || a->descriptorBindingAccelerationStructureUpdateAfterBind)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        if (auto a = Find<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(
                const_cast<void *>(info->pNext), VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR))
            if (a->rayTracingPipelineShaderGroupHandleCaptureReplay || a->rayTracingPipelineShaderGroupHandleCaptureReplayMixed ||
                a->rayTracingPipelineTraceRaysIndirect || a->rayTraversalPrimitiveCulling)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        auto loader = Find<VkLayerDeviceCreateInfo>(const_cast<void *>(info->pNext), VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
        while (loader && loader->function != VK_LAYER_LINK_INFO)
            loader = Find<VkLayerDeviceCreateInfo>(const_cast<void *>(loader->pNext), VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
        if (!loader) return VK_ERROR_INITIALIZATION_FAILED;
        auto link = loader->u.pLayerInfo;
        auto gdpa = link->pfnNextGetDeviceProcAddr;
        auto create = reinterpret_cast<PFN_vkCreateDevice>(
            link->pfnNextGetInstanceProcAddr(instances.at(physicals.at(p)).handle, "vkCreateDevice"));
        loader->u.pLayerInfo = link->pNext;
        VkDeviceCreateInfo ci = *info;
        std::vector<const char *> extensions;
        for (uint32_t j = 0; j < ci.enabledExtensionCount; j++)
            if (!Emulated(ci.ppEnabledExtensionNames[j])) extensions.push_back(ci.ppEnabledExtensionNames[j]);
        ci.enabledExtensionCount = uint32_t(extensions.size());
        ci.ppEnabledExtensionNames = extensions.data();
        std::vector<std::vector<uint64_t>> copies;
        VkBaseOutStructure *head = nullptr;
        VkBaseOutStructure *tail = nullptr;
        // Copy known feature nodes, never unlink or overwrite the caller's feature
        // chain.
        for (auto node = static_cast<const VkBaseInStructure *>(ci.pNext); node; node = node->pNext) {
            size_t size = 0;
            switch (node->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR:
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR:
                    continue;
                case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO:
                    size = sizeof(VkLayerDeviceCreateInfo);
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
                    size = sizeof(VkPhysicalDeviceFeatures2);
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
                    size = sizeof(VkPhysicalDeviceVulkan11Features);
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
                    size = sizeof(VkPhysicalDeviceVulkan12Features);
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
                    size = sizeof(VkPhysicalDeviceVulkan13Features);
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
                    size = sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures);
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
                    size = sizeof(VkPhysicalDeviceSynchronization2Features);
                    break;
                default:
                    return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            copies.emplace_back((size + 7) / 8);
            std::memcpy(copies.back().data(), node, size);
            auto copy = reinterpret_cast<VkBaseOutStructure *>(copies.back().data());
            copy->pNext = nullptr;
            if (tail)
                tail->pNext = copy;
            else
                head = copy;
            tail = copy;
        }
        ci.pNext = head;
        auto core = ci.pEnabledFeatures ? *ci.pEnabledFeatures : VkPhysicalDeviceFeatures{};
        if (auto f = Find<VkPhysicalDeviceFeatures2>(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)) {
            f->features.shaderInt64 = VK_TRUE;
            f->features.shaderInt16 = VK_TRUE;
        } else {
            core.shaderInt64 = VK_TRUE;
            core.shaderInt16 = VK_TRUE;
            ci.pEnabledFeatures = &core;
        }
        VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        bda.bufferDeviceAddress = VK_TRUE;
        if (auto v = Find<VkPhysicalDeviceVulkan12Features>(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES))
            v->bufferDeviceAddress = VK_TRUE;
        else if (auto v = Find<VkPhysicalDeviceBufferDeviceAddressFeatures>(
                     head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES))
            v->bufferDeviceAddress = VK_TRUE;
        else {
            bda.pNext = head;
            ci.pNext = &bda;
        }
        VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        if (auto v = Find<VkPhysicalDeviceVulkan11Features>(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES))
            v->storageBuffer16BitAccess = VK_TRUE;
        else {
            v11.storageBuffer16BitAccess = VK_TRUE;
            v11.pNext = const_cast<void *>(ci.pNext);
            ci.pNext = &v11;
        }
        auto result = create(p, &ci, alloc, out);
        if (result != VK_SUCCESS) return result;
        auto d = std::make_unique<Device>();
        d->handle = *out;
        d->physical = p;
        d->gdpa = gdpa;
        auto &i = instances.at(physicals.at(p));
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(i.gipa(i.handle, "vkGetPhysicalDeviceMemoryProperties"))(
            p, &d->memory);
        try {
            VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pl.pushConstantRangeCount = 1;
            pl.pPushConstantRanges = &range;
            Check(D_FN(*d, CreatePipelineLayout)(*out, &pl, nullptr, &d->build_layout));
            for (size_t pass = 0; pass < d->builders.size(); ++pass)
                d->builders[pass] = Compute(*d, d->build_layout, vel::rt::BuildShader(static_cast<vel::rt::BuildPass>(pass)));
        } catch (...) {
            for (auto pipeline : d->builders)
                if (pipeline) D_FN(*d, DestroyPipeline)(*out, pipeline, nullptr);
            if (d->build_layout) D_FN(*d, DestroyPipelineLayout)(*out, d->build_layout, nullptr);
            D_FN(*d, DestroyDevice)(*out, alloc);
            *out = VK_NULL_HANDLE;
            throw;
        }
        devices.emplace(*out, std::move(d));
        return VK_SUCCESS;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "VEL RT device: %s\n", e.what());
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}
VEL_EXPORT void VKAPI_CALL vkDestroyDevice(VkDevice h, const VkAllocationCallbacks *a) {
    LOCK;
    auto &d = D(h);
    for (auto it = queues.begin(); it != queues.end();) {
        if (it->second == h)
            it = queues.erase(it);
        else
            ++it;
    }
    for (auto &[cb, c] : d.commands) {
        Clear(d, c);
        command_devices.erase(cb);
    }
    for (auto &[h, p] : d.pipelines) {
        D_FN(d, DestroyPipeline)(d.handle, p->compute, nullptr);
        D_FN(d, DestroyPipelineLayout)(d.handle, p->layout, nullptr);
        D_FN(d, DestroyDescriptorSetLayout)(d.handle, p->runtime, nullptr);
    }
    for (auto &[h, p] : d.structures) Free(d, p->descriptor);
    for (auto pipeline : d.builders) D_FN(d, DestroyPipeline)(h, pipeline, nullptr);
    D_FN(d, DestroyPipelineLayout)(h, d.build_layout, nullptr);
    D_FN(d, DestroyDevice)(h, a);
    devices.erase(h);
}

VEL_EXPORT void VKAPI_CALL vkGetDeviceQueue(VkDevice h, uint32_t family, uint32_t index, VkQueue *out) {
    LOCK;
    D_FN(D(h), GetDeviceQueue)(h, family, index, out);
    queues[*out] = h;
}
VEL_EXPORT void VKAPI_CALL vkGetDeviceQueue2(VkDevice h, const VkDeviceQueueInfo2 *info, VkQueue *out) {
    LOCK;
    D_FN(D(h), GetDeviceQueue2)(h, info, out);
    queues[*out] = h;
}
VEL_EXPORT VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    LOCK;
    std::vector<VkSubmitInfo> submits;
    if (count) submits.assign(info, info + count);
    std::vector<std::vector<VkPipelineStageFlags>> stages(count);
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < info[i].waitSemaphoreCount; j++)
            stages[i].push_back(VkPipelineStageFlags(Stages2(info[i].pWaitDstStageMask[j])));
        submits[i].pWaitDstStageMask = stages[i].data();
    }
    return D_FN(D(queues.at(queue)), QueueSubmit)(queue, count, submits.data(), fence);
}
VEL_EXPORT VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *info, VkFence fence) {
    LOCK;
    std::vector<VkSubmitInfo2> submits;
    if (count) submits.assign(info, info + count);
    std::vector<std::vector<VkSemaphoreSubmitInfo>> waits(count), signals(count);
    for (uint32_t i = 0; i < count; i++) {
        if (info[i].waitSemaphoreInfoCount)
            waits[i].assign(info[i].pWaitSemaphoreInfos, info[i].pWaitSemaphoreInfos + info[i].waitSemaphoreInfoCount);
        if (info[i].signalSemaphoreInfoCount)
            signals[i].assign(info[i].pSignalSemaphoreInfos, info[i].pSignalSemaphoreInfos + info[i].signalSemaphoreInfoCount);
        for (auto &x : waits[i]) x.stageMask = Stages2(x.stageMask);
        for (auto &x : signals[i]) x.stageMask = Stages2(x.stageMask);
        submits[i].pWaitSemaphoreInfos = waits[i].data();
        submits[i].pSignalSemaphoreInfos = signals[i].data();
    }
    auto &d = D(queues.at(queue));
    auto fn = D_FN(d, QueueSubmit2);
    if (!fn) fn = d.fn<PFN_vkQueueSubmit2>("vkQueueSubmit2KHR");
    return fn(queue, count, submits.data(), fence);
}

VEL_EXPORT VkResult VKAPI_CALL vkCreateBuffer(VkDevice h, const VkBufferCreateInfo *in, const VkAllocationCallbacks *a,
                                              VkBuffer *out) {
    LOCK;
    auto ci = *in;
    auto rt = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
    if (ci.usage & rt) ci.usage = (ci.usage & ~rt) | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return D_FN(D(h), CreateBuffer)(h, &ci, a, out);
}
VEL_EXPORT VkResult VKAPI_CALL vkCreateShaderModule(VkDevice h, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                    VkShaderModule *out) {
    LOCK;
    bool rt = false;
    if (ci->codeSize < 20 || ci->codeSize % 4) return VK_ERROR_INVALID_SHADER_NV;
    for (size_t n = 5; n < ci->codeSize / 4;) {
        auto word = ci->pCode[n];
        uint32_t size = word >> 16, op = word & 65535;
        if (!size || n + size > ci->codeSize / 4) return VK_ERROR_INVALID_SHADER_NV;
        if (op == 15 && size > 2 && ci->pCode[n + 1] >= 5313 && ci->pCode[n + 1] <= 5318) rt = true;
        n += size;
    }
    if (!rt) return D_FN(D(h), CreateShaderModule)(h, ci, a, out);
    auto module = std::make_unique<std::vector<uint32_t>>(ci->pCode, ci->pCode + ci->codeSize / 4);
    *out = reinterpret_cast<VkShaderModule>(module.get());
    D(h).modules.emplace(*out, std::move(module));
    return VK_SUCCESS;
}
VEL_EXPORT void VKAPI_CALL vkDestroyShaderModule(VkDevice h, VkShaderModule m, const VkAllocationCallbacks *a) {
    LOCK;
    if (!D(h).modules.erase(m)) D_FN(D(h), DestroyShaderModule)(h, m, a);
}
VEL_EXPORT VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice h, const VkDescriptorSetLayoutCreateInfo *in,
                                                           const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    LOCK;
    auto ci = *in;
    std::vector<VkDescriptorSetLayoutBinding> bindings(in->pBindings, in->pBindings + in->bindingCount);
    for (auto &b : bindings) {
        b.stageFlags = Stages(b.stageFlags);
        if (b.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    ci.pBindings = bindings.data();
    return D_FN(D(h), CreateDescriptorSetLayout)(h, &ci, a, out);
}
VEL_EXPORT VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice h, const VkDescriptorPoolCreateInfo *in,
                                                      const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    LOCK;
    auto ci = *in;
    std::vector<VkDescriptorPoolSize> sizes;
    for (uint32_t i = 0; i < in->poolSizeCount; i++) {
        auto s = in->pPoolSizes[i];
        if (s.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) s.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        auto it = std::find_if(sizes.begin(), sizes.end(), [&](auto &q) { return q.type == s.type; });
        if (it == sizes.end())
            sizes.push_back(s);
        else
            it->descriptorCount += s.descriptorCount;
    }
    ci.poolSizeCount = uint32_t(sizes.size());
    ci.pPoolSizes = sizes.data();
    return D_FN(D(h), CreateDescriptorPool)(h, &ci, a, out);
}
VEL_EXPORT void VKAPI_CALL vkUpdateDescriptorSets(VkDevice h, uint32_t n, const VkWriteDescriptorSet *in, uint32_t copies,
                                                  const VkCopyDescriptorSet *copy) {
    LOCK;
    auto &d = D(h);
    std::vector<VkWriteDescriptorSet> writes(in, in + n);
    std::vector<std::vector<VkDescriptorBufferInfo>> buffers(n);
    for (uint32_t i = 0; i < n; i++)
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
            auto as = Find<VkWriteDescriptorSetAccelerationStructureKHR>(
                const_cast<void *>(writes[i].pNext), VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
            if (!as) return;
            for (uint32_t j = 0; j < as->accelerationStructureCount; j++)
                buffers[i].push_back({d.structures.at(as->pAccelerationStructures[j])->descriptor.buffer, 0, 8});
            writes[i].pNext = nullptr;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = buffers[i].data();
        }
    D_FN(d, UpdateDescriptorSets)(h, n, writes.data(), copies, copy);
}
VEL_EXPORT VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice h, const VkPipelineLayoutCreateInfo *in,
                                                      const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    LOCK;
    auto ci = *in;
    Layout l;
    if (in->setLayoutCount) l.sets.assign(in->pSetLayouts, in->pSetLayouts + in->setLayoutCount);
    if (in->pushConstantRangeCount) l.push.assign(in->pPushConstantRanges, in->pPushConstantRanges + in->pushConstantRangeCount);
    for (auto &p : l.push) p.stageFlags = Stages(p.stageFlags);
    ci.pPushConstantRanges = l.push.data();
    auto r = D_FN(D(h), CreatePipelineLayout)(h, &ci, a, out);
    if (r == VK_SUCCESS) D(h).layouts[*out] = std::move(l);
    return r;
}
VEL_EXPORT void VKAPI_CALL vkDestroyPipelineLayout(VkDevice h, VkPipelineLayout l, const VkAllocationCallbacks *a) {
    LOCK;
    D(h).layouts.erase(l);
    D_FN(D(h), DestroyPipelineLayout)(h, l, a);
}

VEL_EXPORT VkResult VKAPI_CALL vkCreateRayTracingPipelinesKHR(VkDevice h, VkDeferredOperationKHR deferred, VkPipelineCache,
                                                              uint32_t count, const VkRayTracingPipelineCreateInfoKHR *infos,
                                                              const VkAllocationCallbacks *, VkPipeline *out) {
    LOCK;
    auto &d = D(h);
    std::fill_n(out, count, VK_NULL_HANDLE);
    if (deferred && d.deferred.find(deferred) == d.deferred.end()) return VK_ERROR_UNKNOWN;
    for (uint32_t k = 0; k < count; k++) {
        const auto &ci = infos[k];
        auto p = std::make_unique<Pipeline>();
        try {
            if (ci.maxPipelineRayRecursionDepth != 1 || ci.pNext || ci.flags || ci.pLibraryInfo || ci.pLibraryInterface ||
                ci.pDynamicState)
                throw std::runtime_error("Unsupported RT pipeline options");
            auto original = d.layouts.at(ci.layout);
            std::vector<vel::rt::Stage> stages;
            for (uint32_t j = 0; j < ci.stageCount; j++) {
                if (ci.pStages[j].pSpecializationInfo || ci.pStages[j].pNext)
                    throw std::runtime_error("Specialization or shader pNext is not implemented");
                stages.push_back({ci.pStages[j].stage, *d.modules.at(ci.pStages[j].module), ci.pStages[j].pName});
            }
            p->sets = uint32_t(original.sets.size());
            p->groups = ci.groupCount;
            auto program = vel::rt::Link(stages, {ci.pGroups, ci.pGroups + ci.groupCount}, p->sets);
            VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            sl.bindingCount = 1;
            sl.pBindings = &binding;
            Check(D_FN(d, CreateDescriptorSetLayout)(h, &sl, nullptr, &p->runtime));
            original.sets.push_back(p->runtime);
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pl.setLayoutCount = uint32_t(original.sets.size());
            pl.pSetLayouts = original.sets.data();
            pl.pushConstantRangeCount = uint32_t(original.push.size());
            pl.pPushConstantRanges = original.push.data();
            Check(D_FN(d, CreatePipelineLayout)(h, &pl, nullptr, &p->layout));
            p->compute = Compute(d, p->layout, program.spirv);
            out[k] = reinterpret_cast<VkPipeline>(p.get());
            d.pipelines.emplace(out[k], std::move(p));
        } catch (const std::exception &e) {
            if (p->compute) D_FN(d, DestroyPipeline)(h, p->compute, nullptr);
            if (p->layout) D_FN(d, DestroyPipelineLayout)(h, p->layout, nullptr);
            if (p->runtime) D_FN(d, DestroyDescriptorSetLayout)(h, p->runtime, nullptr);
            std::fprintf(stderr, "VEL RT pipeline: %s\n", e.what());
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    return deferred ? VK_OPERATION_NOT_DEFERRED_KHR : VK_SUCCESS;
}
VEL_EXPORT void VKAPI_CALL vkDestroyPipeline(VkDevice h, VkPipeline pipeline, const VkAllocationCallbacks *a) {
    LOCK;
    auto &d = D(h);
    auto it = d.pipelines.find(pipeline);
    if (it == d.pipelines.end()) {
        D_FN(d, DestroyPipeline)(h, pipeline, a);
        return;
    }
    auto &p = *it->second;
    D_FN(d, DestroyPipeline)(h, p.compute, nullptr);
    D_FN(d, DestroyPipelineLayout)(h, p.layout, nullptr);
    D_FN(d, DestroyDescriptorSetLayout)(h, p.runtime, nullptr);
    d.pipelines.erase(it);
}
VEL_EXPORT VkResult VKAPI_CALL vkGetRayTracingShaderGroupHandlesKHR(VkDevice h, VkPipeline pipeline, uint32_t first, uint32_t count,
                                                                    size_t size, void *data) {
    LOCK;
    auto &p = *D(h).pipelines.at(pipeline);
    if (first > p.groups || count > p.groups - first || size < size_t(count) * 32) return VK_ERROR_UNKNOWN;
    std::memset(data, 0, size_t(count) * 32);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t group = first + i;
        std::memcpy(static_cast<uint8_t *>(data) + i * 32, &group, 4);
    }
    return VK_SUCCESS;
}
VEL_EXPORT VkDeviceSize VKAPI_CALL vkGetRayTracingShaderGroupStackSizeKHR(VkDevice, VkPipeline, uint32_t, VkShaderGroupShaderKHR) {
    // Traversal reserves two 64-entry uint stacks per invocation in the compute
    // program.
    return 512;
}
VEL_EXPORT void VKAPI_CALL vkCmdSetRayTracingPipelineStackSizeKHR(VkCommandBuffer, uint32_t) {
    // Dynamic pipeline stack state is not accepted by pipeline creation in this
    // subset.
}
VEL_EXPORT VkResult VKAPI_CALL vkCreateDeferredOperationKHR(VkDevice h, const VkAllocationCallbacks *,
                                                            VkDeferredOperationKHR *out) {
    LOCK;
    auto p = std::make_unique<uint32_t>(0);
    *out = reinterpret_cast<VkDeferredOperationKHR>(p.get());
    D(h).deferred.emplace(*out, std::move(p));
    return VK_SUCCESS;
}
VEL_EXPORT void VKAPI_CALL vkDestroyDeferredOperationKHR(VkDevice h, VkDeferredOperationKHR op, const VkAllocationCallbacks *) {
    LOCK;
    D(h).deferred.erase(op);
}
VEL_EXPORT uint32_t VKAPI_CALL vkGetDeferredOperationMaxConcurrencyKHR(VkDevice, VkDeferredOperationKHR) { return 0; }
VEL_EXPORT VkResult VKAPI_CALL vkGetDeferredOperationResultKHR(VkDevice, VkDeferredOperationKHR) { return VK_SUCCESS; }
VEL_EXPORT VkResult VKAPI_CALL vkDeferredOperationJoinKHR(VkDevice, VkDeferredOperationKHR) { return VK_SUCCESS; }

VEL_EXPORT void VKAPI_CALL vkGetAccelerationStructureBuildSizesKHR(VkDevice, VkAccelerationStructureBuildTypeKHR,
                                                                   const VkAccelerationStructureBuildGeometryInfoKHR *ci,
                                                                   const uint32_t *counts,
                                                                   VkAccelerationStructureBuildSizesInfoKHR *out) {
    LOCK;
    out->accelerationStructureSize = out->buildScratchSize = out->updateScratchSize = 0;
    try {
        if (ci->type != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR && ci->type != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
            return;
        if (ci->geometryCount > vel::rt::kMaxASPrimitives ||
            (ci->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR && ci->geometryCount != 1))
            return;
        uint64_t total = 0;
        for (uint32_t i = 0; i < ci->geometryCount; ++i) total += counts[i];
        if (total > vel::rt::kMaxASPrimitives) return;
        out->accelerationStructureSize = vel::rt::AccelerationStructureSize(
            uint32_t(total), ci->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, ci->geometryCount);
        out->buildScratchSize = vel::rt::BuildScratchSize(uint32_t(total));
        if (ci->flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) out->updateScratchSize = out->buildScratchSize;
    } catch (...) {
    }
}
VEL_EXPORT VkResult VKAPI_CALL vkCreateAccelerationStructureKHR(VkDevice h, const VkAccelerationStructureCreateInfoKHR *ci,
                                                                const VkAllocationCallbacks *, VkAccelerationStructureKHR *out) {
    LOCK;
    *out = VK_NULL_HANDLE;
    if (ci->pNext || ci->createFlags || ci->deviceAddress || ci->type == VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    try {
        auto p = std::make_unique<AS>();
        p->info = *ci;
        p->address = Address(D(h), ci->buffer) + ci->offset;
        p->descriptor = Upload(D(h), &p->address, 8);
        *out = reinterpret_cast<VkAccelerationStructureKHR>(p.get());
        D(h).structures.emplace(*out, std::move(p));
        return VK_SUCCESS;
    } catch (...) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
}
VEL_EXPORT void VKAPI_CALL vkDestroyAccelerationStructureKHR(VkDevice h, VkAccelerationStructureKHR as,
                                                             const VkAllocationCallbacks *) {
    LOCK;
    auto &d = D(h);
    auto it = d.structures.find(as);
    if (it != d.structures.end()) {
        Free(d, it->second->descriptor);
        d.structures.erase(it);
    }
}
VEL_EXPORT VkDeviceAddress VKAPI_CALL
vkGetAccelerationStructureDeviceAddressKHR(VkDevice h, const VkAccelerationStructureDeviceAddressInfoKHR *ci) {
    LOCK;
    return D(h).structures.at(ci->accelerationStructure)->address;
}
namespace {
void BuilderBarrier(Device &d, VkCommandBuffer cb) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    D_FN(d, CmdPipelineBarrier)
    (cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
void BuildDispatch(Device &d, VkCommandBuffer cb, vel::rt::BuildPass pass, const vel::rt::BuildParameters &params,
                   uint32_t groups) {
    D_FN(d, CmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.builders[static_cast<size_t>(pass)]);
    D_FN(d, CmdPushConstants)(cb, d.build_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    D_FN(d, CmdDispatch)(cb, std::max(groups, 1u), 1, 1);
    BuilderBarrier(d, cb);
}
void CopyAS(Device &d, VkCommandBuffer cb, const AS &src, AS &dst) {
    vel::rt::BuildParameters params{};
    params.src = src.address;
    params.dst = dst.address;
    params.count = uint32_t(std::min<uint64_t>(std::min(src.info.size, dst.info.size) / 4, UINT32_MAX));
    BuildDispatch(d, cb, vel::rt::BuildPass::Copy, params, std::min((params.count + uint64_t(255)) / 256, uint64_t(65535)));
}
}  // namespace
VEL_EXPORT void VKAPI_CALL vkCmdCopyAccelerationStructureKHR(VkCommandBuffer cb, const VkCopyAccelerationStructureInfoKHR *info) {
    LOCK;
    auto &d = C(cb);
    auto &command = d.commands.at(cb);
    if (info->pNext || (info->mode != VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR &&
                        info->mode != VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR)) {
        Error(command, "AS copy supports CLONE and the upstream's same-size COMPACT");
        return;
    }
    try {
        CopyAS(d, cb, *d.structures.at(info->src), *d.structures.at(info->dst));
        Restore(d, cb);
    } catch (const std::exception &e) {
        Error(command, e.what());
    }
}
VEL_EXPORT void VKAPI_CALL vkCmdBuildAccelerationStructuresKHR(VkCommandBuffer cb, uint32_t n,
                                                               const VkAccelerationStructureBuildGeometryInfoKHR *infos,
                                                               const VkAccelerationStructureBuildRangeInfoKHR *const *ranges) {
    LOCK;
    auto &d = C(cb);
    auto &command = d.commands.at(cb);
    try {
        for (uint32_t i = 0; i < n; ++i) {
            const auto &ci = infos[i];
            constexpr VkBuildAccelerationStructureFlagsKHR supported =
                VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR |
                VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
            if (ci.pNext || (ci.flags & ~supported) || ci.geometryCount > vel::rt::kMaxASPrimitives ||
                (ci.type != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR &&
                 ci.type != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR) ||
                (ci.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR && ci.geometryCount != 1) ||
                (ci.mode != VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR &&
                 ci.mode != VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR))
                throw std::runtime_error("Unsupported acceleration structure build configuration");
            std::vector<vel::rt::GeometryInput> geometries;
            uint64_t total = 0;
            for (uint32_t j = 0; j < ci.geometryCount; ++j) {
                const auto &g = ci.pGeometries ? ci.pGeometries[j] : *ci.ppGeometries[j];
                const auto &r = ranges[i][j];
                vel::rt::GeometryInput input{};
                input.base = uint32_t(total);
                input.count = r.primitiveCount;
                total += r.primitiveCount;
                if (total > vel::rt::kMaxASPrimitives) throw std::runtime_error("Primitive limit exceeded");
                input.first = r.firstVertex;
                input.offset = r.primitiveOffset;
                input.transform_offset = r.transformOffset;
                input.flags = g.flags;
                if (g.pNext) throw std::runtime_error("Geometry extension chain is not implemented");
                if (ci.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR) {
                    if (g.geometryType != VK_GEOMETRY_TYPE_INSTANCES_KHR || g.geometry.instances.pNext)
                        throw std::runtime_error("TLAS requires instance geometry");
                    input.vertices = g.geometry.instances.data.deviceAddress;
                    input.index_type = g.geometry.instances.arrayOfPointers;
                } else if (g.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
                    const auto &t = g.geometry.triangles;
                    if (t.pNext || t.vertexFormat != VK_FORMAT_R32G32B32_SFLOAT || t.vertexStride % 4 ||
                        t.vertexData.deviceAddress % 4 ||
                        (t.indexType != VK_INDEX_TYPE_UINT32 && t.indexType != VK_INDEX_TYPE_UINT16 &&
                         t.indexType != VK_INDEX_TYPE_NONE_KHR))
                        throw std::runtime_error("Triangle input requires float3 vertices and u16/u32/no indices");
                    input.vertices = t.vertexData.deviceAddress;
                    input.indices = t.indexData.deviceAddress;
                    input.transform = t.transformData.deviceAddress;
                    input.stride = t.vertexStride;
                    input.index_type = t.indexType == VK_INDEX_TYPE_UINT32 ? 0 : t.indexType == VK_INDEX_TYPE_UINT16 ? 1 : 2;
                } else if (g.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
                    if (g.geometry.aabbs.pNext) throw std::runtime_error("AABB extension chain is not implemented");
                    input.vertices = g.geometry.aabbs.data.deviceAddress;
                    input.stride = g.geometry.aabbs.stride;
                    input.geometry_type = 1;
                } else
                    throw std::runtime_error("BLAS requires triangles or AABBs");
                geometries.push_back(input);
            }
            auto &dst = *d.structures.at(ci.dstAccelerationStructure);
            if (dst.info.type != ci.type ||
                dst.info.size < vel::rt::AccelerationStructureSize(uint32_t(total), false, ci.geometryCount))
                throw std::runtime_error("AS backing allocation does not match the build");
            if (!ci.scratchData.deviceAddress || ci.scratchData.deviceAddress % 256)
                throw std::runtime_error("Build scratch address must be nonzero and 256-byte aligned");
            if (geometries.empty()) geometries.push_back({});
            auto input = Upload(d, geometries.data(), geometries.size() * sizeof(geometries[0]));
            command.buffers.push_back(input);
            vel::rt::BuildParameters params{};
            params.dst = dst.address;
            params.scratch = ci.scratchData.deviceAddress;
            params.geometries = Address(d, input.buffer);
            params.count = uint32_t(total);
            params.capacity = vel::rt::SortCapacity(params.count);
            params.geometry_count = ci.geometryCount;
            params.top = ci.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            params.flags = ci.flags;
            params.update = ci.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            uint32_t groups = (params.count + 255) / 256;
            using Pass = vel::rt::BuildPass;
            if (params.update) {
                auto &src = *d.structures.at(ci.srcAccelerationStructure);
                if (src.info.type != ci.type) throw std::runtime_error("Update source AS type mismatch");
                if (ci.srcAccelerationStructure != ci.dstAccelerationStructure) CopyAS(d, cb, src, dst);
                if (params.count) BuildDispatch(d, cb, Pass::Load, params, groups);
            } else {
                BuildDispatch(d, cb, Pass::Init, params, std::max((std::max(params.count * 2, ci.geometryCount) + 255) / 256, 1u));
                if (params.count) BuildDispatch(d, cb, Pass::Load, params, groups);
                BuildDispatch(d, cb, Pass::Reduce, params, 1);
                BuildDispatch(d, cb, Pass::Morton, params, (params.capacity + 255) / 256);
                BuildDispatch(d, cb, Pass::PreSort, params, (params.capacity + 2047) / 2048);
                for (uint32_t k = 4096; k <= params.capacity; k *= 2) {
                    params.k = k;
                    for (uint32_t j = k / 2; j >= 2048; j /= 2) {
                        params.j = j;
                        BuildDispatch(d, cb, Pass::OuterSort, params, (params.capacity / 2 + 1023) / 1024);
                    }
                    BuildDispatch(d, cb, Pass::InnerSort, params, (params.capacity + 2047) / 2048);
                }
                BuildDispatch(d, cb, Pass::Reorder, params, groups);
                BuildDispatch(d, cb, Pass::Hierarchy, params, groups);
            }
            // Treelets operate on the original Karras child orientation. The
            // final fit alone puts the smaller subtree first for traversal.
            if (!params.update && !params.top && params.count >= 7 &&
                !(ci.flags & VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR)) {
                params.j = 1;
                BuildDispatch(d, cb, Pass::ClearFit, params, groups);
                BuildDispatch(d, cb, Pass::Fit, params, groups);
                uint32_t passes = (ci.flags & VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) ? 3 : 1;
                for (uint32_t pass = 0, threshold = 7; pass < passes && threshold <= params.count; ++pass, threshold *= 2) {
                    params.k = threshold;
                    BuildDispatch(d, cb, Pass::ClearTreelets, params, groups);
                    BuildDispatch(d, cb, Pass::FindTreelets, params, groups);
                    BuildDispatch(d, cb, Pass::ReorderTreelets, params, params.count / threshold);
                }
            }
            params.j = 0;
            BuildDispatch(d, cb, Pass::ClearFit, params, groups);
            BuildDispatch(d, cb, Pass::Fit, params, groups);
            Restore(d, cb);
        }
    } catch (const std::exception &e) {
        Error(command, e.what());
    }
}

VEL_EXPORT VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice h, const VkCommandBufferAllocateInfo *ci, VkCommandBuffer *out) {
    LOCK;
    auto r = D_FN(D(h), AllocateCommandBuffers)(h, ci, out);
    if (r == VK_SUCCESS)
        for (uint32_t i = 0; i < ci->commandBufferCount; i++) {
            command_devices[out[i]] = h;
            D(h).commands[out[i]] = {};
            D(h).command_pools[out[i]] = ci->commandPool;
        }
    return r;
}
VEL_EXPORT void VKAPI_CALL vkFreeCommandBuffers(VkDevice h, VkCommandPool pool, uint32_t n, const VkCommandBuffer *in) {
    LOCK;
    auto &d = D(h);
    for (uint32_t i = 0; i < n; i++) {
        Clear(d, d.commands.at(in[i]));
        d.commands.erase(in[i]);
        d.command_pools.erase(in[i]);
        command_devices.erase(in[i]);
    }
    D_FN(d, FreeCommandBuffers)(h, pool, n, in);
}
VEL_EXPORT VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *ci) {
    LOCK;
    auto &d = C(cb);
    auto r = D_FN(d, BeginCommandBuffer)(cb, ci);
    if (r == VK_SUCCESS) Clear(d, d.commands.at(cb));
    return r;
}
VEL_EXPORT VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cb) {
    LOCK;
    auto &d = C(cb);
    auto r = D_FN(d, EndCommandBuffer)(cb);
    return d.commands.at(cb).error ? VK_ERROR_VALIDATION_FAILED_EXT : r;
}
VEL_EXPORT VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    LOCK;
    auto &d = C(cb);
    auto r = D_FN(d, ResetCommandBuffer)(cb, flags);
    if (r == VK_SUCCESS) Clear(d, d.commands.at(cb));
    return r;
}
VEL_EXPORT VkResult VKAPI_CALL vkResetCommandPool(VkDevice h, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    LOCK;
    auto &d = D(h);
    auto r = D_FN(d, ResetCommandPool)(h, pool, flags);
    if (r == VK_SUCCESS)
        for (auto &[cb, p] : d.command_pools)
            if (p == pool) Clear(d, d.commands.at(cb));
    return r;
}
VEL_EXPORT void VKAPI_CALL vkDestroyCommandPool(VkDevice h, VkCommandPool pool, const VkAllocationCallbacks *a) {
    LOCK;
    auto &d = D(h);
    for (auto it = d.command_pools.begin(); it != d.command_pools.end();) {
        if (it->second == pool) {
            Clear(d, d.commands.at(it->first));
            d.commands.erase(it->first);
            command_devices.erase(it->first);
            it = d.command_pools.erase(it);
        } else
            ++it;
    }
    D_FN(d, DestroyCommandPool)(h, pool, a);
}
VEL_EXPORT void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline p) {
    LOCK;
    auto &d = C(cb);
    auto &c = d.commands.at(cb);
    if (point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
        c.rt = p;
        return;
    }
    if (point == VK_PIPELINE_BIND_POINT_COMPUTE) c.compute = p;
    D_FN(d, CmdBindPipeline)(cb, point, p);
}
VEL_EXPORT void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout,
                                                   uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t no,
                                                   const uint32_t *offsets) {
    LOCK;
    auto &d = C(cb);
    Bind b{layout, first, {sets, sets + n}, {}};
    if (no) b.offsets.assign(offsets, offsets + no);
    if (point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
        d.commands.at(cb).rt_binds.push_back(std::move(b));
        return;
    }
    if (point == VK_PIPELINE_BIND_POINT_COMPUTE) d.commands.at(cb).compute_binds.push_back(std::move(b));
    D_FN(d, CmdBindDescriptorSets)
    (cb, point, layout, first, n, sets, no, offsets);
}
VEL_EXPORT void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
                                              uint32_t offset, uint32_t size, const void *data) {
    LOCK;
    auto &d = C(cb);
    Push p{layout, stages, offset, {static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size}};
    if (stages & kRT) d.commands.at(cb).rt_push.push_back(p);
    if (stages & VK_SHADER_STAGE_COMPUTE_BIT) d.commands.at(cb).compute_push.push_back(p);
    if (stages & ~kRT) D_FN(d, CmdPushConstants)(cb, layout, stages & ~kRT, offset, size, data);
}
VEL_EXPORT void VKAPI_CALL vkCmdTraceRaysKHR(VkCommandBuffer cb, const VkStridedDeviceAddressRegionKHR *raygen,
                                             const VkStridedDeviceAddressRegionKHR *miss,
                                             const VkStridedDeviceAddressRegionKHR *hit,
                                             const VkStridedDeviceAddressRegionKHR *callable, uint32_t w, uint32_t h, uint32_t z) {
    LOCK;
    auto &d = C(cb);
    auto &c = d.commands.at(cb);
    try {
        if (callable->size || !c.rt || uint64_t(w) * h * z > (1u << 24)) throw std::runtime_error("Unsupported trace dispatch");
        auto &p = *d.pipelines.at(c.rt);
        struct Dispatch {
            uint64_t rg, miss, hit, ms, hs;
            uint32_t pad[2];
            uint32_t dims[4];
        } data{raygen->deviceAddress, miss->deviceAddress, hit->deviceAddress, miss->stride, hit->stride, {0, 0}, {w, h, z, 0}};
        c.buffers.push_back(Upload(d, &data, sizeof(data)));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &size;
        VkDescriptorPool pool;
        Check(D_FN(d, CreateDescriptorPool)(d.handle, &pci, nullptr, &pool));
        c.pools.push_back(pool);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &p.runtime;
        VkDescriptorSet set;
        Check(D_FN(d, AllocateDescriptorSets)(d.handle, &ai, &set));
        VkDescriptorBufferInfo bi{c.buffers.back().buffer, 0, sizeof(data)};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = set;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr.pBufferInfo = &bi;
        D_FN(d, UpdateDescriptorSets)(d.handle, 1, &wr, 0, nullptr);
        D_FN(d, CmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.compute);
        Replay(d, cb, c.rt_binds, c.rt_push, p.layout);
        D_FN(d, CmdBindDescriptorSets)
        (cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, p.sets, 1, &set, 0, nullptr);
        D_FN(d, CmdDispatch)(cb, (w + 7) / 8, (h + 7) / 8, z);
        Restore(d, cb);
    } catch (const std::exception &e) {
        Error(c, e.what());
    }
}

VEL_EXPORT void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                                VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *mem, uint32_t nb,
                                                const VkBufferMemoryBarrier *buf, uint32_t ni, const VkImageMemoryBarrier *img) {
    LOCK;
    std::vector<VkMemoryBarrier> m;
    std::vector<VkBufferMemoryBarrier> b;
    std::vector<VkImageMemoryBarrier> im;
    if (nm) m.assign(mem, mem + nm);
    if (nb) b.assign(buf, buf + nb);
    if (ni) im.assign(img, img + ni);
    for (auto &x : m) {
        x.srcAccessMask = VkAccessFlags(Access2(x.srcAccessMask));
        x.dstAccessMask = VkAccessFlags(Access2(x.dstAccessMask));
    }
    for (auto &x : b) {
        x.srcAccessMask = VkAccessFlags(Access2(x.srcAccessMask));
        x.dstAccessMask = VkAccessFlags(Access2(x.dstAccessMask));
    }
    for (auto &x : im) {
        x.srcAccessMask = VkAccessFlags(Access2(x.srcAccessMask));
        x.dstAccessMask = VkAccessFlags(Access2(x.dstAccessMask));
    }
    D_FN(C(cb), CmdPipelineBarrier)
    (cb, VkPipelineStageFlags(Stages2(src)), VkPipelineStageFlags(Stages2(dst)), flags, nm, m.data(), nb, b.data(), ni, im.data());
}
VEL_EXPORT void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *in) {
    LOCK;
    auto info = *in;
    std::vector<VkMemoryBarrier2> m;
    std::vector<VkBufferMemoryBarrier2> b;
    std::vector<VkImageMemoryBarrier2> im;
    if (in->memoryBarrierCount) m.assign(in->pMemoryBarriers, in->pMemoryBarriers + in->memoryBarrierCount);
    if (in->bufferMemoryBarrierCount) b.assign(in->pBufferMemoryBarriers, in->pBufferMemoryBarriers + in->bufferMemoryBarrierCount);
    if (in->imageMemoryBarrierCount) im.assign(in->pImageMemoryBarriers, in->pImageMemoryBarriers + in->imageMemoryBarrierCount);
    auto fix = [](auto &list) {
        for (auto &x : list) {
            x.srcStageMask = Stages2(x.srcStageMask);
            x.dstStageMask = Stages2(x.dstStageMask);
            x.srcAccessMask = Access2(x.srcAccessMask);
            x.dstAccessMask = Access2(x.dstAccessMask);
        }
    };
    fix(m);
    fix(b);
    fix(im);
    info.pMemoryBarriers = m.data();
    info.pBufferMemoryBarriers = b.data();
    info.pImageMemoryBarriers = im.data();
    auto f = D_FN(C(cb), CmdPipelineBarrier2);
    if (!f) f = C(cb).fn<PFN_vkCmdPipelineBarrier2>("vkCmdPipelineBarrier2KHR");
    f(cb, &info);
}

namespace {
PFN_vkVoidFunction Intercept(const char *n) {
#define ENTRY(name) \
    if (!std::strcmp(n, "vk" #name)) return reinterpret_cast<PFN_vkVoidFunction>(vk##name)
    ENTRY(GetInstanceProcAddr);
    ENTRY(GetDeviceProcAddr);
    ENTRY(CreateInstance);
    ENTRY(DestroyInstance);
    ENTRY(EnumeratePhysicalDevices);
    ENTRY(EnumeratePhysicalDeviceGroups);
    ENTRY(EnumerateDeviceExtensionProperties);
    ENTRY(GetPhysicalDeviceFeatures2);
    ENTRY(GetPhysicalDeviceProperties2);
    if (!std::strcmp(n, "vkEnumeratePhysicalDeviceGroupsKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDeviceGroups);
    if (!std::strcmp(n, "vkGetPhysicalDeviceFeatures2KHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceFeatures2);
    if (!std::strcmp(n, "vkGetPhysicalDeviceProperties2KHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties2);
    ENTRY(CreateDevice);
    ENTRY(DestroyDevice);
    ENTRY(GetDeviceQueue);
    ENTRY(GetDeviceQueue2);
    ENTRY(QueueSubmit);
    ENTRY(QueueSubmit2);
    if (!std::strcmp(n, "vkQueueSubmit2KHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2);
    ENTRY(CreateBuffer);
    ENTRY(CreateShaderModule);
    ENTRY(DestroyShaderModule);
    ENTRY(CreateDescriptorSetLayout);
    ENTRY(CreateDescriptorPool);
    ENTRY(UpdateDescriptorSets);
    ENTRY(CreatePipelineLayout);
    ENTRY(DestroyPipelineLayout);
    ENTRY(CreateRayTracingPipelinesKHR);
    ENTRY(DestroyPipeline);
    ENTRY(GetRayTracingShaderGroupHandlesKHR);
    ENTRY(GetRayTracingShaderGroupStackSizeKHR);
    ENTRY(CmdSetRayTracingPipelineStackSizeKHR);
    ENTRY(CreateDeferredOperationKHR);
    ENTRY(DestroyDeferredOperationKHR);
    ENTRY(GetDeferredOperationMaxConcurrencyKHR);
    ENTRY(GetDeferredOperationResultKHR);
    ENTRY(DeferredOperationJoinKHR);
    ENTRY(GetAccelerationStructureBuildSizesKHR);
    ENTRY(CreateAccelerationStructureKHR);
    ENTRY(DestroyAccelerationStructureKHR);
    ENTRY(GetAccelerationStructureDeviceAddressKHR);
    ENTRY(CmdBuildAccelerationStructuresKHR);
    ENTRY(CmdCopyAccelerationStructureKHR);
    ENTRY(AllocateCommandBuffers);
    ENTRY(FreeCommandBuffers);
    ENTRY(BeginCommandBuffer);
    ENTRY(EndCommandBuffer);
    ENTRY(ResetCommandBuffer);
    ENTRY(ResetCommandPool);
    ENTRY(DestroyCommandPool);
    ENTRY(CmdBindPipeline);
    ENTRY(CmdBindDescriptorSets);
    ENTRY(CmdPushConstants);
    ENTRY(CmdTraceRaysKHR);
    ENTRY(CmdPipelineBarrier);
    ENTRY(CmdPipelineBarrier2);
    if (!std::strcmp(n, "vkCmdPipelineBarrier2KHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier2);
#undef ENTRY
    return nullptr;
}
PFN_vkVoidFunction VKAPI_CALL PhysicalProc(VkInstance i, const char *n) {
    LOCK;
    if (auto f = Intercept(n)) return f;
    if (UnsupportedRT(n)) return nullptr;
    auto &data = instances.at(i);
    return data.gpdpa ? data.gpdpa(i, n) : nullptr;
}
}  // namespace
VEL_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i, const char *n) {
    LOCK;
    if (auto f = Intercept(n)) return f;
    if (UnsupportedRT(n)) return nullptr;
    return i ? instances.at(i).gipa(i, n) : nullptr;
}
VEL_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice h, const char *n) {
    LOCK;
    if (auto f = Intercept(n)) return f;
    // Never send emulated KHR RT commands to a driver that might support a
    // different AS representation.
    if (UnsupportedRT(n)) return nullptr;
    return D(h).gdpa(h, n);
}
VEL_EXPORT VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *p) {
    if (p->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    p->loaderLayerInterfaceVersion = 2;
    p->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    p->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    p->pfnGetPhysicalDeviceProcAddr = PhysicalProc;
    return VK_SUCCESS;
}
