/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Runs traversal_classification.comp on Vulkan 1.2 compute (Lavapipe works).
 * No native RT extensions, features or layer are enabled.
 * Usage: test_traversal_classification <traversal_classification.spv>
 */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../layer/bvh/bvh_ir_contract.h"

#define CHECK(call)                                        \
    do {                                                   \
        VkResult r_ = (call);                              \
        if (r_ != VK_SUCCESS) {                            \
            fprintf(stderr, "%s failed: %d\n", #call, r_); \
            exit(1);                                       \
        }                                                  \
    } while (0)

struct buffer {
    VkBuffer handle;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceAddress address;
};
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory_properties;

static struct buffer make_buffer(VkDeviceSize size) {
    struct buffer b = {0};
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    CHECK(vkCreateBuffer(device, &ci, NULL, &b.handle));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.handle, &req);
    uint32_t type = 0;
    for (; type < memory_properties.memoryTypeCount; ++type)
        if ((req.memoryTypeBits & (1u << type)) && (memory_properties.memoryTypes[type].propertyFlags &
                                                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                                       (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            break;
    if (type == memory_properties.memoryTypeCount) {
        fprintf(stderr, "No coherent host memory\n");
        exit(1);
    }
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags, .allocationSize = req.size, .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(device, &ai, NULL, &b.memory));
    CHECK(vkBindBufferMemory(device, b.handle, b.memory, 0));
    CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    memset(b.mapped, 0, size);
    VkBufferDeviceAddressInfo address = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.handle};
    b.address = vkGetBufferDeviceAddress(device, &address);
    return b;
}

static void destroy_buffer(struct buffer b) {
    vkUnmapMemory(device, b.memory);
    vkDestroyBuffer(device, b.handle, NULL);
    vkFreeMemory(device, b.memory, NULL);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <traversal_classification.spv>\n", argv[0]);
        return 2;
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) {
        fprintf(stderr, "No Vulkan compute device\n");
        return 1;
    }
    VkPhysicalDevice *physical_devices = malloc(count * sizeof(*physical_devices));
    CHECK(vkEnumeratePhysicalDevices(instance, &count, physical_devices));
    VkPhysicalDevice physical = physical_devices[0];
    free(physical_devices);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = malloc(count * sizeof(*families));
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    for (; family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT); ++family) {
    }
    free(families);
    if (family == count) {
        fprintf(stderr, "No compute queue\n");
        return 1;
    }
    float priority = 1;
    VkDeviceQueueCreateInfo queue_ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                        .queueFamilyIndex = family,
                                        .queueCount = 1,
                                        .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .bufferDeviceAddress = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE};
    VkPhysicalDeviceFeatures features = {.shaderInt64 = VK_TRUE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .pNext = &f12,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &queue_ci,
                              .pEnabledFeatures = &features};
    CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    enum { CASE_COUNT = 30 };
    const uint32_t tlas_size = BVH_IR_NODE_OFFSET + 2 * BVH_IR_INSTANCE_SIZE;
    struct buffer nodes = make_buffer(BVH_IR_NODE_OFFSET + 64), tlas = make_buffer(tlas_size);
    struct buffer cases = make_buffer(CASE_COUNT * 16), output = make_buffer(CASE_COUNT * 16);
    ((uint32_t *)nodes.mapped)[BVH_IR_NODE_OFFSET / 4 + 7] = 0;
    ((uint32_t *)nodes.mapped)[BVH_IR_NODE_OFFSET / 4 + 15] = 0x80000000u; /* SW_TRI_OPAQUE */
    ((uint32_t *)tlas.mapped)[BVH_IR_NODE_OFFSET / 4 + 9] = 0xab123456u;
    ((uint32_t *)tlas.mapped)[(BVH_IR_NODE_OFFSET + BVH_IR_INSTANCE_SIZE) / 4 + 9] = 0xcd654321u;
    const uint32_t ray_override[] = {0, 1, 2};
    const uint32_t instance_override[] = {0, 0x40000, 0x80000};
    const uint32_t cull[] = {0, 0x40, 0x80};
    uint32_t expected[CASE_COUNT][2], n = 0;
    for (uint32_t geometry = 0; geometry < 2; ++geometry)
        for (uint32_t ray = 0; ray < 3; ++ray)
            for (uint32_t inst = 0; inst < 3; ++inst)
                /* Vulkan makes the two opacity overrides and the two
                 * opacity cull flags mutually exclusive with each other. */
                for (uint32_t mode = 0; mode < (ray ? 1u : 3u); ++mode, ++n) {
                    uint32_t opaque = ray ? ray == 1 : inst ? inst == 1 : geometry;
                    uint32_t proceed = mode == 0 || (mode == 1 ? !opaque : opaque);
                    uint32_t *row = (uint32_t *)cases.mapped + n * 4;
                    row[0] = geometry;
                    row[1] = ray_override[ray] | cull[mode];
                    row[2] = instance_override[inst];
                    expected[n][0] = proceed;
                    expected[n][1] = proceed ? opaque : 2;
                }
    VkDescriptorSetLayoutBinding bindings[3] = {0};
    for (uint32_t i = 0; i < 3; ++i)
        bindings[i] = (VkDescriptorSetLayoutBinding){.binding = i,
                                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                     .descriptorCount = 1,
                                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &lci, NULL, &set_layout));
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8};
    VkPipelineLayoutCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &set_layout,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &pci, NULL, &layout));
    FILE *file = fopen(argv[1], "rb");
    if (!file) {
        perror(argv[1]);
        return 1;
    }
    fseek(file, 0, SEEK_END);
    long bytes = ftell(file);
    rewind(file);
    uint32_t *code = malloc((size_t)bytes);
    if (bytes <= 0 || bytes % 4 || fread(code, 1, (size_t)bytes, file) != (size_t)bytes) return 1;
    fclose(file);
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &smci, NULL, &module));
    free(code);
    VkComputePipelineCreateInfo pipeline_ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                               .layout = layout,
                                               .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                         .module = module,
                                                         .pName = "main"}};
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, NULL, &pipeline));
    VkDescriptorPoolSize pool_size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3};
    VkDescriptorPoolCreateInfo pool_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool descriptor_pool;
    CHECK(vkCreateDescriptorPool(device, &pool_ci, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = descriptor_pool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &dai, &set));
    VkDescriptorBufferInfo buffer_infos[] = {
        {tlas.handle, 0, tlas_size}, {output.handle, 0, CASE_COUNT * 16}, {cases.handle, 0, CASE_COUNT * 16}};
    VkWriteDescriptorSet writes[3];
    for (uint32_t i = 0; i < 3; ++i)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &buffer_infos[i]};
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    VkCommandPoolCreateInfo command_pool_ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool command_pool;
    CHECK(vkCreateCommandPool(device, &command_pool_ci, NULL, &command_pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = command_pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &cai, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &nodes.address);
    vkCmdDispatch(command, CASE_COUNT, 1, 1);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0,
                         NULL);
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    int failures = 0;
    for (uint32_t i = 0; i < CASE_COUNT; ++i) {
        uint32_t *row = (uint32_t *)output.mapped + i * 4;
        if (row[0] != expected[i][0] || row[1] != expected[i][1] || row[2] != 0x123456 || row[3] != 0x654321) {
            fprintf(stderr, "case %u: got %u/%u/%x/%x, expected %u/%u/123456/654321\n", i, row[0], row[1], row[2], row[3],
                    expected[i][0], expected[i][1]);
            ++failures;
        }
    }
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyShaderModule(device, module, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    destroy_buffer(output);
    destroy_buffer(cases);
    destroy_buffer(tlas);
    destroy_buffer(nodes);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("Traversal classification %s: 30 valid opacity/cull combinations and both SBT selectors (%d failures)\n",
           failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
