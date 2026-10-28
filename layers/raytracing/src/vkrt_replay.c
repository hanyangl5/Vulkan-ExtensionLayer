/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_replay.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct replay_allocation {
    struct replay_allocation *next;
    max_align_t alignment;
    unsigned char bytes[];
};
struct replay_chain_frame {
    const void *source;
    const struct replay_chain_frame *previous;
};
struct replay_arena {
    struct replay_allocation *allocations;
    const VkAllocationCallbacks *allocator;
    VkSystemAllocationScope scope;
    VkResult status;
    VkResult (*normalize_pipeline_layout)(void *user, VkPipelineLayout source, VkPipelineLayout *stable);
    VkResult (*emit_pipeline_layout)(void *user, const char *name, VkPipelineLayout source, VkPipelineLayout *stable);
    const char *command_name;
    const struct replay_chain_frame *chain_path;
    VkCommandBufferUsageFlags begin_flags;
    bool has_begin_context;
    bool in_inheritance;
    bool dynamic_rendering_inheritance;
    void *user;
};
typedef VkResult (*replay_emitter)(const void *data, VkCommandBuffer target, vkrt_replay_resolve resolve, void *user,
                                   vkrt_replay_remap_child remap_child, const VkAllocationCallbacks *allocator,
                                   VkSystemAllocationScope scope);
typedef VkResult (*replay_metadata)(const void *data, struct vkrt_replay_metadata *metadata);
struct vkrt_replay_entry {
    struct vkrt_replay_entry *next;
    const char *name;
    replay_emitter emit;
    replay_metadata describe;
    const void *data;
    struct replay_arena arena;
};
struct vkrt_replay_journal {
    struct vkrt_replay_entry *first, *last;
    size_t count;
    VkResult status;
    const char *error_command;
    bool has_allocator;
    VkAllocationCallbacks allocator;
    VkSystemAllocationScope scope;
    struct replay_arena begin_arena;
    const VkCommandBufferBeginInfo *begin_info;
};

