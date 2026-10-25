/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_as_registry.h"

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
#define H(type, n) ((type)(uintptr_t)(n))
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            ++failures;                                     \
        }                                                   \
    } while (0)
static unsigned failures, operations, failure_at, host_live;
static bool coherent_available = true;
struct resource {
    bool buffer, memory, mapped;
    VkDeviceSize size;
    uint8_t *bytes;
    VkDeviceAddress address;
};
static struct resource resources[128];
static uint32_t current_resource;
static const VkDevice device = H(VkDevice, 1);
static unsigned live_buffers(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < 128; ++i) n += resources[i].buffer;
    return n;
}
static bool fault(void) { return ++operations == failure_at; }
static VKAPI_ATTR void *VKAPI_CALL allocation(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    CHECK(user == &host_live && scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE && alignment && !(alignment & (alignment - 1)));
    if (fault()) return NULL;
    void *out = malloc(size);
    if (out) ++host_live;
    return out;
}
static VKAPI_ATTR void VKAPI_CALL release_allocation(void *user, void *value) {
    CHECK(user == &host_live && host_live && value);
    --host_live;
    free(value);
}
static VkAllocationCallbacks allocator = {.pUserData = &host_live, .pfnAllocation = allocation, .pfnFree = release_allocation};
static void check_allocator(const VkAllocationCallbacks *a) {
    CHECK(a && a->pUserData == &host_live && a->pfnAllocation == allocation && a->pfnFree == release_allocation);
}
static VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice d, const VkBufferCreateInfo *info, const VkAllocationCallbacks *a,
                                                    VkBuffer *out) {
    check_allocator(a);
    CHECK(d == device && info->usage == (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT));
    CHECK(info->size == 71 || info->size == 2055);
    *out = H(VkBuffer, 0xbad);
    if (fault()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    unsigned i = 0;
    while (i < 128 && (resources[i].buffer || resources[i].memory)) ++i;
    CHECK(i < 128);
    if (i == 128) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    resources[i] = (struct resource){.buffer = true, .size = info->size, .address = UINT64_C(0x100000001) + (uint64_t)i * 65536};
    current_resource = i;
    *out = H(VkBuffer, i + 1);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL requirements(VkDevice d, VkBuffer buffer, VkMemoryRequirements *out) {
    unsigned i = (unsigned)(uintptr_t)buffer - 1;
    CHECK(d == device && i < 128 && resources[i].buffer);
    *out = (VkMemoryRequirements){.size = (resources[i].size + 255) & ~UINT64_C(255), .alignment = 1, .memoryTypeBits = 3};
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties *out) {
    memset(out, 0, sizeof(*out));
    out->memoryTypeCount = 2;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    out->memoryTypes[1].propertyFlags =
        coherent_available ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0;
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate_memory(VkDevice d, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *a,
                                                      VkDeviceMemory *out) {
    check_allocator(a);
    const VkMemoryAllocateFlagsInfo *flags = info->pNext;
    CHECK(d == device && info->memoryTypeIndex == 1 && flags && flags->flags == VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);
    *out = H(VkDeviceMemory, 0xbad);
    if (fault()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct resource *r = &resources[current_resource];
    r->bytes = malloc((size_t)info->allocationSize);
    CHECK(r->bytes != NULL);
    r->memory = true;
    memset(r->bytes, 0xcd, (size_t)info->allocationSize);
    *out = H(VkDeviceMemory, current_resource + 257);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL bind_memory(VkDevice d, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    CHECK(d == device && (uintptr_t)buffer + 256 == (uintptr_t)memory && offset == 0);
    return fault() ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL map_memory(VkDevice d, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                                                 VkMemoryMapFlags flags, void **out) {
    struct resource *r = &resources[(uintptr_t)memory - 257];
    CHECK(d == device && offset == 0 && size == VK_WHOLE_SIZE && !flags);
    *out = (void *)(uintptr_t)0xbad;
    if (fault()) return VK_ERROR_MEMORY_MAP_FAILED;
    r->mapped = true;
    *out = r->bytes;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL unmap_memory(VkDevice d, VkDeviceMemory memory) {
    struct resource *r = &resources[(uintptr_t)memory - 257];
    CHECK(d == device && r->mapped);
    r->mapped = false;
}
static VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice d, VkBuffer buffer, const VkAllocationCallbacks *a) {
    check_allocator(a);
    struct resource *r = &resources[(uintptr_t)buffer - 1];
    CHECK(d == device && r->buffer && !r->mapped);
    r->buffer = false;
}
static VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks *a) {
    check_allocator(a);
    struct resource *r = &resources[(uintptr_t)memory - 257];
    CHECK(d == device && r->memory && !r->buffer && !r->mapped);
    free(r->bytes);
    r->bytes = NULL;
    r->memory = false;
}
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL address(VkDevice d, const VkBufferDeviceAddressInfo *info) {
    CHECK(d == device);
    if (fault()) return 0;
    return resources[(uintptr_t)info->buffer - 1].address;
}
static struct vkrt_instance_data instance = {.vtable.GetPhysicalDeviceMemoryProperties = properties};
static struct vkrt_device_data dev = {.device = H(VkDevice, 1),
                                      .physical_device = H(VkPhysicalDevice, 2),
                                      .instance_data = &instance,
                                      .vtable = {.CreateBuffer = create_buffer,
                                                 .DestroyBuffer = destroy_buffer,
                                                 .GetBufferMemoryRequirements = requirements,
                                                 .AllocateMemory = allocate_memory,
                                                 .FreeMemory = free_memory,
                                                 .BindBufferMemory = bind_memory,
                                                 .MapMemory = map_memory,
                                                 .UnmapMemory = unmap_memory,
                                                 .GetBufferDeviceAddress = address}};

/* Decode the actual wire bytes, independently of private registry structs. */
static const uint8_t *gpu_bytes(uint64_t address, size_t size) {
    for (unsigned i = 0; i < 128; ++i)
        if (resources[i].buffer && resources[i].mapped && address >= resources[i].address &&
            address - resources[i].address <= resources[i].size && size <= resources[i].size - (address - resources[i].address))
            return resources[i].bytes + (size_t)(address - resources[i].address);
    CHECK(false);
    return NULL;
}
static uint64_t gpu_u64(uint64_t address) {
    uint64_t v = 0;
    const void *p = gpu_bytes(address, 8);
    if (p) memcpy(&v, p, 8);
    return v;
}
static uint64_t resolve(uint64_t value) {
    CHECK(value && !(value & 255));
    uint64_t header = vkrt_as_registry_address(&dev);
    uint32_t fields[8];
    const void *p = gpu_bytes(header, 64);
    if (!p) return 0;
    memcpy(fields, p, sizeof(fields));
    CHECK(fields[0] == 0x52534156 && fields[1] == 1 && fields[2] == 3 && fields[3] == 8 && fields[4] == 7 && fields[5] == 8 &&
          !fields[6] && !fields[7]);
    CHECK(gpu_u64(header + 40) == 0 && gpu_u64(header + 48) == 0 && gpu_u64(header + 56) == 0);
    uint64_t page = gpu_u64(header + 32), key = value >> 8;
    for (unsigned level = 0; level < 7 && page; ++level) page = gpu_u64(page + ((key >> ((6 - level) * 8)) & 255) * 8);
    return page;
}
static void clean(void) {
    vkrt_as_registry_finish(&dev);
    CHECK(!dev.as_registry && !host_live && !live_buffers());
    for (unsigned i = 0; i < 128; ++i) CHECK(!resources[i].memory && !resources[i].mapped && !resources[i].bytes);
    operations = failure_at = 0;
    coherent_available = true;
}
static void begin(void) {
    clean();
    CHECK(vkrt_as_registry_init(&dev, &allocator) == VK_SUCCESS);
    CHECK(live_buffers() == 2);
}

static void namespace_and_aliases(void) {
    begin();
    const VkBuffer a = H(VkBuffer, 1001), b = H(VkBuffer, 1002), c = H(VkBuffer, 1003);
    allocator.pUserData = (void *)(uintptr_t)0xbad; /* Registry owns a callback copy. */
    const uint64_t size = UINT64_C(1) << 44, offset = UINT64_C(1) << 40;
    CHECK(vkrt_as_registry_buffer_create(&dev, a, size) == VK_SUCCESS);
    CHECK(vkrt_as_registry_buffer_create(&dev, b, 4096) == VK_SUCCESS);
    struct vkrt_as_registry_entry *first, *alias, *far_entry, *end;
    uint64_t av, aliasv, fv, endv;
    CHECK(vkrt_as_registry_acquire(&dev, a, 0, &first, &av) == VK_SUCCESS);
    unsigned pages = live_buffers();
    CHECK(pages == 8 && resolve(av) == 0);
    CHECK(vkrt_as_registry_acquire(&dev, a, 0, &alias, &aliasv) == VK_SUCCESS && alias == first && aliasv == av &&
          live_buffers() == pages);
    CHECK(vkrt_as_registry_acquire(&dev, a, offset, &far_entry, &fv) == VK_SUCCESS && fv - av == offset);
    CHECK(vkrt_as_registry_acquire(&dev, a, size, &end, &endv) == VK_SUCCESS && endv - av == size);
    uint64_t physical = UINT64_C(0x123456780010);
    unsigned before = operations;
    CHECK(vkrt_as_registry_publish(&dev, first, physical) == physical && resolve(av) == physical);
    CHECK(vkrt_as_registry_publish(&dev, alias, physical) == physical && operations == before);
    CHECK(vkrt_as_registry_publish(&dev, alias, physical + 16) == 0 && resolve(av) == physical);
    CHECK(vkrt_as_registry_publish(&dev, far_entry, physical + 4) == physical + 4 && resolve(fv) == physical + 4);
    vkrt_as_registry_release(&dev, first);
    CHECK(resolve(aliasv) == physical);
    vkrt_as_registry_release(&dev, alias);
    CHECK(resolve(av) == 0);
    vkrt_as_registry_release(&dev, far_entry);
    vkrt_as_registry_release(&dev, end);
    CHECK(live_buffers() == 2);
    CHECK(vkrt_as_registry_acquire(&dev, a, 0, &first, &aliasv) == VK_SUCCESS && aliasv == av);
    vkrt_as_registry_buffer_destroy(&dev, a); /* Keep range until this final AS ref dies. */
    CHECK(vkrt_as_registry_buffer_create(&dev, c, 256) == VK_SUCCESS);
    struct vkrt_as_registry_entry *other;
    uint64_t cv;
    CHECK(vkrt_as_registry_acquire(&dev, c, 0, &other, &cv) == VK_SUCCESS && cv != av);
    vkrt_as_registry_release(&dev, first);
    vkrt_as_registry_release(&dev, other);
    vkrt_as_registry_buffer_destroy(&dev, c);
    CHECK(vkrt_as_registry_buffer_create(&dev, c, 256) == VK_SUCCESS);
    CHECK(vkrt_as_registry_acquire(&dev, c, 0, &other, &cv) == VK_SUCCESS && cv == av);
    vkrt_as_registry_release(&dev, other);
    vkrt_as_registry_buffer_destroy(&dev, c);
    vkrt_as_registry_buffer_destroy(&dev, b);
    clean();
    allocator.pUserData = &host_live;
}
static void full_width_and_churn(void) {
    begin();
    const VkBuffer buffer = H(VkBuffer, 2001);
    const uint64_t maximum = UINT64_MAX & ~UINT64_C(255);
    CHECK(vkrt_as_registry_buffer_create(&dev, buffer, maximum - 256) == VK_SUCCESS);
    struct vkrt_as_registry_entry *entry;
    uint64_t value;
    CHECK(vkrt_as_registry_acquire(&dev, buffer, maximum - 256, &entry, &value) == VK_SUCCESS && value == maximum);
    CHECK(vkrt_as_registry_publish(&dev, entry, UINT64_C(0xabcdef123404)) == UINT64_C(0xabcdef123404));
    CHECK(resolve(value) == UINT64_C(0xabcdef123404));
    vkrt_as_registry_release(&dev, entry);
    CHECK(vkrt_as_registry_buffer_create(&dev, H(VkBuffer, 2002), 256) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    for (unsigned i = 0; i < 300; ++i) {
        CHECK(vkrt_as_registry_acquire(&dev, buffer, (uint64_t)i * UINT64_C(0x100000000), &entry, &value) == VK_SUCCESS);
        CHECK(vkrt_as_registry_publish(&dev, entry, UINT64_C(0x100000004)) != 0);
        vkrt_as_registry_release(&dev, entry);
        CHECK(live_buffers() == 2);
    }
    CHECK(vkrt_as_registry_acquire(&dev, buffer, 1, &entry, &value) == VK_ERROR_INITIALIZATION_FAILED && !entry && !value);
    vkrt_as_registry_buffer_destroy(&dev, buffer);
    CHECK(vkrt_as_registry_buffer_create(&dev, buffer, UINT64_MAX) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    clean();
}
static void fault_paths(void) {
    begin();
    CHECK(vkrt_as_registry_buffer_create(&dev, H(VkBuffer, 3001), 65536) == VK_SUCCESS);
    struct vkrt_as_registry_entry *entry;
    uint64_t value;
    CHECK(vkrt_as_registry_acquire(&dev, H(VkBuffer, 3001), 0, &entry, &value) == VK_SUCCESS);
    unsigned full_operations = operations;
    vkrt_as_registry_release(&dev, entry);
    clean();
    for (unsigned fail = 1; fail <= full_operations; ++fail) {
        failure_at = fail;
        VkResult result = vkrt_as_registry_init(&dev, &allocator);
        if (result == VK_SUCCESS) result = vkrt_as_registry_buffer_create(&dev, H(VkBuffer, 3001), 65536);
        if (result == VK_SUCCESS) {
            result = vkrt_as_registry_acquire(&dev, H(VkBuffer, 3001), 0, &entry, &value);
            if (result == VK_SUCCESS)
                vkrt_as_registry_release(&dev, entry);
            else
                CHECK(!entry && !value && live_buffers() == 2);
        }
        CHECK(result != VK_SUCCESS);
        clean();
    }
    coherent_available = false;
    CHECK(vkrt_as_registry_init(&dev, &allocator) == VK_ERROR_FEATURE_NOT_PRESENT);
    clean();
}
struct worker {
    struct vkrt_as_registry_entry *anchor;
    uint64_t value;
    bool valid;
};
#ifdef _WIN32
static DWORD WINAPI alias_worker(void *opaque)
#else
static void *alias_worker(void *opaque)
#endif
{
    struct worker *worker = opaque;
    worker->valid = true;
    for (unsigned i = 0; i < 1000; ++i) {
        struct vkrt_as_registry_entry *entry = NULL;
        uint64_t value = 0;
        if (vkrt_as_registry_acquire(&dev, H(VkBuffer, 4001), 512, &entry, &value) != VK_SUCCESS || entry != worker->anchor ||
            value != worker->value)
            worker->valid = false;
        if (vkrt_as_registry_publish(&dev, entry, UINT64_C(0x1234000010)) != UINT64_C(0x1234000010)) worker->valid = false;
        vkrt_as_registry_release(&dev, entry);
    }
    return 0;
}
static void concurrent_aliases(void) {
    begin();
    CHECK(vkrt_as_registry_buffer_create(&dev, H(VkBuffer, 4001), 65536) == VK_SUCCESS);
    struct worker workers[8];
    struct vkrt_as_registry_entry *anchor;
    uint64_t value;
    CHECK(vkrt_as_registry_acquire(&dev, H(VkBuffer, 4001), 512, &anchor, &value) == VK_SUCCESS);
    unsigned before = operations;
#ifdef _WIN32
    HANDLE threads[8];
    for (unsigned i = 0; i < 8; ++i) {
        workers[i] = (struct worker){.anchor = anchor, .value = value};
        threads[i] = CreateThread(NULL, 0, alias_worker, &workers[i], 0, NULL);
        CHECK(threads[i] != NULL);
    }
    WaitForMultipleObjects(8, threads, TRUE, INFINITE);
    for (unsigned i = 0; i < 8; ++i) CloseHandle(threads[i]);
#else
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) {
        workers[i] = (struct worker){.anchor = anchor, .value = value};
        CHECK(pthread_create(&threads[i], NULL, alias_worker, &workers[i]) == 0);
    }
    for (unsigned i = 0; i < 8; ++i) pthread_join(threads[i], NULL);
#endif
    for (unsigned i = 0; i < 8; ++i) CHECK(workers[i].valid);
    CHECK(resolve(value) == UINT64_C(0x1234000010) && operations == before && live_buffers() == 8);
    vkrt_as_registry_release(&dev, anchor);
    CHECK(live_buffers() == 2);
    clean();
}
int main(void) {
    namespace_and_aliases();
    full_width_and_churn();
    concurrent_aliases();
    fault_paths();
    printf("AS registry checks: %u failures\n", failures);
    return failures != 0;
}
