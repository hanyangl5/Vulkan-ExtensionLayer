/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT */
#ifndef VKRT_SHADER_INTERFACE_H
#define VKRT_SHADER_INTERFACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
struct vkrt_device_data;
struct vkrt_shader_interface_state;
struct vkrt_shader_interface;

/* Device-owned metadata, with device then instance allocation callbacks.
 * No-op on devices without shaderObject. The root wrappers hold this lock
 * across downstream Create/Destroy plus track/forget, preventing handle reuse
 * between native lifetime changes and metadata publication. */
VkResult vkrt_shader_interface_init(struct vkrt_device_data *dev, const VkDeviceCreateInfo *original);
void vkrt_shader_interface_finish(struct vkrt_device_data *dev);
void vkrt_shader_interface_lock(struct vkrt_device_data *dev);
void vkrt_shader_interface_unlock(struct vkrt_device_data *dev);
VkResult vkrt_shader_interface_track_conversion_locked(struct vkrt_device_data *dev, VkSamplerYcbcrConversion conversion,
                                                       const VkSamplerYcbcrConversionCreateInfo *info);
void vkrt_shader_interface_forget_conversion_locked(struct vkrt_device_data *dev, VkSamplerYcbcrConversion conversion);
VkResult vkrt_shader_interface_track_sampler_locked(struct vkrt_device_data *dev, VkSampler sampler,
                                                    const VkSamplerCreateInfo *info);
void vkrt_shader_interface_forget_sampler_locked(struct vkrt_device_data *dev, VkSampler sampler);
VkResult vkrt_shader_interface_track_layout_locked(struct vkrt_device_data *dev, VkDescriptorSetLayout layout,
                                                   const VkDescriptorSetLayoutCreateInfo *info);
void vkrt_shader_interface_forget_layout_locked(struct vkrt_device_data *dev, VkDescriptorSetLayout layout);

/* Capture the original public interface, robustness defaults and source
 * translation policy. IF wire version 2 uses header byte24 for the four-bit
 * source robustness mask and byte28 for its push-range policy.
 * Bytes contain explicit LE integers/float bits, never Vulkan handles/pointers.
 * Unknown optional definitions remain marked unknown in the tracker; capture
 * returns FEATURE_NOT_PRESENT instead of dropping their semantics.
 * Capture locks internally. Free bytes before destroying their parent device. */
VkResult vkrt_shader_interface_capture(struct vkrt_device_data *dev, const VkShaderCreateInfoEXT *info, void **bytes, size_t *size);
void vkrt_shader_interface_free_bytes(struct vkrt_device_data *dev, void *bytes);

/* Validate the complete recipe before creating private native objects.
 * Materialization applies only the recipe's source AS/RT translation policy,
 * regardless of the destination device's public virtual_rt profile. */
VkResult vkrt_shader_interface_validate(const void *bytes, size_t size);
/* Check every decoded temporary SPIR-V group member before compiling any of
 * them. Known registry capabilities use original enabled features, native
 * device version/extension alternatives and actual private backend bits.
 * Unknown optional construction modes return FEATURE_NOT_PRESENT rather
 * than dropping flags or enabling features. This is not full per-operation
 * or extension capability qualification. */
VkResult vkrt_shader_interface_source_supported(struct vkrt_device_data *dev, const VkShaderCreateInfoEXT *info);
VkResult vkrt_shader_interface_materialize(struct vkrt_device_data *dev, const void *bytes, size_t size,
                                           struct vkrt_shader_interface **owned);
void vkrt_shader_interface_apply(const struct vkrt_shader_interface *owned, VkShaderCreateInfoEXT *info);
void vkrt_shader_interface_destroy(struct vkrt_device_data *dev, struct vkrt_shader_interface *owned);
#ifdef __cplusplus
}
#endif
#endif
