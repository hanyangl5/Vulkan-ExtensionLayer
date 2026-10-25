/* SPDX-License-Identifier: MIT
 * Extend AS dependencies to the compute/storage accesses of the builder.
 * Original scopes are retained for other application operations.
 */
#include "vkrt_layer.h"

static VkPipelineStageFlags2 stages(VkPipelineStageFlags2 mask)
{
   return mask | ((mask & VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR) ?
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : 0);
}
static VkAccessFlags2 accesses(VkAccessFlags2 mask)
{
   if (mask & VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR) mask |= VK_ACCESS_2_SHADER_READ_BIT;
   if (mask & VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR) mask |= VK_ACCESS_2_SHADER_WRITE_BIT;
   return mask;
}
static void *copy_array(const void *src, size_t count, size_t size)
{
   if (!count || count > SIZE_MAX / size) return NULL;
   void *p = malloc(count * size);
   if (p) memcpy(p, src, count * size);
   return p;
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
#define FIX_ACCESS(p, n) for (uint32_t i = 0; i < (n); ++i) { \
      (p)[i].srcAccessMask = (VkAccessFlags)accesses((p)[i].srcAccessMask); \
      (p)[i].dstAccessMask = (VkAccessFlags)accesses((p)[i].dstAccessMask); }
      FIX_ACCESS(mc, nm); FIX_ACCESS(bc, nb); FIX_ACCESS(ic, ni);
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
   VkDependencyInfo local = *info;
   VkMemoryBarrier2 *m = copy_array(info->pMemoryBarriers, info->memoryBarrierCount, sizeof(*m));
   VkBufferMemoryBarrier2 *b = copy_array(info->pBufferMemoryBarriers, info->bufferMemoryBarrierCount, sizeof(*b));
   VkImageMemoryBarrier2 *im = copy_array(info->pImageMemoryBarriers, info->imageMemoryBarrierCount, sizeof(*im));
   if ((info->memoryBarrierCount && !m) || (info->bufferMemoryBarrierCount && !b) ||
       (info->imageMemoryBarrierCount && !im)) cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
   else if (!cb->device_data->vtable.CmdPipelineBarrier2) cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
   else {
#define FIX(p, n) for (uint32_t i = 0; i < (n); ++i) { \
      (p)[i].srcStageMask = stages((p)[i].srcStageMask); \
      (p)[i].dstStageMask = stages((p)[i].dstStageMask); \
      (p)[i].srcAccessMask = accesses((p)[i].srcAccessMask); \
      (p)[i].dstAccessMask = accesses((p)[i].dstAccessMask); }
      FIX(m, info->memoryBarrierCount); FIX(b, info->bufferMemoryBarrierCount); FIX(im, info->imageMemoryBarrierCount);
#undef FIX
      local.pMemoryBarriers = m; local.pBufferMemoryBarriers = b; local.pImageMemoryBarriers = im;
      cb->device_data->vtable.CmdPipelineBarrier2(cmd, &local);
   }
   free(m); free(b); free(im);
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
static VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count,
   const VkSubmitInfo *infos, VkFence fence)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, queue);
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   VkSubmitInfo *local = copy_array(infos, count, sizeof(*infos));
   if (count && !local) return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult result = VK_SUCCESS;
   uint32_t copied = 0;
   for (; copied < count; ++copied) {
      VkPipelineStageFlags *m = copy_array(infos[copied].pWaitDstStageMask,
         infos[copied].waitSemaphoreCount, sizeof(*m));
      if (infos[copied].waitSemaphoreCount && !m) { result = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
      local[copied].pWaitDstStageMask = m;
      for (uint32_t j = 0; j < infos[copied].waitSemaphoreCount; ++j) m[j] = (VkPipelineStageFlags)stages(m[j]);
   }
   if (result == VK_SUCCESS) result = dev->vtable.QueueSubmit(queue, count, local, fence);
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
   if (result == VK_SUCCESS) result = dev->vtable.QueueSubmit2(queue, count, local, fence);
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
   ENTRY(CmdPipelineBarrier); ENTRY(CmdPipelineBarrier2);
#undef ENTRY
   if (!strcmp(name, "vkQueueSubmit2KHR")) return (PFN_vkVoidFunction)QueueSubmit2;
   if (!strcmp(name, "vkCmdPipelineBarrier2KHR")) return (PFN_vkVoidFunction)CmdPipelineBarrier2;
   return NULL;
}
