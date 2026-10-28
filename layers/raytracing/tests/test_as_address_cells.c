/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_as_registry.h"

/* Exercise the real VirtualAS and descriptor translation with an ICD boundary
 * that supports only ordinary buffers/memory and exposes no native AS path. */
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
#define HANDLE(type, value) ((type)(uintptr_t)(value))
#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            ++failures;                                          \
        }                                                        \
    } while (0)
static unsigned failures;
static const VkDevice device = HANDLE(VkDevice, 1);
static const VkBuffer backing = HANDLE(VkBuffer, 2);
static const VkDeviceAddress backing_address = UINT64_C(0x123456780010);
static const VkAllocationCallbacks *expected_allocator;
static const VkAllocationCallbacks *expected_pool_allocator;
static unsigned host_allocations, host_frees, buffer_creates, buffer_destroys, memory_allocations, memory_frees;
static unsigned maps, unmaps, flushes, address_queries, updates, native_calls;
static bool bound;
static enum { FAIL_NONE, FAIL_HOST, FAIL_BUFFER, FAIL_MEMORY, FAIL_BIND, FAIL_MAP, FAIL_FLUSH, FAIL_MEMORY_TYPE } fail_stage;
struct cell {
    bool buffer_live, memory_live, mapped;
    bool registry;
    VkDeviceSize size;
    uint8_t *bytes;
};
static struct cell cells[32];
static unsigned cell_count;
static struct vkrt_virtual_as *expected_as;
static const uint32_t *expected_family_pointer;
static uint32_t expected_families[3], expected_family_count;
static unsigned family_allocations, family_frees;
static bool fail_family_allocation;
struct pool_owner {
    unsigned allocations, frees, live;
};
struct pool_header {
    struct pool_owner *owner;
    uint64_t reserved;
};
static VKAPI_ATTR void *VKAPI_CALL pool_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    struct pool_owner *owner = user;
    CHECK(scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE && alignment <= 16);
    struct pool_header *header = malloc(sizeof(*header) + size);
    if (!header) return NULL;
    header->owner = owner;
    ++owner->allocations;
    ++owner->live;
    return header + 1;
}
static VKAPI_ATTR void VKAPI_CALL pool_free(void *user, void *pointer) {
    if (!pointer) return;
    struct pool_header *header = (struct pool_header *)pointer - 1;
    CHECK(header->owner == user && header->owner->live);
    --header->owner->live;
    ++header->owner->frees;
    free(header);
}

