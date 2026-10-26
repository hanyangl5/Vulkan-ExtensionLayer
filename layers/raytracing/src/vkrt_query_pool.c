/* SPDX-License-Identifier: MIT
 * Initial public query backend. Per-recorded-operation storage is reused only
 * after a non-simultaneous CB's old execution has been acquired and harvested.
 * The mandatory replay/stage-scheduling backend remains separate unfinished work.
 */
#include "vkrt_layer.h"
#include "vkrt_query_pool.h"
#include "vkrt_queries.h"
#include "vkrt_as_registry.h"
#include "vkrt_as_query.spv.h"
#include "vkrt_query_format.spv.h"
#include "vkrt_query_publish.spv.h"
#include <time.h>

#define QUERY_POOL_MAGIC UINT64_C(0x56514b5254504f4f)
struct query_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress address;
    void *mapped;
    VkDeviceSize size;
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
struct query_command_pool {
    struct query_command_pool *next;
    VkCommandPool pool;
    uint32_t family;
    VkCommandPoolCreateFlags flags;
};
struct vkrt_query_command {
    struct vkrt_query_command *next;
    struct vkrt_cmd_buffer_data *owner;
    struct query_operation *head, *tail;
    struct query_scratch *scratch;
    struct query_child *children;
};
struct query_queue {
    struct query_queue *next;
    VkQueue queue;
    bool unsafe_waits;
};
struct vkrt_query_device {
    vkrt_mutex_t lock;
    struct query_pool *pools;
    struct vkrt_query_command *commands;
    struct query_command_pool *command_pools;
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
static const VkAllocationCallbacks *pool_allocator(struct query_pool *pool) {
    return pool->has_allocator ? &pool->allocator : NULL;
}
static void destroy_buffer(struct vkrt_device_data *dev, struct query_buffer *buffer) {
    if (buffer->mapped) dev->vtable.UnmapMemory(dev->device, buffer->memory);
    if (buffer->buffer) dev->vtable.DestroyBuffer(dev->device, buffer->buffer, NULL);
    if (buffer->memory) dev->vtable.FreeMemory(dev->device, buffer->memory, NULL);
    *buffer = (struct query_buffer){0};
}
static VkResult create_buffer(struct vkrt_device_data *dev, VkDeviceSize size, struct query_buffer *buffer) {
    if (size > SIZE_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
    buffer->size = size;
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = dev->queue_family_count > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = dev->queue_family_count > 1 ? dev->queue_family_count : 0,
        .pQueueFamilyIndices = dev->queue_family_count > 1 ? dev->queue_families : NULL};
    VkResult result = dev->vtable.CreateBuffer(dev->device, &info, NULL, &buffer->buffer);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements req;
    dev->vtable.GetBufferMemoryRequirements(dev->device, buffer->buffer, &req);
    VkPhysicalDeviceMemoryProperties properties;
    dev->instance_data->vtable.GetPhysicalDeviceMemoryProperties(dev->physical_device, &properties);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags &
                                                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto fail;
    }
    VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
                                       0};
    VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags, req.size, type};
    result = dev->vtable.AllocateMemory(dev->device, &allocation, NULL, &buffer->memory);
    if (result != VK_SUCCESS) goto fail;
    result = dev->vtable.BindBufferMemory(dev->device, buffer->buffer, buffer->memory, 0);
    if (result != VK_SUCCESS) goto fail;
    result = dev->vtable.MapMemory(dev->device, buffer->memory, 0, size, 0, &buffer->mapped);
    if (result != VK_SUCCESS) goto fail;
    memset(buffer->mapped, 0, (size_t)size);
    VkBufferDeviceAddressInfo address = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, buffer->buffer};
    buffer->address = dev->vtable.GetBufferDeviceAddress(dev->device, &address);
    if (!buffer->address) {
        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        goto fail;
    }
    return VK_SUCCESS;
