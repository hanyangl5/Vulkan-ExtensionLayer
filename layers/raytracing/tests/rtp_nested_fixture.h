/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Included after test_rtp_api.c's ordinary public Vulkan fixture helpers.
 */
#ifndef VKRT_RTP_NESTED_FIXTURE_H
#define VKRT_RTP_NESTED_FIXTURE_H
#include <stdint.h>
#include <string.h>
enum { NESTED_ROW_WORDS = 96, NESTED_RAYS = 27, NESTED_CAP = 31 };
static uint32_t nested_mix32(uint32_t hash, uint32_t value) { return (hash ^ value) * UINT32_C(1664525) + UINT32_C(1013904223); }
static uint32_t nested_checksum(uint32_t seed, uint32_t id, uint32_t level, uint32_t kind) {
    uint32_t result = 0;
    for (uint32_t i = 0; i < 17; ++i) {
        uint32_t value = (seed + id * 97u + level * 257u + kind * 13u + i * 19u) ^ (UINT32_C(0x9e3779b9) + i * UINT32_C(0x10203));
        result = result * 33u ^ value;
    }
    return result;
}
/* This iterative CPU oracle computes the whole entry sequence and then the
 * reverse return sequence. GPU shaders instead call TraceRay dynamically from
 * both hit and miss activations, retaining their own local arrays and SSA. */
