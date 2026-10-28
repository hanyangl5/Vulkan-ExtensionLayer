/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_DEBUG_OBJECTS_H
#define VKRT_DEBUG_OBJECTS_H
#include <stdbool.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
struct vkrt_device_data;
/* Annotation and private-data sidecars use the same owned object lifetimes.
 * The integration checks actual typed registries, never handle bit patterns.
 * Destroyers unlink from their registry before calling forget, outside that
 * registry's lock. Debug commands synchronize their registry check and store. */
typedef bool (*vkrt_debug_object_owns)(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle);
VkResult vkrt_debug_objects_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator, vkrt_debug_object_owns owns);
void vkrt_debug_objects_finish(struct vkrt_device_data *dev);
void vkrt_debug_object_forget(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle);
PFN_vkVoidFunction vkrt_debug_objects_proc(const char *name);
/* The existing virtual-AS registry also checks device ownership. */
bool vkrt_virtual_as_owns(struct vkrt_device_data *dev, uint64_t handle);
#ifdef __cplusplus
}
#endif
#endif
