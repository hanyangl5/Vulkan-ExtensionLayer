/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "vkrt_execution_replay.h"
#include "vkrt_layer.h"
#include "vkrt_query_pool.h"
#include "vkrt_rtp.h"
#include <stddef.h>
#ifndef _WIN32
#include <errno.h>
#include <time.h>
#endif
#ifndef VKRT_EXECUTION_SETTLED_LIMIT
#define VKRT_EXECUTION_SETTLED_LIMIT UINT64_MAX
#endif
_Static_assert(VKRT_EXECUTION_SETTLED_LIMIT > 0, "settled clock must have a nonzero range");

struct execution_pool {
    struct execution_pool *next;
    VkCommandPool pool;
    uint32_t family;
    VkCommandPoolCreateFlags flags;
};
struct execution_command {
    struct execution_command *next;
    struct execution_pool *pool;
    VkCommandBuffer source, command;
};
struct execution_frame {
    const struct execution_frame *previous;
    VkCommandBuffer source;
};
struct vkrt_execution_batch {
    struct vkrt_execution_batch *next;
    struct vkrt_execution_device *owner;
    struct execution_pool *pools;
    struct execution_command *commands, *last_command;
    const struct execution_frame *recording;
    size_t command_count;
    VkQueue queue;
    VkFence completion;
    VkResult status;
    bool submitted, ready, observer_associated, quarantined, stamp_valid, cutoff_valid;
    uint64_t settled_stamp, observer_cutoff;
};
struct vkrt_execution_device {
    struct vkrt_device_data *dev;
    struct vkrt_execution_batch *batches;
    uint64_t settled_clock;
    vkrt_mutex_t lock;
    bool has_allocator;
    VkAllocationCallbacks allocator;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkQueueSubmit queue_submit;
    PFN_vkGetFenceStatus fence_status;
    PFN_vkDeviceWaitIdle device_idle;
    PFN_vkCreateCommandPool create_pool;
    PFN_vkDestroyCommandPool destroy_pool;
    PFN_vkAllocateCommandBuffers allocate_commands;
    PFN_vkFreeCommandBuffers free_commands;
    PFN_vkBeginCommandBuffer begin_command;
    PFN_vkEndCommandBuffer end_command;
};

static const VkAllocationCallbacks *callbacks(const struct vkrt_execution_device *state) {
    return state->has_allocator ? &state->allocator : NULL;
}
static void *allocate(struct vkrt_execution_device *state, size_t size) {
    void *memory = state->has_allocator ? state->allocator.pfnAllocation(state->allocator.pUserData, size, _Alignof(max_align_t),
                                                                         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE)
                                        : malloc(size);
    if (memory) memset(memory, 0, size);
    return memory;
}
static void release(struct vkrt_execution_device *state, void *memory) {
    if (!memory) return;
    if (state->has_allocator)
        state->allocator.pfnFree(state->allocator.pUserData, memory);
    else
        free(memory);
}
static VkResult poison(struct vkrt_execution_batch *batch, VkResult error) {
    if (batch->status == VK_SUCCESS) batch->status = error;
    return batch->status;
}
static void destroy_batch(struct vkrt_execution_batch *batch) {
    struct vkrt_execution_device *state = batch->owner;
    while (batch->commands) {
        struct execution_command *record = batch->commands;
        batch->commands = record->next;
        if (record->command) state->free_commands(state->dev->device, record->pool->pool, 1, &record->command);
        release(state, record);
    }
    while (batch->pools) {
        struct execution_pool *pool = batch->pools;
        batch->pools = pool->next;
        if (pool->pool) state->destroy_pool(state->dev->device, pool->pool, callbacks(state));
        release(state, pool);
    }
    if (batch->completion) state->destroy_fence(state->dev->device, batch->completion, callbacks(state));
    release(state, batch);
}
static void unlink_batch(struct vkrt_execution_device *state, struct vkrt_execution_batch *batch) {
    struct vkrt_execution_batch **link = &state->batches;
    while (*link && *link != batch) link = &(*link)->next;
    if (*link) *link = batch->next;
}

