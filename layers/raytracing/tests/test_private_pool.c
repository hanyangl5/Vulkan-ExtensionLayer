/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_layer.h"
#include "vkrt_private_pool.h"
#include "vkrt_memory_allocations.h"
#include <stdatomic.h>

/* The native boundary exposes independently chosen BDA/offset alignments and
 * memory types. No Vulkan device or driver is required for this allocator test. */
#define H(type, value) ((type)(uintptr_t)(value))
static _Atomic unsigned failures;
#define CHECK(test)                                                 \
    do {                                                            \
        if (!(test)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); \
            atomic_fetch_add(&failures, 1);                         \
        }                                                           \
    } while (0)

static const VkDevice device = H(VkDevice, 1);
static const VkPhysicalDevice physical = H(VkPhysicalDevice, 2);
static struct vkrt_device_data dev;
static struct vkrt_instance_data inst;
static const uint32_t families[] = {0, 2};
static enum failure_stage {
    NONE,
    BUFFER,
    REQUIREMENTS,
    MEMORY_DEVICE,
    MEMORY_HOST,
    BIND,
    MAP,
    MAP_NULL,
    ADDRESS_ZERO,
    ADDRESS_OVERFLOW
} fail_stage;
static uint32_t type_bits, fail_device_types;
static VkDeviceSize descriptor_alignment;
static uint32_t native_api_version;
static VkDeviceSize max_buffer_size, max_allocation_size, requirements_extra;
static bool extension3, extension4, ignore_limits;
static VkResult extension_count_result, extension_fill_result;
static unsigned extension_incomplete, extension_calls, properties2_calls, queries3, queries4;
static VkDeviceAddress address_low;
static unsigned buffer_calls, buffer_success, buffer_destroys, memory_calls, memory_success, memory_frees, maps, unmaps;
static uint32_t allocation_limit = 4096;
static unsigned chosen_types[64], chosen_count, next_id;
struct native_chunk {
    struct native_chunk *next;
    VkDeviceSize size;
    uint8_t *bytes;
    bool buffer_live, memory_live, mapped;
    uint32_t id;
};
static struct native_chunk *native_chunks;
static struct {
    _Atomic unsigned calls, allocations, frees, live;
    unsigned fail_on;
} owner;
struct host_header {
    void *owner;
    uint64_t reserved;
};
static VkAllocationCallbacks allocator;

