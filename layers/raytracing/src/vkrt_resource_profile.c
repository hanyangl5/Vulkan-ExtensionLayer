/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_resource_profile.h"
#include <string.h>

enum resource_kind { SAMPLER, UNIFORM, STORAGE, SAMPLED_IMAGE, STORAGE_IMAGE, INPUT_ATTACHMENT, KIND_COUNT };
enum { AS_DESCRIPTORS = 16 };
static uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t max32(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint32_t subtract(uint32_t a, uint32_t b) { return a > b ? a - b : 0; }
static bool fail(enum vkrt_resource_failure *failure, enum vkrt_resource_failure reason) {
    if (failure) *failure = reason;
    return false;
}
static uint64_t sum_resources(const uint32_t values[KIND_COUNT]) {
    /* Pure samplers are not resources under maxPerStageResources. Counting
     * input attachments here is conservative; they are fragment-only. */
    uint64_t result = 0;
    for (unsigned k = UNIFORM; k < KIND_COUNT; ++k) result += values[k];
    return result;
}
static bool shrink_resources(uint32_t values[KIND_COUNT], const uint32_t minimum[KIND_COUNT], uint64_t budget) {
    uint64_t total = sum_resources(values);
    const unsigned order[] = {STORAGE_IMAGE, SAMPLED_IMAGE, STORAGE, UNIFORM, INPUT_ATTACHMENT};
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]) && total > budget; ++i) {
        unsigned k = order[i];
        uint64_t reduction = values[k] - minimum[k];
        if (reduction > total - budget) reduction = total - budget;
        values[k] -= (uint32_t)reduction;
        total -= reduction;
    }
    return total <= budget;
}

bool vkrt_capture_resource_native(PFN_vkGetPhysicalDeviceProperties2 properties, PFN_vkGetPhysicalDeviceFeatures2 features,
                                  VkPhysicalDevice physical, struct vkrt_resource_native *output,
                                  enum vkrt_resource_failure *failure) {
    if (!properties || !features || !physical || !output) return fail(failure, VKRT_RESOURCE_BAD_ARGUMENT);
    struct vkrt_resource_native n = {0};
    VkPhysicalDeviceProperties2 core = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties(physical, &core);
    if (core.properties.apiVersion < VK_API_VERSION_1_2) return fail(failure, VKRT_RESOURCE_API_VERSION);
    n.indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    n.inline_uniform.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES;
    n.maintenance7.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR;
    VkPhysicalDeviceVulkan13Properties v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
                                              .pNext = &n.maintenance7};
    n.indexing.pNext = core.properties.apiVersion >= VK_API_VERSION_1_3 ? (void *)&v13 : (void *)&n.inline_uniform;
    n.inline_uniform.pNext = &n.maintenance7;
    core.pNext = &n.indexing;
    properties(physical, &core);
    n.properties = core.properties;
    if (core.properties.apiVersion >= VK_API_VERSION_1_3) {
        n.inline_uniform.maxInlineUniformBlockSize = v13.maxInlineUniformBlockSize;
        n.inline_uniform.maxPerStageDescriptorInlineUniformBlocks = v13.maxPerStageDescriptorInlineUniformBlocks;
        n.inline_uniform.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks =
            v13.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks;
        n.inline_uniform.maxDescriptorSetInlineUniformBlocks = v13.maxDescriptorSetInlineUniformBlocks;
        n.inline_uniform.maxDescriptorSetUpdateAfterBindInlineUniformBlocks =
            v13.maxDescriptorSetUpdateAfterBindInlineUniformBlocks;
        n.max_inline_uniform_total_size = v13.maxInlineUniformTotalSize;
    }
    VkPhysicalDeviceMaintenance7FeaturesKHR m7 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR};
    n.inline_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES;
    n.inline_features.pNext = &m7;
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &n.inline_features};
    VkPhysicalDeviceFeatures2 supported = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
    features(physical, &supported);
    n.descriptor_indexing = v12.descriptorIndexing;
    n.has_maintenance7 = m7.maintenance7;
    n.indexing_features.descriptorBindingUniformBufferUpdateAfterBind = v12.descriptorBindingUniformBufferUpdateAfterBind;
    n.indexing_features.descriptorBindingSampledImageUpdateAfterBind = v12.descriptorBindingSampledImageUpdateAfterBind;
    n.indexing_features.descriptorBindingStorageBufferUpdateAfterBind = v12.descriptorBindingStorageBufferUpdateAfterBind;
    n.indexing_features.descriptorBindingStorageImageUpdateAfterBind = v12.descriptorBindingStorageImageUpdateAfterBind;
    n.indexing_features.descriptorBindingUniformTexelBufferUpdateAfterBind = v12.descriptorBindingUniformTexelBufferUpdateAfterBind;
    n.indexing_features.descriptorBindingStorageTexelBufferUpdateAfterBind = v12.descriptorBindingStorageTexelBufferUpdateAfterBind;
    /* All query chains were local. The captured snapshot must not retain any
     * address into this function's stack, including ignored optional structs. */
    n.indexing.pNext = n.inline_uniform.pNext = NULL;
    n.inline_features.pNext = NULL;
    *output = n;
    if (failure) *failure = VKRT_RESOURCE_OK;
    return true;
}

