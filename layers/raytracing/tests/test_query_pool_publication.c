/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
static VkResult test_storage_allocate(struct vkrt_device_data *, VkDeviceSize, bool, struct vkrt_private_slice *);
static void test_storage_release(struct vkrt_device_data *, struct vkrt_private_slice *);
#define vkrt_private_pool_allocate test_storage_allocate
#define vkrt_private_pool_release test_storage_release
#include "vkrt_query_pool.c"
#undef vkrt_private_pool_allocate
#undef vkrt_private_pool_release
#include <stdatomic.h>
#include <errno.h>
#include <stddef.h>

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
struct fixture_pool {
    VkCommandPool pool;
    uint32_t family;
    VkCommandPoolCreateFlags flags;
};
static struct vkrt_device_data *pool_device;
static const struct fixture_pool *recorded_pool;
static bool marker_storage_active;
static VkResult marker_storage_allocate(struct vkrt_device_data *, VkDeviceSize, bool, struct vkrt_private_slice *);
static void marker_storage_release(struct vkrt_device_data *, struct vkrt_private_slice *);
bool vkrt_execution_command_pool_info(struct vkrt_device_data *dev, VkCommandPool pool, uint32_t *family,
                                      VkCommandPoolCreateFlags *flags) {
    if (dev != pool_device || !recorded_pool || recorded_pool->pool != pool) return false;
    *family = recorded_pool->family;
    *flags = recorded_pool->flags;
    return true;
}
enum copy_phase { COPY_WAIT_SCOPE, COPY_ACQUIRED, COPY_BOUND, COPY_PUSHED, COPY_DISPATCHED, COPY_VISIBLE, COPY_FINISHED };
static struct {
    struct vkrt_device_data *device;
    VkCommandBuffer command;
    VkPipelineStageFlags native_wait_scopes;
    struct vkrt_query_format_args args;
    struct vkrt_private_slice slice;
    uint8_t storage[64];
    enum copy_phase phase;
    unsigned allocations, releases, pushes, dispatches;
    bool active, acquired;
} copy_scope;
static VkResult test_storage_allocate(struct vkrt_device_data *dev, VkDeviceSize size, bool descriptor,
                                      struct vkrt_private_slice *slice) {
    if (marker_storage_active) return marker_storage_allocate(dev, size, descriptor, slice);
    CHECK(copy_scope.active && dev == copy_scope.device && size == 40 && descriptor);
    if (!copy_scope.active) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    copy_scope.slice = (struct vkrt_private_slice){.buffer = HANDLE(VkBuffer, 301),
                                                   .offset = 16,
                                                   .size = size,
                                                   .address = UINT64_C(0x100000003),
                                                   .mapping = copy_scope.storage,
                                                   .allocation = (struct vkrt_private_allocation *)(uintptr_t)1};
    *slice = copy_scope.slice;
    ++copy_scope.allocations;
    return VK_SUCCESS;
}
static void test_storage_release(struct vkrt_device_data *dev, struct vkrt_private_slice *slice) {
    if (marker_storage_active) {
        marker_storage_release(dev, slice);
        return;
    }
    CHECK(copy_scope.active && dev == copy_scope.device && copy_scope.phase == COPY_FINISHED);
    CHECK(slice->buffer == copy_scope.slice.buffer && slice->offset == copy_scope.slice.offset &&
          slice->size == copy_scope.slice.size && slice->address == copy_scope.slice.address &&
          slice->mapping == copy_scope.slice.mapping && slice->allocation == copy_scope.slice.allocation);
    *slice = (struct vkrt_private_slice){0};
    ++copy_scope.releases;
}
static VKAPI_ATTR void VKAPI_CALL copy_scope_barrier(VkCommandBuffer command, VkPipelineStageFlags source,
                                                     VkPipelineStageFlags destination, VkDependencyFlags flags,
                                                     uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count,
                                                     const VkBufferMemoryBarrier *buffers, uint32_t image_count,
                                                     const VkImageMemoryBarrier *images) {
    CHECK(command == copy_scope.command && !flags && memory_count == 1 && !buffer_count && !buffers && !image_count && !images);
    CHECK(memory->sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER && !memory->pNext);
    if (copy_scope.phase == COPY_WAIT_SCOPE) {
        /* Semaphore acquisition can reach this formatter through native stage
         * scopes, including TRANSFER waits in earlier queue submissions. The
         * bridge must cover those scopes before any physical CS slot read. */
        const bool covers_native_wait = source == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT ||
                                        (source & copy_scope.native_wait_scopes) == copy_scope.native_wait_scopes;
        CHECK(covers_native_wait);
        copy_scope.acquired = covers_native_wait && source == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
                              memory->srcAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) &&
                              destination == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT &&
                              memory->dstAccessMask == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        CHECK(copy_scope.acquired);
        copy_scope.phase = COPY_ACQUIRED;
    } else {
        CHECK(copy_scope.phase == COPY_DISPATCHED);
        CHECK(source == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && memory->srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT &&
              destination == VK_PIPELINE_STAGE_TRANSFER_BIT && memory->dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT);
        copy_scope.phase = COPY_VISIBLE;
    }
}
static VKAPI_ATTR void VKAPI_CALL copy_scope_bind(VkCommandBuffer command, VkPipelineBindPoint point, VkPipeline pipeline) {
    CHECK(command == copy_scope.command && point == VK_PIPELINE_BIND_POINT_COMPUTE && pipeline == HANDLE(VkPipeline, 302));
    CHECK(copy_scope.phase == COPY_ACQUIRED);
    copy_scope.phase = COPY_BOUND;
}
static VKAPI_ATTR void VKAPI_CALL copy_scope_push(VkCommandBuffer command, VkPipelineLayout layout, VkShaderStageFlags stages,
                                                  uint32_t offset, uint32_t size, const void *data) {
    CHECK(command == copy_scope.command && layout == HANDLE(VkPipelineLayout, 303) && stages == VK_SHADER_STAGE_COMPUTE_BIT &&
          !offset && size == sizeof(copy_scope.args));
    CHECK(copy_scope.phase == COPY_BOUND || copy_scope.phase == COPY_PUSHED);
    memcpy(&copy_scope.args, data, sizeof(copy_scope.args));
    copy_scope.phase = COPY_PUSHED;
    ++copy_scope.pushes;
}
static VKAPI_ATTR void VKAPI_CALL copy_scope_dispatch(VkCommandBuffer command, uint32_t x, uint32_t y, uint32_t z) {
    CHECK(command == copy_scope.command && x == 1 && y == 1 && z == 1 && copy_scope.phase == COPY_PUSHED);
    CHECK(copy_scope.acquired); /* Check the bridge at the actual formatter execution endpoint. */
    CHECK(copy_scope.args.input == 4096 && copy_scope.args.input_capacity == 64 &&
          copy_scope.args.output == copy_scope.slice.address && copy_scope.args.output_capacity == 40 &&
          copy_scope.args.stride == 24 && copy_scope.args.first_query == 1 && copy_scope.args.count == 2 &&
          copy_scope.args.flags == (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) && !copy_scope.args.reserved);
    copy_scope.phase = COPY_DISPATCHED;
    ++copy_scope.dispatches;
}
static VKAPI_ATTR void VKAPI_CALL copy_scope_transfer(VkCommandBuffer command, VkBuffer source, VkBuffer destination,
                                                      uint32_t count, const VkBufferCopy *regions) {
    CHECK(command == copy_scope.command && source == copy_scope.slice.buffer && destination == HANDLE(VkBuffer, 304));
    CHECK(copy_scope.phase == COPY_VISIBLE && count == 2 && copy_scope.dispatches == 1);
    for (uint32_t i = 0; i < count; ++i)
        CHECK(regions[i].srcOffset == copy_scope.slice.offset + i * 24 && regions[i].dstOffset == 16 + i * 24 &&
              regions[i].size == 16);
    copy_scope.phase = COPY_FINISHED;
}
static void native_wait_to_formatter_scope(void) {
    memset(&copy_scope, 0, sizeof(copy_scope));
    VkQueueFamilyProperties family = {.queueFlags = VK_QUEUE_COMPUTE_BIT, .timestampValidBits = 0};
    struct fixture_pool recording_pool = {.pool = HANDLE(VkCommandPool, 305), .family = 0};
    struct vkrt_query_device queries = {
        .families = &family, .family_count = 1, .layout = HANDLE(VkPipelineLayout, 303), .format = HANDLE(VkPipeline, 302)};
    vkrt_mutex_init(&queries.lock);
    struct vkrt_device_data dev = {.queries = &queries, .max_build_dispatch_x = 1, .query_marker_uses_events = true};
    pool_device = &dev;
    recorded_pool = &recording_pool;
    dev.vtable.CmdPipelineBarrier = copy_scope_barrier;
    dev.vtable.CmdBindPipeline = copy_scope_bind;
    dev.vtable.CmdPushConstants = copy_scope_push;
    dev.vtable.CmdDispatch = copy_scope_dispatch;
    dev.vtable.CmdCopyBuffer = copy_scope_transfer;
    struct vkrt_cmd_buffer_data cb = {
        .cmd = HANDLE(VkCommandBuffer, 306), .device_data = &dev, .pool = recording_pool.pool, .replaying = true};
    struct query_pool pool = {.magic = QUERY_POOL_MAGIC,
                              .dev = &dev,
                              .type = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                              .count = 4,
                              .storage = {.address = 4096}};
    VkQueryPool handle = (VkQueryPool)(uintptr_t)&pool;
    vkrt_map_object(VKRT_HKEY(cb.cmd), &cb);
    vkrt_map_object(VKRT_HKEY(handle), &pool);
    copy_scope.device = &dev;
    copy_scope.command = cb.cmd;
    copy_scope.native_wait_scopes = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    copy_scope.active = true;
    PFN_vkCmdCopyQueryPoolResults copy = (PFN_vkCmdCopyQueryPoolResults)vkrt_query_pool_proc("vkCmdCopyQueryPoolResults");
    CHECK(copy);
    copy(cb.cmd, handle, 1, 2, HANDLE(VkBuffer, 304), 16, 24, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    CHECK(cb.record_result == VK_SUCCESS && cb.has_virtual_queries && copy_scope.phase == COPY_FINISHED);
    CHECK(copy_scope.allocations == 1 && copy_scope.pushes == 2 && copy_scope.dispatches == 1 && !copy_scope.releases);
    CHECK(vkrt_query_command_reset(&cb) == VK_SUCCESS && !cb.queries && !queries.commands && copy_scope.releases == 1);
    copy_scope.active = false;
    vkrt_unmap_object(VKRT_HKEY(cb.cmd));
    vkrt_unmap_object(VKRT_HKEY(handle));
#ifndef _WIN32
    pthread_mutex_destroy(&queries.lock);
#endif
}
static struct {
    VkCommandBuffer copy, publication;
    struct publish_args args;
    unsigned barriers, dispatches, timestamps, events;
    bool reset, native_copy_pending;
} publication_scope;
static VKAPI_ATTR void VKAPI_CALL scope_copy(VkCommandBuffer command, VkBuffer source, VkBuffer destination, uint32_t count,
                                             const VkBufferCopy *regions) {
    CHECK(command == publication_scope.copy && command != publication_scope.publication);
    CHECK(source && destination && count == 1 && regions[0].size == 8);
    publication_scope.native_copy_pending = true;
}
static VKAPI_ATTR void VKAPI_CALL scope_barrier(VkCommandBuffer command, VkPipelineStageFlags source,
                                                VkPipelineStageFlags destination, VkDependencyFlags flags, uint32_t memory_count,
                                                const VkMemoryBarrier *memory, uint32_t buffer_count,
                                                const VkBufferMemoryBarrier *buffers, uint32_t image_count,
                                                const VkImageMemoryBarrier *images) {
    CHECK(command == publication_scope.publication && !flags && memory_count == 1 && !buffer_count && !buffers && !image_count &&
          !images);
    CHECK(memory->sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER && !memory->pNext);
    if (!publication_scope.barriers) {
        VkPipelineStageFlags required = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        if (publication_scope.reset) {
            required |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            access |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        CHECK(source == required && memory->srcAccessMask == access);
        CHECK(destination == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT &&
              memory->dstAccessMask == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT));
        /* Query reset's implicit first scope includes the earlier COPY's
         * native transfer, even when it was recorded in a different CB. */
        if ((source & VK_PIPELINE_STAGE_TRANSFER_BIT) && (destination & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT))
            publication_scope.native_copy_pending = false;
    } else {
        CHECK(source == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT &&
              destination == (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT) &&
              memory->srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT &&
              memory->dstAccessMask == (VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT));
    }
    ++publication_scope.barriers;
}
static VKAPI_ATTR void VKAPI_CALL scope_bind(VkCommandBuffer command, VkPipelineBindPoint point, VkPipeline pipeline) {
    CHECK(command == publication_scope.publication && point == VK_PIPELINE_BIND_POINT_COMPUTE && pipeline);
}
static VKAPI_ATTR void VKAPI_CALL scope_push(VkCommandBuffer command, VkPipelineLayout layout, VkShaderStageFlags stages,
                                             uint32_t offset, uint32_t size, const void *data) {
    CHECK(command == publication_scope.publication && layout && stages == VK_SHADER_STAGE_COMPUTE_BIT && !offset &&
          size == sizeof(publication_scope.args));
    memcpy(&publication_scope.args, data, sizeof(publication_scope.args));
}
static VKAPI_ATTR void VKAPI_CALL scope_dispatch(VkCommandBuffer command, uint32_t x, uint32_t y, uint32_t z) {
    CHECK(command == publication_scope.publication && x == 1 && y == 1 && z == 1);
    if (!publication_scope.dispatches) {
        CHECK(publication_scope.args.count == 1 && publication_scope.args.reset == publication_scope.reset);
        CHECK(publication_scope.native_copy_pending != publication_scope.reset);
    } else {
        CHECK(!publication_scope.args.count);
    }
    ++publication_scope.dispatches;
}
static VKAPI_ATTR void VKAPI_CALL scope_timestamp(VkCommandBuffer command, VkPipelineStageFlagBits stage, VkQueryPool pool,
                                                  uint32_t query) {
    CHECK(command == publication_scope.publication && stage == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && pool && !query);
    CHECK(publication_scope.barriers == 2 && publication_scope.dispatches == 2);
    ++publication_scope.timestamps;
}
static VKAPI_ATTR void VKAPI_CALL scope_event(VkCommandBuffer command, VkEvent event, VkPipelineStageFlags stage) {
    CHECK(command == publication_scope.publication && event && stage == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    CHECK(publication_scope.barriers == 2 && publication_scope.dispatches == 2 && !publication_scope.args.count);
    ++publication_scope.events;
}
static void reset_native_copy_order(void) {
    struct vkrt_query_device queries = {.layout = HANDLE(VkPipelineLayout, 101), .publish = HANDLE(VkPipeline, 102)};
    struct vkrt_device_data dev = {.queries = &queries, .max_build_dispatch_x = 1, .query_marker_uses_events = false};
    dev.vtable.CmdCopyBuffer = scope_copy;
    dev.vtable.CmdPipelineBarrier = scope_barrier;
    dev.vtable.CmdBindPipeline = scope_bind;
    dev.vtable.CmdPushConstants = scope_push;
    dev.vtable.CmdDispatch = scope_dispatch;
    dev.vtable.CmdWriteTimestamp = scope_timestamp;
    dev.vtable.CmdSetEvent = scope_event;
    struct vkrt_cmd_buffer_data cb = {.cmd = HANDLE(VkCommandBuffer, 104), .device_data = &dev};
    struct query_pool pool = {.count = 1, .storage = {.address = 4096}};
    struct query_operation operation = {
        .pool = &pool, .count = 1, .marker = HANDLE(VkQueryPool, 105), .payload = {.address = 8192}};
    const VkBufferCopy region = {.size = 8};
    for (unsigned mode = 0; mode < 4; ++mode) {
        unsigned reset = mode & 1;
        dev.query_marker_uses_events = mode >= 2;
        operation.event = dev.query_marker_uses_events ? HANDLE(VkEvent, 108) : VK_NULL_HANDLE;
        memset(&publication_scope, 0, sizeof(publication_scope));
        publication_scope.copy = HANDLE(VkCommandBuffer, 103);
        publication_scope.publication = cb.cmd;
        publication_scope.reset = reset != 0;
        dev.vtable.CmdCopyBuffer(publication_scope.copy, HANDLE(VkBuffer, 106), HANDLE(VkBuffer, 107), 1, &region);
        CHECK(publication_scope.native_copy_pending);
        publish_operation(&cb, &operation, reset != 0);
        CHECK(publication_scope.barriers == 2 && publication_scope.dispatches == 2 &&
              publication_scope.timestamps == !dev.query_marker_uses_events &&
              publication_scope.events == dev.query_marker_uses_events);
        CHECK(publication_scope.native_copy_pending != (reset != 0));
    }
}
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
static void pipeline_rollback_admission(struct vkrt_device_data *dev, VkQueue queue) {
    struct vkrt_device_dispatch saved = dev->vtable;
    dev->vtable.CreatePipelineLayout = make_layout;
    dev->vtable.CreateShaderModule = make_shader;
    dev->vtable.CreateComputePipelines = make_compute;
    dev->vtable.DestroyShaderModule = drop_shader;
    dev->vtable.DestroyPipelineLayout = drop_layout;
    dev->vtable.DestroyPipeline = drop_pipeline;
    struct vkrt_query_submission transaction = {0};
    CHECK(vkrt_query_submit_begin(dev, &transaction, queue) == VK_SUCCESS);
    struct vkrt_query_admission *admission = transaction.admission;
    vkrt_query_submit_end(dev, &transaction);
    for (pipeline_fail_at = 1; pipeline_fail_at <= 3; ++pipeline_fail_at) {
        pipeline_calls = destroyed_modules = destroyed_layouts = destroyed_pipelines = 0;
        struct query_queue *queues = dev->queries->queues;
        vkrt_mutex_lock(&dev->queries->lock);
        CHECK(initialize_pipelines(dev) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        vkrt_mutex_unlock(&dev->queries->lock);
        CHECK(dev->queries->queues == queues);
        CHECK(!dev->queries->layout && !dev->queries->property && !dev->queries->publish && !dev->queries->format);
        CHECK(destroyed_modules == pipeline_fail_at && destroyed_layouts == 1 && destroyed_pipelines == pipeline_fail_at - 1);
        CHECK(vkrt_query_submit_begin(dev, &transaction, queue) == VK_SUCCESS && transaction.admission == admission);
        vkrt_query_submit_end(dev, &transaction);
    }
    dev->vtable = saved;
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
    atomic_uint publications;
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
        /* Deterministic native-SUCCESS -> query-publication pause. */
        event_signal(&f->native_success);
        CHECK(event_wait(&f->publish, 5000));
        vkrt_query_submit_result(f->dev, &transaction, VK_SUCCESS);
        atomic_store(&f->publications, 1);
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
        CHECK(atomic_load(&f->publications) == 1);
        vkrt_query_submit_result(f->dev, &transaction, VK_SUCCESS);
        vkrt_query_submit_end(f->dev, &transaction);
        event_signal(&f->b_done);
    } else {
        CHECK(vkrt_query_submit_begin(f->dev, &transaction, f->other_queue) == VK_SUCCESS);
        CHECK(atomic_load(&f->publications) == 0);
        vkrt_query_submit_result(f->dev, &transaction, VK_SUCCESS);
        vkrt_query_submit_end(f->dev, &transaction);
        event_signal(&f->c_done);
    }
    CHECK(!transaction.admission);
    return 0;
}
static void admission_ordering(struct vkrt_device_data *dev, VkQueue queue, VkQueue other_queue) {
    struct admission_fixture fixture = {.dev = dev, .queue = queue, .other_queue = other_queue};
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
}

struct query_debug_tracker {
    struct vkrt_device_data *dev;
    unsigned live, allocations, frees;
};
static VKAPI_ATTR void *VKAPI_CALL debug_query_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    struct query_debug_tracker *tracker = user;
    CHECK(scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE && alignment <= _Alignof(max_align_t));
    CHECK(!vkrt_query_pool_owns(tracker->dev, UINT64_MAX));
    void *memory = malloc(size);
    if (memory) {
        ++tracker->live;
        ++tracker->allocations;
    }
    return memory;
}
static VKAPI_ATTR void VKAPI_CALL debug_query_free(void *user, void *memory) {
    struct query_debug_tracker *tracker = user;
    /* Reenter the typed registry while forget holds debug-lock. This would
     * deadlock if DestroyQueryPool/device finish still held queries->lock. */
    CHECK(!vkrt_query_pool_owns(tracker->dev, UINT64_MAX) && tracker->live);
    --tracker->live;
    ++tracker->frees;
    free(memory);
}
static bool debug_query_owned(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle) {
    return type == VK_OBJECT_TYPE_QUERY_POOL && vkrt_query_pool_owns(dev, handle);
}
static VkQueryPool debug_query_record(struct vkrt_device_data *dev) {
    /* Populate the real typed registry with host-only storage. The public GPU
     * fixture separately exercises the full AS-query CreateQueryPool path. */
    struct query_pool *pool = calloc(1, sizeof(*pool));
    CHECK(pool != NULL);
    if (!pool) return VK_NULL_HANDLE;
    pool->magic = QUERY_POOL_MAGIC;
    pool->dev = dev;
    pool->type = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    pool->count = 1;
    pool->slots = calloc(1, sizeof(*pool->slots));
    CHECK(pool->slots != NULL);
    vkrt_mutex_lock(&dev->queries->lock);
    pool->next = dev->queries->pools;
    dev->queries->pools = pool;
    vkrt_map_object((uint64_t)(uintptr_t)pool, pool);
    vkrt_mutex_unlock(&dev->queries->lock);
    return (VkQueryPool)(uintptr_t)pool;
}
static void debug_query_annotations(struct vkrt_device_data *dev, VkQueryPool handle) {
    PFN_vkSetDebugUtilsObjectNameEXT name = (void *)vkrt_debug_objects_proc("vkSetDebugUtilsObjectNameEXT");
    PFN_vkSetDebugUtilsObjectTagEXT tag = (void *)vkrt_debug_objects_proc("vkSetDebugUtilsObjectTagEXT");
    const VkDebugUtilsObjectNameInfoEXT info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL, VK_OBJECT_TYPE_QUERY_POOL,
                                                (uint64_t)(uintptr_t)handle, "AS property query pool"};
    const uint32_t data = 0x12345678;
    const VkDebugUtilsObjectTagInfoEXT tagging = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT,
                                                  NULL,
                                                  VK_OBJECT_TYPE_QUERY_POOL,
                                                  (uint64_t)(uintptr_t)handle,
                                                  7,
                                                  sizeof(data),
                                                  &data};
    CHECK(name(dev->device, &info) == VK_SUCCESS && tag(dev->device, &tagging) == VK_SUCCESS);
}
static void debug_query_lifetime(void) {
    struct vkrt_device_data dev = {.device = HANDLE(VkDevice, 901)};
    dev.queries = calloc(1, sizeof(*dev.queries));
    CHECK(dev.queries != NULL);
    if (!dev.queries) return;
    vkrt_mutex_init(&dev.queries->lock);
    vkrt_map_object(VKRT_HKEY(dev.device), &dev);
    struct query_debug_tracker tracker = {.dev = &dev};
    VkAllocationCallbacks allocator = {.pUserData = &tracker, .pfnAllocation = debug_query_allocate, .pfnFree = debug_query_free};
    CHECK(vkrt_debug_objects_init(&dev, &allocator, debug_query_owned) == VK_SUCCESS);
    memset(&copy_scope, 0, sizeof(copy_scope));
    copy_scope.active = true;
    copy_scope.device = &dev;
    copy_scope.phase = COPY_FINISHED;
    CHECK(!vkrt_query_pool_owns(&dev, 0) && !vkrt_query_pool_owns(&dev, UINT64_MAX));
    VkQueryPool pool = debug_query_record(&dev);
    uint64_t handle = (uint64_t)(uintptr_t)pool;
    CHECK(vkrt_query_pool_owns(&dev, handle));
    CHECK(!vkrt_query_pool_owns(&dev, handle ^ (UINT64_C(1) << 63)));
    struct vkrt_device_data foreign = {.device = HANDLE(VkDevice, 902), .queries = dev.queries};
    CHECK(!vkrt_query_pool_owns(&foreign, handle));
    debug_query_annotations(&dev, pool);
    CHECK(tracker.live == 4);
    DestroyQueryPool(dev.device, pool, NULL);
    CHECK(!vkrt_query_pool_owns(&dev, handle) && tracker.live == 1);
    for (unsigned i = 0; i < 2; ++i) debug_query_annotations(&dev, debug_query_record(&dev));
    CHECK(tracker.live == 7);
    vkrt_query_device_finish(&dev);
    CHECK(!dev.queries && tracker.live == 1 && copy_scope.releases == 3);
    vkrt_debug_objects_finish(&dev);
    CHECK(!tracker.live && tracker.allocations == tracker.frees);
    vkrt_unmap_object(VKRT_HKEY(dev.device));
    copy_scope.active = false;
}
struct marker_event_record {
    VkEvent event;
    VkResult status;
    bool submitted, fence_complete, destroyed;
};
static struct {
    struct vkrt_device_data *dev;
    struct marker_event_record events[16];
    uint8_t payloads[16][27];
    bool released[16], fail_storage;
    unsigned allocated, released_count, created, destroyed, polls;
    VkResult create_result;
} event_fixture;
static struct marker_event_record *event_record(VkEvent event) {
    for (unsigned i = 0; i < event_fixture.created; ++i)
        if (event_fixture.events[i].event == event) return &event_fixture.events[i];
    CHECK(false);
    return NULL;
}
static VkResult marker_storage_allocate(struct vkrt_device_data *dev, VkDeviceSize size, bool descriptor,
                                        struct vkrt_private_slice *slice) {
    CHECK(dev == event_fixture.dev && size == 24 && !descriptor);
    if (event_fixture.fail_storage) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    CHECK(event_fixture.allocated < VKRT_ARRAY_SIZE(event_fixture.payloads));
    unsigned index = event_fixture.allocated++;
    memset(event_fixture.payloads[index], 0xa5, sizeof(event_fixture.payloads[index]));
    *slice = (struct vkrt_private_slice){.buffer = HANDLE(VkBuffer, 1000 + index),
                                         .offset = 8,
                                         .size = size,
                                         .address = UINT64_C(0x100000000) + 32 * index,
                                         .mapping = event_fixture.payloads[index] + 1,
                                         .allocation = (struct vkrt_private_allocation *)(uintptr_t)(index + 1)};
    return VK_SUCCESS;
}
static void marker_storage_release(struct vkrt_device_data *dev, struct vkrt_private_slice *slice) {
    if (!slice->allocation) return;
    CHECK(dev == event_fixture.dev);
    size_t index = (uintptr_t)slice->allocation - 1;
    CHECK(index < event_fixture.allocated && !event_fixture.released[index]);
    CHECK(event_fixture.payloads[index][0] == 0xa5 && event_fixture.payloads[index][25] == 0xa5 &&
          event_fixture.payloads[index][26] == 0xa5);
    event_fixture.released[index] = true;
    ++event_fixture.released_count;
    *slice = (struct vkrt_private_slice){0};
}
static VKAPI_ATTR VkResult VKAPI_CALL marker_create_event(VkDevice device, const VkEventCreateInfo *info,
                                                          const VkAllocationCallbacks *allocator, VkEvent *event) {
    CHECK(device == event_fixture.dev->device && info->sType == VK_STRUCTURE_TYPE_EVENT_CREATE_INFO && !info->pNext &&
          !info->flags && !allocator);
    if (event_fixture.create_result != VK_SUCCESS) return event_fixture.create_result;
    CHECK(event_fixture.created < VKRT_ARRAY_SIZE(event_fixture.events));
    unsigned index = event_fixture.created++;
    struct marker_event_record *record = &event_fixture.events[index];
    record->event = HANDLE(VkEvent, 2000 + index);
    record->status = VK_EVENT_RESET;
    *event = record->event;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL marker_destroy_event(VkDevice device, VkEvent event, const VkAllocationCallbacks *allocator) {
    CHECK(device == event_fixture.dev->device && !allocator);
    struct marker_event_record *record = event_record(event);
    CHECK(record && !record->destroyed && (!record->submitted || record->fence_complete));
    if (record) record->destroyed = true;
    ++event_fixture.destroyed;
}
static VKAPI_ATTR VkResult VKAPI_CALL marker_get_event(VkDevice device, VkEvent event) {
    CHECK(device == event_fixture.dev->device);
    struct marker_event_record *record = event_record(event);
    CHECK(record && !record->destroyed);
    ++event_fixture.polls;
    return record ? record->status : VK_ERROR_UNKNOWN;
}
static VKAPI_ATTR void VKAPI_CALL marker_families(VkPhysicalDevice physical, uint32_t *count, VkQueueFamilyProperties *families) {
    CHECK(physical == HANDLE(VkPhysicalDevice, 800));
    if (families) *families = (VkQueueFamilyProperties){.queueFlags = VK_QUEUE_COMPUTE_BIT, .timestampValidBits = 0};
    *count = 1;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL marker_instance_proc(VkInstance instance, const char *name) {
    CHECK(instance == HANDLE(VkInstance, 801) && !strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties"));
    return (PFN_vkVoidFunction)marker_families;
}
static void event_backend_admission(void) {
    struct vkrt_instance_data instance = {.instance = HANDLE(VkInstance, 801)};
    instance.vtable.GetInstanceProcAddr = marker_instance_proc;
    struct vkrt_device_data dev = {.physical_device = HANDLE(VkPhysicalDevice, 800),
                                   .instance_data = &instance,
                                   .virtual_rt = true,
                                   .query_marker_uses_events = true};
    CHECK(vkrt_query_device_init(&dev) == VK_ERROR_FEATURE_NOT_PRESENT && !dev.queries);
    dev.virtual_rt = false;
    CHECK(vkrt_query_device_init(&dev) == VK_SUCCESS && !dev.queries);
    dev.query_backend_enabled = true;
    CHECK(vkrt_query_device_init(&dev) == VK_ERROR_FEATURE_NOT_PRESENT && !dev.queries);
    dev.vtable.CmdCopyBuffer = scope_copy;
    dev.vtable.CreateEvent = marker_create_event;
    dev.vtable.DestroyEvent = marker_destroy_event;
    dev.vtable.GetEventStatus = marker_get_event;
    dev.vtable.CmdSetEvent = scope_event;
    const struct vkrt_device_dispatch full = dev.vtable;
    for (unsigned missing = 0; missing < 4; ++missing) {
        dev.vtable = full;
        if (missing == 0) dev.vtable.CreateEvent = NULL;
        if (missing == 1) dev.vtable.DestroyEvent = NULL;
        if (missing == 2) dev.vtable.GetEventStatus = NULL;
        if (missing == 3) dev.vtable.CmdSetEvent = NULL;
        CHECK(vkrt_query_device_init(&dev) == VK_ERROR_FEATURE_NOT_PRESENT && !dev.queries);
    }
    dev.vtable = full;
    CHECK(!dev.vtable.ResetQueryPool && !dev.vtable.GetQueryPoolResults && !dev.vtable.CmdWriteTimestamp);
    CHECK(vkrt_query_device_init(&dev) == VK_SUCCESS && dev.queries && !dev.queries->families[0].timestampValidBits);
    vkrt_query_device_finish(&dev);
    dev.query_marker_uses_events = false;
    CHECK(vkrt_query_device_init(&dev) == VK_ERROR_FEATURE_NOT_PRESENT && !dev.queries);
    dev.query_marker_uses_events = true;
    instance.vtable.GetInstanceProcAddr = NULL;
    CHECK(vkrt_query_device_init(&dev) == VK_ERROR_INITIALIZATION_FAILED && !dev.queries);
}
static void event_occurrences(void) {
    memset(&event_fixture, 0, sizeof(event_fixture));
    VkQueueFamilyProperties family = {.queueFlags = VK_QUEUE_COMPUTE_BIT, .timestampValidBits = 0};
    struct fixture_pool recording_pool = {.pool = HANDLE(VkCommandPool, 810), .family = 0};
    struct vkrt_query_device state = {.families = &family, .family_count = 1};
    vkrt_mutex_init(&state.lock);
    struct vkrt_device_data dev = {.device = HANDLE(VkDevice, 811), .queries = &state, .query_marker_uses_events = true};
    dev.vtable.CreateEvent = marker_create_event;
    dev.vtable.DestroyEvent = marker_destroy_event;
    dev.vtable.GetEventStatus = marker_get_event;
    struct query_host_slot slot = {0};
    uint8_t storage[24] = {0};
    struct query_pool pool = {.magic = QUERY_POOL_MAGIC,
                              .dev = &dev,
                              .type = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                              .count = 1,
                              .slots = &slot,
                              .storage = {.mapped = storage}};
    VkQueryPool handle = (VkQueryPool)(uintptr_t)&pool;
    struct vkrt_cmd_buffer_data cb[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        cb[i] = (struct vkrt_cmd_buffer_data){
            .cmd = HANDLE(VkCommandBuffer, 812 + i), .device_data = &dev, .pool = recording_pool.pool, .replaying = true};
        vkrt_map_object(VKRT_HKEY(cb[i].cmd), &cb[i]);
    }
    vkrt_map_object(VKRT_HKEY(dev.device), &dev);
    vkrt_map_object(VKRT_HKEY(handle), &pool);
    pool_device = &dev;
    recorded_pool = &recording_pool;
    event_fixture.dev = &dev;
    marker_storage_active = true;
    const bool accepted = record_allowed(&cb[0]);
    CHECK(accepted);
    if (!accepted) goto done;
    struct vkrt_cmd_buffer_data source = {.cmd = HANDLE(VkCommandBuffer, 816),
                                          .device_data = &dev,
                                          .pool = recording_pool.pool,
                                          .usage_flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    source.replay.journal = (struct vkrt_replay_journal *)(uintptr_t)1;
    dev.executions = (struct vkrt_execution_device *)(uintptr_t)1;
    vkrt_map_object(VKRT_HKEY(source.cmd), &source);
    const VkAccelerationStructureKHR as = HANDLE(VkAccelerationStructureKHR, 817);
    CmdResetQueryPool(source.cmd, handle, 0, 1);
    CmdWriteAccelerationStructuresPropertiesKHR(source.cmd, 1, &as, pool.type, handle, 0);
    CmdCopyQueryPoolResults(source.cmd, handle, 0, 1, HANDLE(VkBuffer, 818), 0, 16,
                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    CHECK(source.has_virtual_queries && !source.queries && source.record_result == VK_SUCCESS && !event_fixture.allocated &&
          !event_fixture.created && !state.commands);
    vkrt_unmap_object(VKRT_HKEY(source.cmd));
    dev.executions = NULL;
    dev.query_marker_uses_events = false;
    CHECK(!record_allowed(&cb[0]) && cb[0].record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    cb[0].record_result = VK_SUCCESS;
    dev.query_marker_uses_events = true;
    event_fixture.fail_storage = true;
    CHECK(!new_operation(&cb[0], &pool, 0, 1) && cb[0].record_result == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(!event_fixture.allocated && !event_fixture.created && !cb[0].queries->head);
    event_fixture.fail_storage = false;
    cb[0].record_result = VK_SUCCESS;
    event_fixture.create_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(!new_operation(&cb[0], &pool, 0, 1) && cb[0].record_result == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(event_fixture.allocated == 1 && event_fixture.released_count == 1 && !event_fixture.created && !cb[0].queries->head);
    /* An earlier source error prevents any further marker/slice allocation. */
    CHECK(!new_operation(&cb[0], &pool, 0, 1) && event_fixture.allocated == 1);
    cb[0].record_result = VK_SUCCESS;
    event_fixture.create_result = VK_SUCCESS;
    struct query_operation *ops[3];
    for (unsigned i = 0; i < 3; ++i) {
        ops[i] = new_operation(&cb[i], &pool, 0, 1);
        CHECK(ops[i] && ops[i]->event && !ops[i]->marker && !ops[i]->submitted);
        const struct vkrt_as_query_value value = {UINT64_C(0x1234567800000000) + i, 1, 0};
        const uint64_t serial = i + 1;
        memcpy(ops[i]->payload.mapped, &value, sizeof(value));
        memcpy((uint8_t *)ops[i]->payload.mapped + 16, &serial, sizeof(serial));
    }
    CHECK(ops[0]->event != ops[1]->event && ops[1]->event != ops[2]->event && ops[0]->payload.address != ops[1]->payload.address &&
          !event_fixture.destroyed);
    struct vkrt_query_submission transactions[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(vkrt_query_submit_prepare(&dev, &transactions[i], 1, &cb[i].cmd) == VK_SUCCESS);
        event_record(ops[i]->event)->submitted = true;
        vkrt_query_submit_result(&dev, &transactions[i], VK_SUCCESS);
    }
    uint64_t output[4] = {11, 13, 17, 19};
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(output), output, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_NOT_READY);
    CHECK(output[0] == 11 && output[1] == 0 && output[2] == 17 && output[3] == 19 && !slot.initialized);
    const VkResult errors[] = {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST, VK_ERROR_UNKNOWN};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(errors); ++i) {
        event_record(ops[0]->event)->status = errors[i];
        CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(output), output, 32, VK_QUERY_RESULT_64_BIT) == errors[i]);
        CHECK(!ops[0]->harvested && !slot.initialized && !event_fixture.destroyed);
    }
    event_record(ops[0]->event)->status = VK_SUCCESS; /* Not a legal event status. Never treat it as SET. */
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(output), output, 32, VK_QUERY_RESULT_64_BIT) == VK_ERROR_UNKNOWN);
    event_record(ops[0]->event)->status = VK_ERROR_UNKNOWN;
    ResetQueryPool(dev.device, handle, 0, 1);
    CHECK(state.error == VK_ERROR_UNKNOWN && !slot.initialized);
    state.error = VK_SUCCESS;
    event_record(ops[0]->event)->status = VK_EVENT_RESET;
    event_record(ops[1]->event)->status = VK_EVENT_SET;
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(output), output, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS);
    CHECK(output[0] == UINT64_C(0x1234567800000001) && output[1] == 1 && slot.serial == 2 && !ops[0]->harvested);
    CHECK(!event_fixture.destroyed && event_fixture.released_count == 1); /* SET is not batch-fence retirement. */
    event_record(ops[0]->event)->status = VK_EVENT_SET;
    CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(output), output, 32, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS);
    CHECK(slot.serial == 2 && output[0] == UINT64_C(0x1234567800000001)); /* Older event observation cannot overwrite. */
    CHECK(vkrt_query_submit_prepare(&dev, &transactions[0], 1, &cb[0].cmd) == VK_ERROR_FEATURE_NOT_PRESENT);
    /* Unknown submission ownership is quarantined by execution replay, never
     * retired merely because its event is SET. Keep it until a modeled idle. */
    ops[2]->prepared = &transactions[2];
    vkrt_query_submit_result(&dev, &transactions[2], VK_ERROR_UNKNOWN);
    CHECK(!ops[2]->submitted && !event_fixture.destroyed && event_fixture.released_count == 1);
    for (unsigned i = 0; i < 3; ++i) {
        event_record(ops[i]->event)->fence_complete = true;
        CHECK(vkrt_query_command_reset(&cb[i]) == VK_SUCCESS && !cb[i].queries);
        vkrt_unmap_object(VKRT_HKEY(cb[i].cmd));
    }
    struct vkrt_cmd_buffer_data error_cb = {
        .cmd = HANDLE(VkCommandBuffer, 815), .device_data = &dev, .pool = recording_pool.pool, .replaying = true};
    const VkResult completed_statuses[] = {VK_EVENT_RESET, VK_ERROR_DEVICE_LOST};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(completed_statuses); ++i) {
        state.error = VK_SUCCESS;
        struct query_operation *error_op = new_operation(&error_cb, &pool, 0, 1);
        CHECK(error_op && error_op->event);
        error_op->submitted = true;
        struct marker_event_record *error_event = event_record(error_op->event);
        error_event->submitted = error_event->fence_complete = true;
        error_event->status = completed_statuses[i];
        const VkResult expected = i == 0 ? VK_ERROR_UNKNOWN : VK_ERROR_DEVICE_LOST;
        /* Only this modeled completion fence permits retirement. RESET while
         * merely polling above stayed pending; complete+RESET must fail. */
        CHECK(vkrt_query_command_reset(&error_cb) == expected && state.error == expected);
        CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(output), output, 32, VK_QUERY_RESULT_64_BIT) == expected);
        CHECK(slot.serial == 2 && slot.value == UINT64_C(0x1234567800000001));
    }
    CHECK(!state.commands && event_fixture.created == 5 && event_fixture.destroyed == 5 &&
          event_fixture.allocated == event_fixture.released_count);
