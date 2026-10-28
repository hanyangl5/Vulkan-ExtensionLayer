/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_REPLAY_H
#define VKRT_REPLAY_H

#include <vulkan/vulkan.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vkrt_replay_journal;
struct vkrt_replay_entry;

/* Resolve the existing layer command implementation, bypassing this recorder.
 * Re-emission still passes through ordinary AS/RTP/descriptor translation. */
typedef PFN_vkVoidFunction (*vkrt_replay_resolve)(void *user, const char *name);
typedef VkResult (*vkrt_replay_remap_child)(void *user, VkCommandBuffer source, VkCommandBuffer *target);

struct vkrt_replay_context {
    struct vkrt_replay_journal *journal;
    vkrt_replay_resolve resolve;
    void *user;
    void (*capture_error)(void *user, VkResult error);
    VkResult (*normalize_pipeline_layout)(void *user, VkPipelineLayout source, VkPipelineLayout *stable);
    /* Capture may retain a unique semantic token. Re-emission chooses between
     * passing that token to a translating upper hook and a real native handle. */
    VkResult (*emit_pipeline_layout)(void *user, const char *command_name, VkPipelineLayout recorded, VkPipelineLayout *forward);
};

/* Supplied by command-buffer integration. During re-emission the fresh command
 * buffer has a context with journal=NULL, avoiding recursive capture. */
struct vkrt_replay_context *vkrt_replay_lookup_context(VkCommandBuffer command);

struct vkrt_replay_journal *vkrt_replay_journal_create(const VkAllocationCallbacks *allocator, VkSystemAllocationScope scope);
void vkrt_replay_journal_reset(struct vkrt_replay_journal *journal);
void vkrt_replay_journal_destroy(struct vkrt_replay_journal *journal);
/* The integration knows the CB level and must replace ignored primary-buffer
 * pInheritanceInfo with NULL before capture; native Begin still sees app input. */
VkResult vkrt_replay_journal_begin(struct vkrt_replay_journal *journal, const VkCommandBufferBeginInfo *info);
const VkCommandBufferBeginInfo *vkrt_replay_journal_begin_info(const struct vkrt_replay_journal *journal);
VkResult vkrt_replay_journal_status(const struct vkrt_replay_journal *journal);
const char *vkrt_replay_journal_error_command(const struct vkrt_replay_journal *journal);
size_t vkrt_replay_journal_count(const struct vkrt_replay_journal *journal);

/* Entries own every supported pointer-valued command input. Handles retain
 * Vulkan's normal object-lifetime requirements; opaque template payloads and
 * unknown structure schemas fail capture explicitly instead of borrowing data. */
const struct vkrt_replay_entry *vkrt_replay_first(const struct vkrt_replay_journal *journal);
const struct vkrt_replay_entry *vkrt_replay_next(const struct vkrt_replay_entry *entry);
const char *vkrt_replay_entry_name(const struct vkrt_replay_entry *entry);
VkResult vkrt_replay_entry_emit(const struct vkrt_replay_entry *entry, VkCommandBuffer target, vkrt_replay_resolve resolve,
                                void *user, vkrt_replay_remap_child remap_child);
VkResult vkrt_replay_emit(const struct vkrt_replay_journal *journal, VkCommandBuffer target, vkrt_replay_resolve resolve,
                          void *user, vkrt_replay_remap_child remap_child);

/* Every known command gets a typed wrapper, including explicitly unsupported
 * marshal shapes. Unsupported capture poisons only the journal; the wrapper
 * still records the application's original command through the direct hook. */
PFN_vkVoidFunction vkrt_replay_proc(const char *name);

#ifdef __cplusplus
}
#endif
#endif
