// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Development loader adapter. Emulation is opt-in and does not add advertised
// extensions until the complete consumer contract is implemented.
#include <vulkan/vk_layer.h>
#include <vulkan/utility/vk_safe_struct.hpp>
#include <vulkan/utility/vk_struct_helper.hpp>
#include "omm_command_state.h"
#include "omm_device.h"
#include "omm_sync.h"
#include "omm_kernels.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace vel::omm::layer {
constexpr char name[] = "VK_LAYER_VEL_opacity_micromap";
struct Instance {
    VkInstance handle{};
    PFN_vkGetInstanceProcAddr next{};
    PFN_GetPhysicalDeviceProcAddr physical_next{};
    bool experimental_software = false;
    uint32_t api_version = VK_API_VERSION_1_0;
};
struct Command {
    VkCommandPool pool{};
    CommandState state;
};
struct LayoutEntry {
    PushLayoutRef snapshot;
    uint64_t creations = 1;
};
struct Device {
    VkDevice handle{};
    PFN_vkGetDeviceProcAddr next{};
    DeviceBackend* backend{};
    bool software = false;
    VkAllocationCallbacks allocator{};
    bool has_allocator = false;
    std::mutex mutex;
    std::unordered_map<VkCommandBuffer, std::shared_ptr<Command>> commands;
    std::unordered_map<VkPipelineLayout, LayoutEntry> layouts;
    const VkAllocationCallbacks* allocation() const { return has_allocator ? &allocator : nullptr; }
    void finish() {
        commands.clear();
        layouts.clear();
        device_backend_destroy(backend);
        backend = nullptr;
    }
};
std::mutex registry_mutex;
std::unordered_map<uintptr_t, std::shared_ptr<Instance>> instances;
std::unordered_map<uintptr_t, std::shared_ptr<Device>> devices;
template <typename Handle>
uintptr_t key(Handle handle) {
    return handle ? *reinterpret_cast<const uintptr_t*>(handle) : 0;
}
template <typename Handle>
std::shared_ptr<Instance> instance(Handle handle) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found = instances.find(key(handle));
    return found == instances.end() ? nullptr : found->second;
}
template <typename Handle>
std::shared_ptr<Device> device(Handle handle) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found = devices.find(key(handle));
    return found == devices.end() ? nullptr : found->second;
}
std::shared_ptr<Command> command(Device& dev, VkCommandBuffer handle) {
    std::lock_guard<std::mutex> lock(dev.mutex);
    const auto found = dev.commands.find(handle);
    return found == dev.commands.end() ? nullptr : found->second;
}
template <typename Fn>
Fn native(const Device& dev, const char* symbol) {
    return reinterpret_cast<Fn>(dev.next(dev.handle, symbol));
}
template <typename T>
T* mutable_next(const void* chain) {
    return const_cast<T*>(vku::FindStructInPNextChain<T>(chain));
}
bool experimental() {
    const char* value = std::getenv("VK_VEL_OMM_EXPERIMENTAL_SOFTWARE");
    return value && !std::strcmp(value, "1");
}
bool enabled(const VkDeviceCreateInfo& info, const char* extension) {
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
        if (!std::strcmp(info.ppEnabledExtensionNames[i], extension)) return true;
    return false;
}
NativeComputeDispatch compute_dispatch(const Device& dev) {
    return {native<PFN_vkCmdBindPipeline>(dev, "vkCmdBindPipeline"), native<PFN_vkCmdBindShadersEXT>(dev, "vkCmdBindShadersEXT"),
            native<PFN_vkCmdPushConstants>(dev, "vkCmdPushConstants")};
}
void finish_internal(Device& dev, VkCommandBuffer cb, VkResult result) {
    auto state = command(dev, cb);
    if (!state) return;
    state->state.fail(result);
    if (result == VK_SUCCESS) state->state.restore(cb, compute_dispatch(dev));
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                              VkInstance* out) {
    auto* link = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(info->pNext));
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || link->function != VK_LAYER_LINK_INFO))
        link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto physical = link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE, "vkCreateInstance"));
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    try {
        auto state = std::make_shared<Instance>();
        VkResult result = create(info, allocator, out);
        if (result != VK_SUCCESS) return result;
        state->handle = *out;
        state->next = next;
        state->physical_next = physical;
        state->experimental_software = experimental();
        if (info->pApplicationInfo && info->pApplicationInfo->apiVersion) state->api_version = info->pApplicationInfo->apiVersion;
        try {
            std::lock_guard<std::mutex> lock(registry_mutex);
            instances.emplace(key(*out), state);
        } catch (...) {
            reinterpret_cast<PFN_vkDestroyInstance>(next(*out, "vkDestroyInstance"))(*out, allocator);
            *out = VK_NULL_HANDLE;
            throw;
        }
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance handle, const VkAllocationCallbacks* allocator) {
    auto state = instance(handle);
    if (!state) return;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        instances.erase(key(handle));
    }
    reinterpret_cast<PFN_vkDestroyInstance>(state->next(handle, "vkDestroyInstance"))(handle, allocator);
}
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2* features) {
    auto state = instance(physical);
    auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(state->next(state->handle, "vkGetPhysicalDeviceFeatures2"));
    if (!get)
        get = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(state->next(state->handle, "vkGetPhysicalDeviceFeatures2KHR"));
    get(physical, features);
    if (state->experimental_software) {
        auto* micromap = mutable_next<VkPhysicalDeviceOpacityMicromapFeaturesEXT>(features->pNext);
        if (micromap) micromap->micromapCaptureReplay = micromap->micromapHostCommands = VK_FALSE;
#ifdef VK_NV_command_buffer_inheritance
        if (auto* inheritance = mutable_next<VkPhysicalDeviceCommandBufferInheritanceFeaturesNV>(features->pNext))
            inheritance->commandBufferInheritance = VK_FALSE;
#endif
    }
}
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2* properties) {
    auto state = instance(physical);
    auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(state->next(state->handle, "vkGetPhysicalDeviceProperties2"));
    if (!get)
        get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(state->next(state->handle, "vkGetPhysicalDeviceProperties2KHR"));
    get(physical, properties);
    if (state->experimental_software)
        if (auto* micromap = mutable_next<VkPhysicalDeviceOpacityMicromapPropertiesEXT>(properties->pNext)) {
            micromap->maxOpacity2StateSubdivisionLevel = std::min(micromap->maxOpacity2StateSubdivisionLevel, 12u);
            micromap->maxOpacity4StateSubdivisionLevel = std::min(micromap->maxOpacity4StateSubdivisionLevel, 12u);
        }
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks* allocator, VkDevice* out) {
    auto parent = instance(physical);
    auto* link = const_cast<VkLayerDeviceCreateInfo*>(reinterpret_cast<const VkLayerDeviceCreateInfo*>(info->pNext));
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || link->function != VK_LAYER_LINK_INFO))
        link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
    if (!parent || !link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto create =
        reinterpret_cast<PFN_vkCreateDevice>(link->u.pLayerInfo->pfnNextGetInstanceProcAddr(parent->handle, "vkCreateDevice"));
    auto next = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    try {
        auto state = std::make_shared<Device>();
        state->next = next;
        state->software = parent->experimental_software && enabled(*info, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
        state->has_allocator = allocator != nullptr;
        if (allocator) state->allocator = *allocator;
        VkResult result;
        if (state->software) {
            // Current embedded kernels target Vulkan 1.2/SPIR-V 1.5. The lower
            // extension-only environment still needs separate qualification.
            VkPhysicalDeviceProperties properties{};
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(parent->next(parent->handle, "vkGetPhysicalDeviceProperties"))(
                physical, &properties);
            if (parent->api_version < VK_API_VERSION_1_2 || properties.apiVersion < VK_API_VERSION_1_2)
                return VK_ERROR_FEATURE_NOT_PRESENT;
#ifdef VK_NV_command_buffer_inheritance
            const auto* inheritance = vku::FindStructInPNextChain<VkPhysicalDeviceCommandBufferInheritanceFeaturesNV>(info->pNext);
            if (inheritance && inheritance->commandBufferInheritance) return VK_ERROR_FEATURE_NOT_PRESENT;
#endif
            const auto* micromap = vku::FindStructInPNextChain<VkPhysicalDeviceOpacityMicromapFeaturesEXT>(info->pNext);
            const auto* rq = vku::FindStructInPNextChain<VkPhysicalDeviceRayQueryFeaturesKHR>(info->pNext);
            const auto* rtp = vku::FindStructInPNextChain<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(info->pNext);
            // Consumer registration is deliberately fail-closed during this
            // device-API checkpoint. No missing emulated feature is advertised.
            if ((micromap && (micromap->micromapCaptureReplay || micromap->micromapHostCommands)) || (rq && rq->rayQuery) ||
                (rtp && rtp->rayTracingPipeline))
                return VK_ERROR_FEATURE_NOT_PRESENT;
            VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &bda};
            auto get =
                reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(parent->next(parent->handle, "vkGetPhysicalDeviceFeatures2"));
            if (!get)
                get = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                    parent->next(parent->handle, "vkGetPhysicalDeviceFeatures2KHR"));
            if (!get) return VK_ERROR_FEATURE_NOT_PRESENT;
            get(physical, &features);
            if (!bda.bufferDeviceAddress || !features.features.shaderInt64) return VK_ERROR_FEATURE_NOT_PRESENT;
            vku::safe_VkDeviceCreateInfo local(info);
            vku::RemoveExtension(local, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
            vku::RemoveFromPnext(local, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT);
            auto* core = mutable_next<VkPhysicalDeviceFeatures2>(local.pNext);
            if (core)
                core->features.shaderInt64 = VK_TRUE;
            else {
                if (!local.pEnabledFeatures) local.pEnabledFeatures = new VkPhysicalDeviceFeatures{};
                const_cast<VkPhysicalDeviceFeatures*>(local.pEnabledFeatures)->shaderInt64 = VK_TRUE;
            }
            auto* twelve = mutable_next<VkPhysicalDeviceVulkan12Features>(local.pNext);
            auto* address = mutable_next<VkPhysicalDeviceBufferDeviceAddressFeatures>(local.pNext);
            if (twelve)
                twelve->bufferDeviceAddress = VK_TRUE;
            else if (address)
                address->bufferDeviceAddress = VK_TRUE;
            else {
                bda.bufferDeviceAddress = VK_TRUE;
                bda.bufferDeviceAddressCaptureReplay = VK_FALSE;
                bda.bufferDeviceAddressMultiDevice = VK_FALSE;
                bda.pNext = nullptr;
                vku::AddToPnext(local, bda);
            }
            result = create(physical, local.ptr(), allocator, out);
        } else
            result = create(physical, info, allocator, out);
        if (result != VK_SUCCESS) return result;
        state->handle = *out;
        auto destroy = native<PFN_vkDestroyDevice>(*state, "vkDestroyDevice");
        try {
            if (state->software) {
                const DeviceBackendCreateInfo backend{physical,
                                                      *out,
                                                      next,
                                                      state->allocation(),
                                                      vel_omm_build_spv,
                                                      sizeof(vel_omm_build_spv),
                                                      vel_omm_copy_spv,
                                                      sizeof(vel_omm_copy_spv)};
                result = device_backend_create(backend, &state->backend);
                if (result != VK_SUCCESS) {
                    destroy(*out, allocator);
                    *out = VK_NULL_HANDLE;
                    return result;
                }
            }
            std::lock_guard<std::mutex> lock(registry_mutex);
            devices.emplace(key(*out), state);
        } catch (...) {
            state->finish();
            destroy(*out, allocator);
            *out = VK_NULL_HANDLE;
            throw;
        }
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice handle, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (!state) return;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        devices.erase(key(handle));
    }
    state->finish();
    native<PFN_vkDestroyDevice>(*state, "vkDestroyDevice")(handle, allocator);
}

