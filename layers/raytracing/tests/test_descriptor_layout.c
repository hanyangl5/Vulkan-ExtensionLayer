/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_descriptor_layout.h"
#include "vkrt_layer.h"

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static unsigned failures, native_calls;
static bool alias_only, virtual_device;
static const VkDevice device = (VkDevice)(uintptr_t)8;
static const VkDescriptorSetLayoutCreateInfo *original;
#define REQUIRE(x)                                          \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            ++failures;                                     \
        }                                                   \
    } while (0)

static VKAPI_ATTR void VKAPI_CALL native_support(VkDevice d, const VkDescriptorSetLayoutCreateInfo *info,
                                                 VkDescriptorSetLayoutSupport *support) {
    REQUIRE(d == device);
    ++native_calls;
    REQUIRE(info->flags == original->flags && info->pNext == original->pNext && info->bindingCount == original->bindingCount);
    for (uint32_t i = 0; i < info->bindingCount; ++i) {
        const VkDescriptorSetLayoutBinding *source = original->pBindings + i, *out = info->pBindings + i;
        REQUIRE(out->binding == source->binding && out->descriptorCount == source->descriptorCount &&
                out->pImmutableSamplers == source->pImmutableSamplers);
        if (virtual_device) {
            REQUIRE(out->descriptorType == (source->descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                                                ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                : source->descriptorType));
            if (out->stageFlags != VK_SHADER_STAGE_ALL)
                REQUIRE(!(out->stageFlags & (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR)));
        } else {
            REQUIRE(out->descriptorType == source->descriptorType && out->stageFlags == source->stageFlags);
        }
    }
    support->supported = VK_TRUE;
    for (VkBaseOutStructure *p = support->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT)
            ((VkDescriptorSetVariableDescriptorCountLayoutSupport *)p)->maxVariableDescriptorCount = 1000000;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL native_proc(VkDevice d, const char *name) {
    REQUIRE(d == device);
    if ((!alias_only && !strcmp(name, "vkGetDescriptorSetLayoutSupport")) || !strcmp(name, "vkGetDescriptorSetLayoutSupportKHR"))
        return (PFN_vkVoidFunction)native_support;
    return NULL;
}

static void layouts(void) {
    VkSampler samplers[5] = {0};
    VkDescriptorSetLayoutBinding bindings[] = {
        {99, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 17, VK_SHADER_STAGE_RAYGEN_BIT_KHR, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 16, VK_SHADER_STAGE_MISS_BIT_KHR, NULL},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9, VK_SHADER_STAGE_ALL, NULL},
        {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5, VK_SHADER_STAGE_VERTEX_BIT, samplers}};
    VkDescriptorSetLayoutBinding saved[4];
    memcpy(saved, bindings, sizeof(saved));
    VkDescriptorBindingFlags flags[] = {VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT, 0, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 4, .pBindingFlags = flags};
    VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .pNext = &flag_info,
                                            .bindingCount = 4,
                                            .pBindings = bindings};
    original = &info;
    struct vkrt_device_data dev = {0};
    dev.vtable.GetDeviceProcAddr = native_proc;
    vkrt_map_object(VKRT_HKEY(device), &dev);
    PFN_vkGetDescriptorSetLayoutSupport query = (void *)vkrt_descriptor_layout_proc("vkGetDescriptorSetLayoutSupport");
    REQUIRE(query && vkrt_descriptor_layout_proc("vkGetDescriptorSetLayoutSupportKHR") == (PFN_vkVoidFunction)query);
    REQUIRE(!vkrt_descriptor_layout_proc("other"));
    VkDescriptorSetVariableDescriptorCountLayoutSupport variable = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT};
    VkDescriptorSetLayoutSupport support = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT, .pNext = &variable};
    for (unsigned use_virtual = 0; use_virtual < 2; ++use_virtual) {
        virtual_device = use_virtual != 0;
        dev.as_registry = (void *)(uintptr_t)1;
        dev.virtual_rt = use_virtual != 0;
        for (unsigned alias = 0; alias < 2; ++alias) {
            alias_only = alias != 0;
            query(device, &info, &support);
            /* This is a layout-creation query, not a pipeline-budget query:
             * requested17 AS descriptors may be supported despite ASlimit16. */
            REQUIRE(support.supported && variable.maxVariableDescriptorCount == 1000000);
            REQUIRE(support.pNext == &variable && info.pNext == &flag_info && !memcmp(bindings, saved, sizeof(saved)));
        }
    }
    virtual_device = true;
    dev.virtual_rt = true;
    dev.as_registry = (void *)(uintptr_t)1;
    unsigned before = native_calls;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    query(device, &info, &support);
    REQUIRE(!support.supported && !variable.maxVariableDescriptorCount && native_calls == before);
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    query(device, &info, &support);
    REQUIRE(!support.supported && native_calls == before);
    /* ALL is a legal special value on an ordinary descriptor-buffer layout;
     * merely having a virtual AS backend must not reject that ordinary use. */
    info.pBindings = bindings + 2;
    info.bindingCount = 1;
    info.pNext = NULL;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    query(device, &info, &support);
    REQUIRE(support.supported && native_calls == before + 1);
    vkrt_unmap_object(VKRT_HKEY(device));
}

