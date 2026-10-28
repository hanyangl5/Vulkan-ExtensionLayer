/* SPDX-License-Identifier: MIT
 * CPU integration of the real submit translation with controlled query and
 * execution helpers. No production sync source is included or replaced.
 */
#include "vkrt_layer.h"
#include "vkrt_query_pool.h"
#include "vkrt_execution_replay.h"

#define H(T, n) ((T)(uintptr_t)(n))
#define CHECK(test)                                            \
    do {                                                       \
        if (!(test)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #test); \
            exit(1);                                           \
        }                                                      \
    } while (0)
enum { SOURCE_BATCHES = 2, MAX_COMMANDS = 8, EVENT_CAPACITY = 128 };
enum event {
    COLLECT,
    BEGIN,
    END,
    CREATE,
    CLONE,
    PREPARE,
    NATIVE,
    QUERY_RESULT,
    DISCARD,
    SUBMITTED,
    OBSERVER,
    QUARANTINE,
    CHECKPOINT,
    NATIVE_IDLE,
    COMPLETED
};
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static struct vkrt_device_data device;
static struct vkrt_cmd_buffer_data source[4];
static const VkQueue queue = H(VkQueue, 2);
static const VkFence caller_fence = H(VkFence, 3);
static const VkFence completion = H(VkFence, 4);
static const VkQueue other_queue = H(VkQueue, 5);
static unsigned event_count, cases;
static enum event events[EVENT_CAPACITY];
static unsigned collects, creates, clones, fresh, prepares, natives, observers, results, discards, submitted, quarantined;
static unsigned checkpoints, completions;
static unsigned begins, ends;
static unsigned fail_clone, fail_prepare;
static VkResult collect_result, begin_result, create_result, native_result, observer_result, idle_result, last_query_result;
static VkResult clone_failure_result;
static bool use_submit2, query_sources, rtp_sources, helper_enabled, zero_batches, wait_only, prepared_active, caller_has_fence;
static bool admission_active;
static VkCommandBuffer emitted[MAX_COMMANDS], prepared[MAX_COMMANDS];
static size_t emitted_count, prepared_count;
static uint64_t settled_clock, idle_checkpoint, completed_checkpoint;
static VkQueue completed_queue;
static unsigned retired;
static bool idle_probe;
static struct {
    VkQueue queue;
    uint64_t stamp;
    bool settled, retired;
} retention[4];

struct vkrt_execution_batch {
    bool created, pending, discarded, quarantined;
};
static struct vkrt_execution_batch batch;
struct payload {
    VkStructureType sType;
    const void *pNext;
    uint64_t sentinel;
};
static struct {
    struct payload outer[2], semaphore_next[4], command_next[MAX_COMMANDS];
    VkSemaphore waits[2][2], signals[2][2];
    VkPipelineStageFlags masks[2][2];
    uint64_t wait_values[2][2], signal_values[2][2];
    VkTimelineSemaphoreSubmitInfo timelines[2];
    VkCommandBuffer commands[2][3];
    VkSubmitInfo legacy[2];
    VkSemaphoreSubmitInfo waits2[2][2], signals2[2][2];
    VkCommandBufferSubmitInfo commands2[2][3];
    VkSubmitInfo2 sync2[2];
} input, saved_input;

