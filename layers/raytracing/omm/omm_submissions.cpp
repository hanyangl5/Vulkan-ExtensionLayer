// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_submissions.h"
#include <list>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace vel::omm {
namespace {
struct Pending {
    VkQueue queue{};
    VkFence fence{};
    bool owned = false, complete = false;
    uint64_t submitted_serial = 0;  // Zero while native submit prefix is in flight.
    std::vector<AccelerationRecordingRef> recordings;
};
bool contains(uint32_t count, const VkFence* fences, VkFence fence) noexcept {
    for (uint32_t i = 0; i < count; ++i)
        if (fences[i] == fence) return true;
    return false;
}
}  // namespace
struct SubmissionTracker {
    VkDevice device{};
    AccelerationBackend* acceleration{};
    VkAllocationCallbacks allocator{};
    bool custom = false;
    std::mutex mutex;
    std::list<Pending> pending;
    uint64_t submitted_serial = 0;
    PFN_vkQueueSubmit submit{};
    PFN_vkQueueSubmit2 submit2{};
    PFN_vkCreateFence create_fence{};
    PFN_vkDestroyFence destroy_fence{};
    PFN_vkGetFenceStatus status{};
    PFN_vkWaitForFences wait{};
    PFN_vkResetFences reset{};
    PFN_vkQueueWaitIdle queue_idle{};
    PFN_vkDeviceWaitIdle device_idle{};
    const VkAllocationCallbacks* callbacks() const { return custom ? &allocator : nullptr; }

