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
bool vkrt_rtp_debug_owns(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle);
VkResult vkrt_rtp_track_descriptor_layout(struct vkrt_device_data *dev, VkDescriptorSetLayout layout,
                                          const VkDescriptorSetLayoutCreateInfo *info);
void vkrt_rtp_forget_descriptor_layout(struct vkrt_device_data *dev, VkDescriptorSetLayout layout);
bool vkrt_rtp_layout_compatible(struct vkrt_device_data *dev, VkPipelineLayout a, VkPipelineLayout b, uint32_t last_set);
PFN_vkVoidFunction vkrt_rtp_proc(const char *name);
VkResult vkrt_rtp_create_shader(struct vkrt_device_data *dev, const VkShaderModuleCreateInfo *info,
                                const VkAllocationCallbacks *allocator, VkShaderModule *module, bool *handled);
/* Consume a successful native CreateShaderModule result. On bookkeeping failure,
 * destroy it with the live creation allocator and leave the public output NULL.
 * Devices without RTP ownership keep their native handle unchanged. */
VkResult vkrt_rtp_wrap_native_shader(struct vkrt_device_data *dev, VkShaderModule native, const VkAllocationCallbacks *allocator,
                                     VkShaderModule *module);
/* Borrow a tracked native module; raw source/untracked handles return false and
 * clear native. The public Vulkan object synchronization rules still apply. */
bool vkrt_rtp_native_shader(struct vkrt_device_data *dev, VkShaderModule module, VkShaderModule *native);
bool vkrt_rtp_destroy_shader(struct vkrt_device_data *dev, VkShaderModule module, const VkAllocationCallbacks *allocator);
void vkrt_rtp_reset_command(struct vkrt_cmd_buffer_data *cb);
/* Borrow a private native view of an owned public layout. Without RTP, native
 * handles pass through. On an RTP device, NULL/untracked handles return NULL.
 * The command helper also rejects a missing nonzero public/replay identity via
 * record_result. Private implementation handles must bypass these accessors. */
VkPipelineLayout vkrt_rtp_native_layout(struct vkrt_device_data *dev, VkPipelineLayout layout);
VkPipelineLayout vkrt_rtp_command_native_layout(struct vkrt_cmd_buffer_data *cb, VkPipelineLayout layout);
/* Record immutable semantic identities before public layout destruction or
 * handle reuse. They are private to a recording and must not reach the ICD. */
VkResult vkrt_rtp_replay_layout_capture(struct vkrt_cmd_buffer_data *cb, VkPipelineLayout layout, VkPipelineLayout *identity);
VkResult vkrt_rtp_replay_layouts_inherit(struct vkrt_cmd_buffer_data *target, const struct vkrt_cmd_buffer_data *source);
VkResult vkrt_rtp_replay_layout_native(struct vkrt_cmd_buffer_data *cb, VkPipelineLayout identity, VkPipelineLayout *native_layout);
bool vkrt_rtp_bind_pipeline(struct vkrt_cmd_buffer_data *cb, VkPipelineBindPoint point, VkPipeline pipeline);
VkShaderStageFlags vkrt_rtp_push(struct vkrt_cmd_buffer_data *cb, VkPipelineLayout layout, VkShaderStageFlags stages,
                                 uint32_t offset, uint32_t size, const void *values);
#ifdef __cplusplus
}
#endif
#endif
