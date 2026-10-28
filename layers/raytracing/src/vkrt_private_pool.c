/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT
 */
#include "vkrt_layer.h"
#include "vkrt_private_pool.h"

struct vkrt_private_allocation {
    struct vkrt_private_allocation *next;
    struct vkrt_private_chunk *chunk;
    VkDeviceSize offset, size;
};
struct vkrt_private_chunk {
    struct vkrt_private_chunk *next;
    struct vkrt_private_allocation *allocations;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapping;
    VkDeviceAddress address;
    VkDeviceSize size;
    enum vkrt_private_memory_class memory_class;
};
struct vkrt_private_pool {
    struct vkrt_device_data *dev;
    struct vkrt_private_chunk *chunks;
    vkrt_mutex_t lock;
    VkDeviceSize descriptor_alignment;
    bool custom;
    VkAllocationCallbacks allocator;
};
static const VkAllocationCallbacks *callbacks(const struct vkrt_private_pool *pool) {
    return pool->custom ? &pool->allocator : NULL;
}
static void *allocate(struct vkrt_private_pool *pool, size_t size, size_t alignment) {
    const VkAllocationCallbacks *a = callbacks(pool);
    void *p = a ? a->pfnAllocation(a->pUserData, size, alignment, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE) : malloc(size);
    if (p) memset(p, 0, size);
    return p;
}
static void release(struct vkrt_private_pool *pool, void *p) {
    if (!p) return;
    const VkAllocationCallbacks *a = callbacks(pool);
    if (a)
        a->pfnFree(a->pUserData, p);
    else
        free(p);
}
static bool aligned(VkDeviceSize value, VkDeviceSize alignment, VkDeviceSize *out) {
    if (!alignment || (alignment & (alignment - 1)) || value > UINT64_MAX - (alignment - 1)) return false;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return true;
}
static void chunk_destroy(struct vkrt_private_pool *pool, struct vkrt_private_chunk *chunk) {
    struct vkrt_device_data *dev = pool->dev;
    if (chunk->mapping) dev->vtable.UnmapMemory(dev->device, chunk->memory);
    if (chunk->buffer) dev->vtable.DestroyBuffer(dev->device, chunk->buffer, callbacks(pool));
    if (chunk->memory) dev->vtable.FreeMemory(dev->device, chunk->memory, callbacks(pool));
    release(pool, chunk);
}
static VkResult chunk_create(struct vkrt_private_pool *pool, VkDeviceSize size, enum vkrt_private_memory_class memory_class,
                             struct vkrt_private_chunk **out) {
    struct vkrt_device_data *dev = pool->dev;
    struct vkrt_private_chunk *chunk = allocate(pool, sizeof(*chunk), _Alignof(struct vkrt_private_chunk));
    if (!chunk) return VK_ERROR_OUT_OF_HOST_MEMORY;
    chunk->size = size;
    chunk->memory_class = memory_class;
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (dev->queue_family_count > 1) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = dev->queue_family_count;
        info.pQueueFamilyIndices = dev->queue_families;
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult result = dev->vtable.CreateBuffer(dev->device, &info, callbacks(pool), &buffer);
    if (result != VK_SUCCESS) goto fail;
    chunk->buffer = buffer;
    VkMemoryRequirements requirements;
    dev->vtable.GetBufferMemoryRequirements(dev->device, buffer, &requirements);
    if (requirements.size < size) {
        result = VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    VkPhysicalDeviceMemoryProperties properties = {0};
    dev->instance_data->vtable.GetPhysicalDeviceMemoryProperties(dev->physical_device, &properties);
    if (properties.memoryTypeCount > VK_MAX_MEMORY_TYPES) {
        result = VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    uint32_t types[VK_MAX_MEMORY_TYPES], type_count = 0;
    for (unsigned pass = 0; pass < (memory_class == VKRT_PRIVATE_HOST_COHERENT ? 1u : 2u); ++pass)
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
            if (!(requirements.memoryTypeBits & (1u << i)) ||
                (flags & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT)))
                continue;
            if (memory_class == VKRT_PRIVATE_HOST_COHERENT) {
                if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                    continue;
                types[type_count++] = i;
                break; /* Preserve the coherent allocator's existing selection. */
            }
            if (((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) == (pass == 0)) types[type_count++] = i;
        }
    if (!type_count) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto fail;
    }
    /* One buffer per native memory object also satisfies implementations that
     * require dedicated allocation, without depending on a guessed requirement. */
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = buffer};
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &dedicated, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags, .allocationSize = requirements.size};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < type_count; ++i) {
        allocation.memoryTypeIndex = types[i];
        result = dev->vtable.AllocateMemory(dev->device, &allocation, callbacks(pool), &memory);
        if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY || memory_class == VKRT_PRIVATE_HOST_COHERENT) break;
    }
    if (result != VK_SUCCESS) goto fail;
    chunk->memory = memory;
    result = dev->vtable.BindBufferMemory(dev->device, buffer, memory, 0);
    if (result != VK_SUCCESS) goto fail;
    if (memory_class == VKRT_PRIVATE_HOST_COHERENT) {
        void *mapping = NULL;
        result = dev->vtable.MapMemory(dev->device, memory, 0, VK_WHOLE_SIZE, 0, &mapping);
        if (result != VK_SUCCESS) goto fail;
        if (!mapping) {
            dev->vtable.UnmapMemory(dev->device, memory);
            result = VK_ERROR_INITIALIZATION_FAILED;
            goto fail;
        }
        chunk->mapping = mapping;
    }
    VkBufferDeviceAddressInfo address = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
    chunk->address = dev->vtable.GetBufferDeviceAddress(dev->device, &address);
    if (!chunk->address || chunk->address > UINT64_MAX - (size - 1)) {
        result = VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    VKRT_LOG_DBG("Private storage pool arena: bytes=%llu nativeAllocationBytes=%llu class=%s", (unsigned long long)size,
                 (unsigned long long)requirements.size, memory_class == VKRT_PRIVATE_HOST_COHERENT ? "coherent" : "device");
    *out = chunk;
    return VK_SUCCESS;
fail:
    chunk_destroy(pool, chunk);
    return result;
}
VkResult vkrt_private_pool_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator) {
    if (dev->private_pool) return VK_SUCCESS;
    if (!dev->instance_data || !dev->instance_data->vtable.GetPhysicalDeviceMemoryProperties || !dev->vtable.CreateBuffer ||
        !dev->vtable.DestroyBuffer || !dev->vtable.GetBufferMemoryRequirements || !dev->vtable.AllocateMemory ||
        !dev->vtable.FreeMemory || !dev->vtable.BindBufferMemory || !dev->vtable.MapMemory || !dev->vtable.UnmapMemory ||
        !dev->vtable.GetBufferDeviceAddress)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!allocator && dev->queue_families_have_allocator) allocator = &dev->queue_families_allocator;
    if (!allocator && dev->instance_data->has_allocator) allocator = &dev->instance_data->allocator;
    struct vkrt_private_pool seed = {.dev = dev};
    if (allocator) {
        seed.custom = true;
        seed.allocator = *allocator;
    }
    struct vkrt_private_pool *pool = allocate(&seed, sizeof(*pool), _Alignof(struct vkrt_private_pool));
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pool = seed;
    /* The core required-limits maximum is a safe fallback for isolated host
     * adapters without a properties callback. Production uses the actual limit. */
    pool->descriptor_alignment = 256;
    if (dev->instance_data->vtable.GetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties properties = {0};
        dev->instance_data->vtable.GetPhysicalDeviceProperties(dev->physical_device, &properties);
        pool->descriptor_alignment = properties.limits.minStorageBufferOffsetAlignment;
    }
    if (pool->descriptor_alignment < 8) pool->descriptor_alignment = 8;
    if (pool->descriptor_alignment & (pool->descriptor_alignment - 1)) {
        release(pool, pool);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
#ifdef _WIN32
    vkrt_mutex_init(&pool->lock);
#else
    if (pthread_mutex_init(&pool->lock, NULL)) {
        release(pool, pool);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
#endif
    dev->private_pool = pool;
    return VK_SUCCESS;
}
/* Two power-of-two congruences have a solution exactly when the base address
 * is divisible by their common factor. Find the first solution in a hole. */
static bool slice_offset(const struct vkrt_private_chunk *chunk, VkDeviceSize begin, VkDeviceSize physical_alignment,
                         VkDeviceSize offset_alignment, VkDeviceSize *offset) {
    VkDeviceSize common = physical_alignment < offset_alignment ? physical_alignment : offset_alignment;
    if (chunk->address & (common - 1)) return false;
    if (physical_alignment > offset_alignment) {
        VkDeviceAddress address;
        if (begin > UINT64_MAX - chunk->address || !aligned(chunk->address + begin, physical_alignment, &address)) return false;
        *offset = address - chunk->address;
        return true;
    }
    return aligned(begin, offset_alignment, offset);
}
static bool chunk_find(struct vkrt_private_chunk *chunk, VkDeviceSize size, VkDeviceSize physical_alignment,
                       VkDeviceSize offset_alignment, struct vkrt_private_allocation ***link_out, VkDeviceSize *offset) {
    struct vkrt_private_allocation **link = &chunk->allocations;
    VkDeviceSize begin = 0;
    for (;;) {
        VkDeviceSize end = *link ? (*link)->offset : chunk->size;
        if (begin >= chunk->size || !slice_offset(chunk, begin, physical_alignment, offset_alignment, offset)) return false;
        if (*offset <= end && size <= end - *offset) {
            *link_out = link;
            return true;
        }
        if (!*link) return false;
        begin = (*link)->offset + (*link)->size;
        link = &(*link)->next;
    }
}
VkResult vkrt_private_pool_allocate_ex(struct vkrt_device_data *dev, VkDeviceSize size, enum vkrt_private_memory_class memory_class,
                                       VkDeviceSize physical_alignment, VkDeviceSize offset_alignment,
                                       struct vkrt_private_slice *slice) {
    if (!slice) return VK_ERROR_INITIALIZATION_FAILED;
    memset(slice, 0, sizeof(*slice));
    struct vkrt_private_pool *pool = dev ? dev->private_pool : NULL;
    if (!pool || !size || size > SIZE_MAX ||
        (memory_class != VKRT_PRIVATE_HOST_COHERENT && memory_class != VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED) ||
        !physical_alignment || (physical_alignment & (physical_alignment - 1)) || !offset_alignment ||
        (offset_alignment & (offset_alignment - 1)))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct vkrt_private_allocation *allocation = allocate(pool, sizeof(*allocation), _Alignof(struct vkrt_private_allocation));
    if (!allocation) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vkrt_mutex_lock(&pool->lock);
    VkResult result = VK_SUCCESS;
    struct vkrt_private_chunk *chunk = pool->chunks;
    struct vkrt_private_allocation **link = NULL;
    VkDeviceSize offset = 0;
    for (; chunk; chunk = chunk->next)
        if (chunk->memory_class == memory_class && chunk_find(chunk, size, physical_alignment, offset_alignment, &link, &offset))
            goto found;
    VkDeviceSize needed, alignment = physical_alignment > offset_alignment ? physical_alignment : offset_alignment;
    if (size > UINT64_MAX - (alignment - 1) || !aligned(size + alignment - 1, 8, &needed) || needed > SIZE_MAX) {
        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        goto fail;
    }
    if (needed < 65536) needed = 65536;
    result = chunk_create(pool, needed, memory_class, &chunk);
    if (result != VK_SUCCESS) goto fail;
    if (!chunk_find(chunk, size, physical_alignment, offset_alignment, &link, &offset)) {
        /* Do not retain an unusable empty chunk or retry indefinitely when
         * an ICD's base address cannot satisfy both alignment promises. */
        chunk_destroy(pool, chunk);
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto fail;
    }
    chunk->next = pool->chunks;
    pool->chunks = chunk;
    goto found;
fail:
    vkrt_mutex_unlock(&pool->lock);
    release(pool, allocation);
    return result;
found:
    allocation->chunk = chunk;
    allocation->offset = offset;
    allocation->size = size;
    allocation->next = *link;
    *link = allocation;
    *slice = (struct vkrt_private_slice){.buffer = chunk->buffer,
                                         .offset = offset,
                                         .size = size,
                                         .address = chunk->address + offset,
                                         .mapping = chunk->mapping ? (uint8_t *)chunk->mapping + (size_t)offset : NULL,
                                         .allocation = allocation};
    if (slice->mapping) memset(slice->mapping, 0, (size_t)size);
    vkrt_mutex_unlock(&pool->lock);
    return VK_SUCCESS;
}
VkResult vkrt_private_pool_allocate(struct vkrt_device_data *dev, VkDeviceSize size, bool descriptor,
                                    struct vkrt_private_slice *slice) {
    VkDeviceSize offset_alignment = descriptor && dev && dev->private_pool ? dev->private_pool->descriptor_alignment : 1;
    return vkrt_private_pool_allocate_ex(dev, size, VKRT_PRIVATE_HOST_COHERENT, descriptor ? 1 : 8, offset_alignment, slice);
}
void vkrt_private_pool_release(struct vkrt_device_data *dev, struct vkrt_private_slice *slice) {
    struct vkrt_private_allocation *allocation = slice->allocation;
    if (!allocation) return;
    struct vkrt_private_pool *pool = dev->private_pool;
    vkrt_mutex_lock(&pool->lock);
    struct vkrt_private_chunk *chunk = allocation->chunk;
    struct vkrt_private_allocation **link = &chunk->allocations;
    while (*link != allocation) link = &(*link)->next;
    *link = allocation->next;
    if (!chunk->allocations) {
        struct vkrt_private_chunk **owner = &pool->chunks;
        while (*owner != chunk) owner = &(*owner)->next;
        *owner = chunk->next;
        chunk_destroy(pool, chunk);
    }
    vkrt_mutex_unlock(&pool->lock);
    release(pool, allocation);
    memset(slice, 0, sizeof(*slice));
}
void vkrt_private_pool_finish(struct vkrt_device_data *dev) {
    struct vkrt_private_pool *pool = dev->private_pool;
    if (!pool) return;
    while (pool->chunks) {
        struct vkrt_private_chunk *chunk = pool->chunks;
        pool->chunks = chunk->next;
        while (chunk->allocations) {
            struct vkrt_private_allocation *allocation = chunk->allocations;
            chunk->allocations = allocation->next;
            release(pool, allocation);
        }
        chunk_destroy(pool, chunk);
    }
#ifndef _WIN32
    pthread_mutex_destroy(&pool->lock);
#endif
    VkAllocationCallbacks allocator = pool->allocator;
    bool custom = pool->custom;
    dev->private_pool = NULL;
    if (custom)
        allocator.pfnFree(allocator.pUserData, pool);
    else
        free(pool);
}
