/* SPDX-License-Identifier: MIT */
#include "vkrt_deferred.h"
#include "vkrt_layer.h"
#include <stdatomic.h>

#if !defined(_WIN32)
#include <sched.h>
#endif

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static atomic_uint failures;
#define CHECK(value)                                                       \
    do {                                                                   \
        if (!(value)) {                                                    \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value);            \
            atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed); \
        }                                                                  \
    } while (0)
#define H(type, value) ((type)(uintptr_t)(value))

static PFN_vkCreateDeferredOperationKHR create_operation;
static PFN_vkDestroyDeferredOperationKHR destroy_operation;
static PFN_vkGetDeferredOperationMaxConcurrencyKHR concurrency;
static PFN_vkGetDeferredOperationResultKHR result;
static PFN_vkDeferredOperationJoinKHR join;
static const VkDevice device_a = H(VkDevice, 0x100);
static const VkDevice device_b = H(VkDevice, 0x200);
static VkDeferredOperationKHR shared_operation;

struct allocation_tracker {
    VkSystemAllocationScope scope;
    atomic_uint allocations, live, frees;
    atomic_bool fail_next;
    bool reenter;
};
struct allocation_header {
    void *base;
    struct allocation_tracker *owner;
};
static VKAPI_ATTR void *VKAPI_CALL allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    struct allocation_tracker *tracker = user;
    CHECK(scope == tracker->scope);
    CHECK(alignment && !(alignment & (alignment - 1)));
    atomic_fetch_add_explicit(&tracker->allocations, 1, memory_order_relaxed);
    if (tracker->reenter) CHECK(result(device_a, shared_operation) == VK_SUCCESS);
    if (atomic_exchange_explicit(&tracker->fail_next, false, memory_order_relaxed)) return NULL;
    size_t actual_alignment = alignment < _Alignof(struct allocation_header) ? _Alignof(struct allocation_header) : alignment;
    void *base = malloc(size + actual_alignment - 1 + sizeof(struct allocation_header));
    if (!base) return NULL;
    uintptr_t address =
        ((uintptr_t)base + sizeof(struct allocation_header) + actual_alignment - 1) & ~(uintptr_t)(actual_alignment - 1);
    struct allocation_header *header = (struct allocation_header *)address - 1;
    header->base = base;
    header->owner = tracker;
    atomic_fetch_add_explicit(&tracker->live, 1, memory_order_relaxed);
    return (void *)address;
}
static VKAPI_ATTR void VKAPI_CALL deallocate(void *user, void *value) {
    struct allocation_tracker *tracker = user;
    CHECK(value != NULL);
    struct allocation_header *header = (struct allocation_header *)value - 1;
    CHECK(header->owner == tracker);
    if (tracker->reenter) CHECK(result(device_a, shared_operation) == VK_SUCCESS);
    CHECK(atomic_fetch_sub_explicit(&tracker->live, 1, memory_order_relaxed) > 0);
    atomic_fetch_add_explicit(&tracker->frees, 1, memory_order_relaxed);
    free(header->base);
}
static VkAllocationCallbacks callbacks(struct allocation_tracker *tracker) {
    return (VkAllocationCallbacks){.pUserData = tracker, .pfnAllocation = allocate, .pfnFree = deallocate};
}

enum { THREAD_COUNT = 12, ROUNDS = 192 };
static atomic_uint arrived;
static atomic_bool start;
static struct allocation_tracker operation_tracker = {.scope = VK_SYSTEM_ALLOCATION_SCOPE_OBJECT};

