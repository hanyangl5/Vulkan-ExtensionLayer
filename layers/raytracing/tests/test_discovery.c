/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_layer.h"
#include "vkrt_resource_profile.h"

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
static struct vkrt_resource_native native_resources;
static unsigned qualification_queries, legacy_properties_queries, properties2_queries, uuid_queries;

#define INDEX_FIELDS(F)                                     \
    F(maxUpdateAfterBindDescriptorsInAllPools)              \
    F(maxPerStageDescriptorUpdateAfterBindSamplers)         \
    F(maxPerStageDescriptorUpdateAfterBindUniformBuffers)   \
    F(maxPerStageDescriptorUpdateAfterBindStorageBuffers)   \
    F(maxPerStageDescriptorUpdateAfterBindSampledImages)    \
    F(maxPerStageDescriptorUpdateAfterBindStorageImages)    \
    F(maxPerStageDescriptorUpdateAfterBindInputAttachments) \
    F(maxPerStageUpdateAfterBindResources)                  \
    F(maxDescriptorSetUpdateAfterBindSamplers)              \
    F(maxDescriptorSetUpdateAfterBindUniformBuffers)        \
    F(maxDescriptorSetUpdateAfterBindUniformBuffersDynamic) \
    F(maxDescriptorSetUpdateAfterBindStorageBuffers)        \
    F(maxDescriptorSetUpdateAfterBindStorageBuffersDynamic) \
    F(maxDescriptorSetUpdateAfterBindSampledImages)         \
    F(maxDescriptorSetUpdateAfterBindStorageImages)         \
    F(maxDescriptorSetUpdateAfterBindInputAttachments)
#define INLINE_FIELDS(F)                                       \
    F(maxInlineUniformBlockSize)                               \
    F(maxPerStageDescriptorInlineUniformBlocks)                \
    F(maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks) \
    F(maxDescriptorSetInlineUniformBlocks)                     \
    F(maxDescriptorSetUpdateAfterBindInlineUniformBlocks)

static void resource_defaults(void) {
    memset(&native_resources, 0, sizeof(native_resources));
    VkPhysicalDeviceProperties *p = &native_resources.properties;
    p->apiVersion = VK_API_VERSION_1_4;
    p->deviceID = 123;
    p->vendorID = 0x10de;
    p->limits.maxPushConstantsSize = 256;
    p->limits.maxBoundDescriptorSets = 32;
    p->limits.maxMemoryAllocationCount = UINT32_MAX;
    p->limits.maxSamplerAllocationCount = 4000;
    strcpy(p->deviceName, "discovery native device");
    VkPhysicalDeviceLimits *c = &p->limits;
    c->maxPerStageDescriptorSamplers = c->maxPerStageDescriptorUniformBuffers = c->maxPerStageDescriptorStorageBuffers =
        c->maxPerStageDescriptorSampledImages = c->maxPerStageDescriptorStorageImages = c->maxPerStageDescriptorInputAttachments =
            c->maxDescriptorSetSamplers = c->maxDescriptorSetUniformBuffers = c->maxDescriptorSetStorageBuffers =
                c->maxDescriptorSetSampledImages = c->maxDescriptorSetStorageImages = c->maxDescriptorSetInputAttachments = 1048576;
    c->maxPerStageResources = UINT32_MAX;
    c->maxDescriptorSetUniformBuffersDynamic = 15;
    c->maxDescriptorSetStorageBuffersDynamic = 16;
    VkPhysicalDeviceDescriptorIndexingProperties *u = &native_resources.indexing;
#define INITIAL_INDEX(name) u->name = 1048576;
    INDEX_FIELDS(INITIAL_INDEX)
#undef INITIAL_INDEX
    u->maxPerStageUpdateAfterBindResources = u->maxUpdateAfterBindDescriptorsInAllPools = UINT32_MAX;
    u->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 15;
    u->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 16;
    VkPhysicalDeviceDescriptorIndexingFeatures *f = &native_resources.indexing_features;
    f->descriptorBindingSampledImageUpdateAfterBind = f->descriptorBindingUniformBufferUpdateAfterBind =
        f->descriptorBindingStorageBufferUpdateAfterBind = f->descriptorBindingStorageImageUpdateAfterBind =
            f->descriptorBindingUniformTexelBufferUpdateAfterBind = f->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
    native_resources.descriptor_indexing = VK_TRUE;
    VkPhysicalDeviceInlineUniformBlockProperties *i = &native_resources.inline_uniform;
#define INITIAL_INLINE(name) i->name = 32;
    INLINE_FIELDS(INITIAL_INLINE)
#undef INITIAL_INLINE
    i->maxInlineUniformBlockSize = 256;
    i->maxDescriptorSetInlineUniformBlocks = i->maxDescriptorSetUpdateAfterBindInlineUniformBlocks = 64;
    native_resources.inline_features.inlineUniformBlock = VK_TRUE;
    native_resources.inline_features.descriptorBindingInlineUniformBlockUpdateAfterBind = VK_TRUE;
    native_resources.max_inline_uniform_total_size = 3584;
    native_resources.has_maintenance7 = VK_TRUE;
    VkPhysicalDeviceMaintenance7PropertiesKHR *m = &native_resources.maintenance7;
    m->maxDescriptorSetTotalUniformBuffersDynamic = m->maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic = 15;
    m->maxDescriptorSetTotalStorageBuffersDynamic = m->maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic = 16;
    m->maxDescriptorSetTotalBuffersDynamic = m->maxDescriptorSetUpdateAfterBindTotalBuffersDynamic = 31;
}