static VKAPI_ATTR void *VKAPI_CALL host_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    CHECK(user == &owner && alignment <= 16 && scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    unsigned call = atomic_fetch_add(&owner.calls, 1) + 1;
    if (call == owner.fail_on) return NULL;
    struct host_header *p = malloc(sizeof(*p) + size);
    if (!p) return NULL;
    p->owner = user;
    atomic_fetch_add(&owner.allocations, 1);
    atomic_fetch_add(&owner.live, 1);
    return p + 1;
}
static VKAPI_ATTR void VKAPI_CALL host_free(void *user, void *pointer) {
    if (!pointer) return;
    struct host_header *p = (struct host_header *)pointer - 1;
    CHECK(user == &owner && p->owner == user && atomic_load(&owner.live));
    atomic_fetch_add(&owner.frees, 1);
    atomic_fetch_sub(&owner.live, 1);
    free(p);
}
static void check_allocator(const VkAllocationCallbacks *a) {
    CHECK(a && a->pUserData == &owner && a->pfnAllocation == host_allocate && a->pfnFree == host_free);
}
static struct native_chunk *cell(uintptr_t handle) {
    CHECK(handle && handle != 0xbad);
    return (struct native_chunk *)handle;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice d, const VkBufferCreateInfo *info, const VkAllocationCallbacks *a,
                                                    VkBuffer *buffer) {
    check_allocator(a);
    CHECK(d == device && info->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO && !info->pNext);
    CHECK(info->size >= 8 && !(info->size & 7));
    if (!ignore_limits && (native_api_version >= VK_API_VERSION_1_3 || extension4) && max_buffer_size)
        CHECK(info->size <= max_buffer_size);
    CHECK(info->usage ==
          (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    CHECK(info->sharingMode == VK_SHARING_MODE_CONCURRENT && info->queueFamilyIndexCount == 2 &&
          info->pQueueFamilyIndices == families);
    ++buffer_calls;
    *buffer = H(VkBuffer, 0xbad);
    if (fail_stage == BUFFER) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct native_chunk *p = calloc(1, sizeof(*p));
    CHECK(p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->id = ++next_id;
    p->buffer_live = true;
    p->size = info->size;
    p->next = native_chunks;
    native_chunks = p;
    *buffer = H(VkBuffer, p);
    ++buffer_success;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL requirements(VkDevice d, VkBuffer buffer, VkMemoryRequirements *out) {
    struct native_chunk *p = cell((uintptr_t)buffer);
    CHECK(d == device && p->buffer_live);
    *out = (VkMemoryRequirements){
        .size = fail_stage == REQUIREMENTS ? p->size - 1 : ((p->size + 255) & ~UINT64_C(255)) + requirements_extra,
        .alignment = 64,
        .memoryTypeBits = type_bits};
}
static VKAPI_ATTR void VKAPI_CALL memory_properties(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties *out) {
    CHECK(p == physical);
    memset(out, 0, sizeof(*out));
    out->memoryTypeCount = 6;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    out->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    out->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    out->memoryTypes[3].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    out->memoryTypes[4].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT;
    out->memoryTypes[5].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice p, VkPhysicalDeviceProperties *out) {
    CHECK(p == physical);
    memset(out, 0, sizeof(*out));
    out->apiVersion = native_api_version;
    out->limits.minStorageBufferOffsetAlignment = descriptor_alignment;
    out->limits.maxMemoryAllocationCount = allocation_limit;
}
static VKAPI_ATTR void VKAPI_CALL properties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *out) {
    CHECK(p == physical && out->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    ++properties2_calls;
    properties(p, &out->properties);
    for (VkBaseOutStructure *next = out->pNext; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES) {
            CHECK(native_api_version >= VK_API_VERSION_1_1 || extension3);
            ++queries3;
            if (!ignore_limits) ((VkPhysicalDeviceMaintenance3Properties *)next)->maxMemoryAllocationSize = max_allocation_size;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES) {
            CHECK(native_api_version >= VK_API_VERSION_1_3 || extension4);
            ++queries4;
            if (!ignore_limits) ((VkPhysicalDeviceMaintenance4Properties *)next)->maxBufferSize = max_buffer_size;
        } else
            CHECK(false);
    }
}
static VKAPI_ATTR VkResult VKAPI_CALL extensions(VkPhysicalDevice p, const char *layer, uint32_t *count,
                                                 VkExtensionProperties *out) {
    CHECK(p == physical && !layer);
    ++extension_calls;
    if (!out) {
        *count = (unsigned)extension3 + (unsigned)extension4;
        return extension_count_result;
    }
    if (extension_fill_result != VK_SUCCESS) return extension_fill_result;
    if (extension_incomplete) {
        --extension_incomplete;
        return VK_INCOMPLETE;
    }
    unsigned index = 0;
    if (extension3) strcpy(out[index++].extensionName, VK_KHR_MAINTENANCE_3_EXTENSION_NAME);
    if (extension4) strcpy(out[index++].extensionName, VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    CHECK(index == *count);
    *count = index;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate_memory(VkDevice d, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *a,
                                                      VkDeviceMemory *memory) {
    check_allocator(a);
    const VkMemoryAllocateFlagsInfo *flags = info->pNext;
    CHECK(d == device && info->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO && flags &&
          flags->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO && flags->flags == VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);
    const VkMemoryDedicatedAllocateInfo *dedicated = flags->pNext;
    CHECK(dedicated && dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO && !dedicated->image);
    struct native_chunk *p = cell((uintptr_t)dedicated->buffer);
    CHECK(p->buffer_live && !p->memory_live && info->allocationSize == ((p->size + 255) & ~UINT64_C(255)) + requirements_extra);
    if (!ignore_limits && (native_api_version >= VK_API_VERSION_1_1 || extension3) && max_allocation_size)
        CHECK(info->allocationSize <= max_allocation_size);
    CHECK(info->memoryTypeIndex < 4 && (type_bits & (1u << info->memoryTypeIndex)));
    ++memory_calls;
    CHECK(memory_success - memory_frees < allocation_limit);
    if (chosen_count < VKRT_ARRAY_SIZE(chosen_types)) chosen_types[chosen_count++] = info->memoryTypeIndex;
    *memory = H(VkDeviceMemory, 0xbad);
    if (fail_stage == MEMORY_HOST) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (fail_stage == MEMORY_DEVICE || (fail_device_types & (1u << info->memoryTypeIndex))) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    p->bytes = malloc((size_t)info->allocationSize);
    if (!p->bytes) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(p->bytes, 0xcd, (size_t)info->allocationSize);
    p->memory_live = true;
    *memory = H(VkDeviceMemory, p);
    ++memory_success;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL bind_memory(VkDevice d, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    struct native_chunk *p = cell((uintptr_t)buffer);
    CHECK(d == device && offset == 0 && p == cell((uintptr_t)memory) && p->buffer_live && p->memory_live);
    return fail_stage == BIND ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL map_memory(VkDevice d, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                                                 VkMemoryMapFlags flags, void **pointer) {
    struct native_chunk *p = cell((uintptr_t)memory);
    CHECK(d == device && offset == 0 && size == VK_WHOLE_SIZE && !flags && p->memory_live && !p->mapped);
    ++maps;
    *pointer = (void *)(uintptr_t)0xbad;
    if (fail_stage == MAP) return VK_ERROR_MEMORY_MAP_FAILED;
    p->mapped = true;
    *pointer = fail_stage == MAP_NULL ? NULL : p->bytes;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL unmap_memory(VkDevice d, VkDeviceMemory memory) {
    struct native_chunk *p = cell((uintptr_t)memory);
    CHECK(d == device && p->memory_live && p->mapped);
    p->mapped = false;
    ++unmaps;
}
static VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice d, VkBuffer buffer, const VkAllocationCallbacks *a) {
    check_allocator(a);
    struct native_chunk *p = cell((uintptr_t)buffer);
    CHECK(d == device && p->buffer_live && !p->mapped);
    p->buffer_live = false;
    ++buffer_destroys;
}
static VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks *a) {
    check_allocator(a);
    struct native_chunk *p = cell((uintptr_t)memory);
    CHECK(d == device && p->memory_live && !p->buffer_live && !p->mapped);
    p->memory_live = false;
    free(p->bytes);
    p->bytes = NULL;
    ++memory_frees;
}
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL buffer_address(VkDevice d, const VkBufferDeviceAddressInfo *info) {
    struct native_chunk *p = cell((uintptr_t)info->buffer);
    CHECK(d == device && p->buffer_live && p->memory_live && info->sType == VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
    if (fail_stage == ADDRESS_ZERO) return 0;
    if (fail_stage == ADDRESS_OVERFLOW) return UINT64_MAX - 127;
    return UINT64_C(0x800000000) + (uint64_t)p->id * UINT64_C(0x100000) + address_low;
}
static bool empty_slice(const struct vkrt_private_slice *s) {
    return !s->buffer && !s->offset && !s->size && !s->address && !s->mapping && !s->allocation;
}
static void setup(void) {
    CHECK(!dev.private_pool && atomic_load(&owner.live) == 0);
    while (native_chunks) {
        struct native_chunk *p = native_chunks;
        CHECK(!p->buffer_live && !p->memory_live && !p->mapped && !p->bytes);
        native_chunks = p->next;
        free(p);
    }
    memset(&dev, 0, sizeof(dev));
    memset(&inst, 0, sizeof(inst));
    atomic_store(&owner.calls, 0);
    atomic_store(&owner.allocations, 0);
    atomic_store(&owner.frees, 0);
    atomic_store(&owner.live, 0);
    owner.fail_on = 0;
    fail_stage = NONE;
    type_bits = 0x3f;
    fail_device_types = 0;
    descriptor_alignment = 256;
    native_api_version = VK_API_VERSION_1_3;
    max_buffer_size = max_allocation_size = requirements_extra = 0;
    extension3 = extension4 = ignore_limits = false;
    extension_count_result = extension_fill_result = VK_SUCCESS;
    extension_incomplete = extension_calls = properties2_calls = queries3 = queries4 = 0;
    address_low = 3;
    buffer_calls = buffer_success = buffer_destroys = memory_calls = memory_success = memory_frees = maps = unmaps = 0;
    chosen_count = next_id = 0;
    allocator = (VkAllocationCallbacks){.pUserData = &owner, .pfnAllocation = host_allocate, .pfnFree = host_free};
    dev.device = device;
    dev.physical_device = physical;
    dev.instance_data = &inst;
    dev.queue_family_count = VKRT_ARRAY_SIZE(families);
    dev.queue_families = (uint32_t *)families;
    inst.vtable.GetPhysicalDeviceMemoryProperties = memory_properties;
    inst.vtable.GetPhysicalDeviceProperties = properties;
    inst.vtable.GetPhysicalDeviceProperties2 = properties2;
    inst.vtable.EnumerateDeviceExtensionProperties = extensions;
    dev.vtable.CreateBuffer = create_buffer;
    dev.vtable.DestroyBuffer = destroy_buffer;
    dev.vtable.GetBufferMemoryRequirements = requirements;
    dev.vtable.AllocateMemory = allocate_memory;
    dev.vtable.FreeMemory = free_memory;
    dev.vtable.BindBufferMemory = bind_memory;
    dev.vtable.MapMemory = map_memory;
    dev.vtable.UnmapMemory = unmap_memory;
    dev.vtable.GetBufferDeviceAddress = buffer_address;
}
static void finish(void) {
    vkrt_private_pool_finish(&dev);
    vkrt_memory_allocations_finish(&dev);
    CHECK(!dev.private_pool && atomic_load(&owner.live) == 0);
    CHECK(atomic_load(&owner.allocations) == atomic_load(&owner.frees));
    CHECK(buffer_success == buffer_destroys && memory_success == memory_frees);
    for (struct native_chunk *p = native_chunks; p; p = p->next)
        CHECK(!p->buffer_live && !p->memory_live && !p->mapped && !p->bytes);
}
static void initialize(void) { CHECK(vkrt_private_pool_init(&dev, &allocator) == VK_SUCCESS); }
static void shared_count_limit(void) {
    setup();
    allocation_limit = 1; /* Unit-injected native cap, not a reported Vulkan profile. */
    dev.virtual_rt = true;
    CHECK(vkrt_memory_allocations_init(&dev, &allocator) == VK_SUCCESS);
    initialize();
    struct vkrt_private_slice a, b, c;
    CHECK(vkrt_private_pool_allocate(&dev, 32, false, &a) == VK_SUCCESS);
    unsigned calls = memory_calls;
    CHECK(vkrt_private_pool_allocate(&dev, 64, false, &b) == VK_SUCCESS);
    CHECK(a.buffer == b.buffer && memory_calls == calls);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &c) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(memory_calls == calls && memory_success == 1 && memory_frees == 0);
    vkrt_private_pool_release(&dev, &a);
    CHECK(vkrt_private_pool_allocate(&dev, 16, false, &c) == VK_SUCCESS);
    CHECK(c.buffer == b.buffer && memory_calls == calls);
    vkrt_private_pool_release(&dev, &b);
    vkrt_private_pool_release(&dev, &c);
    CHECK(memory_frees == 1);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &c) == VK_SUCCESS);
    vkrt_private_pool_release(&dev, &c);
    finish();
    allocation_limit = 4096;
}
static void shared_count_cleanup(void) {
    static const struct {
        enum failure_stage stage;
        VkResult result;
    } cases[] = {{MEMORY_DEVICE, VK_ERROR_OUT_OF_DEVICE_MEMORY},
                 {MEMORY_HOST, VK_ERROR_OUT_OF_HOST_MEMORY},
                 {BIND, VK_ERROR_OUT_OF_DEVICE_MEMORY},
                 {MAP, VK_ERROR_MEMORY_MAP_FAILED},
                 {MAP_NULL, VK_ERROR_INITIALIZATION_FAILED},
                 {ADDRESS_ZERO, VK_ERROR_INITIALIZATION_FAILED},
                 {ADDRESS_OVERFLOW, VK_ERROR_INITIALIZATION_FAILED},
                 {NONE, VK_ERROR_OUT_OF_HOST_MEMORY}};
    allocation_limit = 1;
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(cases); ++i) {
        setup();
        dev.virtual_rt = true;
        CHECK(vkrt_memory_allocations_init(&dev, &allocator) == VK_SUCCESS);
        initialize();
        fail_stage = cases[i].stage;
        if (fail_stage == NONE) owner.fail_on = atomic_load(&owner.calls) + 3; /* Ledger record after slice/chunk. */
        struct vkrt_private_slice a;
        CHECK(vkrt_private_pool_allocate(&dev, 32, false, &a) == cases[i].result);
        CHECK(empty_slice(&a) && memory_success == memory_frees);
        owner.fail_on = 0;
        fail_stage = NONE;
        /* A failed native allocation or post-allocation setup must leave the
         * sole actual slot available to a subsequent successful chunk. */
        CHECK(vkrt_private_pool_allocate(&dev, 32, false, &a) == VK_SUCCESS);
        vkrt_private_pool_release(&dev, &a);
        finish();
    }
    allocation_limit = 4096;
}
static void native_size_limits(void) {
    for (unsigned class_index = 0; class_index < 2; ++class_index) {
        enum vkrt_private_memory_class memory_class =
            class_index ? VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED : VKRT_PRIVATE_HOST_COHERENT;
        setup();
        max_buffer_size = max_allocation_size = 65536;
        initialize();
        CHECK(queries3 == 1 && queries4 == 1 && !extension_calls);
        struct vkrt_private_slice a;
        CHECK(vkrt_private_pool_allocate_ex(&dev, 65521, memory_class, 16, 1, &a) == VK_SUCCESS);
        CHECK(buffer_calls == 1 && memory_calls == 1 && a.offset == 13 && a.size == 65521);
        vkrt_private_pool_release(&dev, &a);
        memset(&a, 0xcd, sizeof(a));
        CHECK(vkrt_private_pool_allocate_ex(&dev, 65522, memory_class, 16, 1, &a) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        CHECK(empty_slice(&a) && buffer_calls == 1 && memory_calls == 1 && properties2_calls == 1);
        finish();

        setup();
        max_buffer_size = 64;
        max_allocation_size = 256;
        initialize();
        /* The 64KiB preference must not become an artificial support floor. */
        CHECK(vkrt_private_pool_allocate_ex(&dev, 32, memory_class, 16, 1, &a) == VK_SUCCESS);
        CHECK(cell((uintptr_t)a.buffer)->size == 48 && a.offset == 13 && memory_calls == 1);
        vkrt_private_pool_release(&dev, &a);
        CHECK(vkrt_private_pool_allocate_ex(&dev, 50, memory_class, 16, 1, &a) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        CHECK(empty_slice(&a) && buffer_calls == 1);
        finish();

        setup();
        max_buffer_size = 65536;
        max_allocation_size = 65536;
        requirements_extra = 256;
        initialize();
        CHECK(vkrt_private_pool_allocate_ex(&dev, 65521, memory_class, 16, 1, &a) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        CHECK(empty_slice(&a) && buffer_calls == 1 && buffer_destroys == 1 && !memory_calls);
        finish();

        setup();
        max_buffer_size = 65536;
        max_allocation_size = 65792;
        requirements_extra = 256;
        initialize();
        CHECK(vkrt_private_pool_allocate_ex(&dev, 65521, memory_class, 16, 1, &a) == VK_SUCCESS);
        CHECK(memory_calls == 1);
        vkrt_private_pool_release(&dev, &a);
        finish();

        setup();
        max_allocation_size = 65536;
        initialize();
        CHECK(vkrt_private_pool_allocate_ex(&dev, 65522, memory_class, 16, 1, &a) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        CHECK(empty_slice(&a) && !buffer_calls && !memory_calls);
        finish();
    }
    setup();
    max_buffer_size = max_allocation_size = (UINT64_C(1) << 32) + 65536;
    initialize();
    fail_stage = BUFFER; /* Prove full-width limits without a multi-GiB host allocation. */
    struct vkrt_private_slice a;
    CHECK(vkrt_private_pool_allocate_ex(&dev, UINT64_C(1) << 32, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) ==
          VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(empty_slice(&a) && buffer_calls == 1);
    CHECK(vkrt_private_pool_allocate_ex(&dev, max_buffer_size, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) ==
          VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(empty_slice(&a) && buffer_calls == 1);
    finish();
}
static void native_limit_queries(void) {
    struct vkrt_private_slice a;
    setup();
    native_api_version = VK_API_VERSION_1_2;
    max_buffer_size = 1; /* Unsupported maintenance4 must not fabricate this limit. */
    max_allocation_size = 65536;
    initialize();
    CHECK(queries3 == 1 && !queries4 && extension_calls == 1);
    CHECK(vkrt_private_pool_allocate(&dev, 32, false, &a) == VK_SUCCESS);
    vkrt_private_pool_release(&dev, &a);
    finish();

    setup();
    native_api_version = VK_API_VERSION_1_2;
    extension4 = true;
    max_buffer_size = 65536;
    max_allocation_size = 65536;
    extension_incomplete = 1;
    initialize();
    CHECK(queries3 == 1 && queries4 == 1 && extension_calls == 4);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 65522, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) ==
          VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(empty_slice(&a) && !buffer_calls);
    finish();

    setup();
    native_api_version = VK_API_VERSION_1_0;
    extension3 = extension4 = true;
    max_buffer_size = 64;
    max_allocation_size = 256;
    initialize();
    CHECK(queries3 == 1 && queries4 == 1 && extension_calls == 2);
    CHECK(vkrt_private_pool_allocate(&dev, 32, false, &a) == VK_SUCCESS);
    vkrt_private_pool_release(&dev, &a);
    finish();

    setup();
    native_api_version = VK_API_VERSION_1_0;
    max_buffer_size = max_allocation_size = 1;
    initialize();
    CHECK(!queries3 && !queries4 && !properties2_calls && extension_calls == 1);
    CHECK(vkrt_private_pool_allocate(&dev, 32, false, &a) == VK_SUCCESS);
    vkrt_private_pool_release(&dev, &a);
    finish();

    setup();
    ignore_limits = true;
    max_buffer_size = max_allocation_size = 1;
    initialize();
    CHECK(queries3 == 1 && queries4 == 1);
    CHECK(vkrt_private_pool_allocate(&dev, 65536, false, &a) == VK_SUCCESS);
    vkrt_private_pool_release(&dev, &a);
    finish();

    setup();
    inst.vtable.GetPhysicalDeviceProperties2 = NULL;
    initialize();
    CHECK(!queries3 && !queries4 && !extension_calls);
    CHECK(vkrt_private_pool_allocate(&dev, 32, false, &a) == VK_SUCCESS);
    vkrt_private_pool_release(&dev, &a);
    finish();

    setup();
    inst.vtable.GetPhysicalDeviceProperties = NULL;
    max_buffer_size = 65536;
    initialize();
    CHECK(properties2_calls == 2 && queries3 == 1 && queries4 == 1);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 65522, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) ==
          VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(empty_slice(&a) && !buffer_calls);
    finish();

    for (unsigned failure = 0; failure < 5; ++failure) {
        setup();
        native_api_version = VK_API_VERSION_1_2;
        extension4 = true;
        VkResult expected = VK_ERROR_OUT_OF_HOST_MEMORY;
        if (failure == 0)
            owner.fail_on = 2; /* Extension array, after the pool owner. */
        else if (failure == 1)
            extension_count_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        else if (failure == 2)
            extension_fill_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        else if (failure == 3) {
            extension_count_result = VK_ERROR_DEVICE_LOST;
            expected = VK_ERROR_DEVICE_LOST;
        } else {
            extension_incomplete = 8;
            expected = VK_ERROR_INITIALIZATION_FAILED;
        }
        CHECK(vkrt_private_pool_init(&dev, &allocator) == expected);
        CHECK(!dev.private_pool && !atomic_load(&owner.live) && !buffer_calls && !memory_calls);
        finish();
    }
}
static void alignment_and_classes(void) {
    setup();
    initialize();
    struct vkrt_private_slice a, b, c, d;
    CHECK(vkrt_private_pool_allocate(&dev, 8, false, &a) == VK_SUCCESS);
    CHECK(a.offset == 5 && !(a.address & 7) && a.mapping);
    CHECK(vkrt_private_pool_allocate(&dev, 8, true, &b) == VK_SUCCESS);
    CHECK(b.buffer == a.buffer && !(b.offset & 255) && (b.address & 7) == 3);
    vkrt_private_pool_release(&dev, &a);
    CHECK(empty_slice(&a));
    CHECK(vkrt_private_pool_allocate_ex(&dev, 16, VKRT_PRIVATE_HOST_COHERENT, 16, 1, &c) == VK_SUCCESS);
    CHECK(c.buffer == b.buffer && c.offset == 13 && !(c.address & 15) && c.mapping);
    CHECK(c.mapping == cell((uintptr_t)c.buffer)->bytes + c.offset);
    for (unsigned i = 0; i < c.size; ++i) CHECK(((const uint8_t *)c.mapping)[i] == 0);
    const uint64_t marker = UINT64_C(0x9132abcdef012345);
    memcpy(c.mapping, &marker, sizeof(marker));
    uint64_t readback;
    memcpy(&readback, c.mapping, sizeof(readback));
    CHECK(readback == marker);
    unsigned before_maps = maps;
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &d) == VK_SUCCESS);
    CHECK(d.buffer != c.buffer && !(d.address & 15) && d.offset == 13 && !d.mapping && maps == before_maps);
    CHECK(chosen_types[0] == 0 && chosen_types[1] == 1);
    for (unsigned i = 0; i < d.size; ++i)
        CHECK(cell((uintptr_t)d.buffer)->bytes[d.offset + i] == 0xcd); /* No host initialization of the GPU arena. */
    vkrt_private_pool_release(&dev, &d);
    CHECK(unmaps == 0); /* The host-class chunk remains live; device memory was never mapped. */
    vkrt_private_pool_release(&dev, &b);
    vkrt_private_pool_release(&dev, &c);
    CHECK(unmaps == 1 && buffer_destroys == 2);
    finish();
}
static void holes_and_constraints(void) {
    setup();
    initialize();
    struct vkrt_private_slice a, b, c, incompatible;
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) == VK_SUCCESS);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 17, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &b) == VK_SUCCESS);
    CHECK(a.buffer == b.buffer && a.offset + a.size <= b.offset && b.address - a.address == 32);
    VkDeviceAddress old = a.address;
    vkrt_private_pool_release(&dev, &a);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 16, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &c) == VK_SUCCESS);
    CHECK(c.address == old && c.offset + c.size <= b.offset && buffer_success == 1);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 8, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 16, &incompatible) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(empty_slice(&incompatible) && buffer_success == 2 && buffer_destroys == 1);
    vkrt_private_pool_release(&dev, &b);
    vkrt_private_pool_release(&dev, &c);
    address_low = 0;
    CHECK(vkrt_private_pool_allocate_ex(&dev, 17, VKRT_PRIVATE_HOST_COHERENT, 16, 256, &a) == VK_SUCCESS);
    CHECK(!(a.offset & 255) && !(a.address & 15));
    vkrt_private_pool_release(&dev, &a);
    address_low = 8;
    CHECK(vkrt_private_pool_allocate_ex(&dev, 17, VKRT_PRIVATE_HOST_COHERENT, 32, 8, &a) == VK_SUCCESS);
    CHECK(a.offset == 24 && !(a.offset & 7) && !(a.address & 31));
    CHECK(vkrt_private_pool_allocate_ex(&dev, 17, VKRT_PRIVATE_HOST_COHERENT, 32, 8, &b) == VK_SUCCESS);
    CHECK(b.buffer == a.buffer && !(b.offset & 7) && !(b.address & 31) && a.offset + a.size <= b.offset);
    vkrt_private_pool_release(&dev, &a);
    vkrt_private_pool_release(&dev, &b);
    finish();
}
static void memory_fallback(void) {
    setup();
    initialize();
    struct vkrt_private_slice a, b;
    fail_device_types = (1u << 1) | (1u << 3);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) == VK_SUCCESS);
    CHECK(chosen_count == 3 && chosen_types[0] == 1 && chosen_types[1] == 3 && chosen_types[2] == 0 && !a.mapping && maps == 0);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_HOST_COHERENT, 16, 1, &b) == VK_SUCCESS);
    CHECK(a.buffer != b.buffer && b.mapping && maps == 1);
    vkrt_private_pool_release(&dev, &a);
    vkrt_private_pool_release(&dev, &b);
    finish();
    setup();
    initialize();
    type_bits = 1u << 2; /* Noncoherent host memory is still usable by GPU-only storage. */
    fail_stage = MAP;    /* A GPU-only allocation must not call even a failing mapper. */
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) == VK_SUCCESS);
    CHECK(chosen_count == 1 && chosen_types[0] == 2 && !a.mapping && !maps);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_HOST_COHERENT, 16, 1, &b) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(empty_slice(&b) && !maps);
    vkrt_private_pool_release(&dev, &a);
    fail_stage = NONE;
    type_bits = (1u << 4) | (1u << 5);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(empty_slice(&a) && chosen_count == 1);
    finish();
}
static void failure_controls(void) {
    setup();
    owner.fail_on = 1;
    CHECK(vkrt_private_pool_init(&dev, &allocator) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(!dev.private_pool && !atomic_load(&owner.live));
    finish();
    for (unsigned allocation = 1; allocation <= 2; ++allocation) {
        setup();
        initialize();
        owner.fail_on = atomic_load(&owner.calls) + allocation;
        struct vkrt_private_slice a;
        CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_HOST_COHERENT, 16, 1, &a) == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(empty_slice(&a));
        finish();
    }
    static const struct {
        enum failure_stage stage;
        VkResult result;
    } failures_to_try[] = {{BUFFER, VK_ERROR_OUT_OF_DEVICE_MEMORY},
                           {REQUIREMENTS, VK_ERROR_INITIALIZATION_FAILED},
                           {MEMORY_DEVICE, VK_ERROR_OUT_OF_DEVICE_MEMORY},
                           {MEMORY_HOST, VK_ERROR_OUT_OF_HOST_MEMORY},
                           {BIND, VK_ERROR_OUT_OF_DEVICE_MEMORY},
                           {MAP, VK_ERROR_MEMORY_MAP_FAILED},
                           {MAP_NULL, VK_ERROR_INITIALIZATION_FAILED},
                           {ADDRESS_ZERO, VK_ERROR_INITIALIZATION_FAILED},
                           {ADDRESS_OVERFLOW, VK_ERROR_INITIALIZATION_FAILED}};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(failures_to_try); ++i) {
        setup();
        initialize();
        fail_stage = failures_to_try[i].stage;
        struct vkrt_private_slice a;
        memset(&a, 0xcd, sizeof(a));
        CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_HOST_COHERENT, 16, 1, &a) == failures_to_try[i].result);
        CHECK(empty_slice(&a));
        finish();
    }
    setup();
    initialize();
    struct vkrt_private_slice a;
    fail_stage = MEMORY_HOST;
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(memory_calls == 1 && empty_slice(&a));
    fail_stage = MEMORY_DEVICE;
    CHECK(vkrt_private_pool_allocate_ex(&dev, 32, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(memory_calls == 5 && empty_slice(&a)); /* Four eligible types, no protected/lazy allocation. */
    finish();
}
static void invalid_requests(void) {
    setup();
    initialize();
    struct vkrt_private_slice a;
    CHECK(vkrt_private_pool_allocate_ex(&dev, 0, VKRT_PRIVATE_HOST_COHERENT, 16, 1, &a) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(empty_slice(&a));
    CHECK(vkrt_private_pool_allocate_ex(&dev, 16, (enum vkrt_private_memory_class)2, 16, 1, &a) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 16, VKRT_PRIVATE_HOST_COHERENT, 0, 1, &a) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 16, VKRT_PRIVATE_HOST_COHERENT, 16, 0, &a) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 16, VKRT_PRIVATE_HOST_COHERENT, 3, 1, &a) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 16, VKRT_PRIVATE_HOST_COHERENT, 16, 6, &a) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkrt_private_pool_allocate_ex(NULL, 16, VKRT_PRIVATE_HOST_COHERENT, 16, 1, &a) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 16, VKRT_PRIVATE_HOST_COHERENT, 16, 1, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkrt_private_pool_allocate_ex(&dev, UINT64_MAX, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &a) ==
          VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(empty_slice(&a) && buffer_calls == 0);
    finish();
}

