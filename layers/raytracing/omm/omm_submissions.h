// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "omm_acceleration.h"
#include <vector>

namespace vel::omm {
struct SubmissionTracker;
struct SubmissionTrackerCreateInfo {
    VkDevice device{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    AccelerationBackend* acceleration{};
    const VkAllocationCallbacks* allocator{};
};
VkResult submissions_create(const SubmissionTrackerCreateInfo&, SubmissionTracker**);
// Requires completed work (or device-lost teardown), no concurrent tracker calls,
// and a still-valid native VkDevice. Does not inject waits or submissions.
void submissions_destroy(SubmissionTracker*);
// Inputs already contain the layer's synchronization translations. Tokens must
// cover AS writes in direct and secondary command buffers. Retained transactionally
// before the native submit. A fence-less AS-write batch receives a private fence
// on the SAME native submit; application handles and submission arrays stay intact.
VkResult submissions_queue_submit(SubmissionTracker*, VkQueue, uint32_t, const VkSubmitInfo*, VkFence,
                                  const std::vector<AccelerationRecordingRef>&);
VkResult submissions_queue_submit2(SubmissionTracker*, VkQueue, uint32_t, const VkSubmitInfo2*, VkFence,
                                   const std::vector<AccelerationRecordingRef>&);
// Polling never waits. Completed old entries are not repolled after fence reuse.
// A completed recording retires only when every writer to any AS it touches is
// complete; unrelated AS writers on other queues do not delay its retirement.
// Blocking native submit/wait calls run outside the tracker mutex so another
// queue can submit semaphore signals. In-flight submit prefixes block collection;
// idle waits mark only writer generations committed before that wait began.
VkResult submissions_poll(SubmissionTracker*);
VkResult submissions_wait_for_fences(SubmissionTracker*, uint32_t, const VkFence*, VkBool32, uint64_t);
VkResult submissions_get_fence_status(SubmissionTracker*, VkFence);
VkResult submissions_reset_fences(SubmissionTracker*, uint32_t, const VkFence*);
VkResult submissions_destroy_fence(SubmissionTracker*, VkFence, const VkAllocationCallbacks*);
VkResult submissions_queue_wait_idle(SubmissionTracker*, VkQueue);
VkResult submissions_device_wait_idle(SubmissionTracker*);
}  // namespace vel::omm
