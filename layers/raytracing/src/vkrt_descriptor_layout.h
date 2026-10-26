/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_DESCRIPTOR_LAYOUT_H
#define VKRT_DESCRIPTOR_LAYOUT_H
#include <vulkan/vulkan.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Share exactly the same representation between layout creation and support
 * queries. The owned array is NULL on passthrough and freed with free().
 * pNext, binding numbers, array counts and immutable sampler pointers are
 * borrowed unchanged. No descriptor/pipeline limit is imposed by this query. */
VkResult vkrt_prepare_descriptor_layout(bool virtual_rt, const VkDescriptorSetLayoutCreateInfo *source,
                                        VkDescriptorSetLayoutCreateInfo *native, VkDescriptorSetLayoutBinding **owned);
/* Duplicate pool rows are legal. Keep their positions and counts, changing
 * only AS->SSBO, rather than summing uint32 counts or shifting mutable lists. */
VkResult vkrt_prepare_descriptor_pool(bool virtual_rt, const VkDescriptorPoolCreateInfo *source, VkDescriptorPoolCreateInfo *native,
                                      VkDescriptorPoolSize **owned);
PFN_vkVoidFunction vkrt_descriptor_layout_proc(const char *name);
#ifdef __cplusplus
}
#endif
#endif
