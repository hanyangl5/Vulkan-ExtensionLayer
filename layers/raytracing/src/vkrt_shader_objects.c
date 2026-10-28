/* Copyright (c) 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT */
#include "vkrt_shader_objects.h"
#include "vkrt_layer.h"
#include "vkrt_rt_capabilities.h"
#include "vkrt_as_registry.h"
#include "vkrt_spirv_lower.h"
#include "vkrt_shader_interface.h"

/* Envelope fields and interface recipes contain no host pointers or handles.
 * Only proven NoRT groups retain opaque native code; RQ groups rebuild source
 * against the destination logical device instead of copying its resolver BDA. */
enum { HEADER = 128, STAGE_HEADER = 96, FORMAT = 3, SOURCE = 1 };
enum { W_TOTAL = 4, W_GROUP = 6, W_TARGET = 7, W_OPAQUE = 8, W_ELIGIBLE = 21, W_NATIVE_VERSION = 22, W_RQ = 23, W_HASH = 28 };
enum {
    S_TOTAL = 0,
    S_CODE = 2,
    S_NAME = 4,
    S_MAPS = 5,
    S_DATA = 6,
    S_INTERFACE = 8,
    S_SUBGROUP = 10,
    S_CUSTOM = 11,
    S_CUSTOM_ENABLE = 12,
    S_COLORS = 13,
    S_DEPTH = 14,
    S_STENCIL = 15,
    S_STAGE = 16,
    S_NEXT = 17,
    S_FLAGS = 18,
    S_RQ = 19,
    S_REBUILDABLE = 20
};
struct shader_record {
    struct shader_record *next;
    VkShaderEXT shader;
    uint64_t references;
    uint8_t *blob;
    size_t size;
    bool custom_allocator;
    VkAllocationCallbacks allocator;
};
struct vkrt_shader_object_state {
    vkrt_mutex_t lock;
    struct shader_record *records;
    VkPhysicalDeviceShaderObjectPropertiesEXT native;
    uint8_t uuid[VK_UUID_SIZE];
    bool eligible;
    bool custom_allocator;
    VkAllocationCallbacks allocator;
};
static const VkAllocationCallbacks *device_allocator(const struct vkrt_device_data *dev) {
    if (dev->queue_families_have_allocator) return &dev->queue_families_allocator;
    return dev->instance_data && dev->instance_data->has_allocator ? &dev->instance_data->allocator : NULL;
}
static void free_state(struct vkrt_shader_object_state *state) {
#ifndef _WIN32
    pthread_mutex_destroy(&state->lock);
#endif
    if (state->custom_allocator) {
        VkAllocationCallbacks allocator = state->allocator;
        allocator.pfnFree(allocator.pUserData, state);
    } else
        free(state);
}
struct decoded {
    VkShaderCreateInfoEXT info;
    VkSpecializationInfo specialization;
    VkSpecializationMapEntry *maps;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup;
#ifdef VK_EXT_custom_resolve
    VkCustomResolveCreateInfoEXT custom;
#endif
    VkFormat *colors;
    const uint8_t *interface_bytes;
    size_t interface_size;
    struct vkrt_shader_interface *owned_interface;
    void *rewritten;
    VkPushConstantRange *ranges;
};
struct group {
    uint32_t count, target;
    struct decoded *stages;
    const uint8_t *opaque;
    size_t opaque_size;
    bool rq;
};
static uint32_t word(const uint8_t *p, unsigned n) {
    p += n * 4;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t wide(const uint8_t *p, unsigned n) { return word(p, n) | (uint64_t)word(p, n + 1) << 32; }
static void put(uint8_t *p, unsigned n, uint32_t v) {
    p += n * 4;
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static void put_wide(uint8_t *p, unsigned n, uint64_t v) {
    put(p, n, (uint32_t)v);
    put(p, n + 1, (uint32_t)(v >> 32));
}
static uint64_t checksum(const uint8_t *p, size_t size) {
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        value ^= i >= W_HASH * 4 && i < (W_HASH + 2) * 4 ? 0 : p[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}
static bool add(size_t *size, size_t amount) {
    if (amount > SIZE_MAX - *size) return false;
    *size += amount;
    return true;
}
static void uuid(const uint8_t native[VK_UUID_SIZE], bool eligible, uint8_t output[VK_UUID_SIZE]) {
    /* SHA-256(namespace || native UUID || format BE32 || profile), truncated
     * to 128 bits. This fixed 37-byte message fits one padded SHA-256 block. */
    const uint8_t domain[VK_UUID_SIZE] = {0x88, 0x8b, 0xe6, 0xcd, 0x2f, 0xa1, 0x41, 0x77,
                                          0x9e, 0x21, 0x6c, 0x30, 0xb4, 0x58, 0x01, 0x91};
    const uint32_t constants[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
                                    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
                                    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                                    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
                                    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                                    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                                    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                                    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint8_t message[64] = {0};
    memcpy(message, domain, 16);
    memcpy(message + 16, native, 16);
    message[32] = (uint8_t)(FORMAT >> 24);
    message[33] = (uint8_t)(FORMAT >> 16);
    message[34] = (uint8_t)(FORMAT >> 8);
    message[35] = FORMAT;
    message[36] = eligible;
    message[37] = 0x80;
    message[62] = 1;
    message[63] = 40; /* 37 * 8 = 296 */
    uint32_t words[64];
#define ROR(v, n) (((v) >> (n)) | ((v) << (32 - (n))))
    for (unsigned i = 0; i < 16; ++i)
        words[i] = (uint32_t)message[i * 4] << 24 | (uint32_t)message[i * 4 + 1] << 16 | (uint32_t)message[i * 4 + 2] << 8 |
                   message[i * 4 + 3];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t a = words[i - 15], b = words[i - 2];
        words[i] = words[i - 16] + (ROR(a, 7) ^ ROR(a, 18) ^ (a >> 3)) + words[i - 7] + (ROR(b, 17) ^ ROR(b, 19) ^ (b >> 10));
    }
    const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint32_t a = initial[0], b = initial[1], c = initial[2], d = initial[3], e = initial[4], f = initial[5], g = initial[6],
             h = initial[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t one = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + constants[i] + words[i];
        uint32_t two = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + one;
        d = c;
        c = b;
        b = a;
        a = one + two;
    }
#undef ROR
    uint32_t first[4] = {initial[0] + a, initial[1] + b, initial[2] + c, initial[3] + d};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) output[i * 4 + j] = (uint8_t)(first[i] >> (24 - j * 8));
}
static void header(struct vkrt_shader_object_state *state, uint8_t *p, size_t size, uint32_t count, uint32_t target, bool rq) {
    put(p, 0, 0x54524b56);
    put(p, 1, 0x31444853);
    put(p, 2, FORMAT);
    put(p, 3, SOURCE);
    put_wide(p, W_TOTAL, size);
    put(p, W_GROUP, count);
    put(p, W_TARGET, target);
    put(p, W_RQ, rq);
    put(p, W_ELIGIBLE, state->eligible);
    put(p, W_NATIVE_VERSION, state->native.shaderBinaryVersion);
    memcpy(p + 24 * 4, state->uuid, VK_UUID_SIZE);
}
static void *allocate(struct shader_record *record, size_t size) {
    return record->custom_allocator
               ? record->allocator.pfnAllocation(record->allocator.pUserData, size, 16, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE)
               : malloc(size);
}
static void release(struct shader_record *record, void *p) {
    if (!p) return;
    if (record->custom_allocator)
        record->allocator.pfnFree(record->allocator.pUserData, p);
    else
        free(p);
}
static void free_record(struct shader_record *record) {
    if (!record) return;
    release(record, record->blob);
    bool custom = record->custom_allocator;
    VkAllocationCallbacks allocator = record->allocator;
    if (custom)
        allocator.pfnFree(allocator.pUserData, record);
    else
        free(record);
}
static struct shader_record *new_record(const VkAllocationCallbacks *allocator) {
    struct shader_record *record = allocator
                                       ? allocator->pfnAllocation(allocator->pUserData, sizeof(*record),
                                                                  _Alignof(struct shader_record), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE)
                                       : malloc(sizeof(*record));
    if (record) {
        memset(record, 0, sizeof(*record));
        record->references = 1;
        if (allocator) {
            record->custom_allocator = true;
            record->allocator = *allocator;
        }
    }
    return record;
}
VkResult vkrt_shader_objects_init(struct vkrt_device_data *dev) {
    if (!dev->app_requested_shader_objects || !dev->vtable.CreateShadersEXT) return VK_SUCCESS;
    const VkAllocationCallbacks *allocator = device_allocator(dev);
    struct vkrt_shader_object_state *state =
        allocator ? allocator->pfnAllocation(allocator->pUserData, sizeof(*state), _Alignof(struct vkrt_shader_object_state),
                                             VK_SYSTEM_ALLOCATION_SCOPE_DEVICE)
                  : malloc(sizeof(*state));
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(state, 0, sizeof(*state));
    if (allocator) {
        state->custom_allocator = true;
        state->allocator = *allocator;
    }
    vkrt_mutex_init(&state->lock);
    state->native.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &state->native};
    if (!dev->instance_data || !dev->instance_data->vtable.GetPhysicalDeviceProperties2) {
        free_state(state);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    dev->instance_data->vtable.GetPhysicalDeviceProperties2(dev->physical_device, &props);
    state->eligible = vkrt_native_backend_eligible(dev->instance_data, dev->physical_device);
    uuid(state->native.shaderBinaryUUID, state->eligible, state->uuid);
    dev->shader_objects = state;
    return VK_SUCCESS;
}
void vkrt_shader_objects_finish(struct vkrt_device_data *dev) {
    struct vkrt_shader_object_state *state = dev->shader_objects;
    if (!state) return;
    while (state->records) {
        struct shader_record *next = state->records->next;
        free_record(state->records);
        state->records = next;
    }
    free_state(state);
    dev->shader_objects = NULL;
}
static bool rt_capability(uint32_t capability) {
    switch (capability) {
        case 4471:
        case 4472:
        case 4478:
        case 4479: /* query / primitive culling / tracing */
        case 5336:
        case 5340:
        case 5341:
        case 5353: /* position fetch / NV / motion / provisional */
        case 5381:
        case 5383:
        case 5388:
        case 5391:
        case 5409: /* micromaps / reorder / query position fetch */
        case 5418:
        case 5419:
        case 5437:
        case 6020:
        case 6032: /* new geometry / cluster / cull mask / micromap mode */
            return true;
        default:
            return false;
    }
}
static bool rt_opcode(uint32_t op) {
    return (op >= 4445 && op <= 4449) || (op >= 4472 && op <= 4479) || (op >= 5249 && op <= 5281) || (op >= 5304 && op <= 5341) ||
           (op >= 5344 && op <= 5352) || (op >= 6016 && op <= 6032);
}
static bool rq_source(const VkShaderCreateInfoEXT *info) {
    const uint32_t *p = info->pCode;
    size_t count = info->codeSize / 4;
    if (count < 5 || p[0] != 0x07230203) return true; /* not a proven portable source */
    for (size_t i = 5; i < count;) {
        uint32_t length = p[i] >> 16, op = p[i] & 65535;
        if (!length || length > count - i) return true;
        if (rt_opcode(op) || (op == 17 && length >= 2 && rt_capability(p[i + 1])) ||
            (op == 15 && length >= 2 && p[i + 1] >= 5313 && p[i + 1] <= 5318))
            return true;
        if (op == 10 && length >= 2) {
            const char *extension = (const char *)&p[i + 1];
            size_t bytes = (size_t)(length - 1) * 4;
            if (!memchr(extension, 0, bytes)) return true;
            if (!strncmp(extension, "SPV_KHR_ray_", 12) || !strncmp(extension, "SPV_NV_ray_", 11) ||
                strstr(extension, "shader_invocation_reorder") || strstr(extension, "micromap"))
                return true;
        }
        i += length;
    }
    return false;
}
static bool matching_entry(const VkShaderCreateInfoEXT *info) {
    uint32_t model;
    switch (info->stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            model = 0;
            break;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
            model = 1;
            break;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
            model = 2;
            break;
        case VK_SHADER_STAGE_GEOMETRY_BIT:
            model = 3;
            break;
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            model = 4;
            break;
        case VK_SHADER_STAGE_COMPUTE_BIT:
            model = 5;
            break;
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            model = 5364;
            break;
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            model = 5365;
            break;
        default:
            return false;
    }
    const uint32_t *p = info->pCode;
    for (size_t i = 5, count = info->codeSize / 4; i < count;) {
        uint32_t length = p[i] >> 16;
        if (!length || length > count - i) return false;
        if ((p[i] & 65535) == 15 && length >= 4 &&
            (p[i + 1] == model || (model == 5364 && p[i + 1] == 5267) || (model == 5365 && p[i + 1] == 5268))) {
            const char *name = (const char *)&p[i + 3];
            if (memchr(name, 0, (size_t)(length - 3) * 4) && !strcmp(name, info->pName)) return true;
        }
        i += length;
    }
    return false;
}
static unsigned stage_order(VkShaderStageFlagBits stage) {
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            return 0;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            return 1;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
            return 2;
        case VK_SHADER_STAGE_GEOMETRY_BIT:
            return 3;
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            return 4;
        default:
            return 5;
    }
}
static uint32_t linked_prefix(uint32_t count, const VkShaderCreateInfoEXT *infos) {
    uint32_t first = count;
    bool invalid = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!(infos[i].flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT)) continue;
        if (i < first) first = i;
        unsigned order = stage_order(infos[i].stage), following = 6;
        VkShaderStageFlags next = 0;
        for (uint32_t j = 0; j < count; ++j) {
            if (!(infos[j].flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT)) continue;
            unsigned candidate = stage_order(infos[j].stage);
            if (candidate > order && candidate < following) {
                following = candidate;
                next = infos[j].stage;
            }
        }
        if (next && infos[i].nextStage != next) invalid = true;
    }
    /* 08409 also applies to BINARY. An unordered prefix can retain a later
     * peer but lose its predecessor, so that linked group cannot be created
     * with its original nextStage values. Fail at its first member instead;
     * do not rewrite caller flags/nextStage or discard earlier independent
     * inputs. A lone remaining linked stage has no such adjacency constraint. */
    return invalid ? first : count;
}
static bool valid_source_fields(const VkShaderCreateInfoEXT *info) {
    VkShaderStageFlags next = 0;
    switch (info->stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            next = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            break;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
            next = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            break;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
            next = VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            break;
        case VK_SHADER_STAGE_GEOMETRY_BIT:
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            next = VK_SHADER_STAGE_FRAGMENT_BIT;
            break;
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            next = VK_SHADER_STAGE_MESH_BIT_EXT;
            break;
        case VK_SHADER_STAGE_FRAGMENT_BIT:
        case VK_SHADER_STAGE_COMPUTE_BIT:
            break;
        default:
            return false;
    }
    /* Wire v3 recognizes the flag set in Vulkan-Headers 1.4.357. Further
     * flag semantics need a versioned recipe rather than invalid SPIR-V CI. */
    if ((info->flags & ~UINT32_C(0x00049cff)) || (info->nextStage & ~next)) return false;
    if (info->stage == VK_SHADER_STAGE_COMPUTE_BIT && (info->flags & (VK_SHADER_CREATE_LINK_STAGE_BIT_EXT | 0x00040000)))
        return false;
    if (info->stage != VK_SHADER_STAGE_COMPUTE_BIT && (info->flags & VK_SHADER_CREATE_DISPATCH_BASE_BIT_EXT)) return false;
    if (info->stage != VK_SHADER_STAGE_MESH_BIT_EXT && (info->flags & VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT)) return false;
    if (info->stage != VK_SHADER_STAGE_FRAGMENT_BIT && (info->flags & 0x60)) return false;
    if (info->stage != VK_SHADER_STAGE_COMPUTE_BIT && info->stage != VK_SHADER_STAGE_TASK_BIT_EXT &&
        info->stage != VK_SHADER_STAGE_MESH_BIT_EXT && (info->flags & VK_SHADER_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT))
        return false;
    return true;
}
static bool aligned_size(size_t *size) { return add(size, (16 - (*size & 15)) & 15); }
static bool same_group(const VkShaderCreateInfoEXT *infos, uint32_t target, uint32_t index) {
    return target == index || ((infos[target].flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT) &&
                               (infos[index].flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT));
}
static VkResult stage_blob(struct vkrt_device_data *dev, struct shader_record *record, const VkShaderCreateInfoEXT *info,
                           bool rebuildable) {
    uint32_t subgroup = 0;
#ifdef VK_EXT_custom_resolve
    const VkCustomResolveCreateInfoEXT *custom = NULL;
#endif
    void *interface_bytes = NULL;
    size_t interface_size = 0, colors = 0;
    VkResult result = VK_SUCCESS;
    if (rebuildable) {
        for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO)
                subgroup = ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *)p)->requiredSubgroupSize;
