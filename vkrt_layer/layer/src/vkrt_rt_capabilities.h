/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_RT_CAPABILITIES_H
#define VKRT_RT_CAPABILITIES_H
#include "vkrt_common.h"
#include <stddef.h>

/* The current prototype cannot mix native RT with layer-owned AS objects.
 * Keep rejection and discovery masking in one table. Generic pipeline-library
 * support is unrelated and must not be hidden with its RT-only dependent. */
static inline bool vkrt_incompatible_device_extension(const char *name) {
    return !strncmp(name, "VK_KHR_ray_tracing", strlen("VK_KHR_ray_tracing")) ||
           !strncmp(name, "VK_NV_ray_tracing", strlen("VK_NV_ray_tracing")) ||
           !strncmp(name, "VK_EXT_ray_tracing", strlen("VK_EXT_ray_tracing")) || !strcmp(name, "VK_EXT_opacity_micromap") ||
           !strcmp(name, "VK_KHR_opacity_micromap") || !strcmp(name, "VK_NV_displacement_micromap") ||
           !strcmp(name, "VK_ARM_pipeline_opacity_micromap") || !strcmp(name, "VK_EXT_pipeline_library_group_handles") ||
           !strcmp(name, "VK_HUAWEI_invocation_mask") || !strcmp(name, "VK_AMDX_dense_geometry_format") ||
           !strcmp(name, "VK_NV_cluster_acceleration_structure") || !strcmp(name, "VK_NV_partitioned_acceleration_structure") ||
           !strcmp(name, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
}

/* Ranges contain only consecutive VkBool32 members, excluding pNext/padding.
 * Base AS/RQ native truth is retained; every unsupported optional RT bit is
 * reported false and rejected if explicitly requested at device creation. */
static inline bool vkrt_unsupported_rt_feature_range(VkStructureType type, size_t *offset, size_t *bytes) {
#define RANGE(stype, ctype, first, last)                             \
    case stype:                                                      \
        *offset = offsetof(ctype, first);                            \
        *bytes = offsetof(ctype, last) + sizeof(VkBool32) - *offset; \
        return true
    switch (type) {
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
              VkPhysicalDeviceAccelerationStructureFeaturesKHR, accelerationStructureCaptureReplay,
              descriptorBindingAccelerationStructureUpdateAfterBind);
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, VkPhysicalDeviceRayTracingPipelineFeaturesKHR,
              rayTracingPipeline, rayTraversalPrimitiveCulling);
#ifdef VK_KHR_ray_tracing_maintenance1
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR,
              VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR, rayTracingMaintenance1, rayTracingPipelineTraceRaysIndirect2);
#endif
#ifdef VK_KHR_ray_tracing_position_fetch
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR,
              VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR, rayTracingPositionFetch, rayTracingPositionFetch);
#endif
#ifdef VK_NV_ray_tracing_motion_blur
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MOTION_BLUR_FEATURES_NV, VkPhysicalDeviceRayTracingMotionBlurFeaturesNV,
              rayTracingMotionBlur, rayTracingMotionBlurPipelineTraceRaysIndirect);
#endif
#ifdef VK_NV_ray_tracing_invocation_reorder
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV,
              VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV, rayTracingInvocationReorder, rayTracingInvocationReorder);
#endif
#ifdef VK_EXT_ray_tracing_invocation_reorder
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT,
              VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT, rayTracingInvocationReorder, rayTracingInvocationReorder);
#endif
#ifdef VK_EXT_opacity_micromap
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT, VkPhysicalDeviceOpacityMicromapFeaturesEXT, micromap,
              micromapHostCommands);
#endif
#ifdef VK_KHR_opacity_micromap
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_KHR, VkPhysicalDeviceOpacityMicromapFeaturesKHR, micromap,
              micromap);
#endif
#ifdef VK_NV_displacement_micromap
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DISPLACEMENT_MICROMAP_FEATURES_NV, VkPhysicalDeviceDisplacementMicromapFeaturesNV,
              displacementMicromap, displacementMicromap);
#endif
#ifdef VK_ARM_pipeline_opacity_micromap
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_OPACITY_MICROMAP_FEATURES_ARM,
              VkPhysicalDevicePipelineOpacityMicromapFeaturesARM, pipelineOpacityMicromap, pipelineOpacityMicromap);
#endif
#ifdef VK_EXT_pipeline_library_group_handles
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_LIBRARY_GROUP_HANDLES_FEATURES_EXT,
              VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT, pipelineLibraryGroupHandles, pipelineLibraryGroupHandles);
#endif
#ifdef VK_HUAWEI_invocation_mask
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INVOCATION_MASK_FEATURES_HUAWEI, VkPhysicalDeviceInvocationMaskFeaturesHUAWEI,
              invocationMask, invocationMask);
#endif
#ifdef VK_AMDX_dense_geometry_format
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DENSE_GEOMETRY_FORMAT_FEATURES_AMDX,
              VkPhysicalDeviceDenseGeometryFormatFeaturesAMDX, denseGeometryFormat, denseGeometryFormat);
#endif
#ifdef VK_NV_cluster_acceleration_structure
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV,
              VkPhysicalDeviceClusterAccelerationStructureFeaturesNV, clusterAccelerationStructure, clusterAccelerationStructure);
#endif
#ifdef VK_NV_partitioned_acceleration_structure
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PARTITIONED_ACCELERATION_STRUCTURE_FEATURES_NV,
              VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV, partitionedAccelerationStructure,
              partitionedAccelerationStructure);
#endif
        default:
            return false;
    }
#undef RANGE
}
#endif