static void nested_expected_row(uint32_t *row, uint32_t id, uint32_t depth, uint32_t seed, uint32_t flip) {
    for (uint32_t i = 0; i < NESTED_ROW_WORDS; ++i) row[i] = UINT32_C(0xa5a5a5a5);
    uint32_t hash = nested_mix32(seed, id), unwind = 0;
    for (uint32_t level = 1; level <= depth; ++level) {
        uint32_t kind = ((id + level + flip) & 1) == 0 ? 17u : 29u;
        hash = nested_mix32(hash, level * 37u + kind);
        row[16 + 2 * (level - 1)] = hash;
    }
    for (uint32_t level = depth; level; --level) {
        uint32_t kind = ((id + level + flip) & 1) == 0 ? 17u : 29u;
        uint32_t checksum = nested_checksum(seed, id, level, kind);
        hash = nested_mix32(hash, checksum ^ (level * UINT32_C(0x10001)) ^ kind);
        unwind += checksum ^ level ^ kind;
        row[17 + 2 * (level - 1)] = hash;
    }
    uint32_t checksum = nested_checksum(seed, id, 0, 3), scalar = seed ^ (id * 65537u + 0x2468u);
    const uint32_t result[16] = {nested_mix32(hash, checksum ^ scalar),
                                 depth,
                                 unwind,
                                 1,
                                 checksum,
                                 depth,
                                 seed,
                                 id % 3,
                                 (id / 3) % 3,
                                 id / 9,
                                 3,
                                 3,
                                 3,
                                 scalar,
                                 flip,
                                 UINT32_C(0x52544e31)};
    memcpy(row, result, sizeof(result));
}
#ifndef VKRT_NESTED_ORACLE_ONLY
static int run_nested_trace_fixture(const char *raygen_path, const char *miss_path, const char *hit_path) {
    const char *paths[] = {raygen_path, miss_path, hit_path};
    for (unsigned i = 0; i < 3; ++i) {
        size_t count;
        uint32_t *words = read_spv(paths[i], &count);
        bool valid = count >= 5 && words[0] == 0x07230203u && words[1] == 0x00010400u;
        free(words);
        if (!valid) {
            fprintf(stderr, "Nested Trace requires genuinely compiled SPIR-V1.4: %s\n", paths[i]);
            return 1;
        }
    }
    init_vulkan();
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 all = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &properties};
    PFN_vkGetPhysicalDeviceProperties2 query = (void *)vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2KHR");
    REQUIRE(query);
    if (g_fail) return 1;
    query(g_pdev, &all);
    if (properties.maxRayRecursionDepth < NESTED_CAP) {
        printf("Nested Trace depth2/31 requires advertised recursion>=31; device reports %u (not a pass)\n",
               properties.maxRayRecursionDepth);
        vkDestroyCommandPool(g_dev, g_pool, NULL);
        vkDestroyDevice(g_dev, NULL);
        vkDestroyInstance(g_instance, NULL);
        return 77;
    }
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
    struct acceleration blas, tlas;
    prepare(&blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geometry);
    buf_create(sizeof(VkAccelerationStructureInstanceKHR),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &instances);
    *(VkAccelerationStructureInstanceKHR *)instances.mapped =
        (VkAccelerationStructureInstanceKHR){.transform = {.matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}},
                                             .mask = 255,
                                             .accelerationStructureReference = blas.address};
    VkAccelerationStructureGeometryKHR instance_geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = instances.addr}};
    prepare(&tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &instance_geometry);
    buf_create(NESTED_RAYS * NESTED_ROW_WORDS * sizeof(uint32_t) + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, &output);
    const VkShaderStageFlags stages_mask =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    const VkDescriptorSetLayoutBinding bindings[] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages_mask, NULL},
                                                     {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, stages_mask, NULL}};
    VkDescriptorSetLayoutCreateInfo descriptor_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout descriptor_layout;
    CHECK(vkCreateDescriptorSetLayout(g_dev, &descriptor_info, NULL, &descriptor_layout));
    const VkPushConstantRange push = {stages_mask, 0, 16};
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1,
                                              .pSetLayouts = &descriptor_layout,
                                              .pushConstantRangeCount = 1,
                                              .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(g_dev, &layout_info, NULL, &layout));
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                          {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}};
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(g_dev, &pool_info, NULL, &pool));
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &descriptor_layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(g_dev, &allocate, &set));
    const VkDescriptorBufferInfo buffer_info = {output.buf, 0, VK_WHOLE_SIZE};
    const VkWriteDescriptorSetAccelerationStructureKHR as_info = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &tlas.handle};
    VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = 0,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &buffer_info},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .pNext = &as_info,
                                      .dstSet = set,
                                      .dstBinding = 1,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}};
    vkUpdateDescriptorSets(g_dev, 2, writes, 0, NULL);
    VkShaderModule modules[3];
    VkPipelineShaderStageCreateInfo stages[3];
    const VkShaderStageFlagBits kinds[] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
                                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
    VkRayTracingShaderGroupCreateInfoKHR groups[3];
    for (unsigned i = 0; i < 3; ++i) {
        modules[i] = shader(paths[i]);
        stages[i] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = kinds[i], .module = modules[i], .pName = "main"};
        groups[i] = (VkRayTracingShaderGroupCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type = i < 2 ? VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
            .generalShader = i < 2 ? i : VK_SHADER_UNUSED_KHR,
            .closestHitShader = i == 2 ? 2 : VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR};
    }
    uint64_t alignment = properties.shaderGroupBaseAlignment, handle_alignment = properties.shaderGroupHandleAlignment;
    REQUIRE(alignment && handle_alignment && properties.shaderGroupHandleSize);
    if (alignment < handle_alignment) alignment = handle_alignment;
    uint64_t stride = (properties.shaderGroupHandleSize + alignment - 1) / alignment * alignment;
    REQUIRE(stride <= properties.maxShaderGroupStride);
    if (g_fail) return 1;
    buf_create(stride * 3 + alignment, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &sbt);
    const uint64_t sbt_offset = (alignment - sbt.addr % alignment) % alignment;
    unsigned char *handle_bytes = malloc((size_t)properties.shaderGroupHandleSize * 3);
    if (!handle_bytes) return 1;
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .commandPool = g_pool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(g_dev, &command_info, &command));
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    memory_barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                   VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const VkAccelerationStructureBuildRangeInfoKHR *range = &blas.range;
    build(command, 1, &blas.info, &range);
    memory_barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                   VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    range = &tlas.range;
    build(command, 1, &tlas.info, &range);
    memory_barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                   VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(g_queue));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(g_dev, &fence_info, NULL, &fence));
    const uint32_t depths[] = {2, NESTED_CAP};
    for (unsigned depth_index = 0; depth_index < 2; ++depth_index)
        for (unsigned disable_opt = 0; disable_opt < 2; ++disable_opt) {
            uint32_t depth = depths[depth_index];
            VkDynamicState dynamic = VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR;
            const VkPipelineDynamicStateCreateInfo dynamics = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 1, .pDynamicStates = &dynamic};
            const VkRayTracingPipelineCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
                                                            .flags = disable_opt ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0,
                                                            .stageCount = 3,
                                                            .pStages = stages,
                                                            .groupCount = 3,
                                                            .pGroups = groups,
                                                            .maxPipelineRayRecursionDepth = depth,
                                                            .pDynamicState = &dynamics,
                                                            .layout = layout};
            VkPipeline pipeline;
            CHECK(create(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));
            CHECK(handles(g_dev, pipeline, 0, 3, (size_t)properties.shaderGroupHandleSize * 3, handle_bytes));
            for (unsigned i = 0; i < 3; ++i)
                memcpy((uint8_t *)sbt.mapped + sbt_offset + i * stride, handle_bytes + i * properties.shaderGroupHandleSize,
                       properties.shaderGroupHandleSize);
            const uint64_t raygen_stack = stack(g_dev, pipeline, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
            const uint64_t miss_stack = stack(g_dev, pipeline, 1, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
            const uint64_t hit_stack = stack(g_dev, pipeline, 2, VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR);
            const uint64_t maximum = miss_stack > hit_stack ? miss_stack : hit_stack;
            REQUIRE(maximum <= (UINT64_MAX - raygen_stack) / depth);
            const uint64_t budget = raygen_stack + depth * maximum;
            REQUIRE(budget && budget <= UINT32_MAX);
            if (g_fail) return 1;
            printf("Public nested Trace depth=%u opt=%u stack=rg(%llu)+depth*max(miss(%llu),hit(%llu))=%llu\n", depth, !disable_opt,
                   (unsigned long long)raygen_stack, (unsigned long long)miss_stack, (unsigned long long)hit_stack,
                   (unsigned long long)budget);
            for (uint32_t phase = 0; phase < 2; ++phase) {
                const uint32_t push_values[4] = {depth, phase ? UINT32_C(0xf001ba11) : UINT32_C(0x1234abcd), phase, 0};
                memset(output.mapped, 0xa5, (size_t)output.size);
                CHECK(vkResetCommandBuffer(command, 0));
                CHECK(vkBeginCommandBuffer(command, &begin));
                memory_barrier(
                    command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, 0, 1, &set, 0, NULL);
                vkCmdPushConstants(command, layout, stages_mask, 0, sizeof(push_values), push_values);
                set_stack(command, (uint32_t)budget);  // Exact public equation; no private prefix/slack.
                VkStridedDeviceAddressRegionKHR regions[] = {{sbt.addr + sbt_offset, stride, stride},
                                                             {sbt.addr + sbt_offset + stride, stride, stride},
                                                             {sbt.addr + sbt_offset + 2 * stride, stride, stride},
                                                             {0, 0, 0}};
                trace(command, &regions[0], &regions[1], &regions[2], &regions[3], 3, 3, 3);
                memory_barrier(command, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                CHECK(vkEndCommandBuffer(command));
                CHECK(vkResetFences(g_dev, 1, &fence));
                CHECK(vkQueueSubmit(g_queue, 1, &submit, fence));
                VkResult waited = vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX);
                printf("  phase=%u FenceWait=%d\n", phase, waited);
                CHECK(waited);
                const uint32_t *actual = output.mapped;
                for (uint32_t id = 0; id < NESTED_RAYS; ++id) {
                    uint32_t expected[NESTED_ROW_WORDS];
                    nested_expected_row(expected, id, depth, push_values[1], phase);
                    for (uint32_t word = 0; word < NESTED_ROW_WORDS; ++word)
                        if (actual[id * NESTED_ROW_WORDS + word] != expected[word]) {
                            fprintf(stderr, "Nested oracle depth=%u opt=%u phase=%u ray=%u word=%u got=%08x expected=%08x\n", depth,
                                    !disable_opt, phase, id, word, actual[id * NESTED_ROW_WORDS + word], expected[word]);
                            ++g_fail;
                        }
                }
                for (size_t word = NESTED_RAYS * NESTED_ROW_WORDS; word < output.size / 4; ++word)
                    REQUIRE(actual[word] == UINT32_C(0xa5a5a5a5));
                printf("  real hit/miss chain, 17 local words per activation, full entry/exit/padding oracle %s\n",
                       g_fail ? "FAIL" : "PASS");
                if (g_fail) return 1;
            }
            vkDestroyPipeline(g_dev, pipeline, NULL);
        }
    vkDestroyFence(g_dev, fence, NULL);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
    for (unsigned i = 0; i < 3; ++i) vkDestroyShaderModule(g_dev, modules[i], NULL);
    free(handle_bytes);
    vkDestroyDescriptorPool(g_dev, pool, NULL);
    vkDestroyPipelineLayout(g_dev, layout, NULL);
    vkDestroyDescriptorSetLayout(g_dev, descriptor_layout, NULL);
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
    printf("Public Vulkan1.1/SPIR-V1.4 nested Trace depth2+31, optimized/unoptimized, 3D27, exact stack oracle PASS\n");
    return 0;
}
#endif
#endif
