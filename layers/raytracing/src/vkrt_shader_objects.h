/* Copyright (c) 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT */
#ifndef VKRT_SHADER_OBJECTS_H
#define VKRT_SHADER_OBJECTS_H
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
PFN_vkVoidFunction vkrt_shader_object_proc(const char *name);
PFN_vkVoidFunction vkrt_shader_object_instance_proc(const char *name);
struct vkrt_device_data;
VkResult vkrt_shader_objects_init(struct vkrt_device_data *dev);
void vkrt_shader_objects_finish(struct vkrt_device_data *dev);
#ifdef __cplusplus
}
#endif
#endif
