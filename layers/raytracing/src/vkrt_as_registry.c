/* SPDX-License-Identifier: MIT
 * Public AS values occupy an aligned, per-buffer affine namespace. Only live
 * AS offsets allocate GPU radix paths; buffer byte size never allocates GPU
 * shadow storage. These values are AS operands, never raw buffer BDAs.
 */
#include "vkrt_as_registry.h"
#include "vkrt_layer.h"

struct registry_page {
    struct vkrt_private_slice storage;
    struct registry_page *children[VKRT_AS_REGISTRY_SLOTS];
    uint64_t users;
};
struct registry_buffer {
    struct registry_buffer *next;
    struct vkrt_as_registry_entry *entries;
    VkBuffer buffer;
    VkDeviceSize size;
    uint64_t first, count, references;
    VkDeviceAddress physical_base;
    bool has_physical_base;
    bool alive;
};
struct vkrt_as_registry_entry {
    struct vkrt_as_registry_entry *next;
    struct registry_buffer *owner;
    struct registry_page *path[VKRT_AS_REGISTRY_LEVELS];
    uint32_t indices[VKRT_AS_REGISTRY_LEVELS];
    VkDeviceSize offset;
    VkDeviceAddress public_address, physical_address;
    struct vkrt_private_slice descriptor;
    uint64_t references;
};
struct vkrt_as_registry {
    struct vkrt_device_data *dev;
    struct vkrt_private_slice header;
    struct registry_page *root;
    struct registry_buffer *buffers;
    vkrt_mutex_t lock;
    bool has_allocator;
    VkAllocationCallbacks allocator;
};

static const VkAllocationCallbacks *callbacks(const struct vkrt_as_registry *registry) {
    return registry->has_allocator ? &registry->allocator : NULL;
}
static void *allocate(struct vkrt_as_registry *registry, size_t size, size_t alignment) {
    const VkAllocationCallbacks *allocator = callbacks(registry);
    void *out = allocator ? allocator->pfnAllocation(allocator->pUserData, size, alignment, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE)
                          : malloc(size);
    if (out) memset(out, 0, size);
    return out;
}
static void deallocate(struct vkrt_as_registry *registry, void *value) {
    if (!value) return;
    const VkAllocationCallbacks *allocator = callbacks(registry);
    if (allocator)
        allocator->pfnFree(allocator->pUserData, value);
    else
        free(value);
}
static void storage_finish(struct vkrt_as_registry *registry, struct vkrt_private_slice *storage) {
    vkrt_private_pool_release(registry->dev, storage);
}
static VkResult storage_init(struct vkrt_as_registry *registry, VkDeviceSize size, struct vkrt_private_slice *storage) {
    return vkrt_private_pool_allocate(registry->dev, size, false, storage);
}
static VkResult page_create(struct vkrt_as_registry *registry, struct registry_page **out) {
    struct registry_page *page = allocate(registry, sizeof(*page), _Alignof(struct registry_page));
    if (!page) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result = storage_init(registry, VKRT_AS_REGISTRY_SLOTS * sizeof(uint64_t), &page->storage);
    if (result != VK_SUCCESS) {
        deallocate(registry, page);
        return result;
    }
    *out = page;
    return VK_SUCCESS;
}
static void page_destroy(struct vkrt_as_registry *registry, struct registry_page *page) {
    if (!page) return;
    for (uint32_t i = 0; i < VKRT_AS_REGISTRY_SLOTS; ++i) page_destroy(registry, page->children[i]);
    storage_finish(registry, &page->storage);
    deallocate(registry, page);
}

