/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef VKRT_PRIVATE_POOL_H
#define VKRT_PRIVATE_POOL_H
#include <vulkan/vulkan.h>
#include <stdbool.h>

struct vkrt_device_data;
struct vkrt_private_allocation;

/* A slice owns an exclusive lease until release. Host mappings may be unaligned;
 * access them by memcpy. Device-local-preferred slices are never mapped. */
struct vkrt_private_slice {
    VkBuffer buffer;
    VkDeviceSize offset, size;
    VkDeviceAddress address;
    void *mapping;
    struct vkrt_private_allocation *allocation;
};

enum vkrt_private_memory_class { VKRT_PRIVATE_HOST_COHERENT, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED };

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize once before concurrent use. No GPU memory is allocated here.
 * Cache supported native buffer/allocation limits without inventing limits for
 * unavailable/ignored queries. Finish after registry/AS users, before the native
 * device and allocator owner. */
VkResult vkrt_private_pool_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator);
void vkrt_private_pool_finish(struct vkrt_device_data *dev);
/* Align the physical BDA and the native buffer offset independently. Both
 * alignments must be nonzero powers of two. Incompatible constraints fail;
 * neither promise is silently weakened. Memory classes never share chunks.
 * Native chunk sizes include alignment padding; known buffer/allocation limits
 * fail with OUT_OF_DEVICE_MEMORY before the corresponding native call.
 * HOST_COHERENT slices are mapped and zeroed. DEVICE_LOCAL_PREFERRED slices
 * prefer device-local memory, fall back to another buffer-compatible memory
 * type if needed, are not mapped/initialized, and require device-side ordering
 * before reuse. A live slice is never reused by the allocator. */
VkResult vkrt_private_pool_allocate_ex(struct vkrt_device_data *dev, VkDeviceSize size, enum vkrt_private_memory_class memory_class,
                                       VkDeviceSize physical_alignment, VkDeviceSize native_offset_alignment,
                                       struct vkrt_private_slice *slice);
/* descriptor=true aligns the buffer offset to max(minStorageBufferOffsetAlignment,
 * 8), also satisfying native transfer-copy source offset alignment. Its physical
 * BDA may have any alignment; byte-addressed shader output is required there.
 * Otherwise the physical BDA is eight-byte aligned, while the buffer offset and
 * CPU mapping may be unaligned. Access mapped values with memcpy. Pool buffers
 * support STORAGE_BUFFER, SHADER_DEVICE_ADDRESS, and TRANSFER_SRC usage. */
VkResult vkrt_private_pool_allocate(struct vkrt_device_data *dev, VkDeviceSize size, bool descriptor,
                                    struct vkrt_private_slice *slice);
void vkrt_private_pool_release(struct vkrt_device_data *dev, struct vkrt_private_slice *slice);
#ifdef __cplusplus
}
#endif
#endif
