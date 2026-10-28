/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_as_registry.h"

/* Compile the real entry points with a deterministic prefix-allocation fault
 * injector. No Vulkan implementation or hardware RT support is needed. */
static int allocation_budget = -1;
static unsigned live_allocations, allocation_attempts;
static void *query_test_malloc(size_t size) {
    ++allocation_attempts;
    if (!allocation_budget) return NULL;
    if (allocation_budget > 0) --allocation_budget;
    void *p = malloc(size);
    if (p) ++live_allocations;
    return p;
}
static void query_test_free(void *p) {
    if (p) --live_allocations;
    free(p);
}
#define malloc query_test_malloc
#define free query_test_free
#include "vkrt_translate.c"
#include "vkrt_memory_queries.c"
#undef malloc
#undef free

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static unsigned failures, creates, queries, external_queries, handle_queries, registrations;
#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            ++failures;                                          \
        }                                                        \
    } while (0)
#define HANDLE(type, value) ((type)(uintptr_t)(value))
static const VkDevice device = HANDLE(VkDevice, 1);
static const VkPhysicalDevice physical = HANDLE(VkPhysicalDevice, 2);
static const VkBuffer native_buffer = HANDLE(VkBuffer, 3);
static const VkDeviceSize large_size = UINT64_C(0x10000000081);
static const VkBufferUsageFlags2KHR high_usage = UINT64_C(1) << 40;
static const VkBufferUsageFlags native_rt_bits = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                                 VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;

VkResult vkrt_as_registry_buffer_create(struct vkrt_device_data *dev, VkBuffer buffer, VkDeviceSize size) {
    CHECK(dev->device == device && buffer == native_buffer && size == large_size);
    ++registrations;
    return VK_SUCCESS;
}
void vkrt_as_registry_buffer_destroy(struct vkrt_device_data *dev, VkBuffer buffer) {
    (void)dev;
    (void)buffer;
}

struct opaque_node {
    VkStructureType sType;
    const void *pNext;
    uint64_t data[2];
};
static struct opaque_node suffix = {
    (VkStructureType)0x7fff0000, NULL, {UINT64_C(0x123456789abcdef0), UINT64_C(0xfedcba9876543210)}};
static struct opaque_node output_suffix = {(VkStructureType)0x7fff0001, NULL, {43, 89}};
static const struct opaque_node *unknown_input;
static VkBufferCreateInfo *original_buffer;
static VkBufferUsageFlags expected_legacy;
static VkBufferUsageFlags2KHR expected_flags2;
static bool exact_input, translated_flags2;
static const VkBaseInStructure *exhaustive_chain;
static const VkPhysicalDeviceExternalBufferInfo *original_external;
static const VkDeviceBufferMemoryRequirements *original_query;
static VkMemoryRequirements2 *original_requirements;
static VkExternalBufferProperties *original_properties;
static VkMemoryRequirements native_requirements;

