/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_layer.h"
#include <stddef.h>

#define HANDLE(type, value) ((type)(uintptr_t)(value))
#define REQUIRE(x)                                               \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            ++failures;                                          \
        }                                                        \
    } while (0)
static unsigned failures, creates, queries, property_queries;
static unsigned missing_capability;
static uint32_t driver_api;
static const VkPhysicalDevice physical = HANDLE(VkPhysicalDevice, 7);
static const VkDevice created_device = HANDLE(VkDevice, 8);
static const VkDeviceCreateInfo *original;
static const VkAllocationCallbacks *expected_allocator;
static const VkBaseInStructure *opaque_suffix;
static const char *expected_extensions[16];
static uint32_t expected_extension_count;
static VkResult downstream_result;
static bool expect_private_dynamic_indexing;

static const VkBaseInStructure *find_original(VkStructureType type) {
    for (const VkBaseInStructure *p = original->pNext; p; p = p->pNext)
        if (p->sType == type) return p;
    return NULL;
}

/* Private GPU-capability query for virtual RT. Unrelated supported options
 * are true to catch accidental enabling from query results. */
static VKAPI_ATTR void VKAPI_CALL mock_features(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *features) {
    REQUIRE(p == physical);
    ++queries;
    features->features.shaderInt64 = missing_capability != 1;
    features->features.shaderInt16 = missing_capability != 2;
    features->features.shaderStorageBufferArrayDynamicIndexing = missing_capability != 10;
    features->features.geometryShader = VK_TRUE;
    features->features.robustBufferAccess = VK_TRUE;
    unsigned seen = 0;
    for (VkBaseOutStructure *next = features->pNext; next; next = next->pNext) {
        switch (next->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
                VkPhysicalDevice16BitStorageFeatures *f = (void *)next;
                f->storageBuffer16BitAccess = missing_capability != 3;
                f->storageInputOutput16 = f->storagePushConstant16 = VK_TRUE;
                ++seen;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: {
                VkPhysicalDevice8BitStorageFeatures *f = (void *)next;
                f->storageBuffer8BitAccess = missing_capability != 4;
                f->storagePushConstant8 = VK_TRUE;
                ++seen;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
                VkPhysicalDeviceShaderFloat16Int8Features *f = (void *)next;
                f->shaderInt8 = missing_capability != 5;
                f->shaderFloat16 = VK_TRUE;
                ++seen;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
                ((VkPhysicalDeviceScalarBlockLayoutFeatures *)next)->scalarBlockLayout = missing_capability != 6;
                ++seen;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
                VkPhysicalDeviceBufferDeviceAddressFeatures *f = (void *)next;
                f->bufferDeviceAddress = missing_capability != 7;
                f->bufferDeviceAddressCaptureReplay = f->bufferDeviceAddressMultiDevice = VK_TRUE;
                ++seen;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES: {
                VkPhysicalDeviceVulkanMemoryModelFeatures *f = (void *)next;
                f->vulkanMemoryModel = missing_capability != 8;
                f->vulkanMemoryModelDeviceScope = missing_capability != 9;
                f->vulkanMemoryModelAvailabilityVisibilityChains = VK_TRUE;
                ++seen;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
                VkPhysicalDeviceDescriptorIndexingFeatures *f = (void *)next;
                f->shaderStorageBufferArrayNonUniformIndexing = missing_capability != 11;
                f->runtimeDescriptorArray = f->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
                ++seen;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
                VkPhysicalDeviceVariablePointersFeatures *f = (void *)next;
                f->variablePointersStorageBuffer = missing_capability != 12;
                f->variablePointers = VK_TRUE;
                ++seen;
                break;
            }
            default:
                REQUIRE(false);
                break;
        }
    }
    REQUIRE(seen == 8);
}

static VKAPI_ATTR void VKAPI_CALL mock_properties(VkPhysicalDevice p, VkPhysicalDeviceProperties *properties) {
    REQUIRE(p == physical);
    ++property_queries;
    memset(properties, 0, sizeof(*properties));
    properties->apiVersion = driver_api;
    properties->limits.maxComputeWorkGroupInvocations = missing_capability == 18 ? 127 : 128;
    properties->limits.maxComputeWorkGroupSize[0] = missing_capability == 19 ? 127 : 128;
    properties->limits.maxComputeWorkGroupSize[1] = missing_capability == 20 ? 0 : 1;
    properties->limits.maxComputeWorkGroupSize[2] = missing_capability == 21 ? 0 : 1;
    properties->limits.maxComputeWorkGroupCount[0] = missing_capability == 22 ? 0 : 65535;
    properties->limits.maxComputeWorkGroupCount[1] = missing_capability == 23 ? 0 : 1;
    properties->limits.maxComputeWorkGroupCount[2] = missing_capability == 24 ? 0 : 1;
    properties->limits.maxPushConstantsSize = missing_capability == 25 ? 127 : 128;
    properties->limits.maxComputeSharedMemorySize = missing_capability == 26 ? 2067 : 2068;
}

static VKAPI_ATTR void VKAPI_CALL mock_properties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *properties) {
    REQUIRE(properties->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    mock_properties(p, &properties->properties);
    unsigned seen = 0;
    for (VkBaseOutStructure *next = properties->pNext; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
            VkPhysicalDeviceSubgroupProperties *s = (void *)next;
            s->supportedStages = VK_SHADER_STAGE_FRAGMENT_BIT | (missing_capability == 13 ? 0 : VK_SHADER_STAGE_COMPUTE_BIT);
            s->supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                     VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT;
            if (missing_capability == 14) s->supportedOperations &= ~VK_SUBGROUP_FEATURE_BASIC_BIT;
            if (missing_capability == 15) s->supportedOperations &= ~VK_SUBGROUP_FEATURE_BALLOT_BIT;
            if (missing_capability == 16) s->supportedOperations &= ~VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
            s->subgroupSize = 32;
            ++seen;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES) {
            VkPhysicalDeviceFloatControlsProperties *f = (void *)next;
            f->shaderSignedZeroInfNanPreserveFloat32 = missing_capability != 17;
            f->shaderSignedZeroInfNanPreserveFloat16 = f->shaderSignedZeroInfNanPreserveFloat64 = VK_TRUE;
            ++seen;
        } else {
            REQUIRE(false);
        }
    }
    REQUIRE(seen == 2);
}

