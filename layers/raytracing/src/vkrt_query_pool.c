/* SPDX-License-Identifier: MIT
 * Public query publication. Owned recordings defer virtual operations to fresh
 * per-execution CBs; the compatibility host adapter retains a non-simultaneous
 * recorded-operation path. Logical-stage execution remains unfinished work.
 */
#include "vkrt_layer.h"
#include "vkrt_query_pool.h"
#include "vkrt_execution_replay.h"
#include "vkrt_queries.h"
#include "vkrt_as_registry.h"
#include "vkrt_debug_objects.h"
#include "vkrt_as_query.spv.h"
#include "vkrt_query_format.spv.h"
#include "vkrt_query_publish.spv.h"
#include <time.h>

#define QUERY_POOL_MAGIC UINT64_C(0x56514b5254504f4f)
struct query_buffer {
    VkBuffer buffer;
    VkDeviceAddress address;
    void *mapped;
    VkDeviceSize size;
    VkDeviceSize offset;
    struct vkrt_private_slice allocation;
};
struct query_host_slot {
    uint64_t value, serial;
    bool initialized, available;
};
struct query_pool {
    uint64_t magic;
    struct vkrt_device_data *dev;
    VkQueryType type;
    uint32_t count;
    struct query_buffer storage;
    struct query_host_slot *slots;
    struct query_pool *next;
    bool has_allocator;
    VkAllocationCallbacks allocator;
};
struct query_operation {
    struct query_operation *next;
    struct query_pool *pool;
    uint32_t first, count;
    VkQueryPool marker;
    VkEvent event;
    struct query_buffer payload;
    bool submitted, harvested;
    const struct vkrt_query_submission *prepared;
};
struct query_scratch {
    struct query_scratch *next;
    struct query_buffer buffer;
};
struct query_child {
    struct query_child *next;
    VkCommandBuffer command;
};
struct vkrt_query_command {
    struct vkrt_query_command *next;
    struct vkrt_cmd_buffer_data *owner;
    struct query_operation *head, *tail;
    struct query_scratch *scratch;
    struct query_child *children;
};
struct vkrt_query_admission {
    vkrt_mutex_t lock;
};
struct query_queue {
    struct query_queue *next;
    VkQueue queue;
    struct vkrt_query_admission admission;
};
struct vkrt_query_device {
    vkrt_mutex_t lock;
    struct query_pool *pools;
    struct vkrt_query_command *commands;
    struct query_queue *queues;
    VkQueueFamilyProperties *families;
    uint32_t family_count;
    VkPipelineLayout layout;
    VkPipeline property, publish, format;
    VkResult error;
};
struct publish_args {
    uint64_t publication, serials, state, counters;
    uint32_t first, count, reset, reserved;
};
_Static_assert(sizeof(struct publish_args) == 48, "query publication push ABI");

