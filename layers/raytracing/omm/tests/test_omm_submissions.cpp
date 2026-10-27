// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "../omm_submissions.h"
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
};
struct Mock {
    Fence fences[128]{};
    Submit submits[128]{};
    unsigned submit_count = 0, creates = 0, destroys = 0, resets = 0, waits = 0, queue_waits = 0, device_waits = 0;
    unsigned next_private = 64, collect_count = 0;
    uintptr_t collected[128]{};
    bool allow_collect = false;
    VkResult next_submit = VK_SUCCESS, next_create = VK_SUCCESS, next_wait = VK_SUCCESS;
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
template <typename Info>
VkResult submit(VkQueue queue, uint32_t count, const Info* infos, VkFence fence, bool version2) {
    ++mock.submit_count;
    auto& entry = mock.submits[mock.submit_count - 1];
    entry = {queue, fence, infos, count, fence ? state(fence).generation : 0, version2};
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

#undef PROC
    return nullptr;
}
SubmissionTracker* tracker() {
    SubmissionTracker* out = nullptr;
    expect(submissions_create({device, proc, reinterpret_cast<AccelerationBackend*>(1), nullptr}, &out) == VK_SUCCESS,
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
    std::printf("OMM submission retirement: %u checks, %u failures\n", checks.load(), failures.load());
    return failures.load() ? 1 : 0;
}