constexpr VkBufferUsageFlags2 micromap_usage =
    VK_BUFFER_USAGE_2_MICROMAP_STORAGE_BIT_EXT | VK_BUFFER_USAGE_2_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT;
VkBufferUsageFlags2 effective_usage(const VkBufferCreateInfo& info) {
    const auto* flags = vku::FindStructInPNextChain<VkBufferUsageFlags2CreateInfo>(info.pNext);
    return flags ? flags->usage : info.usage;
}
bool translate_buffer(vku::safe_VkBufferCreateInfo& local) {
    auto usage = effective_usage(*local.ptr());
    if (!(usage & micromap_usage)) return true;
    if (local.size > UINT64_MAX - 3) return false;
    local.size = (local.size + 3) & ~VkDeviceSize(3);
    usage = (usage & ~micromap_usage) | VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT;
    local.usage = VkBufferUsageFlags(usage);
    if (auto* flags = mutable_next<VkBufferUsageFlags2CreateInfo>(local.pNext)) flags->usage = usage;
    return true;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice handle, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator,
                                            VkBuffer* out) {
    auto state = device(handle);
    auto create = native<PFN_vkCreateBuffer>(*state, "vkCreateBuffer");
    if (!state->software) return create(handle, info, allocator, out);
    try {
        vku::safe_VkBufferCreateInfo local(info);
        if (!translate_buffer(local)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkResult result = create(handle, local.ptr(), allocator, out);
        if (result != VK_SUCCESS) return result;
        VkBufferCreateInfo original = *info;
        original.usage = VkBufferUsageFlags(effective_usage(*info));
        result = device_track_buffer(state->backend, *out, &original, local.size);
        if (result != VK_SUCCESS) {
            native<PFN_vkDestroyBuffer>(*state, "vkDestroyBuffer")(handle, *out, allocator);
            *out = VK_NULL_HANDLE;
        }
        return result;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice handle, VkBuffer buffer, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->software) device_untrack_buffer(state->backend, buffer);
    native<PFN_vkDestroyBuffer>(*state, "vkDestroyBuffer")(handle, buffer, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice handle, const VkMemoryAllocateInfo* info,
                                              const VkAllocationCallbacks* allocator, VkDeviceMemory* out) {
    auto state = device(handle);
    auto allocate = native<PFN_vkAllocateMemory>(*state, "vkAllocateMemory");
    if (!state->software) return allocate(handle, info, allocator, out);
    try {
        vku::safe_VkMemoryAllocateInfo local(info);
        auto* flags = mutable_next<VkMemoryAllocateFlagsInfo>(local.pNext);
        if (flags)
            flags->flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        else {
            VkMemoryAllocateFlagsInfo extra{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr,
                                            VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
            vku::AddToPnext(local, extra);
        }
        return allocate(handle, local.ptr(), allocator, out);
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice handle, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    auto state = device(handle);
    VkResult result = native<PFN_vkBindBufferMemory>(*state, "vkBindBufferMemory")(handle, buffer, memory, offset);
    if (result == VK_SUCCESS && state->software) device_buffer_bound(state->backend, buffer);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice handle, uint32_t count, const VkBindBufferMemoryInfo* infos) {
    auto state = device(handle);
    auto bind = native<PFN_vkBindBufferMemory2>(*state, "vkBindBufferMemory2");
    if (!bind) bind = native<PFN_vkBindBufferMemory2>(*state, "vkBindBufferMemory2KHR");
    VkResult result = bind(handle, count, infos);
    if (state->software)
        for (uint32_t i = 0; i < count; ++i) {
            const auto* status = vku::FindStructInPNextChain<VkBindMemoryStatus>(infos[i].pNext);
            if (result == VK_SUCCESS || (status && status->pResult && *status->pResult == VK_SUCCESS))
                device_buffer_bound(state->backend, infos[i].buffer);
        }
    return result;
}
VKAPI_ATTR void VKAPI_CALL GetDeviceBufferMemoryRequirements(VkDevice handle, const VkDeviceBufferMemoryRequirements* info,
                                                             VkMemoryRequirements2* out) {
    auto state = device(handle);
    auto get = native<PFN_vkGetDeviceBufferMemoryRequirements>(*state, "vkGetDeviceBufferMemoryRequirements");
    if (!get) get = native<PFN_vkGetDeviceBufferMemoryRequirements>(*state, "vkGetDeviceBufferMemoryRequirementsKHR");
    if (!state->software) {
        get(handle, info, out);
        return;
    }
    try {
        vku::safe_VkBufferCreateInfo buffer(info->pCreateInfo);
        if (!translate_buffer(buffer)) {
            out->memoryRequirements = {};
            return;
        }
        VkDeviceBufferMemoryRequirements local = *info;
        local.pCreateInfo = buffer.ptr();
        get(handle, &local, out);
    } catch (const std::bad_alloc&) {
        out->memoryRequirements = {};
    }
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice handle, const VkCommandBufferAllocateInfo* info,
                                                      VkCommandBuffer* out) {
    auto state = device(handle);
    auto allocate = native<PFN_vkAllocateCommandBuffers>(*state, "vkAllocateCommandBuffers");
    VkResult result = allocate(handle, info, out);
    if (result != VK_SUCCESS || !state->software) return result;
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
            auto entry = std::make_shared<Command>();
            entry->pool = info->commandPool;
            state->commands.emplace(out[i], std::move(entry));
        }
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            for (uint32_t i = 0; i < info->commandBufferCount; ++i) state->commands.erase(out[i]);
        }
        native<PFN_vkFreeCommandBuffers>(*state, "vkFreeCommandBuffers")(handle, info->commandPool, info->commandBufferCount, out);
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice handle, VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers) {
    auto state = device(handle);
    if (state->software) {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (uint32_t i = 0; i < count; ++i) state->commands.erase(buffers[i]);
    }
    native<PFN_vkFreeCommandBuffers>(*state, "vkFreeCommandBuffers")(handle, pool, count, buffers);
}
VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice handle, VkCommandPool pool, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->software) {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (auto i = state->commands.begin(); i != state->commands.end();) {
            if (i->second->pool == pool)
                i = state->commands.erase(i);
            else
                ++i;
        }
    }
    native<PFN_vkDestroyCommandPool>(*state, "vkDestroyCommandPool")(handle, pool, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice handle, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    auto state = device(handle);
    VkResult result = native<PFN_vkResetCommandPool>(*state, "vkResetCommandPool")(handle, pool, flags);
    if (result == VK_SUCCESS && state->software) {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (const auto& pair : state->commands)
            if (pair.second->pool == pool) pair.second->state.reset();
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
    auto state = device(cb);
    VkResult result = native<PFN_vkBeginCommandBuffer>(*state, "vkBeginCommandBuffer")(cb, info);
    if (result == VK_SUCCESS && state->software)
        if (auto entry = command(*state, cb)) entry->state.begin();
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    auto state = device(cb);
    VkResult result = native<PFN_vkResetCommandBuffer>(*state, "vkResetCommandBuffer")(cb, flags);
    if (result == VK_SUCCESS && state->software)
        if (auto entry = command(*state, cb)) entry->state.reset();
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer cb) {
    auto state = device(cb);
    VkResult result = native<PFN_vkEndCommandBuffer>(*state, "vkEndCommandBuffer")(cb);
    if (result == VK_SUCCESS && state->software)
        if (auto entry = command(*state, cb)) return entry->state.status();
    return result;
}
VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    auto state = device(cb);
    native<PFN_vkCmdBindPipeline>(*state, "vkCmdBindPipeline")(cb, point, pipeline);
    if (state->software)
        if (auto entry = command(*state, cb)) entry->state.bind_pipeline(point, pipeline);
}
VKAPI_ATTR void VKAPI_CALL CmdBindShadersEXT(VkCommandBuffer cb, uint32_t count, const VkShaderStageFlagBits* stages,
                                             const VkShaderEXT* shaders) {
    auto state = device(cb);
    native<PFN_vkCmdBindShadersEXT>(*state, "vkCmdBindShadersEXT")(cb, count, stages, shaders);
    if (state->software)
        if (auto entry = command(*state, cb)) entry->state.bind_shaders(count, stages, shaders);
}
VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice handle, const VkPipelineLayoutCreateInfo* info,
                                                    const VkAllocationCallbacks* allocator, VkPipelineLayout* out) {
    auto state = device(handle);
    auto create = native<PFN_vkCreatePipelineLayout>(*state, "vkCreatePipelineLayout");
    VkResult result = create(handle, info, allocator, out);
    if (result != VK_SUCCESS || !state->software) return result;
    VkPipelineLayout snapshot{};
    VkPipelineLayoutCreateInfo private_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    private_info.pushConstantRangeCount = info->pushConstantRangeCount;
    private_info.pPushConstantRanges = info->pPushConstantRanges;
    auto destroy = native<PFN_vkDestroyPipelineLayout>(*state, "vkDestroyPipelineLayout");
    result = create(handle, &private_info, state->allocation(), &snapshot);
    if (result != VK_SUCCESS) {
        destroy(handle, *out, allocator);
        *out = VK_NULL_HANDLE;
        return result;
    }
    try {
        const auto allocation = state->allocator;
        const bool has_allocation = state->has_allocator;
        auto* owned_snapshot = new PushLayoutSnapshot{snapshot};
        snapshot = VK_NULL_HANDLE;
        PushLayoutRef token(owned_snapshot, [handle, destroy, allocation, has_allocation](const auto* owned) {
            destroy(handle, owned->native_layout, has_allocation ? &allocation : nullptr);
            delete owned;
        });
        std::lock_guard<std::mutex> lock(state->mutex);
        auto found = state->layouts.find(*out);
        if (found == state->layouts.end())
            state->layouts.emplace(*out, LayoutEntry{std::move(token), 1});
        else {
            if (found->second.creations == UINT64_MAX) throw std::bad_alloc();
            ++found->second.creations;
        }
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        if (snapshot) destroy(handle, snapshot, state->allocation());
        destroy(handle, *out, allocator);
        *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(VkDevice handle, VkPipelineLayout layout, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->software) {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto found = state->layouts.find(layout);
        if (found != state->layouts.end() && --found->second.creations == 0) state->layouts.erase(found);
    }
    native<PFN_vkDestroyPipelineLayout>(*state, "vkDestroyPipelineLayout")(handle, layout, allocator);
}
VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                                            uint32_t size, const void* values) {
    auto state = device(cb);
    if (state->software)
        if (auto entry = command(*state, cb)) {
            PushLayoutRef snapshot;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                auto found = state->layouts.find(layout);
                if (found != state->layouts.end()) snapshot = found->second.snapshot;
            }
            if (entry->state.push(std::move(snapshot), stages, offset, size, values) != VK_SUCCESS) return;
        }
    native<PFN_vkCmdPushConstants>(*state, "vkCmdPushConstants")(cb, layout, stages, offset, size, values);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateMicromapEXT(VkDevice handle, const VkMicromapCreateInfoEXT* info,
                                                 const VkAllocationCallbacks* allocator, VkMicromapEXT* out) {
    auto state = device(handle);
    return state->software ? device_create_micromap(state->backend, info, allocator, out)
                           : native<PFN_vkCreateMicromapEXT>(*state, "vkCreateMicromapEXT")(handle, info, allocator, out);
}
VKAPI_ATTR void VKAPI_CALL DestroyMicromapEXT(VkDevice handle, VkMicromapEXT micromap, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->software)
        device_destroy_micromap(state->backend, micromap, allocator);
    else
        native<PFN_vkDestroyMicromapEXT>(*state, "vkDestroyMicromapEXT")(handle, micromap, allocator);
}
VKAPI_ATTR void VKAPI_CALL GetMicromapBuildSizesEXT(VkDevice handle, VkAccelerationStructureBuildTypeKHR type,
                                                    const VkMicromapBuildInfoEXT* info, VkMicromapBuildSizesInfoEXT* sizes) {
    auto state = device(handle);
    if (state->software) {
        VkResult result = device_get_build_sizes(state->backend, type, info, sizes);
        if (result != VK_SUCCESS) {
            sizes->micromapSize = sizes->buildScratchSize = 0;
            sizes->discardable = VK_FALSE;
        }
    } else
        native<PFN_vkGetMicromapBuildSizesEXT>(*state, "vkGetMicromapBuildSizesEXT")(handle, type, info, sizes);
}
VKAPI_ATTR void VKAPI_CALL CmdBuildMicromapsEXT(VkCommandBuffer cb, uint32_t count, const VkMicromapBuildInfoEXT* infos) {
    auto state = device(cb);
    if (state->software)
        finish_internal(*state, cb, device_cmd_build(state->backend, cb, count, infos));
    else
        native<PFN_vkCmdBuildMicromapsEXT>(*state, "vkCmdBuildMicromapsEXT")(cb, count, infos);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyMicromapEXT(VkCommandBuffer cb, const VkCopyMicromapInfoEXT* info) {
    auto state = device(cb);
    if (state->software)
        finish_internal(*state, cb, device_cmd_copy(state->backend, cb, info));
    else
        native<PFN_vkCmdCopyMicromapEXT>(*state, "vkCmdCopyMicromapEXT")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL GetDeviceMicromapCompatibilityEXT(VkDevice handle, const VkMicromapVersionInfoEXT* info,
                                                             VkAccelerationStructureCompatibilityKHR* compatibility) {
    auto state = device(handle);
    if (state->software)
        device_get_compatibility(state->backend, info, compatibility);
    else
        native<PFN_vkGetDeviceMicromapCompatibilityEXT>(*state, "vkGetDeviceMicromapCompatibilityEXT")(handle, info, compatibility);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyMicromapToMemoryEXT(VkCommandBuffer cb, const VkCopyMicromapToMemoryInfoEXT* info) {
    auto state = device(cb);
    if (state->software) {
        if (auto entry = command(*state, cb)) entry->state.fail(VK_ERROR_FEATURE_NOT_PRESENT);
    } else
        native<PFN_vkCmdCopyMicromapToMemoryEXT>(*state, "vkCmdCopyMicromapToMemoryEXT")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyMemoryToMicromapEXT(VkCommandBuffer cb, const VkCopyMemoryToMicromapInfoEXT* info) {
    auto state = device(cb);
    if (state->software) {
        if (auto entry = command(*state, cb)) entry->state.fail(VK_ERROR_FEATURE_NOT_PRESENT);
    } else
        native<PFN_vkCmdCopyMemoryToMicromapEXT>(*state, "vkCmdCopyMemoryToMicromapEXT")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdWriteMicromapsPropertiesEXT(VkCommandBuffer cb, uint32_t count, const VkMicromapEXT* micromaps,
                                                          VkQueryType type, VkQueryPool pool, uint32_t first) {
    auto state = device(cb);
    if (state->software) {
        if (auto entry = command(*state, cb)) entry->state.fail(VK_ERROR_FEATURE_NOT_PRESENT);
    } else
        native<PFN_vkCmdWriteMicromapsPropertiesEXT>(*state, "vkCmdWriteMicromapsPropertiesEXT")(cb, count, micromaps, type, pool,
                                                                                                 first);
}
VKAPI_ATTR VkResult VKAPI_CALL BuildMicromapsEXT(VkDevice handle, VkDeferredOperationKHR deferred, uint32_t count,
                                                 const VkMicromapBuildInfoEXT* infos) {
    auto state = device(handle);
    return state->software ? VK_ERROR_FEATURE_NOT_PRESENT
                           : native<PFN_vkBuildMicromapsEXT>(*state, "vkBuildMicromapsEXT")(handle, deferred, count, infos);
}
VKAPI_ATTR VkResult VKAPI_CALL CopyMicromapEXT(VkDevice handle, VkDeferredOperationKHR deferred,
                                               const VkCopyMicromapInfoEXT* info) {
    auto state = device(handle);
    return state->software ? VK_ERROR_FEATURE_NOT_PRESENT
                           : native<PFN_vkCopyMicromapEXT>(*state, "vkCopyMicromapEXT")(handle, deferred, info);
}
VKAPI_ATTR VkResult VKAPI_CALL CopyMicromapToMemoryEXT(VkDevice handle, VkDeferredOperationKHR deferred,
                                                       const VkCopyMicromapToMemoryInfoEXT* info) {
    auto state = device(handle);
    return state->software ? VK_ERROR_FEATURE_NOT_PRESENT
                           : native<PFN_vkCopyMicromapToMemoryEXT>(*state, "vkCopyMicromapToMemoryEXT")(handle, deferred, info);
}
VKAPI_ATTR VkResult VKAPI_CALL CopyMemoryToMicromapEXT(VkDevice handle, VkDeferredOperationKHR deferred,
                                                       const VkCopyMemoryToMicromapInfoEXT* info) {
    auto state = device(handle);
    return state->software ? VK_ERROR_FEATURE_NOT_PRESENT
                           : native<PFN_vkCopyMemoryToMicromapEXT>(*state, "vkCopyMemoryToMicromapEXT")(handle, deferred, info);
}
VKAPI_ATTR VkResult VKAPI_CALL WriteMicromapsPropertiesEXT(VkDevice handle, uint32_t count, const VkMicromapEXT* micromaps,
                                                           VkQueryType type, size_t size, void* data, size_t stride) {
    auto state = device(handle);
    return state->software ? VK_ERROR_FEATURE_NOT_PRESENT
                           : native<PFN_vkWriteMicromapsPropertiesEXT>(*state, "vkWriteMicromapsPropertiesEXT")(
                                 handle, count, micromaps, type, size, data, stride);
}

void fail_command(Device& dev, VkCommandBuffer cb, VkResult result) {
    if (auto entry = command(dev, cb)) entry->state.fail(result);
}
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* info) {
    auto state = device(cb);
    auto call = native<PFN_vkCmdPipelineBarrier2>(*state, "vkCmdPipelineBarrier2");
    if (!call) call = native<PFN_vkCmdPipelineBarrier2>(*state, "vkCmdPipelineBarrier2KHR");
    if (!state->software) {
        call(cb, info);
        return;
    }
    TranslatedDependency translated;
    VkResult result = translated.assign(*info);
    if (result != VK_SUCCESS) {
        fail_command(*state, cb, result);
        return;
    }
    const auto view = translated.view();
    call(cb, &view);
}
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                              VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier* memory,
                                              uint32_t buffer_count, const VkBufferMemoryBarrier* buffers, uint32_t image_count,
                                              const VkImageMemoryBarrier* images) {
    auto state = device(cb);
    auto call = native<PFN_vkCmdPipelineBarrier>(*state, "vkCmdPipelineBarrier");
    if (!state->software) {
        call(cb, src, dst, flags, memory_count, memory, buffer_count, buffers, image_count, images);
        return;
    }
    TranslatedLegacyBarriers translated;
    VkResult result = translated.assign(src, dst, flags, memory_count, memory, buffer_count, buffers, image_count, images);
    if (result != VK_SUCCESS) {
        fail_command(*state, cb, result);
        return;
    }
    const auto v = translated.view();
    call(cb, v.src_stage_mask, v.dst_stage_mask, v.dependency_flags, v.memory_barrier_count, v.memory_barriers,
         v.buffer_memory_barrier_count, v.buffer_memory_barriers, v.image_memory_barrier_count, v.image_memory_barriers);
}
VKAPI_ATTR void VKAPI_CALL CmdSetEvent2(VkCommandBuffer cb, VkEvent event, const VkDependencyInfo* info) {
    auto state = device(cb);
    auto call = native<PFN_vkCmdSetEvent2>(*state, "vkCmdSetEvent2");
    if (!call) call = native<PFN_vkCmdSetEvent2>(*state, "vkCmdSetEvent2KHR");
    if (!state->software) {
        call(cb, event, info);
        return;
    }
    TranslatedDependency translated;
    VkResult result = translated.assign(*info);
    if (result != VK_SUCCESS) {
        fail_command(*state, cb, result);
        return;
    }
    const auto view = translated.view();
    call(cb, event, &view);
}
VKAPI_ATTR void VKAPI_CALL CmdWaitEvents2(VkCommandBuffer cb, uint32_t count, const VkEvent* events,
                                          const VkDependencyInfo* infos) {
    auto state = device(cb);
    auto call = native<PFN_vkCmdWaitEvents2>(*state, "vkCmdWaitEvents2");
    if (!call) call = native<PFN_vkCmdWaitEvents2>(*state, "vkCmdWaitEvents2KHR");
    if (!state->software) {
        call(cb, count, events, infos);
        return;
    }
    try {
        std::vector<TranslatedDependency> translated(count);
        std::vector<VkDependencyInfo> views(count);
        for (uint32_t i = 0; i < count; ++i) {
            VkResult result = translated[i].assign(infos[i]);
            if (result != VK_SUCCESS) {
                fail_command(*state, cb, result);
                return;
            }
            views[i] = translated[i].view();
        }
        call(cb, count, events, views.data());
    } catch (const std::bad_alloc&) {
        fail_command(*state, cb, VK_ERROR_OUT_OF_HOST_MEMORY);
    }
}
VKAPI_ATTR void VKAPI_CALL CmdResetEvent2(VkCommandBuffer cb, VkEvent event, VkPipelineStageFlags2 stages) {
    auto state = device(cb);
    auto call = native<PFN_vkCmdResetEvent2>(*state, "vkCmdResetEvent2");
    if (!call) call = native<PFN_vkCmdResetEvent2>(*state, "vkCmdResetEvent2KHR");
    call(cb, event, state->software ? map_stage2(stages) : stages);
}
VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stages, VkQueryPool pool, uint32_t query) {
    auto state = device(cb);
    auto call = native<PFN_vkCmdWriteTimestamp2>(*state, "vkCmdWriteTimestamp2");
    if (!call) call = native<PFN_vkCmdWriteTimestamp2>(*state, "vkCmdWriteTimestamp2KHR");
    call(cb, state->software ? map_stage2(stages) : stages, pool, query);
}
VKAPI_ATTR void VKAPI_CALL CmdSetEvent(VkCommandBuffer cb, VkEvent event, VkPipelineStageFlags stages) {
    auto state = device(cb);
    native<PFN_vkCmdSetEvent>(*state, "vkCmdSetEvent")(cb, event, state->software ? map_stage(stages) : stages);
}
VKAPI_ATTR void VKAPI_CALL CmdResetEvent(VkCommandBuffer cb, VkEvent event, VkPipelineStageFlags stages) {
    auto state = device(cb);
    native<PFN_vkCmdResetEvent>(*state, "vkCmdResetEvent")(cb, event, state->software ? map_stage(stages) : stages);
}
VKAPI_ATTR void VKAPI_CALL CmdWaitEvents(VkCommandBuffer cb, uint32_t count, const VkEvent* events, VkPipelineStageFlags src,
                                         VkPipelineStageFlags dst, uint32_t memory_count, const VkMemoryBarrier* memory,
                                         uint32_t buffer_count, const VkBufferMemoryBarrier* buffers, uint32_t image_count,
                                         const VkImageMemoryBarrier* images) {
    auto state = device(cb);
    auto call = native<PFN_vkCmdWaitEvents>(*state, "vkCmdWaitEvents");
    if (!state->software) {
        call(cb, count, events, src, dst, memory_count, memory, buffer_count, buffers, image_count, images);
        return;
    }
    TranslatedLegacyBarriers translated;
    VkResult result = translated.assign(src, dst, 0, memory_count, memory, buffer_count, buffers, image_count, images);
    if (result != VK_SUCCESS) {
        fail_command(*state, cb, result);
        return;
    }
    const auto v = translated.view();
    call(cb, count, events, v.src_stage_mask, v.dst_stage_mask, v.memory_barrier_count, v.memory_barriers,
         v.buffer_memory_barrier_count, v.buffer_memory_barriers, v.image_memory_barrier_count, v.image_memory_barriers);
}
VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query) {
    auto state = device(cb);
    native<PFN_vkCmdWriteTimestamp>(*state, "vkCmdWriteTimestamp")(
        cb, state->software ? VkPipelineStageFlagBits(map_stage(stage)) : stage, pool, query);
}
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
    auto state = device(queue);
    auto call = native<PFN_vkQueueSubmit>(*state, "vkQueueSubmit");
    if (!state->software) return call(queue, count, infos, fence);
    try {
        std::vector<vku::safe_VkSubmitInfo> owned;
        std::vector<VkSubmitInfo> views;
        owned.reserve(count);
        views.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            owned.emplace_back(&infos[i]);
            auto& submit = owned.back();
            for (uint32_t j = 0; j < submit.waitSemaphoreCount; ++j)
                const_cast<VkPipelineStageFlags*>(submit.pWaitDstStageMask)[j] = map_stage(submit.pWaitDstStageMask[j]);
            for (uint32_t j = 0; j < submit.commandBufferCount; ++j)
                if (auto entry = command(*state, submit.pCommandBuffers[j]))
                    if (entry->state.status() != VK_SUCCESS) return entry->state.status();
            views.push_back(*submit.ptr());
        }
        return call(queue, count, views.data(), fence);
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence) {
    auto state = device(queue);
    auto call = native<PFN_vkQueueSubmit2>(*state, "vkQueueSubmit2");
    if (!call) call = native<PFN_vkQueueSubmit2>(*state, "vkQueueSubmit2KHR");
    if (!state->software) return call(queue, count, infos, fence);
    try {
        std::vector<vku::safe_VkSubmitInfo2> owned;
        std::vector<VkSubmitInfo2> views;
        owned.reserve(count);
        views.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            owned.emplace_back(&infos[i]);
            auto& submit = owned.back();
            for (uint32_t j = 0; j < submit.waitSemaphoreInfoCount; ++j)
                submit.pWaitSemaphoreInfos[j].stageMask = map_stage2(submit.pWaitSemaphoreInfos[j].stageMask);
            for (uint32_t j = 0; j < submit.signalSemaphoreInfoCount; ++j)
                submit.pSignalSemaphoreInfos[j].stageMask = map_stage2(submit.pSignalSemaphoreInfos[j].stageMask);
            for (uint32_t j = 0; j < submit.commandBufferInfoCount; ++j)
                if (auto entry = command(*state, submit.pCommandBufferInfos[j].commandBuffer))
                    if (entry->state.status() != VK_SUCCESS) return entry->state.status();
            views.push_back(*submit.ptr());
        }
        return call(queue, count, views.data(), fence);
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
bool omm_geometry(const VkAccelerationStructureBuildGeometryInfoKHR& info) {
    constexpr auto flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_OPACITY_MICROMAP_UPDATE_BIT_EXT |
                           VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DISABLE_OPACITY_MICROMAPS_BIT_EXT |
                           VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_OPACITY_MICROMAP_DATA_UPDATE_BIT_EXT;
    if (info.flags & flags) return true;
    for (uint32_t i = 0; i < info.geometryCount; ++i) {
        const auto& geometry = info.pGeometries ? info.pGeometries[i] : *info.ppGeometries[i];
        if (geometry.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR &&
            vku::FindStructInPNextChain<VkAccelerationStructureTrianglesOpacityMicromapEXT>(geometry.geometry.triangles.pNext))
            return true;
    }
    return false;
}
VKAPI_ATTR void VKAPI_CALL CmdBuildAccelerationStructuresKHR(VkCommandBuffer cb, uint32_t count,
                                                             const VkAccelerationStructureBuildGeometryInfoKHR* infos,
                                                             const VkAccelerationStructureBuildRangeInfoKHR* const* ranges) {
    auto state = device(cb);
    // GPU instance inputs can carry OMM flags even without a visible geometry
    // association. No software AS build is forwarded until that adapter exists.
    if (state->software) {
        fail_command(*state, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return;
    }
    native<PFN_vkCmdBuildAccelerationStructuresKHR>(*state, "vkCmdBuildAccelerationStructuresKHR")(cb, count, infos, ranges);
}
VKAPI_ATTR void VKAPI_CALL CmdBuildAccelerationStructuresIndirectKHR(VkCommandBuffer cb, uint32_t count,
                                                                     const VkAccelerationStructureBuildGeometryInfoKHR* infos,
                                                                     const VkDeviceAddress* addresses, const uint32_t* strides,
                                                                     const uint32_t* const* primitive_counts) {
    auto state = device(cb);
    if (state->software) {
        fail_command(*state, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return;
    }
    native<PFN_vkCmdBuildAccelerationStructuresIndirectKHR>(*state, "vkCmdBuildAccelerationStructuresIndirectKHR")(
        cb, count, infos, addresses, strides, primitive_counts);
}
VKAPI_ATTR VkResult VKAPI_CALL BuildAccelerationStructuresKHR(VkDevice handle, VkDeferredOperationKHR deferred, uint32_t count,
                                                              const VkAccelerationStructureBuildGeometryInfoKHR* infos,
                                                              const VkAccelerationStructureBuildRangeInfoKHR* const* ranges) {
    auto state = device(handle);
    if (state->software) return VK_ERROR_FEATURE_NOT_PRESENT;
    return native<PFN_vkBuildAccelerationStructuresKHR>(*state, "vkBuildAccelerationStructuresKHR")(handle, deferred, count, infos,
                                                                                                    ranges);
}
VKAPI_ATTR void VKAPI_CALL GetAccelerationStructureBuildSizesKHR(VkDevice handle, VkAccelerationStructureBuildTypeKHR type,
                                                                 const VkAccelerationStructureBuildGeometryInfoKHR* info,
                                                                 const uint32_t* counts,
                                                                 VkAccelerationStructureBuildSizesInfoKHR* sizes) {
    auto state = device(handle);
    if (state->software && omm_geometry(*info)) {
        // No layer-owned micromap identity is ever forwarded to the driver.
        sizes->accelerationStructureSize = sizes->buildScratchSize = sizes->updateScratchSize = 0;
        return;
    }
    native<PFN_vkGetAccelerationStructureBuildSizesKHR>(*state, "vkGetAccelerationStructureBuildSizesKHR")(handle, type, info,
                                                                                                           counts, sizes);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateQueryPool(VkDevice handle, const VkQueryPoolCreateInfo* info,
                                               const VkAllocationCallbacks* allocator, VkQueryPool* out) {
    auto state = device(handle);
    if (state->software && (info->queryType == VK_QUERY_TYPE_MICROMAP_COMPACTED_SIZE_EXT ||
                            info->queryType == VK_QUERY_TYPE_MICROMAP_SERIALIZATION_SIZE_EXT)) {
        *out = VK_NULL_HANDLE;
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return native<PFN_vkCreateQueryPool>(*state, "vkCreateQueryPool")(handle, info, allocator, out);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice, const char*);
struct Hook {
    const char* symbol;
    PFN_vkVoidFunction function;
};
#define HOOK(fn) {"vk" #fn, reinterpret_cast<PFN_vkVoidFunction>(fn)}
const Hook hooks[] = {
    HOOK(DestroyDevice),
    HOOK(GetDeviceProcAddr),
    HOOK(CreateBuffer),
    HOOK(DestroyBuffer),
    HOOK(AllocateMemory),
    HOOK(BindBufferMemory),
    HOOK(BindBufferMemory2),
    {"vkBindBufferMemory2KHR", reinterpret_cast<PFN_vkVoidFunction>(BindBufferMemory2)},
    HOOK(GetDeviceBufferMemoryRequirements),
    {"vkGetDeviceBufferMemoryRequirementsKHR", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceBufferMemoryRequirements)},
    HOOK(AllocateCommandBuffers),
    HOOK(FreeCommandBuffers),
    HOOK(DestroyCommandPool),
    HOOK(ResetCommandPool),
    HOOK(BeginCommandBuffer),
    HOOK(ResetCommandBuffer),
    HOOK(EndCommandBuffer),
    HOOK(CmdBindPipeline),
    HOOK(CmdBindShadersEXT),
    HOOK(CreatePipelineLayout),
    HOOK(DestroyPipelineLayout),
    HOOK(CmdPushConstants),
    HOOK(CreateMicromapEXT),
    HOOK(DestroyMicromapEXT),
    HOOK(GetMicromapBuildSizesEXT),
    HOOK(CmdBuildMicromapsEXT),
    HOOK(CmdCopyMicromapEXT),
    HOOK(GetDeviceMicromapCompatibilityEXT),
    HOOK(CmdCopyMicromapToMemoryEXT),
    HOOK(CmdCopyMemoryToMicromapEXT),
    HOOK(CmdWriteMicromapsPropertiesEXT),
    HOOK(BuildMicromapsEXT),
    HOOK(CopyMicromapEXT),
    HOOK(CopyMicromapToMemoryEXT),
    HOOK(CopyMemoryToMicromapEXT),
    HOOK(WriteMicromapsPropertiesEXT),
    HOOK(CmdPipelineBarrier),
    HOOK(CmdPipelineBarrier2),
    HOOK(CmdSetEvent),
    HOOK(CmdResetEvent),
    HOOK(CmdWaitEvents),
    HOOK(CmdSetEvent2),
    HOOK(CmdResetEvent2),
    HOOK(CmdWaitEvents2),
    HOOK(CmdWriteTimestamp),
    HOOK(CmdWriteTimestamp2),
    HOOK(QueueSubmit),
    HOOK(QueueSubmit2),
    HOOK(CmdBuildAccelerationStructuresKHR),
    HOOK(CmdBuildAccelerationStructuresIndirectKHR),
    HOOK(BuildAccelerationStructuresKHR),
    HOOK(GetAccelerationStructureBuildSizesKHR),
    HOOK(CreateQueryPool),
    {"vkCmdPipelineBarrier2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdPipelineBarrier2)},
    {"vkCmdSetEvent2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdSetEvent2)},
    {"vkCmdResetEvent2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdResetEvent2)},
    {"vkCmdWaitEvents2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdWaitEvents2)},
    {"vkCmdWriteTimestamp2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdWriteTimestamp2)},
    {"vkQueueSubmit2KHR", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit2)}};
const Hook instance_hooks[] = {
    HOOK(CreateInstance),
    HOOK(DestroyInstance),
    HOOK(CreateDevice),
    HOOK(GetInstanceProcAddr),
    HOOK(GetPhysicalDeviceFeatures2),
    {"vkGetPhysicalDeviceFeatures2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceFeatures2)},
    HOOK(GetPhysicalDeviceProperties2),
    {"vkGetPhysicalDeviceProperties2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceProperties2)}};
#undef HOOK
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance handle, const char* symbol) {
    if (!symbol) return nullptr;
    if (!std::strcmp(symbol, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
    if (!std::strcmp(symbol, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(GetInstanceProcAddr);
    auto state = instance(handle);
    if (!state) return nullptr;
    const auto available = state->next(handle, symbol);
    for (const auto& hook : instance_hooks)
        if (!std::strcmp(symbol, hook.symbol)) return available ? hook.function : nullptr;
    for (const auto& hook : hooks)
        if (!std::strcmp(symbol, hook.symbol)) return available ? hook.function : nullptr;
    return available;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice handle, const char* symbol) {
    auto state = device(handle);
    if (!state || !symbol) return nullptr;
    if (!std::strcmp(symbol, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    if (!std::strcmp(symbol, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr);
    if (state->software)
        for (const auto& hook : hooks)
            if (!std::strcmp(symbol, hook.symbol))
                return (std::strstr(symbol, "Micromap") || state->next(handle, symbol)) ? hook.function : nullptr;
    return state->next(handle, symbol);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetPhysicalDeviceProcAddr(VkInstance handle, const char* symbol) {
    auto state = instance(handle);
    if (!state || !symbol) return nullptr;
    const auto available = state->physical_next ? state->physical_next(handle, symbol) : state->next(handle, symbol);
    if (!std::strcmp(symbol, "vkGetPhysicalDeviceFeatures2") || !std::strcmp(symbol, "vkGetPhysicalDeviceFeatures2KHR"))
        return available ? reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceFeatures2) : nullptr;
    if (!std::strcmp(symbol, "vkGetPhysicalDeviceProperties2") || !std::strcmp(symbol, "vkGetPhysicalDeviceProperties2KHR"))
        return available ? reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceProperties2) : nullptr;
    return available;
}
}  // namespace vel::omm::layer

#if defined(_WIN32)
#define OMM_EXPORT
#else
#define OMM_EXPORT __attribute__((visibility("default")))
#endif
extern "C" OMM_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* symbol) {
    return vel::omm::layer::GetInstanceProcAddr(instance, symbol);
}
extern "C" OMM_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* symbol) {
    return vel::omm::layer::GetDeviceProcAddr(device, symbol);
}
extern "C" OMM_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if (!info || info->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion = 2;
    info->pfnGetInstanceProcAddr = vel::omm::layer::GetInstanceProcAddr;
    info->pfnGetDeviceProcAddr = vel::omm::layer::GetDeviceProcAddr;
    info->pfnGetPhysicalDeviceProcAddr = vel::omm::layer::GetPhysicalDeviceProcAddr;
    return VK_SUCCESS;
}