static void *replay_allocate(const VkAllocationCallbacks *allocator, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    return allocator ? allocator->pfnAllocation(allocator->pUserData, size, alignment, scope) : malloc(size);
}
static void replay_free(const VkAllocationCallbacks *allocator, void *memory) {
    if (!memory) return;
    if (allocator)
        allocator->pfnFree(allocator->pUserData, memory);
    else
        free(memory);
}
static void replay_arena_finish(struct replay_arena *arena) {
    while (arena->allocations) {
        struct replay_allocation *next = arena->allocations->next;
        replay_free(arena->allocator, arena->allocations);
        arena->allocations = next;
    }
}
static void *replay_alloc(struct replay_arena *arena, size_t count, size_t size) {
    if (arena->status != VK_SUCCESS) return NULL;
    if ((count && size > SIZE_MAX / count) || count * size > SIZE_MAX - offsetof(struct replay_allocation, bytes)) {
        arena->status = VK_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }
    struct replay_allocation *allocation = replay_allocate(
        arena->allocator, offsetof(struct replay_allocation, bytes) + count * size, _Alignof(max_align_t), arena->scope);
    if (!allocation) {
        arena->status = VK_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }
    allocation->next = arena->allocations;
    arena->allocations = allocation;
    return allocation->bytes;
}
static void *replay_copy(struct replay_arena *arena, const void *source, size_t count, size_t size) {
    if (!source) return NULL;
    void *target = replay_alloc(arena, count, size);
    if (target && count && size) memcpy(target, source, count * size);
    return target;
}
static void replay_unsupported(struct replay_arena *arena) {
    if (arena->status == VK_SUCCESS) arena->status = VK_ERROR_FEATURE_NOT_PRESENT;
}
static size_t replay_size_value(struct replay_arena *arena, uint64_t value) {
    if (value > SIZE_MAX) {
        if (arena->status == VK_SUCCESS) arena->status = VK_ERROR_OUT_OF_HOST_MEMORY;
        return 0;
    }
    return (size_t)value;
}
static size_t replay_size_add(struct replay_arena *arena, size_t a, size_t b) {
    if (b > SIZE_MAX - a) {
        arena->status = VK_ERROR_OUT_OF_HOST_MEMORY;
        return 0;
    }
    return a + b;
}
static inline size_t replay_size_sub(struct replay_arena *arena, size_t a, size_t b) {
    if (a < b) {
        replay_unsupported(arena);
        return 0;
    }
    return a - b;
}
static size_t replay_size_mul(struct replay_arena *arena, size_t a, size_t b) {
    if (a && b > SIZE_MAX / a) {
        arena->status = VK_ERROR_OUT_OF_HOST_MEMORY;
        return 0;
    }
    return a * b;
}
static size_t replay_size_div(struct replay_arena *arena, size_t a, size_t b) {
    if (!b) {
        replay_unsupported(arena);
        return 0;
    }
    return a / b;
}
static VkPipelineLayout replay_layout(struct replay_arena *arena, VkPipelineLayout source) {
    VkPipelineLayout stable = source;
    if (arena->status == VK_SUCCESS) {
        if (arena->emit_pipeline_layout)
            arena->status = arena->emit_pipeline_layout(arena->user, arena->command_name, source, &stable);
        else if (arena->normalize_pipeline_layout)
            arena->status = arena->normalize_pipeline_layout(arena->user, source, &stable);
    }
    return stable;
}
static void replay_capture_end(struct vkrt_replay_context *context, const char *name, replay_emitter emit, replay_metadata describe,
                               void *data, struct replay_arena *arena) {
    struct vkrt_replay_journal *journal = context->journal;
    if (arena->status == VK_SUCCESS) {
        struct vkrt_replay_entry *entry = replay_alloc(arena, 1, sizeof(*entry));
        if (entry) {
            *entry = (struct vkrt_replay_entry){.name = name, .emit = emit, .describe = describe, .data = data, .arena = *arena};
            if (journal->last)
                journal->last->next = entry;
            else
                journal->first = entry;
            journal->last = entry;
            ++journal->count;
            return;
        }
    }
    if (journal->status == VK_SUCCESS) {
        journal->status = arena->status;
        journal->error_command = name;
    }
    replay_arena_finish(arena);
    if (context->capture_error) context->capture_error(context->user, journal->status);
}
static struct replay_arena replay_capture_begin(const struct vkrt_replay_context *context) {
    const struct vkrt_replay_journal *journal = context->journal;
    return (struct replay_arena){.allocator = journal->has_allocator ? &journal->allocator : NULL,
                                 .scope = journal->scope,
                                 .status = journal->status,
                                 .normalize_pipeline_layout = context->normalize_pipeline_layout,
                                 .user = context->user};
}

