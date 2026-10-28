/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_layer.h"

/* Enabled base extensions still have entry points for their disabled optional
 * features. Keep those symbols owned by the layer on an ICD without native RT;
 * never let a virtual object reach a downstream optional host/indirect command.
 * Calling them with the corresponding feature false is invalid application use.
 */
#define HOST_OPTIONAL(name, parameters, arguments)                                        \
    static VKAPI_ATTR VkResult VKAPI_CALL optional_##name parameters {                    \
        struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);        \
        if (!dev) return VK_ERROR_INITIALIZATION_FAILED;                                  \
        if (!dev->virtual_rt && dev->vtable.GetDeviceProcAddr) {                          \
            PFN_##name native = (PFN_##name)dev->vtable.GetDeviceProcAddr(device, #name); \
            if (native) return native arguments;                                          \
        }                                                                                 \
        return VK_ERROR_FEATURE_NOT_PRESENT;                                              \
    }

HOST_OPTIONAL(vkBuildAccelerationStructuresKHR,
              (VkDevice device, VkDeferredOperationKHR deferred, uint32_t count,
               const VkAccelerationStructureBuildGeometryInfoKHR *infos,
               const VkAccelerationStructureBuildRangeInfoKHR *const *ranges),
              (device, deferred, count, infos, ranges))
HOST_OPTIONAL(vkCopyAccelerationStructureKHR,
              (VkDevice device, VkDeferredOperationKHR deferred, const VkCopyAccelerationStructureInfoKHR *info),
              (device, deferred, info))
HOST_OPTIONAL(vkCopyAccelerationStructureToMemoryKHR,
              (VkDevice device, VkDeferredOperationKHR deferred, const VkCopyAccelerationStructureToMemoryInfoKHR *info),
              (device, deferred, info))
HOST_OPTIONAL(vkCopyMemoryToAccelerationStructureKHR,
              (VkDevice device, VkDeferredOperationKHR deferred, const VkCopyMemoryToAccelerationStructureInfoKHR *info),
              (device, deferred, info))
HOST_OPTIONAL(vkWriteAccelerationStructuresPropertiesKHR,
              (VkDevice device, uint32_t count, const VkAccelerationStructureKHR *objects, VkQueryType type, size_t bytes,
               void *data, size_t stride),
              (device, count, objects, type, bytes, data, stride))
HOST_OPTIONAL(vkGetRayTracingCaptureReplayShaderGroupHandlesKHR,
              (VkDevice device, VkPipeline pipeline, uint32_t first, uint32_t count, size_t bytes, void *data),
              (device, pipeline, first, count, bytes, data))
#undef HOST_OPTIONAL

static VKAPI_ATTR void VKAPI_CALL CmdTraceRaysIndirectKHR(VkCommandBuffer command, const VkStridedDeviceAddressRegionKHR *raygen,
                                                          const VkStridedDeviceAddressRegionKHR *miss,
                                                          const VkStridedDeviceAddressRegionKHR *hit,
                                                          const VkStridedDeviceAddressRegionKHR *callable,
                                                          VkDeviceAddress address) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (!cb) return;
    struct vkrt_device_data *dev = cb->device_data;
    if (dev->virtual_rt) {
        if (cb->record_result == VK_SUCCESS) cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    PFN_vkCmdTraceRaysIndirectKHR native =
        dev->vtable.GetDeviceProcAddr
            ? (PFN_vkCmdTraceRaysIndirectKHR)dev->vtable.GetDeviceProcAddr(dev->device, "vkCmdTraceRaysIndirectKHR")
            : NULL;
    if (native) native(command, raygen, miss, hit, callable, address);
}

PFN_vkVoidFunction vkrt_optional_rt_proc(const char *name) {
#define ENTRY(command) \
    if (!strcmp(name, #command)) return (PFN_vkVoidFunction)optional_##command;
    ENTRY(vkBuildAccelerationStructuresKHR)
    ENTRY(vkCopyAccelerationStructureKHR)
    ENTRY(vkCopyAccelerationStructureToMemoryKHR)
    ENTRY(vkCopyMemoryToAccelerationStructureKHR)
    ENTRY(vkWriteAccelerationStructuresPropertiesKHR)
    ENTRY(vkGetRayTracingCaptureReplayShaderGroupHandlesKHR)
#undef ENTRY
    if (!strcmp(name, "vkCmdTraceRaysIndirectKHR")) return (PFN_vkVoidFunction)CmdTraceRaysIndirectKHR;
    return NULL;
}
