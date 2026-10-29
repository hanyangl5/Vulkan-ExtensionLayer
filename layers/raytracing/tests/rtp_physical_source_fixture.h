/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Public BDA source pointer/aggregate lifetime, with independent full-memory
 * expectations. Included after the common RTP API fixture helpers.
 */
#ifndef VKRT_PHYSICAL_SOURCE_FIXTURE_H
#define VKRT_PHYSICAL_SOURCE_FIXTURE_H
enum { PHYSICAL_RAYS = 27, PHYSICAL_DATA_WORDS = 32, PHYSICAL_ROW_WORDS = 16, PHYSICAL_TAIL_WORDS = 256 };
struct physical_record {
    uint64_t a, b;
    uint32_t index, pad;
};
struct physical_push {
    uint64_t records, output;
    uint32_t seed, pad[3];
};
_Static_assert(sizeof(struct physical_record) == 24 && sizeof(struct physical_push) == 32, "source std430 ABI");

static void physical_check_words(const char *name, const uint32_t *actual, const uint32_t *expected, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (actual[i] != expected[i]) {
            fprintf(stderr, "Physical source %s word=%zu got=%08x expected=%08x\n", name, i, actual[i], expected[i]);
            ++g_fail;
            if (g_fail >= 20) return;
        }
}
struct physical_scene {
    struct acceleration blas, tlas;
    struct gpu_buf boxes, instances;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
};
static void physical_scene_create(struct physical_scene *scene) {
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buf_create(sizeof(VkAabbPositionsKHR), usage, host, &scene->boxes);
    *(VkAabbPositionsKHR *)scene->boxes.mapped = (VkAabbPositionsKHR){-1, -1, 0, 1, 1, 3};
    VkAccelerationStructureGeometryKHR box = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
        .geometry.aabbs = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                           .data.deviceAddress = scene->boxes.addr,
                           .stride = sizeof(VkAabbPositionsKHR)}};
    prepare(&scene->blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &box);
    buf_create(sizeof(VkAccelerationStructureInstanceKHR), usage, host, &scene->instances);
    *(VkAccelerationStructureInstanceKHR *)scene->instances.mapped =
        (VkAccelerationStructureInstanceKHR){.transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}},
                                             .mask = 255,
                                             .accelerationStructureReference = scene->blas.address};
    VkAccelerationStructureGeometryKHR instance = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = scene->instances.addr}};
    prepare(&scene->tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &instance);
    /* Geometry pointers are consumed by Build; retain owned copies below. */
    static VkAccelerationStructureGeometryKHR saved[2];
    saved[0] = box;
    saved[1] = instance;
    scene->blas.info.pGeometries = saved;
    scene->tlas.info.pGeometries = saved + 1;
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR};
    VkDescriptorSetLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    CHECK(vkCreateDescriptorSetLayout(g_dev, &layout, NULL, &scene->descriptor_layout));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    CHECK(vkCreateDescriptorPool(g_dev, &pool, NULL, &scene->pool));
    VkDescriptorSetAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = scene->pool,
                                              .descriptorSetCount = 1,
                                              .pSetLayouts = &scene->descriptor_layout};
    CHECK(vkAllocateDescriptorSets(g_dev, &allocation, &scene->set));
    VkWriteDescriptorSetAccelerationStructureKHR value = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &scene->tlas.handle};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .pNext = &value,
                                  .dstSet = scene->set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    vkUpdateDescriptorSets(g_dev, 1, &write, 0, NULL);
}
static void physical_scene_build(struct physical_scene *scene, VkCommandBuffer command, VkFence fence) {
    PFN_vkCmdBuildAccelerationStructuresKHR build = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    REQUIRE(build);
    if (!build) exit(1);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    memory_barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                   VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const VkAccelerationStructureBuildRangeInfoKHR *range = &scene->blas.range;
    build(command, 1, &scene->blas.info, &range);
    memory_barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                   VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    range = &scene->tlas.range;
    build(command, 1, &scene->tlas.info, &range);
    memory_barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                   VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(g_queue, 1, &submit, fence));
    CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_C(30000000000)));
}
static void physical_interface_oracle(uint32_t id, uint32_t seed, bool alias, const struct gpu_buf *a, const struct gpu_buf *b,
                                      uint32_t *expected_a, uint32_t *expected_b, uint32_t *output) {
    uint32_t n = id % 7 + 3, base = id * PHYSICAL_DATA_WORDS, kind = id % 3 ? id % 3 : 3;
    uint32_t *av = expected_a + base + n, *bv = (alias ? expected_a : expected_b) + base + n;
    av[3] += 11;
    bv[3] += 13;
    uint32_t *pre = kind == 1 ? av : bv, *final = kind == 1 ? bv : av;
    pre[5] += 37;
    pre[6] += 23;
    final[4] += 19;
    if (kind != 3) {
        av[0] += 1;
        bv[7] += 2;
        av[1] += 3;
        if (kind == 2) bv[1] += 5;
        (kind == 1 ? bv : av)[2] += 7 + kind;
    }
    uint64_t address_a = a->addr + base * 4, address_b = (alias ? a->addr : b->addr) + base * 4;
    uint64_t pre_address = kind == 1 ? address_a : address_b, final_address = kind == 1 ? address_b : address_a;
    const uint32_t row[] = {seed + id + (kind == 3 ? 246 : 176 + kind),
                            kind,
                            kind == 3 ? 0 : 32 + kind,
                            1,
                            (uint32_t)final_address,
                            (uint32_t)(final_address >> 32),
                            (uint32_t)final_address,
                            (uint32_t)(final_address >> 32),
                            (uint32_t)pre_address,
                            (uint32_t)(pre_address >> 32),
                            (uint32_t)final_address,
                            (uint32_t)(final_address >> 32),
                            id,
                            seed,
                            n,
                            UINT32_C(0x50494643),
                            kind == 3 ? UINT32_C(0x11111111) : UINT32_C(0x44444444),
                            UINT32_C(0x22222222),
                            UINT32_C(0x33333333)};
    memcpy(output + id * 32, row, sizeof(row));
}
static int run_physical_fixture(const char *const *paths, bool interfaces) {
    init_vulkan();
    const uint32_t stage_count = interfaces ? 6 : 2, group_count = interfaces ? 4 : 2;
    const uint32_t row_words = interfaces ? 32 : PHYSICAL_ROW_WORDS;
    const VkShaderStageFlags stages_mask =
        interfaces ? (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                      VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR)
                   : VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 all = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &properties};
    vkGetPhysicalDeviceProperties2(g_pdev, &all);
    PFN_vkCreateRayTracingPipelinesKHR create = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateRayTracingPipelinesKHR");
    PFN_vkGetRayTracingShaderGroupHandlesKHR handles = (void *)vkGetDeviceProcAddr(g_dev, "vkGetRayTracingShaderGroupHandlesKHR");
    PFN_vkGetRayTracingShaderGroupStackSizeKHR stack = (void *)vkGetDeviceProcAddr(g_dev, "vkGetRayTracingShaderGroupStackSizeKHR");
    PFN_vkCmdSetRayTracingPipelineStackSizeKHR set_stack =
        (void *)vkGetDeviceProcAddr(g_dev, "vkCmdSetRayTracingPipelineStackSizeKHR");
    PFN_vkCmdTraceRaysKHR trace = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdTraceRaysKHR");
    REQUIRE(create && handles && stack && set_stack && trace && properties.shaderGroupHandleAlignment &&
            properties.shaderGroupBaseAlignment);
    if (g_fail) return 1;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const size_t data_words = PHYSICAL_RAYS * PHYSICAL_DATA_WORDS + PHYSICAL_TAIL_WORDS;
    const size_t output_words = PHYSICAL_RAYS * row_words + PHYSICAL_TAIL_WORDS;
    struct gpu_buf a, b, records, output, sbt;
    buf_create(data_words * 4, usage, host, &a);
    buf_create(data_words * 4, usage, host, &b);
    buf_create(PHYSICAL_RAYS * sizeof(struct physical_record) + PHYSICAL_TAIL_WORDS * 4, usage, host, &records);
    buf_create(output_words * 4, usage, host, &output);
    uint64_t alignment = properties.shaderGroupBaseAlignment;
    uint64_t stride = ((properties.shaderGroupHandleSize + alignment - 1) / alignment) * alignment;
    buf_create(stride * group_count + alignment,
               VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host, &sbt);
    uint64_t sbt_offset = (alignment - sbt.addr % alignment) % alignment;
    unsigned char *handle_bytes = malloc((size_t)properties.shaderGroupHandleSize * group_count);
    uint32_t *expected_a = malloc(data_words * 4), *expected_b = malloc(data_words * 4);
    uint32_t *expected_output = malloc(output_words * 4), *expected_records = malloc((size_t)records.size);
    REQUIRE(handle_bytes && expected_a && expected_b && expected_output && expected_records);
    if (g_fail) return 1;
    struct physical_scene scene = {0};
    if (interfaces) {
        REQUIRE(properties.maxRayHitAttributeSize >= 24);
        if (g_fail) return 1;
        physical_scene_create(&scene);
    }
    VkPushConstantRange range = {stages_mask, 0, sizeof(struct physical_push)};
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .pushConstantRangeCount = 1,
                                              .pPushConstantRanges = &range,
                                              .setLayoutCount = interfaces ? 1 : 0,
                                              .pSetLayouts = interfaces ? &scene.descriptor_layout : NULL};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(g_dev, &layout_info, NULL, &layout));
    VkShaderModule modules[6];
    VkPipelineShaderStageCreateInfo stages[6];
    const VkShaderStageFlagBits models[] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR,       VK_SHADER_STAGE_MISS_BIT_KHR,
                                            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,  VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                                            VK_SHADER_STAGE_INTERSECTION_BIT_KHR, VK_SHADER_STAGE_CALLABLE_BIT_KHR};
    for (unsigned i = 0; i < stage_count; ++i) {
        modules[i] = shader(paths[i]);
        stages[i] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = interfaces ? models[i] : (i ? VK_SHADER_STAGE_CALLABLE_BIT_KHR : VK_SHADER_STAGE_RAYGEN_BIT_KHR),
            .module = modules[i],
            .pName = "main"};
    }
    VkRayTracingShaderGroupCreateInfoKHR groups[4];
    for (unsigned i = 0; i < group_count; ++i)
        groups[i] = (VkRayTracingShaderGroupCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                                           .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                                                           .generalShader = i,
                                                           .closestHitShader = VK_SHADER_UNUSED_KHR,
                                                           .anyHitShader = VK_SHADER_UNUSED_KHR,
                                                           .intersectionShader = VK_SHADER_UNUSED_KHR};
    if (interfaces) {
        groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
        groups[2].generalShader = VK_SHADER_UNUSED_KHR;
        groups[2].closestHitShader = 2;
        groups[2].anyHitShader = 3;
        groups[2].intersectionShader = 4;
        groups[3].generalShader = 5;
    }
    const VkDynamicState dynamic = VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR;
    const VkPipelineDynamicStateCreateInfo dynamics = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 1, .pDynamicStates = &dynamic};
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .commandPool = g_pool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(g_dev, &command_info, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(g_dev, &fence_info, NULL, &fence));
    if (interfaces) {
        puts("Building physical interface scene");
        physical_scene_build(&scene, command, fence);
        puts("Physical interface scene completed");
    }
    for (unsigned no_opt = 0; no_opt < 2; ++no_opt) {
        VkRayTracingPipelineCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
                                                  .flags = no_opt ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0,
                                                  .stageCount = stage_count,
                                                  .pStages = stages,
                                                  .groupCount = group_count,
                                                  .pGroups = groups,
                                                  .maxPipelineRayRecursionDepth = 1,
                                                  .pDynamicState = &dynamics,
                                                  .layout = layout};
        VkPipeline pipeline;
        printf("Creating physical %s pipeline (opt=%u)\n", interfaces ? "interfaces" : "source", !no_opt);
        CHECK(create(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));
        CHECK(handles(g_dev, pipeline, 0, group_count, (size_t)properties.shaderGroupHandleSize * group_count, handle_bytes));
        for (unsigned i = 0; i < group_count; ++i)
            memcpy((uint8_t *)sbt.mapped + sbt_offset + i * stride, handle_bytes + i * properties.shaderGroupHandleSize,
                   properties.shaderGroupHandleSize);
        uint64_t rg = stack(g_dev, pipeline, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
        uint64_t callee = stack(g_dev, pipeline, interfaces ? 3 : 1, VK_SHADER_GROUP_SHADER_GENERAL_KHR), extra = 0;
        if (interfaces) {
            uint64_t miss = stack(g_dev, pipeline, 1, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
            uint64_t hit = stack(g_dev, pipeline, 2, VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR);
            uint64_t procedural = stack(g_dev, pipeline, 2, VK_SHADER_GROUP_SHADER_INTERSECTION_KHR) +
                                  stack(g_dev, pipeline, 2, VK_SHADER_GROUP_SHADER_ANY_HIT_KHR);
            extra = miss > hit ? miss : hit;
            if (procedural > extra) extra = procedural;
        }
        REQUIRE(rg <= UINT32_MAX && extra <= UINT32_MAX - rg && callee <= (UINT32_MAX - rg - extra) / 2);
        if (g_fail) return 1;
        uint32_t budget = (uint32_t)(rg + extra + 2 * callee);
        printf("Physical %s public stack rg=%llu + hit/miss=%llu + 2*callable=%llu => %u; opt=%u\n",
               interfaces ? "interfaces" : "source", (unsigned long long)rg, (unsigned long long)extra, (unsigned long long)callee,
               budget, !no_opt);
        for (unsigned phase = 0; phase < (interfaces ? 3u : 2u); ++phase) {
            bool alias = interfaces && phase == 2;
            uint32_t seed = phase == 2 ? UINT32_C(0x0badcafe) : phase ? UINT32_C(0xf001ba11) : UINT32_C(0x1234abcd);
            memset(expected_a, 0xa5, data_words * 4);
            memset(expected_b, 0xa5, data_words * 4);
            memset(expected_output, 0xa5, output_words * 4);
            memset(expected_records, 0xa5, (size_t)records.size);
            for (uint32_t id = 0; id < PHYSICAL_RAYS; ++id) {
                uint32_t idx = id % 7 + 3, base = id * PHYSICAL_DATA_WORDS;
                for (uint32_t j = 0; j < PHYSICAL_DATA_WORDS; ++j) {
                    expected_a[base + j] = seed + 0x10000u + id * 64 + j;
                    expected_b[base + j] = seed + 0x20000u + id * 64 + j;
                }
                ((struct physical_record *)expected_records)[id] =
                    (struct physical_record){a.addr + base * 4, (alias ? a.addr : b.addr) + base * 4, idx, UINT32_C(0xa5a5a5a5)};
            }
            memcpy(a.mapped, expected_a, data_words * 4);
            memcpy(b.mapped, expected_b, data_words * 4);
            memcpy(records.mapped, expected_records, (size_t)records.size);
            memset(output.mapped, 0xa5, (size_t)output.size);
            for (uint32_t id = 0; id < PHYSICAL_RAYS; ++id) {
                uint32_t idx = id % 7 + 3, base = id * PHYSICAL_DATA_WORDS;
                if (interfaces) {
                    physical_interface_oracle(id, seed, alias, &a, &b, expected_a, expected_b, expected_output);
                    continue;
                }
                uint32_t leaf = (expected_a[base + idx] + 10u) ^ (expected_b[base + idx] + 20u);
                expected_a[base + idx] += 10u + ((id & 1) ? 0u : 100u);
                expected_b[base + idx] += 20u + ((id & 1) ? 100u : 0u);
                expected_a[base + idx + 1] += 70u;
                ((id & 1) ? expected_b : expected_a)[base + idx + 2] += 1000u;
                expected_a[base + idx + 3] += 5u;
                expected_b[base + idx + 3] += 7u;
                const uint32_t *av = expected_a + base + idx, *bv = expected_b + base + idx;
                uint32_t row[] = {av[0],
                                  bv[0],
                                  av[1],
                                  bv[1],
                                  av[2],
                                  bv[2],
                                  bv[0],
                                  av[0],
                                  (id & 1) ? bv[0] : av[0],
                                  av[0],
                                  bv[0],
                                  (av[0] ^ bv[0]) + leaf,
                                  id,
                                  seed,
                                  idx,
                                  UINT32_C(0x42504441)};
                memcpy(expected_output + id * PHYSICAL_ROW_WORDS, row, sizeof(row));
                struct physical_record *record = (struct physical_record *)expected_records + id;
                uint64_t original_a = record->a;
                record->a = record->b;
                record->b = original_a;
            }
            CHECK(vkResetCommandBuffer(command, 0));
            VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(vkBeginCommandBuffer(command, &begin));
            memory_barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                           VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
            if (interfaces)
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, 0, 1, &scene.set, 0, NULL);
            struct physical_push push = {records.addr, output.addr, seed, {0, 0, 0}};
            vkCmdPushConstants(command, layout, stages_mask, 0, sizeof(push), &push);
            set_stack(command, budget);
            VkStridedDeviceAddressRegionKHR raygen = {sbt.addr + sbt_offset, stride, stride};
            VkStridedDeviceAddressRegionKHR callable = {sbt.addr + sbt_offset + (interfaces ? 3 : 1) * stride, stride, stride},
                                            empty = {0};
            VkStridedDeviceAddressRegionKHR miss = {sbt.addr + sbt_offset + stride, stride, stride};
            VkStridedDeviceAddressRegionKHR hit = {sbt.addr + sbt_offset + 2 * stride, stride, stride};
            trace(command, &raygen, interfaces ? &miss : &empty, interfaces ? &hit : &empty, &callable, 3, 3, 3);
            memory_barrier(command, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            CHECK(vkEndCommandBuffer(command));
            CHECK(vkResetFences(g_dev, 1, &fence));
            VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
            CHECK(vkQueueSubmit(g_queue, 1, &submit, fence));
            CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_C(30000000000)));
            physical_check_words("A", a.mapped, expected_a, data_words);
            physical_check_words("B", b.mapped, expected_b, data_words);
            physical_check_words("records", records.mapped, expected_records, (size_t)records.size / 4);
            physical_check_words("output", output.mapped, expected_output, output_words);
            printf("  27 invocations phase=%u: pointer identity, nested mutation, full buffers and padding %s\n", phase,
                   g_fail ? "FAIL" : "PASS");
            if (g_fail) return 1;
        }
        vkDestroyPipeline(g_dev, pipeline, NULL);
    }
    vkDestroyFence(g_dev, fence, NULL);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
    for (unsigned i = 0; i < stage_count; ++i) vkDestroyShaderModule(g_dev, modules[i], NULL);
    vkDestroyPipelineLayout(g_dev, layout, NULL);
    if (interfaces) {
        vkDestroyDescriptorPool(g_dev, scene.pool, NULL);
        vkDestroyDescriptorSetLayout(g_dev, scene.descriptor_layout, NULL);
        pDestroyAS(g_dev, scene.tlas.handle, NULL);
        pDestroyAS(g_dev, scene.blas.handle, NULL);
        buf_destroy(&scene.tlas.scratch);
        buf_destroy(&scene.tlas.buffer);
        buf_destroy(&scene.blas.scratch);
        buf_destroy(&scene.blas.buffer);
        buf_destroy(&scene.instances);
        buf_destroy(&scene.boxes);
    }
    buf_destroy(&sbt);
    buf_destroy(&output);
    buf_destroy(&records);
    buf_destroy(&b);
    buf_destroy(&a);
    free(expected_records);
    free(expected_output);
    free(expected_b);
    free(expected_a);
    free(handle_bytes);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    return g_fail ? 1 : 0;
}
static int run_physical_source_fixture(const char *raygen_path, const char *callable_path) {
    const char *paths[] = {raygen_path, callable_path};
    return run_physical_fixture(paths, false);
}
#endif