VkResult vkrt_as_registry_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator) {
    if (!dev->vtable.CreateBuffer || !dev->vtable.DestroyBuffer || !dev->vtable.GetBufferMemoryRequirements ||
        !dev->vtable.AllocateMemory || !dev->vtable.FreeMemory || !dev->vtable.BindBufferMemory || !dev->vtable.MapMemory ||
        !dev->vtable.UnmapMemory || !dev->vtable.GetBufferDeviceAddress || !dev->instance_data ||
        !dev->instance_data->vtable.GetPhysicalDeviceMemoryProperties)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult pool_result = vkrt_private_pool_init(dev, allocator);
    if (pool_result != VK_SUCCESS) return pool_result;
    if (!allocator && dev->queue_families_have_allocator) allocator = &dev->queue_families_allocator;
    if (!allocator && dev->instance_data->has_allocator) allocator = &dev->instance_data->allocator;
    struct vkrt_as_registry seed = {.dev = dev};
    if (allocator) {
        seed.has_allocator = true;
        seed.allocator = *allocator;
    }
    struct vkrt_as_registry *registry = allocate(&seed, sizeof(*registry), _Alignof(struct vkrt_as_registry));
    if (!registry) return VK_ERROR_OUT_OF_HOST_MEMORY;
    registry->dev = dev;
    registry->has_allocator = seed.has_allocator;
    registry->allocator = seed.allocator;
#ifdef _WIN32
    vkrt_mutex_init(&registry->lock);
#else
    if (pthread_mutex_init(&registry->lock, NULL)) {
        deallocate(&seed, registry);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
#endif
    dev->as_registry = registry;
    VkResult result = page_create(registry, &registry->root);
    if (result == VK_SUCCESS) result = storage_init(registry, 64, &registry->header);
    if (result != VK_SUCCESS) {
        vkrt_as_registry_finish(dev);
        return result;
    }
    uint32_t metadata[8] = {VKRT_AS_REGISTRY_MAGIC, VKRT_AS_REGISTRY_VERSION, 3, 8, 7, 8, 0, 0};
    memcpy(registry->header.mapping, metadata, sizeof(metadata));
    memcpy((uint8_t *)registry->header.mapping + 32, &registry->root->storage.address, sizeof(uint64_t));
    return VK_SUCCESS;
}
void vkrt_as_registry_finish(struct vkrt_device_data *dev) {
    struct vkrt_as_registry *registry = dev->as_registry;
    if (!registry) return;
    while (registry->buffers) {
        struct registry_buffer *buffer = registry->buffers;
        registry->buffers = buffer->next;
        while (buffer->entries) {
            struct vkrt_as_registry_entry *entry = buffer->entries;
            buffer->entries = entry->next;
            vkrt_private_pool_release(dev, &entry->descriptor);
            deallocate(registry, entry);
        }
        deallocate(registry, buffer);
    }
    storage_finish(registry, &registry->header);
    page_destroy(registry, registry->root);
#ifndef _WIN32
    pthread_mutex_destroy(&registry->lock);
#endif
    VkAllocationCallbacks copy = registry->allocator;
    bool custom = registry->has_allocator;
    dev->as_registry = NULL;
    if (custom)
        copy.pfnFree(copy.pUserData, registry);
    else
        free(registry);
}
VkDeviceAddress vkrt_as_registry_address(struct vkrt_device_data *dev) {
    return dev->as_registry ? dev->as_registry->header.address : 0;
}
static struct registry_buffer *find_buffer(struct vkrt_as_registry *registry, VkBuffer handle) {
    for (struct registry_buffer *buffer = registry->buffers; buffer; buffer = buffer->next)
        if (buffer->alive && buffer->buffer == handle) return buffer;
    return NULL;
}
static void buffer_remove(struct vkrt_as_registry *registry, struct registry_buffer *buffer) {
    struct registry_buffer **link = &registry->buffers;
    while (*link != buffer) link = &(*link)->next;
    *link = buffer->next;
    deallocate(registry, buffer);
}
VkResult vkrt_as_registry_buffer_create(struct vkrt_device_data *dev, VkBuffer handle, VkDeviceSize size) {
    struct vkrt_as_registry *registry = dev->as_registry;
    if (!registry) return VK_ERROR_INITIALIZATION_FAILED;
    vkrt_mutex_lock(&registry->lock);
    VkResult result = VK_SUCCESS;
    if (find_buffer(registry, handle)) {
        result = VK_ERROR_INITIALIZATION_FAILED;
        goto done;
    }
    /* Slots are 256-byte public addresses. Include offset==size for a legal
     * zero-sized AS at a buffer's aligned end, without overflowing size+255. */
    const uint64_t maximum = UINT64_MAX >> 8;
    uint64_t count = (size >> 8) + 1, first = 1;
    struct registry_buffer **link = &registry->buffers;
    while (*link) {
        if (count <= (*link)->first - first) break;
        first = (*link)->first + (*link)->count;
        link = &(*link)->next;
    }
    if (first > maximum || count > maximum - first + 1) {
        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        goto done;
    }
    struct registry_buffer *buffer = allocate(registry, sizeof(*buffer), _Alignof(struct registry_buffer));
    if (!buffer) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto done;
    }
    *buffer =
        (struct registry_buffer){.next = *link, .buffer = handle, .size = size, .first = first, .count = count, .alive = true};
    *link = buffer;
done:
    vkrt_mutex_unlock(&registry->lock);
    return result;
}
void vkrt_as_registry_buffer_destroy(struct vkrt_device_data *dev, VkBuffer handle) {
    struct vkrt_as_registry *registry = dev->as_registry;
    if (!registry) return;
    vkrt_mutex_lock(&registry->lock);
    struct registry_buffer *buffer = find_buffer(registry, handle);
    if (buffer) {
        buffer->alive = false;
        if (!buffer->references) buffer_remove(registry, buffer);
    }
    vkrt_mutex_unlock(&registry->lock);
}
/* Remove empty unpublished/new paths, or the final path of a released entry.
 * Other live AS paths are immutable, including when a shared page is reused. */
