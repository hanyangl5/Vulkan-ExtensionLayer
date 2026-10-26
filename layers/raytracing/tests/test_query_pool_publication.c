/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_query_pool.c"

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static unsigned failures, acquisitions, resets, native_creates;
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
    vkrt_query_queue_idle(&dev, other_queue);
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, false, 1, &cb.cmd) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkrt_query_queue_idle(&dev, queue);
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, false, 1, &cb.cmd) == VK_SUCCESS);
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, true, 0, NULL) == VK_SUCCESS);
    vkrt_query_submit_result(&dev, &transaction, VK_SUCCESS);
    vkrt_query_queue_idle(&dev, VK_NULL_HANDLE);
    transaction = (struct vkrt_query_submission){0};
    CHECK(vkrt_query_submit_qualify(&dev, &transaction, queue, false, 1, &cb.cmd) == VK_SUCCESS);
    command.scratch = NULL;
    while (state.queues) {
        struct query_queue *next = state.queues->next;
        free(state.queues);
        state.queues = next;
    }
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
    vkrt_map_destroy(&vkrt_global.objects);
    if (!failures)
        printf("Query publication epochs: PASS (native WAIT acquisition, cached reuse, reset ordering, wrap, rollback)\n");
    return failures ? 1 : 0;
}
