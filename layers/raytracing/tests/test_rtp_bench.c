/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * M0 traversal benchmark: a single triangle grid, an RTP pipeline whose raygen
 * casts `spp` rays per invocation, and GPU timestamps around the dispatch. It
 * reports ns/ray and M ray/s so traversal and payload changes can be compared
 * against the HIPRT reference on the same host.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader_object_harness.h"

#ifndef REQUIRE
#define REQUIRE(x)                                       \
    do {                                                 \
        if (!(x)) {                                      \
            fprintf(stderr, "require failed: %s\n", #x); \
            exit(1);                                     \
        }                                                \
    } while (0)
#endif

static VkShaderModule make_shader(const char *path) {
    size_t words = 0;
    uint32_t *code = read_spv(path, &words);
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * sizeof(uint32_t), .pCode = code};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(g_dev, &info, NULL, &module));
    free(code);
    return module;
}

static uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

/* prepare() in the shared harness sizes for a single primitive; the benchmark
 * needs a multi-triangle BLAS, so query the sizes with the real count. */
static void prepare_count(struct acceleration *as, VkAccelerationStructureTypeKHR type,
                          const VkAccelerationStructureGeometryKHR *geometry, uint32_t primitive_count) {
    memset(as, 0, sizeof(*as));
    as->range.primitiveCount = primitive_count;
    as->info =
        (VkAccelerationStructureBuildGeometryInfoKHR){.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                      .type = type,
                                                      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                      .geometryCount = 1,
                                                      .pGeometries = geometry};
    VkAccelerationStructureBuildSizesInfoKHR size = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
    sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &as->info, &as->range.primitiveCount, &size);
    buf_create(size.accelerationStructureSize,
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &as->storage);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = as->storage.buf,
                                                   .size = size.accelerationStructureSize,
                                                   .type = type};
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

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s raygen.spv miss.spv chit.spv [width height spp iterations]\n", argv[0]);
        return 1;
    }
    uint32_t width = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 256;
    uint32_t height = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 10) : 256;
    uint32_t spp = argc > 6 ? (uint32_t)strtoul(argv[6], NULL, 10) : 64;
    uint32_t iterations = argc > 7 ? (uint32_t)strtoul(argv[7], NULL, 10) : 5;
    uint32_t cells = argc > 8 ? (uint32_t)strtoul(argv[8], NULL, 10) : 8;
    init_vulkan();

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &rtp};
    PFN_vkGetPhysicalDeviceProperties2 get_properties =
        (void *)vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2KHR");
    REQUIRE(get_properties);
    get_properties(g_pdev, &properties);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &family_count, families);
    uint32_t timestamp_bits = families[g_qfamily].timestampValidBits;
    free(families);
    if (!timestamp_bits) {
        fprintf(stderr, "queue family has no timestamp support\n");
        return 77;
    }

    PFN_vkCreateRayTracingPipelinesKHR create = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateRayTracingPipelinesKHR");
    PFN_vkGetRayTracingShaderGroupHandlesKHR handles = (void *)vkGetDeviceProcAddr(g_dev, "vkGetRayTracingShaderGroupHandlesKHR");
    PFN_vkGetRayTracingShaderGroupStackSizeKHR stack_size =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetRayTracingShaderGroupStackSizeKHR");
    PFN_vkCmdSetRayTracingPipelineStackSizeKHR set_stack =
        (void *)vkGetDeviceProcAddr(g_dev, "vkCmdSetRayTracingPipelineStackSizeKHR");
    PFN_vkCmdTraceRaysKHR trace = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdTraceRaysKHR");
    PFN_vkCmdBuildAccelerationStructuresKHR build = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    REQUIRE(create && handles && stack_size && set_stack && trace && build);
    if (g_fail) return 1;

    /* Geometry: either a triangle grid in the z=0 plane (two triangles per
     * cell), or, with a scene file as argument 9, one BLAS per mesh and one
     * TLAS instance per BLAS. The scene file is uint32 mesh_count, uint32
     * triangle counts[mesh_count], then non-indexed float3 vertices. */
    const char *scene_path = argc > 9 ? argv[9] : NULL;
    uint32_t mesh_count = 1;
    uint32_t *mesh_triangles = NULL;
    float *scene_vertices = NULL;
    uint32_t triangle_count = 0;
    if (scene_path) {
        FILE *file = fopen(scene_path, "rb");
        REQUIRE(file);
        REQUIRE(fread(&mesh_count, 4, 1, file) == 1 && mesh_count > 0 && mesh_count <= 4096);
        mesh_triangles = calloc(mesh_count, sizeof(uint32_t));
        REQUIRE(mesh_triangles && fread(mesh_triangles, 4, mesh_count, file) == mesh_count);
        for (uint32_t m = 0; m < mesh_count; ++m) triangle_count += mesh_triangles[m];
        scene_vertices = malloc((size_t)triangle_count * 9 * sizeof(float));
        REQUIRE(scene_vertices && fread(scene_vertices, sizeof(float) * 9, triangle_count, file) == triangle_count);
        fclose(file);
    } else {
        if (!cells) cells = 8;
        triangle_count = cells * cells * 2;
        mesh_triangles = calloc(1, sizeof(uint32_t));
        REQUIRE(mesh_triangles);
        mesh_triangles[0] = triangle_count;
    }
    const uint32_t vertex_count = triangle_count * 3;
    struct gpu_buf vertices;
    buf_create(vertex_count * 3 * sizeof(float),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vertices);
    float *vertex_data = vertices.mapped;
    if (scene_vertices) {
        memcpy(vertex_data, scene_vertices, (size_t)vertex_count * 3 * sizeof(float));
        free(scene_vertices);
    } else {
        uint32_t vertex = 0;
        for (uint32_t y = 0; y < cells; ++y)
            for (uint32_t x = 0; x < cells; ++x) {
                const float x0 = -1.0f + 2.0f * (float)x / (float)cells;
                const float x1 = -1.0f + 2.0f * (float)(x + 1) / (float)cells;
                const float y0 = -1.0f + 2.0f * (float)y / (float)cells;
                const float y1 = -1.0f + 2.0f * (float)(y + 1) / (float)cells;
                const float quad[6][3] = {{x0, y0, 0.0f}, {x1, y0, 0.0f}, {x1, y1, 0.0f},
                                          {x0, y0, 0.0f}, {x1, y1, 0.0f}, {x0, y1, 0.0f}};
                for (uint32_t i = 0; i < 6; ++i) {
                    memcpy(vertex_data + vertex * 3, quad[i], sizeof(quad[i]));
                    vertex++;
                }
            }
    }
    VkAccelerationStructureGeometryKHR *geometries = calloc(mesh_count, sizeof(*geometries));
    struct acceleration *blases = calloc(mesh_count, sizeof(*blases));
    REQUIRE(geometries && blases);
    uint64_t first_vertex = 0;
    for (uint32_t m = 0; m < mesh_count; ++m) {
        geometries[m] = (VkAccelerationStructureGeometryKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
            .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                   .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                                   .vertexData.deviceAddress = vertices.addr + first_vertex * 3 * sizeof(float),
                                   .vertexStride = 3 * sizeof(float),
                                   .maxVertex = mesh_triangles[m] * 3 - 1,
                                   .indexType = VK_INDEX_TYPE_NONE_KHR}};
        prepare_count(&blases[m], VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geometries[m], mesh_triangles[m]);
        first_vertex += (uint64_t)mesh_triangles[m] * 3;
    }

    struct gpu_buf instances;
    buf_create(mesh_count * sizeof(VkAccelerationStructureInstanceKHR),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &instances);
    for (uint32_t m = 0; m < mesh_count; ++m)
        ((VkAccelerationStructureInstanceKHR *)instances.mapped)[m] =
            (VkAccelerationStructureInstanceKHR){.transform = {.matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}},
                                                 .mask = 255,
                                                 .accelerationStructureReference = blases[m].address};
    VkAccelerationStructureGeometryKHR instance_geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = instances.addr}};
    struct acceleration tlas;
    prepare_count(&tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &instance_geometry, mesh_count);

    struct gpu_buf result;
    buf_create(2 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &result);

    const VkShaderStageFlags stages_mask =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    const VkDescriptorSetLayoutBinding bindings[] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages_mask, NULL},
                                                     {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, stages_mask, NULL}};
    VkDescriptorSetLayoutCreateInfo descriptor_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout descriptor_layout;
    CHECK(vkCreateDescriptorSetLayout(g_dev, &descriptor_info, NULL, &descriptor_layout));
    const VkPushConstantRange push = {stages_mask, 0, 2 * sizeof(uint32_t)};
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
    const VkDescriptorBufferInfo buffer_info = {result.buf, 0, VK_WHOLE_SIZE};
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
    VkPipelineShaderStageCreateInfo stage_info[3];
    const VkShaderStageFlagBits kinds[] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
                                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
    VkRayTracingShaderGroupCreateInfoKHR groups[3];
    for (unsigned i = 0; i < 3; ++i) {
        modules[i] = make_shader(argv[1 + i]);
        stage_info[i] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = kinds[i], .module = modules[i], .pName = "main"};
        groups[i] = (VkRayTracingShaderGroupCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type = i < 2 ? VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
            .generalShader = i < 2 ? i : VK_SHADER_UNUSED_KHR,
            .closestHitShader = i == 2 ? 2 : VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR};
    }
    uint64_t alignment = rtp.shaderGroupBaseAlignment ? rtp.shaderGroupBaseAlignment : 64;
    uint64_t handle_alignment = rtp.shaderGroupHandleAlignment ? rtp.shaderGroupHandleAlignment : 32;
    if (alignment < handle_alignment) alignment = handle_alignment;
    uint64_t stride = align_up(rtp.shaderGroupHandleSize, alignment);
    struct gpu_buf sbt;
    buf_create(stride * 3 + alignment, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &sbt);
    const uint64_t sbt_offset = (alignment - (sbt.addr % alignment)) % alignment;

    VkDynamicState dynamic = VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR;
    const VkPipelineDynamicStateCreateInfo dynamics = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 1, .pDynamicStates = &dynamic};
    const VkRayTracingPipelineCreateInfoKHR pipeline_info = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
                                                             .stageCount = 3,
                                                             .pStages = stage_info,
                                                             .groupCount = 3,
                                                             .pGroups = groups,
                                                             .maxPipelineRayRecursionDepth = 1,
                                                             .pDynamicState = &dynamics,
                                                             .layout = layout};
    VkPipeline pipeline;
    CHECK(create(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
    unsigned char *handle_bytes = malloc((size_t)rtp.shaderGroupHandleSize * 3);
    REQUIRE(handle_bytes);
    CHECK(handles(g_dev, pipeline, 0, 3, (size_t)rtp.shaderGroupHandleSize * 3, handle_bytes));
    for (unsigned i = 0; i < 3; ++i)
        memcpy((uint8_t *)sbt.mapped + sbt_offset + i * stride, handle_bytes + i * rtp.shaderGroupHandleSize,
               rtp.shaderGroupHandleSize);
    free(handle_bytes);
    const uint64_t raygen_stack = stack_size(g_dev, pipeline, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
    const uint64_t miss_stack = stack_size(g_dev, pipeline, 1, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
    const uint64_t hit_stack = stack_size(g_dev, pipeline, 2, VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR);
    const uint64_t maximum = miss_stack > hit_stack ? miss_stack : hit_stack;
    const uint64_t budget = raygen_stack + maximum;
    REQUIRE(budget && budget <= UINT32_MAX);
    if (g_fail) return 1;

    VkCommandBuffer build_command = begin();
    memory_barrier(build_command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                   VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const VkAccelerationStructureBuildRangeInfoKHR *range;
    for (uint32_t m = 0; m < mesh_count; ++m) {
        range = &blases[m].range;
        build(build_command, 1, &blases[m].info, &range);
    }
    memory_barrier(build_command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                   VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                   VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    range = &tlas.range;
    build(build_command, 1, &tlas.info, &range);
    memory_barrier(build_command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                   VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                   VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    submit(build_command);

    VkQueryPoolCreateInfo query_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    VkQueryPool query_pool;
    CHECK(vkCreateQueryPool(g_dev, &query_info, NULL, &query_pool));
    const VkStridedDeviceAddressRegionKHR regions[] = {{sbt.addr + sbt_offset, stride, stride},
                                                       {sbt.addr + sbt_offset + stride, stride, stride},
                                                       {sbt.addr + sbt_offset + 2 * stride, stride, stride},
                                                       {0, 0, 0}};
    const uint32_t total_rays = width * height * spp;
    uint32_t mode0_hits = 0;
    for (uint32_t mode = 0; mode < 3; ++mode) {
        double best_ns_per_ray = 1e30;
        double best_mrays = 0.0;
        uint32_t best_ticks = 0;
        for (uint32_t iteration = 0; iteration < iterations + 1; ++iteration) {
            VkCommandBuffer command = begin();
            vkCmdResetQueryPool(command, query_pool, 0, 2);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, 0, 1, &set, 0, NULL);
            uint32_t push_values[2] = {spp, mode};
            vkCmdPushConstants(command, layout, stages_mask, 0, sizeof(push_values), push_values);
            set_stack(command, (uint32_t)budget);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, 0);
            trace(command, &regions[0], &regions[1], &regions[2], &regions[3], width, height, 1);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, 1);
            memory_barrier(command, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            CHECK(vkEndCommandBuffer(command));
            VkSubmitInfo submit_info = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
            CHECK(vkQueueSubmit(g_queue, 1, &submit_info, VK_NULL_HANDLE));
            CHECK(vkQueueWaitIdle(g_queue));
            uint64_t timestamps[2] = {0, 0};
            VkResult queried = vkGetQueryPoolResults(g_dev, query_pool, 0, 2, sizeof(timestamps), timestamps, sizeof(uint64_t),
                                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
            if (queried != VK_SUCCESS || timestamps[1] <= timestamps[0]) {
                fprintf(stderr, "timestamp query failed (result=%d, %llu..%llu)\n", (int)queried, (unsigned long long)timestamps[0],
                        (unsigned long long)timestamps[1]);
                return 77;
            }
            if (iteration == 0) continue; /* warmup */
            const uint32_t ticks = (uint32_t)(timestamps[1] - timestamps[0]);
            const double nanoseconds = (double)ticks * (double)properties.properties.limits.timestampPeriod;
            const double ns_per_ray = nanoseconds / (double)total_rays;
            const double mrays = (double)total_rays / nanoseconds * 1e3;
            printf("mode %u (%s) run %u: %u ticks, %.3f ms, %.2f M ray/s, %.2f ns/ray\n", mode,
                   mode == 0   ? "hit plane"
                   : mode == 1 ? "all miss"
                               : "loop only",
                   iteration, ticks, nanoseconds / 1e6, mrays, ns_per_ray);
            if (ns_per_ray < best_ns_per_ray) {
                best_ns_per_ray = ns_per_ray;
                best_mrays = mrays;
                best_ticks = ticks;
            }
        }
        const uint32_t hits = ((const uint32_t *)result.mapped)[0];
        if (mode == 0) mode0_hits = hits;
        printf("mode %u (%s): %ux%u x %u spp = %u rays; best %u ticks, %.2f M ray/s, %.2f ns/ray; invocation-0 hits=%u\n", mode,
               mode == 0   ? "hit plane"
               : mode == 1 ? "all miss"
                           : "loop only",
               width, height, spp, total_rays, best_ticks, best_mrays, best_ns_per_ray, hits);
    }
    if (!mode0_hits) {
        fprintf(stderr, "benchmark rays did not hit the acceleration structure\n");
        return 1;
    }

    vkDestroyQueryPool(g_dev, query_pool, NULL);
    vkDestroyPipeline(g_dev, pipeline, NULL);
    for (unsigned i = 0; i < 3; ++i) vkDestroyShaderModule(g_dev, modules[i], NULL);
    vkDestroyDescriptorPool(g_dev, pool, NULL);
    vkDestroyPipelineLayout(g_dev, layout, NULL);
    vkDestroyDescriptorSetLayout(g_dev, descriptor_layout, NULL);
    pDestroyAS(g_dev, tlas.handle, NULL);
    buf_destroy(&tlas.scratch);
    buf_destroy(&tlas.storage);
    for (uint32_t m = 0; m < mesh_count; ++m) {
        pDestroyAS(g_dev, blases[m].handle, NULL);
        buf_destroy(&blases[m].scratch);
        buf_destroy(&blases[m].storage);
    }
    free(blases);
    free(geometries);
    free(mesh_triangles);
    buf_destroy(&sbt);
    buf_destroy(&result);
    buf_destroy(&instances);
    buf_destroy(&vertices);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    return 0;
}