static void path_prune(struct vkrt_as_registry *registry, struct vkrt_as_registry_entry *entry) {
    for (uint32_t level = VKRT_AS_REGISTRY_LEVELS - 1; level > 0; --level) {
        struct registry_page *page = entry->path[level];
        if (!page || page->users) continue;
        struct registry_page *parent = entry->path[level - 1];
        parent->children[entry->indices[level - 1]] = NULL;
        uint64_t empty = 0;
        memcpy((uint8_t *)parent->storage.mapping + entry->indices[level - 1] * 8, &empty, 8);
        page_destroy(registry, page);
        entry->path[level] = NULL;
    }
}
static void entry_publish(struct vkrt_as_registry_entry *entry, VkDeviceAddress address) {
    if (entry->physical_address == address) return;
    memcpy((uint8_t *)entry->path[VKRT_AS_REGISTRY_LEVELS - 1]->storage.mapping + entry->indices[VKRT_AS_REGISTRY_LEVELS - 1] * 8,
           &address, 8);
    entry->physical_address = address;
}
VkResult vkrt_as_registry_acquire(struct vkrt_device_data *dev, VkBuffer handle, VkDeviceSize offset,
                                  struct vkrt_as_registry_entry **out, VkDeviceAddress *public_address) {
    *out = NULL;
    *public_address = 0;
    struct vkrt_as_registry *registry = dev->as_registry;
    if (!registry) return VK_ERROR_INITIALIZATION_FAILED;
    vkrt_mutex_lock(&registry->lock);
    VkResult result = VK_SUCCESS;
    struct registry_buffer *buffer = find_buffer(registry, handle);
    if (!buffer || (offset & 255) || offset > buffer->size) {
        result = VK_ERROR_INITIALIZATION_FAILED;
        goto done;
    }
    for (struct vkrt_as_registry_entry *entry = buffer->entries; entry; entry = entry->next)
        if (entry->offset == offset) {
            ++entry->references;
            ++buffer->references;
            *out = entry;
            *public_address = entry->public_address;
            goto done;
        }
    struct vkrt_as_registry_entry *entry = allocate(registry, sizeof(*entry), _Alignof(struct vkrt_as_registry_entry));
    if (!entry) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto done;
    }
    entry->owner = buffer;
    entry->offset = offset;
    entry->references = 1;
    uint64_t key = buffer->first + (offset >> 8);
    entry->public_address = key << 8;
    entry->path[0] = registry->root;
    for (uint32_t level = 0; level < VKRT_AS_REGISTRY_LEVELS; ++level) {
        uint32_t index = (uint32_t)((key >> ((VKRT_AS_REGISTRY_LEVELS - level - 1) * 8)) & 255);
        entry->indices[level] = index;
        if (level + 1 == VKRT_AS_REGISTRY_LEVELS) break;
        struct registry_page *parent = entry->path[level];
        if (!parent->children[index]) {
            result = page_create(registry, &parent->children[index]);
            if (result != VK_SUCCESS) {
                path_prune(registry, entry);
                deallocate(registry, entry);
                goto done;
            }
            memcpy((uint8_t *)parent->storage.mapping + index * 8, &parent->children[index]->storage.address, 8);
        }
        entry->path[level + 1] = parent->children[index];
    }
    result = vkrt_private_pool_allocate(dev, sizeof(VkDeviceAddress), true, &entry->descriptor);
    if (result != VK_SUCCESS) {
        path_prune(registry, entry);
        deallocate(registry, entry);
        goto done;
    }
    memcpy(entry->descriptor.mapping, &entry->public_address, sizeof(VkDeviceAddress));
    for (uint32_t level = 0; level < VKRT_AS_REGISTRY_LEVELS; ++level) ++entry->path[level]->users;
    entry->next = buffer->entries;
    buffer->entries = entry;
    ++buffer->references;
    if (buffer->has_physical_base) entry_publish(entry, buffer->physical_base + offset);
    *out = entry;
    *public_address = entry->public_address;
