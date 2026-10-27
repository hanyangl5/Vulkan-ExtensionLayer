// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_acceleration.h"
#include "omm_metadata.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace vel::omm {
namespace {
constexpr VkBuildAccelerationStructureFlagsKHR omm_flags = 0x1c0;
struct Dispatch {
    VkDevice device{};
    VkAllocationCallbacks callbacks{};
    bool custom{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::vector<uint32_t> queue_families;
    PFN_vkCreateBuffer create_buffer{};
    PFN_vkDestroyBuffer destroy_buffer{};
    PFN_vkGetBufferMemoryRequirements requirements{};
    PFN_vkAllocateMemory allocate{};
    PFN_vkFreeMemory free{};
    PFN_vkBindBufferMemory bind{};
    PFN_vkMapMemory map{};
    PFN_vkUnmapMemory unmap{};
    PFN_vkGetBufferDeviceAddress address{};
    const VkAllocationCallbacks* allocator() const { return custom ? &callbacks : nullptr; }
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
        if (handle) d->destroy_buffer(d->device, handle, d->allocator());
        if (memory) d->free(d->device, memory, d->allocator());
    }
};
using BufferRef = std::shared_ptr<Buffer>;
struct Object;
struct Body {
    BufferRef buffer;
    std::vector<std::shared_ptr<Object>> dependencies;
};
struct Object {
    VkAccelerationStructureKHR handle{};
    VkAccelerationStructureTypeKHR type{};
    VkDeviceAddress native{};
    BufferRef root;
    std::shared_ptr<Body> active_body;
    std::vector<std::weak_ptr<Body>> candidates;
};
struct RegistryEntry {
    uint64_t native, root;
};
struct InstancesPush {
    uint64_t input, output, body, status, registry, registry_count, count, generation;
    uint32_t build_flags, phase;
};
struct PublishPush {
    uint64_t root, body, capacity, status;
    uint32_t kind, reserved;
};
static_assert(sizeof(InstancesPush) == 72 && sizeof(PublishPush) == 40);
struct NativeInfo {
    VkAccelerationStructureBuildGeometryInfoKHR info{};
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<const VkAccelerationStructureTrianglesOpacityMicromapEXT*> associations;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
};
struct Job {
    NativeInfo native;
    std::shared_ptr<Object> object;
    std::shared_ptr<Body> body;
    BufferRef sources, status, instances, registry;
    MetadataBuildPush metadata{};
    InstancesPush instance_push{};
};
VkResult copy_info(const VkAccelerationStructureBuildGeometryInfoKHR* input, NativeInfo* output) {
    if (!input || input->sType != VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR ||
        (input->geometryCount && !input->pGeometries && !input->ppGeometries) || (input->pGeometries && input->ppGeometries))
        return VK_ERROR_VALIDATION_FAILED_EXT;
    if (input->mode != VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR || input->pNext ||
        (input->type != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR &&
         input->type != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    output->info = *input;
    output->info.flags &= ~omm_flags;
    output->geometries.resize(input->geometryCount);
    output->associations.resize(input->geometryCount);
    for (uint32_t i = 0; i < input->geometryCount; ++i) {
        auto* original = input->pGeometries ? &input->pGeometries[i] : input->ppGeometries[i];
        if (!original || original->sType != VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR)
            return VK_ERROR_VALIDATION_FAILED_EXT;
        auto& geometry = output->geometries[i];
        geometry = *original;
        if (geometry.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
            const auto* head = static_cast<const VkBaseInStructure*>(geometry.geometry.triangles.pNext);
            for (auto* node = head; node; node = node->pNext) {
                if (node->sType != VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT) continue;
                // Unknown extension structs preceding OMM cannot be cloned by
                // size. Fail closed instead of forwarding a software MM handle.
                if (node != head || output->associations[i]) return VK_ERROR_FEATURE_NOT_PRESENT;
                output->associations[i] = reinterpret_cast<const VkAccelerationStructureTrianglesOpacityMicromapEXT*>(node);
                geometry.geometry.triangles.pNext = node->pNext;
            }
        } else if (geometry.geometryType != VK_GEOMETRY_TYPE_AABBS_KHR && geometry.geometryType != VK_GEOMETRY_TYPE_INSTANCES_KHR)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if ((input->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR) !=
            (geometry.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR))
            return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    output->info.pGeometries = output->geometries.data();
    output->info.ppGeometries = nullptr;
    return VK_SUCCESS;
}
}  // namespace

struct AccelerationBackend {
    std::shared_ptr<Dispatch> d;
    DeviceBackend* micromaps{};
    std::mutex mutex;
    uint64_t generation = 0;
    std::unordered_map<VkAccelerationStructureKHR, std::shared_ptr<Object>> objects;
    VkPipeline pipelines[3]{};
    VkPipelineLayout layouts[3]{};
    PFN_vkCreateShaderModule create_shader{};
    PFN_vkDestroyShaderModule destroy_shader{};
    PFN_vkCreatePipelineLayout create_layout{};
    PFN_vkDestroyPipelineLayout destroy_layout{};
    PFN_vkCreateComputePipelines create_pipeline{};
    PFN_vkDestroyPipeline destroy_pipeline{};
    PFN_vkGetAccelerationStructureDeviceAddressKHR address_as{};
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes{};
    PFN_vkCmdBuildAccelerationStructuresKHR build{};
    PFN_vkCmdBindPipeline bind_pipeline{};
    PFN_vkCmdPushConstants push{};
    PFN_vkCmdDispatch dispatch{};
    PFN_vkCmdPipelineBarrier barrier{};
};
struct AccelerationRecording {
    std::vector<Job> jobs;
};

namespace {
VkResult buffer(AccelerationBackend* b, uint64_t size, bool host, VkBufferUsageFlags usage, BufferRef* out) {
    auto value = std::make_shared<Buffer>();
    value->d = b->d;
    value->size = std::max(uint64_t(16), (size + 15) & ~uint64_t(15));
    if (value->size < size) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = value->size;
    info.usage = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (b->d->queue_families.size() > 1) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = uint32_t(b->d->queue_families.size());
        info.pQueueFamilyIndices = b->d->queue_families.data();
    }
    VkResult result = b->d->create_buffer(b->d->device, &info, b->d->allocator(), &value->handle);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirements;
    b->d->requirements(b->d->device, value->handle, &requirements);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags required = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0;
    for (uint32_t i = 0; i < b->d->memory.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (b->d->memory.memoryTypes[i].propertyFlags & required) == required) {
            type = i;
            if (host || (b->d->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
        }
    }
    if (type == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.pNext = &flags;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    result = b->d->allocate(b->d->device, &allocate, b->d->allocator(), &value->memory);
    if (result != VK_SUCCESS) return result;
    result = b->d->bind(b->d->device, value->handle, value->memory, 0);
    if (result != VK_SUCCESS) return result;
    VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address.buffer = value->handle;
    value->address = b->d->address(b->d->device, &address);
    if (!value->address || value->size > UINT64_MAX - value->address) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (host) {
        result = b->d->map(b->d->device, value->memory, 0, VK_WHOLE_SIZE, 0, &value->mapped);
        if (result != VK_SUCCESS) return result;
        std::memset(value->mapped, 0, size_t(value->size));
    }
    *out = std::move(value);
    return VK_SUCCESS;
}
VkResult pipeline(AccelerationBackend* b, unsigned index, const uint32_t* code, size_t bytes, uint32_t push_size) {
    if (!code || !bytes || (bytes & 3)) return VK_ERROR_INITIALIZATION_FAILED;
    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_info.codeSize = bytes;
    shader_info.pCode = code;
    VkShaderModule shader{};
    auto result = b->create_shader(b->d->device, &shader_info, b->d->allocator(), &shader);
    if (result != VK_SUCCESS) return result;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    result = b->create_layout(b->d->device, &layout, b->d->allocator(), &b->layouts[index]);
    if (result == VK_SUCCESS) {
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.layout = b->layouts[index];
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = shader;
        info.stage.pName = "main";
        result = b->create_pipeline(b->d->device, VK_NULL_HANDLE, 1, &info, b->d->allocator(), &b->pipelines[index]);
    }
    b->destroy_shader(b->d->device, shader, b->d->allocator());
    return result;
}
void dependency(AccelerationBackend* b, VkCommandBuffer cb, VkPipelineStageFlags source_stage, VkAccessFlags source,
                VkPipelineStageFlags destination_stage, VkAccessFlags destination) {
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = source;
    memory.dstAccessMask = destination;
    b->barrier(cb, source_stage, destination_stage, 0, 1, &memory, 0, nullptr, 0, nullptr);
}
void dispatch(AccelerationBackend* b, VkCommandBuffer cb, unsigned pipeline_index, const void* push, uint32_t size, uint32_t x = 1,
              uint32_t y = 1) {
    b->bind_pipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, b->pipelines[pipeline_index]);
    b->push(cb, b->layouts[pipeline_index], VK_SHADER_STAGE_COMPUTE_BIT, 0, size, push);
    b->dispatch(cb, x, y, 1);
}
VkResult prepare_blas(AccelerationBackend* b, Job* job, const VkAccelerationStructureBuildGeometryInfoKHR& original) {
    const auto count = job->native.info.geometryCount;
    std::vector<MetadataGeometryPlan> plans(count);
    std::vector<MetadataGeometrySource> sources(count);
    std::vector<MetadataGeometry> geometries(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& geometry = job->native.geometries[i];
        auto& plan = plans[i];
        plan.geometry_type = geometry.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR ? kMetadataTriangles : kMetadataAabbs;
        plan.geometry_flags = geometry.flags;
        plan.primitive_count = job->native.ranges[i].primitiveCount;
        const auto* association = job->native.associations[i];
        if (!association) continue;
        plan.association = true;
        auto& source = sources[i];
        if (association->indexType == VK_INDEX_TYPE_NONE_KHR)
            source.index_width = 0;
        else if (association->indexType == VK_INDEX_TYPE_UINT16)
            source.index_width = 16;
        else if (association->indexType == VK_INDEX_TYPE_UINT32)
            source.index_width = 32;
        else
            return VK_ERROR_FEATURE_NOT_PRESENT;
        source.index_stride = association->indexStride;
        source.base_triangle = association->baseTriangle;
        if (source.index_width && plan.primitive_count) {
            VkDeviceSize logical, accessible;
            const auto address = association->indexBuffer.deviceAddress;
            auto result = device_address_span(b->micromaps, address, &logical, &accessible);
            if (result != VK_SUCCESS) return result;
            // The word reader needs a known aligned base. For an unaligned
            // interior address use its aligned predecessor only when tracked.
            uint64_t offset = address & 3;
            if (offset) {
                result = device_address_span(b->micromaps, address - offset, &logical, &accessible);
                if (result != VK_SUCCESS) return result;
            }
            if ((accessible & 3) || offset > logical || source.index_stride > UINT32_MAX ||
                (source.index_stride & (source.index_width / 8 - 1)))
                return VK_ERROR_FEATURE_NOT_PRESENT;
            const uint64_t n = plan.primitive_count - 1, width = source.index_width / 8;
            if ((source.index_stride && n > (UINT64_MAX - width) / source.index_stride) ||
                n * source.index_stride + width > logical - offset)
                return VK_ERROR_VALIDATION_FAILED_EXT;
            source.indices_address = address - offset;
            source.indices_accessible_bytes = accessible;
            source.indices_offset = offset;
            source.indices_bytes = logical - offset;
        }
        if (association->micromap) {
            MicromapStorage storage;
            auto result = device_micromap_storage(b->micromaps, association->micromap, &storage);
            if (result != VK_SUCCESS) return result;
            if ((storage.address & 3) || (storage.capacity & 15)) return VK_ERROR_FEATURE_NOT_PRESENT;
            plan.snapshot_capacity = storage.capacity;
            source.snapshot_address = storage.address;
            source.source_snapshot_capacity = storage.capacity;
        }
    }
    uint64_t bytes;
    if (metadata_plan_blas(plans.data(), count, geometries.data(), geometries.size(), &bytes) != MetadataResult::Success)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto result = buffer(b, bytes, false, 0, &job->body->buffer);
    if (result != VK_SUCCESS) return result;
    result = buffer(b, uint64_t(count) * sizeof(MetadataGeometrySource), true, 0, &job->sources);
    if (result != VK_SUCCESS) return result;
    for (uint32_t i = 0; i < count; ++i) sources[i].geometry = geometries[i];
    if (count) std::memcpy(job->sources->mapped, sources.data(), count * sizeof(MetadataGeometrySource));
    job->metadata = {job->sources->address,
                     job->body->buffer->address,
                     bytes,
                     job->status->address,
                     count,
                     ++b->generation,
                     0,
                     original.flags,
                     0};
    return VK_SUCCESS;
}
VkResult prepare_tlas(AccelerationBackend* b, Job* job, const VkAccelerationStructureBuildGeometryInfoKHR& original) {
    if (job->native.info.geometryCount != 1) return VK_ERROR_VALIDATION_FAILED_EXT;
    auto& geometry = job->native.geometries[0].geometry.instances;
    if (geometry.arrayOfPointers) return VK_ERROR_FEATURE_NOT_PRESENT;
    const uint64_t count = job->native.ranges[0].primitiveCount;
    const uint64_t offset = job->native.ranges[0].primitiveOffset;
    VkDeviceSize logical, accessible;
    const auto input = geometry.data.deviceAddress;
    if (count) {
        auto result = device_address_span(b->micromaps, input, &logical, &accessible);
        if (result != VK_SUCCESS) return result;
        if ((input & 3) || offset > logical || count > (logical - offset) / 64) return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    auto result = buffer(b, 64 + count * 32, false, 0, &job->body->buffer);
    if (result != VK_SUCCESS) return result;
    result = buffer(b, count * 64, false, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, &job->instances);
    if (result != VK_SUCCESS) return result;
    std::vector<RegistryEntry> entries;
    for (const auto& pair : b->objects) {
        if (pair.second->type != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR) continue;
        entries.push_back({pair.second->native, pair.second->root->address});
        job->body->dependencies.push_back(pair.second);
    }
    result = buffer(b, entries.size() * sizeof(RegistryEntry), true, 0, &job->registry);
    if (result != VK_SUCCESS) return result;
    if (!entries.empty()) std::memcpy(job->registry->mapped, entries.data(), entries.size() * sizeof(RegistryEntry));
    job->instance_push = {input + offset,
                          job->instances->address,
                          job->body->buffer->address,
                          job->status->address,
                          job->registry->address,
                          entries.size(),
                          count,
                          ++b->generation,
                          original.flags,
                          0};
    geometry.data.deviceAddress = job->instances->address;
    job->native.ranges[0].primitiveOffset = 0;
    return VK_SUCCESS;
}
void collect(Object& object) {
    const auto* root = static_cast<const MetadataRootCell*>(object.root->mapped);
    const uint64_t live = root->magic == kMetadataRootMagic && root->flags == kMetadataReady ? root->body_address : 0;
    // A still-executable old CB can republish a previously retired body. Its
    // recording token retains that body; weak history lets publication regain
    // AS ownership before the caller drops the completed command token.
    std::shared_ptr<Body> active;
    for (const auto& candidate : object.candidates) {
        auto body = candidate.lock();
        if (body && body->buffer->address == live) {
            active = std::move(body);
            break;
        }
    }
    object.active_body = std::move(active);
    object.candidates.erase(std::remove_if(object.candidates.begin(), object.candidates.end(),
                                           [](const std::weak_ptr<Body>& body) { return body.expired(); }),
                            object.candidates.end());
}
}  // namespace

VkResult acceleration_backend_create(const AccelerationBackendCreateInfo& info, AccelerationBackend** output) {
    if (!output || !info.physical_device || !info.device || !info.get_device_proc_addr || !info.micromaps)
        return VK_ERROR_INITIALIZATION_FAILED;
    *output = nullptr;
    AccelerationBackend* b = nullptr;
    try {
        b = new AccelerationBackend;
        b->d = std::make_shared<Dispatch>();
        b->d->device = info.device;
        b->micromaps = info.micromaps;
        if (info.allocator) {
            b->d->callbacks = *info.allocator;
            b->d->custom = true;
        }
        const auto get_memory = info.get_memory_properties ? info.get_memory_properties : vkGetPhysicalDeviceMemoryProperties;
        const auto get_families =
            info.get_queue_family_properties ? info.get_queue_family_properties : vkGetPhysicalDeviceQueueFamilyProperties;
        get_memory(info.physical_device, &b->d->memory);
        uint32_t family_count = 0;
        get_families(info.physical_device, &family_count, nullptr);
        b->d->queue_families.resize(family_count);
        for (uint32_t i = 0; i < family_count; ++i) b->d->queue_families[i] = i;
#define LOAD_D(field, name)                                                                               \
    do {                                                                                                  \
        b->d->field = reinterpret_cast<PFN_vk##name>(info.get_device_proc_addr(info.device, "vk" #name)); \
        if (!b->d->field) {                                                                               \
            acceleration_backend_destroy(b);                                                              \
            return VK_ERROR_INITIALIZATION_FAILED;                                                        \
        }                                                                                                 \
    } while (false)
        LOAD_D(create_buffer, CreateBuffer);
        LOAD_D(destroy_buffer, DestroyBuffer);
        LOAD_D(requirements, GetBufferMemoryRequirements);
        LOAD_D(allocate, AllocateMemory);
        LOAD_D(free, FreeMemory);
        LOAD_D(bind, BindBufferMemory);
        LOAD_D(map, MapMemory);
        LOAD_D(unmap, UnmapMemory);
        LOAD_D(address, GetBufferDeviceAddress);
#undef LOAD_D
#define LOAD(field, name)                                                                              \
    do {                                                                                               \
        b->field = reinterpret_cast<PFN_vk##name>(info.get_device_proc_addr(info.device, "vk" #name)); \
        if (!b->field) {                                                                               \
            acceleration_backend_destroy(b);                                                           \
            return VK_ERROR_INITIALIZATION_FAILED;                                                     \
        }                                                                                              \
    } while (false)
        LOAD(create_shader, CreateShaderModule);
        LOAD(destroy_shader, DestroyShaderModule);
        LOAD(create_layout, CreatePipelineLayout);
        LOAD(destroy_layout, DestroyPipelineLayout);
        LOAD(create_pipeline, CreateComputePipelines);
        LOAD(destroy_pipeline, DestroyPipeline);
        LOAD(address_as, GetAccelerationStructureDeviceAddressKHR);
        LOAD(sizes, GetAccelerationStructureBuildSizesKHR);
        LOAD(build, CmdBuildAccelerationStructuresKHR);
        LOAD(bind_pipeline, CmdBindPipeline);
        LOAD(push, CmdPushConstants);
        LOAD(dispatch, CmdDispatch);
        LOAD(barrier, CmdPipelineBarrier);
#undef LOAD
        auto result = pipeline(b, 0, info.metadata_code, info.metadata_code_size, sizeof(MetadataBuildPush));
        if (result == VK_SUCCESS) result = pipeline(b, 1, info.instances_code, info.instances_code_size, sizeof(InstancesPush));
        if (result == VK_SUCCESS) result = pipeline(b, 2, info.publish_code, info.publish_code_size, sizeof(PublishPush));
        if (result != VK_SUCCESS) {
            acceleration_backend_destroy(b);
            return result;
        }
        *output = b;
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        acceleration_backend_destroy(b);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void acceleration_backend_destroy(AccelerationBackend* b) {
    if (!b) return;
    b->objects.clear();
    if (b->d)
        for (unsigned i = 0; i < 3; ++i) {
            if (b->pipelines[i]) b->destroy_pipeline(b->d->device, b->pipelines[i], b->d->allocator());
            if (b->layouts[i]) b->destroy_layout(b->d->device, b->layouts[i], b->d->allocator());
        }
    delete b;
}
VkResult acceleration_track(AccelerationBackend* b, VkAccelerationStructureKHR handle,
                            const VkAccelerationStructureCreateInfoKHR* info) {
    if (!b || !handle || !info ||
        (info->type != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR &&
         info->type != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    try {
        std::lock_guard<std::mutex> lock(b->mutex);
        if (b->objects.count(handle)) return VK_ERROR_VALIDATION_FAILED_EXT;
        auto object = std::make_shared<Object>();
        object->handle = handle;
        object->type = info->type;
        VkAccelerationStructureDeviceAddressInfoKHR address{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        address.accelerationStructure = handle;
        object->native = b->address_as(b->d->device, &address);
        auto result = buffer(b, sizeof(MetadataRootCell), true, 0, &object->root);
        if (result != VK_SUCCESS) return result;
        b->objects.emplace(handle, std::move(object));
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void acceleration_untrack(AccelerationBackend* b, VkAccelerationStructureKHR handle) {
    if (!b) return;
    std::lock_guard<std::mutex> lock(b->mutex);
    b->objects.erase(handle);
}
VkResult acceleration_identity(AccelerationBackend* b, VkAccelerationStructureKHR handle, VkDeviceAddress* native_address,
                               VkDeviceAddress* root_address, std::shared_ptr<void>* lifetime) {
    if (!b || !native_address || !root_address) return VK_ERROR_VALIDATION_FAILED_EXT;
    std::lock_guard<std::mutex> lock(b->mutex);
    auto found = b->objects.find(handle);
    if (found == b->objects.end()) return VK_ERROR_VALIDATION_FAILED_EXT;
    *native_address = found->second->native;
    *root_address = found->second->root->address;
    if (lifetime) *lifetime = found->second;
    return VK_SUCCESS;
}
VkResult acceleration_root_from_address(AccelerationBackend* b, VkDeviceAddress native_address, VkDeviceAddress* root_address,
                                        std::shared_ptr<void>* lifetime) {
    if (!b || !native_address || !root_address) return VK_ERROR_VALIDATION_FAILED_EXT;
    std::lock_guard<std::mutex> lock(b->mutex);
    for (auto& pair : b->objects)
        if (pair.second->native == native_address) {
            *root_address = pair.second->root->address;
            if (lifetime) *lifetime = pair.second;
            return VK_SUCCESS;
        }
    return VK_ERROR_VALIDATION_FAILED_EXT;
}
VkResult acceleration_get_build_sizes(AccelerationBackend* b, VkAccelerationStructureBuildTypeKHR type,
                                      const VkAccelerationStructureBuildGeometryInfoKHR* input, const uint32_t* maxima,
                                      VkAccelerationStructureBuildSizesInfoKHR* sizes) {
    if (!b || !sizes || (input && input->geometryCount && !maxima)) return VK_ERROR_VALIDATION_FAILED_EXT;
    if (type != VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR) return VK_ERROR_FEATURE_NOT_PRESENT;
    try {
        NativeInfo native;
        auto result = copy_info(input, &native);
        if (result != VK_SUCCESS) return result;
        b->sizes(b->d->device, type, &native.info, maxima, sizes);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VkResult acceleration_cmd_build(AccelerationBackend* b, VkCommandBuffer cb, uint32_t count,
                                const VkAccelerationStructureBuildGeometryInfoKHR* infos,
                                const VkAccelerationStructureBuildRangeInfoKHR* const* ranges, AccelerationRecordingRef* out) {
    if (!b || !cb || !out || (count && (!infos || !ranges))) return VK_ERROR_VALIDATION_FAILED_EXT;
    try {
        std::lock_guard<std::mutex> lock(b->mutex);
        auto recording = std::make_shared<AccelerationRecording>();
        recording->jobs.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto& job = recording->jobs[i];
            auto result = copy_info(&infos[i], &job.native);
            if (result != VK_SUCCESS) return result;
            if (!ranges[i] && infos[i].geometryCount) return VK_ERROR_VALIDATION_FAILED_EXT;
            auto found = b->objects.find(infos[i].dstAccelerationStructure);
            if (found == b->objects.end() || found->second->type != infos[i].type) return VK_ERROR_VALIDATION_FAILED_EXT;
            job.object = found->second;
            job.body = std::make_shared<Body>();
            if (infos[i].geometryCount) job.native.ranges.assign(ranges[i], ranges[i] + infos[i].geometryCount);
            result = buffer(b, 16, false, 0, &job.status);
            if (result != VK_SUCCESS) return result;
            result = infos[i].type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR ? prepare_blas(b, &job, infos[i])
                                                                                      : prepare_tlas(b, &job, infos[i]);
            if (result != VK_SUCCESS) return result;
        }
        // A recording owns unpublished bodies; the submission tracker retains
        // every submitted recording until completion transfers published body
        // ownership to active_body. Keeping unpublished bodies on the AS would
        // leak every record/reset attempt that never reaches a queue.
        // Expired candidates cannot be republished; prune them without reading
        // a GPU root, even while another recording may still be in flight.
        for (auto& job : recording->jobs) {
            auto& candidates = job.object->candidates;
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                            [](const std::weak_ptr<Body>& candidate) { return candidate.expired(); }),
                             candidates.end());
            job.object->candidates.reserve(job.object->candidates.size() + count);
        }
        for (auto& job : recording->jobs) job.object->candidates.push_back(job.body);
        for (auto& job : recording->jobs) {
            dependency(b, cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            const bool blas = job.native.info.type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            if (blas) {
                dispatch(b, cb, 0, &job.metadata, sizeof(job.metadata));
                dependency(b, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                job.metadata.phase = 1;
                for (uint64_t base = 0; base < job.metadata.geometry_count; base += 65535) {
                    job.metadata.geometry_base = base;
                    dispatch(b, cb, 0, &job.metadata, sizeof(job.metadata), 128,
                             uint32_t(std::min(uint64_t(65535), job.metadata.geometry_count - base)));
                }
                job.metadata.phase = 0;
            } else {
                dispatch(b, cb, 1, &job.instance_push, sizeof(job.instance_push));
                dependency(b, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                job.instance_push.phase = 1;
                dispatch(b, cb, 1, &job.instance_push, sizeof(job.instance_push), 128);
                job.instance_push.phase = 0;
            }
            dependency(b, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
            PublishPush publication{job.object->root->address,
                                    job.body->buffer->address,
                                    job.body->buffer->size,
                                    job.status->address,
                                    blas ? kMetadataBlas : kMetadataTlas,
                                    0};
            dispatch(b, cb, 2, &publication, sizeof(publication));
            const auto* range = job.native.ranges.data();
            b->build(cb, 1, &job.native.info, &range);
            dependency(b, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
        }
        *out = std::move(recording);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void acceleration_collect_idle(AccelerationBackend* b) {
    if (!b) return;
    std::lock_guard<std::mutex> lock(b->mutex);
    for (auto& pair : b->objects) collect(*pair.second);
}
void acceleration_collect_completed(AccelerationBackend* b, const AccelerationRecordingRef& recording) {
    if (!b || !recording) return;
    std::lock_guard<std::mutex> lock(b->mutex);
    for (const auto& job : recording->jobs) collect(*job.object);
}
bool acceleration_recordings_overlap(const AccelerationRecordingRef& a, const AccelerationRecordingRef& b) noexcept {
    if (!a || !b) return false;
    for (const auto& left : a->jobs)
        for (const auto& right : b->jobs)
            if (left.object.get() == right.object.get()) return true;
    return false;
}
}  // namespace vel::omm
