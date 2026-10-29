/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_debug_objects.c"
/* Inspect the owned annotation bytes without adding a production query API. */
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
#define H(type, value) ((type)(uintptr_t)(value))
static unsigned failures;
#define CHECK(value)                                            \
    do {                                                        \
        if (!(value)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); \
            ++failures;                                         \
        }                                                       \
    } while (0)
static struct vkrt_device_data first, second, ordinary;
static struct vkrt_instance_data instance;
static struct allocation_owner {
    unsigned calls, allocations, frees, live, fail;
} owner, other_owner;
struct allocation {
    void *owner;
    uint64_t padding;
};
static VKAPI_ATTR void *VKAPI_CALL allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    CHECK((user == &owner || user == &other_owner) && alignment <= _Alignof(max_align_t) &&
          scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    struct allocation_owner *a = user;
    if (++a->calls == a->fail) return NULL;
    struct allocation *allocation = malloc(sizeof(*allocation) + size);
    CHECK(allocation != NULL);
    if (!allocation) return NULL;
    allocation->owner = user;
    ++a->allocations;
    ++a->live;
    return allocation + 1;
}
static VKAPI_ATTR void VKAPI_CALL release(void *user, void *memory) {
    if (!memory) return;
    struct allocation_owner *a = user;
    struct allocation *allocation = (struct allocation *)memory - 1;
    CHECK(allocation->owner == user && a->live != 0);
    --a->live;
    ++a->frees;
    free(allocation);
}
static const VkObjectType kinds[] = {VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
                                     VK_OBJECT_TYPE_PIPELINE,
                                     VK_OBJECT_TYPE_SHADER_MODULE,
                                     VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                                     VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE,
                                     VK_OBJECT_TYPE_QUERY_POOL,
                                     VK_OBJECT_TYPE_DEFERRED_OPERATION_KHR};
static const VkDebugReportObjectTypeEXT marker_kinds[] = {VK_DEBUG_REPORT_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR_EXT,
                                                          VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT,
                                                          VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT,
                                                          VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_LAYOUT_EXT,
                                                          VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_EXT,
                                                          VK_DEBUG_REPORT_OBJECT_TYPE_QUERY_POOL_EXT,
                                                          VK_DEBUG_REPORT_OBJECT_TYPE_UNKNOWN_EXT};
static bool live[7] = {true, true, true, true, true, true, true};
/* Two independently owned public modules may share a native module, whose
 * numeric value may also equal a different raw-RT public token. */
static const struct {
    uint64_t token, native;
} shader_aliases[] = {{UINT64_C(0x8100000000000101), 17}, {UINT64_C(0x8100000000000102), 17}};
static const struct {
    uint64_t token, native;
} layout_aliases[] = {{UINT64_C(0x8200000000000101), 17},
                      {UINT64_C(0x8200000000000102), 17},
                      {UINT64_C(0x8200000000000103), UINT64_C(0x8200000000000101)}};
