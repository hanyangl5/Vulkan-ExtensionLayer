// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "native_fixture.h"
#include "../omm_acceleration.h"
#include "../omm_submissions.h"
#include "../omm_storage.h"
#include "../omm_descriptors.h"
#include <string>
#include <unordered_map>
using namespace vel::omm;
namespace {
unsigned assertions = 0, native_builds = 0, native_updates = 0, sanitized_queries = 0;
unsigned live_private_buffers = 0;
unsigned live_private_memory = 0;
uint32_t physical_family_count = 0;
PFN_vkCmdBuildAccelerationStructuresKHR native_build{};
PFN_vkGetAccelerationStructureBuildSizesKHR native_sizes{};
PFN_vkCreateBuffer native_create_buffer{};
PFN_vkDestroyBuffer native_destroy_buffer{};
PFN_vkAllocateMemory native_allocate_memory{};
PFN_vkFreeMemory native_free_memory{};
PFN_vkCreateAccelerationStructureKHR native_create_as{};
PFN_vkGetAccelerationStructureDeviceAddressKHR native_as_address{};
std::unordered_map<VkAccelerationStructureKHR, VkAccelerationStructureCreateInfoKHR> creations;
DeviceBackend* tracked_buffers{};
unsigned address_queries = 0;
bool fail_private_buffer = false;
VkDeviceAddress original_instances{};
const VkAccelerationStructureBuildGeometryInfoKHR* expected_update{};
void require(bool value, const char* reason) {
    ++assertions;
    if (!value) {
        std::fprintf(stderr, "FAIL acceleration: %s\n", reason);
        std::exit(1);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL spy_create_as(VkDevice device, const VkAccelerationStructureCreateInfoKHR* info,
                                             const VkAllocationCallbacks* allocator, VkAccelerationStructureKHR* out) {
    const auto result = native_create_as(device, info, allocator, out);
    if (result == VK_SUCCESS) creations[*out] = *info;
    return result;
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL spy_as_address(VkDevice device, const VkAccelerationStructureDeviceAddressInfoKHR* info) {
    const auto& creation = creations.at(info->accelerationStructure);
    require(device_buffer_as_address_span(tracked_buffers, creation.buffer, creation.offset, creation.size) == VK_SUCCESS,
            "backend queries native AS addresses only for legal backing storage");
    ++address_queries;
    return native_as_address(device, info);
}
VKAPI_ATTR void VKAPI_CALL spy_build(VkCommandBuffer cb, uint32_t count, const VkAccelerationStructureBuildGeometryInfoKHR* infos,
                                     const VkAccelerationStructureBuildRangeInfoKHR* const* ranges) {
    for (uint32_t i = 0; i < count; ++i) {
        require((infos[i].flags & 0x1c0) == 0, "native AS build strips all OMM flags");
        if (expected_update) {
            require(infos[i].mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR &&
                        infos[i].srcAccelerationStructure == expected_update->srcAccelerationStructure &&
                        infos[i].dstAccelerationStructure == expected_update->dstAccelerationStructure &&
                        infos[i].scratchData.deviceAddress == expected_update->scratchData.deviceAddress &&
                        infos[i].flags == (expected_update->flags & ~VkBuildAccelerationStructureFlagsKHR(0x1c0)),
                    "native UPDATE preserves mode, source, destination, scratch and every ordinary build flag");
        }
        if (infos[i].mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) ++native_updates;
        for (uint32_t g = 0; g < infos[i].geometryCount; ++g) {
            const auto& geometry = infos[i].pGeometries[g];
            if (geometry.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR)
                require(!geometry.geometry.triangles.pNext, "no custom MM reaches native driver");
            if (geometry.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
                require(geometry.geometry.instances.data.deviceAddress != original_instances,
                        "native TLAS uses private GPU instance copy");
                require(ranges[i][g].primitiveOffset == 0, "private instance range begins at zero");
            }
        }
        ++native_builds;
    }
    native_build(cb, count, infos, ranges);
}
VKAPI_ATTR void VKAPI_CALL spy_sizes(VkDevice device, VkAccelerationStructureBuildTypeKHR type,
                                     const VkAccelerationStructureBuildGeometryInfoKHR* info, const uint32_t* maxima,
                                     VkAccelerationStructureBuildSizesInfoKHR* sizes) {
    require((info->flags & 0x1c0) == 0, "native size query strips OMM build flags");
    for (uint32_t g = 0; g < info->geometryCount; ++g)
        if (info->pGeometries[g].geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR)
            require(!info->pGeometries[g].geometry.triangles.pNext, "size query strips software MM pNext");
    ++sanitized_queries;
    native_sizes(device, type, info, maxima, sizes);
}
VKAPI_ATTR VkResult VKAPI_CALL spy_create_buffer(VkDevice device, const VkBufferCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator, VkBuffer* output) {
    if (fail_private_buffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const auto result = native_create_buffer(device, info, allocator, output);
    require((physical_family_count > 1 && info->sharingMode == VK_SHARING_MODE_CONCURRENT &&
             info->queueFamilyIndexCount == physical_family_count) ||
                (physical_family_count <= 1 && info->sharingMode == VK_SHARING_MODE_EXCLUSIVE),
            "private AS buffers use concurrent sharing across available physical queue families");
    if (result == VK_SUCCESS) ++live_private_buffers;
    return result;
}
VKAPI_ATTR void VKAPI_CALL spy_destroy_buffer(VkDevice device, VkBuffer value, const VkAllocationCallbacks* allocator) {
    --live_private_buffers;
    native_destroy_buffer(device, value, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL spy_allocate_memory(VkDevice device, const VkMemoryAllocateInfo* info,
                                                   const VkAllocationCallbacks* allocator, VkDeviceMemory* output) {
    const auto result = native_allocate_memory(device, info, allocator, output);
    if (result == VK_SUCCESS) ++live_private_memory;
    return result;
}
VKAPI_ATTR void VKAPI_CALL spy_free_memory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator) {
    --live_private_memory;
    native_free_memory(device, memory, allocator);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL proc(VkDevice device, const char* name) {
    if (!std::strcmp(name, "vkCmdBuildAccelerationStructuresKHR")) return reinterpret_cast<PFN_vkVoidFunction>(spy_build);
    if (!std::strcmp(name, "vkGetAccelerationStructureBuildSizesKHR")) return reinterpret_cast<PFN_vkVoidFunction>(spy_sizes);
    if (!std::strcmp(name, "vkCreateBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(spy_create_buffer);
    if (!std::strcmp(name, "vkDestroyBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(spy_destroy_buffer);
    if (!std::strcmp(name, "vkAllocateMemory")) return reinterpret_cast<PFN_vkVoidFunction>(spy_allocate_memory);
    if (!std::strcmp(name, "vkFreeMemory")) return reinterpret_cast<PFN_vkVoidFunction>(spy_free_memory);
    if (!std::strcmp(name, "vkGetAccelerationStructureDeviceAddressKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(spy_as_address);
    return vkGetDeviceProcAddr(device, name);
}
std::vector<uint32_t> shader(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file) && file.tellg() > 0 && file.tellg() % 4 == 0, "shader file");
    const auto size = size_t(file.tellg());
    std::vector<uint32_t> result(size / 4);
    file.seekg(0);
    require(bool(file.read(reinterpret_cast<char*>(result.data()), size)), "shader read");
    return result;
}
void track(DeviceBackend* b, const Buffer& value, uint64_t size, VkBufferUsageFlags usage) {
    auto info = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = size;
    info.usage = usage;
    CHECK(device_track_buffer(b, value.handle, &info, size));
    device_buffer_bound(b, value.handle);
}
void storage_address_spans(DeviceBackend* backend) {
    // These are tracker-only identities. No native sparse buffer, binding or
    // address query is involved, so this does not qualify GPU sparse binding.
    const uint64_t sparse_bits = 0x51a50001u, ordinary_bits = 0x51a50002u;
    VkBuffer sparse{}, ordinary{};
    std::memcpy(&sparse, &sparse_bits, sizeof(sparse));
    std::memcpy(&ordinary, &ordinary_bits, sizeof(ordinary));
    auto info = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
    info.size = 4096;
    info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const auto before = address_queries;
    CHECK(device_track_buffer(backend, sparse, &info));
    require(device_buffer_bound_span(backend, sparse, 0, info.size) == VK_ERROR_VALIDATION_FAILED_EXT,
            "strict bound span does not claim a tracker-only sparse buffer is bound");
    require(device_buffer_as_address_span(backend, sparse, 0, info.size) == VK_SUCCESS &&
                device_buffer_as_address_span(backend, sparse, info.size, 0) == VK_SUCCESS,
            "sparse AS address query permits full and empty-end logical spans without BindBufferMemory");
    require(device_buffer_as_address_span(backend, sparse, info.size - 1, 2) == VK_ERROR_VALIDATION_FAILED_EXT &&
                device_buffer_as_address_span(backend, sparse, info.size + 1, 0) == VK_ERROR_VALIDATION_FAILED_EXT &&
                device_buffer_as_address_span(backend, sparse, UINT64_MAX, UINT64_MAX) == VK_ERROR_VALIDATION_FAILED_EXT &&
                device_buffer_as_address_span(backend, sparse, 1, UINT64_MAX) == VK_ERROR_VALIDATION_FAILED_EXT,
            "sparse exemption retains checked logical range and overflow rejection");
    info.flags = 0;
    CHECK(device_track_buffer(backend, ordinary, &info));
    require(device_buffer_as_address_span(backend, ordinary, 0, info.size) == VK_ERROR_VALIDATION_FAILED_EXT,
            "ordinary AS address span still rejects unbound storage");
    device_buffer_bound(backend, ordinary);
    require(device_buffer_as_address_span(backend, ordinary, 0, info.size) == VK_SUCCESS &&
                device_buffer_as_address_span(backend, ordinary, info.size, 0) == VK_SUCCESS &&
                device_buffer_as_address_span(backend, ordinary, info.size - 1, 2) == VK_ERROR_VALIDATION_FAILED_EXT,
            "ordinary AS address span requires binding and retains logical bounds");
    device_untrack_buffer(backend, sparse);
    device_untrack_buffer(backend, ordinary);
    require(device_buffer_as_address_span(backend, sparse, 0, 1) == VK_ERROR_VALIDATION_FAILED_EXT &&
                device_buffer_as_address_span(backend, VK_NULL_HANDLE, 0, 1) == VK_ERROR_VALIDATION_FAILED_EXT &&
                address_queries == before,
            "unknown AS storage rejects without querying native AS addresses");
}
void track_as(DeviceBackend* mm, AccelerationBackend* b, const Acceleration& value, VkAccelerationStructureTypeKHR type) {
    const auto info = creations.at(value.handle);
    require(info.type == type, "fixture AS type matches real create info");
    auto storage = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    storage.size = info.offset + info.size;
    storage.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    CHECK(device_track_buffer(mm, info.buffer, &storage));
    device_buffer_bound(mm, info.buffer);
    const auto before = address_queries;
    CHECK(acceleration_track(b, value.handle, &info));
    require(address_queries == before, "tracking an AS allocates identity without native address queries");
    VkDeviceAddress native{}, root{};
    CHECK(acceleration_identity(b, value.handle, &native, &root));
    require(native == value.address && root != native && root != 0, "native AS identity unchanged, private root tracked");
    VkDeviceAddress by_address{};
    CHECK(acceleration_root_from_address(b, native, &by_address));
    require(root == by_address, "integer native AS identity maps to same root");
}
void release(Fixture& fixture, DeviceBackend* b, Buffer value) {
    device_untrack_buffer(b, value.handle);
    auto found =
        std::find_if(fixture.buffers.begin(), fixture.buffers.end(), [&](const Buffer& v) { return v.handle == value.handle; });
    require(found != fixture.buffers.end(), "fixture source lifetime tracked");
    if (value.mapped) vkUnmapMemory(fixture.device, value.memory);
    vkDestroyBuffer(fixture.device, value.handle, nullptr);
    vkFreeMemory(fixture.device, value.memory, nullptr);
    fixture.buffers.erase(found);
}
void submit_finished(Fixture& f, SubmissionTracker* tracker, VkCommandBuffer command,
                     const std::vector<AccelerationRecordingRef>& recordings, bool private_fence = false) {
    auto submit = init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (private_fence) {
        CHECK(submissions_queue_submit(tracker, f.queue, 1, &submit, VK_NULL_HANDLE, recordings));
        CHECK(submissions_queue_wait_idle(tracker, f.queue));
        return;
    }
    auto create = init<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    VkFence fence{};
    CHECK(vkCreateFence(f.device, &create, nullptr, &fence));
    CHECK(submissions_queue_submit(tracker, f.queue, 1, &submit, fence, recordings));
    CHECK(submissions_wait_for_fences(tracker, 1, &fence, VK_TRUE, UINT64_MAX));
    CHECK(submissions_destroy_fence(tracker, fence, nullptr));
}
void submit_build(Fixture& f, SubmissionTracker* tracker, VkCommandBuffer command,
                  const std::vector<AccelerationRecordingRef>& recordings, bool private_fence = false) {
    CHECK(vkEndCommandBuffer(command));
    submit_finished(f, tracker, command, recordings, private_fence);
    vkFreeCommandBuffers(f.device, f.pool, 1, &command);
}
VkResult resolve_reference(void* user, VkAccelerationStructureKHR handle, DescriptorASIdentity* result) {
    return acceleration_reference(static_cast<AccelerationBackend*>(user), handle, &result->identity_cell_address,
                                  &result->root_cell_address, &result->lifetime);
}
Acceleration unbound_as(Fixture& f, DeviceBackend* mm, AccelerationBackend* as, VkAccelerationStructureTypeKHR type,
                        const VkAccelerationStructureGeometryKHR& geometry, Buffer* backing) {
    Acceleration value{};
    value.build =
        init<VkAccelerationStructureBuildGeometryInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR);
    value.build.type = type;
    value.build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    value.build.geometryCount = 1;
    value.build.pGeometries = &geometry;
    value.range.primitiveCount = 1;
    const uint32_t count = 1;
    auto sizes = init<VkAccelerationStructureBuildSizesInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR);
    f.sizesAS(f.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &value.build, &count, &sizes);
    auto info = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = sizes.accelerationStructureSize;
    info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    CHECK(vkCreateBuffer(f.device, &info, nullptr, &backing->handle));
    CHECK(device_track_buffer(mm, backing->handle, &info));
    auto create = init<VkAccelerationStructureCreateInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR);
    create.buffer = backing->handle;
    create.size = info.size;
    create.type = type;
    CHECK(f.createAS(f.device, &create, nullptr, &value.handle));
    f.structures.push_back(value.handle);
    const auto queries = address_queries, buffers = live_private_buffers;
    fail_private_buffer = true;
    const auto failed = acceleration_track(as, value.handle, &create);
    fail_private_buffer = false;
    require(failed == VK_ERROR_OUT_OF_DEVICE_MEMORY && live_private_buffers == buffers && address_queries == queries,
            "identity allocation OOM leaves native AS unqueried and no private allocation");
    CHECK(acceleration_track(as, value.handle, &create));
    require(address_queries == queries, "unbound CreateAS and identity allocation issue no native AS address query");
    VkDeviceAddress native = 7, root = 8;
    require(acceleration_identity(as, value.handle, &native, &root) == VK_ERROR_VALIDATION_FAILED_EXT && native == 7 && root == 8 &&
                address_queries == queries,
            "unbound legacy identity fails without native query or changing outputs");
    auto scratch = f.buffer(sizes.buildScratchSize + f.scratchAlignment - 1,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    value.build.scratchData.deviceAddress = (scratch.address + f.scratchAlignment - 1) & ~(f.scratchAlignment - 1);
    value.build.dstAccelerationStructure = value.handle;
    return value;
}
void bind_backing(Fixture& f, DeviceBackend* mm, Buffer* buffer, bool bind2) {
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(f.device, buffer->handle, &requirements);
    uint32_t type = 0;
    while (!(requirements.memoryTypeBits & (1u << type))) ++type;
    auto flags = init<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    auto allocate = init<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocate.pNext = &flags;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    CHECK(vkAllocateMemory(f.device, &allocate, nullptr, &buffer->memory));
    if (bind2) {
        auto bind = init<VkBindBufferMemoryInfo>(VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO);
        bind.buffer = buffer->handle;
        bind.memory = buffer->memory;
        CHECK(vkBindBufferMemory2(f.device, 1, &bind));
    } else
        CHECK(vkBindBufferMemory(f.device, buffer->handle, buffer->memory, 0));
    device_buffer_bound(mm, buffer->handle);
    f.buffers.push_back(*buffer);
}
void unbound_descriptor_query(Fixture& f, DeviceBackend* mm, AccelerationBackend* as, SubmissionTracker* submissions,
                              VkPipeline pipeline, VkPipelineLayout layout, const Buffer& output) {
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    for (bool bind2 : {false, true}) {
        auto vertices = f.buffer(36, usage, true);
        const float triangle[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        std::memcpy(vertices.mapped, triangle, sizeof(triangle));
        auto geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.triangles = init<VkAccelerationStructureGeometryTrianglesDataKHR>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
        geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geometry.geometry.triangles.vertexData.deviceAddress = vertices.address;
        geometry.geometry.triangles.vertexStride = 12;
        geometry.geometry.triangles.maxVertex = 2;
        geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        auto blas = f.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometry);
        track_as(mm, as, blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        // A never-bound unrelated BLAS must not be queried by TLAS snapshotting.
        Buffer unused_storage{};
        auto unused = unbound_as(f, mm, as, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometry, &unused_storage);
        auto instances = f.buffer(64, usage, true);
        track(mm, instances, 64, usage);
        original_instances = instances.address;
        VkAccelerationStructureInstanceKHR instance{};
        for (uint32_t axis = 0; axis < 3; ++axis) instance.transform.matrix[axis][axis] = 1;
        instance.mask = 255;
        instance.instanceCustomIndex = 77;
        instance.accelerationStructureReference = blas.address;
        std::memcpy(instances.mapped, &instance, sizeof(instance));
        auto tlas_geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
        tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlas_geometry.geometry.instances = init<VkAccelerationStructureGeometryInstancesDataKHR>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
        tlas_geometry.geometry.instances.data.deviceAddress = instances.address;
        Buffer storage{};
        auto tlas = unbound_as(f, mm, as, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tlas_geometry, &storage);
        DescriptorBackend* descriptors{};
        CHECK(descriptor_backend_create({f.physical, f.device, vkGetDeviceProcAddr, nullptr, resolve_reference, as}, &descriptors));
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                             nullptr};
        auto layout_info = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layout_info.bindingCount = 1;
        layout_info.pBindings = &binding;
        VkDescriptorSetLayout set_layout{};
        CHECK(vkCreateDescriptorSetLayout(f.device, &layout_info, nullptr, &set_layout));
        CHECK(descriptor_track_layout(descriptors, set_layout, &layout_info));
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
        auto pool_info = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        VkDescriptorPool pool{};
        CHECK(vkCreateDescriptorPool(f.device, &pool_info, nullptr, &pool));
        CHECK(descriptor_track_pool(descriptors, pool, &pool_info));
        auto sets_info = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        sets_info.descriptorPool = pool;
        sets_info.descriptorSetCount = 1;
        sets_info.pSetLayouts = &set_layout;
        VkDescriptorSet set{};
        CHECK(vkAllocateDescriptorSets(f.device, &sets_info, &set));
        CHECK(descriptor_allocate_sets(descriptors, &sets_info, &set));
        auto as_write =
            init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
        as_write.accelerationStructureCount = 1;
        as_write.pAccelerationStructures = &tlas.handle;
        auto write = init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        write.pNext = &as_write;
        write.dstSet = set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        const auto queries = address_queries;
        CHECK(descriptor_update(descriptors, 1, &write, 0, nullptr));
        vkUpdateDescriptorSets(f.device, 1, &write, 0, nullptr);
        DescriptorContextRef context;
        CHECK(descriptor_build_context(descriptors, {set}, &context));
        DescriptorASIdentity reference;
        CHECK(descriptor_context_fetch(context, 0, 0, 0, &reference));
        require(reference.identity_cell_address && reference.root_cell_address && !reference.native_address &&
                    address_queries == queries,
                "descriptor write/context before bind retains unresolved stable cell without native address query");
        auto query = f.begin();
        const uint64_t push[] = {0, context.address, output.address};
        vkCmdBindPipeline(query, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(query, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(query, 1, 1, 1);
        Fixture::barrier(query, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_ACCESS_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(query));
        submit_finished(f, submissions, query, {});
        require(static_cast<const uint32_t*>(output.mapped)[1] != 0,
                "unresolved non-null identity fails checked fetch without native traversal");
        auto build = f.begin();
        AccelerationRecordingRef first, second;
        const auto* tlas_range = &tlas.range;
        require(acceleration_cmd_build(as, build, 1, &tlas.build, &tlas_range, &second) == VK_ERROR_VALIDATION_FAILED_EXT &&
                    address_queries == queries,
                "unbound build fails before native address or command prefix");
        bind_backing(f, mm, &storage, bind2);
        fail_private_buffer = true;
        const auto* blas_range = &blas.range;
        const auto builds = native_builds;
        require(acceleration_cmd_build(as, build, 1, &blas.build, &blas_range, &first) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                    native_builds == builds,
                "build allocation OOM publishes no identity and no native prefix");
        fail_private_buffer = false;
        CHECK(acceleration_cmd_build(as, build, 1, &blas.build, &blas_range, &first));
        CHECK(acceleration_cmd_build(as, build, 1, &tlas.build, &tlas_range, &second));
        CHECK(vkEndCommandBuffer(build));
        submit_finished(f, submissions, build, {first, second});
        submit_finished(f, submissions, build, {first, second});  // Replay republishes the same cell.
        vkFreeCommandBuffers(f.device, f.pool, 1, &build);
        first.reset();
        second.reset();
        release(f, mm, instances);
        release(f, mm, vertices);
        for (unsigned replay = 0; replay < 2; ++replay) {
            submit_finished(f, submissions, query, {});
            const auto* result = static_cast<const uint32_t*>(output.mapped);
            require(result[0] == 1 && result[1] == 0 && result[2] == 1 && result[3] == 1 && result[4] == 77 && result[6] == 1,
                    "unchanged pre-bind descriptor/context/CB executes native RQ after build, replay and source destruction");
        }
        vkFreeCommandBuffers(f.device, f.pool, 1, &query);
        acceleration_untrack(as, unused.handle);
        device_untrack_buffer(mm, unused_storage.handle);
        f.buffers.push_back(unused_storage);
        acceleration_untrack(as, tlas.handle);
        acceleration_untrack(as, blas.handle);
        require(live_private_buffers == 4, "descriptor identity retains TLAS, active body and its BLAS dependency");
        reference = {};
        context = {};
        descriptor_backend_destroy(descriptors);
        require(live_private_buffers == 0 && live_private_memory == 0,
                "last descriptor reference retires every private identity/body");
        vkDestroyDescriptorPool(f.device, pool, nullptr);
        vkDestroyDescriptorSetLayout(f.device, set_layout, nullptr);
    }
}
Acceleration updatable_as(Fixture& f, DeviceBackend* mm, AccelerationBackend* as, VkAccelerationStructureTypeKHR type,
                          const VkAccelerationStructureGeometryKHR& geometry, VkBuildAccelerationStructureFlagsKHR flags) {
    Acceleration value{};
    value.build =
        init<VkAccelerationStructureBuildGeometryInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR);
    value.build.type = type;
    value.build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    value.build.flags = flags;
    value.build.geometryCount = 1;
    value.build.pGeometries = &geometry;
    value.range.primitiveCount = 1;
    const uint32_t maximum = 1;
    auto sizes = init<VkAccelerationStructureBuildSizesInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR);
    CHECK(acceleration_get_build_sizes(as, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &value.build, &maximum, &sizes));
    // Query UPDATE as well. Both scratch fields must remain the actual native
    // answers; neither a BUILD conversion nor software body size may replace it.
    auto update = value.build;
    update.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    auto update_sizes = sizes;
    CHECK(acceleration_get_build_sizes(as, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &update, &maximum, &update_sizes));
    auto native_info = update;
    native_info.flags &= ~VkBuildAccelerationStructureFlagsKHR(0x1c0);
    auto native_geometry = geometry;
    if (native_geometry.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) native_geometry.geometry.triangles.pNext = nullptr;
    native_info.pGeometries = &native_geometry;
    auto native_result = sizes;
    native_sizes(f.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &native_info, &maximum, &native_result);
    require(update_sizes.accelerationStructureSize == native_result.accelerationStructureSize &&
                update_sizes.buildScratchSize == native_result.buildScratchSize &&
                update_sizes.updateScratchSize == native_result.updateScratchSize && update_sizes.updateScratchSize != 0,
            "UPDATE size query preserves both native scratch sizes and acceleration structure size");
    const auto storage =
        f.buffer(std::max(sizes.accelerationStructureSize, update_sizes.accelerationStructureSize),
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    auto create = init<VkAccelerationStructureCreateInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR);
    create.type = type;
    create.buffer = storage.handle;
    create.size = std::max(sizes.accelerationStructureSize, update_sizes.accelerationStructureSize);
    CHECK(f.createAS(f.device, &create, nullptr, &value.handle));
    f.structures.push_back(value.handle);
    auto address =
        init<VkAccelerationStructureDeviceAddressInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR);
    address.accelerationStructure = value.handle;
    value.address = f.addressAS(f.device, &address);
    const auto scratch = f.buffer(std::max(sizes.buildScratchSize, update_sizes.updateScratchSize) + f.scratchAlignment - 1,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    value.build.dstAccelerationStructure = value.handle;
    value.build.scratchData.deviceAddress = (scratch.address + f.scratchAlignment - 1) & ~(f.scratchAlignment - 1);
    track_as(mm, as, value, type);
    return value;
}
void destroy_native_as(Fixture& f, DeviceBackend* mm, AccelerationBackend* as, const Acceleration& value) {
    const auto storage = creations.at(value.handle).buffer;
    acceleration_untrack(as, value.handle);
    f.destroyAS(f.device, value.handle, nullptr);
    f.structures.erase(std::remove(f.structures.begin(), f.structures.end(), value.handle), f.structures.end());
    const auto backing = std::find_if(f.buffers.begin(), f.buffers.end(), [&](const Buffer& v) { return v.handle == storage; });
    require(backing != f.buffers.end(), "native UPDATE source backing is owned by fixture");
    release(f, mm, *backing);
}
struct UpdateMicromap {
    VkMicromapEXT handle{};
    Buffer triangles{}, data{}, storage{}, scratch{};
};
UpdateMicromap update_micromap(Fixture& f, DeviceBackend* mm, VkCommandBuffer cb, uint32_t level, uint32_t packed) {
    UpdateMicromap result;
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    result.triangles = f.buffer(8, usage, false);
    result.data = f.buffer(4, usage, false);
    result.scratch = f.buffer(512, usage, false);
    track(mm, result.triangles, 8, usage);
    track(mm, result.data, 4, usage);
    track(mm, result.scratch, 512, usage);
    const uint32_t triangle[] = {0, 0x00020000u | level};
    vkCmdUpdateBuffer(cb, result.triangles.handle, 0, 8, triangle);
    vkCmdUpdateBuffer(cb, result.data.handle, 0, 4, &packed);
    auto info = init<VkMicromapBuildInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT);
    info.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    info.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    VkMicromapUsageEXT usage_count{1, level, VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT};
    info.usageCountsCount = 1;
    info.pUsageCounts = &usage_count;
    auto sizes = init<VkMicromapBuildSizesInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT);
    CHECK(device_get_build_sizes(mm, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &sizes));
    result.storage = f.buffer(sizes.micromapSize, usage, false);
    track(mm, result.storage, sizes.micromapSize, usage | VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT);
    auto create = init<VkMicromapCreateInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT);
    create.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    create.buffer = result.storage.handle;
    create.size = sizes.micromapSize;
    CHECK(device_create_micromap(mm, &create, nullptr, &result.handle));
    info.dstMicromap = result.handle;
    info.data.deviceAddress = result.data.address;
    info.triangleArray.deviceAddress = result.triangles.address;
    info.triangleArrayStride = 8;
    info.scratchData.deviceAddress = (result.scratch.address + 255) & ~uint64_t(255);
    Fixture::barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    CHECK(device_cmd_build(mm, cb, 1, &info));
    return result;
}
void release_micromap(Fixture& f, DeviceBackend* mm, const UpdateMicromap& value) {
    if (!value.handle) return;
    device_destroy_micromap(mm, value.handle, nullptr);
    for (const auto& buffer : {value.triangles, value.data, value.storage, value.scratch}) release(f, mm, buffer);
}
void update_probe(Fixture& f, AccelerationBackend* as, VkPipeline pipeline, VkPipelineLayout layout, const Buffer& output,
                  const Acceleration& tlas, bool hit, uint32_t custom, uint32_t flags, const char* reason) {
    VkDeviceAddress native{}, root{};
    CHECK(acceleration_identity(as, tlas.handle, &native, &root));
    const uint64_t push[] = {native, root, output.address};
    const auto cb = f.begin();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(cb, 1, 1, 1);
    Fixture::barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_HOST_READ_BIT);
    f.submit(cb);
    const auto* result = static_cast<const uint32_t*>(output.mapped);
    require(result[0] == uint32_t(hit) && result[1] == 0 && result[2] == uint32_t(hit) && result[3] == 1 && result[4] == custom &&
                result[5] == flags && result[6] == 1 && result[7] == 1,
            reason);
}
AccelerationRecordingRef record_update(AccelerationBackend* as, VkCommandBuffer cb,
                                       const VkAccelerationStructureBuildGeometryInfoKHR& info,
                                       const VkAccelerationStructureBuildRangeInfoKHR& range) {
    AccelerationRecordingRef result;
    const auto* ranges = &range;
    expected_update = &info;
    CHECK(acceleration_cmd_build(as, cb, 1, &info, &ranges, &result));
    expected_update = nullptr;
    return result;
}
void native_update_tests(Fixture& f, DeviceBackend* mm, AccelerationBackend* as, SubmissionTracker* submissions,
                         VkPipeline pipeline, VkPipelineLayout layout, const Buffer& output) {
    const VkBufferUsageFlags input_usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    // Complete UPDATE descriptions cover all three association states, including
    // unchanged MM association without either OMM update permission flag.
    for (unsigned variant = 0; variant < 5; ++variant) {
        const bool associated = variant != 0, special_only = variant == 4, retained_mm = variant == 1;
        auto source_vertices = f.buffer(36, input_usage, true), update_vertices = f.buffer(36, input_usage, true);
        const float triangle[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const float moved_triangle[] = {0, 0, 0.5f, 1, 0, 0.5f, 0, 1, 0.5f};
        std::memcpy(source_vertices.mapped, triangle, sizeof(triangle));
        std::memcpy(update_vertices.mapped, moved_triangle, sizeof(moved_triangle));
        auto source_geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
        source_geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        source_geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        source_geometry.geometry.triangles = init<VkAccelerationStructureGeometryTrianglesDataKHR>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
        source_geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        source_geometry.geometry.triangles.vertexData.deviceAddress = source_vertices.address;
        source_geometry.geometry.triangles.vertexStride = 12;
        source_geometry.geometry.triangles.maxVertex = 2;
        source_geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        auto initial = f.begin();
        UpdateMicromap original_mm{}, replacement_mm{};
        if (associated && !special_only) {
            original_mm = update_micromap(f, mm, initial, 0, retained_mm ? 1 : 0);
            if (!retained_mm) replacement_mm = update_micromap(f, mm, initial, variant == 2 ? 1 : 0, variant == 2 ? 0x55 : 1);
        }
        Buffer original_indices{}, replacement_indices{};
        auto original_association = init<VkAccelerationStructureTrianglesOpacityMicromapEXT>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT);
        auto replacement_association = original_association;
        if (associated) {
            original_indices = f.buffer(4, input_usage, true);
            replacement_indices = f.buffer(4, input_usage, true);
            track(mm, original_indices, 4, input_usage);
            track(mm, replacement_indices, 4, input_usage);
            *static_cast<uint32_t*>(original_indices.mapped) =
                special_only ? uint32_t(VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_TRANSPARENT_EXT) : 0;
            *static_cast<uint32_t*>(replacement_indices.mapped) =
                special_only ? uint32_t(VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE_EXT) : 0;
            original_association.indexType = replacement_association.indexType = VK_INDEX_TYPE_UINT32;
            original_association.indexBuffer.deviceAddress = original_indices.address;
            replacement_association.indexBuffer.deviceAddress = replacement_indices.address;
            original_association.micromap = original_mm.handle;
            replacement_association.micromap = retained_mm ? original_mm.handle : replacement_mm.handle;
            source_geometry.geometry.triangles.pNext = &original_association;
        }
        auto replacement_geometry = source_geometry;
        replacement_geometry.geometry.triangles.vertexData.deviceAddress = update_vertices.address;
        replacement_geometry.geometry.triangles.pNext = associated ? &replacement_association : nullptr;
        VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        if (variant == 2 || special_only) flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_OPACITY_MICROMAP_UPDATE_BIT_EXT;
        if (variant == 3) flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_OPACITY_MICROMAP_DATA_UPDATE_BIT_EXT;
        auto source_blas = updatable_as(f, mm, as, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, source_geometry, flags);
        auto target_blas = updatable_as(f, mm, as, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, source_geometry, flags);
        auto source_instances = f.buffer(64, input_usage, true), changed_instances = f.buffer(64, input_usage, true),
             target_instances = f.buffer(64, input_usage, true);
        for (const auto& buffer : {source_instances, changed_instances, target_instances}) track(mm, buffer, 64, input_usage);
        VkAccelerationStructureInstanceKHR instance{};
        for (unsigned axis = 0; axis < 3; ++axis) instance.transform.matrix[axis][axis] = 1;
        instance.mask = 255;
        instance.instanceCustomIndex = 77;
        instance.accelerationStructureReference = source_blas.address;
        std::memcpy(source_instances.mapped, &instance, sizeof(instance));
        const uint32_t update_flags = associated ? 0x20u : VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.flags = VkGeometryInstanceFlagsKHR(update_flags);
        instance.instanceCustomIndex = 81;
        std::memcpy(changed_instances.mapped, &instance, sizeof(instance));
        instance.instanceCustomIndex = 83;
        instance.accelerationStructureReference = target_blas.address;
        std::memcpy(target_instances.mapped, &instance, sizeof(instance));
        auto instance_geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
        instance_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        instance_geometry.geometry.instances = init<VkAccelerationStructureGeometryInstancesDataKHR>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
        instance_geometry.geometry.instances.data.deviceAddress = source_instances.address;
        auto source_tlas = updatable_as(f, mm, as, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, instance_geometry,
                                        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR);
        auto target_tlas = updatable_as(f, mm, as, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, instance_geometry,
                                        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR);
        const auto* range = &source_blas.range;
        AccelerationRecordingRef initial_blas, initial_tlas;
        CHECK(acceleration_cmd_build(as, initial, 1, &source_blas.build, &range, &initial_blas));
        range = &source_tlas.range;
        CHECK(acceleration_cmd_build(as, initial, 1, &source_tlas.build, &range, &initial_tlas));
        CHECK(vkEndCommandBuffer(initial));
        submit_finished(f, submissions, initial, {initial_blas, initial_tlas});
        update_probe(f, as, pipeline, layout, output, source_tlas, !associated || retained_mm, 77, 0,
                     "initial native source and canonical association match before UPDATE");
        VkDeviceAddress ignored{}, source_root{}, target_root{};
        CHECK(acceleration_identity(as, source_blas.handle, &ignored, &source_root));
        CHECK(acceleration_identity(as, target_blas.handle, &ignored, &target_root));
        require(source_root != target_root, "out-of-place UPDATE target owns a distinct stable root");

        auto update_blas = target_blas.build;
        update_blas.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        update_blas.srcAccelerationStructure = source_blas.handle;
        const VkAccelerationStructureGeometryKHR* geometry_pointer = &replacement_geometry;
        update_blas.pGeometries = nullptr;
        update_blas.ppGeometries = &geometry_pointer;
        auto target_geometry = instance_geometry;
        target_geometry.geometry.instances.data.deviceAddress = target_instances.address;
        auto update_tlas = target_tlas.build;
        update_tlas.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        update_tlas.srcAccelerationStructure = source_tlas.handle;
        update_tlas.pGeometries = &target_geometry;
        // Record this read of the native source before the in-place UPDATE is
        // even recorded. No host last-body guess is allowed at execution time.
        const auto destination_cb = f.begin();
        const auto baseline_buffers = live_private_buffers, baseline_memory = live_private_memory, builds_before = native_builds;
        AccelerationRecordingRef rejected = initial_blas;
        auto invalid = update_blas;
        invalid.srcAccelerationStructure = VK_NULL_HANDLE;
        range = &target_blas.range;
        require(acceleration_cmd_build(as, destination_cb, 1, &invalid, &range, &rejected) == VK_ERROR_VALIDATION_FAILED_EXT &&
                    rejected == initial_blas && native_builds == builds_before,
                "UPDATE rejects missing source atomically before native commands");
        invalid.srcAccelerationStructure = source_tlas.handle;
        require(acceleration_cmd_build(as, destination_cb, 1, &invalid, &range, &rejected) == VK_ERROR_VALIDATION_FAILED_EXT &&
                    native_builds == builds_before,
                "UPDATE rejects a source of the wrong AS type before native commands");
        fail_private_buffer = true;
        const auto allocation_failed = acceleration_cmd_build(as, destination_cb, 1, &update_blas, &range, &rejected);
        fail_private_buffer = false;
        require(allocation_failed == VK_ERROR_OUT_OF_DEVICE_MEMORY && rejected == initial_blas && native_builds == builds_before &&
                    live_private_buffers == baseline_buffers && live_private_memory == baseline_memory,
                "UPDATE allocation failure preserves result token, private allocation counts and native command prefix");
        rejected.reset();
        auto destination_blas = record_update(as, destination_cb, update_blas, target_blas.range);
        auto destination_tlas = record_update(as, destination_cb, update_tlas, target_tlas.range);
        CHECK(vkEndCommandBuffer(destination_cb));
        auto in_place_blas = update_blas;
        in_place_blas.dstAccelerationStructure = source_blas.handle;
        in_place_blas.scratchData = source_blas.build.scratchData;
        auto changed_geometry = instance_geometry;
        changed_geometry.geometry.instances.data.deviceAddress = changed_instances.address;
        auto in_place_tlas = update_tlas;
        in_place_tlas.dstAccelerationStructure = source_tlas.handle;
        in_place_tlas.scratchData = source_tlas.build.scratchData;
        in_place_tlas.pGeometries = &changed_geometry;
        const auto in_place_cb = f.begin();
        auto in_place_blas_token = record_update(as, in_place_cb, in_place_blas, source_blas.range);
        auto in_place_tlas_token = record_update(as, in_place_cb, in_place_tlas, source_tlas.range);
        require(acceleration_recordings_overlap(initial_blas, in_place_blas_token) &&
                    !acceleration_recordings_overlap(in_place_blas_token, destination_blas),
                "UPDATE retirement overlaps destination writers while independent target metadata does not read source root");
        submit_build(f, submissions, in_place_cb, {in_place_blas_token, in_place_tlas_token});
        in_place_blas_token.reset();
        in_place_tlas_token.reset();
        update_probe(
            f, as, pipeline, layout, output, source_tlas, true, 81, update_flags,
            "native in-place BLAS/TLAS UPDATE preserves identity and publishes changed canonical associations/instance flags");
        CHECK(acceleration_identity(as, source_blas.handle, &ignored, &target_root));
        require(target_root == source_root, "in-place BLAS UPDATE retains the original stable root address");
        submit_finished(f, submissions, destination_cb, {destination_blas, destination_tlas});
        update_probe(f, as, pipeline, layout, output, target_tlas, true, 83, update_flags,
                     "native out-of-place BLAS/TLAS UPDATE consumes the latest native source and complete explicit inputs");
        submit_finished(f, submissions, initial, {initial_blas, initial_tlas});
        update_probe(f, as, pipeline, layout, output, source_tlas, !associated || retained_mm, 77, 0,
                     "old source BUILD CB replay restores its original source metadata");
        update_probe(f, as, pipeline, layout, output, target_tlas, true, 83, update_flags,
                     "replaying source BUILD cannot mutate independent out-of-place UPDATE canonical snapshots");
        submit_finished(f, submissions, destination_cb, {destination_blas, destination_tlas});
        update_probe(f, as, pipeline, layout, output, target_tlas, true, 83, update_flags,
                     "recorded native UPDATE CB replays after a different source body is published");
        vkFreeCommandBuffers(f.device, f.pool, 1, &initial);
        vkFreeCommandBuffers(f.device, f.pool, 1, &destination_cb);
        initial_blas.reset();
        initial_tlas.reset();
        destination_blas.reset();
        destination_tlas.reset();
        destroy_native_as(f, mm, as, source_tlas);
        destroy_native_as(f, mm, as, source_blas);
        for (const auto& buffer : {source_instances, changed_instances, target_instances, source_vertices, update_vertices})
            release(f, mm, buffer);
        if (associated) {
            release(f, mm, original_indices);
            release(f, mm, replacement_indices);
        }
        release_micromap(f, mm, original_mm);
        release_micromap(f, mm, replacement_mm);
        update_probe(
            f, as, pipeline, layout, output, target_tlas, true, 83, update_flags,
            "updated destination traverses after actual source AS destruction and all OMM/index/instance inputs are freed");
        destroy_native_as(f, mm, as, target_tlas);
        destroy_native_as(f, mm, as, target_blas);
        require(live_private_buffers == 0 && live_private_memory == 0,
                "UPDATE/replay/source-destruction retires every source/target metadata allocation");
        std::printf("PASS native AS UPDATE variant %u: in-place, out-of-place, source replay, UPDATE replay, source lifetime\n",
                    variant);
    }
    require(native_updates == 20, "twenty genuine native UPDATE commands reached the driver without conversion to BUILD");
}
}  // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    Fixture f;
    if (!f.open()) return 77;
    const std::string directory = argv[1];
    auto build = shader(directory + "/omm_build.comp.spv"), copy = shader(directory + "/omm_copy.comp.spv");
    auto metadata = shader(directory + "/omm_metadata.comp.spv"), instances = shader(directory + "/omm_instances.comp.spv");
    auto publish = shader(directory + "/omm_publish.comp.spv"), probe = shader(directory + "/acceleration_probe.comp.spv");
    native_build = f.buildAS;
    native_sizes = f.sizesAS;
    native_create_buffer = vkCreateBuffer;
    native_destroy_buffer = vkDestroyBuffer;
    native_allocate_memory = vkAllocateMemory;
    native_free_memory = vkFreeMemory;
    native_create_as = f.createAS;
    native_as_address = f.addressAS;
    f.createAS = spy_create_as;
    vkGetPhysicalDeviceQueueFamilyProperties(f.physical, &physical_family_count, nullptr);
    DeviceBackend* mm{};
    CHECK(device_backend_create(
        {f.physical, f.device, vkGetDeviceProcAddr, nullptr, build.data(), build.size() * 4, copy.data(), copy.size() * 4}, &mm));
    tracked_buffers = mm;
    storage_address_spans(mm);
    AccelerationBackend* as{};
    CHECK(acceleration_backend_create({f.physical, f.device, proc, mm, nullptr, metadata.data(), metadata.size() * 4,
                                       instances.data(), instances.size() * 4, publish.data(), publish.size() * 4},
                                      &as));
    SubmissionTracker* submissions{};
    CHECK(submissions_create({f.device, proc, as, nullptr}, &submissions));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    auto layout_info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    VkPipelineLayout layout{};
    CHECK(vkCreatePipelineLayout(f.device, &layout_info, nullptr, &layout));
    auto module_info = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    module_info.codeSize = probe.size() * 4;
    module_info.pCode = probe.data();
    VkShaderModule module{};
    CHECK(vkCreateShaderModule(f.device, &module_info, nullptr, &module));
    auto pipeline_info = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipeline_info.layout = layout;
    pipeline_info.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    VkPipeline pipeline{};
    CHECK(vkCreateComputePipelines(f.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));
    const VkBufferUsageFlags input_usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto output = f.buffer(32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    for (uint32_t variant = 0; variant < 4; ++variant) {
        auto vertices = f.buffer(36, input_usage, true);
        const float triangle[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        std::memcpy(vertices.mapped, triangle, sizeof(triangle));
        auto geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = variant == 1 ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.triangles = init<VkAccelerationStructureGeometryTrianglesDataKHR>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
        geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geometry.geometry.triangles.vertexData.deviceAddress = vertices.address;
        geometry.geometry.triangles.vertexStride = 12;
        geometry.geometry.triangles.maxVertex = 2;
        geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        auto blas = f.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometry);
        track_as(mm, as, blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        auto input = f.buffer(128, input_usage, false);
        track(mm, input, 128, input_usage);
        original_instances = input.address;
        auto instance_geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
        instance_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        instance_geometry.geometry.instances = init<VkAccelerationStructureGeometryInstancesDataKHR>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
        instance_geometry.geometry.instances.data.deviceAddress = input.address;
        auto tlas = f.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, instance_geometry);
        tlas.range.primitiveOffset = 64;  // Exercise logical range offset, not custom index.
        track_as(mm, as, tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
        VkAccelerationStructureInstanceKHR instance{};
        for (uint32_t axis = 0; axis < 3; ++axis) instance.transform.matrix[axis][axis] = 1;
        instance.instanceCustomIndex = 77;
        instance.mask = 255;
        instance.flags = variant == 1 ? 0x10 : variant == 2 ? 0x20 : 0;
        instance.accelerationStructureReference = blas.address;
        auto command = f.begin();
        vkCmdUpdateBuffer(command, input.handle, 64, sizeof(instance), &instance);
        VkMicromapEXT micromap{};
        Buffer indices{}, triangles{}, data{}, storage{}, scratch{};
        auto association = init<VkAccelerationStructureTrianglesOpacityMicromapEXT>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT);
        if (variant) {
            const auto usage =
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            triangles = f.buffer(8, usage, false);
            data = f.buffer(4, usage, false);
            scratch = f.buffer(512, usage, false);
            indices = f.buffer(4, usage, false);
            track(mm, triangles, 8, usage);
            track(mm, data, 4, usage);
            track(mm, scratch, 512, usage);
            track(mm, indices, 4, usage);
            const uint32_t triangle_record[] = {0, 0x00020000}, state = 3, index = variant == 3 ? 99 : 0;
            vkCmdUpdateBuffer(command, triangles.handle, 0, 8, triangle_record);
            vkCmdUpdateBuffer(command, data.handle, 0, 4, &state);
            vkCmdUpdateBuffer(command, indices.handle, 0, 4, &index);
            auto mm_info = init<VkMicromapBuildInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT);
            mm_info.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
            mm_info.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
            VkMicromapUsageEXT mm_usage{1, 0, VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT};
            mm_info.usageCountsCount = 1;
            mm_info.pUsageCounts = &mm_usage;
            auto mm_sizes = init<VkMicromapBuildSizesInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT);
            CHECK(device_get_build_sizes(mm, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mm_info, &mm_sizes));
            storage = f.buffer(mm_sizes.micromapSize, usage, false);
            track(mm, storage, mm_sizes.micromapSize, usage | VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT);
            auto mm_create = init<VkMicromapCreateInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT);
            mm_create.buffer = storage.handle;
            mm_create.size = mm_sizes.micromapSize;
            mm_create.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
            CHECK(device_create_micromap(mm, &mm_create, nullptr, &micromap));
            mm_info.dstMicromap = micromap;
            mm_info.data.deviceAddress = data.address;
            mm_info.triangleArray.deviceAddress = triangles.address;
            mm_info.triangleArrayStride = 8;
            mm_info.scratchData.deviceAddress = (scratch.address + 255) & ~uint64_t(255);
            Fixture::barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            CHECK(device_cmd_build(mm, command, 1, &mm_info));
            association.indexType = VK_INDEX_TYPE_UINT32;
            association.indexBuffer.deviceAddress = indices.address;
            association.indexStride = 0;
            association.micromap = micromap;
            geometry.geometry.triangles.pNext = &association;
            blas.build.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DISABLE_OPACITY_MICROMAPS_BIT_EXT;
        }
        // Pointer-array geometry form is copied and sanitized independently.
        const VkAccelerationStructureGeometryKHR* geometry_pointer = &geometry;
        blas.build.ppGeometries = &geometry_pointer;
        blas.build.pGeometries = nullptr;
        auto sizes = init<VkAccelerationStructureBuildSizesInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR);
        const uint32_t maximum = 1;
        CHECK(acceleration_get_build_sizes(as, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blas.build, &maximum, &sizes));
        require(geometry.geometry.triangles.pNext == (variant ? &association : nullptr), "application geometry pNext untouched");
        if (variant == 0) {
            const auto baseline_buffers = live_private_buffers, baseline_memory = live_private_memory;
            auto pool_info = init<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = f.family;
            VkCommandPool discard_pool{};
            CHECK(vkCreateCommandPool(f.device, &pool_info, nullptr, &discard_pool));
            auto allocate = init<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
            allocate.commandPool = discard_pool;
            allocate.commandBufferCount = 1;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            VkCommandBuffer discarded{};
            CHECK(vkAllocateCommandBuffers(f.device, &allocate, &discarded));
            for (unsigned attempt = 0; attempt < 100; ++attempt) {
                auto begin = init<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
                CHECK(vkBeginCommandBuffer(discarded, &begin));
                AccelerationRecordingRef discarded_blas, discarded_tlas;
                const auto* discarded_range = &blas.range;
                CHECK(acceleration_cmd_build(as, discarded, 1, &blas.build, &discarded_range, &discarded_blas));
                discarded_range = &tlas.range;
                CHECK(acceleration_cmd_build(as, discarded, 1, &tlas.build, &discarded_range, &discarded_tlas));
                CHECK(vkEndCommandBuffer(discarded));
                CHECK(vkResetCommandBuffer(discarded, 0));
                discarded_blas.reset();
                discarded_tlas.reset();
                require(live_private_buffers == baseline_buffers && live_private_memory == baseline_memory,
                        "record/reset without submission releases every abandoned BLAS/TLAS body and transient allocation");
            }
            vkFreeCommandBuffers(f.device, discard_pool, 1, &discarded);
            vkDestroyCommandPool(f.device, discard_pool, nullptr);
        }
        AccelerationRecordingRef blas_token, tlas_token;
        const auto* build_range = &blas.range;
        CHECK(acceleration_cmd_build(as, command, 1, &blas.build, &build_range, &blas_token));
        build_range = &tlas.range;
        CHECK(acceleration_cmd_build(as, command, 1, &tlas.build, &build_range, &tlas_token));
        require(acceleration_recordings_overlap(blas_token, blas_token) &&
                    !acceleration_recordings_overlap(blas_token, tlas_token) && !acceleration_recordings_overlap(blas_token, {}),
                "recording overlap compares touched AS identities without treating read dependencies as writers");
        if (variant == 0) {
            CHECK(vkEndCommandBuffer(command));
            submit_finished(f, submissions, command, {blas_token, tlas_token});
            // Publish newer bodies and retire the original AS ownership while
            // the original executable CB still owns its recorded allocations.
            auto newer = f.begin();
            AccelerationRecordingRef new_blas, new_tlas;
            build_range = &blas.range;
            CHECK(acceleration_cmd_build(as, newer, 1, &blas.build, &build_range, &new_blas));
            build_range = &tlas.range;
            CHECK(acceleration_cmd_build(as, newer, 1, &tlas.build, &build_range, &new_tlas));
            require(acceleration_recordings_overlap(blas_token, new_blas) && acceleration_recordings_overlap(tlas_token, new_tlas),
                    "rebuild recordings of same AS overlap for safe submission retirement");
            submit_build(f, submissions, newer, {new_blas, new_tlas});
            new_blas.reset();
            new_tlas.reset();
            // Replay the older executable CB, then regain active ownership of
            // its bodies before its recording token is released below.
            submit_finished(f, submissions, command, {blas_token, tlas_token});
            vkFreeCommandBuffers(f.device, f.pool, 1, &command);
        } else
            submit_build(f, submissions, command, {blas_token, tlas_token}, variant == 2);
        blas_token.reset();
        tlas_token.reset();
        if (variant == 0) {
            require(live_private_buffers == 4, "replayed old CB bodies survive token release with exactly one body per AS");
            for (unsigned rebuild = 0; rebuild < 3; ++rebuild) {
                auto newer = f.begin();
                AccelerationRecordingRef new_blas, new_tlas;
                build_range = &blas.range;
                CHECK(acceleration_cmd_build(as, newer, 1, &blas.build, &build_range, &new_blas));
                build_range = &tlas.range;
                CHECK(acceleration_cmd_build(as, newer, 1, &tlas.build, &build_range, &new_tlas));
                submit_build(f, submissions, newer, {new_blas, new_tlas});
                new_blas.reset();
                new_tlas.reset();
                require(live_private_buffers == 4, "fence-only rebuild retirement retains bounded live private buffers");
            }
        }
        // All public build inputs and the micromap are destroyed before traversal.
        release(f, mm, input);
        if (variant) {
            device_destroy_micromap(mm, micromap, nullptr);
            release(f, mm, indices);
            release(f, mm, triangles);
            release(f, mm, data);
            release(f, mm, storage);
            release(f, mm, scratch);
        }
        VkDeviceAddress native{}, root{};
        CHECK(acceleration_identity(as, tlas.handle, &native, &root));
        const uint64_t push[] = {native, root, output.address};
        command = f.begin();
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(command, 1, 1, 1);
        Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_ACCESS_HOST_READ_BIT);
        f.submit(command);
        const auto* result = static_cast<const uint32_t*>(output.mapped);
        if (variant == 3) {
            // The TLAS remains valid but its BLAS root must be invalidated by
            // semantic GPU index failure. The classifier safely fails closed.
            require(result[0] == 0 && result[1] != 0 && result[2] == 0 && result[3] == 1,
                    "invalid GPU OMM index never publishes a ready BLAS root");
        } else
            require(result[0] == 1 && result[1] == 0 && result[2] == 1 && result[3] == 1,
                    "native query + production classifier accepts expected hit after source destruction");
        require(result[4] == 77, "native custom index preserved");
        require(result[5] == uint32_t(instance.flags), "GPU captures original OMM instance flags by original instance ID");
        require(result[6] == 1 && result[7] == 1, "status-gated root ready and generation matches GPU body");
        std::printf("PASS acceleration variant %u: hit/status/decision/candidates=%u/%u/%u/%u flags=%u\n", variant, result[0],
                    result[1], result[2], result[3], result[5]);
        acceleration_untrack(as, tlas.handle);
        acceleration_untrack(as, blas.handle);
        require(live_private_buffers == 0 && live_private_memory == 0,
                "untracking completed AS releases all private buffers and memory");
    }
    require(native_builds == 216 && sanitized_queries == 4,
            "all 200 discarded plus sixteen submitted builds and four sanitized size queries reached native API");
    unbound_descriptor_query(f, mm, as, submissions, pipeline, layout, output);
    native_update_tests(f, mm, as, submissions, pipeline, layout, output);
    CHECK(vkDeviceWaitIdle(f.device));
    acceleration_collect_idle(as);
    submissions_destroy(submissions);
    acceleration_backend_destroy(as);
    device_backend_destroy(mm);
    vkDestroyPipeline(f.device, pipeline, nullptr);
    vkDestroyShaderModule(f.device, module, nullptr);
    vkDestroyPipelineLayout(f.device, layout, nullptr);
    std::printf("PASS native acceleration integration: %u assertions\n", assertions);
}
