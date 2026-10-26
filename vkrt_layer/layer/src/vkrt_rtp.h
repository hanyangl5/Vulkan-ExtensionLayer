/* SPDX-License-Identifier: MIT */
#ifndef VKRT_RTP_H
#define VKRT_RTP_H
#include <stdbool.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
struct vkrt_device_data;
struct vkrt_cmd_buffer_data;
struct vkrt_rtp_state;
struct vkrt_rtp_command;
VkResult vkrt_rtp_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator);
void vkrt_rtp_finish(struct vkrt_device_data *dev);
PFN_vkVoidFunction vkrt_rtp_proc(const char *name);
VkResult vkrt_rtp_create_shader(struct vkrt_device_data *dev, const VkShaderModuleCreateInfo *info,
                                const VkAllocationCallbacks *allocator, VkShaderModule *module, bool *handled);
bool vkrt_rtp_destroy_shader(struct vkrt_device_data *dev, VkShaderModule module);
void vkrt_rtp_reset_command(struct vkrt_cmd_buffer_data *cb);
VkPipelineLayout vkrt_rtp_native_layout(struct vkrt_device_data *dev, VkPipelineLayout layout);
bool vkrt_rtp_bind_pipeline(struct vkrt_cmd_buffer_data *cb, VkPipelineBindPoint point, VkPipeline pipeline);
VkShaderStageFlags vkrt_rtp_push(struct vkrt_cmd_buffer_data *cb, VkPipelineLayout layout, VkShaderStageFlags stages,
                                 uint32_t offset, uint32_t size, const void *values);
#ifdef __cplusplus
}
#endif
#endif
