/* SPDX-License-Identifier: MIT
 * Public loader/device proof for the CPU-only deferred-operation dependency.
 * No BDA, AS, ray-query or native ray-tracing feature/extension is requested.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <vulkan/vulkan.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *software_layer = "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY";
static const char *validation_layer = "VK_LAYER_KHRONOS_validation";
static atomic_uint validation_errors;
static unsigned failures;
#define CHECK(condition)                                            \
    do {                                                            \
        if (!(condition)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition); \
            ++failures;                                             \
        }                                                           \
    } while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT types,
                                                    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)types;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        atomic_fetch_add_explicit(&validation_errors, 1, memory_order_relaxed);
    fprintf(stderr, "validation: %s\n", data->pMessage);
    return VK_FALSE;
}

static bool deferred_enumerated(VkPhysicalDevice physical, const char *layer) {
    uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(physical, layer, &count, NULL);
    CHECK(result == VK_SUCCESS);
    if (result != VK_SUCCESS) return false;
    VkExtensionProperties *extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) return false;
    result = vkEnumerateDeviceExtensionProperties(physical, layer, &count, extensions);
    CHECK(result == VK_SUCCESS);
    unsigned found = 0;
    for (uint32_t i = 0; result == VK_SUCCESS && i < count; ++i)
        if (!strcmp(extensions[i].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
            ++found;
            CHECK(extensions[i].specVersion >= VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION);
        }
    free(extensions);
    CHECK(found == 1);
    return found == 1;
}

static bool ordinary_buffer(VkPhysicalDevice physical, VkDevice device) {
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = 256,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult result = vkCreateBuffer(device, &info, NULL, &buffer);
    CHECK(result == VK_SUCCESS);
    if (result != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    uint32_t type = UINT32_MAX;
    bool mapped_type = false;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if (!(requirements.memoryTypeBits & (1u << i))) continue;
        if (type == UINT32_MAX) type = i;
        VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            mapped_type = true;
            break;
        }
    }
    CHECK(type != UINT32_MAX);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (type != UINT32_MAX) {
        /* No allocation flags or pNext, especially no DEVICE_ADDRESS bit. */
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type};
        result = vkAllocateMemory(device, &allocation, NULL, &memory);
        CHECK(result == VK_SUCCESS);
        if (result == VK_SUCCESS) {
            result = vkBindBufferMemory(device, buffer, memory, 0);
            CHECK(result == VK_SUCCESS);
            if (result == VK_SUCCESS && mapped_type) {
                void *mapping = NULL;
                result = vkMapMemory(device, memory, 0, 256, 0, &mapping);
                CHECK(result == VK_SUCCESS && mapping);
                if (result == VK_SUCCESS) {
                    memset(mapping, 0x5a, 256);
                    vkUnmapMemory(device, memory);
                }
            }
        }
    } else {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    vkDestroyBuffer(device, buffer, NULL);
    if (memory) vkFreeMemory(device, memory, NULL);
    return result == VK_SUCCESS;
}

