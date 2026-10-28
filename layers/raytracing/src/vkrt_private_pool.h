/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef VKRT_PRIVATE_POOL_H
#define VKRT_PRIVATE_POOL_H
#include <vulkan/vulkan.h>
#include <stdbool.h>

struct vkrt_device_data;
struct vkrt_private_allocation;

/* Device-owned coherent storage shared by sparse AS pages and descriptor
 * values. The CPU and BDA alignments are independent; access mapping by memcpy. */
struct vkrt_private_slice {
    VkBuffer buffer;
    VkDeviceSize offset, size;
    VkDeviceAddress address;
    void *mapping;
    struct vkrt_private_allocation *allocation;
};

/* Initialize once before concurrent use. No GPU memory is allocated here.
 * Finish after registry/AS users, before the native device and allocator owner. */
VkResult vkrt_private_pool_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator);
void vkrt_private_pool_finish(struct vkrt_device_data *dev);
/* descriptor=true aligns the buffer offset to minStorageBufferOffsetAlignment;
 * otherwise the physical address is eight-byte aligned. Allocation never
 * assumes that the native buffer BDA itself is eight-byte aligned. */
VkResult vkrt_private_pool_allocate(struct vkrt_device_data *dev, VkDeviceSize size, bool descriptor,
                                    struct vkrt_private_slice *slice);
void vkrt_private_pool_release(struct vkrt_device_data *dev, struct vkrt_private_slice *slice);
#endif