static const uint64_t parallel_handles[] = {100, 101, 102, 103};
static bool owns(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle) {
    if (dev == &second) return type == VK_OBJECT_TYPE_PIPELINE && handle == 17;
    if (dev != &first) return false;
    if (type == VK_OBJECT_TYPE_SHADER_MODULE)
        for (unsigned i = 0; i < VKRT_ARRAY_SIZE(shader_aliases); ++i)
            if (handle == shader_aliases[i].token) return true;
    if (type == VK_OBJECT_TYPE_PIPELINE_LAYOUT)
        for (unsigned i = 0; i < VKRT_ARRAY_SIZE(layout_aliases); ++i)
            if (handle == layout_aliases[i].token) return true;
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(kinds); ++i)
        if (type == kinds[i] && handle == 17 && live[i]) return true;
    // Distinct live objects used by the parallel host test, not a handle rule
    // in production. The integration uses real AS/RTP/template registries.
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(parallel_handles); ++i)
        if (type == VK_OBJECT_TYPE_PIPELINE && handle == parallel_handles[i]) return true;
    return false;
}
static const void *expected_info;
static VkResult native_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
static unsigned native_calls, native_lookups;
static bool native_available = true;
static PFN_vkSetPrivateData set_private[2];
static PFN_vkGetPrivateData get_private[2];
static PFN_vkDestroyPrivateDataSlot destroy_slot[2];
static const VkPrivateDataSlot slot_a = H(VkPrivateDataSlot, 0x601), slot_b = H(VkPrivateDataSlot, 0x602);
static struct {
    VkDevice device;
    VkObjectType type;
    uint64_t handle, value;
    VkPrivateDataSlot slot;
    uint64_t *output;
    const VkAllocationCallbacks *allocator;
    unsigned sets, gets, destroys;
    bool core_only, recreate;
} private_native;
static VKAPI_ATTR VkResult VKAPI_CALL native_set_private(VkDevice device, VkObjectType type, uint64_t handle,
                                                         VkPrivateDataSlot slot, uint64_t value) {
    CHECK(device == private_native.device && type == private_native.type && handle == private_native.handle &&
          slot == private_native.slot && value == private_native.value);
    ++private_native.sets;
    return native_result;
}
static VKAPI_ATTR void VKAPI_CALL native_get_private(VkDevice device, VkObjectType type, uint64_t handle, VkPrivateDataSlot slot,
                                                     uint64_t *data) {
    CHECK(device == private_native.device && type == private_native.type && handle == private_native.handle &&
          slot == private_native.slot && data == private_native.output);
    ++private_native.gets;
    *data = UINT64_C(0xfedcba9876543210);
}
static VKAPI_ATTR void VKAPI_CALL native_destroy_slot(VkDevice device, VkPrivateDataSlot slot,
                                                      const VkAllocationCallbacks *allocator) {
    CHECK(device == private_native.device && slot == private_native.slot && allocator == private_native.allocator);
    ++private_native.destroys;
    if (private_native.recreate) {
        uint64_t data = UINT64_MAX;
        get_private[0](first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot, &data);
        CHECK(data == 0); /* Old sidecar generation is gone before native free. */
        // Model another native allocation reusing the numeric slot before
        // this destructor returns. A later clear would erase its new value.
        CHECK(set_private[1](first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot, UINT64_C(0xabcdef0187654321)) == VK_SUCCESS);
    }
}
#define NATIVE(name, type)                                                                                                \
    static VKAPI_ATTR VkResult VKAPI_CALL name(VkDevice device, const type *info) {                                       \
        CHECK((device == first.device || device == second.device || device == ordinary.device) && info == expected_info); \
        ++native_calls;                                                                                                   \
        return native_result;                                                                                             \
    }
