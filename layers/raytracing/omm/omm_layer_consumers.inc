// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Included inside vel::omm::layer; native handles remain application handles.
VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(VkDevice handle, const VkCommandPoolCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator, VkCommandPool* out) {
    auto state = device(handle);
    auto result = native<PFN_vkCreateCommandPool>(*state, "vkCreateCommandPool")(handle, info, allocator, out);
    if (result == VK_SUCCESS && state->software) {
        try {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->pool_families.emplace(*out, info->queueFamilyIndex);
        } catch (const std::bad_alloc&) {
            native<PFN_vkDestroyCommandPool>(*state, "vkDestroyCommandPool")(handle, *out, allocator);
            *out = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyQueryPool(VkDevice handle, VkQueryPool pool, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->properties && properties_is_pool(state->properties, pool))
        properties_destroy_pool(state->properties, pool, allocator);
    else
        native<PFN_vkDestroyQueryPool>(*state, "vkDestroyQueryPool")(handle, pool, allocator);
}
VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count) {
    auto state = device(cb);
    if (state->properties && properties_is_pool(state->properties, pool))
        record_property(*state, cb, [&](uint32_t family, PropertyRecordingRef* out) {
            return properties_cmd_reset(state->properties, cb, family, pool, first, count, out);
        });
    else
        native<PFN_vkCmdResetQueryPool>(*state, "vkCmdResetQueryPool")(cb, pool, first, count);
}
VKAPI_ATTR void VKAPI_CALL ResetQueryPool(VkDevice handle, VkQueryPool pool, uint32_t first, uint32_t count) {
    auto state = device(handle);
    if (state->properties && properties_is_pool(state->properties, pool)) {
        const auto result = properties_reset_host(state->properties, pool, first, count);
        if (result != VK_SUCCESS) fail_device(*state, result);
    } else {
        auto call = native<PFN_vkResetQueryPool>(*state, "vkResetQueryPool");
        if (!call) call = native<PFN_vkResetQueryPool>(*state, "vkResetQueryPoolEXT");
        call(handle, pool, first, count);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL GetQueryPoolResults(VkDevice handle, VkQueryPool pool, uint32_t first, uint32_t count, size_t size,
                                                   void* data, VkDeviceSize stride, VkQueryResultFlags flags) {
    auto state = device(handle);
    return state->properties && properties_is_pool(state->properties, pool)
               ? properties_get_results(state->properties, pool, first, count, size, data, stride, flags)
               : native<PFN_vkGetQueryPoolResults>(*state, "vkGetQueryPoolResults")(handle, pool, first, count, size, data, stride,
                                                                                    flags);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count,
                                                   VkBuffer buffer, VkDeviceSize offset, VkDeviceSize stride,
                                                   VkQueryResultFlags flags) {
    auto state = device(cb);
    if (state->properties && properties_is_pool(state->properties, pool))
        record_property(*state, cb, [&](uint32_t family, PropertyRecordingRef* out) {
            return properties_cmd_copy_results(state->properties, cb, family, pool, first, count, buffer, offset, stride, flags,
                                               out);
        });
    else
        native<PFN_vkCmdCopyQueryPoolResults>(*state, "vkCmdCopyQueryPoolResults")(cb, pool, first, count, buffer, offset, stride,
                                                                                   flags);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateAccelerationStructureKHR(VkDevice handle, const VkAccelerationStructureCreateInfoKHR* info,
                                                              const VkAllocationCallbacks* allocator,
                                                              VkAccelerationStructureKHR* out) {
    auto state = device(handle);
    auto result =
        native<PFN_vkCreateAccelerationStructureKHR>(*state, "vkCreateAccelerationStructureKHR")(handle, info, allocator, out);
    if (result == VK_SUCCESS && state->accelerations) {
        result = acceleration_track(state->accelerations, *out, info);
        if (result != VK_SUCCESS) {
            native<PFN_vkDestroyAccelerationStructureKHR>(*state, "vkDestroyAccelerationStructureKHR")(handle, *out, allocator);
            *out = VK_NULL_HANDLE;
        }
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyAccelerationStructureKHR(VkDevice handle, VkAccelerationStructureKHR structure,
                                                           const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->submissions) submissions_poll(state->submissions);
    if (state->accelerations) acceleration_untrack(state->accelerations, structure);
    native<PFN_vkDestroyAccelerationStructureKHR>(*state, "vkDestroyAccelerationStructureKHR")(handle, structure, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice handle, const VkDescriptorSetLayoutCreateInfo* info,
                                                         const VkAllocationCallbacks* allocator, VkDescriptorSetLayout* out) {
    auto state = device(handle);
    auto result = native<PFN_vkCreateDescriptorSetLayout>(*state, "vkCreateDescriptorSetLayout")(handle, info, allocator, out);
    if (result != VK_SUCCESS || !state->descriptors) return result;
    result = descriptor_track_layout(state->descriptors, *out, info);
    if (result == VK_SUCCESS) {
        result = consumer_track_set_layout(state->consumers, *out, info);
        if (result != VK_SUCCESS) descriptor_untrack_layout(state->descriptors, *out);
    }
    if (result != VK_SUCCESS) {
        native<PFN_vkDestroyDescriptorSetLayout>(*state, "vkDestroyDescriptorSetLayout")(handle, *out, allocator);
        *out = VK_NULL_HANDLE;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(VkDevice handle, VkDescriptorSetLayout layout,
                                                      const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->descriptors) {
        consumer_untrack_set_layout(state->consumers, layout);
        descriptor_untrack_layout(state->descriptors, layout);
    }
    native<PFN_vkDestroyDescriptorSetLayout>(*state, "vkDestroyDescriptorSetLayout")(handle, layout, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorPool(VkDevice handle, const VkDescriptorPoolCreateInfo* info,
                                                    const VkAllocationCallbacks* allocator, VkDescriptorPool* out) {
    auto state = device(handle);
    auto result = native<PFN_vkCreateDescriptorPool>(*state, "vkCreateDescriptorPool")(handle, info, allocator, out);
    if (result == VK_SUCCESS && state->descriptors) {
        result = descriptor_track_pool(state->descriptors, *out, info);
        if (result != VK_SUCCESS) {
            native<PFN_vkDestroyDescriptorPool>(*state, "vkDestroyDescriptorPool")(handle, *out, allocator);
            *out = VK_NULL_HANDLE;
        }
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(VkDevice handle, VkDescriptorPool pool, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->descriptors) descriptor_untrack_pool(state->descriptors, pool);
    native<PFN_vkDestroyDescriptorPool>(*state, "vkDestroyDescriptorPool")(handle, pool, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool(VkDevice handle, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags) {
    auto state = device(handle);
    auto result = native<PFN_vkResetDescriptorPool>(*state, "vkResetDescriptorPool")(handle, pool, flags);
    if (result == VK_SUCCESS && state->descriptors) descriptor_reset_pool(state->descriptors, pool);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice handle, const VkDescriptorSetAllocateInfo* info,
                                                      VkDescriptorSet* out) {
    auto state = device(handle);
    DescriptorAllocation* allocation = nullptr;
    if (state->descriptors) {
        const auto prepared = descriptor_prepare_allocate_sets(state->descriptors, info, &allocation);
        if (prepared != VK_SUCCESS) {
            for (uint32_t i = 0; i < info->descriptorSetCount; ++i) out[i] = VK_NULL_HANDLE;
            return prepared;
        }
    }
    auto result = native<PFN_vkAllocateDescriptorSets>(*state, "vkAllocateDescriptorSets")(handle, info, out);
    if (allocation) {
        if (result == VK_SUCCESS)
            descriptor_commit_allocate_sets(allocation, out);
        else
            descriptor_abort_allocate_sets(allocation);
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL FreeDescriptorSets(VkDevice handle, VkDescriptorPool pool, uint32_t count,
                                                  const VkDescriptorSet* sets) {
    auto state = device(handle);
    auto result = native<PFN_vkFreeDescriptorSets>(*state, "vkFreeDescriptorSets")(handle, pool, count, sets);
    if (result == VK_SUCCESS && state->descriptors) descriptor_free_sets(state->descriptors, pool, count, sets);
    return result;
}
VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice handle, uint32_t count, const VkWriteDescriptorSet* writes,
                                                uint32_t copy_count, const VkCopyDescriptorSet* copies) {
    auto state = device(handle);
    if (state->descriptors) {
        auto result = descriptor_update(state->descriptors, count, writes, copy_count, copies);
        if (result != VK_SUCCESS) {
            fail_device(*state, result);
            return;
        }
    }
    native<PFN_vkUpdateDescriptorSets>(*state, "vkUpdateDescriptorSets")(handle, count, writes, copy_count, copies);
}
void bind_shadow_sets(Device& state, VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
                      uint32_t count, const VkDescriptorSet* sets) {
    if (!state.consumers) return;
    if (auto entry = command(state, cb)) {
        auto* binding = point == VK_PIPELINE_BIND_POINT_COMPUTE    ? &entry->compute
                        : point == VK_PIPELINE_BIND_POINT_GRAPHICS ? &entry->graphics
                                                                   : nullptr;
        if (binding) {
            const auto result = consumer_bind_sets(state.consumers, *binding, layout, first, count, sets);
            if (result != VK_SUCCESS) entry->state.fail(result);
        }
    }
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout,
                                                 uint32_t first, uint32_t count, const VkDescriptorSet* sets, uint32_t offset_count,
                                                 const uint32_t* offsets) {
    auto state = device(cb);
    bind_shadow_sets(*state, cb, point, layout, first, count, sets);
    native<PFN_vkCmdBindDescriptorSets>(*state, "vkCmdBindDescriptorSets")(cb, point, layout, first, count, sets, offset_count,
                                                                           offsets);
    if (state->consumers && point == VK_PIPELINE_BIND_POINT_COMPUTE) refresh_compute(*state, cb);
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets2(VkCommandBuffer cb, const VkBindDescriptorSetsInfo* info) {
    auto state = device(cb);
    if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        bind_shadow_sets(*state, cb, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->firstSet, info->descriptorSetCount,
                         info->pDescriptorSets);
    if (info->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
        bind_shadow_sets(*state, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->firstSet, info->descriptorSetCount,
                         info->pDescriptorSets);
    auto call = native<PFN_vkCmdBindDescriptorSets2>(*state, "vkCmdBindDescriptorSets2");
    if (!call) call = native<PFN_vkCmdBindDescriptorSets2>(*state, "vkCmdBindDescriptorSets2KHR");
    call(cb, info);
    if (state->consumers && (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)) refresh_compute(*state, cb);
}
VKAPI_ATTR void VKAPI_CALL CmdPushConstants2(VkCommandBuffer cb, const VkPushConstantsInfo* info) {
    CmdPushConstants(cb, info->layout, info->stageFlags, info->offset, info->size, info->pValues);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice handle, const VkShaderModuleCreateInfo* info,
                                                  const VkAllocationCallbacks* allocator, VkShaderModule* out) {
    auto state = device(handle);
    return state->consumers ? consumer_create_shader(state->consumers, info, allocator, out)
                            : native<PFN_vkCreateShaderModule>(*state, "vkCreateShaderModule")(handle, info, allocator, out);
}
VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice handle, VkShaderModule module, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->consumers)
        consumer_destroy_shader(state->consumers, module, allocator);
    else
        native<PFN_vkDestroyShaderModule>(*state, "vkDestroyShaderModule")(handle, module, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice handle, VkPipelineCache cache, uint32_t count,
                                                      const VkComputePipelineCreateInfo* infos,
                                                      const VkAllocationCallbacks* allocator, VkPipeline* out) {
    auto state = device(handle);
    return state->consumers ? consumer_create_compute(state->consumers, cache, count, infos, allocator, out)
                            : native<PFN_vkCreateComputePipelines>(*state, "vkCreateComputePipelines")(handle, cache, count, infos,
                                                                                                       allocator, out);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice handle, VkPipelineCache cache, uint32_t count,
                                                       const VkGraphicsPipelineCreateInfo* infos,
                                                       const VkAllocationCallbacks* allocator, VkPipeline* out) {
    auto state = device(handle);
    if (state->consumers)
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t j = 0; j < infos[i].stageCount; ++j)
                if (consumer_shader_uses_query(state->consumers, infos[i].pStages[j])) {
                    for (uint32_t k = 0; k < count; ++k) out[k] = VK_NULL_HANDLE;
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                }
    return native<PFN_vkCreateGraphicsPipelines>(*state, "vkCreateGraphicsPipelines")(handle, cache, count, infos, allocator, out);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(VkDevice handle, VkDeferredOperationKHR deferred, VkPipelineCache cache,
                                                            uint32_t count, const VkRayTracingPipelineCreateInfoKHR* infos,
                                                            const VkAllocationCallbacks* allocator, VkPipeline* out) {
    auto state = device(handle);
    if (state->software) {
        for (uint32_t i = 0; i < count; ++i) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return native<PFN_vkCreateRayTracingPipelinesKHR>(*state, "vkCreateRayTracingPipelinesKHR")(handle, deferred, cache, count,
                                                                                                infos, allocator, out);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateShadersEXT(VkDevice handle, uint32_t count, const VkShaderCreateInfoEXT* infos,
                                                const VkAllocationCallbacks* allocator, VkShaderEXT* out) {
    auto state = device(handle);
    if (state->software) {
        for (uint32_t i = 0; i < count; ++i) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return native<PFN_vkCreateShadersEXT>(*state, "vkCreateShadersEXT")(handle, count, infos, allocator, out);
}
VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice handle, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->consumers)
        consumer_destroy_pipeline(state->consumers, pipeline, allocator);
    else
        native<PFN_vkDestroyPipeline>(*state, "vkDestroyPipeline")(handle, pipeline, allocator);
}
bool dispatch_ready(Device& state, VkCommandBuffer cb) {
    if (!state.software) return true;
    refresh_compute(state, cb);
    auto entry = command(state, cb);
    return entry && state.error.load() == VK_SUCCESS && entry->state.status() == VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    auto state = device(cb);
    if (dispatch_ready(*state, cb)) native<PFN_vkCmdDispatch>(*state, "vkCmdDispatch")(cb, x, y, z);
}
VKAPI_ATTR void VKAPI_CALL CmdDispatchBase(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y,
                                           uint32_t z) {
    auto state = device(cb);
    auto call = native<PFN_vkCmdDispatchBase>(*state, "vkCmdDispatchBase");
    if (!call) call = native<PFN_vkCmdDispatchBase>(*state, "vkCmdDispatchBaseKHR");
    if (dispatch_ready(*state, cb)) call(cb, bx, by, bz, x, y, z);
}
VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset) {
    auto state = device(cb);
    if (dispatch_ready(*state, cb)) native<PFN_vkCmdDispatchIndirect>(*state, "vkCmdDispatchIndirect")(cb, buffer, offset);
}
VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer cb, uint32_t count, const VkCommandBuffer* children) {
    auto state = device(cb);
    if (state->software) {
        try {
            if (auto entry = command(*state, cb)) {
                entry->secondary.reserve(entry->secondary.size() + count);
                for (uint32_t i = 0; i < count; ++i) {
                    auto child = command(*state, children[i]);
                    if (!child || child->state.status() != VK_SUCCESS) {
                        entry->state.fail(child ? child->state.status() : VK_ERROR_INITIALIZATION_FAILED);
                        return;
                    }
                    entry->secondary.push_back(std::move(child));
                }
                entry->compute.logical_pipeline = VK_NULL_HANDLE;
                entry->compute.sets.clear();
                entry->compute.set_layouts.clear();
                entry->graphics.logical_pipeline = VK_NULL_HANDLE;
                entry->graphics.sets.clear();
                entry->graphics.set_layouts.clear();
            }
        } catch (const std::bad_alloc&) {
            fail_command(*state, cb, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
        }
    }
    native<PFN_vkCmdExecuteCommands>(*state, "vkCmdExecuteCommands")(cb, count, children);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(VkDevice handle, const VkDescriptorUpdateTemplateCreateInfo* info,
                                                              const VkAllocationCallbacks* allocator,
                                                              VkDescriptorUpdateTemplate* out) {
    auto state = device(handle);
    if (state->software)
        for (uint32_t i = 0; i < info->descriptorUpdateEntryCount; ++i)
            if (info->pDescriptorUpdateEntries[i].descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
                *out = VK_NULL_HANDLE;
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
    auto call = native<PFN_vkCreateDescriptorUpdateTemplate>(*state, "vkCreateDescriptorUpdateTemplate");
    if (!call) call = native<PFN_vkCreateDescriptorUpdateTemplate>(*state, "vkCreateDescriptorUpdateTemplateKHR");
    return call(handle, info, allocator, out);
}
VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice handle, uint32_t count, const VkFence* fences, VkBool32 all,
                                             uint64_t timeout) {
    auto state = device(handle);
    return state->submissions ? submissions_wait_for_fences(state->submissions, count, fences, all, timeout)
                              : native<PFN_vkWaitForFences>(*state, "vkWaitForFences")(handle, count, fences, all, timeout);
}
VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(VkDevice handle, VkFence fence) {
    auto state = device(handle);
    return state->submissions ? submissions_get_fence_status(state->submissions, fence)
                              : native<PFN_vkGetFenceStatus>(*state, "vkGetFenceStatus")(handle, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice handle, uint32_t count, const VkFence* fences) {
    auto state = device(handle);
    return state->submissions ? submissions_reset_fences(state->submissions, count, fences)
                              : native<PFN_vkResetFences>(*state, "vkResetFences")(handle, count, fences);
}
VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice handle, VkFence fence, const VkAllocationCallbacks* allocator) {
    auto state = device(handle);
    if (state->submissions) {
        auto result = submissions_destroy_fence(state->submissions, fence, allocator);
        if (result != VK_SUCCESS) fail_device(*state, result);
    } else
        native<PFN_vkDestroyFence>(*state, "vkDestroyFence")(handle, fence, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue) {
    auto state = device(queue);
    return state->submissions ? submissions_queue_wait_idle(state->submissions, queue)
                              : native<PFN_vkQueueWaitIdle>(*state, "vkQueueWaitIdle")(queue);
}
VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice handle) {
    auto state = device(handle);
    return state->submissions ? submissions_device_wait_idle(state->submissions)
                              : native<PFN_vkDeviceWaitIdle>(*state, "vkDeviceWaitIdle")(handle);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyAccelerationStructureKHR(VkCommandBuffer cb, const VkCopyAccelerationStructureInfoKHR* info) {
    auto state = device(cb);
    if (state->software) {
        fail_command(*state, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return;
    }
    native<PFN_vkCmdCopyAccelerationStructureKHR>(*state, "vkCmdCopyAccelerationStructureKHR")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyAccelerationStructureToMemoryKHR(VkCommandBuffer cb,
                                                                   const VkCopyAccelerationStructureToMemoryInfoKHR* info) {
    auto state = device(cb);
    if (state->software) {
        fail_command(*state, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return;
    }
    native<PFN_vkCmdCopyAccelerationStructureToMemoryKHR>(*state, "vkCmdCopyAccelerationStructureToMemoryKHR")(cb, info);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyMemoryToAccelerationStructureKHR(VkCommandBuffer cb,
                                                                   const VkCopyMemoryToAccelerationStructureInfoKHR* info) {
    auto state = device(cb);
    if (state->software) {
        fail_command(*state, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return;
    }
    native<PFN_vkCmdCopyMemoryToAccelerationStructureKHR>(*state, "vkCmdCopyMemoryToAccelerationStructureKHR")(cb, info);
}
VKAPI_ATTR VkResult VKAPI_CALL CopyAccelerationStructureKHR(VkDevice handle, VkDeferredOperationKHR deferred,
                                                            const VkCopyAccelerationStructureInfoKHR* info) {
    auto state = device(handle);
    return state->software
               ? VK_ERROR_FEATURE_NOT_PRESENT
               : native<PFN_vkCopyAccelerationStructureKHR>(*state, "vkCopyAccelerationStructureKHR")(handle, deferred, info);
}
VKAPI_ATTR VkResult VKAPI_CALL CopyAccelerationStructureToMemoryKHR(VkDevice handle, VkDeferredOperationKHR deferred,
                                                                    const VkCopyAccelerationStructureToMemoryInfoKHR* info) {
    auto state = device(handle);
    return state->software ? VK_ERROR_FEATURE_NOT_PRESENT
                           : native<PFN_vkCopyAccelerationStructureToMemoryKHR>(*state, "vkCopyAccelerationStructureToMemoryKHR")(
                                 handle, deferred, info);
}
VKAPI_ATTR VkResult VKAPI_CALL CopyMemoryToAccelerationStructureKHR(VkDevice handle, VkDeferredOperationKHR deferred,
                                                                    const VkCopyMemoryToAccelerationStructureInfoKHR* info) {
    auto state = device(handle);
    return state->software ? VK_ERROR_FEATURE_NOT_PRESENT
                           : native<PFN_vkCopyMemoryToAccelerationStructureKHR>(*state, "vkCopyMemoryToAccelerationStructureKHR")(
                                 handle, deferred, info);
}
VKAPI_ATTR void VKAPI_CALL GetDeviceAccelerationStructureCompatibilityKHR(VkDevice handle,
                                                                          const VkAccelerationStructureVersionInfoKHR* info,
                                                                          VkAccelerationStructureCompatibilityKHR* out) {
    auto state = device(handle);
    if (state->software)
        *out = VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR;
    else
        native<PFN_vkGetDeviceAccelerationStructureCompatibilityKHR>(*state, "vkGetDeviceAccelerationStructureCompatibilityKHR")(
            handle, info, out);
}
