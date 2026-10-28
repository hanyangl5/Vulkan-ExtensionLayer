/* SPDX-License-Identifier: MIT
 * Public GPU construction/serialization/deserialization. Only the serialized
 * artifact is mapped; AS output and scratch storage remain device-local.
 */
#define main vkrt_unused_serialization_gpu_build_main
#include "test_gpu_build.c"
#undef main
#include "vkrt_as_serialization.h"

struct serial_buffer {
    struct gpu_buf storage;
    VkDeviceAddress address;
    uint8_t *bytes;
    uint64_t capacity;
};
static struct built_as source_blas, source_tlas, restored_blas, restored_tlas;
static struct serial_buffer blas_serial, tlas_serial, repeated_serial;
static PFN_vkCmdCopyAccelerationStructureToMemoryKHR serialize_as;
static PFN_vkCmdCopyMemoryToAccelerationStructureKHR deserialize_as;

static void storage(struct built_as *as, uint64_t size) {
    memset(as, 0, sizeof(*as));
    as->size = size;
    as->offset = 256;
    /* A legal ordinary-buffer suballocation can put the physical AS header
     * at 16 mod 256. Public AS values still satisfy their 256-byte contract. */
    as->output.size = size + 512;
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = as->output.size,
                                 .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(g_dev, &buffer, NULL, &as->output.buf));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(g_dev, as->output.buf, &requirements);
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = requirements.size + requirements.alignment,
        .memoryTypeIndex = find_mem_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(g_dev, &allocation, NULL, &as->output.mem));
    CHECK(vkBindBufferMemory(g_dev, as->output.buf, as->output.mem, requirements.alignment));
    VkBufferDeviceAddressInfo address = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = as->output.buf};
    as->output.addr = vkGetBufferDeviceAddress(g_dev, &address);
    printf("Serialize AS storage: bindAlignment=%llu physicalRootMod256=%llu\n", (unsigned long long)requirements.alignment,
           (unsigned long long)((as->output.addr + 256) & 255));
    VkAccelerationStructureCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                 .buffer = as->output.buf,
                                                 .offset = 256,
                                                 .size = size,
                                                 .type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR};
    CHECK(pCreateAS(g_dev, &info, NULL, &as->handle));
    if (as->output.mapped) {
        fprintf(stderr, "FAIL mapped AS output\n");
        ++g_fail;
    }
}
static uint64_t source(struct built_as *as, VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR *geometry,
                       uint32_t count) {
    VkAccelerationStructureBuildGeometryInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                        .type = type,
                                                        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                        .geometryCount = 1,
                                                        .pGeometries = geometry};
    VkAccelerationStructureBuildSizesInfoKHR sizes = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    PFN_vkGetAccelerationStructureBuildSizesKHR get = (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
    get(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
    storage(as, sizes.accelerationStructureSize + 1024);
    as->info = info;
    as->info.dstAccelerationStructure = as->handle;
    as->ranges[0].primitiveCount = count;
    buf_create(sizes.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &as->scratch);
    as->info.scratchData.deviceAddress = (as->scratch.addr + 255) & ~UINT64_C(255);
    return sizes.accelerationStructureSize;
}
static VkDeviceAddress address_of(VkAccelerationStructureKHR as) {
    PFN_vkGetAccelerationStructureDeviceAddressKHR get =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureDeviceAddressKHR");
    VkAccelerationStructureDeviceAddressInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                        .accelerationStructure = as};
    return get(g_dev, &info);
}
static void serial_storage(struct serial_buffer *serial, uint64_t capacity) {
    serial->capacity = capacity;
    buf_create(capacity + 255,
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &serial->storage);
    serial->address = (serial->storage.addr + 255) & ~UINT64_C(255);
    serial->bytes = (uint8_t *)serial->storage.mapped + (size_t)(serial->address - serial->storage.addr);
    memset(serial->bytes, 0xa5, (size_t)capacity);
}
static uint64_t u64(const struct serial_buffer *serial, uint64_t offset) {
    uint64_t value;
    memcpy(&value, serial->bytes + (size_t)offset, 8);
    return value;
}
static void dependency(VkCommandBuffer command) {
    VkMemoryBarrier memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                               VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_TRANSFER_READ_BIT |
                                               VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &memory, 0, NULL, 0, NULL);
}
static void serialize(VkCommandBuffer command, VkAccelerationStructureKHR as, const struct serial_buffer *serial) {
    VkCopyAccelerationStructureToMemoryInfoKHR info = {.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR,
                                                       .src = as,
                                                       .dst.deviceAddress = serial->address,
                                                       .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR};
    serialize_as(command, &info);
    dependency(command);
}
static void deserialize(VkCommandBuffer command, VkAccelerationStructureKHR as, const struct serial_buffer *serial) {
    VkCopyMemoryToAccelerationStructureInfoKHR info = {.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR,
                                                       .src.deviceAddress = serial->address,
                                                       .dst = as,
                                                       .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR};
    deserialize_as(command, &info);
    dependency(command);
}
static void first_commands(VkCommandBuffer command) {
    const VkAccelerationStructureBuildRangeInfoKHR *range = source_blas.ranges;
    pBuild(command, 1, &source_blas.info, &range);
    dependency(command);
    range = source_tlas.ranges;
    pBuild(command, 1, &source_tlas.info, &range);
    dependency(command);
    serialize(command, source_blas.handle, &blas_serial);
    serialize(command, source_tlas.handle, &tlas_serial);
}
static void restore_commands(VkCommandBuffer command) {
    /* Vulkan explicitly permits list entries referring to created, unbuilt
     * BLAS objects. Build the target TLAS before populating its new BLAS. */
    deserialize(command, restored_tlas.handle, &tlas_serial);
    deserialize(command, restored_blas.handle, &blas_serial);
}
static void repeat_commands(VkCommandBuffer command) { serialize(command, restored_tlas.handle, &repeated_serial); }
static void hit(VkAccelerationStructureKHR tlas, unsigned instance) {
    const float origin[] = {3 + instance * 10.0f, 0, 2}, direction[] = {0, 0, -1};
    struct out_block result;
    run_ray(tlas, origin, direction, 0, 100, 0, 255, &result);
    expect_u("serialized hit", result.hit_type, 1);
    expect_u("serialized primitive", result.primitive_index, 0);
    expect_u("serialized instance", result.instance_custom_index, 77 + instance);
    expect_u("serialized getters", result.getter_errors, 0);
    expect_f("serialized transformed distance", result.t, 3, 1e-5f);
}
static void prefix(const struct serial_buffer *serial, uint32_t count, VkDeviceAddress expected_handle) {
    const uint64_t physical = u64(serial, 40), total = u64(serial, 32);
    if (total > serial->capacity || total != vkrt_as_serialization_size(physical, count) || u64(serial, 48) != count) {
        fprintf(stderr, "FAIL serialization prefix size/count\n");
        ++g_fail;
        return;
    }
    for (uint32_t i = 0; i < count; ++i)
        if (u64(serial, 56 + (uint64_t)i * 8) != expected_handle) {
            fprintf(stderr, "FAIL relocation entry is not the public BLAS value\n");
            ++g_fail;
        }
}
int main(void) {
    init_vulkan();
    pBuild = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    serialize_as = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdCopyAccelerationStructureToMemoryKHR");
    deserialize_as = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdCopyMemoryToAccelerationStructureKHR");
    PFN_vkGetDeviceAccelerationStructureCompatibilityKHR compatible =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetDeviceAccelerationStructureCompatibilityKHR");
    if (!pBuild || !serialize_as || !deserialize_as || !compatible) return 1;
    struct gpu_buf vertices, instances;
    const float triangle[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    buf_create(sizeof(triangle),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vertices);
    memcpy(vertices.mapped, triangle, sizeof(triangle));
    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                               .vertexData.deviceAddress = vertices.addr,
                               .vertexStride = 12,
                               .maxVertex = 2,
                               .indexType = VK_INDEX_TYPE_NONE_KHR}};
    uint64_t blas_bound = source(&source_blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geometry, 1);
    VkDeviceAddress original_public = address_of(source_blas.handle);
    VkAccelerationStructureInstanceKHR records[3];
    memset(records, 0, sizeof(records));
    for (unsigned i = 0; i < 3; ++i) {
        records[i].transform = (VkTransformMatrixKHR){{{2, 0, 0, 3 + i * 10.0f}, {0, 1, 0, 0}, {0, 0, 1, -1}}};
        records[i].instanceCustomIndex = 77 + i;
        records[i].mask = 255;
        records[i].instanceShaderBindingTableRecordOffset = 0x12345;
        records[i].accelerationStructureReference = original_public;
    }
    buf_create(sizeof(records),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &instances);
    memcpy(instances.mapped, records, sizeof(records));
    VkAccelerationStructureGeometryKHR top = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = instances.addr}};
    uint64_t top_bound = source(&source_tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &top, 3);
    /* Canonical BVH4's queried build size bounds its built representation;
     * this allocation deliberately exceeds the exact GPU serialization size. */
    serial_storage(&blas_serial, vkrt_as_serialization_size(blas_bound, 0));
    serial_storage(&tlas_serial, vkrt_as_serialization_size(top_bound, 3));
    serial_storage(&repeated_serial, vkrt_as_serialization_size(top_bound, 3));
    g_before_dispatch = first_commands;
    hit(source_tlas.handle, 0);
    g_before_dispatch = NULL;
    prefix(&blas_serial, 0, 0);
    prefix(&tlas_serial, 3, original_public);
    if (u64(&blas_serial, 40) >= source_blas.size || u64(&tlas_serial, 40) >= source_tlas.size) {
        fprintf(stderr, "FAIL serialization used oversized object capacity instead of GPU size\n");
        ++g_fail;
    }
    VkPhysicalDeviceIDProperties ids = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ids};
    vkGetPhysicalDeviceProperties2(g_pdev, &properties);
    if (memcmp(tlas_serial.bytes, ids.driverUUID, VK_UUID_SIZE)) {
        fprintf(stderr, "FAIL driverUUID prefix\n");
        ++g_fail;
    }
    uint8_t version[32];
    memcpy(version, tlas_serial.bytes, sizeof(version));
    VkAccelerationStructureVersionInfoKHR version_info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR,
                                                          .pVersionData = version};
    VkAccelerationStructureCompatibilityKHR result;
    compatible(g_dev, &version_info, &result);
    expect_u("matching compatibility", result, VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR);
    for (unsigned i = 0; i < sizeof(version); ++i) {
        version[i] ^= 1;
        compatible(g_dev, &version_info, &result);
        version[i] ^= 1;
        expect_u("changed UUID incompatible", result, VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR);
    }
    destroy_as(&source_tlas);
    destroy_as(&source_blas);
    storage(&restored_blas, u64(&blas_serial, 40));
    storage(&restored_tlas, u64(&tlas_serial, 40));
    /* Derive the unbuilt BLAS value through the guaranteed GENERIC relative
     * offset relation. Querying a sibling must publish the bound buffer base;
     * this target BLAS itself has never been passed to GetASAddress. */
    VkAccelerationStructureCreateInfoKHR anchor_info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                        .buffer = restored_blas.output.buf,
                                                        .offset = 0,
                                                        .size = restored_blas.size,
                                                        .type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR};
    VkAccelerationStructureKHR anchor;
    CHECK(pCreateAS(g_dev, &anchor_info, NULL, &anchor));
    VkDeviceAddress replacement = address_of(anchor) + 256;
    pDestroyAS(g_dev, anchor, NULL);
    for (unsigned i = 0; i < 3; ++i) memcpy(tlas_serial.bytes + 56 + i * 8, &replacement, 8);
    /* The destination TLAS has never been queried for an AS address. */
    g_before_dispatch = restore_commands;
    hit(restored_tlas.handle, 0);
    g_before_dispatch = NULL;
    hit(restored_tlas.handle, 1);
    hit(restored_tlas.handle, 2);
    g_before_dispatch = repeat_commands;
    hit(restored_tlas.handle, 2);
    g_before_dispatch = NULL;
    prefix(&repeated_serial, 3, replacement);
    destroy_as(&restored_tlas);
    destroy_as(&restored_blas);
    buf_destroy(&repeated_serial.storage);
    buf_destroy(&tlas_serial.storage);
    buf_destroy(&blas_serial.storage);
    buf_destroy(&instances);
    buf_destroy(&vertices);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    printf("Public GPU AS serialization %s (%d failures, rewritten public BLAS list)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail != 0;
}