static void check_core(VkPhysicalDeviceFeatures actual, VkPhysicalDeviceFeatures expected) {
    expected.shaderInt64 = expected.shaderInt16 = VK_TRUE;
    if (expect_private_dynamic_indexing) expected.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    REQUIRE(!memcmp(&actual, &expected, sizeof(expected)));
}

/* Compare feature payloads without C struct padding becoming a test oracle. */
static size_t bool_payload_size(VkStructureType type) {
#define PAYLOAD(stype, ctype, last) \
    case stype:                     \
        return offsetof(ctype, last) + sizeof(VkBool32) - sizeof(VkBaseInStructure)
    switch (type) {
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features, shaderDrawParameters);
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features,
                subgroupBroadcastDynamicId);
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures,
                storageInputOutput16);
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures, storagePushConstant8);
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features,
                shaderInt8);
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures,
                scalarBlockLayout);
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures,
                bufferDeviceAddressMultiDevice);
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VkPhysicalDeviceVulkanMemoryModelFeatures,
                vulkanMemoryModelAvailabilityVisibilityChains);
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures,
                runtimeDescriptorArray);
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, VkPhysicalDeviceVariablePointersFeatures,
                variablePointers);
        PAYLOAD(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV,
                VkPhysicalDevicePerStageDescriptorSetFeaturesNV, dynamicPipelineLayout);
        default:
            REQUIRE(false);
            return 0;
    }
#undef PAYLOAD
}
#define EXPECT_NODE(type, actual, edits)                                                                                         \
    do {                                                                                                                         \
        const type *source = (const void *)find_original((actual)->sType);                                                       \
        type expected = source ? *source : (type){.sType = (actual)->sType};                                                     \
        expected.pNext = (void *)(actual)->pNext;                                                                                \
        edits;                                                                                                                   \
        REQUIRE(!memcmp((const char *)&expected + sizeof(VkBaseInStructure), (const char *)(actual) + sizeof(VkBaseInStructure), \
                        bool_payload_size((actual)->sType)));                                                                    \
    } while (0)