static bool ordinary_all_stage_push(VkDevice device, uint32_t family) {
    VkPushConstantRange range = {VK_SHADER_STAGE_ALL, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult result = vkCreatePipelineLayout(device, &layout_info, NULL, &layout);
    CHECK(result == VK_SUCCESS);
    if (result != VK_SUCCESS) return false;
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool pool = VK_NULL_HANDLE;
    result = vkCreateCommandPool(device, &pool_info, NULL, &pool);
    CHECK(result == VK_SUCCESS);
    if (result == VK_SUCCESS) {
        VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .commandPool = pool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1};
        VkCommandBuffer command = VK_NULL_HANDLE;
        result = vkAllocateCommandBuffers(device, &allocate, &command);
        CHECK(result == VK_SUCCESS);
        if (result == VK_SUCCESS) {
            VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            result = vkBeginCommandBuffer(command, &begin);
            CHECK(result == VK_SUCCESS);
            if (result == VK_SUCCESS) {
                const uint32_t value = 0x12345678;
                /* ALL is legal without enabling each stage's feature. The
                 * implicit inclusion of RT bits must not activate emulation. */
                vkCmdPushConstants(command, layout, VK_SHADER_STAGE_ALL, 0, sizeof(value), &value);
                result = vkEndCommandBuffer(command);
                CHECK(result == VK_SUCCESS);
                if (result == VK_SUCCESS) {
                    VkQueue queue = VK_NULL_HANDLE;
                    vkGetDeviceQueue(device, family, 0, &queue);
                    VkSubmitInfo submit = {
                        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
                    result = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
                    CHECK(result == VK_SUCCESS);
                    if (result == VK_SUCCESS) {
                        result = vkQueueWaitIdle(queue);
                        CHECK(result == VK_SUCCESS);
                    }
                }
            }
        }
        vkDestroyCommandPool(device, pool, NULL);
    }
    vkDestroyPipelineLayout(device, layout, NULL);
    printf("Ordinary ALL-stage push on featureless device: %s\n", result == VK_SUCCESS ? "PASS" : "FAIL");
    return result == VK_SUCCESS;
}

static bool check_device(VkPhysicalDevice physical) {
    if (!deferred_enumerated(physical, NULL) || !deferred_enumerated(physical, software_layer)) return false;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count ? count : 1, sizeof(*families));
    if (!families) return false;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    while (family < count &&
           (!families[family].queueCount || !(families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))))
        ++family;
    free(families);
    CHECK(family < count);
    if (family == count) return false;
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = family,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    const char *extensions[] = {VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    /* Deliberately Vulkan1.0-compatible and entirely featureless. In
     * particular, do not include any BDA/storage/int64 feature structure. */
    const VkPhysicalDeviceFeatures features = {0};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &queue,
                               .enabledExtensionCount = 1,
                               .ppEnabledExtensionNames = extensions,
                               .pEnabledFeatures = &features};
    VkDevice device = VK_NULL_HANDLE;
    VkResult created = vkCreateDevice(physical, &info, NULL, &device);
    CHECK(created == VK_SUCCESS);
    if (created != VK_SUCCESS) {
        fprintf(stderr, "deferred-only vkCreateDevice returned %d\n", created);
        return false;
    }
    PFN_vkCreateDeferredOperationKHR create =
        (PFN_vkCreateDeferredOperationKHR)vkGetDeviceProcAddr(device, "vkCreateDeferredOperationKHR");
    PFN_vkDestroyDeferredOperationKHR destroy =
        (PFN_vkDestroyDeferredOperationKHR)vkGetDeviceProcAddr(device, "vkDestroyDeferredOperationKHR");
    PFN_vkGetDeferredOperationMaxConcurrencyKHR concurrency =
        (PFN_vkGetDeferredOperationMaxConcurrencyKHR)vkGetDeviceProcAddr(device, "vkGetDeferredOperationMaxConcurrencyKHR");
    PFN_vkGetDeferredOperationResultKHR result =
        (PFN_vkGetDeferredOperationResultKHR)vkGetDeviceProcAddr(device, "vkGetDeferredOperationResultKHR");
    PFN_vkDeferredOperationJoinKHR join = (PFN_vkDeferredOperationJoinKHR)vkGetDeviceProcAddr(device, "vkDeferredOperationJoinKHR");
    CHECK(create && destroy && concurrency && result && join);
    bool complete = create && destroy && concurrency && result && join;
    CHECK(ordinary_buffer(physical, device));
    CHECK(ordinary_all_stage_push(device, family));
    if (complete) {
        VkDeferredOperationKHR operations[8] = {0};
        for (unsigned i = 0; i < 8; ++i) {
            VkResult code = create(device, NULL, &operations[i]);
            CHECK(code == VK_SUCCESS && operations[i] != VK_NULL_HANDLE);
            complete &= code == VK_SUCCESS && operations[i] != VK_NULL_HANDLE;
        }
        for (unsigned i = 0; i < 8; ++i) {
            if (!operations[i]) continue;
            CHECK(concurrency(device, operations[i]) == 0);
            CHECK(result(device, operations[i]) == VK_SUCCESS);
            CHECK(join(device, operations[i]) == VK_SUCCESS);
            CHECK(result(device, operations[i]) == VK_SUCCESS);
        }
        for (unsigned i = 0; i < 8; ++i)
            if (operations[i]) destroy(device, operations[i], NULL);
        destroy(device, VK_NULL_HANDLE, NULL);
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    printf("%s: deferred-only device, zero enabled features, five owned APIs %s\n", properties.deviceName,
           complete && !failures ? "PASS" : "FAIL");
    vkDestroyDevice(device, NULL);
    return complete;
}