static void note(enum event event) {
    CHECK(event_count < EVENT_CAPACITY);
    events[event_count++] = event;
}
static bool needs_execution(VkCommandBuffer command) {
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(source); ++i)
        if (source[i].cmd == command) return (query_sources || rtp_sources) && i < 2;
    CHECK(false);
    return false;
}
static unsigned expected_clone_calls(void) {
    return !zero_batches && !wait_only && (query_sources || rtp_sources) && helper_enabled ? 5 : 0;
}
static VkPipelineStageFlags2 translated(VkPipelineStageFlags2 mask) {
    /* Independent expected mapping: preserve every unrelated flag. */
    if (mask & VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR)
        mask = (mask & ~VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR) | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (mask & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR)
        mask = (mask & ~VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (mask & VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR)
        mask = (mask & ~VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR) | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    return mask;
}

bool vkrt_execution_command_requires_replay(const struct vkrt_cmd_buffer_data *cb) {
    CHECK(cb && cb->device_data == &device);
    if (rtp_sources) CHECK(cb->has_rtp_execution == needs_execution(cb->cmd) && !cb->has_virtual_queries);
    return needs_execution(cb->cmd);
}
VkResult vkrt_execution_collect(struct vkrt_device_data *dev, size_t *retired_count) {
    CHECK(dev == &device && !retired_count);
    if (idle_probe) CHECK(completions == 1 && event_count && events[event_count - 1] == COMPLETED);
    ++collects;
    note(COLLECT);
    return collect_result;
}
VkResult vkrt_execution_batch_create(struct vkrt_device_data *dev, VkQueue q, struct vkrt_execution_batch **out) {
    CHECK(dev == &device && q == queue && out && !*out && helper_enabled && !batch.created);
    CHECK(begins == 1 && !ends && admission_active == (device.queries != NULL) && !prepares && !natives);
    ++creates;
    note(CREATE);
    if (create_result != VK_SUCCESS) return create_result;
    batch.created = true;
    *out = &batch;
    return VK_SUCCESS;
}
VkResult vkrt_execution_batch_clone(struct vkrt_execution_batch *b, VkCommandBuffer original, VkCommandBuffer *target) {
    CHECK(b == &batch && batch.created && !batch.pending && !batch.discarded && target);
    ++clones;
    note(CLONE);
    if (clones == fail_clone) return clone_failure_result;
    *target = needs_execution(original) ? H(VkCommandBuffer, 100 + ++fresh) : original;
    CHECK(emitted_count < MAX_COMMANDS);
    emitted[emitted_count++] = *target;
    return VK_SUCCESS;
}
VkFence vkrt_execution_batch_completion_fence(const struct vkrt_execution_batch *b) {
    CHECK(!b || (b == &batch && batch.created && !batch.discarded));
    return b ? completion : VK_NULL_HANDLE;
}
VkResult vkrt_execution_batch_submitted(struct vkrt_execution_batch *b) {
    CHECK(b == &batch && !batch.pending && !batch.discarded && results == 1 && !prepared_active);
    CHECK(event_count && events[event_count - 1] == END && ends == 1 && !admission_active && natives == 1);
    ++submitted;
    note(SUBMITTED);
    /* Model the helper's documented native observation, including its result
     * normalization. The real sync translation owns only the original submit. */
    VkResult result = device.vtable.QueueSubmit(queue, 0, NULL, completion);
    batch.pending = true;
    return result == VK_ERROR_DEVICE_LOST ? result : VK_SUCCESS;
}
void vkrt_execution_batch_quarantine(struct vkrt_execution_batch *b) {
    CHECK(b == &batch && batch.created && !batch.pending && !batch.discarded && !prepared_active);
    CHECK(results == 1 && natives == 1 && !observers && !submitted);
    CHECK(last_query_result == VK_ERROR_DEVICE_LOST || last_query_result == VK_ERROR_UNKNOWN);
    CHECK(event_count && events[event_count - 1] == END && ends == 1 && !admission_active);
    ++quarantined;
    note(QUARANTINE);
    batch.pending = batch.quarantined = true;
}
VkResult vkrt_execution_batch_discard(struct vkrt_execution_batch *b) {
    CHECK(b == &batch && batch.created && !batch.pending && !batch.discarded && !prepared_active);
    CHECK(results == 1 && event_count && events[event_count - 1] == END && ends == 1 && !admission_active);
    ++discards;
    note(DISCARD);
    batch.discarded = true;
    return VK_SUCCESS;
}
VkResult vkrt_query_submit_begin(struct vkrt_device_data *dev, struct vkrt_query_submission *transaction, VkQueue q) {
    CHECK(dev == &device && transaction && !transaction->admission && q == queue && !admission_active);
    CHECK(collects == 1 && !begins && !ends && !creates && !prepares && !natives && !results);
    ++begins;
    note(BEGIN);
    if (!dev->queries) return VK_SUCCESS;
    if (begin_result != VK_SUCCESS) return begin_result;
    transaction->queue = q;
    transaction->admission = (void *)(uintptr_t)7;
    admission_active = true;
    return VK_SUCCESS;
}
void vkrt_query_submit_end(struct vkrt_device_data *dev, struct vkrt_query_submission *transaction) {
    CHECK(dev == &device && transaction && begins == 1 && !ends && results == 1 && !submitted && !observers && !discards &&
          !quarantined);
    CHECK(event_count && events[event_count - 1] == QUERY_RESULT);
    CHECK((transaction->admission != NULL) == admission_active);
    ++ends;
    note(END);
    transaction->admission = NULL;
    admission_active = false;
}
VkResult vkrt_query_submit_prepare(struct vkrt_device_data *dev, const struct vkrt_query_submission *transaction, uint32_t count,
                                   const VkCommandBuffer *commands) {
    CHECK(dev == &device && transaction->queue == (dev->queries ? queue : VK_NULL_HANDLE) && !natives);
    CHECK(begins == 1 && !ends && admission_active == (dev->queries != NULL));
    CHECK(clones == expected_clone_calls());
    ++prepares;
    note(PREPARE);
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(prepared_count < MAX_COMMANDS);
        prepared[prepared_count++] = commands[i];
    }
    prepared_active = true; /* Query rollback must precede command/payload disposal. */
    return prepares == fail_prepare ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
void vkrt_query_submit_result(struct vkrt_device_data *dev, const struct vkrt_query_submission *transaction, VkResult result) {
    CHECK(dev == &device && transaction && !discards && !quarantined);
    if (results) {
        CHECK(results == 1 && result == VK_ERROR_DEVICE_LOST && last_query_result == VK_SUCCESS);
        CHECK(submitted == 1 && observers == 1 && observer_result == VK_ERROR_DEVICE_LOST && batch.pending && !prepared_active);
        CHECK(event_count && events[event_count - 1] == OBSERVER);
        CHECK(ends == 1 && !admission_active && !transaction->admission);
    } else {
        CHECK(!submitted && !observers);
        CHECK(begins == 1 && !ends && admission_active == (dev->queries && begin_result == VK_SUCCESS));
    }
    ++results;
    note(QUERY_RESULT);
    prepared_active = false;
    last_query_result = result;
}
uint64_t vkrt_execution_checkpoint(struct vkrt_device_data *dev) {
    CHECK(dev == &device && idle_probe && !checkpoints && !completions && !event_count);
    ++checkpoints;
    idle_checkpoint = settled_clock;
    note(CHECKPOINT);
    return idle_checkpoint;
}
void vkrt_execution_queue_completed(struct vkrt_device_data *dev, VkQueue q, uint64_t checkpoint) {
    CHECK(dev == &device && idle_probe && checkpoints == 1 && !completions && !collects && idle_result == VK_SUCCESS);
    CHECK(checkpoint == idle_checkpoint && checkpoint < settled_clock && events[event_count - 1] == NATIVE_IDLE);
    ++completions;
    completed_queue = q;
    completed_checkpoint = checkpoint;
    note(COMPLETED);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(retention); ++i)
        if (retention[i].settled && (!q || retention[i].queue == q) && retention[i].stamp <= checkpoint) {
            retention[i].retired = true;
            ++retired;
        }
}