fail:
    destroy_buffer(dev, buffer);
    return result;
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
        while (q->queues) {
            struct query_queue *next = q->queues->next;
            free(q->queues);
            q->queues = next;
        }
        if (q->property) dev->vtable.DestroyPipeline(dev->device, q->property, NULL);
        if (q->publish) dev->vtable.DestroyPipeline(dev->device, q->publish, NULL);
        if (q->layout) dev->vtable.DestroyPipelineLayout(dev->device, q->layout, NULL);
        q->property = q->publish = VK_NULL_HANDLE;
        q->layout = VK_NULL_HANDLE;
    }
    return result;
}
VkResult vkrt_query_device_init(struct vkrt_device_data *dev) {
    if (!dev->query_backend_enabled || !dev->vtable.ResetQueryPool || !dev->vtable.GetQueryPoolResults ||
        !dev->vtable.CmdWriteTimestamp || !dev->vtable.CmdCopyBuffer)
        return VK_SUCCESS;
    struct vkrt_query_device *q = calloc(1, sizeof(*q));
    if (!q) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vkrt_mutex_init(&q->lock);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)dev->instance_data->vtable.GetInstanceProcAddr(
            dev->instance_data->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!get) {
        free(q);
        return VK_SUCCESS;
    }
    get(dev->physical_device, &q->family_count, NULL);
    q->families = calloc(q->family_count, sizeof(*q->families));
    if (!q->families) {
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
    uint64_t marker[2] = {0};
    VkResult result = dev->vtable.GetQueryPoolResults(dev->device, op->marker, 0, 1, sizeof(marker), marker, 16,
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY) return result;
    if (!marker[1] && !completed_cb) return VK_SUCCESS;
    result =
        dev->vtable.GetQueryPoolResults(dev->device, op->marker, 0, 1, sizeof(marker), marker, 16,
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (result != VK_SUCCESS) return result;
    const struct vkrt_as_query_value *values = op->payload.mapped;
    const uint64_t *serials = (const void *)((const uint8_t *)op->payload.mapped + (size_t)op->count * 16);
    for (uint32_t i = 0; i < op->count; ++i) {
        struct query_host_slot *slot = &op->pool->slots[op->first + i];
        if (!slot->initialized || later(serials[i], slot->serial))
            *slot = (struct query_host_slot){values[i].value, serials[i], true, values[i].valid != 0};
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
    if (!cb->device_data->queries || (cb->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) ||
        cb->native_active_queries) {
        VKRT_LOG_ERR("AS query backend requires non-simultaneous recording without active native queries");
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return false;
    }
    struct vkrt_query_device *q = cb->device_data->queries;
    uint32_t family = UINT32_MAX;
    VkCommandPoolCreateFlags flags = 0;
    vkrt_mutex_lock(&q->lock);
    for (struct query_command_pool *pool = q->command_pools; pool; pool = pool->next)
        if (pool->pool == cb->pool) {
            family = pool->family;
            flags = pool->flags;
            break;
        }
    vkrt_mutex_unlock(&q->lock);
    if ((flags & VK_COMMAND_POOL_CREATE_PROTECTED_BIT) || family >= q->family_count || !q->families[family].timestampValidBits ||
        !(q->families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        VKRT_LOG_ERR("AS query backend requires an unprotected compute pool with timestamp support");
        return false;
    }
    return true;
}
static void dependency(struct vkrt_device_data *dev, VkCommandBuffer command) {
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    dev->vtable.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                   &barrier, 0, NULL, 0, NULL);
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
    VkResult result = create_buffer(cb->device_data, (VkDeviceSize)count * 24, &op->payload);
    VkQueryPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 1};
    if (result == VK_SUCCESS) result = cb->device_data->vtable.CreateQueryPool(cb->device_data->device, &info, NULL, &op->marker);
    if (result != VK_SUCCESS) {
        destroy_buffer(cb->device_data, &op->payload);
        free(op);
        cb->record_result = result;
        return NULL;
    }
    cb->device_data->vtable.ResetQueryPool(cb->device_data->device, op->marker, 0, 1);
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
    dependency(dev, cb->cmd);
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
     * the following timestamp's source scope. Its invocations return without
     * touching memory; no clock value or host/device atomic trick is used. */
    struct publish_args sentinel = {0};
    dev->vtable.CmdPushConstants(cb->cmd, dev->queries->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sentinel), &sentinel);
    dev->vtable.CmdDispatch(cb->cmd, 1, 1, 1);
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
        pool->slots ? create_buffer(dev, (VkDeviceSize)pool->count * 24, &pool->storage) : VK_ERROR_OUT_OF_HOST_MEMORY;
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
    vkrt_mutex_lock(&dev->queries->lock);
    VkResult result = poll_device(dev);
    if (result == VK_SUCCESS) {
        struct vkrt_as_query_value *values = pool->storage.mapped;
        uint64_t *counters = (void *)((uint8_t *)pool->storage.mapped + (size_t)pool->count * 16);
        for (uint32_t i = first; i < first + count; ++i) {
            struct query_host_slot *slot = &pool->slots[i];
            slot->serial = counters[i] = slot->initialized ? slot->serial + 1 : 1;
            slot->initialized = true;
            slot->available = false;
            slot->value = 0;
            values[i] = (struct vkrt_as_query_value){0};
        }
    }
    vkrt_mutex_unlock(&dev->queries->lock);
}
static VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer command, VkQueryPool handle, uint32_t first, uint32_t count) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    struct query_pool *pool = find_pool(handle);
    if (!pool) {
        cb->device_data->vtable.CmdResetQueryPool(command, handle, first, count);
        return;
    }
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
    struct query_operation *op = new_operation(cb, pool, first, count);
    if (!op) return;
    struct vkrt_device_data *dev = cb->device_data;
    dependency(dev, command);
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
    VkResult result = create_buffer(cb->device_data, required, &scratch->buffer);
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
    dependency(dev, command);
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
        regions[i] = (VkBufferCopy){(VkDeviceSize)i * stride, offset + (VkDeviceSize)i * stride, bytes};
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
    struct vkrt_query_command **link = &dev->queries->commands;
    while (*link && *link != q) link = &(*link)->next;
    if (*link) *link = q->next;
    vkrt_mutex_unlock(&dev->queries->lock);
    while (q->head) {
        struct query_operation *next = q->head->next;
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
/* A wait at TRANSFER can protect a native query copy while allowing its
 * private CS formatter to run too early. Such a wait also scopes later batches
 * and later submissions on the same queue. Remember it until a native idle
 * acquire, instead of broadening waits and potentially blocking a query prefix. */
static bool command_has_copy(VkCommandBuffer command) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (!cb || !cb->queries) return false;
    if (cb->queries->scratch) return true;
    for (struct query_child *child = cb->queries->children; child; child = child->next)
        if (command_has_copy(child->command)) return true;
    return false;
}
VkResult vkrt_query_submit_qualify(struct vkrt_device_data *dev, struct vkrt_query_submission *transaction, VkQueue queue,
                                   bool unsafe_wait, uint32_t count, const VkCommandBuffer *commands) {
    if (!dev->queries) return VK_SUCCESS;
    vkrt_mutex_lock(&dev->queries->lock);
    struct query_queue *state = dev->queries->queues;
    while (state && state->queue != queue) state = state->next;
    VkResult result = VK_SUCCESS;
    if (!state) {
        state = calloc(1, sizeof(*state));
        if (!state)
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
        else {
            state->queue = queue;
            state->next = dev->queries->queues;
            dev->queries->queues = state;
        }
    }
    if (state && (state->unsafe_waits || transaction->unsafe_waits || unsafe_wait))
        for (uint32_t i = 0; i < count; ++i)
            if (command_has_copy(commands[i])) {
                VKRT_LOG_ERR("AS query copy requires COMPUTE-scoped waits and no unresolved narrower queue wait history");
                result = VK_ERROR_FEATURE_NOT_PRESENT;
                break;
            }
    if (result == VK_SUCCESS) {
        transaction->queue = queue;
        transaction->unsafe_waits |= unsafe_wait;
    }
    vkrt_mutex_unlock(&dev->queries->lock);
    return result;
}
void vkrt_query_queue_idle(struct vkrt_device_data *dev, VkQueue queue) {
    if (!dev->queries) return;
    vkrt_mutex_lock(&dev->queries->lock);
    for (struct query_queue *state = dev->queries->queues; state; state = state->next)
        if (!queue || state->queue == queue) state->unsafe_waits = false;
    vkrt_mutex_unlock(&dev->queries->lock);
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
        VkResult result = harvest(dev, op, true);
        if (result != VK_SUCCESS) return result;
        dev->vtable.ResetQueryPool(dev->device, op->marker, 0, 1);
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
    if (result == VK_SUCCESS && transaction->unsafe_waits)
        for (struct query_queue *queue = dev->queries->queues; queue; queue = queue->next)
            if (queue->queue == transaction->queue) queue->unsafe_waits = true;
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
    while (q->pools) DestroyQueryPool(dev->device, (VkQueryPool)(uintptr_t)q->pools, pool_allocator(q->pools));
    while (q->command_pools) {
        struct query_command_pool *next = q->command_pools->next;
        free(q->command_pools);
        q->command_pools = next;
    }
    while (q->queues) {
        struct query_queue *next = q->queues->next;
        free(q->queues);
        q->queues = next;
    }
    if (q->property) dev->vtable.DestroyPipeline(dev->device, q->property, NULL);
    if (q->publish) dev->vtable.DestroyPipeline(dev->device, q->publish, NULL);
    if (q->format) dev->vtable.DestroyPipeline(dev->device, q->format, NULL);
    if (q->layout) dev->vtable.DestroyPipelineLayout(dev->device, q->layout, NULL);
    free(q->families);
    free(q);
    dev->queries = NULL;
}
static VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *info,
                                                        const VkAllocationCallbacks *allocator, VkCommandPool *pool) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    VkResult result = dev->vtable.CreateCommandPool(device, info, allocator, pool);
    if (result != VK_SUCCESS || !dev->queries) return result;
    struct query_command_pool *record = calloc(1, sizeof(*record));
    if (!record) {
        dev->vtable.DestroyCommandPool(device, *pool, allocator);
        *pool = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    vkrt_mutex_lock(&dev->queries->lock);
    *record = (struct query_command_pool){dev->queries->command_pools, *pool, info->queueFamilyIndex, info->flags};
    dev->queries->command_pools = record;
    vkrt_mutex_unlock(&dev->queries->lock);
    return VK_SUCCESS;
}
void vkrt_query_forget_command_pool(struct vkrt_device_data *dev, VkCommandPool pool) {
    if (!dev->queries) return;
    vkrt_mutex_lock(&dev->queries->lock);
    struct query_command_pool **link = &dev->queries->command_pools;
    while (*link && (*link)->pool != pool) link = &(*link)->next;
    if (*link) {
        struct query_command_pool *record = *link;
        *link = record->next;
        free(record);
    }
    vkrt_mutex_unlock(&dev->queries->lock);
}
static VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer command, uint32_t count, const VkCommandBuffer *children) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (cb->device_data->queries) {
        for (uint32_t i = 0; i < count; ++i) {
            struct vkrt_cmd_buffer_data *child = VKRT_FIND(struct vkrt_cmd_buffer_data, children[i]);
            if (!child || !child->queries) continue;
            if ((cb->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) || cb->native_active_queries) {
                cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
                return;
            }
            struct vkrt_query_command *state = command_state(cb);
            if (!state) return;
            struct query_child *record = malloc(sizeof(*record));
            if (!record) {
                cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
                return;
            }
            *record = (struct query_child){state->children, children[i]};
            state->children = record;
        }
    }
    cb->device_data->vtable.CmdExecuteCommands(command, count, children);
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
    ENTRY(CreateCommandPool);
    ENTRY(CmdExecuteCommands);
    ENTRY(CmdBeginQuery);
    ENTRY(CmdEndQuery);
    ENTRY(CmdBeginQueryIndexedEXT);
    ENTRY(CmdEndQueryIndexedEXT);
#undef ENTRY
    if (!strcmp(name, "vkResetQueryPoolEXT")) return (PFN_vkVoidFunction)ResetQueryPool;
    return NULL;
}
