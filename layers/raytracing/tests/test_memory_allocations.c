/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_memory_allocations.h"
#include <stdatomic.h>
#include <time.h>

/* Independent native admission guard. It counts actual successful native
 * objects and deliberately stalls calls at the native boundary. Small caps
 * are unit injections, never advertised Vulkan capabilities. */
#define H(type, value) ((type)(uintptr_t)(value))
static _Atomic unsigned failures;
#define CHECK(test)                                            \
    do {                                                       \
        if (!(test)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #test); \
            atomic_fetch_add(&failures, 1);                    \
        }                                                      \
    } while (0)
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static struct vkrt_device_data dev;
static struct vkrt_instance_data instance;
static VkAllocationCallbacks native_allocator, owner_allocator;
static VkMemoryAllocateFlagsInfo address_flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                                  .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
static VkMemoryAllocateInfo info = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &address_flags, .allocationSize = 16, .memoryTypeIndex = 0};
static uint32_t cap;
static vkrt_mutex_t native_lock;
static VkDeviceMemory native_objects[32];
static unsigned native_live, native_peak, next_id, other_live;
static _Atomic unsigned native_calls, free_calls, allocation_entered, thread_done, owner_live, owner_calls;
static _Atomic bool hold_allocate, release_allocate, hold_free, release_free, free_entered;
static _Atomic bool fail_host;
static VkResult native_result;
static bool reenter_free;
static VkDeviceMemory reused_handle, reentered_memory;

