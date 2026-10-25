/* SPDX-License-Identifier: MIT
 *
 * A virtual AS is a layer-owned token backed by ordinary BVHIR buffer storage.
 * Its descriptor is a lifetime-owned cell containing the root BDA.
 */
#include "vkrt_layer.h"
#include "vkrt_build_layout.h"

VkResult vkrt_device_queue_families_init(struct vkrt_device_data *dev, const VkDeviceCreateInfo *ci,
                                         const VkAllocationCallbacks *allocator) {
    if (ci->queueCreateInfoCount && !ci->pQueueCreateInfos) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = 0;
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
        bool seen = false;
        for (uint32_t j = 0; j < i; ++j)
            seen |= ci->pQueueCreateInfos[i].queueFamilyIndex == ci->pQueueCreateInfos[j].queueFamilyIndex;
        if (!seen) ++count;
    }
    if (!count) return VK_SUCCESS;
#if SIZE_MAX <= UINT32_MAX
    if (count > SIZE_MAX / sizeof(uint32_t)) return VK_ERROR_OUT_OF_HOST_MEMORY;
#endif
    size_t size = (size_t)count * sizeof(uint32_t);
    uint32_t *families =
        allocator ? allocator->pfnAllocation(allocator->pUserData, size, _Alignof(uint32_t), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE)
                  : malloc(size);
    if (!families) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
        uint32_t family = ci->pQueueCreateInfos[i].queueFamilyIndex;
        bool seen = false;
        for (uint32_t j = 0; j < cursor; ++j) seen |= families[j] == family;
        if (!seen) families[cursor++] = family;
    }
    dev->queue_family_count = count;
    dev->queue_families = families;
    if (allocator) {
        dev->queue_families_have_allocator = true;
        dev->queue_families_allocator = *allocator;
    }
    return VK_SUCCESS;
}

void vkrt_device_queue_families_finish(struct vkrt_device_data *dev) {
    if (dev->queue_families) {
        if (dev->queue_families_have_allocator) {
            const VkAllocationCallbacks *allocator = &dev->queue_families_allocator;
            allocator->pfnFree(allocator->pUserData, dev->queue_families);
        } else {
            free(dev->queue_families);
        }
    }
    dev->queue_families = NULL;
    dev->queue_family_count = 0;
    dev->queue_families_have_allocator = false;
}

static const VkAllocationCallbacks *as_allocator(const struct vkrt_virtual_as *as) {
    return as->has_allocator ? &as->allocator : NULL;
}

static void free_virtual_as(struct vkrt_device_data *dev, struct vkrt_virtual_as *as) {
    const VkAllocationCallbacks *allocator = as_allocator(as);
    if (as->address_cell_mapping) dev->vtable.UnmapMemory(dev->device, as->address_cell_memory);
    if (as->address_cell_buffer) dev->vtable.DestroyBuffer(dev->device, as->address_cell_buffer, allocator);
    if (as->address_cell_memory) dev->vtable.FreeMemory(dev->device, as->address_cell_memory, allocator);
#ifndef _WIN32
    pthread_mutex_destroy(&as->address_lock);
#endif
    if (allocator) {
        /* The callbacks themselves live in the record being freed. */
        VkAllocationCallbacks copy = *allocator;
        copy.pfnFree(copy.pUserData, as);
    } else {
        free(as);
    }
}

static VkResult flush_address_cell(struct vkrt_device_data *dev, const struct vkrt_virtual_as *as) {
    if (as->address_cell_coherent) return VK_SUCCESS;
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = as->address_cell_memory, .offset = 0, .size = VK_WHOLE_SIZE};
    return dev->vtable.FlushMappedMemoryRanges(dev->device, 1, &range);
}

