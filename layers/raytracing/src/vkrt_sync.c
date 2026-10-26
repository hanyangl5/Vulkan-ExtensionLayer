/* SPDX-License-Identifier: MIT
 * Translate virtual AS/RT dependencies to the compute/storage accesses of the
 * builder and traversal. Unrelated application scopes are retained.
 */
#include "vkrt_layer.h"
#include "vkrt_chain.h"
#include "vkrt_query_pool.h"

static VkPipelineStageFlags2 stages(VkPipelineStageFlags2 mask)
{
    const VkPipelineStageFlags2 virtual_stages = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR;
    return (mask & ~virtual_stages) | ((mask & virtual_stages) ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : 0);
}
static VkAccessFlags2 accesses(VkAccessFlags2 mask, VkPipelineStageFlags2 original_stages) {
    if (mask & VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR) mask |= VK_ACCESS_2_SHADER_READ_BIT;
    if (mask & VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR) mask |= VK_ACCESS_2_SHADER_WRITE_BIT;
    const VkPipelineStageFlags2 serialized_stages = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                                    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR |
                                                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    if (original_stages & serialized_stages) {
        /* AS serialization exposes TRANSFER accesses at AS BUILD/COPY stages,
         * while this implementation executes shader buffer loads/stores. Mixed
         * native transfer scopes must still retain their original accesses. */
        if (mask & VK_ACCESS_2_TRANSFER_READ_BIT) mask |= VK_ACCESS_2_SHADER_READ_BIT;
        if (mask & VK_ACCESS_2_TRANSFER_WRITE_BIT) mask |= VK_ACCESS_2_SHADER_WRITE_BIT;
        const VkPipelineStageFlags2 native_transfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                                                      VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
                                                      VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        if (!(original_stages & native_transfer)) mask &= ~(VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    return mask & ~(VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
}
static void *copy_array(const void *src, size_t count, size_t size)
{
   if (!count || count > SIZE_MAX / size) return NULL;
   void *p = malloc(count * size);
   if (p) memcpy(p, src, count * size);
   return p;
}

struct dependency_copy {
    VkDependencyInfo info;
    VkMemoryBarrier2 *memory;
    VkBufferMemoryBarrier2 *buffers;
    VkImageMemoryBarrier2 *images;
};

static void free_dependency(struct dependency_copy *copy) {
    free(copy->memory);
    free(copy->buffers);
    free(copy->images);
}

static bool copy_dependency(const VkDependencyInfo *source, struct dependency_copy *copy) {
    memset(copy, 0, sizeof(*copy));
    copy->info = *source;
    copy->memory = copy_array(source->pMemoryBarriers, source->memoryBarrierCount, sizeof(*copy->memory));
    copy->buffers = copy_array(source->pBufferMemoryBarriers, source->bufferMemoryBarrierCount, sizeof(*copy->buffers));
    copy->images = copy_array(source->pImageMemoryBarriers, source->imageMemoryBarrierCount, sizeof(*copy->images));
    if ((source->memoryBarrierCount && !copy->memory) || (source->bufferMemoryBarrierCount && !copy->buffers) ||
        (source->imageMemoryBarrierCount && !copy->images))
        return false;
#define FIX(p, n)                                                                   \
    for (uint32_t i = 0; i < (n); ++i) {                                            \
        (p)[i].srcAccessMask = accesses((p)[i].srcAccessMask, (p)[i].srcStageMask); \
        (p)[i].dstAccessMask = accesses((p)[i].dstAccessMask, (p)[i].dstStageMask); \
        (p)[i].srcStageMask = stages((p)[i].srcStageMask);                          \
        (p)[i].dstStageMask = stages((p)[i].dstStageMask);                          \
    }
    FIX(copy->memory, source->memoryBarrierCount);
    FIX(copy->buffers, source->bufferMemoryBarrierCount);
    FIX(copy->images, source->imageMemoryBarrierCount);
#undef FIX
    copy->info.pMemoryBarriers = copy->memory;
    copy->info.pBufferMemoryBarriers = copy->buffers;
    copy->info.pImageMemoryBarriers = copy->images;
    return true;
}

static VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer cmd,
   VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
   uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
   uint32_t ni, const VkImageMemoryBarrier *im)
{
   struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
   if (!cb) return;
   VkMemoryBarrier *mc = copy_array(m, nm, sizeof(*m));
   VkBufferMemoryBarrier *bc = copy_array(b, nb, sizeof(*b));
   VkImageMemoryBarrier *ic = copy_array(im, ni, sizeof(*im));
   if ((nm && !mc) || (nb && !bc) || (ni && !ic)) cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
   else {
#define FIX_ACCESS(p, n)                                                          \
   for (uint32_t i = 0; i < (n); ++i) {                                           \
       (p)[i].srcAccessMask = (VkAccessFlags)accesses((p)[i].srcAccessMask, src); \
       (p)[i].dstAccessMask = (VkAccessFlags)accesses((p)[i].dstAccessMask, dst); \
   }
       FIX_ACCESS(mc, nm);
       FIX_ACCESS(bc, nb);
       FIX_ACCESS(ic, ni);
#undef FIX_ACCESS
      cb->device_data->vtable.CmdPipelineBarrier(cmd, (VkPipelineStageFlags)stages(src),
         (VkPipelineStageFlags)stages(dst), flags, nm, mc, nb, bc, ni, ic);
   }
   free(mc); free(bc); free(ic);
}

static VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo *info)
{
   struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
   if (!cb) return;
   if (!cb->device_data->vtable.CmdPipelineBarrier2) {
       cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
       return;
   }
   struct dependency_copy copy;
   if (!copy_dependency(info, &copy))
       cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
   else
       cb->device_data->vtable.CmdPipelineBarrier2(cmd, &copy.info);
   free_dependency(&copy);
}

static VKAPI_ATTR void VKAPI_CALL CmdSetEvent(VkCommandBuffer cmd, VkEvent event, VkPipelineStageFlags mask) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
    if (!cb) return;
    if (!cb->device_data->vtable.CmdSetEvent) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    cb->device_data->vtable.CmdSetEvent(cmd, event, (VkPipelineStageFlags)stages(mask));
}