struct vkrt_replay_journal *vkrt_replay_journal_create(const VkAllocationCallbacks *allocator, VkSystemAllocationScope scope) {
    struct vkrt_replay_journal *journal = replay_allocate(allocator, sizeof(*journal), _Alignof(struct vkrt_replay_journal), scope);
    if (!journal) return NULL;
    *journal = (struct vkrt_replay_journal){.scope = scope};
    if (allocator) {
        journal->has_allocator = true;
        journal->allocator = *allocator;
    }
    return journal;
}
void vkrt_replay_journal_reset(struct vkrt_replay_journal *journal) {
    if (!journal) return;
    while (journal->first) {
        struct vkrt_replay_entry *next = journal->first->next;
        struct replay_arena arena = journal->first->arena;
        replay_arena_finish(&arena);
        journal->first = next;
    }
    journal->last = NULL;
    journal->count = 0;
    journal->status = VK_SUCCESS;
    journal->error_command = NULL;
    replay_arena_finish(&journal->begin_arena);
    journal->begin_info = NULL;
}
void vkrt_replay_journal_destroy(struct vkrt_replay_journal *journal) {
    if (!journal) return;
    const bool custom = journal->has_allocator;
    VkAllocationCallbacks allocator = journal->allocator;
    vkrt_replay_journal_reset(journal);
    replay_free(custom ? &allocator : NULL, journal);
}
VkResult vkrt_replay_journal_status(const struct vkrt_replay_journal *journal) { return journal ? journal->status : VK_SUCCESS; }
const char *vkrt_replay_journal_error_command(const struct vkrt_replay_journal *journal) {
    return journal ? journal->error_command : NULL;
}
size_t vkrt_replay_journal_count(const struct vkrt_replay_journal *journal) { return journal ? journal->count : 0; }
const struct vkrt_replay_entry *vkrt_replay_first(const struct vkrt_replay_journal *journal) {
    return journal ? journal->first : NULL;
}
const struct vkrt_replay_entry *vkrt_replay_next(const struct vkrt_replay_entry *entry) { return entry ? entry->next : NULL; }
const char *vkrt_replay_entry_name(const struct vkrt_replay_entry *entry) { return entry ? entry->name : NULL; }
VkResult vkrt_replay_entry_metadata(const struct vkrt_replay_entry *entry, struct vkrt_replay_metadata *metadata) {
    if (!metadata) return VK_ERROR_INITIALIZATION_FAILED;
    memset(metadata, 0, sizeof(*metadata));
    if (!entry) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = entry->describe ? entry->describe(entry->data, metadata) : VK_ERROR_FEATURE_NOT_PRESENT;
    if (result != VK_SUCCESS) memset(metadata, 0, sizeof(*metadata));
    return result;
}
VkResult vkrt_replay_entry_emit(const struct vkrt_replay_entry *entry, VkCommandBuffer target, vkrt_replay_resolve resolve,
                                void *user, vkrt_replay_remap_child remap_child) {
    return entry && resolve
               ? entry->emit(entry->data, target, resolve, user, remap_child, entry->arena.allocator, entry->arena.scope)
               : VK_ERROR_INITIALIZATION_FAILED;
}
VkResult vkrt_replay_emit(const struct vkrt_replay_journal *journal, VkCommandBuffer target, vkrt_replay_resolve resolve,
                          void *user, vkrt_replay_remap_child remap_child) {
    if (!journal) return VK_SUCCESS;
    if (journal->status != VK_SUCCESS) return journal->status;
    for (const struct vkrt_replay_entry *entry = journal->first; entry; entry = entry->next) {
        VkResult result = vkrt_replay_entry_emit(entry, target, resolve, user, remap_child);
        if (result != VK_SUCCESS) return result;
    }
    return VK_SUCCESS;
}

#include "vkrt_replay_generated.inc"

VkResult vkrt_replay_journal_begin(struct vkrt_replay_journal *journal, const VkCommandBufferBeginInfo *info) {
    if (!journal || !info) return VK_ERROR_INITIALIZATION_FAILED;
    vkrt_replay_journal_reset(journal);
    journal->begin_arena = (struct replay_arena){.allocator = journal->has_allocator ? &journal->allocator : NULL,
                                                 .scope = journal->scope,
                                                 .begin_flags = info->flags,
                                                 .has_begin_context = true};
    VkCommandBufferBeginInfo *copy = replay_alloc(&journal->begin_arena, 1, sizeof(*copy));
    if (copy) replay_clone_VkCommandBufferBeginInfo(&journal->begin_arena, copy, info);
    journal->begin_info = copy;
    journal->status = journal->begin_arena.status;
    if (journal->status != VK_SUCCESS) journal->error_command = "vkBeginCommandBuffer";
    return journal->status;
}
const VkCommandBufferBeginInfo *vkrt_replay_journal_begin_info(const struct vkrt_replay_journal *journal) {
    return journal ? journal->begin_info : NULL;
}
