/* SPDX-License-Identifier: MIT */
#include "vkrt_as_serialization.h"
#include "vkrt_layer.h"
#include "vkrt_as_registry.h"
#include "bvh4_abi.h"
#include "vkrt_bvh4_serialization.spv.h"

_Static_assert(sizeof(struct vkrt_serialization_args) == 64, "serialization push ABI");
struct vkrt_serialization_state {
    VkPipelineLayout layout;
    VkPipeline pipelines[2];
    uint8_t version_data[32];
    bool has_allocator;
    VkAllocationCallbacks allocator;
};
static const VkAllocationCallbacks *callbacks(const struct vkrt_serialization_state *state) {
    return state->has_allocator ? &state->allocator : NULL;
}
void vkrt_as_serialization_finish(struct vkrt_device_data *dev) {
    struct vkrt_serialization_state *state = dev->serialization;
    if (!state) return;
    const VkAllocationCallbacks *allocator = callbacks(state);
    for (unsigned i = 0; i < 2; ++i)
        if (state->pipelines[i]) dev->vtable.DestroyPipeline(dev->device, state->pipelines[i], allocator);
    if (state->layout) dev->vtable.DestroyPipelineLayout(dev->device, state->layout, allocator);
    bool has_allocator = state->has_allocator;
    VkAllocationCallbacks saved = state->allocator;
    dev->serialization = NULL;
    if (has_allocator)
        saved.pfnFree(saved.pUserData, state);
    else
        free(state);
}
VkResult vkrt_as_serialization_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator) {
    if (dev->serialization) return VK_SUCCESS;
    if (!dev->instance_data || !dev->instance_data->vtable.GetPhysicalDeviceProperties2 || !dev->vtable.CreatePipelineLayout ||
        !dev->vtable.DestroyPipelineLayout || !dev->vtable.CreateShaderModule || !dev->vtable.DestroyShaderModule ||
        !dev->vtable.CreateComputePipelines || !dev->vtable.DestroyPipeline || !vkrt_as_registry_address(dev))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct vkrt_serialization_state *state =
        allocator ? allocator->pfnAllocation(allocator->pUserData, sizeof(*state), _Alignof(struct vkrt_serialization_state),
                                             VK_SYSTEM_ALLOCATION_SCOPE_DEVICE)
                  : malloc(sizeof(*state));
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(state, 0, sizeof(*state));
    if (allocator) {
        state->has_allocator = true;
        state->allocator = *allocator;
    }
    dev->serialization = state;
    VkPhysicalDeviceIDProperties identity = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &identity};
    dev->instance_data->vtable.GetPhysicalDeviceProperties2(dev->physical_device, &properties);
    memcpy(state->version_data, identity.driverUUID, VK_UUID_SIZE);
    /* The first UUID is exactly the reported driverUUID, as Vulkan requires.
     * The second binds the full deviceUUID to this layer's explicit format.
     * A BVH4/serialization ABI change necessarily changes compatibility. */
    const uint8_t format_uuid[VK_UUID_SIZE] = {0x56, 0x4b, 0x52, 0x54, 0x53, 0x45, 0x52, 0x31,
                                               0x91, 0x5e, 0x78, 0xb4, 0x2c, 0xd8, 0x46, 0x13};
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) state->version_data[VK_UUID_SIZE + i] = identity.deviceUUID[i] ^ format_uuid[i];
    for (unsigned i = 0; i < 4; ++i) {
        state->version_data[VK_UUID_SIZE + i] ^= (uint8_t)(VEL_RT_BVH4_ABI_VERSION >> (i * 8));
        state->version_data[VK_UUID_SIZE + 4 + i] ^= (uint8_t)(VKRT_AS_SERIAL_FORMAT_VERSION >> (i * 8));
    }
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct vkrt_serialization_args)};
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    VkPipelineLayout created_layout = VK_NULL_HANDLE;
    VkResult result = dev->vtable.CreatePipelineLayout(dev->device, &layout, callbacks(state), &created_layout);
    if (result != VK_SUCCESS) {
        vkrt_as_serialization_finish(dev);
        return result;
    }
    state->layout = created_layout;
    VkShaderModuleCreateInfo shader = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = sizeof(vkrt_spv_vkrt_bvh4_serialization),
                                       .pCode = vkrt_spv_vkrt_bvh4_serialization};
    VkShaderModule module = VK_NULL_HANDLE;
    result = dev->vtable.CreateShaderModule(dev->device, &shader, callbacks(state), &module);
    if (result == VK_SUCCESS) {
        struct {
            uint32_t operation, padding;
            uint64_t context;
        } values = {0, 0, vkrt_as_registry_address(dev)};
        const VkSpecializationMapEntry entries[] = {{0, 0, 4}, {6, 8, 8}};
        VkSpecializationInfo spec = {2, entries, sizeof(values), &values};
        for (unsigned i = 0; i < 2 && result == VK_SUCCESS; ++i) {
            values.operation = i;
            VkComputePipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                    .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                              .module = module,
                                                              .pName = "main",
                                                              .pSpecializationInfo = &spec},
                                                    .layout = state->layout};
            result = dev->vtable.CreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &pipeline, callbacks(state),
                                                        &state->pipelines[i]);
        }
        dev->vtable.DestroyShaderModule(dev->device, module, callbacks(state));
    }
    if (result != VK_SUCCESS) vkrt_as_serialization_finish(dev);
    return result;
}
void vkrt_as_serialization_compatibility(struct vkrt_device_data *dev, const VkAccelerationStructureVersionInfoKHR *info,
                                         VkAccelerationStructureCompatibilityKHR *compatibility) {
    if (!compatibility) return;
    *compatibility = VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR;
    if (dev && dev->serialization && info && info->sType == VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR &&
        !info->pNext && info->pVersionData && memcmp(info->pVersionData, dev->serialization->version_data, 32) == 0)
        *compatibility = VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR;
}
static void barrier(struct vkrt_device_data *dev, VkCommandBuffer command) {
    VkMemoryBarrier memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    dev->vtable.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory,
                                   0, NULL, 0, NULL);
}
static void record(struct vkrt_device_data *dev, struct vkrt_cmd_buffer_data *cb, VkAccelerationStructureKHR handle,
                   VkDeviceAddress serialized, uint32_t deserialize) {
    struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, handle);
    if (!as || cb->device_data != dev || as->owner != dev->device || as->type > VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR ||
        !serialized || (serialized & 255)) {
        cb->record_result = VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    if (as->size < sizeof(VelRtBvh4Header) || serialized > UINT64_MAX - VKRT_AS_SERIAL_HEADER_SIZE) {
        cb->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return;
    }
    struct vkrt_serialization_state *state = dev->serialization;
    if (!state || !state->pipelines[deserialize] || !state->layout || !dev->max_build_dispatch_x || !dev->vtable.CmdBindPipeline ||
        !dev->vtable.CmdPushConstants || !dev->vtable.CmdDispatch || !dev->vtable.CmdPipelineBarrier) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    /* Deserialization constructs its destination, including descriptor-only
     * and GENERIC arithmetic use. No CPU readback or later GetASAddress call. */
    VkDeviceAddress physical = vkrt_as_backing_address(dev, handle);
    if (!physical || (physical & 3)) {
        cb->record_result = VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    if (physical > UINT64_MAX - as->size) {
        cb->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return;
    }
    struct vkrt_serialization_args args = {.source = deserialize ? serialized : physical,
                                           .destination = deserialize ? physical : serialized,
                                           .as_capacity = as->size,
                                           .destination_type = as->type};
    memcpy(args.version_data, state->version_data, sizeof(args.version_data));
    uint64_t groups = (as->size / 4 + 63) / 64;
    if (groups > 1024) groups = 1024;
    if (groups > dev->max_build_dispatch_x) groups = dev->max_build_dispatch_x;
    barrier(dev, cb->cmd);
    dev->vtable.CmdBindPipeline(cb->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state->pipelines[deserialize]);
    dev->vtable.CmdPushConstants(cb->cmd, state->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
    dev->vtable.CmdDispatch(cb->cmd, (uint32_t)groups, 1, 1);
    barrier(dev, cb->cmd);
    vkrt_replay_compute_binding(cb);
    vkrt_replay_push_constants(cb);
}
void vkrt_cmd_serialize_as(struct vkrt_device_data *dev, VkCommandBuffer command,
                           const VkCopyAccelerationStructureToMemoryInfoKHR *info) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    if (!info || info->sType != VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR || info->pNext ||
        info->mode != VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    record(dev, cb, info->src, info->dst.deviceAddress, 0);
}
void vkrt_cmd_deserialize_as(struct vkrt_device_data *dev, VkCommandBuffer command,
                             const VkCopyMemoryToAccelerationStructureInfoKHR *info) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    if (!info || info->sType != VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR || info->pNext ||
        info->mode != VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    record(dev, cb, info->dst, info->src.deviceAddress, 1);
}