#ifdef VK_EXT_custom_resolve
            else if (p->sType == VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT)
                custom = (const void *)p;
#endif
            else if (p->sType != VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        }
#ifdef VK_EXT_custom_resolve
        colors = custom ? custom->colorAttachmentCount : 0;
#endif
        result = vkrt_shader_interface_capture(dev, info, &interface_bytes, &interface_size);
        if (result != VK_SUCCESS) return result;
    }
    size_t names = strlen(info->pName) + 1, maps = info->pSpecializationInfo ? info->pSpecializationInfo->mapEntryCount : 0;
    size_t data = info->pSpecializationInfo ? info->pSpecializationInfo->dataSize : 0, size = STAGE_HEADER;
    if (names > UINT32_MAX || maps > SIZE_MAX / 16 || colors > SIZE_MAX / 4 || !add(&size, info->codeSize) || !add(&size, names) ||
        !add(&size, maps * 16) || !add(&size, data) || !add(&size, colors * 4) || !add(&size, interface_size) ||
        !aligned_size(&size)) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto done;
    }
    record->blob = allocate(record, size);
    if (!record->blob) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto done;
    }
    record->size = size;
    uint8_t *p = record->blob;
    memset(p, 0, size);
    put_wide(p, S_TOTAL, size);
    put_wide(p, S_CODE, info->codeSize);
    put(p, S_NAME, (uint32_t)names);
    put(p, S_MAPS, (uint32_t)maps);
    put_wide(p, S_DATA, data);
    put_wide(p, S_INTERFACE, interface_size);
    put(p, S_SUBGROUP, subgroup);
    put(p, S_RQ, rq_source(info));
    put(p, S_REBUILDABLE, rebuildable);
    put(p, S_STAGE, info->stage);
    put(p, S_NEXT, info->nextStage);
    put(p, S_FLAGS, info->flags);