    void collect() noexcept {
        for (auto& entry : pending) {
            if (!entry.submitted_serial || !entry.complete) continue;
            for (auto& recording : entry.recordings) {
                if (!recording) continue;
                bool safe = true;
                for (const auto& writer : pending) {
                    if (writer.submitted_serial && writer.complete) continue;
                    for (const auto& candidate : writer.recordings)
                        if (acceleration_recordings_overlap(recording, candidate)) {
                            safe = false;
                            break;
                        }
                    if (!safe) break;
                }
                if (safe) {
                    // Acquire active-body AS ownership before dropping this
                    // submission's token. Other ASes can continue building.
                    acceleration_collect_completed(acceleration, recording);
                    recording.reset();
                }
            }
        }
        for (auto entry = pending.begin(); entry != pending.end();) {
            bool retained = false;
            for (const auto& recording : entry->recordings)
                if (recording) {
                    retained = true;
                    break;
                }
            if (entry->submitted_serial && entry->complete && !retained) {
                if (entry->owned) destroy_fence(device, entry->fence, callbacks());
                entry = pending.erase(entry);
            } else
                ++entry;
        }
    }
    VkResult poll(VkFence known_fence = VK_NULL_HANDLE, VkResult known_result = VK_NOT_READY) noexcept {
        for (auto& entry : pending) {
            if (entry.complete || !entry.submitted_serial) continue;
            const auto result = entry.fence == known_fence ? known_result : status(device, entry.fence);
            if (result == VK_SUCCESS)
                entry.complete = true;
            else if (result != VK_NOT_READY)
                return result;
        }
        collect();
        return VK_SUCCESS;
    }
    bool incomplete(uint32_t count, const VkFence* fences) const noexcept {
        for (const auto& entry : pending)
            if (!entry.complete && contains(count, fences, entry.fence)) return true;
        return false;
    }
};
VkResult submissions_create(const SubmissionTrackerCreateInfo& info, SubmissionTracker** output) {
    if (!output || !info.device || !info.get_device_proc_addr || !info.acceleration) return VK_ERROR_INITIALIZATION_FAILED;
    *output = nullptr;
    try {
        auto* tracker = new SubmissionTracker;
        tracker->device = info.device;
        tracker->acceleration = info.acceleration;
        if (info.allocator) {
            tracker->allocator = *info.allocator;
            tracker->custom = true;
        }
#define LOAD(field, name)                                                                                \
    tracker->field = reinterpret_cast<PFN_vk##name>(info.get_device_proc_addr(info.device, "vk" #name)); \
    if (!tracker->field) {                                                                               \
        delete tracker;                                                                                  \
        return VK_ERROR_INITIALIZATION_FAILED;                                                           \
    }
        LOAD(submit, QueueSubmit);

        tracker->submit2 = reinterpret_cast<PFN_vkQueueSubmit2>(info.get_device_proc_addr(info.device, "vkQueueSubmit2"));
        if (!tracker->submit2)
            tracker->submit2 = reinterpret_cast<PFN_vkQueueSubmit2>(info.get_device_proc_addr(info.device, "vkQueueSubmit2KHR"));
        LOAD(create_fence, CreateFence);

        LOAD(destroy_fence, DestroyFence);
        LOAD(status, GetFenceStatus);
        LOAD(wait, WaitForFences);
        LOAD(reset, ResetFences);

        LOAD(queue_idle, QueueWaitIdle);
        LOAD(device_idle, DeviceWaitIdle);

#undef LOAD
        *output = tracker;
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void submissions_destroy(SubmissionTracker* tracker) {
    if (!tracker) return;
    for (const auto& entry : tracker->pending)
        if (entry.owned) tracker->destroy_fence(tracker->device, entry.fence, tracker->callbacks());
    delete tracker;
}
namespace {
template <typename Info, typename Submit>
VkResult queue_submit(SubmissionTracker* tracker, VkQueue queue, uint32_t count, const Info* infos, VkFence fence,
                      const std::vector<AccelerationRecordingRef>& recordings, Submit submit) {
    if (!tracker || !queue || (count && !infos) || !submit) return VK_ERROR_INITIALIZATION_FAILED;
    std::unique_lock<std::mutex> lock(tracker->mutex);
    auto result = tracker->poll();
    if (result != VK_SUCCESS) return result;
    if (recordings.empty()) {
        lock.unlock();
        return submit(queue, count, infos, fence);
    }
    try {
        // Both the list node and every token copy exist before any native submit.
        tracker->pending.emplace_back();
        auto pending = std::prev(tracker->pending.end());
        try {
            pending->recordings = recordings;
        } catch (...) {
            tracker->pending.erase(pending);
            throw;
        }
        pending->queue = queue;
        pending->fence = fence;
        if (!fence) {
            VkFenceCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            result = tracker->create_fence(tracker->device, &create, tracker->callbacks(), &pending->fence);
            if (result != VK_SUCCESS) {
                tracker->pending.erase(pending);
                return result;
            }
            pending->owned = true;
        }
        const auto submitted_fence = pending->fence;
        lock.unlock();
        result = submit(queue, count, infos, submitted_fence);
        lock.lock();
        if (result != VK_SUCCESS) {
            if (pending->owned) tracker->destroy_fence(tracker->device, pending->fence, tracker->callbacks());
            tracker->pending.erase(pending);
        } else
            pending->submitted_serial = ++tracker->submitted_serial;
        return result;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (const std::length_error&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
}  // namespace
VkResult submissions_queue_submit(SubmissionTracker* tracker, VkQueue queue, uint32_t count, const VkSubmitInfo* infos,
                                  VkFence fence, const std::vector<AccelerationRecordingRef>& recordings) {
    return queue_submit(tracker, queue, count, infos, fence, recordings, tracker ? tracker->submit : nullptr);
}
VkResult submissions_queue_submit2(SubmissionTracker* tracker, VkQueue queue, uint32_t count, const VkSubmitInfo2* infos,
                                   VkFence fence, const std::vector<AccelerationRecordingRef>& recordings) {
    return queue_submit(tracker, queue, count, infos, fence, recordings, tracker ? tracker->submit2 : nullptr);
}
VkResult submissions_poll(SubmissionTracker* tracker) {
    if (!tracker) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lock(tracker->mutex);
    return tracker->poll();
}
VkResult submissions_wait_for_fences(SubmissionTracker* tracker, uint32_t count, const VkFence* fences, VkBool32 all,
                                     uint64_t timeout) {
    if (!tracker || (count && !fences)) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t cutoff = 0;
    {
        std::lock_guard<std::mutex> lock(tracker->mutex);
        cutoff = tracker->submitted_serial;
    }
    const auto result = tracker->wait(tracker->device, count, fences, all, timeout);
    if (result != VK_SUCCESS && result != VK_TIMEOUT) return result;
    std::lock_guard<std::mutex> lock(tracker->mutex);
    if (result == VK_SUCCESS && all)
        for (auto& entry : tracker->pending)
            if (entry.submitted_serial && entry.submitted_serial <= cutoff && !entry.complete &&
                contains(count, fences, entry.fence))
                entry.complete = true;
    const auto polled = tracker->poll();
    return polled == VK_SUCCESS ? result : polled;
}
VkResult submissions_get_fence_status(SubmissionTracker* tracker, VkFence fence) {
    if (!tracker) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lock(tracker->mutex);
    const auto result = tracker->status(tracker->device, fence);
    if (result != VK_SUCCESS && result != VK_NOT_READY) return result;
    const auto polled = tracker->poll(fence, result);
    return polled == VK_SUCCESS ? result : polled;
}
VkResult submissions_reset_fences(SubmissionTracker* tracker, uint32_t count, const VkFence* fences) {
    if (!tracker || (count && !fences)) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lock(tracker->mutex);
    const auto result = tracker->poll();
    if (result != VK_SUCCESS) return result;
    if (tracker->incomplete(count, fences)) return VK_ERROR_VALIDATION_FAILED_EXT;
    return tracker->reset(tracker->device, count, fences);
}
VkResult submissions_destroy_fence(SubmissionTracker* tracker, VkFence fence, const VkAllocationCallbacks* allocator) {
    if (!tracker) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lock(tracker->mutex);
    const auto result = tracker->poll();
    if (result != VK_SUCCESS) return result;
    if (tracker->incomplete(1, &fence)) return VK_ERROR_VALIDATION_FAILED_EXT;
    tracker->destroy_fence(tracker->device, fence, allocator);
    return VK_SUCCESS;
}
VkResult submissions_queue_wait_idle(SubmissionTracker* tracker, VkQueue queue) {
    if (!tracker) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t cutoff = 0;
    {
        std::lock_guard<std::mutex> lock(tracker->mutex);
        cutoff = tracker->submitted_serial;
    }
    const auto result = tracker->queue_idle(queue);
    if (result != VK_SUCCESS) return result;
    std::lock_guard<std::mutex> lock(tracker->mutex);
    for (auto& entry : tracker->pending)
        if (entry.queue == queue && entry.submitted_serial && entry.submitted_serial <= cutoff) entry.complete = true;
    return tracker->poll();
}
VkResult submissions_device_wait_idle(SubmissionTracker* tracker) {
    if (!tracker) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t cutoff = 0;
    {
        std::lock_guard<std::mutex> lock(tracker->mutex);
        cutoff = tracker->submitted_serial;
    }
    const auto result = tracker->device_idle(tracker->device);
    if (result != VK_SUCCESS) return result;
    std::lock_guard<std::mutex> lock(tracker->mutex);
    for (auto& entry : tracker->pending)
        if (entry.submitted_serial && entry.submitted_serial <= cutoff) entry.complete = true;
    return tracker->poll();
}
}  // namespace vel::omm