done:
    marker_storage_active = false;
    for (unsigned i = 0; i < 3; ++i) vkrt_unmap_object(VKRT_HKEY(cb[i].cmd));
    vkrt_unmap_object(VKRT_HKEY(handle));
    vkrt_unmap_object(VKRT_HKEY(dev.device));
#ifndef _WIN32
    pthread_mutex_destroy(&state.lock);
#endif
}
static struct {
    struct vkrt_device_data *device;
    VkQueryPool handle;
    VkQueryPoolCreateInfo replacement;
    VkAllocationCallbacks allocator;
    VkResult result;
    unsigned allocations, frees, creates, destroys, begins, ends, indexed_begins, indexed_ends;
    bool fail_allocation, reuse_on_destroy;
} native_scope;
static void *VKAPI_CALL scope_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    CHECK(user == &native_scope && alignment == _Alignof(struct native_query_pool) && scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (native_scope.fail_allocation) return NULL;
    ++native_scope.allocations;
    return malloc(size);
}
static void VKAPI_CALL scope_free(void *user, void *allocation) {
    CHECK(user == &native_scope && allocation);
    ++native_scope.frees;
    free(allocation);
}
static VKAPI_ATTR VkResult VKAPI_CALL scope_create(VkDevice device, const VkQueryPoolCreateInfo *info,
                                                   const VkAllocationCallbacks *allocator, VkQueryPool *out) {
    CHECK(device == native_scope.device->device && info && allocator == &native_scope.allocator);
    ++native_scope.creates;
    if (native_scope.result == VK_SUCCESS) *out = native_scope.handle;
    return native_scope.result;
}
static VKAPI_ATTR void VKAPI_CALL scope_destroy(VkDevice device, VkQueryPool pool, const VkAllocationCallbacks *allocator) {
    CHECK(device == native_scope.device->device && pool == native_scope.handle && allocator == &native_scope.allocator);
    ++native_scope.destroys;
    if (native_scope.reuse_on_destroy) {
        native_scope.reuse_on_destroy = false;
        VkQueryPool replacement = VK_NULL_HANDLE;
        CHECK(CreateQueryPool(device, &native_scope.replacement, allocator, &replacement) == VK_SUCCESS && replacement == pool);
    }
}
static VKAPI_ATTR void VKAPI_CALL scope_begin(VkCommandBuffer command, VkQueryPool pool, uint32_t query,
                                              VkQueryControlFlags flags) {
    CHECK(command == HANDLE(VkCommandBuffer, 0x7002) && pool && query == 0 && !flags);
    ++native_scope.begins;
}
static VKAPI_ATTR void VKAPI_CALL scope_end(VkCommandBuffer command, VkQueryPool pool, uint32_t query) {
    CHECK(command == HANDLE(VkCommandBuffer, 0x7002) && pool && query == 0);
    ++native_scope.ends;
}
static VKAPI_ATTR void VKAPI_CALL scope_begin_indexed(VkCommandBuffer command, VkQueryPool pool, uint32_t query,
                                                      VkQueryControlFlags flags, uint32_t index) {
    CHECK(index == 0);
    scope_begin(command, pool, query, flags);
    ++native_scope.indexed_begins;
}
static VKAPI_ATTR void VKAPI_CALL scope_end_indexed(VkCommandBuffer command, VkQueryPool pool, uint32_t query, uint32_t index) {
    CHECK(index == 0);
    scope_end(command, pool, query);
    ++native_scope.indexed_ends;
}
static void native_query_scopes(void) {
    VkQueueFamilyProperties family = {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT};
    struct vkrt_query_device state = {.families = &family, .family_count = 1};
    vkrt_mutex_init(&state.lock);
    struct vkrt_device_data dev = {.device = HANDLE(VkDevice, 0x7001), .queries = &state, .query_marker_uses_events = true};
    struct vkrt_cmd_buffer_data cb = {.cmd = HANDLE(VkCommandBuffer, 0x7002), .device_data = &dev};
    struct fixture_pool command_pool = {.pool = HANDLE(VkCommandPool, 0x7003), .family = 0};
    cb.pool = command_pool.pool;
    pool_device = &dev;
    recorded_pool = &command_pool;
    dev.vtable.CreateQueryPool = scope_create;
    dev.vtable.DestroyQueryPool = scope_destroy;
    dev.vtable.CmdBeginQuery = scope_begin;
    dev.vtable.CmdEndQuery = scope_end;
    dev.vtable.CmdBeginQueryIndexedEXT = scope_begin_indexed;
    dev.vtable.CmdEndQueryIndexedEXT = scope_end_indexed;
    vkrt_map_object(VKRT_HKEY(dev.device), &dev);
    vkrt_map_object(VKRT_HKEY(cb.cmd), &cb);
    native_scope.device = &dev;
    native_scope.allocator =
        (VkAllocationCallbacks){.pUserData = &native_scope, .pfnAllocation = scope_allocate, .pfnFree = scope_free};
    const VkQueryType types[] = {VK_QUERY_TYPE_OCCLUSION, VK_QUERY_TYPE_PIPELINE_STATISTICS, VK_QUERY_TYPE_PIPELINE_STATISTICS,
                                 VK_QUERY_TYPE_PERFORMANCE_QUERY_KHR, VK_QUERY_TYPE_PIPELINE_STATISTICS};
    const VkQueryPipelineStatisticFlags statistics[] = {0, VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT,
                                                        VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT, 0,
                                                        UINT32_C(0x80000000)};
    VkQueryPool pools[5];
    for (unsigned i = 0; i < 5; ++i) {
        native_scope.handle = HANDLE(VkQueryPool, UINT64_C(0x123400007010) + i);
        VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                      .queryType = types[i],
                                      .queryCount = 1,
                                      .pipelineStatistics = statistics[i]};
        CHECK(CreateQueryPool(dev.device, &info, &native_scope.allocator, &pools[i]) == VK_SUCCESS);
        /* The public info may die immediately; classification owns its fields. */
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.pipelineStatistics = UINT32_MAX;
        CHECK(native_query_compute_sensitive(&dev, pools[i]) == (i >= 2));
    }
    CHECK(native_query_compute_sensitive(&dev, HANDLE(VkQueryPool, UINT64_C(0x987600007010))));
    for (unsigned replay = 0; replay < 2; ++replay) {
        cb.replaying = replay != 0;
        CmdBeginQuery(cb.cmd, pools[0], 0, 0);
        CHECK(cb.native_active_queries == 1 && !cb.native_compute_sensitive_queries && record_allowed(&cb));
        CmdBeginQuery(cb.cmd, pools[1], 0, 0);
        CHECK(cb.native_active_queries == 2 && !cb.native_compute_sensitive_queries && record_allowed(&cb));
        CmdEndQuery(cb.cmd, pools[1], 0);
        CmdBeginQuery(cb.cmd, pools[2], 0, 0);
        CHECK(cb.native_active_queries == 2 && cb.native_compute_sensitive_queries == 1 && !record_allowed(&cb));
        CHECK(cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
        cb.record_result = VK_SUCCESS;
        CmdEndQuery(cb.cmd, pools[0], 0);
        CHECK(cb.native_active_queries == 1 && cb.native_compute_sensitive_queries == 1 && !record_allowed(&cb));
        cb.record_result = VK_SUCCESS;
        CmdEndQuery(cb.cmd, pools[2], 0);
        CHECK(!cb.native_active_queries && !cb.native_compute_sensitive_queries && record_allowed(&cb));
        CmdBeginQueryIndexedEXT(cb.cmd, pools[0], 0, 0, 0);
        CHECK(cb.native_active_queries == 1 && !cb.native_compute_sensitive_queries && record_allowed(&cb));
        CmdEndQueryIndexedEXT(cb.cmd, pools[0], 0, 0);
        CmdBeginQueryIndexedEXT(cb.cmd, pools[3], 0, 0, 0);
        CHECK(cb.native_active_queries == 1 && cb.native_compute_sensitive_queries == 1 && !record_allowed(&cb));
        cb.record_result = VK_SUCCESS;
        CmdEndQueryIndexedEXT(cb.cmd, pools[3], 0, 0);
        CHECK(!cb.native_active_queries && !cb.native_compute_sensitive_queries);
    }
    CHECK(native_scope.begins == 10 && native_scope.ends == 10 && native_scope.indexed_begins == 4 &&
          native_scope.indexed_ends == 4);
    /* Metadata OOM is transactional and never calls native Create; native
     * errors release the preallocated row without publishing a fake scope. */
    native_scope.fail_allocation = true;
    VkQueryPool failed = HANDLE(VkQueryPool, 99);
    unsigned creates = native_scope.creates;
    VkQueryPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 1};
    CHECK(CreateQueryPool(dev.device, &info, &native_scope.allocator, &failed) == VK_ERROR_OUT_OF_HOST_MEMORY && !failed &&
          native_scope.creates == creates);
    native_scope.fail_allocation = false;
    native_scope.result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    CHECK(CreateQueryPool(dev.device, &info, &native_scope.allocator, &failed) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !failed);
    native_scope.result = VK_SUCCESS;
    /* A native destructor may allow another Create to reuse the numeric
     * handle before it returns. The new compute-sensitive generation wins. */
    native_scope.handle = pools[0];
    native_scope.replacement = info;
    native_scope.replacement.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    native_scope.replacement.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
    native_scope.reuse_on_destroy = true;
    DestroyQueryPool(dev.device, pools[0], &native_scope.allocator);
    CHECK(native_query_compute_sensitive(&dev, pools[0]));
    for (unsigned i = 0; i < 5; ++i) {
        native_scope.handle = pools[i];
        DestroyQueryPool(dev.device, pools[i], &native_scope.allocator);
        CHECK(native_query_compute_sensitive(&dev, pools[i]));
    }
    CHECK(!state.native_pools && native_scope.allocations == native_scope.frees && native_scope.destroys == 6);
    vkrt_unmap_object(VKRT_HKEY(cb.cmd));
    vkrt_unmap_object(VKRT_HKEY(dev.device));