#ifdef VK_EXT_custom_resolve
    if (custom) {
        put(p, S_CUSTOM, 1);
        put(p, S_CUSTOM_ENABLE, custom->customResolve);
        put(p, S_COLORS, (uint32_t)colors);
        put(p, S_DEPTH, custom->depthAttachmentFormat);
        put(p, S_STENCIL, custom->stencilAttachmentFormat);
    }
#endif
    size_t offset = STAGE_HEADER;
    memcpy(p + offset, info->pCode, info->codeSize);
    offset += info->codeSize;
    memcpy(p + offset, info->pName, names);
    offset += names;
    for (size_t i = 0; i < maps; ++i) {
        const VkSpecializationMapEntry *entry = &info->pSpecializationInfo->pMapEntries[i];
        put(p + offset, 0, entry->constantID);
        put(p + offset, 1, entry->offset);
        put_wide(p + offset, 2, entry->size);
        offset += 16;
    }
    if (data) memcpy(p + offset, info->pSpecializationInfo->pData, data);
    offset += data;
#ifdef VK_EXT_custom_resolve
    for (size_t i = 0; i < colors; ++i) put(p + offset, (unsigned)i, custom->pColorAttachmentFormats[i]);
#endif
    offset += colors * 4;
    if (interface_size) memcpy(p + offset, interface_bytes, interface_size);