int main(int argc, char **argv) {
    bool validation = argc == 2;
    bool above = validation && !strcmp(argv[1], "--validation-above");
    bool below = validation && !strcmp(argv[1], "--validation-below");
    if (argc > 2 || (validation && !above && !below)) {
        fprintf(stderr, "usage: %s [--validation-above|--validation-below]\n", argv[0]);
        return 1;
    }
    if (validation) {
        /* Environment-forced layers precede ppEnabledLayerNames. Clear only
         * this process's override so the two explicit test orders are exact. */
#if defined(_WIN32)
        if (_putenv_s("VK_INSTANCE_LAYERS", "") != 0) return 1;
#else
        if (unsetenv("VK_INSTANCE_LAYERS") != 0) return 1;
#endif
    }
    const char *layers[2] = {software_layer, validation_layer};
    if (above) {
        layers[0] = validation_layer;
        layers[1] = software_layer;
    }
    const char *instance_extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkDebugUtilsMessengerCreateInfoEXT debug = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_message};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "VKRT deferred-only device proof",
                             .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pNext = validation ? &debug : NULL,
                                 .pApplicationInfo = &app,
                                 .enabledLayerCount = validation ? 2 : 1,
                                 .ppEnabledLayerNames = layers,
                                 .enabledExtensionCount = validation ? 1 : 0,
                                 .ppEnabledExtensionNames = validation ? instance_extensions : NULL};
    VkInstance instance = VK_NULL_HANDLE;
    VkResult created = vkCreateInstance(&info, NULL, &instance);
    CHECK(created == VK_SUCCESS);
    if (created != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance returned %d\n", created);
        return 1;
    }
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug = NULL;
    if (validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create_debug =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        destroy_debug = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        CHECK(create_debug && destroy_debug);
        if (create_debug && destroy_debug) CHECK(create_debug(instance, &debug, NULL, &messenger) == VK_SUCCESS);
    }
    uint32_t count = 0;
    VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count, NULL);
    CHECK(enumerated == VK_SUCCESS && count);
    VkPhysicalDevice *physical = calloc(count ? count : 1, sizeof(*physical));
    CHECK(physical != NULL);
    if (physical && count && enumerated == VK_SUCCESS) {
        enumerated = vkEnumeratePhysicalDevices(instance, &count, physical);
        CHECK(enumerated == VK_SUCCESS);
        if (enumerated == VK_SUCCESS)
            for (uint32_t i = 0; i < count; ++i) CHECK(check_device(physical[i]));
    }
    free(physical);
    if (messenger) destroy_debug(instance, messenger, NULL);
    vkDestroyInstance(instance, NULL);
    unsigned errors = atomic_load_explicit(&validation_errors, memory_order_relaxed);
    CHECK(errors == 0);
    printf("Deferred-only public Vulkan proof: layer order=%s, validation errors=%u, %s\n",
           above   ? "validation > VKRT"
           : below ? "VKRT > validation"
                   : "VKRT",
           errors, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