static VKAPI_ATTR VkResult VKAPI_CALL mock_create(VkPhysicalDevice p, const VkDeviceCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator, VkDevice *device) {
    REQUIRE(p == physical && allocator == expected_allocator);
    ++creates;
    REQUIRE(info->flags == original->flags && info->queueCreateInfoCount == original->queueCreateInfoCount);
    REQUIRE(info->pQueueCreateInfos == original->pQueueCreateInfos && info->ppEnabledLayerNames == original->ppEnabledLayerNames);
    REQUIRE(info->enabledLayerCount == original->enabledLayerCount);
    REQUIRE(info->enabledExtensionCount == expected_extension_count);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const char *name = info->ppEnabledExtensionNames[i];
        REQUIRE(strcmp(name, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME));
        REQUIRE(strcmp(name, VK_KHR_RAY_QUERY_EXTENSION_NAME));
        REQUIRE(strcmp(name, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME));
        if (i < expected_extension_count) REQUIRE(!strcmp(name, expected_extensions[i]));
    }
    unsigned core2 = 0, storage16 = 0, storage8 = 0, int8 = 0, scalar = 0, memory = 0, bda = 0;
    unsigned descriptor = 0;
    unsigned pointers = 0;
    for (const VkBaseInStructure *next = info->pNext; next; next = next->pNext) {
        switch (next->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: {
                const VkPhysicalDeviceFeatures2 *f = (const void *)next;
                const VkPhysicalDeviceFeatures2 *source = (const void *)find_original(next->sType);
                REQUIRE(source != NULL && info->pEnabledFeatures == NULL);
                if (source) check_core(f->features, source->features);
                ++core2;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
                EXPECT_NODE(VkPhysicalDeviceVulkan11Features, next, expected.storageBuffer16BitAccess = VK_TRUE;
                            if (expect_private_dynamic_indexing) expected.variablePointersStorageBuffer = VK_TRUE);
                ++storage16;
                ++pointers;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
                EXPECT_NODE(
                    VkPhysicalDeviceVulkan12Features, next,
                    expected.storageBuffer8BitAccess = expected.shaderInt8 = expected.scalarBlockLayout = VK_TRUE;
                    expected.vulkanMemoryModel = expected.vulkanMemoryModelDeviceScope = expected.bufferDeviceAddress = VK_TRUE;
                    if (expect_private_dynamic_indexing) expected.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE);
                ++storage8;
                ++int8;
                ++scalar;
                ++memory;
                ++bda;
                ++descriptor;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
                EXPECT_NODE(VkPhysicalDeviceDescriptorIndexingFeatures, next,
                            if (expect_private_dynamic_indexing) expected.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE);
                ++descriptor;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
                EXPECT_NODE(VkPhysicalDeviceVariablePointersFeatures, next,
                            if (expect_private_dynamic_indexing) expected.variablePointersStorageBuffer = VK_TRUE);
                ++pointers;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
                EXPECT_NODE(VkPhysicalDevice16BitStorageFeatures, next, expected.storageBuffer16BitAccess = VK_TRUE);
                ++storage16;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
                EXPECT_NODE(VkPhysicalDevice8BitStorageFeatures, next, expected.storageBuffer8BitAccess = VK_TRUE);
                ++storage8;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
                EXPECT_NODE(VkPhysicalDeviceShaderFloat16Int8Features, next, expected.shaderInt8 = VK_TRUE);
                ++int8;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
                EXPECT_NODE(VkPhysicalDeviceScalarBlockLayoutFeatures, next, expected.scalarBlockLayout = VK_TRUE);
                ++scalar;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
                EXPECT_NODE(VkPhysicalDeviceBufferDeviceAddressFeatures, next, expected.bufferDeviceAddress = VK_TRUE);
                ++bda;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
                EXPECT_NODE(VkPhysicalDeviceVulkanMemoryModelFeatures, next,
                            expected.vulkanMemoryModel = expected.vulkanMemoryModelDeviceScope = VK_TRUE);
                ++memory;
                break;
            case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO: {
                const VkLayerDeviceCreateInfo *loader = (const void *)next;
                const VkLayerDeviceCreateInfo *source = (const void *)find_original(next->sType);
                REQUIRE(source && loader->function == source->function && loader->u.pLayerInfo == source->u.pLayerInfo);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV:
                EXPECT_NODE(VkPhysicalDevicePerStageDescriptorSetFeaturesNV, next, (void)0);
                break;
            default:
                REQUIRE(next == opaque_suffix);
                break;
        }
        /* No virtual feature structure reaches the no-RT driver, even if false. */
        REQUIRE(next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);
        REQUIRE(next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR);
        REQUIRE(next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR);
    }
    if (!core2)
        check_core(*info->pEnabledFeatures,
                   original->pEnabledFeatures ? *original->pEnabledFeatures : (VkPhysicalDeviceFeatures){0});
    REQUIRE(core2 <= 1 && storage16 == 1 && storage8 == 1 && int8 == 1 && scalar == 1 && memory == 1 && bda == 1);
    REQUIRE(descriptor <= 1 && (!expect_private_dynamic_indexing || descriptor == 1));
    REQUIRE(pointers <= 1 && (!expect_private_dynamic_indexing || pointers == 1));
    if (downstream_result == VK_SUCCESS) *device = created_device;
    return downstream_result;
}
#undef EXPECT_NODE

static bool virtual_node(VkStructureType type) {
    return type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR ||
           type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR ||
           type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
}

/* An independent ordinary-device boundary: no backend feature augmentation,
 * no capability query, and no alteration of native feature payloads/order. */
static VKAPI_ATTR VkResult VKAPI_CALL mock_non_rt_create(VkPhysicalDevice p, const VkDeviceCreateInfo *info,
                                                         const VkAllocationCallbacks *callbacks, VkDevice *device) {
    REQUIRE(p == physical && callbacks == expected_allocator);
    ++creates;
    REQUIRE(info->flags == original->flags && info->queueCreateInfoCount == original->queueCreateInfoCount);
    REQUIRE(info->pQueueCreateInfos == original->pQueueCreateInfos && info->ppEnabledLayerNames == original->ppEnabledLayerNames);
    REQUIRE(info->enabledLayerCount == original->enabledLayerCount && info->pEnabledFeatures == original->pEnabledFeatures);
    REQUIRE(info->enabledExtensionCount == expected_extension_count);
    for (uint32_t i = 0; i < info->enabledExtensionCount && i < expected_extension_count; ++i)
        REQUIRE(!strcmp(info->ppEnabledExtensionNames[i], expected_extensions[i]));
    const VkBaseInStructure *expected = original->pNext;
    for (const VkBaseInStructure *actual = info->pNext; actual; actual = actual->pNext) {
        while (expected && virtual_node(expected->sType)) expected = expected->pNext;
        REQUIRE(expected && actual->sType == expected->sType);
        if (!expected || actual->sType != expected->sType) break;
        if (actual->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            REQUIRE(!memcmp(&((const VkPhysicalDeviceFeatures2 *)actual)->features,
                            &((const VkPhysicalDeviceFeatures2 *)expected)->features, sizeof(VkPhysicalDeviceFeatures)));
        } else if (actual->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            const VkLayerDeviceCreateInfo *a = (const void *)actual, *e = (const void *)expected;
            REQUIRE(a->function == e->function && a->u.pLayerInfo == e->u.pLayerInfo);
        } else if (actual->sType == VK_STRUCTURE_TYPE_MAX_ENUM) {
            REQUIRE(actual == expected && actual == opaque_suffix);
        } else {
            REQUIRE(!memcmp((const char *)actual + sizeof(VkBaseInStructure), (const char *)expected + sizeof(VkBaseInStructure),
                            bool_payload_size(actual->sType)));
        }
        expected = expected->pNext;
    }
    while (expected && virtual_node(expected->sType)) expected = expected->pNext;
    REQUIRE(!expected);
    if (downstream_result == VK_SUCCESS) *device = created_device;
    return downstream_result;
}

static struct vkrt_instance_data instance;
static VkDeviceCreateInfo basic;
static VkAllocationCallbacks allocator;

static VkResult call(const VkDeviceCreateInfo *info) {
    original = info;
    expected_allocator = &allocator;
    VkDevice device = VK_NULL_HANDLE;
    VkResult result = vkrt_create_device_with_features(&instance, physical, mock_create, info, &allocator, &device);
    if (result == VK_SUCCESS) REQUIRE(device == created_device);
    return result;
}

static VkResult call_non_rt(const VkDeviceCreateInfo *info) {
    original = info;
    expected_allocator = &allocator;
    unsigned before_queries = queries, before_properties = property_queries;
    VkDevice device = VK_NULL_HANDLE;
    VkResult result = vkrt_create_device_with_features(&instance, physical, mock_non_rt_create, info, &allocator, &device);
    REQUIRE(queries == before_queries && property_queries == before_properties);
    if (result == VK_SUCCESS) REQUIRE(device == created_device);
    return result;
}

static void defaults(void) {
    memset(&instance, 0, sizeof(instance));
    instance.api_version = VK_API_VERSION_1_2;
    instance.vtable.GetPhysicalDeviceFeatures2 = mock_features;
    instance.vtable.GetPhysicalDeviceProperties = mock_properties;
    instance.vtable.GetPhysicalDeviceProperties2 = mock_properties2;
    driver_api = VK_API_VERSION_1_2;
    basic = (VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    expected_extension_count = 0;
    opaque_suffix = NULL;
    missing_capability = 0;
    downstream_result = VK_SUCCESS;
    expect_private_dynamic_indexing = false;
}

static void non_rt_boundary(void) {
    defaults();
    VkPhysicalDeviceFeatures core = {.samplerAnisotropy = VK_TRUE, .shaderFloat64 = VK_TRUE};
    VkPhysicalDeviceFeatures saved_core = core;
    const char *extensions[] = {"VK_KHR_swapchain", VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, "VK_EXT_robustness2",
                                VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};
    expected_extensions[0] = extensions[0];
    expected_extensions[1] = extensions[2];
    expected_extensions[2] = extensions[3];
    expected_extension_count = 3;
    basic.pEnabledFeatures = &core;
    basic.ppEnabledExtensionNames = extensions;
    basic.enabledExtensionCount = VKRT_ARRAY_SIZE(extensions);
    VkDeviceCreateInfo saved_basic = basic;
    for (missing_capability = 1; missing_capability <= 26; ++missing_capability) {
        REQUIRE(call_non_rt(&basic) == VK_SUCCESS);
        REQUIRE(!memcmp(&core, &saved_core, sizeof(core)) && !memcmp(&basic, &saved_basic, sizeof(basic)));
    }

    /* Missing GPU-query entry points cannot prevent the CPU-only path. */
    instance.api_version = driver_api = VK_API_VERSION_1_0;
    instance.vtable.GetPhysicalDeviceFeatures2 = NULL;
    instance.vtable.GetPhysicalDeviceProperties = NULL;
    instance.vtable.GetPhysicalDeviceProperties2 = NULL;
    REQUIRE(call_non_rt(&basic) == VK_SUCCESS);
    basic.enabledExtensionCount = 0;
    basic.ppEnabledExtensionNames = NULL;
    expected_extension_count = 0;
    REQUIRE(call_non_rt(&basic) == VK_SUCCESS);
    const char *deferred = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
    basic.enabledExtensionCount = 1;
    basic.ppEnabledExtensionNames = &deferred;
    REQUIRE(call_non_rt(&basic) == VK_SUCCESS);

    /* Native feature chains remain byte-identical even when consumed false
     * RT nodes occur between them. An opaque unmodified suffix stays borrowed. */
    instance.api_version = driver_api = VK_API_VERSION_1_2;
    struct {
        VkStructureType sType;
        const void *pNext;
        uint64_t values[2];
    } suffix = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM, .values = {0x0123456789abcdefull, 0xfedcba9876543210ull}};
    opaque_suffix = (const void *)&suffix;
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &suffix,
                                            .timelineSemaphore = VK_TRUE,
                                            .shaderBufferInt64Atomics = VK_TRUE};
    VkPhysicalDeviceVulkan11Features v11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12, .multiview = VK_TRUE};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .pNext = &v11};
    VkPhysicalDeviceRayQueryFeaturesKHR query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &rtp};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &query};
    VkPhysicalDeviceFeatures2 core2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &as,
                                       .features = {.geometryShader = VK_TRUE, .shaderFloat64 = VK_TRUE}};
    VkLayerDeviceLink link = {0};
    VkLayerDeviceCreateInfo loader = {.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                      .pNext = &core2,
                                      .function = VK_LAYER_LINK_INFO,
                                      .u.pLayerInfo = &link};
    basic.pEnabledFeatures = NULL;
    basic.pNext = &loader;
    VkPhysicalDeviceVulkan12Features saved12 = v12;
    VkPhysicalDeviceVulkan11Features saved11 = v11;
    VkPhysicalDeviceFeatures2 saved2 = core2;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR saved_as = as;
    VkPhysicalDeviceRayQueryFeaturesKHR saved_query = query;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR saved_rtp = rtp;
    VkLayerDeviceCreateInfo saved_loader = loader;
    REQUIRE(call_non_rt(&basic) == VK_SUCCESS);
    REQUIRE(!memcmp(&v12, &saved12, sizeof(v12)) && !memcmp(&v11, &saved11, sizeof(v11)));
    REQUIRE(!memcmp(&core2, &saved2, sizeof(core2)) && !memcmp(&loader, &saved_loader, sizeof(loader)));
    REQUIRE(!memcmp(&as, &saved_as, sizeof(as)) && !memcmp(&query, &saved_query, sizeof(query)));
    REQUIRE(!memcmp(&rtp, &saved_rtp, sizeof(rtp)) && suffix.values[0] == 0x0123456789abcdefull);

    VkPhysicalDeviceBufferDeviceAddressFeatures native_bda = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .pNext = &suffix};
    basic.pNext = &native_bda;
    basic.pEnabledFeatures = &core;
    REQUIRE(call_non_rt(&basic) == VK_SUCCESS);
    REQUIRE(!native_bda.bufferDeviceAddress && native_bda.pNext == &suffix);
    downstream_result = VK_ERROR_DEVICE_LOST;
    REQUIRE(call_non_rt(&basic) == VK_ERROR_DEVICE_LOST);
}

