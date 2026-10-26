/* Copyright (c) 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_shader_objects.h"
#include "vkrt_shader_interface.h"
#include "vkrt_as_registry.h"
#include "vkrt_spirv_lower.h"
#include "vkrt_rt_capabilities.h"
#include <stdatomic.h>

/* Native shader compilation and SPIR-V rewriting are spies; the actual interface
 * codec/materializer is linked. Public GPU/CTS tests validate real SPIR-V. */
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
#define H(t, n) ((t)(uintptr_t)(n))
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            atomic_fetch_add(&failures, 1);                 \
        }                                                   \
    } while (0)
static atomic_uint failures, allocations, frees;
static atomic_int allocation_budget = -1;
static struct vkrt_device_data dev;
static struct vkrt_instance_data inst;
static bool eligible = true;
static unsigned native_calls, temporary_calls, final_calls, binary_gets, native_destroys, rewrites, registry_inits;
static unsigned layout_creates, layout_destroys, get_fail_size;
static unsigned malformed_count, allocation_failures, allocation_successes;
static VkResult temporary_result = VK_SUCCESS, final_result = VK_SUCCESS, binary_result = VK_SUCCESS;
static unsigned temporary_successes = UINT32_MAX, final_successes = UINT32_MAX;
static bool bridge, expect_temporary;
static bool leave_rt_dependency;
static VkResult layout_result = VK_SUCCESS;
static const VkShaderCreateInfoEXT *outer;
static uint32_t outer_count;
static VkShaderCreateInfoEXT saved_group[2];
static uint32_t saved_group_count, saved_specialization = 37;
static VkShaderEXT force_public, force_temporary;
static uint32_t force_temporary_index;
static const VkAllocationCallbacks *public_allocator;
static unsigned next_handle = 100, next_layout = 10000;
struct native_shader {
    VkShaderEXT handle;
    unsigned references;
    uint32_t words[4];
};
static struct native_shader native_shaders[2048];
static size_t native_shader_count;
#ifdef _WIN32
static bool gate_retirement;
static VkShaderEXT retiring;
static HANDLE retired, release_retirement, creating, native_creation;
#endif

