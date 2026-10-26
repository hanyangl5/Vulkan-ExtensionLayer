/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_resource_profile.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define REQUIRE(x)                                          \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            ++failures;                                     \
        }                                                   \
    } while (0)
static uint32_t maximum(uint32_t a, uint32_t b) { return a > b ? a : b; }
static struct vkrt_resource_native native(void) {
    struct vkrt_resource_native n = {0};
    n.properties.apiVersion = VK_API_VERSION_1_4;
    VkPhysicalDeviceLimits *c = &n.properties.limits;
    c->maxPerStageDescriptorSamplers = c->maxPerStageDescriptorUniformBuffers = c->maxPerStageDescriptorStorageBuffers =
        c->maxPerStageDescriptorSampledImages = c->maxPerStageDescriptorStorageImages = c->maxPerStageDescriptorInputAttachments =
            c->maxDescriptorSetSamplers = c->maxDescriptorSetUniformBuffers = c->maxDescriptorSetStorageBuffers =
                c->maxDescriptorSetSampledImages = c->maxDescriptorSetStorageImages = c->maxDescriptorSetInputAttachments = 1048576;
    c->maxDescriptorSetUniformBuffersDynamic = 15;
    c->maxDescriptorSetStorageBuffersDynamic = 16;
    c->maxPerStageResources = UINT32_MAX;
    c->maxPushConstantsSize = 256;
    c->maxMemoryAllocationCount = UINT32_MAX;
    c->maxSamplerAllocationCount = 4000;
    c->maxBoundDescriptorSets = 32;
    VkPhysicalDeviceDescriptorIndexingProperties *u = &n.indexing;
    u->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    u->maxPerStageDescriptorUpdateAfterBindSamplers = u->maxPerStageDescriptorUpdateAfterBindUniformBuffers =
        u->maxPerStageDescriptorUpdateAfterBindStorageBuffers = u->maxPerStageDescriptorUpdateAfterBindSampledImages =
            u->maxPerStageDescriptorUpdateAfterBindStorageImages = u->maxPerStageDescriptorUpdateAfterBindInputAttachments =
                u->maxDescriptorSetUpdateAfterBindSamplers = u->maxDescriptorSetUpdateAfterBindUniformBuffers =
                    u->maxDescriptorSetUpdateAfterBindStorageBuffers = u->maxDescriptorSetUpdateAfterBindSampledImages =
                        u->maxDescriptorSetUpdateAfterBindStorageImages = u->maxDescriptorSetUpdateAfterBindInputAttachments =
                            1048576;
    u->maxPerStageUpdateAfterBindResources = u->maxUpdateAfterBindDescriptorsInAllPools = UINT32_MAX;
    u->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 15;
    u->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 16;
    VkPhysicalDeviceDescriptorIndexingFeatures *f = &n.indexing_features;
    f->descriptorBindingUniformBufferUpdateAfterBind = f->descriptorBindingSampledImageUpdateAfterBind =
        f->descriptorBindingStorageBufferUpdateAfterBind = f->descriptorBindingStorageImageUpdateAfterBind =
            f->descriptorBindingUniformTexelBufferUpdateAfterBind = f->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
    n.descriptor_indexing = VK_TRUE;
    n.inline_features.inlineUniformBlock = n.inline_features.descriptorBindingInlineUniformBlockUpdateAfterBind = VK_TRUE;
    n.inline_uniform.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES;
    n.inline_uniform.maxInlineUniformBlockSize = 256;
    n.inline_uniform.maxPerStageDescriptorInlineUniformBlocks =
        n.inline_uniform.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks =
            n.inline_uniform.maxDescriptorSetInlineUniformBlocks =
                n.inline_uniform.maxDescriptorSetUpdateAfterBindInlineUniformBlocks = 32;
    n.max_inline_uniform_total_size = 3584;
    n.has_maintenance7 = VK_TRUE;
    n.maintenance7.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR;
    n.maintenance7.maxDescriptorSetTotalUniformBuffersDynamic = 15;
    n.maintenance7.maxDescriptorSetTotalStorageBuffersDynamic = 16;
    n.maintenance7.maxDescriptorSetTotalBuffersDynamic = 31;
    n.maintenance7.maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic = 15;
    n.maintenance7.maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic = 16;
    n.maintenance7.maxDescriptorSetUpdateAfterBindTotalBuffersDynamic = 31;
    return n;
}