static void yield_thread(void) {
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}
static void worker_body(unsigned index) {
    atomic_fetch_add_explicit(&arrived, 1, memory_order_release);
    while (!atomic_load_explicit(&start, memory_order_acquire)) yield_thread();
    VkDevice owner = index & 1 ? device_a : device_b;
    VkDevice other = index & 1 ? device_b : device_a;
    VkAllocationCallbacks allocator = callbacks(&operation_tracker);
    for (unsigned round = 0; round < ROUNDS; ++round) {
        CHECK(result(device_a, shared_operation) == VK_SUCCESS);
        CHECK(join(device_a, shared_operation) == VK_SUCCESS);
        CHECK(concurrency(device_a, shared_operation) == 0);
        VkDeferredOperationKHR operation = VK_NULL_HANDLE;
        CHECK(create_operation(owner, &allocator, &operation) == VK_SUCCESS);
        CHECK(operation != VK_NULL_HANDLE);
        CHECK(result(owner, operation) == VK_SUCCESS);
        CHECK(join(owner, operation) == VK_SUCCESS);
        CHECK(result(other, operation) == VK_ERROR_UNKNOWN);
        /* A foreign destroy must not unlink or free the real owner's record. */
        destroy_operation(other, operation, &allocator);
        CHECK(result(owner, operation) == VK_SUCCESS);
        destroy_operation(owner, operation, &allocator);
    }
}
#if defined(_WIN32)
static DWORD WINAPI worker(void *argument) {
    worker_body((unsigned)(uintptr_t)argument);
    return 0;
}
#else
static void *worker(void *argument) {
    worker_body((unsigned)(uintptr_t)argument);
    return NULL;
}
#endif