static void virtual_requests(void) {
    defaults();
    expect_private_dynamic_indexing = true;
    VkLayerDeviceLink link = {0};
    VkLayerDeviceCreateInfo loader = {
        .sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &link};
    VkPhysicalDeviceRayQueryFeaturesKHR query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &loader, .rayQuery = VK_TRUE};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &query,
        .accelerationStructure = VK_TRUE};
    const char *extensions[] = {"VK_KHR_swapchain", VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, "VK_EXT_robustness2",
                                VK_KHR_RAY_QUERY_EXTENSION_NAME};
    expected_extensions[0] = extensions[0];
    expected_extensions[1] = extensions[3];
    expected_extension_count = 2;
    VkPhysicalDeviceFeatures core = {.samplerAnisotropy = VK_TRUE, .shaderFloat64 = VK_TRUE};
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1};
    basic.pNext = &as;
    basic.pEnabledFeatures = &core;
    basic.enabledExtensionCount = VKRT_ARRAY_SIZE(extensions);
    basic.ppEnabledExtensionNames = extensions;
    basic.queueCreateInfoCount = 1;
    basic.pQueueCreateInfos = &queue;
    VkDeviceCreateInfo saved_basic = basic;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR saved_as = as;
    VkPhysicalDeviceRayQueryFeaturesKHR saved_query = query;
    VkLayerDeviceCreateInfo saved_loader = loader;
    VkPhysicalDeviceFeatures saved_core = core;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(!memcmp(&saved_basic, &basic, sizeof(basic)) && !memcmp(&saved_as, &as, sizeof(as)));
    REQUIRE(!memcmp(&saved_query, &query, sizeof(query)) && !memcmp(&saved_loader, &loader, sizeof(loader)));
    REQUIRE(!memcmp(&saved_core, &core, sizeof(core)));
    /* The loader link node can precede a removed feature and must be copied
     * without consuming or rewriting its already-advanced layer pointer. */
    loader.pNext = &as;
    query.pNext = NULL;
    basic.pNext = &loader;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(loader.pNext == &as && loader.u.pLayerInfo == &link && as.pNext == &query && query.pNext == NULL);
    downstream_result = VK_ERROR_DEVICE_LOST;
    REQUIRE(call(&basic) == VK_ERROR_DEVICE_LOST);

    /* False native feature structs still must not reach the driver. */
    defaults();
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp = {.sType =
                                                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    as.accelerationStructure = query.rayQuery = VK_FALSE;
    as.pNext = &query;
    query.pNext = &rtp;
    basic.pNext = &as;
    REQUIRE(call_non_rt(&basic) == VK_SUCCESS);

    /* Synchronous deferred operations are layer-owned even without AS/RQ. */
    const char *deferred[] = {VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    basic.pNext = NULL;
    basic.enabledExtensionCount = 1;
    basic.ppEnabledExtensionNames = deferred;
    expected_extension_count = 0;
    REQUIRE(call_non_rt(&basic) == VK_SUCCESS);
}

static void base_pipeline_requests(void) {
    defaults();
    expect_private_dynamic_indexing = true;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR pipeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .rayTracingPipeline = VK_TRUE};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR saved_pipeline = pipeline;
    basic.pNext = &pipeline;
    REQUIRE(vkrt_device_uses_virtual_rt(&basic));
    REQUIRE(call(&basic) == VK_SUCCESS && !memcmp(&pipeline, &saved_pipeline, sizeof(pipeline)));

    /* An extension-only request must initialize the same virtual backend and
     * must not forward a native RTP extension or feature to the ICD. */
    const char *extension = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;
    basic.pNext = NULL;
    basic.ppEnabledExtensionNames = &extension;
    basic.enabledExtensionCount = 1;
    REQUIRE(vkrt_device_uses_virtual_rt(&basic));
    REQUIRE(call(&basic) == VK_SUCCESS);

    VkLayerDeviceLink link = {0};
    VkLayerDeviceCreateInfo loader = {
        .sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &link};
    pipeline.pNext = &loader;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &pipeline,
        .accelerationStructure = VK_TRUE};
    VkPhysicalDeviceRayQueryFeaturesKHR query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &as, .rayQuery = VK_TRUE};
    const char *extensions[] = {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                "VK_KHR_swapchain"};
    basic.pNext = &query;
    basic.ppEnabledExtensionNames = extensions;
    basic.enabledExtensionCount = VKRT_ARRAY_SIZE(extensions);
    expected_extensions[0] = extensions[4];
    expected_extension_count = 1;
    saved_pipeline = pipeline;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR saved_as = as;
    VkPhysicalDeviceRayQueryFeaturesKHR saved_query = query;
    VkLayerDeviceCreateInfo saved_loader = loader;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(!memcmp(&pipeline, &saved_pipeline, sizeof(pipeline)) && !memcmp(&as, &saved_as, sizeof(as)));
    REQUIRE(!memcmp(&query, &saved_query, sizeof(query)) && !memcmp(&loader, &saved_loader, sizeof(loader)));
    downstream_result = VK_ERROR_DEVICE_LOST;
    REQUIRE(call(&basic) == VK_ERROR_DEVICE_LOST);
}