static void pools(void) {
    const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    VkMutableDescriptorTypeListEXT lists[] = {{0}, {0}, {2, types}, {0}};
    VkMutableDescriptorTypeCreateInfoEXT mutable = {.sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
                                                    .mutableDescriptorTypeListCount = 4,
                                                    .pMutableDescriptorTypeLists = lists};
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, UINT32_MAX},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, UINT32_MAX},
                                    {VK_DESCRIPTOR_TYPE_MUTABLE_EXT, 5},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 4}};
    VkDescriptorPoolSize saved[4];
    memcpy(saved, sizes, sizeof(saved));
    VkDescriptorPoolCreateInfo source = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                         .pNext = &mutable,
                                         .maxSets = UINT32_MAX,
                                         .poolSizeCount = 4,
                                         .pPoolSizes = sizes};
    VkDescriptorPoolCreateInfo native;
    VkDescriptorPoolSize *owned;
    REQUIRE(vkrt_prepare_descriptor_pool(false, &source, &native, &owned) == VK_SUCCESS && !owned && native.pPoolSizes == sizes);
    REQUIRE(vkrt_prepare_descriptor_pool(true, &source, &native, &owned) == VK_SUCCESS && owned);
    REQUIRE(native.poolSizeCount == 4 && native.pNext == &mutable && native.maxSets == UINT32_MAX);
    uint64_t total = 0;
    for (uint32_t i = 0; i < native.poolSizeCount; ++i) {
        if (native.pPoolSizes[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) total += native.pPoolSizes[i].descriptorCount;
        REQUIRE(native.pPoolSizes[i].descriptorCount == source.pPoolSizes[i].descriptorCount);
    }
    REQUIRE(total == UINT64_C(2) * UINT32_MAX && native.pPoolSizes[2].type == VK_DESCRIPTOR_TYPE_MUTABLE_EXT);
    REQUIRE(mutable.pMutableDescriptorTypeLists[2].pDescriptorTypes == types && !memcmp(saved, sizes, sizeof(saved)));
    free(owned);
    const VkDescriptorType unsupported[] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    lists[2].descriptorTypeCount = 1;
    lists[2].pDescriptorTypes = unsupported;
    REQUIRE(vkrt_prepare_descriptor_pool(true, &source, &native, &owned) == VK_ERROR_FEATURE_NOT_PRESENT && !owned);
}
int main(void) {
    layouts();
    pools();
    printf("Descriptor layout/support/pool translation: %s (%u failures, %u native queries)\n", failures ? "FAIL" : "PASS",
           failures, native_calls);
    return failures ? 1 : 0;
}
