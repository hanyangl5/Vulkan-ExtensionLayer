/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Standalone CPU integration of execution ownership and the typed journal.
 */
#include "vkrt_execution_replay.h"
#include "vkrt_layer.h"
#include "vkrt_query_pool.h"
#include "vkrt_rtp.h"

#define H(T, n) ((T)(uintptr_t)(n))
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            exit(1);                                        \
        }                                                   \
    } while (0)
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static struct vkrt_device_data device;
static const VkQueue queue_a = H(VkQueue, 400), queue_b = H(VkQueue, 401);
enum failure { FAIL_NONE, FAIL_FENCE, FAIL_POOL, FAIL_ALLOCATE, FAIL_LOADER, FAIL_BEGIN, FAIL_INHERIT, FAIL_RESOLVE, FAIL_END };
static enum failure failure;
static unsigned next_handle = 1000, payload_identity, frame_identity;
static unsigned creates, destroys, allocations, frees, fence_creates, fence_destroys, polls, inherited;
static bool teardown_tracking, teardown_proven;
static unsigned device_idle_calls, teardown_start_frees, teardown_start_destroys, teardown_start_fence_destroys;
static VkResult teardown_results[5];
static unsigned teardown_result_count, teardown_result_index;
static void teardown_cleanup_guard(void) { CHECK(!teardown_tracking || teardown_proven); }
struct command_record {
    struct command_record *next;
    struct vkrt_cmd_buffer_data *owner;
    bool internal, loader, begun, inherited, ended;
    unsigned payload, frame;
    uint32_t child_count;
    VkCommandBuffer children[8];
};
struct pool_record {
    struct pool_record *next;
    VkCommandPool handle;
    uint32_t family;
    VkCommandPoolCreateFlags flags;
    bool internal;
};
struct fence_record {
    struct fence_record *next;
    VkFence handle;
    bool complete, associated, pending, native_busy;
    VkQueue queue;
    unsigned sequence;
};
static struct command_record *commands;
static struct pool_record *pools;
static struct fence_record *fences;
static VkResult status_error;
static bool prefix_available;
static struct command_record *command(VkCommandBuffer handle) {
    for (struct command_record *p = commands; p; p = p->next)
        if (p->owner->cmd == handle) return p;
    return NULL;
}
static struct pool_record *pool(VkCommandPool handle) {
    for (struct pool_record *p = pools; p; p = p->next)
        if (p->handle == handle) return p;
    return NULL;
}
static struct fence_record *fence(VkFence handle) {
    for (struct fence_record *p = fences; p; p = p->next)
        if (p->handle == handle) return p;
    return NULL;
}
static struct pool_record *new_pool(uint32_t family, VkCommandPoolCreateFlags flags, bool internal) {
    struct pool_record *p = calloc(1, sizeof(*p));
    CHECK(p);
    *p = (struct pool_record){
        .next = pools, .handle = H(VkCommandPool, ++next_handle), .family = family, .flags = flags, .internal = internal};
    pools = p;
    if (!internal && device.executions) CHECK(vkrt_execution_track_command_pool(&device, p->handle, family, flags) == VK_SUCCESS);
    return p;
}
static VkResult init_execution(const VkAllocationCallbacks *allocator) {
    VkResult result = vkrt_execution_device_init(&device, allocator);
    if (result == VK_SUCCESS)
        for (const struct pool_record *p = pools; p; p = p->next)
            if (!p->internal) {
                result = vkrt_execution_track_command_pool(&device, p->handle, p->family, p->flags);
                if (result != VK_SUCCESS) break;
            }
    if (result != VK_SUCCESS) vkrt_execution_device_finish(&device);
    return result;
}
static struct command_record *new_command(struct pool_record *p, VkCommandBufferLevel level, bool internal, bool required) {
    struct command_record *record = calloc(1, sizeof(*record));
    CHECK(record);
    record->owner = calloc(1, sizeof(*record->owner));
    CHECK(record->owner);
    *record->owner = (struct vkrt_cmd_buffer_data){
        .cmd = H(VkCommandBuffer, ++next_handle), .device_data = &device, .pool = p->handle, .level = level};
    record->owner->replay.journal = vkrt_replay_journal_create(NULL, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    CHECK(record->owner->replay.journal);
    record->internal = internal;
    record->owner->has_virtual_queries = required;
    record->next = commands;
    commands = record;
    vkrt_map_object(VKRT_HKEY(record->owner->cmd), record->owner);
    return record;
}
static void delete_command(VkCommandBuffer handle) {
    struct command_record **link = &commands;
    while (*link && (*link)->owner->cmd != handle) link = &(*link)->next;
    CHECK(*link);
    struct command_record *record = *link;
    *link = record->next;
    vkrt_unmap_object(VKRT_HKEY(handle));
    vkrt_replay_journal_destroy(record->owner->replay.journal);
    free(record->owner);
    free(record);
}
static void destroy_native_pool(VkDevice dev, VkCommandPool handle) {
    teardown_cleanup_guard();
    CHECK(dev == device.device);
    struct pool_record **link = &pools;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    CHECK(*link && (*link)->internal);
    for (struct command_record *p = commands; p; p = p->next) CHECK(p->owner->pool != handle);
    struct pool_record *p = *link;
    *link = p->next;
    free(p);
    ++destroys;
}
static VKAPI_ATTR VkResult VKAPI_CALL make_pool(VkDevice dev, const VkCommandPoolCreateInfo *info,
                                                const VkAllocationCallbacks *allocator, VkCommandPool *out) {
    CHECK(dev == device.device && info->sType == VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    (void)allocator;
    if (failure == FAIL_POOL) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = new_pool(info->queueFamilyIndex, info->flags, true)->handle;
    ++creates;
    VkResult result = vkrt_execution_track_command_pool(&device, *out, info->queueFamilyIndex, info->flags);
    if (result != VK_SUCCESS) {
        /* Match the production Create hook: no metadata was acquired, so its
         * rollback calls native Destroy directly instead of forgetting. */
        destroy_native_pool(dev, *out);
        *out = VK_NULL_HANDLE;
    }
    return result;
}
static VKAPI_ATTR void VKAPI_CALL destroy_pool(VkDevice dev, VkCommandPool handle, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    uint32_t family;
    VkCommandPoolCreateFlags flags;
    CHECK(vkrt_execution_command_pool_info(&device, handle, &family, &flags));
    CHECK(pool(handle) && family == pool(handle)->family && flags == pool(handle)->flags);
    /* Every successfully created private replay pool reaches this ordinary
     * command hook, including discard, completion and device teardown. */
    vkrt_execution_forget_command_pool(&device, handle);
    CHECK(!vkrt_execution_command_pool_info(&device, handle, &family, &flags));
    destroy_native_pool(dev, handle);
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate_commands(VkDevice dev, const VkCommandBufferAllocateInfo *info,
                                                        VkCommandBuffer *out) {
    CHECK(dev == device.device && info->commandBufferCount == 1);
    if (failure == FAIL_ALLOCATE) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct pool_record *p = pool(info->commandPool);
    CHECK(p && p->internal);
    *out = new_command(p, info->level, true, true)->owner->cmd;
    ++allocations;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL free_commands(VkDevice dev, VkCommandPool handle, uint32_t count, const VkCommandBuffer *list) {
    teardown_cleanup_guard();
    CHECK(dev == device.device && count == 1 && pool(handle));
    CHECK(command(list[0])->internal && command(list[0])->owner->pool == handle);
    delete_command(list[0]);
    ++frees;
}
static VKAPI_ATTR VkResult VKAPI_CALL loader_data(VkDevice dev, void *object) {
    CHECK(dev == device.device);
    struct command_record *p = command((VkCommandBuffer)object);
    CHECK(p && p->internal && !p->begun);
    if (failure == FAIL_LOADER) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->loader = true;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL begin_command(VkCommandBuffer handle, const VkCommandBufferBeginInfo *info) {
    struct command_record *p = command(handle);
    CHECK(p && p->internal && p->loader);
    CHECK(p->owner->replaying && !p->owner->replay.journal && !p->inherited);
    CHECK((info->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) &&
          !(info->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT));
    if (p->owner->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
        CHECK(info->pInheritanceInfo && info->pInheritanceInfo->renderPass == H(VkRenderPass, 99));
    if (failure == FAIL_BEGIN) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    p->begun = true;
    p->owner->usage_flags = info->flags;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL end_command(VkCommandBuffer handle) {
    struct command_record *p = command(handle);
    CHECK(p && p->begun && p->inherited);
    if (failure == FAIL_END) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->ended = true;
    return p->owner->record_result;
}
VkResult vkrt_rtp_replay_layouts_inherit(struct vkrt_cmd_buffer_data *target, const struct vkrt_cmd_buffer_data *source) {
    struct command_record *p = command(target->cmd);
    CHECK(p && p->begun && !p->inherited && command(source->cmd));
    if (failure == FAIL_INHERIT) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->inherited = true;
    ++inherited;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL query_reset(VkCommandBuffer handle, VkQueryPool query, uint32_t first, uint32_t count) {
    struct command_record *p = command(handle);
    CHECK(p && query == H(VkQueryPool, 55) && first == 1 && count == 2);
    if (p->internal) {
        CHECK(p->begun && p->inherited && !p->payload);
        p->payload = ++payload_identity;
    } else {
        p->owner->has_virtual_queries = true;
    }
}
static VKAPI_ATTR void VKAPI_CALL trace(VkCommandBuffer handle, const VkStridedDeviceAddressRegionKHR *raygen,
                                        const VkStridedDeviceAddressRegionKHR *miss, const VkStridedDeviceAddressRegionKHR *hit,
                                        const VkStridedDeviceAddressRegionKHR *callable, uint32_t x, uint32_t y, uint32_t z) {
    struct command_record *p = command(handle);
    CHECK(p && raygen->deviceAddress == 0x1000 && !miss->size && !hit->size && !callable->size && x == 1 && y == 1 && z == 1);
    if (p->internal) CHECK(p->begun && p->inherited && !p->frame);
    if (p->internal)
        p->frame = ++frame_identity;
    else
        p->owner->has_rtp_execution = true;
}
static VKAPI_ATTR void VKAPI_CALL execute_commands(VkCommandBuffer handle, uint32_t count, const VkCommandBuffer *children) {
    struct command_record *p = command(handle);
    CHECK(p && count <= 8);
    p->child_count = count;
    memcpy(p->children, children, count * sizeof(*children));
    if (p->internal) {
        CHECK(p->inherited);
        for (uint32_t i = 0; i < count; ++i) {
            struct command_record *child = command(children[i]);
            CHECK(child);
            if (child->internal) CHECK(child->ended);
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            struct command_record *child = command(children[i]);
            CHECK(child);
            p->owner->has_virtual_queries |= child->owner->has_virtual_queries;
            p->owner->has_rtp_execution |= child->owner->has_rtp_execution;
        }
    }
}
static VKAPI_ATTR void VKAPI_CALL opaque(VkCommandBuffer handle, VkDescriptorUpdateTemplate update, VkPipelineLayout layout,
                                         uint32_t set, const void *data) {
    CHECK(command(handle));
    (void)update;
    (void)layout;
    (void)set;
    (void)data;
}
PFN_vkVoidFunction vkrt_device_direct_proc(struct vkrt_device_data *dev, const char *name) {
    CHECK(dev == &device);
    if (failure == FAIL_RESOLVE) return NULL;
    if (!strcmp(name, "vkCmdResetQueryPool")) return (PFN_vkVoidFunction)query_reset;
    if (!strcmp(name, "vkCmdTraceRaysKHR")) return (PFN_vkVoidFunction)trace;
    if (!strcmp(name, "vkCmdExecuteCommands")) return (PFN_vkVoidFunction)execute_commands;
    if (!strcmp(name, "vkCmdPushDescriptorSetWithTemplateKHR")) return (PFN_vkVoidFunction)opaque;
    return NULL;
}
static PFN_vkVoidFunction resolve_record(void *user, const char *name) {
    return vkrt_device_direct_proc(((struct vkrt_cmd_buffer_data *)user)->device_data, name);
}
struct vkrt_replay_context *vkrt_replay_lookup_context(VkCommandBuffer handle) {
    struct command_record *p = command(handle);
    CHECK(p);
    p->owner->replay.resolve = resolve_record;
    p->owner->replay.user = p->owner;
    return &p->owner->replay;
}
PFN_vkVoidFunction vkrt_command_proc(const char *name) {
    if (!strcmp(name, "vkCreateCommandPool")) return (PFN_vkVoidFunction)make_pool;
    if (!strcmp(name, "vkAllocateCommandBuffers")) return (PFN_vkVoidFunction)allocate_commands;
    if (!strcmp(name, "vkFreeCommandBuffers")) return (PFN_vkVoidFunction)free_commands;
    if (!strcmp(name, "vkDestroyCommandPool")) return (PFN_vkVoidFunction)destroy_pool;
    if (!strcmp(name, "vkBeginCommandBuffer")) return (PFN_vkVoidFunction)begin_command;
    if (!strcmp(name, "vkEndCommandBuffer")) return (PFN_vkVoidFunction)end_command;
    return NULL;
}
static VkResult observer_error;
static unsigned observations;
static void (*observer_hook)(void);
static VKAPI_ATTR VkResult VKAPI_CALL make_fence(VkDevice dev, const VkFenceCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator, VkFence *out) {
    CHECK(dev == device.device && info->sType == VK_STRUCTURE_TYPE_FENCE_CREATE_INFO && !info->pNext && !info->flags);
    (void)allocator;
    if (failure == FAIL_FENCE) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct fence_record *p = calloc(1, sizeof(*p));
    CHECK(p);
    *p = (struct fence_record){.next = fences, .handle = H(VkFence, ++next_handle)};
    fences = p;
    *out = p->handle;
    ++fence_creates;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_fence(VkDevice dev, VkFence handle, const VkAllocationCallbacks *allocator) {
    teardown_cleanup_guard();
    CHECK(dev == device.device);
    (void)allocator;
    struct fence_record **link = &fences;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    CHECK(*link && !(*link)->native_busy && (!(*link)->pending || (*link)->complete));
    struct fence_record *p = *link;
    *link = p->next;
    free(p);
    ++fence_destroys;
}
static VKAPI_ATTR VkResult VKAPI_CALL fence_status(VkDevice dev, VkFence handle) {
    CHECK(dev == device.device);
    struct fence_record *p = fence(handle);
    CHECK(p && p->associated && !p->native_busy);
    ++polls;
    if (status_error) return status_error;
    if (!p->complete) return VK_NOT_READY;
    /* A later fence proves completion of preceding same-queue observer uses.
     * Their native API calls can STILL be in flight, checked independently. */
    for (struct fence_record *q = fences; q; q = q->next)
        if (q->queue == p->queue && q->sequence <= p->sequence) q->complete = true;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL observe(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence handle) {
    CHECK((queue == queue_a || queue == queue_b) && count == 0 && !submits);
    struct fence_record *p = fence(handle);
    CHECK(p && !p->sequence);
    VkResult result = observer_error;
    observer_error = VK_SUCCESS;
    p->queue = queue;
    p->sequence = ++observations;
    p->native_busy = true;
    p->associated = result == VK_SUCCESS;
    p->pending = result == VK_SUCCESS || result == VK_ERROR_UNKNOWN;
    if (observer_hook) {
        void (*hook)(void) = observer_hook;
        observer_hook = NULL;
        hook();
    }
    CHECK(fence(handle) == p); /* No observer may retire its in-flight API use. */
    p->native_busy = false;
    return result;
}
static void native_idle(VkQueue queue) {
    for (struct fence_record *p = fences; p; p = p->next)
        if (!queue || p->queue == queue) p->complete = true;
}
static VKAPI_ATTR VkResult VKAPI_CALL device_wait_idle(VkDevice dev) {
    CHECK(dev == device.device);
    ++device_idle_calls;
    if (teardown_tracking) {
        CHECK(frees == teardown_start_frees && destroys == teardown_start_destroys &&
              fence_destroys == teardown_start_fence_destroys);
        CHECK(teardown_result_index < teardown_result_count);
    }
    VkResult result = teardown_tracking ? teardown_results[teardown_result_index++] : VK_SUCCESS;
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) {
        native_idle(VK_NULL_HANDLE);
        teardown_proven = true;
    }
    return result;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_proc(VkDevice dev, const char *name) {
    CHECK(dev == device.device);
    if (!strcmp(name, "vkCreateFence")) return (PFN_vkVoidFunction)make_fence;
    if (!strcmp(name, "vkDestroyFence")) return (PFN_vkVoidFunction)destroy_fence;
    if (!strcmp(name, "vkGetFenceStatus")) return (PFN_vkVoidFunction)fence_status;
    if (!strcmp(name, "vkQueueSubmit")) return (PFN_vkVoidFunction)observe;
    if (!strcmp(name, "vkDeviceWaitIdle")) return (PFN_vkVoidFunction)device_wait_idle;
    return NULL;
}
static void begin_source(struct command_record *p) {
    p->owner->has_virtual_queries = false;
    p->owner->has_rtp_execution = false;
    VkCommandBufferInheritanceInfo inheritance = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
                                                  .renderPass = H(VkRenderPass, 99)};
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    if (p->owner->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
        info.flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        info.pInheritanceInfo = &inheritance;
    }
    CHECK(vkrt_replay_journal_begin(p->owner->replay.journal, &info) == VK_SUCCESS);
}
static void record_query(struct command_record *p) {
    ((PFN_vkCmdResetQueryPool)vkrt_replay_proc("vkCmdResetQueryPool"))(p->owner->cmd, H(VkQueryPool, 55), 1, 2);
}
static void record_trace(struct command_record *p) {
    VkStridedDeviceAddressRegionKHR raygen = {.deviceAddress = 0x1000, .stride = 64, .size = 64}, empty = {0};
    ((PFN_vkCmdTraceRaysKHR)vkrt_replay_proc("vkCmdTraceRaysKHR"))(p->owner->cmd, &raygen, &empty, &empty, &empty, 1, 1, 1);
}
static void record_children(struct command_record *p, uint32_t count, const VkCommandBuffer *children) {
    ((PFN_vkCmdExecuteCommands)vkrt_replay_proc("vkCmdExecuteCommands"))(p->owner->cmd, count, children);
}
static void check_balanced(void) {
    CHECK(creates == destroys && allocations == frees && fence_creates == fence_destroys);
    CHECK(!fences);
}
static void test_rtp_without_queries(struct pool_record *application_pool) {
    CHECK(!device.queries && !device.query_backend_enabled);
    struct command_record *source = new_command(application_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false, false);
    begin_source(source);
    record_trace(source);
    source->owner->native_active_queries = 1; /* RTP alone does not enter query qualification. */
    CHECK(source->owner->has_rtp_execution && !source->owner->has_virtual_queries && !source->frame);
    CHECK(vkrt_execution_command_requires_replay(source->owner));
    struct vkrt_execution_batch *batch;
    CHECK(vkrt_execution_batch_create(&device, queue_a, &batch) == VK_SUCCESS);
    VkCommandBuffer first, second;
    CHECK(vkrt_execution_batch_clone(batch, source->owner->cmd, &first) == VK_SUCCESS && first != source->owner->cmd);
    CHECK(vkrt_execution_batch_clone(batch, source->owner->cmd, &second) == VK_SUCCESS && first != second);
    CHECK(command(first)->frame && command(second)->frame && command(first)->frame != command(second)->frame);
    CHECK(!vkrt_execution_command_requires_replay(command(first)->owner));
    VkFence completion = vkrt_execution_batch_completion_fence(batch);
    CHECK(vkrt_execution_batch_submitted(batch) == VK_SUCCESS);
    size_t retired;
    CHECK(vkrt_execution_collect(&device, &retired) == VK_SUCCESS && retired == 0 && command(first) && command(second));
    fence(completion)->complete = true;
    CHECK(vkrt_execution_collect(&device, &retired) == VK_SUCCESS && retired == 1 && !command(first) && !command(second));
    source->owner->native_active_queries = 0;
    delete_command(source->owner->cmd);
    check_balanced();
}
static void test_generic_pool_metadata(struct pool_record *application_pool) {
    uint32_t family = UINT32_MAX;
    VkCommandPoolCreateFlags flags = UINT32_MAX;
    CHECK(!device.queries && vkrt_execution_command_pool_info(&device, application_pool->handle, &family, &flags));
    CHECK(family == application_pool->family && flags == application_pool->flags);
    CHECK(vkrt_execution_track_command_pool(&device, application_pool->handle, family, flags) == VK_SUCCESS);
    CHECK(vkrt_execution_track_command_pool(&device, application_pool->handle, family + 1, flags) ==
          VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkrt_execution_command_pool_info(&device, application_pool->handle, &family, &flags) &&
          family == application_pool->family);
    vkrt_execution_forget_command_pool(&device, application_pool->handle);
    family = UINT32_MAX;
    CHECK(!vkrt_execution_command_pool_info(&device, application_pool->handle, &family, &flags) && family == UINT32_MAX);
    CHECK(vkrt_execution_track_command_pool(&device, application_pool->handle, application_pool->family, application_pool->flags) ==
          VK_SUCCESS);
}
static void test_occurrences(struct command_record *primary, struct command_record *secondary, struct command_record *plain) {
    VkCommandBuffer children[] = {secondary->owner->cmd, secondary->owner->cmd, plain->owner->cmd};
    record_children(primary, 3, children);
    struct vkrt_execution_batch *batch;
    CHECK(vkrt_execution_batch_create(&device, queue_a, &batch) == VK_SUCCESS);
    VkFence completion = vkrt_execution_batch_completion_fence(batch);
    CHECK(completion);
    VkCommandBuffer first, second;
    CHECK(vkrt_execution_batch_clone(batch, primary->owner->cmd, &first) == VK_SUCCESS);
    CHECK(vkrt_execution_batch_clone(batch, primary->owner->cmd, &second) == VK_SUCCESS && first != second);
    CHECK(vkrt_execution_batch_clone_count(batch) == 8 && vkrt_execution_batch_status(batch) == VK_SUCCESS);
    struct command_record *a = command(first), *b = command(second);
    CHECK(a->children[0] != a->children[1] && a->children[0] != b->children[0] && a->children[1] != b->children[1]);
    CHECK(command(a->children[0])->payload != command(a->children[1])->payload);
    CHECK(command(a->children[0])->payload != command(b->children[0])->payload);
    CHECK(a->children[2] != plain->owner->cmd && b->children[2] != plain->owner->cmd && a->children[2] != b->children[2]);
    CHECK(command(a->children[2])->frame != plain->frame && command(b->children[2])->frame != plain->frame);
    CHECK(command(a->children[2])->frame != command(b->children[2])->frame);
    CHECK(creates - destroys == 1 && allocations - frees == 8);
    for (size_t i = 0; i < 8; ++i) CHECK(command(vkrt_execution_batch_clone_command(batch, i))->internal);
    CHECK(!vkrt_execution_batch_clone_command(batch, 8));
    CHECK(vkrt_execution_batch_submitted(batch) == VK_SUCCESS);
    CHECK(vkrt_execution_batch_discard(batch) == VK_NOT_READY);
    prefix_available = true; /* Per-op availability cannot retire a command buffer. */
    size_t retired = 99;
    CHECK(vkrt_execution_collect(&device, &retired) == VK_SUCCESS && retired == 0 && prefix_available);
    CHECK(command(first) && command(second) && allocations - frees == 8);
    status_error = VK_ERROR_DEVICE_LOST;
    CHECK(vkrt_execution_collect(&device, &retired) == VK_ERROR_DEVICE_LOST && retired == 0 && command(first));
    status_error = VK_SUCCESS;
    fence(completion)->complete = true;
    CHECK(vkrt_execution_collect(&device, &retired) == VK_SUCCESS && retired == 1);
    CHECK(!command(first) && !command(second));
    check_balanced();
}
static VkCommandBuffer accepted(struct command_record *source, VkQueue queue, VkResult observation_result, VkFence *completion) {
    struct vkrt_execution_batch *batch;
    CHECK(vkrt_execution_batch_create(&device, queue, &batch) == VK_SUCCESS);
    VkCommandBuffer target;
    CHECK(vkrt_execution_batch_clone(batch, source->owner->cmd, &target) == VK_SUCCESS);
    *completion = vkrt_execution_batch_completion_fence(batch);
    observer_error = observation_result;
    CHECK(vkrt_execution_batch_submitted(batch) ==
          (observation_result == VK_ERROR_DEVICE_LOST ? VK_ERROR_DEVICE_LOST : VK_SUCCESS));
    return target;
}
static void test_observation_failures(struct command_record *source) {
    VkFence failed_fences[3], other_fence, good_fence;
    const VkResult errors[] = {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN};
    VkCommandBuffer failed[3];
    for (unsigned i = 0; i < 3; ++i) failed[i] = accepted(source, queue_a, errors[i], &failed_fences[i]);
    VkCommandBuffer other = accepted(source, queue_b, VK_ERROR_OUT_OF_HOST_MEMORY, &other_fence);
    size_t retired = 99;
    unsigned before_polls = polls;
    CHECK(vkrt_execution_collect(&device, &retired) == VK_SUCCESS && retired == 0 && polls == before_polls);
    VkCommandBuffer good = accepted(source, queue_a, VK_SUCCESS, &good_fence);
    fence(good_fence)->complete = true;
    CHECK(vkrt_execution_collect(&device, &retired) == VK_SUCCESS && retired == 4);
    CHECK(!command(good) && command(other));
    for (unsigned i = 0; i < 3; ++i) CHECK(!command(failed[i]) && !fence(failed_fences[i]));
    uint64_t checkpoint = vkrt_execution_checkpoint(&device);
    native_idle(queue_a);
    vkrt_execution_queue_completed(&device, queue_a, checkpoint);
    CHECK(command(other));
    native_idle(queue_b);
    vkrt_execution_queue_completed(&device, queue_b, checkpoint);
    CHECK(!command(other) && !fence(other_fence));
    check_balanced();
}
static void test_idle_checkpoint_and_quarantine(struct command_record *source) {
    VkFence a_fence, b_fence;
    VkCommandBuffer a = accepted(source, queue_a, VK_ERROR_OUT_OF_HOST_MEMORY, &a_fence);
    uint64_t checkpoint = vkrt_execution_checkpoint(&device);
    native_idle(queue_a);
    VkCommandBuffer b = accepted(source, queue_a, VK_ERROR_OUT_OF_HOST_MEMORY, &b_fence);
    vkrt_execution_queue_completed(&device, queue_a, checkpoint);
    CHECK(!command(a) && command(b));
    checkpoint = vkrt_execution_checkpoint(&device);
    native_idle(NULL);
    vkrt_execution_queue_completed(&device, VK_NULL_HANDLE, checkpoint);
    CHECK(!command(b));
    struct vkrt_execution_batch *quarantine;
    CHECK(vkrt_execution_batch_create(&device, queue_a, &quarantine) == VK_SUCCESS);
    VkCommandBuffer unknown;
    CHECK(vkrt_execution_batch_clone(quarantine, source->owner->cmd, &unknown) == VK_SUCCESS);
    unsigned before_observations = observations;
    vkrt_execution_batch_quarantine(quarantine);
    CHECK(observations == before_observations);
    VkFence good_fence;
    VkCommandBuffer good = accepted(source, queue_a, VK_SUCCESS, &good_fence);
    fence(good_fence)->complete = true;
    size_t retired;
    CHECK(vkrt_execution_collect(&device, &retired) == VK_SUCCESS && retired == 1 && !command(good) && command(unknown));
    checkpoint = vkrt_execution_checkpoint(&device);
    native_idle(queue_a);
    vkrt_execution_queue_completed(&device, queue_a, checkpoint);
    CHECK(!command(unknown));
    VkFence lost_fence;
    VkCommandBuffer lost = accepted(source, queue_a, VK_ERROR_DEVICE_LOST, &lost_fence);
    CHECK(command(lost) && fence(lost_fence));
    CHECK(vkrt_execution_collect(&device, &retired) == VK_SUCCESS && retired == 0);
    checkpoint = vkrt_execution_checkpoint(&device);
    native_idle(queue_a);
    vkrt_execution_queue_completed(&device, queue_a, checkpoint);
    check_balanced();
}
static struct {
    struct command_record *source;
    VkCommandBuffer outer;
    VkFence outer_fence;
    uint64_t checkpoint;
} busy;
static void busy_observation_hook(void) {
    CHECK(fence(busy.outer_fence)->native_busy);
    VkFence inner_fence;
    VkCommandBuffer inner = accepted(busy.source, queue_a, VK_SUCCESS, &inner_fence);
    fence(inner_fence)->complete = true;
    size_t retired;
    CHECK(vkrt_execution_collect(&device, &retired) == VK_SUCCESS && retired == 1);
    CHECK(!command(inner) && command(busy.outer) && fence(busy.outer_fence)->native_busy);
    busy.checkpoint = vkrt_execution_checkpoint(&device);
    native_idle(queue_a);
    vkrt_execution_queue_completed(&device, queue_a, busy.checkpoint);
    CHECK(command(busy.outer));
}
static void test_inflight_observer(struct command_record *source) {
    struct vkrt_execution_batch *outer;
    CHECK(vkrt_execution_batch_create(&device, queue_a, &outer) == VK_SUCCESS);
    busy.source = source;
    busy.outer_fence = vkrt_execution_batch_completion_fence(outer);
    CHECK(vkrt_execution_batch_clone(outer, source->owner->cmd, &busy.outer) == VK_SUCCESS);
    observer_hook = busy_observation_hook;
    CHECK(vkrt_execution_batch_submitted(outer) == VK_SUCCESS);
    vkrt_execution_queue_completed(&device, queue_a, busy.checkpoint);
    CHECK(command(busy.outer) && !fence(busy.outer_fence)->native_busy);
    size_t retired;
    CHECK(vkrt_execution_collect(&device, &retired) == VK_SUCCESS && retired == 1 && !command(busy.outer));
    check_balanced();
}
static void teardown_expect(const VkResult *results, unsigned count) {
    CHECK(count && count <= 5);
    memcpy(teardown_results, results, count * sizeof(*results));
    teardown_result_count = count;
    teardown_result_index = 0;
    teardown_start_frees = frees;
    teardown_start_destroys = destroys;
    teardown_start_fence_destroys = fence_destroys;
    teardown_proven = false;
    teardown_tracking = true;
}
static void teardown_finish(unsigned before, unsigned expected_calls) {
    vkrt_execution_device_finish(&device);
    CHECK(device_idle_calls == before + expected_calls && teardown_proven && !device.executions);
    CHECK(teardown_result_index == teardown_result_count);
    teardown_tracking = false;
    check_balanced();
    CHECK(init_execution(NULL) == VK_SUCCESS);
}
static void test_device_teardown(struct command_record *source) {
    CHECK(device_idle_calls == 0); /* Normal collect/observation never drains. */
    unsigned before = device_idle_calls;
    vkrt_execution_device_finish(&device);
    CHECK(device_idle_calls == before && !device.executions);
    CHECK(init_execution(NULL) == VK_SUCCESS);
    struct vkrt_execution_batch *prepared;
    CHECK(vkrt_execution_batch_create(&device, queue_a, &prepared) == VK_SUCCESS);
    VkCommandBuffer target;
    CHECK(vkrt_execution_batch_clone(prepared, source->owner->cmd, &target) == VK_SUCCESS);
    vkrt_execution_device_finish(&device);
    CHECK(device_idle_calls == before && !device.executions);
    check_balanced(); /* Prepared-only objects are not pending. */
    CHECK(init_execution(NULL) == VK_SUCCESS);
    /* Accepted work needs a drain even when observer OOM left an unassociated
     * fence. Every retry checks that no CB/pool/fence cleanup happened early. */
    VkFence completion;
    (void)accepted(source, queue_a, VK_ERROR_OUT_OF_HOST_MEMORY, &completion);
    const VkResult retry[] = {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_UNKNOWN, VK_ERROR_UNKNOWN,
                              VK_SUCCESS};
    teardown_expect(retry, 5);
    teardown_finish(before, 5);
    before = device_idle_calls;
    /* Caller-fence completion does not complete the hidden observer. */
    (void)accepted(source, queue_a, VK_SUCCESS, &completion);
    CHECK(fence(completion)->pending && !fence(completion)->complete);
    const VkResult lost[] = {VK_ERROR_DEVICE_LOST};
    teardown_expect(lost, 1);
    teardown_finish(before, 1);
    before = device_idle_calls;
    CHECK(vkrt_execution_batch_create(&device, queue_a, &prepared) == VK_SUCCESS);
    CHECK(vkrt_execution_batch_clone(prepared, source->owner->cmd, &target) == VK_SUCCESS);
    vkrt_execution_batch_quarantine(prepared);
    const VkResult success[] = {VK_SUCCESS};
    teardown_expect(success, 1);
    teardown_finish(before, 1); /* Quarantine also requires the drain. */
}
#ifdef VKRT_EXECUTION_SETTLED_LIMIT
static void test_clock_saturation(struct command_record *source) {
    for (unsigned i = 0; i < VKRT_EXECUTION_SETTLED_LIMIT; ++i) {
        VkFence unused;
        (void)accepted(source, queue_a, VK_ERROR_OUT_OF_HOST_MEMORY, &unused);
    }
    struct vkrt_execution_batch *blocked = (void *)(uintptr_t)1;
    CHECK(vkrt_execution_batch_create(&device, queue_a, &blocked) == VK_ERROR_OUT_OF_HOST_MEMORY && !blocked);
    uint64_t checkpoint = vkrt_execution_checkpoint(&device);
    CHECK(checkpoint == VKRT_EXECUTION_SETTLED_LIMIT);
    native_idle(queue_a);
    vkrt_execution_queue_completed(&device, queue_a, checkpoint);
    CHECK(vkrt_execution_batch_create(&device, queue_a, &blocked) == VK_ERROR_OUT_OF_HOST_MEMORY && !blocked);
    check_balanced();
    vkrt_execution_device_finish(&device);
}
#endif
static void test_failure_paths(struct command_record *source) {
    for (enum failure point = FAIL_FENCE; point <= FAIL_END; ++point) {
        failure = point;
        struct vkrt_execution_batch *batch = NULL;
        VkResult result = vkrt_execution_batch_create(&device, queue_a, &batch);
        if (point == FAIL_FENCE)
            CHECK(result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !batch);
        else {
            CHECK(result == VK_SUCCESS && batch);
            VkCommandBuffer target = H(VkCommandBuffer, 1);
            result = vkrt_execution_batch_clone(batch, source->owner->cmd, &target);
            CHECK(result < VK_SUCCESS && !target && vkrt_execution_batch_status(batch) == result);
            CHECK(vkrt_execution_batch_clone(batch, source->owner->cmd, &target) == result && !target);
            CHECK(vkrt_execution_batch_discard(batch) == VK_SUCCESS);
        }
        failure = FAIL_NONE;
        check_balanced();
    }
    PFN_vkSetDeviceLoaderData saved_loader = device.set_loader_data;
    device.set_loader_data = NULL;
    struct vkrt_execution_batch *batch;
    CHECK(vkrt_execution_batch_create(&device, queue_a, &batch) == VK_SUCCESS);
    VkCommandBuffer target;
    CHECK(vkrt_execution_batch_clone(batch, source->owner->cmd, &target) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(vkrt_execution_batch_discard(batch) == VK_SUCCESS);
    device.set_loader_data = saved_loader;
    check_balanced();
}
static void test_pool_identity(struct command_record *source) {
    struct pool_record *other_pool =
        new_pool(7, VK_COMMAND_POOL_CREATE_PROTECTED_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, false);
    struct command_record *other = new_command(other_pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, false, true);
    begin_source(other);
    record_query(other);
    struct vkrt_execution_batch *batch;
    CHECK(vkrt_execution_batch_create(&device, queue_a, &batch) == VK_SUCCESS);
    VkCommandBuffer a, b;
    CHECK(vkrt_execution_batch_clone(batch, source->owner->cmd, &a) == VK_SUCCESS);
    CHECK(vkrt_execution_batch_clone(batch, other->owner->cmd, &b) == VK_SUCCESS);
    struct pool_record *first = pool(command(a)->owner->pool), *second = pool(command(b)->owner->pool);
    CHECK(first != second && first->family == 3 && second->family == 7);
    CHECK(second->flags == (VK_COMMAND_POOL_CREATE_PROTECTED_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT));
    CHECK(vkrt_execution_batch_discard(batch) == VK_SUCCESS);
    check_balanced();
    VkCommandPool original = source->owner->pool;
    source->owner->pool = H(VkCommandPool, 1);
    CHECK(vkrt_execution_batch_create(&device, queue_a, &batch) == VK_SUCCESS);
    CHECK(vkrt_execution_batch_clone(batch, source->owner->cmd, &a) == VK_ERROR_FEATURE_NOT_PRESENT && !a);
    CHECK(vkrt_execution_batch_discard(batch) == VK_SUCCESS);
    source->owner->pool = original;
    check_balanced();
}
static void test_admission_and_cycle(struct command_record *plain, struct command_record *primary,
                                     struct command_record *secondary) {
    plain->owner->has_rtp_execution = false;
    ((PFN_vkCmdPushDescriptorSetWithTemplateKHR)vkrt_replay_proc("vkCmdPushDescriptorSetWithTemplateKHR"))(
        plain->owner->cmd, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, (void *)(uintptr_t)1);
    CHECK(vkrt_replay_journal_status(plain->owner->replay.journal) == VK_ERROR_FEATURE_NOT_PRESENT);
    struct vkrt_execution_batch *batch;
    CHECK(vkrt_execution_batch_create(&device, queue_a, &batch) == VK_SUCCESS);
    VkCommandBuffer target;
    CHECK(vkrt_execution_batch_clone(batch, plain->owner->cmd, &target) == VK_SUCCESS && target == plain->owner->cmd);
    plain->owner->has_virtual_queries = true;
    CHECK(vkrt_execution_batch_clone(batch, plain->owner->cmd, &target) == VK_ERROR_FEATURE_NOT_PRESENT && !target);
    CHECK(vkrt_execution_batch_clone_count(batch) == 0 && vkrt_execution_batch_discard(batch) == VK_SUCCESS);
    plain->owner->has_virtual_queries = false;
    begin_source(primary);
    record_trace(primary);
    VkCommandBuffer unsupported_child[] = {plain->owner->cmd};
    record_children(primary, 1, unsupported_child);
    CHECK(vkrt_execution_batch_create(&device, queue_a, &batch) == VK_SUCCESS);
    CHECK(vkrt_execution_batch_clone(batch, primary->owner->cmd, &target) == VK_ERROR_FEATURE_NOT_PRESENT && !target);
    CHECK(vkrt_execution_batch_clone_count(batch) == 1 && vkrt_execution_batch_discard(batch) == VK_SUCCESS);
    begin_source(primary);
    begin_source(secondary);
    record_query(secondary);
    VkCommandBuffer recursive[] = {secondary->owner->cmd};
    record_children(primary, 1, recursive);
    record_children(secondary, 1, recursive);
    CHECK(vkrt_execution_batch_create(&device, queue_a, &batch) == VK_SUCCESS);
    CHECK(vkrt_execution_batch_clone(batch, primary->owner->cmd, &target) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(vkrt_execution_batch_clone_count(batch) == 2);
    CHECK(vkrt_execution_batch_discard(batch) == VK_SUCCESS);
    check_balanced();
}
struct allocation_state {
    unsigned live, calls, fail_after;
};
static VKAPI_ATTR void *VKAPI_CALL host_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    struct allocation_state *state = user;
    (void)alignment;
    CHECK(scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (state->calls++ >= state->fail_after) return NULL;
    void *p = malloc(size);
    if (p) ++state->live;
    return p;
}
static VKAPI_ATTR void VKAPI_CALL host_free(void *user, void *memory) {
    struct allocation_state *state = user;
    if (memory) {
        CHECK(state->live);
        --state->live;
        free(memory);
    }
}
static void test_host_oom(struct command_record *source) {
    vkrt_execution_device_finish(&device);
    for (unsigned fail = 0; fail < 24; ++fail) {
        struct allocation_state state = {.fail_after = fail};
        VkAllocationCallbacks allocator = {.pUserData = &state, .pfnAllocation = host_allocate, .pfnFree = host_free};
        VkResult result = init_execution(&allocator);
        if (result == VK_SUCCESS) {
            struct vkrt_execution_batch *batch = NULL;
            result = vkrt_execution_batch_create(&device, queue_a, &batch);
            if (result == VK_SUCCESS) {
                VkCommandBuffer target;
                result = vkrt_execution_batch_clone(batch, source->owner->cmd, &target);
                CHECK(result == VK_SUCCESS || result == VK_ERROR_OUT_OF_HOST_MEMORY);
                CHECK(vkrt_execution_batch_discard(batch) == VK_SUCCESS);
            } else
                CHECK(result == VK_ERROR_OUT_OF_HOST_MEMORY);
            vkrt_execution_device_finish(&device);
        } else
            CHECK(result == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(!state.live && !device.executions);
        check_balanced();
    }
}
int main(void) {
    device.device = H(VkDevice, 42);
    device.virtual_rt = true;
    device.vtable.GetDeviceProcAddr = get_proc;
    device.set_loader_data = loader_data;
    vkrt_map_object(VKRT_HKEY(device.device), &device);
    CHECK(init_execution(NULL) == VK_SUCCESS);
    struct pool_record *application_pool = new_pool(3, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, false);
    struct command_record *primary = new_command(application_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false, true);
    struct command_record *secondary = new_command(application_pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, false, true);
    struct command_record *plain = new_command(application_pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, false, false);
    begin_source(primary);
    begin_source(secondary);
    begin_source(plain);
    record_query(secondary);
    record_trace(plain);
#ifdef VKRT_EXECUTION_SETTLED_LIMIT
    test_clock_saturation(secondary);
#else
    test_generic_pool_metadata(application_pool);
    test_rtp_without_queries(application_pool);
    test_occurrences(primary, secondary, plain);
    test_observation_failures(secondary);
    test_idle_checkpoint_and_quarantine(secondary);
    test_inflight_observer(secondary);
    test_device_teardown(secondary);
    test_failure_paths(secondary);
    test_pool_identity(secondary);
    test_admission_and_cycle(plain, primary, secondary);
    begin_source(secondary);
    record_query(secondary);
    begin_source(primary);
    VkCommandBuffer children[] = {secondary->owner->cmd, secondary->owner->cmd};
    record_children(primary, 2, children);
    test_host_oom(primary);
    CHECK(polls >= 3 && inherited >= 6);
#endif
    while (commands) delete_command(commands->owner->cmd);
    while (pools) {
        struct pool_record *next = pools->next;
        CHECK(!pools->internal);
        free(pools);
        pools = next;
    }
    vkrt_unmap_object(VKRT_HKEY(device.device));
#ifdef VKRT_EXECUTION_SETTLED_LIMIT
    puts("execution replay host: settled clock saturation/closed admission/idle cleanup passed");
#else
    puts(
        "execution replay host: fresh occurrences/secondary identity/private observation/settled cutoff/idle "
        "checkpoint/quarantine/teardown drain/rollback/OOM "
        "passed");
#endif
    return 0;
}
