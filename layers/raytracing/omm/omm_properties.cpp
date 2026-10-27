// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_properties.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace vel::omm {
namespace {
struct Dispatch {
    VkDevice device{};
    VkAllocationCallbacks allocator{};
    bool custom = false;
    VkPhysicalDeviceMemoryProperties memory{};
    std::vector<VkQueueFamilyProperties> families;
    std::vector<uint32_t> family_indices;
    PFN_vkCreateBuffer create_buffer{};
    PFN_vkDestroyBuffer destroy_buffer{};
    PFN_vkGetBufferMemoryRequirements requirements{};
    PFN_vkAllocateMemory allocate{};
    PFN_vkFreeMemory free{};
    PFN_vkBindBufferMemory bind{};
    PFN_vkMapMemory map{};
    PFN_vkUnmapMemory unmap{};
    PFN_vkGetBufferDeviceAddress address{};
    PFN_vkCreateQueryPool create_pool{};
    PFN_vkDestroyQueryPool destroy_pool{};
    const VkAllocationCallbacks* callbacks() const { return custom ? &allocator : nullptr; }
};
struct Buffer {
    std::shared_ptr<Dispatch> d;
    VkBuffer handle{};
    VkDeviceMemory memory{};
    VkDeviceAddress address{};
    uint64_t size{};
    void* mapped{};
    ~Buffer() {
        if (mapped) d->unmap(d->device, memory);
        if (handle) d->destroy_buffer(d->device, handle, d->callbacks());
        if (memory) d->free(d->device, memory, d->callbacks());
    }
};
using BufferRef = std::shared_ptr<Buffer>;
struct Pool {
    std::shared_ptr<Dispatch> d;
    VkQueryPool native{};
    VkQueryType type{};
    uint32_t count{};
    VkAllocationCallbacks allocator{};
    bool custom = false;
    BufferRef values;
    ~Pool() {
        if (native) d->destroy_pool(d->device, native, custom ? &allocator : nullptr);
    }
};
struct Push {
    uint64_t source, capacity, values, native_results, destination, stride;
    uint32_t count, flags, phase, reserved;
};
static_assert(sizeof(Push) == 64);
bool supported(VkQueryType type) {
    return type == VK_QUERY_TYPE_MICROMAP_COMPACTED_SIZE_EXT || type == VK_QUERY_TYPE_MICROMAP_SERIALIZATION_SIZE_EXT;
}
VkResult span(uint32_t count, VkDeviceSize stride, VkQueryResultFlags flags, uint64_t* bytes) {
    constexpr VkQueryResultFlags allowed =
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT;
    if (flags & ~allowed) return VK_ERROR_FEATURE_NOT_PRESENT;
    const uint64_t width = (flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4,
                   record = width * ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ? 2 : 1);
    if ((count > 1 && (!stride || stride % width)) ||
        (count && uint64_t(count - 1) > (UINT64_MAX - record) / (stride ? stride : 1)))
        return VK_ERROR_VALIDATION_FAILED_EXT;
    *bytes = count ? uint64_t(count - 1) * stride + record : 0;
    return VK_SUCCESS;
}
}  // namespace
struct PropertyBackend {
    std::shared_ptr<Dispatch> d;
    DeviceBackend* micromaps{};
    std::mutex mutex;
    std::unordered_map<VkQueryPool, std::shared_ptr<Pool>> pools;
    VkPipeline pipeline{};
    VkPipelineLayout layout{};
    PFN_vkCreateShaderModule create_shader{};
    PFN_vkDestroyShaderModule destroy_shader{};
    PFN_vkCreatePipelineLayout create_layout{};
    PFN_vkDestroyPipelineLayout destroy_layout{};
    PFN_vkCreateComputePipelines create_pipeline{};
    PFN_vkDestroyPipeline destroy_pipeline{};
    PFN_vkCmdBindPipeline bind_pipeline{};
    PFN_vkCmdPushConstants push{};
    PFN_vkCmdDispatch dispatch{};
    PFN_vkCmdPipelineBarrier barrier{};
    PFN_vkCmdResetQueryPool reset{};
    PFN_vkResetQueryPool reset_host{};
    PFN_vkCmdWriteTimestamp timestamp{};
    PFN_vkGetQueryPoolResults results{};
    PFN_vkCmdCopyQueryPoolResults copy{};
    PFN_vkCmdFillBuffer fill{};
    PFN_vkCmdCopyBuffer copy_buffer{};
};
struct PropertyRecording {
    std::shared_ptr<Pool> pool;
    BufferRef scratch, formatted;
};
namespace {
VkResult buffer(PropertyBackend* b, uint64_t bytes, bool host, BufferRef* out) {
    if (bytes > UINT64_MAX - 15) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto result = std::make_shared<Buffer>();
    result->d = b->d;
    result->size = std::max(uint64_t(16), (bytes + 15) & ~uint64_t(15));
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = result->size;
    info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (b->d->family_indices.size() > 1) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = uint32_t(b->d->family_indices.size());
        info.pQueueFamilyIndices = b->d->family_indices.data();
    }
    auto status = b->d->create_buffer(b->d->device, &info, b->d->callbacks(), &result->handle);
    if (status != VK_SUCCESS) return status;
    VkMemoryRequirements requirements;
    b->d->requirements(b->d->device, result->handle, &requirements);
    const VkMemoryPropertyFlags needed = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < b->d->memory.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (b->d->memory.memoryTypes[i].propertyFlags & needed) == needed) {
            type = i;
            if (host || (b->d->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
        }
    if (type == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext = &flags;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    status = b->d->allocate(b->d->device, &allocate, b->d->callbacks(), &result->memory);
    if (status != VK_SUCCESS) return status;
    status = b->d->bind(b->d->device, result->handle, result->memory, 0);
    if (status != VK_SUCCESS) return status;
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = result->handle;
    result->address = b->d->address(b->d->device, &address);
    if (!result->address || result->size > UINT64_MAX - result->address) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (host) {
        status = b->d->map(b->d->device, result->memory, 0, VK_WHOLE_SIZE, 0, &result->mapped);
        if (status != VK_SUCCESS) return status;
        std::memset(result->mapped, 0, size_t(result->size));
    }
    *out = std::move(result);
    return VK_SUCCESS;
}
VkResult lookup(PropertyBackend* b, VkQueryPool handle, uint32_t first, uint32_t count, std::shared_ptr<Pool>* out) {
    auto found = b->pools.find(handle);
    if (found == b->pools.end() || first > found->second->count || count > found->second->count - first)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    *out = found->second;
    return VK_SUCCESS;
}
VkResult queue(PropertyBackend* b, uint32_t family) {
    if (family >= b->d->families.size()) return VK_ERROR_VALIDATION_FAILED_EXT;
    const auto& properties = b->d->families[family];
    return properties.timestampValidBits && (properties.queueFlags & VK_QUEUE_COMPUTE_BIT) ? VK_SUCCESS
                                                                                           : VK_ERROR_FEATURE_NOT_PRESENT;
}
void barrier(PropertyBackend* b, VkCommandBuffer cb, VkPipelineStageFlags src_stage, VkAccessFlags src,
             VkPipelineStageFlags dst_stage, VkAccessFlags dst) {
    VkMemoryBarrier memory{};
    memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memory.srcAccessMask = src;
    memory.dstAccessMask = dst;
    b->barrier(cb, src_stage, dst_stage, 0, 1, &memory, 0, nullptr, 0, nullptr);
}
void dispatch(PropertyBackend* b, VkCommandBuffer cb, const Push& push) {
    b->bind_pipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, b->pipeline);
    b->push(cb, b->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    b->dispatch(
        cb, push.phase == 1 ? 1 : uint32_t(std::min(uint64_t(128), std::max(uint64_t(1), (uint64_t(push.count) + 63) / 64))), 1, 1);
}
}  // namespace
VkResult properties_create(const PropertyBackendCreateInfo& info, PropertyBackend** out) {
    if (!out || !info.device || !info.physical_device || !info.get_device_proc_addr || !info.micromaps || !info.property_code ||
        !info.property_code_size || (info.property_code_size & 3))
        return VK_ERROR_INITIALIZATION_FAILED;
    *out = nullptr;
    PropertyBackend* b = nullptr;
    try {
        b = new PropertyBackend;
        b->d = std::make_shared<Dispatch>();
        b->d->device = info.device;
        b->micromaps = info.micromaps;
        if (info.allocator) {
            b->d->allocator = *info.allocator;
            b->d->custom = true;
        }
        const auto get_memory = info.get_memory_properties ? info.get_memory_properties : vkGetPhysicalDeviceMemoryProperties;
        const auto get_families =
            info.get_queue_family_properties ? info.get_queue_family_properties : vkGetPhysicalDeviceQueueFamilyProperties;
        get_memory(info.physical_device, &b->d->memory);
        uint32_t count = 0;
        get_families(info.physical_device, &count, nullptr);
        b->d->families.resize(count);
        b->d->family_indices.resize(count);
        get_families(info.physical_device, &count, b->d->families.data());
        for (uint32_t i = 0; i < count; ++i) b->d->family_indices[i] = i;
#define LOAD_D(field, name)                                                                           \
    b->d->field = reinterpret_cast<PFN_vk##name>(info.get_device_proc_addr(info.device, "vk" #name)); \
    if (!b->d->field) {                                                                               \
        properties_destroy(b);                                                                        \
        return VK_ERROR_INITIALIZATION_FAILED;                                                        \
    }
        LOAD_D(create_buffer, CreateBuffer);

        LOAD_D(destroy_buffer, DestroyBuffer);
        LOAD_D(requirements, GetBufferMemoryRequirements);
        LOAD_D(allocate, AllocateMemory);

        LOAD_D(free, FreeMemory);
        LOAD_D(bind, BindBufferMemory);
        LOAD_D(map, MapMemory);
        LOAD_D(unmap, UnmapMemory);

        LOAD_D(address, GetBufferDeviceAddress);
        LOAD_D(create_pool, CreateQueryPool);
        LOAD_D(destroy_pool, DestroyQueryPool);

#undef LOAD_D
#define LOAD(field, name)                                                                          \
    b->field = reinterpret_cast<PFN_vk##name>(info.get_device_proc_addr(info.device, "vk" #name)); \
    if (!b->field) {                                                                               \
        properties_destroy(b);                                                                     \
        return VK_ERROR_INITIALIZATION_FAILED;                                                     \
    }
        LOAD(create_shader, CreateShaderModule);
        LOAD(destroy_shader, DestroyShaderModule);

        LOAD(create_layout, CreatePipelineLayout);
        LOAD(destroy_layout, DestroyPipelineLayout);

        LOAD(create_pipeline, CreateComputePipelines);
        LOAD(destroy_pipeline, DestroyPipeline);

        LOAD(bind_pipeline, CmdBindPipeline);
        LOAD(push, CmdPushConstants);
        LOAD(dispatch, CmdDispatch);

        LOAD(barrier, CmdPipelineBarrier);
        LOAD(reset, CmdResetQueryPool);

        LOAD(timestamp, CmdWriteTimestamp);
        LOAD(results, GetQueryPoolResults);

        LOAD(copy, CmdCopyQueryPoolResults);
        LOAD(fill, CmdFillBuffer);

        LOAD(copy_buffer, CmdCopyBuffer);

#undef LOAD
        b->reset_host = reinterpret_cast<PFN_vkResetQueryPool>(info.get_device_proc_addr(info.device, "vkResetQueryPool"));
        if (!b->reset_host)
            b->reset_host = reinterpret_cast<PFN_vkResetQueryPool>(info.get_device_proc_addr(info.device, "vkResetQueryPoolEXT"));
        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = info.property_code_size;
        module_info.pCode = info.property_code;
        VkShaderModule module{};
        auto status = b->create_shader(info.device, &module_info, b->d->callbacks(), &module);
        if (status != VK_SUCCESS) {
            properties_destroy(b);
            return status;
        }
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        status = b->create_layout(info.device, &layout, b->d->callbacks(), &b->layout);
        if (status == VK_SUCCESS) {
            VkComputePipelineCreateInfo pipeline{};
            pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipeline.layout = b->layout;
            pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipeline.stage.module = module;
            pipeline.stage.pName = "main";
            status = b->create_pipeline(info.device, VK_NULL_HANDLE, 1, &pipeline, b->d->callbacks(), &b->pipeline);
        }
        b->destroy_shader(info.device, module, b->d->callbacks());
        if (status != VK_SUCCESS) {
            properties_destroy(b);
            return status;
        }
        *out = b;
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        properties_destroy(b);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void properties_destroy(PropertyBackend* b) {
    if (!b) return;
    b->pools.clear();
    if (b->pipeline) b->destroy_pipeline(b->d->device, b->pipeline, b->d->callbacks());
    if (b->layout) b->destroy_layout(b->d->device, b->layout, b->d->callbacks());
    delete b;
}
bool properties_is_pool(PropertyBackend* b, VkQueryPool handle) noexcept {
    if (!b) return false;
    std::lock_guard<std::mutex> lock(b->mutex);
    return b->pools.count(handle) != 0;
}
VkResult properties_create_pool(PropertyBackend* b, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks* allocator,
                                VkQueryPool* out) {
    if (!b || !info || !out || !supported(info->queryType) || !info->queryCount) return VK_ERROR_VALIDATION_FAILED_EXT;
    if (info->pNext || info->flags) return VK_ERROR_FEATURE_NOT_PRESENT;
    *out = VK_NULL_HANDLE;
    try {
        std::lock_guard<std::mutex> lock(b->mutex);
        auto pool = std::make_shared<Pool>();
        pool->d = b->d;
        pool->type = info->queryType;
        pool->count = info->queryCount;
        if (allocator) {
            pool->allocator = *allocator;
            pool->custom = true;
        }
        auto status = buffer(b, uint64_t(info->queryCount) * 8, true, &pool->values);
        if (status != VK_SUCCESS) return status;
        auto native = *info;
        native.queryType = VK_QUERY_TYPE_TIMESTAMP;
        native.pipelineStatistics = 0;
        status = b->d->create_pool(b->d->device, &native, allocator, &pool->native);
        if (status != VK_SUCCESS) return status;
        const auto handle = pool->native;
        b->pools.emplace(handle, std::move(pool));
        *out = handle;
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void properties_destroy_pool(PropertyBackend* b, VkQueryPool handle, const VkAllocationCallbacks*) {
    if (!b) return;
    std::lock_guard<std::mutex> lock(b->mutex);
    b->pools.erase(handle);
}
VkResult properties_reset_host(PropertyBackend* b, VkQueryPool handle, uint32_t first, uint32_t count) {
    if (!b || !b->reset_host) return VK_ERROR_FEATURE_NOT_PRESENT;
    std::shared_ptr<Pool> pool;
    {
        std::lock_guard<std::mutex> lock(b->mutex);
        auto status = lookup(b, handle, first, count, &pool);
        if (status != VK_SUCCESS) return status;
    }
    b->reset_host(b->d->device, pool->native, first, count);
    // Numeric values become undefined; no CPU access to potentially GPU-owned
    // value bytes is needed. Native availability is the sole host-read gate.
    return VK_SUCCESS;
}
VkResult properties_get_results(PropertyBackend* b, VkQueryPool handle, uint32_t first, uint32_t count, size_t size, void* output,
                                VkDeviceSize stride, VkQueryResultFlags flags) {
    if (!b || !output) return VK_ERROR_VALIDATION_FAILED_EXT;
    uint64_t required;
    auto status = span(count, stride, flags, &required);
    if (status != VK_SUCCESS || required > size) return status == VK_SUCCESS ? VK_ERROR_VALIDATION_FAILED_EXT : status;
    try {
        std::shared_ptr<Pool> pool;
        {
            std::lock_guard<std::mutex> lock(b->mutex);
            status = lookup(b, handle, first, count, &pool);
            if (status != VK_SUCCESS) return status;
        }
        if (!count) return VK_SUCCESS;
        std::vector<uint64_t> native(size_t(count) * 2, 0);
        const auto result =
            b->results(b->d->device, pool->native, first, count, native.size() * 8, native.data(), 16,
                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | (flags & VK_QUERY_RESULT_WAIT_BIT));
        if (result != VK_SUCCESS && result != VK_NOT_READY) return result;
        auto* destination = static_cast<uint8_t*>(output);
        const size_t width = (flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4;
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t available = native[size_t(i) * 2 + 1] != 0;
            if (available || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) {
                uint64_t value = 0;
                if (available) std::memcpy(&value, static_cast<const uint8_t*>(pool->values->mapped) + uint64_t(first + i) * 8, 8);
                std::memcpy(destination + uint64_t(i) * stride, &value, width);
            }
            if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
                std::memcpy(destination + uint64_t(i) * stride + width, &available, width);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VkResult properties_cmd_reset(PropertyBackend* b, VkCommandBuffer cb, uint32_t family, VkQueryPool handle, uint32_t first,
                              uint32_t count, PropertyRecordingRef* out) {
    if (!b || !cb || !out) return VK_ERROR_VALIDATION_FAILED_EXT;
    try {
        std::lock_guard<std::mutex> lock(b->mutex);
        auto status = queue(b, family);
        if (status != VK_SUCCESS) return status;
        auto recording = std::make_shared<PropertyRecording>();
        status = lookup(b, handle, first, count, &recording->pool);
        if (status != VK_SUCCESS) return status;
        if (!count) {
            *out = std::move(recording);
            return VK_SUCCESS;
        }
        Push push{};
        push.values = recording->pool->values->address + uint64_t(first) * 8;
        push.count = count;
        b->reset(cb, recording->pool->native, first, count);
        barrier(b, cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        dispatch(b, cb, push);
        barrier(b, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        *out = std::move(recording);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VkResult properties_cmd_write(PropertyBackend* b, VkCommandBuffer cb, uint32_t family, uint32_t count,
                              const VkMicromapEXT* micromaps, VkQueryType type, VkQueryPool handle, uint32_t first,
                              PropertyRecordingRef* out) {
    if (!b || !cb || !out || (count && !micromaps)) return VK_ERROR_VALIDATION_FAILED_EXT;
    try {
        std::lock_guard<std::mutex> lock(b->mutex);
        auto status = queue(b, family);
        if (status != VK_SUCCESS) return status;
        auto recording = std::make_shared<PropertyRecording>();
        status = lookup(b, handle, first, count, &recording->pool);
        if (status != VK_SUCCESS) return status;
        if (recording->pool->type != type) return VK_ERROR_VALIDATION_FAILED_EXT;
        if (!count) {
            *out = std::move(recording);
            return VK_SUCCESS;
        }
        std::vector<MicromapStorage> sources(count);
        for (uint32_t i = 0; i < count; ++i) {
            status = device_micromap_storage(b->micromaps, micromaps[i], &sources[i]);
            if (status != VK_SUCCESS) return status;
            if (sources[i].capacity < 64 || (sources[i].address & 3)) return VK_ERROR_VALIDATION_FAILED_EXT;
        }
        barrier(b, cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        for (uint32_t i = 0; i < count; ++i) {
            Push push{};
            push.source = sources[i].address;
            push.capacity = sources[i].capacity;
            push.values = recording->pool->values->address + uint64_t(first + i) * 8;
            push.phase = 1;
            push.flags = type == VK_QUERY_TYPE_MICROMAP_SERIALIZATION_SIZE_EXT ? 1 : 0;
            dispatch(b, cb, push);
        }
        barrier(b, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
        for (uint32_t i = 0; i < count; ++i)
            b->timestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, recording->pool->native, first + i);
        *out = std::move(recording);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VkResult properties_cmd_copy_results(PropertyBackend* b, VkCommandBuffer cb, uint32_t family, VkQueryPool handle, uint32_t first,
                                     uint32_t count, VkBuffer destination, VkDeviceSize offset, VkDeviceSize stride,
                                     VkQueryResultFlags flags, PropertyRecordingRef* out) {
    if (!b || !cb || !out) return VK_ERROR_VALIDATION_FAILED_EXT;
    uint64_t required;
    auto status = span(count, stride, flags, &required);
    if (status != VK_SUCCESS) return status;
    try {
        std::lock_guard<std::mutex> lock(b->mutex);
        status = queue(b, family);
        if (status != VK_SUCCESS) return status;
        auto recording = std::make_shared<PropertyRecording>();
        status = lookup(b, handle, first, count, &recording->pool);
        if (status != VK_SUCCESS) return status;
        if (!count) {
            *out = std::move(recording);
            return VK_SUCCESS;
        }
        VkBufferDeviceAddressInfo address{};
        address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        address.buffer = destination;
        const auto base = b->d->address(b->d->device, &address);
        VkDeviceSize logical, accessible;
        status = device_address_span(b->micromaps, base, &logical, &accessible);
        if (status != VK_SUCCESS || offset > logical || required > logical - offset ||
            (offset & ((flags & VK_QUERY_RESULT_64_BIT) ? 7 : 3)))
            return VK_ERROR_VALIDATION_FAILED_EXT;
        status = buffer(b, uint64_t(count) * 16, false, &recording->scratch);
        if (status != VK_SUCCESS) return status;
        const uint64_t record =
            ((flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4) * ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ? 2 : 1);
        status = buffer(b, uint64_t(count) * record, false, &recording->formatted);
        if (status != VK_SUCCESS) return status;
        std::vector<VkBufferCopy> regions(count);
        for (uint32_t i = 0; i < count; ++i) regions[i] = {uint64_t(i) * record, offset + uint64_t(i) * stride, record};
        Push push{};
        push.values = recording->pool->values->address + uint64_t(first) * 8;
        push.native_results = recording->scratch->address;
        push.destination = recording->formatted->address;
        push.stride = record;
        push.count = count;
        push.flags = flags;
        push.phase = 2;
        b->fill(cb, recording->scratch->handle, 0, VK_WHOLE_SIZE, 0);
        b->fill(cb, recording->formatted->handle, 0, VK_WHOLE_SIZE, 0);
        barrier(b, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        b->copy(cb, recording->pool->native, first, count, recording->scratch->handle, 0, 16,
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | (flags & VK_QUERY_RESULT_WAIT_BIT));
        barrier(b, cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        dispatch(b, cb, push);
        barrier(b, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        if (count) b->copy_buffer(cb, recording->formatted->handle, destination, count, regions.data());
        *out = std::move(recording);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
}  // namespace vel::omm
