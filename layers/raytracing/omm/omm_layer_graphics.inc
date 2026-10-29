// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Included by omm_layer.cpp after the shared command and consumer adapters.

// Generated commands can bind pipelines and descriptor contexts on the GPU.
// They require their own consumer path before software OMM can execute them.
VKAPI_ATTR void VKAPI_CALL CmdPreprocessGeneratedCommandsNV(VkCommandBuffer cb, const VkGeneratedCommandsInfoNV* info) {
    auto state = device(cb);
    if (state->software) {
        fail_command(*state, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return;
    }
    native<PFN_vkCmdPreprocessGeneratedCommandsNV>(*state, "vkCmdPreprocessGeneratedCommandsNV")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdPreprocessGeneratedCommandsEXT(VkCommandBuffer cb, const VkGeneratedCommandsInfoEXT* info,
                                                             VkCommandBuffer state_cb) {
    auto state = device(cb);
    if (state->software) {
        fail_command(*state, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return;
    }
    native<PFN_vkCmdPreprocessGeneratedCommandsEXT>(*state, "vkCmdPreprocessGeneratedCommandsEXT")(cb, info, state_cb);
}
VKAPI_ATTR void VKAPI_CALL CmdExecuteGeneratedCommandsNV(VkCommandBuffer cb, VkBool32 preprocessed,
                                                         const VkGeneratedCommandsInfoNV* info) {
    auto state = device(cb);
    if (state->software) {
        fail_command(*state, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return;
    }
    native<PFN_vkCmdExecuteGeneratedCommandsNV>(*state, "vkCmdExecuteGeneratedCommandsNV")(cb, preprocessed, info);
}
VKAPI_ATTR void VKAPI_CALL CmdExecuteGeneratedCommandsEXT(VkCommandBuffer cb, VkBool32 preprocessed,
                                                          const VkGeneratedCommandsInfoEXT* info) {
    auto state = device(cb);
    if (state->software) {
        fail_command(*state, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return;
    }
    native<PFN_vkCmdExecuteGeneratedCommandsEXT>(*state, "vkCmdExecuteGeneratedCommandsEXT")(cb, preprocessed, info);
}
VKAPI_ATTR void VKAPI_CALL CmdBindPipelineShaderGroupNV(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline,
                                                        uint32_t group) {
    auto state = device(cb);
    if (state->software) {
        fail_command(*state, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return;
    }
    native<PFN_vkCmdBindPipelineShaderGroupNV>(*state, "vkCmdBindPipelineShaderGroupNV")(cb, point, pipeline, group);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice handle, const VkRenderPassCreateInfo* info,
                                                const VkAllocationCallbacks* allocator, VkRenderPass* out) {
    auto state = device(handle);
    auto result = native<PFN_vkCreateRenderPass>(*state, "vkCreateRenderPass")(handle, info, allocator, out);
    if (result != VK_SUCCESS || !state->consumers) return result;
    result = consumer_track_render_pass(state->consumers, *out, info);
    if (result != VK_SUCCESS) {
        native<PFN_vkDestroyRenderPass>(*state, "vkDestroyRenderPass")(handle, *out, allocator);
        *out = VK_NULL_HANDLE;
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice handle, const VkRenderPassCreateInfo2* info,
                                                 const VkAllocationCallbacks* allocator, VkRenderPass* out) {
    auto state = device(handle);
    auto call = native<PFN_vkCreateRenderPass2>(*state, "vkCreateRenderPass2");
    if (!call) call = native<PFN_vkCreateRenderPass2>(*state, "vkCreateRenderPass2KHR");
    auto result = call(handle, info, allocator, out);
    if (result != VK_SUCCESS || !state->consumers) return result;
    result = consumer_track_render_pass2(state->consumers, *out, info);
    if (result != VK_SUCCESS) {
        native<PFN_vkDestroyRenderPass>(*state, "vkDestroyRenderPass")(handle, *out, allocator);
        *out = VK_NULL_HANDLE;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(VkDevice handle, VkRenderPass pass, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->consumers) consumer_untrack_render_pass(state->consumers, pass);
    native<PFN_vkDestroyRenderPass>(*state, "vkDestroyRenderPass")(handle, pass, allocator);
}

// Descriptor binds only change the shadow context. Defer its pipeline change
// until a draw: an extra bind of a static-state pipeline between a dynamic-state
// setter and a later public dynamic pipeline bind would invalidate that state.
// At a valid draw, rebinding a variant with the same static/dynamic declarations
// preserves all dynamic state required by that draw.
VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                                   uint32_t first_instance) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDraw>(*state, "vkCmdDraw")(cb, vertices, instances, first_vertex, first_instance);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cb, uint32_t indices, uint32_t instances, uint32_t first_index,
                                          int32_t vertex_offset, uint32_t first_instance) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawIndexed>(*state, "vkCmdDrawIndexed")(cb, indices, instances, first_index, vertex_offset,
                                                                 first_instance);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t count,
                                           uint32_t stride) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb)) native<PFN_vkCmdDrawIndirect>(*state, "vkCmdDrawIndirect")(cb, buffer, offset, count, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t count,
                                                  uint32_t stride) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawIndexedIndirect>(*state, "vkCmdDrawIndexedIndirect")(cb, buffer, offset, count, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawMeshTasksIndirectNV(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t count,
                                                      uint32_t stride) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawMeshTasksIndirectNV>(*state, "vkCmdDrawMeshTasksIndirectNV")(cb, buffer, offset, count, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawMeshTasksIndirectEXT(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t count,
                                                       uint32_t stride) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawMeshTasksIndirectEXT>(*state, "vkCmdDrawMeshTasksIndirectEXT")(cb, buffer, offset, count, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawMeshTasksIndirectCountNV(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset,
                                                           VkBuffer counter, VkDeviceSize counter_offset, uint32_t max_count,
                                                           uint32_t stride) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawMeshTasksIndirectCountNV>(*state, "vkCmdDrawMeshTasksIndirectCountNV")(
            cb, buffer, offset, counter, counter_offset, max_count, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawMeshTasksIndirectCountEXT(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset,
                                                            VkBuffer counter, VkDeviceSize counter_offset, uint32_t max_count,
                                                            uint32_t stride) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawMeshTasksIndirectCountEXT>(*state, "vkCmdDrawMeshTasksIndirectCountEXT")(
            cb, buffer, offset, counter, counter_offset, max_count, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawMeshTasksNV(VkCommandBuffer cb, uint32_t count, uint32_t first) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb)) native<PFN_vkCmdDrawMeshTasksNV>(*state, "vkCmdDrawMeshTasksNV")(cb, count, first);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawMeshTasksEXT(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb)) native<PFN_vkCmdDrawMeshTasksEXT>(*state, "vkCmdDrawMeshTasksEXT")(cb, x, y, z);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawClusterHUAWEI(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb)) native<PFN_vkCmdDrawClusterHUAWEI>(*state, "vkCmdDrawClusterHUAWEI")(cb, x, y, z);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawClusterIndirectHUAWEI(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawClusterIndirectHUAWEI>(*state, "vkCmdDrawClusterIndirectHUAWEI")(cb, buffer, offset);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawMultiEXT(VkCommandBuffer cb, uint32_t count, const VkMultiDrawInfoEXT* infos, uint32_t instances,
                                           uint32_t first_instance, uint32_t stride) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawMultiEXT>(*state, "vkCmdDrawMultiEXT")(cb, count, infos, instances, first_instance, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawMultiIndexedEXT(VkCommandBuffer cb, uint32_t count, const VkMultiDrawIndexedInfoEXT* infos,
                                                  uint32_t instances, uint32_t first_instance, uint32_t stride,
                                                  const int32_t* vertex_offsets) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawMultiIndexedEXT>(*state, "vkCmdDrawMultiIndexedEXT")(cb, count, infos, instances, first_instance,
                                                                                 stride, vertex_offsets);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectByteCountEXT(VkCommandBuffer cb, uint32_t instances, uint32_t first_instance,
                                                       VkBuffer counter, VkDeviceSize counter_offset, uint32_t vertex_offset,
                                                       uint32_t stride) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawIndirectByteCountEXT>(*state, "vkCmdDrawIndirectByteCountEXT")(cb, instances, first_instance, counter,
                                                                                           counter_offset, vertex_offset, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect2KHR(VkCommandBuffer cb, const VkDrawIndirect2InfoKHR* info) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb)) native<PFN_vkCmdDrawIndirect2KHR>(*state, "vkCmdDrawIndirect2KHR")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect2KHR(VkCommandBuffer cb, const VkDrawIndirect2InfoKHR* info) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb)) native<PFN_vkCmdDrawIndexedIndirect2KHR>(*state, "vkCmdDrawIndexedIndirect2KHR")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawMeshTasksIndirect2EXT(VkCommandBuffer cb, const VkDrawIndirect2InfoKHR* info) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawMeshTasksIndirect2EXT>(*state, "vkCmdDrawMeshTasksIndirect2EXT")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCount2KHR(VkCommandBuffer cb, const VkDrawIndirectCount2InfoKHR* info) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb)) native<PFN_vkCmdDrawIndirectCount2KHR>(*state, "vkCmdDrawIndirectCount2KHR")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount2KHR(VkCommandBuffer cb, const VkDrawIndirectCount2InfoKHR* info) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawIndexedIndirectCount2KHR>(*state, "vkCmdDrawIndexedIndirectCount2KHR")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawMeshTasksIndirectCount2EXT(VkCommandBuffer cb, const VkDrawIndirectCount2InfoKHR* info) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawMeshTasksIndirectCount2EXT>(*state, "vkCmdDrawMeshTasksIndirectCount2EXT")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectByteCount2EXT(VkCommandBuffer cb, uint32_t instances, uint32_t first_instance,
                                                        const VkBindTransformFeedbackBuffer2InfoEXT* info, uint32_t vertex_offset,
                                                        uint32_t stride) {
    auto state = device(cb);
    if (refresh_graphics(*state, cb))
        native<PFN_vkCmdDrawIndirectByteCount2EXT>(*state, "vkCmdDrawIndirectByteCount2EXT")(cb, instances, first_instance, info,
                                                                                             vertex_offset, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkBuffer counter,
                                                VkDeviceSize counter_offset, uint32_t max_count, uint32_t stride) {
    auto state = device(cb);
    auto call = native<PFN_vkCmdDrawIndirectCount>(*state, "vkCmdDrawIndirectCount");
    if (!call) call = native<PFN_vkCmdDrawIndirectCount>(*state, "vkCmdDrawIndirectCountKHR");
    if (!call) call = native<PFN_vkCmdDrawIndirectCount>(*state, "vkCmdDrawIndirectCountAMD");
    if (refresh_graphics(*state, cb)) call(cb, buffer, offset, counter, counter_offset, max_count, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkBuffer counter,
                                                       VkDeviceSize counter_offset, uint32_t max_count, uint32_t stride) {
    auto state = device(cb);
    auto call = native<PFN_vkCmdDrawIndexedIndirectCount>(*state, "vkCmdDrawIndexedIndirectCount");
    if (!call) call = native<PFN_vkCmdDrawIndexedIndirectCount>(*state, "vkCmdDrawIndexedIndirectCountKHR");
    if (!call) call = native<PFN_vkCmdDrawIndexedIndirectCount>(*state, "vkCmdDrawIndexedIndirectCountAMD");
    if (refresh_graphics(*state, cb)) call(cb, buffer, offset, counter, counter_offset, max_count, stride);
}
VKAPI_ATTR void VKAPI_CALL CmdBeginTransformFeedbackEXT(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer* buffers,
                                                        const VkDeviceSize* offsets) {
    auto state = device(cb);
    native<PFN_vkCmdBeginTransformFeedbackEXT>(*state, "vkCmdBeginTransformFeedbackEXT")(cb, first, count, buffers, offsets);
    if (state->software)
        if (auto entry = command(*state, cb)) entry->transform_feedback_active = true;
}
VKAPI_ATTR void VKAPI_CALL CmdBeginTransformFeedback2EXT(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                                         const VkBindTransformFeedbackBuffer2InfoEXT* infos) {
    auto state = device(cb);
    native<PFN_vkCmdBeginTransformFeedback2EXT>(*state, "vkCmdBeginTransformFeedback2EXT")(cb, first, count, infos);
    if (state->software)
        if (auto entry = command(*state, cb)) entry->transform_feedback_active = true;
}
VKAPI_ATTR void VKAPI_CALL CmdEndTransformFeedbackEXT(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer* buffers,
                                                      const VkDeviceSize* offsets) {
    auto state = device(cb);
    native<PFN_vkCmdEndTransformFeedbackEXT>(*state, "vkCmdEndTransformFeedbackEXT")(cb, first, count, buffers, offsets);
    if (state->software)
        if (auto entry = command(*state, cb)) entry->transform_feedback_active = false;
}
VKAPI_ATTR void VKAPI_CALL CmdEndTransformFeedback2EXT(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                                       const VkBindTransformFeedbackBuffer2InfoEXT* infos) {
    auto state = device(cb);
    native<PFN_vkCmdEndTransformFeedback2EXT>(*state, "vkCmdEndTransformFeedback2EXT")(cb, first, count, infos);
    if (state->software)
        if (auto entry = command(*state, cb)) entry->transform_feedback_active = false;
}