#ifndef _WIN32
    pthread_mutex_destroy(&state.lock);
#endif
}
struct query_allocation_context {
    unsigned allocations, frees, native_destroys;
};
static void *VKAPI_CALL query_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    struct query_allocation_context *context = user;
    CHECK(scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT && alignment <= _Alignof(max_align_t));
    void *allocation = malloc(size);
    if (allocation) ++context->allocations;
    return allocation;
}
static void *VKAPI_CALL query_reallocate(void *user, void *allocation, size_t size, size_t alignment,
                                         VkSystemAllocationScope scope) {
    (void)user;
    CHECK(scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT && alignment <= _Alignof(max_align_t));
    return realloc(allocation, size);
}
static void VKAPI_CALL query_free(void *user, void *allocation) {
    struct query_allocation_context *context = user;
    if (allocation) ++context->frees;
    free(allocation);
}
static VKAPI_ATTR VkResult VKAPI_CALL allocation_create_query(VkDevice device, const VkQueryPoolCreateInfo *info,
                                                              const VkAllocationCallbacks *allocator, VkQueryPool *pool) {
    CHECK(device == HANDLE(VkDevice, 0x8123) && info->queryType == VK_QUERY_TYPE_OCCLUSION && allocator);
    *pool = HANDLE(VkQueryPool, 0x8345);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL allocation_destroy_query(VkDevice device, VkQueryPool pool,
                                                           const VkAllocationCallbacks *allocator) {
    CHECK(device == HANDLE(VkDevice, 0x8123) && pool == HANDLE(VkQueryPool, 0x8345) && allocator);
    ++((struct query_allocation_context *)allocator->pUserData)->native_destroys;
}
static void query_destroy_callbacks(bool virtual_pool, bool distinct_destroy) {
    struct query_allocation_context creation = {0}, destruction = {0};
    VkAllocationCallbacks allocator = {&creation, query_allocate, query_reallocate, query_free, NULL, NULL};
    VkAllocationCallbacks current = allocator;
    if (distinct_destroy) current.pUserData = &destruction;
    struct vkrt_query_device state = {.format = HANDLE(VkPipeline, 0x8234)};
    vkrt_mutex_init(&state.lock);
    struct vkrt_device_data dev = {.device = HANDLE(VkDevice, 0x8123), .queries = &state};
    dev.vtable.CreateQueryPool = allocation_create_query;
    dev.vtable.DestroyQueryPool = allocation_destroy_query;
    CHECK(vkrt_map_object(VKRT_HKEY(dev.device), &dev));
    memset(&event_fixture, 0, sizeof(event_fixture));
    event_fixture.dev = &dev;
    marker_storage_active = true;
    VkQueryPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = virtual_pool ? VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR : VK_QUERY_TYPE_OCCLUSION,
        .queryCount = 1};
    VkQueryPool pool = VK_NULL_HANDLE;
    PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)vkrt_query_pool_proc("vkCreateQueryPool");
    PFN_vkDestroyQueryPool destroy = (PFN_vkDestroyQueryPool)vkrt_query_pool_proc("vkDestroyQueryPool");
    VkResult result = create(dev.device, &info, &allocator, &pool);
    CHECK(result == VK_SUCCESS && pool);
    if (result == VK_SUCCESS) destroy(dev.device, pool, &current);
    unsigned frees = virtual_pool ? 2 : 1;
    CHECK(creation.allocations == frees && !destruction.allocations);
    CHECK(creation.frees == (distinct_destroy ? 0 : frees) && destruction.frees == (distinct_destroy ? frees : 0));
    CHECK(creation.native_destroys == (!virtual_pool && !distinct_destroy) &&
          destruction.native_destroys == (!virtual_pool && distinct_destroy));
    CHECK(!state.pools && !state.native_pools);
    if (virtual_pool) CHECK(event_fixture.allocated == 1 && event_fixture.released_count == 1);
    marker_storage_active = false;
    vkrt_unmap_object(VKRT_HKEY(dev.device));
