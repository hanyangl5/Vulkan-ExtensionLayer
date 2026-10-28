/* SPDX-License-Identifier: MIT */
#ifndef VKRT_QUERY_POOL_H
#define VKRT_QUERY_POOL_H
#include "vkrt_common.h"
struct vkrt_device_data;
struct vkrt_cmd_buffer_data;
struct vkrt_query_device;
struct vkrt_query_command;
struct vkrt_query_admission;

/* Owned query recordings replay with execution-private storage. Native query
 * overlap and unsupported recording contexts remain explicitly bounded. */
VkResult vkrt_query_device_init(struct vkrt_device_data *dev);
void vkrt_query_device_finish(struct vkrt_device_data *dev);
bool vkrt_query_pool_owns(struct vkrt_device_data *dev, uint64_t handle);
VkResult vkrt_query_command_reset(struct vkrt_cmd_buffer_data *cb);
/* Query-only secondary qualification and publication bookkeeping. RTP-only
 * recordings do not inherit native-query/timestamp restrictions from here. */
bool vkrt_query_record_children(struct vkrt_cmd_buffer_data *cb, uint32_t count, const VkCommandBuffer *children);
struct vkrt_query_submission {
    uint8_t identity;
    VkQueue queue;
    struct vkrt_query_admission *admission;
};
/* Serialize one queue's source preparation/native-submit/publication transaction.
 * Release immediately after submit_result, before any observer native call.
 * Never hold this admission across native Queue/DeviceWaitIdle. */
VkResult vkrt_query_submit_begin(struct vkrt_device_data *dev, struct vkrt_query_submission *transaction, VkQueue queue);
void vkrt_query_submit_end(struct vkrt_device_data *dev, struct vkrt_query_submission *transaction);
VkResult vkrt_query_submit_prepare(struct vkrt_device_data *dev, const struct vkrt_query_submission *transaction, uint32_t count,
                                   const VkCommandBuffer *commands);
void vkrt_query_submit_result(struct vkrt_device_data *dev, const struct vkrt_query_submission *transaction, VkResult result);
PFN_vkVoidFunction vkrt_query_pool_proc(const char *name);

#endif
