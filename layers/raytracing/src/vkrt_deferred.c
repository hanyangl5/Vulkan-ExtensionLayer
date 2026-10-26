/* SPDX-License-Identifier: MIT
 * Synchronous VK_KHR_deferred_host_operations. No command is deferred, and
 * these private operation handles are never passed to the downstream ICD.
 */
#include "vkrt_deferred.h"
#include "vkrt_layer.h"

struct vkrt_deferred_operation {
    struct vkrt_deferred_operation *next;
    bool has_allocator;
    VkAllocationCallbacks allocator;
};

struct vkrt_deferred_state {
    vkrt_mutex_t lock;
    struct vkrt_deferred_operation *operations;
    bool has_allocator;
    VkAllocationCallbacks allocator;
};

static void *allocate(const VkAllocationCallbacks *allocator, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    void *value = allocator ? allocator->pfnAllocation(allocator->pUserData, size, alignment, scope) : malloc(size);
    if (value) memset(value, 0, size);
    return value;
}

static void deallocate(const VkAllocationCallbacks *allocator, void *value) {
    if (allocator)
        allocator->pfnFree(allocator->pUserData, value);
    else
        free(value);
}

static void operation_free(struct vkrt_deferred_operation *operation) {
    /* Keep the callback record alive across its own pfnFree call. */
    bool has_allocator = operation->has_allocator;
    VkAllocationCallbacks allocator = operation->allocator;
    deallocate(has_allocator ? &allocator : NULL, operation);
}

VkResult vkrt_deferred_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator) {
    if (dev->deferred) return VK_SUCCESS;
    struct vkrt_deferred_state *state =
        allocate(allocator, sizeof(*state), _Alignof(struct vkrt_deferred_state), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
#if defined(_WIN32)
    vkrt_mutex_init(&state->lock);
#else
    if (pthread_mutex_init(&state->lock, NULL) != 0) {
        deallocate(allocator, state);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
#endif
    if (allocator) {
        state->has_allocator = true;
        state->allocator = *allocator;
    }
    dev->deferred = state;
    return VK_SUCCESS;
}

void vkrt_deferred_finish(struct vkrt_device_data *dev) {
    struct vkrt_deferred_state *state = dev->deferred;
    if (!state) return;
    /* Device destruction is externally synchronized. No operation or API
     * lookup may remain active across teardown of its owning device. */
    vkrt_mutex_lock(&state->lock);
    struct vkrt_deferred_operation *operation = state->operations;
    state->operations = NULL;
    vkrt_mutex_unlock(&state->lock);
    while (operation) {
        struct vkrt_deferred_operation *next = operation->next;
        operation_free(operation);
        operation = next;
    }
#if !defined(_WIN32)
    pthread_mutex_destroy(&state->lock);
#endif
    dev->deferred = NULL;
    bool has_allocator = state->has_allocator;
    VkAllocationCallbacks allocator = state->allocator;
    deallocate(has_allocator ? &allocator : NULL, state);
}

/* The state lock is held whenever a list record is read. We compare tokens
 * before dereferencing a record, so foreign-device or removed handles cannot
 * be mistaken for an owned object. No record survives unlocking. */
static struct vkrt_deferred_operation *find_locked(struct vkrt_deferred_state *state, VkDeferredOperationKHR handle) {
    for (struct vkrt_deferred_operation *operation = state->operations; operation; operation = operation->next)
        if ((VkDeferredOperationKHR)(uintptr_t)operation == handle) return operation;
    return NULL;
}

bool vkrt_deferred_valid(struct vkrt_device_data *dev, VkDeferredOperationKHR operation) {
    if (!dev || !dev->deferred) return false;
    if (operation == VK_NULL_HANDLE) return true;
    struct vkrt_deferred_state *state = dev->deferred;
    vkrt_mutex_lock(&state->lock);
    bool valid = find_locked(state, operation) != NULL;
    vkrt_mutex_unlock(&state->lock);
    return valid;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDeferredOperationKHR(VkDevice device, const VkAllocationCallbacks *allocator,
                                                                 VkDeferredOperationKHR *out) {
    if (!out) return VK_ERROR_UNKNOWN;
    *out = VK_NULL_HANDLE;
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !dev->deferred) return VK_ERROR_UNKNOWN;
    struct vkrt_deferred_operation *operation =
        allocate(allocator, sizeof(*operation), _Alignof(struct vkrt_deferred_operation), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!operation) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (allocator) {
        operation->has_allocator = true;
        operation->allocator = *allocator;
    }
    struct vkrt_deferred_state *state = dev->deferred;
    vkrt_mutex_lock(&state->lock);
    operation->next = state->operations;
    state->operations = operation;
    vkrt_mutex_unlock(&state->lock);
    *out = (VkDeferredOperationKHR)(uintptr_t)operation;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDeferredOperationKHR(VkDevice device, VkDeferredOperationKHR handle,
                                                              const VkAllocationCallbacks *allocator) {
    (void)allocator; /* The valid caller provides callbacks compatible with creation. */
    if (handle == VK_NULL_HANDLE) return;
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !dev->deferred) return;
    struct vkrt_deferred_state *state = dev->deferred;
    struct vkrt_deferred_operation *removed = NULL;
    vkrt_mutex_lock(&state->lock);
    for (struct vkrt_deferred_operation **link = &state->operations; *link; link = &(*link)->next) {
        if ((VkDeferredOperationKHR)(uintptr_t)*link != handle) continue;
        removed = *link;
        *link = removed->next;
        break;
    }
    vkrt_mutex_unlock(&state->lock);
    if (removed) operation_free(removed);
}

static VKAPI_ATTR uint32_t VKAPI_CALL GetDeferredOperationMaxConcurrencyKHR(VkDevice device, VkDeferredOperationKHR operation) {
    /* No useful work exists, both for a fresh and a synchronously used object.
     * Check ownership even though zero is also a defensive invalid-handle value. */
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    (void)vkrt_deferred_valid(dev, operation);
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDeferredOperationResultKHR(VkDevice device, VkDeferredOperationKHR operation) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    return operation != VK_NULL_HANDLE && vkrt_deferred_valid(dev, operation) ? VK_SUCCESS : VK_ERROR_UNKNOWN;
}

static VKAPI_ATTR VkResult VKAPI_CALL DeferredOperationJoinKHR(VkDevice device, VkDeferredOperationKHR operation) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    return operation != VK_NULL_HANDLE && vkrt_deferred_valid(dev, operation) ? VK_SUCCESS : VK_ERROR_UNKNOWN;
}

PFN_vkVoidFunction vkrt_deferred_proc(const char *name) {
    if (!name) return NULL;
#define ENTRY(command) \
    if (!strcmp(name, "vk" #command)) return (PFN_vkVoidFunction)command
    ENTRY(CreateDeferredOperationKHR);
    ENTRY(DestroyDeferredOperationKHR);
    ENTRY(GetDeferredOperationMaxConcurrencyKHR);
    ENTRY(GetDeferredOperationResultKHR);
    ENTRY(DeferredOperationJoinKHR);
#undef ENTRY
    return NULL;
}