static bool as_type(VkQueryType type) {
    return type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR ||
           type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR ||
           type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SIZE_KHR ||
           type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_BOTTOM_LEVEL_POINTERS_KHR;
}
static struct query_pool *find_pool(VkQueryPool handle) {
    struct query_pool *pool = VKRT_FIND(struct query_pool, handle);
    return pool && pool->magic == QUERY_POOL_MAGIC ? pool : NULL;
}
bool vkrt_query_pool_owns(struct vkrt_device_data *dev, uint64_t handle) {
    if (!dev || !dev->queries || !handle) return false;
    struct vkrt_query_device *state = dev->queries;
    vkrt_mutex_lock(&state->lock);
    bool owned = false;
    for (const struct query_pool *pool = state->pools; pool; pool = pool->next)
        if ((uint64_t)(uintptr_t)pool == handle && pool->dev == dev) {
            owned = true;
            break;
        }
    vkrt_mutex_unlock(&state->lock);
    return owned;
}
static const VkAllocationCallbacks *pool_allocator(struct query_pool *pool) {
    return pool->has_allocator ? &pool->allocator : NULL;
}
static void destroy_buffer(struct vkrt_device_data *dev, struct query_buffer *buffer) {
    vkrt_private_pool_release(dev, &buffer->allocation);
    *buffer = (struct query_buffer){0};
}
static VkResult create_buffer(struct vkrt_device_data *dev, VkDeviceSize size, bool native_copy, struct query_buffer *buffer) {
    if (size > SIZE_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct vkrt_private_slice slice;
    /* BDA state/payload needs physical 8-byte alignment. Native copy scratch
     * needs aligned buffer offsets instead; its formatter stores individual
     * bytes and does not assume an aligned native buffer device address. */
    VkResult result = vkrt_private_pool_allocate(dev, size, native_copy, &slice);
    if (result != VK_SUCCESS) return result;
    *buffer = (struct query_buffer){.buffer = slice.buffer,
                                    .address = slice.address,
                                    .mapped = slice.mapping,
                                    .size = size,
                                    .offset = slice.offset,
                                    .allocation = slice};
    return VK_SUCCESS;
}
static VkResult make_pipeline(struct vkrt_device_data *dev, const uint32_t *words, size_t size, VkPipeline *pipeline) {
    VkShaderModuleCreateInfo shader = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = words};
    VkShaderModule module;
    VkResult result = dev->vtable.CreateShaderModule(dev->device, &shader, NULL, &module);
    if (result != VK_SUCCESS) return result;
    VkComputePipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                  .module = module,
                                                  .pName = "main"},
                                        .layout = dev->queries->layout};
    result = dev->vtable.CreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &info, NULL, pipeline);
    dev->vtable.DestroyShaderModule(dev->device, module, NULL);
    return result;
}
static VkResult initialize_pipelines(struct vkrt_device_data *dev) {
    struct vkrt_query_device *q = dev->queries;
    if (q->format) return VK_SUCCESS;
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct vkrt_query_format_args)};
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    VkResult result = dev->vtable.CreatePipelineLayout(dev->device, &layout, NULL, &q->layout);
    if (result == VK_SUCCESS) result = make_pipeline(dev, vkrt_spv_vkrt_as_query, sizeof(vkrt_spv_vkrt_as_query), &q->property);
    if (result == VK_SUCCESS)
        result = make_pipeline(dev, vkrt_spv_vkrt_query_publish, sizeof(vkrt_spv_vkrt_query_publish), &q->publish);
    if (result == VK_SUCCESS)
        result = make_pipeline(dev, vkrt_spv_vkrt_query_format, sizeof(vkrt_spv_vkrt_query_format), &q->format);
    if (result != VK_SUCCESS) {
        /* Queue admission belongs to the device, not these pipelines.
         * An initialization retry must not invalidate an active admission lock. */
        if (q->property) dev->vtable.DestroyPipeline(dev->device, q->property, NULL);
        if (q->publish) dev->vtable.DestroyPipeline(dev->device, q->publish, NULL);
        if (q->layout) dev->vtable.DestroyPipelineLayout(dev->device, q->layout, NULL);
        q->property = q->publish = VK_NULL_HANDLE;
        q->layout = VK_NULL_HANDLE;
    }
    return result;
}
VkResult vkrt_query_device_init(struct vkrt_device_data *dev) {
    if (!dev->query_backend_enabled) return dev->virtual_rt ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS;
    if (!dev->vtable.CmdCopyBuffer) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (dev->query_marker_uses_events) {
        if (!dev->vtable.CreateEvent || !dev->vtable.DestroyEvent || !dev->vtable.GetEventStatus || !dev->vtable.CmdSetEvent)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    } else if (!dev->vtable.CreateQueryPool || !dev->vtable.DestroyQueryPool || !dev->vtable.ResetQueryPool ||
               !dev->vtable.GetQueryPoolResults || !dev->vtable.CmdWriteTimestamp)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct vkrt_query_device *q = calloc(1, sizeof(*q));
    if (!q) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vkrt_mutex_init(&q->lock);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get = NULL;
    if (dev->instance_data && dev->instance_data->vtable.GetInstanceProcAddr)
        get = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)dev->instance_data->vtable.GetInstanceProcAddr(
            dev->instance_data->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!get) {
#ifndef _WIN32
        pthread_mutex_destroy(&q->lock);
#endif
        free(q);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    get(dev->physical_device, &q->family_count, NULL);
    if (!q->family_count) {
#ifndef _WIN32
        pthread_mutex_destroy(&q->lock);
#endif
        free(q);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    q->families = calloc(q->family_count, sizeof(*q->families));
    if (!q->families) {
#ifndef _WIN32
        pthread_mutex_destroy(&q->lock);
#endif
        free(q);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    get(dev->physical_device, &q->family_count, q->families);
    dev->queries = q;
    return VK_SUCCESS;
}
static bool later(uint64_t a, uint64_t b) { return a != b && a - b < (UINT64_C(1) << 63); }
static VkResult harvest(struct vkrt_device_data *dev, struct query_operation *op, bool completed_cb) {
    if (!op->submitted || op->harvested || !op->pool) return VK_SUCCESS;
    if (op->event) {
        VkResult status = dev->vtable.GetEventStatus(dev->device, op->event);
        /* Polling RESET merely delays acquisition. A submitted recording
         * known complete must have executed this fresh event's sole setter;
         * RESET then cannot silently discard an unpublished result at reset. */
        if (status == VK_EVENT_RESET) return completed_cb ? VK_ERROR_UNKNOWN : VK_SUCCESS;
        if (status != VK_EVENT_SET) return status < 0 ? status : VK_ERROR_UNKNOWN;
        /* This fresh event has one setter, after the CS sentinel. SET only
         * observes completion; the publication's CS-to-HOST memory barrier
         * below is still required for the coherent payload reads. Neither
         * SET nor harvesting retires the event or its execution-owned slice. */
    } else {
        uint64_t marker[2] = {0};
        VkResult result = dev->vtable.GetQueryPoolResults(dev->device, op->marker, 0, 1, sizeof(marker), marker, 16,
                                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS && result != VK_NOT_READY) return result;
        if (!marker[1] && !completed_cb) return VK_SUCCESS;
        result = dev->vtable.GetQueryPoolResults(
            dev->device, op->marker, 0, 1, sizeof(marker), marker, 16,
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (result != VK_SUCCESS) return result;
    }
    for (uint32_t i = 0; i < op->count; ++i) {
        struct vkrt_as_query_value value;
        uint64_t serial;
        memcpy(&value, (const uint8_t *)op->payload.mapped + (size_t)i * 16, sizeof(value));
        memcpy(&serial, (const uint8_t *)op->payload.mapped + (size_t)op->count * 16 + (size_t)i * 8, sizeof(serial));
        struct query_host_slot *slot = &op->pool->slots[op->first + i];
        if (!slot->initialized || later(serial, slot->serial))
            *slot = (struct query_host_slot){value.value, serial, true, value.valid != 0};
    }
    op->harvested = true;
    return VK_SUCCESS;
}
static VkResult poll_device(struct vkrt_device_data *dev) {
    if (dev->queries->error != VK_SUCCESS) return dev->queries->error;
    for (struct vkrt_query_command *cb = dev->queries->commands; cb; cb = cb->next)
        for (struct query_operation *op = cb->head; op; op = op->next) {
            VkResult result = harvest(dev, op, false);
            if (result != VK_SUCCESS) return result;
        }
    return VK_SUCCESS;
}
static struct vkrt_query_command *command_state(struct vkrt_cmd_buffer_data *cb) {
    if (cb->queries) return cb->queries;
    struct vkrt_query_command *q = calloc(1, sizeof(*q));
    if (!q) {
        cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }
    q->owner = cb;
    vkrt_mutex_lock(&cb->device_data->queries->lock);
    q->next = cb->device_data->queries->commands;
    cb->device_data->queries->commands = q;
    vkrt_mutex_unlock(&cb->device_data->queries->lock);
    cb->queries = q;
    return q;
}
static bool record_allowed(struct vkrt_cmd_buffer_data *cb) {
    if (!cb || cb->record_result != VK_SUCCESS) return false;
    const bool execution_owned = cb->replaying || (cb->replay.journal && cb->device_data->executions);
    if (!cb->device_data->queries || (!execution_owned && (cb->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)) ||
        cb->native_active_queries) {
        VKRT_LOG_ERR("AS queries require isolated execution storage and no active native query");
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return false;
    }
    uint32_t family = UINT32_MAX;
    VkCommandPoolCreateFlags flags = 0;
    vkrt_execution_command_pool_info(cb->device_data, cb->pool, &family, &flags);
    struct vkrt_query_device *q = cb->device_data->queries;
    if ((flags & VK_COMMAND_POOL_CREATE_PROTECTED_BIT) || family >= q->family_count ||
        (!cb->device_data->query_marker_uses_events && !q->families[family].timestampValidBits) ||
        !(q->families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        VKRT_LOG_ERR("AS query backend requires an unprotected compute pool and the selected marker support");
        return false;
    }
    return true;
}
static bool capture_only(struct vkrt_cmd_buffer_data *cb) {
    cb->has_virtual_queries = true;
    return !cb->replaying && cb->replay.journal && cb->device_data->executions;
}
enum query_dependency { QUERY_DEPENDENCY_PROPERTY, QUERY_DEPENDENCY_RESET, QUERY_DEPENDENCY_COPY };
static void dependency(struct vkrt_device_data *dev, VkCommandBuffer command, enum query_dependency kind) {
    VkPipelineStageFlags source = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    if (kind == QUERY_DEPENDENCY_RESET) {
        /* Reset has an implicit dependency on earlier commands using the same
         * queries, including COPY's final native transfer in another CB. */
        source |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        access |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (kind == QUERY_DEPENDENCY_COPY) {
        /* The native semaphore wait and this ALL-to-CS memory dependency
         * form a scope chain even without an intervening transfer action.
         * Acquire the query publication before its physical CS formatter. */
        source = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, access,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    dev->vtable.CmdPipelineBarrier(command, source, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
}
static struct query_operation *new_operation(struct vkrt_cmd_buffer_data *cb, struct query_pool *pool, uint32_t first,
                                             uint32_t count) {
    if (!record_allowed(cb) || !count || first >= pool->count || count > pool->count - first) return NULL;
    struct vkrt_query_command *q = command_state(cb);
    if (!q) return NULL;
    struct query_operation *op = calloc(1, sizeof(*op));
    if (!op) {
        cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }
    op->pool = pool;
    op->first = first;
    op->count = count;
    VkResult result = create_buffer(cb->device_data, (VkDeviceSize)count * 24, false, &op->payload);
    if (result == VK_SUCCESS && cb->device_data->query_marker_uses_events) {
        /* No DEVICE_ONLY flag: publication is observed with GetEventStatus.
         * A replay occurrence owns this fresh, initially unsignaled event. */
        const VkEventCreateInfo info = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        result = cb->device_data->vtable.CreateEvent(cb->device_data->device, &info, NULL, &op->event);
    } else if (result == VK_SUCCESS) {
        const VkQueryPoolCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 1};
        result = cb->device_data->vtable.CreateQueryPool(cb->device_data->device, &info, NULL, &op->marker);
        if (result == VK_SUCCESS) cb->device_data->vtable.ResetQueryPool(cb->device_data->device, op->marker, 0, 1);
    }
    if (result != VK_SUCCESS) {
        destroy_buffer(cb->device_data, &op->payload);
        free(op);
        cb->record_result = result;
        return NULL;
    }
    vkrt_mutex_lock(&cb->device_data->queries->lock);
    if (q->tail)
        q->tail->next = op;
    else
        q->head = op;
    q->tail = op;
    vkrt_mutex_unlock(&cb->device_data->queries->lock);
    return op;
}
static void publish_operation(struct vkrt_cmd_buffer_data *cb, struct query_operation *op, bool reset) {
    struct vkrt_device_data *dev = cb->device_data;
    struct publish_args args = {op->payload.address,
                                op->payload.address + (uint64_t)op->count * 16,
                                op->pool->storage.address,
                                op->pool->storage.address + (uint64_t)op->pool->count * 16,
                                op->first,
                                op->count,
                                reset,
                                0};
    dependency(dev, cb->cmd, reset ? QUERY_DEPENDENCY_RESET : QUERY_DEPENDENCY_PROPERTY);
    dev->vtable.CmdBindPipeline(cb->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->queries->publish);
    dev->vtable.CmdPushConstants(cb->cmd, dev->queries->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
    uint64_t limit = (uint64_t)dev->max_build_dispatch_x * 64;
    for (uint64_t start = 0; start < op->count;) {
        struct publish_args part = args;
        part.count = (uint32_t)((uint64_t)op->count - start < limit ? (uint64_t)op->count - start : limit);
        part.first += (uint32_t)start;
        part.publication += start * 16;
        part.serials += start * 8;
        dev->vtable.CmdPushConstants(cb->cmd, dev->queries->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(part), &part);
        dev->vtable.CmdDispatch(cb->cmd, (part.count - 1) / 64 + 1, 1, 1);
        start += part.count;
    }
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
                               VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT};
    dev->vtable.CmdPipelineBarrier(cb->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0,
                                   NULL);
    /* A real CS action belongs both to the barrier's destination scope and
     * the following marker's source scope. Its invocations return without
     * touching memory; no clock value or host/device atomic trick is used. */
    struct publish_args sentinel = {0};
    dev->vtable.CmdPushConstants(cb->cmd, dev->queries->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sentinel), &sentinel);
    dev->vtable.CmdDispatch(cb->cmd, 1, 1, 1);
    if (op->event)
        dev->vtable.CmdSetEvent(cb->cmd, op->event, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    else
        dev->vtable.CmdWriteTimestamp(cb->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, op->marker, 0);
    vkrt_replay_compute_binding(cb);
    vkrt_replay_push_constants(cb);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo *info,
                                                      const VkAllocationCallbacks *allocator, VkQueryPool *handle) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (!as_type(info->queryType)) return dev->vtable.CreateQueryPool(device, info, allocator, handle);
    *handle = VK_NULL_HANDLE;
    if (!dev->queries || info->pNext || info->flags) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct query_pool *pool = allocator ? allocator->pfnAllocation(allocator->pUserData, sizeof(*pool), _Alignof(struct query_pool),
                                                                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT)
                                        : malloc(sizeof(*pool));
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(pool, 0, sizeof(*pool));
    pool->magic = QUERY_POOL_MAGIC;
    pool->dev = dev;
    pool->type = info->queryType;
    pool->count = info->queryCount;
    if (allocator) {
        pool->has_allocator = true;
        pool->allocator = *allocator;
    }
    size_t slot_bytes = (size_t)pool->count * sizeof(*pool->slots);
    pool->slots = allocator ? allocator->pfnAllocation(allocator->pUserData, slot_bytes, _Alignof(struct query_host_slot),
                                                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT)
                            : malloc(slot_bytes);
    if (pool->slots) memset(pool->slots, 0, slot_bytes);
    VkResult result =
        pool->slots ? create_buffer(dev, (VkDeviceSize)pool->count * 24, false, &pool->storage) : VK_ERROR_OUT_OF_HOST_MEMORY;
    vkrt_mutex_lock(&dev->queries->lock);
    if (result == VK_SUCCESS) result = initialize_pipelines(dev);
    if (result == VK_SUCCESS) {
        vkrt_global_ensure();
        vkrt_mutex_lock(&vkrt_global.objects_lock);
        vkrt_map_insert(&vkrt_global.objects, (uint64_t)(uintptr_t)pool, pool);
        if (vkrt_map_lookup(&vkrt_global.objects, (uint64_t)(uintptr_t)pool) != pool) result = VK_ERROR_OUT_OF_HOST_MEMORY;
        vkrt_mutex_unlock(&vkrt_global.objects_lock);
    }
    if (result == VK_SUCCESS) {
        pool->next = dev->queries->pools;
        dev->queries->pools = pool;
    }
    vkrt_mutex_unlock(&dev->queries->lock);
    if (result != VK_SUCCESS) {
        destroy_buffer(dev, &pool->storage);
        if (pool->slots) {
            if (allocator)
                allocator->pfnFree(allocator->pUserData, pool->slots);
            else
                free(pool->slots);
        }
        if (allocator)
            allocator->pfnFree(allocator->pUserData, pool);
        else
            free(pool);
        return result;
    }
    *handle = (VkQueryPool)(uintptr_t)pool;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL DestroyQueryPool(VkDevice device, VkQueryPool handle, const VkAllocationCallbacks *allocator) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    struct query_pool *pool = find_pool(handle);
    if (!pool) {
        if (dev) dev->vtable.DestroyQueryPool(device, handle, allocator);
        return;
    }
    vkrt_mutex_lock(&dev->queries->lock);
    for (struct vkrt_query_command *cb = dev->queries->commands; cb; cb = cb->next)
        for (struct query_operation *op = cb->head; op; op = op->next)
            if (op->pool == pool) op->pool = NULL;
    struct query_pool **link = &dev->queries->pools;
    while (*link && *link != pool) link = &(*link)->next;
    if (*link) *link = pool->next;
    vkrt_unmap_object(VKRT_HKEY(handle));
    vkrt_mutex_unlock(&dev->queries->lock);
    vkrt_debug_object_forget(dev, VK_OBJECT_TYPE_QUERY_POOL, (uint64_t)(uintptr_t)pool);
    destroy_buffer(dev, &pool->storage);
    if (pool->has_allocator)
        pool->allocator.pfnFree(pool->allocator.pUserData, pool->slots);
    else
        free(pool->slots);
    VkAllocationCallbacks callbacks = pool->allocator;
    if (pool->has_allocator)
        callbacks.pfnFree(callbacks.pUserData, pool);
    else
        free(pool);
}
static VKAPI_ATTR VkResult VKAPI_CALL GetQueryPoolResults(VkDevice device, VkQueryPool handle, uint32_t first, uint32_t count,
                                                          size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags flags) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    struct query_pool *pool = find_pool(handle);
    if (!pool) return dev->vtable.GetQueryPoolResults(device, handle, first, count, size, data, stride, flags);
    VkResult completion = vkrt_execution_collect(dev, NULL);
    if (completion != VK_SUCCESS) return completion;
    size_t width = flags & VK_QUERY_RESULT_64_BIT ? 8 : 4;
    size_t bytes = width * (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT ? 2 : 1);
    if (first >= pool->count || count > pool->count - first ||
        (count && (stride > SIZE_MAX || (count > 1 && stride > (SIZE_MAX - bytes) / (count - 1)) ||
                   (size_t)(count - 1) * (size_t)stride + bytes > size)))
        return VK_ERROR_UNKNOWN;
    for (;;) {
        vkrt_mutex_lock(&dev->queries->lock);
        VkResult result = poll_device(dev);
        bool ready = true;
        for (uint32_t i = 0; i < count; ++i) ready &= pool->slots[first + i].available;
        if (result == VK_SUCCESS && (ready || !(flags & VK_QUERY_RESULT_WAIT_BIT))) {
            for (uint32_t i = 0; i < count; ++i) {
                const struct query_host_slot *slot = &pool->slots[first + i];
                uint8_t *destination = (uint8_t *)data + (size_t)i * (size_t)stride;
                uint64_t value = slot->available ? slot->value : 0, availability = slot->available;
                if (slot->available || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) memcpy(destination, &value, width);
                if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) memcpy(destination + width, &availability, width);
            }
            result = ready ? VK_SUCCESS : VK_NOT_READY;
        }
        vkrt_mutex_unlock(&dev->queries->lock);
        if (result != VK_SUCCESS || ready || !(flags & VK_QUERY_RESULT_WAIT_BIT)) return result;
#ifdef _WIN32
        Sleep(1);
#else
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
#endif
    }
}
static VKAPI_ATTR void VKAPI_CALL ResetQueryPool(VkDevice device, VkQueryPool handle, uint32_t first, uint32_t count) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    struct query_pool *pool = find_pool(handle);
    if (!pool) {
        dev->vtable.ResetQueryPool(device, handle, first, count);
        return;
    }
    VkResult completion = vkrt_execution_collect(dev, NULL);
    if (completion != VK_SUCCESS) {
        vkrt_mutex_lock(&dev->queries->lock);
        dev->queries->error = completion;
        vkrt_mutex_unlock(&dev->queries->lock);
        return;
    }
    vkrt_mutex_lock(&dev->queries->lock);
    VkResult result = poll_device(dev);
    if (result == VK_SUCCESS) {
        for (uint32_t i = first; i < first + count; ++i) {
            struct query_host_slot *slot = &pool->slots[i];
            slot->serial = slot->initialized ? slot->serial + 1 : 1;
            memcpy((uint8_t *)pool->storage.mapped + (size_t)pool->count * 16 + (size_t)i * 8, &slot->serial, sizeof(slot->serial));
            slot->initialized = true;
            slot->available = false;
            slot->value = 0;
            memset((uint8_t *)pool->storage.mapped + (size_t)i * 16, 0, sizeof(struct vkrt_as_query_value));
        }
    } else
        dev->queries->error = result;
    vkrt_mutex_unlock(&dev->queries->lock);
}
static VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer command, VkQueryPool handle, uint32_t first, uint32_t count) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    struct query_pool *pool = find_pool(handle);
    if (!pool) {
        cb->device_data->vtable.CmdResetQueryPool(command, handle, first, count);
        return;
    }
    if (!record_allowed(cb)) return;
    if (capture_only(cb)) return;
    struct query_operation *op = new_operation(cb, pool, first, count);
    if (op) publish_operation(cb, op, true);
}
static VKAPI_ATTR void VKAPI_CALL CmdWriteAccelerationStructuresPropertiesKHR(VkCommandBuffer command, uint32_t count,
                                                                              const VkAccelerationStructureKHR *structures,
                                                                              VkQueryType type, VkQueryPool handle,
                                                                              uint32_t first) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    struct query_pool *pool = find_pool(handle);
    if (!cb || !pool || pool->type != type) {
        if (cb) cb->record_result = VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    if (!record_allowed(cb)) return;
    if (capture_only(cb)) return;
    struct query_operation *op = new_operation(cb, pool, first, count);
    if (!op) return;
    struct vkrt_device_data *dev = cb->device_data;
    dependency(dev, command, QUERY_DEPENDENCY_PROPERTY);
    dev->vtable.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, dev->queries->property);
    for (uint32_t i = 0; i < count; ++i) {
        struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, structures[i]);
        VkDeviceAddress address = as && as->owner == dev->device ? vkrt_as_backing_address(dev, structures[i]) : 0;
        if (!address) {
            cb->record_result = VK_ERROR_INITIALIZATION_FAILED;
            return;
        }
        struct vkrt_as_query_args args = {address, as->size, op->payload.address + (uint64_t)i * 16, type, 0};
        dev->vtable.CmdPushConstants(command, dev->queries->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
        dev->vtable.CmdDispatch(command, 1, 1, 1);
    }
    publish_operation(cb, op, false);
}
static VKAPI_ATTR void VKAPI_CALL CmdCopyQueryPoolResults(VkCommandBuffer command, VkQueryPool handle, uint32_t first,
                                                          uint32_t count, VkBuffer destination, VkDeviceSize offset,
                                                          VkDeviceSize stride, VkQueryResultFlags flags) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    struct query_pool *pool = find_pool(handle);
    if (!pool) {
        cb->device_data->vtable.CmdCopyQueryPoolResults(command, handle, first, count, destination, offset, stride, flags);
        return;
    }
    if (!record_allowed(cb) || !count) return;
    if (capture_only(cb)) return;
    struct vkrt_query_command *q = command_state(cb);
    if (!q) return;
    VkDeviceSize width = flags & VK_QUERY_RESULT_64_BIT ? 8 : 4;
    VkDeviceSize bytes = width * (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT ? 2 : 1);
    if ((count > 1 && stride > (UINT64_MAX - bytes) / (count - 1)) || first >= pool->count || count > pool->count - first) {
        cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        return;
    }
    VkDeviceSize required = (count - 1) * stride + bytes;
    struct query_scratch *scratch = calloc(1, sizeof(*scratch));
    VkBufferCopy *regions = calloc(count, sizeof(*regions));
    if (!scratch || !regions) {
        free(scratch);
        free(regions);
        cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        return;
    }
    VkResult result = create_buffer(cb->device_data, required, true, &scratch->buffer);
    if (result != VK_SUCCESS) {
        free(scratch);
        free(regions);
        cb->record_result = result;
        return;
    }
    scratch->next = q->scratch;
    q->scratch = scratch;
    struct vkrt_device_data *dev = cb->device_data;
    struct vkrt_query_format_args args = {
        pool->storage.address, (uint64_t)pool->count * 16, scratch->buffer.address, required, stride, first, count, flags, 0};
    dependency(dev, command, QUERY_DEPENDENCY_COPY);
    dev->vtable.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, dev->queries->format);
    dev->vtable.CmdPushConstants(command, dev->queries->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
    uint64_t limit = (uint64_t)dev->max_build_dispatch_x * 64;
    for (uint64_t start = 0; start < count;) {
        struct vkrt_query_format_args part = args;
        part.count = (uint32_t)((uint64_t)count - start < limit ? (uint64_t)count - start : limit);
        part.first_query += (uint32_t)start;
        part.output += start * stride;
        part.output_capacity -= start * stride;
        dev->vtable.CmdPushConstants(command, dev->queries->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(part), &part);
        dev->vtable.CmdDispatch(command, (part.count - 1) / 64 + 1, 1, 1);
        start += part.count;
    }
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    dev->vtable.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                                   NULL, 0, NULL);
    for (uint32_t i = 0; i < count; ++i)
        regions[i] = (VkBufferCopy){scratch->buffer.offset + (VkDeviceSize)i * stride, offset + (VkDeviceSize)i * stride, bytes};
    dev->vtable.CmdCopyBuffer(command, scratch->buffer.buffer, destination, count, regions);
    free(regions);
    vkrt_replay_compute_binding(cb);
    vkrt_replay_push_constants(cb);
}