static void aggregate_and_individual_features(void) {
    defaults();
    expect_private_dynamic_indexing = true;
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE,
                                            .shaderFloat16 = VK_TRUE,
                                            .descriptorIndexing = VK_TRUE};
    VkPhysicalDeviceVulkan11Features v11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .pNext = &v12,
                                            .multiview = VK_TRUE,
                                            .storagePushConstant16 = VK_TRUE};
    VkPhysicalDeviceRayQueryFeaturesKHR query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &v11, .rayQuery = VK_TRUE};
    VkPhysicalDeviceFeatures2 core = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                      .pNext = &query,
                                      .features = {.geometryShader = VK_TRUE, .shaderFloat64 = VK_TRUE}};
    basic.pNext = &core;
    VkPhysicalDeviceVulkan12Features saved12 = v12;
    VkPhysicalDeviceVulkan11Features saved11 = v11;
    VkPhysicalDeviceFeatures2 saved_core = core;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(!memcmp(&saved12, &v12, sizeof(v12)) && !memcmp(&saved11, &v11, sizeof(v11)));
    REQUIRE(!memcmp(&saved_core, &core, sizeof(core)));

    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
                                                       .bufferDeviceAddressCaptureReplay = VK_TRUE};
    VkPhysicalDeviceVulkanMemoryModelFeatures memory = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
                                                        .pNext = &bda,
                                                        .vulkanMemoryModelAvailabilityVisibilityChains = VK_TRUE};
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
                                                        .pNext = &memory};
    VkPhysicalDeviceShaderFloat16Int8Features int8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, .pNext = &scalar, .shaderFloat16 = VK_TRUE};
    VkPhysicalDevice8BitStorageFeatures storage8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, .pNext = &int8, .storagePushConstant8 = VK_TRUE};
    VkPhysicalDevice16BitStorageFeatures storage16 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, .pNext = &storage8, .storageInputOutput16 = VK_TRUE};
    core.pNext = &query;
    query.pNext = &storage16;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(!bda.bufferDeviceAddress && bda.bufferDeviceAddressCaptureReplay);
    REQUIRE(!memory.vulkanMemoryModel && memory.vulkanMemoryModelAvailabilityVisibilityChains);
    REQUIRE(!storage16.storageBuffer16BitAccess && storage16.storageInputOutput16);
}

