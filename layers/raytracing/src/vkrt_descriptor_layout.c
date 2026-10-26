/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_descriptor_layout.h"
#include "vkrt_layer.h"
#include "vkrt_rt_capabilities.h"

static bool mutable_as(const void *next) {
    for (const VkBaseInStructure *p = next; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT) continue;
        const VkMutableDescriptorTypeCreateInfoEXT *m = (const void *)p;
        for (uint32_t i = 0; i < m->mutableDescriptorTypeListCount; ++i)
            for (uint32_t j = 0; j < m->pMutableDescriptorTypeLists[i].descriptorTypeCount; ++j)
                if (m->pMutableDescriptorTypeLists[i].pDescriptorTypes[j] == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)
                    return true;
    }
    return false;
}

VkResult vkrt_prepare_descriptor_layout(bool virtual_rt, const VkDescriptorSetLayoutCreateInfo *source,
                                        VkDescriptorSetLayoutCreateInfo *native, VkDescriptorSetLayoutBinding **owned) {
    if (!source || !native || !owned) return VK_ERROR_INITIALIZATION_FAILED;
    *native = *source;
    *owned = NULL;
    if (!virtual_rt) return VK_SUCCESS;
    if (mutable_as(source->pNext)) return VK_ERROR_FEATURE_NOT_PRESENT;
    bool rewrite = false;
    for (uint32_t i = 0; i < source->bindingCount; ++i) {
        const VkDescriptorSetLayoutBinding *b = &source->pBindings[i];
        rewrite |= b->descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR ||
                   vkrt_compute_shader_stages(b->stageFlags) != b->stageFlags;
    }
    if (!rewrite) return VK_SUCCESS;
    /* These command encodings need their own translation. In particular an
     * AS descriptor-buffer payload is not an SSBO address-cell descriptor. */
    if (source->flags &
        (VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT | VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if ((uint64_t)source->bindingCount * sizeof(**owned) > SIZE_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkDescriptorSetLayoutBinding *bindings = malloc((size_t)source->bindingCount * sizeof(*bindings));
    if (!bindings) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < source->bindingCount; ++i) {
        bindings[i] = source->pBindings[i];
        bindings[i].stageFlags = vkrt_compute_shader_stages(bindings[i].stageFlags);
        if (bindings[i].descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    native->pBindings = bindings;
    *owned = bindings;
    return VK_SUCCESS;
}

VkResult vkrt_prepare_descriptor_pool(bool virtual_rt, const VkDescriptorPoolCreateInfo *source, VkDescriptorPoolCreateInfo *native,
                                      VkDescriptorPoolSize **owned) {
    if (!source || !native || !owned) return VK_ERROR_INITIALIZATION_FAILED;
    *native = *source;
    *owned = NULL;
    if (!virtual_rt) return VK_SUCCESS;
    if (mutable_as(source->pNext)) return VK_ERROR_FEATURE_NOT_PRESENT;
    bool rewrite = false;
    for (uint32_t i = 0; i < source->poolSizeCount; ++i)
        rewrite |= source->pPoolSizes[i].type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    if (!rewrite) return VK_SUCCESS;
    if ((uint64_t)source->poolSizeCount * sizeof(**owned) > SIZE_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkDescriptorPoolSize *sizes = malloc((size_t)source->poolSizeCount * sizeof(*sizes));
    if (!sizes) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < source->poolSizeCount; ++i) {
        sizes[i] = source->pPoolSizes[i];
        if (sizes[i].type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) sizes[i].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    native->pPoolSizes = sizes;
    *owned = sizes;
    return VK_SUCCESS;
}

static void unsupported(VkDescriptorSetLayoutSupport *support) {
    support->supported = VK_FALSE;
    for (VkBaseOutStructure *p = support->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT)
            ((VkDescriptorSetVariableDescriptorCountLayoutSupport *)p)->maxVariableDescriptorCount = 0;
}

static VKAPI_ATTR void VKAPI_CALL GetDescriptorSetLayoutSupport(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
                                                                VkDescriptorSetLayoutSupport *support) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !dev->vtable.GetDeviceProcAddr) {
        unsupported(support);
        return;
    }
    PFN_vkGetDescriptorSetLayoutSupport query =
        (PFN_vkGetDescriptorSetLayoutSupport)dev->vtable.GetDeviceProcAddr(device, "vkGetDescriptorSetLayoutSupport");
    if (!query)
        query = (PFN_vkGetDescriptorSetLayoutSupport)dev->vtable.GetDeviceProcAddr(device, "vkGetDescriptorSetLayoutSupportKHR");
    if (!query) {
        unsupported(support);
        return;
    }
    VkDescriptorSetLayoutCreateInfo native;
    VkDescriptorSetLayoutBinding *owned;
    VkResult result = vkrt_prepare_descriptor_layout(dev->virtual_rt, info, &native, &owned);
    if (result != VK_SUCCESS) {
        unsupported(support);
        return;
    }
    query(device, &native, support);
    free(owned);
    /* Do not clamp maxVariableDescriptorCount to public pipeline limits. This
     * query deliberately ignores those limits and must agree with creatability,
     * including the maxPerSetDescriptors guarantee. A supported requested count
     * must never exceed its returned maxVariableDescriptorCount. */
}

PFN_vkVoidFunction vkrt_descriptor_layout_proc(const char *name) {
    if (!strcmp(name, "vkGetDescriptorSetLayoutSupport") || !strcmp(name, "vkGetDescriptorSetLayoutSupportKHR"))
        return (PFN_vkVoidFunction)GetDescriptorSetLayoutSupport;
    return NULL;
}