VkResult vkrt_query_command_reset(struct vkrt_cmd_buffer_data *cb) {
    struct vkrt_query_command *q = cb->queries;
    if (!q) return VK_SUCCESS;
    struct vkrt_device_data *dev = cb->device_data;
    vkrt_mutex_lock(&dev->queries->lock);
    VkResult result = VK_SUCCESS;
    for (struct query_operation *op = q->head; op; op = op->next) {
        VkResult acquired = harvest(dev, op, true);
        if (result == VK_SUCCESS) result = acquired;
    }
    /* FreeCommandBuffers is void, including batch-fence retirement. Preserve
     * any native observation error for the next public query/submit call. */
    if (result != VK_SUCCESS && dev->queries->error == VK_SUCCESS) dev->queries->error = result;
    struct vkrt_query_command **link = &dev->queries->commands;
    while (*link && *link != q) link = &(*link)->next;
    if (*link) *link = q->next;
    vkrt_mutex_unlock(&dev->queries->lock);
    while (q->head) {
        struct query_operation *next = q->head->next;
        if (q->head->event)
            dev->vtable.DestroyEvent(dev->device, q->head->event, NULL);
        else
            dev->vtable.DestroyQueryPool(dev->device, q->head->marker, NULL);
        destroy_buffer(dev, &q->head->payload);
        free(q->head);
        q->head = next;
    }
    while (q->scratch) {
        struct query_scratch *next = q->scratch->next;
        destroy_buffer(dev, &q->scratch->buffer);
        free(q->scratch);
        q->scratch = next;
    }
    while (q->children) {
        struct query_child *next = q->children->next;
        free(q->children);
        q->children = next;
    }
    free(q);
    cb->queries = NULL;
    return result;
}
/* Caller holds queries->lock. Queue nodes live until device finish, so a
 * transaction can release that list lock before waiting on its admission. */
