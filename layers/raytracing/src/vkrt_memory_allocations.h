/* SPDX-License-Identifier: MIT */
#ifndef VKRT_MEMORY_ALLOCATIONS_H
#define VKRT_MEMORY_ALLOCATIONS_H
#include "vkrt_common.h"

struct vkrt_device_data;
/* Tracks actual native memory objects, including calls in flight. This does
 * not reserve a private quota or change the public memory limits/heaps. */
VkResult vkrt_memory_allocations_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator);
void vkrt_memory_allocations_finish(struct vkrt_device_data *dev);
VkResult vkrt_memory_allocate_native(struct vkrt_device_data *dev, const VkMemoryAllocateInfo *info,
                                     const VkAllocationCallbacks *allocator, VkDeviceMemory *memory);
void vkrt_memory_free_native(struct vkrt_device_data *dev, VkDeviceMemory memory, const VkAllocationCallbacks *allocator);
#endif