bool vkrt_query_resource_profile(PFN_vkGetPhysicalDeviceProperties2 properties, PFN_vkGetPhysicalDeviceFeatures2 features,
                                 VkPhysicalDevice physical, struct vkrt_resource_profile *output,
                                 enum vkrt_resource_failure *failure) {
    if (!output) return fail(failure, VKRT_RESOURCE_BAD_ARGUMENT);
    struct vkrt_resource_native native;
    if (!vkrt_capture_resource_native(properties, features, physical, &native, failure)) return false;
    return vkrt_plan_resource_profile(&native, output, failure);
}

/* The pipeline-layout VUIDs distinguish static set totals from dynamic totals,
 * although per-stage UBO/SSBO limits count both. The all-set checks use maxima
 * of core and update-after-bind limits. See VUIDs 03017/03018, 03023/03024,
 * 03029..03032 and 03037..03040, not an assumed division by stage count.
 * https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineLayoutCreateInfo.html
 */
bool vkrt_plan_resource_profile(const struct vkrt_resource_native *n, struct vkrt_resource_profile *output,
                                enum vkrt_resource_failure *failure) {
    if (!n || !output) return fail(failure, VKRT_RESOURCE_BAD_ARGUMENT);
    if (n->properties.apiVersion < VK_API_VERSION_1_2) return fail(failure, VKRT_RESOURCE_API_VERSION);
    const VkPhysicalDeviceLimits *c = &n->properties.limits;
    const VkPhysicalDeviceDescriptorIndexingProperties *u = &n->indexing;
    const VkPhysicalDeviceDescriptorIndexingFeatures *f = &n->indexing_features;
    bool v14 = n->properties.apiVersion >= VK_API_VERSION_1_4;
    const uint32_t core_min_stage[KIND_COUNT] = {16, v14 ? 15 : 12, 4, 16, 4, 4};
    const uint32_t core_min_set[KIND_COUNT] = {96, v14 ? 90 : 72, v14 ? 96 : 24, 96, v14 ? 144 : 24, 4};
    const uint32_t core_min_resources = v14 ? 200 : 128;
    const uint32_t native_stage[KIND_COUNT] = {c->maxPerStageDescriptorSamplers,       c->maxPerStageDescriptorUniformBuffers,
                                               c->maxPerStageDescriptorStorageBuffers, c->maxPerStageDescriptorSampledImages,
                                               c->maxPerStageDescriptorStorageImages,  c->maxPerStageDescriptorInputAttachments};
    const uint32_t native_set[KIND_COUNT] = {c->maxDescriptorSetSamplers,       c->maxDescriptorSetUniformBuffers,
                                             c->maxDescriptorSetStorageBuffers, c->maxDescriptorSetSampledImages,
                                             c->maxDescriptorSetStorageImages,  c->maxDescriptorSetInputAttachments};
    const uint32_t native_u_stage[KIND_COUNT] = {
        u->maxPerStageDescriptorUpdateAfterBindSamplers,       u->maxPerStageDescriptorUpdateAfterBindUniformBuffers,
        u->maxPerStageDescriptorUpdateAfterBindStorageBuffers, u->maxPerStageDescriptorUpdateAfterBindSampledImages,
        u->maxPerStageDescriptorUpdateAfterBindStorageImages,  u->maxPerStageDescriptorUpdateAfterBindInputAttachments};
    const uint32_t native_u_set[KIND_COUNT] = {
        u->maxDescriptorSetUpdateAfterBindSamplers,       u->maxDescriptorSetUpdateAfterBindUniformBuffers,
        u->maxDescriptorSetUpdateAfterBindStorageBuffers, u->maxDescriptorSetUpdateAfterBindSampledImages,
        u->maxDescriptorSetUpdateAfterBindStorageImages,  u->maxDescriptorSetUpdateAfterBindInputAttachments};
    const bool update[KIND_COUNT] = {
        f->descriptorBindingSampledImageUpdateAfterBind != 0,
        f->descriptorBindingUniformBufferUpdateAfterBind != 0,
        f->descriptorBindingStorageBufferUpdateAfterBind != 0,
        f->descriptorBindingSampledImageUpdateAfterBind || f->descriptorBindingUniformTexelBufferUpdateAfterBind,
        f->descriptorBindingStorageImageUpdateAfterBind || f->descriptorBindingStorageTexelBufferUpdateAfterBind,
        false};
    uint32_t all_min_set[KIND_COUNT], core_stage[KIND_COUNT], all_stage[KIND_COUNT];
    uint32_t core_set[KIND_COUNT], all_set[KIND_COUNT];
    for (unsigned k = 0; k < KIND_COUNT; ++k) {
        if (native_stage[k] < core_min_stage[k] || native_set[k] < core_min_set[k])
            return fail(failure, VKRT_RESOURCE_CORE_MINIMUM);
        uint32_t reserved = k == STORAGE ? AS_DESCRIPTORS : 0;
        core_stage[k] = subtract(native_stage[k], reserved);
        all_stage[k] = subtract(max32(native_stage[k], native_u_stage[k]), reserved);
        all_min_set[k] = core_min_set[k];
        if (update[k] && k != UNIFORM) all_min_set[k] = max32(all_min_set[k], 500000);
        if (core_stage[k] < core_min_stage[k]) return fail(failure, VKRT_RESOURCE_STAGE_HEADROOM);
    }
    if (c->maxDescriptorSetUniformBuffersDynamic < 8 || c->maxDescriptorSetStorageBuffersDynamic < 4)
        return fail(failure, VKRT_RESOURCE_CORE_MINIMUM);
    uint32_t dynamic_u = min32(c->maxDescriptorSetUniformBuffersDynamic, min32(subtract(core_stage[UNIFORM], core_min_set[UNIFORM]),
                                                                               subtract(all_stage[UNIFORM], all_min_set[UNIFORM])));
    uint32_t dynamic_s = min32(c->maxDescriptorSetStorageBuffersDynamic, min32(subtract(core_stage[STORAGE], core_min_set[STORAGE]),
                                                                               subtract(all_stage[STORAGE], all_min_set[STORAGE])));
    if (dynamic_u < 8 || dynamic_s < 4) return fail(failure, VKRT_RESOURCE_SET_HEADROOM);
    uint32_t core_resources = subtract(c->maxPerStageResources, AS_DESCRIPTORS);
    uint32_t all_resources = subtract(max32(c->maxPerStageResources, u->maxPerStageUpdateAfterBindResources), AS_DESCRIPTORS);
    if (core_resources < core_min_resources ||
        (n->descriptor_indexing && min32(u->maxPerStageUpdateAfterBindResources, all_resources) < 500000))
        return fail(failure, VKRT_RESOURCE_AGGREGATE_HEADROOM);
    uint64_t core_floor = sum_resources(core_min_set), all_floor = sum_resources(all_min_set);
    if (core_floor + 12 > core_resources || all_floor + 12 > all_resources) return fail(failure, VKRT_RESOURCE_AGGREGATE_HEADROOM);
    uint64_t dynamic_budget = core_resources - core_floor;
    if (dynamic_budget > all_resources - all_floor) dynamic_budget = all_resources - all_floor;
    if ((uint64_t)dynamic_u + dynamic_s > dynamic_budget) {
        uint64_t reduction = (uint64_t)dynamic_u + dynamic_s - dynamic_budget;
        uint32_t step = min32(dynamic_u - 8, reduction > UINT32_MAX ? UINT32_MAX : (uint32_t)reduction);
        dynamic_u -= step;
        reduction -= step;
        if (reduction > dynamic_s - 4) return fail(failure, VKRT_RESOURCE_AGGREGATE_HEADROOM);
        dynamic_s -= (uint32_t)reduction;
    }
    for (unsigned k = 0; k < KIND_COUNT; ++k) {
        uint32_t dynamic = k == UNIFORM ? dynamic_u : k == STORAGE ? dynamic_s : 0;
        uint32_t reserved = k == STORAGE ? AS_DESCRIPTORS : 0;
        all_set[k] = min32(subtract(max32(native_set[k], native_u_set[k]), reserved), subtract(all_stage[k], dynamic));
        core_set[k] = min32(subtract(native_set[k], reserved), subtract(core_stage[k], dynamic));
        if (all_set[k] < all_min_set[k] || core_set[k] < core_min_set[k]) return fail(failure, VKRT_RESOURCE_SET_HEADROOM);
    }
    if (!shrink_resources(all_set, all_min_set, (uint64_t)all_resources - dynamic_u - dynamic_s))
        return fail(failure, VKRT_RESOURCE_AGGREGATE_HEADROOM);
    for (unsigned k = 0; k < KIND_COUNT; ++k) core_set[k] = min32(core_set[k], all_set[k]);
    if (!shrink_resources(core_set, core_min_set, (uint64_t)core_resources - dynamic_u - dynamic_s))
        return fail(failure, VKRT_RESOURCE_AGGREGATE_HEADROOM);

    struct vkrt_resource_profile p = {0};
    p.limits = *c;
    p.indexing = *u;
    p.indexing.pNext = NULL;
    uint32_t *public_stage[KIND_COUNT] = {
        &p.limits.maxPerStageDescriptorSamplers,       &p.limits.maxPerStageDescriptorUniformBuffers,
        &p.limits.maxPerStageDescriptorStorageBuffers, &p.limits.maxPerStageDescriptorSampledImages,
        &p.limits.maxPerStageDescriptorStorageImages,  &p.limits.maxPerStageDescriptorInputAttachments};
    uint32_t *public_set[KIND_COUNT] = {&p.limits.maxDescriptorSetSamplers,       &p.limits.maxDescriptorSetUniformBuffers,
                                        &p.limits.maxDescriptorSetStorageBuffers, &p.limits.maxDescriptorSetSampledImages,
                                        &p.limits.maxDescriptorSetStorageImages,  &p.limits.maxDescriptorSetInputAttachments};
    uint32_t *public_u_stage[KIND_COUNT] = {&p.indexing.maxPerStageDescriptorUpdateAfterBindSamplers,
                                            &p.indexing.maxPerStageDescriptorUpdateAfterBindUniformBuffers,
                                            &p.indexing.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
                                            &p.indexing.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                            &p.indexing.maxPerStageDescriptorUpdateAfterBindStorageImages,
                                            &p.indexing.maxPerStageDescriptorUpdateAfterBindInputAttachments};
    uint32_t *public_u_set[KIND_COUNT] = {
        &p.indexing.maxDescriptorSetUpdateAfterBindSamplers,       &p.indexing.maxDescriptorSetUpdateAfterBindUniformBuffers,
        &p.indexing.maxDescriptorSetUpdateAfterBindStorageBuffers, &p.indexing.maxDescriptorSetUpdateAfterBindSampledImages,
        &p.indexing.maxDescriptorSetUpdateAfterBindStorageImages,  &p.indexing.maxDescriptorSetUpdateAfterBindInputAttachments};
    for (unsigned k = 0; k < KIND_COUNT; ++k) {
        *public_stage[k] = core_stage[k];
        *public_set[k] = core_set[k];
        *public_u_stage[k] = min32(native_u_stage[k], all_stage[k]);
        *public_u_set[k] = min32(native_u_set[k], all_set[k]);
        if (update[k]) {
            uint32_t stage_min = k == UNIFORM ? core_min_stage[k] : 500000;
            if (*public_u_stage[k] < stage_min || *public_u_set[k] < all_min_set[k] || *public_u_stage[k] < *public_stage[k] ||
                *public_u_set[k] < *public_set[k])
                return fail(failure, VKRT_RESOURCE_UPDATE_AFTER_BIND_MINIMUM);
        }
    }
    p.limits.maxDescriptorSetUniformBuffersDynamic = dynamic_u;
    p.limits.maxDescriptorSetStorageBuffersDynamic = dynamic_s;
    p.indexing.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic =
        min32(u->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic, dynamic_u);
    p.indexing.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic =
        min32(u->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic, dynamic_s);
    p.limits.maxPerStageResources = core_resources;
    p.indexing.maxPerStageUpdateAfterBindResources = min32(u->maxPerStageUpdateAfterBindResources, all_resources);
    p.inline_uniform = n->inline_uniform;
    p.inline_uniform.pNext = NULL;
    p.max_inline_uniform_total_size = n->max_inline_uniform_total_size;
    if (n->inline_features.inlineUniformBlock) {
        VkPhysicalDeviceInlineUniformBlockProperties *i = &p.inline_uniform;
        uint32_t all =
            min32(max32(i->maxDescriptorSetInlineUniformBlocks, i->maxDescriptorSetUpdateAfterBindInlineUniformBlocks),
                  max32(i->maxPerStageDescriptorInlineUniformBlocks, i->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks));
        i->maxDescriptorSetInlineUniformBlocks =
            min32(i->maxDescriptorSetInlineUniformBlocks, min32(i->maxPerStageDescriptorInlineUniformBlocks, all));
        i->maxDescriptorSetUpdateAfterBindInlineUniformBlocks = min32(i->maxDescriptorSetUpdateAfterBindInlineUniformBlocks, all);
        if (i->maxInlineUniformBlockSize < 256 || i->maxPerStageDescriptorInlineUniformBlocks < 4 ||
            i->maxDescriptorSetInlineUniformBlocks < 4 ||
            (n->properties.apiVersion >= VK_API_VERSION_1_3 && n->max_inline_uniform_total_size < 256) ||
            (n->inline_features.descriptorBindingInlineUniformBlockUpdateAfterBind &&
             (i->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks < i->maxPerStageDescriptorInlineUniformBlocks ||
              i->maxDescriptorSetUpdateAfterBindInlineUniformBlocks < i->maxDescriptorSetInlineUniformBlocks)))
            return fail(failure, VKRT_RESOURCE_INLINE_MINIMUM);
    }
    p.maintenance7 = n->maintenance7;
    p.maintenance7.pNext = NULL;
    if (n->has_maintenance7) {
        const VkPhysicalDeviceMaintenance7PropertiesKHR *m = &n->maintenance7;
        if (m->maxDescriptorSetTotalUniformBuffersDynamic < dynamic_u ||
            m->maxDescriptorSetTotalStorageBuffersDynamic < dynamic_s ||
            m->maxDescriptorSetTotalBuffersDynamic < (uint64_t)dynamic_u + dynamic_s)
            return fail(failure, VKRT_RESOURCE_MAINTENANCE7_MINIMUM);
        p.maintenance7.maxDescriptorSetTotalUniformBuffersDynamic = dynamic_u;
        p.maintenance7.maxDescriptorSetTotalStorageBuffersDynamic = dynamic_s;
        p.maintenance7.maxDescriptorSetTotalBuffersDynamic = dynamic_u + dynamic_s;
        p.maintenance7.maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic =
            p.indexing.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic;
        p.maintenance7.maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic =
            p.indexing.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic;
        p.maintenance7.maxDescriptorSetUpdateAfterBindTotalBuffersDynamic =
            p.maintenance7.maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic +
            p.maintenance7.maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic;
    }
    p.as_per_stage = p.as_per_set = AS_DESCRIPTORS;
    p.core_resource_bound = AS_DESCRIPTORS + sum_resources(core_set) + dynamic_u + dynamic_s;
    p.all_resource_bound = AS_DESCRIPTORS + sum_resources(all_set) + dynamic_u + dynamic_s;
    *output = p;
    if (failure) *failure = VKRT_RESOURCE_OK;
    return true;
}

