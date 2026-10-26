/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_resource_profile.h"
#include "vkrt_descriptor_layout.h"
#include "vkrt_layer.h"

/* Native budget proof: no native AS/RQ/RTP extension or feature is enabled.
 * This creates and allocates the translated, fully occupied SSBO layout; it
 * does not claim complete application shader/resource qualification. */
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static bool check(VkResult result, const char *operation) {
    if (result == VK_SUCCESS) return true;
    fprintf(stderr, "%s: %d\n", operation, result);
    return false;
}
#define CHECK(call)                  \
    do {                             \
        if (!check((call), #call)) { \
            result = 1;              \
            goto done;               \
        }                            \
    } while (0)
int main(void) {
    int result = 77;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding *bindings = NULL;
    VkDescriptorPoolSize *sizes = NULL;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) goto done;
    VkPhysicalDevice *devices = malloc((size_t)count * sizeof(*devices));
    if (!devices) {
        result = 1;
        goto done;
    }
    if (!check(vkEnumeratePhysicalDevices(instance, &count, devices), "vkEnumeratePhysicalDevices")) {
        free(devices);
        result = 1;
        goto done;
    }
    PFN_vkGetPhysicalDeviceProperties2 properties = (void *)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceFeatures2 features = (void *)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2");
    struct vkrt_resource_profile profile;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        enum vkrt_resource_failure why;
        if (vkrt_query_resource_profile(properties, features, devices[i], &profile, &why)) {
            physical = devices[i];
            break;
        }
        printf("Device%u descriptor profile unavailable: reason%d\n", i, why);
    }
    free(devices);
    if (!physical) goto done;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *queues = malloc((size_t)count * sizeof(*queues));
    if (!queues) {
        result = 1;
        goto done;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
    uint32_t family = 0;
    while (family < count && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    free(queues);
    if (family == count) goto done;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = family,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    VkDescriptorSetLayoutBinding source[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, profile.as_per_set, VK_SHADER_STAGE_RAYGEN_BIT_KHR, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, profile.limits.maxDescriptorSetStorageBuffers, VK_SHADER_STAGE_MISS_BIT_KHR, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, profile.limits.maxDescriptorSetStorageBuffersDynamic,
         VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, NULL}};
    VkDescriptorSetLayoutCreateInfo original = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = source};
    VkDescriptorSetLayoutCreateInfo native;
    CHECK(vkrt_prepare_descriptor_layout(true, &original, &native, &bindings));
    VkDescriptorSetLayoutSupport support = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
    vkGetDescriptorSetLayoutSupport(device, &native, &support);
    if (!support.supported) {
        fprintf(stderr, "Translated full-budget layout is not supported\n");
        result = 1;
        goto done;
    }
    CHECK(vkCreateDescriptorSetLayout(device, &native, NULL, &layout));
    VkDescriptorPoolSize source_sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, source[0].descriptorCount},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, source[1].descriptorCount},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, source[2].descriptorCount}};
    VkDescriptorPoolCreateInfo original_pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = source_sizes};
    VkDescriptorPoolCreateInfo native_pool;
    CHECK(vkrt_prepare_descriptor_pool(true, &original_pool, &native_pool, &sizes));
    CHECK(vkCreateDescriptorPool(device, &native_pool, NULL, &pool));
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &allocate, &set));
    VkPipelineLayoutCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &layout};
    VkPipelineLayout pipeline_layout;
    CHECK(vkCreatePipelineLayout(device, &pipeline_info, NULL, &pipeline_layout));
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    uint64_t total = (uint64_t)source[0].descriptorCount + source[1].descriptorCount + source[2].descriptorCount;
    printf("Native full-budget descriptor layout/allocate PASS: staticSSBO=%u dynamicSSBO=%u AS=%u translatedTotal=%llu\n",
           source[1].descriptorCount, source[2].descriptorCount, source[0].descriptorCount, (unsigned long long)total);
    result = 0;
done:
    free(bindings);
    free(sizes);
    if (pool) vkDestroyDescriptorPool(device, pool, NULL);
    if (layout) vkDestroyDescriptorSetLayout(device, layout, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    return result;
}
