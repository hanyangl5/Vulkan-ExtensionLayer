/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_REPLAY_H
#define VKRT_REPLAY_H

#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vkrt_replay_journal;
struct vkrt_replay_entry;

/* Reviewed command semantics for the submission frontend. These are facts
 * about one owned entry, not a complete dependency graph or replay preamble. */
enum vkrt_replay_kind {
    VKRT_REPLAY_UNKNOWN,
    VKRT_REPLAY_STATE,
    VKRT_REPLAY_ACTION,
    VKRT_REPLAY_BARRIER,
    VKRT_REPLAY_EVENT_SET,
    VKRT_REPLAY_EVENT_RESET,
    VKRT_REPLAY_EVENT_WAIT,
    VKRT_REPLAY_QUERY_BEGIN,
    VKRT_REPLAY_QUERY_END,
    VKRT_REPLAY_QUERY_RESET,
    VKRT_REPLAY_QUERY_COPY,
    VKRT_REPLAY_QUERY_TIMESTAMP,
    VKRT_REPLAY_AS_PROPERTY,
    VKRT_REPLAY_AS_BUILD,
    VKRT_REPLAY_AS_COPY,
    VKRT_REPLAY_TRACE,
    VKRT_REPLAY_EXECUTE_SECONDARIES,
    VKRT_REPLAY_RENDER_BEGIN,
    VKRT_REPLAY_RENDER_NEXT,
    VKRT_REPLAY_RENDER_END,
    VKRT_REPLAY_CONDITIONAL_BEGIN,
    VKRT_REPLAY_CONDITIONAL_END,
    VKRT_REPLAY_TRANSFORM_FEEDBACK_BEGIN,
    VKRT_REPLAY_TRANSFORM_FEEDBACK_END
};
enum vkrt_replay_metadata_flag_bits {
    VKRT_REPLAY_LOWERED = 1u << 0,
    /* Graphics stages depend on the bound pipeline/shaders, attachments and
     * dynamic state. Any masks supplied below are only the known subset. */
    VKRT_REPLAY_GRAPHICS_CONTEXT = 1u << 1,
    /* Resolve the AS BUILD/AS COPY synchronization aliases for the enabled
     * profile; this does not claim that COPY replaces BUILD exclusively. */
    VKRT_REPLAY_AS_COPY_CONTEXT = 1u << 2,
    /* Native versus virtual pool type affects physical execution and native
     * query scopes. Query reset also adds range-specific implicit ordering. */
    VKRT_REPLAY_QUERY_POOL_CONTEXT = 1u << 3,
    /* Raw application stage scopes must be expanded by the queue/frontend;
     * do not treat them as normalized individual-stage graph masks. */
    VKRT_REPLAY_RAW_SCOPES = 1u << 4,
    /* An operation consumes indirect parameters as part of its work. */
    VKRT_REPLAY_INDIRECT = 1u << 5,
    /* Registry state commands which cannot be replayed outside their native
     * scope must not be hoisted into an ordinary suffix preamble. */
    VKRT_REPLAY_RENDER_LOCAL = 1u << 6,
    VKRT_REPLAY_VIDEO_LOCAL = 1u << 7
};
typedef VkFlags vkrt_replay_metadata_flags;
enum vkrt_replay_state_kind {
    VKRT_REPLAY_STATE_DYNAMIC,
    VKRT_REPLAY_STATE_PIPELINE,
    VKRT_REPLAY_STATE_SHADERS,
    VKRT_REPLAY_STATE_DESCRIPTORS,
    VKRT_REPLAY_STATE_PUSH_CONSTANTS,
    VKRT_REPLAY_STATE_VERTEX_INDEX,
    VKRT_REPLAY_STATE_DEVICE_MASK,
    VKRT_REPLAY_STATE_RT_STACK
};
enum vkrt_replay_render_kind { VKRT_REPLAY_RENDER_PASS, VKRT_REPLAY_DYNAMIC_RENDERING };
enum vkrt_replay_as_copy_kind { VKRT_REPLAY_AS_CLONE_COMPACT, VKRT_REPLAY_AS_SERIALIZE, VKRT_REPLAY_AS_DESERIALIZE };