#ifndef _WIN32
    pthread_mutex_destroy(&state.lock);
#endif
}
int main(void) {
    /* Compatible allocators may change pUserData between API calls. Both
     * native metadata and virtual AS slots belong to the current destructor. */
    for (unsigned virtual_pool = 0; virtual_pool < 2; ++virtual_pool)
        for (unsigned distinct_destroy = 0; distinct_destroy < 2; ++distinct_destroy)
            query_destroy_callbacks(virtual_pool != 0, distinct_destroy != 0);
    native_query_scopes();
    event_backend_admission();
    event_occurrences();
    native_wait_to_formatter_scope();
    reset_native_copy_order();
    struct vkrt_query_device state = {0};
    vkrt_mutex_init(&state.lock);
    struct vkrt_device_data dev = {.device = HANDLE(VkDevice, 1), .queries = &state, .query_marker_uses_events = false};
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
    dev.queries = NULL;
    CHECK(create_pool && create_pool(dev.device, &native_info, &native_allocator, &native_pool) == VK_SUCCESS);
    dev.queries = &state;
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
    const VkResult rollback_errors[] = {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(rollback_errors); ++i) {
        transaction = (struct vkrt_query_submission){0};
        CHECK(vkrt_query_submit_prepare(&dev, &transaction, 1, &cb.cmd) == VK_SUCCESS);
        CHECK(write_op.prepared == &transaction && reset_op.prepared == &transaction);
        vkrt_query_submit_result(&dev, &transaction, rollback_errors[i]);
        CHECK(!write_op.submitted && !reset_op.submitted && !write_op.prepared && !reset_op.prepared);
        CHECK(GetQueryPoolResults(dev.device, handle, 0, 1, sizeof(out), out, 32,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS);
        CHECK(out[0] == write_payload.value.value && out[1] == 1);
    }
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
    VkQueue queue = HANDLE(VkQueue, 6), other_queue = HANDLE(VkQueue, 7);
    pipeline_rollback_admission(&dev, queue);
    admission_ordering(&dev, queue, other_queue);
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
    struct fixture_pool recording_pool = {.pool = HANDLE(VkCommandPool, 5), .family = 0};
    state.family_count = 1;
    state.families = &family;
    pool_device = &dev;
    recorded_pool = &recording_pool;
    cb.pool = recording_pool.pool;
    CHECK(!record_allowed(&cb) && cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    cb.record_result = VK_SUCCESS;
    family.queueFlags |= VK_QUEUE_COMPUTE_BIT;
    recording_pool.flags = VK_COMMAND_POOL_CREATE_PROTECTED_BIT;
    CHECK(!record_allowed(&cb) && cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    cb.record_result = VK_SUCCESS;
    recording_pool.flags = 0;
    cb.native_active_queries = 1;
    cb.native_compute_sensitive_queries = 1;
    CHECK(!record_allowed(&cb) && cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    cb.record_result = VK_SUCCESS;
    cb.native_active_queries = 0;
    cb.native_compute_sensitive_queries = 0;
    cb.usage_flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    CHECK(!record_allowed(&cb) && cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    cb.record_result = VK_SUCCESS;
    cb.replay.journal = (struct vkrt_replay_journal *)(uintptr_t)1;
    dev.executions = (struct vkrt_execution_device *)(uintptr_t)1;
    CHECK(record_allowed(&cb));
    cb.has_virtual_queries = true;
    cb.replaying = true;
    CHECK(record_allowed(&cb));
    struct vkrt_cmd_buffer_data rtp_parent = {.device_data = &dev,
                                              .native_active_queries = 1,
                                              .native_compute_sensitive_queries = 1,
                                              .usage_flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    struct vkrt_cmd_buffer_data rtp_child = {.cmd = HANDLE(VkCommandBuffer, 99), .device_data = &dev, .has_rtp_execution = true};
    vkrt_map_object(VKRT_HKEY(rtp_child.cmd), &rtp_child);
    CHECK(vkrt_query_record_children(&rtp_parent, 1, &rtp_child.cmd) && !rtp_parent.has_virtual_queries &&
          rtp_parent.record_result == VK_SUCCESS && !rtp_parent.queries);
    rtp_child.has_virtual_queries = true;
    CHECK(!vkrt_query_record_children(&rtp_parent, 1, &rtp_child.cmd) && rtp_parent.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    rtp_parent.record_result = VK_SUCCESS;
    rtp_parent.native_compute_sensitive_queries = 0; /* A surrounding occlusion query is safe for the child too. */
    rtp_parent.replay.journal = (struct vkrt_replay_journal *)(uintptr_t)1;
    CHECK(vkrt_query_record_children(&rtp_parent, 1, &rtp_child.cmd) && rtp_parent.has_virtual_queries &&
          rtp_parent.record_result == VK_SUCCESS && !rtp_parent.queries);
    vkrt_unmap_object(VKRT_HKEY(rtp_child.cmd));
    debug_query_lifetime();
    vkrt_map_destroy(&vkrt_global.objects);
    if (!failures)
        printf(
            "Query publication epochs: PASS (timestamp-zero events, fresh occurrence/status/OOM/retirement, "
            "timestamp fallback, acquire/cache/reset/rollback, pipeline retry/admission ordering, "
            "native COPY-to-RESET and wait-to-formatter scopes)\n");
    return failures ? 1 : 0;
}
