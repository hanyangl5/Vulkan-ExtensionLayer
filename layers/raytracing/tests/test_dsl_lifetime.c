/* SPDX-License-Identifier: Apache-2.0
 * Native public compute control for descriptor-set-layout creation-time lifetime.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(call)                               \
    do {                                          \
        VkResult r = (call);                      \
        if (r != VK_SUCCESS) {                    \
            fprintf(stderr, "%s=%d\n", #call, r); \
            return 1;                             \
        }                                         \
    } while (0)
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 1;
    int retain = argc == 3 && !strcmp(argv[2], "--retain-dsl");
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 1;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    uint32_t *words = malloc((size_t)size);
    if (!words || fread(words, 1, (size_t)size, file) != (size_t)size) return 1;
    fclose(file);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    uint32_t family = 0;
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    free(families);
    if (family == family_count) return 77;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = family,
                                   .queueCount = 1,
                                   .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice device;
    CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout set;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set));
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    if (!retain) vkDestroyDescriptorSetLayout(device, set, NULL);
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = words};
    VkShaderModule shader;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader));
    free(words);
    VkComputePipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                 .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                           .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                           .module = shader,
                                                           .pName = "main"},
                                                 .layout = layout};
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &allocate, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdDispatch(command, 1, 1, 1);
    CHECK(vkEndCommandBuffer(command));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    if (retain) vkDestroyDescriptorSetLayout(device, set, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("Native public compute DSL lifetime (%s) PASS\n", retain ? "retained" : "destroyed before pipeline");
    return 0;
}