static struct query_queue *query_queue_get(struct vkrt_query_device *q, VkQueue queue) {
    struct query_queue *state = q->queues;
    while (state && state->queue != queue) state = state->next;
    if (state) return state;
    state = calloc(1, sizeof(*state));
    if (!state) return NULL;
#ifdef _WIN32
    vkrt_mutex_init(&state->admission.lock);
#else
    if (pthread_mutex_init(&state->admission.lock, NULL)) {
        free(state);
        return NULL;
    }
#endif
    state->queue = queue;
    state->next = q->queues;
    q->queues = state;
    return state;
}
static void query_queue_destroy(struct query_queue *state) {
#ifndef _WIN32
    pthread_mutex_destroy(&state->admission.lock);
#endif
    free(state);
}
VkResult vkrt_query_submit_begin(struct vkrt_device_data *dev, struct vkrt_query_submission *transaction, VkQueue queue) {
    if (!dev || !transaction || transaction->admission || !queue) return VK_ERROR_INITIALIZATION_FAILED;
    if (!dev->queries) return VK_SUCCESS;
    vkrt_mutex_lock(&dev->queries->lock);
    struct query_queue *state = query_queue_get(dev->queries, queue);
    VkResult result = state ? dev->queries->error : VK_ERROR_OUT_OF_HOST_MEMORY;
    vkrt_mutex_unlock(&dev->queries->lock);
    if (result != VK_SUCCESS) return result;
    vkrt_mutex_lock(&state->admission.lock);
    vkrt_mutex_lock(&dev->queries->lock);
    result = dev->queries->error;
    vkrt_mutex_unlock(&dev->queries->lock);
    if (result != VK_SUCCESS) {
        vkrt_mutex_unlock(&state->admission.lock);
        return result;
    }
    transaction->admission = &state->admission;
    transaction->queue = queue;
    return VK_SUCCESS;
}
void vkrt_query_submit_end(struct vkrt_device_data *dev, struct vkrt_query_submission *transaction) {
    (void)dev;
    if (!transaction || !transaction->admission) return;
    struct vkrt_query_admission *admission = transaction->admission;
    transaction->admission = NULL;
    vkrt_mutex_unlock(&admission->lock);
}
static VkResult prepare_command(struct vkrt_device_data *dev, VkCommandBuffer command,
                                const struct vkrt_query_submission *transaction) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (!cb || !cb->queries) return VK_SUCCESS;
    for (struct query_child *child = cb->queries->children; child; child = child->next) {
        VkResult result = prepare_command(dev, child->command, transaction);
        if (result != VK_SUCCESS) return result;
    }
    for (struct query_operation *op = cb->queries->head; op; op = op->next) {
        if (op->prepared == transaction) return VK_ERROR_UNKNOWN;
        /* Production replay creates a new operation/event per occurrence.
         * Direct host adapters may still resubmit the timestamp fallback, but
         * reusing an event would expose its previous SET before the new work. */
        if (op->event && op->submitted) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkResult result = harvest(dev, op, true);
        if (result != VK_SUCCESS) return result;
        if (!op->event) dev->vtable.ResetQueryPool(dev->device, op->marker, 0, 1);
        op->submitted = true;
        op->harvested = false;
        op->prepared = transaction;
    }
    return VK_SUCCESS;
}
VkResult vkrt_query_submit_prepare(struct vkrt_device_data *dev, const struct vkrt_query_submission *transaction, uint32_t count,
                                   const VkCommandBuffer *commands) {
    if (!dev->queries) return VK_SUCCESS;
    vkrt_mutex_lock(&dev->queries->lock);
    VkResult result = poll_device(dev);
    for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i) result = prepare_command(dev, commands[i], transaction);
    vkrt_mutex_unlock(&dev->queries->lock);
    return result;
}
void vkrt_query_submit_result(struct vkrt_device_data *dev, const struct vkrt_query_submission *transaction, VkResult result) {
    if (!dev->queries || !transaction) return;
    vkrt_mutex_lock(&dev->queries->lock);
    if (result == VK_ERROR_DEVICE_LOST) dev->queries->error = result;
    for (struct vkrt_query_command *cb = dev->queries->commands; cb; cb = cb->next)
        for (struct query_operation *op = cb->head; op; op = op->next)
            if (op->prepared == transaction) {
                if (result != VK_SUCCESS) op->submitted = false;
                op->prepared = 0;
            }
    vkrt_mutex_unlock(&dev->queries->lock);
}
void vkrt_query_device_finish(struct vkrt_device_data *dev) {
    struct vkrt_query_device *q = dev->queries;
    if (!q) return;
    /* DestroyQueryPool releases q->lock before forgetting debug annotations;
     * an annotation lookup takes debug-lock then q->lock. Never nest them in
     * the opposite order while draining the device's externally synced list. */
    while (q->pools) DestroyQueryPool(dev->device, (VkQueryPool)(uintptr_t)q->pools, pool_allocator(q->pools));
    while (q->queues) {
        struct query_queue *next = q->queues->next;
        query_queue_destroy(q->queues);
        q->queues = next;
    }
    if (q->property) dev->vtable.DestroyPipeline(dev->device, q->property, NULL);
    if (q->publish) dev->vtable.DestroyPipeline(dev->device, q->publish, NULL);
    if (q->format) dev->vtable.DestroyPipeline(dev->device, q->format, NULL);
    if (q->layout) dev->vtable.DestroyPipelineLayout(dev->device, q->layout, NULL);
    free(q->families);
#ifndef _WIN32
    pthread_mutex_destroy(&q->lock);
#endif
    free(q);
    dev->queries = NULL;
}
bool vkrt_query_record_children(struct vkrt_cmd_buffer_data *cb, uint32_t count, const VkCommandBuffer *children) {
    if (cb->device_data->queries) {
        for (uint32_t i = 0; i < count; ++i) {
            struct vkrt_cmd_buffer_data *child = VKRT_FIND(struct vkrt_cmd_buffer_data, children[i]);
            if (!child || (!child->queries && !child->has_virtual_queries)) continue;
            const bool execution_owned = cb->replaying || (cb->replay.journal && cb->device_data->executions);
            if ((!execution_owned && (cb->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)) ||
                cb->native_active_queries) {
                cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
                return false;
            }
            cb->has_virtual_queries = true;
            if (!cb->replaying && cb->replay.journal && cb->device_data->executions) continue;
            struct vkrt_query_command *state = command_state(cb);
            if (!state) return false;
            struct query_child *record = malloc(sizeof(*record));
            if (!record) {
                cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
                return false;
            }
            *record = (struct query_child){state->children, children[i]};
            state->children = record;
        }
    }
    return true;
}
static VKAPI_ATTR void VKAPI_CALL CmdBeginQuery(VkCommandBuffer command, VkQueryPool pool, uint32_t query,
                                                VkQueryControlFlags flags) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    ++cb->native_active_queries;
    cb->device_data->vtable.CmdBeginQuery(command, pool, query, flags);
}
static VKAPI_ATTR void VKAPI_CALL CmdEndQuery(VkCommandBuffer command, VkQueryPool pool, uint32_t query) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (cb->native_active_queries) --cb->native_active_queries;
    cb->device_data->vtable.CmdEndQuery(command, pool, query);
}
static VKAPI_ATTR void VKAPI_CALL CmdBeginQueryIndexedEXT(VkCommandBuffer command, VkQueryPool pool, uint32_t query,
                                                          VkQueryControlFlags flags, uint32_t index) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    ++cb->native_active_queries;
    cb->device_data->vtable.CmdBeginQueryIndexedEXT(command, pool, query, flags, index);
}
static VKAPI_ATTR void VKAPI_CALL CmdEndQueryIndexedEXT(VkCommandBuffer command, VkQueryPool pool, uint32_t query, uint32_t index) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (cb->native_active_queries) --cb->native_active_queries;
    cb->device_data->vtable.CmdEndQueryIndexedEXT(command, pool, query, index);
}
PFN_vkVoidFunction vkrt_query_pool_proc(const char *name) {
#define ENTRY(api) \
    if (!strcmp(name, "vk" #api)) return (PFN_vkVoidFunction)api
    ENTRY(CreateQueryPool);
    ENTRY(DestroyQueryPool);
    ENTRY(GetQueryPoolResults);
    ENTRY(ResetQueryPool);
    ENTRY(CmdResetQueryPool);
    ENTRY(CmdWriteAccelerationStructuresPropertiesKHR);
    ENTRY(CmdCopyQueryPoolResults);
    ENTRY(CmdBeginQuery);
    ENTRY(CmdEndQuery);
    ENTRY(CmdBeginQueryIndexedEXT);
    ENTRY(CmdEndQueryIndexedEXT);
#undef ENTRY
    if (!strcmp(name, "vkResetQueryPoolEXT")) return (PFN_vkVoidFunction)ResetQueryPool;
    return NULL;
}
