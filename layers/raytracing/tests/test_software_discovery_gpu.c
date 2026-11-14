/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <vulkan/vulkan.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic unsigned validation_errors;
#define REQUIRE(x)                                               \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)
#define CHECK(x) REQUIRE((x) == VK_SUCCESS)
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT type,
                                                 const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        atomic_fetch_add(&validation_errors, 1);
        fprintf(stderr, "Validation Error: %s\n", data->pMessage);
    }
    return VK_FALSE;
}
static const char *extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                                   VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME};
static const char *commands[] = {"vkCreateAccelerationStructureKHR",
                                 "vkDestroyAccelerationStructureKHR",
                                 "vkCmdBuildAccelerationStructuresKHR",
                                 "vkCmdBuildAccelerationStructuresIndirectKHR",
                                 "vkBuildAccelerationStructuresKHR",
                                 "vkCopyAccelerationStructureKHR",
                                 "vkCopyAccelerationStructureToMemoryKHR",
                                 "vkCopyMemoryToAccelerationStructureKHR",
                                 "vkWriteAccelerationStructuresPropertiesKHR",
                                 "vkCmdCopyAccelerationStructureKHR",
                                 "vkCmdCopyAccelerationStructureToMemoryKHR",
                                 "vkCmdCopyMemoryToAccelerationStructureKHR",
                                 "vkGetAccelerationStructureDeviceAddressKHR",
                                 "vkCmdWriteAccelerationStructuresPropertiesKHR",
                                 "vkGetDeviceAccelerationStructureCompatibilityKHR",
                                 "vkGetAccelerationStructureBuildSizesKHR",
                                 "vkCmdTraceRaysKHR",
                                 "vkCreateRayTracingPipelinesKHR",
                                 "vkGetRayTracingShaderGroupHandlesKHR",
                                 "vkGetRayTracingCaptureReplayShaderGroupHandlesKHR",
                                 "vkCmdTraceRaysIndirectKHR",
                                 "vkGetRayTracingShaderGroupStackSizeKHR",
                                 "vkCmdSetRayTracingPipelineStackSizeKHR",
                                 "vkCreateDeferredOperationKHR",
                                 "vkDestroyDeferredOperationKHR",
                                 "vkGetDeferredOperationMaxConcurrencyKHR",
                                 "vkGetDeferredOperationResultKHR",
                                 "vkDeferredOperationJoinKHR"};
