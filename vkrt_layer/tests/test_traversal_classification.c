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
#include "bvh4_abi.h"

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

/* Intentionally hand-shaped ABI fixtures pin traversal topology, including a
 * depth that a balanced reference encoder would not normally produce. */
static struct buffer runtime_buffer(uint32_t type, uint32_t nodes, uint32_t triangles, uint32_t instances) {
    uint64_t triangle_offset = sizeof(VelRtBvh4Header) + (uint64_t)nodes * sizeof(VelRtBvh4Node);
    uint64_t instance_offset = triangle_offset + (uint64_t)triangles * sizeof(VelRtBvh4Triangle);
    uint64_t parents = instance_offset + (uint64_t)instances * sizeof(VelRtBvh4Instance);
    uint64_t size = (parents + (triangles + instances) * 4 + 255) & ~UINT64_C(255);
    struct buffer out = make_buffer(size);
    VelRtBvh4Header *h = out.mapped;
    *h = (VelRtBvh4Header){.magic = VEL_RT_BVH4_MAGIC,
                           .version = VEL_RT_BVH4_ABI_VERSION,
                           .type = type,
                           .root_id = 0,
                           .node_count = nodes,
                           .triangle_count = triangles,
                           .instance_count = instances,
                           .node_offset = sizeof(*h),
                           .triangle_offset = triangle_offset,
                           .aabb_offset = instance_offset,
                           .instance_offset = instance_offset,
                           .leaf_parent_offset = parents,
                           .total_size = size};
    return out;
}
static void triangle_at(VelRtBvh4Triangle *leaf, float x, float z, uint32_t primitive) {
    *leaf = (VelRtBvh4Triangle){.vertices = {{x - 1, -1, z}, {x + 1, -1, z}, {x, 1, z}}, .primitive_id = primitive};
}
static void child_bounds(VelRtBvh4Node *node, uint32_t slot, float xlo, float xhi, float z) {
    const float lo[] = {xlo, -1, z}, hi[] = {xhi, 1, z};
    memcpy(node->bounds_min[slot], lo, sizeof(lo));
    memcpy(node->bounds_max[slot], hi, sizeof(hi));
}
static struct buffer instance_root(struct buffer blas) {
    struct buffer out = runtime_buffer(0, 0, 0, 1);
    VelRtBvh4Header *h = out.mapped, *source = blas.mapped;
    h->root_id = VEL_RT_BVH4_INSTANCE << VEL_RT_BVH4_KIND_SHIFT;
    memcpy(h->bounds_min, source->bounds_min, sizeof(h->bounds_min));
    memcpy(h->bounds_max, source->bounds_max, sizeof(h->bounds_max));
    VelRtBvh4Instance *instance = (void *)((uint8_t *)out.mapped + h->instance_offset);
    instance->blas_address = blas.address;
    instance->custom_index_mask = 0xff000000u;
    for (unsigned axis = 0; axis < 3; ++axis)
        instance->object_to_world[axis * 4 + axis] = instance->world_to_object[axis * 4 + axis] = 1;
    *(uint32_t *)((uint8_t *)out.mapped + h->leaf_parent_offset) = VEL_RT_BVH4_INVALID_ID;
    return out;
}
static struct buffer deep_blas(void) {
    enum { DEPTH = 140, TRIANGLES = 3 * DEPTH + 1 };
    struct buffer out = runtime_buffer(1, DEPTH, TRIANGLES, 0);
    VelRtBvh4Header *h = out.mapped;
    const float lo[] = {-1, -1, 0}, hi[] = {11, 1, 0};
    memcpy(h->bounds_min, lo, sizeof(lo));
    memcpy(h->bounds_max, hi, sizeof(hi));
    VelRtBvh4Node *nodes = (void *)((uint8_t *)out.mapped + h->node_offset);
    VelRtBvh4Triangle *triangles = (void *)((uint8_t *)out.mapped + h->triangle_offset);
    uint32_t *parents = (void *)((uint8_t *)out.mapped + h->leaf_parent_offset);
    for (uint32_t level = 0; level < DEPTH; ++level) {
        nodes[level].child_count = 4;
        nodes[level].parent_id = level ? level - 1 : VEL_RT_BVH4_INVALID_ID;
        for (uint32_t slot = 0; slot < 3; ++slot) {
            uint32_t leaf = 3 * level + slot;
            nodes[level].child_id[slot] = (VEL_RT_BVH4_TRIANGLE << VEL_RT_BVH4_KIND_SHIFT) | leaf;
            child_bounds(&nodes[level], slot, 9, 11, 0);
            triangle_at(&triangles[leaf], 10, 0, 1000 + leaf);
            parents[leaf] = level;
        }
        nodes[level].child_id[3] =
            level + 1 < DEPTH ? level + 1 : (VEL_RT_BVH4_TRIANGLE << VEL_RT_BVH4_KIND_SHIFT) | (TRIANGLES - 1);
        child_bounds(&nodes[level], 3, -1, level + 1 < DEPTH ? 11 : 1, 0);
    }
    triangle_at(&triangles[TRIANGLES - 1], 0, 0, 77);
    parents[TRIANGLES - 1] = DEPTH - 1;
    return out;
}
static struct buffer resume_blas(void) {
    struct buffer out = runtime_buffer(1, 1, 4, 0);
    VelRtBvh4Header *h = out.mapped;
    const float lo[] = {-1, -1, 4}, hi[] = {1, 1, 9.5f}, heights[] = {8, 9, 4, 9.5f};
    memcpy(h->bounds_min, lo, sizeof(lo));
    memcpy(h->bounds_max, hi, sizeof(hi));
    VelRtBvh4Node *node = (void *)((uint8_t *)out.mapped + h->node_offset);
    VelRtBvh4Triangle *triangles = (void *)((uint8_t *)out.mapped + h->triangle_offset);
    node->child_count = 4;
    node->parent_id = VEL_RT_BVH4_INVALID_ID;
    for (uint32_t slot = 0; slot < 4; ++slot) {
        node->child_id[slot] = (VEL_RT_BVH4_TRIANGLE << VEL_RT_BVH4_KIND_SHIFT) | slot;
        child_bounds(node, slot, -1, 1, heights[slot]);
        triangle_at(&triangles[slot], 0, heights[slot], slot);
        /* All four leaf parents are root ID0, already zero initialized. */
    }
    return out;
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

