/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_software_profile.h"
#include "bvh_ir_contract.h"
#include "typed_runtime_abi.h"

void vkrt_software_rt_profile_init(const struct vkrt_resource_profile *resources, struct vkrt_software_rt_profile *output) {
    if (!resources || !output) return;
    struct vkrt_software_rt_profile p = {0};
    p.resources = *resources;
    p.resources.indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    p.resources.inline_uniform.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES;
    p.resources.maintenance7.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR;
    p.resources.indexing.pNext = p.resources.inline_uniform.pNext = p.resources.maintenance7.pNext = NULL;
    p.as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    /* Geometry IDs are decoded through the runtime's 24-bit geometry mask.
     * Instance/primitive limits fit the shared indexed construction IR; the
     * scratch layout aligns every region to 256 bytes. These values also meet
     * the base comparisons in the pinned v1.4.357 required-limit table:
     * https://github.com/KhronosGroup/Vulkan-Docs/blob/v1.4.357/chapters/limits.adoc#L6746 */
    p.as.maxGeometryCount = UINT64_C(0xffffff);
    p.as.maxInstanceCount = UINT64_C(0xffffff);
    p.as.maxPrimitiveCount = BVH_IR_MAX_LEAVES;
    p.as.maxPerStageDescriptorAccelerationStructures = p.resources.as_per_stage;
    p.as.maxPerStageDescriptorUpdateAfterBindAccelerationStructures = p.resources.as_per_stage;
    p.as.maxDescriptorSetAccelerationStructures = p.resources.as_per_set;
    p.as.maxDescriptorSetUpdateAfterBindAccelerationStructures = p.resources.as_per_set;
    p.as.minAccelerationStructureScratchOffsetAlignment = 256;
    p.rtp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    p.rtp.shaderGroupHandleSize = VEL_RT_SBT_HANDLE_BYTES;
    /* The iterative BDA scheduler supports real nested Trace activations.
     * 31 is the explicit software profile limit, not a KHR specification maximum. */
    p.rtp.maxRayRecursionDepth = 31;
    p.rtp.maxShaderGroupStride = 4096;
    p.rtp.shaderGroupBaseAlignment = 64;
    /* Optional capture/replay is unsupported; this is not an ordinary handle. */
    p.rtp.shaderGroupHandleCaptureReplaySize = 0;
    p.rtp.maxRayDispatchInvocationCount = 1u << 30;
    p.rtp.shaderGroupHandleAlignment = 32;
    p.rtp.maxRayHitAttributeSize = 32;
    *output = p;
}

#define AS_PROPERTIES(F)                                          \
    F(maxGeometryCount)                                           \
    F(maxInstanceCount)                                           \
    F(maxPrimitiveCount)                                          \
    F(maxPerStageDescriptorAccelerationStructures)                \
    F(maxPerStageDescriptorUpdateAfterBindAccelerationStructures) \
    F(maxDescriptorSetAccelerationStructures)                     \
    F(maxDescriptorSetUpdateAfterBindAccelerationStructures)      \
    F(minAccelerationStructureScratchOffsetAlignment)
#define RTP_PROPERTIES(F)                 \
    F(shaderGroupHandleSize)              \
    F(maxRayRecursionDepth)               \
    F(maxShaderGroupStride)               \
    F(shaderGroupBaseAlignment)           \
    F(shaderGroupHandleCaptureReplaySize) \
    F(maxRayDispatchInvocationCount)      \
    F(shaderGroupHandleAlignment)         \
    F(maxRayHitAttributeSize)

void vkrt_apply_software_rt_profile(const struct vkrt_software_rt_profile *profile, VkPhysicalDeviceProperties2 *properties) {
    if (!profile || !properties) return;
    /* Applying resource limits may itself write into profile-owned nodes. */
    const struct vkrt_software_rt_profile p = *profile;
    vkrt_apply_resource_profile(&p.resources, properties);
    for (VkBaseOutStructure *node = properties->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR) {
            VkPhysicalDeviceAccelerationStructurePropertiesKHR *target = (void *)node;
#define COPY_AS(name) target->name = p.as.name;
            AS_PROPERTIES(COPY_AS)
#undef COPY_AS
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR) {
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR *target = (void *)node;
#define COPY_RTP(name) target->name = p.rtp.name;
            RTP_PROPERTIES(COPY_RTP)
#undef COPY_RTP
        }
    }
}
