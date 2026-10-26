/* SPDX-License-Identifier: MIT */
#ifndef VKRT_BUFFER_USAGE_H
#define VKRT_BUFFER_USAGE_H

#include "vkrt_common.h"
#include "vkrt_buffer_chain_storage.h"

/* Creation and creation-info queries must describe exactly the same native
 * buffer. Keep the 64-bit effective usage intact, including Flags2 bits which
 * cannot be represented by VkBufferCreateInfo::usage. */
static inline VkBufferUsageFlags2KHR vkrt_buffer_rt_usage_mask(void) {
    return (VkBufferUsageFlags2KHR)VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
}

static inline VkBufferUsageFlags2KHR vkrt_buffer_usage_translate(VkBufferUsageFlags2KHR usage) {
    if (!(usage & vkrt_buffer_rt_usage_mask())) return usage;
    /* Software AS storage, build inputs and SBT records use SSBO/BDA accesses.
     * Ordinary buffers retain their original creation requirements. */
    return (usage & ~vkrt_buffer_rt_usage_mask()) | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
}

static inline const VkBufferUsageFlags2CreateInfoKHR *vkrt_buffer_usage_flags2(const void *pNext) {
    for (const VkBaseInStructure *p = pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR) return (const void *)p;
    return NULL;
}

struct vkrt_buffer_usage_chain {
    const void *pNext;
    VkBaseOutStructure *prefix;
    VkBufferUsageFlags2CreateInfoKHR usage2;
    union vkrt_buffer_chain_node nodes[VKRT_BUFFER_CHAIN_NODE_COUNT];
    size_t node_count;
};

static inline VkResult vkrt_buffer_usage_chain_translate(struct vkrt_buffer_usage_chain *chain, const void *pNext) {
    *chain = (struct vkrt_buffer_usage_chain){.pNext = pNext};
    const VkBufferUsageFlags2CreateInfoKHR *existing = vkrt_buffer_usage_flags2(pNext);
    if (!existing || !(existing->usage & vkrt_buffer_rt_usage_mask())) return VK_SUCCESS;

    /* Only a modified node needs copying. Borrow its opaque suffix unchanged,
     * and copy the prefix so neither the caller's links nor payloads change.
     * Registry-derived slots cover every valid enabled-platform prefix, each
     * of which may occur once. Valid void queries cannot fail on allocation. */
    chain->usage2 = *existing;
    chain->usage2.usage = vkrt_buffer_usage_translate(existing->usage);
    VkBaseOutStructure **tail = &chain->prefix;
    for (const VkBaseInStructure *p = pNext; p != (const VkBaseInStructure *)existing; p = p->pNext) {
        size_t size = vkrt_buffer_chain_prefix_size(p->sType);
        if (!size || chain->node_count == VKRT_BUFFER_CHAIN_NODE_COUNT) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkBaseOutStructure *copy = (void *)&chain->nodes[chain->node_count++];
        memcpy(copy, p, size);
        copy->pNext = NULL;
        *tail = copy;
        tail = &copy->pNext;
    }
    *tail = (VkBaseOutStructure *)&chain->usage2;
    chain->pNext = chain->prefix;
    return VK_SUCCESS;
}

struct vkrt_buffer_create_info {
    VkBufferCreateInfo info;
    VkBufferUsageFlags2KHR original_usage;
    struct vkrt_buffer_usage_chain chain;
};

static inline VkResult vkrt_buffer_create_info_translate(struct vkrt_buffer_create_info *local, const VkBufferCreateInfo *info) {
    local->info = *info;
    const VkBufferUsageFlags2CreateInfoKHR *usage2 = vkrt_buffer_usage_flags2(info->pNext);
    local->original_usage = usage2 ? usage2->usage : info->usage;
    local->info.usage = (VkBufferUsageFlags)vkrt_buffer_usage_translate(info->usage);
    VkResult result = vkrt_buffer_usage_chain_translate(&local->chain, info->pNext);
    if (result == VK_SUCCESS) local->info.pNext = local->chain.pNext;
    return result;
}

#endif