done:
    vkrt_shader_interface_free_bytes(dev, interface_bytes);
    return result;
}
static VkResult source_blob(struct vkrt_device_data *dev, struct vkrt_shader_object_state *state, uint32_t count,
                            const VkShaderCreateInfoEXT *infos, uint32_t target, struct shader_record **stages,
                            struct shader_record *record) {
    uint32_t group_count = 0, group_target = 0;
    bool rq = false;
    uint32_t begin = (infos[target].flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT) ? 0 : target;
    uint32_t end = (infos[target].flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT) ? count : target + 1;
    for (uint32_t i = begin; i < end; ++i)
        if (same_group(infos, target, i)) rq |= rq_source(&infos[i]);
    size_t size = HEADER;
    for (uint32_t i = begin; i < end; ++i) {
        if (!same_group(infos, target, i)) continue;
        if (infos[i].codeType != VK_SHADER_CODE_TYPE_SPIRV_EXT) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
        if (!stages[i]) {
            stages[i] = new_record(device_allocator(dev));
            if (!stages[i]) return VK_ERROR_OUT_OF_HOST_MEMORY;
            VkResult result = stage_blob(dev, stages[i], &infos[i], rq);
            if (result != VK_SUCCESS) return result;
        }
        if (!add(&size, stages[i]->size)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (i == target) group_target = group_count;
        ++group_count;
    }
    record->blob = allocate(record, size);
    if (!record->blob) return VK_ERROR_OUT_OF_HOST_MEMORY;
    record->size = size;
    memset(record->blob, 0, HEADER);
    header(state, record->blob, size, group_count, group_target, rq);
    size_t offset = HEADER;
    for (uint32_t i = begin; i < end; ++i)
        if (same_group(infos, target, i)) {
            memcpy(record->blob + offset, stages[i]->blob, stages[i]->size);
            offset += stages[i]->size;
        }
    put_wide(record->blob, W_HASH, checksum(record->blob, size));
    return VK_SUCCESS;
}
static void group_finish(struct vkrt_device_data *dev, struct group *group) {
    for (uint32_t i = 0; group->stages && i < group->count; ++i) {
        struct decoded *stage = &group->stages[i];
        free(stage->maps);
        free(stage->colors);
        free(stage->rewritten);
        free(stage->ranges);
        vkrt_shader_interface_destroy(dev, stage->owned_interface);
    }
    free(group->stages);
    memset(group, 0, sizeof(*group));
}
static bool envelope(const VkShaderCreateInfoEXT *info) {
    return info->codeSize >= 8 && word(info->pCode, 0) == 0x54524b56 && word(info->pCode, 1) == 0x31444853;
}
static VkResult decode_stage(const uint8_t *p, size_t available, bool rebuildable, struct decoded *decoded, size_t *used) {
    if (available < STAGE_HEADER) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    uint64_t total = wide(p, S_TOTAL), code = wide(p, S_CODE), data = wide(p, S_DATA), interface_bytes_size = wide(p, S_INTERFACE);
    uint32_t names = word(p, S_NAME), maps = word(p, S_MAPS), colors = word(p, S_COLORS);
    size_t expected = STAGE_HEADER;
    if (total > available || total > SIZE_MAX || (total & 15) || code > SIZE_MAX || data > SIZE_MAX ||
        interface_bytes_size > SIZE_MAX || (uint64_t)maps * 16 > SIZE_MAX || (uint64_t)colors * 4 > SIZE_MAX ||
        !add(&expected, (size_t)code) || !add(&expected, names) || !add(&expected, (size_t)maps * 16) ||
        !add(&expected, (size_t)data) || !add(&expected, (size_t)colors * 4) || !add(&expected, (size_t)interface_bytes_size) ||
        expected > total || !code || code < 20 || code % 4 || !names || word(p + STAGE_HEADER, 0) != 0x07230203 ||
        p[STAGE_HEADER + (size_t)code + names - 1] || memchr(p + STAGE_HEADER + (size_t)code, 0, names - 1) || word(p, S_RQ) > 1 ||
        word(p, S_CUSTOM) > 1 || word(p, S_CUSTOM_ENABLE) > 1 || word(p, S_REBUILDABLE) != (uint32_t)rebuildable || word(p, 21) ||
        word(p, 22) || word(p, 23) ||
        (!word(p, S_CUSTOM) && (colors || word(p, S_CUSTOM_ENABLE) || word(p, S_DEPTH) || word(p, S_STENCIL))) ||
        (rebuildable ? !interface_bytes_size : (interface_bytes_size || word(p, S_SUBGROUP) || word(p, S_CUSTOM))))
        return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    for (size_t i = expected; i < (size_t)total; ++i)
        if (p[i]) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    if (!aligned_size(&expected) || expected != total ||
        !vkrt_spirv_validate_source((const uint32_t *)(p + STAGE_HEADER), (size_t)code / 4))
        return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
#ifndef VK_EXT_custom_resolve
    if (word(p, S_CUSTOM)) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
#endif
    if (maps) {
        decoded->maps = calloc(maps, sizeof(*decoded->maps));
        if (!decoded->maps) return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    const uint8_t *map = p + STAGE_HEADER + (size_t)code + names;
    for (uint32_t i = 0; i < maps; ++i) {
        uint64_t entry_size = wide(map + (size_t)i * 16, 2);
        uint32_t offset = word(map + (size_t)i * 16, 1), id = word(map + (size_t)i * 16, 0);
        if (entry_size > SIZE_MAX || offset >= data || entry_size > data - offset) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
        for (uint32_t j = 0; j < i; ++j)
            if (decoded->maps[j].constantID == id) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
        decoded->maps[i] = (VkSpecializationMapEntry){id, offset, (size_t)entry_size};
    }
    decoded->specialization = (VkSpecializationInfo){maps, decoded->maps, (size_t)data, map + (size_t)maps * 16};
    decoded->info = (VkShaderCreateInfoEXT){.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                            .stage = (VkShaderStageFlagBits)word(p, S_STAGE),
                                            .nextStage = word(p, S_NEXT),
                                            .flags = word(p, S_FLAGS),
                                            .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                            .pCode = p + STAGE_HEADER,
                                            .codeSize = (size_t)code,
                                            .pName = (const char *)p + STAGE_HEADER + (size_t)code,
                                            .pSpecializationInfo = maps || data ? &decoded->specialization : NULL};
    if (decoded->info.stage & VKRT_RTP_SHADER_STAGES || decoded->info.nextStage & VKRT_RTP_SHADER_STAGES ||
        (uint32_t)rq_source(&decoded->info) != word(p, S_RQ) || !matching_entry(&decoded->info) ||
        !valid_source_fields(&decoded->info))
        return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    if (rebuildable) {
        VkResult result = vkrt_spirv_validate_specialized_source(decoded->info.pCode, decoded->info.codeSize / 4,
                                                                 decoded->info.pSpecializationInfo);
        if (result != VK_SUCCESS) return result;
    }
    const uint8_t *formats = map + (size_t)maps * 16 + (size_t)data;
#ifdef VK_EXT_custom_resolve
    if (word(p, S_CUSTOM)) {
        if (colors) {
            decoded->colors = calloc(colors, sizeof(*decoded->colors));
            if (!decoded->colors) return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (uint32_t i = 0; i < colors; ++i) decoded->colors[i] = (VkFormat)word(formats, i);
        decoded->custom = (VkCustomResolveCreateInfoEXT){VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT,
                                                         NULL,
                                                         word(p, S_CUSTOM_ENABLE),
                                                         colors,
                                                         decoded->colors,
                                                         (VkFormat)word(p, S_DEPTH),
                                                         (VkFormat)word(p, S_STENCIL)};
        decoded->info.pNext = &decoded->custom;
    }
#endif
    if (word(p, S_SUBGROUP)) {
        decoded->subgroup = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, decoded->info.pNext, word(p, S_SUBGROUP)};
        decoded->info.pNext = &decoded->subgroup;
    }
    decoded->interface_bytes = formats + (size_t)colors * 4;
    decoded->interface_size = (size_t)interface_bytes_size;
    if (rebuildable) {
        VkResult result = vkrt_shader_interface_validate(decoded->interface_bytes, decoded->interface_size);
        if (result != VK_SUCCESS) return result;
    }
    *used = (size_t)total;
    return VK_SUCCESS;
}
static VkResult decode(struct vkrt_shader_object_state *state, const VkShaderCreateInfoEXT *info, struct group *group) {
    const uint8_t *p = info->pCode;
    size_t size = info->codeSize;
    if (size < HEADER || word(p, 0) != 0x54524b56 || word(p, 1) != 0x31444853 || word(p, 2) != FORMAT || word(p, 3) != SOURCE ||
        wide(p, W_TOTAL) != size || wide(p, W_HASH) != checksum(p, size) || memcmp(p + 96, state->uuid, VK_UUID_SIZE) ||
        word(p, W_NATIVE_VERSION) > state->native.shaderBinaryVersion || word(p, W_ELIGIBLE) != (uint32_t)state->eligible ||
        word(p, W_RQ) > 1 || word(p, 30) || word(p, 31))
        return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    for (unsigned i = 10; i < 21; ++i)
        if (word(p, i)) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    uint32_t count = word(p, W_GROUP), target = word(p, W_TARGET);
    uint64_t opaque = wide(p, W_OPAQUE);
    /* Seven graphics-stage bits exist in this recipe version. A linked group
     * must have unique stages; unlinked records always have one stage. */
    if (!count || count > 7 || target >= count || opaque > size - HEADER ||
        count > (size - HEADER - (size_t)opaque) / STAGE_HEADER || (word(p, W_RQ) ? opaque != 0 : opaque == 0))
        return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    group->count = count;
    group->target = target;
    group->rq = word(p, W_RQ) != 0;
    group->stages = calloc(count, sizeof(*group->stages));
    if (!group->stages) return VK_ERROR_OUT_OF_HOST_MEMORY;
    size_t offset = HEADER, end = size - (size_t)opaque;
    bool actual_rq = false;
    for (uint32_t i = 0; i < count; ++i) {
        size_t used;
        VkResult result = decode_stage(p + offset, end - offset, group->rq, &group->stages[i], &used);
        if (result != VK_SUCCESS) return result;
        actual_rq |= rq_source(&group->stages[i].info);
        offset += used;
    }
    if (offset != end || actual_rq != group->rq || group->stages[target].info.stage != info->stage)
        return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    if (count > 1) {
        VkShaderStageFlags unique = 0;
        bool mesh = false, classic = false;
        for (uint32_t i = 0; i < count; ++i) {
            const VkShaderCreateInfoEXT *stage = &group->stages[i].info;
            if (!(stage->flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT) || stage->stage == VK_SHADER_STAGE_COMPUTE_BIT ||
                (unique & stage->stage))
                return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
            unique |= stage->stage;
            mesh |= (stage->stage & (VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)) != 0;
            classic |= (stage->stage & (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT)) != 0;
            unsigned order = stage_order(stage->stage), following = 6;
            VkShaderStageFlags expected_next = 0;
            for (uint32_t j = 0; j < count; ++j) {
                unsigned candidate = stage_order(group->stages[j].info.stage);
                if (candidate > order && candidate < following) {
                    following = candidate;
                    expected_next = group->stages[j].info.stage;
                }
            }
            if (expected_next && stage->nextStage != expected_next) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
        }
        if (mesh && classic) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    }
    group->opaque = p + end;
    group->opaque_size = (size_t)opaque;
    return VK_SUCCESS;
}
static struct shader_record *find(struct vkrt_shader_object_state *state, VkShaderEXT shader) {
    for (struct shader_record *p = state->records; p; p = p->next)
        if (p->shader == shader) return p;
    return NULL;
}
static VKAPI_ATTR VkResult VKAPI_CALL GetShaderBinaryDataEXT(VkDevice device, VkShaderEXT shader, size_t *size, void *data) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !dev->shader_objects) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct vkrt_shader_object_state *state = dev->shader_objects;
    vkrt_mutex_lock(&state->lock);
    struct shader_record *record = find(state, shader);
    VkResult result = record && record->blob ? VK_SUCCESS : VK_ERROR_UNKNOWN;
    if (result == VK_SUCCESS) {
        if (!data)
            *size = record->size;
        else if (*size < record->size) {
            *size = 0;
            result = VK_INCOMPLETE;
        } else {
            memcpy(data, record->blob, record->size);
            *size = record->size;
        }
    }
    vkrt_mutex_unlock(&state->lock);
    return result;
}
static VKAPI_ATTR void VKAPI_CALL DestroyShaderEXT(VkDevice device, VkShaderEXT shader, const VkAllocationCallbacks *allocator) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev) return;
    if (!dev->shader_objects) {
        if (dev->vtable.DestroyShaderEXT) dev->vtable.DestroyShaderEXT(device, shader, allocator);
        return;
    }
    struct vkrt_shader_object_state *state = dev->shader_objects;
    vkrt_mutex_lock(&state->lock);
    /* Native handle reuse and canonical-cache retirement are one transaction.
     * A concurrent creation must not merge into a record for a retired handle. */
    if (dev->vtable.DestroyShaderEXT) dev->vtable.DestroyShaderEXT(device, shader, allocator);
    struct shader_record **link = &state->records;
    while (*link && (*link)->shader != shader) link = &(*link)->next;
    struct shader_record *record = *link;
    if (record && --record->references == 0) {
        *link = record->next;
        free_record(record);
    }
    vkrt_mutex_unlock(&state->lock);
}
static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *properties) {
    struct vkrt_instance_data *inst = VKRT_FIND(struct vkrt_instance_data, physical);
    if (!inst || !inst->vtable.GetPhysicalDeviceProperties2) return;
    bool requested = false;
    for (VkBaseOutStructure *p = properties->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT) {
            VkPhysicalDeviceShaderObjectPropertiesEXT *shader = (void *)p;
            memset(shader->shaderBinaryUUID, 0, VK_UUID_SIZE);
            shader->shaderBinaryVersion = 0;
            requested = true;
        }
    inst->vtable.GetPhysicalDeviceProperties2(physical, properties);
    if (!requested) return;
    bool eligible = vkrt_native_backend_eligible(inst, physical);
    for (VkBaseOutStructure *p = properties->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT) {
            VkPhysicalDeviceShaderObjectPropertiesEXT *shader = (void *)p;
            uint8_t transformed[VK_UUID_SIZE];
            uuid(shader->shaderBinaryUUID, eligible, transformed);
            memcpy(shader->shaderBinaryUUID, transformed, VK_UUID_SIZE);
        }
}