    enum { CASE_COUNT = 32 };
    const uint32_t tlas_size = 768;
    struct buffer nodes = make_buffer(512), tlas = make_buffer(tlas_size);
    struct buffer cases = make_buffer(CASE_COUNT * 16), output = make_buffer(CASE_COUNT * 16);
    VelRtBvh4Header *blas_header = nodes.mapped, *tlas_header = tlas.mapped;
    *blas_header = (VelRtBvh4Header){.magic = VEL_RT_BVH4_MAGIC,
                                     .version = VEL_RT_BVH4_ABI_VERSION,
                                     .type = 1,
                                     .root_id = 0,
                                     .node_count = 1,
                                     .aabb_count = 2,
                                     .node_offset = 128,
                                     .triangle_offset = 256,
                                     .aabb_offset = 256,
                                     .instance_offset = 352,
                                     .leaf_parent_offset = 352,
                                     .total_size = 512};
    *tlas_header = (VelRtBvh4Header){.magic = VEL_RT_BVH4_MAGIC,
                                     .version = VEL_RT_BVH4_ABI_VERSION,
                                     .type = 0,
                                     .root_id = 0,
                                     .node_count = 1,
                                     .instance_count = 2,
                                     .node_offset = 128,
                                     .triangle_offset = 256,
                                     .aabb_offset = 256,
                                     .instance_offset = 256,
                                     .leaf_parent_offset = 512,
                                     .total_size = tlas_size};
    VelRtBvh4Node *blas_root = (void *)((uint8_t *)nodes.mapped + blas_header->node_offset);
    VelRtBvh4Node *tlas_root = (void *)((uint8_t *)tlas.mapped + tlas_header->node_offset);
    blas_root->child_count = tlas_root->child_count = 2;
    blas_root->parent_id = tlas_root->parent_id = VEL_RT_BVH4_INVALID_ID;
    VelRtBvh4Aabb *boxes = (void *)((uint8_t *)nodes.mapped + blas_header->aabb_offset);
    VelRtBvh4Instance *instances = (void *)((uint8_t *)tlas.mapped + tlas_header->instance_offset);
    for (uint32_t i = 0; i < 2; ++i) {
        boxes[i].primitive_id = i;
        boxes[i].geometry_flags = i; /* Original Vulkan OPAQUE bit is 1. */
        instances[i].blas_address = nodes.address;
        instances[i].custom_index_mask = 0xff000000u;
        instances[i].sbt_offset_flags = i ? 0xcd654321u : 0xab123456u;
        instances[i].instance_id = i;
        blas_root->child_id[i] = (VEL_RT_BVH4_AABB << VEL_RT_BVH4_KIND_SHIFT) | i;
        tlas_root->child_id[i] = (VEL_RT_BVH4_INSTANCE << VEL_RT_BVH4_KIND_SHIFT) | i;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            blas_header->bounds_max[axis] = tlas_header->bounds_max[axis] = 1;
            boxes[i].bounds_max[axis] = blas_root->bounds_max[i][axis] = tlas_root->bounds_max[i][axis] = 1;
            instances[i].object_to_world[axis * 4 + axis] = instances[i].world_to_object[axis * 4 + axis] = 1;
        }
    }
    for (uint32_t i = 2; i < 4; ++i) blas_root->child_id[i] = tlas_root->child_id[i] = VEL_RT_BVH4_INVALID_ID;
    struct buffer deep = deep_blas(), deep_tlas = instance_root(deep);
    struct buffer resume = resume_blas(), resume_tlas = instance_root(resume);
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
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 32};
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
    VkDeviceAddress addresses[4] = {nodes.address, tlas.address, deep_tlas.address, resume_tlas.address};
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(addresses), addresses);
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
    for (uint32_t i = 0; i < 30; ++i) {
        uint32_t *row = (uint32_t *)output.mapped + i * 4;
        if (row[0] != expected[i][0] || row[1] != expected[i][1] || row[2] != 0x123456 || row[3] != 0x654321) {
            fprintf(stderr, "case %u: got %u/%u/%x/%x, expected %u/%u/123456/654321\n", i, row[0], row[1], row[2], row[3],
                    expected[i][0], expected[i][1]);
            ++failures;
        }
    }
    const uint32_t deep_expected[] = {1, 77, 0, 0}, resume_expected[] = {3, 11, 3, 0x3f000000};
    const uint32_t *results = output.mapped;
    if (memcmp(results + 30 * 4, deep_expected, sizeof(deep_expected)) ||
        memcmp(results + 31 * 4, resume_expected, sizeof(resume_expected))) {
        fprintf(stderr, "parent walk/resume got deep %u/%u/%u/%x, resume %u/%u/%u/%x\n", results[120], results[121], results[122],
                results[123], results[124], results[125], results[126], results[127]);
        ++failures;
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
    destroy_buffer(deep_tlas);
    destroy_buffer(deep);
    destroy_buffer(resume_tlas);
    destroy_buffer(resume);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf(
        "Traversal classification %s: 30 opacity/cull cases, SBT selectors, depth140/four-child walk, candidate resume (%d "
        "failures)\n",
        failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