/* The production component predicate is independently tested with the real
 * planner in test_device_features. This boundary spy deliberately uses that
 * same pure planner, while preserving its transactional output contract. */
bool vkrt_virtual_rt_resource_profile(struct vkrt_instance_data *inst, VkPhysicalDevice p, struct vkrt_resource_profile *profile) {
    REQUIRE(inst == VKRT_FIND(struct vkrt_instance_data, physical) && p == physical);
    ++qualification_queries;
    return vkrt_plan_resource_profile(&native_resources, profile, NULL);
}
static VKAPI_ATTR void VKAPI_CALL mock_properties(VkPhysicalDevice p, VkPhysicalDeviceProperties *properties) {
    REQUIRE(p == physical);
    ++legacy_properties_queries;
    *properties = native_resources.properties;
}
static VKAPI_ATTR void VKAPI_CALL mock_properties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *properties) {
    REQUIRE(p == physical && properties->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    ++properties2_queries;
    properties->properties = native_resources.properties;
    for (VkBaseOutStructure *next = properties->pNext; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES) {
            VkPhysicalDeviceDescriptorIndexingProperties *u = (void *)next;
#define SET_INDEX(name) u->name = native_resources.indexing.name;
            INDEX_FIELDS(SET_INDEX)
#undef SET_INDEX
            u->quadDivergentImplicitLod = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
            VkPhysicalDeviceVulkan12Properties *u = (void *)next;
#define SET_INDEX(name) u->name = native_resources.indexing.name;
            INDEX_FIELDS(SET_INDEX)
#undef SET_INDEX
            u->shaderDenormPreserveFloat32 = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES) {
            VkPhysicalDeviceInlineUniformBlockProperties *i = (void *)next;
#define SET_INLINE(name) i->name = native_resources.inline_uniform.name;
            INLINE_FIELDS(SET_INLINE)
#undef SET_INLINE
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES) {
            VkPhysicalDeviceVulkan13Properties *i = (void *)next;
#define SET_INLINE(name) i->name = native_resources.inline_uniform.name;
            INLINE_FIELDS(SET_INLINE)
#undef SET_INLINE
            i->maxInlineUniformTotalSize = native_resources.max_inline_uniform_total_size;
            i->storageTexelBufferOffsetAlignmentBytes = 128;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR) {
            VkPhysicalDeviceAccelerationStructurePropertiesKHR *a = (void *)next;
            a->maxPerStageDescriptorAccelerationStructures = a->maxDescriptorSetAccelerationStructures =
                a->maxPerStageDescriptorUpdateAfterBindAccelerationStructures =
                    a->maxDescriptorSetUpdateAfterBindAccelerationStructures = 1048576;
            a->maxGeometryCount = 777;
            a->maxInstanceCount = 888;
            a->maxPrimitiveCount = 999;
            a->minAccelerationStructureScratchOffsetAlignment = 256;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR) {
            VkPhysicalDeviceMaintenance7PropertiesKHR *m = (void *)next;
            m->maxDescriptorSetTotalUniformBuffersDynamic = m->maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic = 15;
            m->maxDescriptorSetTotalStorageBuffersDynamic = m->maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic = 16;
            m->maxDescriptorSetTotalBuffersDynamic = m->maxDescriptorSetUpdateAfterBindTotalBuffersDynamic = 31;
            m->robustFragmentShadingRateAttachmentAccess = m->separateDepthStencilAttachmentAccess = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT) {
            VkPhysicalDeviceShaderObjectPropertiesEXT *s = (void *)next;
            memset(s->shaderBinaryUUID, 0x3c, sizeof(s->shaderBinaryUUID));
            s->shaderBinaryVersion = 55;
        } else
            REQUIRE(next->sType == VK_STRUCTURE_TYPE_MAX_ENUM);
    }
}
static VKAPI_ATTR void VKAPI_CALL uuid_properties(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *properties) {
    ++uuid_queries;
    struct vkrt_instance_data *inst = VKRT_FIND(struct vkrt_instance_data, p);
    REQUIRE(inst && inst->vtable.GetPhysicalDeviceProperties2);
    inst->vtable.GetPhysicalDeviceProperties2(p, properties);
    for (VkBaseOutStructure *next = properties->pNext; next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT) {
            VkPhysicalDeviceShaderObjectPropertiesEXT *s = (void *)next;
            for (unsigned i = 0; i < VK_UUID_SIZE; ++i) s->shaderBinaryUUID[i] ^= 0xff;
        }
}
PFN_vkVoidFunction vkrt_shader_object_instance_proc(const char *name) {
    return !strcmp(name, "vkGetPhysicalDeviceProperties2") || !strcmp(name, "vkGetPhysicalDeviceProperties2KHR")
               ? (PFN_vkVoidFunction)uuid_properties
               : NULL;
}
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

