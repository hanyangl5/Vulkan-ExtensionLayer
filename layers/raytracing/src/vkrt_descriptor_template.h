/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_DESCRIPTOR_TEMPLATE_H
#define VKRT_DESCRIPTOR_TEMPLATE_H
#include "vkrt_replay.h"
#ifdef __cplusplus
extern "C" {
#endif
struct vkrt_device_data;
/* Source capture consumes metadata immediately and records only ordinary push
 * writes. The template may be destroyed before the recorded command executes. */
VkResult vkrt_descriptor_template_capture(struct vkrt_replay_context *context, const VkPushDescriptorSetWithTemplateInfo *info);
PFN_vkVoidFunction vkrt_descriptor_template_proc(const char *name);
/* Call before releasing the native device; command histories own their values. */
void vkrt_descriptor_template_finish(struct vkrt_device_data *dev);
bool vkrt_descriptor_template_owns(struct vkrt_device_data *dev, uint64_t handle);
#ifdef __cplusplus
}
#endif
#endif
