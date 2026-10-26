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
static bool capability_harness, advertised, maintenance6_mode, dynamic_layout, graphics_queue, shader_object_mode;
#include <vulkan/vulkan.h>
static VkQueue second_queue;
static PFN_vkCmdBindDescriptorSets2 bind2[2];
static PFN_vkCmdPushConstants2 push2[2];
static unsigned bind_count, push_count;
static const VkPipelineLayoutCreateInfo *inline_layout;
static PFN_vkCreateShadersEXT create_shaders;
static PFN_vkDestroyShaderEXT destroy_shader;
static PFN_vkCmdBindShadersEXT bind_shaders;
static VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *a,
                              VkDevice *out);
static VkResult create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *a, VkInstance *out);
#define vkCreateDevice create_device
#define vkCreateInstance create_instance
#define VKRT_TEST_FIXTURE_ONLY
#include "00_single_tri/main.c"
#undef vkCreateDevice
#undef vkCreateInstance

static VkResult create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *a, VkInstance *out) {
    VkApplicationInfo app = *info->pApplicationInfo;
    VkInstanceCreateInfo local = *info;
    if (maintenance6_mode)
        app.apiVersion = VK_API_VERSION_1_4;
    else if (shader_object_mode)
        app.apiVersion = VK_API_VERSION_1_3;
    local.pApplicationInfo = &app;
    return vkCreateInstance(&local, a, out);
}
static VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *a,
                              VkDevice *out) {
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceShaderObjectFeaturesEXT objects = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingFeatures rendering = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
                                                          .pNext = &objects};
    VkPhysicalDevicePerStageDescriptorSetFeaturesNV per_stage = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV, .pNext = &rendering};
    VkPhysicalDeviceMaintenance6Features six = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES,
                                                .pNext = &per_stage};
    VkPhysicalDeviceMaintenance4Features maintenance = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES,
                                                        .pNext = &six};
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
    if (maintenance6_mode && !six.maintenance6) {
        printf("Maintenance6 API proof requires maintenance6\n");
        exit(77);
    }
    if (shader_object_mode && (!objects.shaderObject || !rendering.dynamicRendering)) {
        printf("Compute shader-object interoperation requires shaderObject and dynamicRendering\n");
        exit(77);
    }
    dynamic_layout = maintenance6_mode && per_stage.dynamicPipelineLayout;
    VkDeviceCreateInfo local = *info;
    maintenance.pNext = (void *)info->pNext;
    if (maintenance6_mode) {
        six.pNext = maintenance.pNext;
        maintenance.pNext = &six;
        if (dynamic_layout) {
            per_stage.perStageDescriptorSet = VK_FALSE;
            per_stage.pNext = six.pNext;
            six.pNext = &per_stage;
        }
    }
    local.pNext = &maintenance;
    if (shader_object_mode) {
        objects.pNext = maintenance.pNext;
        rendering.pNext = &objects;
        maintenance.pNext = &rendering;
    }
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enabled = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &maintenance,
        .rayTracingPipeline = VK_TRUE};
    const char **extensions = malloc((info->enabledExtensionCount + 5) * sizeof(*extensions));
    if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(extensions, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof(*extensions));
    extensions[local.enabledExtensionCount++] = VK_KHR_MAINTENANCE_4_EXTENSION_NAME;
    if (maintenance6_mode) extensions[local.enabledExtensionCount++] = VK_KHR_MAINTENANCE_6_EXTENSION_NAME;
    if (dynamic_layout) extensions[local.enabledExtensionCount++] = VK_NV_PER_STAGE_DESCRIPTOR_SET_EXTENSION_NAME;
    if (shader_object_mode) extensions[local.enabledExtensionCount++] = VK_EXT_SHADER_OBJECT_EXTENSION_NAME;
    local.ppEnabledExtensionNames = extensions;
    if (advertised) {
        extensions[local.enabledExtensionCount++] = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;
        local.pNext = &enabled;
        local.ppEnabledExtensionNames = extensions;
    }
    // Request a second queue when available, so independent recorded contexts
    // can execute without a cross-queue wait or shared mutable frame storage.
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties *families = malloc((size_t)family_count * sizeof(*families));
    if (!families) {
        free(extensions);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    VkDeviceQueueCreateInfo queue_info = info->pQueueCreateInfos[0];
    const float priorities[] = {1.0f, 1.0f};
    graphics_queue =
        queue_info.queueFamilyIndex < family_count && (families[queue_info.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    bool two_queues = info->queueCreateInfoCount == 1 && queue_info.queueFamilyIndex < family_count &&
                      families[queue_info.queueFamilyIndex].queueCount >= 2;
    if (two_queues) {
        queue_info.queueCount = 2;
        queue_info.pQueuePriorities = priorities;
        local.pQueueCreateInfos = &queue_info;
    }
    VkResult result = vkCreateDevice(physical, &local, a, out);
    if (result == VK_SUCCESS && two_queues) vkGetDeviceQueue(*out, queue_info.queueFamilyIndex, 1, &second_queue);
    free(families);
    free(extensions);
    return result;
}
// Alternate both core and KHR aliases, and both ordinary handles and the
// inline dynamic layout path where supported. Each original bind point keeps
// its independent state even when the implementation dispatches RT in compute.
static void push_values(VkCommandBuffer command, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                        const void *values) {
    if (!maintenance6_mode) {
        vkCmdPushConstants(command, layout, stages, offset, size, values);
        return;
    }
    unsigned sequence = push_count++;
    VkPushConstantsInfo info = {.sType = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
                                .layout = layout,
                                .stageFlags = stages,
                                .offset = offset,
                                .size = size,
                                .pValues = values};
    if (dynamic_layout && (sequence & 1)) {
        info.layout = VK_NULL_HANDLE;
        info.pNext = inline_layout;
    }
    push2[(sequence >> 1) & 1](command, &info);
}
static void bind_sets(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout, VkDescriptorSet set) {
    if (!maintenance6_mode) {
        vkCmdBindDescriptorSets(command, point, layout, 0, 1, &set, 0, NULL);
        return;
    }
    unsigned sequence = bind_count++;
    VkBindDescriptorSetsInfo info = {
        .sType = VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO,
        .layout = layout,
        .stageFlags = point == VK_PIPELINE_BIND_POINT_COMPUTE ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        .descriptorSetCount = 1,
        .pDescriptorSets = &set};
    if (!sequence && graphics_queue) info.stageFlags = VK_SHADER_STAGE_ALL;
    if (dynamic_layout && (sequence & 1)) {
        info.layout = VK_NULL_HANDLE;
        info.pNext = inline_layout;
    }
    bind2[(sequence >> 1) & 1](command, &info);
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
    maintenance6_mode = argc == 6 && !strcmp(argv[5], "--maintenance6");
    shader_object_mode = argc == 6 && !strcmp(argv[5], "--shader-object");
    if (argc != 5 && !capability_harness && !maintenance6_mode && !shader_object_mode) {
        fprintf(stderr, "usage: %s rgen.spv miss.spv chit.spv compute.spv [--capability-harness|--maintenance6|--shader-object]\n",
                argv[0]);
        return 1;
    }
    init_vulkan();
    if (shader_object_mode) {
        create_shaders = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
        destroy_shader = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
        bind_shaders = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBindShadersEXT");
        REQUIRE(create_shaders && destroy_shader && bind_shaders);
        if (g_fail) return 1;
    }
    if (maintenance6_mode) {
        bind2[0] = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBindDescriptorSets2");
        bind2[1] = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBindDescriptorSets2KHR");
        push2[0] = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdPushConstants2");
        push2[1] = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdPushConstants2KHR");
        REQUIRE(bind2[0] && bind2[1] && push2[0] && push2[1]);
        if (g_fail) return 1;
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
    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(g_pdev, &device_properties);
    VkDeviceSize output_stride = device_properties.limits.minStorageBufferOffsetAlignment;
    if (output_stride < 8 * 16) output_stride = 8 * 16;
    output_stride = (8 * 16 + output_stride - 1) / output_stride * output_stride;
    buf_create(output_stride + 8 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, &output);
    buf_create(3 * 64, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host, &sbt);
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, NULL}};
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(g_dev, &dslci, NULL, &dsl));
    VkDescriptorSetLayout empty_layout;
    VkDescriptorSetLayoutCreateInfo empty_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    CHECK(vkCreateDescriptorSetLayout(g_dev, &empty_info, NULL, &empty_layout));
    // Occupy every advertised set slot, and the complete native compute push
    // range below. The private context must fit without an appended slot/range.
    uint32_t layout_count = device_properties.limits.maxBoundDescriptorSets;
    VkDescriptorSetLayout *layout_sets = malloc((size_t)layout_count * sizeof(*layout_sets));
    if (!layout_sets) return 1;
    for (uint32_t i = 0; i < layout_count; ++i) layout_sets[i] = empty_layout;
    layout_sets[0] = dsl;
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                         {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2}};
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 2, .pPoolSizes = pool_sizes};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(g_dev, &pci, NULL, &pool));
    VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = pool,
                                         .descriptorSetCount = 1,
                                         .pSetLayouts = &dsl};
    VkDescriptorSet sets[2];
    for (unsigned target = 0; target < 2; ++target) CHECK(vkAllocateDescriptorSets(g_dev, &alloc, &sets[target]));
    VkDescriptorBufferInfo bi = {output.buf, 0, 8 * 16};
    VkWriteDescriptorSetAccelerationStructureKHR as_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &tlas.handle};
    VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = sets[0],
                                      .dstBinding = 0,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bi},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .pNext = &as_write,
                                      .dstSet = sets[0],
                                      .dstBinding = 1,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}};
    for (unsigned target = 0; target < 2; ++target) {
        bi.offset = target * output_stride;
        writes[0].dstSet = writes[1].dstSet = sets[target];
        vkUpdateDescriptorSets(g_dev, 2, writes, 0, NULL);
    }
    VkPushConstantRange ranges[] = {{VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 4},
                                    {VK_SHADER_STAGE_MISS_BIT_KHR, 8, 4},
                                    {VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 12, 4},
                                    {VK_SHADER_STAGE_COMPUTE_BIT, 0, device_properties.limits.maxPushConstantsSize}};
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = layout_count,
                                      .pSetLayouts = layout_sets,
                                      .pushConstantRangeCount = shader_object_mode ? 3 : 4,
                                      .pPushConstantRanges = ranges};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(g_dev, &lci, NULL, &layout));
    VkPipelineLayout compute_layout = layout;
    if (shader_object_mode) {
        VkPipelineLayoutCreateInfo ordinary = lci;
        ordinary.pushConstantRangeCount = 1;
        ordinary.pPushConstantRanges = &ranges[3];
        CHECK(vkCreatePipelineLayout(g_dev, &ordinary, NULL, &compute_layout));
    }
    inline_layout = &lci;
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
                                                .layout = compute_layout};
    VkPipeline compute;
    CHECK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &compute_info, NULL, &compute));
    VkShaderEXT compute_shader = VK_NULL_HANDLE;
    if (shader_object_mode) {
        size_t words;
        uint32_t *code = read_spv(argv[4], &words);
        VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                      .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                      .codeSize = words * 4,
                                      .pCode = code,
                                      .pName = "main",
                                      .setLayoutCount = layout_count,
                                      .pSetLayouts = layout_sets,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &ranges[3]};
        CHECK(create_shaders(g_dev, 1, &info, NULL, &compute_shader));
        free(code);
    }
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
    unsigned trace_calls = 0;
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
        if (shader_object_mode) {
            const VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
            // The second iteration switches back from an explicitly unbound
            // shader object to a pipeline, checking both disturbance directions.
            bind_shaders(command, 1, &stage, iteration ? NULL : &compute_shader);
            if (iteration) vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, compute);
        }
        bind_sets(command, VK_PIPELINE_BIND_POINT_COMPUTE, shader_object_mode ? compute_layout : layout, sets[0]);
        uint32_t ordinary = 123 + iteration;
        push_values(command, shader_object_mode ? compute_layout : layout, VK_SHADER_STAGE_COMPUTE_BIT, 4, 4, &ordinary);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt);
        bind_sets(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, sets[0]);
        uint32_t values[] = {3 + iteration, 7 + iteration, 11 + iteration};
        const uint32_t push_offsets[] = {0, 8, 12};
        for (unsigned i = 0; i < 3; ++i) {
            push_values(command, layout, kinds[i] | (shader_object_mode ? 0 : VK_SHADER_STAGE_COMPUTE_BIT), push_offsets[i], 4,
                        &values[i]);
            if (shader_object_mode)
                push_values(command, compute_layout, VK_SHADER_STAGE_COMPUTE_BIT, push_offsets[i], 4, &values[i]);
        }
        const uint32_t tail = 91 + iteration;
        push_values(command, shader_object_mode ? compute_layout : layout, VK_SHADER_STAGE_COMPUTE_BIT, 124, 4, &tail);
        set_stack(command, (uint32_t)budget + iteration * 16);
        VkStridedDeviceAddressRegionKHR regions[] = {
            {sbt.addr, 64, 64}, {sbt.addr + 64, 64, 64}, {sbt.addr + 128, 64, 64}, {0, 0, 0}};
        trace(command, &regions[0], &regions[1], &regions[2], &regions[3], 8, 1, 1);
        ++trace_calls;
        memory_barrier(command, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdDispatch(command, 8, 1, 1);
        memory_barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        values[0] += 5;
        push_values(command, layout, VK_SHADER_STAGE_RAYGEN_BIT_KHR | (shader_object_mode ? 0 : VK_SHADER_STAGE_COMPUTE_BIT), 0, 4,
                    &values[0]);
        if (shader_object_mode) push_values(command, compute_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &values[0]);
        bind_sets(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, sets[1]);
        vkDestroyPipelineLayout(g_dev, layout, NULL);
        trace(command, &regions[0], &regions[1], &regions[2], &regions[3], 8, 1, 1);
        ++trace_calls;
        memory_barrier(command, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
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
            REQUIRE(result[i * 4] == expected && result[i * 4 + 1] == 8 && result[i * 4 + 2] == i &&
                    result[i * 4 + 3] == ordinary + values[0] + tail);
            const uint32_t *second = (const uint32_t *)((const uint8_t *)output.mapped + output_stride);
            REQUIRE(second[i * 4] == expected + 5 && second[i * 4 + 1] == 8 && second[i * 4 + 2] == i && second[i * 4 + 3] == 0);
        }
        printf("Canonical public RTP GPU trace/reset/layout-lifetime/compute-restore iteration%u %s\n", iteration,
               g_fail ? "FAIL" : "PASS");
    }
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
    if (second_queue) {
        CHECK(vkCreatePipelineLayout(g_dev, &lci, NULL, &layout));
        ca.commandBufferCount = 2;
        VkCommandBuffer concurrent[2];
        CHECK(vkAllocateCommandBuffers(g_dev, &ca, concurrent));
        memset(output.mapped, 0xa5, (size_t)output.size);
        for (unsigned target = 0; target < 2; ++target) {
            VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(vkBeginCommandBuffer(concurrent[target], &begin));
            memory_barrier(concurrent[target], VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                           VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
            vkCmdBindPipeline(concurrent[target], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt);
            bind_sets(concurrent[target], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, sets[target]);
            const uint32_t values[] = {3 + target, 7 + target, 11 + target};
            const uint32_t offsets[] = {0, 8, 12};
            for (unsigned i = 0; i < 3; ++i)
                push_values(concurrent[target], layout, kinds[i] | (shader_object_mode ? 0 : VK_SHADER_STAGE_COMPUTE_BIT),
                            offsets[i], 4, &values[i]);
            set_stack(concurrent[target], (uint32_t)budget + 16);
            VkStridedDeviceAddressRegionKHR regions[] = {
                {sbt.addr, 64, 64}, {sbt.addr + 64, 64, 64}, {sbt.addr + 128, 64, 64}, {0, 0, 0}};
            trace(concurrent[target], &regions[0], &regions[1], &regions[2], &regions[3], 8, 1, 1);
            ++trace_calls;
            memory_barrier(concurrent[target], VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            CHECK(vkEndCommandBuffer(concurrent[target]));
        }
        VkQueue queues[] = {g_queue, second_queue};
        VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore ready;
        CHECK(vkCreateSemaphore(g_dev, &semaphore_info, NULL, &ready));
        // Publish the earlier queue0 AS builds to queue1. The independent trace
        // submissions follow this signal and do not wait on each other.
        VkSubmitInfo publish = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1, .pSignalSemaphores = &ready};
        CHECK(vkQueueSubmit(g_queue, 1, &publish, VK_NULL_HANDLE));
        const VkPipelineStageFlags ready_stage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        for (unsigned target = 0; target < 2; ++target) {
            VkSubmitInfo submit = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &concurrent[target]};
            if (target) {
                submit.waitSemaphoreCount = 1;
                submit.pWaitSemaphores = &ready;
                submit.pWaitDstStageMask = &ready_stage;
            }
            CHECK(vkQueueSubmit(queues[target], 1, &submit, VK_NULL_HANDLE));
        }
        for (unsigned target = 0; target < 2; ++target) CHECK(vkQueueWaitIdle(queues[target]));
        vkDestroySemaphore(g_dev, ready, NULL);
        for (unsigned target = 0; target < 2; ++target) {
            const uint32_t *result = (const uint32_t *)((const uint8_t *)output.mapped + target * output_stride);
            for (unsigned i = 0; i < 8; ++i) {
                uint32_t expected = (i & 1) ? 19 + 2 * target : 54 + 2 * target;
                REQUIRE(result[i * 4] == expected && result[i * 4 + 1] == 8 && result[i * 4 + 2] == i && result[i * 4 + 3] == 0);
            }
        }
        printf("Cached native RTP pipeline with two queues/disjoint contexts %s\n", g_fail ? "FAIL" : "PASS");
        vkFreeCommandBuffers(g_dev, g_pool, 2, concurrent);
        vkDestroyPipelineLayout(g_dev, layout, NULL);
    }
    vkDestroyPipeline(g_dev, rt, NULL);
    vkDestroyPipeline(g_dev, compute, NULL);
    if (compute_shader) destroy_shader(g_dev, compute_shader, NULL);
    if (shader_object_mode) vkDestroyPipelineLayout(g_dev, compute_layout, NULL);
    vkDestroyDescriptorPool(g_dev, pool, NULL);
    vkDestroyDescriptorSetLayout(g_dev, dsl, NULL);
    vkDestroyDescriptorSetLayout(g_dev, empty_layout, NULL);
    free(layout_sets);
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
    printf("Recorded public Trace calls=%u across two stack budgets; occupied set slots=%u and compute push bytes=%u\n",
           trace_calls, layout_count, device_properties.limits.maxPushConstantsSize);
    if (maintenance6_mode)
        printf("Maintenance6 core/KHR API aliases with dynamic layouts=%u: %s\n", dynamic_layout, g_fail ? "FAIL" : "PASS");
    if (shader_object_mode) printf("Compute shader object/RTP/pipeline state restoration: %s\n", g_fail ? "FAIL" : "PASS");
    printf("RTP API test mode=%s %s\n", advertised ? "advertised" : "explicit capability harness", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
