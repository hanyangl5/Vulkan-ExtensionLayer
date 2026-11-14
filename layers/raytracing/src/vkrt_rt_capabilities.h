/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_RT_CAPABILITIES_H
#define VKRT_RT_CAPABILITIES_H
#include "vkrt_common.h"
#include <stddef.h>
#include <vulkan/vulkan_beta.h>

#define VKRT_RTP_SHADER_STAGES                                                                             \
    (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | \
     VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR)
static inline VkShaderStageFlags vkrt_compute_shader_stages(VkShaderStageFlags stages) {
    if (stages == VK_SHADER_STAGE_ALL) return stages;
    return (stages & ~VKRT_RTP_SHADER_STAGES) | ((stages & VKRT_RTP_SHADER_STAGES) ? VK_SHADER_STAGE_COMPUTE_BIT : 0);
}

/* The current prototype cannot mix native RT with layer-owned AS objects.
 * Keep rejection and discovery masking in one table. Generic pipeline-library
 * support is unrelated and must not be hidden with its RT-only dependent. */
static inline bool vkrt_incompatible_device_extension(const char *name) {
    if (!strcmp(name, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) return false;
    return !strncmp(name, "VK_KHR_ray_tracing", strlen("VK_KHR_ray_tracing")) ||
           !strncmp(name, "VK_NV_ray_tracing", strlen("VK_NV_ray_tracing")) ||
           !strncmp(name, "VK_EXT_ray_tracing", strlen("VK_EXT_ray_tracing")) || !strcmp(name, "VK_EXT_opacity_micromap") ||
           !strcmp(name, "VK_KHR_opacity_micromap") || !strcmp(name, "VK_NV_displacement_micromap") ||
           !strcmp(name, "VK_ARM_pipeline_opacity_micromap") || !strcmp(name, "VK_EXT_pipeline_library_group_handles") ||
           !strcmp(name, "VK_HUAWEI_invocation_mask") || !strcmp(name, "VK_AMDX_dense_geometry_format") ||
           !strcmp(name, "VK_NV_cluster_acceleration_structure") || !strcmp(name, "VK_NV_partitioned_acceleration_structure");
}

/* The legacy EXT address ABI cannot mix with the private core/KHR BDA
 * backend. It remains a legal native-only device extension. Pipeline binary
 * key queries likewise need a name gate because their feature may be false. */
/* The software RT backend owns AS objects, but VK_KHR_device_address_commands
 * creates and consumes AS objects through device-address commands that this
 * layer does not virtualize. Hide it from a software RT profile: a native
 * driver advertising it would otherwise pass CTS selection and then create
 * driver-owned AS objects behind the layer's back. Ordinary non-RT devices
 * keep native support. */
static inline bool vkrt_device_address_commands_extension(const char *name) {
#ifdef VK_KHR_device_address_commands
    return !strcmp(name, VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME);
#else
    return !strcmp(name, "VK_KHR_device_address_commands");
#endif
}

static inline bool vkrt_virtual_rt_incompatible_extension(const char *name) {
    return !strcmp(name, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) || !strcmp(name, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) ||
           vkrt_device_address_commands_extension(name);
}

/* These optional interfaces consume shader/pipeline objects that the software
 * RT backend owns. Hide them from its candidate profile without changing
 * ordinary device admission. Enabling a name alone is harmless except for
 * pipeline_binary: GetPipelineKey can consume modules without pipelineBinaries. */
static inline bool vkrt_virtual_object_extension(const char *name) {
    return !strcmp(name, VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME) ||
           !strcmp(name, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) ||
           !strcmp(name, VK_EXT_PIPELINE_PROPERTIES_EXTENSION_NAME) || !strcmp(name, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) ||
           !strcmp(name, VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME) ||
           !strcmp(name, VK_NV_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME) ||
           !strcmp(name, VK_NV_DEVICE_GENERATED_COMMANDS_COMPUTE_EXTENSION_NAME) ||
           !strcmp(name, VK_ARM_DATA_GRAPH_EXTENSION_NAME) || !strcmp(name, VK_AMDX_SHADER_ENQUEUE_EXTENSION_NAME);
}

/* SDK357 has no promoted/core aliases for these structures. Do not mask their
 * unrelated dependencies such as pipelineCreationCacheControl or maintenance5. */
static inline bool vkrt_virtual_object_feature_range(VkStructureType type, size_t *offset, size_t *bytes) {
#define RANGE(stype, ctype, first, last)                             \
    case stype:                                                      \
        *offset = offsetof(ctype, first);                            \
        *bytes = offsetof(ctype, last) + sizeof(VkBool32) - *offset; \
        return true
    switch ((uint32_t)type) {
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT,
              VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT, shaderModuleIdentifier, shaderModuleIdentifier);
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
              VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR, pipelineExecutableInfo, pipelineExecutableInfo);
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROPERTIES_FEATURES_EXT, VkPhysicalDevicePipelinePropertiesFeaturesEXT,
              pipelinePropertiesIdentifier, pipelinePropertiesIdentifier);
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, VkPhysicalDevicePipelineBinaryFeaturesKHR,
              pipelineBinaries, pipelineBinaries);
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT,
              VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT, deviceGeneratedCommands, dynamicGeneratedPipelineLayout);
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV,
              VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV, deviceGeneratedCommands, deviceGeneratedCommands);
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV,
              VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV, deviceGeneratedCompute,
              deviceGeneratedComputeCaptureReplay);
        /* Graph pipeline creation accepts a ShaderModule in its pNext chain.
         * It has no adapter for the public software module/layout namespace. */
        RANGE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_FEATURES_ARM, VkPhysicalDeviceDataGraphFeaturesARM, dataGraph,
              dataGraphShaderModule);
        /* The SDK beta typedef is available without globally enabling beta
         * enums. Its protected sType is from registry extension135, offset0. */
        RANGE((VkStructureType)1000134000, VkPhysicalDeviceShaderEnqueueFeaturesAMDX, shaderEnqueue, shaderMeshEnqueue);
        default:
            return false;
    }
#undef RANGE
}

/* Ranges contain only consecutive VkBool32 members, excluding pNext/padding.
 * Software profile selection supplies base AS/RQ/RTP truth; every unsupported
 * optional RT bit is false and rejected if requested at device creation. */
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
              rayTracingPipelineShaderGroupHandleCaptureReplay, rayTraversalPrimitiveCulling);
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
#if defined(VK_ENABLE_BETA_EXTENSIONS) && defined(VK_NV_displacement_micromap)
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
#if defined(VK_ENABLE_BETA_EXTENSIONS) && defined(VK_AMDX_dense_geometry_format)
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