#define CORE_LIMITS(F)                       \
    F(maxPerStageDescriptorSamplers)         \
    F(maxPerStageDescriptorUniformBuffers)   \
    F(maxPerStageDescriptorStorageBuffers)   \
    F(maxPerStageDescriptorSampledImages)    \
    F(maxPerStageDescriptorStorageImages)    \
    F(maxPerStageDescriptorInputAttachments) \
    F(maxPerStageResources)                  \
    F(maxDescriptorSetSamplers)              \
    F(maxDescriptorSetUniformBuffers)        \
    F(maxDescriptorSetUniformBuffersDynamic) \
    F(maxDescriptorSetStorageBuffers)        \
    F(maxDescriptorSetStorageBuffersDynamic) \
    F(maxDescriptorSetSampledImages)         \
    F(maxDescriptorSetStorageImages)         \
    F(maxDescriptorSetInputAttachments)
#define INDEXING_LIMITS(F)                                  \
    F(maxPerStageDescriptorUpdateAfterBindSamplers)         \
    F(maxPerStageDescriptorUpdateAfterBindUniformBuffers)   \
    F(maxPerStageDescriptorUpdateAfterBindStorageBuffers)   \
    F(maxPerStageDescriptorUpdateAfterBindSampledImages)    \
    F(maxPerStageDescriptorUpdateAfterBindStorageImages)    \
    F(maxPerStageDescriptorUpdateAfterBindInputAttachments) \
    F(maxPerStageUpdateAfterBindResources)                  \
    F(maxDescriptorSetUpdateAfterBindSamplers)              \
    F(maxDescriptorSetUpdateAfterBindUniformBuffers)        \
    F(maxDescriptorSetUpdateAfterBindUniformBuffersDynamic) \
    F(maxDescriptorSetUpdateAfterBindStorageBuffers)        \
    F(maxDescriptorSetUpdateAfterBindStorageBuffersDynamic) \
    F(maxDescriptorSetUpdateAfterBindSampledImages)         \
    F(maxDescriptorSetUpdateAfterBindStorageImages)         \
    F(maxDescriptorSetUpdateAfterBindInputAttachments)
