// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_device.h"
#include "omm_storage.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

namespace vel::omm {
namespace {
constexpr uint32_t kMaximumSubdivision = 12;
constexpr VkDeviceSize kScratchSize = 256;
// Bump this UUID when the pointer-free canonical storage contract changes.
constexpr uint8_t kStorageCompatibilityUuid[VK_UUID_SIZE] = {0xe8, 0xf6, 0x8c, 0x3a, 0xbd, 0x19, 0x4d, 0x79,
                                                             0x92, 0x8b, 0x53, 0x2e, 0x01, 0x00, 0x00, 0x01};

struct Allocation {
    VkAllocationCallbacks callbacks{};
    bool custom = false;
};
Allocation allocation(const VkAllocationCallbacks* callbacks) {
    Allocation result;
    if (callbacks) {
        result.callbacks = *callbacks;
        result.custom = true;
    }
    return result;
}
void* allocate(const Allocation& allocator, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    return allocator.custom ? allocator.callbacks.pfnAllocation(allocator.callbacks.pUserData, size, alignment, scope)
                            : std::malloc(size);
}
void release(const Allocation& allocator, void* memory) {
    if (allocator.custom)
        allocator.callbacks.pfnFree(allocator.callbacks.pUserData, memory);
    else
        std::free(memory);
}
const VkAllocationCallbacks* callback_pointer(const Allocation& allocator) {
    return allocator.custom ? &allocator.callbacks : nullptr;
}

struct BufferRecord {
    BufferRecord* next = nullptr;
    VkBuffer handle{};
    VkDeviceSize size = 0, accessible = 0;
    VkBufferCreateFlags flags = 0;
    VkBufferUsageFlags2KHR usage = 0, native_usage = 0;
    bool bound = false;
};
struct Micromap {
    Micromap* next = nullptr;
    VkMicromapEXT handle{};
    VkBuffer buffer{};
    VkDeviceSize offset = 0, capacity = 0;
    Allocation allocator;
};
struct BuildPush {
    uint64_t triangles, data, output, status, triangles_size, data_size, output_size, count, stride;
    uint64_t triangles_accessible, data_accessible, phase;
};
struct CopyPush {
    uint64_t source, destination, source_capacity, destination_capacity;
};
struct SerializePush {
    uint64_t source, destination, source_capacity, destination_capacity;
    uint32_t version[2 * VK_UUID_SIZE / sizeof(uint32_t)];
    uint64_t deserialize;
};
static_assert(sizeof(BuildPush) == 96 && sizeof(CopyPush) == 32 && sizeof(SerializePush) == 72);

bool add(uint64_t a, uint64_t b, uint64_t* output) {
    if (b > UINT64_MAX - a) return false;
    *output = a + b;
    return true;
}
bool multiply(uint64_t a, uint64_t b, uint64_t* output) {
    if (a && b > UINT64_MAX / a) return false;
    *output = a * b;
    return true;
}
bool overlap(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size) {
    return a_size && b_size && a < b + b_size && b < a + a_size;
}

VkResult plan(const VkMicromapBuildInfoEXT* info, StorageLayout* output) {
    if (!info || info->sType != VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT) return VK_ERROR_VALIDATION_FAILED_EXT;
    if (info->pNext || info->type != VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT || info->mode != VK_BUILD_MICROMAP_MODE_BUILD_EXT ||
        (info->flags & ~VkBuildMicromapFlagsEXT(7)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if ((info->flags & 3) == 3 || (info->pUsageCounts && info->ppUsageCounts) ||
        (info->usageCountsCount && !info->pUsageCounts && !info->ppUsageCounts))
        return VK_ERROR_VALIDATION_FAILED_EXT;
    uint64_t count = 0, payload = 0;
    for (uint32_t i = 0; i < info->usageCountsCount; ++i) {
        const auto* usage = info->pUsageCounts ? &info->pUsageCounts[i] : info->ppUsageCounts[i];
        if (!usage) return VK_ERROR_VALIDATION_FAILED_EXT;
        if (usage->subdivisionLevel > kMaximumSubdivision) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        uint64_t bytes, span;
        if (storage_data_size(StorageFormat(usage->format), usage->subdivisionLevel, &bytes) != StorageResult::Success)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        if (!add(bytes, 7, &bytes) || !multiply(bytes & ~uint64_t(7), usage->count, &span) || !add(payload, span, &payload) ||
            !add(count, usage->count, &count))
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    uint64_t records, data_begin, total;
    if (!multiply(count, kStorageRecordSize, &records) || !add(records, kStorageHeaderSize + 15, &data_begin) ||
        !add(data_begin & ~uint64_t(15), payload, &total) || !add(total, 15, &total))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *output = {count, kStorageHeaderSize, data_begin & ~uint64_t(15), total & ~uint64_t(15)};
    return VK_SUCCESS;
}
}  // namespace

struct DeviceBackend {
    VkPhysicalDevice physical{};
    VkDevice device{};
    Allocation allocator;
    std::mutex mutex;
    BufferRecord* buffers = nullptr;
    Micromap* micromaps = nullptr;
    VkPipeline build_pipeline{}, copy_pipeline{}, serialize_pipeline{};
    VkPipelineLayout build_layout{}, copy_layout{}, serialize_layout{};
    uint8_t serialization_version[2 * VK_UUID_SIZE]{};
    PFN_vkCreateShaderModule create_shader{};
    PFN_vkDestroyShaderModule destroy_shader{};
    PFN_vkCreatePipelineLayout create_layout{};
    PFN_vkDestroyPipelineLayout destroy_layout{};
    PFN_vkCreateComputePipelines create_pipeline{};
    PFN_vkDestroyPipeline destroy_pipeline{};
    PFN_vkGetBufferDeviceAddress get_address{};
    PFN_vkCmdBindPipeline bind_pipeline{};
    PFN_vkCmdPushConstants push_constants{};
    PFN_vkCmdDispatch dispatch{};
    PFN_vkCmdPipelineBarrier barrier{};

    BufferRecord* buffer(VkBuffer handle) {
        for (auto* entry = buffers; entry; entry = entry->next)
            if (entry->handle == handle) return entry;
        return nullptr;
    }
    Micromap* micromap(VkMicromapEXT handle) {
        for (auto* entry = micromaps; entry; entry = entry->next)
            if (entry->handle == handle) return entry;
        return nullptr;
    }
    bool buffer_address(BufferRecord* entry, VkDeviceAddress* address) {
        if (!entry || !entry->bound ||
            !(entry->native_usage & (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT)))
            return false;
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = entry->handle;
        *address = get_address(device, &info);
        return *address && entry->accessible <= UINT64_MAX - *address;
    }
    bool address_range(VkDeviceAddress address, VkDeviceSize* logical, VkDeviceSize* accessible) {
        VkDeviceSize largest_logical = 0, selected_accessible = 0;
        for (auto* entry = buffers; entry; entry = entry->next) {
            VkDeviceAddress base;
            if (!buffer_address(entry, &base) || address < base || address - base >= entry->size) continue;
            const uint64_t offset = address - base;
            // Aliased buffers may have overlapping device addresses. Vulkan
            // selects a buffer that satisfies valid usage when one exists; an
            // earlier small alias must not truncate a later valid large span.
            if (entry->size - offset > largest_logical) {
                largest_logical = entry->size - offset;
                selected_accessible = entry->accessible - offset;
            }
        }
        if (!largest_logical) return false;
        *logical = largest_logical;
        *accessible = selected_accessible;
        return true;
    }
    VkResult storage(VkMicromapEXT handle, MicromapStorage* output) {
        auto* object = micromap(handle);
        if (!object) return VK_ERROR_VALIDATION_FAILED_EXT;
        VkDeviceAddress base;
        if (!buffer_address(buffer(object->buffer), &base)) return VK_ERROR_VALIDATION_FAILED_EXT;
        *output = {object->buffer, object->offset, object->capacity, base + object->offset};
        return VK_SUCCESS;
    }
    void dependency(VkCommandBuffer command, VkAccessFlags source, VkAccessFlags destination) {
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memory.srcAccessMask = source;
        memory.dstAccessMask = destination;
        barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0,
                nullptr);
    }
    void record_serialization(VkCommandBuffer command, SerializePush& push) {
        std::memcpy(push.version, serialization_version, sizeof(push.version));
        bind_pipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, serialize_pipeline);
        push_constants(command, serialize_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        dispatch(command, 128, 1, 1);
    }
    VkResult prepare_build(const VkMicromapBuildInfoEXT& info, BuildPush* push) {
        StorageLayout layout;
        VkResult result = plan(&info, &layout);
        if (result != VK_SUCCESS) return result;
        MicromapStorage destination;
        result = storage(info.dstMicromap, &destination);
        if (result != VK_SUCCESS) return result;
        if (layout.size > destination.capacity || (destination.address & 3)) return VK_ERROR_VALIDATION_FAILED_EXT;
        BuildPush prepared{};
        prepared.output = destination.address;
        prepared.output_size = layout.size;
        prepared.count = layout.triangle_count;
        prepared.stride = info.triangleArrayStride;
        prepared.status = info.scratchData.deviceAddress;
        uint64_t scratch_size, scratch_accessible;
        if ((prepared.status & 255) || !address_range(prepared.status, &scratch_size, &scratch_accessible) ||
            scratch_size < kScratchSize)
            return VK_ERROR_VALIDATION_FAILED_EXT;
        if (layout.triangle_count) {
            prepared.triangles = info.triangleArray.deviceAddress;
            prepared.data = info.data.deviceAddress;
            if ((prepared.triangles & 3) || (prepared.data & 3) ||
                !address_range(prepared.triangles, &prepared.triangles_size, &prepared.triangles_accessible) ||
                !address_range(prepared.data, &prepared.data_size, &prepared.data_accessible))
                return VK_ERROR_VALIDATION_FAILED_EXT;
            if (prepared.stride < 8) return VK_ERROR_VALIDATION_FAILED_EXT;
            uint64_t required;
            if (!multiply(prepared.count - 1, prepared.stride, &required) || !add(required, 8, &required) ||
                required > prepared.triangles_size)
                return VK_ERROR_VALIDATION_FAILED_EXT;
            // Word-access padding is explicit; a valid tiny input is not silently
            // sent to a kernel that could read beyond its native buffer range.
            if ((prepared.triangles_accessible & 3) || (prepared.data_accessible & 3)) return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (overlap(prepared.output, prepared.output_size, prepared.status, 4) ||
            overlap(prepared.output, prepared.output_size, prepared.triangles, prepared.triangles_accessible) ||
            overlap(prepared.output, prepared.output_size, prepared.data, prepared.data_accessible) ||
            overlap(prepared.status, 4, prepared.triangles, prepared.triangles_accessible) ||
            overlap(prepared.status, 4, prepared.data, prepared.data_accessible))
            return VK_ERROR_VALIDATION_FAILED_EXT;
        *push = prepared;
        return VK_SUCCESS;
    }
};

namespace {
VkResult make_pipeline(DeviceBackend* backend, const uint32_t* code, size_t bytes, uint32_t push_size, VkPipelineLayout* layout,
                       VkPipeline* pipeline) {
    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_info.codeSize = bytes;
    shader_info.pCode = code;
    VkShaderModule shader{};
    VkResult result = backend->create_shader(backend->device, &shader_info, callback_pointer(backend->allocator), &shader);
    if (result != VK_SUCCESS) return result;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    result = backend->create_layout(backend->device, &layout_info, callback_pointer(backend->allocator), layout);
    if (result == VK_SUCCESS) {
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = shader;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = *layout;
        result = backend->create_pipeline(backend->device, VK_NULL_HANDLE, 1, &pipeline_info, callback_pointer(backend->allocator),
                                          pipeline);
    }
    backend->destroy_shader(backend->device, shader, callback_pointer(backend->allocator));
    return result;
}
}  // namespace

VkResult device_backend_create(const DeviceBackendCreateInfo& info, DeviceBackend** output) {
    if (!output || !info.physical_device || !info.device || !info.get_device_proc_addr || !info.build_code || !info.copy_code ||
        !info.build_code_size || info.build_code_size % 4 || !info.copy_code_size || info.copy_code_size % 4 ||
        (info.serialize_code && (!info.serialize_code_size || info.serialize_code_size % 4 || !info.get_properties2)) ||
        (!info.serialize_code && info.serialize_code_size))
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto allocator = allocation(info.allocator);
    void* memory = allocate(allocator, sizeof(DeviceBackend), alignof(DeviceBackend), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (!memory) return VK_ERROR_OUT_OF_HOST_MEMORY;
    auto* backend = new (memory) DeviceBackend;
    backend->allocator = allocator;
    backend->physical = info.physical_device;
    backend->device = info.device;
#define LOAD(member, name)                                                                             \
    do {                                                                                               \
        backend->member = reinterpret_cast<PFN_##name>(info.get_device_proc_addr(info.device, #name)); \
    } while (false)
    LOAD(create_shader, vkCreateShaderModule);
    LOAD(destroy_shader, vkDestroyShaderModule);
    LOAD(create_layout, vkCreatePipelineLayout);
    LOAD(destroy_layout, vkDestroyPipelineLayout);
    LOAD(create_pipeline, vkCreateComputePipelines);
    LOAD(destroy_pipeline, vkDestroyPipeline);
    LOAD(get_address, vkGetBufferDeviceAddress);
    LOAD(bind_pipeline, vkCmdBindPipeline);
    LOAD(push_constants, vkCmdPushConstants);
    LOAD(dispatch, vkCmdDispatch);
    LOAD(barrier, vkCmdPipelineBarrier);
#undef LOAD
    if (!backend->create_shader || !backend->destroy_shader || !backend->create_layout || !backend->destroy_layout ||
        !backend->create_pipeline || !backend->destroy_pipeline || !backend->get_address || !backend->bind_pipeline ||
        !backend->push_constants || !backend->dispatch || !backend->barrier) {
        device_backend_destroy(backend);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkResult result = make_pipeline(backend, info.build_code, info.build_code_size, sizeof(BuildPush), &backend->build_layout,
                                    &backend->build_pipeline);
    if (result == VK_SUCCESS)
        result = make_pipeline(backend, info.copy_code, info.copy_code_size, sizeof(CopyPush), &backend->copy_layout,
                               &backend->copy_pipeline);
    if (result == VK_SUCCESS && info.serialize_code) {
        VkPhysicalDeviceIDProperties identifiers{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identifiers};
        info.get_properties2(info.physical_device, &properties);
        std::memcpy(backend->serialization_version, identifiers.driverUUID, VK_UUID_SIZE);
        std::memcpy(backend->serialization_version + VK_UUID_SIZE, kStorageCompatibilityUuid, VK_UUID_SIZE);
        result = make_pipeline(backend, info.serialize_code, info.serialize_code_size, sizeof(SerializePush),
                               &backend->serialize_layout, &backend->serialize_pipeline);
    }
    if (result != VK_SUCCESS) {
        device_backend_destroy(backend);
        return result;
    }
    *output = backend;
    return VK_SUCCESS;
}

void device_backend_destroy(DeviceBackend* backend) {
    if (!backend) return;
    while (backend->micromaps) {
        auto* object = backend->micromaps;
        backend->micromaps = object->next;
        const auto allocator = object->allocator;
        object->~Micromap();
        release(allocator, object);
    }
    while (backend->buffers) {
        auto* entry = backend->buffers;
        backend->buffers = entry->next;
        release(backend->allocator, entry);
    }
    if (backend->copy_pipeline)
        backend->destroy_pipeline(backend->device, backend->copy_pipeline, callback_pointer(backend->allocator));
    if (backend->serialize_pipeline)
        backend->destroy_pipeline(backend->device, backend->serialize_pipeline, callback_pointer(backend->allocator));
    if (backend->build_pipeline)
        backend->destroy_pipeline(backend->device, backend->build_pipeline, callback_pointer(backend->allocator));
    if (backend->copy_layout) backend->destroy_layout(backend->device, backend->copy_layout, callback_pointer(backend->allocator));
    if (backend->serialize_layout)
        backend->destroy_layout(backend->device, backend->serialize_layout, callback_pointer(backend->allocator));
    if (backend->build_layout)
        backend->destroy_layout(backend->device, backend->build_layout, callback_pointer(backend->allocator));
    const auto allocator = backend->allocator;
    backend->~DeviceBackend();
    release(allocator, backend);
}

VkResult device_track_buffer(DeviceBackend* backend, VkBuffer handle, const VkBufferCreateInfo* info, VkDeviceSize accessible,
                             VkBufferUsageFlags2 native_usage) {
    if (!backend || !handle || !info || info->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO || !info->size ||
        (accessible && accessible < info->size))
        return VK_ERROR_VALIDATION_FAILED_EXT;
    std::lock_guard<std::mutex> lock(backend->mutex);
    if (backend->buffer(handle)) return VK_ERROR_VALIDATION_FAILED_EXT;
    void* memory = allocate(backend->allocator, sizeof(BufferRecord), alignof(BufferRecord), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (!memory) return VK_ERROR_OUT_OF_HOST_MEMORY;
    auto* entry = new (memory) BufferRecord;
    entry->handle = handle;
    entry->size = info->size;
    entry->accessible = accessible ? accessible : info->size;
    entry->flags = info->flags;
    entry->usage = info->usage;
    for (auto* next = reinterpret_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR)
            entry->usage = reinterpret_cast<const VkBufferUsageFlags2CreateInfoKHR*>(next)->usage;
    entry->native_usage = native_usage ? native_usage : entry->usage;
    entry->next = backend->buffers;
    backend->buffers = entry;
    return VK_SUCCESS;
}
void device_buffer_bound(DeviceBackend* backend, VkBuffer handle) {
    if (!backend) return;
    std::lock_guard<std::mutex> lock(backend->mutex);
    if (auto* entry = backend->buffer(handle)) entry->bound = true;
}
void device_untrack_buffer(DeviceBackend* backend, VkBuffer handle) {
    if (!backend) return;
    std::lock_guard<std::mutex> lock(backend->mutex);
    auto** link = &backend->buffers;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    if (*link) {
        auto* entry = *link;
        *link = entry->next;
        release(backend->allocator, entry);
    }
}
VkResult device_buffer_bound_span(DeviceBackend* backend, VkBuffer handle, VkDeviceSize offset, VkDeviceSize size) {
    if (!backend) return VK_ERROR_VALIDATION_FAILED_EXT;
    std::lock_guard<std::mutex> lock(backend->mutex);
    auto* entry = backend->buffer(handle);
    return entry && entry->bound && offset <= entry->size && size <= entry->size - offset ? VK_SUCCESS
                                                                                          : VK_ERROR_VALIDATION_FAILED_EXT;
}
VkResult device_buffer_as_address_span(DeviceBackend* backend, VkBuffer handle, VkDeviceSize offset, VkDeviceSize size) {
    if (!backend) return VK_ERROR_VALIDATION_FAILED_EXT;
    std::lock_guard<std::mutex> lock(backend->mutex);
    auto* entry = backend->buffer(handle);
    return entry && (entry->bound || (entry->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)) && offset <= entry->size &&
                   size <= entry->size - offset
               ? VK_SUCCESS
               : VK_ERROR_VALIDATION_FAILED_EXT;
}

VkResult device_create_micromap(DeviceBackend* backend, const VkMicromapCreateInfoEXT* info, const VkAllocationCallbacks* callbacks,
                                VkMicromapEXT* output) {
    if (!backend || !info || !output || info->sType != VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    if (info->pNext || info->createFlags || info->deviceAddress || info->type != VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    std::lock_guard<std::mutex> lock(backend->mutex);
    auto* entry = backend->buffer(info->buffer);
    if (!entry || !(entry->usage & VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT) || (info->offset & 255) ||
        info->size < kStorageHeaderSize || info->offset > entry->size || info->size > entry->size - info->offset)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    const auto allocator = callbacks ? allocation(callbacks) : backend->allocator;
    void* memory = allocate(allocator, sizeof(Micromap), alignof(Micromap), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!memory) return VK_ERROR_OUT_OF_HOST_MEMORY;
    auto* object = new (memory) Micromap;
    object->allocator = allocator;
    object->buffer = info->buffer;
    object->offset = info->offset;
    object->capacity = info->size;
#if VK_USE_64_BIT_PTR_DEFINES
    object->handle = reinterpret_cast<VkMicromapEXT>(object);
#else
    object->handle = static_cast<VkMicromapEXT>(reinterpret_cast<uintptr_t>(object));
#endif
    object->next = backend->micromaps;
    backend->micromaps = object;
    *output = object->handle;
    return VK_SUCCESS;
}
void device_destroy_micromap(DeviceBackend* backend, VkMicromapEXT handle, const VkAllocationCallbacks*) {
    if (!backend || !handle) return;
    std::lock_guard<std::mutex> lock(backend->mutex);
    auto** link = &backend->micromaps;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    if (!*link) return;
    auto* object = *link;
    *link = object->next;
    const auto allocator = object->allocator;
    object->~Micromap();
    release(allocator, object);
}
VkResult device_get_build_sizes(DeviceBackend* backend, VkAccelerationStructureBuildTypeKHR type,
                                const VkMicromapBuildInfoEXT* info, VkMicromapBuildSizesInfoEXT* sizes) {
    if (!backend || !sizes || sizes->sType != VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    if (type != VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR) return VK_ERROR_FEATURE_NOT_PRESENT;
    StorageLayout layout;
    const auto result = plan(info, &layout);
    if (result != VK_SUCCESS) return result;
    sizes->micromapSize = layout.size;
    sizes->buildScratchSize = kScratchSize;
    sizes->discardable = VK_FALSE;
    return VK_SUCCESS;
}
VkResult device_micromap_storage(DeviceBackend* backend, VkMicromapEXT handle, MicromapStorage* output) {
    if (!backend || !output) return VK_ERROR_VALIDATION_FAILED_EXT;
    std::lock_guard<std::mutex> lock(backend->mutex);
    return backend->storage(handle, output);
}
VkResult device_address_span(DeviceBackend* backend, VkDeviceAddress address, VkDeviceSize* logical, VkDeviceSize* accessible) {
    if (!backend || !logical || !accessible) return VK_ERROR_VALIDATION_FAILED_EXT;
    std::lock_guard<std::mutex> lock(backend->mutex);
    return backend->address_range(address, logical, accessible) ? VK_SUCCESS : VK_ERROR_VALIDATION_FAILED_EXT;
}
VkResult device_cmd_build(DeviceBackend* backend, VkCommandBuffer command, uint32_t count, const VkMicromapBuildInfoEXT* infos) {
    if (!backend || !command || (count && !infos)) return VK_ERROR_VALIDATION_FAILED_EXT;
    std::lock_guard<std::mutex> lock(backend->mutex);
    for (uint32_t i = 0; i < count; ++i) {
        BuildPush ignored;
        const auto result = backend->prepare_build(infos[i], &ignored);
        if (result != VK_SUCCESS) return result;
    }
    for (uint32_t i = 0; i < count; ++i) {
        BuildPush push;
        const auto result = backend->prepare_build(infos[i], &push);
        if (result != VK_SUCCESS) return result;
        backend->bind_pipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, backend->build_pipeline);
        backend->push_constants(command, backend->build_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        backend->dispatch(command, 1, 1, 1);
        backend->dependency(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        push.phase = 1;
        backend->push_constants(command, backend->build_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        backend->dispatch(command, 128, 1, 1);
        if (i + 1 < count)
            backend->dependency(command, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    return VK_SUCCESS;
}
VkResult device_cmd_copy(DeviceBackend* backend, VkCommandBuffer command, const VkCopyMicromapInfoEXT* info) {
    if (!backend || !command || !info || info->sType != VK_STRUCTURE_TYPE_COPY_MICROMAP_INFO_EXT)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    if (info->pNext || (info->mode != VK_COPY_MICROMAP_MODE_CLONE_EXT && info->mode != VK_COPY_MICROMAP_MODE_COMPACT_EXT))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    std::lock_guard<std::mutex> lock(backend->mutex);
    MicromapStorage source, destination;
    auto result = backend->storage(info->src, &source);
    if (result != VK_SUCCESS) return result;
    result = backend->storage(info->dst, &destination);
    if (result != VK_SUCCESS) return result;
    if (overlap(source.address, source.capacity, destination.address, destination.capacity)) return VK_ERROR_VALIDATION_FAILED_EXT;
    CopyPush push{source.address, destination.address, source.capacity, destination.capacity};
    backend->bind_pipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, backend->copy_pipeline);
    backend->push_constants(command, backend->copy_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    backend->dispatch(command, 128, 1, 1);
    return VK_SUCCESS;
}
VkResult device_cmd_serialize(DeviceBackend* backend, VkCommandBuffer command, const VkCopyMicromapToMemoryInfoEXT* info) {
    if (!backend || !command || !info || info->sType != VK_STRUCTURE_TYPE_COPY_MICROMAP_TO_MEMORY_INFO_EXT)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    if (info->pNext || info->mode != VK_COPY_MICROMAP_MODE_SERIALIZE_EXT) return VK_ERROR_FEATURE_NOT_PRESENT;
    std::lock_guard<std::mutex> lock(backend->mutex);
    if (!backend->serialize_pipeline) return VK_ERROR_FEATURE_NOT_PRESENT;
    MicromapStorage source;
    auto result = backend->storage(info->src, &source);
    if (result != VK_SUCCESS) return result;
    SerializePush push{};
    push.source = source.address;
    push.source_capacity = source.capacity;
    push.destination = info->dst.deviceAddress;
    VkDeviceSize accessible;
    if ((push.source & 3) || (push.destination & 255) ||
        !backend->address_range(push.destination, &push.destination_capacity, &accessible) ||
        push.destination_capacity < 2 * VK_UUID_SIZE + kStorageHeaderSize)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    backend->record_serialization(command, push);
    return VK_SUCCESS;
}
VkResult device_cmd_deserialize(DeviceBackend* backend, VkCommandBuffer command, const VkCopyMemoryToMicromapInfoEXT* info) {
    if (!backend || !command || !info || info->sType != VK_STRUCTURE_TYPE_COPY_MEMORY_TO_MICROMAP_INFO_EXT)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    if (info->pNext || info->mode != VK_COPY_MICROMAP_MODE_DESERIALIZE_EXT) return VK_ERROR_FEATURE_NOT_PRESENT;
    std::lock_guard<std::mutex> lock(backend->mutex);
    if (!backend->serialize_pipeline) return VK_ERROR_FEATURE_NOT_PRESENT;
    MicromapStorage destination;
    auto result = backend->storage(info->dst, &destination);
    if (result != VK_SUCCESS) return result;
    SerializePush push{};
    push.source = info->src.deviceAddress;
    push.destination = destination.address;
    push.destination_capacity = destination.capacity;
    push.deserialize = 1;
    VkDeviceSize accessible;
    if ((push.source & 255) || (push.destination & 3) || !backend->address_range(push.source, &push.source_capacity, &accessible) ||
        push.source_capacity < 2 * VK_UUID_SIZE + kStorageHeaderSize)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    backend->record_serialization(command, push);
    return VK_SUCCESS;
}
void device_get_compatibility(DeviceBackend* backend, const VkMicromapVersionInfoEXT* info,
                              VkAccelerationStructureCompatibilityKHR* compatibility) {
    if (!compatibility) return;
    *compatibility = VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR;
    if (backend && backend->serialize_pipeline && info && info->sType == VK_STRUCTURE_TYPE_MICROMAP_VERSION_INFO_EXT &&
        !info->pNext && info->pVersionData &&
        !std::memcmp(info->pVersionData, backend->serialization_version, sizeof(backend->serialization_version)))
        *compatibility = VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR;
}
}  // namespace vel::omm
