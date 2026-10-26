/* SPDX-License-Identifier: MIT */
#include "vkrt_as_copy.h"
#include "vkrt_layer.h"
#include "bvh4_abi.h"
#include "vkrt_bvh4_copy.spv.h"

_Static_assert(sizeof(struct vkrt_as_copy_args) == 40, "BVH4 copy push ABI");

void vkrt_as_copy_finish(struct vkrt_device_data *dev) {
    if (dev->copy_pipeline) dev->vtable.DestroyPipeline(dev->device, dev->copy_pipeline, NULL);
    if (dev->copy_pipeline_layout) dev->vtable.DestroyPipelineLayout(dev->device, dev->copy_pipeline_layout, NULL);
    dev->copy_pipeline = VK_NULL_HANDLE;
    dev->copy_pipeline_layout = VK_NULL_HANDLE;
}

VkResult vkrt_as_copy_init(struct vkrt_device_data *dev) {
    if (!dev->vtable.CreatePipelineLayout || !dev->vtable.DestroyPipelineLayout || !dev->vtable.CreateShaderModule ||
        !dev->vtable.DestroyShaderModule || !dev->vtable.CreateComputePipelines || !dev->vtable.DestroyPipeline)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct vkrt_as_copy_args)};
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    VkPipelineLayout created_layout = VK_NULL_HANDLE;
    VkResult result = dev->vtable.CreatePipelineLayout(dev->device, &layout, NULL, &created_layout);
    if (result != VK_SUCCESS) return result;
    dev->copy_pipeline_layout = created_layout;
    VkShaderModuleCreateInfo shader = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = sizeof(vkrt_spv_vkrt_bvh4_copy),
                                       .pCode = vkrt_spv_vkrt_bvh4_copy};
    VkShaderModule module = VK_NULL_HANDLE;
    result = dev->vtable.CreateShaderModule(dev->device, &shader, NULL, &module);
    if (result == VK_SUCCESS) {
        VkComputePipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                          .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                          .module = module,
                                                          .pName = "main"},
                                                .layout = dev->copy_pipeline_layout};
        result = dev->vtable.CreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &dev->copy_pipeline);
        dev->vtable.DestroyShaderModule(dev->device, module, NULL);
    }
    if (result != VK_SUCCESS) vkrt_as_copy_finish(dev);
    return result;
}

static void copy_barrier(struct vkrt_device_data *dev, VkCommandBuffer command) {
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    dev->vtable.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier,
                                   0, NULL, 0, NULL);
}

void vkrt_cmd_copy_as(struct vkrt_device_data *dev, VkCommandBuffer command, const VkCopyAccelerationStructureInfoKHR *info) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    if (!info || info->sType != VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR || info->pNext ||
        (info->mode != VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR &&
         info->mode != VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR)) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    struct vkrt_virtual_as *source = VKRT_FIND(struct vkrt_virtual_as, info->src);
    struct vkrt_virtual_as *destination = VKRT_FIND(struct vkrt_virtual_as, info->dst);
    if (cb->device_data != dev || !source || !destination || source->owner != dev->device || destination->owner != dev->device ||
        source == destination || destination->type > VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR) {
        cb->record_result = VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    if (source->size < sizeof(VelRtBvh4Header) || destination->size < sizeof(VelRtBvh4Header)) {
        cb->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return;
    }
    if (!dev->copy_pipeline || !dev->copy_pipeline_layout || !dev->max_build_dispatch_x || !dev->vtable.CmdBindPipeline ||
        !dev->vtable.CmdPushConstants || !dev->vtable.CmdDispatch || !dev->vtable.CmdPipelineBarrier) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    VkDeviceAddress src = vkrt_as_backing_address(dev, info->src);
    /* Copy constructs its destination. Publish its preallocated registry leaf
     * even if the application uses only a descriptor or GENERIC arithmetic. */
    VkDeviceAddress dst = vkrt_as_backing_address(dev, info->dst);
    if (!src || !dst || src == dst || (src & 3) || (dst & 3)) {
        cb->record_result = VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    if (src > UINT64_MAX - source->size || dst > UINT64_MAX - destination->size) {
        cb->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return;
    }
    struct vkrt_as_copy_args args = {src, dst, source->size, destination->size, destination->type, 0};
    uint64_t capacity = source->size < destination->size ? source->size : destination->size;
    uint64_t groups = (capacity / 4 + 63) / 64;
    if (groups > 1024) groups = 1024;
    if (groups > dev->max_build_dispatch_x) groups = dev->max_build_dispatch_x;
    copy_barrier(dev, command);
    dev->vtable.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, dev->copy_pipeline);
    dev->vtable.CmdPushConstants(command, dev->copy_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
    dev->vtable.CmdDispatch(command, (uint32_t)groups, 1, 1);
    copy_barrier(dev, command);
    /* Descriptors and graphics pipeline state were never changed. */
    if (cb->bound_compute_pipeline)
        dev->vtable.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, cb->bound_compute_pipeline);
    for (struct vkrt_saved_push *push = cb->push_head; push; push = push->next)
        dev->vtable.CmdPushConstants(command, push->layout, push->stages, push->offset, push->size, push->bytes);
}
