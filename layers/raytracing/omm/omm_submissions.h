// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "omm_acceleration.h"
#include <vector>

namespace vel::omm {
struct SubmissionTracker;
// Default Linux implementation duplicates sync FDs with CLOEXEC and polls with
// timeout zero. The optional overrides permit deterministic transaction tests
// on platforms without VK_KHR_external_fence_fd. No borrowed application FD is
// retained. poll returns SUCCESS only for a proven signal, NOT_READY otherwise.
struct SubmissionSyncFdOps {
    int (*duplicate)(int) = nullptr;
    VkResult (*poll)(int) = nullptr;
    void (*close)(int) = nullptr;
};
struct SubmissionTrackerCreateInfo {
    VkDevice device{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    AccelerationBackend* acceleration{};
    const VkAllocationCallbacks* allocator{};
    bool timeline_semaphore = false;  // Actually enabled on the native device.
    SubmissionSyncFdOps sync_fd_ops{};
    uint64_t max_timeline_value_difference = UINT64_MAX;
};
VkResult submissions_create(const SubmissionTrackerCreateInfo&, SubmissionTracker**);
// Requires completed work (or device-lost teardown), no concurrent tracker calls,
// and a still-valid native VkDevice. Does not inject waits or submissions.
void submissions_destroy(SubmissionTracker*);
// Inputs already contain the layer's synchronization translations. Tokens must
// cover AS writes in direct and secondary command buffers. Retained transactionally
// before the native submit. With timeline_semaphore enabled, append a private
// per-queue timeline signal to the LAST info in the SAME native submit. Preserve
// all application fences, flags, chains, semaphore values and array elements.
// Without that native feature, a fence-less batch uses a private native fence.
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
// External-payload mutations preserve each older submission generation. A
// successful copy export can never be undone: post-export dup/poll failure keeps
// the original result/FD and retains the unknown generation until a reliable
// later same-queue completion or explicit queue/device idle proves completion.
// Reference-shared fallback fences likewise cannot serve as private AS lifetime
// proofs; the complete no-timeline/cross-platform lifecycle matrix is unqualified.
VkResult submissions_get_fence_fd(SubmissionTracker*, const VkFenceGetFdInfoKHR*, int*);
VkResult submissions_import_fence_fd(SubmissionTracker*, const VkImportFenceFdInfoKHR*);
#ifdef VK_USE_PLATFORM_WIN32_KHR
VkResult submissions_get_fence_win32_handle(SubmissionTracker*, const VkFenceGetWin32HandleInfoKHR*, HANDLE*);
VkResult submissions_import_fence_win32_handle(SubmissionTracker*, const VkImportFenceWin32HandleInfoKHR*);
#endif
VkResult submissions_queue_wait_idle(SubmissionTracker*, VkQueue);
VkResult submissions_device_wait_idle(SubmissionTracker*);
}  // namespace vel::omm
