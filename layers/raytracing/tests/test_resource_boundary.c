/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <vulkan/vulkan.h>
static VkResult budget_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *source, const VkAllocationCallbacks *allocator,
                              VkDevice *device);
#define vkCreateDevice budget_device
#define VKRT_TEST_FIXTURE_ONLY
#include "00_single_tri/main.c"
#undef vkCreateDevice
#include "vkrt_resource_profile.h"

static VkCommandBuffer cmd_begin(void) {
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = g_pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(g_dev, &allocate, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(command, &begin));
    return command;
}
static void cmd_submit(VkCommandBuffer command) {
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(g_queue));
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
}

static VkResult budget_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *source, const VkAllocationCallbacks *allocator,
                              VkDevice *device) {
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!supported.rayTracingPipeline || !features.features.shaderStorageBufferArrayDynamicIndexing) exit(77);
    VkDeviceCreateInfo local = *source;
    VkPhysicalDeviceFeatures core = source->pEnabledFeatures ? *source->pEnabledFeatures : (VkPhysicalDeviceFeatures){0};
    core.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    local.pEnabledFeatures = &core;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = (void *)source->pNext,
        .rayTracingPipeline = VK_TRUE};
    local.pNext = &rt;
    const char **extensions = calloc((size_t)source->enabledExtensionCount + 1, sizeof(*extensions));
    if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(extensions, source->ppEnabledExtensionNames, source->enabledExtensionCount * sizeof(*extensions));
    extensions[local.enabledExtensionCount++] = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;
    local.ppEnabledExtensionNames = extensions;
    VkResult result = vkCreateDevice(physical, &local, allocator, device);
    free(extensions);
    return result;
}
struct acceleration {
    struct gpu_buf storage, scratch;
    VkAccelerationStructureKHR handle;
    VkDeviceAddress address;
    VkAccelerationStructureBuildGeometryInfoKHR info;
    VkAccelerationStructureBuildRangeInfoKHR range;
};
static void prepare(struct acceleration *as, VkAccelerationStructureTypeKHR type,
                    const VkAccelerationStructureGeometryKHR *geometry, uint32_t count) {
    memset(as, 0, sizeof(*as));
    as->range.primitiveCount = count;
    as->info =
        (VkAccelerationStructureBuildGeometryInfoKHR){.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                      .type = type,
                                                      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                      .geometryCount = 1,
                                                      .pGeometries = geometry};
    VkAccelerationStructureBuildSizesInfoKHR size = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
    sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &as->info, &count, &size);
    buf_create(size.accelerationStructureSize,
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &as->storage);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .type = type,
                                                   .buffer = as->storage.buf,
                                                   .size = size.accelerationStructureSize};
    CHECK(pCreateAS(g_dev, &create, NULL, &as->handle));
    VkAccelerationStructureDeviceAddressInfoKHR address = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = as->handle};
    PFN_vkGetAccelerationStructureDeviceAddressKHR get =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureDeviceAddressKHR");
    as->address = get(g_dev, &address);
    buf_create(size.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &as->scratch);
    as->info.dstAccelerationStructure = as->handle;
    as->info.scratchData.deviceAddress = (as->scratch.addr + 255) & ~UINT64_C(255);
}
static void barrier(VkCommandBuffer command, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst,
                    VkAccessFlags da) {
    VkMemoryBarrier memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da};
    vkCmdPipelineBarrier(command, src, dst, 0, 1, &memory, 0, NULL, 0, NULL);
}
static VkShaderModule shader(const char *path, uint32_t descriptor_set) {
    size_t count;
    uint32_t *words = read_spv(path, &count);
    /* Test generation selects the last legal set on the actual device. Keep
     * every shader instruction unchanged; only DescriptorSet decorations vary. */
    for (size_t p = 5; p < count; p += words[p] >> 16)
        if ((words[p] & 65535) == 71 && (words[p] >> 16) == 4 && words[p + 2] == 34) words[p + 3] = descriptor_set;
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = count * 4, .pCode = words};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(g_dev, &info, NULL, &module));
    free(words);
    return module;
}
int main(int argc, char **argv) {
    int destroy_early = argc == 5 && !strcmp(argv[4], "--destroy-dsl-before-pipeline");
    if (argc != 4 && !destroy_early) {
        fprintf(stderr, "usage: %s rgen.spv miss.spv closest.spv [--destroy-dsl-before-pipeline]\n", argv[0]);
        return 1;
    }
    init_vulkan();
    struct vkrt_resource_profile profile = {0};
    enum vkrt_resource_failure why;
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    bool software = layers && strstr(layers, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY");
    if (!software &&
        !vkrt_query_resource_profile(vkGetPhysicalDeviceProperties2, vkGetPhysicalDeviceFeatures2, g_pdev, &profile, &why)) {
        fprintf(stderr, "Descriptor profile unavailable: %d\n", why);
        return 77;
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(g_pdev, &properties);
    if (software) {
        /* The layer now publishes the proven budget itself. Planning again
         * from public values would reserve AS headroom twice and fail to
         * exercise the advertised boundary. Native controls still plan once. */
        VkPhysicalDeviceAccelerationStructurePropertiesKHR as = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 public_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &as};
        vkGetPhysicalDeviceProperties2(g_pdev, &public_properties);
        profile.limits = public_properties.properties.limits;
        profile.as_per_stage = as.maxPerStageDescriptorAccelerationStructures;
        profile.as_per_set = as.maxDescriptorSetAccelerationStructures;
        if (profile.as_per_stage != 16 || profile.as_per_set != 16 ||
            properties.limits.maxDescriptorSetStorageBuffers != profile.limits.maxDescriptorSetStorageBuffers ||
            properties.limits.maxDescriptorSetStorageBuffersDynamic != profile.limits.maxDescriptorSetStorageBuffersDynamic ||
            properties.limits.maxPerStageDescriptorStorageBuffers != profile.limits.maxPerStageDescriptorStorageBuffers) {
            fprintf(stderr, "Inconsistent public descriptor profile\n");
            return 1;
        }
    }
    uint32_t set_count = properties.limits.maxBoundDescriptorSets, push_bytes = properties.limits.maxPushConstantsSize;
    uint32_t static_count = profile.limits.maxDescriptorSetStorageBuffers;
    uint32_t dynamic_count = profile.limits.maxDescriptorSetStorageBuffersDynamic;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    struct gpu_buf vertices, instances, input, output, sbt;
    buf_create(36, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &vertices);
    const float triangle[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    memcpy(vertices.mapped, triangle, sizeof(triangle));
    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                               .vertexData.deviceAddress = vertices.addr,
                               .vertexStride = 12,
                               .maxVertex = 2,
                               .indexType = VK_INDEX_TYPE_NONE_KHR}};
    struct acceleration blas, tlas, empty;
    prepare(&blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geometry, 1);
    buf_create(sizeof(VkAccelerationStructureInstanceKHR),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &instances);
    *(VkAccelerationStructureInstanceKHR *)instances.mapped =
        (VkAccelerationStructureInstanceKHR){.transform = {.matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}},
                                             .mask = 255,
                                             .accelerationStructureReference = blas.address};
    VkAccelerationStructureGeometryKHR instance = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = instances.addr}};
    prepare(&tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &instance, 1);
    prepare(&empty, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &instance, 0);
    PFN_vkCmdBuildAccelerationStructuresKHR build = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    VkCommandBuffer command = cmd_begin();
    struct acceleration *structures[] = {&blas, &tlas, &empty};
    for (unsigned i = 0; i < 3; ++i) {
        const VkAccelerationStructureBuildRangeInfoKHR *range = &structures[i]->range;
        build(command, 1, &structures[i]->info, &range);
        barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    }
    cmd_submit(command);
    buf_create(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, &input);
    buf_create(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, &output);
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 16, VK_SHADER_STAGE_RAYGEN_BIT_KHR, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_count, VK_SHADER_STAGE_RAYGEN_BIT_KHR, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, dynamic_count, VK_SHADER_STAGE_RAYGEN_BIT_KHR, NULL}};
    VkDescriptorSetLayoutCreateInfo dsl_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayoutSupport support = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
    vkGetDescriptorSetLayoutSupport(g_dev, &dsl_info, &support);
    if (!support.supported) {
        fprintf(stderr, "Full AS/SSBO layout unsupported\n");
        return 1;
    }
    VkDescriptorSetLayout layout, binding_layout, empty_layout;
    CHECK(vkCreateDescriptorSetLayout(g_dev, &dsl_info, NULL, &layout));
    CHECK(vkCreateDescriptorSetLayout(g_dev, &dsl_info, NULL, &binding_layout));
    dsl_info.bindingCount = 0;
    CHECK(vkCreateDescriptorSetLayout(g_dev, &dsl_info, NULL, &empty_layout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 16},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_count},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, dynamic_count}};
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = sizes};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(g_dev, &pool_info, NULL, &pool));
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(g_dev, &allocate, &set));
    VkDescriptorBufferInfo *statics = calloc(static_count, sizeof(*statics)), *dynamics = calloc(dynamic_count, sizeof(*dynamics));
    if (!statics || !dynamics) return 1;
    for (uint32_t i = 0; i < static_count; ++i) statics[i] = (VkDescriptorBufferInfo){input.buf, 0, 4};
    for (uint32_t i = 0; i < dynamic_count; ++i) dynamics[i] = (VkDescriptorBufferInfo){output.buf, 0, 4};
    VkAccelerationStructureKHR scenes[16];
    for (unsigned i = 0; i < 16; ++i) scenes[i] = i == 15 ? empty.handle : tlas.handle;
    VkWriteDescriptorSetAccelerationStructureKHR as_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 16,
        .pAccelerationStructures = scenes};
    VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .pNext = &as_write,
                                      .dstSet = set,
                                      .dstBinding = 0,
                                      .descriptorCount = 16,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = 1,
                                      .descriptorCount = static_count,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = statics},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = 2,
                                      .descriptorCount = dynamic_count,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                                      .pBufferInfo = dynamics}};
    vkUpdateDescriptorSets(g_dev, 3, writes, 0, NULL);
    free(statics);
    free(dynamics);
    VkDescriptorSetLayout *layouts = calloc(set_count, sizeof(*layouts));
    uint32_t *push = calloc(push_bytes / 4, 4), *offsets = calloc(dynamic_count, 4);
    if (!layouts || !push || !offsets) return 1;
    for (uint32_t i = 0; i < set_count; ++i) layouts[i] = i == set_count - 1 ? layout : empty_layout;
    VkPushConstantRange range = {VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, push_bytes};
    VkPipelineLayoutCreateInfo pl_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = set_count,
                                          .pSetLayouts = layouts,
                                          .pushConstantRangeCount = 1,
                                          .pPushConstantRanges = &range};
    VkPipelineLayout pipeline_layout, compatible_layout;
    CHECK(vkCreatePipelineLayout(g_dev, &pl_info, NULL, &pipeline_layout));
    layouts[set_count - 1] = binding_layout;
    CHECK(vkCreatePipelineLayout(g_dev, &pl_info, NULL, &compatible_layout));
    if (destroy_early) {
        // Optional legal-lifetime edge also has a standalone native compute
        // control, because SDK validation retains stale DSL child references.
        vkDestroyDescriptorSetLayout(g_dev, layout, NULL);
        vkDestroyDescriptorSetLayout(g_dev, binding_layout, NULL);
        vkDestroyDescriptorSetLayout(g_dev, empty_layout, NULL);
    }
    VkShaderModule modules[3];
    const VkShaderStageFlagBits kinds[] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
                                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
    VkPipelineShaderStageCreateInfo stages[3];
    uint32_t values[] = {static_count, push_bytes / 4, dynamic_count};
    VkSpecializationMapEntry entries[] = {{23, 0, 4}, {24, 4, 4}, {25, 8, 4}};
    VkSpecializationInfo specialization = {3, entries, sizeof(values), values};
    for (unsigned i = 0; i < 3; ++i) {
        modules[i] = shader(argv[i + 1], set_count - 1);
        stages[i] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                      .stage = kinds[i],
                                                      .module = modules[i],
                                                      .pName = "main",
                                                      .pSpecializationInfo = i ? NULL : &specialization};
    }
    VkRayTracingShaderGroupCreateInfoKHR groups[3];
    for (unsigned i = 0; i < 3; ++i)
        groups[i] = (VkRayTracingShaderGroupCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                                           .type = i == 2 ? VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR
                                                                          : VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                                                           .generalShader = i == 2 ? VK_SHADER_UNUSED_KHR : i,
                                                           .closestHitShader = i == 2 ? i : VK_SHADER_UNUSED_KHR,
                                                           .anyHitShader = VK_SHADER_UNUSED_KHR,
                                                           .intersectionShader = VK_SHADER_UNUSED_KHR};
    VkRayTracingPipelineCreateInfoKHR pipeline_info = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
                                                       .stageCount = 3,
                                                       .pStages = stages,
                                                       .groupCount = 3,
                                                       .pGroups = groups,
                                                       .maxPipelineRayRecursionDepth = 1,
                                                       .layout = pipeline_layout};
    PFN_vkCreateRayTracingPipelinesKHR create_pipeline = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateRayTracingPipelinesKHR");
    VkPipeline pipeline;
    CHECK(create_pipeline(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
    for (unsigned i = 0; i < 3; ++i) vkDestroyShaderModule(g_dev, modules[i], NULL);
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &rt};
    vkGetPhysicalDeviceProperties2(g_pdev, &props);
    uint32_t stride = rt.shaderGroupBaseAlignment;
    if (stride < 32) stride = 32;
    buf_create(4 * stride, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host, &sbt);
    VkDeviceAddress base = (sbt.addr + stride - 1) & ~(VkDeviceAddress)(stride - 1);
    uint8_t handles[96];
    PFN_vkGetRayTracingShaderGroupHandlesKHR get_handles =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetRayTracingShaderGroupHandlesKHR");
    CHECK(get_handles(g_dev, pipeline, 0, 3, sizeof(handles), handles));
    for (unsigned i = 0; i < 3; ++i) memcpy((uint8_t *)sbt.mapped + base - sbt.addr + i * stride, handles + 32 * i, 32);
    VkStridedDeviceAddressRegionKHR rg = {base, 32, 32}, miss = {base + stride, stride, stride},
                                    hit = {base + 2 * stride, stride, stride}, call = {0};
    PFN_vkCmdTraceRaysKHR trace = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdTraceRaysKHR");
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        *(uint32_t *)input.mapped = 5;
        *(uint32_t *)output.mapped = 0;
        push[0] = iteration ? 0 : 15;
        push[1] = static_count - 1;
        push[2] = dynamic_count - 1;
        push[3] = push_bytes / 4 - 1;
        push[push[3]] = 77;
        command = cmd_begin();
        barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, compatible_layout, set_count - 1, 1, &set,
                                dynamic_count, offsets);
        vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, push_bytes, push);
        trace(command, &rg, &miss, &hit, &call, 1, 1, 1);
        barrier(command, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
        cmd_submit(command);
        uint32_t expected = iteration ? 99 : 89;
        if (*(uint32_t *)input.mapped != 123 || *(uint32_t *)output.mapped != expected) {
            fprintf(stderr, "Boundary result%u: input=%u output=%u expected=%u\n", iteration, *(uint32_t *)input.mapped,
                    *(uint32_t *)output.mapped, expected);
            ++g_fail;
        }
    }
    printf(
        "Public RTP resource boundary %s: AS16 staticSSBO%u dynamicSSBO%u set%u/%u push%u; miss/hit last-slot writes; distinct "
        "compatible DSLs%s\n",
        g_fail ? "FAIL" : "PASS", static_count, dynamic_count, set_count - 1, set_count, push_bytes,
        destroy_early ? " destroyed before pipeline" : " retained");
    vkDestroyPipeline(g_dev, pipeline, NULL);
    vkDestroyPipelineLayout(g_dev, pipeline_layout, NULL);
    vkDestroyPipelineLayout(g_dev, compatible_layout, NULL);
    vkDestroyDescriptorPool(g_dev, pool, NULL);
    if (!destroy_early) {
        vkDestroyDescriptorSetLayout(g_dev, layout, NULL);
        vkDestroyDescriptorSetLayout(g_dev, binding_layout, NULL);
        vkDestroyDescriptorSetLayout(g_dev, empty_layout, NULL);
    }
    for (unsigned i = 0; i < 3; ++i) {
        pDestroyAS(g_dev, structures[i]->handle, NULL);
        buf_destroy(&structures[i]->scratch);
        buf_destroy(&structures[i]->storage);
    }
    buf_destroy(&sbt);
    buf_destroy(&input);
    buf_destroy(&output);
    buf_destroy(&instances);
    buf_destroy(&vertices);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    free(layouts);
    free(push);
    free(offsets);
    return g_fail ? 1 : 0;
}
