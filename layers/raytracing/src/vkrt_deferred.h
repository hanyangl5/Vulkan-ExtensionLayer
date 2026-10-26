/* SPDX-License-Identifier: MIT */
#ifndef VKRT_DEFERRED_H
#define VKRT_DEFERRED_H

#include <stdbool.h>
#include <vulkan/vulkan.h>

struct vkrt_device_data;
struct vkrt_deferred_state;

VkResult vkrt_deferred_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator);
void vkrt_deferred_finish(struct vkrt_device_data *dev);

/* Optional deferral arguments may be null. Non-null handles must belong to
 * this device. Commands execute synchronously and never put a handle into a
 * pending state. On successful requested deferral, the command itself returns
 * VK_OPERATION_NOT_DEFERRED_KHR; the operation remains complete with SUCCESS. */
bool vkrt_deferred_valid(struct vkrt_device_data *dev, VkDeferredOperationKHR operation);
PFN_vkVoidFunction vkrt_deferred_proc(const char *name);

#endif