static VKAPI_ATTR void VKAPI_CALL CmdResetEvent(VkCommandBuffer cmd, VkEvent event, VkPipelineStageFlags mask) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
    if (!cb) return;
    if (!cb->device_data->vtable.CmdResetEvent) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    cb->device_data->vtable.CmdResetEvent(cmd, event, (VkPipelineStageFlags)stages(mask));
}

static VKAPI_ATTR void VKAPI_CALL CmdWaitEvents(VkCommandBuffer cmd, uint32_t count, const VkEvent *events,
                                                VkPipelineStageFlags src, VkPipelineStageFlags dst, uint32_t nm,
                                                const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni,
                                                const VkImageMemoryBarrier *im) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
    if (!cb) return;
    if (!cb->device_data->vtable.CmdWaitEvents) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    VkMemoryBarrier *mc = copy_array(m, nm, sizeof(*m));
    VkBufferMemoryBarrier *bc = copy_array(b, nb, sizeof(*b));
    VkImageMemoryBarrier *ic = copy_array(im, ni, sizeof(*im));
    if ((nm && !mc) || (nb && !bc) || (ni && !ic))
        cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    else {
#define FIX(p, n)                                                                  \
    for (uint32_t i = 0; i < (n); ++i) {                                           \
        (p)[i].srcAccessMask = (VkAccessFlags)accesses((p)[i].srcAccessMask, src); \
        (p)[i].dstAccessMask = (VkAccessFlags)accesses((p)[i].dstAccessMask, dst); \
    }
        FIX(mc, nm);
        FIX(bc, nb);
        FIX(ic, ni);
#undef FIX
        cb->device_data->vtable.CmdWaitEvents(cmd, count, events, (VkPipelineStageFlags)stages(src),
                                              (VkPipelineStageFlags)stages(dst), nm, mc, nb, bc, ni, ic);
    }
    free(mc);
    free(bc);
    free(ic);
}

static VKAPI_ATTR void VKAPI_CALL CmdSetEvent2(VkCommandBuffer cmd, VkEvent event, const VkDependencyInfo *info) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
    if (!cb) return;
    if (!cb->device_data->vtable.CmdSetEvent2) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    struct dependency_copy copy;
    if (!copy_dependency(info, &copy))
        cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    else
        cb->device_data->vtable.CmdSetEvent2(cmd, event, &copy.info);
    free_dependency(&copy);
}

static VKAPI_ATTR void VKAPI_CALL CmdResetEvent2(VkCommandBuffer cmd, VkEvent event, VkPipelineStageFlags2 mask) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
    if (!cb) return;
    if (!cb->device_data->vtable.CmdResetEvent2) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    cb->device_data->vtable.CmdResetEvent2(cmd, event, stages(mask));
}