static VKAPI_ATTR void *VKAPI_CALL allocate_device(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    CHECK(user == &dev && alignment <= 16 && scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    int budget = atomic_load(&allocation_budget);
    if (budget == 0) return NULL;
    if (budget > 0) atomic_fetch_sub(&allocation_budget, 1);
    void *p = malloc(size);
    if (p) atomic_fetch_add(&allocations, 1);
    return p;
}
static VKAPI_ATTR void VKAPI_CALL free_device(void *user, void *pointer) {
    CHECK(user == &dev);
    if (pointer) {
        atomic_fetch_add(&frees, 1);
        free(pointer);
    }
}
struct object_owner {
    bool live;
    unsigned calls;
};
static VKAPI_ATTR void *VKAPI_CALL allocate_object(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    struct object_owner *o = user;
    CHECK(o->live && alignment <= 16 && scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    ++o->calls;
    return malloc(size);
}
static VKAPI_ATTR void VKAPI_CALL free_object(void *user, void *pointer) {
    struct object_owner *o = user;
    CHECK(o->live);
    ++o->calls;
    free(pointer);
}
static struct native_shader *lookup(VkShaderEXT shader) {
    for (size_t i = 0; i < native_shader_count; ++i)
        if (native_shaders[i].handle == shader) return &native_shaders[i];
    CHECK(false);
    return NULL;
}
static unsigned live_native(void) {
    unsigned n = 0;
    for (size_t i = 0; i < native_shader_count; ++i) n += native_shaders[i].references;
    return n;
}
static unsigned live_allocations(void) { return atomic_load(&allocations) - atomic_load(&frees); }
static uint32_t read32(const void *data, size_t offset) {
    const uint8_t *p = (const uint8_t *)data + offset;
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t read64(const void *data, size_t offset) { return read32(data, offset) | (uint64_t)read32(data, offset + 4) << 32; }
static void write32(void *data, size_t offset, uint32_t value) {
    uint8_t *p = (uint8_t *)data + offset;
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static void write64(void *data, size_t offset, uint64_t value) {
    write32(data, offset, (uint32_t)value);
    write32(data, offset + 4, (uint32_t)(value >> 32));
}
static void seal(uint8_t *p, size_t size) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= i >= 112 && i < 120 ? 0 : p[i];
        hash *= UINT64_C(1099511628211);
    }
    write64(p, 112, hash);
}
bool vkrt_native_backend_eligible(struct vkrt_instance_data *instance, VkPhysicalDevice physical) {
    CHECK(instance == &inst && physical == dev.physical_device);
    return eligible;
}
VkResult vkrt_as_registry_init(struct vkrt_device_data *device, const VkAllocationCallbacks *allocator) {
    CHECK(device == &dev && allocator && allocator->pfnAllocation == allocate_device);
    ++registry_inits;
    device->as_registry = (void *)(uintptr_t)2;
    return VK_SUCCESS;
}
VkDeviceAddress vkrt_as_registry_address(struct vkrt_device_data *device) {
    CHECK(device == &dev);
    return 4096;
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *info) {
    CHECK(physical == dev.physical_device);
    for (VkBaseOutStructure *next = info->pNext; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT) {
            VkPhysicalDeviceShaderObjectPropertiesEXT *p = (void *)next;
            for (unsigned i = 0; i < VK_UUID_SIZE; ++i) p->shaderBinaryUUID[i] = (uint8_t)(i + 1);
            p->shaderBinaryVersion = 5;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES) {
            VkPhysicalDeviceSubgroupSizeControlProperties *p = (void *)next;
            p->minSubgroupSize = p->maxSubgroupSize = 32;
            p->maxComputeWorkgroupSubgroups = 32;
            p->requiredSubgroupSizeStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        } else
            CHECK(false);
    }
}
static bool has_query(const uint32_t *p, size_t count) {
    for (size_t i = 5; i < count;) {
        uint32_t n = p[i] >> 16;
        if (!n || n > count - i) return false;
        if ((p[i] & 65535) == 4472) return true;
        i += n;
    }
    return false;
}
int vkrt_spirv_validate_source(const uint32_t *p, size_t count) {
    if (count < 5 || p[0] != 0x07230203 || p[4]) return 0;
    for (size_t i = 5; i < count;) {
        uint32_t n = p[i] >> 16;
        if (!n || n > count - i) return 0;
        i += n;
    }
    return 1;
}
/* Compiler semantics are covered with real SPIRV-Tools in test_spirv_lower.
 * This fixture isolates the binary bridge and uses a source-validation spy. */
VkResult vkrt_spirv_validate_specialized_source(const uint32_t *p, size_t count, const VkSpecializationInfo *specialization) {
    (void)specialization;
    return vkrt_spirv_validate_source(p, count) ? VK_SUCCESS : VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
}
VkResult vkrt_rewrite_shader_module(struct vkrt_device_data *device, const VkShaderModuleCreateInfo *in,
                                    VkShaderModuleCreateInfo *out, bool *rq, bool *rt) {
    CHECK(device == &dev && !in->flags && !in->pNext);
    ++rewrites;
    *out = *in;
    *rt = false;
    *rq = has_query(in->pCode, in->codeSize / 4);
    if (*rq) {
        uint32_t *copy = malloc(in->codeSize);
        CHECK(copy != NULL);
        memcpy(copy, in->pCode, in->codeSize);
        copy[2] |= 0x10000000;
        size_t written = 5;
        for (size_t i = 5; i < in->codeSize / 4;) {
            uint32_t length = in->pCode[i] >> 16, op = in->pCode[i] & 65535;
            bool dependency = op == 4472 || (op == 17 && in->pCode[i + 1] == 4472) ||
                              (op == 10 && !strcmp((const char *)&in->pCode[i + 1], "SPV_KHR_ray_query"));
            if (leave_rt_dependency || !dependency) {
                memmove(copy + written, in->pCode + i, (size_t)length * 4);
                written += length;
            }
            i += length;
        }
        out->pCode = copy;
        out->codeSize = written * 4;
    }
    return VK_SUCCESS;
}
static uint32_t specialization(const VkShaderCreateInfoEXT *info) {
    return info->pSpecializationInfo && info->pSpecializationInfo->dataSize >= 4
               ? *(const uint32_t *)info->pSpecializationInfo->pData
               : 0;
}
static void check_outer(const VkShaderCreateInfoEXT *info, uint32_t i) {
    const VkShaderCreateInfoEXT *source = &outer[i];
    CHECK(info->stage == source->stage && info->nextStage == source->nextStage && info->flags == source->flags);
    CHECK(info->pName == source->pName && info->pSpecializationInfo == source->pSpecializationInfo && info->pNext == source->pNext);
    CHECK(info->setLayoutCount == source->setLayoutCount && info->pSetLayouts == source->pSetLayouts);
    uint32_t count = 0;
    for (uint32_t j = 0; j < source->pushConstantRangeCount; ++j) {
        VkPushConstantRange p = source->pPushConstantRanges[j];
        if (dev.virtual_rt && p.stageFlags != VK_SHADER_STAGE_ALL) p.stageFlags &= ~VKRT_RTP_SHADER_STAGES;
        if (p.stageFlags) {
            CHECK(count < info->pushConstantRangeCount);
            if (count < info->pushConstantRangeCount) CHECK(!memcmp(&p, &info->pPushConstantRanges[count], sizeof(p)));
            ++count;
        }
    }
    CHECK(info->pushConstantRangeCount == count);
}
static unsigned logical_stage(VkShaderStageFlagBits stage) {
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
static void check_linked_topology(uint32_t count, const VkShaderCreateInfoEXT *infos) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!(infos[i].flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT)) continue;
        unsigned following = 6;
        VkShaderStageFlags next = 0;
        for (uint32_t j = 0; j < count; ++j) {
            if ((infos[j].flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT) &&
                logical_stage(infos[j].stage) > logical_stage(infos[i].stage) && logical_stage(infos[j].stage) < following) {
                following = logical_stage(infos[j].stage);
                next = infos[j].stage;
            }
        }
        if (next) CHECK(infos[i].nextStage == next); /* VUID-08409, including native BINARY. */
    }
}
static VKAPI_ATTR VkResult VKAPI_CALL native_create(VkDevice device, uint32_t count, const VkShaderCreateInfoEXT *infos,
                                                    const VkAllocationCallbacks *allocator, VkShaderEXT *output) {
    CHECK(device == dev.device);
    check_linked_topology(count, infos);
    bool temporary = allocator && allocator->pfnAllocation == allocate_device;
    ++native_calls;
    if (temporary) {
        ++temporary_calls;
        CHECK(expect_temporary && count == saved_group_count);
        for (uint32_t i = 0; i < count; ++i) {
            CHECK(infos[i].codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT && !strcmp(infos[i].pName, "main"));
            CHECK(infos[i].stage == saved_group[i].stage && infos[i].flags == saved_group[i].flags &&
                  infos[i].nextStage == saved_group[i].nextStage);
            CHECK(((const uint32_t *)infos[i].pCode)[2] ==
                  (((const uint32_t *)saved_group[i].pCode)[2] |
                   (has_query(saved_group[i].pCode, saved_group[i].codeSize / 4) ? 0x10000000u : 0)));
            if (i == 1) {
                CHECK(specialization(&infos[i]) == saved_specialization);
                CHECK(infos[i].setLayoutCount == 1 && infos[i].pSetLayouts &&
                      infos[i].pSetLayouts[0] != H(VkDescriptorSetLayout, 3));
                CHECK(infos[i].pushConstantRangeCount == 1 &&
                      infos[i].pPushConstantRanges[0].stageFlags == VK_SHADER_STAGE_FRAGMENT_BIT &&
                      infos[i].pPushConstantRanges[0].offset == 4 && infos[i].pPushConstantRanges[0].size == 8);
                const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *subgroup = infos[i].pNext;
                CHECK(subgroup && subgroup->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO &&
                      subgroup->requiredSubgroupSize == 32);
            }
        }
    } else {
        ++final_calls;
        CHECK(allocator == public_allocator && count <= outer_count);
#ifdef _WIN32
        if (gate_retirement) SetEvent(native_creation);
#endif
        for (uint32_t i = 0; i < count; ++i) {
            check_outer(&infos[i], i);
            CHECK(infos[i].codeType == (bridge ? VK_SHADER_CODE_TYPE_BINARY_EXT : VK_SHADER_CODE_TYPE_SPIRV_EXT));
            if (bridge) {
                CHECK(infos[i].codeSize == 16 && !((uintptr_t)infos[i].pCode & 15));
                CHECK(read32(infos[i].pCode, 0) == 0x4e415449 && read32(infos[i].pCode, 4) == (uint32_t)infos[i].stage);
                if (infos[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    CHECK(read32(infos[i].pCode, 8) == (0x10000000u | 22) && read32(infos[i].pCode, 12) == 37);
            }
        }
    }
    VkResult result = temporary ? temporary_result : final_result;
    unsigned successes = temporary ? temporary_successes : final_successes;
    for (uint32_t i = 0; i < count; ++i) {
        output[i] = VK_NULL_HANDLE;
        if (i >= successes) continue;
        VkShaderEXT chosen =
            temporary ? (i == force_temporary_index ? force_temporary : VK_NULL_HANDLE) : (i == 0 ? force_public : VK_NULL_HANDLE);
        if (!chosen) chosen = H(VkShaderEXT, next_handle++);
        struct native_shader *record = NULL;
        for (size_t n = 0; n < native_shader_count; ++n)
            if (native_shaders[n].handle == chosen) record = &native_shaders[n];
        if (!record) {
            CHECK(native_shader_count < 2048);
            record = &native_shaders[native_shader_count++];
            memset(record, 0, sizeof(*record));
            record->handle = chosen;
        }
        uint32_t bytes[4] = {0x4e415449, infos[i].stage, 0, 0};
        if (infos[i].codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT) {
            bytes[2] = ((const uint32_t *)infos[i].pCode)[2];
            bytes[3] = specialization(&infos[i]);
        } else
            memcpy(bytes, infos[i].pCode, 16);
        if (record->references)
            CHECK(!memcmp(record->words, bytes, 16));
        else
            memcpy(record->words, bytes, 16);
        ++record->references;
        output[i] = chosen;
    }
    force_public = force_temporary = VK_NULL_HANDLE;
    return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_get(VkDevice device, VkShaderEXT shader, size_t *size, void *data) {
    CHECK(device == dev.device);
    ++binary_gets;
    struct native_shader *p = lookup(shader);
    CHECK(p && p->references);
    if (binary_result != VK_SUCCESS) return binary_result;
    if (!data) {
        *size = 16;
        if (binary_gets == get_fail_size) atomic_store(&allocation_budget, 0);
    } else {
        CHECK(!((uintptr_t)data & 15) && *size >= 16);
        memcpy(data, p->words, 16);
        *size = 16;
    }
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL native_destroy(VkDevice device, VkShaderEXT shader, const VkAllocationCallbacks *allocator) {
    CHECK(device == dev.device);
    if (!shader) return;
    CHECK(allocator == public_allocator || (allocator && allocator->pfnAllocation == allocate_device));
    if (allocator && allocator->pfnAllocation == allocate_object) CHECK(((struct object_owner *)allocator->pUserData)->live);
#ifdef _WIN32
    if (gate_retirement && shader == retiring) {
        SetEvent(retired);
        CHECK(WaitForSingleObject(release_retirement, 5000) == WAIT_OBJECT_0);
    }
#endif
    struct native_shader *p = lookup(shader);
    CHECK(p && p->references);
    if (p && p->references) --p->references;
    ++native_destroys;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_layout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *output) {
    CHECK(device == dev.device && allocator && allocator->pfnAllocation == allocate_device);
    CHECK(info->bindingCount == 1 && info->pBindings[0].binding == 2 && info->pBindings[0].descriptorCount == 1);
    CHECK(info->pBindings[0].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    CHECK(info->pBindings[0].stageFlags == (VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT));
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *flags = info->pNext;
    CHECK(flags && flags->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO && flags->bindingCount == 1 &&
          flags->pBindingFlags[0] == VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
    if (layout_result != VK_SUCCESS) return layout_result;
    ++layout_creates;
    *output = H(VkDescriptorSetLayout, next_layout++);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice device, VkDescriptorSetLayout layout,
                                                 const VkAllocationCallbacks *allocator) {
    CHECK(device == dev.device && (uintptr_t)layout >= 10000 && allocator && allocator->pfnAllocation == allocate_device);
    ++layout_destroys;
}

static PFN_vkCreateShadersEXT create;
static PFN_vkDestroyShaderEXT destroy;
static PFN_vkGetShaderBinaryDataEXT get;
static void plan(const VkShaderCreateInfoEXT *infos, uint32_t count, const VkAllocationCallbacks *allocator, bool use_bridge) {
    outer = infos;
    outer_count = count;
    public_allocator = allocator;
    bridge = use_bridge;
    temporary_result = final_result = binary_result = VK_SUCCESS;
    temporary_successes = final_successes = UINT32_MAX;
    expect_temporary = false;
    layout_result = VK_SUCCESS;
    leave_rt_dependency = false;
    force_temporary_index = 0;
    get_fail_size = 0;
}
static void destroy_all(VkShaderEXT *shaders, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        if (shaders[i]) destroy(dev.device, shaders[i], public_allocator);
}
struct blob {
    uint8_t *bytes;
    size_t size;
};
static struct blob export_shader(VkShaderEXT shader) {
    struct blob b = {0};
    CHECK(get(dev.device, shader, &b.size, NULL) == VK_SUCCESS && b.size > 224);
    b.bytes = malloc(b.size);
    CHECK(b.bytes && !((uintptr_t)b.bytes & 15));
    size_t capacity = b.size;
    CHECK(get(dev.device, shader, &capacity, b.bytes) == VK_SUCCESS && capacity == b.size);
    return b;
}
static VkShaderCreateInfoEXT binary_info(const struct blob *b, VkShaderStageFlagBits stage) {
    return (VkShaderCreateInfoEXT){.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                   .stage = stage,
                                   .codeType = VK_SHADER_CODE_TYPE_BINARY_EXT,
                                   .codeSize = b->size,
                                   .pCode = b->bytes};
}
static size_t make_source(uint32_t *p, uint32_t model, uint32_t tag, bool query) {
    uint32_t prefix[] = {0x07230203, 0x00010500, tag, 8, 0, 0x00020011, 1};
    memcpy(p, prefix, sizeof(prefix));
    size_t count = sizeof(prefix) / 4;
    if (query) {
        p[count++] = 0x00020011;
        p[count++] = 4472;
        p[count++] = 0x0006000a;
        memset(p + count, 0, 20);
        memcpy(p + count, "SPV_KHR_ray_query", sizeof("SPV_KHR_ray_query"));
        count += 5;
    }
    uint32_t entry[] = {0x0003000e, 0, 1, 0x0005000f, model, 1, 0x6e69616d, 0};
    memcpy(p + count, entry, sizeof(entry));
    count += sizeof(entry) / 4;
    if (model == 5) {
        uint32_t size[] = {0x00060010, 1, 17, 1, 1, 1};
        memcpy(p + count, size, sizeof(size));
        count += sizeof(size) / 4;
    } else if (model == 4) {
        p[count++] = 0x00030010;
        p[count++] = 1;
        p[count++] = 7;
    }
    uint32_t types[] = {0x00020013, 2, 0x00030021, 3, 2};
    memcpy(p + count, types, sizeof(types));
    count += sizeof(types) / 4;
    if (query) {
        p[count++] = 0x00021178;
        p[count++] = 7;
    }
    uint32_t body[] = {0x00050036, 2, 1, 0, 3, 0x000200f8, 4, 0x000100fd, 0x00010038};
    memcpy(p + count, body, sizeof(body));
    count += sizeof(body) / 4;
    return count * 4;
}
static struct blob plain_case(const VkAllocationCallbacks *allocator) {
    uint32_t words[64];
    VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                  .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                  .pName = "main",
                                  .pCode = words,
                                  .codeSize = make_source(words, 5, 17, false)};
    VkBaseInStructure unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM};
    VkDescriptorSetLayout arbitrary = H(VkDescriptorSetLayout, 333);
    info.pNext = &unknown;
    info.setLayoutCount = 1;
    info.pSetLayouts = &arbitrary;
    plan(&info, 1, allocator, false);
    unsigned gets = binary_gets;
    VkShaderEXT shader;
    CHECK(create(dev.device, 1, &info, allocator, &shader) == VK_SUCCESS && shader);
    CHECK(binary_gets == gets + 2); /* NoRQ bypasses untracked DSL and unknown pNext. */
    struct blob b = export_shader(shader);
    CHECK(read32(b.bytes, 8) == 3 && read32(b.bytes, 24) == 1 && read32(b.bytes, 28) == 0 && read64(b.bytes, 32) == 16 &&
          !read32(b.bytes, 92));
    uint8_t *copy = malloc(b.size);
    memset(copy, 0xcd, b.size);
    size_t short_size = b.size - 1;
    CHECK(get(dev.device, shader, &short_size, copy) == VK_INCOMPLETE && short_size == 0);
    for (size_t i = 0; i < b.size; ++i) CHECK(copy[i] == 0xcd);
    size_t size = b.size;
    CHECK(get(dev.device, shader, &size, copy) == VK_SUCCESS && !memcmp(copy, b.bytes, b.size));
    free(copy);
    destroy(dev.device, shader, allocator);
    return b;
}
static struct blob linked_case(const VkAllocationCallbacks *allocator) {
    static uint32_t words[2][64];
    uint32_t value = 37;
    VkSpecializationMapEntry entry = {7, 0, 4};
    VkSpecializationInfo specialization_info = {1, &entry, 4, &value};
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR, 4, 8};
    VkDescriptorSetLayout set = H(VkDescriptorSetLayout, 3);
    VkDescriptorSetLayoutBinding binding = {2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                                            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, NULL};
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 1, .pBindingFlags = &flags};
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .pNext = &flag_info,
                                              .bindingCount = 1,
                                              .pBindings = &binding};
    vkrt_shader_interface_lock(&dev);
    CHECK(vkrt_shader_interface_track_layout_locked(&dev, set, &layout) == VK_SUCCESS);
    vkrt_shader_interface_unlock(&dev);
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, NULL, 32};
    VkShaderCreateInfoEXT info[2] = {{.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                      .flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT,
                                      .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                      .nextStage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                      .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                      .pCode = words[0],
                                      .codeSize = make_source(words[0], 0, 21, false),
                                      .pName = "main"},
                                     {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                      .pNext = &subgroup,
                                      .flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT,
                                      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                      .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                      .pCode = words[1],
                                      .codeSize = make_source(words[1], 4, 22, true),
                                      .pName = "main",
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &set,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &push,
                                      .pSpecializationInfo = &specialization_info}};
    memcpy(saved_group, info, sizeof(info));
    saved_group_count = 2;
    VkShaderCreateInfoEXT immutable[2];
    memcpy(immutable, info, sizeof(info));
    plan(info, 2, allocator, false);
    VkShaderEXT shaders[2];
    unsigned gets = binary_gets;
    CHECK(create(dev.device, 2, info, allocator, shaders) == VK_SUCCESS && shaders[0] && shaders[1]);
    CHECK(binary_gets == gets);
    CHECK(!memcmp(immutable, info, sizeof(info)) && value == 37 && subgroup.requiredSubgroupSize == 32 &&
          push.stageFlags == (VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR));
    struct blob b = export_shader(shaders[1]);
    CHECK(read32(b.bytes, 24) == 2 && read32(b.bytes, 28) == 1 && !read64(b.bytes, 32) && read32(b.bytes, 92) == 1);
    struct blob peer = export_shader(shaders[0]);
    CHECK(read32(peer.bytes, 24) == 2 && read32(peer.bytes, 28) == 0 && !read64(peer.bytes, 32) && read32(peer.bytes, 92) == 1);
    free(peer.bytes);
    destroy_all(shaders, 2);
    vkrt_shader_interface_lock(&dev);
    vkrt_shader_interface_forget_layout_locked(&dev, set);
    vkrt_shader_interface_unlock(&dev);
    value = 99;
    subgroup.requiredSubgroupSize = 64;
    return b;
}
static void opaque_import(const struct blob *blob, const VkAllocationCallbacks *allocator) {
    VkBaseInStructure unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM};
    uint32_t poison = 99;
    VkSpecializationMapEntry map = {7, 0, 4};
    VkSpecializationInfo ignored = {1, &map, 4, &poison};
    VkDescriptorSetLayout different = H(VkDescriptorSetLayout, 444);
    VkPushConstantRange push = {VK_SHADER_STAGE_ALL, 80, 4};
    VkShaderCreateInfoEXT info = binary_info(blob, VK_SHADER_STAGE_COMPUTE_BIT);
    info.pNext = &unknown;
    info.pName = NULL;
    info.pSpecializationInfo = &ignored;
    info.setLayoutCount = 1;
    info.pSetLayouts = &different;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    plan(&info, 1, allocator, true);
    unsigned before = rewrites, calls = temporary_calls, gets = binary_gets;
    bool old_backend = dev.shader_object_backend;
    dev.virtual_rt = dev.shader_object_backend = false;
    VkShaderEXT shader;
    CHECK(create(dev.device, 1, &info, allocator, &shader) == VK_SUCCESS && shader);
    CHECK(rewrites == before && temporary_calls == calls && binary_gets == gets);
    struct blob copy = export_shader(shader);
    CHECK(copy.size == blob->size && !memcmp(copy.bytes, blob->bytes, blob->size));
    free(copy.bytes);
    destroy(dev.device, shader, allocator);
    dev.virtual_rt = true;
    dev.shader_object_backend = old_backend;
}
static void unused_specialization(const VkAllocationCallbacks *allocator) {
    uint32_t words[64];
    uint8_t value = 0;
    VkSpecializationMapEntry unused = {999, 0, 0};
    VkSpecializationInfo specialization_info = {1, &unused, 1, &value};
    VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                  .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                  .pName = "main",
                                  .pCode = words,
                                  .codeSize = make_source(words, 5, 55, false),
                                  .pSpecializationInfo = &specialization_info};
    plan(&info, 1, allocator, false);
    VkShaderEXT shader;
    CHECK(create(dev.device, 1, &info, allocator, &shader) == VK_SUCCESS && shader);
    struct blob b = export_shader(shader);
    destroy(dev.device, shader, allocator);
    /* 00776 constrains only IDs present in the module; an unused ID's size0
     * is legal. 00773 still requires its offset to be strictly below dataSize. */
    opaque_import(&b, allocator);
    size_t map = 224 + (size_t)read64(b.bytes, 128 + 8) + read32(b.bytes, 128 + 16);
    CHECK(read32(b.bytes, map) == 999 && read32(b.bytes, map + 4) == 0 && !read64(b.bytes, map + 8));
    write32(b.bytes, map + 4, 1);
    seal(b.bytes, b.size);
    info = binary_info(&b, VK_SHADER_STAGE_COMPUTE_BIT);
    plan(&info, 1, allocator, true);
    unsigned calls = native_calls;
    CHECK(create(dev.device, 1, &info, allocator, &shader) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT && !shader &&
          native_calls == calls);
    free(b.bytes);
}
static void rq_import(const struct blob *blob, const VkAllocationCallbacks *allocator) {
    VkBaseInStructure unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM};
    VkPushConstantRange outer_push = {VK_SHADER_STAGE_ALL, 80, 4};
    uint32_t poison = 99;
    VkSpecializationMapEntry map = {7, 0, 4};
    VkSpecializationInfo ignored = {1, &map, 4, &poison};
    VkShaderCreateInfoEXT info = binary_info(blob, VK_SHADER_STAGE_FRAGMENT_BIT);
    info.pNext = &unknown;
    info.pName = NULL;
    info.pSpecializationInfo = &ignored;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &outer_push;
    plan(&info, 1, allocator, true);
    expect_temporary = true;
    unsigned before = native_calls, destroyed = native_destroys, layouts = layout_creates, old_rewrites = rewrites;
    dev.virtual_rt = false;
    dev.as_registry = NULL;
    VkShaderEXT shader;
    CHECK(create(dev.device, 1, &info, allocator, &shader) == VK_SUCCESS && shader);
    CHECK(native_calls == before + 2 && native_destroys == destroyed + 2 && rewrites == old_rewrites + 1);
    CHECK(layout_creates == layouts + 1 && layout_creates == layout_destroys && registry_inits);
    struct blob copy = export_shader(shader);
    CHECK(copy.size == blob->size && !memcmp(copy.bytes, blob->bytes, blob->size));
    free(copy.bytes);
    destroy(dev.device, shader, allocator);
    /* Every nonnull temporary handle must retire on partial creation failure. */
    plan(&info, 1, allocator, true);
    expect_temporary = true;
    temporary_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    temporary_successes = 1;
    destroyed = native_destroys;
    before = final_calls;
    CHECK(create(dev.device, 1, &info, allocator, &shader) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !shader);
    CHECK(native_destroys == destroyed + 1 && final_calls == before && !live_native() && layout_creates == layout_destroys);
    /* Native binary extraction failure also destroys both temporary handles. */
    plan(&info, 1, allocator, true);
    expect_temporary = true;
    binary_result = VK_ERROR_UNKNOWN;
    destroyed = native_destroys;
    CHECK(create(dev.device, 1, &info, allocator, &shader) == VK_ERROR_UNKNOWN && !shader);
    CHECK(native_destroys == destroyed + 2 && !live_native() && layout_creates == layout_destroys);
    plan(&info, 1, allocator, true);
    expect_temporary = true;
    layout_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    before = native_calls;
    CHECK(create(dev.device, 1, &info, allocator, &shader) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !shader && native_calls == before);
    CHECK(!live_native() && layout_creates == layout_destroys);
    /* A lowerer claiming success while retaining an RT dependency must never
     * manufacture a supposedly portable native opaque binary. */
    plan(&info, 1, allocator, true);
    expect_temporary = true;
    leave_rt_dependency = true;
    CHECK(create(dev.device, 1, &info, allocator, &shader) == VK_ERROR_FEATURE_NOT_PRESENT && !shader && native_calls == before);
    CHECK(!live_native() && layout_creates == layout_destroys);
    /* Device allocator failures across capture/materialization/native transport. */
    unsigned baseline = live_allocations();
    unsigned failed = 0, succeeded = 0;
    for (int budget = 0; budget < 50; ++budget) {
        plan(&info, 1, allocator, true);
        expect_temporary = true;
        atomic_store(&allocation_budget, budget);
        VkResult result = create(dev.device, 1, &info, allocator, &shader);
        atomic_store(&allocation_budget, -1);
        CHECK(result == VK_SUCCESS || result == VK_ERROR_OUT_OF_HOST_MEMORY);
        if (result == VK_SUCCESS) {
            ++succeeded;
            CHECK(shader);
            destroy(dev.device, shader, allocator);
        } else {
            ++failed;
            CHECK(!shader);
        }
        CHECK(!live_native() && live_allocations() == baseline && layout_creates == layout_destroys);
    }
    CHECK(failed && succeeded);
    allocation_failures = failed;
    allocation_successes = succeeded;
    dev.virtual_rt = true;
}
static void malformed(const struct blob *source, const VkAllocationCallbacks *allocator) {
    struct blob bad = {.bytes = malloc(source->size), .size = source->size};
    CHECK(bad.bytes);
    size_t first = 128;
    size_t second = first + (size_t)read64(source->bytes, first);
    size_t code_size = (size_t)read64(source->bytes, second + 8);
    size_t interface_offset = second + 96 + code_size + read32(source->bytes, second + 16) +
                              (size_t)read32(source->bytes, second + 20) * 16 + (size_t)read64(source->bytes, second + 24) +
                              (size_t)read32(source->bytes, second + 52) * 4;
    CHECK(read32(source->bytes, interface_offset + 4) == 2 && read32(source->bytes, interface_offset + 24) == 0 &&
          read32(source->bytes, interface_offset + 28) == 1);
    const struct {
        size_t offset;
        uint64_t value;
        bool wide;
    } cases[] = {{8, 2, false},
                 {16, source->size - 1, true},
                 {24, UINT32_MAX, false},
                 {28, 2, false},
                 {32, 16, true},
                 {40, 1, false},
                 {120, 1, false},
                 {first, UINT64_MAX, true},
                 {second + 16, 0, false},
                 {second + 24, UINT64_MAX, true},
                 {second + 32, UINT64_MAX, true},
                 {second + 64, 0, false},
                 {second + 68, VK_SHADER_STAGE_VERTEX_BIT, false},
                 {second + 72, UINT32_C(0x80000000), false},
                 {second + 72, 0, false},
                 {second + 96 + code_size, UINT32_C(0x6e696178), false},
                 {second + 76, 0, false},
                 {second + 84, 1, false},
                 {second + 96, 0, false},
                 {interface_offset + 4, 1, false},
                 {interface_offset + 8, UINT64_MAX, true},
                 {interface_offset + 16, UINT32_MAX, false},
                 {interface_offset + 24, 16, false},
                 {interface_offset + 28, 2, false},
                 {interface_offset + 32, 0, false},
                 {interface_offset + 44 + 8, 2, false},
                 {interface_offset + 68 + 4, UINT32_MAX, false},
                 {interface_offset + 68 + 16, 16, false},
                 {interface_offset + 68 + 28, 1, false}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        ++malformed_count;
        memcpy(bad.bytes, source->bytes, bad.size);
        if (cases[i].wide)
            write64(bad.bytes, cases[i].offset, cases[i].value);
        else
            write32(bad.bytes, cases[i].offset, (uint32_t)cases[i].value);
        seal(bad.bytes, bad.size);
        VkShaderCreateInfoEXT info = binary_info(&bad, VK_SHADER_STAGE_FRAGMENT_BIT);
        plan(&info, 1, allocator, true);
        unsigned calls = native_calls, layouts = layout_creates, baseline = live_allocations();
        VkShaderEXT shader = H(VkShaderEXT, 9);
        VkResult result = create(dev.device, 1, &info, allocator, &shader);
        if (result != VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT) fprintf(stderr, "Malformed mutation %zu result %d\n", i, result);
        CHECK(result == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT && !shader && native_calls == calls && layout_creates == layouts &&
              live_allocations() == baseline);
    }
    free(bad.bytes);
}
static void prefixes(const struct blob *blob, const VkAllocationCallbacks *allocator) {
    uint8_t garbage[16] = {0};
    VkShaderCreateInfoEXT infos[3];
    for (unsigned i = 0; i < 3; ++i) infos[i] = binary_info(blob, VK_SHADER_STAGE_COMPUTE_BIT);
    infos[1].pCode = garbage;
    infos[1].codeSize = sizeof(garbage);
    plan(infos, 3, allocator, true);
    unsigned before = final_calls;
    VkShaderEXT out[3] = {H(VkShaderEXT, 9), H(VkShaderEXT, 9), H(VkShaderEXT, 9)};
    CHECK(create(dev.device, 3, infos, allocator, out) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
    CHECK(out[0] && !out[1] && !out[2] && final_calls == before + 1);
    struct blob first = export_shader(out[0]);
    CHECK(first.size == blob->size && !memcmp(first.bytes, blob->bytes, blob->size));
    free(first.bytes);
    destroy_all(out, 3);
    /* Native final failure preserves its valid partial outputs. */
    infos[1] = infos[0];
    plan(infos, 3, allocator, true);
    final_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    final_successes = 1;
    CHECK(create(dev.device, 3, infos, allocator, out) == VK_ERROR_OUT_OF_DEVICE_MEMORY && out[0] && !out[1] && !out[2]);
    destroy_all(out, 3);
    /* An opaque cache allocation failure AFTER native creation keeps prefix0. */
    uint32_t words[64];
    VkShaderCreateInfoEXT source = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                    .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                    .pName = "main",
                                    .pCode = words,
                                    .codeSize = make_source(words, 5, 31, false)};
    for (unsigned i = 0; i < 3; ++i) infos[i] = source;
    plan(infos, 3, allocator, false);
    get_fail_size = binary_gets + 3;
    unsigned destroyed = native_destroys;
    CHECK(create(dev.device, 3, infos, allocator, out) == VK_ERROR_OUT_OF_HOST_MEMORY && out[0] && !out[1] && !out[2]);
    atomic_store(&allocation_budget, -1);
    CHECK(native_destroys == destroyed + 2);
    first = export_shader(out[0]);
    free(first.bytes);
    destroy_all(out, 3);
    /* Unknown native opaque binaries never bypass the layer UUID contract. */
    for (unsigned profile = 0; profile < 2; ++profile) {
        dev.virtual_rt = profile != 0;
        infos[0] = binary_info(blob, VK_SHADER_STAGE_COMPUTE_BIT);
        infos[0].pCode = garbage;
        infos[0].codeSize = 16;
        plan(infos, 1, allocator, true);
        before = native_calls;
        CHECK(create(dev.device, 1, infos, allocator, out) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT && !out[0] &&
              native_calls == before);
    }
    dev.virtual_rt = true;
}
static void linked_prefixes(const struct blob *compute, const VkAllocationCallbacks *allocator) {
    struct blob graphics[2];
    const VkShaderStageFlagBits stages[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t words[64];
        VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                      .stage = stages[i],
                                      .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                      .pName = "main",
                                      .pCode = words,
                                      .codeSize = make_source(words, i ? 4 : 0, 41 + i, false)};
        plan(&info, 1, allocator, false);
        VkShaderEXT shader;
        CHECK(create(dev.device, 1, &info, allocator, &shader) == VK_SUCCESS && shader);
        graphics[i] = export_shader(shader);
        destroy(dev.device, shader, allocator);
    }
    uint8_t garbage[16] = {0};
    VkShaderCreateInfoEXT infos[] = {binary_info(compute, VK_SHADER_STAGE_COMPUTE_BIT),
                                     binary_info(&graphics[0], VK_SHADER_STAGE_VERTEX_BIT),
                                     binary_info(&graphics[1], VK_SHADER_STAGE_FRAGMENT_BIT),
                                     {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                      .stage = VK_SHADER_STAGE_GEOMETRY_BIT,
                                      .codeType = VK_SHADER_CODE_TYPE_BINARY_EXT,
                                      .codeSize = sizeof(garbage),
                                      .pCode = garbage}};
    for (unsigned i = 1; i < 4; ++i) infos[i].flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
    infos[1].nextStage = VK_SHADER_STAGE_GEOMETRY_BIT;
    infos[3].nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkShaderCreateInfoEXT saved[4];
    memcpy(saved, infos, sizeof(saved));
    plan(infos, 4, allocator, true);
    unsigned calls = final_calls, temporaries = temporary_calls;
    VkShaderEXT output[4];
    CHECK(create(dev.device, 4, infos, allocator, output) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
    CHECK(output[0] && !output[1] && !output[2] && !output[3] && final_calls == calls + 1);
    CHECK(temporary_calls == temporaries && !memcmp(saved, infos, sizeof(saved)));
    struct blob retained = export_shader(output[0]);
    CHECK(retained.size == compute->size && !memcmp(retained.bytes, compute->bytes, retained.size));
    free(retained.bytes);
    destroy_all(output, 4);
    /* A VERT-only remaining linked prefix has no later peer and is legal.
     * Preserve that success without changing its original nextStage=GEOM. */
    VkShaderCreateInfoEXT simple[] = {infos[1], infos[3]};
    plan(simple, 2, allocator, true);
    calls = final_calls;
    CHECK(create(dev.device, 2, simple, allocator, output) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
    CHECK(output[0] && !output[1] && final_calls == calls + 1);
    CHECK(simple[0].flags == saved[1].flags && simple[0].nextStage == saved[1].nextStage);
    destroy_all(output, 2);
    for (unsigned i = 0; i < 2; ++i) free(graphics[i].bytes);
    CHECK(!live_native());
    printf("Linked-prefix topology: failed unordered group + retained independent COMP; legal VERT-only prefix PASS\n");
}
static void aliases(const struct blob *blob) {
    struct object_owner owners[2] = {{true, 0}, {true, 0}};
    VkAllocationCallbacks callbacks[2] = {{.pUserData = &owners[0], .pfnAllocation = allocate_object, .pfnFree = free_object},
                                          {.pUserData = &owners[1], .pfnAllocation = allocate_object, .pfnFree = free_object}};
    VkShaderCreateInfoEXT info = binary_info(blob, VK_SHADER_STAGE_COMPUTE_BIT);
    VkShaderEXT out[2];
    for (unsigned i = 0; i < 2; ++i) {
        plan(&info, 1, &callbacks[i], true);
        force_public = H(VkShaderEXT, 7000);
        CHECK(create(dev.device, 1, &info, &callbacks[i], &out[i]) == VK_SUCCESS && out[i]);
    }
    CHECK(out[0] == out[1]);
    public_allocator = &callbacks[0];
    destroy(dev.device, out[0], &callbacks[0]);
    owners[0].live = false;
    struct blob copy = export_shader(out[1]);
    CHECK(copy.size == blob->size && !memcmp(copy.bytes, blob->bytes, blob->size));
    free(copy.bytes);
    public_allocator = &callbacks[1];
    destroy(dev.device, out[1], &callbacks[1]);
    owners[1].live = false;
    CHECK(!owners[0].calls && !owners[1].calls && !live_native());
}
static void mixed_and_temporary_alias(const struct blob *plain, const struct blob *rq, const VkAllocationCallbacks *allocator) {
    uint32_t words[64];
    VkShaderCreateInfoEXT infos[2] = {{.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                       .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                       .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                       .pName = "main",
                                       .pCode = words,
                                       .codeSize = make_source(words, 5, 39, false)},
                                      binary_info(plain, VK_SHADER_STAGE_COMPUTE_BIT)};
    VkShaderCreateInfoEXT old_group[2];
    memcpy(old_group, saved_group, sizeof(old_group));
    saved_group[0] = infos[0];
    saved_group_count = 1;
    plan(infos, 2, allocator, true);
    expect_temporary = true;
    unsigned calls = native_calls, destroyed = native_destroys;
    VkShaderEXT shaders[2];
    CHECK(create(dev.device, 2, infos, allocator, shaders) == VK_SUCCESS && shaders[0] && shaders[1]);
    CHECK(native_calls == calls + 2 && native_destroys == destroyed + 1);
    struct blob source = export_shader(shaders[0]), binary = export_shader(shaders[1]);
    CHECK(read32(source.bytes, 224 + 8) == 39 && read64(source.bytes, 32) == 16);
    CHECK(binary.size == plain->size && !memcmp(binary.bytes, plain->bytes, plain->size));
    free(source.bytes);
    free(binary.bytes);
    destroy_all(shaders, 2);
    memcpy(saved_group, old_group, sizeof(old_group));
    saved_group_count = 2;
    /* Temporary compilation can return a handle already owned by an application
     * shader. Retiring that native reference must not retire its public cache. */
    VkShaderCreateInfoEXT info = binary_info(rq, VK_SHADER_STAGE_FRAGMENT_BIT);
    plan(&info, 1, allocator, true);
    expect_temporary = true;
    CHECK(create(dev.device, 1, &info, allocator, &shaders[0]) == VK_SUCCESS && shaders[0]);
    plan(&info, 1, allocator, true);
    expect_temporary = true;
    force_temporary = shaders[0];
    force_temporary_index = 1;
    CHECK(create(dev.device, 1, &info, allocator, &shaders[1]) == VK_SUCCESS && shaders[1]);
    CHECK(lookup(shaders[0])->references == 1);
    binary = export_shader(shaders[0]);
    CHECK(binary.size == rq->size && !memcmp(binary.bytes, rq->bytes, rq->size));
    free(binary.bytes);
    destroy_all(shaders, 2);
    /* Source provenance, rather than a forgeable cached groupRQ bit, proves
     * whether native opaque transport is permitted. */
    struct blob bad = {.bytes = malloc(plain->size), .size = plain->size};
    memcpy(bad.bytes, plain->bytes, bad.size);
    write32(bad.bytes, 224 + 6 * 4, 4472); /* Shader capability -> RayQueryKHR. */
    seal(bad.bytes, bad.size);
    info = binary_info(&bad, VK_SHADER_STAGE_COMPUTE_BIT);
    plan(&info, 1, allocator, true);
    calls = native_calls;
    CHECK(create(dev.device, 1, &info, allocator, shaders) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT && !shaders[0] &&
          native_calls == calls);
    free(bad.bytes);
    CHECK(!live_native() && layout_creates == layout_destroys);
}
#ifdef _WIN32
struct concurrent_call {
    VkShaderCreateInfoEXT info;
    const VkAllocationCallbacks *allocator;
    VkShaderEXT shader;
    VkResult result;
};
static DWORD WINAPI retire_thread(void *argument) {
    struct concurrent_call *c = argument;
    destroy(dev.device, c->shader, c->allocator);
    return 0;
}
static DWORD WINAPI create_thread(void *argument) {
    struct concurrent_call *c = argument;
    SetEvent(creating);
    c->result = create(dev.device, 1, &c->info, c->allocator, &c->shader);
    return 0;
}
static void retirement(const VkAllocationCallbacks *allocator) {
    uint32_t words[64];
    struct concurrent_call call = {.info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                            .pName = "main",
                                            .pCode = words,
                                            .codeSize = make_source(words, 5, 40, false)},
                                   .allocator = allocator};
    plan(&call.info, 1, allocator, false);
    force_public = H(VkShaderEXT, 7777);
    CHECK(create(dev.device, 1, &call.info, allocator, &call.shader) == VK_SUCCESS);
    retiring = call.shader;
    retired = CreateEvent(NULL, TRUE, FALSE, NULL);
    release_retirement = CreateEvent(NULL, TRUE, FALSE, NULL);
    creating = CreateEvent(NULL, TRUE, FALSE, NULL);
    native_creation = CreateEvent(NULL, TRUE, FALSE, NULL);
    gate_retirement = true;
    HANDLE a = CreateThread(NULL, 0, retire_thread, &call, 0, NULL);
    CHECK(WaitForSingleObject(retired, 5000) == WAIT_OBJECT_0);
    words[2] = 41;
    force_public = retiring;
    HANDLE b = CreateThread(NULL, 0, create_thread, &call, 0, NULL);
    CHECK(WaitForSingleObject(creating, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(native_creation, 100) == WAIT_TIMEOUT);
    SetEvent(release_retirement);
    CHECK(WaitForSingleObject(a, 5000) == WAIT_OBJECT_0 && WaitForSingleObject(b, 5000) == WAIT_OBJECT_0);
    gate_retirement = false;
    CHECK(call.result == VK_SUCCESS && call.shader == retiring);
    struct blob fresh = export_shader(call.shader);
    CHECK(read32(fresh.bytes, 224 + 8) == 41); /* stale alias cache would retain40. */
    free(fresh.bytes);
    destroy(dev.device, call.shader, allocator);
    CloseHandle(a);
    CloseHandle(b);
    CloseHandle(retired);
    CloseHandle(release_retirement);
    CloseHandle(creating);
    CloseHandle(native_creation);
}
#endif
int main(void) {
    dev.device = H(VkDevice, 1);
    dev.physical_device = H(VkPhysicalDevice, 4);
    dev.instance_data = &inst;
    dev.virtual_rt = dev.shader_object_backend = dev.app_requested_shader_objects = true;
    dev.as_registry = (void *)(uintptr_t)2;
    dev.queue_families_have_allocator = true;
    dev.queue_families_allocator =
        (VkAllocationCallbacks){.pUserData = &dev, .pfnAllocation = allocate_device, .pfnFree = free_device};
    dev.vtable.CreateShadersEXT = native_create;
    dev.vtable.DestroyShaderEXT = native_destroy;
    dev.vtable.GetShaderBinaryDataEXT = native_get;
    dev.vtable.CreateDescriptorSetLayout = native_layout;
    dev.vtable.DestroyDescriptorSetLayout = destroy_layout;
    inst.vtable.GetPhysicalDeviceProperties2 = properties;
    VkPhysicalDeviceVulkan12Features features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .descriptorBindingPartiallyBound = VK_TRUE};
    VkPhysicalDeviceVulkan13Features features13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &features, .subgroupSizeControl = VK_TRUE};
    VkPhysicalDeviceFeatures core = {.geometryShader = VK_TRUE};
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features13, .pEnabledFeatures = &core};
    CHECK(vkrt_shader_interface_init(&dev, &device_info) == VK_SUCCESS && vkrt_shader_objects_init(&dev) == VK_SUCCESS);
    vkrt_map_object(VKRT_HKEY(dev.device), &dev);
    vkrt_map_object(VKRT_HKEY(dev.physical_device), &inst);
    create = (PFN_vkCreateShadersEXT)vkrt_shader_object_proc("vkCreateShadersEXT");
    destroy = (PFN_vkDestroyShaderEXT)vkrt_shader_object_proc("vkDestroyShaderEXT");
    get = (PFN_vkGetShaderBinaryDataEXT)vkrt_shader_object_proc("vkGetShaderBinaryDataEXT");
    CHECK(create && destroy && get && !vkrt_shader_object_proc("vkOther"));
    struct object_owner owner = {true, 0};
    VkAllocationCallbacks allocator = {.pUserData = &owner, .pfnAllocation = allocate_object, .pfnFree = free_object};
    struct blob plain = plain_case(&allocator);
    opaque_import(&plain, &allocator);
    unused_specialization(&allocator);
    struct blob rq = linked_case(&allocator);
    rq_import(&rq, &allocator);
    malformed(&rq, &allocator);
    prefixes(&plain, &allocator);
    linked_prefixes(&plain, &allocator);
    aliases(&plain);
    mixed_and_temporary_alias(&plain, &rq, &allocator);