NATIVE(native_name, VkDebugUtilsObjectNameInfoEXT)
NATIVE(native_tag, VkDebugUtilsObjectTagInfoEXT)
NATIVE(native_marker_name, VkDebugMarkerObjectNameInfoEXT)
NATIVE(native_marker_tag, VkDebugMarkerObjectTagInfoEXT)
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL native_proc_lookup(VkDevice device, const char *name) {
    CHECK(device == first.device || device == second.device || device == ordinary.device);
    ++native_lookups;
    if (!native_available) return NULL;
    if (!strcmp(name, "vkSetPrivateDataEXT") || !strcmp(name, "vkGetPrivateDataEXT") ||
        !strcmp(name, "vkDestroyPrivateDataSlotEXT")) {
        if (private_native.core_only) return NULL;
    }
    if (!strcmp(name, "vkSetPrivateData") || !strcmp(name, "vkSetPrivateDataEXT")) return (PFN_vkVoidFunction)native_set_private;
    if (!strcmp(name, "vkGetPrivateData") || !strcmp(name, "vkGetPrivateDataEXT")) return (PFN_vkVoidFunction)native_get_private;
    if (!strcmp(name, "vkDestroyPrivateDataSlot") || !strcmp(name, "vkDestroyPrivateDataSlotEXT"))
        return (PFN_vkVoidFunction)native_destroy_slot;
    if (!strcmp(name, "vkSetDebugUtilsObjectNameEXT")) return (PFN_vkVoidFunction)native_name;
    if (!strcmp(name, "vkSetDebugUtilsObjectTagEXT")) return (PFN_vkVoidFunction)native_tag;
    if (!strcmp(name, "vkDebugMarkerSetObjectNameEXT")) return (PFN_vkVoidFunction)native_marker_name;
    if (!strcmp(name, "vkDebugMarkerSetObjectTagEXT")) return (PFN_vkVoidFunction)native_marker_tag;
    CHECK(false);
    return NULL;
}
static PFN_vkSetDebugUtilsObjectNameEXT set_name;
static PFN_vkSetDebugUtilsObjectTagEXT set_tag;
static PFN_vkDebugMarkerSetObjectNameEXT marker_name;
static PFN_vkDebugMarkerSetObjectTagEXT marker_tag;
static struct object_annotation *annotation(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle) {
    struct debug_objects *state = find_device(dev);
    return state ? *find_object(state, type, handle) : NULL;
}
static VkResult name_object(VkDevice device, VkObjectType type, uint64_t handle, const char *text) {
    const VkDebugUtilsObjectNameInfoEXT info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL, type, handle, text};
    return set_name(device, &info);
}
static VkResult tag_object(VkDevice device, VkObjectType type, uint64_t handle, uint64_t tag_name, size_t size, const void *data) {
    const VkDebugUtilsObjectTagInfoEXT info = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT, NULL, type, handle, tag_name, size, data};
    return set_tag(device, &info);
}
static void owned_annotations(void) {
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(kinds); ++i) {
        char text[] = "virtual UTF-8 \xe5\x85\x89\xe7\xba\xbf";
        unsigned char bytes[] = {0, 255, 17, 0, 42};
        CHECK(name_object(first.device, kinds[i], 17, text) == VK_SUCCESS);
        CHECK(tag_object(first.device, kinds[i], 17, UINT64_MAX, sizeof(bytes), bytes) == VK_SUCCESS);
        memset(text, 0xcd, sizeof(text));
        memset(bytes, 0xcd, sizeof(bytes));
        struct object_annotation *object = annotation(&first, kinds[i], 17);
        CHECK(object && !strcmp(object->name, "virtual UTF-8 \xe5\x85\x89\xe7\xba\xbf"));
        CHECK(object && object->tags && object->tags->name == UINT64_MAX && object->tags->size == 5 &&
              object->tags->bytes[0] == 0 && object->tags->bytes[1] == 255 && object->tags->bytes[4] == 42);
        const uint32_t more = 0x12345678;
        if (marker_kinds[i] != VK_DEBUG_REPORT_OBJECT_TYPE_UNKNOWN_EXT) {
            VkDebugMarkerObjectNameInfoEXT n = {VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT, NULL, marker_kinds[i], 17,
                                                "legacy"};
            VkDebugMarkerObjectTagInfoEXT t = {
                VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_TAG_INFO_EXT, NULL, marker_kinds[i], 17, 7, sizeof(more), &more};
            CHECK(marker_name(first.device, &n) == VK_SUCCESS && marker_tag(first.device, &t) == VK_SUCCESS);
        } else {
            /* Debug-report has no deferred-operation enum. Do not invent a
             * marker value for a type only representable in debug-utils. */
            CHECK(name_object(first.device, kinds[i], 17, "legacy") == VK_SUCCESS);
            CHECK(tag_object(first.device, kinds[i], 17, 7, sizeof(more), &more) == VK_SUCCESS);
        }
        CHECK(object && !strcmp(object->name, "legacy") && object->tags->next && object->tags->next->name == 7);
        CHECK(name_object(first.device, kinds[i], 17, NULL) == VK_SUCCESS && !object->name && object->tags);
        CHECK(!native_calls && !native_lookups);
        live[i] = false;
        vkrt_debug_object_forget(&first, kinds[i], 17);
        CHECK(!annotation(&first, kinds[i], 17));
        live[i] = true; /* Reuse of a token starts without old annotations. */
        CHECK(name_object(first.device, kinds[i], 17, "reused") == VK_SUCCESS);
        CHECK(annotation(&first, kinds[i], 17) && !annotation(&first, kinds[i], 17)->tags);
        CHECK(name_object(first.device, kinds[i], 17, "") == VK_SUCCESS);
        CHECK(!annotation(&first, kinds[i], 17));
    }
    CHECK(name_object(second.device, VK_OBJECT_TYPE_PIPELINE, 17, "other-device") == VK_SUCCESS);
    CHECK(!annotation(&first, VK_OBJECT_TYPE_PIPELINE, 17) && annotation(&second, VK_OBJECT_TYPE_PIPELINE, 17));
    native_available = false;
    CHECK(name_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, "without-native-debug") == VK_SUCCESS);
    CHECK(name_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, NULL) == VK_SUCCESS && !native_calls && !native_lookups);
    native_available = true;
}
static void native_forwarding(void) {
    const uint64_t high = UINT64_C(0xfffffffffffffff1);
    VkDebugUtilsObjectNameInfoEXT n = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, H(const void *, 1),
                                       VK_OBJECT_TYPE_PIPELINE, high, H(const char *, 1)};
    expected_info = &n;
    CHECK(set_name(first.device, &n) == native_result);
    n.objectType = VK_OBJECT_TYPE_UNKNOWN;
    n.objectHandle = 17;
    CHECK(set_name(first.device, &n) == native_result);
    n.objectType = VK_OBJECT_TYPE_BUFFER;
    CHECK(set_name(first.device, &n) == native_result); /* Same value, another real object type. */
    n.objectType = VK_OBJECT_TYPE_PIPELINE;
    CHECK(set_name(ordinary.device, &n) == native_result); /* No virtual debug state. */
    VkDebugUtilsObjectTagInfoEXT t = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT,
                                      H(const void *, 2),
                                      VK_OBJECT_TYPE_BUFFER,
                                      high,
                                      55,
                                      SIZE_MAX,
                                      H(const void *, 1)};
    expected_info = &t;
    CHECK(set_tag(first.device, &t) == native_result); /* Native handles keep their original ignored/unread fields. */
    VkDebugMarkerObjectNameInfoEXT mn = {VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT, H(const void *, 3),
                                         VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT, high, H(const char *, 1)};
    expected_info = &mn;
    CHECK(marker_name(first.device, &mn) == native_result);
    VkDebugMarkerObjectTagInfoEXT mt = {VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_TAG_INFO_EXT,
                                        H(const void *, 4),
                                        VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT,
                                        high,
                                        55,
                                        SIZE_MAX,
                                        H(const void *, 1)};
    expected_info = &mt;
    CHECK(marker_tag(first.device, &mt) == native_result);
    CHECK(native_calls == 7 && native_lookups == 7);
    native_available = false;
    expected_info = &n;
    CHECK(set_name(ordinary.device, &n) == VK_ERROR_EXTENSION_NOT_PRESENT);
    native_available = true;
}
static void allocation_failures(void) {
    for (unsigned position = 1; position <= 2; ++position) {
        owner.fail = owner.calls + position;
        CHECK(name_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, "first") == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(!annotation(&first, VK_OBJECT_TYPE_PIPELINE, 17));
        owner.fail = 0;
        const unsigned char bytes[] = {1, 0, 7};
        owner.fail = owner.calls + position;
        CHECK(tag_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, 1, sizeof(bytes), bytes) == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(!annotation(&first, VK_OBJECT_TYPE_PIPELINE, 17));
        owner.fail = 0;
    }
    CHECK(name_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, "keep") == VK_SUCCESS);
    const unsigned char before[] = {3, 0, 5}, after[] = {8, 9};
    CHECK(tag_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, 1, sizeof(before), before) == VK_SUCCESS);
    owner.fail = owner.calls + 1;
    CHECK(name_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, "replace") == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(!strcmp(annotation(&first, VK_OBJECT_TYPE_PIPELINE, 17)->name, "keep"));
    owner.fail = owner.calls + 1;
    CHECK(tag_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, 1, sizeof(after), after) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(annotation(&first, VK_OBJECT_TYPE_PIPELINE, 17)->tags->size == sizeof(before) &&
          !memcmp(annotation(&first, VK_OBJECT_TYPE_PIPELINE, 17)->tags->bytes, before, sizeof(before)));
    owner.fail = 0;
    unsigned prior_calls = owner.calls;
    CHECK(tag_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, 2, SIZE_MAX, H(const void *, 1)) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(owner.calls == prior_calls);
    CHECK(tag_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, 1, sizeof(after), after) == VK_SUCCESS);
    CHECK(annotation(&first, VK_OBJECT_TYPE_PIPELINE, 17)->tags->size == sizeof(after));
    vkrt_debug_object_forget(&first, VK_OBJECT_TYPE_PIPELINE, 17);
}
static uint64_t private_value_of(VkDevice device, VkObjectType type, uint64_t handle, VkPrivateDataSlot slot, unsigned alias) {
    uint64_t data = UINT64_MAX;
    get_private[alias](device, type, handle, slot, &data);
    return data;
}
static void public_alias_annotations(VkObjectType type, VkDebugReportObjectTypeEXT marker_type, const uint64_t tokens[3]) {
    const char *names[] = {"first public identity", "second public identity", "third public identity"};
    const unsigned before_calls = native_calls, before_lookups = native_lookups;
    const unsigned before_sets = private_native.sets, before_gets = private_native.gets;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned char bytes[] = {(unsigned char)i, 0, 255};
        CHECK(name_object(first.device, type, tokens[i], names[i]) == VK_SUCCESS);
        CHECK(tag_object(first.device, type, tokens[i], 77, sizeof(bytes), bytes) == VK_SUCCESS);
        bytes[0] = 99;
        CHECK(set_private[i & 1](first.device, type, tokens[i], slot_a, 100 + i) == VK_SUCCESS);
        CHECK(private_value_of(first.device, type, tokens[i], slot_a, (i + 1) & 1) == 100 + i);
        const struct object_annotation *object = annotation(&first, type, tokens[i]);
        CHECK(object && !strcmp(object->name, names[i]) && object->tags && object->tags->bytes[0] == i);
    }
    VkDebugMarkerObjectNameInfoEXT name = {VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT, NULL, marker_type, tokens[0],
                                           "renamed wrapper"};
    const unsigned char bytes[] = {42, 0, 13};
    VkDebugMarkerObjectTagInfoEXT tag = {
        VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_TAG_INFO_EXT, NULL, marker_type, tokens[1], 77, sizeof(bytes), bytes};
    expected_info = &name;
    CHECK(marker_name(first.device, &name) == VK_SUCCESS);
    expected_info = &tag;
    CHECK(marker_tag(first.device, &tag) == VK_SUCCESS);
    const struct object_annotation *a = annotation(&first, type, tokens[0]);
    const struct object_annotation *b = annotation(&first, type, tokens[1]);
    const struct object_annotation *c = annotation(&first, type, tokens[2]);
    CHECK(a && a->name && !strcmp(a->name, "renamed wrapper"));
    CHECK(b && b->name && !strcmp(b->name, names[1]));
    CHECK(c && c->name && !strcmp(c->name, names[2]));
    CHECK(b && b->tags && b->tags->bytes[0] == 42);
    CHECK(c && c->tags && c->tags->bytes[0] == 2);
    CHECK(set_private[1](first.device, type, tokens[0], slot_a, 0) == VK_SUCCESS);
    CHECK(private_value_of(first.device, type, tokens[0], slot_a, 0) == 0);
    vkrt_debug_object_forget(&first, type, tokens[0]);
    CHECK(!annotation(&first, type, tokens[0]));
    CHECK(private_value_of(first.device, type, tokens[1], slot_a, 0) == 101);
    CHECK(private_value_of(first.device, type, tokens[2], slot_a, 1) == 102);
    CHECK(native_calls == before_calls && native_lookups == before_lookups && private_native.sets == before_sets &&
          private_native.gets == before_gets);
    vkrt_debug_object_forget(&first, type, tokens[1]);
    vkrt_debug_object_forget(&first, type, tokens[2]);
}
static void shader_module_alias_annotations(void) {
    CHECK(shader_aliases[0].token != shader_aliases[1].token && shader_aliases[0].native == shader_aliases[1].native &&
          shader_aliases[0].native == 17);
    const uint64_t tokens[] = {shader_aliases[0].token, shader_aliases[1].token, 17};
    public_alias_annotations(VK_OBJECT_TYPE_SHADER_MODULE, VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT, tokens);
}
static void pipeline_layout_alias_annotations(void) {
    CHECK(layout_aliases[0].token != layout_aliases[1].token && layout_aliases[0].native == layout_aliases[1].native &&
          layout_aliases[2].native == layout_aliases[0].token);
    const uint64_t tokens[] = {layout_aliases[0].token, layout_aliases[1].token, layout_aliases[2].token};
    public_alias_annotations(VK_OBJECT_TYPE_PIPELINE_LAYOUT, VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_LAYOUT_EXT, tokens);
    /* Ordinary native devices can legally use the same numeric value as a
     * different device's public token. Keep their original debug parameters. */
    const unsigned before_calls = native_calls, before_lookups = native_lookups;
    VkDebugMarkerObjectNameInfoEXT name = {VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT, NULL,
                                           VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_LAYOUT_EXT, tokens[0], "native layout"};
    expected_info = &name;
    CHECK(marker_name(ordinary.device, &name) == native_result);
    const uint32_t data = 0x12345678;
    VkDebugMarkerObjectTagInfoEXT tag = {VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_TAG_INFO_EXT,
                                         NULL,
                                         VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_LAYOUT_EXT,
                                         tokens[0],
                                         88,
                                         sizeof(data),
                                         &data};
    expected_info = &tag;
    CHECK(marker_tag(ordinary.device, &tag) == native_result);
    CHECK(native_calls == before_calls + 2 && native_lookups == before_lookups + 2);
}
static void private_data_values(void) {
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(kinds); ++i) {
        unsigned before = owner.calls;
        CHECK(private_value_of(first.device, kinds[i], 17, slot_a, i & 1) == 0 && owner.calls == before);
        CHECK(set_private[i & 1](first.device, kinds[i], 17, slot_a, 0) == VK_SUCCESS && owner.calls == before);
        CHECK(set_private[0](first.device, kinds[i], 17, slot_a, UINT64_C(0xfedcba9876543210) + i) == VK_SUCCESS);
        CHECK(set_private[1](first.device, kinds[i], 17, slot_b, UINT64_C(0x8123456789abcdef) + i) == VK_SUCCESS);
        CHECK(private_value_of(first.device, kinds[i], 17, slot_a, 1) == UINT64_C(0xfedcba9876543210) + i);
        CHECK(name_object(first.device, kinds[i], 17, "name beside private values") == VK_SUCCESS);
        CHECK(name_object(first.device, kinds[i], 17, NULL) == VK_SUCCESS);
        CHECK(private_value_of(first.device, kinds[i], 17, slot_b, 0) == UINT64_C(0x8123456789abcdef) + i);
        CHECK(set_private[1](first.device, kinds[i], 17, slot_a, 0) == VK_SUCCESS);
        CHECK(private_value_of(first.device, kinds[i], 17, slot_a, 0) == 0);
        CHECK(private_value_of(first.device, kinds[i], 17, slot_b, 1) == UINT64_C(0x8123456789abcdef) + i);
        live[i] = false;
        vkrt_debug_object_forget(&first, kinds[i], 17);
        live[i] = true;
        CHECK(!annotation(&first, kinds[i], 17) && private_value_of(first.device, kinds[i], 17, slot_b, 0) == 0);
    }
    for (unsigned position = 1; position <= 2; ++position) {
        owner.fail = owner.calls + position;
        CHECK(set_private[0](first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, 1) == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(!annotation(&first, VK_OBJECT_TYPE_PIPELINE, 17) &&
              private_value_of(first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, 1) == 0);
        owner.fail = 0;
    }
    CHECK(set_private[0](first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, UINT64_MAX) == VK_SUCCESS);
    unsigned before = owner.calls;
    owner.fail = owner.calls + 1;
    CHECK(set_private[1](first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, UINT64_C(0x123456789abcdef0)) == VK_SUCCESS);
    CHECK(owner.calls == before); /* Replacing an existing scalar never allocates. */
    CHECK(set_private[1](first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_b, 2) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(private_value_of(first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, 0) == UINT64_C(0x123456789abcdef0));
    CHECK(private_value_of(first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_b, 1) == 0);
    owner.fail = 0;
    CHECK(set_private[0](first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_b, 3) == VK_SUCCESS);
    CHECK(set_private[0](second.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, 9) == VK_SUCCESS);
    private_native.device = first.device;
    private_native.slot = slot_a;
    private_native.allocator = NULL;
    destroy_slot[0](first.device, slot_a, NULL);
    CHECK(private_value_of(first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, 1) == 0);
    CHECK(private_value_of(first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_b, 1) == 3);
    CHECK(private_value_of(second.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, 1) == 9);
    CHECK(set_private[0](first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, 7) == VK_SUCCESS);
    private_native.recreate = true;
    destroy_slot[1](first.device, slot_a, NULL);
    private_native.recreate = false;
    CHECK(private_value_of(first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, 0) == UINT64_C(0xabcdef0187654321));
    destroy_slot[0](first.device, slot_a, NULL);
    CHECK(private_value_of(first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_a, 0) == 0);
    CHECK(name_object(first.device, VK_OBJECT_TYPE_PIPELINE, 17, "keep name on slot deletion") == VK_SUCCESS);
    private_native.slot = slot_b;
    destroy_slot[1](first.device, slot_b, NULL);
    CHECK(annotation(&first, VK_OBJECT_TYPE_PIPELINE, 17) &&
          !strcmp(annotation(&first, VK_OBJECT_TYPE_PIPELINE, 17)->name, "keep name on slot deletion"));
    CHECK(private_value_of(first.device, VK_OBJECT_TYPE_PIPELINE, 17, slot_b, 0) == 0);
    vkrt_debug_object_forget(&first, VK_OBJECT_TYPE_PIPELINE, 17);
    // The remaining second-device value is reclaimed by device finish below.
    private_native.type = VK_OBJECT_TYPE_BUFFER;
    private_native.handle = UINT64_C(0xffffffffffffffe1);
    private_native.value = UINT64_C(0x8765432101234567);
    private_native.slot = H(VkPrivateDataSlot, UINT64_C(0xfedcba9876543210));
    const VkAllocationCallbacks slot_allocator = {.pUserData = &owner, .pfnAllocation = allocate, .pfnFree = release};
    private_native.allocator = &slot_allocator;
    for (unsigned core_only = 0; core_only < 2; ++core_only) {
        private_native.core_only = core_only != 0;
        CHECK(set_private[core_only](first.device, private_native.type, private_native.handle, private_native.slot,
                                     private_native.value) == native_result);
        uint64_t data = 0;
        private_native.output = &data;
        get_private[core_only](first.device, private_native.type, private_native.handle, private_native.slot, &data);
        CHECK(data == UINT64_C(0xfedcba9876543210));
        destroy_slot[core_only](first.device, private_native.slot, &slot_allocator);
    }
    private_native.core_only = false;
    CHECK(private_native.sets == 2 && private_native.gets == 2 && private_native.destroys == 6);
}
#ifdef _WIN32
static DWORD WINAPI worker(void *argument) {
#else
static void *worker(void *argument) {
#endif
    uintptr_t index = (uintptr_t)argument;
    for (uint32_t value = 0; value < 200; ++value) {
        char name[32];
        snprintf(name, sizeof(name), "thread-%u-%u", (unsigned)index, value);
        if (name_object(first.device, VK_OBJECT_TYPE_PIPELINE, 100 + index, name) != VK_SUCCESS ||
            tag_object(first.device, VK_OBJECT_TYPE_PIPELINE, 100 + index, index, sizeof(value), &value) != VK_SUCCESS) {
#ifdef _WIN32
            return 1;
#else
            return (void *)1;
#endif
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
int main(void) {
    first.device = H(VkDevice, 1);
    second.device = H(VkDevice, 2);
    ordinary.device = H(VkDevice, 3);
    first.vtable.GetDeviceProcAddr = second.vtable.GetDeviceProcAddr = ordinary.vtable.GetDeviceProcAddr = native_proc_lookup;
    vkrt_map_object(VKRT_HKEY(first.device), &first);
    vkrt_map_object(VKRT_HKEY(second.device), &second);
    vkrt_map_object(VKRT_HKEY(ordinary.device), &ordinary);
    VkAllocationCallbacks allocator = {.pUserData = &owner, .pfnAllocation = allocate, .pfnFree = release};
    owner.fail = 1;
    CHECK(vkrt_debug_objects_init(&first, &allocator, owns) == VK_ERROR_OUT_OF_HOST_MEMORY && !find_device(&first) && !owner.live);
    owner.fail = 0;
    CHECK(vkrt_debug_objects_init(&first, &allocator, owns) == VK_SUCCESS);
    unsigned prior_init_calls = owner.calls;
    owner.fail = owner.calls + 1;
    CHECK(vkrt_debug_objects_init(&first, &allocator, owns) == VK_SUCCESS && owner.calls == prior_init_calls);
    owner.fail = 0;
    instance.has_allocator = true;
    instance.allocator = allocator;
    instance.allocator.pUserData = &other_owner;
    second.instance_data = &instance;
    CHECK(vkrt_debug_objects_init(&second, NULL, owns) == VK_SUCCESS);
    set_name = (PFN_vkSetDebugUtilsObjectNameEXT)vkrt_debug_objects_proc("vkSetDebugUtilsObjectNameEXT");
    set_tag = (PFN_vkSetDebugUtilsObjectTagEXT)vkrt_debug_objects_proc("vkSetDebugUtilsObjectTagEXT");
    marker_name = (PFN_vkDebugMarkerSetObjectNameEXT)vkrt_debug_objects_proc("vkDebugMarkerSetObjectNameEXT");
    marker_tag = (PFN_vkDebugMarkerSetObjectTagEXT)vkrt_debug_objects_proc("vkDebugMarkerSetObjectTagEXT");
    set_private[0] = (void *)vkrt_debug_objects_proc("vkSetPrivateData");
    set_private[1] = (void *)vkrt_debug_objects_proc("vkSetPrivateDataEXT");
    get_private[0] = (void *)vkrt_debug_objects_proc("vkGetPrivateData");
    get_private[1] = (void *)vkrt_debug_objects_proc("vkGetPrivateDataEXT");
    destroy_slot[0] = (void *)vkrt_debug_objects_proc("vkDestroyPrivateDataSlot");
    destroy_slot[1] = (void *)vkrt_debug_objects_proc("vkDestroyPrivateDataSlotEXT");
    CHECK(set_name && set_tag && marker_name && marker_tag && !vkrt_debug_objects_proc("vkUnknown"));
    CHECK(set_private[0] && set_private[1] && get_private[0] && get_private[1] && destroy_slot[0] && destroy_slot[1]);
    owned_annotations();
    native_forwarding();
    allocation_failures();
    private_data_values();
    shader_module_alias_annotations();
    pipeline_layout_alias_annotations();
    if (failures) {
        vkrt_debug_objects_finish(&first);
        vkrt_debug_objects_finish(&second);
        fprintf(stderr, "Debug annotation checks failed before the parallel test\n");
        return 1;
    }
#ifdef _WIN32
    HANDLE threads[4];
    for (uintptr_t i = 0; i < 4; ++i) threads[i] = CreateThread(NULL, 0, worker, (void *)i, 0, NULL);
    CHECK(WaitForMultipleObjects(4, threads, TRUE, INFINITE) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 4; ++i) {
        DWORD status = 1;
        CHECK(GetExitCodeThread(threads[i], &status) && status == 0);
        CloseHandle(threads[i]);
    }
#else
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; ++i) CHECK(!pthread_create(&threads[i], NULL, worker, (void *)i));
    for (unsigned i = 0; i < 4; ++i) {
        void *result;
        CHECK(!pthread_join(threads[i], &result) && !result);
    }
#endif
    for (unsigned i = 0; i < 4; ++i) {
        struct object_annotation *object = annotation(&first, VK_OBJECT_TYPE_PIPELINE, 100 + i);
        uint32_t value = 0;
        if (object && object->tags) memcpy(&value, object->tags->bytes, sizeof(value));
        CHECK(object && object->tags && value == 199);
    }
    ordinary.instance_data = &instance;
    ordinary.queue_families_have_allocator = true;
    ordinary.queue_families_allocator = allocator;
    unsigned prior_device = owner.calls, prior_instance = other_owner.calls;
    CHECK(vkrt_debug_objects_init(&ordinary, NULL, owns) == VK_SUCCESS);
    CHECK(owner.calls == prior_device + 1 && other_owner.calls == prior_instance);
    vkrt_debug_objects_finish(&first);
    vkrt_debug_objects_finish(&second);
    vkrt_debug_objects_finish(&ordinary);
    CHECK(!devices && !owner.live && owner.allocations == owner.frees && !other_owner.live &&
          other_owner.allocations == other_owner.frees);
    printf(
        "Virtual object metadata: %s (%u failures; utils/marker, private data, owned bytes, native forwarding, OOM, reuse, 800 "
        "parallel "
        "updates)\n",
        failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