static void versions_and_opaque_chains(void) {
    defaults();
    const char *query_extension = VK_KHR_RAY_QUERY_EXTENSION_NAME;
    basic.ppEnabledExtensionNames = &query_extension;
    basic.enabledExtensionCount = 1;
    expect_private_dynamic_indexing = true;
    static const char *required[] = {VK_KHR_16BIT_STORAGE_EXTENSION_NAME,       VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
                                     VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME,
                                     VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                                     VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME,   VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME};
    for (uint32_t instance_api = 0; instance_api < 3; ++instance_api) {
        for (uint32_t device_api = 0; device_api < 3; ++device_api) {
            instance.api_version = VK_MAKE_VERSION(1, instance_api, 0);
            driver_api = VK_MAKE_VERSION(1, device_api, 0);
            expected_extension_count = 0;
            for (uint32_t i = 0; i < VKRT_ARRAY_SIZE(required); ++i)
                if (instance_api < (i == 0 || i == 6 ? 1u : 2u) || device_api < (i == 0 || i == 6 ? 1u : 2u))
                    expected_extensions[expected_extension_count++] = required[i];
            unsigned before = creates;
            VkDeviceCreateInfo saved = basic;
            VkResult result = call(&basic);
            if (device_api < 2)
                REQUIRE(result == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
            else
                REQUIRE(result == VK_SUCCESS);
            REQUIRE(!memcmp(&basic, &saved, sizeof(basic)));
        }
    }
    /* A requested alias is not duplicated when private requirements add it. */
    instance.api_version = VK_API_VERSION_1_0;
    driver_api = VK_API_VERSION_1_2;
    const char *extensions[] = {required[5], "VK_KHR_swapchain", VK_KHR_RAY_QUERY_EXTENSION_NAME};
    basic.ppEnabledExtensionNames = extensions;
    basic.enabledExtensionCount = 3;
    expected_extensions[0] = required[5];
    expected_extensions[1] = extensions[1];
    expected_extension_count = 2;
    for (uint32_t i = 0; i < VKRT_ARRAY_SIZE(required); ++i)
        if (i != 5) expected_extensions[expected_extension_count++] = required[i];
    REQUIRE(call(&basic) == VK_SUCCESS);

    defaults();
    struct {
        VkStructureType sType;
        const void *pNext;
        uint64_t payload[2];
    } suffix = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM, .payload = {0x123456789abcdef0ull, 0xfedcba9876543210ull}};
    opaque_suffix = (const void *)&suffix;
    VkPhysicalDeviceRayQueryFeaturesKHR query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &suffix, .rayQuery = VK_TRUE};
    basic.pNext = &query;
    expect_private_dynamic_indexing = true;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(query.pNext == &suffix && suffix.payload[0] == 0x123456789abcdef0ull);
    basic.pNext = &suffix;
    expect_private_dynamic_indexing = false;
    REQUIRE(call_non_rt(&basic) == VK_SUCCESS);
    suffix.pNext = &query;
    query.pNext = NULL;
    unsigned before = creates;
    REQUIRE(call(&basic) == VK_ERROR_EXTENSION_NOT_PRESENT && creates == before);
    REQUIRE(suffix.pNext == &query && query.pNext == NULL);
}