int main(void) {
    create_operation = (PFN_vkCreateDeferredOperationKHR)vkrt_deferred_proc("vkCreateDeferredOperationKHR");
    destroy_operation = (PFN_vkDestroyDeferredOperationKHR)vkrt_deferred_proc("vkDestroyDeferredOperationKHR");
    concurrency = (PFN_vkGetDeferredOperationMaxConcurrencyKHR)vkrt_deferred_proc("vkGetDeferredOperationMaxConcurrencyKHR");
    result = (PFN_vkGetDeferredOperationResultKHR)vkrt_deferred_proc("vkGetDeferredOperationResultKHR");
    join = (PFN_vkDeferredOperationJoinKHR)vkrt_deferred_proc("vkDeferredOperationJoinKHR");
    CHECK(create_operation && destroy_operation && concurrency && result && join);
    CHECK(!vkrt_deferred_proc(NULL) && !vkrt_deferred_proc("vkGetDeferredOperationResult"));

    struct vkrt_device_data a = {.device = device_a}, b = {.device = device_b};
    struct allocation_tracker device_tracker = {.scope = VK_SYSTEM_ALLOCATION_SCOPE_DEVICE};
    VkAllocationCallbacks device_allocator = callbacks(&device_tracker);
    atomic_store(&device_tracker.fail_next, true);
    CHECK(vkrt_deferred_init(&a, &device_allocator) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(!a.deferred && atomic_load(&device_tracker.live) == 0);
    CHECK(vkrt_deferred_init(&a, &device_allocator) == VK_SUCCESS);
    CHECK(vkrt_deferred_init(&a, &device_allocator) == VK_SUCCESS);
    CHECK(atomic_load(&device_tracker.live) == 1);
    CHECK(vkrt_deferred_init(&b, NULL) == VK_SUCCESS);
    vkrt_map_object(VKRT_HKEY(device_a), &a);
    vkrt_map_object(VKRT_HKEY(device_b), &b);
    CHECK(vkrt_deferred_valid(&a, VK_NULL_HANDLE));
    CHECK(!vkrt_deferred_valid(NULL, VK_NULL_HANDLE));

    CHECK(create_operation(device_a, NULL, &shared_operation) == VK_SUCCESS);
    CHECK(result(device_a, shared_operation) == VK_SUCCESS);
    CHECK(join(device_a, shared_operation) == VK_SUCCESS);
    CHECK(concurrency(device_a, shared_operation) == 0);
    CHECK(vkrt_deferred_valid(&a, shared_operation));
    CHECK(!vkrt_deferred_valid(&b, shared_operation));
    CHECK(result(device_a, VK_NULL_HANDLE) == VK_ERROR_UNKNOWN);
    CHECK(join(device_a, VK_NULL_HANDLE) == VK_ERROR_UNKNOWN);
    destroy_operation(device_a, VK_NULL_HANDLE, NULL);

    VkAllocationCallbacks operation_allocator = callbacks(&operation_tracker);
    atomic_store(&operation_tracker.fail_next, true);
    VkDeferredOperationKHR operation = H(VkDeferredOperationKHR, 0xbad);
    CHECK(create_operation(device_a, &operation_allocator, &operation) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(operation == VK_NULL_HANDLE && atomic_load(&operation_tracker.live) == 0);
    CHECK(create_operation(H(VkDevice, 0x300), NULL, &operation) == VK_ERROR_UNKNOWN);
    CHECK(operation == VK_NULL_HANDLE);

    /* User callback storage can disappear after Create. Retain the creation
     * callbacks in the operation, rather than borrowing their stack address. */
    CHECK(create_operation(device_a, &operation_allocator, &operation) == VK_SUCCESS);
    VkAllocationCallbacks compatible_allocator = operation_allocator;
    memset(&operation_allocator, 0, sizeof(operation_allocator));
    destroy_operation(device_a, operation, &compatible_allocator);
    CHECK(!vkrt_deferred_valid(&a, operation));
    CHECK(atomic_load(&operation_tracker.live) == 0);

    /* Allocation/free callbacks query an existing operation. This catches
     * holding a device mutex across application-controlled callback calls. */
    operation_tracker.reenter = true;
#if defined(_WIN32)
    HANDLE threads[THREAD_COUNT];
    for (unsigned i = 0; i < THREAD_COUNT; ++i) {
        threads[i] = CreateThread(NULL, 0, worker, (void *)(uintptr_t)i, 0, NULL);
        if (!threads[i]) abort();
    }
#else
    pthread_t threads[THREAD_COUNT];
    for (unsigned i = 0; i < THREAD_COUNT; ++i)
        if (pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i) != 0) abort();
#endif
    while (atomic_load_explicit(&arrived, memory_order_acquire) != THREAD_COUNT) yield_thread();
    atomic_store_explicit(&start, true, memory_order_release);
    for (unsigned i = 0; i < THREAD_COUNT; ++i) {
#if defined(_WIN32)
        CHECK(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0);
        CHECK(CloseHandle(threads[i]));
#else
        CHECK(pthread_join(threads[i], NULL) == 0);
#endif
    }
    operation_tracker.reenter = false;
    CHECK(atomic_load(&operation_tracker.live) == 0);
    CHECK(atomic_load(&operation_tracker.frees) == THREAD_COUNT * ROUNDS + 1);
    destroy_operation(device_a, shared_operation, NULL);
    CHECK(!vkrt_deferred_valid(&a, shared_operation));

    /* Partial device initialization/teardown must reclaim its owned records
     * and use each creation allocator even when callbacks differ by object. */
    operation_allocator = callbacks(&operation_tracker);
    CHECK(create_operation(device_a, &operation_allocator, &operation) == VK_SUCCESS);
    CHECK(create_operation(device_b, NULL, &operation) == VK_SUCCESS);
    memset(&device_allocator, 0, sizeof(device_allocator));
    vkrt_deferred_finish(&a);
    vkrt_deferred_finish(&b);
    vkrt_deferred_finish(&a);
    CHECK(!a.deferred && !b.deferred);
    CHECK(atomic_load(&device_tracker.live) == 0);
    CHECK(atomic_load(&device_tracker.frees) == 1);
    CHECK(atomic_load(&operation_tracker.live) == 0);
    CHECK(atomic_load(&operation_tracker.allocations) == atomic_load(&operation_tracker.frees) + 1);
    vkrt_unmap_object(VKRT_HKEY(device_a));
    vkrt_unmap_object(VKRT_HKEY(device_b));
    vkrt_map_destroy(&vkrt_global.objects);
    unsigned count = atomic_load(&failures);
    if (!count)
        printf("deferred operation allocator/ownership and %u concurrent join/create/destroy rounds PASS\n", THREAD_COUNT * ROUNDS);
    return count ? 1 : 0;
}
