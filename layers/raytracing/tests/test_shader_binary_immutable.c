/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * RQ binary execution after original immutable interfaces and device die.
 */
#define main immutable_unused_triangle_main
#include "test_shader_object_rq.c"
#undef main

static VkSampler sampler(void) {
    VkSamplerReductionModeCreateInfo reduction = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
                                                  .reductionMode = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE};
    VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .pNext = &reduction,
                                .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                .maxLod = 1,
                                .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    VkSampler value;
    CHECK(vkCreateSampler(g_dev, &info, NULL, &value));
    return value;
}
static VkDescriptorSetLayout make_immutable_layout(VkSampler immutable) {
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &immutable}};
    VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout layout;
    CHECK(vkCreateDescriptorSetLayout(g_dev, &info, NULL, &layout));
    return layout;
}
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    init_vulkan();
    VkDevice source_device = g_dev;
    VkCommandPool source_commands = g_pool;
    VkSampler original_sampler = sampler();
    VkDescriptorSetLayout original_layout = make_immutable_layout(original_sampler);
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    size_t count;
    uint32_t *words = read_spv(argv[1], &count);
    VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                  .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                  .codeSize = count * 4,
                                  .pCode = words,
                                  .pName = "main",
                                  .setLayoutCount = 1,
                                  .pSetLayouts = &original_layout,
                                  .pushConstantRangeCount = 1,
                                  .pPushConstantRanges = &push};
    PFN_vkCreateShadersEXT create_source = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
    PFN_vkDestroyShaderEXT destroy_source = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
    PFN_vkGetShaderBinaryDataEXT get_source = (void *)vkGetDeviceProcAddr(g_dev, "vkGetShaderBinaryDataEXT");
    VkShaderEXT shader;
    CHECK(create_source(g_dev, 1, &info, NULL, &shader));
    // Layouts need survive only source creation. Get must still export them.
    vkDestroyDescriptorSetLayout(g_dev, original_layout, NULL);
    vkDestroySampler(g_dev, original_sampler, NULL);
    free(words);
    size_t binary_size;
    CHECK(get_source(g_dev, shader, &binary_size, NULL));
    void *binary = malloc(binary_size);
    if (!binary) return 1;
    CHECK(get_source(g_dev, shader, &binary_size, binary));
    destroy_source(g_dev, shader, NULL);

    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = g_qfamily,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkPhysicalDeviceShaderObjectFeaturesEXT objects = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
                                                       .shaderObject = VK_TRUE};
    VkPhysicalDeviceDynamicRenderingFeatures rendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, .pNext = &objects, .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceFeatures features = {.shaderInt64 = VK_TRUE};
    const char *extension = VK_EXT_SHADER_OBJECT_EXTENSION_NAME;
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .pNext = &rendering,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &queue,
                                 .pEnabledFeatures = &features,
                                 .enabledExtensionCount = 1,
                                 .ppEnabledExtensionNames = &extension};
    CHECK(vkCreateDevice(g_pdev, &device, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qfamily, 0, &g_queue);
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_qfamily};
    CHECK(vkCreateCommandPool(g_dev, &pool_info, NULL, &g_pool));
    PFN_vkCreateShadersEXT create = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
    PFN_vkDestroyShaderEXT destroy = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
    PFN_vkCmdBindShadersEXT bind = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBindShadersEXT");
    // No outer descriptor or push ranges: only the original recipe can compile.
    info = (VkShaderCreateInfoEXT){.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                   .codeType = VK_SHADER_CODE_TYPE_BINARY_EXT,
                                   .codeSize = binary_size,
                                   .pCode = binary};
    // Initialize B's resolver while A remains alive, preventing address reuse.
    CHECK(create(g_dev, 1, &info, NULL, &shader));
    destroy(g_dev, shader, NULL);
    vkDestroyCommandPool(source_device, source_commands, NULL);
    vkDestroyDevice(source_device, NULL);
    CHECK(create(g_dev, 1, &info, NULL, &shader));

    VkSampler destination_sampler = sampler();
    VkDescriptorSetLayout destination_layout = make_immutable_layout(destination_sampler);
    VkPipelineLayoutCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 1,
                                                .pSetLayouts = &destination_layout,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(g_dev, &pipeline_info, NULL, &layout));
    struct gpu_buf output;
    buf_create(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               &output);
    *(uint32_t *)output.mapped = 0;
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                                    .extent = {1, 1, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage image;
    CHECK(vkCreateImage(g_dev, &image_info, NULL, &image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(g_dev, image, &requirements);
    VkMemoryAllocateInfo memory_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = find_mem_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(g_dev, &memory_info, NULL, &memory));
    CHECK(vkBindImageMemory(g_dev, image, memory, 0));
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = image,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = VK_FORMAT_R8G8B8A8_UNORM,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    CHECK(vkCreateImageView(g_dev, &view_info, NULL, &view));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool descriptors;
    CHECK(vkCreateDescriptorPool(g_dev, &descriptor_pool_info, NULL, &descriptors));
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = descriptors,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &destination_layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(g_dev, &allocate, &set));
    VkDescriptorBufferInfo buffer = {output.buf, 0, 4};
    VkDescriptorImageInfo sampled = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = 0,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &buffer},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = 3,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &sampled}};
    vkUpdateDescriptorSets(g_dev, 2, writes, 0, NULL);
    VkCommandBuffer command = begin();
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
    VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &barrier.subresourceRange);
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
    bind(command, 1, &stage, &shader);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    uint32_t values[] = {0, 0, 0, 7};
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, values);
    vkCmdDispatch(command, 1, 1, 1);
    memory_barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                   VK_ACCESS_HOST_READ_BIT);
    submit(command);
    uint32_t actual = *(uint32_t *)output.mapped;
    if (actual != 262) {
        fprintf(stderr, "Immutable sampler result %u expected262\n", actual);
        ++g_fail;
    }
    destroy(g_dev, shader, NULL);
    vkDestroyDescriptorPool(g_dev, descriptors, NULL);
    vkDestroyPipelineLayout(g_dev, layout, NULL);
    vkDestroyDescriptorSetLayout(g_dev, destination_layout, NULL);
    vkDestroySampler(g_dev, destination_sampler, NULL);
    vkDestroyImageView(g_dev, view, NULL);
    vkDestroyImage(g_dev, image, NULL);
    vkFreeMemory(g_dev, memory, NULL);
    buf_destroy(&output);
    free(binary);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    printf("RQ binary immutable sampler + push reconstruction after source device destruction: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