static VKAPI_ATTR void VKAPI_CALL CmdWaitEvents2(VkCommandBuffer cmd, uint32_t count, const VkEvent *events,
                                                 const VkDependencyInfo *infos) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
    if (!cb) return;
    if (!cb->device_data->vtable.CmdWaitEvents2) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    struct dependency_copy *copies = count ? calloc(count, sizeof(*copies)) : NULL;
    VkDependencyInfo *local = copy_array(infos, count, sizeof(*infos));
    if (count && (!copies || !local)) {
        cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        free(copies);
        free(local);
        return;
    }
    bool ok = true;
    for (uint32_t i = 0; i < count; ++i) {
        if (!copy_dependency(&infos[i], &copies[i])) {
            ok = false;
            break;
        }
        local[i] = copies[i].info;
    }
    if (ok)
        cb->device_data->vtable.CmdWaitEvents2(cmd, count, events, local);
    else
        cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < count; ++i) free_dependency(&copies[i]);
    free(copies);
    free(local);
}

static VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits mask, VkQueryPool pool,
                                                    uint32_t query) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
    if (!cb) return;
    if (!cb->device_data->vtable.CmdWriteTimestamp) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    cb->device_data->vtable.CmdWriteTimestamp(cmd, (VkPipelineStageFlagBits)stages(mask), pool, query);
}

static VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp2(VkCommandBuffer cmd, VkPipelineStageFlags2 mask, VkQueryPool pool,
                                                     uint32_t query) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
    if (!cb) return;
    if (!cb->device_data->vtable.CmdWriteTimestamp2) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    cb->device_data->vtable.CmdWriteTimestamp2(cmd, stages(mask), pool, query);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *info,
                                                       const VkAllocationCallbacks *allocator, VkRenderPass *pass) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (!dev->vtable.CreateRenderPass) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkRenderPassCreateInfo local = *info;
    VkSubpassDependency *deps = copy_array(info->pDependencies, info->dependencyCount, sizeof(*deps));
    if (info->dependencyCount && !deps) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        deps[i].srcAccessMask = (VkAccessFlags)accesses(deps[i].srcAccessMask, deps[i].srcStageMask);
        deps[i].dstAccessMask = (VkAccessFlags)accesses(deps[i].dstAccessMask, deps[i].dstStageMask);
        deps[i].srcStageMask = (VkPipelineStageFlags)stages(deps[i].srcStageMask);
        deps[i].dstStageMask = (VkPipelineStageFlags)stages(deps[i].dstStageMask);
    }
    local.pDependencies = deps;
    VkResult result = dev->vtable.CreateRenderPass(device, &local, allocator, pass);
    free(deps);
    return result;
}

struct barrier_override {
    VkBaseOutStructure *head;
    const void *suffix;
};
static void free_override(struct barrier_override *copy) {
    while (copy->head && (const void *)copy->head != copy->suffix) {
        VkBaseOutStructure *next = copy->head->pNext;
        free(copy->head);
        copy->head = next;
    }
}
static VkResult copy_override(const void *chain, struct barrier_override *copy, const void **out) {
    const VkBaseInStructure *existing = chain;
    while (existing && existing->sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER_2) existing = existing->pNext;
    *out = chain;
    if (!existing) return VK_SUCCESS;
    VkBaseOutStructure **tail = &copy->head;
    for (const VkBaseInStructure *p = chain;; p = p->pNext) {
        size_t size = p == existing ? sizeof(VkMemoryBarrier2) : vkrt_chain_size(p->sType);
        if (!size) return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkBaseOutStructure *node = malloc(size);
        if (!node) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(node, p, size);
        node->pNext = NULL;
        *tail = node;
        tail = &node->pNext;
        if (p == existing) {
            VkMemoryBarrier2 *barrier = (VkMemoryBarrier2 *)node;
            barrier->srcAccessMask = accesses(barrier->srcAccessMask, barrier->srcStageMask);
            barrier->dstAccessMask = accesses(barrier->dstAccessMask, barrier->dstStageMask);
            barrier->srcStageMask = stages(barrier->srcStageMask);
            barrier->dstStageMask = stages(barrier->dstStageMask);
            node->pNext = (void *)existing->pNext;
            copy->suffix = existing->pNext;
            break;
        }
    }
    *out = copy->head;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *info,
                                                        const VkAllocationCallbacks *allocator, VkRenderPass *pass) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (!dev->vtable.CreateRenderPass2) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkRenderPassCreateInfo2 local = *info;
    VkSubpassDependency2 *deps = copy_array(info->pDependencies, info->dependencyCount, sizeof(*deps));
    struct barrier_override *overrides = info->dependencyCount ? calloc(info->dependencyCount, sizeof(*overrides)) : NULL;
    if (info->dependencyCount && (!deps || !overrides)) {
        free(deps);
        free(overrides);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        deps[i].srcAccessMask = (VkAccessFlags)accesses(deps[i].srcAccessMask, deps[i].srcStageMask);
        deps[i].dstAccessMask = (VkAccessFlags)accesses(deps[i].dstAccessMask, deps[i].dstStageMask);
        deps[i].srcStageMask = (VkPipelineStageFlags)stages(deps[i].srcStageMask);
        deps[i].dstStageMask = (VkPipelineStageFlags)stages(deps[i].dstStageMask);
        // VkMemoryBarrier2 in a dependency's pNext overrides its legacy masks.
        // Clone only its known prefix and preserve the borrowed suffix unchanged.
        result = copy_override(deps[i].pNext, &overrides[i], &deps[i].pNext);
        if (result != VK_SUCCESS) goto done;
    }
    local.pDependencies = deps;
    result = dev->vtable.CreateRenderPass2(device, &local, allocator, pass);
