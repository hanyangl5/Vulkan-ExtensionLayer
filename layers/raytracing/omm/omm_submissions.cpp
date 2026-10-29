// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_submissions.h"
#include <vulkan/utility/vk_safe_struct.hpp>
#include <vulkan/utility/vk_struct_helper.hpp>
#include <algorithm>
#include <limits>
#include <list>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>
#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace vel::omm {
namespace {
struct Pending {
    VkQueue queue{};
    VkFence fence{};
    bool owned = false, complete = false;
    bool attached = true, trusted = true;
    unsigned paused = 0;
    VkSemaphore timeline{};
    uint64_t timeline_value = 0;
    int sync_fd = -1;
    uint64_t submitted_serial = 0;  // Zero while native submit prefix is in flight.
    std::vector<AccelerationRecordingRef> recordings;
};
struct QueueTimeline {
    VkQueue queue{};
    VkSemaphore semaphore{};
    uint64_t reserved = 0;
};
struct ExternalFence {
    VkFence fence{};
    bool permanent_shared = false, temporary = false, temporary_shared = false;
    bool shared() const noexcept { return temporary ? temporary_shared : permanent_shared; }
    void reset() noexcept { temporary = false; }
    void share() noexcept { (temporary ? temporary_shared : permanent_shared) = true; }
    void import(bool is_temporary) noexcept {
        temporary = is_temporary;
        (temporary ? temporary_shared : permanent_shared) = true;
    }
};
int duplicate_sync_fd(int fd) {
#if defined(__linux__)
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
    return -1;
#endif
}
VkResult poll_sync_fd(int fd) {
#if defined(__linux__)
    pollfd item{fd, POLLIN, 0};
    const auto result = poll(&item, 1, 0);
    // Errors and invalid observers are never completion evidence. The original
    // user export has already succeeded; a later queue/device idle can recover.
    return result > 0 && (item.revents & POLLIN) && !(item.revents & (POLLNVAL | POLLERR)) ? VK_SUCCESS : VK_NOT_READY;
#else
    return VK_NOT_READY;
#endif
}
void close_sync_fd(int fd) {
#if defined(__linux__)
    close(fd);
#else
    (void)fd;
#endif
}
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
    std::list<QueueTimeline> timelines;
    std::list<ExternalFence> external_fences;
    bool use_timeline = false;
    SubmissionSyncFdOps fd_ops{};
    uint64_t max_timeline_value_difference = UINT64_MAX;
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
    PFN_vkCreateSemaphore create_semaphore{};
    PFN_vkDestroySemaphore destroy_semaphore{};
    PFN_vkGetSemaphoreCounterValue counter{};
    PFN_vkGetFenceFdKHR get_fd{};
    PFN_vkImportFenceFdKHR import_fd{};
#ifdef VK_USE_PLATFORM_WIN32_KHR
    PFN_vkGetFenceWin32HandleKHR get_win32{};
    PFN_vkImportFenceWin32HandleKHR import_win32{};
#endif
    const VkAllocationCallbacks* callbacks() const { return custom ? &allocator : nullptr; }
    ExternalFence* external(VkFence fence, bool create = false) {
        for (auto& item : external_fences)
            if (item.fence == fence) return &item;
        if (!create) return nullptr;
        external_fences.push_back({fence});
        return &external_fences.back();
    }
    VkResult timeline(VkQueue queue, VkSemaphore* semaphore, uint64_t* value) {
        auto found = std::find_if(timelines.begin(), timelines.end(), [&](const auto& item) { return item.queue == queue; });
        if (found == timelines.end()) {
            timelines.push_back({queue});
            found = std::prev(timelines.end());
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
            const auto result = create_semaphore(device, &info, callbacks(), &found->semaphore);
            if (result != VK_SUCCESS) {
                timelines.erase(found);
                return result;
            }
        }
        if (found->reserved == std::numeric_limits<uint64_t>::max()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        *semaphore = found->semaphore;
        *value = ++found->reserved;  // Failed submits leave harmless unused gaps.
        return VK_SUCCESS;
    }
    void cancel_reservation(VkSemaphore semaphore, uint64_t value) noexcept {
        for (auto& timeline : timelines)
            if (timeline.semaphore == semaphore && timeline.reserved == value) {
                --timeline.reserved;
                break;
            }
    }

    void collect() noexcept {
        for (auto& entry : pending) {
            if (!entry.submitted_serial || !entry.complete) continue;
            if (entry.sync_fd >= 0) {
                fd_ops.close(entry.sync_fd);
                entry.sync_fd = -1;
            }
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
            VkResult result = VK_NOT_READY;
            if (entry.timeline) {
                uint64_t value = 0;
                result = counter(device, entry.timeline, &value);
                if (result == VK_SUCCESS) result = value >= entry.timeline_value ? VK_SUCCESS : VK_NOT_READY;
            } else if (entry.sync_fd >= 0)
                result = fd_ops.poll(entry.sync_fd);
            else if (entry.attached && entry.trusted && !entry.paused)
                result = entry.fence == known_fence ? known_result : status(device, entry.fence);
            if (result == VK_SUCCESS)
                entry.complete = true;
            else if (result != VK_NOT_READY)
                return result;
        }
        // A proven later submission on the same queue also completes every
        // earlier writer there, including an unobservable exported generation.
        // Different queues never share a private timeline or completion bound.
        for (auto& entry : pending)
            if (entry.submitted_serial && !entry.complete)
                for (const auto& later : pending)
                    if (later.queue == entry.queue && later.complete && later.submitted_serial >= entry.submitted_serial) {
                        entry.complete = true;
                        break;
                    }
        collect();
        return VK_SUCCESS;
    }
    bool incomplete(uint32_t count, const VkFence* fences) const noexcept {
        for (const auto& entry : pending)
            if (!entry.complete && entry.attached && (entry.timeline || entry.trusted) && contains(count, fences, entry.fence))
                return true;
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
        tracker->use_timeline = info.timeline_semaphore;
        tracker->max_timeline_value_difference = info.max_timeline_value_difference;
        tracker->fd_ops = {info.sync_fd_ops.duplicate ? info.sync_fd_ops.duplicate : duplicate_sync_fd,
                           info.sync_fd_ops.poll ? info.sync_fd_ops.poll : poll_sync_fd,
                           info.sync_fd_ops.close ? info.sync_fd_ops.close : close_sync_fd};
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
        tracker->get_fd = reinterpret_cast<PFN_vkGetFenceFdKHR>(info.get_device_proc_addr(info.device, "vkGetFenceFdKHR"));
        tracker->import_fd = reinterpret_cast<PFN_vkImportFenceFdKHR>(info.get_device_proc_addr(info.device, "vkImportFenceFdKHR"));
#ifdef VK_USE_PLATFORM_WIN32_KHR
        tracker->get_win32 =
            reinterpret_cast<PFN_vkGetFenceWin32HandleKHR>(info.get_device_proc_addr(info.device, "vkGetFenceWin32HandleKHR"));
        tracker->import_win32 = reinterpret_cast<PFN_vkImportFenceWin32HandleKHR>(
            info.get_device_proc_addr(info.device, "vkImportFenceWin32HandleKHR"));
#endif
        if (tracker->use_timeline) {
            LOAD(create_semaphore, CreateSemaphore);
            LOAD(destroy_semaphore, DestroySemaphore);
            tracker->counter = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
                info.get_device_proc_addr(info.device, "vkGetSemaphoreCounterValue"));
            if (!tracker->counter)
                tracker->counter = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
                    info.get_device_proc_addr(info.device, "vkGetSemaphoreCounterValueKHR"));
            if (!tracker->counter) {
                delete tracker;
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

#undef LOAD
        *output = tracker;
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void submissions_destroy(SubmissionTracker* tracker) {
    if (!tracker) return;
    for (const auto& entry : tracker->pending) {
        if (entry.owned) tracker->destroy_fence(tracker->device, entry.fence, tracker->callbacks());
        if (entry.sync_fd >= 0) tracker->fd_ops.close(entry.sync_fd);
    }
    for (const auto& timeline : tracker->timelines)
        tracker->destroy_semaphore(tracker->device, timeline.semaphore, tracker->callbacks());
    delete tracker;
}
namespace {
template <typename Info>
struct TimelineSubmission;
template <>
struct TimelineSubmission<VkSubmitInfo> {
    std::vector<vku::safe_VkSubmitInfo> owned;
    std::vector<VkSubmitInfo> native;
    void prepare(uint32_t count, const VkSubmitInfo* infos, VkSemaphore semaphore, uint64_t value) {
        owned.resize(count);
        native.resize(count);
        for (uint32_t i = 0; i < count; ++i) owned[i].initialize(&infos[i]);
        auto& last = owned.back();
        const auto old_count = last.signalSemaphoreCount;
        if (old_count == UINT32_MAX) throw std::length_error("Semaphore count overflow");
        auto signals = std::make_unique<VkSemaphore[]>(size_t(old_count) + 1);
        std::copy_n(last.pSignalSemaphores, old_count, signals.get());
        signals[old_count] = semaphore;
        auto* timeline =
            const_cast<VkTimelineSemaphoreSubmitInfo*>(vku::FindStructInPNextChain<VkTimelineSemaphoreSubmitInfo>(last.pNext));
        if (!timeline) {
            VkTimelineSemaphoreSubmitInfo added{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            vku::AddToPnext(last, added);
            timeline =
                const_cast<VkTimelineSemaphoreSubmitInfo*>(vku::FindStructInPNextChain<VkTimelineSemaphoreSubmitInfo>(last.pNext));
        }
        auto values = std::make_unique<uint64_t[]>(size_t(old_count) + 1);
        for (uint32_t i = 0; i < old_count; ++i)
            values[i] = i < timeline->signalSemaphoreValueCount ? timeline->pSignalSemaphoreValues[i] : 0;
        values[old_count] = value;
        if (auto* group = const_cast<VkDeviceGroupSubmitInfo*>(vku::FindStructInPNextChain<VkDeviceGroupSubmitInfo>(last.pNext))) {
            if (group->signalSemaphoreCount) {
                auto indices = std::make_unique<uint32_t[]>(size_t(old_count) + 1);
                std::copy_n(group->pSignalSemaphoreDeviceIndices, old_count, indices.get());
                indices[old_count] = 0;
                delete[] group->pSignalSemaphoreDeviceIndices;
                group->pSignalSemaphoreDeviceIndices = indices.release();
                group->signalSemaphoreCount = old_count + 1;
            }
        }
        delete[] last.pSignalSemaphores;
        last.pSignalSemaphores = signals.release();
        last.signalSemaphoreCount = old_count + 1;
        delete[] timeline->pSignalSemaphoreValues;
        timeline->pSignalSemaphoreValues = values.release();
        timeline->signalSemaphoreValueCount = old_count + 1;
        for (uint32_t i = 0; i < count; ++i) native[i] = *owned[i].ptr();
    }
};
template <>
struct TimelineSubmission<VkSubmitInfo2> {
    std::vector<vku::safe_VkSubmitInfo2> owned;
    std::vector<VkSubmitInfo2> native;
    void prepare(uint32_t count, const VkSubmitInfo2* infos, VkSemaphore semaphore, uint64_t value) {
        owned.resize(count);
        native.resize(count);
        for (uint32_t i = 0; i < count; ++i) owned[i].initialize(&infos[i]);
        auto& last = owned.back();
        const auto old_count = last.signalSemaphoreInfoCount;
        if (old_count == UINT32_MAX) throw std::length_error("Semaphore count overflow");
        auto signals = std::make_unique<vku::safe_VkSemaphoreSubmitInfo[]>(size_t(old_count) + 1);
        for (uint32_t i = 0; i < old_count; ++i) signals[i].initialize(&last.pSignalSemaphoreInfos[i]);
        signals[old_count].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[old_count].semaphore = semaphore;
        signals[old_count].value = value;
        signals[old_count].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals[old_count].deviceIndex = 0;
        delete[] last.pSignalSemaphoreInfos;
        last.pSignalSemaphoreInfos = signals.release();
        last.signalSemaphoreInfoCount = old_count + 1;
        for (uint32_t i = 0; i < count; ++i) native[i] = *owned[i].ptr();
    }
};
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
        TimelineSubmission<Info> translated;
        const Info* native_infos = infos;
        try {
            pending->recordings = recordings;
            pending->queue = queue;
            pending->fence = fence;
            if (auto* external = tracker->external(fence)) pending->trusted = !external->shared();
            if (tracker->use_timeline) {
                pending->attached = fence != VK_NULL_HANDLE;
                if (!count) {
                    tracker->pending.erase(pending);
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                result = tracker->timeline(queue, &pending->timeline, &pending->timeline_value);
                if (result != VK_SUCCESS) {
                    tracker->pending.erase(pending);
                    return result;
                }
                translated.prepare(count, infos, pending->timeline, pending->timeline_value);
                native_infos = translated.native.data();
                uint64_t observed = 0;
                lock.unlock();
                result = tracker->counter(tracker->device, pending->timeline, &observed);
                lock.lock();
                if (result != VK_SUCCESS || observed >= pending->timeline_value ||
                    pending->timeline_value - observed > tracker->max_timeline_value_difference) {
                    tracker->cancel_reservation(pending->timeline, pending->timeline_value);
                    tracker->pending.erase(pending);
                    return result != VK_SUCCESS ? result : VK_ERROR_OUT_OF_DEVICE_MEMORY;
                }
            } else if (!fence) {
                VkFenceCreateInfo create{};
                create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                result = tracker->create_fence(tracker->device, &create, tracker->callbacks(), &pending->fence);
                if (result != VK_SUCCESS) {
                    tracker->pending.erase(pending);
                    return result;
                }
                pending->owned = true;
            }
        } catch (...) {
            if (pending->timeline) tracker->cancel_reservation(pending->timeline, pending->timeline_value);
            if (pending->owned) tracker->destroy_fence(tracker->device, pending->fence, tracker->callbacks());
            tracker->pending.erase(pending);
            throw;
        }
        const auto submitted_fence = pending->fence;
        lock.unlock();
        result = submit(queue, count, native_infos, submitted_fence);
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
            if (entry.submitted_serial && entry.submitted_serial <= cutoff && !entry.complete && entry.attached && entry.trusted &&
                !entry.timeline && !entry.paused && contains(count, fences, entry.fence))
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
    const auto reset = tracker->reset(tracker->device, count, fences);
    if (reset == VK_SUCCESS) {
        // Freeze this payload generation before later native handle reuse. A
        // successful operation on a shared/replaced payload is not a private
        // completion observation of our original queue work.
        for (auto& entry : tracker->pending)
            if (entry.attached && contains(count, fences, entry.fence)) entry.attached = false;
        for (uint32_t i = 0; i < count; ++i)
            if (auto* external = tracker->external(fences[i])) external->reset();
        tracker->collect();
    }
    return reset;
}
VkResult submissions_destroy_fence(SubmissionTracker* tracker, VkFence fence, const VkAllocationCallbacks* allocator) {
    if (!tracker) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lock(tracker->mutex);
    const auto result = tracker->poll();
    if (result != VK_SUCCESS) return result;
    if (tracker->incomplete(1, &fence)) return VK_ERROR_VALIDATION_FAILED_EXT;
    tracker->destroy_fence(tracker->device, fence, allocator);
    for (auto& entry : tracker->pending)
        if (entry.attached && entry.fence == fence) entry.attached = false;
    tracker->external_fences.remove_if([&](const auto& entry) { return entry.fence == fence; });
    tracker->collect();
    return VK_SUCCESS;
}
namespace {
// Native external operations may run outside the mutex. Pausing fallback
// observers before that prefix prevents another queue's poll from reading a
// newly replaced/reset payload before this transaction records its generation.
void pause_fence(SubmissionTracker& tracker, VkFence fence) noexcept {
    for (auto& entry : tracker.pending)
        if (entry.attached && entry.fence == fence) ++entry.paused;
}
void resume_fence(SubmissionTracker& tracker, VkFence fence) noexcept {
    for (auto& entry : tracker.pending)
        if (entry.attached && entry.fence == fence && entry.paused) --entry.paused;
}
template <typename Call>
VkResult export_reference(SubmissionTracker* tracker, VkFence fence, Call call) {
    if (!tracker) return VK_ERROR_INITIALIZATION_FAILED;
    try {
        std::unique_lock<std::mutex> lock(tracker->mutex);
        auto result = tracker->poll();
        if (result != VK_SUCCESS) return result;
        auto* external = tracker->external(fence, true);  // All allocation precedes export.
        pause_fence(*tracker, fence);
        lock.unlock();
        result = call();
        lock.lock();
        resume_fence(*tracker, fence);
        if (result == VK_SUCCESS) {
            external->share();
            for (auto& entry : tracker->pending)
                if (entry.attached && entry.fence == fence) entry.trusted = false;
        }
        return result;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
template <typename Call>
VkResult import_payload(SubmissionTracker* tracker, VkFence fence, bool temporary, Call call) {
    if (!tracker) return VK_ERROR_INITIALIZATION_FAILED;
    try {
        std::unique_lock<std::mutex> lock(tracker->mutex);
        auto result = tracker->poll();
        if (result != VK_SUCCESS) return result;
        if (tracker->incomplete(1, &fence)) return VK_ERROR_VALIDATION_FAILED_EXT;
        auto* external = tracker->external(fence, true);
        pause_fence(*tracker, fence);
        lock.unlock();
        result = call();
        lock.lock();
        resume_fence(*tracker, fence);
        if (result == VK_SUCCESS) {
            for (auto& entry : tracker->pending)
                if (entry.attached && entry.fence == fence) entry.attached = false;
            external->import(temporary);
            tracker->collect();
        }
        return result;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
}  // namespace
VkResult submissions_get_fence_fd(SubmissionTracker* tracker, const VkFenceGetFdInfoKHR* info, int* output) {
    if (!tracker || !info || !output) return VK_ERROR_INITIALIZATION_FAILED;
    if (!tracker->get_fd) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->handleType != VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT)
        return export_reference(tracker, info->fence, [&] { return tracker->get_fd(tracker->device, info, output); });
    std::unique_lock<std::mutex> lock(tracker->mutex);
    auto result = tracker->poll();
    if (result != VK_SUCCESS) return result;
    pause_fence(*tracker, info->fence);
    lock.unlock();
    result = tracker->get_fd(tracker->device, info, output);
    lock.lock();
    resume_fence(*tracker, info->fence);
    if (result != VK_SUCCESS) return result;
    for (auto& entry : tracker->pending) {
        if (!entry.attached || entry.fence != info->fence) continue;
        entry.attached = false;
        if (entry.complete || entry.timeline || !entry.trusted) continue;
        if (*output == -1)
            entry.complete = true;
        else
            entry.sync_fd = tracker->fd_ops.duplicate(*output);
        // Failed dup deliberately leaves an unobservable generation retained.
        // Never change successful export's result/FD or inspect the reset fence.
    }
    if (auto* external = tracker->external(info->fence)) external->reset();
    tracker->collect();
    return result;
}
VkResult submissions_import_fence_fd(SubmissionTracker* tracker, const VkImportFenceFdInfoKHR* info) {
    if (!tracker || !info) return VK_ERROR_INITIALIZATION_FAILED;
    if (!tracker->import_fd) return VK_ERROR_FEATURE_NOT_PRESENT;
    return import_payload(tracker, info->fence, (info->flags & VK_FENCE_IMPORT_TEMPORARY_BIT) != 0,
                          [&] { return tracker->import_fd(tracker->device, info); });
}
#ifdef VK_USE_PLATFORM_WIN32_KHR
VkResult submissions_get_fence_win32_handle(SubmissionTracker* tracker, const VkFenceGetWin32HandleInfoKHR* info, HANDLE* output) {
    if (!tracker || !info || !output) return VK_ERROR_INITIALIZATION_FAILED;
    if (!tracker->get_win32) return VK_ERROR_FEATURE_NOT_PRESENT;
    return export_reference(tracker, info->fence, [&] { return tracker->get_win32(tracker->device, info, output); });
}
VkResult submissions_import_fence_win32_handle(SubmissionTracker* tracker, const VkImportFenceWin32HandleInfoKHR* info) {
    if (!tracker || !info) return VK_ERROR_INITIALIZATION_FAILED;
    if (!tracker->import_win32) return VK_ERROR_FEATURE_NOT_PRESENT;
    return import_payload(tracker, info->fence, (info->flags & VK_FENCE_IMPORT_TEMPORARY_BIT) != 0,
                          [&] { return tracker->import_win32(tracker->device, info); });
}
#endif
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
