/* SPDX-License-Identifier: MIT */
#include "vkrt_stage_schedule.h"

#include <stdlib.h>
#include <string.h>

struct stage_node {
    struct vkrt_stage_node view;
    vkrt_stage_id *dependencies;
    vkrt_stage_id *waits;
};

struct vkrt_stage_schedule {
    struct stage_node *nodes;
    size_t count;
    size_t capacity;
};

static bool atomic_stages(VkPipelineStageFlags2 stages) {
    const VkPipelineStageFlags2 aggregate = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                                            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                                            VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
    return !(stages & aggregate);
}

static vkrt_stage_id *allocate_ids(size_t count) {
    if (!count || count > SIZE_MAX / sizeof(vkrt_stage_id)) return NULL;
    return malloc(count * sizeof(vkrt_stage_id));
}

static enum vkrt_stage_result append(struct vkrt_stage_schedule *schedule, struct stage_node *node, vkrt_stage_id *id) {
    if (schedule->count == schedule->capacity) {
        size_t capacity = schedule->capacity ? schedule->capacity * 2 : 16;
        if (capacity <= schedule->capacity || capacity > SIZE_MAX / sizeof(*schedule->nodes)) return VKRT_STAGE_OUT_OF_MEMORY;
        struct stage_node *nodes = realloc(schedule->nodes, capacity * sizeof(*nodes));
        if (!nodes) return VKRT_STAGE_OUT_OF_MEMORY;
        schedule->nodes = nodes;
        schedule->capacity = capacity;
    }
    node->view.dependencies = node->dependencies;
    node->view.waits = node->waits;
    *id = schedule->count;
    schedule->nodes[schedule->count++] = *node;
    return VKRT_STAGE_OK;
}

static enum vkrt_stage_result copy_dependencies(const struct vkrt_stage_schedule *schedule, size_t count, const vkrt_stage_id *ids,
                                                struct stage_node *node) {
    if (count && !ids) return VKRT_STAGE_INVALID;
    if (count > SIZE_MAX / sizeof(*ids)) return VKRT_STAGE_OUT_OF_MEMORY;
    for (size_t i = 0; i < count; ++i)
        if (ids[i] >= schedule->count) return VKRT_STAGE_INVALID;
    if (count) {
        node->dependencies = allocate_ids(count);
        if (!node->dependencies) return VKRT_STAGE_OUT_OF_MEMORY;
        memcpy(node->dependencies, ids, count * sizeof(*ids));
    }
    node->view.dependency_count = count;
    return VKRT_STAGE_OK;
}

struct vkrt_stage_schedule *vkrt_stage_schedule_create(void) { return calloc(1, sizeof(struct vkrt_stage_schedule)); }

void vkrt_stage_schedule_destroy(struct vkrt_stage_schedule *schedule) {
    if (!schedule) return;
    for (size_t i = 0; i < schedule->count; ++i) {
        free(schedule->nodes[i].dependencies);
        free(schedule->nodes[i].waits);
    }
    free(schedule->nodes);
    free(schedule);
}

size_t vkrt_stage_schedule_count(const struct vkrt_stage_schedule *schedule) { return schedule ? schedule->count : 0; }

enum vkrt_stage_result vkrt_stage_schedule_wait(struct vkrt_stage_schedule *schedule, const struct vkrt_stage_wait *wait,
                                                vkrt_stage_id *id) {
    if (id) *id = VKRT_STAGE_ID_INVALID;
    if (!schedule || !wait || !id) return VKRT_STAGE_INVALID;
    if (!wait->scope_complete) return VKRT_STAGE_INCOMPLETE_CAPTURE;
    if (!atomic_stages(wait->logical_dst_scope)) return VKRT_STAGE_INVALID;
    struct stage_node node = {0};
    node.view.kind = VKRT_STAGE_WAIT;
    node.view.original_mask = wait->original_mask;
    node.view.logical_stages = wait->logical_dst_scope;
    node.view.cookie = wait->cookie;
    return append(schedule, &node, id);
}

