/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Public maintenance5 inline-module creation control, without a GPU dispatch.
 */
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call)                                     \
    do {                                                \
        VkResult status = (call);                       \
        if (status != VK_SUCCESS) {                     \
            fprintf(stderr, "%s: %d\n", #call, status); \
            return 1;                                   \
        }                                               \
    } while (0)

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 1;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size < 20 || size % 4) {
        fclose(file);
        return 1;
    }
    uint32_t *words = malloc((size_t)size);
    if (!words || fread(words, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(words);
        return 1;
    }
    fclose(file);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) return 77;
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) return 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice physical = devices[0];
    free(devices);
    VkPhysicalDeviceMaintenance5FeaturesKHR five = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &five};
    VkPhysicalDeviceVulkan12Features twelve = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &as};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &twelve};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!five.maintenance5 || !as.accelerationStructure || !twelve.bufferDeviceAddress) {
        free(words);
        vkDestroyInstance(instance, NULL);
        return 77;
    }
    /* The layer must not advertise ray query; discovery coverage lives in
     * test_discovery so this native-control run stays layer-agnostic. */
    twelve = (VkPhysicalDeviceVulkan12Features){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &as, .bufferDeviceAddress = VK_TRUE};
    as = (VkPhysicalDeviceAccelerationStructureFeaturesKHR){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &five,
        .accelerationStructure = VK_TRUE};
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    VkQueueFamilyProperties *properties = calloc(families, sizeof(*properties));
    if (!properties) return 1;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, properties);
    uint32_t family = 0;
    while (family < families && !(properties[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    free(properties);
    if (family == families) {
        free(words);
        vkDestroyInstance(instance, NULL);
        return 77;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = family,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    const char *extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                VK_KHR_MAINTENANCE_5_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                      .pNext = &twelve,
                                      .queueCreateInfoCount = 1,
                                      .pQueueCreateInfos = &queue,
                                      .enabledExtensionCount = 3,
                                      .ppEnabledExtensionNames = extensions};
    VkDevice device;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout set;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set));
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 128};
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1,
                                              .pSetLayouts = &set,
                                              .pushConstantRangeCount = 1,
                                              .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkShaderModuleCreateInfo source = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = words};
    VkComputePipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                 .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                           .pNext = &source,
                                                           .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                           .module = VK_NULL_HANDLE,
                                                           .pName = "main"},
                                                 .layout = layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult status = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline);
    int failure = status != VK_SUCCESS || !pipeline;
    printf("Public inline native source: result=%d pipeline=%s => %s\n", status, pipeline ? "created" : "null",
           failure ? "FAIL" : "PASS");
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    free(words);
    return failure;
}
