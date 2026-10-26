/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_rtp.h"

static void reset_state(struct vkrt_cmd_buffer_data *cb)
{
    vkrt_rtp_reset_command(cb);
    struct vkrt_saved_push *p = cb->push_head;
    while (p) {
        struct vkrt_saved_push *next = p->next;
        free(p);
        p = next;
    }
   cb->push_head = cb->push_tail = NULL;
   cb->bound_compute_pipeline = VK_NULL_HANDLE;
   cb->record_result = VK_SUCCESS;
}

/* Caller holds the per-device list lock. */
static void remove_cb(struct vkrt_device_data *dev, VkCommandBuffer cmd)
{
   struct vkrt_cmd_buffer_data **link = &dev->command_buffers;
   while (*link && (*link)->cmd != cmd) link = &(*link)->next;
   if (!*link) return;
   struct vkrt_cmd_buffer_data *cb = *link;
   *link = cb->next;
   vkrt_unmap_object(VKRT_HKEY(cmd));
   reset_state(cb);
   free(cb);
}

void vkrt_commands_finish(struct vkrt_device_data *dev)
{
   vkrt_mutex_lock(&dev->command_buffers_lock);
   while (dev->command_buffers) remove_cb(dev, dev->command_buffers->cmd);
   vkrt_mutex_unlock(&dev->command_buffers_lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice device,
   const VkCommandBufferAllocateInfo *info, VkCommandBuffer *commands)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
   VkResult r = dev->vtable.AllocateCommandBuffers(device, info, commands);
   if (r != VK_SUCCESS) return r;
   vkrt_mutex_lock(&dev->command_buffers_lock);
   uint32_t i;
   for (i = 0; i < info->commandBufferCount; ++i) {
      struct vkrt_cmd_buffer_data *cb = calloc(1, sizeof(*cb));
      if (!cb) break;
      cb->cmd = commands[i]; cb->device_data = dev; cb->pool = info->commandPool;
      cb->next = dev->command_buffers; dev->command_buffers = cb;
      vkrt_map_object(VKRT_HKEY(cb->cmd), cb);
   }
   if (i != info->commandBufferCount) {
      for (uint32_t j = 0; j < i; ++j) remove_cb(dev, commands[j]);
      dev->vtable.FreeCommandBuffers(device, info->commandPool, info->commandBufferCount, commands);
      r = VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   vkrt_mutex_unlock(&dev->command_buffers_lock);
   return r;
}

static VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice device, VkCommandPool pool,
   uint32_t count, const VkCommandBuffer *commands)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
   vkrt_mutex_lock(&dev->command_buffers_lock);
   for (uint32_t i = 0; i < count; ++i) remove_cb(dev, commands[i]);
   vkrt_mutex_unlock(&dev->command_buffers_lock);
   dev->vtable.FreeCommandBuffers(device, pool, count, commands);
}

static VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice device, VkCommandPool pool,
   const VkAllocationCallbacks *allocator)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
   vkrt_mutex_lock(&dev->command_buffers_lock);
   struct vkrt_cmd_buffer_data *cb = dev->command_buffers;
   while (cb) {
      struct vkrt_cmd_buffer_data *next = cb->next;
      if (cb->pool == pool) remove_cb(dev, cb->cmd);
      cb = next;
   }
   vkrt_mutex_unlock(&dev->command_buffers_lock);
   dev->vtable.DestroyCommandPool(device, pool, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice device, VkCommandPool pool,
   VkCommandPoolResetFlags flags)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
   VkResult r = dev->vtable.ResetCommandPool(device, pool, flags);
   if (r == VK_SUCCESS) {
      vkrt_mutex_lock(&dev->command_buffers_lock);
      for (struct vkrt_cmd_buffer_data *cb = dev->command_buffers; cb; cb = cb->next)
         if (cb->pool == pool) reset_state(cb);
      vkrt_mutex_unlock(&dev->command_buffers_lock);
   }
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info)
{
   struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
   if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = cb->device_data->vtable.BeginCommandBuffer(cmd, info);
   if (r == VK_SUCCESS) reset_state(cb);
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer cmd, VkCommandBufferResetFlags flags)
{
   struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
   if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = cb->device_data->vtable.ResetCommandBuffer(cmd, flags);
   if (r == VK_SUCCESS) reset_state(cb);
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer cmd)
{
   struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
   if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = cb->device_data->vtable.EndCommandBuffer(cmd);
   return cb->record_result != VK_SUCCESS ? cb->record_result : r;
}

static VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline)
{
   struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
   if (!cb) return;
   if (vkrt_rtp_bind_pipeline(cb, point, pipeline)) return;
   if (point == VK_PIPELINE_BIND_POINT_COMPUTE) cb->bound_compute_pipeline = pipeline;
   cb->device_data->vtable.CmdBindPipeline(cmd, point, pipeline);
}

static VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout,
   VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *values)
{
   struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
   if (!cb) return;
   stages = vkrt_rtp_push(cb, layout, stages, offset, size, values);
   if (!stages || cb->record_result != VK_SUCCESS) return;
   layout = vkrt_rtp_native_layout(cb->device_data, layout);
   /* Replay all affected pushes in order: a layout range may cover both
    * graphics and compute, and restoring such a push must not overwrite
    * a later graphics-only update with stale bytes. */
   {
       struct vkrt_saved_push *p = malloc(sizeof(*p) + size);
       if (!p)
           cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
       else {
           p->next = NULL;
           p->layout = layout;
           p->stages = stages;
           p->offset = offset;
           p->size = size;
           memcpy(p->bytes, values, size);
           if (cb->push_tail)
               cb->push_tail->next = p;
           else
               cb->push_head = p;
           cb->push_tail = p;
       }
   }
   cb->device_data->vtable.CmdPushConstants(cmd, layout, stages, offset, size, values);
}

PFN_vkVoidFunction vkrt_command_proc(const char *name)
{
#define ENTRY(fn) if (strcmp(name, "vk" #fn) == 0) return (PFN_vkVoidFunction)fn
   ENTRY(AllocateCommandBuffers); ENTRY(FreeCommandBuffers); ENTRY(DestroyCommandPool);
   ENTRY(ResetCommandPool); ENTRY(BeginCommandBuffer); ENTRY(ResetCommandBuffer);
   ENTRY(EndCommandBuffer); ENTRY(CmdBindPipeline); ENTRY(CmdPushConstants);
#undef ENTRY
   return NULL;
}
