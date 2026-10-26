/* SPDX-License-Identifier: MIT */
#ifndef VKRT_AS_REGISTRY_H
#define VKRT_AS_REGISTRY_H
#include <vulkan/vulkan.h>
#include "vkrt_private_pool.h"

struct vkrt_device_data;
struct vkrt_as_registry;
struct vkrt_as_registry_entry;

#define VKRT_AS_REGISTRY_MAGIC 0x52534156u
#define VKRT_AS_REGISTRY_VERSION 1u
#define VKRT_AS_REGISTRY_LEVELS 7u
#define VKRT_AS_REGISTRY_SLOTS 256u

VkResult vkrt_as_registry_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator);
void vkrt_as_registry_finish(struct vkrt_device_data *dev);
VkDeviceAddress vkrt_as_registry_address(struct vkrt_device_data *dev);
VkResult vkrt_as_registry_buffer_create(struct vkrt_device_data *dev, VkBuffer buffer, VkDeviceSize size);
void vkrt_as_registry_buffer_destroy(struct vkrt_device_data *dev, VkBuffer buffer);
VkResult vkrt_as_registry_acquire(struct vkrt_device_data *dev, VkBuffer buffer, VkDeviceSize offset,
                                  struct vkrt_as_registry_entry **entry, VkDeviceAddress *public_address);
void vkrt_as_registry_release(struct vkrt_device_data *dev, struct vkrt_as_registry_entry *entry);
const struct vkrt_private_slice *vkrt_as_registry_descriptor(const struct vkrt_as_registry_entry *entry);
VkDeviceAddress vkrt_as_registry_publish(struct vkrt_device_data *dev, struct vkrt_as_registry_entry *entry,
                                         VkDeviceAddress physical_address);
#endif