static void descriptor_array_feature(void) {
    defaults();
    VkPhysicalDeviceFeatures core = {.samplerAnisotropy = VK_TRUE};
    basic.pEnabledFeatures = &core;
    /* A non-RT request neither requires nor privately enables this feature. */
    missing_capability = 10;
    REQUIRE(call_non_rt(&basic) == VK_SUCCESS);
    REQUIRE(!core.shaderStorageBufferArrayDynamicIndexing);

    const char *extension = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
    basic.enabledExtensionCount = 1;
    basic.ppEnabledExtensionNames = &extension;
    expect_private_dynamic_indexing = true;
    unsigned before = creates;
    REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
    REQUIRE(!core.shaderStorageBufferArrayDynamicIndexing);

    /* Legacy core features and Features2 both receive a private cloned bit. */
    missing_capability = 0;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(!core.shaderStorageBufferArrayDynamicIndexing);
    VkPhysicalDeviceFeatures2 core2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .features = core};
    basic.pNext = &core2;
    basic.pEnabledFeatures = NULL;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(!core2.features.shaderStorageBufferArrayDynamicIndexing);

    /* A base ray-query feature is sufficient to trigger the same requirement,
     * even when the caller did not repeat the extension name in this fixture. */
    VkPhysicalDeviceRayQueryFeaturesKHR query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
                                                 .rayQuery = VK_TRUE};
    core2.pNext = &query;
    basic.enabledExtensionCount = 0;
    missing_capability = 10;
    before = creates;
    REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
    REQUIRE(!core2.features.shaderStorageBufferArrayDynamicIndexing && query.rayQuery);

    missing_capability = 11;
    REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
    REQUIRE(!core2.features.shaderStorageBufferArrayDynamicIndexing && query.rayQuery);
    missing_capability = 12;
    REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
    REQUIRE(!core2.features.shaderStorageBufferArrayDynamicIndexing && query.rayQuery);

    /* Edit an existing individual descriptor-indexing struct without enabling
     * its unrelated options from the query or mutating the caller's payload. */
    missing_capability = 0;
    VkPhysicalDeviceDescriptorIndexingFeatures descriptor = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
        .pNext = &query,
        .descriptorBindingPartiallyBound = VK_TRUE};
    core2.pNext = &descriptor;
    VkPhysicalDeviceDescriptorIndexingFeatures saved_descriptor = descriptor;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(!memcmp(&descriptor, &saved_descriptor, sizeof(descriptor)));
    REQUIRE(core2.pNext == &descriptor && !core2.features.shaderStorageBufferArrayDynamicIndexing);
    VkPhysicalDeviceVariablePointersFeatures pointers = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, .pNext = &descriptor, .variablePointers = VK_TRUE};
    core2.pNext = &pointers;
    VkPhysicalDeviceVariablePointersFeatures saved_pointers = pointers;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(!memcmp(&pointers, &saved_pointers, sizeof(pointers)));

    /* Both the application and physical-device version participate in alias
     * promotion. A private descriptor-indexing feature needs its EXT name
     * below core 1.2, including when that alias was already requested. */
    const char *required[] = {VK_KHR_16BIT_STORAGE_EXTENSION_NAME,       VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
                              VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME,
                              VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                              VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME,   VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME};
    for (uint32_t application_version = 0; application_version < 3; ++application_version) {
        for (uint32_t device_version = 0; device_version < 3; ++device_version) {
            instance.api_version = VK_MAKE_VERSION(1, application_version, 0);
            driver_api = VK_MAKE_VERSION(1, device_version, 0);
            expected_extension_count = 0;
            for (unsigned i = 0; i < VKRT_ARRAY_SIZE(required); ++i)
                if (application_version < (i == 0 || i == 6 ? 1u : 2u) || device_version < (i == 0 || i == 6 ? 1u : 2u))
                    expected_extensions[expected_extension_count++] = required[i];
            unsigned before = creates;
            VkResult result = call(&basic);
            if (device_version < 2)
                REQUIRE(result == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
            else
                REQUIRE(result == VK_SUCCESS);
            REQUIRE(!memcmp(&pointers, &saved_pointers, sizeof(pointers)));
        }
    }
    instance.api_version = VK_API_VERSION_1_1;
    driver_api = VK_API_VERSION_1_2;
    const char *descriptor_alias = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
    basic.ppEnabledExtensionNames = &descriptor_alias;
    basic.enabledExtensionCount = 1;
    expected_extensions[0] = descriptor_alias;
    expected_extension_count = 1;
    for (unsigned i = 1; i < 6; ++i) expected_extensions[expected_extension_count++] = required[i];
    REQUIRE(call(&basic) == VK_SUCCESS);

    instance.api_version = VK_API_VERSION_1_0;
    const char *pointers_alias = VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME;
    basic.ppEnabledExtensionNames = &pointers_alias;
    expected_extensions[0] = pointers_alias;
    expected_extension_count = 1;
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(required); ++i)
        if (i != 6) expected_extensions[expected_extension_count++] = required[i];
    REQUIRE(call(&basic) == VK_SUCCESS);
}

static void builder_property_prerequisites(void) {
    defaults();
    struct {
        VkStructureType sType;
        const void *pNext;
        uint64_t payload;
    } suffix = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM, .payload = UINT64_C(0x123456789abcdef0)};
    VkPhysicalDeviceRayQueryFeaturesKHR query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &suffix, .rayQuery = VK_TRUE};
    VkPhysicalDeviceFeatures core = {.samplerAnisotropy = VK_TRUE};
    basic.pNext = &query;
    basic.pEnabledFeatures = &core;
    opaque_suffix = (const void *)&suffix;
    expect_private_dynamic_indexing = true;
    const VkDeviceCreateInfo saved = basic;
    const VkPhysicalDeviceRayQueryFeaturesKHR saved_query = query;
    const VkPhysicalDeviceFeatures saved_core = core;
    for (missing_capability = 13; missing_capability <= 26; ++missing_capability) {
        unsigned before = creates;
        REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
        REQUIRE(!memcmp(&basic, &saved, sizeof(basic)) && !memcmp(&query, &saved_query, sizeof(query)) &&
                !memcmp(&core, &saved_core, sizeof(core)) && suffix.payload == UINT64_C(0x123456789abcdef0));
    }
    missing_capability = 0;
    /* The mock defaults are the selected kernels' exact workgroup bounds. */
    REQUIRE(call(&basic) == VK_SUCCESS);
    unsigned before = creates, before_queries = queries;
    instance.vtable.GetPhysicalDeviceProperties2 = NULL;
    REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before && queries == before_queries);
    REQUIRE(!memcmp(&basic, &saved, sizeof(basic)) && !memcmp(&query, &saved_query, sizeof(query)) &&
            !memcmp(&core, &saved_core, sizeof(core)));
}

