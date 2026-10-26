/* SPDX-License-Identifier: MIT
 * Public driver-backed buffer requirement queries. Run with the canonical
 * layer, and with validation above and below it, through VK_INSTANCE_LAYERS.
 */
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CHECK_VK(call)                                    \
    do {                                                  \
        VkResult r = (call);                              \
        if (r != VK_SUCCESS) {                            \
            fprintf(stderr, "%s failed: %d\n", #call, r); \
            exit(1);                                      \
        }                                                 \
    } while (0)
static unsigned failures, validation_errors, cases;
#define REQUIRE(test)                                               \
    do {                                                            \
        if (!(test)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); \
            ++failures;                                             \
        }                                                           \
    } while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT type,
                                                     const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    }
    return VK_FALSE;
}
static bool has_extension(const VkExtensionProperties *extensions, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return true;
    return false;
}
static bool equal_requirements(const VkMemoryRequirements *a, const VkMemoryRequirements *b) {
    return a->size == b->size && a->alignment == b->alignment && a->memoryTypeBits == b->memoryTypeBits;
}
static bool equal_external(const VkExternalBufferProperties *a, const VkExternalBufferProperties *b) {
    return a->externalMemoryProperties.externalMemoryFeatures == b->externalMemoryProperties.externalMemoryFeatures &&
           a->externalMemoryProperties.exportFromImportedHandleTypes == b->externalMemoryProperties.exportFromImportedHandleTypes &&
           a->externalMemoryProperties.compatibleHandleTypes == b->externalMemoryProperties.compatibleHandleTypes;
}
static void external_case(VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceExternalBufferProperties get,
                          PFN_vkGetPhysicalDeviceExternalBufferProperties get_khr, VkBufferUsageFlags usage, bool flags2) {
    const VkBufferUsageFlags rt_bits = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                       VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
    VkBufferUsageFlags native_usage = usage & ~rt_bits;
    if (usage & rt_bits) native_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBufferUsageFlags2CreateInfoKHR usage2 = {VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR, NULL, usage};
    VkBufferUsageFlags2CreateInfoKHR native2 = {VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR, NULL, native_usage};
    VkPhysicalDeviceExternalBufferInfo info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                                               .pNext = flags2 ? &usage2 : NULL,
                                               .usage = flags2 ? 0 : usage,
#ifdef _WIN32
                                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
#else
                                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
#endif
    VkPhysicalDeviceExternalBufferInfo reference = info;
    reference.usage = flags2 ? 0 : native_usage;
    reference.pNext = flags2 ? &native2 : NULL;
    VkPhysicalDeviceExternalBufferInfo saved = info;
    VkExternalBufferProperties actual = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    VkExternalBufferProperties expected = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    get(physical, &info, &actual);
    get(physical, &reference, &expected);
    REQUIRE(equal_external(&actual, &expected));
    if (get_khr) {
        VkExternalBufferProperties alias = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        get_khr(physical, &info, &alias);
        REQUIRE(equal_external(&actual, &alias));
    }
    REQUIRE(!memcmp(&info, &saved, sizeof(info)) && usage2.usage == usage);
    printf("physical external query before device: usage 0x%x%s, features 0x%x\n", usage, flags2 ? " Flags2" : "",
           actual.externalMemoryProperties.externalMemoryFeatures);
}

