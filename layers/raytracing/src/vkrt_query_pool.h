/* SPDX-License-Identifier: MIT */
#ifndef VKRT_QUERY_POOL_H
#define VKRT_QUERY_POOL_H
#include "vkrt_common.h"
struct vkrt_device_data;
struct vkrt_cmd_buffer_data;
struct vkrt_query_device;
struct vkrt_query_command;
struct vkrt_query_admission;

/* Owned query recordings replay with execution-private storage. Active native
 * queries and unresolved logical-stage interactions remain explicitly bounded. */
VkResult vkrt_query_device_init(struct vkrt_device_data *dev);
void vkrt_query_device_finish(struct vkrt_device_data *dev);
void vkrt_query_forget_command_pool(struct vkrt_device_data *dev, VkCommandPool pool);
VkResult vkrt_query_command_reset(struct vkrt_cmd_buffer_data *cb);
bool vkrt_query_command_pool_info(struct vkrt_device_data *dev, VkCommandPool pool, uint32_t *family,
                                  VkCommandPoolCreateFlags *flags);
bool vkrt_query_command_requires_replay(const struct vkrt_cmd_buffer_data *cb);
struct vkrt_query_submission {
    uint8_t identity;
    VkQueue queue;
    bool unsafe_waits;
    struct vkrt_query_admission *admission;
};
/* Native query copies are TRANSFER operations; their private formatter is CS.
 * Retain unsafe wait history rather than silently extending semaphore scopes. */
VkResult vkrt_query_submit_qualify(struct vkrt_device_data *dev, struct vkrt_query_submission *transaction, VkQueue queue,
                                   bool unsafe_wait, uint32_t count, const VkCommandBuffer *commands);
/* Serialize one queue's source qualify/native-submit/publication transaction.
 * Release immediately after submit_result, before any observer native call.
 * Never hold this admission across native Queue/DeviceWaitIdle. */
VkResult vkrt_query_submit_begin(struct vkrt_device_data *dev, struct vkrt_query_submission *transaction, VkQueue queue);
void vkrt_query_submit_end(struct vkrt_device_data *dev, struct vkrt_query_submission *transaction);
/* Capture before native idle; clearing is bounded to already-published history. */
uint64_t vkrt_query_queue_checkpoint(struct vkrt_device_data *dev);
void vkrt_query_queue_idle(struct vkrt_device_data *dev, VkQueue queue, uint64_t checkpoint);
VkResult vkrt_query_submit_prepare(struct vkrt_device_data *dev, const struct vkrt_query_submission *transaction, uint32_t count,
                                   const VkCommandBuffer *commands);
void vkrt_query_submit_result(struct vkrt_device_data *dev, const struct vkrt_query_submission *transaction, VkResult result);
PFN_vkVoidFunction vkrt_query_pool_proc(const char *name);

#endif
