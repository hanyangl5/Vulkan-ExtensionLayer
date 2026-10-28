/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Included by the test boundary, not linked into production. Count actual
 * native allocations, including the upper layer's private buffers. Reservations
 * make concurrent allocations observable without holding a lock in the driver.
 */
struct guard_memory_record {
    VkDeviceMemory handle;
    int freeing;
    struct guard_memory_record *next;
};
struct guard_memory_device {
    uint32_t used;
    struct guard_memory_record *records;
};

static void guard_memory_properties_profile(VkPhysicalDeviceProperties *properties) {
    if (!guard_memory_count) return;
    if (properties->limits.maxMemoryAllocationCount < 4096)
        guard_fatal("memory_count_substrate", "native maxMemoryAllocationCount is below 4096");
    properties->limits.maxMemoryAllocationCount = 4096;
    guard_lock();
    ++guard_memory_properties;
    guard_unlock();
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                                           const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    PFN_vkAllocateMemory next = GUARD_NEXT(AllocateMemory, device, "AllocateMemory");
    if (!guard_memory_count) return next(device, info, allocator, memory);
    struct guard_dispatch data = guard_find(device, 1);
    if (!data.memory || !next) guard_fatal("memory_count_dispatch", "vkAllocateMemory");
    struct guard_memory_record *record = calloc(1, sizeof(*record));
    if (!record) return VK_ERROR_OUT_OF_HOST_MEMORY;
    guard_lock();
    ++guard_memory_attempts;
    int full = data.memory->used == 4096;
    if (!full) ++data.memory->used;
    guard_unlock();
    if (full) {
        free(record);
        guard_fatal("memory_count_overflow", "native vkAllocateMemory would exceed 4096 live allocations");
    }
    VkResult result = next(device, info, allocator, memory);
    guard_lock();
    if (result == VK_SUCCESS) {
        record->handle = *memory;
        record->next = data.memory->records;
        data.memory->records = record;
        ++guard_memory_allocations;
        ++guard_memory_live;
        if (guard_memory_peak < data.memory->used) guard_memory_peak = data.memory->used;
    } else {
        --data.memory->used;
        free(record);
    }
    guard_unlock();
    guard_accept();
    return result;
}

static VKAPI_ATTR void VKAPI_CALL guard_FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator) {
    PFN_vkFreeMemory next = GUARD_NEXT(FreeMemory, device, "FreeMemory");
    if (!guard_memory_count || !memory) {
        next(device, memory, allocator);
        return;
    }
    struct guard_dispatch data = guard_find(device, 1);
    if (!data.memory || !next) guard_fatal("memory_count_dispatch", "vkFreeMemory");
    guard_lock();
    struct guard_memory_record *record = data.memory->records;
    while (record && (record->handle != memory || record->freeing)) record = record->next;
    if (record) record->freeing = 1;
    guard_unlock();
    if (!record) guard_fatal("memory_count_unknown_free", "vkFreeMemory");
    next(device, memory, allocator);
    guard_lock();
    struct guard_memory_record **link = &data.memory->records;
    while (*link != record) link = &(*link)->next;
    *link = record->next;
    --data.memory->used;
    --guard_memory_live;
    ++guard_memory_frees;
    guard_unlock();
    free(record);
    guard_accept();
}

static void guard_memory_device_finish(VkDevice device) {
    if (!guard_memory_count) return;
    struct guard_dispatch data = guard_find(device, 1);
    if (!data.memory || data.memory->used || data.memory->records)
        guard_fatal("memory_count_leak", "vkDestroyDevice with live native allocations");
    free(data.memory);
}
