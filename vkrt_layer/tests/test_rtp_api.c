/* SPDX-License-Identifier: MIT
 * Actual canonical RTP API execution. --capability-harness is an explicit
 * development mode while base RTP advertisement remains masked: it calls the
 * same public layer entrypoints on an AS/RQ device, not a separate scheduler.
 * This mode validates downstream ICD traffic; it does not prove advertised
 * RTP-feature conformance and must not enable validation above the layer.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdbool.h>
static bool capability_harness, advertised;
#include <vulkan/vulkan.h>
static VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *a,
                              VkDevice *out);
#define vkCreateDevice create_device
#define VKRT_TEST_FIXTURE_ONLY
#include "00_single_tri/main.c"
#undef vkCreateDevice

static VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *a,
                              VkDevice *out) {
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceMaintenance4Features maintenance = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
    supported.pNext = &maintenance;
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported};
    PFN_vkGetPhysicalDeviceFeatures2 query = (void *)vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2");
    query(physical, &features);
    advertised = supported.rayTracingPipeline != 0;
    if (!maintenance.maintenance4) {
        printf("Layout destruction/rebind proof requires maintenance4\n");
        exit(77);
    }
    if (!advertised && !capability_harness) {
        printf("Base RTP advertisement pending; use explicit --capability-harness for development\n");
        exit(77);
    }
    VkDeviceCreateInfo local = *info;
    maintenance.pNext = (void *)info->pNext;
    local.pNext = &maintenance;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enabled = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &maintenance,
        .rayTracingPipeline = VK_TRUE};
    const char **extensions = malloc((info->enabledExtensionCount + 2) * sizeof(*extensions));
    if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(extensions, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof(*extensions));
    extensions[local.enabledExtensionCount++] = VK_KHR_MAINTENANCE_4_EXTENSION_NAME;
    local.ppEnabledExtensionNames = extensions;
    if (advertised) {
        extensions[local.enabledExtensionCount++] = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;
        local.pNext = &enabled;
        local.ppEnabledExtensionNames = extensions;
    }
    VkResult result = vkCreateDevice(physical, &local, a, out);
    free(extensions);
    return result;
}
#define REQUIRE(v)                                          \
    do {                                                    \
        if (!(v)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #v); \
            ++g_fail;                                       \
        }                                                   \
    } while (0)
struct acceleration {
    struct gpu_buf buffer, scratch;
    VkAccelerationStructureKHR handle;
    VkDeviceAddress address;
    VkAccelerationStructureBuildGeometryInfoKHR info;
    VkAccelerationStructureBuildRangeInfoKHR range;
};
static void prepare(struct acceleration *as, VkAccelerationStructureTypeKHR type,
                    const VkAccelerationStructureGeometryKHR *geometry) {
    memset(as, 0, sizeof(*as));
    as->range.primitiveCount = 1;
    as->info =
        (VkAccelerationStructureBuildGeometryInfoKHR){.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                      .type = type,
                                                      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                      .geometryCount = 1,
                                                      .pGeometries = geometry};
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
    uint32_t count = 1;
    VkAccelerationStructureBuildSizesInfoKHR size = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &as->info, &count, &size);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    buf_create(size.accelerationStructureSize,
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host,
               &as->buffer);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .type = type,
                                                   .buffer = as->buffer.buf,
                                                   .size = size.accelerationStructureSize};
    CHECK(pCreateAS(g_dev, &create, NULL, &as->handle));
    VkAccelerationStructureDeviceAddressInfoKHR address = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = as->handle};
    PFN_vkGetAccelerationStructureDeviceAddressKHR get =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureDeviceAddressKHR");
    as->address = get(g_dev, &address);
    buf_create(size.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host,
               &as->scratch);
    as->info.dstAccelerationStructure = as->handle;
    as->info.scratchData.deviceAddress = (as->scratch.addr + 255) & ~UINT64_C(255);
}
static VkShaderModule shader(const char *path) {
    size_t count;
    uint32_t *code = read_spv(path, &count);
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = count * 4, .pCode = code};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(g_dev, &ci, NULL, &module));
    free(code);
    return module;
}
static void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                           VkAccessFlags dstAccess) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = srcAccess, .dstAccessMask = dstAccess};
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &b, 0, NULL, 0, NULL);
}
int main(int argc, char **argv) {
    capability_harness = argc == 6 && !strcmp(argv[5], "--capability-harness");
    if (argc != 5 && !capability_harness) {
        fprintf(stderr, "usage: %s rgen.spv miss.spv chit.spv compute.spv [--capability-harness]\n", argv[0]);
        return 1;
    }
    init_vulkan();
    PFN_vkCreateRayTracingPipelinesKHR create = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateRayTracingPipelinesKHR");
    PFN_vkGetRayTracingShaderGroupHandlesKHR handles = (void *)vkGetDeviceProcAddr(g_dev, "vkGetRayTracingShaderGroupHandlesKHR");
    PFN_vkGetRayTracingShaderGroupStackSizeKHR stack = (void *)vkGetDeviceProcAddr(g_dev, "vkGetRayTracingShaderGroupStackSizeKHR");
    PFN_vkCmdSetRayTracingPipelineStackSizeKHR set_stack =
        (void *)vkGetDeviceProcAddr(g_dev, "vkCmdSetRayTracingPipelineStackSizeKHR");
    PFN_vkCmdTraceRaysKHR trace = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdTraceRaysKHR");
    PFN_vkCmdBuildAccelerationStructuresKHR build = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    REQUIRE(create && handles && stack && set_stack && trace && build);
    if (g_fail) return 1;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    struct gpu_buf vertices, instances, output, sbt;
    buf_create(36, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &vertices);
    const float triangle[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    memcpy(vertices.mapped, triangle, sizeof(triangle));
    VkAccelerationStructureGeometryKHR geom = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                               .vertexData.deviceAddress = vertices.addr,
                               .vertexStride = 12,
                               .maxVertex = 2,
                               .indexType = VK_INDEX_TYPE_NONE_KHR}};
    struct acceleration blas, tlas;
    prepare(&blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geom);
    buf_create(sizeof(VkAccelerationStructureInstanceKHR),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &instances);
    *(VkAccelerationStructureInstanceKHR *)instances.mapped =
        (VkAccelerationStructureInstanceKHR){.transform = {.matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}},
                                             .mask = 255,
                                             .accelerationStructureReference = blas.address};
    VkAccelerationStructureGeometryKHR inst = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = instances.addr}};
    prepare(&tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &inst);
    buf_create(8 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, &output);
    buf_create(3 * 64, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host, &sbt);
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, NULL}};
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(g_dev, &dslci, NULL, &dsl));
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                         {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}};
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = pool_sizes};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(g_dev, &pci, NULL, &pool));
    VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = pool,
                                         .descriptorSetCount = 1,
                                         .pSetLayouts = &dsl};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(g_dev, &alloc, &set));
    VkDescriptorBufferInfo bi = {output.buf, 0, output.size};
    VkWriteDescriptorSetAccelerationStructureKHR as_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &tlas.handle};
    VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = 0,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bi},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .pNext = &as_write,
                                      .dstSet = set,
                                      .dstBinding = 1,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}};
    vkUpdateDescriptorSets(g_dev, 2, writes, 0, NULL);
    VkPushConstantRange ranges[] = {{VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 4},
                                    {VK_SHADER_STAGE_MISS_BIT_KHR, 8, 4},
                                    {VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 12, 4},
                                    {VK_SHADER_STAGE_COMPUTE_BIT, 4, 4}};
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &dsl,
                                      .pushConstantRangeCount = 4,
                                      .pPushConstantRanges = ranges};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(g_dev, &lci, NULL, &layout));
    VkShaderModule modules[4];
    for (unsigned i = 0; i < 4; ++i) modules[i] = shader(argv[i + 1]);
    VkPipelineShaderStageCreateInfo stages[3];
    VkShaderStageFlagBits kinds[] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
                                     VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
    for (unsigned i = 0; i < 3; ++i)
        stages[i] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = kinds[i], .module = modules[i], .pName = "main"};
    VkRayTracingShaderGroupCreateInfoKHR groups[3];
    for (unsigned i = 0; i < 3; ++i)
        groups[i] = (VkRayTracingShaderGroupCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type = i < 2 ? VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
            .generalShader = i < 2 ? i : VK_SHADER_UNUSED_KHR,
            .closestHitShader = i == 2 ? 2 : VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR};
    VkDynamicState dynamic = VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR;
    VkPipelineDynamicStateCreateInfo dynamic_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 1, .pDynamicStates = &dynamic};
    VkRayTracingPipelineCreateInfoKHR rtci = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
                                              .stageCount = 3,
                                              .pStages = stages,
                                              .groupCount = 3,
                                              .pGroups = groups,
                                              .maxPipelineRayRecursionDepth = 1,
                                              .pDynamicState = &dynamic_info,
                                              .layout = layout};
    VkPipeline rt;
    CHECK(create(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtci, NULL, &rt));
    VkComputePipelineCreateInfo compute_info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                          .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                          .module = modules[3],
                                                          .pName = "main"},
                                                .layout = layout};
    VkPipeline compute;
    CHECK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &compute_info, NULL, &compute));
    for (unsigned i = 0; i < 4; ++i) vkDestroyShaderModule(g_dev, modules[i], NULL);
    uint8_t handle_bytes[3 * 32];
    CHECK(handles(g_dev, rt, 0, 3, sizeof(handle_bytes), handle_bytes));
    for (unsigned i = 0; i < 3; ++i) memcpy((uint8_t *)sbt.mapped + i * 64, handle_bytes + i * 32, 32);
    VkDeviceSize a = stack(g_dev, rt, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR),
                 b = stack(g_dev, rt, 1, VK_SHADER_GROUP_SHADER_GENERAL_KHR),
                 c = stack(g_dev, rt, 2, VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR);
    VkDeviceSize budget = a + (b > c ? b : c);
    REQUIRE(budget && budget <= UINT32_MAX);
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = g_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(g_dev, &ca, &command));
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        if (iteration) {
            CHECK(vkResetCommandBuffer(command, 0));
            CHECK(vkCreatePipelineLayout(g_dev, &lci, NULL, &layout));
        }
        memset(output.mapped, 0xa5, (size_t)output.size);
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(command, &begin));
        memory_barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        const VkAccelerationStructureBuildRangeInfoKHR *r = &blas.range;
        build(command, 1, &blas.info, &r);
        memory_barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        r = &tlas.range;
        build(command, 1, &tlas.info, &r);
        memory_barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                       VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, compute);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        uint32_t ordinary = 123 + iteration;
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 4, 4, &ordinary);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, 0, 1, &set, 0, NULL);
        const uint32_t values[] = {3 + iteration, 7 + iteration, 11 + iteration};
        const uint32_t push_offsets[] = {0, 8, 12};
        for (unsigned i = 0; i < 3; ++i) vkCmdPushConstants(command, layout, kinds[i], push_offsets[i], 4, &values[i]);
        set_stack(command, (uint32_t)budget);
        vkDestroyPipelineLayout(g_dev, layout, NULL);
        VkStridedDeviceAddressRegionKHR regions[] = {
            {sbt.addr, 64, 64}, {sbt.addr + 64, 64, 64}, {sbt.addr + 128, 64, 64}, {0, 0, 0}};
        trace(command, &regions[0], &regions[1], &regions[2], &regions[3], 8, 1, 1);
        memory_barrier(command, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdDispatch(command, 8, 1, 1);
        memory_barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_ACCESS_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(g_queue));
        const uint32_t *result = output.mapped;
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t expected = (i & 1) ? 19 + 2 * iteration : 54 + 2 * iteration;
            REQUIRE(result[i * 4] == expected && result[i * 4 + 1] == 8 && result[i * 4 + 2] == i && result[i * 4 + 3] == ordinary);
        }
        printf("Canonical public RTP GPU trace/reset/layout-lifetime/compute-restore iteration%u %s\n", iteration,
               g_fail ? "FAIL" : "PASS");
    }
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
    vkDestroyPipeline(g_dev, rt, NULL);
    vkDestroyPipeline(g_dev, compute, NULL);
    vkDestroyDescriptorPool(g_dev, pool, NULL);
    vkDestroyDescriptorSetLayout(g_dev, dsl, NULL);
    pDestroyAS(g_dev, tlas.handle, NULL);
    pDestroyAS(g_dev, blas.handle, NULL);
    buf_destroy(&tlas.scratch);
    buf_destroy(&tlas.buffer);
    buf_destroy(&blas.scratch);
    buf_destroy(&blas.buffer);
    buf_destroy(&sbt);
    buf_destroy(&output);
    buf_destroy(&instances);
    buf_destroy(&vertices);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    printf("RTP API test mode=%s %s\n", advertised ? "advertised" : "explicit capability harness", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