static void buffer_case(VkDevice device, VkDevice ordinary_device, VkPhysicalDevice physical, VkBufferUsageFlags usage, bool flags2,
                        bool query_khr_enabled, bool requirements2_khr_enabled) {
    PFN_vkGetDeviceBufferMemoryRequirements query =
        (PFN_vkGetDeviceBufferMemoryRequirements)vkGetDeviceProcAddr(device, "vkGetDeviceBufferMemoryRequirements");
    PFN_vkGetDeviceBufferMemoryRequirements query_khr =
        query_khr_enabled
            ? (PFN_vkGetDeviceBufferMemoryRequirements)vkGetDeviceProcAddr(device, "vkGetDeviceBufferMemoryRequirementsKHR")
            : NULL;
    PFN_vkGetBufferMemoryRequirements2 get2 =
        (PFN_vkGetBufferMemoryRequirements2)vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
    PFN_vkGetBufferMemoryRequirements2 get2_khr =
        requirements2_khr_enabled
            ? (PFN_vkGetBufferMemoryRequirements2)vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2KHR")
            : NULL;
    REQUIRE(query && get2 && (!query_khr_enabled || query_khr) && (!requirements2_khr_enabled || get2_khr));
    if (!query || !get2) return;
    VkBufferUsageFlags2CreateInfoKHR usage2 = {VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR, NULL, usage};
    VkBufferOpaqueCaptureAddressCreateInfo suffix = {VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO, NULL, 0};
    usage2.pNext = &suffix;
    VkExternalMemoryBufferCreateInfo prefix = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, &usage2, 0};
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .pNext = flags2 ? (const void *)&prefix : (const void *)&suffix,
                               .size = 65537 + cases * 8193,
                               .usage = flags2 ? 0 : usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBufferCreateInfo saved = info;
    VkBufferUsageFlags2CreateInfoKHR saved2 = usage2;
    VkDeviceBufferMemoryRequirements request = {VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, NULL, &info};
    VkMemoryDedicatedRequirements query_dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 predicted = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &query_dedicated};
    query(device, &request, &predicted);
    REQUIRE(predicted.memoryRequirements.memoryTypeBits && predicted.memoryRequirements.size >= info.size);
    if (query_khr) {
        VkMemoryRequirements2 alias = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        query_khr(device, &request, &alias);
        REQUIRE(equal_requirements(&predicted.memoryRequirements, &alias.memoryRequirements));
    }
    VkBuffer buffer;
    CHECK_VK(vkCreateBuffer(device, &info, NULL, &buffer));
    VkMemoryRequirements actual;
    vkGetBufferMemoryRequirements(device, buffer, &actual);
    REQUIRE(equal_requirements(&predicted.memoryRequirements, &actual));
    VkBufferMemoryRequirementsInfo2 handle_info = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, NULL, buffer};
    VkMemoryDedicatedRequirements actual_dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 actual2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &actual_dedicated};
    get2(device, &handle_info, &actual2);
    REQUIRE(equal_requirements(&actual, &actual2.memoryRequirements));
    REQUIRE(actual_dedicated.prefersDedicatedAllocation == query_dedicated.prefersDedicatedAllocation &&
            actual_dedicated.requiresDedicatedAllocation == query_dedicated.requiresDedicatedAllocation);
    if (get2_khr) {
        VkMemoryRequirements2 alias = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        get2_khr(device, &handle_info, &alias);
        REQUIRE(equal_requirements(&actual, &alias.memoryRequirements));
    }
    /* Bind using the predicted requirements, including an AS buffer whose
     * public usage omitted BDA. The allocation entry point adds private flags. */
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        if (predicted.memoryRequirements.memoryTypeBits & (1u << i)) {
            memory_type = i;
            break;
        }
    REQUIRE(memory_type != UINT32_MAX);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = predicted.memoryRequirements.size,
                                       .memoryTypeIndex = memory_type};
    VkDeviceMemory memory;
    CHECK_VK(vkAllocateMemory(device, &allocation, NULL, &memory));
    CHECK_VK(vkBindBufferMemory(device, buffer, memory, 0));
    if (usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT && !flags2) {
        VkBuffer ordinary;
        CHECK_VK(vkCreateBuffer(ordinary_device, &info, NULL, &ordinary));
        VkMemoryRequirements ordinary_requirements;
        vkGetBufferMemoryRequirements(ordinary_device, ordinary, &ordinary_requirements);
        REQUIRE(equal_requirements(&actual, &ordinary_requirements));
        vkDestroyBuffer(ordinary_device, ordinary, NULL);
    }
    REQUIRE(!memcmp(&info, &saved, sizeof(info)) && !memcmp(&usage2, &saved2, sizeof(usage2)));
    REQUIRE(prefix.pNext == &usage2 && usage2.pNext == &suffix && !suffix.opaqueCaptureAddress);
    printf("public create-info/real-buffer requirements: usage 0x%x%s, size %llu, alignment %llu, types 0x%x\n", usage,
           flags2 ? " Flags2" : "", (unsigned long long)actual.size, (unsigned long long)actual.alignment, actual.memoryTypeBits);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    ++cases;
}