static VKAPI_ATTR void *VKAPI_CALL allocate_families(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    CHECK(user == &family_allocations && size == expected_family_count * sizeof(uint32_t));
    CHECK(alignment == _Alignof(uint32_t) && scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    ++family_allocations;
    return fail_family_allocation ? NULL : malloc(size);
}
static VKAPI_ATTR void VKAPI_CALL free_families(void *user, void *allocation) {
    CHECK(user == &family_allocations && allocation != NULL);
    ++family_frees;
    free(allocation);
}

static void check_allocator(const VkAllocationCallbacks *allocator) {
    CHECK((allocator == NULL) == (expected_pool_allocator == NULL));
    if (allocator && expected_pool_allocator) {
        CHECK(allocator->pUserData == expected_pool_allocator->pUserData);
        CHECK(allocator->pfnAllocation == expected_pool_allocator->pfnAllocation &&
              allocator->pfnFree == expected_pool_allocator->pfnFree);
    }
}
static struct cell *buffer_cell(VkBuffer buffer) {
    uintptr_t index = (uintptr_t)buffer - 100;
    CHECK(index < cell_count);
    return &cells[index < cell_count ? index : 0];
}
static struct cell *memory_cell(VkDeviceMemory memory) {
    uintptr_t index = (uintptr_t)memory - 200;
    CHECK(index < cell_count);
    return &cells[index < cell_count ? index : 0];
}
static VKAPI_ATTR void *VKAPI_CALL host_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    CHECK(user == &host_allocations && size == sizeof(struct vkrt_virtual_as));
    CHECK(alignment >= _Alignof(struct vkrt_virtual_as) && scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    ++host_allocations;
    return fail_stage == FAIL_HOST ? NULL : malloc(size);
}
static VKAPI_ATTR void VKAPI_CALL host_free(void *user, void *allocation) {
    CHECK(user == &host_allocations && allocation != NULL);
    ++host_frees;
    free(allocation);
}
static VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice d, const VkBufferCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    ++buffer_creates;
    check_allocator(allocator); /* Native chunks never borrow the AS object's callbacks. */
    CHECK(d == device && info->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO && !info->pNext);
    CHECK(info->size >= 65536 && !(info->size & 7));
    CHECK(info->usage == (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT));
    if (expected_family_count > 1) {
        CHECK(info->sharingMode == VK_SHARING_MODE_CONCURRENT && info->queueFamilyIndexCount == expected_family_count);
        CHECK(info->pQueueFamilyIndices == expected_family_pointer);
        CHECK(!memcmp(info->pQueueFamilyIndices, expected_families, sizeof(uint32_t) * expected_family_count));
    } else {
        CHECK(info->sharingMode == VK_SHARING_MODE_EXCLUSIVE);
        CHECK(info->queueFamilyIndexCount == 0 && info->pQueueFamilyIndices == NULL);
    }
    *buffer = HANDLE(VkBuffer, 0xbad); /* A failure output must never be destroyed. */
    if (fail_stage == FAIL_BUFFER) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    CHECK(cell_count < VKRT_ARRAY_SIZE(cells));
    cells[cell_count].buffer_live = true;
    cells[cell_count].registry = true;
    cells[cell_count].size = info->size;
    *buffer = HANDLE(VkBuffer, 100 + cell_count++);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL requirements(VkDevice d, VkBuffer buffer, VkMemoryRequirements *info) {
    CHECK(d == device && buffer_cell(buffer)->buffer_live);
    *info = (VkMemoryRequirements){.size = (buffer_cell(buffer)->size + 255) & ~UINT64_C(255),
                                   .alignment = 64,
                                   .memoryTypeBits = fail_stage == FAIL_MEMORY_TYPE ? 1 : 6};
}
static VKAPI_ATTR void VKAPI_CALL memory_properties(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties *info) {
    CHECK(physical == HANDLE(VkPhysicalDevice, 3));
    memset(info, 0, sizeof(*info));
    info->memoryTypeCount = 3;
    info->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    info->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    info->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate_memory(VkDevice d, const VkMemoryAllocateInfo *info,
                                                      const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    struct cell *cell = &cells[cell_count - 1];
    ++memory_allocations;
    check_allocator(allocator);
    const VkMemoryAllocateFlagsInfo *flags = info->pNext;
    CHECK(flags && flags->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO &&
          flags->flags == VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);
    const VkMemoryDedicatedAllocateInfo *dedicated = flags ? flags->pNext : NULL;
    CHECK(dedicated && dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO &&
          dedicated->buffer == HANDLE(VkBuffer, 100 + cell_count - 1) && !dedicated->image);
    CHECK(d == device && info->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    CHECK(info->allocationSize == ((cell->size + 255) & ~UINT64_C(255)) && info->memoryTypeIndex == 2);
    *memory = HANDLE(VkDeviceMemory, 0xbad);
    if (fail_stage == FAIL_MEMORY) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    cell->memory_live = true;
    cell->bytes = malloc((size_t)info->allocationSize);
    CHECK(cell->bytes != NULL);
    memset(cell->bytes, 0xcd, (size_t)info->allocationSize);
    *memory = HANDLE(VkDeviceMemory, 200 + cell_count - 1);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL bind_memory(VkDevice d, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    CHECK(d == device && offset == 0 && buffer_cell(buffer) == memory_cell(memory));
    CHECK(buffer_cell(buffer)->buffer_live && memory_cell(memory)->memory_live);
    return fail_stage == FAIL_BIND ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL map_memory(VkDevice d, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                                                 VkMemoryMapFlags flags, void **pointer) {
    CHECK(d == device && offset == 0 && size == VK_WHOLE_SIZE && flags == 0);
    struct cell *cell = memory_cell(memory);
    ++maps;
    CHECK(cell->memory_live && !cell->mapped);
    *pointer = (void *)(uintptr_t)0xbad;
    if (fail_stage == FAIL_MAP) return VK_ERROR_MEMORY_MAP_FAILED;
    cell->mapped = true;
    *pointer = cell->bytes;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL unmap_memory(VkDevice d, VkDeviceMemory memory) {
    struct cell *cell = memory_cell(memory);
    ++unmaps;
    CHECK(d == device && cell->memory_live && cell->mapped);
    cell->mapped = false;
}
static VKAPI_ATTR VkResult VKAPI_CALL flush_memory(VkDevice d, uint32_t count, const VkMappedMemoryRange *range) {
    ++flushes;
    CHECK(d == device && count == 1);
    CHECK(range->sType == VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE && !range->pNext);
    CHECK(range->offset == 0 && range->size == VK_WHOLE_SIZE && memory_cell(range->memory)->mapped);
    return fail_stage == FAIL_FLUSH ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice d, VkBuffer buffer, const VkAllocationCallbacks *allocator) {
    struct cell *cell = buffer_cell(buffer);
    ++buffer_destroys;
    check_allocator(allocator);
    CHECK(d == device && cell->buffer_live && !cell->mapped);
    cell->buffer_live = false;
}
static VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks *allocator) {
    struct cell *cell = memory_cell(memory);
    ++memory_frees;
    check_allocator(allocator);
    CHECK(d == device && cell->memory_live && !cell->buffer_live && !cell->mapped);
    cell->memory_live = false;
    free(cell->bytes);
    cell->bytes = NULL;
}
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL get_address(VkDevice d, const VkBufferDeviceAddressInfo *info) {
    if (info->buffer != backing) {
        CHECK(buffer_cell(info->buffer)->registry);
        return UINT64_C(0x800000003) + ((uintptr_t)info->buffer - 100) * UINT64_C(0x100000);
    }
    ++address_queries;
    CHECK(d == device && bound && info->buffer == backing && info->sType == VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
    return backing_address;
}
static VKAPI_ATTR void VKAPI_CALL update_descriptors(VkDevice d, uint32_t count, const VkWriteDescriptorSet *writes,
                                                     uint32_t copy_count, const VkCopyDescriptorSet *copies) {
    ++updates;
    CHECK(d == device && count == 1 && copy_count == 0 && copies == NULL);
    CHECK(writes->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && !writes->pNext && writes->descriptorCount == 1);
    CHECK(writes->dstBinding == 5 && writes->dstArrayElement == 2);
    CHECK(writes->pBufferInfo->buffer == expected_as->address_cell.buffer);
    CHECK(writes->pBufferInfo->offset == expected_as->address_cell.offset && writes->pBufferInfo->range == 8);
    CHECK(writes->pBufferInfo->offset && !(writes->pBufferInfo->offset & 255));
}
static VKAPI_ATTR VkResult VKAPI_CALL native_create(VkDevice d, const VkAccelerationStructureCreateInfoKHR *info,
                                                    const VkAllocationCallbacks *allocator, VkAccelerationStructureKHR *out) {
    ++native_calls;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
static VKAPI_ATTR void VKAPI_CALL native_destroy(VkDevice d, VkAccelerationStructureKHR as,
                                                 const VkAllocationCallbacks *allocator) {
    ++native_calls;
}
static struct vkrt_instance_data instance_data = {.vtable.GetPhysicalDeviceMemoryProperties = memory_properties};
static struct vkrt_device_data device_data = {.device = HANDLE(VkDevice, 1),
                                              .virtual_rt = true,
                                              .physical_device = HANDLE(VkPhysicalDevice, 3),
                                              .instance_data = &instance_data,
                                              .vtable = {.CreateBuffer = create_buffer,
                                                         .GetBufferMemoryRequirements = requirements,
                                                         .AllocateMemory = allocate_memory,
                                                         .BindBufferMemory = bind_memory,
                                                         .MapMemory = map_memory,
                                                         .UnmapMemory = unmap_memory,
                                                         .FlushMappedMemoryRanges = flush_memory,
                                                         .DestroyBuffer = destroy_buffer,
                                                         .FreeMemory = free_memory,
                                                         .GetBufferDeviceAddress = get_address,
                                                         .UpdateDescriptorSets = update_descriptors,
                                                         .CreateAccelerationStructureKHR = native_create,
                                                         .DestroyAccelerationStructureKHR = native_destroy}};
static VkAllocationCallbacks allocator = {.pUserData = &host_allocations, .pfnAllocation = host_allocate, .pfnFree = host_free};

static VkResult create_as(VkDeviceSize offset, VkAccelerationStructureKHR *handle) {
    VkAccelerationStructureCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                 .buffer = backing,
                                                 .offset = offset,
                                                 .size = 4096,
                                                 .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR};
    return vkrt_create_virtual_as(&device_data, &info, expected_allocator, handle);
}
static VkDeviceAddress cell_address(struct vkrt_virtual_as *as) {
    VkDeviceAddress address;
    memcpy(&address, as->address_cell.mapping, sizeof(address));
    return address;
}
static void write_as(VkAccelerationStructureKHR handle) {
    expected_as = VKRT_FIND(struct vkrt_virtual_as, handle);
    VkWriteDescriptorSetAccelerationStructureKHR chain = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &handle};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .pNext = &chain,
                                  .dstBinding = 5,
                                  .dstArrayElement = 2,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    vkrt_UpdateDescriptorSets(device, 1, &write, 0, NULL);
}
static void check_no_live_cells(void) {
    unsigned registry_live = 0;
    for (unsigned i = 0; i < cell_count; ++i)
        if (cells[i].registry && cells[i].buffer_live)
            ++registry_live;
        else
            CHECK(!cells[i].buffer_live && !cells[i].memory_live && !cells[i].mapped && !cells[i].bytes);
    CHECK(registry_live == (device_data.as_registry ? 1u : 0u));
    CHECK(buffer_destroys <= buffer_creates && memory_frees <= memory_allocations && native_calls == 0);
}
static void reset(void) {
    vkrt_as_registry_finish(&device_data);
    vkrt_private_pool_finish(&device_data);
    check_no_live_cells();
    CHECK(!device_data.queue_families && device_data.queue_family_count == 0);
    memset(cells, 0, sizeof(cells));
    cell_count = host_allocations = host_frees = buffer_creates = buffer_destroys = memory_allocations = memory_frees = 0;
    maps = unmaps = flushes = address_queries = updates = native_calls = 0;
    bound = false;
    fail_stage = FAIL_NONE;
    expected_allocator = &allocator;
    expected_pool_allocator = NULL;
    expected_family_pointer = NULL;
    expected_family_count = family_allocations = family_frees = 0;
    fail_family_allocation = false;
    CHECK(vkrt_as_registry_init(&device_data, NULL) == VK_SUCCESS);
    CHECK(vkrt_as_registry_buffer_create(&device_data, backing, 65536) == VK_SUCCESS);
}

struct query_thread {
    VkAccelerationStructureKHR handle;
    bool valid;
    VkDeviceAddress expected;
};
#ifdef _WIN32
static DWORD WINAPI query_address(void *opaque)
#else
static void *query_address(void *opaque)
#endif
{
    struct query_thread *query = opaque;
    query->valid = true;
    for (unsigned i = 0; i < 2000; ++i)
        if (vkrt_as_device_address(&device_data, query->handle) != query->expected) query->valid = false;
    return 0;
}
static void lifecycle(void) {
    reset();
    VkAccelerationStructureKHR handle;
    CHECK(create_as(0x600, &handle) == VK_SUCCESS);
    struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, handle);
    CHECK(as != NULL && cell_address(as) == as->public_address && (as->public_address & 255) == 0 && address_queries == 0);
    CHECK(as->address_cell.buffer != backing && as->address_cell.offset && !(as->address_cell.offset & 255));
    CHECK(host_allocations == 1 && buffer_creates == 1 && memory_allocations == 1 && maps == 1);
    CHECK(flushes == 0);
    /* Legal descriptor update before binding may neither query BDA nor publish. */
    write_as(handle);
    CHECK(updates == 1 && address_queries == 0 && cell_address(as) == as->public_address);
    bound = true;
    struct query_thread queries[8];
#ifdef _WIN32
    HANDLE threads[8];
    for (unsigned i = 0; i < 8; ++i) {
        queries[i].handle = handle;
        queries[i].expected = as->public_address;
        threads[i] = CreateThread(NULL, 0, query_address, &queries[i], 0, NULL);
        CHECK(threads[i] != NULL);
    }
    WaitForMultipleObjects(8, threads, TRUE, INFINITE);
    for (unsigned i = 0; i < 8; ++i) CloseHandle(threads[i]);
#else
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) {
        queries[i].handle = handle;
        queries[i].expected = as->public_address;
        CHECK(pthread_create(&threads[i], NULL, query_address, &queries[i]) == 0);
    }
    for (unsigned i = 0; i < 8; ++i) pthread_join(threads[i], NULL);
#endif
    for (unsigned i = 0; i < 8; ++i) CHECK(queries[i].valid);
    CHECK(address_queries == 1 && cell_address(as) == as->public_address && as->header_address == backing_address + 0x600);
    CHECK(flushes == 0);
    const struct cell *descriptor_cell = buffer_cell(as->address_cell.buffer);
    CHECK(descriptor_cell->bytes[as->address_cell.offset + 8] == 0xcd); /* No write beyond the descriptor's eight bytes. */
    write_as(handle);
    CHECK(updates == 2 && address_queries == 1 && flushes == 0);
    /* A foreign device must neither resolve nor destroy this object's cell. */
    struct vkrt_device_data foreign = {.device = HANDLE(VkDevice, 99)};
    CHECK(vkrt_as_device_address(&foreign, handle) == 0);
    vkrt_destroy_virtual_as(&foreign, handle);
    CHECK(VKRT_FIND(struct vkrt_virtual_as, handle) == as);
    vkrt_destroy_virtual_as(&device_data, handle);
    CHECK(VKRT_FIND(struct vkrt_virtual_as, handle) == NULL);
    CHECK(host_frees == 1 && !buffer_destroys && !memory_frees && !unmaps); /* Registry still owns its shared chunk. */
    check_no_live_cells();
}
static void failure_paths(void) {
    for (unsigned stage = FAIL_HOST; stage <= FAIL_MEMORY_TYPE; ++stage) {
        if (stage == FAIL_FLUSH) continue; /* Coherent pooled cells have no flush operation to fail. */
        reset();
        fail_stage = stage;
        VkResult expected = stage == FAIL_HOST          ? VK_ERROR_OUT_OF_HOST_MEMORY
                            : stage == FAIL_MAP         ? VK_ERROR_MEMORY_MAP_FAILED
                            : stage == FAIL_MEMORY_TYPE ? VK_ERROR_FEATURE_NOT_PRESENT
                                                        : VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (stage == FAIL_HOST) {
            VkAccelerationStructureKHR handle = HANDLE(VkAccelerationStructureKHR, 0xbad);
            CHECK(create_as(0, &handle) == expected && handle == VK_NULL_HANDLE && !host_frees);
        } else {
            /* Force genuine chunk growth; ordinary small AS cells correctly
             * reuse the existing allocation and need no native allocation. */
            struct vkrt_private_slice slice;
            memset(&slice, 0xcd, sizeof(slice));
            CHECK(vkrt_private_pool_allocate(&device_data, 131072, false, &slice) == expected);
            CHECK(!slice.buffer && !slice.allocation && !slice.mapping);
        }
        CHECK(address_queries == 0);
        check_no_live_cells();
        fail_stage = FAIL_NONE;
        VkAccelerationStructureKHR recovered;
        CHECK(create_as(0, &recovered) == VK_SUCCESS); /* Failure leaves the old pool/registry usable. */
        vkrt_destroy_virtual_as(&device_data, recovered);
    }
}
static void independent_objects(void) {
    reset();
    expected_allocator = NULL;
    VkAccelerationStructureKHR a, b;
    CHECK(create_as(0x600, &a) == VK_SUCCESS && create_as(0x1000, &b) == VK_SUCCESS);
    struct vkrt_virtual_as *first = VKRT_FIND(struct vkrt_virtual_as, a);
    struct vkrt_virtual_as *second = VKRT_FIND(struct vkrt_virtual_as, b);
    CHECK(first->address_cell.buffer == second->address_cell.buffer && first->address_cell.offset != second->address_cell.offset);
    bound = true;
    CHECK(vkrt_as_device_address(&device_data, b) == second->public_address);
    CHECK(cell_address(first) == first->public_address && cell_address(second) == second->public_address);
    CHECK(vkrt_as_device_address(&device_data, a) == first->public_address);
    CHECK(second->public_address - first->public_address == 0xa00);
    write_as(a);
    write_as(b);
    vkrt_destroy_virtual_as(&device_data, a);
    CHECK(vkrt_as_device_address(&device_data, b) == second->public_address);
    vkrt_destroy_virtual_as(&device_data, b);
    CHECK(address_queries == 2 && !buffer_destroys && !memory_frees && host_allocations == 0 && memory_allocations == 1);
    vkrt_destroy_virtual_as(&device_data, VK_NULL_HANDLE);
    check_no_live_cells();
}
static void publication_never_flushes(void) {
    reset();
    VkAccelerationStructureKHR handle;
    CHECK(create_as(0x600, &handle) == VK_SUCCESS);
    struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, handle);
    bound = true;
    fail_stage = FAIL_FLUSH;
    CHECK(vkrt_as_device_address(&device_data, handle) == as->public_address && as->header_address == backing_address + 0x600);
    CHECK(address_queries == 1 && flushes == 0);
    fail_stage = FAIL_NONE;
    CHECK(vkrt_as_device_address(&device_data, handle) == as->public_address);
    CHECK(as->header_address == backing_address + 0x600 && cell_address(as) == as->public_address);
    CHECK(vkrt_as_backing_address(&device_data, handle) == as->header_address);
    CHECK(address_queries == 1 && flushes == 0);
    vkrt_destroy_virtual_as(&device_data, handle);
    check_no_live_cells();
}
static void generic_values_and_capture_rejection(void) {
    reset();
    VkAccelerationStructureCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                 .buffer = backing,
                                                 .offset = 256,
                                                 .size = 4096,
                                                 .type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR};
    VkAccelerationStructureKHR a, b, alias;
    CHECK(vkrt_create_virtual_as(&device_data, &info, &allocator, &a) == VK_SUCCESS);
    CHECK(vkrt_create_virtual_as(&device_data, &info, &allocator, &alias) == VK_SUCCESS);
    struct vkrt_virtual_as *first = VKRT_FIND(struct vkrt_virtual_as, a), *other = VKRT_FIND(struct vkrt_virtual_as, alias);
    CHECK(first->address_cell.buffer == other->address_cell.buffer && first->address_cell.offset == other->address_cell.offset &&
          first->address_cell.mapping == other->address_cell.mapping);
    info.offset = 768;
    CHECK(vkrt_create_virtual_as(&device_data, &info, &allocator, &b) == VK_SUCCESS);
    bound = true;
    VkDeviceAddress av = vkrt_as_device_address(&device_data, a), bv = vkrt_as_device_address(&device_data, b);
    CHECK(av && !(av & 255) && !(bv & 255) && bv - av == 512);
    CHECK(vkrt_as_device_address(&device_data, alias) == av);
    CHECK(vkrt_as_backing_address(&device_data, a) == backing_address + 256);
    CHECK((vkrt_as_backing_address(&device_data, a) & 127) == 16);
    vkrt_destroy_virtual_as(&device_data, a);
    CHECK(vkrt_as_device_address(&device_data, alias) == av);
    CHECK(cell_address(other) == av);
    vkrt_destroy_virtual_as(&device_data, alias);
    vkrt_destroy_virtual_as(&device_data, b);
    check_no_live_cells();

    /* Capture/replay is not advertised by this backend. In particular, never
     * silently accept an external address as one of our public AS values. */
    unsigned before = buffer_creates;
    info.deviceAddress = 0x1000;
    CHECK(vkrt_create_virtual_as(&device_data, &info, &allocator, &a) == VK_ERROR_FEATURE_NOT_PRESENT && !a);
    info.deviceAddress = 0;
    info.createFlags = VK_ACCELERATION_STRUCTURE_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_KHR;
    CHECK(vkrt_create_virtual_as(&device_data, &info, &allocator, &a) == VK_ERROR_FEATURE_NOT_PRESENT && !a);
    CHECK(buffer_creates == before && host_frees == host_allocations);
    check_no_live_cells();
}
static void independent_callback_owners(void) {
    reset();
    vkrt_as_registry_finish(&device_data);
    vkrt_private_pool_finish(&device_data);
    struct pool_owner device_owner = {0}, instance_owner = {0};
    VkAllocationCallbacks device_callbacks = {.pUserData = &device_owner, .pfnAllocation = pool_allocate, .pfnFree = pool_free};
    VkAllocationCallbacks instance_callbacks = {.pUserData = &instance_owner, .pfnAllocation = pool_allocate, .pfnFree = pool_free};
    instance_data.has_allocator = true;
    instance_data.allocator = instance_callbacks;
    device_data.queue_families_have_allocator = true;
    device_data.queue_families_allocator = device_callbacks;
    expected_pool_allocator = &device_callbacks;
    for (unsigned use_instance = 0; use_instance < 2; ++use_instance) {
        if (use_instance) {
            device_data.queue_families_have_allocator = false;
            expected_pool_allocator = &instance_callbacks;
        }
        CHECK(vkrt_as_registry_init(&device_data, NULL) == VK_SUCCESS);
        CHECK(vkrt_as_registry_buffer_create(&device_data, backing, 65536) == VK_SUCCESS);
        VkAccelerationStructureKHR handle;
        unsigned objects_before = host_allocations;
        CHECK(create_as(256, &handle) == VK_SUCCESS && host_allocations == objects_before + 1);
        struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, handle);
        CHECK(cell_address(as) == as->public_address);
        CHECK(use_instance ? instance_owner.live > 0 && device_owner.live == 0 : device_owner.live > 0 && instance_owner.live == 0);
        allocator.pUserData = (void *)(uintptr_t)0xbad; /* AS callback struct can disappear independently. */
        vkrt_destroy_virtual_as(&device_data, handle);
        allocator.pUserData = &host_allocations;
        CHECK(host_allocations == host_frees);
        CHECK(use_instance ? instance_owner.live > 0 : device_owner.live > 0); /* DEVICE arena outlives AS owner. */
        vkrt_as_registry_finish(&device_data);
        vkrt_private_pool_finish(&device_data);
        CHECK(!device_owner.live && !instance_owner.live);
        CHECK(device_owner.allocations == device_owner.frees && instance_owner.allocations == instance_owner.frees);
    }
    instance_data.has_allocator = false;
    device_data.queue_families_have_allocator = false;
    expected_pool_allocator = NULL;
    check_no_live_cells();
}
static void queue_family_sharing(void) {
    reset();
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queues[5];
    const uint32_t original[] = {5, 1, 5, 9, 1};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(queues); ++i) {
        queues[i] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .flags = i > 1 ? VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT : 0,
                                              .queueFamilyIndex = original[i],
                                              .queueCount = 1,
                                              .pQueuePriorities = &priority};
    }
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = VKRT_ARRAY_SIZE(queues),
                               .pQueueCreateInfos = queues};
    VkAllocationCallbacks callbacks = {
        .pUserData = &family_allocations, .pfnAllocation = allocate_families, .pfnFree = free_families};
    expected_family_count = 3;
    expected_families[0] = 5;
    expected_families[1] = 1;
    expected_families[2] = 9;
    CHECK(vkrt_device_queue_families_init(&device_data, &info, &callbacks) == VK_SUCCESS);
    CHECK(family_allocations == 1 && device_data.queue_family_count == 3);
    expected_family_pointer = device_data.queue_families;
    CHECK(!memcmp(device_data.queue_families, expected_families, sizeof(expected_families)));
    /* Neither the caller's queue array nor callbacks need outlive creation. */
    queues[0].queueFamilyIndex = 31;
    callbacks.pUserData = (void *)(uintptr_t)0xbad;
    struct vkrt_private_slice grown;
    CHECK(vkrt_private_pool_allocate(&device_data, 131072, false, &grown) == VK_SUCCESS);
    vkrt_private_pool_release(&device_data, &grown); /* Inspect actual concurrent-sharing chunk creation. */
    VkAccelerationStructureKHR first, second;
    CHECK(create_as(0, &first) == VK_SUCCESS && create_as(256, &second) == VK_SUCCESS);
    CHECK(family_allocations == 1 && device_data.queue_families == expected_family_pointer);
    vkrt_destroy_virtual_as(&device_data, first);
    vkrt_destroy_virtual_as(&device_data, second);
    vkrt_device_queue_families_finish(&device_data);
    CHECK(family_frees == 1 && !device_data.queue_families && device_data.queue_family_count == 0);
    check_no_live_cells();

    reset();
    /* Duplicate queue infos with different creation flags still identify
     * one family and must not produce invalid CONCURRENT sharing. */
    queues[0].queueFamilyIndex = queues[1].queueFamilyIndex = 7;
    queues[0].flags = 0;
    queues[1].flags = VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
    info.queueCreateInfoCount = 2;
    expected_family_count = 1;
    CHECK(vkrt_device_queue_families_init(&device_data, &info, NULL) == VK_SUCCESS);
    CHECK(device_data.queue_family_count == 1 && device_data.queue_families[0] == 7);
    CHECK(create_as(0, &first) == VK_SUCCESS);
    vkrt_destroy_virtual_as(&device_data, first);
    vkrt_device_queue_families_finish(&device_data);
    CHECK(!device_data.queue_families && family_allocations == 0 && family_frees == 0);
    check_no_live_cells();

    reset();
    expected_family_count = 1;
    callbacks.pUserData = &family_allocations;
    fail_family_allocation = true;
    CHECK(vkrt_device_queue_families_init(&device_data, &info, &callbacks) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(family_allocations == 1 && family_frees == 0);
    CHECK(!device_data.queue_families && device_data.queue_family_count == 0 && device_data.queue_families_have_allocator);
    CHECK(device_data.queue_families_allocator.pUserData == &family_allocations);
    vkrt_device_queue_families_finish(&device_data);
    CHECK(family_frees == 0 && !device_data.queue_families_have_allocator);

    reset();
    /* maintenance9 permits zero-queue devices. Device-lifetime shader caches
     * still need the original DEVICE allocator even with no family array. */
    info.queueCreateInfoCount = 0;
    info.pQueueCreateInfos = NULL;
    callbacks.pUserData = &family_allocations;
    CHECK(vkrt_device_queue_families_init(&device_data, &info, &callbacks) == VK_SUCCESS);
    CHECK(!device_data.queue_families && !device_data.queue_family_count && device_data.queue_families_have_allocator);
    CHECK(!family_allocations && !family_frees);
    callbacks.pUserData = (void *)(uintptr_t)0xbad;
    CHECK(device_data.queue_families_allocator.pUserData == &family_allocations);
    vkrt_device_queue_families_finish(&device_data);
    CHECK(!device_data.queue_families_have_allocator && !family_allocations && !family_frees);
}
int main(void) {
    vkrt_map_object(VKRT_HKEY(device), &device_data);
    lifecycle();
    failure_paths();
    independent_objects();
    publication_never_flushes();
    generic_values_and_capture_rejection();
    independent_callback_owners();
    queue_family_sharing();
    vkrt_as_registry_finish(&device_data);
    vkrt_private_pool_finish(&device_data);
    check_no_live_cells();
    vkrt_unmap_object(VKRT_HKEY(device));
    vkrt_map_destroy(&vkrt_global.objects);
    printf("AS address-cell checks: %u failures\n", failures);
    return failures != 0;
}