#ifdef _WIN32
    retirement(&allocator);
#endif
    /* Physical identity is stable across logical profiles, and namespace differs
     * from native UUID; the independent FORMAT3 SHA vector pins the algorithm. */
    VkPhysicalDeviceShaderObjectPropertiesEXT shader = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties_info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &shader};
    PFN_vkGetPhysicalDeviceProperties2 query = (void *)vkrt_shader_object_instance_proc("vkGetPhysicalDeviceProperties2KHR");
    query(dev.physical_device, &properties_info);
    CHECK(shader.shaderBinaryVersion == 5 && !memcmp(shader.shaderBinaryUUID, plain.bytes + 96, VK_UUID_SIZE));
    const uint8_t known[] = {0xba, 0xf3, 0x6b, 0x98, 0x40, 0x99, 0x5b, 0x13, 0x26, 0xc0, 0xd0, 0x71, 0xa2, 0xf9, 0xce, 0xcf};
    CHECK(!memcmp(shader.shaderBinaryUUID, known, VK_UUID_SIZE));
    eligible = false;
    query(dev.physical_device, &properties_info);
    const uint8_t other[] = {0x0b, 0xc3, 0x09, 0xa6, 0x58, 0xeb, 0x87, 0xc2, 0x63, 0x37, 0x75, 0x56, 0x1c, 0x43, 0xd2, 0x82};
    CHECK(!memcmp(shader.shaderBinaryUUID, other, VK_UUID_SIZE));
    eligible = true;
    /* Saved device allocator can disappear from selection only at device teardown;
     * a device created without one instead uses the still-live instance owner. */
    dev.queue_families_have_allocator = false;
    inst.has_allocator = true;
    inst.allocator = dev.queue_families_allocator;
    opaque_import(&plain, &allocator);
    free(plain.bytes);
    free(rq.bytes);
    vkrt_shader_objects_finish(&dev);
    vkrt_shader_interface_finish(&dev);
    CHECK(!live_native() && layout_creates == layout_destroys && allocations == frees && !owner.calls);
    vkrt_unmap_object(VKRT_HKEY(dev.device));
    vkrt_unmap_object(VKRT_HKEY(dev.physical_device));
    printf("FORMAT3 linked/opaque bridge +real interface codec, prefix failures, allocator and retirement contracts %s\n",
           failures ? "FAIL" : "PASS");
    printf("Resealed malformed inputs: %u; allocator positions: %u failures/%u successes; native live references: %u\n",
           malformed_count, allocation_failures, allocation_successes, live_native());
    return failures != 0;
}