int main(void) {
    uint32_t instance_count = 0;
    CHECK_VK(vkEnumerateInstanceExtensionProperties(NULL, &instance_count, NULL));
    VkExtensionProperties *instance_extensions = calloc(instance_count, sizeof(*instance_extensions));
    if (!instance_extensions) return 1;
    CHECK_VK(vkEnumerateInstanceExtensionProperties(NULL, &instance_count, instance_extensions));
    const char *instance_names[2];
    unsigned instance_name_count = 0;
    bool debug = has_extension(instance_extensions, instance_count, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    bool external_khr = has_extension(instance_extensions, instance_count, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
    if (debug) instance_names[instance_name_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (external_khr) instance_names[instance_name_count++] = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME;
    free(instance_extensions);
    VkDebugUtilsMessengerCreateInfoEXT debug_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        .pfnUserCallback = debug_callback};
    VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                     .pApplicationName = "public memory query parity",
                                     .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo create_instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                            .pNext = debug ? &debug_info : NULL,
                                            .pApplicationInfo = &application,
                                            .enabledExtensionCount = instance_name_count,
                                            .ppEnabledExtensionNames = instance_names};
    VkInstance instance;
    CHECK_VK(vkCreateInstance(&create_instance, NULL, &instance));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (debug) {
        PFN_vkCreateDebugUtilsMessengerEXT create_debug =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        CHECK_VK(create_debug(instance, &debug_info, NULL, &messenger));
    }
    uint32_t count = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) return 77;
    VkPhysicalDevice *physicals = calloc(count, sizeof(*physicals));
    if (!physicals) return 1;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &count, physicals));
    VkPhysicalDevice physical = physicals[0];
    free(physicals);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    printf("memory query device: %s, API %u.%u\n", properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
           VK_API_VERSION_MINOR(properties.apiVersion));
    if (properties.apiVersion < VK_API_VERSION_1_3) return 77;
    CHECK_VK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) return 1;
    CHECK_VK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
    const char *device_names[8] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                   VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME};
    unsigned device_name_count = 3;
    for (unsigned i = 0; i < device_name_count; ++i)
        if (!has_extension(extensions, count, device_names[i])) return 77;
    bool maintenance4_khr = has_extension(extensions, count, VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    bool maintenance5_khr = has_extension(extensions, count, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    bool requirements2_khr = has_extension(extensions, count, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    if (maintenance4_khr) device_names[device_name_count++] = VK_KHR_MAINTENANCE_4_EXTENSION_NAME;
    if (maintenance5_khr) device_names[device_name_count++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
    if (requirements2_khr) device_names[device_name_count++] = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME;
    free(extensions);
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceMaintenance4Features maintenance4 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES,
                                                         .pNext = maintenance5_khr ? &maintenance5 : NULL};
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
                                                       .pNext = &maintenance4};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &bda};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR pipeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .pNext = &acceleration};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &pipeline};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!acceleration.accelerationStructure || !pipeline.rayTracingPipeline || !bda.bufferDeviceAddress ||
        !maintenance4.maintenance4)
        return 77;
    bool flags2 = maintenance5_khr && maintenance5.maintenance5;
    /* Query external compatibility before creating either logical device. */
    PFN_vkGetPhysicalDeviceExternalBufferProperties external =
        (PFN_vkGetPhysicalDeviceExternalBufferProperties)vkGetInstanceProcAddr(instance,
                                                                               "vkGetPhysicalDeviceExternalBufferProperties");
    PFN_vkGetPhysicalDeviceExternalBufferProperties external_alias =
        external_khr ? (PFN_vkGetPhysicalDeviceExternalBufferProperties)vkGetInstanceProcAddr(
                           instance, "vkGetPhysicalDeviceExternalBufferPropertiesKHR")
                     : NULL;
    REQUIRE(external && (!external_khr || external_alias));
    if (!external) return 1;
    const VkBufferUsageFlags usages[] = {
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    for (unsigned i = 0; i < ARRAY_SIZE(usages); ++i) external_case(physical, external, external_alias, usages[i], false);
    if (flags2)
        for (unsigned i = 0; i < ARRAY_SIZE(usages); ++i) external_case(physical, external, external_alias, usages[i], true);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    if (!families) return 1;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            family = i;
            break;
        }
    free(families);
    if (family == UINT32_MAX) return 77;
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &priority};
    /* Enable only the public features this fixture actually uses. */
    pipeline = (VkPhysicalDeviceRayTracingPipelineFeaturesKHR){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &acceleration,
        .rayTracingPipeline = VK_TRUE};
    acceleration = (VkPhysicalDeviceAccelerationStructureFeaturesKHR){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &bda,
        .accelerationStructure = VK_TRUE};
    bda = (VkPhysicalDeviceBufferDeviceAddressFeatures){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
                                                        .pNext = &maintenance4,
                                                        .bufferDeviceAddress = VK_TRUE};
    maintenance4.maintenance4 = VK_TRUE;
    maintenance5.maintenance5 = flags2;
    VkDeviceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .pNext = &pipeline,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &queue,
                                 .enabledExtensionCount = device_name_count,
                                 .ppEnabledExtensionNames = device_names};
    VkDevice device;
    CHECK_VK(vkCreateDevice(physical, &create, NULL, &device));
    VkDeviceCreateInfo ordinary_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
    VkDevice ordinary_device;
    CHECK_VK(vkCreateDevice(physical, &ordinary_create, NULL, &ordinary_device));
    for (unsigned i = 0; i < ARRAY_SIZE(usages); ++i)
        buffer_case(device, ordinary_device, physical, usages[i], false, maintenance4_khr, requirements2_khr);
    if (flags2)
        for (unsigned i = 0; i < ARRAY_SIZE(usages); ++i)
            buffer_case(device, ordinary_device, physical, usages[i], true, maintenance4_khr, requirements2_khr);
    else
        printf("Flags2 cases unavailable: maintenance5 is not supported\n");
    vkDestroyDevice(ordinary_device, NULL);
    vkDestroyDevice(device, NULL);
    if (messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        destroy_debug(instance, messenger, NULL);
    }
    vkDestroyInstance(instance, NULL);
    printf("Public memory query parity: %s (%u cases, %u failures, %u validation errors)\n",
           failures || validation_errors ? "FAIL" : "PASS", cases, failures, validation_errors);
    return failures || validation_errors ? 1 : 0;
}