static void properties(void) {
    PFN_vkGetPhysicalDeviceProperties legacy = (void *)vkrt_discovery_proc("vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties2 core_query = (void *)vkrt_discovery_proc("vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceProperties2 khr_query = (void *)vkrt_discovery_proc("vkGetPhysicalDeviceProperties2KHR");
    REQUIRE(legacy && core_query && core_query == khr_query);
    /* Native RT/qualified, native RT/budget failure, ordinary no-RT with either
     * budget, and an old ICD which ignores the queried RT feature structures. */
    for (unsigned mode = 0; mode < 5; ++mode) {
        resource_defaults();
        native_rt = mode < 2 || mode == 4;
        ignore_rt_features = mode == 4;
        if (mode == 1 || mode == 3) native_resources.properties.limits.maxPerStageDescriptorStorageBuffers = 16;
        bool apply = mode == 0;
        struct vkrt_resource_profile expected = {0};
        if (apply) REQUIRE(vkrt_plan_resource_profile(&native_resources, &expected, NULL));
        VkPhysicalDeviceProperties ordinary;
        unsigned old_legacy = legacy_properties_queries, old_full = properties2_queries, old_uuid = uuid_queries;
        unsigned old_qualification = qualification_queries;
        legacy(physical, &ordinary);
        REQUIRE(legacy_properties_queries == old_legacy + 1 && properties2_queries == old_full && uuid_queries == old_uuid);
        REQUIRE(qualification_queries == old_qualification + (native_rt && !ignore_rt_features ? 1 : 0));
        if (apply) {
            REQUIRE(ordinary.limits.maxPerStageDescriptorStorageBuffers == 1048560);
            REQUIRE(ordinary.limits.maxDescriptorSetStorageBuffers == 1048544);
            REQUIRE(ordinary.limits.maxDescriptorSetUniformBuffers == 1048561);
        } else
            REQUIRE(!memcmp(&ordinary, &native_resources.properties, sizeof(ordinary)));
        REQUIRE(ordinary.apiVersion == VK_API_VERSION_1_4 && ordinary.deviceID == 123 && ordinary.vendorID == 0x10de);
        REQUIRE(!strcmp(ordinary.deviceName, "discovery native device") && ordinary.limits.maxPushConstantsSize == 256 &&
                ordinary.limits.maxBoundDescriptorSets == 32 && ordinary.limits.maxMemoryAllocationCount == UINT32_MAX &&
                ordinary.limits.maxSamplerAllocationCount == 4000);
        for (unsigned aliases = 0; aliases < 2; ++aliases) {
            struct {
                VkStructureType sType;
                void *pNext;
                uint64_t sentinel;
            } tail = {VK_STRUCTURE_TYPE_MAX_ENUM, NULL, UINT64_C(0xabcdef0123456789)};
            VkPhysicalDeviceShaderObjectPropertiesEXT shader = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT, .pNext = &tail};
            VkPhysicalDeviceMaintenance7PropertiesKHR m7 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR,
                                                            .pNext = &shader};
            VkPhysicalDeviceAccelerationStructurePropertiesKHR as = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR, .pNext = &m7};
            VkPhysicalDeviceInlineUniformBlockProperties inline_uniform = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES, .pNext = &as};
            VkPhysicalDeviceDescriptorIndexingProperties indexing = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES, .pNext = &inline_uniform};
            VkPhysicalDeviceVulkan13Properties v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
                                                      .pNext = &as};
            VkPhysicalDeviceVulkan12Properties v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
                                                      .pNext = &v13};
            void *head = aliases ? (void *)&v12 : (void *)&indexing;
            VkPhysicalDeviceProperties2 full = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = head};
            old_full = properties2_queries;
            old_uuid = uuid_queries;
            old_qualification = qualification_queries;
            (aliases ? khr_query : core_query)(physical, &full);
            REQUIRE(properties2_queries == old_full + 1 && uuid_queries == old_uuid + 1);
            REQUIRE(qualification_queries == old_qualification + (native_rt && !ignore_rt_features ? 1 : 0));
            REQUIRE(!memcmp(&full.properties, &ordinary, sizeof(ordinary)));
            REQUIRE(full.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 && full.pNext == head &&
                    as.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR && as.pNext == &m7 &&
                    m7.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR && m7.pNext == &shader &&
                    shader.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT && shader.pNext == &tail &&
                    tail.sType == VK_STRUCTURE_TYPE_MAX_ENUM && !tail.pNext && tail.sentinel == UINT64_C(0xabcdef0123456789));
            REQUIRE(as.maxPerStageDescriptorAccelerationStructures == (apply ? 16u : 1048576u));
            REQUIRE(as.maxDescriptorSetAccelerationStructures == (apply ? 16u : 1048576u));
            REQUIRE(as.maxPerStageDescriptorUpdateAfterBindAccelerationStructures == (apply ? 16u : 1048576u));
            REQUIRE(as.maxDescriptorSetUpdateAfterBindAccelerationStructures == (apply ? 16u : 1048576u));
            REQUIRE(as.maxGeometryCount == 777 && as.maxInstanceCount == 888 && as.maxPrimitiveCount == 999 &&
                    as.minAccelerationStructureScratchOffsetAlignment == 256);
            REQUIRE(m7.maxDescriptorSetTotalBuffersDynamic == 31 && m7.maxDescriptorSetUpdateAfterBindTotalBuffersDynamic == 31 &&
                    m7.robustFragmentShadingRateAttachmentAccess && m7.separateDepthStencilAttachmentAccess);
            REQUIRE(shader.shaderBinaryVersion == 55);
            for (unsigned byte = 0; byte < VK_UUID_SIZE; ++byte) REQUIRE(shader.shaderBinaryUUID[byte] == 0xc3);
            if (aliases) {
                REQUIRE(v12.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES && v12.pNext == &v13 &&
                        v13.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES && v13.pNext == &as);
#define CHECK_INDEX(name) REQUIRE(v12.name == (apply ? expected.indexing.name : native_resources.indexing.name));
                INDEX_FIELDS(CHECK_INDEX)
#undef CHECK_INDEX
#define CHECK_INLINE(name) REQUIRE(v13.name == (apply ? expected.inline_uniform.name : native_resources.inline_uniform.name));
                INLINE_FIELDS(CHECK_INLINE)
#undef CHECK_INLINE
                REQUIRE(v12.shaderDenormPreserveFloat32 && v13.storageTexelBufferOffsetAlignmentBytes == 128 &&
                        v13.maxInlineUniformTotalSize == 3584);
            } else {
                REQUIRE(indexing.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES &&
                        indexing.pNext == &inline_uniform &&
                        inline_uniform.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES &&
                        inline_uniform.pNext == &as && indexing.quadDivergentImplicitLod);
#define CHECK_INDEX(name) REQUIRE(indexing.name == (apply ? expected.indexing.name : native_resources.indexing.name));
                INDEX_FIELDS(CHECK_INDEX)
#undef CHECK_INDEX
#define CHECK_INLINE(name) \
    REQUIRE(inline_uniform.name == (apply ? expected.inline_uniform.name : native_resources.inline_uniform.name));
                INLINE_FIELDS(CHECK_INLINE)
#undef CHECK_INLINE
            }
        }
    }
    resource_defaults();
    native_rt = VK_TRUE;
    ignore_rt_features = false;
}