VkResult vkrt_as_registry_buffer_create(struct vkrt_device_data *d, VkBuffer buffer, VkDeviceSize size) {
    (void)d;
    (void)buffer;
    (void)size;
    return VK_SUCCESS;
}
void vkrt_as_registry_buffer_destroy(struct vkrt_device_data *d, VkBuffer buffer) {
    (void)d;
    (void)buffer;
}
static void pause_thread(void) {
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec duration = {.tv_nsec = 1000000};
    nanosleep(&duration, NULL);
#endif
}
static uint64_t milliseconds(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}
static void wait_flag(_Atomic bool *flag) {
    uint64_t deadline = milliseconds() + 5000;
    while (!atomic_load(flag) && milliseconds() < deadline) pause_thread();
    CHECK(atomic_load(flag));
}
static VKAPI_ATTR void *VKAPI_CALL owner_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    CHECK(user == &owner_live && alignment <= 16 && scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    atomic_fetch_add(&owner_calls, 1);
    if (atomic_load(&fail_host)) return NULL;
    void *p = malloc(size);
    if (p) atomic_fetch_add(&owner_live, 1);
    return p;
}
static VKAPI_ATTR void VKAPI_CALL owner_free(void *user, void *p) {
    CHECK(user == &owner_live && p);
    atomic_fetch_sub(&owner_live, 1);
    free(p);
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *out) {
    CHECK(physical == H(VkPhysicalDevice, 2));
    memset(out, 0, sizeof(*out));
    out->limits.maxMemoryAllocationCount = cap;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_allocate(VkDevice device, const VkMemoryAllocateInfo *allocation,
                                                      const VkAllocationCallbacks *allocator, VkDeviceMemory *out) {
    CHECK((device == dev.device || device == H(VkDevice, 3)) && allocation == &info && allocator == &native_allocator);
    atomic_fetch_add(&native_calls, 1);
    atomic_fetch_add(&allocation_entered, 1);
    if (atomic_load(&hold_allocate)) wait_flag(&release_allocate);
    if (native_result != VK_SUCCESS) return native_result;
    if (device == H(VkDevice, 3)) {
        CHECK(other_live < cap);
        ++other_live;
        *out = reused_handle;
        return VK_SUCCESS;
    }
    vkrt_mutex_lock(&native_lock);
    /* This is the external oracle for 04101; it is not the ledger counter. */
    CHECK(native_live < cap);
    if (native_live >= cap) {
        vkrt_mutex_unlock(&native_lock);
        return VK_ERROR_TOO_MANY_OBJECTS;
    }
    unsigned slot = 0;
    while (slot < VKRT_ARRAY_SIZE(native_objects) && native_objects[slot]) ++slot;
    CHECK(slot < VKRT_ARRAY_SIZE(native_objects));
    *out = reused_handle ? reused_handle : H(VkDeviceMemory, UINT64_C(0x123400000000) + ++next_id);
    native_objects[slot] = *out;
    ++native_live;
    if (native_live > native_peak) native_peak = native_live;
    vkrt_mutex_unlock(&native_lock);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL native_free(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator) {
    CHECK((device == dev.device || device == H(VkDevice, 3)) && allocator == &native_allocator);
    atomic_fetch_add(&free_calls, 1);
    if (device == H(VkDevice, 3)) {
        CHECK(other_live == 1);
        --other_live;
        return;
    }
    vkrt_mutex_lock(&native_lock);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(native_objects); ++i)
        if (memory && native_objects[i] == memory) {
            native_objects[i] = VK_NULL_HANDLE;
            --native_live;
            break;
        }
    vkrt_mutex_unlock(&native_lock);
    atomic_store(&free_entered, true);
    if (atomic_load(&hold_free)) wait_flag(&release_free);
    if (reenter_free) {
        reenter_free = false;
        reused_handle = memory;
        CHECK(vkrt_memory_allocate_native(&dev, &info, &native_allocator, &reentered_memory) == VK_SUCCESS);
        CHECK(reentered_memory == memory);
        reused_handle = VK_NULL_HANDLE;
    }
}
static void setup(uint32_t limit) {
    CHECK(!dev.memory_allocations && !native_live && !other_live && !atomic_load(&owner_live));
    memset(&dev, 0, sizeof(dev));
    memset(&instance, 0, sizeof(instance));
    memset(native_objects, 0, sizeof(native_objects));
    atomic_store(&native_calls, 0);
    atomic_store(&free_calls, 0);
    atomic_store(&allocation_entered, 0);
    atomic_store(&thread_done, 0);
    atomic_store(&hold_allocate, false);
    atomic_store(&release_allocate, false);
    atomic_store(&hold_free, false);
    atomic_store(&release_free, false);
    atomic_store(&free_entered, false);
    atomic_store(&fail_host, false);
    native_result = VK_SUCCESS;
    reenter_free = false;
    reused_handle = reentered_memory = VK_NULL_HANDLE;
    cap = limit;
    native_peak = next_id = 0;
    owner_allocator = (VkAllocationCallbacks){.pUserData = &owner_live, .pfnAllocation = owner_allocate, .pfnFree = owner_free};
    /* Distinct callback-struct addresses with compatible callback functions.
     * Native forwarding must preserve the caller's pointer, not our copy. */
    native_allocator = owner_allocator;
    instance.vtable.GetPhysicalDeviceProperties = properties;
    dev = (struct vkrt_device_data){.device = H(VkDevice, 1),
                                    .physical_device = H(VkPhysicalDevice, 2),
                                    .instance_data = &instance,
                                    .virtual_rt = true,
                                    .vtable.AllocateMemory = native_allocate,
                                    .vtable.FreeMemory = native_free};
    vkrt_map_object(VKRT_HKEY(dev.device), &dev);
}
static void initialize(void) { CHECK(vkrt_memory_allocations_init(&dev, &owner_allocator) == VK_SUCCESS); }
static void finish(void) {
    CHECK(native_live == 0);
    vkrt_memory_allocations_finish(&dev);
    CHECK(!dev.memory_allocations && !atomic_load(&owner_live));
}
static void public_limit_and_failures(void) {
    setup(2);
    initialize();
    VkDeviceMemory a = VK_NULL_HANDLE, b = VK_NULL_HANDLE, c = VK_NULL_HANDLE;
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &a) == VK_SUCCESS);
    CHECK(vkrt_memory_allocate_native(&dev, &info, &native_allocator, &b) == VK_SUCCESS);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &c) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(atomic_load(&native_calls) == 2 && native_peak == 2);
    atomic_store(&fail_host, true);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &c) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    atomic_store(&fail_host, false);
    vkrt_FreeMemory(dev.device, H(VkDeviceMemory, 0x777), &native_allocator);
    vkrt_FreeMemory(dev.device, VK_NULL_HANDLE, &native_allocator);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &c) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    vkrt_FreeMemory(dev.device, a, &native_allocator);
    const VkResult errors[] = {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(errors); ++i) {
        native_result = errors[i];
        CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &c) == errors[i]);
    }
    native_result = VK_SUCCESS;
    unsigned calls = atomic_load(&native_calls), live = atomic_load(&owner_live);
    atomic_store(&fail_host, true);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &c) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(atomic_load(&native_calls) == calls && atomic_load(&owner_live) == live);
    atomic_store(&fail_host, false);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &c) == VK_SUCCESS);
    vkrt_FreeMemory(dev.device, b, &native_allocator);
    vkrt_FreeMemory(dev.device, c, &native_allocator);
    finish();

    setup(0);
    CHECK(vkrt_memory_allocations_init(&dev, &owner_allocator) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(!dev.memory_allocations && atomic_load(&native_calls) == 0);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &a) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(atomic_load(&native_calls) == 0);
    setup(1);
    atomic_store(&fail_host, true);
    CHECK(vkrt_memory_allocations_init(&dev, &owner_allocator) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(!dev.memory_allocations && !atomic_load(&owner_live));
    atomic_store(&fail_host, false);
    dev.virtual_rt = false;
    CHECK(vkrt_memory_allocations_init(&dev, &owner_allocator) == VK_SUCCESS && !dev.memory_allocations);
    info.pNext = NULL;
    unsigned owner_before = atomic_load(&owner_calls);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &a) == VK_SUCCESS);
    vkrt_FreeMemory(dev.device, a, &native_allocator);
    CHECK(atomic_load(&native_calls) == 1 && atomic_load(&free_calls) == 1 && info.pNext == NULL);
    CHECK(atomic_load(&owner_calls) == owner_before);
    info.pNext = &address_flags;
    finish();
}
static void device_ownership_and_teardown(void) {
    setup(1);
    initialize();
    VkDeviceMemory first = VK_NULL_HANDLE, second = VK_NULL_HANDLE;
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &first) == VK_SUCCESS);
    struct vkrt_device_data other = dev;
    other.device = H(VkDevice, 3);
    other.memory_allocations = NULL;
    CHECK(vkrt_memory_allocations_init(&other, &owner_allocator) == VK_SUCCESS);
    reused_handle = first;
    CHECK(vkrt_memory_allocate_native(&other, &info, &native_allocator, &second) == VK_SUCCESS);
    CHECK(second == first);
    reused_handle = VK_NULL_HANDLE;
    /* Distinct devices can use the same numeric handle. Freeing the other
     * device's object must not credit this device's allocation count. */
    vkrt_memory_free_native(&other, second, &native_allocator);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &second) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    vkrt_memory_allocations_finish(&other);
    vkrt_FreeMemory(dev.device, first, &native_allocator);
    finish();

    setup(2);
    instance.has_allocator = true;
    instance.allocator = owner_allocator;
    CHECK(vkrt_memory_allocations_init(&dev, NULL) == VK_SUCCESS);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &first) == VK_SUCCESS);
    unsigned free_before = atomic_load(&free_calls);
    vkrt_memory_allocations_finish(&dev);
    CHECK(!dev.memory_allocations && !atomic_load(&owner_live) && atomic_load(&free_calls) == free_before);
    /* Native DestroyDevice owns any remaining native children. The ledger
     * destructor only frees bookkeeping and never synthesizes a FreeMemory. */
    native_free(dev.device, first, &native_allocator);
    CHECK(native_live == 0);
}
struct request {
    VkDeviceMemory memory;
    VkResult result;
    bool freeing;
};
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID argument) {
#else
static void *worker(void *argument) {
#endif
    struct request *request = argument;
    if (request->freeing)
        vkrt_FreeMemory(dev.device, request->memory, &native_allocator);
    else
        request->result = vkrt_AllocateMemory(dev.device, &info, &native_allocator, &request->memory);
    atomic_fetch_add(&thread_done, 1);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
#ifdef _WIN32
typedef HANDLE test_thread;
static test_thread start(struct request *request) {
    HANDLE thread = CreateThread(NULL, 0, worker, request, 0, NULL);
    CHECK(thread != NULL);
    return thread;
}
static void join(test_thread thread) {
    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
}
#else
typedef pthread_t test_thread;
static test_thread start(struct request *request) {
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, worker, request));
    return thread;
}
static void join(test_thread thread) { CHECK(!pthread_join(thread, NULL)); }
#endif
static void concurrent_reservations(void) {
    setup(3);
    initialize();
    atomic_store(&hold_allocate, true);
    struct request requests[8] = {0};
    test_thread threads[8];
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(threads); ++i) threads[i] = start(&requests[i]);
    uint64_t deadline = milliseconds() + 2000;
    while ((atomic_load(&allocation_entered) < 3 || atomic_load(&thread_done) < 5) && milliseconds() < deadline) pause_thread();
    CHECK(atomic_load(&allocation_entered) == 3 && atomic_load(&thread_done) == 5);
    atomic_store(&release_allocate, true);
    unsigned success = 0;
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(threads); ++i) join(threads[i]);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(threads); ++i) {
        if (requests[i].result == VK_SUCCESS) {
            ++success;
            vkrt_FreeMemory(dev.device, requests[i].memory, &native_allocator);
        } else
            CHECK(requests[i].result == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    }
    CHECK(success == 3 && native_peak == 3 && atomic_load(&native_calls) == 3);
    finish();
}
static void free_credit_and_reuse(void) {
    setup(1);
    initialize();
    struct request request = {.freeing = true};
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &request.memory) == VK_SUCCESS);
    atomic_store(&hold_free, true);
    test_thread thread = start(&request);
    wait_flag(&free_entered);
    VkDeviceMemory b = VK_NULL_HANDLE;
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &b) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(atomic_load(&native_calls) == 1);
    atomic_store(&release_free, true);
    join(thread);
    atomic_store(&hold_free, false);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &b) == VK_SUCCESS);
    vkrt_FreeMemory(dev.device, b, &native_allocator);
    finish();

    setup(2);
    initialize();
    VkDeviceMemory a = VK_NULL_HANDLE, c = VK_NULL_HANDLE;
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &a) == VK_SUCCESS);
    reenter_free = true;
    vkrt_FreeMemory(dev.device, a, &native_allocator);
    CHECK(reentered_memory == a && native_live == 1);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &b) == VK_SUCCESS);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &c) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    vkrt_FreeMemory(dev.device, reentered_memory, &native_allocator);
    CHECK(vkrt_AllocateMemory(dev.device, &info, &native_allocator, &c) == VK_SUCCESS);
    vkrt_FreeMemory(dev.device, b, &native_allocator);
    vkrt_FreeMemory(dev.device, c, &native_allocator);
    finish();
}
int main(void) {
    vkrt_mutex_init(&native_lock);
    public_limit_and_failures();
    device_ownership_and_teardown();
    concurrent_reservations();
    free_credit_and_reuse();
    printf("Memory allocation ledger: %s (%u failures; native guard/reservations/free credit/reuse/callbacks)\n",
           atomic_load(&failures) ? "FAIL" : "PASS", atomic_load(&failures));
    return atomic_load(&failures) ? 1 : 0;
}
