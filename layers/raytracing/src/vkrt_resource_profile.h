/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_RESOURCE_PROFILE_H
#define VKRT_RESOURCE_PROFILE_H
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* This is a descriptor-budget proof for the flat RTP compute module, not a
 * complete backend capability predicate. In particular it says nothing about
 * private memory allocation counts, shader coverage or AS construction limits. */
struct vkrt_resource_native {
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceDescriptorIndexingProperties indexing;
    VkPhysicalDeviceDescriptorIndexingFeatures indexing_features;
    VkPhysicalDeviceInlineUniformBlockProperties inline_uniform;
    VkPhysicalDeviceInlineUniformBlockFeatures inline_features;
    VkPhysicalDeviceMaintenance7PropertiesKHR maintenance7;
    VkBool32 descriptor_indexing;
    VkBool32 has_maintenance7;
    uint32_t max_inline_uniform_total_size;
};

enum vkrt_resource_failure {
    VKRT_RESOURCE_OK,
    VKRT_RESOURCE_BAD_ARGUMENT,
    VKRT_RESOURCE_API_VERSION,
    VKRT_RESOURCE_CORE_MINIMUM,
    VKRT_RESOURCE_STAGE_HEADROOM,
    VKRT_RESOURCE_SET_HEADROOM,
    VKRT_RESOURCE_AGGREGATE_HEADROOM,
    VKRT_RESOURCE_UPDATE_AFTER_BIND_MINIMUM,
    VKRT_RESOURCE_INLINE_MINIMUM,
    VKRT_RESOURCE_MAINTENANCE7_MINIMUM,
};

struct vkrt_resource_profile {
    VkPhysicalDeviceLimits limits;
    VkPhysicalDeviceDescriptorIndexingProperties indexing;
    VkPhysicalDeviceInlineUniformBlockProperties inline_uniform;
    VkPhysicalDeviceMaintenance7PropertiesKHR maintenance7;
    uint32_t max_inline_uniform_total_size;
    uint32_t as_per_stage, as_per_set;
    /* Proven upper bounds after AS descriptors become native SSBOs. */
    uint64_t core_resource_bound, all_resource_bound;
};

/* On failure, output remains untouched. The conservative minimums cover all
 * six core graphics stages and the higher Vulkan 1.4 limits when reported.
 * Every public AS limit is 16; AS update-after-bind remains unsupported. */
bool vkrt_plan_resource_profile(const struct vkrt_resource_native *native, struct vkrt_resource_profile *output,
                                enum vkrt_resource_failure *failure);

/* Uses downstream physical-device functions, never this layer's discovery
 * wrappers. Optional outputs start zero so ignored extension structs do not
 * fabricate support. No caller input/output chains are borrowed or modified.
 * Both functions leave output untouched on failure; captured links are NULL. */
bool vkrt_capture_resource_native(PFN_vkGetPhysicalDeviceProperties2 properties, PFN_vkGetPhysicalDeviceFeatures2 features,
                                  VkPhysicalDevice physical, struct vkrt_resource_native *output,
                                  enum vkrt_resource_failure *failure);
bool vkrt_query_resource_profile(PFN_vkGetPhysicalDeviceProperties2 properties, PFN_vkGetPhysicalDeviceFeatures2 features,
                                 VkPhysicalDevice physical, struct vkrt_resource_profile *output,
                                 enum vkrt_resource_failure *failure);

/* Apply only descriptor-related fields. Output-chain addresses and unrelated
 * native properties are preserved, including Vulkan 1.2/1.3 aliases. */
void vkrt_apply_resource_profile(const struct vkrt_resource_profile *profile, VkPhysicalDeviceProperties2 *properties);

#ifdef __cplusplus
}
#endif

#endif