struct vkrt_replay_metadata {
    enum vkrt_replay_kind kind;
    vkrt_replay_metadata_flags flags;
    /* Exact work stages only when the corresponding context flag is absent.
     * Synchronization/reset/boundary commands have no action-stage mask.
     * Physical masks describe native commands, except LOWERED AS/RTP work;
     * virtual QUERY_COPY adds CS formatting before its native COPY stage. */
    VkPipelineStageFlags2 logical_stages;
    VkPipelineStageFlags2 physical_stages;
    union {
        struct {
            enum vkrt_replay_state_kind kind;
            VkPipelineBindPoint bind_point;
            VkPipeline pipeline;
            VkShaderStageFlags affected_stages;
            uint32_t shader_count;
            const VkShaderStageFlagBits *shader_stages;
            const VkShaderEXT *shaders;
            uint32_t device_mask;
            uint32_t stack_size;
        } state;
        struct {
            bool synchronization2;
            VkPipelineStageFlags2 src_stage_mask, dst_stage_mask;
            VkDependencyFlags dependency_flags;
            uint32_t memory_barrier_count, buffer_barrier_count, image_barrier_count;
            const VkMemoryBarrier *memory_barriers;
            const VkBufferMemoryBarrier *buffer_barriers;
            const VkImageMemoryBarrier *image_barriers;
            /* One for Barrier2/SetEvent2; event_count for WaitEvents2. The
             * original per-barrier scopes/ranges/pNext remain intact. */
            uint32_t dependency_info_count;
            const VkDependencyInfo *dependency_infos;
            VkEvent event;
            uint32_t event_count;
            const VkEvent *events;
        } sync;
        struct {
            VkQueryPool pool;
            uint32_t first, count, index;
            VkQueryControlFlags control_flags;
            VkQueryResultFlags result_flags;
            VkQueryType type;
            VkPipelineStageFlags2 stage_mask;
            VkBuffer destination;
            VkDeviceSize offset, stride;
            const VkAccelerationStructureKHR *acceleration_structures;
        } query;
        struct {
            uint32_t count;
            const VkAccelerationStructureBuildGeometryInfoKHR *infos;
            const VkAccelerationStructureBuildRangeInfoKHR *const *ranges;
            const VkDeviceAddress *indirect_addresses;
            const uint32_t *indirect_strides;
            const uint32_t *const *max_primitive_counts;
        } as_build;
        struct {
            enum vkrt_replay_as_copy_kind kind;
            const VkCopyAccelerationStructureInfoKHR *copy;
            const VkCopyAccelerationStructureToMemoryInfoKHR *serialize;
            const VkCopyMemoryToAccelerationStructureInfoKHR *deserialize;
        } as_copy;
        struct {
            const VkStridedDeviceAddressRegionKHR *raygen, *miss, *hit, *callable;
            uint32_t width, height, depth;
            VkDeviceAddress indirect_address;
        } trace;
        struct {
            uint32_t count;
            const VkCommandBuffer *commands;
        } children;
        struct {
            enum vkrt_replay_render_kind kind;
            VkSubpassContents contents;
            const VkRenderPassBeginInfo *render_pass;
            const VkRenderingInfo *rendering;
            const VkSubpassBeginInfo *subpass_begin;
            const VkSubpassEndInfo *subpass_end;
        } render;
        struct {
            const VkConditionalRenderingBeginInfoEXT *begin;
        } conditional;
        struct {
            uint32_t first, count;
            const VkBuffer *counter_buffers;
            const VkDeviceSize *counter_offsets;
        } transform_feedback;
    } data;
};

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
    /* Template payloads have no XML extent. An integration with immutable
     * template metadata can normalize them into ordinary owned push writes. */
    VkResult (*capture_descriptor_template)(struct vkrt_replay_context *context, const VkPushDescriptorSetWithTemplateInfo *info);
};

/* Typed capture-only entry points reuse the owning command schemas. The first
 * argument is a context rather than a command buffer, and every entry returns
 * the journal status. They neither look up command state nor forward a native
 * command. A standalone history may omit resolve and normalization callbacks.
 * Consumers of their PFN_vkrt_capture_Cmd* typedefs explicitly include
 * vkrt_replay_capture_generated.h from the generator's output directory. */
PFN_vkVoidFunction vkrt_replay_capture_proc(const char *name);

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
 * Vulkan's normal object-lifetime requirements. Templates need an immutable
 * integration schema; opaque inputs never fall back to borrowed data. */
const struct vkrt_replay_entry *vkrt_replay_first(const struct vkrt_replay_journal *journal);
const struct vkrt_replay_entry *vkrt_replay_next(const struct vkrt_replay_entry *entry);
const char *vkrt_replay_entry_name(const struct vkrt_replay_entry *entry);
/* No allocation. Returned pointers are immutable views into the entry and
 * remain valid until journal reset/destruction. Unknown semantics return
 * FEATURE_NOT_PRESENT with a zeroed UNKNOWN result, without poisoning capture
 * or preventing ordinary replay. The frontend must resolve context flags and
 * explicit/implicit dependencies before admitting any slice. */
VkResult vkrt_replay_entry_metadata(const struct vkrt_replay_entry *entry, struct vkrt_replay_metadata *metadata);
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
