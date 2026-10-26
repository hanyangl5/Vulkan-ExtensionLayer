/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_layer.h"

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
#define HANDLE(type, value) ((type)(uintptr_t)(value))
#define REQUIRE(x)                                               \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            ++failures;                                          \
        }                                                        \
    } while (0)
static unsigned failures, enumeration_calls, feature_queries;
static const VkPhysicalDevice physical = HANDLE(VkPhysicalDevice, 7);
static VkBool32 native_rt;
static bool ignore_rt_features;
static bool ignore_per_stage_features;
static bool hide_pipeline_base;
static VkResult enumeration_error;
static bool grow_once, incomplete_fill;
static bool hide_deferred;
static const char *last_layer;
static const char *native_names[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                     VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                     VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                                     VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                     "VK_KHR_ray_tracing_maintenance1",
                                     "VK_KHR_ray_tracing_position_fetch",
                                     "VK_KHR_ray_tracing_pipeline_unsupported",
                                     "VK_EXT_pipeline_library_group_handles",
                                     "VK_KHR_pipeline_library",
                                     "VK_EXT_ray_tracing_invocation_reorder",
                                     "VK_HUAWEI_invocation_mask",
                                     "VK_EXT_opacity_micromap",
                                     "VK_ARM_pipeline_opacity_micromap",
                                     "VK_NV_cluster_acceleration_structure",
                                     "VK_NV_partitioned_acceleration_structure",
                                     "VK_AMDX_dense_geometry_format",
                                     "VK_KHR_opacity_micromap",
                                     VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                                     "VK_KHR_swapchain",
                                     "VK_EXT_robustness2"};
static const char *visible_names[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                      VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                      VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                                      VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                      "VK_KHR_pipeline_library",
                                      "VK_KHR_swapchain",
                                      "VK_EXT_robustness2"};

