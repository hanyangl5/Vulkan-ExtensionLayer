/* SPDX-License-Identifier: MIT */
#ifndef VKRT_AS_COPY_H
#define VKRT_AS_COPY_H
#include <vulkan/vulkan.h>
struct vkrt_device_data;
struct vkrt_as_copy_args {
    uint64_t source, destination, source_capacity, destination_capacity;
    uint32_t destination_type, reserved;
};
VkResult vkrt_as_copy_init(struct vkrt_device_data *dev);
void vkrt_as_copy_finish(struct vkrt_device_data *dev);
void vkrt_cmd_copy_as(struct vkrt_device_data *dev, VkCommandBuffer command, const VkCopyAccelerationStructureInfoKHR *info);
#endif