enum vkrt_stage_result vkrt_stage_schedule_work(struct vkrt_stage_schedule *schedule, const struct vkrt_stage_work *work,
                                                size_t dependency_count, const vkrt_stage_id *dependencies, vkrt_stage_id *id) {
    if (id) *id = VKRT_STAGE_ID_INVALID;
    if (!schedule || !work || !id) return VKRT_STAGE_INVALID;
    if (!work->dependencies_complete || !work->replay_complete) return VKRT_STAGE_INCOMPLETE_CAPTURE;
    if (!work->logical_stages || !work->physical_stages || !atomic_stages(work->logical_stages) ||
        !atomic_stages(work->physical_stages))
        return VKRT_STAGE_INVALID;

    struct stage_node node = {0};
    node.view.kind = VKRT_STAGE_WORK;
    node.view.logical_stages = work->logical_stages;
    node.view.physical_stages = work->physical_stages;
    node.view.cookie = work->replay_cookie;
    for (size_t i = 0; i < schedule->count; ++i) {
        const struct vkrt_stage_node *wait = &schedule->nodes[i].view;
        if (wait->kind != VKRT_STAGE_WAIT || !(work->logical_stages & wait->logical_stages)) continue;
        /* This node cannot be split into its partly gated native stages. Even
         * an already acquired wait retains this admission check: otherwise
         * acceptance would depend on incidental runtime timing. */
        if (work->logical_stages & ~wait->logical_stages) return VKRT_STAGE_PARTIAL_SCOPE;
        ++node.view.wait_count;
    }
    enum vkrt_stage_result result = copy_dependencies(schedule, dependency_count, dependencies, &node);
    if (result != VKRT_STAGE_OK) return result;
    if (node.view.wait_count) {
        node.waits = allocate_ids(node.view.wait_count);
        if (!node.waits) {
            free(node.dependencies);
            return VKRT_STAGE_OUT_OF_MEMORY;
        }
        size_t n = 0;
        for (size_t i = 0; i < schedule->count; ++i) {
            const struct vkrt_stage_node *wait = &schedule->nodes[i].view;
            if (wait->kind == VKRT_STAGE_WAIT && (work->logical_stages & wait->logical_stages)) node.waits[n++] = i;
        }
    }
    result = append(schedule, &node, id);
    if (result != VKRT_STAGE_OK) {
        free(node.dependencies);
        free(node.waits);
    }
    return result;
}

enum vkrt_stage_result vkrt_stage_schedule_join(struct vkrt_stage_schedule *schedule, enum vkrt_stage_kind kind, uint64_t cookie,
                                                bool dependencies_complete, size_t dependency_count,
                                                const vkrt_stage_id *dependencies, vkrt_stage_id *id) {
    if (id) *id = VKRT_STAGE_ID_INVALID;
    if (!schedule || !id || (kind != VKRT_STAGE_SIGNAL && kind != VKRT_STAGE_FENCE)) return VKRT_STAGE_INVALID;
    if (!dependencies_complete) return VKRT_STAGE_INCOMPLETE_CAPTURE;
    struct stage_node node = {0};
    node.view.kind = kind;
    node.view.cookie = cookie;
    enum vkrt_stage_result result = copy_dependencies(schedule, dependency_count, dependencies, &node);
    if (result != VKRT_STAGE_OK) return result;
    result = append(schedule, &node, id);
    if (result != VKRT_STAGE_OK) free(node.dependencies);
    return result;
}

enum vkrt_stage_result vkrt_stage_schedule_full_join(struct vkrt_stage_schedule *schedule, enum vkrt_stage_kind kind,
                                                     uint64_t cookie, vkrt_stage_id *id) {
    if (id) *id = VKRT_STAGE_ID_INVALID;
    if (!schedule || !id || (kind != VKRT_STAGE_SIGNAL && kind != VKRT_STAGE_FENCE)) return VKRT_STAGE_INVALID;
    vkrt_stage_id *ids = allocate_ids(schedule->count);
    if (schedule->count && !ids) return VKRT_STAGE_OUT_OF_MEMORY;
    for (size_t i = 0; i < schedule->count; ++i) ids[i] = i;
    enum vkrt_stage_result result = vkrt_stage_schedule_join(schedule, kind, cookie, true, schedule->count, ids, id);
    free(ids);
    return result;
}

