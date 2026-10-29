// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "../omm_submissions.h"
#include <vulkan/utility/vk_struct_helper.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

static thread_local int allocation_budget = -1;
void* operator new(std::size_t size) {
    if (allocation_budget >= 0 && allocation_budget-- == 0) throw std::bad_alloc();
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

using namespace vel::omm;
namespace {
std::atomic<unsigned> checks{0}, failures{0};
unsigned destroyed_tokens = 0;
void expect(bool value, const char* reason) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL submissions: %s\n", reason);
    }
}
template <typename T>
T handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}
template <typename T>
uintptr_t value(T h) {
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<uintptr_t>(h);
    else
        return static_cast<uintptr_t>(h);
}
struct Fence {
    bool live = false, signaled = false;
    unsigned generation = 0, polls = 0, destroys = 0;
    VkResult error = VK_SUCCESS;
};
struct Submit {
    VkQueue queue{};
    VkFence fence{};
    const void* infos{};
    uint32_t count = 0;
    unsigned generation = 0;
    bool version2 = false;
    VkSemaphore observer{};
    uint64_t observer_value = 0;
    VkSemaphore app_signals[8]{}, app_waits[8]{};
    uint64_t signal_values[8]{}, wait_values[8]{};
    VkPipelineStageFlags2 signal_stages[8]{};
    uint32_t signal_devices[8]{}, signal_count = 0, wait_count = 0, command_count = 0;
    VkSubmitFlags flags = 0;
    bool protected_submit = false;
};
struct Timeline {
    bool live = false;
    uint64_t counter = 0;
    unsigned polls = 0;
};
struct Fd {
    bool live = false;
    unsigned payload = 0;
};
struct Mock {
    Fence fences[128]{};
    Submit submits[128]{};
    unsigned submit_count = 0, creates = 0, destroys = 0, resets = 0, waits = 0, queue_waits = 0, device_waits = 0;
    unsigned next_private = 64, collect_count = 0;
    uintptr_t collected[128]{};
    Timeline timelines[128]{};
    Fd fds[512]{};
    bool payloads[128]{};
    unsigned next_timeline = 64, semaphore_creates = 0, semaphore_destroys = 0;
    unsigned next_fd = 64, next_payload = 1, exports = 0, imports = 0, duplicates = 0, closes = 0;
    bool fail_dup = false, export_minus_one = false, export_oom_after_success = false, signal_during_export = false;
    bool allow_collect = false;
    VkResult next_submit = VK_SUCCESS, next_create = VK_SUCCESS, next_wait = VK_SUCCESS;
    VkResult next_semaphore = VK_SUCCESS, next_export = VK_SUCCESS, next_import = VK_SUCCESS, next_counter = VK_SUCCESS;
} mock;
std::atomic<int> thread_mode{0};
std::mutex progress_mutex;
std::condition_variable progress;
bool entered = false, released = false;
VkDevice device = handle<VkDevice>(1);
VkQueue queue1 = handle<VkQueue>(11), queue2 = handle<VkQueue>(12);
Fence& state(VkFence fence) { return mock.fences[value(fence)]; }
VkFence user_fence(unsigned id) {
    auto fence = handle<VkFence>(id);
    state(fence).live = true;
    return fence;
}
struct TokenData {
    unsigned id, as;
};
AccelerationRecordingRef token(unsigned id, unsigned as = 1) {
    return AccelerationRecordingRef(reinterpret_cast<AccelerationRecording*>(new TokenData{id, as}), [](AccelerationRecording* p) {
        delete reinterpret_cast<TokenData*>(p);
        ++destroyed_tokens;
    });
}
VKAPI_ATTR VkResult VKAPI_CALL create_fence(VkDevice, const VkFenceCreateInfo* info, const VkAllocationCallbacks*, VkFence* out) {
    ++mock.creates;
    expect(info->flags == 0, "private fence starts unsignaled");
    if (mock.next_create != VK_SUCCESS) return mock.next_create;
    *out = user_fence(mock.next_private++);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_fence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
    if (!fence) return;
    expect(state(fence).live, "fence is live at native destruction");
    state(fence).live = false;
    ++state(fence).destroys;
    ++mock.destroys;
}
VKAPI_ATTR VkResult VKAPI_CALL status(VkDevice, VkFence fence) {
    expect(state(fence).live, "native status never polls destroyed or retired fence");
    ++state(fence).polls;
    if (state(fence).error != VK_SUCCESS) return state(fence).error;
    return state(fence).signaled ? VK_SUCCESS : VK_NOT_READY;
}
VKAPI_ATTR VkResult VKAPI_CALL create_semaphore(VkDevice, const VkSemaphoreCreateInfo* info, const VkAllocationCallbacks*,
                                                VkSemaphore* out) {
    ++mock.semaphore_creates;
    const auto* type = vku::FindStructInPNextChain<VkSemaphoreTypeCreateInfo>(info->pNext);
    expect(type && type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE && type->initialValue == 0,
           "private per-queue timeline begins at zero");
    if (mock.next_semaphore != VK_SUCCESS) return mock.next_semaphore;
    *out = handle<VkSemaphore>(mock.next_timeline++);
    mock.timelines[value(*out)].live = true;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_semaphore(VkDevice, VkSemaphore semaphore, const VkAllocationCallbacks*) {
    expect(mock.timelines[value(semaphore)].live, "private timeline destroyed exactly once while device is live");
    mock.timelines[value(semaphore)].live = false;
    ++mock.semaphore_destroys;
}
VKAPI_ATTR VkResult VKAPI_CALL counter(VkDevice, VkSemaphore semaphore, uint64_t* out) {
    auto& timeline = mock.timelines[value(semaphore)];
    expect(timeline.live, "counter only polls a live private timeline");
    ++timeline.polls;
    if (mock.next_counter != VK_SUCCESS) return mock.next_counter;
    *out = timeline.counter;
    return VK_SUCCESS;
}
int duplicate_fd(int fd) {
    ++mock.duplicates;
    expect(fd >= 0 && mock.fds[fd].live, "duplicate receives application-owned live sync FD");
    if (mock.fail_dup) return -1;
    const auto result = int(mock.next_fd++);
    mock.fds[result] = mock.fds[fd];
    return result;
}
VkResult poll_fd(int fd) {
    expect(mock.fds[fd].live, "old payload observer never polls closed application FD");
    return mock.payloads[mock.fds[fd].payload] ? VK_SUCCESS : VK_NOT_READY;
}
void close_fd(int fd) {
    expect(mock.fds[fd].live, "owned duplicate closes once");
    mock.fds[fd].live = false;
    ++mock.closes;
}
VKAPI_ATTR VkResult VKAPI_CALL get_fd(VkDevice, const VkFenceGetFdInfoKHR* info, int* out) {
    ++mock.exports;
    if (mock.next_export != VK_SUCCESS) return mock.next_export;
    if (info->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT) {
        if (mock.signal_during_export) state(info->fence).signaled = true;
        const auto payload = mock.next_payload++;
        mock.payloads[payload] = state(info->fence).signaled;
        if (mock.export_minus_one && mock.payloads[payload])
            *out = -1;
        else {
            *out = int(mock.next_fd++);
            mock.fds[*out] = {true, payload};
        }
        state(info->fence).signaled = false;
        ++state(info->fence).generation;
    } else {
        *out = int(mock.next_fd++);
        mock.fds[*out] = {true, mock.next_payload++};
    }
    if (mock.export_oom_after_success) allocation_budget = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL import_fd(VkDevice, const VkImportFenceFdInfoKHR* info) {
    ++mock.imports;
    if (mock.next_import != VK_SUCCESS) return mock.next_import;
    state(info->fence).signaled = info->fd == -1 || mock.payloads[mock.fds[info->fd].payload];
    ++state(info->fence).generation;
    if (info->fd >= 0) mock.fds[info->fd].live = false;  // Native successful import consumes ownership.
    return VK_SUCCESS;
}
#ifdef VK_USE_PLATFORM_WIN32_KHR
VKAPI_ATTR VkResult VKAPI_CALL get_win32(VkDevice, const VkFenceGetWin32HandleInfoKHR*, HANDLE* out) {
    ++mock.exports;
    if (mock.next_export != VK_SUCCESS) return mock.next_export;
    *out = reinterpret_cast<HANDLE>(uintptr_t(0x1234));
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL import_win32(VkDevice, const VkImportFenceWin32HandleInfoKHR* info) {
    ++mock.imports;
    if (mock.next_import != VK_SUCCESS) return mock.next_import;
    state(info->fence).signaled = false;
    ++state(info->fence).generation;
    return VK_SUCCESS;
}
#endif
template <typename Info>
VkResult submit(VkQueue queue, uint32_t count, const Info* infos, VkFence fence, bool version2) {
    ++mock.submit_count;
    auto& entry = mock.submits[mock.submit_count - 1];
    entry = {queue, fence, infos, count, fence ? state(fence).generation : 0, version2};
    if (count) {
        const auto& last = infos[count - 1];
        if constexpr (std::is_same_v<Info, VkSubmitInfo>) {
            const auto* timeline = vku::FindStructInPNextChain<VkTimelineSemaphoreSubmitInfo>(last.pNext);
            const auto* group = vku::FindStructInPNextChain<VkDeviceGroupSubmitInfo>(last.pNext);
            const auto* protected_info = vku::FindStructInPNextChain<VkProtectedSubmitInfo>(last.pNext);
            entry.protected_submit = protected_info && protected_info->protectedSubmit;
            entry.signal_count = last.signalSemaphoreCount;
            entry.wait_count = last.waitSemaphoreCount;
            entry.command_count = last.commandBufferCount;
            for (uint32_t i = 0; i < entry.signal_count; ++i) {
                entry.app_signals[i] = last.pSignalSemaphores[i];
                entry.signal_values[i] =
                    timeline && i < timeline->signalSemaphoreValueCount ? timeline->pSignalSemaphoreValues[i] : 0;
                entry.signal_devices[i] = group && group->signalSemaphoreCount ? group->pSignalSemaphoreDeviceIndices[i] : 0;
            }
            for (uint32_t i = 0; i < entry.wait_count; ++i) {
                entry.app_waits[i] = last.pWaitSemaphores[i];
                entry.wait_values[i] = timeline && i < timeline->waitSemaphoreValueCount ? timeline->pWaitSemaphoreValues[i] : 0;
            }
        } else {
            entry.flags = last.flags;
            entry.signal_count = last.signalSemaphoreInfoCount;
            entry.wait_count = last.waitSemaphoreInfoCount;
            entry.command_count = last.commandBufferInfoCount;
            for (uint32_t i = 0; i < entry.signal_count; ++i) {
                entry.app_signals[i] = last.pSignalSemaphoreInfos[i].semaphore;
                entry.signal_values[i] = last.pSignalSemaphoreInfos[i].value;
                entry.signal_stages[i] = last.pSignalSemaphoreInfos[i].stageMask;
                entry.signal_devices[i] = last.pSignalSemaphoreInfos[i].deviceIndex;
            }
            for (uint32_t i = 0; i < entry.wait_count; ++i) {
                entry.app_waits[i] = last.pWaitSemaphoreInfos[i].semaphore;
                entry.wait_values[i] = last.pWaitSemaphoreInfos[i].value;
            }
        }
        if (entry.signal_count && value(entry.app_signals[entry.signal_count - 1]) >= 64) {
            entry.observer = entry.app_signals[entry.signal_count - 1];
            entry.observer_value = entry.signal_values[entry.signal_count - 1];
        }
    }
    const auto mode = thread_mode.load();
    if (mode == 2 && queue == queue1) {
        std::unique_lock<std::mutex> lock(progress_mutex);
        entered = true;
        progress.notify_all();
        if (!progress.wait_for(lock, std::chrono::seconds(2), [] { return released; })) return VK_TIMEOUT;
    } else if (mode && queue == queue2) {
        std::lock_guard<std::mutex> lock(progress_mutex);
        if (mode == 1) state(handle<VkFence>(1)).signaled = true;
        released = true;
        progress.notify_all();
    }
    return mock.next_submit;
}
VKAPI_ATTR VkResult VKAPI_CALL submit1(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
    return submit(queue, count, infos, fence, false);
}
VKAPI_ATTR VkResult VKAPI_CALL submit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence) {
    return submit(queue, count, infos, fence, true);
}
VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t) {
    ++mock.waits;
    if (thread_mode.load() == 1) {
        std::unique_lock<std::mutex> lock(progress_mutex);
        entered = true;
        progress.notify_all();
        if (!progress.wait_for(lock, std::chrono::seconds(2), [] { return released; })) return VK_TIMEOUT;
    }
    if (mock.next_wait != VK_SUCCESS) return mock.next_wait;
    bool any = false, every = true;
    for (uint32_t i = 0; i < count; ++i) {
        any |= state(fences[i]).signaled;
        every &= state(fences[i]).signaled;
    }
    return (all ? every : any) ? VK_SUCCESS : VK_TIMEOUT;
}
VKAPI_ATTR VkResult VKAPI_CALL reset(VkDevice, uint32_t count, const VkFence* fences) {
    ++mock.resets;
    for (uint32_t i = 0; i < count; ++i) {
        state(fences[i]).signaled = false;
        ++state(fences[i]).generation;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL queue_idle(VkQueue queue) {
    ++mock.queue_waits;
    for (unsigned i = 0; i < mock.submit_count; ++i) {
        const auto& entry = mock.submits[i];
        if (entry.queue == queue && entry.fence && state(entry.fence).generation == entry.generation)
            state(entry.fence).signaled = true;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL device_idle(VkDevice) {
    ++mock.device_waits;
    for (auto& fence : mock.fences)
        if (fence.live) fence.signaled = true;
    if (thread_mode.load() == 3) {
        std::unique_lock<std::mutex> lock(progress_mutex);
        entered = true;
        progress.notify_all();
        if (!progress.wait_for(lock, std::chrono::seconds(2), [] { return released; })) return VK_TIMEOUT;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL proc(VkDevice, const char* name) {
#define PROC(n, f) \
    if (!std::strcmp(name, "vk" n)) return reinterpret_cast<PFN_vkVoidFunction>(f);
    PROC("QueueSubmit", submit1);

    PROC("QueueSubmit2", submit2);
    PROC("QueueSubmit2KHR", submit2);
    PROC("CreateFence", create_fence);

    PROC("DestroyFence", destroy_fence);
    PROC("GetFenceStatus", status);
    PROC("WaitForFences", wait);
    PROC("ResetFences", reset);

    PROC("QueueWaitIdle", queue_idle);
    PROC("DeviceWaitIdle", device_idle);
    PROC("CreateSemaphore", create_semaphore);
    PROC("DestroySemaphore", destroy_semaphore);
    PROC("GetSemaphoreCounterValue", counter);
    PROC("GetSemaphoreCounterValueKHR", counter);
    PROC("GetFenceFdKHR", get_fd);
    PROC("ImportFenceFdKHR", import_fd);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    PROC("GetFenceWin32HandleKHR", get_win32);
    PROC("ImportFenceWin32HandleKHR", import_win32);
#endif

#undef PROC
    return nullptr;
}
SubmissionTracker* tracker(bool timeline = false, uint64_t max_difference = UINT64_MAX) {
    SubmissionTracker* out = nullptr;
    expect(submissions_create({device,
                               proc,
                               reinterpret_cast<AccelerationBackend*>(1),
                               nullptr,
                               timeline,
                               {duplicate_fd, poll_fd, close_fd},
                               max_difference},
                              &out) == VK_SUCCESS,
           "tracker initializes downstream dispatch");
    return out;
}
void reset_mock() {
    mock = Mock{};
    destroyed_tokens = 0;
    thread_mode = 0;
    entered = false;
    released = false;
}
void staggeredAndReuse() {
    reset_mock();
    auto* t = tracker();
    auto a = user_fence(1), b = user_fence(2);
    auto first = token(1), second = token(2), third = token(3);
    std::weak_ptr<AccelerationRecording> weak_first = first, weak_second = second, weak_third = third;
    std::vector<AccelerationRecordingRef> tokens{first};
    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "first queue AS write submits");
    tokens = {second};
    expect(submissions_queue_submit(t, queue2, 1, &info, b, tokens) == VK_SUCCESS, "second queue AS write submits");
    first.reset();
    second.reset();
    tokens.clear();
    expect(!weak_first.expired() && !weak_second.expired(), "pending tokens survive application CB reset");
    state(a).signaled = true;
    expect(submissions_reset_fences(t, 1, &a) == VK_SUCCESS && mock.resets == 1,
           "reset captures prior completed generation before native reset");
    const auto polls_a = state(a).polls;
    tokens = {third};
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "reset user fence reused by later AS write");
    third.reset();
    tokens.clear();
    expect(mock.collect_count == 0 && !weak_first.expired(), "completed queue1 generation retained while queue2 writer is pending");
    state(b).signaled = true;
    expect(submissions_get_fence_status(t, b) == VK_SUCCESS && mock.collect_count == 0,
           "staggered second queue completes while reused fence remains pending");
    expect(state(a).polls == polls_a + 1, "only new incomplete fence generation is polled, completed old entry is never repolled");
    state(a).signaled = true;
    mock.allow_collect = true;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 3,
           "all writer generations collect together after global quiescence");
    expect(weak_first.expired() && weak_second.expired() && weak_third.expired() && destroyed_tokens == 3,
           "all pending token owners release after body publication ownership is collected");
    expect(mock.destroys == 0, "user fences remain application-owned");
    expect(submissions_destroy_fence(t, a, nullptr) == VK_SUCCESS && submissions_destroy_fence(t, b, nullptr) == VK_SUCCESS,
           "user fence destruction forwards exactly once after retirement");
    submissions_destroy(t);
}
void privateAndFailures() {
    reset_mock();
    auto* t = tracker();
    auto recording = token(9);
    std::weak_ptr<AccelerationRecording> weak = recording;
    std::vector<AccelerationRecordingRef> tokens{recording};
    VkSubmitInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    expect(submissions_queue_submit2(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_SUCCESS,
           "fenceless submit2 accepts AS recording");
    const auto private_fence = mock.submits[0].fence;
    expect(
        private_fence && mock.submit_count == 1 && mock.creates == 1 && mock.submits[0].version2 && mock.submits[0].infos == &info,
        "one private fence is attached to same original submit2 without extra submits or array replacement");
    recording.reset();
    tokens.clear();
    expect(!weak.expired() && submissions_poll(t) == VK_SUCCESS && mock.collect_count == 0,
           "unsignaled private fence keeps AS resources live");
    state(private_fence).signaled = true;
    mock.allow_collect = true;
    expect(submissions_poll(t) == VK_SUCCESS && weak.expired() && state(private_fence).destroys == 1,
           "private fence and pending token destroyed exactly once after collection");
    auto failed = token(10);
    tokens = {failed};
    mock.next_submit = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const auto destroys = mock.destroys;
    expect(submissions_queue_submit2(t, queue2, 1, &info, VK_NULL_HANDLE, tokens) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
               mock.destroys == destroys + 1,
           "native failed submit destroys private unsignaled fence and rolls back pending entry");
    mock.next_submit = VK_SUCCESS;
    tokens.clear();
    failed.reset();
    const auto creates = mock.creates, submits = mock.submit_count;
    auto oom = token(11);
    tokens = {oom};
    for (int budget : {0, 1}) {
        allocation_budget = budget;
        const auto result = submissions_queue_submit2(t, queue1, 1, &info, VK_NULL_HANDLE, tokens);
        allocation_budget = -1;
        expect(result == VK_ERROR_OUT_OF_HOST_MEMORY && mock.creates == creates && mock.submit_count == submits,
               "pending node/token-copy OOM happens before private fence creation and native submit prefix");
    }
    mock.next_create = VK_ERROR_OUT_OF_HOST_MEMORY;
    expect(submissions_queue_submit2(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_ERROR_OUT_OF_HOST_MEMORY &&
               mock.submit_count == submits,
           "private fence creation failure rolls back prepared token ownership without submitting");
    mock.next_create = VK_SUCCESS;
    const auto waits = mock.waits, queue_waits = mock.queue_waits, device_waits = mock.device_waits;
    expect(waits == 0 && queue_waits == 0 && device_waits == 0, "poll/submit/failure paths never inject waits");
    submissions_destroy(t);
}
void waitAndIdleScopes() {
    reset_mock();
    auto* t = tracker();
    auto a = user_fence(1), b = user_fence(2);
    std::vector<AccelerationRecordingRef> tokens{token(1)};
    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "queue1 writer registered for idle scope test");
    tokens = {token(2)};
    expect(submissions_queue_submit(t, queue2, 1, &info, b, tokens) == VK_SUCCESS, "queue2 writer registered for idle scope test");
    tokens.clear();
    expect(submissions_queue_wait_idle(t, queue1) == VK_SUCCESS && mock.queue_waits == 1 && mock.collect_count == 0,
           "queue idle completes only matching queue and cannot retire another queue's writer");
    expect(submissions_wait_for_fences(t, 1, &b, VK_TRUE, 0) == VK_TIMEOUT && mock.waits == 1 && mock.collect_count == 0,
           "user fence timeout returns unchanged and performs no premature collection");
    mock.allow_collect = true;
    expect(submissions_device_wait_idle(t) == VK_SUCCESS && mock.device_waits == 1 && mock.collect_count == 2,
           "successful user device idle completes every registered writer and collects");
    auto c = user_fence(3), d = user_fence(4);
    tokens = {token(3)};
    expect(submissions_queue_submit(t, queue1, 1, &info, c, tokens) == VK_SUCCESS, "first wait-any writer registers");
    tokens = {token(4)};
    expect(submissions_queue_submit(t, queue2, 1, &info, d, tokens) == VK_SUCCESS, "second wait-any writer registers");
    tokens.clear();
    mock.allow_collect = false;
    state(c).signaled = true;
    const VkFence both[] = {c, d};
    expect(submissions_wait_for_fences(t, 2, both, VK_FALSE, 0) == VK_SUCCESS && mock.collect_count == 2,
           "wait-any success inspects statuses instead of marking every requested fence complete");
    state(d).signaled = true;
    mock.allow_collect = true;
    expect(submissions_wait_for_fences(t, 2, both, VK_TRUE, 0) == VK_SUCCESS && mock.collect_count == 4,
           "wait-all success retires every matching complete writer");
    tokens = {token(5)};
    auto e = user_fence(5);
    expect(submissions_queue_submit(t, queue1, 1, &info, e, tokens) == VK_SUCCESS, "error-propagation writer registers");
    tokens.clear();
    state(e).error = VK_ERROR_DEVICE_LOST;
    expect(submissions_poll(t) == VK_ERROR_DEVICE_LOST && submissions_get_fence_status(t, e) == VK_ERROR_DEVICE_LOST,
           "native fence-status device loss propagates without reading GPU roots");
    expect(submissions_reset_fences(t, 1, &e) == VK_ERROR_DEVICE_LOST, "device loss prevents reset from hiding pending writer");
    state(e).error = VK_SUCCESS;
    expect(submissions_reset_fences(t, 1, &e) == VK_ERROR_VALIDATION_FAILED_EXT,
           "invalid reset of unsignaled active submission fails closed");
    state(e).signaled = true;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 5, "writer can retire after later successful completion");
    submissions_destroy(t);
}
void multithreadProgress() {
    reset_mock();
    auto* t = tracker();
    auto a = user_fence(1);
    auto recording = token(1);
    std::vector<AccelerationRecordingRef> tokens{recording}, empty;
    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "cross-queue wait writer registers");
    tokens.clear();
    recording.reset();
    mock.allow_collect = true;
    thread_mode = 1;
    std::atomic<int> waited{VK_NOT_READY};
    std::thread waiter([&] { waited = submissions_wait_for_fences(t, 1, &a, VK_TRUE, UINT64_MAX); });
    {
        std::unique_lock<std::mutex> lock(progress_mutex);
        expect(progress.wait_for(lock, std::chrono::seconds(2), [] { return entered; }), "native user wait reached blocking spy");
    }
    expect(submissions_queue_submit(t, queue2, 1, &info, VK_NULL_HANDLE, empty) == VK_SUCCESS,
           "second queue signal submit progresses while another thread waits for a fence");
    waiter.join();
    expect(waited.load() == VK_SUCCESS && mock.collect_count == 1,
           "cross-queue signal releases user wait without layer-added deadlock");
    submissions_destroy(t);

    reset_mock();
    t = tracker();
    a = user_fence(1);
    tokens = {token(2)};
    thread_mode = 2;
    std::atomic<int> submitted{VK_NOT_READY};
    std::thread submitter([&] { submitted = submissions_queue_submit(t, queue1, 1, &info, a, tokens); });
    {
        std::unique_lock<std::mutex> lock(progress_mutex);
        expect(progress.wait_for(lock, std::chrono::seconds(2), [] { return entered; }),
               "native AS submit reached backpressure spy");
    }
    const auto polled = state(a).polls;
    expect(submissions_poll(t) == VK_SUCCESS && state(a).polls == polled && mock.collect_count == 0,
           "registered in-flight native submit blocks collection and is never polled as a submitted fence");
    expect(submissions_queue_submit(t, queue2, 1, &info, VK_NULL_HANDLE, empty) == VK_SUCCESS,
           "another queue can submit a signal while native AS submit is backpressured");
    submitter.join();
    expect(submitted.load() == VK_SUCCESS, "backpressured native submit completes after independent queue signal");
    tokens.clear();
    state(a).signaled = true;
    mock.allow_collect = true;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 1,
           "committed native submit retires after actual fence completion");
    submissions_destroy(t);

    reset_mock();
    t = tracker();
    a = user_fence(1);
    auto b = user_fence(2);
    tokens = {token(3)};
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "pre-idle writer registers");
    tokens = {token(4)};
    thread_mode = 3;
    std::atomic<int> idled{VK_NOT_READY};
    std::thread idler([&] { idled = submissions_device_wait_idle(t); });
    {
        std::unique_lock<std::mutex> lock(progress_mutex);
        expect(progress.wait_for(lock, std::chrono::seconds(2), [] { return entered; }),
               "native device-idle spy captures prior work");
    }
    // The native idle spy completed earlier work and is now returning while a
    // later queue submission races. Its fence remains unsignaled.
    state(b).signaled = false;
    mock.allow_collect = true;
    expect(submissions_queue_submit(t, queue2, 1, &info, b, tokens) == VK_SUCCESS,
           "new AS writer can submit during explicit device idle wait");
    idler.join();
    expect(idled.load() == VK_SUCCESS && mock.collect_count == 1 && !state(b).signaled,
           "idle cutoff never marks later submitted writer complete or reads roots while it is pending");
    tokens.clear();
    state(b).signaled = true;
    mock.allow_collect = true;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 2,
           "old and newer generations collect only after newer fence completes");
    submissions_destroy(t);
}
void independentASProgress() {
    reset_mock();
    auto* t = tracker();
    auto a = user_fence(1), b = user_fence(2);
    auto first = token(1, 101), other = token(2, 202);
    std::weak_ptr<AccelerationRecording> first_weak = first, other_weak = other;
    std::vector<AccelerationRecordingRef> tokens{first};
    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "first independent AS writer submits");
    tokens = {other};
    expect(submissions_queue_submit(t, queue2, 1, &info, b, tokens) == VK_SUCCESS, "second independent AS writer remains pending");
    tokens.clear();
    first.reset();
    other.reset();
    mock.allow_collect = true;
    state(a).signaled = true;
    expect(submissions_get_fence_status(t, a) == VK_SUCCESS && mock.collect_count == 1 && first_weak.expired() &&
               !other_weak.expired(),
           "completed AS retires while another queue continues writing a distinct AS");
    for (unsigned frame = 0; frame < 3; ++frame) {
        expect(submissions_reset_fences(t, 1, &a) == VK_SUCCESS, "independent AS frame fence resets");
        auto current = token(10 + frame, 101);
        std::weak_ptr<AccelerationRecording> weak = current;
        tokens = {current};
        expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS,
               "independent AS can rebuild while other AS stays pending");
        current.reset();
        tokens.clear();
        state(a).signaled = true;
        expect(submissions_get_fence_status(t, a) == VK_SUCCESS && weak.expired() && !other_weak.expired() &&
                   mock.collect_count == frame + 2,
               "fence-only distinct-AS loop retires each frame instead of accumulating behind unrelated writer");
    }
    state(b).signaled = true;
    expect(submissions_poll(t) == VK_SUCCESS && other_weak.expired() && mock.collect_count == 5,
           "remaining independent AS retires when its own writer completes");
    submissions_destroy(t);
}
void destroyedCompletedFence() {
    reset_mock();
    auto* t = tracker();
    auto a = user_fence(1), b = user_fence(2);
    auto first = token(1), second = token(2);
    std::vector<AccelerationRecordingRef> tokens{first};
    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "destroy-fence completed generation registers");
    tokens = {second};
    expect(submissions_queue_submit(t, queue2, 1, &info, b, tokens) == VK_SUCCESS,
           "overlapping writer keeps completed generation retained");
    tokens.clear();
    first.reset();
    second.reset();
    state(a).signaled = true;
    expect(submissions_destroy_fence(t, a, nullptr) == VK_SUCCESS && !state(a).live && mock.collect_count == 0,
           "completed user fence can be destroyed while its token waits for overlapping AS writer");
    const auto polls = state(a).polls;
    expect(submissions_poll(t) == VK_SUCCESS && state(a).polls == polls,
           "destroyed completed fence is never repolled even when retained token still exists");
    state(b).signaled = true;
    mock.allow_collect = true;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 2 && destroyed_tokens == 2,
           "destroyed fence generation's token retires after remaining overlapping writer completes");
    submissions_destroy(t);
}
void timelineGenerations() {
    reset_mock();
    auto* t = tracker(true);
    auto a = user_fence(1), b = user_fence(2);
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    auto first = token(1, 101), second = token(2, 101), other = token(3, 202);
    std::weak_ptr<AccelerationRecording> weak_first = first, weak_second = second, weak_other = other;
    std::vector<AccelerationRecordingRef> tokens{first};
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS,
           "timeline observes original user-fence AS submit");
    const auto q1 = mock.submits[0].observer;
    expect(q1 && mock.submits[0].observer_value == 1 && mock.submits[0].fence == a && mock.creates == 0,
           "private timeline appends signal to same submit without replacing its application fence");
    VkFenceGetFdInfoKHR export_info{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, nullptr, a, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    int fd = -99;
    expect(submissions_get_fence_fd(t, &export_info, &fd) == VK_SUCCESS && fd >= 0 && state(a).generation == 1,
           "pending copy export resets user payload while private timeline retains old generation");
    tokens = {second};
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS && mock.submits[1].observer == q1 &&
               mock.submits[1].observer_value == 2,
           "reused native fence receives a new private per-queue counter generation");
    tokens = {other};
    expect(submissions_queue_submit(t, queue2, 1, &info, b, tokens) == VK_SUCCESS, "independent queue timeline writer submits");
    const auto q2 = mock.submits[2].observer;
    expect(q2 && q2 != q1 && mock.submits[2].observer_value == 1 && mock.semaphore_creates == 2,
           "queues have independent private timelines and counter sequences");
    first.reset();
    second.reset();
    other.reset();
    tokens.clear();
    state(a).signaled = true;
    state(b).signaled = true;
    const VkFence both[] = {a, b};
    expect(submissions_wait_for_fences(t, 2, both, VK_TRUE, 0) == VK_SUCCESS && mock.collect_count == 0,
           "externally signaled or reused user payloads do not complete private AS generations");
    expect(submissions_get_fence_status(t, a) == VK_SUCCESS && !weak_first.expired() && !weak_second.expired(),
           "user fence status result is preserved without trusting its signal as retirement evidence");
    mock.allow_collect = true;
    mock.timelines[value(q2)].counter = 1;
    expect(submissions_poll(t) == VK_SUCCESS && weak_other.expired() && !weak_first.expired(),
           "completed independent queue retires without implying another queue's counter progressed");
    mock.timelines[value(q1)].counter = 1;
    expect(submissions_poll(t) == VK_SUCCESS && !weak_first.expired() && !weak_second.expired(),
           "completed old timeline generation remains while same AS has a later writer");
    mock.timelines[value(q1)].counter = 2;
    expect(submissions_poll(t) == VK_SUCCESS && weak_first.expired() && weak_second.expired() && mock.collect_count == 3,
           "both same-queue generations retire only at actual last writer timeline completion");
    expect(mock.submit_count == 3 && mock.waits == 1 && !mock.queue_waits && !mock.device_waits && !mock.duplicates,
           "timeline generation observation adds no submit, wait, or unnecessary FD duplicate");
    expect(submissions_destroy_fence(t, VK_NULL_HANDLE, nullptr) == VK_SUCCESS,
           "destroying NULL fence cannot match fenceless timeline entries");
    close_fd(fd);
    submissions_destroy(t);
    expect(mock.semaphore_destroys == 2, "private queue timelines are released during completed tracker teardown");
}
void timelineSubmitShapes() {
    reset_mock();
    auto* t = tracker(true);
    const VkSemaphore waits[] = {handle<VkSemaphore>(1), handle<VkSemaphore>(2)},
                      signals[] = {handle<VkSemaphore>(3), handle<VkSemaphore>(4)};
    const VkPipelineStageFlags stages[] = {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    const uint64_t wait_values[] = {23, 47}, signal_values[] = {67, 89};
    const uint32_t wait_devices[] = {1, 0}, signal_devices[] = {0, 1}, command_masks[] = {3};
    const VkCommandBuffer command = handle<VkCommandBuffer>(5);
    VkTimelineSemaphoreSubmitInfo timeline{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr, 2, wait_values, 2, signal_values};
    VkDeviceGroupSubmitInfo group{
        VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO, &timeline, 2, wait_devices, 1, command_masks, 2, signal_devices};
    VkProtectedSubmitInfo protected_info{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO, &group, VK_TRUE};
    VkSubmitInfo infos[2] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO},
                             {VK_STRUCTURE_TYPE_SUBMIT_INFO, &protected_info, 2, waits, stages, 1, &command, 2, signals}};
    std::vector<AccelerationRecordingRef> tokens{token(1)};
    expect(submissions_queue_submit(t, queue1, 2, infos, VK_NULL_HANDLE, tokens) == VK_SUCCESS,
           "submit1 supports an existing timeline structure behind other pNext nodes");
    const auto& first = mock.submits[0];
    expect(
        first.count == 2 && first.signal_count == 3 && first.wait_count == 2 && first.command_count == 1 && first.protected_submit,
        "submit1 preserves array length, commands, waits and protected-submit chain");
    expect(first.app_signals[0] == signals[0] && first.app_signals[1] == signals[1] && first.signal_values[0] == 67 &&
               first.signal_values[1] == 89 && first.wait_values[0] == 23 && first.wait_values[1] == 47 &&
               first.signal_devices[0] == 0 && first.signal_devices[1] == 1 && first.signal_devices[2] == 0,
           "submit1 preserves original timeline values/device indices and appends private device0 signal");
    expect(infos[0].signalSemaphoreCount == 0 && infos[1].signalSemaphoreCount == 2 && timeline.signalSemaphoreValueCount == 2 &&
               signal_values[1] == 89 && group.signalSemaphoreCount == 2,
           "submit1 translation never mutates caller arrays, values or extension chain");
    mock.timelines[value(first.observer)].counter = first.observer_value;
    mock.allow_collect = true;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 1,
           "fenceless timeline generation retires without an internal fence");
    VkSubmitInfo binary{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    binary.signalSemaphoreCount = 1;
    binary.pSignalSemaphores = signals;
    expect(submissions_queue_submit(t, queue1, 1, &binary, VK_NULL_HANDLE, tokens) == VK_SUCCESS &&
               mock.submits[1].signal_count == 2 && mock.submits[1].signal_values[0] == 0,
           "submit1 adds zero value for an original binary signal when introducing timeline info");
    expect(submissions_destroy_fence(t, VK_NULL_HANDLE, nullptr) == VK_SUCCESS && mock.collect_count == 1,
           "NULL fence destruction cannot complete a pending fenceless timeline submission");
    mock.timelines[value(first.observer)].counter = mock.submits[1].observer_value;
    expect(submissions_poll(t) == VK_SUCCESS, "binary+private timeline submission retires");
    VkSemaphoreSubmitInfo wait2{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, waits[0], 113,
                                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,     1};
    VkSemaphoreSubmitInfo signal2[2] = {
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, signals[0], 137, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 1},
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, signals[1], 151, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0}};
    VkCommandBufferSubmitInfo command2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, command, 3};
    VkSubmitInfo2 info2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, VK_SUBMIT_PROTECTED_BIT, 1, &wait2, 1, &command2, 2, signal2};
    expect(submissions_queue_submit2(t, queue2, 1, &info2, VK_NULL_HANDLE, tokens) == VK_SUCCESS,
           "submit2 appends a private timeline signal");
    const auto& second = mock.submits[2];
    expect(second.flags == VK_SUBMIT_PROTECTED_BIT && second.signal_count == 3 && second.wait_values[0] == 113 &&
               second.signal_values[0] == 137 && second.signal_values[1] == 151 &&
               second.signal_stages[0] == VK_PIPELINE_STAGE_2_TRANSFER_BIT && second.signal_devices[0] == 1 &&
               second.signal_stages[2] == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT && second.signal_devices[2] == 0,
           "submit2 preserves app flags/stages/values/device indices and covers private completion with ALL_COMMANDS");
    expect(info2.signalSemaphoreInfoCount == 2 && signal2[0].stageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT,
           "submit2 translation preserves caller storage");
    mock.timelines[value(second.observer)].counter = second.observer_value;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 3 && !mock.creates && !mock.waits,
           "all timeline submit forms retire without injected native fences or waits");
    submissions_destroy(t);
}
void timelineFailuresAndProgress() {
    for (bool version2 : {false, true}) {
        bool reached_submit = false;
        for (int budget = 0; budget < 96; ++budget) {
            reset_mock();
            auto* t = tracker(true);
            VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            const VkSemaphore signal = handle<VkSemaphore>(3), wait = handle<VkSemaphore>(4);
            const uint64_t signal_value = 97, wait_value = 17;
            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            const uint32_t signal_device = 1;
            VkTimelineSemaphoreSubmitInfo values{
                VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr, 1, &wait_value, 1, &signal_value};
            VkDeviceGroupSubmitInfo group{
                VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO, &values, 0, nullptr, 0, nullptr, 1, &signal_device};
            info.pNext = &group;
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &wait;
            info.pWaitDstStageMask = &stage;
            info.signalSemaphoreCount = 1;
            info.pSignalSemaphores = &signal;
            VkSemaphoreSubmitInfo signal2{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, signal, signal_value,
                                          VK_PIPELINE_STAGE_2_TRANSFER_BIT,        1};
            VkSemaphoreSubmitInfo wait2{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, wait, wait_value,
                                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,  0};
            VkSubmitInfo2 info2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, 0, 1, &wait2, 0, nullptr, 1, &signal2};
            std::vector<AccelerationRecordingRef> tokens{token(1)};
            std::weak_ptr<AccelerationRecording> weak = tokens[0];
            allocation_budget = budget;
            const auto result = version2 ? submissions_queue_submit2(t, queue1, 1, &info2, VK_NULL_HANDLE, tokens)
                                         : submissions_queue_submit(t, queue1, 1, &info, VK_NULL_HANDLE, tokens);
            allocation_budget = -1;
            tokens.clear();
            if (result == VK_ERROR_OUT_OF_HOST_MEMORY) {
                expect(!mock.submit_count && weak.expired(),
                       "every timeline prepare OOM rolls back tokens before native submission");
            } else {
                expect(result == VK_SUCCESS && mock.submit_count == 1,
                       "timeline preparation eventually reaches exactly one native submit");
                mock.allow_collect = true;
                mock.timelines[value(mock.submits[0].observer)].counter = mock.submits[0].observer_value;
                expect(submissions_poll(t) == VK_SUCCESS && weak.expired(),
                       "successful post-OOM timeline transaction retires normally");
                reached_submit = true;
            }
            submissions_destroy(t);
            if (reached_submit) break;
        }
        expect(reached_submit, "timeline preparation fault sweep reaches its success boundary for both submit APIs");
    }
    reset_mock();
    auto* t = tracker(true);
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    std::vector<AccelerationRecordingRef> tokens{token(1)};
    mock.next_semaphore = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    expect(submissions_queue_submit2(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
               !mock.submit_count,
           "private timeline creation failure occurs before native submit");
    mock.next_semaphore = VK_SUCCESS;
    mock.next_submit = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    expect(submissions_queue_submit2(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_ERROR_OUT_OF_DEVICE_MEMORY,
           "native failed timeline submit removes its pending writer generation");
    mock.next_submit = VK_SUCCESS;
    expect(
        submissions_queue_submit2(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_SUCCESS && mock.submits[1].observer_value == 2,
        "a native failed timeline submission leaves a monotonic unused counter gap");
    tokens.clear();
    mock.allow_collect = true;
    mock.timelines[value(mock.submits[1].observer)].counter = 1;
    expect(submissions_poll(t) == VK_SUCCESS && !mock.collect_count,
           "a failed submission counter cannot complete the newer generation");
    mock.timelines[value(mock.submits[1].observer)].counter = 2;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 1,
           "newer successful generation retires at its actual counter");
    submissions_destroy(t);
    reset_mock();
    t = tracker(true);
    auto a = user_fence(1);
    tokens = {token(2)};
    std::vector<AccelerationRecordingRef> empty;
    VkSubmitInfo info1{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    thread_mode = 2;
    std::atomic<int> submitted{VK_NOT_READY};
    std::thread submitter([&] { submitted = submissions_queue_submit(t, queue1, 1, &info1, a, tokens); });
    {
        std::unique_lock<std::mutex> lock(progress_mutex);
        expect(progress.wait_for(lock, std::chrono::seconds(2), [] { return entered; }),
               "timeline native submit reaches backpressure without holding tracker mutex");
    }
    const auto observer = mock.submits[0].observer;
    const auto polled = mock.timelines[value(observer)].polls;
    expect(submissions_poll(t) == VK_SUCCESS && mock.timelines[value(observer)].polls == polled,
           "in-flight timeline generation is not polled before native submit commits");
    expect(submissions_queue_submit(t, queue2, 1, &info1, VK_NULL_HANDLE, empty) == VK_SUCCESS,
           "independent queue can signal while timeline native submit is backpressured");
    submitter.join();
    expect(submitted == VK_SUCCESS, "timeline native submit completes after unrelated signal progress");
    tokens.clear();
    mock.allow_collect = true;
    mock.timelines[value(observer)].counter = mock.submits[0].observer_value;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 1,
           "committed formerly in-flight timeline generation retires");
    submissions_destroy(t);
}
void syncFdGenerations() {
    reset_mock();
    auto* t = tracker();
    auto a = user_fence(1);
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    std::vector<AccelerationRecordingRef> tokens{token(1)};
    std::weak_ptr<AccelerationRecording> weak = tokens[0];
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "sync FD fallback writer submits");
    tokens.clear();
    VkFenceGetFdInfoKHR get{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, nullptr, a, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    int fd = -99;
    expect(submissions_get_fence_fd(t, &get, &fd) == VK_SUCCESS && fd >= 0 && mock.duplicates == 1,
           "pending SYNC_FD copy-export installs an independently owned payload duplicate");
    const auto payload = mock.fds[fd].payload;
    VkImportFenceFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, nullptr, a, VK_FENCE_IMPORT_TEMPORARY_BIT,
                                    VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,  -1};
    expect(submissions_import_fence_fd(t, &imported) == VK_SUCCESS && state(a).signaled,
           "new signaled temporary import can replace reset original fence payload while detached old generation remains pending");
    expect(submissions_get_fence_status(t, a) == VK_SUCCESS && !weak.expired() && !mock.collect_count,
           "new imported signal cannot complete an older copy-exported AS writer");
    expect(submissions_wait_for_fences(t, 1, &a, VK_TRUE, 0) == VK_SUCCESS && !weak.expired(),
           "wait-all on new imported payload cannot complete old detached generation");
    expect(submissions_destroy_fence(t, a, nullptr) == VK_SUCCESS && !state(a).live && !weak.expired(),
           "original fence can be destroyed independently from exported pending payload observer");
    close_fd(fd);  // Application closes its own handle before old AS completion.
    mock.payloads[payload] = true;
    mock.allow_collect = true;
    expect(submissions_poll(t) == VK_SUCCESS && weak.expired() && mock.collect_count == 1 && mock.closes == 2,
           "duplicate remains live after application close and retires old AS writer when that payload actually signals");
    submissions_destroy(t);

    reset_mock();
    t = tracker();
    a = user_fence(1);
    get.fence = a;
    tokens = {token(2)};
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "duplicate-failure recovery writer submits");
    weak = tokens[0];
    tokens.clear();
    mock.fail_dup = true;
    mock.export_oom_after_success = true;
    expect(submissions_get_fence_fd(t, &get, &fd) == VK_SUCCESS && fd >= 0,
           "successful native export preserves result/output despite post-export OOM and dup failure");
    allocation_budget = -1;
    mock.export_oom_after_success = false;
    state(a).signaled = true;
    expect(submissions_get_fence_status(t, a) == VK_SUCCESS && !weak.expired(),
           "unobservable old payload never falls back to reused handle signal");
    close_fd(fd);
    tokens = {token(3)};
    expect(submissions_queue_submit(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_SUCCESS,
           "later application AS batch creates ordinary private fence fallback");
    tokens.clear();
    state(mock.submits[1].fence).signaled = true;
    mock.allow_collect = true;
    expect(submissions_poll(t) == VK_SUCCESS && weak.expired() && mock.collect_count == 2,
           "later proven submission on same queue safely recovers unobservable old generation without an injected wait");
    submissions_destroy(t);

    reset_mock();
    t = tracker();
    a = user_fence(1);
    get.fence = a;
    tokens = {token(4)};
    weak = tokens[0];
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "failed-export rollback writer submits");
    tokens.clear();
    mock.next_export = VK_ERROR_TOO_MANY_OBJECTS;
    fd = 123;
    expect(submissions_get_fence_fd(t, &get, &fd) == VK_ERROR_TOO_MANY_OBJECTS && fd == 123 && !state(a).generation,
           "failed native export leaves output and original fence generation intact");
    state(a).signaled = true;
    mock.allow_collect = true;
    expect(submissions_poll(t) == VK_SUCCESS && weak.expired() && !mock.duplicates,
           "failed export restores original attached observer");
    submissions_destroy(t);

    reset_mock();
    t = tracker();
    a = user_fence(1);
    get.fence = a;
    tokens = {token(5)};
    weak = tokens[0];
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "minus-one export writer submits");
    tokens.clear();
    mock.signal_during_export = true;
    mock.export_minus_one = true;
    mock.allow_collect = true;
    expect(submissions_get_fence_fd(t, &get, &fd) == VK_SUCCESS && fd == -1 && weak.expired() && !mock.duplicates,
           "SYNC_FD minus-one returned during native export proves that exact old trusted generation completed");
    submissions_destroy(t);
}
void timelineDifferenceLimit() {
    reset_mock();
    auto* t = tracker(true, 2);
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    std::vector<AccelerationRecordingRef> tokens{token(1)};
    expect(submissions_queue_submit(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_SUCCESS &&
               submissions_queue_submit(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_SUCCESS,
           "private signal values may reach exact maxTimelineSemaphoreValueDifference boundary");
    expect(submissions_queue_submit(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
               mock.submit_count == 2,
           "uncompleted private counter difference cannot exceed actual device limit in a native submission");
    const auto observer = mock.submits[0].observer;
    mock.timelines[value(observer)].counter = 1;
    expect(
        submissions_queue_submit(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_SUCCESS && mock.submits[2].observer_value == 3,
        "limit rejection rolls back reservation and allows retry after actual timeline progress");
    tokens.clear();
    mock.allow_collect = true;
    mock.timelines[value(observer)].counter = 3;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 3,
           "counter-limit retries retain and collect exactly successful generations");
    submissions_destroy(t);
    reset_mock();
    t = tracker(true, 2);
    tokens = {token(2)};
    mock.next_counter = VK_ERROR_DEVICE_LOST;
    expect(submissions_queue_submit(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_ERROR_DEVICE_LOST && !mock.submit_count,
           "private counter query error propagates before native prefix");
    mock.next_counter = VK_SUCCESS;
    expect(
        submissions_queue_submit(t, queue1, 1, &info, VK_NULL_HANDLE, tokens) == VK_SUCCESS && mock.submits[0].observer_value == 1,
        "failed pre-submit counter query rolls back reservation without a phantom signal generation");
    tokens.clear();
    mock.allow_collect = true;
    mock.timelines[value(mock.submits[0].observer)].counter = 1;
    expect(submissions_poll(t) == VK_SUCCESS && mock.collect_count == 1, "retry after counter query error retires normally");
    submissions_destroy(t);
}
void referenceAndImportGenerations() {
    reset_mock();
    auto* t = tracker();
    auto a = user_fence(1);
    VkFenceGetFdInfoKHR get{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, nullptr, a, VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT};
    int fd = -1;
    expect(submissions_get_fence_fd(t, &get, &fd) == VK_SUCCESS && !state(a).generation && !mock.resets,
           "opaque reference export preserves native payload and reset semantics");
    std::vector<AccelerationRecordingRef> tokens{token(1)};
    std::weak_ptr<AccelerationRecording> weak = tokens[0];
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS,
           "previously reference-exported fence accepts AS submission");
    tokens.clear();
    state(a).signaled = true;
    expect(submissions_get_fence_status(t, a) == VK_SUCCESS && !weak.expired() && !mock.collect_count,
           "future fallback submission does not trust reference-shared payload signal for internal lifetime");
    mock.allow_collect = true;
    expect(submissions_queue_wait_idle(t, queue1) == VK_SUCCESS && weak.expired(),
           "explicit queue idle recovers reference-shared fallback generation");
    close_fd(fd);
    submissions_destroy(t);

    reset_mock();
    t = tracker();
    a = user_fence(1);
    auto b = user_fence(2);
    tokens = {token(2)};
    weak = tokens[0];
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "completed-before-import writer submits");
    tokens = {token(3)};
    expect(submissions_queue_submit(t, queue2, 1, &info, b, tokens) == VK_SUCCESS,
           "overlapping writer retains completed old generation across import");
    tokens.clear();
    state(a).signaled = true;
    const auto payload = mock.next_payload++;
    fd = int(mock.next_fd++);
    mock.fds[fd] = {true, payload};
    VkImportFenceFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, nullptr, a, VK_FENCE_IMPORT_TEMPORARY_BIT,
                                    VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,  fd};
    expect(submissions_import_fence_fd(t, &imported) == VK_SUCCESS && !state(a).signaled && !mock.fds[fd].live,
           "successful import freezes prior completed generation before consuming/replacing payload");
    const auto polls = state(a).polls;
    state(b).signaled = true;
    mock.allow_collect = true;
    expect(submissions_poll(t) == VK_SUCCESS && weak.expired() && mock.collect_count == 2 && state(a).polls == polls,
           "completed old generation is never repolled against unsignaled imported payload");
    submissions_destroy(t);
}
void referenceMutationNeverCompletesWork() {
    for (unsigned operation = 0; operation < 3; ++operation) {
        reset_mock();
        auto* t = tracker();
        auto a = user_fence(1);
        VkFenceGetFdInfoKHR get{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, nullptr, a, VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT};
        int fd = -1;
        expect(submissions_get_fence_fd(t, &get, &fd) == VK_SUCCESS, "shared generation mutation starts from reference export");
        std::vector<AccelerationRecordingRef> tokens{token(1)};
        std::weak_ptr<AccelerationRecording> weak = tokens[0];
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS,
               "shared generation mutation writer registers");
        tokens.clear();
        state(a).signaled = true;
        VkImportFenceFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, nullptr, a, VK_FENCE_IMPORT_TEMPORARY_BIT,
                                        VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,  -1};
        VkResult result = VK_NOT_READY;
        if (operation == 0) result = submissions_reset_fences(t, 1, &a);
        if (operation == 1) result = submissions_import_fence_fd(t, &imported);
        if (operation == 2) result = submissions_destroy_fence(t, a, nullptr);
        expect(result == VK_SUCCESS && !weak.expired() && !mock.collect_count,
               "successful reset/import/destroy of shared handle does not prove original local AS generation completed");
        mock.allow_collect = true;
        expect(submissions_queue_wait_idle(t, queue1) == VK_SUCCESS && weak.expired(),
               "shared detached generation recovers only at actual same-queue completion boundary");
        close_fd(fd);
        submissions_destroy(t);
    }
    reset_mock();
    auto* t = tracker();
    auto a = user_fence(1);
    VkImportFenceFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, nullptr, a, VK_FENCE_IMPORT_TEMPORARY_BIT,
                                    VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,  -1};
    allocation_budget = 0;
    auto result = submissions_import_fence_fd(t, &imported);
    allocation_budget = -1;
    expect(result == VK_ERROR_OUT_OF_HOST_MEMORY && !mock.imports,
           "import bookkeeping OOM occurs before native ownership transfer");
    mock.next_import = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    expect(submissions_import_fence_fd(t, &imported) == VK_ERROR_INVALID_EXTERNAL_HANDLE && !state(a).signaled,
           "failed import leaves current native payload unchanged");
    mock.next_import = VK_SUCCESS;
    expect(submissions_import_fence_fd(t, &imported) == VK_SUCCESS && submissions_reset_fences(t, 1, &a) == VK_SUCCESS,
           "temporary import reset restores original permanent payload tracking");
    std::vector<AccelerationRecordingRef> tokens{token(1)};
    std::weak_ptr<AccelerationRecording> weak = tokens[0];
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS,
           "restored ordinary permanent payload can observe a later submission");
    tokens.clear();
    state(a).signaled = true;
    mock.allow_collect = true;
    expect(submissions_get_fence_status(t, a) == VK_SUCCESS && weak.expired(),
           "temporary import does not permanently poison original ordinary fence observer");
    submissions_destroy(t);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    reset_mock();
    t = tracker();
    a = user_fence(1);
    VkFenceGetWin32HandleInfoKHR get_win{VK_STRUCTURE_TYPE_FENCE_GET_WIN32_HANDLE_INFO_KHR, nullptr, a,
                                         VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
    HANDLE win_handle = nullptr;
    expect(submissions_get_fence_win32_handle(t, &get_win, &win_handle) == VK_SUCCESS && win_handle && !state(a).generation,
           "Win32 opaque export is reference transference and never copy-resets native payload");
    tokens = {token(2)};
    weak = tokens[0];
    expect(submissions_queue_submit(t, queue1, 1, &info, a, tokens) == VK_SUCCESS, "Win32 shared fence AS writer submits");
    tokens.clear();
    state(a).signaled = true;
    expect(submissions_get_fence_status(t, a) == VK_SUCCESS && !weak.expired(),
           "Win32 shared signal cannot prematurely retire AS work");
    VkImportFenceWin32HandleInfoKHR import_win{
        VK_STRUCTURE_TYPE_IMPORT_FENCE_WIN32_HANDLE_INFO_KHR, nullptr,    a,      VK_FENCE_IMPORT_TEMPORARY_BIT,
        VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT,       win_handle, nullptr};
    expect(submissions_import_fence_win32_handle(t, &import_win) == VK_SUCCESS && !weak.expired() && win_handle,
           "Win32 import freezes old generation without consuming caller handle or proving local completion");
    mock.allow_collect = true;
    expect(submissions_device_wait_idle(t) == VK_SUCCESS && weak.expired(),
           "Win32 unobservable generation safely retires at explicit device idle");
    submissions_destroy(t);
#endif
}
}  // namespace
namespace vel::omm {
void acceleration_collect_completed(AccelerationBackend*, const AccelerationRecordingRef& recording) {
    expect(mock.allow_collect, "collector is invoked only at proven touched-AS writer quiescence");
    expect(bool(recording), "recording token remains live during metadata ownership collection");
    mock.collected[mock.collect_count++] = value(recording.get());
}
bool acceleration_recordings_overlap(const AccelerationRecordingRef& a, const AccelerationRecordingRef& b) noexcept {
    if (!a || !b) return false;
    return reinterpret_cast<const TokenData*>(a.get())->as == reinterpret_cast<const TokenData*>(b.get())->as;
}
}  // namespace vel::omm
int main() {
    staggeredAndReuse();
    privateAndFailures();
    waitAndIdleScopes();
    multithreadProgress();
    independentASProgress();
    destroyedCompletedFence();
    timelineGenerations();
    timelineSubmitShapes();
    timelineFailuresAndProgress();
    timelineDifferenceLimit();
    syncFdGenerations();
    referenceAndImportGenerations();
    referenceMutationNeverCompletesWork();
    std::printf("OMM submission retirement: %u checks, %u failures\n", checks.load(), failures.load());
    return failures.load() ? 1 : 0;
}
