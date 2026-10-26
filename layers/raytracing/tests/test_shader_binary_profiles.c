/* Copyright (c) 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT
 * Two logical devices with different public RT profiles share binary identity.
 */
#define main shader_object_triangle_fixture_main
#include "test_shader_object_rq.c"
#undef main
struct context {
    VkDevice device;
    VkQueue queue;
    VkCommandPool commands;
    struct gpu_buf output;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
};
static void seal_binary(uint8_t *bytes, size_t size) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= i >= 112 && i < 120 ? 0 : bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    for (unsigned i = 0; i < 8; ++i) bytes[112 + i] = (uint8_t)(hash >> (8 * i));
}
static void activate(struct context *context) {
    g_dev = context->device;
    g_queue = context->queue;
    g_pool = context->commands;
}
static void initialize(struct context *context) {
    activate(context);
    buf_create(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               &context->output);
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dsl = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    CHECK(vkCreateDescriptorSetLayout(g_dev, &dsl, NULL, &context->dsl));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    CHECK(vkCreateDescriptorPool(g_dev, &pool, NULL, &context->pool));
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = context->pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &context->dsl};
    CHECK(vkAllocateDescriptorSets(g_dev, &allocate, &context->set));
    VkDescriptorBufferInfo buffer = {context->output.buf, 0, 4};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = context->set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &buffer};
    vkUpdateDescriptorSets(g_dev, 1, &write, 0, NULL);
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &context->dsl,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &push};
    CHECK(vkCreatePipelineLayout(g_dev, &layout, NULL, &context->layout));
}
static void execute(struct context *context, VkShaderEXT shader) {
    activate(context);
    *(uint32_t *)context->output.mapped = 0;
    PFN_vkCmdBindShadersEXT bind = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBindShadersEXT");
    VkCommandBuffer command = begin();
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
    bind(command, 1, &stage, &shader);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, context->layout, 0, 1, &context->set, 0, NULL);
    uint32_t push[] = {0, 0, 0, 7};
    vkCmdPushConstants(command, context->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(command, 1, 1, 1);
    memory_barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                   VK_ACCESS_HOST_READ_BIT);
    submit(command);
    if (*(uint32_t *)context->output.mapped != 48) {
        fprintf(stderr, "Cross-profile specialization result=%u expected48\n", *(uint32_t *)context->output.mapped);
        ++g_fail;
    }
}
static void finish(struct context *context) {
    activate(context);
    vkDestroyPipelineLayout(g_dev, context->layout, NULL);
    vkDestroyDescriptorPool(g_dev, context->pool, NULL);
    vkDestroyDescriptorSetLayout(g_dev, context->dsl, NULL);
    buf_destroy(&context->output);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
}
int main(int argc, char **argv) {
    if (argc != 5) return 1;
    init_vulkan();
    struct context contexts[2] = {{.device = g_dev, .queue = g_queue, .commands = g_pool}, {0}};
    initialize(&contexts[0]);
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = g_qfamily,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkPhysicalDeviceShaderObjectFeaturesEXT objects = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
                                                       .shaderObject = VK_TRUE};
    VkPhysicalDeviceDynamicRenderingFeatures rendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, .pNext = &objects, .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceFeatures core = {.shaderInt64 = VK_TRUE};
    const char *extension = VK_EXT_SHADER_OBJECT_EXTENSION_NAME;
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .pNext = &rendering,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &queue,
                                 .pEnabledFeatures = &core,
                                 .enabledExtensionCount = 1,
                                 .ppEnabledExtensionNames = &extension};
    CHECK(vkCreateDevice(g_pdev, &device, NULL, &contexts[1].device));
    vkGetDeviceQueue(contexts[1].device, g_qfamily, 0, &contexts[1].queue);
    VkCommandPoolCreateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_qfamily};
    CHECK(vkCreateCommandPool(contexts[1].device, &commands, NULL, &contexts[1].commands));
    initialize(&contexts[1]);
    void *blobs[2];
    size_t sizes[2];
    void *linked_blobs[2];
    size_t linked_sizes[2];
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    uint32_t original = 41, poison = 99;
    VkSpecializationMapEntry map = {7, 0, 4};
    VkSpecializationInfo specialization = {1, &map, 4, &original}, ignored = {1, &map, 4, &poison};
    for (unsigned i = 0; i < 2; ++i) {
        activate(&contexts[0]);
        size_t count;
        uint32_t *words = read_spv(argv[i + 1], &count);
        VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                      .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                      .codeSize = count * 4,
                                      .pCode = words,
                                      .pName = "main",
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &contexts[0].dsl,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &push,
                                      .pSpecializationInfo = &specialization};
        PFN_vkCreateShadersEXT create = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
        PFN_vkDestroyShaderEXT destroy = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
        PFN_vkGetShaderBinaryDataEXT get = (void *)vkGetDeviceProcAddr(g_dev, "vkGetShaderBinaryDataEXT");
        VkShaderEXT shader;
        CHECK(create(g_dev, 1, &info, NULL, &shader));
        free(words);
        execute(&contexts[0], shader);
        CHECK(get(g_dev, shader, &sizes[i], NULL));
        blobs[i] = malloc(sizes[i]);
        if (!blobs[i]) return 1;
        CHECK(get(g_dev, shader, &sizes[i], blobs[i]));
        destroy(g_dev, shader, NULL);
        // Initialize B's distinct resolver while A's context is still alive,
        // then destroy the probe. The final recreation is after A is destroyed.
        if (!i) {
            activate(&contexts[1]);
            create = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
            destroy = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
            info.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
            info.pCode = blobs[i];
            info.codeSize = sizes[i];
            info.pName = NULL;
            info.pSpecializationInfo = &ignored;
            info.pSetLayouts = &contexts[1].dsl;
            CHECK(create(g_dev, 1, &info, NULL, &shader));
            destroy(g_dev, shader, NULL);
        }
    }
    activate(&contexts[0]);
    VkShaderCreateInfoEXT linked[2];
    VkShaderEXT linked_shaders[2];
    VkPushConstantRange fragment_push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    PFN_vkCreateShadersEXT create_linked = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
    PFN_vkDestroyShaderEXT destroy_linked = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
    PFN_vkGetShaderBinaryDataEXT get_linked = (void *)vkGetDeviceProcAddr(g_dev, "vkGetShaderBinaryDataEXT");
    for (unsigned i = 0; i < 2; ++i) {
        size_t count;
        uint32_t *words = read_spv(argv[i + 3], &count);
        linked[i] = (VkShaderCreateInfoEXT){.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                            .flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT,
                                            .stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT,
                                            .nextStage = i ? 0 : VK_SHADER_STAGE_FRAGMENT_BIT,
                                            .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                            .codeSize = count * 4,
                                            .pCode = words,
                                            .pName = "main",
                                            .pushConstantRangeCount = 1,
                                            .pPushConstantRanges = &fragment_push};
    }
    CHECK(create_linked(g_dev, 2, linked, NULL, linked_shaders));
    for (unsigned i = 0; i < 2; ++i) {
        free((void *)linked[i].pCode);
        CHECK(get_linked(g_dev, linked_shaders[i], &linked_sizes[i], NULL));
        linked_blobs[i] = malloc(linked_sizes[i]);
        if (!linked_blobs[i]) return 1;
        CHECK(get_linked(g_dev, linked_shaders[i], &linked_sizes[i], linked_blobs[i]));
        destroy_linked(g_dev, linked_shaders[i], NULL);
        linked[i].codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
        linked[i].codeSize = linked_sizes[i];
        linked[i].pCode = linked_blobs[i];
        linked[i].pName = NULL;
    }
    finish(&contexts[0]);
    for (unsigned i = 0; i < 2; ++i) {
        activate(&contexts[1]);
        VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                      .codeType = VK_SHADER_CODE_TYPE_BINARY_EXT,
                                      .codeSize = sizes[i],
                                      .pCode = blobs[i],
                                      .pName = NULL,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &contexts[1].dsl,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &push,
                                      .pSpecializationInfo = &ignored};
        PFN_vkCreateShadersEXT create = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
        PFN_vkDestroyShaderEXT destroy = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
        VkShaderEXT shader;
        CHECK(create(g_dev, 1, &info, NULL, &shader));
        execute(&contexts[1], shader);
        destroy(g_dev, shader, NULL);
        free(blobs[i]);
    }
    activate(&contexts[1]);
    /* Ordinary ShaderObject-only source creation also exports the same layer
     * format and reconstructs source after the original shader is destroyed. */
    size_t ordinary_count;
    uint32_t *ordinary_words = read_spv(argv[2], &ordinary_count);
    VkShaderCreateInfoEXT ordinary = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                      .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                      .codeSize = ordinary_count * 4,
                                      .pCode = ordinary_words,
                                      .pName = "main",
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &contexts[1].dsl,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &push,
                                      .pSpecializationInfo = &specialization};
    PFN_vkCreateShadersEXT ordinary_create = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
    PFN_vkDestroyShaderEXT ordinary_destroy = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
    PFN_vkGetShaderBinaryDataEXT ordinary_get = (void *)vkGetDeviceProcAddr(g_dev, "vkGetShaderBinaryDataEXT");
    VkShaderEXT ordinary_shader;
    CHECK(ordinary_create(g_dev, 1, &ordinary, NULL, &ordinary_shader));
    execute(&contexts[1], ordinary_shader);
    size_t ordinary_size;
    CHECK(ordinary_get(g_dev, ordinary_shader, &ordinary_size, NULL));
    uint8_t *ordinary_blob = malloc(ordinary_size);
    if (!ordinary_blob) return 1;
    CHECK(ordinary_get(g_dev, ordinary_shader, &ordinary_size, ordinary_blob));
    ordinary_destroy(g_dev, ordinary_shader, NULL);
    free(ordinary_words);
    ordinary.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
    ordinary.pCode = ordinary_blob;
    ordinary.codeSize = ordinary_size;
    ordinary.pName = NULL;
    ordinary.pSpecializationInfo = &ignored;
    CHECK(ordinary_create(g_dev, 1, &ordinary, NULL, &ordinary_shader));
    execute(&contexts[1], ordinary_shader);
    ordinary_destroy(g_dev, ordinary_shader, NULL);
    /* Recomputed envelope checksum still cannot turn malformed SPIR-V into
     * valid opaque input. A native downcall would emit SPIR-V validation errors. */
    PFN_vkGetPhysicalDeviceProperties2 get_properties = (void *)vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2");
    VkPhysicalDeviceShaderObjectPropertiesEXT binary_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 physical_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                       .pNext = &binary_properties};
    get_properties(g_pdev, &physical_properties);
    const uint8_t magic[] = {0x56, 0x4b, 0x52, 0x54};
    int layer_binary = ordinary_size > 152 && !memcmp(ordinary_blob, magic, sizeof(magic));
    if (layer_binary) {
        if (memcmp(ordinary_blob + 96, binary_properties.shaderBinaryUUID, VK_UUID_SIZE)) ++g_fail;
        ordinary_blob[150] = ordinary_blob[151] = 0; /* first instruction word count zero */
        seal_binary(ordinary_blob, ordinary_size);
        ordinary_shader = (VkShaderEXT)(uintptr_t)1;
        VkResult malformed = ordinary_create(g_dev, 1, &ordinary, NULL, &ordinary_shader);
        if (malformed != VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT || ordinary_shader) ++g_fail;
    }
    free(ordinary_blob);
    create_linked = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT");
    destroy_linked = (void *)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
    CHECK(create_linked(g_dev, 2, linked, NULL, linked_shaders));
    for (unsigned i = 0; i < 2; ++i) {
        destroy_linked(g_dev, linked_shaders[i], NULL);
        free(linked_blobs[i]);
    }
    finish(&contexts[1]);
    vkDestroyInstance(g_instance, NULL);
    printf(
        "Cross-profile RQ/ordinary source envelopes: destroyed RT device -> ShaderObject-only device, entry/spec +linked VERT/FRAG "
        "retained %s\n",
        g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