/* Independent complete-layout oracle: these are descriptor totals which can
 * be distributed among six RT stages and therefore become one compute stage.
 * The native SSBO total includes STATIC + DYNAMIC + emulated AS descriptors. */
static void bounds(const struct vkrt_resource_native *n, const struct vkrt_resource_profile *p) {
    const VkPhysicalDeviceLimits *c = &p->limits, *nc = &n->properties.limits;
    const VkPhysicalDeviceDescriptorIndexingProperties *u = &p->indexing, *nu = &n->indexing;
    uint64_t core_s = (uint64_t)c->maxDescriptorSetStorageBuffers + c->maxDescriptorSetStorageBuffersDynamic + p->as_per_set;
    uint64_t all_s = (uint64_t)maximum(c->maxDescriptorSetStorageBuffers, u->maxDescriptorSetUpdateAfterBindStorageBuffers) +
                     maximum(c->maxDescriptorSetStorageBuffersDynamic, u->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic) +
                     p->as_per_set;
    REQUIRE(core_s <= nc->maxPerStageDescriptorStorageBuffers);
    REQUIRE((uint64_t)c->maxDescriptorSetStorageBuffers + p->as_per_set <= nc->maxDescriptorSetStorageBuffers);
    REQUIRE(all_s <= maximum(nc->maxPerStageDescriptorStorageBuffers, nu->maxPerStageDescriptorUpdateAfterBindStorageBuffers));
    REQUIRE((uint64_t)maximum(c->maxDescriptorSetStorageBuffers, u->maxDescriptorSetUpdateAfterBindStorageBuffers) +
                p->as_per_set <=
            maximum(nc->maxDescriptorSetStorageBuffers, nu->maxDescriptorSetUpdateAfterBindStorageBuffers));
    REQUIRE((uint64_t)c->maxDescriptorSetUniformBuffers + c->maxDescriptorSetUniformBuffersDynamic <=
            nc->maxPerStageDescriptorUniformBuffers);
    REQUIRE((uint64_t)maximum(c->maxDescriptorSetUniformBuffers, u->maxDescriptorSetUpdateAfterBindUniformBuffers) +
                maximum(c->maxDescriptorSetUniformBuffersDynamic, u->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic) <=
            maximum(nc->maxPerStageDescriptorUniformBuffers, nu->maxPerStageDescriptorUpdateAfterBindUniformBuffers));
    uint64_t core_total = core_s + c->maxDescriptorSetUniformBuffers + c->maxDescriptorSetUniformBuffersDynamic +
                          c->maxDescriptorSetSampledImages + c->maxDescriptorSetStorageImages + c->maxDescriptorSetInputAttachments;
    uint64_t all_total =
        all_s + maximum(c->maxDescriptorSetUniformBuffers, u->maxDescriptorSetUpdateAfterBindUniformBuffers) +
        maximum(c->maxDescriptorSetUniformBuffersDynamic, u->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic) +
        maximum(c->maxDescriptorSetSampledImages, u->maxDescriptorSetUpdateAfterBindSampledImages) +
        maximum(c->maxDescriptorSetStorageImages, u->maxDescriptorSetUpdateAfterBindStorageImages) +
        maximum(c->maxDescriptorSetInputAttachments, u->maxDescriptorSetUpdateAfterBindInputAttachments);
    REQUIRE(core_total <= nc->maxPerStageResources);
    REQUIRE(all_total <= maximum(nc->maxPerStageResources, nu->maxPerStageUpdateAfterBindResources));
    REQUIRE(p->core_resource_bound >= core_total && p->all_resource_bound >= all_total);
    REQUIRE((uint64_t)c->maxPerStageDescriptorStorageBuffers + p->as_per_stage <= nc->maxPerStageDescriptorStorageBuffers);
    REQUIRE((uint64_t)maximum(c->maxPerStageDescriptorStorageBuffers, u->maxPerStageDescriptorUpdateAfterBindStorageBuffers) +
                p->as_per_stage <=
            maximum(nc->maxPerStageDescriptorStorageBuffers, nu->maxPerStageDescriptorUpdateAfterBindStorageBuffers));
    REQUIRE(c->maxMemoryAllocationCount == nc->maxMemoryAllocationCount);
    REQUIRE(c->maxSamplerAllocationCount == nc->maxSamplerAllocationCount);
    REQUIRE(c->maxBoundDescriptorSets == nc->maxBoundDescriptorSets);
    REQUIRE(c->maxPushConstantsSize == nc->maxPushConstantsSize);
#define IMAGE_BOUND(name)                                                                              \
    REQUIRE(c->maxDescriptorSet##name <= nc->maxPerStageDescriptor##name);                             \
    REQUIRE(maximum(c->maxDescriptorSet##name, u->maxDescriptorSetUpdateAfterBind##name) <=            \
            maximum(nc->maxPerStageDescriptor##name, nu->maxPerStageDescriptorUpdateAfterBind##name)); \
    REQUIRE(maximum(c->maxDescriptorSet##name, u->maxDescriptorSetUpdateAfterBind##name) <=            \
            maximum(nc->maxDescriptorSet##name, nu->maxDescriptorSetUpdateAfterBind##name));
    IMAGE_BOUND(Samplers)
    IMAGE_BOUND(SampledImages)
    IMAGE_BOUND(StorageImages)
    IMAGE_BOUND(InputAttachments)
#undef IMAGE_BOUND
}

static void rejected(struct vkrt_resource_native n) {
    struct vkrt_resource_profile p, saved;
    memset(&p, 0xa5, sizeof(p));
    saved = p;
    enum vkrt_resource_failure why = VKRT_RESOURCE_OK;
    REQUIRE(!vkrt_plan_resource_profile(&n, &p, &why));
    REQUIRE(why != VKRT_RESOURCE_OK && !memcmp(&p, &saved, sizeof(p)));
}

static void planning(void) {
    struct vkrt_resource_native n = native(), saved = n;
    struct vkrt_resource_profile p;
    enum vkrt_resource_failure why;
    REQUIRE(vkrt_plan_resource_profile(&n, &p, &why) && why == VKRT_RESOURCE_OK);
    REQUIRE(!memcmp(&n, &saved, sizeof(n)));
    REQUIRE(p.as_per_set == 16 && p.as_per_stage == 16);
    REQUIRE(p.limits.maxPerStageDescriptorStorageBuffers == 1048560);
    REQUIRE(p.limits.maxDescriptorSetStorageBuffers == 1048544);
    REQUIRE(p.limits.maxDescriptorSetUniformBuffers == 1048561);
    REQUIRE(p.indexing.maxDescriptorSetUpdateAfterBindStorageBuffers == 1048544);
    bounds(&n, &p);
    n = native();
    n.properties.apiVersion = VK_API_VERSION_1_1;
    rejected(n);
    n = native();
    n.properties.limits.maxPerStageDescriptorStorageBuffers = 16;
    rejected(n);
    n = native();
    n.properties.limits.maxDescriptorSetStorageBuffers = 111;
    rejected(n);
    n = native();
    n.properties.limits.maxPerStageDescriptorUniformBuffers = 97;
    rejected(n);
    n = native();
    n.properties.limits.maxDescriptorSetUniformBuffersDynamic = 7;
    rejected(n);
    n = native();
    n.properties.limits.maxDescriptorSetStorageBuffersDynamic = 3;
    rejected(n);
    n = native();
    n.properties.limits.maxPerStageResources = 200;
    rejected(n);
    n = native();
    n.indexing.maxPerStageDescriptorUpdateAfterBindStorageBuffers = 499999;
    rejected(n);
    n = native();
    n.indexing.maxDescriptorSetUpdateAfterBindSampledImages = 499999;
    rejected(n);
    n = native();
    n.inline_uniform.maxPerStageDescriptorInlineUniformBlocks = 3;
    rejected(n);
    n = native();
    n.max_inline_uniform_total_size = 255;
    rejected(n);
    n = native();
    n.maintenance7.maxDescriptorSetTotalBuffersDynamic = 30;
    rejected(n);

    /* A UAB maximum smaller than the core maximum is legal when the matching
     * UAB feature is absent. The max-pair proof must still hold. */
    n = native();
    n.indexing_features.descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
    n.indexing.maxPerStageDescriptorUpdateAfterBindStorageBuffers = 500000;
    n.indexing.maxDescriptorSetUpdateAfterBindStorageBuffers = 500000;
    REQUIRE(vkrt_plan_resource_profile(&n, &p, &why));
    bounds(&n, &p);

    /* These independent maxima do not fit simultaneously in one native stage;
     * the planner reduces global set totals without violating any minimum. */
    n = native();
    n.properties.limits.maxPerStageResources = n.indexing.maxPerStageUpdateAfterBindResources = 2000000;
    REQUIRE(vkrt_plan_resource_profile(&n, &p, &why));
    bounds(&n, &p);
    REQUIRE(p.indexing.maxDescriptorSetUpdateAfterBindStorageBuffers >= 500000);
    REQUIRE(p.indexing.maxDescriptorSetUpdateAfterBindSampledImages >= 500000);
    REQUIRE(p.indexing.maxDescriptorSetUpdateAfterBindStorageImages >= 500000);

    /* A small, no-UAB Vulkan 1.2 profile exactly fits static/dynamic/AS totals. */
    n = native();
    n.properties.apiVersion = VK_API_VERSION_1_2;
    memset(&n.indexing, 0, sizeof(n.indexing));
    memset(&n.indexing_features, 0, sizeof(n.indexing_features));
    n.descriptor_indexing = n.has_maintenance7 = VK_FALSE;
    memset(&n.inline_features, 0, sizeof(n.inline_features));
    VkPhysicalDeviceLimits *c = &n.properties.limits;
    c->maxPerStageDescriptorSamplers = c->maxDescriptorSetSamplers = 96;
    c->maxPerStageDescriptorUniformBuffers = 80;
    c->maxDescriptorSetUniformBuffers = 72;
    c->maxPerStageDescriptorStorageBuffers = 44;
    c->maxDescriptorSetStorageBuffers = 40;
    c->maxPerStageDescriptorSampledImages = c->maxDescriptorSetSampledImages = 96;
    c->maxPerStageDescriptorStorageImages = c->maxDescriptorSetStorageImages = 24;
    c->maxPerStageDescriptorInputAttachments = c->maxDescriptorSetInputAttachments = 4;
    c->maxPerStageResources = 248;
    REQUIRE(vkrt_plan_resource_profile(&n, &p, &why));
    REQUIRE(p.limits.maxDescriptorSetUniformBuffersDynamic == 8 && p.limits.maxDescriptorSetStorageBuffersDynamic == 4);
    bounds(&n, &p);
    --c->maxPerStageResources;
    rejected(n);
}

static uint32_t random_state = 0x5eeda511;
static uint32_t next_random(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}
static void adversarial(void) {
    for (unsigned iteration = 0; iteration < 1001; ++iteration) {
        struct vkrt_resource_native n = native();
        VkPhysicalDeviceLimits *c = &n.properties.limits;
        VkPhysicalDeviceDescriptorIndexingProperties *u = &n.indexing;
        uint32_t *cs[] = {&c->maxPerStageDescriptorSamplers,       &c->maxPerStageDescriptorUniformBuffers,
                          &c->maxPerStageDescriptorStorageBuffers, &c->maxPerStageDescriptorSampledImages,
                          &c->maxPerStageDescriptorStorageImages,  &c->maxPerStageDescriptorInputAttachments};
        uint32_t *ct[] = {&c->maxDescriptorSetSamplers,       &c->maxDescriptorSetUniformBuffers,
                          &c->maxDescriptorSetStorageBuffers, &c->maxDescriptorSetSampledImages,
                          &c->maxDescriptorSetStorageImages,  &c->maxDescriptorSetInputAttachments};
        uint32_t *us[] = {
            &u->maxPerStageDescriptorUpdateAfterBindSamplers,       &u->maxPerStageDescriptorUpdateAfterBindUniformBuffers,
            &u->maxPerStageDescriptorUpdateAfterBindStorageBuffers, &u->maxPerStageDescriptorUpdateAfterBindSampledImages,
            &u->maxPerStageDescriptorUpdateAfterBindStorageImages,  &u->maxPerStageDescriptorUpdateAfterBindInputAttachments};
        uint32_t *ut[] = {&u->maxDescriptorSetUpdateAfterBindSamplers,       &u->maxDescriptorSetUpdateAfterBindUniformBuffers,
                          &u->maxDescriptorSetUpdateAfterBindStorageBuffers, &u->maxDescriptorSetUpdateAfterBindSampledImages,
                          &u->maxDescriptorSetUpdateAfterBindStorageImages,  &u->maxDescriptorSetUpdateAfterBindInputAttachments};
        for (unsigned k = 0; k < 6; ++k) {
            *cs[k] = 600000 + next_random() % 500000;
            *ct[k] = 600000 + next_random() % 500000;
            *us[k] = *cs[k] + next_random() % 500000;
            *ut[k] = *ct[k] + next_random() % 500000;
            if (iteration == 1000) *cs[k] = *ct[k] = *us[k] = *ut[k] = UINT32_MAX;
        }
        c->maxPerStageResources = 2000000 + next_random() % 2000000;
        u->maxPerStageUpdateAfterBindResources = c->maxPerStageResources + next_random() % 1000000;
        c->maxDescriptorSetUniformBuffersDynamic = 8 + next_random() % 24;
        c->maxDescriptorSetStorageBuffersDynamic = 4 + next_random() % 32;
        n.maintenance7.maxDescriptorSetTotalUniformBuffersDynamic = c->maxDescriptorSetUniformBuffersDynamic;
        n.maintenance7.maxDescriptorSetTotalStorageBuffersDynamic = c->maxDescriptorSetStorageBuffersDynamic;
        n.maintenance7.maxDescriptorSetTotalBuffersDynamic =
            c->maxDescriptorSetUniformBuffersDynamic + c->maxDescriptorSetStorageBuffersDynamic;
        if (iteration == 1000) {
            c->maxPerStageResources = u->maxPerStageUpdateAfterBindResources = UINT32_MAX;
            c->maxDescriptorSetUniformBuffersDynamic = c->maxDescriptorSetStorageBuffersDynamic = UINT32_MAX;
            n.has_maintenance7 = VK_FALSE;
        }
        struct vkrt_resource_profile p;
        enum vkrt_resource_failure why;
        REQUIRE(vkrt_plan_resource_profile(&n, &p, &why));
        if (why == VKRT_RESOURCE_OK) bounds(&n, &p);
    }
}

static void application(void) {
    struct vkrt_resource_native n = native();
    struct vkrt_resource_profile p;
    REQUIRE(vkrt_plan_resource_profile(&n, &p, NULL));
    struct {
        VkStructureType sType;
        void *pNext;
        uint64_t sentinel;
    } tail = {VK_STRUCTURE_TYPE_MAX_ENUM, NULL, UINT64_C(0xabcdef0123456789)};
    VkPhysicalDeviceMaintenance7PropertiesKHR m = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR,
                                                   .pNext = &tail,
                                                   .robustFragmentShadingRateAttachmentAccess = VK_TRUE};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR a = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR, .pNext = &m, .maxGeometryCount = 777};
    VkPhysicalDeviceInlineUniformBlockProperties i = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES,
                                                      .pNext = &a};
    VkPhysicalDeviceDescriptorIndexingProperties u = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES,
                                                      .pNext = &i,
                                                      .quadDivergentImplicitLod = VK_TRUE};
    VkPhysicalDeviceProperties2 core = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &u};
    core.properties.deviceID = 123;
    core.properties.limits.maxPushConstantsSize = 192;
    vkrt_apply_resource_profile(&p, &core);
    REQUIRE(core.pNext == &u && u.pNext == &i && i.pNext == &a && a.pNext == &m && m.pNext == &tail);
    REQUIRE(tail.sentinel == UINT64_C(0xabcdef0123456789));
    REQUIRE(a.maxGeometryCount == 777 && a.maxDescriptorSetAccelerationStructures == 16 &&
            !a.maxDescriptorSetUpdateAfterBindAccelerationStructures);
    REQUIRE(u.quadDivergentImplicitLod && m.robustFragmentShadingRateAttachmentAccess && core.properties.deviceID == 123);
    REQUIRE(core.properties.limits.maxPushConstantsSize == 192);
    REQUIRE(u.maxDescriptorSetUpdateAfterBindStorageBuffers == p.indexing.maxDescriptorSetUpdateAfterBindStorageBuffers);
    VkPhysicalDeviceVulkan13Properties v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = &tail};
    VkPhysicalDeviceVulkan12Properties v12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, .pNext = &v13, .shaderDenormPreserveFloat32 = VK_TRUE};
    core.pNext = &v12;
    vkrt_apply_resource_profile(&p, &core);
    REQUIRE(core.pNext == &v12 && v12.pNext == &v13 && v13.pNext == &tail && v12.shaderDenormPreserveFloat32);
    REQUIRE(v12.maxDescriptorSetUpdateAfterBindStorageBuffers == p.indexing.maxDescriptorSetUpdateAfterBindStorageBuffers);
    REQUIRE(v13.maxDescriptorSetInlineUniformBlocks == p.inline_uniform.maxDescriptorSetInlineUniformBlocks &&
            v13.maxInlineUniformTotalSize == p.max_inline_uniform_total_size);
}

int main(void) {
    planning();
    adversarial();
    application();
    printf("Flat RTP descriptor budget: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
