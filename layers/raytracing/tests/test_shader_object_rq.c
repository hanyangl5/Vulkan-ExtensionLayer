/* Copyright (c) 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT
 * Public RQ shader-object source/binary execution with mixed RT push ranges.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <vulkan/vulkan.h>
static VkResult make_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
static VkResult make_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
#define vkCreateDevice make_device
#define vkCreateInstance make_instance
#define VKRT_TEST_FIXTURE_ONLY
#include "00_single_tri/main.c"
#undef vkCreateDevice
#undef vkCreateInstance

static VkResult make_instance(const VkInstanceCreateInfo *source, const VkAllocationCallbacks *allocator, VkInstance *out) {
    VkApplicationInfo application = *source->pApplicationInfo;
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo local = *source;
    local.pApplicationInfo = &application;
    return vkCreateInstance(&local, allocator, out);
}
static VkResult make_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *source, const VkAllocationCallbacks *allocator,
                            VkDevice *out) {
    VkPhysicalDeviceShaderObjectFeaturesEXT objects = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingFeatures rendering = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
                                                          .pNext = &objects};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .pNext = &rendering};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &rt};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!objects.shaderObject || !rendering.dynamicRendering || !rt.rayTracingPipeline || !features.features.shaderInt64) exit(77);
    objects.pNext = (void *)source->pNext;
    const char **extensions = calloc((size_t)source->enabledExtensionCount + 2, sizeof(*extensions));
    if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(extensions, source->ppEnabledExtensionNames, source->enabledExtensionCount * sizeof(*extensions));
    VkDeviceCreateInfo local = *source;
    VkPhysicalDeviceFeatures core = source->pEnabledFeatures ? *source->pEnabledFeatures : (VkPhysicalDeviceFeatures){0};
    core.shaderInt64 = VK_TRUE;
    local.pEnabledFeatures = &core;
    local.pNext = &rt;
    local.ppEnabledExtensionNames = extensions;
    extensions[local.enabledExtensionCount++] = VK_EXT_SHADER_OBJECT_EXTENSION_NAME;
    extensions[local.enabledExtensionCount++] = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;
    VkResult result = vkCreateDevice(physical, &local, allocator, out);
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
                    const VkAccelerationStructureGeometryKHR *geometry) {
    memset(as, 0, sizeof(*as));
    as->range.primitiveCount = 1;
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
static void memory_barrier(VkCommandBuffer command, VkPipelineStageFlags source, VkAccessFlags src,
                           VkPipelineStageFlags destination, VkAccessFlags dst) {
    VkMemoryBarrier memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst};
    vkCmdPipelineBarrier(command, source, destination, 0, 1, &memory, 0, NULL, 0, NULL);
}
static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = g_pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(g_dev, &allocate, &command));
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &info));
    return command;
}
static void submit(VkCommandBuffer command) {
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(g_queue, 1, &info, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(g_queue));
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
}
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    init_vulkan();
    VkDevice devices[2] = {g_dev, VK_NULL_HANDLE};
    VkCommandPool pools[2] = {g_pool, VK_NULL_HANDLE};
    VkQueue queues[2] = {g_queue, VK_NULL_HANDLE};
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = g_qfamily,
                                          .queueCount = 1,
                                          .pQueuePriorities = &priority};
    VkPhysicalDeviceBufferDeviceAddressFeatures address = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .bufferDeviceAddress = VK_TRUE};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &address,
        .accelerationStructure = VK_TRUE};
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &acceleration, .rayQuery = VK_TRUE};
    const char *extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,  VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,    VK_KHR_SPIRV_1_4_EXTENSION_NAME,
                                VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                      .pNext = &ray_query,
                                      .queueCreateInfoCount = 1,
                                      .pQueueCreateInfos = &queue_info,
                                      .enabledExtensionCount = ARRAY_SIZE(extensions),
                                      .ppEnabledExtensionNames = extensions};
    CHECK(make_device(g_pdev, &device_info, NULL, &devices[1]));
    vkGetDeviceQueue(devices[1], g_qfamily, 0, &queues[1]);
    VkCommandPoolCreateInfo pool_create = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_qfamily};
    CHECK(vkCreateCommandPool(devices[1], &pool_create, NULL, &pools[1]));
    void *exported[2] = {NULL, NULL};
    size_t exported_sizes[2] = {0, 0};
    for (unsigned device_index = 0; device_index < 2; ++device_index) {
        g_dev = devices[device_index];
        g_queue = queues[device_index];
        g_pool = pools[device_index];
        PFN_vkCreateShadersEXT create = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
        PFN_vkDestroyShaderEXT destroy = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
        PFN_vkGetShaderBinaryDataEXT binary = (void *)vkGetDeviceProcAddr(g_dev, "vkGetShaderBinaryDataEXT");
        PFN_vkCmdBindShadersEXT bind = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBindShadersEXT");
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        struct gpu_buf vertices, instances, output;
        buf_create(36,
                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
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
        VkAccelerationStructureGeometryKHR instance = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                                   .data.deviceAddress = instances.addr}};
        prepare(&tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &instance);
        PFN_vkCmdBuildAccelerationStructuresKHR build = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
        VkCommandBuffer command = begin();
        struct acceleration *structures[] = {&blas, &tlas};
        for (unsigned i = 0; i < 2; ++i) {
            const VkAccelerationStructureBuildRangeInfoKHR *range = &structures[i]->range;
            build(command, 1, &structures[i]->info, &range);
            memory_barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                           VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                           VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT);
        }
        submit(command);
        buf_create(8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, &output);
        VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo dsl_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
        VkDescriptorSetLayout dsl;
        CHECK(vkCreateDescriptorSetLayout(g_dev, &dsl_info, NULL, &dsl));
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
        VkDescriptorPool pool;
        CHECK(vkCreateDescriptorPool(g_dev, &pool_info, NULL, &pool));
        VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                .descriptorPool = pool,
                                                .descriptorSetCount = 1,
                                                .pSetLayouts = &dsl};
        VkDescriptorSet set;
        CHECK(vkAllocateDescriptorSets(g_dev, &allocate, &set));
        VkWriteDescriptorSetAccelerationStructureKHR as_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1,
            .pAccelerationStructures = &tlas.handle};
        VkDescriptorBufferInfo buffer = {output.buf, 0, 8};
        VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .pNext = &as_write,
                                          .dstSet = set,
                                          .dstBinding = 0,
                                          .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
                                         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet = set,
                                          .dstBinding = 1,
                                          .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          .pBufferInfo = &buffer}};
        vkUpdateDescriptorSets(g_dev, 2, writes, 0, NULL);
        size_t word_count;
        uint32_t *words = read_spv(argv[1], &word_count);
        for (unsigned all = 0; all < 2; ++all) {
            VkPushConstantRange ranges[] = {
                {all ? VK_SHADER_STAGE_ALL : VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 48},
                {VK_SHADER_STAGE_MISS_BIT_KHR, 64, 4}};
            uint32_t range_count = all ? 1 : 2;
            VkPipelineLayoutCreateInfo pl_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                  .setLayoutCount = 1,
                                                  .pSetLayouts = &dsl,
                                                  .pushConstantRangeCount = range_count,
                                                  .pPushConstantRanges = ranges};
            VkPipelineLayout layout;
            CHECK(vkCreatePipelineLayout(g_dev, &pl_info, NULL, &layout));
            VkShaderCreateInfoEXT shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                                 .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                 .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                                 .codeSize = word_count * 4,
                                                 .pCode = words,
                                                 .pName = "main",
                                                 .setLayoutCount = 1,
                                                 .pSetLayouts = &dsl,
                                                 .pushConstantRangeCount = range_count,
                                                 .pPushConstantRanges = ranges};
            if (device_index) {
                shader_info.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
                shader_info.pCode = exported[all];
                shader_info.codeSize = exported_sizes[all];
                shader_info.pName = NULL;
            }
            VkShaderEXT shaders[2];
            CHECK(create(g_dev, 1, &shader_info, NULL, &shaders[0]));
            size_t binary_size = 0;
            CHECK(binary(g_dev, shaders[0], &binary_size, NULL));
            void *bytes = malloc(binary_size);
            if (!bytes) return 1;
            CHECK(binary(g_dev, shaders[0], &binary_size, bytes));
            if (!device_index) {
                exported[all] = malloc(binary_size);
                if (!exported[all]) return 1;
                memcpy(exported[all], bytes, binary_size);
                exported_sizes[all] = binary_size;
            }
            shader_info.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
            shader_info.codeSize = binary_size;
            shader_info.pCode = bytes;
            CHECK(create(g_dev, 1, &shader_info, NULL, &shaders[1]));
            free(bytes);
            for (unsigned kind = 0; kind < 2; ++kind) {
                memset(output.mapped, 0, 8);
                command = begin();
                const VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
                bind(command, 1, &stage, &shaders[kind]);
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
                for (unsigned ray = 0; ray < 2; ++ray) {
                    struct {
                        float origin[3], tmin, direction[3], tmax;
                        uint32_t flags, mask, index, bias;
                    } push = {{ray ? 4.f : 0.f, 0, -1}, 0, {0, 0, 1}, 10, 1 /* gl_RayFlagsOpaqueEXT */, 255, ray, 7};
                    vkCmdPushConstants(command, layout, ranges[0].stageFlags, 0, sizeof(push), &push);
                    vkCmdDispatch(command, 1, 1, 1);
                }
                memory_barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                submit(command);
                const uint32_t *result = output.mapped;
                if (result[0] != 107 || result[1] != 207) {
                    fprintf(stderr, "RQ shaderobject all%u binary%u results %u/%u\n", all, kind, result[0], result[1]);
                    ++g_fail;
                }
            }
            for (unsigned i = 0; i < 2; ++i) destroy(g_dev, shaders[i], NULL);
            vkDestroyPipelineLayout(g_dev, layout, NULL);
        }
        free(words);
        vkDestroyDescriptorPool(g_dev, pool, NULL);
        vkDestroyDescriptorSetLayout(g_dev, dsl, NULL);
        for (unsigned i = 0; i < 2; ++i) {
            pDestroyAS(g_dev, structures[i]->handle, NULL);
            buf_destroy(&structures[i]->scratch);
            buf_destroy(&structures[i]->storage);
        }
        buf_destroy(&output);
        buf_destroy(&instances);
        buf_destroy(&vertices);
        vkDestroyCommandPool(g_dev, g_pool, NULL);
        vkDestroyDevice(g_dev, NULL);
    }
    free(exported[0]);
    free(exported[1]);
    vkDestroyInstance(g_instance, NULL);
    printf("Public RQ compute shader objects mixed/ALL push ranges +SPIRV/binary hit/miss across two independent devices %s\n",
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