static VkResult translate_push_ranges(struct vkrt_device_data *dev, VkShaderCreateInfoEXT *info, VkPushConstantRange **owned) {
    bool needed = false;
    for (uint32_t i = 0; dev->virtual_rt && i < info->pushConstantRangeCount; ++i)
        needed |= info->pPushConstantRanges[i].stageFlags != VK_SHADER_STAGE_ALL &&
                  (info->pPushConstantRanges[i].stageFlags & VKRT_RTP_SHADER_STAGES) != 0;
    if (!needed) return VK_SUCCESS;
    *owned = calloc(info->pushConstantRangeCount, sizeof(**owned));
    if (!*owned) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t total = 0;
    for (uint32_t i = 0; i < info->pushConstantRangeCount; ++i) {
        VkPushConstantRange range = info->pPushConstantRanges[i];
        if (range.stageFlags != VK_SHADER_STAGE_ALL) range.stageFlags &= ~VKRT_RTP_SHADER_STAGES;
        if (range.stageFlags) (*owned)[total++] = range;
    }
    info->pPushConstantRanges = *owned;
    info->pushConstantRangeCount = total;
    return VK_SUCCESS;
}
static VkResult ensure_resolver(struct vkrt_device_data *dev) {
    if (!dev->shader_object_backend) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct vkrt_shader_object_state *state = dev->shader_objects;
    vkrt_mutex_lock(&state->lock);
    VkResult result = dev->as_registry ? VK_SUCCESS : vkrt_as_registry_init(dev, device_allocator(dev));
    vkrt_mutex_unlock(&state->lock);
    return result;
}
static VkResult rewrite_source(struct vkrt_device_data *dev, VkShaderCreateInfoEXT *info, void **owned) {
    bool rq = rq_source(info);
    if (rq) {
        VkResult result = ensure_resolver(dev);
        if (result != VK_SUCCESS) return result;
    }
    if (!dev->virtual_rt && !rq) return VK_SUCCESS;
    VkShaderModuleCreateInfo source = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = info->codeSize,
                                       .pCode = info->pCode},
                             rewritten;
    bool lowered_rq, rt;
    VkResult result = vkrt_rewrite_shader_module(dev, &source, &rewritten, &lowered_rq, &rt);
    if (rewritten.pCode != source.pCode) *owned = (void *)rewritten.pCode;
    if (result != VK_SUCCESS || rt) return result == VK_SUCCESS ? VK_ERROR_FEATURE_NOT_PRESENT : result;
    info->pCode = rewritten.pCode;
    info->codeSize = rewritten.codeSize;
    if (rq_source(info)) return VK_ERROR_FEATURE_NOT_PRESENT;
    return VK_SUCCESS;
}
static VkResult native_binary(struct vkrt_device_data *dev, struct shader_record *owner, VkShaderEXT shader, uint8_t **bytes,
                              size_t *size) {
    *bytes = NULL;
    *size = 0;
    if (!dev->vtable.GetShaderBinaryDataEXT) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult result = dev->vtable.GetShaderBinaryDataEXT(dev->device, shader, size, NULL);
    if (result != VK_SUCCESS) return result == VK_INCOMPLETE ? VK_ERROR_UNKNOWN : result;
    if (!*size) return VK_ERROR_UNKNOWN;
    uint8_t *data = allocate(owner, *size);
    if (!data) return VK_ERROR_OUT_OF_HOST_MEMORY;
    size_t capacity = *size;
    result = dev->vtable.GetShaderBinaryDataEXT(dev->device, shader, size, data);
    if (result != VK_SUCCESS || !*size || *size > capacity) {
        release(owner, data);
        return result == VK_SUCCESS || result == VK_INCOMPLETE ? VK_ERROR_UNKNOWN : result;
    }
    *bytes = data;
    return VK_SUCCESS;
}
static VkResult append_opaque(struct shader_record *record, const void *bytes, size_t amount) {
    size_t size = record->size;
    if (!amount || !add(&size, amount)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint8_t *copy = allocate(record, size);
    if (!copy) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(copy, record->blob, record->size);
    memcpy(copy + record->size, bytes, amount);
    put_wide(copy, W_TOTAL, size);
    put_wide(copy, W_OPAQUE, amount);
    put_wide(copy, W_HASH, checksum(copy, size));
    release(record, record->blob);
    record->blob = copy;
    record->size = size;
    return VK_SUCCESS;
}
static VkResult original_group(struct vkrt_device_data *dev, uint32_t count, const VkShaderCreateInfoEXT *infos, uint32_t target,
                               struct group *group) {
    uint32_t begin = (infos[target].flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT) ? 0 : target;
    uint32_t end = (infos[target].flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT) ? count : target + 1;
    for (uint32_t i = begin; i < end; ++i)
        if (same_group(infos, target, i)) ++group->count;
    group->stages = calloc(group->count, sizeof(*group->stages));
    if (!group->stages) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t index = 0;
    for (uint32_t i = begin; i < end; ++i)
        if (same_group(infos, target, i)) {
            struct decoded *stage = &group->stages[index];
            if (i == target) group->target = index;
            stage->info = infos[i];
            VkResult result = translate_push_ranges(dev, &stage->info, &stage->ranges);
            if (result != VK_SUCCESS) return result;
            ++index;
        }
    return VK_SUCCESS;
}
static VkResult compile_group(struct vkrt_device_data *dev, struct group *group, struct shader_record *owner, uint8_t **binary,
                              size_t *binary_size) {
    if (!dev->vtable.GetShaderBinaryDataEXT || !dev->vtable.DestroyShaderEXT) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (group->rq && !dev->shader_object_backend) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkShaderCreateInfoEXT *infos = calloc(group->count, sizeof(*infos));
    VkShaderEXT *shaders = calloc(group->count, sizeof(*shaders));
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!infos || !shaders) goto done;
    if (group->rq)
        for (uint32_t i = 0; i < group->count; ++i) {
            result = vkrt_shader_interface_source_supported(dev, &group->stages[i].info);
            if (result != VK_SUCCESS) goto done;
        }
    for (uint32_t i = 0; i < group->count; ++i) {
        struct decoded *stage = &group->stages[i];
        if (group->rq) {
            result = vkrt_shader_interface_materialize(dev, stage->interface_bytes, stage->interface_size, &stage->owned_interface);
            if (result != VK_SUCCESS) goto done;
            vkrt_shader_interface_apply(stage->owned_interface, &stage->info);
        }
        result = rewrite_source(dev, &stage->info, &stage->rewritten);
        if (result != VK_SUCCESS) goto done;
        infos[i] = stage->info;
    }
    /* Compile complete original linked groups, not an isolated exported stage.
     * Temporary handles may alias public/native objects; retire each returned
     * native reference directly without touching wrapper cache references. */
    struct vkrt_shader_object_state *state = dev->shader_objects;
    vkrt_mutex_lock(&state->lock);
    result = dev->vtable.CreateShadersEXT(dev->device, group->count, infos, device_allocator(dev), shaders);
    if (result == VK_SUCCESS) {
        if (!shaders[group->target])
            result = VK_ERROR_UNKNOWN;
        else
            result = native_binary(dev, owner, shaders[group->target], binary, binary_size);
    }
    for (uint32_t i = 0; i < group->count; ++i)
        if (shaders[i]) dev->vtable.DestroyShaderEXT(dev->device, shaders[i], device_allocator(dev));
    vkrt_mutex_unlock(&state->lock);
done:
    free(infos);
    free(shaders);
    return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL CreateShadersEXT(VkDevice device, uint32_t count, const VkShaderCreateInfoEXT *infos,
                                                       const VkAllocationCallbacks *allocator, VkShaderEXT *shaders) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    for (uint32_t i = 0; i < count; ++i) shaders[i] = VK_NULL_HANDLE;
    if (!dev || !dev->vtable.CreateShadersEXT) return dev ? VK_ERROR_FEATURE_NOT_PRESENT : VK_ERROR_INITIALIZATION_FAILED;
    if (!dev->shader_objects) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct vkrt_shader_object_state *state = dev->shader_objects;
    VkShaderCreateInfoEXT *native = calloc(count, sizeof(*native));
    void **codes = calloc(count, sizeof(*codes));
    VkPushConstantRange **ranges = calloc(count, sizeof(*ranges));
    struct shader_record **records = calloc(count, sizeof(*records)), **stages = calloc(count, sizeof(*stages));
    bool bridge = false, changed = dev->virtual_rt;
    for (uint32_t i = 0; i < count; ++i) bridge |= infos[i].codeType == VK_SHADER_CODE_TYPE_BINARY_EXT;
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY, pending = VK_SUCCESS;
    uint32_t prepared = 0;
    if (count && (!native || !codes || !ranges || !records || !stages)) goto cleanup;
    for (uint32_t i = 0; i < count; ++i) {
        struct group group = {0};
        native[i] = infos[i];
        records[i] = new_record(device_allocator(dev));
        result = records[i] ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
        if (result == VK_SUCCESS && ((infos[i].stage | infos[i].nextStage) & VKRT_RTP_SHADER_STAGES))
            result = VK_ERROR_FEATURE_NOT_PRESENT;
        if (result == VK_SUCCESS && infos[i].codeType == VK_SHADER_CODE_TYPE_BINARY_EXT) {
            result = envelope(&infos[i]) ? decode(state, &infos[i], &group) : VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
            if (result == VK_SUCCESS) {
                records[i]->blob = allocate(records[i], infos[i].codeSize);
                if (!records[i]->blob)
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                else {
                    memcpy(records[i]->blob, infos[i].pCode, infos[i].codeSize);
                    records[i]->size = infos[i].codeSize;
                }
            }
            if (result == VK_SUCCESS) {
                if (group.rq) {
                    size_t binary_size;
                    result = compile_group(dev, &group, records[i], (uint8_t **)&codes[i], &binary_size);
                    if (result == VK_SUCCESS) {
                        native[i].pCode = codes[i];
                        native[i].codeSize = binary_size;
                    }
                } else {
                    native[i].pCode = group.opaque;
                    native[i].codeSize = group.opaque_size;
                }
            }
            changed = true;
        } else if (result == VK_SUCCESS) {
            result = source_blob(dev, state, count, infos, i, stages, records[i]);
            if (result == VK_SUCCESS && bridge) {
                if (word(records[i]->blob, W_RQ)) {
                    VkShaderCreateInfoEXT source = infos[i];
                    source.pCode = records[i]->blob;
                    source.codeSize = records[i]->size;
                    result = decode(state, &source, &group);
                } else
                    result = original_group(dev, count, infos, i, &group);
                size_t binary_size = 0;
                if (result == VK_SUCCESS) result = compile_group(dev, &group, records[i], (uint8_t **)&codes[i], &binary_size);
                if (result == VK_SUCCESS) {
                    native[i].codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
                    native[i].pCode = codes[i];
                    native[i].codeSize = binary_size;
                    if (!group.rq) result = append_opaque(records[i], codes[i], binary_size);
                }
                changed = true;
            } else if (result == VK_SUCCESS)
                result = rewrite_source(dev, &native[i], &codes[i]);
        }
        group_finish(dev, &group);
        if (result == VK_SUCCESS) result = translate_push_ranges(dev, &native[i], &ranges[i]);
        if (result != VK_SUCCESS) {
            pending = result;
            break;
        }
        ++prepared;
    }
    /* Keep independent successes before the failed element or linked group.
     * The first NULL identifies that failure unit. Every final bridge element
     * is BINARY, including SOURCE peers, but linked adjacency still applies. */
    if (pending != VK_SUCCESS) prepared = linked_prefix(prepared, native);
    if (!prepared) {
        result = pending;
        goto cleanup;
    }
    vkrt_mutex_lock(&state->lock);
    result = dev->vtable.CreateShadersEXT(device, prepared, changed ? native : infos, allocator, shaders);
    for (uint32_t i = 0; i < prepared; ++i) {
        if (!shaders[i] || find(state, shaders[i])) continue;
        if (infos[i].codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT && !word(records[i]->blob, W_RQ) &&
            !wide(records[i]->blob, W_OPAQUE)) {
            uint8_t *binary = NULL;
            size_t binary_size = 0;
            VkResult cached = native_binary(dev, records[i], shaders[i], &binary, &binary_size);
            if (cached == VK_SUCCESS) cached = append_opaque(records[i], binary, binary_size);
            release(records[i], binary);
            if (cached != VK_SUCCESS) {
                /* Nothing from this failed cache slot or its tail is exposed.
                 * Earlier complete envelopes and native partial results survive. */
                for (uint32_t j = i; j < prepared; ++j)
                    if (shaders[j]) {
                        dev->vtable.DestroyShaderEXT(device, shaders[j], allocator);
                        shaders[j] = VK_NULL_HANDLE;
                    }
                if (result == VK_SUCCESS) result = cached;
                break;
            }
        }
    }
    for (uint32_t i = 0; i < prepared; ++i)
        if (shaders[i]) {
            struct shader_record *existing = find(state, shaders[i]);
            if (existing)
                ++existing->references;
            else {
                records[i]->shader = shaders[i];
                records[i]->next = state->records;
                state->records = records[i];
                records[i] = NULL;
            }
        }
    vkrt_mutex_unlock(&state->lock);
    if (result == VK_SUCCESS && pending != VK_SUCCESS) result = pending;
cleanup:
    for (uint32_t i = 0; i < count; ++i) {
        if (codes && codes[i]) {
            if (bridge && records && records[i])
                release(records[i], codes[i]);
            else if (bridge) {
                const VkAllocationCallbacks *owner = device_allocator(dev);
                if (owner)
                    owner->pfnFree(owner->pUserData, codes[i]);
                else
                    free(codes[i]);
            } else
                free(codes[i]);
        }
        if (ranges) free(ranges[i]);
        if (records) free_record(records[i]);
        if (stages) free_record(stages[i]);
    }
    free(ranges);
    free(codes);
    free(native);
    free(records);
    free(stages);
    return result;
}

PFN_vkVoidFunction vkrt_shader_object_proc(const char *name) {
    if (!strcmp(name, "vkCreateShadersEXT")) return (PFN_vkVoidFunction)CreateShadersEXT;
    if (!strcmp(name, "vkGetShaderBinaryDataEXT")) return (PFN_vkVoidFunction)GetShaderBinaryDataEXT;
    if (!strcmp(name, "vkDestroyShaderEXT")) return (PFN_vkVoidFunction)DestroyShaderEXT;
    return NULL;
}
PFN_vkVoidFunction vkrt_shader_object_instance_proc(const char *name) {
    return !strcmp(name, "vkGetPhysicalDeviceProperties2") || !strcmp(name, "vkGetPhysicalDeviceProperties2KHR")
               ? (PFN_vkVoidFunction)GetPhysicalDeviceProperties2
               : NULL;
}