static VkResult create_address_cell(struct vkrt_device_data *dev, struct vkrt_virtual_as *as) {
    if (!dev->vtable.CreateBuffer || !dev->vtable.GetBufferMemoryRequirements || !dev->vtable.AllocateMemory ||
        !dev->vtable.BindBufferMemory || !dev->vtable.MapMemory || !dev->vtable.UnmapMemory || !dev->vtable.DestroyBuffer ||
        !dev->vtable.FreeMemory || !dev->instance_data || !dev->instance_data->vtable.GetPhysicalDeviceMemoryProperties)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    const VkAllocationCallbacks *allocator = as_allocator(as);
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = sizeof(VkDeviceAddress),
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (dev->queue_family_count > 1) {
        /* The application cannot transfer ownership of this hidden buffer.
         * Concurrent sharing makes every enabled family independently legal. */
        buffer.sharingMode = VK_SHARING_MODE_CONCURRENT;
        buffer.queueFamilyIndexCount = dev->queue_family_count;
        buffer.pQueueFamilyIndices = dev->queue_families;
    }
    VkBuffer cell_buffer = VK_NULL_HANDLE;
    VkResult result = dev->vtable.CreateBuffer(dev->device, &buffer, allocator, &cell_buffer);
    if (result != VK_SUCCESS) return result;
    as->address_cell_buffer = cell_buffer;

    VkMemoryRequirements requirements;
    dev->vtable.GetBufferMemoryRequirements(dev->device, as->address_cell_buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    dev->instance_data->vtable.GetPhysicalDeviceMemoryProperties(dev->physical_device, &properties);
    uint32_t type = UINT32_MAX;
    /* Prefer coherent memory; whole-allocation flushes avoid assumptions
     * about the device's nonCoherentAtomSize. */
    for (unsigned pass = 0; pass < 2 && type == UINT32_MAX; ++pass) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
            if (!(requirements.memoryTypeBits & (1u << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
            if (pass == 0 && !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
            if (!(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && !dev->vtable.FlushMappedMemoryRanges) continue;
            type = i;
            as->address_cell_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            break;
        }
    }
    if (type == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;

    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type};
    VkDeviceMemory cell_memory = VK_NULL_HANDLE;
    result = dev->vtable.AllocateMemory(dev->device, &allocation, allocator, &cell_memory);
    if (result != VK_SUCCESS) return result;
    as->address_cell_memory = cell_memory;
    result = dev->vtable.BindBufferMemory(dev->device, as->address_cell_buffer, as->address_cell_memory, 0);
    if (result != VK_SUCCESS) return result;
    void *mapping = NULL;
    result = dev->vtable.MapMemory(dev->device, as->address_cell_memory, 0, VK_WHOLE_SIZE, 0, &mapping);
    if (result != VK_SUCCESS) return result;
    as->address_cell_mapping = mapping;
    memset(as->address_cell_mapping, 0, sizeof(VkDeviceAddress));
    return flush_address_cell(dev, as);
}

VkResult vkrt_create_virtual_as(struct vkrt_device_data *dev, const VkAccelerationStructureCreateInfoKHR *ci,
                                const VkAllocationCallbacks *allocator, VkAccelerationStructureKHR *out_as) {
    *out_as = VK_NULL_HANDLE;
    struct vkrt_virtual_as *as = allocator
                                     ? allocator->pfnAllocation(allocator->pUserData, sizeof(*as), _Alignof(struct vkrt_virtual_as),
                                                                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT)
                                     : malloc(sizeof(*as));
    if (!as) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(as, 0, sizeof(*as));
    vkrt_mutex_init(&as->address_lock);
    if (allocator) {
        as->has_allocator = true;
        as->allocator = *allocator;
    }
    as->type = ci->type;
    as->owner = dev->device;
    as->backing_buffer = ci->buffer;
    as->offset = ci->offset;
    as->size = ci->size;

    /* Allocate here so failures can be returned. The AS backing buffer need
     * not be bound at creation: do not query its device address. */
    VkResult result = create_address_cell(dev, as);
    if (result != VK_SUCCESS) {
        free_virtual_as(dev, as);
        return result;
    }
    VkAccelerationStructureKHR token = (VkAccelerationStructureKHR)(uintptr_t)as;
    vkrt_map_object(VKRT_HKEY(token), as);
    if (VKRT_FIND(struct vkrt_virtual_as, token) != as) {
        free_virtual_as(dev, as);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *out_as = token;
    return VK_SUCCESS;
}

void vkrt_destroy_virtual_as(struct vkrt_device_data *dev, VkAccelerationStructureKHR handle) {
    if (handle == VK_NULL_HANDLE) return;
    struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, handle);
    if (!as || as->owner != dev->device) {
        VKRT_LOG_WARN("destroy_virtual_as: unknown or foreign handle %p", (void *)handle);
        return;
    }
    /* The application must complete all uses, including GPU descriptor
     * reads, before destroying the AS. */
    vkrt_unmap_object(VKRT_HKEY(handle));
    free_virtual_as(dev, as);
}

void vkrt_as_build_sizes(struct vkrt_device_data *dev, const VkAccelerationStructureBuildGeometryInfoKHR *pBuildInfo,
                         const uint32_t *pMaxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR *pSizeInfo) {
    struct vkrt_build_layout l = vkrt_build_layout(pBuildInfo, pMaxPrimitiveCounts, NULL);
    pSizeInfo->accelerationStructureSize = l.as_size;
    pSizeInfo->buildScratchSize = l.scratch_size;
    pSizeInfo->updateScratchSize = 0; /* refit is explicitly unsupported */
}

VkDeviceAddress vkrt_as_device_address(struct vkrt_device_data *dev, VkAccelerationStructureKHR handle) {
    struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, handle);
    if (!as || as->owner != dev->device) {
        VKRT_LOG_WARN("as_device_address: unknown or foreign handle %p", (void *)handle);
        return 0;
    }

    /* Only valid GetASAddress/build calls resolve this address, after backing
     * memory is bound. Descriptor updates may precede binding and just use the
     * zero-initialized cell. Valid nonsparse buffer bindings are immutable,
     * so later descriptor updates never rewrite a cell in use by the GPU. */
    vkrt_mutex_lock(&as->address_lock);
    VkDeviceAddress address = as->header_address;
    if (!address && dev->vtable.GetBufferDeviceAddress && as->backing_buffer) {
        VkBufferDeviceAddressInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = as->backing_buffer};
        VkDeviceAddress base = dev->vtable.GetBufferDeviceAddress(dev->device, &info);
        if (base && as->offset <= UINT64_MAX - base) {
            address = base + as->offset;
            memcpy(as->address_cell_mapping, &address, sizeof(address));
            VkResult result = flush_address_cell(dev, as);
            if (result == VK_SUCCESS) {
                as->header_address = address;
            } else {
                VKRT_LOG_ERR("as_device_address: address-cell flush failed (%d)", result);
                address = 0;
            }
        }
    }
    vkrt_mutex_unlock(&as->address_lock);
    return address;
}
