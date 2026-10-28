/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_software_profile.h"
#include "bvh_ir_builder.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(value)                                            \
    do {                                                        \
        if (!(value)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); \
            ++failures;                                         \
        }                                                       \
    } while (0)

static struct vkrt_resource_profile resources(void) {
    struct vkrt_resource_profile r;
    memset(&r, 0, sizeof(r));
    r.as_per_stage = r.as_per_set = 16;
    r.limits.maxPerStageDescriptorStorageBuffers = 73;
    r.limits.maxDescriptorSetStorageBuffers = 79;
    r.limits.maxPerStageResources = 131;
    r.indexing.maxDescriptorSetUpdateAfterBindStorageBuffers = 83;
    r.indexing.maxPerStageUpdateAfterBindResources = 139;
    r.inline_uniform.maxInlineUniformBlockSize = 512;
    r.inline_uniform.maxDescriptorSetInlineUniformBlocks = 7;
    r.max_inline_uniform_total_size = 4096;
    r.maintenance7.maxDescriptorSetTotalBuffersDynamic = 17;
    r.core_resource_bound = 211;
    r.all_resource_bound = 223;
    /* The planned value snapshot must not retain borrowed query-chain links. */
    r.indexing.pNext = r.inline_uniform.pNext = r.maintenance7.pNext = (void *)(uintptr_t)1;
    return r;
}
static void expect_as(VkPhysicalDeviceAccelerationStructurePropertiesKHR *p, uint32_t stage, uint32_t set) {
    p->maxGeometryCount = UINT64_C(16777215);
    p->maxInstanceCount = UINT64_C(16777215);
    p->maxPrimitiveCount = UINT64_C(536870911);
    p->maxPerStageDescriptorAccelerationStructures = stage;
    p->maxPerStageDescriptorUpdateAfterBindAccelerationStructures = stage;
    p->maxDescriptorSetAccelerationStructures = set;
    p->maxDescriptorSetUpdateAfterBindAccelerationStructures = set;
    p->minAccelerationStructureScratchOffsetAlignment = 256;
}
static void expect_rtp(VkPhysicalDeviceRayTracingPipelinePropertiesKHR *p) {
    p->shaderGroupHandleSize = 32;
    p->maxRayRecursionDepth = 31;
    p->maxShaderGroupStride = 4096;
    p->shaderGroupBaseAlignment = 64;
    p->shaderGroupHandleCaptureReplaySize = 0;
    p->maxRayDispatchInvocationCount = 1073741824;
    p->shaderGroupHandleAlignment = 32;
    p->maxRayHitAttributeSize = 32;
}
static void test_init(void) {
    struct vkrt_resource_profile r = resources(), saved = r;
    struct vkrt_software_rt_profile p;
    memset(&p, 0xa5, sizeof(p));
    vkrt_software_rt_profile_init(&r, &p);
    CHECK(!memcmp(&r, &saved, sizeof(r)));
    CHECK(p.resources.limits.maxPerStageDescriptorStorageBuffers == 73 && p.resources.core_resource_bound == 211 &&
          p.resources.all_resource_bound == 223);
    CHECK(!p.resources.indexing.pNext && !p.resources.inline_uniform.pNext && !p.resources.maintenance7.pNext);
    CHECK(p.resources.indexing.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES &&
          p.resources.inline_uniform.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES &&
          p.resources.maintenance7.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR);
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as = {0};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp = {0};
    as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    rtp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    expect_as(&as, 16, 16);
    expect_rtp(&rtp);
    CHECK(!memcmp(&p.as, &as, sizeof(as)) && !memcmp(&p.rtp, &rtp, sizeof(rtp)));
    /* No hardcoded descriptor budget may replace the input resource proof. */
    p.resources.as_per_stage = 19;
    p.resources.as_per_set = 23;
    vkrt_software_rt_profile_init(&p.resources, &p);
    expect_as(&as, 19, 23);
    CHECK(!memcmp(&p.as, &as, sizeof(as)) && p.resources.as_per_stage == 19 && p.resources.as_per_set == 23);
    struct vkrt_software_rt_profile untouched = p;
    vkrt_software_rt_profile_init(NULL, &p);
    vkrt_software_rt_profile_init(&r, NULL);
    CHECK(!memcmp(&p, &untouched, sizeof(p)));
}
static void test_apply(unsigned poison) {
    struct vkrt_resource_profile r = resources();
    struct vkrt_software_rt_profile p;
    vkrt_software_rt_profile_init(&r, &p);
    VkPhysicalDeviceProperties2 core;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as[2];
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp[2];
    VkPhysicalDeviceIDProperties unrelated;
    VkPhysicalDeviceDescriptorIndexingProperties indexing;
    memset(&core, poison, sizeof(core));
    memset(as, poison, sizeof(as));
    memset(rtp, poison, sizeof(rtp));
    memset(&unrelated, poison, sizeof(unrelated));
    memset(&indexing, poison, sizeof(indexing));
    core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    core.pNext = &as[0];
    for (unsigned n = 0; n < 2; ++n) {
        as[n].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        rtp[n].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    }
    as[0].pNext = &unrelated;
    unrelated.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    unrelated.pNext = &rtp[0];
    rtp[0].pNext = &as[1];
    as[1].pNext = &indexing;
    indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    indexing.pNext = &rtp[1];
    rtp[1].pNext = NULL;
    const VkPhysicalDeviceIDProperties saved = unrelated;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR expected_as[2];
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR expected_rtp[2];
    memcpy(expected_as, as, sizeof(as));
    memcpy(expected_rtp, rtp, sizeof(rtp));
    for (unsigned n = 0; n < 2; ++n) {
        expect_as(&expected_as[n], 16, 16);
        expect_rtp(&expected_rtp[n]);
    }
    const uint32_t saved_vendor = core.properties.vendorID, saved_push = core.properties.limits.maxPushConstantsSize;
    const uint32_t saved_pool_limit = indexing.maxUpdateAfterBindDescriptorsInAllPools;
    vkrt_apply_software_rt_profile(&p, &core);
    CHECK(!memcmp(as, expected_as, sizeof(as)) && !memcmp(rtp, expected_rtp, sizeof(rtp)));
    CHECK(!memcmp(&unrelated, &saved, sizeof(unrelated)));
    CHECK(core.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 && core.pNext == &as[0] &&
          core.properties.vendorID == saved_vendor && core.properties.limits.maxPushConstantsSize == saved_push);
    CHECK(core.properties.limits.maxPerStageDescriptorStorageBuffers == 73 &&
          core.properties.limits.maxDescriptorSetStorageBuffers == 79 && core.properties.limits.maxPerStageResources == 131);
    CHECK(indexing.maxDescriptorSetUpdateAfterBindStorageBuffers == 83 && indexing.maxPerStageUpdateAfterBindResources == 139 &&
          indexing.maxUpdateAfterBindDescriptorsInAllPools == saved_pool_limit && indexing.pNext == &rtp[1]);
    const VkPhysicalDeviceProperties2 applied = core;
    vkrt_apply_software_rt_profile(NULL, &core);
    vkrt_apply_software_rt_profile(&p, NULL);
    CHECK(!memcmp(&core, &applied, sizeof(core)));
}
static void test_aliases(void) {
    struct vkrt_resource_profile r = resources();
    struct vkrt_software_rt_profile p;
    vkrt_software_rt_profile_init(&r, &p);
    /* Source-owned nodes may participate in the acyclic destination chain. */
    p.as.pNext = &p.rtp;
    p.rtp.pNext = &p.resources.indexing;
    p.resources.indexing.pNext = NULL;
    VkPhysicalDeviceProperties2 core = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p.as};
    vkrt_apply_software_rt_profile(&p, &core);
    CHECK(p.as.pNext == &p.rtp && p.rtp.pNext == &p.resources.indexing && !p.resources.indexing.pNext);
    CHECK(p.as.maxPrimitiveCount == 536870911 && p.rtp.maxRayDispatchInvocationCount == 1073741824);
    VkPhysicalDeviceVulkan12Properties v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceMaintenance7PropertiesKHR m7 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR};
    v12.pNext = &v13;
    v13.pNext = &m7;
    v12.driverID = VK_DRIVER_ID_MESA_LLVMPIPE;
    v13.maxSubgroupSize = 97;
    core.pNext = &v12;
    vkrt_apply_software_rt_profile(&p, &core);
    CHECK(v12.maxDescriptorSetUpdateAfterBindStorageBuffers == 83 && v12.maxPerStageUpdateAfterBindResources == 139 &&
          v12.driverID == VK_DRIVER_ID_MESA_LLVMPIPE && v12.pNext == &v13);
    CHECK(v13.maxInlineUniformBlockSize == 512 && v13.maxDescriptorSetInlineUniformBlocks == 7 &&
          v13.maxInlineUniformTotalSize == 4096 && v13.maxSubgroupSize == 97 && v13.pNext == &m7);
    CHECK(m7.maxDescriptorSetTotalBuffersDynamic == 17 && !m7.pNext);
}
static void test_formats(void) {
    const VkFormat supported[] = {VK_FORMAT_R32G32_SFLOAT,  VK_FORMAT_R32G32B32_SFLOAT,   VK_FORMAT_R32G32B32A32_SFLOAT,
                                  VK_FORMAT_R16G16_SFLOAT,  VK_FORMAT_R16G16B16_SFLOAT,   VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_FORMAT_R16G16_SNORM,   VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R8G8_SNORM,
                                  VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R16G16_UNORM,       VK_FORMAT_R16G16B16A16_UNORM,
                                  VK_FORMAT_R8G8_UNORM,     VK_FORMAT_R8G8B8A8_UNORM,     VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    VkAccelerationStructureGeometryKHR geometry = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                                                   .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR};
    geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    for (int format = 0; format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; ++format) {
        bool expected = false;
        for (unsigned n = 0; n < sizeof(supported) / sizeof(supported[0]); ++n) expected |= format == (int)supported[n];
        geometry.geometry.triangles.vertexFormat = (VkFormat)format;
        CHECK(bvh_supported_vertex_format((VkFormat)format) == expected);
        CHECK(bvh_ir_supported_geometry(&geometry) == expected);
    }
    const VkFormat invalid[] = {(VkFormat)-1, VK_FORMAT_MAX_ENUM, VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, VK_FORMAT_R16G16B16_SNORM,
                                VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    for (unsigned n = 0; n < sizeof(invalid) / sizeof(invalid[0]); ++n) {
        CHECK(!bvh_supported_vertex_format(invalid[n]));
        geometry.geometry.triangles.vertexFormat = invalid[n];
        CHECK(!bvh_ir_supported_geometry(&geometry));
    }
    geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geometry.geometry.triangles.vertexStride = UINT64_C(1) << 32;
    CHECK(!bvh_ir_supported_geometry(&geometry));
    geometry.geometry.triangles.vertexStride = 12;
    geometry.pNext = (void *)(uintptr_t)1;
    CHECK(!bvh_ir_supported_geometry(&geometry));
    geometry.pNext = NULL;
    CHECK(bvh_ir_supported_geometry(&geometry));
}
int main(void) {
    test_init();
    test_apply(0);
    test_apply(0xa5);
    test_apply(0xff);
    test_aliases();
    test_formats();
    printf("Software RT properties/resource aliases/canonical vertex formats %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