static void check_chain(const void *pNext) {
    unsigned usage_nodes = 0;
    const VkBaseInStructure *original = exhaustive_chain;
    for (const VkBaseInStructure *p = pNext; p; p = p->pNext) {
        if (original && original->sType != VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR) {
            size_t size = vkrt_buffer_chain_prefix_size(original->sType);
            CHECK(size && original->sType == p->sType && original != p);
            union vkrt_buffer_chain_node expected;
            memcpy(&expected, original, size);
            ((VkBaseOutStructure *)&expected)->pNext = (void *)p->pNext;
            CHECK(!memcmp(&expected, p, size));
            original = original->pNext;
            continue;
        }
        if (p->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR) {
            const VkBufferUsageFlags2CreateInfoKHR *u = (const void *)p;
            CHECK(u->usage == expected_flags2 && !(u->usage & native_rt_bits));
            CHECK(u->pNext == &suffix);
            ++usage_nodes;
        } else if (p->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO) {
            const VkExternalMemoryBufferCreateInfo *external = (const void *)p;
            CHECK(external->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT);
            if (translated_flags2) CHECK(p != original_buffer->pNext);
        } else if (p->sType == VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO) {
            const VkBufferOpaqueCaptureAddressCreateInfo *address = (const void *)p;
            CHECK(address->opaqueCaptureAddress == UINT64_C(0x123456789000));
        } else
            CHECK(p == (const VkBaseInStructure *)&suffix || p == (const VkBaseInStructure *)unknown_input);
    }
    CHECK(usage_nodes == (expected_flags2 ? 1u : 0u));
}
static void check_create_info(const VkBufferCreateInfo *info) {
    CHECK(info->sType == original_buffer->sType && info->flags == original_buffer->flags);
    CHECK(info->size == large_size && info->usage == expected_legacy && !(info->usage & native_rt_bits));
    CHECK(info->sharingMode == original_buffer->sharingMode);
    CHECK(info->queueFamilyIndexCount == original_buffer->queueFamilyIndexCount);
    CHECK(info->pQueueFamilyIndices == original_buffer->pQueueFamilyIndices);
    if (exact_input) CHECK(info == original_buffer);
    if (!translated_flags2) CHECK(info->pNext == original_buffer->pNext);
    check_chain(info->pNext);
}
static void write_requirements(VkMemoryRequirements2 *requirements) {
    CHECK(requirements == original_requirements && requirements->sType == VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2);
    requirements->memoryRequirements = native_requirements;
    VkMemoryDedicatedRequirements *dedicated = requirements->pNext;
    CHECK(dedicated && dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
    CHECK(dedicated->pNext == &output_suffix);
    dedicated->prefersDedicatedAllocation = VK_TRUE;
    dedicated->requiresDedicatedAllocation = VK_FALSE;
}
static VKAPI_ATTR VkResult VKAPI_CALL mock_create(VkDevice d, const VkBufferCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    CHECK(d == device && allocator == NULL);
    check_create_info(info);
    *buffer = native_buffer;
    ++creates;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL mock_query(VkDevice d, const VkDeviceBufferMemoryRequirements *info,
                                             VkMemoryRequirements2 *requirements) {
    CHECK(d == device && info->sType == original_query->sType && info->pNext == original_query->pNext);
    if (exact_input) CHECK(info == original_query);
    check_create_info(info->pCreateInfo);
    write_requirements(requirements);
    ++queries;
}
static const VkBufferMemoryRequirementsInfo2 *original_handle_query;
static VKAPI_ATTR void VKAPI_CALL mock_handle_query(VkDevice d, const VkBufferMemoryRequirementsInfo2 *info,
                                                    VkMemoryRequirements2 *requirements) {
    CHECK(d == device && info == original_handle_query && info->buffer == native_buffer);
    write_requirements(requirements);
    ++handle_queries;
}
static VKAPI_ATTR void VKAPI_CALL mock_external(VkPhysicalDevice p, const VkPhysicalDeviceExternalBufferInfo *info,
                                                VkExternalBufferProperties *properties) {
    CHECK(p == physical && properties == original_properties);
    CHECK(info->sType == original_external->sType && info->flags == original_external->flags);
    CHECK(info->handleType == original_external->handleType);
    CHECK(info->usage == expected_legacy && !(info->usage & native_rt_bits));
    if (exact_input) CHECK(info == original_external);
    if (!translated_flags2) CHECK(info->pNext == original_external->pNext);
    check_chain(info->pNext);
    CHECK(properties->sType == VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES && properties->pNext == &output_suffix);
    properties->externalMemoryProperties = (VkExternalMemoryProperties){
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
            VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
    ++external_queries;
}

static bool khr_only;
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mock_gdpa(VkDevice d, const char *name) {
    CHECK(d == device);
    if (!strcmp(name, khr_only ? "vkGetDeviceBufferMemoryRequirementsKHR" : "vkGetDeviceBufferMemoryRequirements"))
        return (PFN_vkVoidFunction)mock_query;
    if (!strcmp(name, khr_only ? "vkGetBufferMemoryRequirements2KHR" : "vkGetBufferMemoryRequirements2"))
        return (PFN_vkVoidFunction)mock_handle_query;
    return NULL;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mock_gipa(VkInstance instance, const char *name) {
    CHECK(instance == HANDLE(VkInstance, 5));
    if (!strcmp(name, khr_only ? "vkGetPhysicalDeviceExternalBufferPropertiesKHR" : "vkGetPhysicalDeviceExternalBufferProperties"))
        return (PFN_vkVoidFunction)mock_external;
    return NULL;
}

int main(void) {
    struct vkrt_device_data dev = {.device = device, .virtual_rt = true, .as_registry = (void *)1};
    struct vkrt_instance_data instance = {0};
    for (unsigned mode = 0; mode < 2; ++mode) {
        khr_only = mode != 0;
        vkrt_device_dispatch_load(&dev.vtable, mock_gdpa, device);
        vkrt_instance_dispatch_load(&instance.vtable, mock_gipa, HANDLE(VkInstance, 5));
        CHECK(dev.vtable.GetDeviceBufferMemoryRequirements == mock_query);
        CHECK(dev.vtable.GetBufferMemoryRequirements2 == mock_handle_query);
        CHECK(instance.vtable.GetPhysicalDeviceExternalBufferProperties == mock_external);
    }
    dev.vtable.CreateBuffer = mock_create;
    vkrt_map_object(VKRT_HKEY(device), &dev);
    vkrt_map_object(VKRT_HKEY(physical), &instance);
    uint32_t families[] = {1, 4};
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = large_size,
                                 .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 .sharingMode = VK_SHARING_MODE_CONCURRENT,
                                 .queueFamilyIndexCount = 2,
                                 .pQueueFamilyIndices = families};
    original_buffer = &buffer;
    VkDeviceBufferMemoryRequirements query = {.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, .pCreateInfo = &buffer};
    original_query = &query;
    VkMemoryDedicatedRequirements dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, .pNext = &output_suffix};
    VkMemoryRequirements2 requirements = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
    original_requirements = &requirements;
    native_requirements = (VkMemoryRequirements){UINT64_C(0x20000001000), 8192, 0x88};
    VkBuffer result_buffer;
    PFN_vkGetDeviceBufferMemoryRequirements get_query = (void *)vkrt_memory_query_proc("vkGetDeviceBufferMemoryRequirements");
    CHECK(get_query && get_query == (void *)vkrt_memory_query_proc("vkGetDeviceBufferMemoryRequirementsKHR"));
    CHECK(!vkrt_memory_query_proc("vkUnrelatedMemoryQuery"));
    expected_legacy =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBufferCreateInfo saved_buffer = buffer;
    get_query(device, &query, &requirements);
    CHECK(vkrt_CreateBuffer(device, &buffer, NULL, &result_buffer) == VK_SUCCESS);
    CHECK(result_buffer == native_buffer && registrations == 1 && queries == 1 && creates == 1);
    CHECK(!memcmp(&buffer, &saved_buffer, sizeof(buffer)) && query.pCreateInfo == &buffer);
    CHECK(!memcmp(&requirements.memoryRequirements, &native_requirements, sizeof(native_requirements)));
    CHECK(requirements.pNext == &dedicated && dedicated.prefersDedicatedAllocation && !dedicated.requiresDedicatedAllocation);

    /* A legacy-usage rewrite can borrow an entirely opaque chain unchanged. */
    buffer.pNext = &suffix;
    allocation_budget = 0;
    get_query(device, &query, &requirements);
    CHECK(queries == 2 && !live_allocations && buffer.pNext == &suffix);
    allocation_budget = -1;

    VkBufferUsageFlags2CreateInfoKHR flags2 = {VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR, &suffix,
                                               high_usage | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                                   VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBufferOpaqueCaptureAddressCreateInfo address = {VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO, &flags2,
                                                      UINT64_C(0x123456789000)};
    VkExternalMemoryBufferCreateInfo external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, &address,
                                                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
    buffer.pNext = &external;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    saved_buffer = buffer;
    VkBufferUsageFlags2CreateInfoKHR saved_flags2 = flags2;
    VkBufferOpaqueCaptureAddressCreateInfo saved_address = address;
    VkExternalMemoryBufferCreateInfo saved_external = external;
    expected_legacy = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    expected_flags2 = high_usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    translated_flags2 = true;
    get_query(device, &query, &requirements);
    CHECK(vkrt_CreateBuffer(device, &buffer, NULL, &result_buffer) == VK_SUCCESS);
    CHECK(queries == 3 && creates == 2 && registrations == 2 && !live_allocations);
    CHECK(!memcmp(&buffer, &saved_buffer, sizeof(buffer)) && !memcmp(&flags2, &saved_flags2, sizeof(flags2)));
    CHECK(!memcmp(&address, &saved_address, sizeof(address)) && !memcmp(&external, &saved_external, sizeof(external)));

    /* Valid queries and creation use registry-bounded stack storage even if
     * every attempted host allocation would fail. */
    for (int fault = 0; fault < 2; ++fault) {
        allocation_budget = fault;
        requirements.memoryRequirements = native_requirements;
        get_query(device, &query, &requirements);
        CHECK(requirements.memoryRequirements.size == native_requirements.size &&
              requirements.memoryRequirements.alignment == native_requirements.alignment &&
              requirements.memoryRequirements.memoryTypeBits == native_requirements.memoryTypeBits);
        allocation_budget = fault;
        CHECK(vkrt_CreateBuffer(device, &buffer, NULL, &result_buffer) == VK_SUCCESS);
        CHECK(queries == 4u + fault && creates == 3u + fault && !live_allocations && !allocation_attempts);
        CHECK(requirements.pNext == &dedicated && dedicated.pNext == &output_suffix);
    }
    allocation_budget = -1;
    struct opaque_node unknown_prefix = {(VkStructureType)0x7fff0002, &flags2, {11, 17}};
    unknown_input = &unknown_prefix;
    external.pNext = &unknown_prefix;
    get_query(device, &query, &requirements);
    CHECK(!requirements.memoryRequirements.memoryTypeBits && queries == 5 && !live_allocations);
    CHECK(vkrt_CreateBuffer(device, &buffer, NULL, &result_buffer) == VK_ERROR_EXTENSION_NOT_PRESENT);
    CHECK(creates == 4 && external.pNext == &unknown_prefix && unknown_prefix.pNext == &flags2 && !live_allocations);
    external.pNext = &address;

    VkExternalBufferProperties properties = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES, .pNext = &output_suffix};
    original_properties = &properties;
    VkPhysicalDeviceExternalBufferInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
    original_external = &external_info;
    PFN_vkGetPhysicalDeviceExternalBufferProperties get_external =
        (void *)vkrt_memory_query_proc("vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    CHECK(get_external && get_external == (void *)vkrt_memory_query_proc("vkGetPhysicalDeviceExternalBufferProperties"));
    /* Physical capability queries must work before there is a logical device. */
    vkrt_unmap_object(VKRT_HKEY(device));
    expected_legacy =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    expected_flags2 = 0;
    translated_flags2 = false;
    VkPhysicalDeviceExternalBufferInfo saved_external_info = external_info;
    get_external(physical, &external_info, &properties);
    CHECK(!memcmp(&external_info, &saved_external_info, sizeof(external_info)));
    CHECK(properties.externalMemoryProperties.externalMemoryFeatures ==
          (VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
           VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT));
    external_info.pNext = &flags2;
    external_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    expected_legacy = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    expected_flags2 = high_usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    translated_flags2 = true;
    get_external(physical, &external_info, &properties);
    CHECK(external_queries == 2 && external_info.pNext == &flags2 && !memcmp(&flags2, &saved_flags2, sizeof(flags2)));
    external_info.pNext = &external;
    allocation_budget = 1;
    get_external(physical, &external_info, &properties);
    CHECK(properties.externalMemoryProperties.externalMemoryFeatures && external_queries == 3 && !live_allocations);
    allocation_budget = -1;
    external_info.pNext = &unknown_prefix;
    get_external(physical, &external_info, &properties);
    CHECK(!properties.externalMemoryProperties.compatibleHandleTypes && external_queries == 3 && !live_allocations);
    vkrt_map_object(VKRT_HKEY(device), &dev);

    /* Ordinary Flags2 and opaque prefixes pass through without allocating,
     * including on a device which never enabled the virtual RT profile. */
    flags2.usage = high_usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.pNext = &unknown_prefix;
    expected_flags2 = flags2.usage;
    expected_legacy = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    translated_flags2 = false;
    allocation_budget = 0;
    get_query(device, &query, &requirements);
    CHECK(vkrt_CreateBuffer(device, &buffer, NULL, &result_buffer) == VK_SUCCESS);
    dev.virtual_rt = false; /* A private lazy registry must not change ordinary API behavior. */
    exact_input = true;
    get_query(device, &query, &requirements);
    CHECK(vkrt_CreateBuffer(device, &buffer, NULL, &result_buffer) == VK_SUCCESS);
    external_info.pNext = &unknown_prefix;
    get_external(physical, &external_info, &properties);
    CHECK(queries == 7 && creates == 6 && external_queries == 4 && registrations == 4 && !live_allocations);

    VkBufferMemoryRequirementsInfo2 handle_info = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, &suffix, native_buffer};
    original_handle_query = &handle_info;
    PFN_vkGetBufferMemoryRequirements2 get_handle = (void *)vkrt_memory_query_proc("vkGetBufferMemoryRequirements2KHR");
    CHECK(get_handle && get_handle == (void *)vkrt_memory_query_proc("vkGetBufferMemoryRequirements2"));
    get_handle(device, &handle_info, &requirements);
    CHECK(handle_queries == 1 && handle_info.pNext == &suffix && requirements.pNext == &dedicated);
    CHECK(output_suffix.data[0] == 43 && output_suffix.data[1] == 89 && suffix.data[0] == UINT64_C(0x123456789abcdef0));

    /* Exercise every known prefix layout at its registry-derived maximum
     * length. The bytes are opaque to the layer except for chain links. */
    const VkStructureType all_types[] = {
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT,
        VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO,
        VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_BUFFER_CREATE_INFO_NV,
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT,
        VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
#ifdef VK_USE_PLATFORM_FUCHSIA
        VK_STRUCTURE_TYPE_BUFFER_COLLECTION_BUFFER_CREATE_INFO_FUCHSIA,
#endif
    };
    _Static_assert(sizeof(all_types) / sizeof(all_types[0]) == VKRT_BUFFER_CHAIN_NODE_COUNT,
                   "update exhaustive prefix fixture when the SDK adds buffer extension structs");
    union vkrt_buffer_chain_node all_nodes[VKRT_BUFFER_CHAIN_NODE_COUNT];
    memset(all_nodes, 0x5d, sizeof(all_nodes));
    for (unsigned i = 0; i < VKRT_BUFFER_CHAIN_NODE_COUNT; ++i) {
        VkBaseOutStructure *node = (void *)&all_nodes[i];
        node->sType = all_types[i];
        node->pNext = i + 1 == VKRT_BUFFER_CHAIN_NODE_COUNT ? (void *)&flags2 : (void *)&all_nodes[i + 1];
    }
    buffer.pNext = &all_nodes[0];
    flags2.usage = high_usage | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    expected_flags2 = high_usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    exhaustive_chain = (void *)&all_nodes[0];
    dev.virtual_rt = true;
    exact_input = false;
    translated_flags2 = true;
    get_query(device, &query, &requirements);
    CHECK(vkrt_CreateBuffer(device, &buffer, NULL, &result_buffer) == VK_SUCCESS);
    CHECK(requirements.memoryRequirements.memoryTypeBits == native_requirements.memoryTypeBits && !allocation_attempts);
    vkrt_map_destroy(&vkrt_global.objects);
    CHECK(!allocation_attempts);
    if (!failures)
        printf(
            "memory query translation: creation parity, Flags2, aliases, immutable allocation-free chains, ordinary passthrough "
            "passed\n");
    return failures ? 1 : 0;
}
