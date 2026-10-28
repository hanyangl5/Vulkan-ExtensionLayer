/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_query_pool.c"
#include <stdatomic.h>
#include <errno.h>

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
VkResult vkrt_execution_collect(struct vkrt_device_data *dev, size_t *retired) {
    (void)dev;
    if (retired) *retired = 0;
    return VK_SUCCESS;
}
static _Atomic unsigned failures;
static unsigned acquisitions, resets, native_creates;
#define CHECK(test)                                                 \
    do {                                                            \
        if (!(test)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); \
            ++failures;                                             \
        }                                                           \
    } while (0)
#define HANDLE(type, value) ((type)(uintptr_t)(value))
static uint64_t marker_availability[4];
static const VkQueryPoolCreateInfo *native_create_info;
static const VkAllocationCallbacks *native_create_allocator;
static VKAPI_ATTR VkResult VKAPI_CALL native_create(VkDevice device, const VkQueryPoolCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkQueryPool *pool) {
    CHECK(device == HANDLE(VkDevice, 1) && info == native_create_info && allocator == native_create_allocator);
    CHECK(info->queryType == VK_QUERY_TYPE_TIMESTAMP && info->queryCount == 3);
    ++native_creates;
    *pool = HANDLE(VkQueryPool, 9);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_get(VkDevice device, VkQueryPool marker, uint32_t first, uint32_t count, size_t size,
                                                 void *data, VkDeviceSize stride, VkQueryResultFlags flags) {
    CHECK(device == HANDLE(VkDevice, 1) && first == 0 && count == 1 && size == 16 && stride == 16);
    uint64_t *out = data;
    out[0] = UINT64_C(0xfedcba9876543210); /* Never interpreted as a property. */
    out[1] = marker_availability[(uintptr_t)marker];
    if (flags & VK_QUERY_RESULT_WAIT_BIT) {
        CHECK(out[1]);
        ++acquisitions;
    }
    return out[1] ? VK_SUCCESS : VK_NOT_READY;
}
static VKAPI_ATTR void VKAPI_CALL native_reset(VkDevice device, VkQueryPool marker, uint32_t first, uint32_t count) {
    CHECK(device == HANDLE(VkDevice, 1) && first == 0 && count == 1);
    marker_availability[(uintptr_t)marker] = 0;
    ++resets;
}
/* Unused lowering paths still link to the real helper interface. */
VkDeviceAddress vkrt_as_backing_address(struct vkrt_device_data *dev, VkAccelerationStructureKHR handle) {
    (void)dev;
    (void)handle;
    return 0;
}
static unsigned pipeline_calls, pipeline_fail_at, destroyed_modules, destroyed_pipelines, destroyed_layouts;
static VKAPI_ATTR VkResult VKAPI_CALL make_layout(VkDevice device, const VkPipelineLayoutCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator, VkPipelineLayout *layout) {
    CHECK(device == HANDLE(VkDevice, 1) && info->pushConstantRangeCount == 1 && !allocator);
    *layout = HANDLE(VkPipelineLayout, 100);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL make_shader(VkDevice device, const VkShaderModuleCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator, VkShaderModule *module) {
    CHECK(device == HANDLE(VkDevice, 1) && info->codeSize && info->pCode && !allocator);
    *module = HANDLE(VkShaderModule, 101);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL make_compute(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                   const VkComputePipelineCreateInfo *info, const VkAllocationCallbacks *allocator,
                                                   VkPipeline *pipeline) {
    CHECK(device == HANDLE(VkDevice, 1) && !cache && count == 1 && info->layout == HANDLE(VkPipelineLayout, 100) && !allocator);
    *pipeline = VK_NULL_HANDLE;
    if (++pipeline_calls == pipeline_fail_at) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *pipeline = HANDLE(VkPipeline, 110 + pipeline_calls);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL drop_shader(VkDevice device, VkShaderModule module, const VkAllocationCallbacks *allocator) {
    CHECK(device == HANDLE(VkDevice, 1) && module == HANDLE(VkShaderModule, 101) && !allocator);
    ++destroyed_modules;
}
static VKAPI_ATTR void VKAPI_CALL drop_layout(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks *allocator) {
    CHECK(device == HANDLE(VkDevice, 1) && layout == HANDLE(VkPipelineLayout, 100) && !allocator);
    ++destroyed_layouts;
}
static VKAPI_ATTR void VKAPI_CALL drop_pipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *allocator) {
    CHECK(device == HANDLE(VkDevice, 1) && pipeline && !allocator);
    ++destroyed_pipelines;
}
static void pipeline_rollback_history(struct vkrt_device_data *dev, VkQueue queue, VkCommandBuffer copy) {
    struct vkrt_device_dispatch saved = dev->vtable;
    dev->vtable.CreatePipelineLayout = make_layout;
    dev->vtable.CreateShaderModule = make_shader;
    dev->vtable.CreateComputePipelines = make_compute;
    dev->vtable.DestroyShaderModule = drop_shader;
    dev->vtable.DestroyPipelineLayout = drop_layout;
    dev->vtable.DestroyPipeline = drop_pipeline;
    for (pipeline_fail_at = 1; pipeline_fail_at <= 3; ++pipeline_fail_at) {
        pipeline_calls = destroyed_modules = destroyed_layouts = destroyed_pipelines = 0;
        struct query_queue *history = dev->queries->queues;
        vkrt_mutex_lock(&dev->queries->lock);
        CHECK(initialize_pipelines(dev) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        vkrt_mutex_unlock(&dev->queries->lock);
        CHECK(dev->queries->queues == history);
        CHECK(!dev->queries->layout && !dev->queries->property && !dev->queries->publish && !dev->queries->format);
        CHECK(destroyed_modules == pipeline_fail_at && destroyed_layouts == 1 && destroyed_pipelines == pipeline_fail_at - 1);
        struct vkrt_query_submission transaction = {0};
        CHECK(vkrt_query_submit_qualify(dev, &transaction, queue, false, 1, &copy) == VK_ERROR_FEATURE_NOT_PRESENT);
    }
    dev->vtable = saved;
}
static void checkpoint_history(struct vkrt_device_data *dev, VkQueue queue, VkQueue other_queue, VkCommandBuffer copy) {
    struct vkrt_query_submission unsafe = {0}, check = {0};
    CHECK(vkrt_query_submit_qualify(dev, &unsafe, queue, true, 0, NULL) == VK_SUCCESS);
    vkrt_query_submit_result(dev, &unsafe, VK_SUCCESS);
    uint64_t checkpoint = vkrt_query_queue_checkpoint(dev);
    unsafe = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(dev, &unsafe, queue, true, 0, NULL) == VK_SUCCESS);
    vkrt_query_submit_result(dev, &unsafe, VK_SUCCESS);
    vkrt_query_queue_idle(dev, queue, checkpoint);
    CHECK(vkrt_query_submit_qualify(dev, &check, queue, false, 1, &copy) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkrt_query_queue_idle(dev, queue, vkrt_query_queue_checkpoint(dev));
    check = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(dev, &check, queue, false, 1, &copy) == VK_SUCCESS);
    unsafe = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(dev, &unsafe, queue, true, 0, NULL) == VK_SUCCESS);
    vkrt_query_submit_result(dev, &unsafe, VK_SUCCESS);
    checkpoint = vkrt_query_queue_checkpoint(dev);
    unsafe = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(dev, &unsafe, other_queue, true, 0, NULL) == VK_SUCCESS);
    vkrt_query_submit_result(dev, &unsafe, VK_SUCCESS);
    vkrt_query_queue_idle(dev, VK_NULL_HANDLE, checkpoint);
    check = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(dev, &check, queue, false, 1, &copy) == VK_SUCCESS);
    check = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(dev, &check, other_queue, false, 1, &copy) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkrt_query_queue_idle(dev, VK_NULL_HANDLE, vkrt_query_queue_checkpoint(dev));
}
static void saturated_history(struct vkrt_device_data *dev, VkQueue queue, VkCommandBuffer copy) {
    dev->queries->unsafe_epoch = UINT64_MAX - 1;
    struct vkrt_query_submission transaction = {0};
    CHECK(vkrt_query_submit_qualify(dev, &transaction, queue, true, 0, NULL) == VK_SUCCESS);
    vkrt_query_submit_result(dev, &transaction, VK_SUCCESS);
    uint64_t checkpoint = vkrt_query_queue_checkpoint(dev);
    CHECK(checkpoint == UINT64_MAX);
    vkrt_query_queue_idle(dev, queue, checkpoint);
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(dev, &transaction, queue, false, 1, &copy) == VK_SUCCESS);
    CHECK(vkrt_query_submit_qualify(dev, &transaction, queue, true, 0, NULL) == VK_SUCCESS);
    vkrt_query_submit_result(dev, &transaction, VK_SUCCESS);
    vkrt_query_queue_idle(dev, VK_NULL_HANDLE, checkpoint);
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(dev, &transaction, queue, false, 1, &copy) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(vkrt_query_queue_checkpoint(dev) == UINT64_MAX);
}
struct test_event {
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_mutex_t lock;
    pthread_cond_t condition;
    bool set;
#endif
};
static void event_init(struct test_event *event) {
#ifdef _WIN32
    event->handle = CreateEvent(NULL, TRUE, FALSE, NULL);
    CHECK(event->handle);
#else
    CHECK(!pthread_mutex_init(&event->lock, NULL));
    CHECK(!pthread_cond_init(&event->condition, NULL));
    event->set = false;
#endif
}
static void event_signal(struct test_event *event) {
#ifdef _WIN32
    CHECK(SetEvent(event->handle));
#else
    pthread_mutex_lock(&event->lock);
    event->set = true;
    pthread_cond_broadcast(&event->condition);
    pthread_mutex_unlock(&event->lock);
#endif
}
static bool event_wait(struct test_event *event, uint32_t milliseconds) {
#ifdef _WIN32
    return WaitForSingleObject(event->handle, milliseconds) == WAIT_OBJECT_0;
#else
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&event->lock);
    int status = 0;
    while (!event->set && !status) status = pthread_cond_timedwait(&event->condition, &event->lock, &deadline);
    bool result = event->set;
    pthread_mutex_unlock(&event->lock);
    return result;
