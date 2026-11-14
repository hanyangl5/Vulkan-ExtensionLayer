/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_layer.h"
#include "vkrt_resource_profile.h"
#include "vkrt_software_profile.h"
#include <vulkan/vulkan_beta.h>

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
static unsigned native_rt;
static bool ignore_rt_properties;
static bool ignore_rt_features;
static bool ignore_per_stage_features;
static bool hide_pipeline_base;
static VkResult enumeration_error;
static bool grow_once, incomplete_fill;
static bool hide_deferred;
static bool hide_library, duplicate_library;
static bool expose_optional_objects;
static const char *last_layer;
static struct vkrt_resource_native native_resources;
static unsigned qualification_queries, legacy_properties_queries, properties2_queries, uuid_queries;
static unsigned query_capabilities = 3;
static VkShaderStageFlags native_subgroup_stages;

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

/* The production predicate is independently tested in test_device_features.
 * This discovery boundary spy supplies its mandatory-query decision and uses
 * the same pure budget planner, preserving transactional output on rejection. */
bool vkrt_virtual_rt_profile(struct vkrt_instance_data *inst, VkPhysicalDevice p, struct vkrt_software_rt_profile *profile) {
    REQUIRE(inst == VKRT_FIND(struct vkrt_instance_data, physical) && p == physical);
    ++qualification_queries;
    struct vkrt_resource_profile resources;
    if ((query_capabilities & 2) == 0 || !vkrt_plan_resource_profile(&native_resources, &resources, NULL)) return false;
    vkrt_software_rt_profile_init(&resources, profile);
    return true;
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
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
            VkPhysicalDeviceSubgroupProperties *s = (void *)next;
            s->subgroupSize = 32;
            s->supportedStages = native_subgroup_stages;
            s->supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT;
            s->quadOperationsInAllStages = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES) {
            VkPhysicalDeviceVulkan11Properties *s = (void *)next;
            memset(s->deviceUUID, 0x71, sizeof(s->deviceUUID));
            s->subgroupSize = 32;
            s->subgroupSupportedStages = native_subgroup_stages;
            s->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT;
            s->subgroupQuadOperationsInAllStages = VK_TRUE;
            s->maxMemoryAllocationSize = UINT64_C(0x123456789abcdef0);
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
            if (ignore_rt_properties) continue;
            a->maxPerStageDescriptorAccelerationStructures = a->maxDescriptorSetAccelerationStructures =
                a->maxPerStageDescriptorUpdateAfterBindAccelerationStructures =
                    a->maxDescriptorSetUpdateAfterBindAccelerationStructures = 1048576;
            a->maxGeometryCount = 777;
            a->maxInstanceCount = 888;
            a->maxPrimitiveCount = 999;
            a->minAccelerationStructureScratchOffsetAlignment = 256;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR) {
            if (ignore_rt_properties) continue;
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR *r = (void *)next;
            r->shaderGroupHandleSize = 48;
            r->maxRayRecursionDepth = 99;
            r->maxShaderGroupStride = 8192;
            r->shaderGroupBaseAlignment = 512;
            r->shaderGroupHandleCaptureReplaySize = 128;
            r->maxRayDispatchInvocationCount = 999;
            r->shaderGroupHandleAlignment = 64;
            r->maxRayHitAttributeSize = 64;
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
static const char *visible_names[] = {"VK_KHR_swapchain",
                                      "VK_EXT_robustness2",
                                      VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                      VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                                      VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
                                      VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
static const char *optional_object_names[] = {VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME,
                                              VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
                                              VK_EXT_PIPELINE_PROPERTIES_EXTENSION_NAME,
                                              VK_KHR_PIPELINE_BINARY_EXTENSION_NAME,
                                              VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
                                              VK_NV_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
                                              VK_NV_DEVICE_GENERATED_COMMANDS_COMPUTE_EXTENSION_NAME,
                                              VK_ARM_DATA_GRAPH_EXTENSION_NAME,
                                              VK_AMDX_SHADER_ENQUEUE_EXTENSION_NAME};
static uint32_t extension_version(const char *name) {
    if (!strcmp(name, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) return VK_KHR_ACCELERATION_STRUCTURE_SPEC_VERSION;
    if (!strcmp(name, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) return VK_KHR_RAY_TRACING_PIPELINE_SPEC_VERSION;
    if (!strcmp(name, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME)) return VK_KHR_PIPELINE_LIBRARY_SPEC_VERSION;
    if (!strcmp(name, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) return VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION;
    for (uint32_t i = 0; i < VKRT_ARRAY_SIZE(native_names); ++i)
        if (!strcmp(name, native_names[i])) return 100 + i;
    REQUIRE(false);
    return 0;
}

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
    VkExtensionProperties rows[32];
    uint32_t total = 0;
    uint32_t start = native_rt ? 0 : 2;
    for (uint32_t i = start; i < VKRT_ARRAY_SIZE(native_names); ++i) {
        if ((hide_deferred && !strcmp(native_names[i], VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) ||
            (hide_library && !strcmp(native_names[i], VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME)))
            continue;
        strcpy(rows[total].extensionName, native_names[i]);
        rows[total++].specVersion = 100 + i;
    }
    if (duplicate_library && !hide_library) rows[total++] = (VkExtensionProperties){VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, 999};
    if (expose_optional_objects)
        for (unsigned i = 0; i < VKRT_ARRAY_SIZE(optional_object_names); ++i) {
            strcpy(rows[total].extensionName, optional_object_names[i]);
            rows[total++].specVersion = 201 + i;
        }
    if (expose_optional_objects) {
        strcpy(rows[total].extensionName, VK_ARM_TENSORS_EXTENSION_NAME);
        rows[total++].specVersion = 501;
    }
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
    memcpy(properties, rows, *count * sizeof(*properties));
    return capacity < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mock_features(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *features) {
    REQUIRE(p == physical);
    ++feature_queries;
    features->features.shaderInt64 = features->features.geometryShader = VK_TRUE;
    for (VkBaseOutStructure *next = features->pNext; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR) {
            VkPhysicalDeviceAccelerationStructureFeaturesKHR *as = (void *)next;
            if (!ignore_rt_features) as->accelerationStructure = (native_rt & 1) != 0;
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
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT) {
            ((VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT *)next)->shaderModuleIdentifier = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR) {
            ((VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR *)next)->pipelineExecutableInfo = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROPERTIES_FEATURES_EXT) {
            ((VkPhysicalDevicePipelinePropertiesFeaturesEXT *)next)->pipelinePropertiesIdentifier = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR) {
            ((VkPhysicalDevicePipelineBinaryFeaturesKHR *)next)->pipelineBinaries = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT) {
            VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT *g = (void *)next;
            g->deviceGeneratedCommands = g->dynamicGeneratedPipelineLayout = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV) {
            ((VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV *)next)->deviceGeneratedCommands = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV) {
            VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV *g = (void *)next;
            g->deviceGeneratedCompute = g->deviceGeneratedComputePipelines = g->deviceGeneratedComputeCaptureReplay = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_FEATURES_ARM) {
            VkPhysicalDeviceDataGraphFeaturesARM *g = (void *)next;
            g->dataGraph = g->dataGraphUpdateAfterBind = g->dataGraphSpecializationConstants = g->dataGraphDescriptorBuffer =
                g->dataGraphShaderModule = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TENSOR_FEATURES_ARM) {
            VkPhysicalDeviceTensorFeaturesARM *t = (void *)next;
            t->tensorNonPacked = t->shaderTensorAccess = t->shaderStorageTensorArrayDynamicIndexing =
                t->shaderStorageTensorArrayNonUniformIndexing = t->descriptorBindingStorageTensorUpdateAfterBind = t->tensors =
                    VK_TRUE;
        } else if (next->sType == (VkStructureType)1000134000) {
            VkPhysicalDeviceShaderEnqueueFeaturesAMDX *g = (void *)next;
            g->shaderEnqueue = g->shaderMeshEnqueue = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT) {
            VkPhysicalDeviceMeshShaderFeaturesEXT *m = (void *)next;
            m->taskShader = m->meshShader = m->multiviewMeshShader = m->primitiveFragmentShadingRateMeshShader =
                m->meshShaderQueries = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT) {
            ((VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *)next)->graphicsPipelineLibrary = VK_TRUE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES) {
            ((VkPhysicalDevicePipelineCreationCacheControlFeatures *)next)->pipelineCreationCacheControl = VK_TRUE;
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
    for (unsigned rt = 0; rt < 8; ++rt) {
        native_rt = rt;
        uint32_t count = 99, start = 0;
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
                REQUIRE(output[i].specVersion == extension_version(output[i].extensionName));
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
    REQUIRE(enumerate(physical, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY", &count, &property) == VK_INCOMPLETE && count == 1);
    REQUIRE(!strcmp(property.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME));
    REQUIRE(property.specVersion == VK_KHR_ACCELERATION_STRUCTURE_SPEC_VERSION);
    VkExtensionProperties owned[5];
    count = 5;
    REQUIRE(enumerate(physical, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY", &count, owned) == VK_SUCCESS && count == 4);
    for (uint32_t i = 0; i < count; ++i) {
        REQUIRE(!strcmp(owned[i].extensionName, visible_names[i + 2]));
        REQUIRE(owned[i].specVersion == extension_version(owned[i].extensionName));
    }
    REQUIRE(enumeration_calls == before);
    /* A driver with no deferred extension still exposes the layer's complete
     * synchronous implementation, appended once to its native extensions. */
    hide_deferred = true;
    for (unsigned rt = 0; rt < 8; ++rt) {
        native_rt = rt;
        VkExtensionProperties output[10];
        count = 10;
        REQUIRE(enumerate(physical, NULL, &count, output) == VK_SUCCESS);
        REQUIRE(count == VKRT_ARRAY_SIZE(visible_names));
        REQUIRE(!strcmp(output[count - 1].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME));
        REQUIRE(output[count - 1].specVersion == VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION);
    }
    hide_deferred = false;
}

static void ineligible_library(PFN_vkEnumerateDeviceExtensionProperties enumerate) {
    query_capabilities = 0;
    for (unsigned absent = 0; absent < 2; ++absent) {
        hide_library = absent != 0;
        VkExtensionProperties output[10];
        uint32_t count = VKRT_ARRAY_SIZE(output);
        REQUIRE(enumerate(physical, NULL, &count, output) == VK_SUCCESS && count == 5 - absent);
        unsigned libraries = 0, legacy_addresses = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (!strcmp(output[i].extensionName, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME)) {
                ++libraries;
                /* The mock assigns 100+index over native_names; ray query is
                 * no longer part of that list, so pipeline-library is index 7. */
                REQUIRE(output[i].specVersion == 107);
            } else if (!strcmp(output[i].extensionName, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
                ++legacy_addresses;
                REQUIRE(output[i].specVersion == extension_version(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME));
            }
        REQUIRE(libraries == !absent);
        REQUIRE(legacy_addresses == 1); /* The software-ineligible native profile preserves EXT BDA. */
        count = 10;
        REQUIRE(enumerate(physical, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY", &count, output) == VK_SUCCESS && count == 1);
        REQUIRE(!strcmp(output[0].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME));
    }
    query_capabilities = 3;
    hide_library = false;
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
    for (unsigned rt = 0; rt < 8; ++rt) {
        native_rt = rt;
        query(physical, &core);
        REQUIRE(core.features.shaderInt64 && core.features.geometryShader);
        REQUIRE(!rq.rayQuery && as.accelerationStructure && rtp.rayTracingPipeline);
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
    /* Software support is independent of every native base-feature combination. */
    native_rt = VK_TRUE;
    hide_pipeline_base = true;
    query(physical, &core);
    REQUIRE(!rq.rayQuery && as.accelerationStructure && rtp.rayTracingPipeline);
    hide_pipeline_base = false;
    /* Ignored/stale native RT fields neither grant nor remove software support. */
    ignore_rt_features = true;
    ignore_per_stage_features = true;
    rq.rayQuery = as.accelerationStructure = rtp.rayTracingPipeline = VK_TRUE;
    per_stage.perStageDescriptorSet = per_stage.dynamicPipelineLayout = VK_TRUE;
    query(physical, &core);
    REQUIRE(!rq.rayQuery && as.accelerationStructure && rtp.rayTracingPipeline);
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
        bool apply = mode != 1 && mode != 3;
        ignore_rt_properties = mode == 4;
        struct vkrt_resource_profile expected = {0};
        if (apply) REQUIRE(vkrt_plan_resource_profile(&native_resources, &expected, NULL));
        VkPhysicalDeviceProperties ordinary;
        unsigned old_legacy = legacy_properties_queries, old_full = properties2_queries, old_uuid = uuid_queries;
        unsigned old_qualification = qualification_queries;
        legacy(physical, &ordinary);
        REQUIRE(legacy_properties_queries == old_legacy + 1 && properties2_queries == old_full && uuid_queries == old_uuid);
        REQUIRE(qualification_queries == old_qualification + 1);
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
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR, .pNext = &m7};
            VkPhysicalDeviceAccelerationStructurePropertiesKHR as = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR, .pNext = &rtp};
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
            REQUIRE(qualification_queries == old_qualification + 1);
            REQUIRE(!memcmp(&full.properties, &ordinary, sizeof(ordinary)));
            REQUIRE(full.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 && full.pNext == head &&
                    as.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR && as.pNext == &rtp &&
                    rtp.pNext == &m7 && m7.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR &&
                    m7.pNext == &shader && shader.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT &&
                    shader.pNext == &tail && tail.sType == VK_STRUCTURE_TYPE_MAX_ENUM && !tail.pNext &&
                    tail.sentinel == UINT64_C(0xabcdef0123456789));
            REQUIRE(as.maxPerStageDescriptorAccelerationStructures == (apply ? 16u : 0u));
            REQUIRE(as.maxDescriptorSetAccelerationStructures == (apply ? 16u : 0u));
            REQUIRE(as.maxPerStageDescriptorUpdateAfterBindAccelerationStructures == (apply ? 16u : 0u));
            REQUIRE(as.maxDescriptorSetUpdateAfterBindAccelerationStructures == (apply ? 16u : 0u));
            REQUIRE(as.maxGeometryCount == (apply ? 0xffffffu : 0u) && as.maxInstanceCount == (apply ? 0xffffffu : 0u) &&
                    as.maxPrimitiveCount == (apply ? 0x1fffffffu : 0u) &&
                    as.minAccelerationStructureScratchOffsetAlignment == (apply ? 256u : 0u));
            REQUIRE(rtp.shaderGroupHandleSize == (apply ? 32u : 0u) && rtp.maxRayRecursionDepth == (apply ? 31u : 0u) &&
                    rtp.maxShaderGroupStride == (apply ? 4096u : 0u) && rtp.shaderGroupBaseAlignment == (apply ? 64u : 0u) &&
                    !rtp.shaderGroupHandleCaptureReplaySize && rtp.maxRayDispatchInvocationCount == (apply ? 1u << 30 : 0u) &&
                    rtp.shaderGroupHandleAlignment == (apply ? 32u : 0u) && rtp.maxRayHitAttributeSize == (apply ? 32u : 0u));
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
    ignore_rt_properties = false;
}

static void subgroup_properties(void) {
    PFN_vkGetPhysicalDeviceProperties2 queries[] = {(void *)vkrt_discovery_proc("vkGetPhysicalDeviceProperties2"),
                                                    (void *)vkrt_discovery_proc("vkGetPhysicalDeviceProperties2KHR")};
    const VkShaderStageFlags ordinary =
        VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    const VkShaderStageFlags rt[] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR,       VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                                     VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,  VK_SHADER_STAGE_MISS_BIT_KHR,
                                     VK_SHADER_STAGE_INTERSECTION_BIT_KHR, VK_SHADER_STAGE_CALLABLE_BIT_KHR};
    /* Query prerequisites and descriptor budgets independently determine
     * software eligibility. Unsupported devices must retain native properties. */
    for (unsigned mode = 0; mode < 3; ++mode) {
        resource_defaults();
        query_capabilities = mode == 0 ? 0 : 3;
        if (mode == 1) native_resources.properties.limits.maxPerStageDescriptorStorageBuffers = 16;
        for (unsigned stages = 0; stages <= VKRT_ARRAY_SIZE(rt); ++stages) {
            native_subgroup_stages = ordinary | (stages ? rt[stages - 1] : 0);
            const VkShaderStageFlags expected = mode == 2 ? ordinary : native_subgroup_stages;
            for (unsigned alias = 0; alias < VKRT_ARRAY_SIZE(queries); ++alias)
                for (unsigned core11 = 0; core11 < 2; ++core11) {
                    struct {
                        VkStructureType sType;
                        void *pNext;
                        uint64_t sentinel;
                    } tail = {VK_STRUCTURE_TYPE_MAX_ENUM, NULL, UINT64_C(0xabcdef0123456789)};
                    VkPhysicalDeviceSubgroupProperties subgroup = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
                                                                   .pNext = &tail};
                    VkPhysicalDeviceVulkan11Properties v11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
                                                              .pNext = &tail};
                    void *head = core11 ? (void *)&v11 : (void *)&subgroup;
                    VkPhysicalDeviceProperties2 full = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = head};
                    queries[alias](physical, &full);
                    REQUIRE(full.pNext == head && tail.sType == VK_STRUCTURE_TYPE_MAX_ENUM && !tail.pNext &&
                            tail.sentinel == UINT64_C(0xabcdef0123456789));
                    if (core11) {
                        REQUIRE(
                            v11.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES && v11.pNext == &tail &&
                            v11.subgroupSize == 32 && v11.subgroupSupportedStages == expected &&
                            v11.subgroupSupportedOperations == (VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT) &&
                            v11.subgroupQuadOperationsInAllStages && v11.maxMemoryAllocationSize == UINT64_C(0x123456789abcdef0));
                        for (unsigned byte = 0; byte < VK_UUID_SIZE; ++byte) REQUIRE(v11.deviceUUID[byte] == 0x71);
                    } else {
                        REQUIRE(subgroup.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES &&
                                subgroup.pNext == &tail && subgroup.subgroupSize == 32 && subgroup.supportedStages == expected &&
                                subgroup.supportedOperations == (VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT) &&
                                subgroup.quadOperationsInAllStages);
                    }
                }
        }
    }
    resource_defaults();
    query_capabilities = 3;
}

static void expect_rt_masked(PFN_vkEnumerateDeviceExtensionProperties enumerate, PFN_vkGetPhysicalDeviceFeatures2 query) {
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
    REQUIRE(enumerate(physical, NULL, &count, NULL) == VK_SUCCESS && count == VKRT_ARRAY_SIZE(visible_names) - 1);
    const uint32_t visible = count;
    for (uint32_t capacity = 0; capacity <= visible + 1; ++capacity) {
        VkExtensionProperties output[10];
        memset(output, 0xa5, sizeof(output));
        count = capacity;
        REQUIRE(enumerate(physical, NULL, &count, output) == (capacity < visible ? VK_INCOMPLETE : VK_SUCCESS));
        REQUIRE(count == (capacity < visible ? capacity : visible));
        const char *expected[] = {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                                  "VK_KHR_swapchain", "VK_EXT_robustness2", VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
        for (uint32_t i = 0; i < count; ++i) REQUIRE(!strcmp(output[i].extensionName, expected[i]));
    }
}

static unsigned format_api;
static bool ignore_format3;
static VKAPI_ATTR void VKAPI_CALL native_format(VkPhysicalDevice p, VkFormat format, VkFormatProperties *out) {
    REQUIRE(p == physical);
    (void)format;
    *out = (VkFormatProperties){
        VK_FORMAT_FEATURE_BLIT_SRC_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
        VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT | (native_rt ? VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR : 0)};
}
static VKAPI_ATTR void VKAPI_CALL native_format2(VkPhysicalDevice p, VkFormat format, VkFormatProperties2 *out) {
    native_format(p, format, &out->formatProperties);
    for (VkBaseOutStructure *n = out->pNext; n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 && !ignore_format3) {
            VkFormatProperties3 *f = (void *)n;
            f->linearTilingFeatures = out->formatProperties.linearTilingFeatures;
            f->optimalTilingFeatures = out->formatProperties.optimalTilingFeatures;
            f->bufferFeatures = out->formatProperties.bufferFeatures | (UINT64_C(1) << 42);
        }
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL format_gipa(VkInstance instance, const char *name) {
    (void)instance;
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties")) return (PFN_vkVoidFunction)native_format;
    if ((!strcmp(name, "vkGetPhysicalDeviceFormatProperties2") && format_api == 0) ||
        (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") && format_api <= 1))
        return (PFN_vkVoidFunction)native_format2;
    return NULL;
}
static void formats(struct vkrt_instance_data *instance) {
    instance->vtable.GetInstanceProcAddr = format_gipa;
    PFN_vkGetPhysicalDeviceFormatProperties query = (void *)vkrt_discovery_proc("vkGetPhysicalDeviceFormatProperties");
    PFN_vkGetPhysicalDeviceFormatProperties2 query2 = (void *)vkrt_discovery_proc("vkGetPhysicalDeviceFormatProperties2");
    REQUIRE(query && query2 && vkrt_discovery_proc("vkGetPhysicalDeviceFormatProperties2KHR") == (PFN_vkVoidFunction)query2);
    const VkFormat cases[] = {VK_FORMAT_R32G32B32_SFLOAT,         VK_FORMAT_R16G16_SNORM, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R32_SFLOAT,   VK_FORMAT_UNDEFINED};
    for (unsigned native = 0; native < 2; ++native)
        for (unsigned qualified = 0; qualified < 2; ++qualified)
            for (format_api = 0; format_api < 3; ++format_api)
                for (unsigned stale = 0; stale < 2; ++stale)
                    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(cases); ++i) {
                        native_rt = native;
                        query_capabilities = qualified ? 3 : 0;
                        ignore_format3 = stale != 0;
                        bool advertised_format = qualified && i < 4;
                        VkFormatFeatureFlags expected =
                            VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
                            (advertised_format ? VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR : 0);
                        VkFormatProperties basic;
                        memset(&basic, 0xa5, sizeof(basic));
                        query(physical, cases[i], &basic);
                        REQUIRE(basic.linearTilingFeatures == VK_FORMAT_FEATURE_BLIT_SRC_BIT &&
                                basic.optimalTilingFeatures == VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT &&
                                basic.bufferFeatures == expected);
                        struct {
                            VkStructureType sType;
                            void *pNext;
                            uint64_t value;
                        } tail = {VK_STRUCTURE_TYPE_MAX_ENUM, NULL, UINT64_C(0x123456789abcdef0)};
                        VkFormatProperties3 flags;
                        memset(&flags, 0xa5, sizeof(flags));
                        flags.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
                        flags.pNext = &tail;
                        VkFormatProperties2 full = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &flags, {0}};
                        query2(physical, cases[i], &full);
                        REQUIRE(!memcmp(&basic, &full.formatProperties, sizeof(basic)) && full.pNext == &flags &&
                                flags.pNext == &tail && tail.value == UINT64_C(0x123456789abcdef0));
                        const bool ignored = format_api < 2 && ignore_format3;
                        VkFormatFeatureFlags2 expected3 = ignored ? 0 : VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
                        if (!ignored && format_api < 2) expected3 |= UINT64_C(1) << 42;
                        if (advertised_format) expected3 |= VK_FORMAT_FEATURE_2_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR;
                        REQUIRE(flags.bufferFeatures == expected3 &&
                                flags.linearTilingFeatures == (ignored ? 0u : VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                                flags.optimalTilingFeatures == (ignored ? 0u : VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT));
                    }
    query_capabilities = 3;
    ignore_format3 = false;
}

static void budget_failure(PFN_vkEnumerateDeviceExtensionProperties enumerate, PFN_vkGetPhysicalDeviceFeatures2 query) {
    native_resources.properties.limits.maxPerStageDescriptorStorageBuffers = 16;
    expect_rt_masked(enumerate, query);
    resource_defaults();
}

static void query_backend_failure(PFN_vkEnumerateDeviceExtensionProperties enumerate, PFN_vkGetPhysicalDeviceFeatures2 query) {
    native_rt = VK_TRUE;
    for (unsigned missing = 2; missing <= 2; ++missing) {
        query_capabilities = 3 & ~missing; /* Buffer atomic64 remains mandatory; core events remove host-reset need. */
        expect_rt_masked(enumerate, query);
        PFN_vkGetPhysicalDeviceProperties legacy = (void *)vkrt_discovery_proc("vkGetPhysicalDeviceProperties");
        VkPhysicalDeviceProperties actual;
        legacy(physical, &actual);
        REQUIRE(!memcmp(&actual, &native_resources.properties, sizeof(actual)));
    }
    /* Native RT and hostQueryReset are both absent; core events still provide
     * the mandatory software query marker and discovery stays consistent. */
    query_capabilities = 2;
    native_rt = VK_FALSE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &as};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .pNext = &rq};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &rtp};
    query(physical, &f);
    REQUIRE(as.accelerationStructure && !rq.rayQuery && rtp.rayTracingPipeline);
    uint32_t count = 0;
    REQUIRE(enumerate(physical, NULL, &count, NULL) == VK_SUCCESS && count == VKRT_ARRAY_SIZE(visible_names));
    query_capabilities = 3;
}

static void optional_virtual_object_consumers(PFN_vkEnumerateDeviceExtensionProperties enumerate,
                                              PFN_vkGetPhysicalDeviceFeatures2 query) {
    VkPhysicalDevicePipelineCreationCacheControlFeatures cache = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .pNext = &cache};
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT identifier = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, .pNext = &gpl};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, .pNext = &identifier};
    VkPhysicalDevicePipelinePropertiesFeaturesEXT properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROPERTIES_FEATURES_EXT, .pNext = &executable};
    VkPhysicalDevicePipelineBinaryFeaturesKHR binaries = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR,
                                                          .pNext = &properties};
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT generated = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT, .pNext = &binaries};
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV generated_nv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV, .pNext = &generated};
    VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV compute_nv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV, .pNext = &generated_nv};
    VkPhysicalDeviceDataGraphFeaturesARM data_graph = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_FEATURES_ARM,
                                                       .pNext = &compute_nv};
    VkPhysicalDeviceTensorFeaturesARM tensors = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TENSOR_FEATURES_ARM,
                                                 .pNext = &data_graph};
    VkPhysicalDeviceShaderEnqueueFeaturesAMDX enqueue = {.sType = (VkStructureType)1000134000, .pNext = &tensors};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
                                                  .pNext = &enqueue};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &mesh};
    VkBool32 *bits[] = {&identifier.shaderModuleIdentifier,
                        &executable.pipelineExecutableInfo,
                        &properties.pipelinePropertiesIdentifier,
                        &binaries.pipelineBinaries,
                        &generated.deviceGeneratedCommands,
                        &generated.dynamicGeneratedPipelineLayout,
                        &generated_nv.deviceGeneratedCommands,
                        &compute_nv.deviceGeneratedCompute,
                        &compute_nv.deviceGeneratedComputePipelines,
                        &compute_nv.deviceGeneratedComputeCaptureReplay,
                        &data_graph.dataGraph,
                        &data_graph.dataGraphUpdateAfterBind,
                        &data_graph.dataGraphSpecializationConstants,
                        &data_graph.dataGraphDescriptorBuffer,
                        &data_graph.dataGraphShaderModule,
                        &enqueue.shaderEnqueue,
                        &enqueue.shaderMeshEnqueue};
    expose_optional_objects = true;
    for (unsigned eligible = 0; eligible < 2; ++eligible) {
        query_capabilities = eligible ? 3 : 0;
        query(physical, &f);
        for (unsigned bit = 0; bit < VKRT_ARRAY_SIZE(bits); ++bit) REQUIRE(*bits[bit] == !eligible);
        REQUIRE(gpl.graphicsPipelineLibrary && cache.pipelineCreationCacheControl);
        REQUIRE(mesh.taskShader && mesh.meshShader && mesh.multiviewMeshShader && mesh.primitiveFragmentShadingRateMeshShader &&
                mesh.meshShaderQueries);
        REQUIRE(tensors.tensors && tensors.tensorNonPacked && tensors.shaderTensorAccess &&
                tensors.shaderStorageTensorArrayDynamicIndexing && tensors.shaderStorageTensorArrayNonUniformIndexing &&
                tensors.descriptorBindingStorageTensorUpdateAfterBind);
        REQUIRE(f.pNext == &mesh && mesh.pNext == &enqueue && enqueue.pNext == &tensors && tensors.pNext == &data_graph &&
                data_graph.pNext == &compute_nv && compute_nv.pNext == &generated_nv && generated_nv.pNext == &generated &&
                generated.pNext == &binaries && binaries.pNext == &properties && properties.pNext == &executable &&
                executable.pNext == &identifier && identifier.pNext == &gpl && gpl.pNext == &cache && !cache.pNext);
        VkExtensionProperties rows[40];
        uint32_t count = 0;
        REQUIRE(enumerate(physical, NULL, &count, NULL) == VK_SUCCESS && count <= VKRT_ARRAY_SIZE(rows));
        uint32_t expected_count = count;
        count = VKRT_ARRAY_SIZE(rows);
        REQUIRE(enumerate(physical, NULL, &count, rows) == VK_SUCCESS && count == expected_count);
        for (unsigned i = 0; i < VKRT_ARRAY_SIZE(optional_object_names); ++i) {
            unsigned found = 0;
            for (unsigned row = 0; row < count; ++row)
                if (!strcmp(rows[row].extensionName, optional_object_names[i])) {
                    ++found;
                    REQUIRE(rows[row].specVersion == 201 + i);
                }
            REQUIRE(found == !eligible);
        }
        unsigned found_tensors = 0;
        for (unsigned row = 0; row < count; ++row)
            if (!strcmp(rows[row].extensionName, VK_ARM_TENSORS_EXTENSION_NAME)) {
                ++found_tensors;
                REQUIRE(rows[row].specVersion == 501);
            }
        REQUIRE(found_tensors == 1);
    }
    expose_optional_objects = false;
    query_capabilities = 3;
}

int main(void) {
    resource_defaults();
    struct vkrt_instance_data instance = {0};
    instance.api_version = VK_API_VERSION_1_2;
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
    formats(&instance);
    enumeration(enumerate);
    hide_library = true;
    enumeration(enumerate);
    hide_library = false;
    duplicate_library = true;
    enumeration(enumerate);
    duplicate_library = false;
    ineligible_library(enumerate);
    properties();
    subgroup_properties();
    budget_failure(enumerate, query);
    query_backend_failure(enumerate, query);
    optional_virtual_object_consumers(enumerate, query);
    features(query, &instance);
    vkrt_unmap_object(VKRT_HKEY(physical));
    printf("Descriptor discovery property modes: 5; legacy queries %u, Properties2/UUID compositions %u/%u\n",
           legacy_properties_queries, properties2_queries, uuid_queries);
    printf("Software RT discovery: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
