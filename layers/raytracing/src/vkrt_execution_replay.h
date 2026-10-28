/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_EXECUTION_REPLAY_H
#define VKRT_EXECUTION_REPLAY_H
#include <vulkan/vulkan.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

struct vkrt_device_data;
struct vkrt_execution_device;
struct vkrt_execution_batch;

VkResult vkrt_execution_device_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator);
/* Exclusive device teardown: no concurrent helper/native submission calls.
 * Only here, submitted batches trigger a native DeviceWaitIdle drain before
 * freeing hidden observer resources. OOM/UNKNOWN retry with a 1ms backoff until
 * SUCCESS or DEVICE_LOST; no fixed retry budget permits an unsafe release.
 * Normal submission/collect paths never use this drain or readback mechanism. */
void vkrt_execution_device_finish(struct vkrt_device_data *dev);

VkResult vkrt_execution_batch_create(struct vkrt_device_data *dev, VkQueue queue, struct vkrt_execution_batch **batch);
/* A top-level source with no virtual query dependency is returned unchanged.
 * Every secondary occurrence inside a replayed parent is cloned, including
 * query-free children that may own mutable RTP frame packets.
 * A batch is prepared by one thread; failure poisons it until discarded. */
VkResult vkrt_execution_batch_clone(struct vkrt_execution_batch *batch, VkCommandBuffer source, VkCommandBuffer *target);
VkResult vkrt_execution_batch_status(const struct vkrt_execution_batch *batch);
size_t vkrt_execution_batch_clone_count(const struct vkrt_execution_batch *batch);
VkCommandBuffer vkrt_execution_batch_clone_command(const struct vkrt_execution_batch *batch, size_t index);
/* Private observation only. Never substitute this for the ORIGINAL caller's
 * fence: batch_submitted performs its own separate empty native observation. */
VkFence vkrt_execution_batch_completion_fence(const struct vkrt_execution_batch *batch);

/* After successful ORIGINAL work submission, consume the batch and issue an
 * empty native observation submit with its private fence. Observation OOM or
 * UNKNOWN retains resources and still returns SUCCESS for accepted work; only
 * DEVICE_LOST is reported. No public fence or semaphore payload is changed. */
VkResult vkrt_execution_batch_submitted(struct vkrt_execution_batch *batch);
/* Original work returned LOST/UNKNOWN and may have partial native effects.
 * Consume without observation; retain until explicit idle proof or teardown. */
void vkrt_execution_batch_quarantine(struct vkrt_execution_batch *batch);
/* Discard only an unsubmitted batch, after rolling back any query preparation
 * transaction. Pending work returns VK_NOT_READY. */
VkResult vkrt_execution_batch_discard(struct vkrt_execution_batch *batch);
/* Poll successful private observations. A successful fence also covers older
 * settled batches on the same queue, including failed observations. In-flight
 * native calls and original-work quarantine are excluded. Call without other
 * device/query/command locks held. The count output is optional. */
VkResult vkrt_execution_collect(struct vkrt_device_data *dev, size_t *retired_count);
/* Capture BEFORE native idle. After successful QueueWaitIdle(queue) or
 * DeviceWaitIdle(NULL), retire only settled batches bounded by that checkpoint. */
uint64_t vkrt_execution_checkpoint(struct vkrt_device_data *dev);
void vkrt_execution_queue_completed(struct vkrt_device_data *dev, VkQueue queue, uint64_t checkpoint);
#ifdef __cplusplus
}
#endif
#endif