static void budget_failure(PFN_vkEnumerateDeviceExtensionProperties enumerate, PFN_vkGetPhysicalDeviceFeatures2 query) {
    native_resources.properties.limits.maxPerStageDescriptorStorageBuffers = 16;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp = {.sType =
                                                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &rtp};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &rq};
    VkPhysicalDeviceDescriptorIndexingFeatures indexing = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
                                                           .pNext = &as};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &indexing};
    query(physical, &f);
    REQUIRE(!as.accelerationStructure && !rq.rayQuery && !rtp.rayTracingPipeline);
    REQUIRE(f.features.shaderInt64 && f.features.geometryShader && indexing.runtimeDescriptorArray &&
            indexing.descriptorBindingPartiallyBound);
    uint32_t count = 0;
    REQUIRE(enumerate(physical, NULL, &count, NULL) == VK_SUCCESS && count == VKRT_ARRAY_SIZE(visible_names) - 3);
    const uint32_t visible = count;
    for (uint32_t capacity = 0; capacity <= visible + 1; ++capacity) {
        VkExtensionProperties output[10];
        memset(output, 0xa5, sizeof(output));
        count = capacity;
        REQUIRE(enumerate(physical, NULL, &count, output) == (capacity < visible ? VK_INCOMPLETE : VK_SUCCESS));
        REQUIRE(count == (capacity < visible ? capacity : visible));
        for (uint32_t i = 0; i < count; ++i) REQUIRE(!strcmp(output[i].extensionName, visible_names[i + 3]));
    }
    resource_defaults();
}