/* Overlapping callers must never receive overlapping live leases, regardless
 * of release order or whether their chunks are coherent or GPU-only. */
static atomic_flag tracker_lock = ATOMIC_FLAG_INIT;
static struct vkrt_private_slice tracked[16];
static void track(const struct vkrt_private_slice *s, bool add) {
    while (atomic_flag_test_and_set_explicit(&tracker_lock, memory_order_acquire)) {
    }
    unsigned free_slot = VKRT_ARRAY_SIZE(tracked), match = VKRT_ARRAY_SIZE(tracked);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(tracked); ++i) {
        if (!tracked[i].allocation && free_slot == VKRT_ARRAY_SIZE(tracked)) free_slot = i;
        if (tracked[i].allocation == s->allocation) match = i;
        if (add && tracked[i].allocation && tracked[i].buffer == s->buffer)
            CHECK(s->offset + s->size <= tracked[i].offset || tracked[i].offset + tracked[i].size <= s->offset);
    }
    if (add) {
        CHECK(free_slot < VKRT_ARRAY_SIZE(tracked));
        if (free_slot < VKRT_ARRAY_SIZE(tracked)) tracked[free_slot] = *s;
    } else {
        CHECK(match < VKRT_ARRAY_SIZE(tracked));
        if (match < VKRT_ARRAY_SIZE(tracked)) tracked[match] = (struct vkrt_private_slice){0};
    }
    atomic_flag_clear_explicit(&tracker_lock, memory_order_release);
}
#ifdef _WIN32
static DWORD WINAPI concurrent_worker(LPVOID argument) {
#else
static void *concurrent_worker(void *argument) {
#endif
    uintptr_t id = (uintptr_t)argument;
    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        struct vkrt_private_slice a, b;
        enum vkrt_private_memory_class kind =
            (iteration + id) & 1 ? VKRT_PRIVATE_HOST_COHERENT : VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED;
        CHECK(vkrt_private_pool_allocate_ex(&dev, 33 + id, kind, 16, 1, &a) == VK_SUCCESS);
        track(&a, true);
        CHECK(vkrt_private_pool_allocate_ex(&dev, 17 + id, kind, 16, 1, &b) == VK_SUCCESS);
        track(&b, true);
        CHECK(!(a.address & 15) && !(b.address & 15));
        CHECK((a.mapping != NULL) == (kind == VKRT_PRIVATE_HOST_COHERENT));
        track(&a, false);
        vkrt_private_pool_release(&dev, &a);
        track(&b, false);
        vkrt_private_pool_release(&dev, &b);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void concurrency_and_finish(void) {
    setup();
    initialize();
#ifdef _WIN32
    HANDLE threads[4];
    for (uintptr_t i = 0; i < 4; ++i) {
        threads[i] = CreateThread(NULL, 0, concurrent_worker, (void *)i, 0, NULL);
        CHECK(threads[i]);
    }
    CHECK(WaitForMultipleObjects(VKRT_ARRAY_SIZE(threads), threads, TRUE, INFINITE) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(threads); ++i) CloseHandle(threads[i]);
#else
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; ++i) CHECK(!pthread_create(&threads[i], NULL, concurrent_worker, (void *)i));
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(threads); ++i) CHECK(!pthread_join(threads[i], NULL));
#endif
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(tracked); ++i) CHECK(empty_slice(&tracked[i]));
    struct vkrt_private_slice a, b;
    CHECK(vkrt_private_pool_allocate_ex(&dev, 64, VKRT_PRIVATE_HOST_COHERENT, 16, 1, &a) == VK_SUCCESS);
    CHECK(vkrt_private_pool_allocate_ex(&dev, 64, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &b) == VK_SUCCESS);
    finish(); /* Exclusive device teardown also cleans all outstanding leases. */
}
int main(void) {
    shared_count_limit();
    shared_count_cleanup();
    native_size_limits();
    native_limit_queries();
    alignment_and_classes();
    holes_and_constraints();
    memory_fallback();
    failure_controls();
    invalid_requests();
    concurrency_and_finish();
    setup(); /* Release the native mock records after their final lifetime checks. */
    unsigned result = atomic_load(&failures);
    printf("Private pool: %s (%u failures; native limits/alignment/classes/holes/fallback/failures/1600 concurrent leases)\n",
           result ? "FAIL" : "PASS", result);
    return result ? 1 : 0;
}