#define INLINE_LIMITS(F)                                       \
    F(maxInlineUniformBlockSize)                               \
    F(maxPerStageDescriptorInlineUniformBlocks)                \
    F(maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks) \
    F(maxDescriptorSetInlineUniformBlocks)                     \
    F(maxDescriptorSetUpdateAfterBindInlineUniformBlocks)
#define DYNAMIC_LIMITS(F)                                        \
    F(maxDescriptorSetTotalUniformBuffersDynamic)                \
    F(maxDescriptorSetTotalStorageBuffersDynamic)                \
    F(maxDescriptorSetTotalBuffersDynamic)                       \
    F(maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic) \
    F(maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic) \
    F(maxDescriptorSetUpdateAfterBindTotalBuffersDynamic)

void vkrt_apply_resource_profile(const struct vkrt_resource_profile *profile, VkPhysicalDeviceProperties2 *properties) {
    if (!profile || !properties) return;
#define COPY_CORE(name) properties->properties.limits.name = profile->limits.name;
    CORE_LIMITS(COPY_CORE)
#undef COPY_CORE
    for (VkBaseOutStructure *p = properties->pNext; p; p = p->pNext) {
        switch (p->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES: {
                VkPhysicalDeviceDescriptorIndexingProperties *target = (void *)p;
#define COPY_INDEXING(name) target->name = profile->indexing.name;
                INDEXING_LIMITS(COPY_INDEXING)
#undef COPY_INDEXING
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: {
                VkPhysicalDeviceVulkan12Properties *target = (void *)p;
#define COPY_INDEXING(name) target->name = profile->indexing.name;
                INDEXING_LIMITS(COPY_INDEXING)
#undef COPY_INDEXING
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES: {
                VkPhysicalDeviceInlineUniformBlockProperties *target = (void *)p;
#define COPY_INLINE(name) target->name = profile->inline_uniform.name;
                INLINE_LIMITS(COPY_INLINE)
#undef COPY_INLINE
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES: {
                VkPhysicalDeviceVulkan13Properties *target = (void *)p;
#define COPY_INLINE(name) target->name = profile->inline_uniform.name;
                INLINE_LIMITS(COPY_INLINE)
#undef COPY_INLINE
                target->maxInlineUniformTotalSize = profile->max_inline_uniform_total_size;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR: {
                VkPhysicalDeviceMaintenance7PropertiesKHR *target = (void *)p;
#define COPY_DYNAMIC(name) target->name = profile->maintenance7.name;
                DYNAMIC_LIMITS(COPY_DYNAMIC)
#undef COPY_DYNAMIC
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR: {
                VkPhysicalDeviceAccelerationStructurePropertiesKHR *target = (void *)p;
                target->maxPerStageDescriptorAccelerationStructures = profile->as_per_stage;
                target->maxDescriptorSetAccelerationStructures = profile->as_per_set;
                /* In the pinned 1.4.357 rules, 03572/03574 count ordinary AS
                 * bindings too. These ALL-layout caps retain the same reserved
                 * AS budget; they do not enable the optional UAB binding bit. */
                target->maxPerStageDescriptorUpdateAfterBindAccelerationStructures = profile->as_per_stage;
                target->maxDescriptorSetUpdateAfterBindAccelerationStructures = profile->as_per_set;
                break;
            }
            default:
                break;
        }
    }
}