static bool extension_support(VkPhysicalDevice physical, uint32_t needed_count, const char *const *needed) {
    uint32_t count = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *found = calloc(count ? count : 1, sizeof(*found));
    REQUIRE(found);
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, found));
    bool eligible = true;
    for (unsigned n = 0; n < needed_count; ++n) {
        unsigned matches = 0;
        for (uint32_t i = 0; i < count; ++i) matches += !strcmp(found[i].extensionName, needed[n]);
        REQUIRE(matches <= 1);
        eligible &= matches == 1;
    }
    free(found);
    return eligible;
}
int main(int argc, char **argv) {
    bool software_abi = false, vulkan11 = false, as_only = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--software-abi"))
            software_abi = true;
        else if (!strcmp(argv[i], "--vulkan11"))
            vulkan11 = true;
        else if (!strcmp(argv[i], "--as-only"))
            as_only = true;
        else
            REQUIRE(false);
    }
    const char *requested[12];
    uint32_t requested_count = 0;
    for (unsigned i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        if (!as_only || i == 0 || i == 3) requested[requested_count++] = extensions[i];
    if (vulkan11) {
        requested[requested_count++] = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
        requested[requested_count++] = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
        if (!as_only) {
            requested[requested_count++] = VK_KHR_SPIRV_1_4_EXTENSION_NAME;
            requested[requested_count++] = VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME;
        }
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "public RT discovery",
                             .apiVersion = vulkan11 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_2};
    const char *debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                                                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                                                .pfnUserCallback = validation};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pNext = &debug,
                               .pApplicationInfo = &app,
                               .enabledExtensionCount = 1,
                               .ppEnabledExtensionNames = &debug_extension};
    VkInstance instance;
    CHECK(vkCreateInstance(&ci, NULL, &instance));
    PFN_vkCreateDebugUtilsMessengerEXT create_debug = (void *)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug = (void *)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    REQUIRE(create_debug && destroy_debug);
    VkDebugUtilsMessengerEXT messenger;
    CHECK(create_debug(instance, &debug, NULL, &messenger));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    VkPhysicalDevice *devices = calloc(count ? count : 1, sizeof(*devices));
    REQUIRE(devices);
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count && !physical; ++i) {
        if (!extension_support(devices[i], requested_count, requested)) continue;
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &rtp};
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &as};
        vkGetPhysicalDeviceFeatures2(devices[i], &features);
        if (!as.accelerationStructure || (!as_only && !rtp.rayTracingPipeline)) continue;
        VkFormatProperties format;
        vkGetPhysicalDeviceFormatProperties(devices[i], VK_FORMAT_R32G32B32_SFLOAT, &format);
        if (!(format.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR)) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, NULL);
        VkQueueFamilyProperties *q = calloc(families ? families : 1, sizeof(*q));
        REQUIRE(q);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, q);
        for (uint32_t n = 0; n < families; ++n)
            if (q[n].queueCount && (q[n].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                physical = devices[i];
                family = n;
                break;
            }
        free(q);
    }
    free(devices);
    if (!physical) {
        destroy_debug(instance, messenger, NULL);
        vkDestroyInstance(instance, NULL);
        printf("Public RT discovery: no qualifying advertised device (not a fallback pass)\n");
        return 77;
    }
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR, .pNext = as_only ? NULL : &rt};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &as};
    vkGetPhysicalDeviceProperties2(physical, &props);
    REQUIRE(as.maxGeometryCount >= 0xffffff && as.maxInstanceCount >= 0xffffff && as.maxPrimitiveCount >= 0x1fffffff);
    REQUIRE(as.maxPerStageDescriptorAccelerationStructures >= 16 && as.maxDescriptorSetAccelerationStructures >= 16);
    REQUIRE(as.minAccelerationStructureScratchOffsetAlignment && as.minAccelerationStructureScratchOffsetAlignment <= 256);
    REQUIRE(!(as.minAccelerationStructureScratchOffsetAlignment & (as.minAccelerationStructureScratchOffsetAlignment - 1)));
    if (!as_only)
        REQUIRE(rt.shaderGroupHandleSize == 32 && rt.maxRayRecursionDepth >= 1 && rt.maxShaderGroupStride >= 4096 &&
                rt.shaderGroupBaseAlignment && rt.shaderGroupBaseAlignment <= 64 &&
                rt.maxRayDispatchInvocationCount >= (1u << 30) && rt.shaderGroupHandleAlignment &&
                rt.shaderGroupHandleAlignment <= 32 && rt.maxRayHitAttributeSize >= 32);
    if (software_abi && !as_only)
        REQUIRE(rt.maxRayRecursionDepth == 31 && rt.maxShaderGroupStride == 4096 && rt.shaderGroupBaseAlignment == 64 &&
                rt.shaderGroupHandleAlignment == 32 && !rt.shaderGroupHandleCaptureReplaySize && rt.maxRayHitAttributeSize == 32);
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enable_rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .rayTracingPipeline = VK_TRUE};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enable_as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = as_only ? NULL : &enable_rt,
        .accelerationStructure = VK_TRUE};
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = family,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                      .pNext = &enable_as,
                                      .queueCreateInfoCount = 1,
                                      .pQueueCreateInfos = &queue,
                                      .enabledExtensionCount = requested_count,
                                      .ppEnabledExtensionNames = requested};
    VkDevice device;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (as_only && i >= 16 && i < 23) continue;
        if (!vkGetDeviceProcAddr(device, commands[i])) {
            fprintf(stderr, "Missing enabled-extension command: %s\n", commands[i]);
            exit(1);
        }
    }
    vkDestroyDevice(device, NULL);
    destroy_debug(instance, messenger, NULL);
    vkDestroyInstance(instance, NULL);
    REQUIRE(!atomic_load(&validation_errors));
    printf(
        "Public RT discovery/device creation/%u extension entry points: PASS (device=%s, API=%s, AS-only=%u, validation "
        "errors=0)\n",
        as_only ? 21u : 28u, props.properties.deviceName, vulkan11 ? "1.1" : "1.2", as_only);
    return 0;
}
