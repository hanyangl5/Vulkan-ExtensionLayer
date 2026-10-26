/* SPDX-License-Identifier: MIT
 * Public UPDATE semantics using GPU builds and unmapped AS/scratch storage.
 * The same scene is valid for the software layer and the native ICD.
 */
#define main vkrt_unused_gpu_build_main
#include "test_gpu_build.c"
#undef main

static void dependency(VkCommandBuffer cmd) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                     .commandPool = g_pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                     .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(g_dev, &a, &cmd));
    VkCommandBufferBeginInfo b = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &b));
    dependency(cmd);
    return cmd;
}
static void submit(VkCommandBuffer cmd) {
    dependency(cmd);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(g_queue, 1, &s, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(g_queue));
    vkFreeCommandBuffers(g_dev, g_pool, 1, &cmd);
}
static void record(VkCommandBuffer cmd, const struct built_as *as, VkAccelerationStructureKHR source) {
    VkAccelerationStructureBuildGeometryInfoKHR info = as->info;
    info.srcAccelerationStructure = source;
    info.mode = source ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    const VkAccelerationStructureBuildRangeInfoKHR *range = as->ranges;
    pBuild(cmd, 1, &info, &range);
    dependency(cmd);
}
static void prepare(struct built_as *as, VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR *geometry) {
    memset(as, 0, sizeof(*as));
    as->info =
        (VkAccelerationStructureBuildGeometryInfoKHR){.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                      .type = type,
                                                      .flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
                                                      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                      .geometryCount = 1,
                                                      .pGeometries = geometry};
    as->ranges[0].primitiveCount = 1;
    uint32_t count = 1;
    VkAccelerationStructureBuildSizesInfoKHR size = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
    sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &as->info, &count, &size);
    if (!size.updateScratchSize || (g_software && size.updateScratchSize != size.buildScratchSize)) {
        fprintf(stderr, "FAIL missing/full-rebuild UPDATE scratch contract\n");
        ++g_fail;
    }
    as->size = size.accelerationStructureSize;
    as->offset = 256;
    buf_create(as->size + 256,
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &as->output);
    VkAccelerationStructureCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                               .buffer = as->output.buf,
                                               .offset = 256,
                                               .size = as->size,
                                               .type = type};
    CHECK(pCreateAS(g_dev, &ci, NULL, &as->handle));
    as->info.dstAccelerationStructure = as->handle;
    VkDeviceSize scratch = size.buildScratchSize > size.updateScratchSize ? size.buildScratchSize : size.updateScratchSize;
    buf_create(scratch + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &as->scratch);
    as->info.scratchData.deviceAddress = (as->scratch.addr + 255) & ~UINT64_C(255);
    VkAccelerationStructureDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                      .accelerationStructure = as->handle};
    PFN_vkGetAccelerationStructureDeviceAddressKHR address =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureDeviceAddressKHR");
    as->address = address(g_dev, &ai);
    expect_u("unmapped output", as->output.mapped != NULL, 0);
    expect_u("unmapped scratch", as->scratch.mapped != NULL, 0);
}
static void snapshot(VkCommandBuffer cmd, const struct built_as *as, const struct gpu_buf *readback, VkDeviceSize offset) {
    VkBufferCopy copy = {256, offset, as->size};
    vkCmdCopyBuffer(cmd, as->output.buf, readback->buf, 1, &copy);
    dependency(cmd);
}
static void expect(const struct built_as *tlas, float x, bool hit) {
    const float origin[] = {x, 0, 2}, direction[] = {0, 0, -1};
    struct out_block out;
    run_ray(tlas->handle, origin, direction, 0, 100, 0, 255, &out);
    expect_u("updated triangle", out.hit_type, hit ? 1 : 0);
    if (hit) {
        expect_u("updated primitive", out.primitive_index, 0);
        expect_u("updated custom instance", out.instance_custom_index, 77);
        expect_u("updated getter semantics", out.getter_errors, 0);
        expect_f("updated distance", out.t, 2, 1e-5f);
    }
}
int main(void) {
    init_vulkan();
    g_software = getenv("VK_INSTANCE_LAYERS") && strstr(getenv("VK_INSTANCE_LAYERS"), "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY");
    pBuild = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    if (!pBuild) return 1;
    struct gpu_buf vertices, instances, readback;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkBufferUsageFlags input =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buf_create(27 * sizeof(float), input, host, &vertices);
    const float triangle[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    for (unsigned i = 0; i < 3; ++i) {
        float *p = (float *)vertices.mapped + 9 * i;
        memcpy(p, triangle, sizeof(triangle));
        for (unsigned j = 0; j < 3; ++j) p[3 * j] += 4.0f * i;
    }
    VkAccelerationStructureGeometryKHR geometry[3];
    for (unsigned i = 0; i < 3; ++i)
        geometry[i] = (VkAccelerationStructureGeometryKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
            .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                   .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                                   .vertexData.deviceAddress = vertices.addr + i * sizeof(triangle),
                                   .vertexStride = 12,
                                   .maxVertex = 2,
                                   .indexType = VK_INDEX_TYPE_NONE_KHR}};
    struct built_as source, destination, source_tlas, destination_tlas, translated_tlas;
    prepare(&source, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geometry[0]);
    prepare(&destination, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geometry[1]);
    buf_create(2 * sizeof(VkAccelerationStructureInstanceKHR), input, host, &instances);
    VkAccelerationStructureInstanceKHR instance = {.transform.matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}},
                                                   .instanceCustomIndex = 77,
                                                   .mask = 255,
                                                   .accelerationStructureReference = source.address};
    memcpy(instances.mapped, &instance, sizeof(instance));
    instance.accelerationStructureReference = destination.address;
    memcpy((char *)instances.mapped + sizeof(instance), &instance, sizeof(instance));
    VkAccelerationStructureGeometryKHR top[2];
    for (unsigned i = 0; i < 2; ++i)
        top[i] = (VkAccelerationStructureGeometryKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                                   .data.deviceAddress = instances.addr + i * sizeof(instance)}};
    prepare(&source_tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &top[0]);
    prepare(&destination_tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &top[1]);
    prepare(&translated_tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &top[0]);
    buf_create(2 * source.size + 2 * source_tlas.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host, &readback);
    VkCommandBuffer cmd = begin();
    vkCmdFillBuffer(cmd, source.output.buf, 0, VK_WHOLE_SIZE, 0);
    dependency(cmd);
    record(cmd, &source, VK_NULL_HANDLE);
    record(cmd, &source_tlas, VK_NULL_HANDLE);
    snapshot(cmd, &source, &readback, 0);
    record(cmd, &destination, source.handle);
    snapshot(cmd, &source, &readback, source.size);
    record(cmd, &destination_tlas, VK_NULL_HANDLE);
    submit(cmd);
    expect_u("out-of-place BLAS preserves every source storage byte",
             memcmp(readback.mapped, (char *)readback.mapped + source.size, (size_t)source.size) != 0, 0);
    expect(&source_tlas, 0, true);
    expect(&source_tlas, 4, false);
    expect(&destination_tlas, 4, true);
    expect(&destination_tlas, 0, false);
    VkDeviceAddress stable_address = source.address;
    source.info.pGeometries = &geometry[2];
    cmd = begin();
    record(cmd, &source, source.handle);
    record(cmd, &source_tlas, source_tlas.handle);
    submit(cmd);
    VkAccelerationStructureDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                      .accelerationStructure = source.handle};
    PFN_vkGetAccelerationStructureDeviceAddressKHR address =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureDeviceAddressKHR");
    expect_u("in-place public BLAS value stable", address(g_dev, &ai) == stable_address, 1);
    expect(&source_tlas, 8, true);
    expect(&source_tlas, 0, false);
    expect(&destination_tlas, 4, true);
    instance = *(VkAccelerationStructureInstanceKHR *)instances.mapped;
    instance.transform.matrix[0][3] = 3;
    memcpy(instances.mapped, &instance, sizeof(instance));
    cmd = begin();
    snapshot(cmd, &source_tlas, &readback, 2 * source.size);
    record(cmd, &translated_tlas, source_tlas.handle);
    snapshot(cmd, &source_tlas, &readback, 2 * source.size + source_tlas.size);
    submit(cmd);
    expect_u("out-of-place TLAS preserves every source storage byte",
             memcmp((char *)readback.mapped + 2 * source.size, (char *)readback.mapped + 2 * source.size + source_tlas.size,
                    (size_t)source_tlas.size) != 0,
             0);
    expect(&source_tlas, 8, true);
    expect(&translated_tlas, 11, true);
    expect(&translated_tlas, 8, false);
    destroy_as(&translated_tlas);
    destroy_as(&destination_tlas);
    destroy_as(&source_tlas);
    destroy_as(&destination);
    destroy_as(&source);
    buf_destroy(&readback);
    buf_destroy(&instances);
    buf_destroy(&vertices);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    printf("Public GPU AS UPDATE %s (%d failures; unmapped storage, in/out-of-place BLAS/TLAS)\n", g_fail ? "FAIL" : "PASS",
           g_fail);
    return g_fail != 0;
}