static void rejection(void) {
    defaults();
    const char *rt_extension = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
    basic.ppEnabledExtensionNames = &rt_extension;
    basic.enabledExtensionCount = 1;
    expect_private_dynamic_indexing = true;
    for (missing_capability = 1; missing_capability <= 9; ++missing_capability) {
        unsigned before = creates;
        REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
    }
    missing_capability = 0;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    basic.pNext = &as;
    VkBool32 *optional[] = {&as.accelerationStructureCaptureReplay, &as.accelerationStructureIndirectBuild,
                            &as.accelerationStructureHostCommands, &as.descriptorBindingAccelerationStructureUpdateAfterBind};
    for (uint32_t i = 0; i < VKRT_ARRAY_SIZE(optional); ++i) {
        unsigned before = creates, query_count = queries;
        *optional[i] = VK_TRUE;
        REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before && queries == query_count);
        *optional[i] = VK_FALSE;
    }
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp = {.sType =
                                                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    basic.pNext = &rtp;
    rtp.rayTracingPipeline = VK_TRUE;
    VkBool32 *pipeline[] = {&rtp.rayTracingPipelineShaderGroupHandleCaptureReplay,
                            &rtp.rayTracingPipelineShaderGroupHandleCaptureReplayMixed, &rtp.rayTracingPipelineTraceRaysIndirect,
                            &rtp.rayTraversalPrimitiveCulling};
    for (uint32_t i = 0; i < VKRT_ARRAY_SIZE(pipeline); ++i) {
        unsigned before = creates;
        *pipeline[i] = VK_TRUE;
        REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
        *pipeline[i] = VK_FALSE;
    }
#ifdef VK_EXT_pipeline_library_group_handles
    VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT groups = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_LIBRARY_GROUP_HANDLES_FEATURES_EXT,
        .pipelineLibraryGroupHandles = VK_TRUE};
    basic.pNext = &groups;
    unsigned before = creates;
    REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
    groups.pipelineLibraryGroupHandles = VK_FALSE;
    REQUIRE(call(&basic) == VK_SUCCESS);
#endif
    basic.pNext = NULL;
    const char *unsupported[] = {"VK_KHR_ray_tracing_pipeline_unsupported",
                                 "VK_KHR_ray_tracing_maintenance1",
                                 "VK_KHR_ray_tracing_position_fetch",
                                 "VK_NV_ray_tracing",
                                 "VK_EXT_opacity_micromap",
                                 VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                                 "VK_EXT_pipeline_library_group_handles",
                                 "VK_EXT_ray_tracing_invocation_reorder",
                                 "VK_HUAWEI_invocation_mask",
                                 "VK_KHR_opacity_micromap",
                                 "VK_ARM_pipeline_opacity_micromap",
                                 "VK_NV_cluster_acceleration_structure"};
    for (uint32_t i = 0; i < VKRT_ARRAY_SIZE(unsupported); ++i) {
        basic.enabledExtensionCount = 1;
        basic.ppEnabledExtensionNames = &unsupported[i];
        unsigned before = creates;
        REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before);
    }
    defaults();
    instance.vtable.GetPhysicalDeviceFeatures2 = NULL;
    basic.ppEnabledExtensionNames = &rt_extension;
    basic.enabledExtensionCount = 1;
    REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT);
}

static void per_stage_descriptor_interaction(void) {
    defaults();
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .rayTracingPipeline = VK_TRUE};
    struct {
        VkStructureType sType;
        void *pNext;
        uint64_t sentinel;
    } suffix = {VK_STRUCTURE_TYPE_MAX_ENUM, NULL, UINT64_C(0xabcddcba01234567)};
    VkPhysicalDevicePerStageDescriptorSetFeaturesNV per_stage = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV,
        .pNext = &rt,
        .perStageDescriptorSet = VK_TRUE,
        .dynamicPipelineLayout = VK_TRUE};
    rt.pNext = &suffix;
    basic.pNext = &per_stage;
    unsigned before = creates, query_count = queries;
    VkPhysicalDevicePerStageDescriptorSetFeaturesNV saved = per_stage;
    REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT && creates == before && queries == query_count);
    REQUIRE(!memcmp(&per_stage, &saved, sizeof(saved)) && rt.pNext == &suffix);
    per_stage.perStageDescriptorSet = VK_FALSE;
    saved = per_stage;
    expect_private_dynamic_indexing = true;
    opaque_suffix = (const void *)&suffix;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(!memcmp(&per_stage, &saved, sizeof(saved)) && suffix.sentinel == UINT64_C(0xabcddcba01234567));
    /* An ordinary device keeps both unrelated native feature bits. */
    defaults();
    per_stage.perStageDescriptorSet = VK_TRUE;
    per_stage.pNext = &suffix;
    basic.pNext = &per_stage;
    opaque_suffix = (const void *)&suffix;
    REQUIRE(call_non_rt(&basic) == VK_SUCCESS);
    REQUIRE(per_stage.perStageDescriptorSet && per_stage.dynamicPipelineLayout && per_stage.pNext == &suffix);
}

int main(void) {
    non_rt_boundary();
    virtual_requests();
    base_pipeline_requests();
    aggregate_and_individual_features();
    versions_and_opaque_chains();
    descriptor_array_feature();
    builder_property_prerequisites();
    rejection();
    per_stage_descriptor_interaction();
    printf("No-native-RT device creation boundary: %s (%u failures, %u creates, %u capability queries)\n",
           failures ? "FAIL" : "PASS", failures, creates, queries);
    return failures ? 1 : 0;
}
