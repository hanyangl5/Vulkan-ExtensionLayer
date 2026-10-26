/* Copyright (c) 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_shader_objects.h"
#include "vkrt_rt_capabilities.h"
#include "vkrt_as_registry.h"
#include "vkrt_spirv_lower.h"
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
#define H(type, n) ((type)(uintptr_t)(n))
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            ++failures;                                     \
        }                                                   \
    } while (0)
static unsigned failures, native_calls, rewrites;
static struct vkrt_device_data dev;
static struct vkrt_instance_data inst;
static unsigned allocations, deallocations;
static bool allocation_failure;
static bool eligible = true;
static uint8_t uuid_last_xor;
bool vkrt_native_backend_eligible(struct vkrt_instance_data *instance, VkPhysicalDevice physical) {
    CHECK(instance == &inst && physical == dev.physical_device);
    return eligible;
}
VkResult vkrt_as_registry_init(struct vkrt_device_data *device, const VkAllocationCallbacks *allocator) {
    CHECK(device == &dev && !allocator);
    device->as_registry = (void *)(uintptr_t)2;
    return VK_SUCCESS;
}
VkDeviceAddress vkrt_as_registry_address(struct vkrt_device_data *device) {
    CHECK(device == &dev);
    return 4096;
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out) {
    CHECK(physical == dev.physical_device);
    VkPhysicalDeviceShaderObjectPropertiesEXT *p = out->pNext;
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) p->shaderBinaryUUID[i] = (uint8_t)(i + 1);
    p->shaderBinaryUUID[15] ^= uuid_last_xor;
    p->shaderBinaryVersion = 5;
}
static VKAPI_ATTR void *VKAPI_CALL allocation(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    CHECK(user == &dev && alignment <= 16 && scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (allocation_failure) return NULL;
    ++allocations;
    return malloc(size);
}
static VKAPI_ATTR void VKAPI_CALL deallocation(void *user, void *p) {
    CHECK(user == &dev);
    ++deallocations;
    free(p);
}
static VkResult native_result;
static bool passthrough, importing;
#ifdef _WIN32
static bool gate_retirement;
static HANDLE retired, release_retirement, creating, native_creation;
#endif
static const VkShaderCreateInfoEXT *source_infos;
static const VkAllocationCallbacks *expected_allocator;
static uint32_t code[] = {0x07230203, 1, 2, 3, 0};
int vkrt_spirv_validate_source(const uint32_t *words, size_t count) {
    /* Native-independent syntax spy. Real SPIRV-Tools validation is also
     * exercised by the public GPU malformed-envelope regression. */
    if (count < 5 || words[0] != 0x07230203) return 0;
    for (size_t i = 5; i < count;) {
        uint32_t length = words[i] >> 16;
        if (!length || length > count - i) return 0;
        i += length;
    }
    return 1;
}
static void seal(uint8_t *bytes, size_t size) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= i >= 112 && i < 120 ? 0 : bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    for (unsigned i = 0; i < 8; ++i) bytes[112 + i] = (uint8_t)(hash >> (8 * i));
}
struct object_allocator {
    bool live;
    unsigned calls;
};
static VKAPI_ATTR void *VKAPI_CALL object_allocation(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    struct object_allocator *context = user;
    CHECK(context->live && alignment <= 16 && scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    ++context->calls;
    return malloc(size);
}
static VKAPI_ATTR void VKAPI_CALL object_free(void *user, void *pointer) {
    struct object_allocator *context = user;
    CHECK(context->live);
    ++context->calls;
    free(pointer);
}
static VKAPI_ATTR void VKAPI_CALL native_destroy(VkDevice device, VkShaderEXT shader, const VkAllocationCallbacks *allocator) {
    CHECK(device == dev.device && shader == H(VkShaderEXT, 100) && allocator == expected_allocator);
    if (allocator && allocator->pfnAllocation == object_allocation) CHECK(((struct object_allocator *)allocator->pUserData)->live);
#ifdef _WIN32
    if (gate_retirement) {
        SetEvent(retired);
        CHECK(WaitForSingleObject(release_retirement, 5000) == WAIT_OBJECT_0);
    }
#endif
}
VkResult vkrt_rewrite_shader_module(struct vkrt_device_data *d, const VkShaderModuleCreateInfo *info, VkShaderModuleCreateInfo *out,
                                    bool *rq, bool *rt) {
    CHECK(d == &dev && !info->pNext && !info->flags);
    ++rewrites;
    *out = *info;
    *rq = *rt = false;
    if (info->pCode[4] == 2) {
        *rt = true;
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (info->pCode[4] == 1) {
        uint32_t *copy = malloc(info->codeSize);
        CHECK(copy != NULL);
        memcpy(copy, info->pCode, info->codeSize);
        copy[4] = 10;
        out->pCode = copy;
        *rq = true;
    }
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_create(VkDevice device, uint32_t count, const VkShaderCreateInfoEXT *infos,
                                                    const VkAllocationCallbacks *allocator, VkShaderEXT *shaders) {
    CHECK(device == dev.device && allocator == expected_allocator);
#ifdef _WIN32
    if (gate_retirement) SetEvent(native_creation);
#endif
    ++native_calls;
    if (passthrough)
        CHECK(infos == source_infos);
    else
        for (uint32_t i = 0; i < count; ++i) {
            CHECK(infos[i].flags == source_infos[i].flags);
            if (!importing) {
                CHECK(infos[i].pNext == source_infos[i].pNext);
                CHECK(infos[i].pName == source_infos[i].pName &&
                      infos[i].pSpecializationInfo == source_infos[i].pSpecializationInfo);
            } else {
                CHECK(!strcmp(infos[i].pName, "entry"));
                CHECK(infos[i].pSpecializationInfo && *(const uint32_t *)infos[i].pSpecializationInfo->pData == 37);
                CHECK(((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *)infos[i].pNext)->requiredSubgroupSize == 32);
            }
            CHECK(infos[i].pSetLayouts == source_infos[i].pSetLayouts);
            if (infos[i].codeType == VK_SHADER_CODE_TYPE_BINARY_EXT)
                CHECK(infos[i].pCode == source_infos[i].pCode);
            else
                CHECK(((const uint32_t *)infos[i].pCode)[4] == 10);
            CHECK(infos[i].pushConstantRangeCount == 2);
            CHECK(infos[i].pPushConstantRanges[0].stageFlags == VK_SHADER_STAGE_COMPUTE_BIT);
            CHECK(infos[i].pPushConstantRanges[0].offset == 4 && infos[i].pPushConstantRanges[0].size == 8);
            CHECK(infos[i].pPushConstantRanges[1].stageFlags == VK_SHADER_STAGE_ALL);
        }
    for (uint32_t i = 0; i < count; ++i) shaders[i] = i && native_result != VK_SUCCESS ? VK_NULL_HANDLE : H(VkShaderEXT, 100 + i);
    return native_result;
}
#ifdef _WIN32
struct concurrent_call {
    PFN_vkCreateShadersEXT create;
    PFN_vkDestroyShaderEXT destroy;
    const VkShaderCreateInfoEXT *info;
    const VkAllocationCallbacks *allocator;
    VkShaderEXT shader;
    VkResult result;
};
static DWORD WINAPI retire_shader(void *argument) {
    struct concurrent_call *call = argument;
    call->destroy(dev.device, call->shader, call->allocator);
    return 0;
}
static DWORD WINAPI create_shader(void *argument) {
    struct concurrent_call *call = argument;
    SetEvent(creating);
    call->result = call->create(dev.device, 1, call->info, call->allocator, &call->shader);
    return 0;
}
#endif
int main(void) {
    dev.device = H(VkDevice, 1);
    dev.physical_device = H(VkPhysicalDevice, 4);
    dev.instance_data = &inst;
    dev.virtual_rt = dev.shader_object_backend = dev.app_requested_shader_objects = true;
    dev.as_registry = (void *)(uintptr_t)2;
    dev.vtable.CreateShadersEXT = native_create;
    inst.vtable.GetPhysicalDeviceProperties2 = properties;
    CHECK(vkrt_shader_objects_init(&dev) == VK_SUCCESS);
    vkrt_map_object(VKRT_HKEY(dev.device), &dev);
    vkrt_map_object(VKRT_HKEY(dev.physical_device), &inst);
    PFN_vkCreateShadersEXT create = (PFN_vkCreateShadersEXT)vkrt_shader_object_proc("vkCreateShadersEXT");
    CHECK(create && !vkrt_shader_object_proc("vkOther"));
    VkPushConstantRange ranges[] = {{VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR, 4, 8},
                                    {VK_SHADER_STAGE_MISS_BIT_KHR, 24, 4},
                                    {VK_SHADER_STAGE_ALL, 32, 4}};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo chain = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, NULL, 32};
    VkDescriptorSetLayout set = H(VkDescriptorSetLayout, 3);
    uint32_t specialized = 37;
    VkSpecializationMapEntry entry = {7, 0, 4};
    VkSpecializationInfo specialization = {1, &entry, 4, &specialized};
    VkShaderCreateInfoEXT infos[2] = {{.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                       .pNext = &chain,
                                       .flags = VK_SHADER_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT,
                                       .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                       .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                       .codeSize = sizeof(code),
                                       .pCode = code,
                                       .pName = "entry",
                                       .setLayoutCount = 1,
                                       .pSetLayouts = &set,
                                       .pushConstantRangeCount = 3,
                                       .pPushConstantRanges = ranges,
                                       .pSpecializationInfo = &specialization}};
    infos[1] = infos[0];
    source_infos = infos;
    VkShaderCreateInfoEXT saved[2];
    memcpy(saved, infos, sizeof(saved));
    VkPushConstantRange saved_ranges[3];
    memcpy(saved_ranges, ranges, sizeof(ranges));
    VkAllocationCallbacks allocator = {.pUserData = &dev, .pfnAllocation = allocation, .pfnFree = deallocation};
    dev.queue_families_have_allocator = true;
    dev.queue_families_allocator = allocator;
    expected_allocator = &allocator;
    VkShaderEXT out[2] = {0};
    code[4] = 1;
    CHECK(create(dev.device, 2, infos, &allocator, out) == VK_SUCCESS);
    CHECK(native_calls == 1 && rewrites == 2 && out[0] == H(VkShaderEXT, 100) && out[1] == H(VkShaderEXT, 101));
    CHECK(!memcmp(saved, infos, sizeof(saved)) && !memcmp(saved_ranges, ranges, sizeof(ranges)) && code[4] == 1);
    PFN_vkGetShaderBinaryDataEXT get = (PFN_vkGetShaderBinaryDataEXT)vkrt_shader_object_proc("vkGetShaderBinaryDataEXT");
    PFN_vkDestroyShaderEXT destroy = (PFN_vkDestroyShaderEXT)vkrt_shader_object_proc("vkDestroyShaderEXT");
    size_t binary_size = 0;
    CHECK(get(dev.device, out[0], &binary_size, NULL) == VK_SUCCESS && binary_size > 128);
    uint8_t *binary = malloc(binary_size), *copy = malloc(binary_size);
    CHECK(binary && copy);
    memset(binary, 0xa5, binary_size);
    size_t short_size = binary_size - 1;
    CHECK(get(dev.device, out[0], &short_size, binary) == VK_INCOMPLETE && !short_size);
    for (size_t i = 0; i < binary_size; ++i) CHECK(binary[i] == 0xa5);
    size_t actual = binary_size;
    CHECK(get(dev.device, out[0], &actual, binary) == VK_SUCCESS && actual == binary_size);
    actual = binary_size;
    CHECK(get(dev.device, out[0], &actual, copy) == VK_SUCCESS && !memcmp(binary, copy, binary_size));
    destroy(dev.device, out[0], &allocator);
    destroy(dev.device, out[1], &allocator);
    infos[0].codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
    infos[0].pCode = binary;
    infos[0].codeSize = binary_size;
    infos[0].pName = NULL;
    infos[0].pSpecializationInfo = NULL;
    infos[0].pNext = NULL;
    specialized = 99;
    chain.requiredSubgroupSize = 64;
    importing = true;
    CHECK(create(dev.device, 1, infos, &allocator, out) == VK_SUCCESS);
    destroy(dev.device, out[0], &allocator);
#ifdef _WIN32
    /* The driver retires a native handle before returning from Destroy. A
     * concurrent independent creation may reuse it, but must not merge into
     * the retired object's cache entry. Pause that exact lifetime boundary. */
    CHECK(create(dev.device, 1, infos, &allocator, out) == VK_SUCCESS);
    uint32_t distinct_code[5];
    memcpy(distinct_code, code, sizeof(code));
    distinct_code[2] = 999;
    VkShaderCreateInfoEXT distinct_info = saved[0];
    distinct_info.pCode = distinct_code;
    source_infos = &distinct_info;
    importing = false;
    struct concurrent_call retiring = {.destroy = destroy, .allocator = &allocator, .shader = out[0]},
                           replacement = {.create = create, .info = &distinct_info, .allocator = &allocator};
    retired = CreateEvent(NULL, TRUE, FALSE, NULL);
    release_retirement = CreateEvent(NULL, TRUE, FALSE, NULL);
    creating = CreateEvent(NULL, TRUE, FALSE, NULL);
    native_creation = CreateEvent(NULL, TRUE, FALSE, NULL);
    CHECK(retired && release_retirement && creating && native_creation);
    gate_retirement = true;
    dev.vtable.DestroyShaderEXT = native_destroy;
    HANDLE retire_thread = CreateThread(NULL, 0, retire_shader, &retiring, 0, NULL);
    CHECK(retire_thread && WaitForSingleObject(retired, 5000) == WAIT_OBJECT_0);
    HANDLE create_thread = CreateThread(NULL, 0, create_shader, &replacement, 0, NULL);
    CHECK(create_thread && WaitForSingleObject(creating, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(native_creation, 1000) == WAIT_TIMEOUT);
    SetEvent(release_retirement);
    CHECK(WaitForSingleObject(retire_thread, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(create_thread, 5000) == WAIT_OBJECT_0);
    CHECK(replacement.result == VK_SUCCESS && replacement.shader == retiring.shader);
    gate_retirement = false;
    actual = binary_size;
    CHECK(get(dev.device, replacement.shader, &actual, copy) == VK_SUCCESS && ((uint32_t *)(copy + 128))[2] == 999);
    destroy(dev.device, replacement.shader, &allocator);
    dev.vtable.DestroyShaderEXT = NULL;
    CloseHandle(retire_thread);
    CloseHandle(create_thread);
    CloseHandle(retired);
    CloseHandle(release_retirement);
    CloseHandle(creating);
    CloseHandle(native_creation);
    source_infos = infos;
    importing = true;
#endif
    unsigned unchanged = native_calls;
    binary[binary_size - 1] ^= 1;
    CHECK(create(dev.device, 1, infos, &allocator, out) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT && !out[0] &&
          native_calls == unchanged);
    binary[binary_size - 1] ^= 1;
    /* A well-formed envelope/checksum is not proof of valid embedded source.
     * Append a zero-word-count instruction and reseal the transport checksum. */
    uint8_t *malformed = malloc(binary_size + 4);
    CHECK(malformed != NULL);
    memcpy(malformed, binary, 148);
    memset(malformed + 148, 0, 4);
    memcpy(malformed + 152, binary + 148, binary_size - 148);
    for (unsigned i = 0; i < 8; ++i) malformed[16 + i] = (uint8_t)((uint64_t)(binary_size + 4) >> (8 * i));
    malformed[24] = 24;
    seal(malformed, binary_size + 4);
    infos[0].pCode = malformed;
    infos[0].codeSize = binary_size + 4;
    dev.virtual_rt = false;
    CHECK(create(dev.device, 1, infos, &allocator, out) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT && !out[0] &&
          native_calls == unchanged);
    dev.virtual_rt = true;
    free(malformed);
    infos[0].pCode = binary;
    infos[0].codeSize = 8;
    CHECK(create(dev.device, 1, infos, &allocator, out) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT && !out[0]);
    VkPhysicalDeviceShaderObjectPropertiesEXT shader = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &shader};
    PFN_vkGetPhysicalDeviceProperties2 property =
        (PFN_vkGetPhysicalDeviceProperties2)vkrt_shader_object_instance_proc("vkGetPhysicalDeviceProperties2KHR");
    property(dev.physical_device, &props);
    CHECK(shader.shaderBinaryVersion == 5 && !memcmp(shader.shaderBinaryUUID, binary + 96, VK_UUID_SIZE));
    /* Independent Python hashlib SHA-256 vector for the 37-byte namespace,
     * native bytes 01..10, format BE32=1 and eligible profile=1. */
    const uint8_t known[] = {0x83, 0x86, 0x06, 0x7d, 0x11, 0xdc, 0xfa, 0x3a, 0xfa, 0x68, 0x6f, 0x3b, 0xfa, 0x8a, 0xb8, 0x48};
    CHECK(!memcmp(shader.shaderBinaryUUID, known, VK_UUID_SIZE));
    eligible = false;
    uuid_last_xor = 0xf6;
    property(dev.physical_device, &props);
    CHECK(memcmp(shader.shaderBinaryUUID, known, VK_UUID_SIZE) != 0);
    const uint8_t paired[] = {0xf2, 0x42, 0x5f, 0xb3, 0xa3, 0xfd, 0xfc, 0x84, 0x47, 0xb8, 0xcb, 0x8d, 0x6a, 0xbf, 0xef, 0x61};
    CHECK(!memcmp(shader.shaderBinaryUUID, paired, VK_UUID_SIZE));
    eligible = true;
    uuid_last_xor = 0;
    importing = false;
    memcpy(infos, saved, sizeof(saved));
    specialized = 37;
    chain.requiredSubgroupSize = 32;
    /* Equal native handles can represent multiple live objects. Shared cache
     * bytes must survive retirement of either object's allocator callbacks. */
    struct object_allocator owners[2] = {{true, 0}, {true, 0}};
    VkAllocationCallbacks objects[2] = {{.pUserData = &owners[0], .pfnAllocation = object_allocation, .pfnFree = object_free},
                                        {.pUserData = &owners[1], .pfnAllocation = object_allocation, .pfnFree = object_free}};
    VkShaderEXT aliases[2];
    dev.vtable.DestroyShaderEXT = native_destroy;
    for (unsigned i = 0; i < 2; ++i) {
        expected_allocator = &objects[i];
        CHECK(create(dev.device, 1, infos, &objects[i], &aliases[i]) == VK_SUCCESS);
    }
    CHECK(aliases[0] == aliases[1]);
    expected_allocator = &objects[0];
    destroy(dev.device, aliases[0], &objects[0]);
    owners[0].live = false;
    actual = binary_size;
    CHECK(get(dev.device, aliases[1], &actual, copy) == VK_SUCCESS && !memcmp(binary, copy, binary_size));
    expected_allocator = &objects[1];
    destroy(dev.device, aliases[1], &objects[1]);
    owners[1].live = false;
    CHECK(!owners[0].calls && !owners[1].calls);
    dev.vtable.DestroyShaderEXT = NULL;
    expected_allocator = &allocator;
    unsigned unchanged_calls = native_calls;
    allocation_failure = true;
    out[0] = H(VkShaderEXT, 9);
    CHECK(create(dev.device, 1, infos, &allocator, out) == VK_ERROR_OUT_OF_HOST_MEMORY && !out[0] &&
          native_calls == unchanged_calls);
    allocation_failure = false;
    /* A null device allocator uses the original instance's DEVICE allocator. */
    dev.queue_families_have_allocator = false;
    inst.has_allocator = true;
    inst.allocator = allocator;
    CHECK(create(dev.device, 1, infos, &allocator, out) == VK_SUCCESS);
    destroy(dev.device, out[0], &allocator);
    free(copy);
    free(binary);
    native_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    CHECK(create(dev.device, 2, infos, &allocator, out) == native_result && out[0] == H(VkShaderEXT, 100) && !out[1]);
    infos[1].pCode = (uint32_t[]){0x07230203, 1, 2, 3, 2};
    unsigned before = native_calls;
    out[0] = out[1] = H(VkShaderEXT, 9);
    CHECK(create(dev.device, 2, infos, &allocator, out) == VK_ERROR_FEATURE_NOT_PRESENT && !out[0] && !out[1] &&
          native_calls == before);
    infos[1] = infos[0];
    infos[1].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    CHECK(create(dev.device, 2, infos, &allocator, out) == VK_ERROR_FEATURE_NOT_PRESENT && !out[0] && !out[1]);
    infos[1] = infos[0];
    infos[0].codeType = infos[1].codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
    native_result = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    before = rewrites;
    CHECK(create(dev.device, 2, infos, &allocator, out) == native_result && rewrites == before && !out[0] && !out[1]);
    dev.as_registry = NULL;
    dev.virtual_rt = false;
    passthrough = true;
    CHECK(create(dev.device, 2, infos, &allocator, out) == native_result && rewrites == before);
    infos[0].codeType = infos[1].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    native_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(create(dev.device, 2, infos, &allocator, out) == native_result && rewrites == before);
    dev.vtable.CreateShadersEXT = NULL;
    out[0] = out[1] = H(VkShaderEXT, 9);
    CHECK(create(dev.device, 2, infos, &allocator, out) == VK_ERROR_FEATURE_NOT_PRESENT && !out[0] && !out[1]);
    vkrt_shader_objects_finish(&dev);
    CHECK(allocations == deallocations);
    vkrt_unmap_object(VKRT_HKEY(dev.device));
    vkrt_unmap_object(VKRT_HKEY(dev.physical_device));
    printf("Shader-object RQ lowering/range translation/native partial+binary semantics %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
