/* Copyright (c) 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT */
#include "vkrt_shader_objects.h"
#include "vkrt_layer.h"
#include "vkrt_rt_capabilities.h"
#include "vkrt_as_registry.h"
#include "vkrt_spirv_lower.h"

/* The wire format contains no host pointers, Vulkan handles, or resolver BDA.
 * Source records are recompiled against the destination logical device. */
enum { HEADER = 128, FORMAT = 1, SOURCE = 1 };
enum {
    W_TOTAL = 4,
    W_CODE = 6,
    W_NAME = 8,
    W_MAPS = 9,
    W_DATA = 10,
    W_SUBGROUP = 12,
    W_CUSTOM = 13,
    W_CUSTOM_ENABLE = 14,
    W_COLORS = 15,
    W_DEPTH = 16,
    W_STENCIL = 17,
    W_STAGE = 18,
    W_NEXT = 19,
    W_FLAGS = 20,
    W_ELIGIBLE = 21,
    W_NATIVE_VERSION = 22,
    W_RQ = 23,
    W_HASH = 28
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
    VkSpecializationInfo specialization;
    VkSpecializationMapEntry *maps;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup;
#ifdef VK_EXT_custom_resolve
    VkCustomResolveCreateInfoEXT custom;
#endif
    VkFormat *colors;
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
static void header(struct vkrt_shader_object_state *state, uint8_t *p, unsigned kind, size_t size, size_t code) {
    put(p, 0, 0x54524b56);
    put(p, 1, 0x31444853);
    put(p, 2, FORMAT);
    put(p, 3, kind);
    put_wide(p, W_TOTAL, size);
    put_wide(p, W_CODE, code);
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
static bool rq_source(const VkShaderCreateInfoEXT *info) {
    const uint32_t *p = info->pCode;
    size_t count = info->codeSize / 4;
    if (count < 5 || p[0] != 0x07230203) return false;
    for (size_t i = 5; i < count;) {
        uint32_t length = p[i] >> 16, op = p[i] & 65535;
        if (!length || length > count - i) return false;
        if (op == 4472 || op == 5341) return true; /* RayQuery / AS types */
        i += length;
    }
    return false;
}
static VkResult source_blob(struct vkrt_shader_object_state *state, struct shader_record *record, const VkShaderCreateInfoEXT *info,
                            bool rq) {
    uint32_t subgroup = 0;
#ifdef VK_EXT_custom_resolve
    const VkCustomResolveCreateInfoEXT *custom = NULL;
#endif
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
    size_t names = strlen(info->pName) + 1, maps = info->pSpecializationInfo ? info->pSpecializationInfo->mapEntryCount : 0;
    size_t data = info->pSpecializationInfo ? info->pSpecializationInfo->dataSize : 0, colors = 0;
#ifdef VK_EXT_custom_resolve
    colors = custom ? custom->colorAttachmentCount : 0;
#endif
    size_t size = HEADER;
    if (names > UINT32_MAX || maps > SIZE_MAX / 16 || colors > SIZE_MAX / 4 || !add(&size, info->codeSize) || !add(&size, names) ||
        !add(&size, maps * 16) || !add(&size, data) || !add(&size, colors * 4))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    record->blob = allocate(record, size);
    if (!record->blob) return VK_ERROR_OUT_OF_HOST_MEMORY;
    record->size = size;
    memset(record->blob, 0, size);
    uint8_t *p = record->blob;
    header(state, p, SOURCE, size, info->codeSize);
    put(p, W_NAME, (uint32_t)names);
    put(p, W_MAPS, (uint32_t)maps);
    put_wide(p, W_DATA, data);
    put(p, W_SUBGROUP, subgroup);
#ifdef VK_EXT_custom_resolve
    put(p, W_CUSTOM, custom != NULL);
#endif
    put(p, W_RQ, rq);
    put(p, W_STAGE, info->stage);
    put(p, W_NEXT, info->nextStage);
    put(p, W_FLAGS, info->flags);
#ifdef VK_EXT_custom_resolve
    if (custom) {
        put(p, W_CUSTOM_ENABLE, custom->customResolve);
        put(p, W_COLORS, (uint32_t)colors);
        put(p, W_DEPTH, custom->depthAttachmentFormat);
        put(p, W_STENCIL, custom->stencilAttachmentFormat);
    }
#endif
    size_t offset = HEADER;
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
    put_wide(p, W_HASH, checksum(p, size));
    return VK_SUCCESS;
}

static bool envelope(const VkShaderCreateInfoEXT *info) {
    return info->codeSize >= 8 && word(info->pCode, 0) == 0x54524b56 && word(info->pCode, 1) == 0x31444853;
}
static VkResult decode(struct vkrt_shader_object_state *state, const VkShaderCreateInfoEXT *info, struct shader_record *record,
                       struct decoded *decoded, VkShaderCreateInfoEXT *native) {
    const uint8_t *p = info->pCode;
    size_t size = info->codeSize;
    if (size < HEADER || word(p, 2) != FORMAT || wide(p, W_TOTAL) != size || wide(p, W_HASH) != checksum(p, size) ||
        memcmp(p + 24 * 4, state->uuid, VK_UUID_SIZE) || word(p, W_NATIVE_VERSION) > state->native.shaderBinaryVersion ||
        word(p, W_ELIGIBLE) != (uint32_t)state->eligible || word(p, 30) || word(p, 31))
        return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    uint64_t code = wide(p, W_CODE), data = wide(p, W_DATA);
    uint32_t names = word(p, W_NAME), maps = word(p, W_MAPS), colors = word(p, W_COLORS);
    size_t expected = HEADER;
    if (code > SIZE_MAX || data > SIZE_MAX || (uint64_t)maps * 16 > SIZE_MAX || (uint64_t)colors * 4 > SIZE_MAX ||
        !add(&expected, (size_t)code) || !add(&expected, names) || !add(&expected, (size_t)maps * 16) ||
        !add(&expected, (size_t)data) || !add(&expected, (size_t)colors * 4) || expected != size || !code)
        return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    unsigned kind = word(p, 3);
    if (kind == SOURCE) {
        if (code < 20 || code % 4 || word(p + HEADER, 0) != 0x07230203 || !names || p[HEADER + (size_t)code + names - 1] ||
            memchr(p + HEADER + (size_t)code, 0, names - 1) || word(p, W_STAGE) != (uint32_t)info->stage || word(p, W_RQ) > 1 ||
            word(p, W_CUSTOM) > 1 || (!word(p, W_CUSTOM) && colors))
            return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
        if (!vkrt_spirv_validate_source((const uint32_t *)(p + HEADER), (size_t)code / 4))
            return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
#ifndef VK_EXT_custom_resolve
        if (word(p, W_CUSTOM)) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
#endif
        if (maps) {
            decoded->maps = calloc(maps, sizeof(*decoded->maps));
            if (!decoded->maps) return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const uint8_t *map = p + HEADER + (size_t)code + names;
        for (uint32_t i = 0; i < maps; ++i) {
            uint64_t entry_size = wide(map + (size_t)i * 16, 2);
            uint32_t offset = word(map + (size_t)i * 16, 1);
            if (entry_size > SIZE_MAX || offset > data || entry_size > data - offset)
                return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
            decoded->maps[i] = (VkSpecializationMapEntry){word(map + (size_t)i * 16, 0), offset, (size_t)entry_size};
        }
        decoded->specialization = (VkSpecializationInfo){maps, decoded->maps, (size_t)data, map + (size_t)maps * 16};
        native->codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        native->pName = (const char *)p + HEADER + (size_t)code;
        native->pSpecializationInfo = maps || data ? &decoded->specialization : NULL;
        native->pNext = NULL;
#ifdef VK_EXT_custom_resolve
        if (word(p, W_CUSTOM)) {
            if (colors) {
                decoded->colors = calloc(colors, sizeof(*decoded->colors));
                if (!decoded->colors) return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            const uint8_t *formats = map + (size_t)maps * 16 + (size_t)data;
            for (uint32_t i = 0; i < colors; ++i) decoded->colors[i] = (VkFormat)word(formats, i);
            decoded->custom = (VkCustomResolveCreateInfoEXT){VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT,
                                                             NULL,
                                                             word(p, W_CUSTOM_ENABLE),
                                                             colors,
                                                             decoded->colors,
                                                             (VkFormat)word(p, W_DEPTH),
                                                             (VkFormat)word(p, W_STENCIL)};
            native->pNext = &decoded->custom;
        }
#endif
        if (word(p, W_SUBGROUP)) {
            decoded->subgroup = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, native->pNext, word(p, W_SUBGROUP)};
            native->pNext = &decoded->subgroup;
        }
    } else
        return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    record->blob = allocate(record, size);
    if (!record->blob) return VK_ERROR_OUT_OF_HOST_MEMORY;
    record->size = size;
    memcpy(record->blob, p, size);
    native->pCode = p + HEADER;
    native->codeSize = (size_t)code;
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

static VKAPI_ATTR VkResult VKAPI_CALL CreateShadersEXT(VkDevice device, uint32_t count, const VkShaderCreateInfoEXT *infos,
                                                       const VkAllocationCallbacks *allocator, VkShaderEXT *shaders) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !dev->vtable.CreateShadersEXT) {
        for (uint32_t i = 0; i < count; ++i) shaders[i] = VK_NULL_HANDLE;
        return dev ? VK_ERROR_FEATURE_NOT_PRESENT : VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i) shaders[i] = VK_NULL_HANDLE;
    if (!dev->shader_objects) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct vkrt_shader_object_state *state = dev->shader_objects;
    VkShaderCreateInfoEXT *native = calloc(count, sizeof(*native));
    void **codes = calloc(count, sizeof(*codes));
    VkPushConstantRange **ranges = calloc(count, sizeof(*ranges));
    struct shader_record **records = calloc(count, sizeof(*records));
    struct decoded *decoded = calloc(count, sizeof(*decoded));
    bool changed = dev->virtual_rt;
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (count && (!native || !codes || !ranges || !records || !decoded)) goto cleanup;
    for (uint32_t i = 0; i < count; ++i) {
        native[i] = infos[i];
        /* Native non-dispatchable handles may alias between creations. The
         * canonical binary must outlive every alias, so its owner is the
         * device (falling back to the instance allocator), never a shader's
         * allocation callbacks. Native OBJECT allocations remain native. */
        records[i] = new_record(device_allocator(dev));
        if (!records[i]) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto cleanup;
        }
        bool imported = infos[i].codeType == VK_SHADER_CODE_TYPE_BINARY_EXT && envelope(&infos[i]);
        if (imported) {
            result = decode(state, &infos[i], records[i], &decoded[i], &native[i]);
            if (result != VK_SUCCESS) goto cleanup;
            changed = true;
        } else if (infos[i].codeType == VK_SHADER_CODE_TYPE_BINARY_EXT) {
            result = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
            goto cleanup;
        }
        if ((dev->virtual_rt || imported) && ((infos[i].stage | infos[i].nextStage) & VKRT_RTP_SHADER_STAGES)) {
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            goto cleanup;
        }
        bool rq = native[i].codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT && rq_source(&native[i]);
        if (infos[i].codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT) {
            result = source_blob(state, records[i], &infos[i], rq);
            if (result != VK_SUCCESS) goto cleanup;
        }
        if (imported && rq) {
            if (!dev->shader_object_backend) {
                result = VK_ERROR_FEATURE_NOT_PRESENT;
                goto cleanup;
            }
            vkrt_mutex_lock(&state->lock);
            if (!dev->as_registry)
                result = vkrt_as_registry_init(dev, device_allocator(dev));
            else
                result = VK_SUCCESS;
            vkrt_mutex_unlock(&state->lock);
            if (result != VK_SUCCESS) goto cleanup;
        }
        if (native[i].codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT && (dev->virtual_rt || (imported && rq))) {
            VkShaderModuleCreateInfo source = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                               .codeSize = native[i].codeSize,
                                               .pCode = native[i].pCode},
                                     rewritten;
            bool lowered_rq, rt;
            result = vkrt_rewrite_shader_module(dev, &source, &rewritten, &lowered_rq, &rt);
            if (rewritten.pCode != source.pCode) codes[i] = (void *)rewritten.pCode;
            if (result != VK_SUCCESS || rt) {
                if (result == VK_SUCCESS) result = VK_ERROR_FEATURE_NOT_PRESENT;
                goto cleanup;
            }
            native[i].pCode = rewritten.pCode;
            native[i].codeSize = rewritten.codeSize;
            if (lowered_rq)
                VKRT_LOG_DBG("shader-object RQ resolver=%llu profile=%u import=%u",
                             (unsigned long long)vkrt_as_registry_address(dev), dev->virtual_rt, imported);
        }
        bool rewrite = false;
        for (uint32_t j = 0; dev->virtual_rt && j < infos[i].pushConstantRangeCount; ++j)
            rewrite |= infos[i].pPushConstantRanges[j].stageFlags != VK_SHADER_STAGE_ALL &&
                       (infos[i].pPushConstantRanges[j].stageFlags & VKRT_RTP_SHADER_STAGES) != 0;
        if (rewrite) {
            ranges[i] = calloc(infos[i].pushConstantRangeCount, sizeof(**ranges));
            if (!ranges[i]) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto cleanup;
            }
            uint32_t total = 0;
            for (uint32_t j = 0; j < infos[i].pushConstantRangeCount; ++j) {
                VkPushConstantRange range = infos[i].pPushConstantRanges[j];
                if (range.stageFlags != VK_SHADER_STAGE_ALL) range.stageFlags &= ~VKRT_RTP_SHADER_STAGES;
                if (range.stageFlags) ranges[i][total++] = range;
            }
            native[i].pPushConstantRanges = ranges[i];
            native[i].pushConstantRangeCount = total;
        }
    }
    /* Keep native batching, partial creation, callbacks and binary errors. All
     * handles stay native, including successful elements of a failing batch. */
    vkrt_mutex_lock(&state->lock);
    result = dev->vtable.CreateShadersEXT(device, count, changed ? native : infos, allocator, shaders);
    for (uint32_t i = 0; i < count; ++i)
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
cleanup:
    for (uint32_t i = 0; i < count; ++i) {
        if (codes) free(codes[i]);
        if (ranges) free(ranges[i]);
        if (records) free_record(records[i]);
        if (decoded) {
            free(decoded[i].maps);
            free(decoded[i].colors);
        }
    }
    free(ranges);
    free(codes);
    free(native);
    free(records);
    free(decoded);
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