done:
    vkrt_mutex_unlock(&registry->lock);
    return result;
}
void vkrt_as_registry_release(struct vkrt_device_data *dev, struct vkrt_as_registry_entry *entry) {
    struct vkrt_as_registry *registry = dev->as_registry;
    if (!registry || !entry) return;
    vkrt_mutex_lock(&registry->lock);
    struct registry_buffer *buffer = entry->owner;
    --buffer->references;
    if (!--entry->references) {
        vkrt_private_pool_release(dev, &entry->descriptor);
        uint64_t empty = 0;
        memcpy(
            (uint8_t *)entry->path[VKRT_AS_REGISTRY_LEVELS - 1]->storage.mapping + entry->indices[VKRT_AS_REGISTRY_LEVELS - 1] * 8,
            &empty, 8);
        struct vkrt_as_registry_entry **link = &buffer->entries;
        while (*link != entry) link = &(*link)->next;
        *link = entry->next;
        for (uint32_t level = 0; level < VKRT_AS_REGISTRY_LEVELS; ++level) --entry->path[level]->users;
        path_prune(registry, entry);
        deallocate(registry, entry);
    }
    if (!buffer->alive && !buffer->references) buffer_remove(registry, buffer);
    vkrt_mutex_unlock(&registry->lock);
}
VkDeviceAddress vkrt_as_registry_publish(struct vkrt_device_data *dev, struct vkrt_as_registry_entry *entry,
                                         VkDeviceAddress physical_address) {
    struct vkrt_as_registry *registry = dev->as_registry;
    if (!registry || !entry || !physical_address || (physical_address & 3)) return 0;
    vkrt_mutex_lock(&registry->lock);
    struct registry_buffer *buffer = entry->owner;
    VkDeviceAddress result = 0;
    if (physical_address < entry->offset) goto done;
    VkDeviceAddress base = physical_address - entry->offset;
    if (base > UINT64_MAX - buffer->size) goto done;
    if (buffer->has_physical_base) {
        if (buffer->physical_base == base)
            result = entry->physical_address;
        else
            VKRT_LOG_ERR("AS registry: immutable backing address changed");
    } else {
        /* One valid address query proves this ordinary buffer is bound. Its
         * binding cannot change. Publish every preallocated offset, including
         * unbuilt BLAS referenced by GENERIC arithmetic during deserialization.
         * Future AS creations use this same cached base without another query.
         * Subsequent Get/Build calls perform no GPU mapping writes. */
        buffer->physical_base = base;
        buffer->has_physical_base = true;
        for (struct vkrt_as_registry_entry *current = buffer->entries; current; current = current->next)
            entry_publish(current, base + current->offset);
        result = entry->physical_address;
    }
done:
    vkrt_mutex_unlock(&registry->lock);
    return result;
}

const struct vkrt_private_slice *vkrt_as_registry_descriptor(const struct vkrt_as_registry_entry *entry) {
    return &entry->descriptor;
}