#endif
}
static void event_finish(struct test_event *event) {
#ifdef _WIN32
    CHECK(CloseHandle(event->handle));
#else
    pthread_cond_destroy(&event->condition);
    pthread_mutex_destroy(&event->lock);
#endif
}
struct admission_fixture {
    struct vkrt_device_data *dev;
    VkQueue queue, other_queue;
    VkCommandBuffer copy;
    struct vkrt_query_admission *watched;
    struct test_event native_success, publish, contended, b_began, b_done, c_done;
};
struct admission_worker {
    struct admission_fixture *fixture;
    unsigned index;
};
#ifdef _WIN32
static DWORD WINAPI admission_worker(void *opaque)
#else
static void *admission_worker(void *opaque)
#endif
{
    struct admission_worker *worker = opaque;
    struct admission_fixture *f = worker->fixture;
    struct vkrt_query_submission transaction = {0};
    if (worker->index == 0) {
        CHECK(vkrt_query_submit_begin(f->dev, &transaction, f->queue) == VK_SUCCESS);
        CHECK(vkrt_query_submit_qualify(f->dev, &transaction, f->queue, true, 0, NULL) == VK_SUCCESS);
        /* Deterministic native-SUCCESS -> query-publication pause. */
        event_signal(&f->native_success);
        CHECK(event_wait(&f->publish, 5000));
        vkrt_query_submit_result(f->dev, &transaction, VK_SUCCESS);
        vkrt_query_submit_end(f->dev, &transaction);
    } else if (worker->index == 1) {
        /* Prove real contention while A is paused, rather than infer it from
         * a sleep or scheduling delay. B then calls the production begin API. */
#ifdef _WIN32
        bool acquired = TryAcquireSRWLockExclusive(&f->watched->lock) != 0;
        if (acquired) ReleaseSRWLockExclusive(&f->watched->lock);
        CHECK(!acquired);
#else
        int acquired = pthread_mutex_trylock(&f->watched->lock);
        if (!acquired) pthread_mutex_unlock(&f->watched->lock);
        CHECK(acquired == EBUSY);
#endif
        event_signal(&f->contended);
        CHECK(vkrt_query_submit_begin(f->dev, &transaction, f->queue) == VK_SUCCESS);
        event_signal(&f->b_began);
        CHECK(vkrt_query_submit_qualify(f->dev, &transaction, f->queue, false, 1, &f->copy) == VK_ERROR_FEATURE_NOT_PRESENT);
        vkrt_query_submit_result(f->dev, &transaction, VK_ERROR_FEATURE_NOT_PRESENT);
        vkrt_query_submit_end(f->dev, &transaction);
        event_signal(&f->b_done);
    } else {
        CHECK(vkrt_query_submit_begin(f->dev, &transaction, f->other_queue) == VK_SUCCESS);
        CHECK(vkrt_query_submit_qualify(f->dev, &transaction, f->other_queue, false, 1, &f->copy) == VK_SUCCESS);
        vkrt_query_submit_result(f->dev, &transaction, VK_SUCCESS);
        vkrt_query_submit_end(f->dev, &transaction);
        event_signal(&f->c_done);
    }
    CHECK(!transaction.admission);
    return 0;
}
static void admission_ordering(struct vkrt_device_data *dev, VkQueue queue, VkQueue other_queue, VkCommandBuffer copy) {
    vkrt_query_queue_idle(dev, VK_NULL_HANDLE, vkrt_query_queue_checkpoint(dev));
    struct admission_fixture fixture = {.dev = dev, .queue = queue, .other_queue = other_queue, .copy = copy};
    struct test_event *events[] = {&fixture.native_success, &fixture.publish, &fixture.contended,
                                   &fixture.b_began,        &fixture.b_done,  &fixture.c_done};
    for (unsigned i = 0; i < 6; ++i) event_init(events[i]);
    struct admission_worker workers[] = {{&fixture, 0}, {&fixture, 1}, {&fixture, 2}};
#ifdef _WIN32
    HANDLE threads[3];
    threads[0] = CreateThread(NULL, 0, admission_worker, &workers[0], 0, NULL);
    CHECK(threads[0]);
#else
    pthread_t threads[3];
    CHECK(!pthread_create(&threads[0], NULL, admission_worker, &workers[0]));
#endif
    CHECK(event_wait(&fixture.native_success, 5000));
    vkrt_mutex_lock(&dev->queries->lock);
    for (struct query_queue *q = dev->queries->queues; q; q = q->next)
        if (q->queue == queue) fixture.watched = &q->admission;
    vkrt_mutex_unlock(&dev->queries->lock);
    CHECK(fixture.watched);
#ifdef _WIN32
    for (unsigned i = 1; i < 3; ++i) {
        threads[i] = CreateThread(NULL, 0, admission_worker, &workers[i], 0, NULL);
        CHECK(threads[i]);
    }
#else
    for (unsigned i = 1; i < 3; ++i) CHECK(!pthread_create(&threads[i], NULL, admission_worker, &workers[i]));
#endif
    CHECK(event_wait(&fixture.contended, 5000));
    CHECK(!event_wait(&fixture.b_began, 0));
    CHECK(event_wait(&fixture.c_done, 5000)); /* Another queue is independent. */
    event_signal(&fixture.publish);
    CHECK(event_wait(&fixture.b_done, 5000));
#ifdef _WIN32
    CHECK(WaitForMultipleObjects(3, threads, TRUE, 5000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 3; ++i) CloseHandle(threads[i]);
#else
    for (unsigned i = 0; i < 3; ++i) pthread_join(threads[i], NULL);
#endif
    for (unsigned i = 0; i < 6; ++i) event_finish(events[i]);
    vkrt_query_queue_idle(dev, VK_NULL_HANDLE, vkrt_query_queue_checkpoint(dev));
}

int main(void) {
    struct vkrt_query_device state = {0};
    vkrt_mutex_init(&state.lock);
    struct vkrt_device_data dev = {.device = HANDLE(VkDevice, 1), .queries = &state};
    dev.vtable.GetQueryPoolResults = native_get;
    dev.vtable.CreateQueryPool = native_create;
    dev.vtable.ResetQueryPool = native_reset;
    struct query_host_slot slots[1] = {0};
    struct query_pool pool = {.magic = QUERY_POOL_MAGIC,
                              .dev = &dev,
                              .type = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                              .count = 1,
                              .slots = slots};
    struct {
        struct vkrt_as_query_value value;
        uint64_t serial;
    } write_payload = {{UINT64_C(0x100000081), 1, 0}, 1};
    struct {
        struct vkrt_as_query_value value;
        uint64_t serial;
    } reset_payload = {{0}, 2};
    struct query_operation write_op = {.pool = &pool,
                                       .first = 0,
                                       .count = 1,
                                       .marker = HANDLE(VkQueryPool, 2),
                                       .payload = {.mapped = &write_payload},
                                       .submitted = true};
    struct query_operation reset_op = {.pool = &pool,
                                       .first = 0,
                                       .count = 1,
                                       .marker = HANDLE(VkQueryPool, 3),
                                       .payload = {.mapped = &reset_payload},
                                       .submitted = false};
    write_op.next = &reset_op;
    struct vkrt_cmd_buffer_data cb = {.cmd = HANDLE(VkCommandBuffer, 4), .device_data = &dev};
    struct vkrt_query_command command = {.owner = &cb, .head = &write_op, .tail = &reset_op};
    cb.queries = &command;
    state.commands = &command;
    vkrt_map_object(VKRT_HKEY(dev.device), &dev);
    vkrt_map_object(VKRT_HKEY(cb.cmd), &cb);
    VkQueryPool handle = (VkQueryPool)(uintptr_t)&pool;
    vkrt_map_object(VKRT_HKEY(handle), &pool);
    /* Exercise the actual public proc, not the retired translation shim.
     * Ordinary pools preserve the exact native input and allocator pointers. */
    VkBaseInStructure native_suffix = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM};
    VkQueryPoolCreateInfo native_info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                         .pNext = &native_suffix,
                                         .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                         .queryCount = 3};
    VkAllocationCallbacks native_allocator = {0};
    native_create_info = &native_info;
    native_create_allocator = &native_allocator;
    VkQueryPool native_pool = VK_NULL_HANDLE;
    PFN_vkCreateQueryPool create_pool = (void *)vkrt_query_pool_proc("vkCreateQueryPool");
    CHECK(create_pool && create_pool(dev.device, &native_info, &native_allocator, &native_pool) == VK_SUCCESS);
    CHECK(native_pool == HANDLE(VkQueryPool, 9) && native_creates == 1 && !find_pool(native_pool));
    CHECK(native_info.pNext == &native_suffix);
    uint64_t out[4] = {11, 13, 17, 19};
    marker_availability[2] = 1;
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(out), out, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS);
    CHECK(acquisitions == 1 && out[0] == UINT64_C(0x100000081) && out[1] == 1 && out[2] == 17 && out[3] == 19);

    /* Legal resubmit first snapshots the old epoch and resets only private
     * native markers. Public old availability remains until a GPU reset runs. */
    reset_op.submitted = false;
    struct vkrt_query_submission transaction = {0};
    CHECK(vkrt_query_submit_prepare(&dev, &transaction, 1, &cb.cmd) == VK_SUCCESS);
    vkrt_query_submit_result(&dev, &transaction, VK_SUCCESS);
    CHECK(resets == 2 && !marker_availability[2] && !marker_availability[3]);
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(out), out, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
          VK_SUCCESS);
    CHECK(out[0] == UINT64_C(0x100000081) && out[1] == 1 && acquisitions == 1);

    marker_availability[3] = 1;
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(out), out, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_NOT_READY);
    CHECK(out[0] == UINT64_C(0x100000081) && out[1] == 0 && slots[0].serial == 2);
    write_payload.value.value = UINT64_C(0xfedcba9876543210);
    write_payload.serial = 3;
    marker_availability[2] = 1;
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(out), out, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
          VK_SUCCESS);
    CHECK(out[0] == write_payload.value.value && out[1] == 1 && acquisitions == 3);

    /* Device execution order, not registration/list order, chooses the result. */
    write_op.harvested = false;
    reset_op.harvested = false;
    slots[0] = (struct query_host_slot){7, UINT64_MAX - 1, true, true};
    write_payload.serial = 0;
    reset_payload.serial = UINT64_MAX;
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(out), out, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS);
    CHECK(slots[0].serial == 0 && out[0] == write_payload.value.value && out[1] == 1);

    /* Rollback never erases the cached public value or waits for future work. */
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_prepare(&dev, &transaction, 1, &cb.cmd) == VK_SUCCESS);
    vkrt_query_submit_result(&dev, &transaction, VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(!write_op.submitted && !reset_op.submitted);
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(out), out, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS);
    CHECK(out[0] == write_payload.value.value && out[1] == 1);
    /* An allocation failure before this transaction's preflight must not erase
     * an earlier completed but still unharvested execution. */
    write_op.submitted = true;
    write_op.harvested = false;
    write_payload.serial = 1;
    marker_availability[2] = 1;
    transaction = (struct vkrt_query_submission){0};
    vkrt_query_submit_result(&dev, &transaction, VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(write_op.submitted);
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(out), out, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS);
    CHECK(slots[0].serial == 1 && out[0] == write_payload.value.value);
    CHECK(later(0, UINT64_MAX) && !later(UINT64_MAX, 0));
    /* Unsafe semaphore wait history belongs to the native queue and survives
     * separate submit calls. Only successful native idle clears it. */
    VkQueue queue = HANDLE(VkQueue, 6), other_queue = HANDLE(VkQueue, 7);
    struct query_scratch copy_scratch = {0};
    command.scratch = &copy_scratch;
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, true, 1, &cb.cmd) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkrt_query_submit_result(&dev, &transaction, VK_ERROR_FEATURE_NOT_PRESENT);
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, false, 1, &cb.cmd) == VK_SUCCESS);
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, true, 0, NULL) == VK_SUCCESS);
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, false, 1, &cb.cmd) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkrt_query_submit_result(&dev, &transaction, VK_ERROR_FEATURE_NOT_PRESENT);
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, true, 0, NULL) == VK_SUCCESS);
    vkrt_query_submit_result(&dev, &transaction, VK_SUCCESS);
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, false, 1, &cb.cmd) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, other_queue, false, 1, &cb.cmd) == VK_SUCCESS);
    pipeline_rollback_history(&dev, queue, cb.cmd);
    vkrt_query_queue_idle(&dev, other_queue, vkrt_query_queue_checkpoint(&dev));
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, false, 1, &cb.cmd) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkrt_query_queue_idle(&dev, queue, vkrt_query_queue_checkpoint(&dev));
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, false, 1, &cb.cmd) == VK_SUCCESS);
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, true, 0, NULL) == VK_SUCCESS);
    vkrt_query_submit_result(&dev, &transaction, VK_SUCCESS);
    vkrt_query_queue_idle(&dev, VK_NULL_HANDLE, vkrt_query_queue_checkpoint(&dev));
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, false, 1, &cb.cmd) == VK_SUCCESS);
    checkpoint_history(&dev, queue, other_queue, cb.cmd);
    admission_ordering(&dev, queue, other_queue, cb.cmd);
    saturated_history(&dev, queue, cb.cmd);
    command.scratch = NULL;
    while (state.queues) {
        struct query_queue *next = state.queues->next;
        query_queue_destroy(state.queues);
        state.queues = next;
    }
    /* Pool BDA alignment does not imply CPU mapping alignment. Query payload
     * acquisition and host reset must preserve every surrounding byte. */
    uint8_t unaligned_payload[27], unaligned_storage[27];
    memset(unaligned_payload, 0xa5, sizeof(unaligned_payload));
    memset(unaligned_storage, 0xa5, sizeof(unaligned_storage));
    const struct vkrt_as_query_value expected_value = {UINT64_C(0xfedcba1234567890), 1, 0};
    const uint64_t expected_serial = 99;
    memcpy(unaligned_payload + 1, &expected_value, sizeof(expected_value));
    memcpy(unaligned_payload + 17, &expected_serial, sizeof(expected_serial));
    write_op.payload.mapped = unaligned_payload + 1;
    write_op.harvested = false;
    write_op.submitted = true;
    reset_op.submitted = false;
    pool.storage.mapped = unaligned_storage + 1;
    slots[0] = (struct query_host_slot){0};
    marker_availability[2] = 1;
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(out), out, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS);
    CHECK(out[0] == expected_value.value && out[1] == 1 && slots[0].serial == expected_serial);
    ResetQueryPool(dev.device, handle, 0, 1);
    uint64_t stored_serial = 0;
    memcpy(&stored_serial, unaligned_storage + 17, sizeof(stored_serial));
    CHECK(stored_serial == 100 && !slots[0].available);
    for (size_t i = 1; i < 17; ++i) CHECK(!unaligned_storage[i]);
    CHECK(unaligned_storage[0] == 0xa5 && unaligned_storage[25] == 0xa5 && unaligned_storage[26] == 0xa5);
    CHECK(unaligned_payload[0] == 0xa5 && unaligned_payload[25] == 0xa5 && unaligned_payload[26] == 0xa5);
    VkQueueFamilyProperties family = {.queueFlags = VK_QUEUE_GRAPHICS_BIT, .timestampValidBits = 64};
    struct query_command_pool recording_pool = {.pool = HANDLE(VkCommandPool, 5), .family = 0};
    state.family_count = 1;
    state.families = &family;
    state.command_pools = &recording_pool;
    cb.pool = recording_pool.pool;
    CHECK(!record_allowed(&cb) && cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    cb.record_result = VK_SUCCESS;
    family.queueFlags |= VK_QUEUE_COMPUTE_BIT;
    recording_pool.flags = VK_COMMAND_POOL_CREATE_PROTECTED_BIT;
    CHECK(!record_allowed(&cb) && cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    cb.record_result = VK_SUCCESS;
    recording_pool.flags = 0;
    cb.native_active_queries = 1;
    CHECK(!record_allowed(&cb) && cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    cb.record_result = VK_SUCCESS;
    cb.native_active_queries = 0;
    cb.usage_flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    CHECK(!record_allowed(&cb) && cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    cb.record_result = VK_SUCCESS;
    cb.replay.journal = (struct vkrt_replay_journal *)(uintptr_t)1;
    dev.executions = (struct vkrt_execution_device *)(uintptr_t)1;
    CHECK(record_allowed(&cb));
    cb.has_virtual_queries = true;
    CHECK(vkrt_query_command_requires_replay(&cb));
    cb.replaying = true;
    CHECK(record_allowed(&cb) && !vkrt_query_command_requires_replay(&cb));
    vkrt_map_destroy(&vkrt_global.objects);
    if (!failures)
        printf(
            "Query publication epochs: PASS (acquire/cache/reset/rollback, pipeline retry history, idle checkpoint, admission "
            "ordering, saturation)\n");
    return failures ? 1 : 0;
}
