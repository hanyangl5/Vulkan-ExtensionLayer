/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Generated typed native RT rejection stubs. */

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkBindAccelerationStructureMemoryNV(
    VkDevice device, uint32_t bindInfoCount, const VkBindAccelerationStructureMemoryInfoNV* pBindInfos) {
    (void)device;
    (void)bindInfoCount;
    (void)pBindInfos;
    guard_reject("native_entrypoint", "vkBindAccelerationStructureMemoryNV");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL
guard_vkBuildAccelerationStructuresKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, uint32_t infoCount,
                                       const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
                                       const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos) {
    (void)device;
    (void)deferredOperation;
    (void)infoCount;
    (void)pInfos;
    (void)ppBuildRangeInfos;
    guard_reject("native_entrypoint", "vkBuildAccelerationStructuresKHR");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkBuildMicromapsEXT(VkDevice device, VkDeferredOperationKHR deferredOperation,
                                                                uint32_t infoCount, const VkMicromapBuildInfoEXT* pInfos) {
    (void)device;
    (void)deferredOperation;
    (void)infoCount;
    (void)pInfos;
    guard_reject("native_entrypoint", "vkBuildMicromapsEXT");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdBuildAccelerationStructureNV(
    VkCommandBuffer commandBuffer, const VkAccelerationStructureInfoNV* pInfo, VkBuffer instanceData, VkDeviceSize instanceOffset,
    VkBool32 update, VkAccelerationStructureNV dst, VkAccelerationStructureNV src, VkBuffer scratch, VkDeviceSize scratchOffset) {
    (void)commandBuffer;
    (void)pInfo;
    (void)instanceData;
    (void)instanceOffset;
    (void)update;
    (void)dst;
    (void)src;
    (void)scratch;
    (void)scratchOffset;
    guard_reject("native_entrypoint", "vkCmdBuildAccelerationStructureNV");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdBuildAccelerationStructuresIndirectKHR(
    VkCommandBuffer commandBuffer, uint32_t infoCount, const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
    const VkDeviceAddress* pIndirectDeviceAddresses, const uint32_t* pIndirectStrides,
    const uint32_t* const* ppMaxPrimitiveCounts) {
    (void)commandBuffer;
    (void)infoCount;
    (void)pInfos;
    (void)pIndirectDeviceAddresses;
    (void)pIndirectStrides;
    (void)ppMaxPrimitiveCounts;
    guard_reject("native_entrypoint", "vkCmdBuildAccelerationStructuresIndirectKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdBuildAccelerationStructuresKHR(
    VkCommandBuffer commandBuffer, uint32_t infoCount, const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
    const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos) {
    (void)commandBuffer;
    (void)infoCount;
    (void)pInfos;
    (void)ppBuildRangeInfos;
    guard_reject("native_entrypoint", "vkCmdBuildAccelerationStructuresKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdBuildClusterAccelerationStructureIndirectNV(
    VkCommandBuffer commandBuffer, const VkClusterAccelerationStructureCommandsInfoNV* pCommandInfos) {
    (void)commandBuffer;
    (void)pCommandInfos;
    guard_reject("native_entrypoint", "vkCmdBuildClusterAccelerationStructureIndirectNV");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdBuildMicromapsEXT(VkCommandBuffer commandBuffer, uint32_t infoCount,
                                                               const VkMicromapBuildInfoEXT* pInfos) {
    (void)commandBuffer;
    (void)infoCount;
    (void)pInfos;
    guard_reject("native_entrypoint", "vkCmdBuildMicromapsEXT");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdBuildPartitionedAccelerationStructuresNV(
    VkCommandBuffer commandBuffer, const VkBuildPartitionedAccelerationStructureInfoNV* pBuildInfo) {
    (void)commandBuffer;
    (void)pBuildInfo;
    guard_reject("native_entrypoint", "vkCmdBuildPartitionedAccelerationStructuresNV");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdCopyAccelerationStructureKHR(VkCommandBuffer commandBuffer,
                                                                          const VkCopyAccelerationStructureInfoKHR* pInfo) {
    (void)commandBuffer;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCmdCopyAccelerationStructureKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdCopyAccelerationStructureNV(VkCommandBuffer commandBuffer,
                                                                         VkAccelerationStructureNV dst,
                                                                         VkAccelerationStructureNV src,
                                                                         VkCopyAccelerationStructureModeKHR mode) {
    (void)commandBuffer;
    (void)dst;
    (void)src;
    (void)mode;
    guard_reject("native_entrypoint", "vkCmdCopyAccelerationStructureNV");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdCopyAccelerationStructureToMemoryKHR(
    VkCommandBuffer commandBuffer, const VkCopyAccelerationStructureToMemoryInfoKHR* pInfo) {
    (void)commandBuffer;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCmdCopyAccelerationStructureToMemoryKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdCopyMemoryToAccelerationStructureKHR(
    VkCommandBuffer commandBuffer, const VkCopyMemoryToAccelerationStructureInfoKHR* pInfo) {
    (void)commandBuffer;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCmdCopyMemoryToAccelerationStructureKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdCopyMemoryToMicromapEXT(VkCommandBuffer commandBuffer,
                                                                     const VkCopyMemoryToMicromapInfoEXT* pInfo) {
    (void)commandBuffer;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCmdCopyMemoryToMicromapEXT");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdCopyMicromapEXT(VkCommandBuffer commandBuffer, const VkCopyMicromapInfoEXT* pInfo) {
    (void)commandBuffer;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCmdCopyMicromapEXT");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdCopyMicromapToMemoryEXT(VkCommandBuffer commandBuffer,
                                                                     const VkCopyMicromapToMemoryInfoEXT* pInfo) {
    (void)commandBuffer;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCmdCopyMicromapToMemoryEXT");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdSetRayTracingPipelineStackSizeKHR(VkCommandBuffer commandBuffer,
                                                                               uint32_t pipelineStackSize) {
    (void)commandBuffer;
    (void)pipelineStackSize;
    guard_reject("native_entrypoint", "vkCmdSetRayTracingPipelineStackSizeKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdTraceRaysIndirect2KHR(VkCommandBuffer commandBuffer,
                                                                   VkDeviceAddress indirectDeviceAddress) {
    (void)commandBuffer;
    (void)indirectDeviceAddress;
    guard_reject("native_entrypoint", "vkCmdTraceRaysIndirect2KHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdTraceRaysIndirectKHR(
    VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable, VkDeviceAddress indirectDeviceAddress) {
    (void)commandBuffer;
    (void)pRaygenShaderBindingTable;
    (void)pMissShaderBindingTable;
    (void)pHitShaderBindingTable;
    (void)pCallableShaderBindingTable;
    (void)indirectDeviceAddress;
    guard_reject("native_entrypoint", "vkCmdTraceRaysIndirectKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdTraceRaysKHR(VkCommandBuffer commandBuffer,
                                                          const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
                                                          const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable,
                                                          const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
                                                          const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable,
                                                          uint32_t width, uint32_t height, uint32_t depth) {
    (void)commandBuffer;
    (void)pRaygenShaderBindingTable;
    (void)pMissShaderBindingTable;
    (void)pHitShaderBindingTable;
    (void)pCallableShaderBindingTable;
    (void)width;
    (void)height;
    (void)depth;
    guard_reject("native_entrypoint", "vkCmdTraceRaysKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdTraceRaysNV(
    VkCommandBuffer commandBuffer, VkBuffer raygenShaderBindingTableBuffer, VkDeviceSize raygenShaderBindingOffset,
    VkBuffer missShaderBindingTableBuffer, VkDeviceSize missShaderBindingOffset, VkDeviceSize missShaderBindingStride,
    VkBuffer hitShaderBindingTableBuffer, VkDeviceSize hitShaderBindingOffset, VkDeviceSize hitShaderBindingStride,
    VkBuffer callableShaderBindingTableBuffer, VkDeviceSize callableShaderBindingOffset, VkDeviceSize callableShaderBindingStride,
    uint32_t width, uint32_t height, uint32_t depth) {
    (void)commandBuffer;
    (void)raygenShaderBindingTableBuffer;
    (void)raygenShaderBindingOffset;
    (void)missShaderBindingTableBuffer;
    (void)missShaderBindingOffset;
    (void)missShaderBindingStride;
    (void)hitShaderBindingTableBuffer;
    (void)hitShaderBindingOffset;
    (void)hitShaderBindingStride;
    (void)callableShaderBindingTableBuffer;
    (void)callableShaderBindingOffset;
    (void)callableShaderBindingStride;
    (void)width;
    (void)height;
    (void)depth;
    guard_reject("native_entrypoint", "vkCmdTraceRaysNV");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdWriteAccelerationStructuresPropertiesKHR(
    VkCommandBuffer commandBuffer, uint32_t accelerationStructureCount, const VkAccelerationStructureKHR* pAccelerationStructures,
    VkQueryType queryType, VkQueryPool queryPool, uint32_t firstQuery) {
    (void)commandBuffer;
    (void)accelerationStructureCount;
    (void)pAccelerationStructures;
    (void)queryType;
    (void)queryPool;
    (void)firstQuery;
    guard_reject("native_entrypoint", "vkCmdWriteAccelerationStructuresPropertiesKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdWriteAccelerationStructuresPropertiesNV(
    VkCommandBuffer commandBuffer, uint32_t accelerationStructureCount, const VkAccelerationStructureNV* pAccelerationStructures,
    VkQueryType queryType, VkQueryPool queryPool, uint32_t firstQuery) {
    (void)commandBuffer;
    (void)accelerationStructureCount;
    (void)pAccelerationStructures;
    (void)queryType;
    (void)queryPool;
    (void)firstQuery;
    guard_reject("native_entrypoint", "vkCmdWriteAccelerationStructuresPropertiesNV");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkCmdWriteMicromapsPropertiesEXT(VkCommandBuffer commandBuffer, uint32_t micromapCount,
                                                                         const VkMicromapEXT* pMicromaps, VkQueryType queryType,
                                                                         VkQueryPool queryPool, uint32_t firstQuery) {
    (void)commandBuffer;
    (void)micromapCount;
    (void)pMicromaps;
    (void)queryType;
    (void)queryPool;
    (void)firstQuery;
    guard_reject("native_entrypoint", "vkCmdWriteMicromapsPropertiesEXT");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCompileDeferredNV(VkDevice device, VkPipeline pipeline, uint32_t shader) {
    (void)device;
    (void)pipeline;
    (void)shader;
    guard_reject("native_entrypoint", "vkCompileDeferredNV");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCopyAccelerationStructureKHR(VkDevice device,
                                                                           VkDeferredOperationKHR deferredOperation,
                                                                           const VkCopyAccelerationStructureInfoKHR* pInfo) {
    (void)device;
    (void)deferredOperation;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCopyAccelerationStructureKHR");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCopyAccelerationStructureToMemoryKHR(
    VkDevice device, VkDeferredOperationKHR deferredOperation, const VkCopyAccelerationStructureToMemoryInfoKHR* pInfo) {
    (void)device;
    (void)deferredOperation;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCopyAccelerationStructureToMemoryKHR");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCopyMemoryToAccelerationStructureKHR(
    VkDevice device, VkDeferredOperationKHR deferredOperation, const VkCopyMemoryToAccelerationStructureInfoKHR* pInfo) {
    (void)device;
    (void)deferredOperation;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCopyMemoryToAccelerationStructureKHR");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCopyMemoryToMicromapEXT(VkDevice device, VkDeferredOperationKHR deferredOperation,
                                                                      const VkCopyMemoryToMicromapInfoEXT* pInfo) {
    (void)device;
    (void)deferredOperation;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCopyMemoryToMicromapEXT");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCopyMicromapEXT(VkDevice device, VkDeferredOperationKHR deferredOperation,
                                                              const VkCopyMicromapInfoEXT* pInfo) {
    (void)device;
    (void)deferredOperation;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCopyMicromapEXT");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCopyMicromapToMemoryEXT(VkDevice device, VkDeferredOperationKHR deferredOperation,
                                                                      const VkCopyMicromapToMemoryInfoEXT* pInfo) {
    (void)device;
    (void)deferredOperation;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkCopyMicromapToMemoryEXT");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCreateAccelerationStructureKHR(
    VkDevice device, const VkAccelerationStructureCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkAccelerationStructureKHR* pAccelerationStructure) {
    (void)device;
    (void)pCreateInfo;
    (void)pAllocator;
    (void)pAccelerationStructure;
    guard_reject("native_entrypoint", "vkCreateAccelerationStructureKHR");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCreateAccelerationStructureNV(VkDevice device,
                                                                            const VkAccelerationStructureCreateInfoNV* pCreateInfo,
                                                                            const VkAllocationCallbacks* pAllocator,
                                                                            VkAccelerationStructureNV* pAccelerationStructure) {
    (void)device;
    (void)pCreateInfo;
    (void)pAllocator;
    (void)pAccelerationStructure;
    guard_reject("native_entrypoint", "vkCreateAccelerationStructureNV");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCreateMicromapEXT(VkDevice device, const VkMicromapCreateInfoEXT* pCreateInfo,
                                                                const VkAllocationCallbacks* pAllocator, VkMicromapEXT* pMicromap) {
    (void)device;
    (void)pCreateInfo;
    (void)pAllocator;
    (void)pMicromap;
    guard_reject("native_entrypoint", "vkCreateMicromapEXT");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCreateRayTracingPipelinesKHR(
    VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    (void)device;
    (void)deferredOperation;
    (void)pipelineCache;
    (void)createInfoCount;
    (void)pCreateInfos;
    (void)pAllocator;
    (void)pPipelines;
    guard_reject("native_entrypoint", "vkCreateRayTracingPipelinesKHR");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkCreateRayTracingPipelinesNV(VkDevice device, VkPipelineCache pipelineCache,
                                                                          uint32_t createInfoCount,
                                                                          const VkRayTracingPipelineCreateInfoNV* pCreateInfos,
                                                                          const VkAllocationCallbacks* pAllocator,
                                                                          VkPipeline* pPipelines) {
    (void)device;
    (void)pipelineCache;
    (void)createInfoCount;
    (void)pCreateInfos;
    (void)pAllocator;
    (void)pPipelines;
    guard_reject("native_entrypoint", "vkCreateRayTracingPipelinesNV");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR void VKAPI_CALL guard_vkDestroyAccelerationStructureKHR(VkDevice device,
                                                                          VkAccelerationStructureKHR accelerationStructure,
                                                                          const VkAllocationCallbacks* pAllocator) {
    (void)device;
    (void)accelerationStructure;
    (void)pAllocator;
    guard_reject("native_entrypoint", "vkDestroyAccelerationStructureKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkDestroyAccelerationStructureNV(VkDevice device,
                                                                         VkAccelerationStructureNV accelerationStructure,
                                                                         const VkAllocationCallbacks* pAllocator) {
    (void)device;
    (void)accelerationStructure;
    (void)pAllocator;
    guard_reject("native_entrypoint", "vkDestroyAccelerationStructureNV");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkDestroyMicromapEXT(VkDevice device, VkMicromapEXT micromap,
                                                             const VkAllocationCallbacks* pAllocator) {
    (void)device;
    (void)micromap;
    (void)pAllocator;
    guard_reject("native_entrypoint", "vkDestroyMicromapEXT");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkGetAccelerationStructureBuildSizesKHR(
    VkDevice device, VkAccelerationStructureBuildTypeKHR buildType, const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo,
    const uint32_t* pMaxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo) {
    (void)device;
    (void)buildType;
    (void)pBuildInfo;
    (void)pMaxPrimitiveCounts;
    (void)pSizeInfo;
    guard_reject("native_entrypoint", "vkGetAccelerationStructureBuildSizesKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR VkDeviceAddress VKAPI_CALL
guard_vkGetAccelerationStructureDeviceAddressKHR(VkDevice device, const VkAccelerationStructureDeviceAddressInfoKHR* pInfo) {
    (void)device;
    (void)pInfo;
    guard_reject("native_entrypoint", "vkGetAccelerationStructureDeviceAddressKHR");
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkGetAccelerationStructureHandleNV(VkDevice device,
                                                                               VkAccelerationStructureNV accelerationStructure,
                                                                               size_t dataSize, void* pData) {
    (void)device;
    (void)accelerationStructure;
    (void)dataSize;
    (void)pData;
    guard_reject("native_entrypoint", "vkGetAccelerationStructureHandleNV");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR void VKAPI_CALL guard_vkGetAccelerationStructureMemoryRequirementsNV(
    VkDevice device, const VkAccelerationStructureMemoryRequirementsInfoNV* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
    (void)device;
    (void)pInfo;
    (void)pMemoryRequirements;
    guard_reject("native_entrypoint", "vkGetAccelerationStructureMemoryRequirementsNV");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkGetClusterAccelerationStructureBuildSizesNV(
    VkDevice device, const VkClusterAccelerationStructureInputInfoNV* pInfo, VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo) {
    (void)device;
    (void)pInfo;
    (void)pSizeInfo;
    guard_reject("native_entrypoint", "vkGetClusterAccelerationStructureBuildSizesNV");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL
guard_vkGetDeviceAccelerationStructureCompatibilityKHR(VkDevice device, const VkAccelerationStructureVersionInfoKHR* pVersionInfo,
                                                       VkAccelerationStructureCompatibilityKHR* pCompatibility) {
    (void)device;
    (void)pVersionInfo;
    (void)pCompatibility;
    guard_reject("native_entrypoint", "vkGetDeviceAccelerationStructureCompatibilityKHR");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkGetDeviceMicromapCompatibilityEXT(
    VkDevice device, const VkMicromapVersionInfoEXT* pVersionInfo, VkAccelerationStructureCompatibilityKHR* pCompatibility) {
    (void)device;
    (void)pVersionInfo;
    (void)pCompatibility;
    guard_reject("native_entrypoint", "vkGetDeviceMicromapCompatibilityEXT");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkGetMicromapBuildSizesEXT(VkDevice device, VkAccelerationStructureBuildTypeKHR buildType,
                                                                   const VkMicromapBuildInfoEXT* pBuildInfo,
                                                                   VkMicromapBuildSizesInfoEXT* pSizeInfo) {
    (void)device;
    (void)buildType;
    (void)pBuildInfo;
    (void)pSizeInfo;
    guard_reject("native_entrypoint", "vkGetMicromapBuildSizesEXT");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR void VKAPI_CALL guard_vkGetPartitionedAccelerationStructuresBuildSizesNV(
    VkDevice device, const VkPartitionedAccelerationStructureInstancesInputNV* pInfo,
    VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo) {
    (void)device;
    (void)pInfo;
    (void)pSizeInfo;
    guard_reject("native_entrypoint", "vkGetPartitionedAccelerationStructuresBuildSizesNV");
    _Exit(86); /* A forbidden void call cannot report VkResult. */
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR(VkDevice device, VkPipeline pipeline,
                                                                                              uint32_t firstGroup,
                                                                                              uint32_t groupCount, size_t dataSize,
                                                                                              void* pData) {
    (void)device;
    (void)pipeline;
    (void)firstGroup;
    (void)groupCount;
    (void)dataSize;
    (void)pData;
    guard_reject("native_entrypoint", "vkGetRayTracingCaptureReplayShaderGroupHandlesKHR");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkGetRayTracingShaderGroupHandlesKHR(VkDevice device, VkPipeline pipeline,
                                                                                 uint32_t firstGroup, uint32_t groupCount,
                                                                                 size_t dataSize, void* pData) {
    (void)device;
    (void)pipeline;
    (void)firstGroup;
    (void)groupCount;
    (void)dataSize;
    (void)pData;
    guard_reject("native_entrypoint", "vkGetRayTracingShaderGroupHandlesKHR");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkGetRayTracingShaderGroupHandlesNV(VkDevice device, VkPipeline pipeline,
                                                                                uint32_t firstGroup, uint32_t groupCount,
                                                                                size_t dataSize, void* pData) {
    (void)device;
    (void)pipeline;
    (void)firstGroup;
    (void)groupCount;
    (void)dataSize;
    (void)pData;
    guard_reject("native_entrypoint", "vkGetRayTracingShaderGroupHandlesNV");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkDeviceSize VKAPI_CALL guard_vkGetRayTracingShaderGroupStackSizeKHR(VkDevice device, VkPipeline pipeline,
                                                                                       uint32_t group,
                                                                                       VkShaderGroupShaderKHR groupShader) {
    (void)device;
    (void)pipeline;
    (void)group;
    (void)groupShader;
    guard_reject("native_entrypoint", "vkGetRayTracingShaderGroupStackSizeKHR");
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkWriteAccelerationStructuresPropertiesKHR(
    VkDevice device, uint32_t accelerationStructureCount, const VkAccelerationStructureKHR* pAccelerationStructures,
    VkQueryType queryType, size_t dataSize, void* pData, size_t stride) {
    (void)device;
    (void)accelerationStructureCount;
    (void)pAccelerationStructures;
    (void)queryType;
    (void)dataSize;
    (void)pData;
    (void)stride;
    guard_reject("native_entrypoint", "vkWriteAccelerationStructuresPropertiesKHR");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_vkWriteMicromapsPropertiesEXT(VkDevice device, uint32_t micromapCount,
                                                                          const VkMicromapEXT* pMicromaps, VkQueryType queryType,
                                                                          size_t dataSize, void* pData, size_t stride) {
    (void)device;
    (void)micromapCount;
    (void)pMicromaps;
    (void)queryType;
    (void)dataSize;
    (void)pData;
    (void)stride;
    guard_reject("native_entrypoint", "vkWriteMicromapsPropertiesEXT");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static PFN_vkVoidFunction guard_native_proc(const char* name) {
    if (!strcmp(name, "vkBindAccelerationStructureMemoryNV")) return (PFN_vkVoidFunction)guard_vkBindAccelerationStructureMemoryNV;
    if (!strcmp(name, "vkBuildAccelerationStructuresKHR")) return (PFN_vkVoidFunction)guard_vkBuildAccelerationStructuresKHR;
    if (!strcmp(name, "vkBuildMicromapsEXT")) return (PFN_vkVoidFunction)guard_vkBuildMicromapsEXT;
    if (!strcmp(name, "vkCmdBuildAccelerationStructureNV")) return (PFN_vkVoidFunction)guard_vkCmdBuildAccelerationStructureNV;
    if (!strcmp(name, "vkCmdBuildAccelerationStructuresIndirectKHR"))
        return (PFN_vkVoidFunction)guard_vkCmdBuildAccelerationStructuresIndirectKHR;
    if (!strcmp(name, "vkCmdBuildAccelerationStructuresKHR")) return (PFN_vkVoidFunction)guard_vkCmdBuildAccelerationStructuresKHR;
    if (!strcmp(name, "vkCmdBuildClusterAccelerationStructureIndirectNV"))
        return (PFN_vkVoidFunction)guard_vkCmdBuildClusterAccelerationStructureIndirectNV;
    if (!strcmp(name, "vkCmdBuildMicromapsEXT")) return (PFN_vkVoidFunction)guard_vkCmdBuildMicromapsEXT;
    if (!strcmp(name, "vkCmdBuildPartitionedAccelerationStructuresNV"))
        return (PFN_vkVoidFunction)guard_vkCmdBuildPartitionedAccelerationStructuresNV;
    if (!strcmp(name, "vkCmdCopyAccelerationStructureKHR")) return (PFN_vkVoidFunction)guard_vkCmdCopyAccelerationStructureKHR;
    if (!strcmp(name, "vkCmdCopyAccelerationStructureNV")) return (PFN_vkVoidFunction)guard_vkCmdCopyAccelerationStructureNV;
    if (!strcmp(name, "vkCmdCopyAccelerationStructureToMemoryKHR"))
        return (PFN_vkVoidFunction)guard_vkCmdCopyAccelerationStructureToMemoryKHR;
    if (!strcmp(name, "vkCmdCopyMemoryToAccelerationStructureKHR"))
        return (PFN_vkVoidFunction)guard_vkCmdCopyMemoryToAccelerationStructureKHR;
    if (!strcmp(name, "vkCmdCopyMemoryToMicromapEXT")) return (PFN_vkVoidFunction)guard_vkCmdCopyMemoryToMicromapEXT;
    if (!strcmp(name, "vkCmdCopyMicromapEXT")) return (PFN_vkVoidFunction)guard_vkCmdCopyMicromapEXT;
    if (!strcmp(name, "vkCmdCopyMicromapToMemoryEXT")) return (PFN_vkVoidFunction)guard_vkCmdCopyMicromapToMemoryEXT;
    if (!strcmp(name, "vkCmdSetRayTracingPipelineStackSizeKHR"))
        return (PFN_vkVoidFunction)guard_vkCmdSetRayTracingPipelineStackSizeKHR;
    if (!strcmp(name, "vkCmdTraceRaysIndirect2KHR")) return (PFN_vkVoidFunction)guard_vkCmdTraceRaysIndirect2KHR;
    if (!strcmp(name, "vkCmdTraceRaysIndirectKHR")) return (PFN_vkVoidFunction)guard_vkCmdTraceRaysIndirectKHR;
    if (!strcmp(name, "vkCmdTraceRaysKHR")) return (PFN_vkVoidFunction)guard_vkCmdTraceRaysKHR;
    if (!strcmp(name, "vkCmdTraceRaysNV")) return (PFN_vkVoidFunction)guard_vkCmdTraceRaysNV;
    if (!strcmp(name, "vkCmdWriteAccelerationStructuresPropertiesKHR"))
        return (PFN_vkVoidFunction)guard_vkCmdWriteAccelerationStructuresPropertiesKHR;
    if (!strcmp(name, "vkCmdWriteAccelerationStructuresPropertiesNV"))
        return (PFN_vkVoidFunction)guard_vkCmdWriteAccelerationStructuresPropertiesNV;
    if (!strcmp(name, "vkCmdWriteMicromapsPropertiesEXT")) return (PFN_vkVoidFunction)guard_vkCmdWriteMicromapsPropertiesEXT;
    if (!strcmp(name, "vkCompileDeferredNV")) return (PFN_vkVoidFunction)guard_vkCompileDeferredNV;
    if (!strcmp(name, "vkCopyAccelerationStructureKHR")) return (PFN_vkVoidFunction)guard_vkCopyAccelerationStructureKHR;
    if (!strcmp(name, "vkCopyAccelerationStructureToMemoryKHR"))
        return (PFN_vkVoidFunction)guard_vkCopyAccelerationStructureToMemoryKHR;
    if (!strcmp(name, "vkCopyMemoryToAccelerationStructureKHR"))
        return (PFN_vkVoidFunction)guard_vkCopyMemoryToAccelerationStructureKHR;
    if (!strcmp(name, "vkCopyMemoryToMicromapEXT")) return (PFN_vkVoidFunction)guard_vkCopyMemoryToMicromapEXT;
    if (!strcmp(name, "vkCopyMicromapEXT")) return (PFN_vkVoidFunction)guard_vkCopyMicromapEXT;
    if (!strcmp(name, "vkCopyMicromapToMemoryEXT")) return (PFN_vkVoidFunction)guard_vkCopyMicromapToMemoryEXT;
    if (!strcmp(name, "vkCreateAccelerationStructureKHR")) return (PFN_vkVoidFunction)guard_vkCreateAccelerationStructureKHR;
    if (!strcmp(name, "vkCreateAccelerationStructureNV")) return (PFN_vkVoidFunction)guard_vkCreateAccelerationStructureNV;
    if (!strcmp(name, "vkCreateMicromapEXT")) return (PFN_vkVoidFunction)guard_vkCreateMicromapEXT;
    if (!strcmp(name, "vkCreateRayTracingPipelinesKHR")) return (PFN_vkVoidFunction)guard_vkCreateRayTracingPipelinesKHR;
    if (!strcmp(name, "vkCreateRayTracingPipelinesNV")) return (PFN_vkVoidFunction)guard_vkCreateRayTracingPipelinesNV;
    if (!strcmp(name, "vkDestroyAccelerationStructureKHR")) return (PFN_vkVoidFunction)guard_vkDestroyAccelerationStructureKHR;
    if (!strcmp(name, "vkDestroyAccelerationStructureNV")) return (PFN_vkVoidFunction)guard_vkDestroyAccelerationStructureNV;
    if (!strcmp(name, "vkDestroyMicromapEXT")) return (PFN_vkVoidFunction)guard_vkDestroyMicromapEXT;
    if (!strcmp(name, "vkGetAccelerationStructureBuildSizesKHR"))
        return (PFN_vkVoidFunction)guard_vkGetAccelerationStructureBuildSizesKHR;
    if (!strcmp(name, "vkGetAccelerationStructureDeviceAddressKHR"))
        return (PFN_vkVoidFunction)guard_vkGetAccelerationStructureDeviceAddressKHR;
    if (!strcmp(name, "vkGetAccelerationStructureHandleNV")) return (PFN_vkVoidFunction)guard_vkGetAccelerationStructureHandleNV;
    if (!strcmp(name, "vkGetAccelerationStructureMemoryRequirementsNV"))
        return (PFN_vkVoidFunction)guard_vkGetAccelerationStructureMemoryRequirementsNV;
    if (!strcmp(name, "vkGetClusterAccelerationStructureBuildSizesNV"))
        return (PFN_vkVoidFunction)guard_vkGetClusterAccelerationStructureBuildSizesNV;
    if (!strcmp(name, "vkGetDeviceAccelerationStructureCompatibilityKHR"))
        return (PFN_vkVoidFunction)guard_vkGetDeviceAccelerationStructureCompatibilityKHR;
    if (!strcmp(name, "vkGetDeviceMicromapCompatibilityEXT")) return (PFN_vkVoidFunction)guard_vkGetDeviceMicromapCompatibilityEXT;
    if (!strcmp(name, "vkGetMicromapBuildSizesEXT")) return (PFN_vkVoidFunction)guard_vkGetMicromapBuildSizesEXT;
    if (!strcmp(name, "vkGetPartitionedAccelerationStructuresBuildSizesNV"))
        return (PFN_vkVoidFunction)guard_vkGetPartitionedAccelerationStructuresBuildSizesNV;
    if (!strcmp(name, "vkGetRayTracingCaptureReplayShaderGroupHandlesKHR"))
        return (PFN_vkVoidFunction)guard_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR;
    if (!strcmp(name, "vkGetRayTracingShaderGroupHandlesKHR"))
        return (PFN_vkVoidFunction)guard_vkGetRayTracingShaderGroupHandlesKHR;
    if (!strcmp(name, "vkGetRayTracingShaderGroupHandlesNV")) return (PFN_vkVoidFunction)guard_vkGetRayTracingShaderGroupHandlesNV;
    if (!strcmp(name, "vkGetRayTracingShaderGroupStackSizeKHR"))
        return (PFN_vkVoidFunction)guard_vkGetRayTracingShaderGroupStackSizeKHR;
    if (!strcmp(name, "vkWriteAccelerationStructuresPropertiesKHR"))
        return (PFN_vkVoidFunction)guard_vkWriteAccelerationStructuresPropertiesKHR;
    if (!strcmp(name, "vkWriteMicromapsPropertiesEXT")) return (PFN_vkVoidFunction)guard_vkWriteMicromapsPropertiesEXT;
    return NULL;
}
