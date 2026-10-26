/* SPDX-License-Identifier: MIT
 * Legal public GPU builds, CLONE/COMPACT and ray queries. AS output and scratch
 * allocations are never mapped; the copy implementation cannot use CPU reads.
 */
#define main vkrt_unused_gpu_build_main
#include "test_gpu_build.c"
#undef main

static PFN_vkCmdCopyAccelerationStructureKHR pCopy;
static struct built_as copy_blas_source, copy_blas_clone, copy_blas_compact;
static struct built_as copy_tlas_source, copy_tlas_clone, copy_tlas_compact;

static void prepare_copy_storage(struct built_as *out, VkDeviceSize size) {
    memset(out, 0, sizeof(*out));
    out->size = size;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buf_create(size + 512, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &out->output);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = out->output.buf,
                                                   .offset = 256,
                                                   .size = size,
                                                   .type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR};
    CHECK(pCreateAS(g_dev, &create, NULL, &out->handle));
    if (out->output.mapped) {
        fprintf(stderr, "AS copy fixture unexpectedly mapped AS storage\n");
        ++g_fail;
    }
}

static VkDeviceSize prepare_copy_source(struct built_as *out, VkAccelerationStructureTypeKHR type,
                                        const VkAccelerationStructureGeometryKHR *geometry,
                                        const VkAccelerationStructureBuildRangeInfoKHR *range) {
    VkAccelerationStructureBuildGeometryInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                        .type = type,
                                                        .flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
                                                        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                        .geometryCount = 1,
                                                        .pGeometries = geometry};
    VkAccelerationStructureBuildSizesInfoKHR sizes = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
    get_sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &range->primitiveCount, &sizes);
    /* A destination of queried size is valid even when the source object was
     * created oversized. Copy length must come from the GPU-built header. */
    prepare_copy_storage(out, sizes.accelerationStructureSize + 1024);
    out->info = info;
    out->info.dstAccelerationStructure = out->handle;
    out->ranges[0] = *range;
    buf_create(sizes.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &out->scratch);
    out->info.scratchData.deviceAddress = (out->scratch.addr + 255) & ~UINT64_C(255);
    return sizes.accelerationStructureSize;
}

static void as_dependency(VkCommandBuffer command) {
    VkMemoryBarrier memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &memory, 0, NULL, 0, NULL);
}
static void copy_one(VkCommandBuffer command, VkAccelerationStructureKHR source, VkAccelerationStructureKHR destination,
                     VkCopyAccelerationStructureModeKHR mode) {
    VkCopyAccelerationStructureInfoKHR copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR, .src = source, .dst = destination, .mode = mode};
    pCopy(command, &copy);
    as_dependency(command);
}
static void record_copy_chain(VkCommandBuffer command) {
    const VkAccelerationStructureBuildRangeInfoKHR *range = copy_blas_source.ranges;
    pBuild(command, 1, &copy_blas_source.info, &range);
    as_dependency(command);
    copy_one(command, copy_blas_source.handle, copy_blas_clone.handle, VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR);
    copy_one(command, copy_blas_clone.handle, copy_blas_compact.handle, VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR);
    range = copy_tlas_source.ranges;
    pBuild(command, 1, &copy_tlas_source.info, &range);
    as_dependency(command);
    copy_one(command, copy_tlas_source.handle, copy_tlas_clone.handle, VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR);
    copy_one(command, copy_tlas_clone.handle, copy_tlas_compact.handle, VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR);
}
static void expect_copy_hit(float x, uint32_t primitive) {
    const float origin[] = {x, 0, 2}, direction[] = {0, 0, -1};
    struct out_block result;
    run_ray(copy_tlas_compact.handle, origin, direction, 0, 100, 0, 255, &result);
    expect_u("copied committed triangle", result.hit_type, 1);
    expect_u("copied primitive", result.primitive_index, primitive);
    expect_u("copied custom instance", result.instance_custom_index, 77);
    expect_u("copied getter semantics", result.getter_errors, 0);
    expect_f("copied transformed distance", result.t, 3, 1e-5f);
}

