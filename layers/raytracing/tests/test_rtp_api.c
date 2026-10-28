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
static bool capability_harness, advertised, maintenance6_mode, dynamic_layout, graphics_queue, shader_object_mode, derivatives_mode;
static unsigned library_mode;
/* 1: push writes, 2: update templates, 3: descriptor-buffer offsets. */
static unsigned descriptor_mode;
static bool separate_compute, descriptor_core;
static bool debug_names_mode, vulkan11_mode, physical_source_mode;
static bool private_data_mode, private_core, private_extension;
#include <vulkan/vulkan.h>
#if VK_USE_64_BIT_PTR_DEFINES
#define OBJECT_HANDLE(object) ((uint64_t)(uintptr_t)(object))
#else
#define OBJECT_HANDLE(object) ((uint64_t)(object))
#endif
static uint32_t requested_api;
static VkQueue second_queue;
static PFN_vkCmdBindDescriptorSets2 bind2[2];
static PFN_vkCmdPushConstants2 push2[2];
static unsigned bind_count, push_count;
static const VkPipelineLayoutCreateInfo *inline_layout;
static PFN_vkCreateShadersEXT create_shaders;
static PFN_vkDestroyShaderEXT destroy_shader;
static PFN_vkCmdBindShadersEXT bind_shaders;
static PFN_vkSetDebugUtilsObjectNameEXT debug_name;
static PFN_vkSetDebugUtilsObjectTagEXT debug_tag;
static PFN_vkCreatePrivateDataSlot create_private_slot[2];
static PFN_vkDestroyPrivateDataSlot destroy_private_slot[2];
static PFN_vkSetPrivateData set_private_data[2];
static PFN_vkGetPrivateData get_private_data[2];
static VkPrivateDataSlot private_slot;
static PFN_vkCmdPushDescriptorSetKHR push_descriptors[2];
static PFN_vkCmdPushDescriptorSet2 push_descriptors2[2];
static PFN_vkCmdPushDescriptorSetWithTemplateKHR push_template[2];
static PFN_vkCmdPushDescriptorSetWithTemplate2 push_template2[2];
static PFN_vkCmdBindDescriptorBuffersEXT bind_descriptor_buffers;
static PFN_vkCmdSetDescriptorBufferOffsetsEXT descriptor_offsets;
static PFN_vkCmdSetDescriptorBufferOffsets2EXT descriptor_offsets2;
static VkDescriptorBufferInfo compute_output;
static VkDescriptorBufferBindingInfoEXT descriptor_binding;
static VkDeviceSize descriptor_record_stride;
static const VkPipelineLayoutCreateInfo *compute_inline_layout;
static VkPipelineLayout compute_public_layout;
static unsigned descriptor_calls;
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
    if (vulkan11_mode)
        app.apiVersion = VK_API_VERSION_1_1;
    else if (maintenance6_mode || descriptor_mode)
        app.apiVersion = VK_API_VERSION_1_4;
    else if (shader_object_mode)
        app.apiVersion = VK_API_VERSION_1_3;
    PFN_vkEnumerateInstanceVersion version = (void *)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t loader_api = VK_API_VERSION_1_0;
    if (version) CHECK(version(&loader_api));
    if (vulkan11_mode && loader_api < VK_API_VERSION_1_1) {
        printf("Vulkan1.1 application proof requires a Vulkan1.1 loader\n");
        exit(77);
    }
    if (app.apiVersion > loader_api) app.apiVersion = loader_api;
    requested_api = app.apiVersion;
    local.pApplicationInfo = &app;
    const char **extensions = NULL;
    if (debug_names_mode) {
        uint32_t count = 0;
        CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL));
        VkExtensionProperties *available = calloc(count ? count : 1, sizeof(*available));
        if (!available) return VK_ERROR_OUT_OF_HOST_MEMORY;
        CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, available));
        bool found = false;
        for (uint32_t i = 0; i < count; ++i) found |= !strcmp(available[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        free(available);
        if (!found) {
            printf("Virtual-object annotation proof requires debug-utils\n");
            exit(77);
        }
        extensions = malloc(((size_t)info->enabledExtensionCount + 1) * sizeof(*extensions));
        if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (info->enabledExtensionCount)
            memcpy(extensions, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof(*extensions));
        extensions[local.enabledExtensionCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        local.ppEnabledExtensionNames = extensions;
    }
    VkResult result = vkCreateInstance(&local, a, out);
    free(extensions);
    return result;
}
static bool device_extension(VkPhysicalDevice physical, const char *name) {
    uint32_t count = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *values = calloc(count ? count : 1, sizeof(*values));
    if (!values) exit(1);
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, values));
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) found |= !strcmp(values[i].extensionName, name);
    free(values);
    return found;
}
static VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *a,
                              VkDevice *out) {
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDevicePrivateDataFeatures private_data = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT buffers = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, .pNext = &private_data};
    VkPhysicalDeviceShaderObjectFeaturesEXT objects = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
                                                       .pNext = &buffers};
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
    VkPhysicalDeviceProperties native_properties;
    vkGetPhysicalDeviceProperties(physical, &native_properties);
    descriptor_core = native_properties.apiVersion >= VK_API_VERSION_1_4 && requested_api >= VK_API_VERSION_1_4;
    private_core = native_properties.apiVersion >= VK_API_VERSION_1_3 && requested_api >= VK_API_VERSION_1_3;
    private_extension = private_data_mode && device_extension(physical, VK_EXT_PRIVATE_DATA_EXTENSION_NAME);
    if (private_data_mode && (!private_data.privateData || (!private_core && !private_extension))) {
        printf("Virtual-object private-data proof requires enabled native privateData support\n");
        exit(77);
    }
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
    if (descriptor_mode && !device_extension(physical, descriptor_mode == 3 ? VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME
                                                                            : VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        printf("Compute descriptor interoperation requires the requested native descriptor extension\n");
        exit(77);
    }
    if (descriptor_mode == 3 && !buffers.descriptorBuffer) {
        printf("Descriptor-buffer interoperation requires descriptorBuffer\n");
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
        objects.pNext = NULL;
        objects.pNext = maintenance.pNext;
        rendering.pNext = &objects;
        maintenance.pNext = &rendering;
    }
    if (descriptor_mode == 3) {
        buffers =
            (VkPhysicalDeviceDescriptorBufferFeaturesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
                                                          .pNext = maintenance.pNext,
                                                          .descriptorBuffer = VK_TRUE};
        maintenance.pNext = &buffers;
    }
    if (private_data_mode) {
        private_data.pNext = maintenance.pNext;
        private_data.privateData = VK_TRUE;
        maintenance.pNext = &private_data;
    }
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enabled = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &maintenance,
        .rayTracingPipeline = VK_TRUE};
    const char **extensions = malloc((info->enabledExtensionCount + 10) * sizeof(*extensions));
    if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(extensions, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof(*extensions));
    extensions[local.enabledExtensionCount++] = VK_KHR_MAINTENANCE_4_EXTENSION_NAME;
    if (maintenance6_mode) extensions[local.enabledExtensionCount++] = VK_KHR_MAINTENANCE_6_EXTENSION_NAME;
    if (dynamic_layout) extensions[local.enabledExtensionCount++] = VK_NV_PER_STAGE_DESCRIPTOR_SET_EXTENSION_NAME;
    if (shader_object_mode) extensions[local.enabledExtensionCount++] = VK_EXT_SHADER_OBJECT_EXTENSION_NAME;
    if (library_mode) extensions[local.enabledExtensionCount++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
    if (descriptor_mode)
        extensions[local.enabledExtensionCount++] =
            descriptor_mode == 3 ? VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME : VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    if (private_extension) extensions[local.enabledExtensionCount++] = VK_EXT_PRIVATE_DATA_EXTENSION_NAME;
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
    VkPhysicalDeviceFeatures enabled_core = info->pEnabledFeatures ? *info->pEnabledFeatures : (VkPhysicalDeviceFeatures){0};
    if (physical_source_mode) {
        if (!features.features.shaderInt64) exit(77);
        enabled_core.shaderInt64 = VK_TRUE;
        local.pEnabledFeatures = &enabled_core;
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
        info.pNext = layout == compute_public_layout ? compute_inline_layout : inline_layout;
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
static void clobber_input(void *data, size_t size) {
    volatile uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0xcd;
}
static void annotate_object(VkObjectType type, uint64_t handle, const char *name) {
    if (debug_names_mode) {
        const VkDebugUtilsObjectNameInfoEXT naming = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL, type, handle, name};
        CHECK(debug_name(g_dev, &naming));
        unsigned char bytes[] = {0, 19, 255, 0, 37};
        const VkDebugUtilsObjectTagInfoEXT tagging = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT,
                                                      NULL,
                                                      type,
                                                      handle,
                                                      UINT64_C(0x7261797472616365),
                                                      sizeof(bytes),
                                                      bytes};
        CHECK(debug_tag(g_dev, &tagging));
        clobber_input(bytes, sizeof(bytes));
    }
    if (private_data_mode) {
        uint64_t value = UINT64_MAX;
        get_private_data[0](g_dev, type, handle, private_slot, &value);
        REQUIRE(value == 0);
        const uint64_t expected = UINT64_C(0xfedcba9876543210) ^ type;
        CHECK(set_private_data[0](g_dev, type, handle, private_slot, expected));
        get_private_data[1](g_dev, type, handle, private_slot, &value);
        REQUIRE(value == expected);
        CHECK(set_private_data[1](g_dev, type, handle, private_slot, expected ^ UINT64_C(0x1234567800000000)));
        get_private_data[0](g_dev, type, handle, private_slot, &value);
        REQUIRE(value == (expected ^ UINT64_C(0x1234567800000000)));
        CHECK(set_private_data[0](g_dev, type, handle, private_slot, 0));
        get_private_data[1](g_dev, type, handle, private_slot, &value);
        REQUIRE(value == 0);
        CHECK(set_private_data[1](g_dev, type, handle, private_slot, expected));
    }
}
static void annotate_auxiliary_objects(void) {
    if (!debug_names_mode && !private_data_mode) return;
    VkQueryPool pool;
    const VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                        .queryCount = 1};
    CHECK(vkCreateQueryPool(g_dev, &info, NULL, &pool));
    annotate_object(VK_OBJECT_TYPE_QUERY_POOL, OBJECT_HANDLE(pool), "public compacted-size query pool");
    vkDestroyQueryPool(g_dev, pool, NULL);
    PFN_vkCreateDeferredOperationKHR create = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateDeferredOperationKHR");
    PFN_vkDestroyDeferredOperationKHR destroy = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyDeferredOperationKHR");
    REQUIRE(create && destroy);
    if (g_fail) return;
    VkDeferredOperationKHR operation;
    CHECK(create(g_dev, NULL, &operation));
    annotate_object(VK_OBJECT_TYPE_DEFERRED_OPERATION_KHR, OBJECT_HANDLE(operation), "public deferred operation");
    destroy(g_dev, operation, NULL);
}
static void bind_compute_descriptors(VkCommandBuffer command, VkPipelineLayout layout, VkPipelineLayout ordinary_layout,
                                     VkDescriptorSet ordinary) {
    if (!descriptor_mode) {
        bind_sets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, ordinary);
        return;
    }
    // This ordinary set addresses the other output. It must be superseded by
    // the later push/offset operation, including during private restoration.
    bind_sets(command, VK_PIPELINE_BIND_POINT_COMPUTE, ordinary_layout, ordinary);
    unsigned sequence = descriptor_calls++;
    unsigned alias = sequence & 1;
    bool use_inline = maintenance6_mode && dynamic_layout && (sequence & 1);
    if (descriptor_mode == 3) {
        bind_descriptor_buffers(command, 1, &descriptor_binding);
        uint32_t index = 0;
        VkDeviceSize offset = 0;
        if (maintenance6_mode) {
            VkSetDescriptorBufferOffsetsInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SET_DESCRIPTOR_BUFFER_OFFSETS_INFO_EXT,
                                                        .pNext = use_inline ? compute_inline_layout : NULL,
                                                        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                                        .layout = use_inline ? VK_NULL_HANDLE : layout,
                                                        .setCount = 1,
                                                        .pBufferIndices = &index,
                                                        .pOffsets = &offset};
            descriptor_offsets2(command, &info);
        } else
            descriptor_offsets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &index, &offset);
        // Both the public replay journal and RTP's restore history must own
        // these arrays after the caller clobbers them.
        clobber_input(&offset, sizeof(offset));
        clobber_input(&index, sizeof(index));
    } else if (descriptor_mode == 2) {
        struct template_payload {
            uint64_t unused[3];
            VkDescriptorBufferInfo value;
        } payload = {{17, 29, 41}, compute_output};
        VkDescriptorUpdateTemplateEntry entry = {.dstBinding = 0,
                                                 .descriptorCount = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                 .offset = offsetof(struct template_payload, value),
                                                 .stride = sizeof(payload)};
        VkDescriptorUpdateTemplateCreateInfo create = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
                                                       .descriptorUpdateEntryCount = 1,
                                                       .pDescriptorUpdateEntries = &entry,
                                                       .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR,
                                                       .pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE,
                                                       .pipelineLayout = layout,
                                                       .set = 0};
        VkDescriptorUpdateTemplate update;
        CHECK(vkCreateDescriptorUpdateTemplate(g_dev, &create, NULL, &update));
        annotate_object(VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, OBJECT_HANDLE(update), "public descriptor template");
        if (maintenance6_mode) {
            VkPushDescriptorSetWithTemplateInfo info = {.sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO,
                                                        .pNext = use_inline ? compute_inline_layout : NULL,
                                                        .descriptorUpdateTemplate = update,
                                                        .layout = use_inline ? VK_NULL_HANDLE : layout,
                                                        .set = 0,
                                                        .pData = &payload};
            push_template2[alias](command, &info);
        } else
            push_template[alias](command, update, layout, 0, &payload);
        clobber_input(&payload, sizeof(payload));
        // Replay must own the values independently of the caller's payload
        // storage and the template handle, which is consumed while recording.
        vkDestroyDescriptorUpdateTemplate(g_dev, update, NULL);
    } else {
        VkDescriptorBufferInfo value = compute_output;
        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstBinding = 0,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &value};
        if (maintenance6_mode) {
            VkPushDescriptorSetInfo info = {.sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO,
                                            .pNext = use_inline ? compute_inline_layout : NULL,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .layout = use_inline ? VK_NULL_HANDLE : layout,
                                            .set = 0,
                                            .descriptorWriteCount = 1,
                                            .pDescriptorWrites = &write};
            push_descriptors2[alias](command, &info);
        } else
            push_descriptors[alias](command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &write);
        clobber_input(&value, sizeof(value));
    }
}
#include "rtp_nested_fixture.h"
#include "rtp_physical_source_fixture.h"
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[3], "--physical-source")) {
        physical_source_mode = vulkan11_mode = true;
        return run_physical_source_fixture(argv[1], argv[2]);
    }
    if (argc == 5 && !strcmp(argv[4], "--nested-trace")) {
        vulkan11_mode = true;
        return run_nested_trace_fixture(argv[1], argv[2], argv[3]);
    }
    capability_harness = argc == 6 && !strcmp(argv[5], "--capability-harness");
    maintenance6_mode = argc == 6 && !strcmp(argv[5], "--maintenance6");
    shader_object_mode = argc == 6 && !strcmp(argv[5], "--shader-object");
    vulkan11_mode = argc == 6 && !strcmp(argv[5], "--vulkan11");
    debug_names_mode = argc == 6 && !strcmp(argv[5], "--push-template-debug");
    private_data_mode = argc == 6 && !strcmp(argv[5], "--push-template-private-data");
    derivatives_mode = argc == 6 && !strcmp(argv[5], "--derivatives");
    library_mode = argc == 6 && !strcmp(argv[5], "--libraries") ? 1 : argc == 6 && !strcmp(argv[5], "--library-stages") ? 2 : 0;
    if (argc == 6) {
        if (!strcmp(argv[5], "--push-descriptors") || !strcmp(argv[5], "--push-descriptors-maintenance6")) descriptor_mode = 1;
        if (!strcmp(argv[5], "--push-template") || !strcmp(argv[5], "--push-template-maintenance6")) descriptor_mode = 2;
        if (debug_names_mode || private_data_mode) descriptor_mode = 2;
        if (!strcmp(argv[5], "--descriptor-buffer") || !strcmp(argv[5], "--descriptor-buffer-maintenance6")) descriptor_mode = 3;
        if (descriptor_mode && strstr(argv[5], "maintenance6")) maintenance6_mode = true;
    }
    separate_compute = shader_object_mode || descriptor_mode;
    if (argc != 5 && !capability_harness && !maintenance6_mode && !shader_object_mode && !derivatives_mode && !library_mode &&
        !descriptor_mode && !vulkan11_mode) {
        fprintf(
            stderr,
            "usage: %s rgen.spv miss.spv chit.spv compute.spv "
            "[--capability-harness|--maintenance6|--shader-object|--derivatives|--libraries|--library-stages|"
            "--push-descriptors[-maintenance6]|--push-template[-maintenance6]|--push-template-debug|--push-template-private-data|"
            "--descriptor-buffer[-maintenance6]|--vulkan11]\n",
            argv[0]);
        return 1;
    }
    if (vulkan11_mode) {
        for (unsigned i = 1; i <= 4; ++i) {
            size_t count;
            uint32_t *code = read_spv(argv[i], &count);
            bool compatible = count >= 5 && code[0] == 0x07230203 && code[1] == 0x00010400;
            free(code);
            if (!compatible) {
                fprintf(stderr, "--vulkan11 requires separately compiled SPIR-V1.4 input: %s\n", argv[i]);
                return 1;
            }
        }
    }
    init_vulkan();
    if (debug_names_mode) {
        debug_name = (void *)vkGetDeviceProcAddr(g_dev, "vkSetDebugUtilsObjectNameEXT");
        debug_tag = (void *)vkGetDeviceProcAddr(g_dev, "vkSetDebugUtilsObjectTagEXT");
        REQUIRE(debug_name && debug_tag);
    }
    if (private_data_mode) {
        create_private_slot[1] = private_extension ? (void *)vkGetDeviceProcAddr(g_dev, "vkCreatePrivateDataSlotEXT")
                                                   : (void *)vkGetDeviceProcAddr(g_dev, "vkCreatePrivateDataSlot");
        destroy_private_slot[1] = private_extension ? (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyPrivateDataSlotEXT")
                                                    : (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyPrivateDataSlot");
        set_private_data[1] = private_extension ? (void *)vkGetDeviceProcAddr(g_dev, "vkSetPrivateDataEXT")
                                                : (void *)vkGetDeviceProcAddr(g_dev, "vkSetPrivateData");
        get_private_data[1] = private_extension ? (void *)vkGetDeviceProcAddr(g_dev, "vkGetPrivateDataEXT")
                                                : (void *)vkGetDeviceProcAddr(g_dev, "vkGetPrivateData");
        create_private_slot[0] = private_core ? (PFN_vkCreatePrivateDataSlot)vkGetDeviceProcAddr(g_dev, "vkCreatePrivateDataSlot")
                                              : create_private_slot[1];
        destroy_private_slot[0] = private_core
                                      ? (PFN_vkDestroyPrivateDataSlot)vkGetDeviceProcAddr(g_dev, "vkDestroyPrivateDataSlot")
                                      : destroy_private_slot[1];
        set_private_data[0] =
            private_core ? (PFN_vkSetPrivateData)vkGetDeviceProcAddr(g_dev, "vkSetPrivateData") : set_private_data[1];
        get_private_data[0] =
            private_core ? (PFN_vkGetPrivateData)vkGetDeviceProcAddr(g_dev, "vkGetPrivateData") : get_private_data[1];
        REQUIRE(create_private_slot[0] && create_private_slot[1] && destroy_private_slot[0] && destroy_private_slot[1] &&
                set_private_data[0] && set_private_data[1] && get_private_data[0] && get_private_data[1]);
        if (g_fail) return 1;
        const VkPrivateDataSlotCreateInfo slot_info = {.sType = VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO};
        CHECK(create_private_slot[0](g_dev, &slot_info, NULL, &private_slot));
    }
    if (descriptor_mode == 1) {
        push_descriptors[1] = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdPushDescriptorSetKHR");
        push_descriptors[0] = descriptor_core ? (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(g_dev, "vkCmdPushDescriptorSet")
                                              : push_descriptors[1];
        if (maintenance6_mode) {
            push_descriptors2[1] = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdPushDescriptorSet2KHR");
            push_descriptors2[0] = descriptor_core
                                       ? (PFN_vkCmdPushDescriptorSet2)vkGetDeviceProcAddr(g_dev, "vkCmdPushDescriptorSet2")
                                       : push_descriptors2[1];
            REQUIRE(push_descriptors2[0] && push_descriptors2[1]);
        }
        REQUIRE(push_descriptors[0] && push_descriptors[1]);
    } else if (descriptor_mode == 2) {
        push_template[1] = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdPushDescriptorSetWithTemplateKHR");
        push_template[0] = descriptor_core ? (PFN_vkCmdPushDescriptorSetWithTemplateKHR)vkGetDeviceProcAddr(
                                                 g_dev, "vkCmdPushDescriptorSetWithTemplate")
                                           : push_template[1];
        if (maintenance6_mode) {
            push_template2[1] = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdPushDescriptorSetWithTemplate2KHR");
            push_template2[0] = descriptor_core ? (PFN_vkCmdPushDescriptorSetWithTemplate2)vkGetDeviceProcAddr(
                                                      g_dev, "vkCmdPushDescriptorSetWithTemplate2")
                                                : push_template2[1];
            REQUIRE(push_template2[0] && push_template2[1]);
        }
        REQUIRE(push_template[0] && push_template[1]);
    } else if (descriptor_mode == 3) {
        bind_descriptor_buffers = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBindDescriptorBuffersEXT");
        descriptor_offsets = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdSetDescriptorBufferOffsetsEXT");
        descriptor_offsets2 = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdSetDescriptorBufferOffsets2EXT");
        REQUIRE(bind_descriptor_buffers && descriptor_offsets && (!maintenance6_mode || descriptor_offsets2));
    }
    if (g_fail) return 1;
    annotate_auxiliary_objects();
    if (shader_object_mode) {
        create_shaders = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
        destroy_shader = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
        bind_shaders = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBindShadersEXT");
        REQUIRE(create_shaders && destroy_shader && bind_shaders);
        if (g_fail) return 1;
    }
    if (maintenance6_mode) {
        bind2[1] = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBindDescriptorSets2KHR");
        bind2[0] =
            descriptor_core ? (PFN_vkCmdBindDescriptorSets2)vkGetDeviceProcAddr(g_dev, "vkCmdBindDescriptorSets2") : bind2[1];
        push2[1] = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdPushConstants2KHR");
        push2[0] = descriptor_core ? (PFN_vkCmdPushConstants2)vkGetDeviceProcAddr(g_dev, "vkCmdPushConstants2") : push2[1];
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
    annotate_object(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, OBJECT_HANDLE(blas.handle), "public BLAS");
    annotate_object(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, OBJECT_HANDLE(tlas.handle), "public TLAS");
    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(g_pdev, &device_properties);
    VkDeviceSize output_stride = device_properties.limits.minStorageBufferOffsetAlignment;
    if (output_stride < 8 * 16) output_stride = 8 * 16;
    output_stride = (8 * 16 + output_stride - 1) / output_stride * output_stride;
    buf_create(output_stride + 8 * 16,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (descriptor_mode == 3 ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0), host,
               &output);
    compute_output = (VkDescriptorBufferInfo){output.buf, 0, 8 * 16};
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
                                      .pushConstantRangeCount = separate_compute ? 3 : 4,
                                      .pPushConstantRanges = ranges};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(g_dev, &lci, NULL, &layout));
    VkPipelineLayout compute_layout = layout;
    VkDescriptorSetLayout compute_dsl = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo ordinary = lci;
    if (separate_compute) {
        ordinary.pushConstantRangeCount = 1;
        ordinary.pPushConstantRanges = &ranges[3];
        if (descriptor_mode) {
            VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
            VkDescriptorSetLayoutCreateInfo create = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                      .flags = descriptor_mode == 3
                                                                   ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
                                                                   : VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                                      .bindingCount = 1,
                                                      .pBindings = &binding};
            CHECK(vkCreateDescriptorSetLayout(g_dev, &create, NULL, &compute_dsl));
            ordinary.setLayoutCount = 1;
            ordinary.pSetLayouts = &compute_dsl;
        }
        CHECK(vkCreatePipelineLayout(g_dev, &ordinary, NULL, &compute_layout));
    }
    compute_public_layout = compute_layout;
    compute_inline_layout = &ordinary;
    struct gpu_buf descriptor_storage = {0};
    if (descriptor_mode == 3) {
        PFN_vkGetDescriptorSetLayoutSizeEXT layout_size = (void *)vkGetDeviceProcAddr(g_dev, "vkGetDescriptorSetLayoutSizeEXT");
        PFN_vkGetDescriptorSetLayoutBindingOffsetEXT binding_offset =
            (void *)vkGetDeviceProcAddr(g_dev, "vkGetDescriptorSetLayoutBindingOffsetEXT");
        PFN_vkGetDescriptorEXT get_descriptor = (void *)vkGetDeviceProcAddr(g_dev, "vkGetDescriptorEXT");
        REQUIRE(layout_size && binding_offset && get_descriptor);
        if (g_fail) return 1;
        VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_properties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                  .pNext = &descriptor_properties};
        vkGetPhysicalDeviceProperties2(g_pdev, &properties);
        VkDeviceSize bytes, offset;
        layout_size(g_dev, compute_dsl, &bytes);
        binding_offset(g_dev, compute_dsl, 0, &offset);
        VkDeviceSize alignment = descriptor_properties.descriptorBufferOffsetAlignment;
        REQUIRE(alignment && bytes && descriptor_properties.storageBufferDescriptorSize);
        if (g_fail) return 1;
        descriptor_record_stride = (bytes + alignment - 1) / alignment * alignment;
        buf_create(descriptor_record_stride * 2,
                   VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host,
                   &descriptor_storage);
        descriptor_binding = (VkDescriptorBufferBindingInfoEXT){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
                                                                .address = descriptor_storage.addr,
                                                                .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT};
        for (unsigned i = 0; i < 2; ++i) {
            VkDescriptorAddressInfoEXT address = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
                                                  .address = output.addr + i * output_stride,
                                                  .range = 8 * 16};
            VkDescriptorGetInfoEXT descriptor = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                                                 .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                 .data.pStorageBuffer = &address};
            get_descriptor(g_dev, &descriptor, descriptor_properties.storageBufferDescriptorSize,
                           (uint8_t *)descriptor_storage.mapped + i * descriptor_record_stride + offset);
        }
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
    VkPipeline derivative_indexed = VK_NULL_HANDLE, derivative_handle = VK_NULL_HANDLE;
    VkPipeline retained_libraries[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (library_mode) {
        VkRayTracingPipelineInterfaceCreateInfoKHR library_interface = {
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR,
            .maxPipelineRayPayloadSize = 16,
            .maxPipelineRayHitAttributeSize = 8};
        VkRayTracingPipelineCreateInfoKHR lib = rtci;
        lib.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
        lib.pLibraryInterface = &library_interface;
        lib.pDynamicState = NULL;
        VkPipeline *libraries = retained_libraries;
        VkPipelineLibraryCreateInfoKHR links = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 1, .pLibraries = &libraries[0]};
        if (library_mode == 2) {
            // Groups in the final pipeline may reference stages supplied only
            // by a library that has no groups of its own.
            lib.groupCount = 0;
            lib.pGroups = NULL;
            CHECK(create(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &lib, NULL, &libraries[0]));
            rtci.stageCount = 0;
            rtci.pStages = NULL;
        } else {
            VkRayTracingShaderGroupCreateInfoKHR hit = groups[2];
            hit.closestHitShader = 0;
            lib.stageCount = lib.groupCount = 1;
            lib.pStages = &stages[2];
            lib.pGroups = &hit;
            CHECK(create(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &lib, NULL, &libraries[0]));
            // Empty intermediate libraries preserve the complete nested lists.
            lib.stageCount = lib.groupCount = 0;
            lib.pStages = NULL;
            lib.pGroups = NULL;
            lib.pLibraryInfo = &links;
            CHECK(create(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &lib, NULL, &libraries[1]));
            links.pLibraries = &libraries[1];
            lib.stageCount = lib.groupCount = 2;
            lib.pStages = stages;
            lib.pGroups = groups;
            CHECK(create(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &lib, NULL, &libraries[2]));
            links.pLibraries = &libraries[2];
            rtci.stageCount = rtci.groupCount = 0;
            rtci.pStages = NULL;
            rtci.pGroups = NULL;
        }
        rtci.pLibraryInfo = &links;
        rtci.pLibraryInterface = &library_interface;
        CHECK(create(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtci, NULL, &rt));
        // Library lifetimes follow all linked pipelines, including later
        // dispatches; retain the complete chain through execution.
    } else if (derivatives_mode) {
        VkRayTracingPipelineCreateInfoKHR batch[2] = {rtci, rtci};
        batch[0].flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT | VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        batch[0].basePipelineIndex = -1;
        batch[1].flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
        batch[1].basePipelineIndex = 0;
        VkPipelineCacheCreateInfo cache_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        VkPipelineCache cache;
        CHECK(vkCreatePipelineCache(g_dev, &cache_info, NULL, &cache));
        VkPipeline pipelines[2];
        CHECK(create(g_dev, VK_NULL_HANDLE, cache, 2, batch, NULL, pipelines));
        derivative_indexed = pipelines[1];
        VkRayTracingPipelineCreateInfoKHR child = rtci;
        child.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        child.basePipelineHandle = derivative_indexed;
        child.basePipelineIndex = -1;
        CHECK(create(g_dev, VK_NULL_HANDLE, cache, 1, &child, NULL, &derivative_handle));
        // The layer must not retain the application cache for later lazy stack
        // variants, which are first created while recording the traces below.
        vkDestroyPipelineCache(g_dev, cache, NULL);
        // Both children must remain independently usable after their original
        // base is destroyed, before any trace is recorded or submitted.
        vkDestroyPipeline(g_dev, pipelines[0], NULL);
        rt = derivative_indexed;
    } else {
        CHECK(create(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtci, NULL, &rt));
    }
    VkComputePipelineCreateInfo compute_info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                .flags = descriptor_mode == 3 ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0,
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
    annotate_object(VK_OBJECT_TYPE_PIPELINE, OBJECT_HANDLE(rt), "public RTP pipeline");
    annotate_object(VK_OBJECT_TYPE_PIPELINE, OBJECT_HANDLE(compute), "native compute pipeline");
    for (unsigned i = 0; i < 4; ++i) {
        annotate_object(VK_OBJECT_TYPE_SHADER_MODULE, OBJECT_HANDLE(modules[i]), "public shader module");
        vkDestroyShaderModule(g_dev, modules[i], NULL);
    }
    uint8_t handle_bytes[3 * 32];
    CHECK(handles(g_dev, rt, 0, 3, sizeof(handle_bytes), handle_bytes));
    for (unsigned i = 0; i < 3; ++i) memcpy((uint8_t *)sbt.mapped + i * 64, handle_bytes + i * 32, 32);
    VkDeviceSize a = stack(g_dev, rt, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR),
                 b = stack(g_dev, rt, 1, VK_SHADER_GROUP_SHADER_GENERAL_KHR),
                 c = stack(g_dev, rt, 2, VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR);
    VkDeviceSize budget = a + (b > c ? b : c);
    REQUIRE(budget <= UINT32_MAX);
    // A native implementation can report zero stack use for these shaders.
    // A small positive allocation remains a valid conservative stack budget.
    if (!budget) budget = 16;
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = g_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer command;
    unsigned trace_calls = 0;
    CHECK(vkAllocateCommandBuffers(g_dev, &ca, &command));
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        if (derivatives_mode && iteration) {
            rt = derivative_handle;
            CHECK(handles(g_dev, rt, 0, 3, sizeof(handle_bytes), handle_bytes));
            for (unsigned i = 0; i < 3; ++i) memcpy((uint8_t *)sbt.mapped + i * 64, handle_bytes + i * 32, 32);
            a = stack(g_dev, rt, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
            b = stack(g_dev, rt, 1, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
            c = stack(g_dev, rt, 2, VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR);
            budget = a + (b > c ? b : c);
            REQUIRE(budget <= UINT32_MAX);
            if (!budget) budget = 16;
        }
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
        bind_compute_descriptors(command, separate_compute ? compute_layout : layout, layout, sets[descriptor_mode ? 1 : 0]);
        uint32_t ordinary = 123 + iteration;
        push_values(command, separate_compute ? compute_layout : layout, VK_SHADER_STAGE_COMPUTE_BIT, 4, 4, &ordinary);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt);
        bind_sets(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, sets[0]);
        uint32_t values[] = {3 + iteration, 7 + iteration, 11 + iteration};
        const uint32_t push_offsets[] = {0, 8, 12};
        for (unsigned i = 0; i < 3; ++i) {
            push_values(command, layout, kinds[i] | (separate_compute ? 0 : VK_SHADER_STAGE_COMPUTE_BIT), push_offsets[i], 4,
                        &values[i]);
            if (separate_compute) push_values(command, compute_layout, VK_SHADER_STAGE_COMPUTE_BIT, push_offsets[i], 4, &values[i]);
        }
        const uint32_t tail = 91 + iteration;
        push_values(command, separate_compute ? compute_layout : layout, VK_SHADER_STAGE_COMPUTE_BIT, 124, 4, &tail);
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
        push_values(command, layout, VK_SHADER_STAGE_RAYGEN_BIT_KHR | (separate_compute ? 0 : VK_SHADER_STAGE_COMPUTE_BIT), 0, 4,
                    &values[0]);
        if (separate_compute) push_values(command, compute_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &values[0]);
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
                push_values(concurrent[target], layout, kinds[i] | (separate_compute ? 0 : VK_SHADER_STAGE_COMPUTE_BIT), offsets[i],
                            4, &values[i]);
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
    if (private_data_mode) {
        const VkPrivateDataSlot previous = private_slot;
        destroy_private_slot[0](g_dev, private_slot, NULL);
        const VkPrivateDataSlotCreateInfo slot_info = {.sType = VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO};
        CHECK(create_private_slot[1](g_dev, &slot_info, NULL, &private_slot));
        annotate_object(VK_OBJECT_TYPE_PIPELINE, OBJECT_HANDLE(rt), "RTP pipeline with replacement private slot");
        annotate_object(VK_OBJECT_TYPE_PIPELINE, OBJECT_HANDLE(compute), "native compute with replacement private slot");
        annotate_object(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, OBJECT_HANDLE(blas.handle),
                        "BLAS with replacement private slot");
        printf("Private-data slot numeric reuse observed=%u\n", previous == private_slot);
        destroy_private_slot[1](g_dev, private_slot, NULL);
        private_slot = VK_NULL_HANDLE;
    }
    vkDestroyPipeline(g_dev, rt, NULL);
    for (unsigned i = 3; i-- > 0;)
        if (retained_libraries[i]) vkDestroyPipeline(g_dev, retained_libraries[i], NULL);
    if (derivative_indexed) vkDestroyPipeline(g_dev, derivative_indexed, NULL);
    vkDestroyPipeline(g_dev, compute, NULL);
    if (compute_shader) destroy_shader(g_dev, compute_shader, NULL);
    if (separate_compute) vkDestroyPipelineLayout(g_dev, compute_layout, NULL);
    if (compute_dsl) vkDestroyDescriptorSetLayout(g_dev, compute_dsl, NULL);
    if (descriptor_mode == 3) buf_destroy(&descriptor_storage);
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
    if (descriptor_mode)
        printf(
            "Compute descriptors mode=%u maintenance6=%u dynamic-layout=%u core-alias=%u; immediate Dispatch after Trace without "
            "descriptor rebinding: %s\n",
            descriptor_mode, maintenance6_mode, dynamic_layout, descriptor_core, g_fail ? "FAIL" : "PASS");
    if (debug_names_mode)
        printf("Debug-utils name/tag on virtual AS/RTP/shader/template/query/deferred and native compute objects: %s\n",
               g_fail ? "FAIL" : "PASS");
    if (private_data_mode)
        printf("PrivateData on virtual AS/RTP/shader/template/query/deferred and native compute objects; native slots: %s\n",
               g_fail ? "FAIL" : "PASS");
    if (vulkan11_mode) printf("Vulkan1.1 application with genuine SPIR-V1.4 shader inputs: %s\n", g_fail ? "FAIL" : "PASS");
    if (derivatives_mode)
        printf("RTP base/index/handle derivatives, optimization flags and destroyed base before trace: %s\n",
               g_fail ? "FAIL" : "PASS");
    if (library_mode)
        printf("RTP linked library mode=%u, nested definitions and later stack variants: %s\n", library_mode,
               g_fail ? "FAIL" : "PASS");
    printf("RTP API test mode=%s %s\n", advertised ? "advertised" : "explicit capability harness", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