done:
    for (uint32_t i = 0; i < info->dependencyCount; ++i) free_override(&overrides[i]);
    free(overrides);
    free(deps);
    return result;
}

/* Queues are device-owned aliases, removed by DestroyDevice. */
static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
   if (!dev) return;
   dev->vtable.GetDeviceQueue(device, family, index, queue);
   if (*queue) vkrt_map_object(VKRT_HKEY(*queue), dev);
}
static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
   if (!dev || !dev->vtable.GetDeviceQueue2) return;
   dev->vtable.GetDeviceQueue2(device, info, queue);
   if (*queue) vkrt_map_object(VKRT_HKEY(*queue), dev);
}
static VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, queue);
    if (!dev || !dev->vtable.QueueWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = dev->vtable.QueueWaitIdle(queue);
    if (result == VK_SUCCESS) vkrt_query_queue_idle(dev, queue);
    return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !dev->vtable.DeviceWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = dev->vtable.DeviceWaitIdle(device);
    if (result == VK_SUCCESS) vkrt_query_queue_idle(dev, VK_NULL_HANDLE);
    return result;
}
static bool query_wait_protects_compute(VkPipelineStageFlags2 mask) {
    return (stages(mask) & (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT |
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) != 0;
}
static VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count,
   const VkSubmitInfo *infos, VkFence fence)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, queue);
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   VkSubmitInfo *local = copy_array(infos, count, sizeof(*infos));
   if (count && !local) return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult result = VK_SUCCESS;
   struct vkrt_query_submission transaction = {0};
   uint32_t copied = 0;
   for (; copied < count; ++copied) {
      VkPipelineStageFlags *m = copy_array(infos[copied].pWaitDstStageMask,
         infos[copied].waitSemaphoreCount, sizeof(*m));
      if (infos[copied].waitSemaphoreCount && !m) { result = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
      local[copied].pWaitDstStageMask = m;
      for (uint32_t j = 0; j < infos[copied].waitSemaphoreCount; ++j) m[j] = (VkPipelineStageFlags)stages(m[j]);
   }
   for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i) {
       bool unsafe_wait = false;
       for (uint32_t j = 0; j < infos[i].waitSemaphoreCount; ++j)
           unsafe_wait |= !query_wait_protects_compute(infos[i].pWaitDstStageMask[j]);
       result =
           vkrt_query_submit_qualify(dev, &transaction, queue, unsafe_wait, infos[i].commandBufferCount, infos[i].pCommandBuffers);
   }
   if (result == VK_SUCCESS)
       for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i)
           result = vkrt_query_submit_prepare(dev, &transaction, infos[i].commandBufferCount, infos[i].pCommandBuffers);
   if (result == VK_SUCCESS) result = dev->vtable.QueueSubmit(queue, count, local, fence);
   vkrt_query_submit_result(dev, &transaction, result);
   for (uint32_t i = 0; i < copied; ++i) free((void *)local[i].pWaitDstStageMask);
   free(local);
   return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count,
   const VkSubmitInfo2 *infos, VkFence fence)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, queue);
   if (!dev || !dev->vtable.QueueSubmit2) return VK_ERROR_FEATURE_NOT_PRESENT;
   VkSubmitInfo2 *local = copy_array(infos, count, sizeof(*infos));
   if (count && !local) return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult result = VK_SUCCESS;
   struct vkrt_query_submission transaction = {0};
   uint32_t copied = 0;
   for (; copied < count; ++copied) {
      const VkSubmitInfo2 *s = &infos[copied];
      VkSemaphoreSubmitInfo *w = copy_array(s->pWaitSemaphoreInfos, s->waitSemaphoreInfoCount, sizeof(*w));
      VkSemaphoreSubmitInfo *sig = copy_array(s->pSignalSemaphoreInfos, s->signalSemaphoreInfoCount, sizeof(*sig));
      if ((s->waitSemaphoreInfoCount && !w) || (s->signalSemaphoreInfoCount && !sig)) {
         free(w); free(sig); result = VK_ERROR_OUT_OF_HOST_MEMORY; break;
      }
      for (uint32_t j = 0; j < s->waitSemaphoreInfoCount; ++j) w[j].stageMask = stages(w[j].stageMask);
      for (uint32_t j = 0; j < s->signalSemaphoreInfoCount; ++j) sig[j].stageMask = stages(sig[j].stageMask);
      local[copied].pWaitSemaphoreInfos = w; local[copied].pSignalSemaphoreInfos = sig;
   }
   for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i) {
       bool unsafe_wait = false;
       for (uint32_t j = 0; j < infos[i].waitSemaphoreInfoCount; ++j)
           unsafe_wait |= !query_wait_protects_compute(infos[i].pWaitSemaphoreInfos[j].stageMask);
       result = vkrt_query_submit_qualify(dev, &transaction, queue, unsafe_wait, 0, NULL);
       for (uint32_t j = 0; j < infos[i].commandBufferInfoCount && result == VK_SUCCESS; ++j)
           result = vkrt_query_submit_qualify(dev, &transaction, queue, false, 1, &infos[i].pCommandBufferInfos[j].commandBuffer);
   }
   if (result == VK_SUCCESS)
       for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i)
           for (uint32_t j = 0; j < infos[i].commandBufferInfoCount && result == VK_SUCCESS; ++j)
               result = vkrt_query_submit_prepare(dev, &transaction, 1, &infos[i].pCommandBufferInfos[j].commandBuffer);
   if (result == VK_SUCCESS) result = dev->vtable.QueueSubmit2(queue, count, local, fence);
   vkrt_query_submit_result(dev, &transaction, result);
   for (uint32_t i = 0; i < copied; ++i) {
      free((void *)local[i].pWaitSemaphoreInfos); free((void *)local[i].pSignalSemaphoreInfos);
   }
   free(local);
   return result;
}
PFN_vkVoidFunction vkrt_sync_proc(const char *name)
{
#define ENTRY(fn) if (!strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction)fn
   ENTRY(GetDeviceQueue); ENTRY(GetDeviceQueue2); ENTRY(QueueSubmit); ENTRY(QueueSubmit2);
   ENTRY(QueueWaitIdle);
   ENTRY(DeviceWaitIdle);
   ENTRY(CmdPipelineBarrier); ENTRY(CmdPipelineBarrier2);
   ENTRY(CmdSetEvent);
   ENTRY(CmdResetEvent);
   ENTRY(CmdWaitEvents);
   ENTRY(CmdSetEvent2);
   ENTRY(CmdResetEvent2);
   ENTRY(CmdWaitEvents2);
   ENTRY(CmdWriteTimestamp);
   ENTRY(CmdWriteTimestamp2);
   ENTRY(CreateRenderPass);
   ENTRY(CreateRenderPass2);
#undef ENTRY
   if (!strcmp(name, "vkQueueSubmit2KHR")) return (PFN_vkVoidFunction)QueueSubmit2;
   if (!strcmp(name, "vkCmdPipelineBarrier2KHR")) return (PFN_vkVoidFunction)CmdPipelineBarrier2;
   if (!strcmp(name, "vkCmdSetEvent2KHR")) return (PFN_vkVoidFunction)CmdSetEvent2;
   if (!strcmp(name, "vkCmdResetEvent2KHR")) return (PFN_vkVoidFunction)CmdResetEvent2;
   if (!strcmp(name, "vkCmdWaitEvents2KHR")) return (PFN_vkVoidFunction)CmdWaitEvents2;
   if (!strcmp(name, "vkCmdWriteTimestamp2KHR")) return (PFN_vkVoidFunction)CmdWriteTimestamp2;
   if (!strcmp(name, "vkCreateRenderPass2KHR")) return (PFN_vkVoidFunction)CreateRenderPass2;
   return NULL;
}