int main(void) {
    init_vulkan();
    pBuild = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    pCopy = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdCopyAccelerationStructureKHR");
    if (!pBuild || !pCopy) return 1;
    /* The encoded BLAS exceeds one 1024-workgroup copy pass. Its query hits
     * primitive zero; all remaining triangles lie far outside this ray. */
    enum { triangle_count = 4097 };
    struct gpu_buf vertices, instances;
    buf_create((VkDeviceSize)triangle_count * 9 * sizeof(float),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vertices);
    const float triangle[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    for (uint32_t i = 0; i < triangle_count; ++i) {
        float *v = (float *)vertices.mapped + (size_t)i * 9;
        memcpy(v, triangle, sizeof(triangle));
        if (i)
            for (uint32_t vertex = 0; vertex < 3; ++vertex) v[vertex * 3] += 100.0f + (float)i * 4.0f;
    }
    VkAccelerationStructureGeometryKHR triangles = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                               .vertexData.deviceAddress = vertices.addr,
                               .vertexStride = 12,
                               .maxVertex = triangle_count * 3 - 1,
                               .indexType = VK_INDEX_TYPE_NONE_KHR}};
    VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = triangle_count};
    VkDeviceSize blas_size =
        prepare_copy_source(&copy_blas_source, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &triangles, &range);
    prepare_copy_storage(&copy_blas_clone, blas_size);
    prepare_copy_storage(&copy_blas_compact, blas_size);
    PFN_vkGetAccelerationStructureDeviceAddressKHR get_address =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureDeviceAddressKHR");
    VkAccelerationStructureDeviceAddressInfoKHR address_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = copy_blas_compact.handle};
    VkAccelerationStructureInstanceKHR instance = {.transform.matrix = {{2, 0, 0, 3}, {0, 1, 0, 0}, {0, 0, 1, -1}},
                                                   .instanceCustomIndex = 77,
                                                   .mask = 255,
                                                   .instanceShaderBindingTableRecordOffset = 0x12345,
                                                   .accelerationStructureReference = get_address(g_dev, &address_info)};
    buf_create(sizeof(instance),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &instances);
    memcpy(instances.mapped, &instance, sizeof(instance));
    VkAccelerationStructureGeometryKHR instance_geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = instances.addr}};
    range.primitiveCount = 1;
    VkDeviceSize tlas_size =
        prepare_copy_source(&copy_tlas_source, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &instance_geometry, &range);
    prepare_copy_storage(&copy_tlas_clone, tlas_size);
    prepare_copy_storage(&copy_tlas_compact, tlas_size);
    /* Neither TLAS copy destination is queried for an address. Construction
     * must publish its mapping before the descriptor-driven ray query. */
    g_before_dispatch = record_copy_chain;
    expect_copy_hit(3, 0);
    g_before_dispatch = NULL;
    expect_copy_hit(3 + 2 * (100 + (triangle_count - 1) * 4), triangle_count - 1);
    destroy_as(&copy_tlas_clone);
    destroy_as(&copy_tlas_source);
    destroy_as(&copy_blas_clone);
    destroy_as(&copy_blas_source);
    expect_copy_hit(3, 0); /* Destination retains the same physical BLAS reference. */
    const float miss_origin[] = {30, 0, 2}, direction[] = {0, 0, -1};
    struct out_block miss;
    run_ray(copy_tlas_compact.handle, miss_origin, direction, 0, 100, 0, 255, &miss);
    expect_u("copied miss", miss.hit_type, 0);
    destroy_as(&copy_tlas_compact);
    destroy_as(&copy_blas_compact);
    buf_destroy(&instances);
    buf_destroy(&vertices);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    printf("Public GPU AS CLONE/COMPACT %s (%d failures, unmapped AS storage)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail != 0;
}
