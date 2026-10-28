/* SPDX-License-Identifier: MIT */
#ifndef VKRT_STAGE_SCHEDULE_H
#define VKRT_STAGE_SCHEDULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* A logical dependency planner, not a native queue executor. See
 * docs/QUERY_STAGE_SCHEDULING.md for its admission and token contracts. */
typedef size_t vkrt_stage_id;
#define VKRT_STAGE_ID_INVALID SIZE_MAX

enum vkrt_stage_result {
    VKRT_STAGE_OK,
    VKRT_STAGE_OUT_OF_MEMORY,
    VKRT_STAGE_INVALID,
    VKRT_STAGE_INCOMPLETE_CAPTURE,
    VKRT_STAGE_PARTIAL_SCOPE,
    VKRT_STAGE_BLOCKED
};

enum vkrt_stage_kind { VKRT_STAGE_WAIT, VKRT_STAGE_WORK, VKRT_STAGE_SIGNAL, VKRT_STAGE_FENCE };

/* Stage scopes contain individual stage bits, with aggregate aliases and
 * logical earlier/later stages already expanded by the capture frontend.
 * In particular, raw TRANSFER, ALL_GRAPHICS and ALL_COMMANDS are not scopes.
 * Keeping the original mask separate prevents accidental physical rewriting. */
struct vkrt_stage_wait {
    VkPipelineStageFlags2 original_mask;
    VkPipelineStageFlags2 logical_dst_scope;
    uint64_t cookie;
    bool scope_complete;
};

struct vkrt_stage_work {
    VkPipelineStageFlags2 logical_stages;
    VkPipelineStageFlags2 physical_stages;
    uint64_t replay_cookie;
    /* Complete synchronization/state dependencies and a replay preamble are
     * required even if the dependency array is empty. A whole native query or
     * render-pass scope may be supplied as one indivisible work item. */
    bool dependencies_complete;
    bool replay_complete;
};

struct vkrt_stage_signal {
    VkPipelineStageFlags2 original_mask;
    VkPipelineStageFlags2 logical_src_scope;
    uint64_t cookie;
    /* Additional dependencies below must describe semaphore/fence operation
     * ordering and wait consumption; those rules are not command stage bits. */
    bool ordering_complete;
};

struct vkrt_stage_schedule;

/* Returned pointers remain valid only until the next planner mutation. */
struct vkrt_stage_node {
    enum vkrt_stage_kind kind;
    VkPipelineStageFlags2 original_mask;
    VkPipelineStageFlags2 logical_stages;
    VkPipelineStageFlags2 physical_stages;
    uint64_t cookie;
    const vkrt_stage_id *dependencies;
    size_t dependency_count;
    const vkrt_stage_id *waits;
    size_t wait_count;
    uint64_t completion_token;
};

struct vkrt_stage_schedule *vkrt_stage_schedule_create(void);
void vkrt_stage_schedule_destroy(struct vkrt_stage_schedule *schedule);
size_t vkrt_stage_schedule_count(const struct vkrt_stage_schedule *schedule);

/* Successful insertion copies all inputs. Failed insertions leave the graph
 * unchanged and set *id to INVALID. IDs are execution identities, never CB
 * handles: replaying one secondary twice requires two work insertions. */
enum vkrt_stage_result vkrt_stage_schedule_wait(struct vkrt_stage_schedule *schedule, const struct vkrt_stage_wait *wait,
                                                vkrt_stage_id *id);
enum vkrt_stage_result vkrt_stage_schedule_work(struct vkrt_stage_schedule *schedule, const struct vkrt_stage_work *work,
                                                size_t dependency_count, const vkrt_stage_id *dependencies, vkrt_stage_id *id);

/* Exact joins are useful for stage-scoped submit2 signals. The frontend must
 * supply their entire first scope, including signal-operation ordering. This
 * helper does not infer that scope from the physical stages or from array
 * order. A legacy full signal / queue-submit fence can use full_join instead. */
enum vkrt_stage_result vkrt_stage_schedule_join(struct vkrt_stage_schedule *schedule, enum vkrt_stage_kind kind, uint64_t cookie,
                                                bool dependencies_complete, size_t dependency_count,
                                                const vkrt_stage_id *dependencies, vkrt_stage_id *id);
/* Derives command dependencies from the expanded first scope; additional
 * ordering dependencies still come from the submission frontend. A partly
 * included indivisible work item is rejected instead of delaying the signal
 * to that item's end. Same-batch signals need not depend on each other. */
enum vkrt_stage_result vkrt_stage_schedule_signal(struct vkrt_stage_schedule *schedule, const struct vkrt_stage_signal *signal,
                                                  size_t ordering_count, const vkrt_stage_id *ordering_dependencies,
                                                  vkrt_stage_id *id);
enum vkrt_stage_result vkrt_stage_schedule_full_join(struct vkrt_stage_schedule *schedule, enum vkrt_stage_kind kind,
                                                     uint64_t cookie, vkrt_stage_id *id);

bool vkrt_stage_schedule_node(const struct vkrt_stage_schedule *schedule, vkrt_stage_id id, struct vkrt_stage_node *node);

/* A wait is never returned by next(). A nonzero acquire token certifies an
 * already completed native acquire with its device memory dependency. A host
 * timeline counter observation alone is not that certificate. Unknown binary
 * or imported semaphore readiness must remain unresolved. */
enum vkrt_stage_result vkrt_stage_schedule_acquire(struct vkrt_stage_schedule *schedule, vkrt_stage_id wait,
                                                   uint64_t acquire_token);

/* Dependencies need only be emitted, not host-completed: the executor must
 * connect their completion tokens to the emitted node using GPU dependencies.
 * It must not place an unresolved aliased-stage native wait ahead of later,
 * logically ungated work. No host wait or readback is performed here. */
enum vkrt_stage_result vkrt_stage_schedule_next(const struct vkrt_stage_schedule *schedule, vkrt_stage_id *id);
enum vkrt_stage_result vkrt_stage_schedule_issue(struct vkrt_stage_schedule *schedule, vkrt_stage_id id, uint64_t completion_token);

#endif
