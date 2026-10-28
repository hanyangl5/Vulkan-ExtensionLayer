/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_memory_allocations.h"

struct memory_allocation {
    struct memory_allocation *next;
    VkDeviceMemory memory;
    bool freeing;
};
struct vkrt_memory_allocations {
    vkrt_mutex_t lock;
    struct memory_allocation *allocations;
    uint32_t occupied, limit;
    bool custom;
    VkAllocationCallbacks allocator;
};
static void *host_allocate(struct vkrt_memory_allocations *state, size_t size, size_t alignment) {
    return state->custom
               ? state->allocator.pfnAllocation(state->allocator.pUserData, size, alignment, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE)
               : malloc(size);
}
static void host_free(struct vkrt_memory_allocations *state, void *pointer) {
    if (state->custom)
        state->allocator.pfnFree(state->allocator.pUserData, pointer);
    else
        free(pointer);
}
VkResult vkrt_memory_allocations_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator) {
    if (dev->memory_allocations || !dev->virtual_rt) return VK_SUCCESS;
    if (!dev->instance_data || !dev->vtable.AllocateMemory || !dev->vtable.FreeMemory) return VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDeviceProperties properties = {0};
    if (dev->instance_data->vtable.GetPhysicalDeviceProperties) {
        dev->instance_data->vtable.GetPhysicalDeviceProperties(dev->physical_device, &properties);
    } else if (dev->instance_data->vtable.GetPhysicalDeviceProperties2) {
        VkPhysicalDeviceProperties2 two = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        dev->instance_data->vtable.GetPhysicalDeviceProperties2(dev->physical_device, &two);
        properties = two.properties;
    }
    if (!properties.limits.maxMemoryAllocationCount) return VK_ERROR_INITIALIZATION_FAILED;
    struct vkrt_memory_allocations seed = {.limit = properties.limits.maxMemoryAllocationCount};
    if (allocator) {
        seed.custom = true;
        seed.allocator = *allocator;
    } else if (dev->instance_data->has_allocator) {
        seed.custom = true;
        seed.allocator = dev->instance_data->allocator;
    }
    struct vkrt_memory_allocations *state = host_allocate(&seed, sizeof(*state), _Alignof(struct vkrt_memory_allocations));
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *state = seed;
#ifdef _WIN32
    vkrt_mutex_init(&state->lock);
#else
    if (pthread_mutex_init(&state->lock, NULL)) {
        host_free(state, state);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
#endif
    dev->memory_allocations = state;
    return VK_SUCCESS;
}
VkResult vkrt_memory_allocate_native(struct vkrt_device_data *dev, const VkMemoryAllocateInfo *info,
                                     const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    struct vkrt_memory_allocations *state = dev->memory_allocations;
    if (!state) {
        if (dev->virtual_rt) return VK_ERROR_INITIALIZATION_FAILED;
        return dev->vtable.AllocateMemory(dev->device, info, allocator, memory);
    }
    vkrt_mutex_lock(&state->lock);
    bool full = state->occupied >= state->limit;
    if (!full) ++state->occupied; /* Both live objects and native calls in flight. */
    vkrt_mutex_unlock(&state->lock);
    if (full) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    /* Acquire host storage before the native call: a successful native object
     * must never become untracked because bookkeeping allocation failed.
     * Allocation callbacks, like native calls, execute outside the lock. */
    struct memory_allocation *entry = host_allocate(state, sizeof(*entry), _Alignof(struct memory_allocation));
    if (!entry) {
        vkrt_mutex_lock(&state->lock);
        --state->occupied;
        vkrt_mutex_unlock(&state->lock);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkResult result = dev->vtable.AllocateMemory(dev->device, info, allocator, memory);
    vkrt_mutex_lock(&state->lock);
    if (result == VK_SUCCESS) {
        *entry = (struct memory_allocation){.next = state->allocations, .memory = *memory};
        state->allocations = entry;
    } else {
        --state->occupied;
    }
    vkrt_mutex_unlock(&state->lock);
    if (result != VK_SUCCESS) host_free(state, entry);
    return result;
}
void vkrt_memory_free_native(struct vkrt_device_data *dev, VkDeviceMemory memory, const VkAllocationCallbacks *allocator) {
    struct vkrt_memory_allocations *state = dev->memory_allocations;
    struct memory_allocation *entry = NULL;
    if (state && memory) {
        vkrt_mutex_lock(&state->lock);
        for (entry = state->allocations; entry; entry = entry->next)
            if (entry->memory == memory && !entry->freeing) {
                entry->freeing = true;
                break;
            }
        vkrt_mutex_unlock(&state->lock);
    }
    /* Unknown/native objects and NULL preserve native forwarding. An object
     * being freed still occupies its slot until this call has returned. */
    dev->vtable.FreeMemory(dev->device, memory, allocator);
    if (!entry) return;
    vkrt_mutex_lock(&state->lock);
    struct memory_allocation **link = &state->allocations;
    while (*link != entry) link = &(*link)->next;
    *link = entry->next;
    --state->occupied;
    vkrt_mutex_unlock(&state->lock);
    /* Remove by record identity, not handle: the native implementation can
     * reuse a numeric handle while the previous FreeMemory is returning. */
    host_free(state, entry);
}
void vkrt_memory_allocations_finish(struct vkrt_device_data *dev) {
    struct vkrt_memory_allocations *state = dev->memory_allocations;
    if (!state) return;
    dev->memory_allocations = NULL;
    /* Device destruction is externally synchronized. Native child destruction
     * belongs to its owners/DestroyDevice; only discard our host records here. */
    while (state->allocations) {
        struct memory_allocation *entry = state->allocations;
        state->allocations = entry->next;
        host_free(state, entry);
    }
#ifndef _WIN32
    pthread_mutex_destroy(&state->lock);
#endif
    host_free(state, state);
}
