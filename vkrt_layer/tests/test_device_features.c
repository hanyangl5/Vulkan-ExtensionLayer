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
static unsigned failures, creates, queries;
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

static const VkBaseInStructure *find_original(VkStructureType type) {
    for (const VkBaseInStructure *p = original->pNext; p; p = p->pNext)
        if (p->sType == type) return p;
    return NULL;
}

/* This boundary has no RT extensions or features. The query writes unrelated
 * supported options as true to catch accidental enabling from query results. */
static VKAPI_ATTR void VKAPI_CALL mock_features(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *features) {
    REQUIRE(p == physical);
    ++queries;
    features->features.shaderInt64 = missing_capability != 1;
    features->features.shaderInt16 = missing_capability != 2;
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
            default:
                REQUIRE(false);
                break;
        }
    }
    REQUIRE(seen == 6);
}

static VKAPI_ATTR void VKAPI_CALL mock_properties(VkPhysicalDevice p, VkPhysicalDeviceProperties *properties) {
    REQUIRE(p == physical);
    memset(properties, 0, sizeof(*properties));
    properties->apiVersion = driver_api;
}

static void check_core(VkPhysicalDeviceFeatures actual, VkPhysicalDeviceFeatures expected) {
    expected.shaderInt64 = expected.shaderInt16 = VK_TRUE;
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
                EXPECT_NODE(VkPhysicalDeviceVulkan11Features, next, expected.storageBuffer16BitAccess = VK_TRUE);
                ++storage16;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
                EXPECT_NODE(
                    VkPhysicalDeviceVulkan12Features, next,
                    expected.storageBuffer8BitAccess = expected.shaderInt8 = expected.scalarBlockLayout = VK_TRUE;
                    expected.vulkanMemoryModel = expected.vulkanMemoryModelDeviceScope = expected.bufferDeviceAddress = VK_TRUE);
                ++storage8;
                ++int8;
                ++scalar;
                ++memory;
                ++bda;
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
    if (downstream_result == VK_SUCCESS) *device = created_device;
    return downstream_result;
}
#undef EXPECT_NODE

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

static void defaults(void) {
    memset(&instance, 0, sizeof(instance));
    instance.api_version = VK_API_VERSION_1_2;
    instance.vtable.GetPhysicalDeviceFeatures2 = mock_features;
    instance.vtable.GetPhysicalDeviceProperties = mock_properties;
    driver_api = VK_API_VERSION_1_2;
    basic = (VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    expected_extension_count = 0;
    opaque_suffix = NULL;
    missing_capability = 0;
    downstream_result = VK_SUCCESS;
}

static void virtual_requests(void) {
    defaults();
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
    REQUIRE(call(&basic) == VK_SUCCESS);

    /* Deferred operations without emulated AS/RQ remain a native request. */
    const char *deferred[] = {VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    basic.pNext = NULL;
    basic.enabledExtensionCount = 1;
    basic.ppEnabledExtensionNames = deferred;
    expected_extensions[0] = deferred[0];
    expected_extension_count = 1;
    REQUIRE(call(&basic) == VK_SUCCESS);
}

static void aggregate_and_individual_features(void) {
    defaults();
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
    core.pNext = &storage16;
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(!bda.bufferDeviceAddress && bda.bufferDeviceAddressCaptureReplay);
    REQUIRE(!memory.vulkanMemoryModel && memory.vulkanMemoryModelAvailabilityVisibilityChains);
    REQUIRE(!storage16.storageBuffer16BitAccess && storage16.storageInputOutput16);
}

static void versions_and_opaque_chains(void) {
    defaults();
    static const char *required[] = {VK_KHR_16BIT_STORAGE_EXTENSION_NAME,       VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
                                     VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME,
                                     VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};
    for (uint32_t instance_api = 0; instance_api < 3; ++instance_api) {
        for (uint32_t device_api = 0; device_api < 3; ++device_api) {
            instance.api_version = VK_MAKE_VERSION(1, instance_api, 0);
            driver_api = VK_MAKE_VERSION(1, device_api, 0);
            expected_extension_count = 0;
            for (uint32_t i = 0; i < VKRT_ARRAY_SIZE(required); ++i)
                if (instance_api < (i ? 2u : 1u) || device_api < (i ? 2u : 1u))
                    expected_extensions[expected_extension_count++] = required[i];
            REQUIRE(call(&basic) == VK_SUCCESS);
        }
    }
    /* A requested alias is not duplicated when private requirements add it. */
    instance.api_version = VK_API_VERSION_1_0;
    driver_api = VK_API_VERSION_1_2;
    const char *extensions[] = {required[5], "VK_KHR_swapchain"};
    basic.ppEnabledExtensionNames = extensions;
    basic.enabledExtensionCount = 2;
    expected_extensions[0] = required[5];
    expected_extensions[1] = extensions[1];
    for (uint32_t i = 0; i < 5; ++i) expected_extensions[2 + i] = required[i];
    expected_extension_count = 7;
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
    REQUIRE(call(&basic) == VK_SUCCESS);
    REQUIRE(query.pNext == &suffix && suffix.payload[0] == 0x123456789abcdef0ull);
    basic.pNext = &suffix;
    REQUIRE(call(&basic) == VK_SUCCESS);
    suffix.pNext = &query;
    query.pNext = NULL;
    unsigned before = creates;
    REQUIRE(call(&basic) == VK_ERROR_EXTENSION_NOT_PRESENT && creates == before);
    REQUIRE(suffix.pNext == &query && query.pNext == NULL);
}

static void rejection(void) {
    defaults();
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
    VkBool32 *pipeline[] = {&rtp.rayTracingPipeline, &rtp.rayTracingPipelineShaderGroupHandleCaptureReplay,
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
    const char *unsupported[] = {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
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
    REQUIRE(call(&basic) == VK_ERROR_FEATURE_NOT_PRESENT);
}

int main(void) {
    virtual_requests();
    aggregate_and_individual_features();
    versions_and_opaque_chains();
    rejection();
    printf("No-native-RT device creation boundary: %s (%u failures, %u creates, %u capability queries)\n",
           failures ? "FAIL" : "PASS", failures, creates, queries);
    return failures ? 1 : 0;
}