static VKAPI_ATTR VkResult VKAPI_CALL mock_enumerate(VkPhysicalDevice p, const char *layer, uint32_t *count,
                                                     VkExtensionProperties *properties) {
    REQUIRE(p == physical);
    ++enumeration_calls;
    last_layer = layer;
    if (enumeration_error != VK_SUCCESS) return enumeration_error;
    if (layer) {
        if (strcmp(layer, "unrelated_layer")) return VK_ERROR_LAYER_NOT_PRESENT;
        if (!properties) {
            *count = 1;
            return VK_SUCCESS;
        }
        if (!*count) return VK_INCOMPLETE;
        *count = 1;
        strcpy(properties[0].extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        properties[0].specVersion = 123;
        return VK_SUCCESS;
    }
    uint32_t total = (uint32_t)VKRT_ARRAY_SIZE(native_names);
    uint32_t start = native_rt ? 0 : 3;
    total -= start;
    if (hide_deferred) --total;
    if (!properties) {
        *count = total;
        return VK_SUCCESS;
    }
    if (grow_once || incomplete_fill) {
        grow_once = false;
        *count = 0;
        return VK_INCOMPLETE;
    }
    uint32_t capacity = *count;
    *count = capacity < total ? capacity : total;
    for (uint32_t i = start, out = 0; out < *count; ++i) {
        if (hide_deferred && !strcmp(native_names[i], VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) continue;
        strcpy(properties[out].extensionName, native_names[i]);
        properties[out++].specVersion = 100 + i;
    }
    return capacity < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mock_features(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *features) {
    REQUIRE(p == physical);
    ++feature_queries;
    features->features.shaderInt64 = features->features.geometryShader = VK_TRUE;
    for (VkBaseOutStructure *next = features->pNext; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR) {
            VkPhysicalDeviceAccelerationStructureFeaturesKHR *as = (void *)next;
            if (!ignore_rt_features) as->accelerationStructure = native_rt;
            as->accelerationStructureCaptureReplay = as->accelerationStructureIndirectBuild = VK_TRUE;
            as->accelerationStructureHostCommands = as->descriptorBindingAccelerationStructureUpdateAfterBind = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR) {
            if (!ignore_rt_features) ((VkPhysicalDeviceRayQueryFeaturesKHR *)next)->rayQuery = native_rt;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR) {
            VkPhysicalDeviceRayTracingPipelineFeaturesKHR *rtp = (void *)next;
            if (!ignore_rt_features) rtp->rayTracingPipeline = hide_pipeline_base ? VK_FALSE : native_rt;
            rtp->rayTracingPipelineShaderGroupHandleCaptureReplay = VK_TRUE;
            rtp->rayTracingPipelineShaderGroupHandleCaptureReplayMixed = rtp->rayTracingPipelineTraceRaysIndirect = VK_TRUE;
            rtp->rayTraversalPrimitiveCulling = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            VkPhysicalDeviceDescriptorIndexingFeatures *indexing = (void *)next;
            indexing->runtimeDescriptorArray = indexing->descriptorBindingPartiallyBound = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV) {
            VkPhysicalDevicePerStageDescriptorSetFeaturesNV *per_stage = (void *)next;
            if (!ignore_per_stage_features) per_stage->perStageDescriptorSet = per_stage->dynamicPipelineLayout = VK_TRUE;
        }
#ifdef VK_EXT_pipeline_library_group_handles
        else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_LIBRARY_GROUP_HANDLES_FEATURES_EXT) {
            ((VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT *)next)->pipelineLibraryGroupHandles = VK_TRUE;
        }
#endif
        else
            REQUIRE(next->sType == VK_STRUCTURE_TYPE_MAX_ENUM);
    }
}

static void enumeration(PFN_vkEnumerateDeviceExtensionProperties enumerate) {
    for (unsigned rt = 0; rt < 2; ++rt) {
        native_rt = rt;
        uint32_t count = 99, start = rt ? 0 : 3;
        const uint32_t visible = (uint32_t)VKRT_ARRAY_SIZE(visible_names) - start;
        REQUIRE(enumerate(physical, NULL, &count, NULL) == VK_SUCCESS && count == visible);
        for (uint32_t capacity = 0; capacity <= visible + 2; ++capacity) {
            VkExtensionProperties output[10];
            memset(output, 0xa5, sizeof(output));
            VkExtensionProperties saved[10];
            memcpy(saved, output, sizeof(saved));
            count = capacity;
            VkResult result = enumerate(physical, NULL, &count, output);
            REQUIRE(result == (capacity < visible ? VK_INCOMPLETE : VK_SUCCESS));
            REQUIRE(count == (capacity < visible ? capacity : visible));
            for (uint32_t i = 0; i < count; ++i) {
                REQUIRE(!strcmp(output[i].extensionName, visible_names[i + start]));
                uint32_t native_index = 0;
                while (strcmp(native_names[native_index], visible_names[i + start])) ++native_index;
                REQUIRE(output[i].specVersion == (!strcmp(output[i].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)
                                                      ? VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION
                                                      : 100 + native_index));
            }
            REQUIRE(!memcmp(output + count, saved + count, (10 - count) * sizeof(output[0])));
        }
    }
    native_rt = VK_TRUE;
    grow_once = true;
    uint32_t count = 0;
    REQUIRE(enumerate(physical, NULL, &count, NULL) == VK_SUCCESS && count == VKRT_ARRAY_SIZE(visible_names));
    incomplete_fill = true;
    REQUIRE(enumerate(physical, NULL, &count, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    incomplete_fill = false;
    enumeration_error = VK_ERROR_DEVICE_LOST;
    REQUIRE(enumerate(physical, NULL, &count, NULL) == VK_ERROR_DEVICE_LOST);
    enumeration_error = VK_SUCCESS;

    /* A named different layer is passed through without filtering its API. */
    VkExtensionProperties property;
    count = 1;
    REQUIRE(enumerate(physical, "unrelated_layer", &count, &property) == VK_SUCCESS && count == 1);
    REQUIRE(!strcmp(last_layer, "unrelated_layer"));
    REQUIRE(!strcmp(property.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) && property.specVersion == 123);
    REQUIRE(enumerate(physical, "missing_layer", &count, NULL) == VK_ERROR_LAYER_NOT_PRESENT);
    unsigned before = enumeration_calls;
    count = 42;
    REQUIRE(enumerate(VK_NULL_HANDLE, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY", &count, NULL) == VK_SUCCESS && count == 1);
    count = 0;
    REQUIRE(enumerate(physical, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY", &count, &property) == VK_INCOMPLETE && count == 0);
    count = 1;
    REQUIRE(enumerate(physical, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY", &count, &property) == VK_SUCCESS && count == 1);
    REQUIRE(!strcmp(property.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME));
    REQUIRE(property.specVersion == VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION);
    REQUIRE(enumeration_calls == before);
    /* A driver with no deferred extension still exposes the layer's complete
     * synchronous implementation, appended once to its native extensions. */
    hide_deferred = true;
    for (unsigned rt = 0; rt < 2; ++rt) {
        native_rt = rt;
        VkExtensionProperties output[10];
        count = 10;
        REQUIRE(enumerate(physical, NULL, &count, output) == VK_SUCCESS);
        REQUIRE(count == VKRT_ARRAY_SIZE(visible_names) - (rt ? 0 : 3));
        REQUIRE(!strcmp(output[count - 1].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME));
        REQUIRE(output[count - 1].specVersion == VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION);
    }
    hide_deferred = false;
}

static void features(PFN_vkGetPhysicalDeviceFeatures2 query, struct vkrt_instance_data *instance) {
    struct {
        VkStructureType sType;
        void *pNext;
        uint64_t sentinel;
    } suffix = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM, .sentinel = UINT64_C(0xabcdef0123456789)};
    VkPhysicalDevicePerStageDescriptorSetFeaturesNV per_stage = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV, .pNext = &suffix};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .pNext = &per_stage};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &rtp};
    VkPhysicalDeviceRayQueryFeaturesKHR rq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &as};
    VkPhysicalDeviceDescriptorIndexingFeatures indexing = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
                                                           .pNext = &rq};
#ifdef VK_EXT_pipeline_library_group_handles
    VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT groups = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_LIBRARY_GROUP_HANDLES_FEATURES_EXT, .pNext = &indexing};
    void *head = &groups;
#else
    void *head = &indexing;
#endif
    VkPhysicalDeviceFeatures2 core = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = head};
    for (unsigned rt = 0; rt < 2; ++rt) {
        native_rt = rt;
        query(physical, &core);
        REQUIRE(core.features.shaderInt64 && core.features.geometryShader);
        REQUIRE(rq.rayQuery == rt && as.accelerationStructure == rt && rtp.rayTracingPipeline == rt);
        REQUIRE(!as.accelerationStructureCaptureReplay && !as.accelerationStructureIndirectBuild &&
                !as.accelerationStructureHostCommands && !as.descriptorBindingAccelerationStructureUpdateAfterBind);
        REQUIRE(!rtp.rayTracingPipelineShaderGroupHandleCaptureReplay &&
                !rtp.rayTracingPipelineShaderGroupHandleCaptureReplayMixed && !rtp.rayTracingPipelineTraceRaysIndirect &&
                !rtp.rayTraversalPrimitiveCulling);
        REQUIRE(indexing.runtimeDescriptorArray && indexing.descriptorBindingPartiallyBound);
        REQUIRE(core.pNext == head && indexing.pNext == &rq && rq.pNext == &as && as.pNext == &rtp && rtp.pNext == &per_stage &&
                per_stage.pNext == &suffix);
        REQUIRE(!per_stage.perStageDescriptorSet && per_stage.dynamicPipelineLayout);
        REQUIRE(suffix.sentinel == UINT64_C(0xabcdef0123456789));
#ifdef VK_EXT_pipeline_library_group_handles
        REQUIRE(groups.pNext == &indexing && !groups.pipelineLibraryGroupHandles);
#endif
    }
    /* Native AS/RQ support is not evidence of native RTP base support. */
    native_rt = VK_TRUE;
    hide_pipeline_base = true;
    query(physical, &core);
    REQUIRE(rq.rayQuery && as.accelerationStructure && !rtp.rayTracingPipeline);
    hide_pipeline_base = false;
    /* An old no-RT ICD can ignore those structs; stale caller bytes are not
     * native support and must not become an emulated base-feature claim. */
    ignore_rt_features = true;
    ignore_per_stage_features = true;
    rq.rayQuery = as.accelerationStructure = rtp.rayTracingPipeline = VK_TRUE;
    per_stage.perStageDescriptorSet = per_stage.dynamicPipelineLayout = VK_TRUE;
    query(physical, &core);
    REQUIRE(!rq.rayQuery && !as.accelerationStructure && !rtp.rayTracingPipeline);
    REQUIRE(!per_stage.perStageDescriptorSet && !per_stage.dynamicPipelineLayout);
    instance->vtable.GetPhysicalDeviceFeatures2 = NULL;
    rq.rayQuery = as.accelerationStructure = rtp.rayTracingPipeline = VK_TRUE;
    per_stage.perStageDescriptorSet = per_stage.dynamicPipelineLayout = VK_TRUE;
    unsigned before = feature_queries;
    query(physical, &core);
    REQUIRE(!rq.rayQuery && !as.accelerationStructure && !rtp.rayTracingPipeline && before == feature_queries);
    REQUIRE(!per_stage.perStageDescriptorSet && !per_stage.dynamicPipelineLayout);
}

int main(void) {
    struct vkrt_instance_data instance = {0};
    instance.vtable.GetPhysicalDeviceFeatures2 = mock_features;
    instance.vtable.EnumerateDeviceExtensionProperties = mock_enumerate;
    vkrt_map_object(VKRT_HKEY(physical), &instance);
    PFN_vkEnumerateDeviceExtensionProperties enumerate = (void *)vkrt_discovery_proc("vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceFeatures2 query = (void *)vkrt_discovery_proc("vkGetPhysicalDeviceFeatures2");
    REQUIRE(enumerate != NULL && query != NULL);
    REQUIRE(vkrt_discovery_proc("vkGetPhysicalDeviceFeatures2KHR") == (PFN_vkVoidFunction)query);
    REQUIRE(vkrt_discovery_proc("unknown") == NULL);
    enumeration(enumerate);
    features(query, &instance);
    vkrt_unmap_object(VKRT_HKEY(physical));
    printf("Conservative RT discovery: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