enum vkrt_stage_result vkrt_stage_schedule_signal(struct vkrt_stage_schedule *schedule, const struct vkrt_stage_signal *signal,
                                                  size_t ordering_count, const vkrt_stage_id *ordering_dependencies,
                                                  vkrt_stage_id *id) {
    if (id) *id = VKRT_STAGE_ID_INVALID;
    if (!schedule || !signal || !id || (ordering_count && !ordering_dependencies)) return VKRT_STAGE_INVALID;
    if (!signal->ordering_complete) return VKRT_STAGE_INCOMPLETE_CAPTURE;
    if (!atomic_stages(signal->logical_src_scope)) return VKRT_STAGE_INVALID;
    size_t count = ordering_count;
    for (size_t i = 0; i < schedule->count; ++i) {
        const struct vkrt_stage_node *work = &schedule->nodes[i].view;
        if (work->kind != VKRT_STAGE_WORK || !(work->logical_stages & signal->logical_src_scope)) continue;
        if (work->logical_stages & ~signal->logical_src_scope) return VKRT_STAGE_PARTIAL_SCOPE;
        if (count == SIZE_MAX) return VKRT_STAGE_OUT_OF_MEMORY;
        ++count;
    }
    vkrt_stage_id *deps = allocate_ids(count);
    if (count && !deps) return VKRT_STAGE_OUT_OF_MEMORY;
    if (ordering_count) memcpy(deps, ordering_dependencies, ordering_count * sizeof(*deps));
    size_t n = ordering_count;
    for (size_t i = 0; i < schedule->count; ++i) {
        const struct vkrt_stage_node *work = &schedule->nodes[i].view;
        if (work->kind == VKRT_STAGE_WORK && (work->logical_stages & signal->logical_src_scope)) deps[n++] = i;
    }
    enum vkrt_stage_result result = vkrt_stage_schedule_join(schedule, VKRT_STAGE_SIGNAL, signal->cookie, true, count, deps, id);
    free(deps);
    if (result == VKRT_STAGE_OK) {
        schedule->nodes[*id].view.original_mask = signal->original_mask;
        schedule->nodes[*id].view.logical_stages = signal->logical_src_scope;
    }
    return result;
}

bool vkrt_stage_schedule_node(const struct vkrt_stage_schedule *schedule, vkrt_stage_id id, struct vkrt_stage_node *node) {
    if (!schedule || !node || id >= schedule->count) return false;
    *node = schedule->nodes[id].view;
    return true;
}

static bool ready(const struct vkrt_stage_schedule *schedule, const struct vkrt_stage_node *node) {
    if (node->kind == VKRT_STAGE_WAIT || node->completion_token) return false;
    for (size_t i = 0; i < node->dependency_count; ++i)
        if (!schedule->nodes[node->dependencies[i]].view.completion_token) return false;
    for (size_t i = 0; i < node->wait_count; ++i)
        if (!schedule->nodes[node->waits[i]].view.completion_token) return false;
    return true;
}

enum vkrt_stage_result vkrt_stage_schedule_acquire(struct vkrt_stage_schedule *schedule, vkrt_stage_id wait,
                                                   uint64_t acquire_token) {
    if (!schedule || wait >= schedule->count || !acquire_token || schedule->nodes[wait].view.kind != VKRT_STAGE_WAIT ||
        schedule->nodes[wait].view.completion_token)
        return VKRT_STAGE_INVALID;
    schedule->nodes[wait].view.completion_token = acquire_token;
    return VKRT_STAGE_OK;
}

enum vkrt_stage_result vkrt_stage_schedule_next(const struct vkrt_stage_schedule *schedule, vkrt_stage_id *id) {
    if (id) *id = VKRT_STAGE_ID_INVALID;
    if (!schedule || !id) return VKRT_STAGE_INVALID;
    for (size_t i = 0; i < schedule->count; ++i) {
        if (ready(schedule, &schedule->nodes[i].view)) {
            *id = i;
            return VKRT_STAGE_OK;
        }
    }
    return VKRT_STAGE_BLOCKED;
}

enum vkrt_stage_result vkrt_stage_schedule_issue(struct vkrt_stage_schedule *schedule, vkrt_stage_id id,
                                                 uint64_t completion_token) {
    if (!schedule || id >= schedule->count || !completion_token) return VKRT_STAGE_INVALID;
    if (!ready(schedule, &schedule->nodes[id].view)) return VKRT_STAGE_BLOCKED;
    schedule->nodes[id].view.completion_token = completion_token;
    return VKRT_STAGE_OK;
}