VkResult vkrt_execution_device_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator) {
    if (!dev || !dev->vtable.GetDeviceProcAddr) return VK_ERROR_INITIALIZATION_FAILED;
    if (dev->executions) return VK_SUCCESS;
    struct vkrt_execution_device initial = {.dev = dev, .has_allocator = allocator != NULL};
    if (allocator) initial.allocator = *allocator;
    struct vkrt_execution_device *state = allocate(&initial, sizeof(*state));
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *state = initial;
#define NATIVE(field, type, name) state->field = (type)dev->vtable.GetDeviceProcAddr(dev->device, name)
    NATIVE(create_fence, PFN_vkCreateFence, "vkCreateFence");
    NATIVE(destroy_fence, PFN_vkDestroyFence, "vkDestroyFence");
    NATIVE(queue_submit, PFN_vkQueueSubmit, "vkQueueSubmit");
    NATIVE(fence_status, PFN_vkGetFenceStatus, "vkGetFenceStatus");
    NATIVE(device_idle, PFN_vkDeviceWaitIdle, "vkDeviceWaitIdle");
#undef NATIVE
    state->create_pool = (PFN_vkCreateCommandPool)vkrt_query_pool_proc("vkCreateCommandPool");
    state->destroy_pool = (PFN_vkDestroyCommandPool)vkrt_command_proc("vkDestroyCommandPool");
    state->allocate_commands = (PFN_vkAllocateCommandBuffers)vkrt_command_proc("vkAllocateCommandBuffers");
    state->free_commands = (PFN_vkFreeCommandBuffers)vkrt_command_proc("vkFreeCommandBuffers");
    state->begin_command = (PFN_vkBeginCommandBuffer)vkrt_command_proc("vkBeginCommandBuffer");
    state->end_command = (PFN_vkEndCommandBuffer)vkrt_command_proc("vkEndCommandBuffer");
    if (!state->create_fence || !state->destroy_fence || !state->queue_submit || !state->fence_status || !state->device_idle ||
        !state->create_pool || !state->destroy_pool || !state->allocate_commands || !state->free_commands ||
        !state->begin_command || !state->end_command) {
        release(state, state);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    vkrt_mutex_init(&state->lock);
    dev->executions = state;
    return VK_SUCCESS;
}
void vkrt_execution_device_finish(struct vkrt_device_data *dev) {
    struct vkrt_execution_device *state = dev ? dev->executions : NULL;
    if (!state) return;
    /* The application may only have waited its original caller fence. Our
     * separate observation can still be pending, so teardown owns the drain.
     * No concurrent helper/native submission calls are permitted here. */
    bool submitted = false;
    for (const struct vkrt_execution_batch *batch = state->batches; batch; batch = batch->next) submitted |= batch->submitted;
    if (submitted) {
        VkResult previous = VK_SUCCESS;
        for (;;) {
            VkResult result = state->device_idle(dev->device);
            if (result == VK_SUCCESS) break;
            if (result != previous) {
                VKRT_LOG_WARN("Execution replay teardown native idle returned %d; retaining objects until completion or loss",
                              (int)result);
                previous = result;
            }
            if (result == VK_ERROR_DEVICE_LOST) break;
#ifdef _WIN32
            Sleep(1);
#else
            struct timespec pause = {0, 1000000};
            while (nanosleep(&pause, &pause) && errno == EINTR) {
            }
#endif
        }
    }
    while (state->batches) {
        struct vkrt_execution_batch *batch = state->batches;
        state->batches = batch->next;
        destroy_batch(batch);
    }
    dev->executions = NULL;
#if !defined(_WIN32)
    pthread_mutex_destroy(&state->lock);
#endif
    release(state, state);
}
VkResult vkrt_execution_batch_create(struct vkrt_device_data *dev, VkQueue queue, struct vkrt_execution_batch **out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = NULL;
    struct vkrt_execution_device *state = dev ? dev->executions : NULL;
    if (!state || !queue) return VK_ERROR_INITIALIZATION_FAILED;
    vkrt_mutex_lock(&state->lock);
    bool exhausted = state->settled_clock == VKRT_EXECUTION_SETTLED_LIMIT;
    vkrt_mutex_unlock(&state->lock);
    if (exhausted) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct vkrt_execution_batch *batch = allocate(state, sizeof(*batch));
    if (!batch) return VK_ERROR_OUT_OF_HOST_MEMORY;
    batch->owner = state;
    batch->queue = queue;
    VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkResult result = state->create_fence(dev->device, &info, callbacks(state), &batch->completion);
    if (result != VK_SUCCESS) {
        release(state, batch);
        return result;
    }
    vkrt_mutex_lock(&state->lock);
    if (state->settled_clock == VKRT_EXECUTION_SETTLED_LIMIT) {
        vkrt_mutex_unlock(&state->lock);
        destroy_batch(batch);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    batch->next = state->batches;
    state->batches = batch;
    vkrt_mutex_unlock(&state->lock);
    *out = batch;
    return VK_SUCCESS;
}
static VkResult find_pool(struct vkrt_execution_batch *batch, const struct vkrt_cmd_buffer_data *source,
                          struct execution_pool **out) {
    struct vkrt_execution_device *state = batch->owner;
    uint32_t family;
    VkCommandPoolCreateFlags flags;
    if (!vkrt_query_command_pool_info(state->dev, source->pool, &family, &flags)) return VK_ERROR_FEATURE_NOT_PRESENT;
    for (struct execution_pool *pool = batch->pools; pool; pool = pool->next) {
        if (pool->family == family && pool->flags == flags) {
            *out = pool;
            return VK_SUCCESS;
        }
    }
    struct execution_pool *pool = allocate(state, sizeof(*pool));
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pool = (struct execution_pool){.family = family, .flags = flags};
    VkCommandPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = flags, .queueFamilyIndex = family};
    VkResult result = state->create_pool(state->dev->device, &info, callbacks(state), &pool->pool);
    if (result != VK_SUCCESS) {
        release(state, pool);
        return result;
    }
    pool->next = batch->pools;
    batch->pools = pool;
    *out = pool;
    return VK_SUCCESS;
}
static PFN_vkVoidFunction resolve(void *user, const char *name) {
    struct vkrt_execution_batch *batch = user;
    return vkrt_device_direct_proc(batch->owner->dev, name);
}
static VkResult clone_command(struct vkrt_execution_batch *batch, VkCommandBuffer command, VkCommandBuffer *target, bool force);
static VkResult remap_child(void *user, VkCommandBuffer source, VkCommandBuffer *target) {
    /* Query-free secondaries can still own mutable RTP frame packets. A replayed
     * parent is self-contained, with fresh state for every child occurrence. */
    return clone_command(user, source, target, true);
}
static VkResult clone_command(struct vkrt_execution_batch *batch, VkCommandBuffer command, VkCommandBuffer *target, bool force) {
    if (!batch || !target) return VK_ERROR_INITIALIZATION_FAILED;
    *target = VK_NULL_HANDLE;
    if (batch->submitted) return VK_ERROR_INITIALIZATION_FAILED;
    if (batch->status != VK_SUCCESS) return batch->status;
    struct vkrt_execution_device *state = batch->owner;
    struct vkrt_cmd_buffer_data *source = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (!source || source->device_data != state->dev) return poison(batch, VK_ERROR_INITIALIZATION_FAILED);
    if (force && source->level != VK_COMMAND_BUFFER_LEVEL_SECONDARY) return poison(batch, VK_ERROR_INITIALIZATION_FAILED);
    if (!force && !vkrt_query_command_requires_replay(source)) {
        *target = command;
        return VK_SUCCESS;
    }
    for (const struct execution_frame *p = batch->recording; p; p = p->previous)
        if (p->source == command) return poison(batch, VK_ERROR_FEATURE_NOT_PRESENT);
    if (source->record_result != VK_SUCCESS) return poison(batch, source->record_result);
    if (!source->replay.journal) return poison(batch, VK_ERROR_FEATURE_NOT_PRESENT);
    VkResult result = vkrt_replay_journal_status(source->replay.journal);
    if (result != VK_SUCCESS) return poison(batch, result);
    const VkCommandBufferBeginInfo *recorded = vkrt_replay_journal_begin_info(source->replay.journal);
    if (!recorded || !state->dev->set_loader_data) return poison(batch, VK_ERROR_FEATURE_NOT_PRESENT);
    struct execution_pool *pool;
    result = find_pool(batch, source, &pool);
    if (result != VK_SUCCESS) return poison(batch, result);
    struct execution_command *clone = allocate(state, sizeof(*clone));
    if (!clone) return poison(batch, VK_ERROR_OUT_OF_HOST_MEMORY);
    clone->source = command;
    clone->pool = pool;
    VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = pool->pool,
                                              .level = source->level,
                                              .commandBufferCount = 1};
    result = state->allocate_commands(state->dev->device, &allocation, &clone->command);
    if (result != VK_SUCCESS) {
        release(state, clone);
        return poison(batch, result);
    }
    if (batch->last_command)
        batch->last_command->next = clone;
    else
        batch->commands = clone;
    batch->last_command = clone;
    ++batch->command_count;
    result = state->dev->set_loader_data(state->dev->device, clone->command);
    if (result != VK_SUCCESS) return poison(batch, result);
    struct vkrt_cmd_buffer_data *owner = VKRT_FIND(struct vkrt_cmd_buffer_data, clone->command);
    if (!owner || owner->device_data != state->dev) return poison(batch, VK_ERROR_INITIALIZATION_FAILED);
    vkrt_replay_journal_destroy(owner->replay.journal);
    owner->replay.journal = NULL;
    owner->replaying = true;
    VkCommandBufferBeginInfo begin = *recorded;
    begin.flags = (begin.flags & ~VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = state->begin_command(clone->command, &begin);
    if (result != VK_SUCCESS) return poison(batch, result);
    result = vkrt_rtp_replay_layouts_inherit(owner, source);
    if (result != VK_SUCCESS) return poison(batch, result);
    const struct execution_frame frame = {.previous = batch->recording, .source = command};
    batch->recording = &frame;
    result = vkrt_replay_emit(source->replay.journal, clone->command, resolve, batch, remap_child);
    batch->recording = frame.previous;
    if (result == VK_SUCCESS) result = state->end_command(clone->command);
    if (result != VK_SUCCESS) return poison(batch, result);
    *target = clone->command;
    return VK_SUCCESS;
}
VkResult vkrt_execution_batch_clone(struct vkrt_execution_batch *batch, VkCommandBuffer source, VkCommandBuffer *target) {
    return clone_command(batch, source, target, false);
}
VkResult vkrt_execution_batch_status(const struct vkrt_execution_batch *batch) {
    return batch ? batch->status : VK_ERROR_INITIALIZATION_FAILED;
}
size_t vkrt_execution_batch_clone_count(const struct vkrt_execution_batch *batch) { return batch ? batch->command_count : 0; }
VkCommandBuffer vkrt_execution_batch_clone_command(const struct vkrt_execution_batch *batch, size_t index) {
    const struct execution_command *p = batch ? batch->commands : NULL;
    while (p && index) {
        p = p->next;
        --index;
    }
    return p ? p->command : VK_NULL_HANDLE;
}
VkFence vkrt_execution_batch_completion_fence(const struct vkrt_execution_batch *batch) {
    return batch ? batch->completion : VK_NULL_HANDLE;
}
/* Publish only after ALL native calls referring to this batch have returned.
 * Saturation is fail-closed: no wrapped stamp can make newer work look old. */
static void settle(struct vkrt_execution_device *state, struct vkrt_execution_batch *batch) {
    if (state->settled_clock < VKRT_EXECUTION_SETTLED_LIMIT) {
        batch->settled_stamp = ++state->settled_clock;
        batch->stamp_valid = true;
    }
    batch->ready = true;
}
VkResult vkrt_execution_batch_submitted(struct vkrt_execution_batch *batch) {
    if (!batch) return VK_ERROR_INITIALIZATION_FAILED;
    struct vkrt_execution_device *state = batch->owner;
    vkrt_mutex_lock(&state->lock);
    batch->submitted = true;
    batch->observer_cutoff = state->settled_clock;
    batch->cutoff_valid = state->settled_clock != VKRT_EXECUTION_SETTLED_LIMIT;
    vkrt_mutex_unlock(&state->lock);
    /* This is an observation, not part of the application's work transaction.
     * Failure cannot turn already accepted original work into an OOM result. */
    VkResult result = state->queue_submit(batch->queue, 0, NULL, batch->completion);
    vkrt_mutex_lock(&state->lock);
    batch->observer_associated = result == VK_SUCCESS;
    settle(state, batch);
    vkrt_mutex_unlock(&state->lock);
    /* Pointer ownership passed to the collector at publication above. */
    return result == VK_ERROR_DEVICE_LOST ? result : VK_SUCCESS;
}
void vkrt_execution_batch_quarantine(struct vkrt_execution_batch *batch) {
    if (!batch) return;
    struct vkrt_execution_device *state = batch->owner;
    vkrt_mutex_lock(&state->lock);
    batch->submitted = batch->quarantined = true;
    settle(state, batch);
    vkrt_mutex_unlock(&state->lock);
}
VkResult vkrt_execution_batch_discard(struct vkrt_execution_batch *batch) {
    if (!batch) return VK_SUCCESS;
    struct vkrt_execution_device *state = batch->owner;
    vkrt_mutex_lock(&state->lock);
    if (batch->submitted) {
        vkrt_mutex_unlock(&state->lock);
        return VK_NOT_READY;
    }
    unlink_batch(state, batch);
    vkrt_mutex_unlock(&state->lock);
    destroy_batch(batch);
    return VK_SUCCESS;
}
VkResult vkrt_execution_collect(struct vkrt_device_data *dev, size_t *retired_count) {
    if (retired_count) *retired_count = 0;
    struct vkrt_execution_device *state = dev ? dev->executions : NULL;
    if (!state) return VK_SUCCESS;
    struct vkrt_execution_batch *completed = NULL;
    VkResult error = VK_SUCCESS;
    vkrt_mutex_lock(&state->lock);
    struct vkrt_execution_batch **link = &state->batches;
    while (*link) {
        struct vkrt_execution_batch *batch = *link;
        if (!batch->ready || !batch->observer_associated) {
            link = &batch->next;
            continue;
        }
        VkResult result = state->fence_status(dev->device, batch->completion);
        if (result == VK_SUCCESS) {
            *link = batch->next;
            batch->next = completed;
            completed = batch;
        } else {
            if (result != VK_NOT_READY && error == VK_SUCCESS) error = result;
            link = &batch->next;
        }
    }
    /* Native observer calls that were still in flight at a successful proof's
     * cutoff are excluded, even if their GPU work happened to be queued earlier. */
    struct vkrt_execution_batch *proofs = completed;
    for (struct vkrt_execution_batch *proof = proofs; proof; proof = proof->next) {
        if (!proof->cutoff_valid) continue;
        link = &state->batches;
        while (*link) {
            struct vkrt_execution_batch *batch = *link;
            if (!batch->ready || batch->quarantined || !batch->stamp_valid || batch->queue != proof->queue ||
                batch->settled_stamp > proof->observer_cutoff) {
                link = &batch->next;
                continue;
            }
            *link = batch->next;
            batch->next = completed;
            completed = batch;
        }
    }
    vkrt_mutex_unlock(&state->lock);
    while (completed) {
        struct vkrt_execution_batch *next = completed->next;
        destroy_batch(completed);
        if (retired_count) ++*retired_count;
        completed = next;
    }
    return error;
}
uint64_t vkrt_execution_checkpoint(struct vkrt_device_data *dev) {
    struct vkrt_execution_device *state = dev ? dev->executions : NULL;
    if (!state) return 0;
    vkrt_mutex_lock(&state->lock);
    uint64_t stamp = state->settled_clock;
    vkrt_mutex_unlock(&state->lock);
    return stamp;
}
void vkrt_execution_queue_completed(struct vkrt_device_data *dev, VkQueue queue, uint64_t checkpoint) {
    struct vkrt_execution_device *state = dev ? dev->executions : NULL;
    if (!state) return;
    struct vkrt_execution_batch *completed = NULL;
    vkrt_mutex_lock(&state->lock);
    struct vkrt_execution_batch **link = &state->batches;
    while (*link) {
        struct vkrt_execution_batch *batch = *link;
        if (!batch->ready || !batch->stamp_valid || (queue && batch->queue != queue) || batch->settled_stamp > checkpoint) {
            link = &batch->next;
            continue;
        }
        *link = batch->next;
        batch->next = completed;
        completed = batch;
    }
    vkrt_mutex_unlock(&state->lock);
    while (completed) {
        struct vkrt_execution_batch *next = completed->next;
        destroy_batch(completed);
        completed = next;
    }
}
