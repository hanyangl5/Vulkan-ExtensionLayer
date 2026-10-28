/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_NO_NATIVE_RT_GUARD_POLICY_H
#define VKRT_NO_NATIVE_RT_GUARD_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "no_native_rt_guard_registry.h"

#define GUARD_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int guard_contains(const uint32_t *values, size_t count, uint32_t value) {
    for (size_t i = 0; i < count; ++i)
        if (values[i] == value) return 1;
    return 0;
}

static int guard_extension(const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < GUARD_COUNT(guard_rt_extensions); ++i)
        if (!strcmp(name, guard_rt_extensions[i])) return 1;
    /* Fail closed for later revisions in the same native-RT extension families. */
    return strstr(name, "ray_tracing") || strstr(name, "ray_query") || strstr(name, "acceleration_structure") ||
           strstr(name, "opacity_micromap") || strstr(name, "displacement_micromap");
}

static int guard_feature_chain(const void *chain) {
    for (const VkBaseInStructure *p = chain; p; p = p->pNext)
        for (size_t i = 0; i < GUARD_COUNT(guard_rt_feature_fields); ++i)
            if (p->sType == guard_rt_feature_fields[i].type) {
                VkBool32 value;
                memcpy(&value, (const char *)p + guard_rt_feature_fields[i].offset, sizeof(value));
                if (value) return 1;
            }
    return 0;
}

static void guard_mask_features(void *chain) {
    const VkBool32 zero = VK_FALSE;
    for (VkBaseOutStructure *p = chain; p; p = p->pNext)
        for (size_t i = 0; i < GUARD_COUNT(guard_rt_feature_fields); ++i)
            if (p->sType == guard_rt_feature_fields[i].type)
                memcpy((char *)p + guard_rt_feature_fields[i].offset, &zero, sizeof(zero));
}

static void guard_mask_properties(void *chain) {
    for (VkBaseOutStructure *p = chain; p; p = p->pNext)
        for (size_t i = 0; i < GUARD_COUNT(guard_rt_property_tails); ++i)
            if (p->sType == guard_rt_property_tails[i].type)
                memset((char *)p + guard_rt_property_tails[i].offset, 0, guard_rt_property_tails[i].size);
}

static int guard_stages(VkPipelineStageFlags2 stages) {
    return (stages & (VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                      VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR | VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT)) != 0;
}

static int guard_access(VkAccessFlags2 access) {
    return (access & (VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                      VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT)) != 0;
}

static int guard_shader_stages(VkShaderStageFlags stages) {
    if (stages == VK_SHADER_STAGE_ALL) return 0; /* ALL is legal on an ordinary non-RT driver. */
    return (stages & (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                      VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR)) != 0;
}

static int guard_buffer_usage(VkBufferUsageFlags2 usage) {
    return (usage & (VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_2_SHADER_BINDING_TABLE_BIT_KHR |
                     VK_BUFFER_USAGE_2_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_2_MICROMAP_STORAGE_BIT_EXT)) != 0;
}

static int guard_buffer_info(const VkBufferCreateInfo *info) {
    if (guard_buffer_usage(info->usage)) return 1;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO &&
            guard_buffer_usage(((const VkBufferUsageFlags2CreateInfo *)p)->usage))
            return 1;
    return 0;
}

static int guard_descriptor(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR || type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV;
}

static int guard_query(VkQueryType type) {
    return type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR ||
           type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR ||
           type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_BOTTOM_LEVEL_POINTERS_KHR ||
           type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SIZE_KHR ||
           type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_NV ||
           type == VK_QUERY_TYPE_MICROMAP_SERIALIZATION_SIZE_EXT || type == VK_QUERY_TYPE_MICROMAP_COMPACTED_SIZE_EXT;
}

static int guard_spirv_string(const uint32_t *words, size_t count, const char *match) {
    size_t size = count * sizeof(uint32_t);
    const char *text = (const char *)words;
    if (!memchr(text, 0, size)) return -1;
    return strstr(text, match) != NULL;
}

/* Returns NULL for known non-RT instructions, otherwise a stable rejection kind.
 * This checks the lower boundary, not source validity; normal SPIR-V validation
 * remains enabled independently. Malformed/unknown instructions fail closed. */
static const char *guard_spirv(const void *bytes, size_t byte_count) {
    if (!bytes || byte_count < 5 * sizeof(uint32_t) || byte_count % sizeof(uint32_t)) return "malformed_spirv";
    const uint32_t *words = bytes;
    if (words[0] != 0x07230203u) return "malformed_spirv";
    size_t count = byte_count / sizeof(uint32_t);
    for (size_t i = 5; i < count;) {
        uint32_t size = words[i] >> 16, opcode = words[i] & 0xffff;
        if (!size || size > count - i) return "malformed_spirv";
        if (guard_contains(guard_opcodes, GUARD_COUNT(guard_opcodes), opcode)) return "native_rt_opcode";
        if (!guard_contains(guard_known_opcodes, GUARD_COUNT(guard_known_opcodes), opcode)) return "unknown_spirv_opcode";
        if (opcode == 17) { /* OpCapability */
            if (size != 2) return "malformed_spirv";
            if (guard_contains(guard_capabilities, GUARD_COUNT(guard_capabilities), words[i + 1])) return "native_rt_capability";
        }
        if (opcode == 15) { /* OpEntryPoint */
            if (size < 4) return "malformed_spirv";
            if (guard_contains(guard_models, GUARD_COUNT(guard_models), words[i + 1])) return "native_rt_execution_model";
        }
        if (opcode == 10) { /* OpExtension */
            if (size < 2) return "malformed_spirv";
            const char *names[] = {"ray_tracing", "ray_query", "shader_invocation_reorder", "opacity_micromap",
                                   "displacement_micromap"};
            for (size_t n = 0; n < GUARD_COUNT(names); ++n) {
                int found = guard_spirv_string(words + i + 1, size - 1, names[n]);
                if (found < 0) return "malformed_spirv";
                if (found) return "native_rt_extension";
            }
        }
        i += size;
    }
    return NULL;
}

static int guard_dependency(const VkDependencyInfo *info) {
    for (uint32_t i = 0; i < info->memoryBarrierCount; ++i) {
        const VkMemoryBarrier2 *p = &info->pMemoryBarriers[i];
        if (guard_stages(p->srcStageMask | p->dstStageMask) || guard_access(p->srcAccessMask | p->dstAccessMask)) return 1;
    }
    for (uint32_t i = 0; i < info->bufferMemoryBarrierCount; ++i) {
        const VkBufferMemoryBarrier2 *p = &info->pBufferMemoryBarriers[i];
        if (guard_stages(p->srcStageMask | p->dstStageMask) || guard_access(p->srcAccessMask | p->dstAccessMask)) return 1;
    }
    for (uint32_t i = 0; i < info->imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier2 *p = &info->pImageMemoryBarriers[i];
        if (guard_stages(p->srcStageMask | p->dstStageMask) || guard_access(p->srcAccessMask | p->dstAccessMask)) return 1;
    }
    return 0;
}

#endif