int main(void) {
    resource_defaults();
    struct vkrt_instance_data instance = {0};
    instance.vtable.GetPhysicalDeviceFeatures2 = mock_features;
    instance.vtable.GetPhysicalDeviceProperties = mock_properties;
    instance.vtable.GetPhysicalDeviceProperties2 = mock_properties2;
    instance.vtable.EnumerateDeviceExtensionProperties = mock_enumerate;
    vkrt_map_object(VKRT_HKEY(physical), &instance);
    PFN_vkEnumerateDeviceExtensionProperties enumerate = (void *)vkrt_discovery_proc("vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceFeatures2 query = (void *)vkrt_discovery_proc("vkGetPhysicalDeviceFeatures2");
    REQUIRE(enumerate != NULL && query != NULL);
    REQUIRE(vkrt_discovery_proc("vkGetPhysicalDeviceFeatures2KHR") == (PFN_vkVoidFunction)query);
    REQUIRE(vkrt_discovery_proc("unknown") == NULL);
    enumeration(enumerate);
    properties();
    budget_failure(enumerate, query);
    features(query, &instance);
    vkrt_unmap_object(VKRT_HKEY(physical));
    printf("Descriptor discovery property modes: 5; legacy queries %u, Properties2/UUID compositions %u/%u\n",
           legacy_properties_queries, properties2_queries, uuid_queries);
    printf("Conservative RT discovery: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