static VkCommandBuffer expected_command(unsigned row, unsigned column, size_t flat) {
    return expected_clone_calls() ? emitted[flat] : input.commands[row][column];
}
static VKAPI_ATTR VkResult VKAPI_CALL native_legacy(VkQueue q, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    if (fence == completion) {
        CHECK(q == queue && !count && !info && natives == 1 && submitted == 1 && results == 1 && !observers);
        CHECK(!prepared_active && !discards && !quarantined && last_query_result == VK_SUCCESS);
        CHECK(event_count && events[event_count - 1] == SUBMITTED);
        ++observers;
        note(OBSERVER);
        return observer_result;
    }
    CHECK(q == queue && fence == (caller_has_fence ? caller_fence : VK_NULL_HANDLE) && !natives && !results && !discards &&
          !submitted);
    CHECK(begins == 1 && !ends && admission_active == (device.queries != NULL));
    CHECK(prepared_count == (zero_batches || wait_only ? 0 : 5));
    ++natives;
    note(NATIVE);
    const unsigned source_count = zero_batches ? 0 : SOURCE_BATCHES;
    CHECK(count == source_count); /* Observation never mutates the source batch list. */
    size_t flat = 0;
    for (unsigned row = 0; row < source_count; ++row) {
        const VkSubmitInfo *original = &input.legacy[row], *actual = &info[row];
        CHECK(actual->sType == original->sType && actual->pNext == original->pNext);
        CHECK(actual->waitSemaphoreCount == original->waitSemaphoreCount && actual->pWaitSemaphores == original->pWaitSemaphores);
        CHECK(actual->pWaitDstStageMask != original->pWaitDstStageMask);
        for (uint32_t i = 0; i < actual->waitSemaphoreCount; ++i)
            CHECK(actual->pWaitDstStageMask[i] == translated(original->pWaitDstStageMask[i]));
        CHECK(actual->signalSemaphoreCount == original->signalSemaphoreCount &&
              actual->pSignalSemaphores == original->pSignalSemaphores);
        CHECK(actual->commandBufferCount == original->commandBufferCount);
        CHECK((actual->pCommandBuffers != original->pCommandBuffers) == (expected_clone_calls() != 0));
        for (uint32_t i = 0; i < actual->commandBufferCount; ++i, ++flat) {
            CHECK(actual->pCommandBuffers[i] == expected_command(row, i, flat));
            CHECK(prepared[flat] == actual->pCommandBuffers[i]);
        }
    }
    return native_result;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_sync2(VkQueue q, uint32_t count, const VkSubmitInfo2 *info, VkFence fence) {
    CHECK(q == queue && fence == (caller_has_fence ? caller_fence : VK_NULL_HANDLE) && !natives && !results && !discards &&
          !submitted);
    CHECK(begins == 1 && !ends && admission_active == (device.queries != NULL));
    CHECK(prepared_count == (zero_batches || wait_only ? 0 : 5));
    ++natives;
    note(NATIVE);
    const unsigned source_count = zero_batches ? 0 : SOURCE_BATCHES;
    CHECK(count == source_count);
    size_t flat = 0;
    for (unsigned row = 0; row < source_count; ++row) {
        const VkSubmitInfo2 *original = &input.sync2[row], *actual = &info[row];
        CHECK(actual->sType == original->sType && actual->pNext == original->pNext && actual->flags == original->flags);
        CHECK(actual->waitSemaphoreInfoCount == original->waitSemaphoreInfoCount &&
              actual->signalSemaphoreInfoCount == original->signalSemaphoreInfoCount);
        CHECK(actual->pWaitSemaphoreInfos != original->pWaitSemaphoreInfos &&
              actual->pSignalSemaphoreInfos != original->pSignalSemaphoreInfos);
        for (uint32_t i = 0; i < actual->waitSemaphoreInfoCount; ++i) {
            const VkSemaphoreSubmitInfo *a = &actual->pWaitSemaphoreInfos[i], *b = &original->pWaitSemaphoreInfos[i];
            CHECK(a->sType == b->sType && a->pNext == b->pNext && a->semaphore == b->semaphore && a->value == b->value &&
                  a->deviceIndex == b->deviceIndex && a->stageMask == translated(b->stageMask));
        }
        for (uint32_t i = 0; i < actual->signalSemaphoreInfoCount; ++i) {
            const VkSemaphoreSubmitInfo *a = &actual->pSignalSemaphoreInfos[i], *b = &original->pSignalSemaphoreInfos[i];
            CHECK(a->sType == b->sType && a->pNext == b->pNext && a->semaphore == b->semaphore && a->value == b->value &&
                  a->deviceIndex == b->deviceIndex && a->stageMask == translated(b->stageMask));
        }
        CHECK(actual->commandBufferInfoCount == original->commandBufferInfoCount);
        CHECK((actual->pCommandBufferInfos != original->pCommandBufferInfos) == (expected_clone_calls() != 0));
        for (uint32_t i = 0; i < actual->commandBufferInfoCount; ++i, ++flat) {
            CHECK(actual->pCommandBufferInfos[i].sType == original->pCommandBufferInfos[i].sType &&
                  actual->pCommandBufferInfos[i].pNext == original->pCommandBufferInfos[i].pNext &&
                  actual->pCommandBufferInfos[i].deviceMask == original->pCommandBufferInfos[i].deviceMask);
            CHECK(actual->pCommandBufferInfos[i].commandBuffer == expected_command(row, i, flat));
            CHECK(prepared[flat] == actual->pCommandBufferInfos[i].commandBuffer);
        }
    }
    return native_result;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_queue_idle(VkQueue q) {
    CHECK(q == queue && checkpoints == 1 && events[event_count - 1] == CHECKPOINT && !admission_active);
    note(NATIVE_IDLE);
    ++settled_clock; /* A concurrent submission settles after the captured bound. */
    return idle_result;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_device_idle(VkDevice dev) {
    CHECK(dev == device.device && checkpoints == 1 && events[event_count - 1] == CHECKPOINT && !admission_active);
    note(NATIVE_IDLE);
    ++settled_clock;
    return idle_result;
}

static void reset(bool sync2) {
    event_count = collects = creates = clones = fresh = prepares = natives = observers = results = discards = submitted =
        quarantined = checkpoints = completions = retired = begins = ends = 0;
    fail_clone = fail_prepare = 0;
    emitted_count = prepared_count = 0;
    collect_result = begin_result = create_result = native_result = observer_result = idle_result = VK_SUCCESS;
    clone_failure_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    last_query_result = VK_ERROR_UNKNOWN;
    use_submit2 = sync2;
    query_sources = helper_enabled = caller_has_fence = true;
    zero_batches = wait_only = prepared_active = idle_probe = admission_active = rtp_sources = false;
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(source); ++i) {
        source[i].has_rtp_execution = false;
        source[i].native_active_queries = 0;
    }
    settled_clock = 10;
    idle_checkpoint = completed_checkpoint = 0;
    completed_queue = H(VkQueue, 99);
    memset(retention, 0, sizeof(retention));
    retention[0].queue = retention[2].queue = retention[3].queue = queue;
    retention[1].queue = other_queue;
    retention[0].stamp = retention[1].stamp = retention[3].stamp = 10;
    retention[2].stamp = 11;
    retention[0].settled = retention[1].settled = retention[2].settled = true;
    batch = (struct vkrt_execution_batch){0};
    device.executions = (void *)(uintptr_t)1;
    device.queries = (void *)(uintptr_t)6;
    memset(&input, 0, sizeof(input));
    for (unsigned row = 0; row < SOURCE_BATCHES; ++row) {
        input.outer[row] = (struct payload){VK_STRUCTURE_TYPE_MAX_ENUM, NULL, UINT64_C(0x1234567800000000) + row};
        input.waits[row][0] = H(VkSemaphore, 20 + row);
        input.signals[row][0] = H(VkSemaphore, 30 + row);
        input.wait_values[row][0] = 40 + row;
        input.signal_values[row][0] = 50 + row;
        input.masks[row][0] = row ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                  : VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        input.timelines[row] = (VkTimelineSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                               .pNext = &input.outer[row],
                                                               .waitSemaphoreValueCount = 1,
                                                               .pWaitSemaphoreValues = input.wait_values[row],
                                                               .signalSemaphoreValueCount = 1,
                                                               .pSignalSemaphoreValues = input.signal_values[row]};
        const unsigned count = row ? 2 : 3;
        input.commands[row][0] = source[row].cmd;
        input.commands[row][1] = row ? source[0].cmd : source[2].cmd;
        if (!row) input.commands[row][2] = source[0].cmd;
        input.legacy[row] = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                           .pNext = &input.timelines[row],
                                           .waitSemaphoreCount = 1,
                                           .pWaitSemaphores = input.waits[row],
                                           .pWaitDstStageMask = input.masks[row],
                                           .commandBufferCount = count,
                                           .pCommandBuffers = input.commands[row],
                                           .signalSemaphoreCount = 1,
                                           .pSignalSemaphores = input.signals[row]};
        input.semaphore_next[row * 2] = (struct payload){VK_STRUCTURE_TYPE_MAX_ENUM, NULL, UINT64_C(0xabcdef00) + row};
        input.semaphore_next[row * 2 + 1] = (struct payload){VK_STRUCTURE_TYPE_MAX_ENUM, NULL, UINT64_C(0xdecafe00) + row};
        input.waits2[row][0] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                                       .pNext = &input.semaphore_next[row * 2],
                                                       .semaphore = input.waits[row][0],
                                                       .value = input.wait_values[row][0],
                                                       .stageMask = input.masks[row][0],
                                                       .deviceIndex = 2};
        input.signals2[row][0] =
            (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                    .pNext = &input.semaphore_next[row * 2 + 1],
                                    .semaphore = input.signals[row][0],
                                    .value = input.signal_values[row][0],
                                    .stageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                    .deviceIndex = 3};
        for (unsigned i = 0; i < count; ++i) {
            input.command_next[row * 3 + i] = (struct payload){VK_STRUCTURE_TYPE_MAX_ENUM, NULL, UINT64_C(0x55550000) + i};
            input.commands2[row][i] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                                  .pNext = &input.command_next[row * 3 + i],
                                                                  .commandBuffer = input.commands[row][i],
                                                                  .deviceMask = 5};
        }
        input.sync2[row] = (VkSubmitInfo2){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                           .pNext = &input.outer[row],
                                           .flags = (VkSubmitFlags)row,
                                           .waitSemaphoreInfoCount = 1,
                                           .pWaitSemaphoreInfos = input.waits2[row],
                                           .commandBufferInfoCount = count,
                                           .pCommandBufferInfos = input.commands2[row],
                                           .signalSemaphoreInfoCount = 1,
                                           .pSignalSemaphoreInfos = input.signals2[row]};
    }
}
static VkResult run(void) {
    memcpy(&saved_input, &input, sizeof(input));
    VkResult result;
    if (use_submit2) {
        PFN_vkQueueSubmit2 proc = (PFN_vkQueueSubmit2)vkrt_sync_proc("vkQueueSubmit2KHR");
        CHECK(proc && proc == (PFN_vkQueueSubmit2)vkrt_sync_proc("vkQueueSubmit2"));
        result = proc(queue, zero_batches ? 0 : SOURCE_BATCHES, zero_batches ? NULL : input.sync2,
                      caller_has_fence ? caller_fence : VK_NULL_HANDLE);
    } else {
        PFN_vkQueueSubmit proc = (PFN_vkQueueSubmit)vkrt_sync_proc("vkQueueSubmit");
        CHECK(proc);
        result = proc(queue, zero_batches ? 0 : SOURCE_BATCHES, zero_batches ? NULL : input.legacy,
                      caller_has_fence ? caller_fence : VK_NULL_HANDLE);
    }
    CHECK(!memcmp(&input, &saved_input, sizeof(input)));
    CHECK(collects == 1 && events[0] == COLLECT);
    if (results) CHECK(last_query_result == result && !prepared_active && begins == 1 && ends == 1 && !admission_active);
    ++cases;
    return result;
}
static void success_and_no_query(bool sync2) {
    reset(sync2);
    CHECK(run() == VK_SUCCESS);
    CHECK(creates == 1 && clones == 5 && fresh == 4 && natives == 1 && results == 1 && submitted == 1 && observers == 1 &&
          !discards && !quarantined && batch.pending);
    CHECK(emitted[0] != emitted[2] && emitted[0] != emitted[4] && emitted[2] != emitted[4]);
    CHECK(emitted[1] == source[2].cmd && emitted[3] != source[1].cmd);
    reset(sync2);
    caller_has_fence = false;
    CHECK(run() == VK_SUCCESS);
    CHECK(natives == 1 && results == 1 && submitted == 1 && observers == 1 && batch.pending && !quarantined && !discards);
    reset(sync2);
    query_sources = false;
    CHECK(run() == VK_SUCCESS);
    CHECK(!creates && !clones && !submitted && !observers && !quarantined && !discards && natives == 1 && results == 1);
    reset(sync2);
    query_sources = false;
    rtp_sources = true;
    device.queries = NULL;
    for (unsigned i = 0; i < 2; ++i) source[i].has_rtp_execution = true;
    source[0].native_active_queries = 1;
    CHECK(run() == VK_SUCCESS);
    CHECK(creates == 1 && clones == 5 && fresh == 4 && observers == 1 && !admission_active && !device.queries);
    reset(sync2);
    wait_only = true;
    for (unsigned row = 0; row < SOURCE_BATCHES; ++row) {
        input.legacy[row].commandBufferCount = 0;
        input.sync2[row].commandBufferInfoCount = 0;
    }
    CHECK(run() == VK_SUCCESS);
    /* An ordinary wait-only source keeps the original batches and fence. It
     * does not allocate a replay batch or issue a hidden observation. */
    CHECK(!creates && !clones && !submitted && !observers && !quarantined && !discards && natives == 1 && results == 1);
    CHECK(prepares == (sync2 ? 0u : SOURCE_BATCHES));
    reset(sync2);
    helper_enabled = false;
    device.executions = NULL;
    CHECK(run() == VK_SUCCESS);
    CHECK(!creates && !clones && !submitted && !observers && !quarantined && !discards && natives == 1);
    reset(sync2);
    zero_batches = true;
    CHECK(run() == VK_SUCCESS);
    CHECK(!creates && !clones && !prepares && natives == 1 && !submitted && !observers && !quarantined && !discards);
    reset(sync2);
    device.queries = NULL;
    query_sources = false;
    begin_result = VK_ERROR_OUT_OF_HOST_MEMORY; /* No query backend means a no-op admission. */
    CHECK(run() == VK_SUCCESS);
    CHECK(begins == 1 && ends == 1 && !admission_active && !creates && !observers && natives == 1);
}
static void observer_failures(bool sync2) {
    const VkResult errors[] = {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN, VK_ERROR_DEVICE_LOST};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(errors); ++i) {
        reset(sync2);
        observer_result = errors[i];
        const VkResult expected = errors[i] == VK_ERROR_DEVICE_LOST ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
        CHECK(run() == expected);
        CHECK(natives == 1 && submitted == 1 && observers == 1 && !discards && !quarantined && batch.pending);
        CHECK(results == (errors[i] == VK_ERROR_DEVICE_LOST ? 2u : 1u) && last_query_result == expected);
        CHECK(events[event_count - 1] == (errors[i] == VK_ERROR_DEVICE_LOST ? QUERY_RESULT : OBSERVER));
    }
}
static void rtp_only_failures(bool sync2) {
    const VkResult errors[] = {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN, VK_ERROR_DEVICE_LOST};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(errors); ++i) {
        reset(sync2);
        query_sources = false;
        rtp_sources = true;
        device.queries = NULL;
        for (unsigned source_index = 0; source_index < 2; ++source_index) source[source_index].has_rtp_execution = true;
        native_result = errors[i];
        CHECK(run() == errors[i]);
        CHECK(natives == 1 && clones == 5 && !observers && !submitted && !admission_active);
        if (errors[i] == VK_ERROR_OUT_OF_HOST_MEMORY || errors[i] == VK_ERROR_OUT_OF_DEVICE_MEMORY)
            CHECK(discards == 1 && !quarantined && batch.discarded);
        else
            CHECK(!discards && quarantined == 1 && batch.pending && batch.quarantined);
    }
}
static void failures(bool sync2) {
    reset(sync2);
    collect_result = VK_ERROR_DEVICE_LOST;
    CHECK(run() == VK_ERROR_DEVICE_LOST);
    CHECK(!creates && !clones && !prepares && !natives && !results && !discards && !submitted && !observers && !quarantined &&
          !begins && !ends);
    const VkResult begin_errors[] = {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST,
                                     VK_ERROR_UNKNOWN};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(begin_errors); ++i) {
        reset(sync2);
        begin_result = begin_errors[i];
        CHECK(run() == begin_errors[i]);
        CHECK(begins == 1 && ends == 1 && results == 1 && !admission_active && !creates && !clones && !prepares && !natives &&
              !observers && !discards && !submitted && !quarantined);
        CHECK(event_count == 4 && events[1] == BEGIN && events[2] == QUERY_RESULT && events[3] == END);
    }
    reset(sync2);
    create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    CHECK(run() == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(creates == 1 && !clones && !prepares && !natives && results == 1 && !discards && !submitted);
    reset(sync2);
    fail_clone = 3;
    CHECK(run() == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(creates == 1 && clones == 3 && fresh == 1 && !prepares && !natives && results == 1 && discards == 1 && !submitted);
    reset(sync2);
    fail_clone = 3;
    clone_failure_result = VK_ERROR_UNKNOWN;
    CHECK(run() == VK_ERROR_UNKNOWN);
    CHECK(creates == 1 && clones == 3 && fresh == 1 && !prepares && !natives && results == 1 && discards == 1 && !submitted &&
          !quarantined);
    reset(sync2);
    fail_prepare = 2;
    CHECK(run() == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(prepares == 2 && !natives && results == 1 && discards == 1 && !submitted);
    const VkResult errors[] = {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST, VK_ERROR_UNKNOWN};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(errors); ++i) {
        reset(sync2);
        native_result = errors[i];
        CHECK(run() == errors[i] && natives == 1 && results == 1);
        if (i < 2)
            CHECK(discards == 1 && !submitted && !observers && !quarantined && batch.discarded && !batch.pending);
        else
            CHECK(!discards && !submitted && !observers && quarantined == 1 && batch.pending && batch.quarantined &&
                  !batch.discarded);
        reset(sync2);
        query_sources = false;
        native_result = errors[i];
        CHECK(run() == errors[i] && natives == 1 && results == 1);
        CHECK(!creates && !clones && !submitted && !observers && !discards && !quarantined);
    }
}
static void idle_ordering(void) {
    for (unsigned dev = 0; dev < 2; ++dev) {
        for (unsigned failure = 0; failure < 3; ++failure) {
            reset(false);
            idle_probe = true;
            if (failure == 1) idle_result = VK_ERROR_DEVICE_LOST;
            if (failure == 2) collect_result = VK_ERROR_OUT_OF_HOST_MEMORY;
            VkResult result;
            if (dev) {
                PFN_vkDeviceWaitIdle proc = (PFN_vkDeviceWaitIdle)vkrt_sync_proc("vkDeviceWaitIdle");
                result = proc(device.device);
            } else {
                PFN_vkQueueWaitIdle proc = (PFN_vkQueueWaitIdle)vkrt_sync_proc("vkQueueWaitIdle");
                result = proc(queue);
            }
            CHECK(events[0] == CHECKPOINT && events[1] == NATIVE_IDLE && checkpoints == 1 && idle_checkpoint == 10 &&
                  settled_clock == 11 && !begins && !ends && !admission_active);
            if (failure == 1)
                CHECK(result == idle_result && event_count == 2 && !completions && !retired && !collects);
            else {
                /* Explicit idle retires its pre-call settled prefix before
                 * optional observation collection reports an independent error. */
                CHECK(result == collect_result && event_count == 4 && events[2] == COMPLETED && events[3] == COLLECT &&
                      collects == 1 && completions == 1 && completed_checkpoint == 10 &&
                      completed_queue == (dev ? VK_NULL_HANDLE : queue));
                CHECK(retired == (dev ? 2u : 1u) && retention[0].retired && retention[1].retired == (dev != 0) &&
                      !retention[2].retired && !retention[3].retired);
            }
            ++cases;
        }
    }
}
int main(void) {
    device.device = H(VkDevice, 1);
    device.vtable.QueueSubmit = native_legacy;
    device.vtable.QueueSubmit2 = native_sync2;
    device.vtable.QueueWaitIdle = native_queue_idle;
    device.vtable.DeviceWaitIdle = native_device_idle;
    vkrt_map_object(VKRT_HKEY(device.device), &device);
    vkrt_map_object(VKRT_HKEY(queue), &device);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(source); ++i) {
        source[i] = (struct vkrt_cmd_buffer_data){.cmd = H(VkCommandBuffer, 10 + i), .device_data = &device};
        vkrt_map_object(VKRT_HKEY(source[i].cmd), &source[i]);
    }
    for (unsigned sync2 = 0; sync2 < 2; ++sync2) {
        success_and_no_query(sync2 != 0);
        observer_failures(sync2 != 0);
        rtp_only_failures(sync2 != 0);
        failures(sync2 != 0);
    }
    idle_ordering();
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(source); ++i) vkrt_unmap_object(VKRT_HKEY(source[i].cmd));
    vkrt_unmap_object(VKRT_HKEY(queue));
    vkrt_unmap_object(VKRT_HKEY(device.device));
    printf("sync execution host: %u submit/rollback/retention/idle cases passed\n", cases);
    return 0;
}
