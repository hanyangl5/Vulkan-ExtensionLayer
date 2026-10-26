/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_rtp.h"
#include "vkrt_rt_capabilities.h"

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
   cb->bound_compute_shader = VK_NULL_HANDLE;
   cb->compute_shader_object = false;
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
   if (point == VK_PIPELINE_BIND_POINT_COMPUTE) {
       cb->bound_compute_pipeline = pipeline;
       cb->bound_compute_shader = VK_NULL_HANDLE;
       cb->compute_shader_object = false;
   }
   cb->device_data->vtable.CmdBindPipeline(cmd, point, pipeline);
}

static VKAPI_ATTR void VKAPI_CALL CmdBindShadersEXT(VkCommandBuffer cmd, uint32_t count, const VkShaderStageFlagBits *stages,
                                                    const VkShaderEXT *shaders) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    if (!cb->device_data->vtable.CmdBindShadersEXT) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (stages[i] == VK_SHADER_STAGE_COMPUTE_BIT) {
            cb->bound_compute_pipeline = VK_NULL_HANDLE;
            cb->bound_compute_shader = shaders ? shaders[i] : VK_NULL_HANDLE;
            cb->compute_shader_object = true;
        }
    }
    cb->device_data->vtable.CmdBindShadersEXT(cmd, count, stages, shaders);
}

static VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout,
   VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *values)
{
   struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
   if (!cb) return;
   if (!cb->device_data->rtp) {
       cb->device_data->vtable.CmdPushConstants(cmd, layout, stages, offset, size, values);
       return;
   }
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
           p->bank = 0;
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

static VKAPI_ATTR void VKAPI_CALL CmdPushConstants2(VkCommandBuffer cmd, const VkPushConstantsInfo *info) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    struct vkrt_device_data *dev = cb->device_data;
    if (!dev->vtable.CmdPushConstants2) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    if (!dev->rtp) {
        dev->vtable.CmdPushConstants2(cmd, info);
        return;
    }

    const VkPipelineLayoutCreateInfo *inline_layout = NULL;
    uint32_t bank = 0;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO) inline_layout = (const VkPipelineLayoutCreateInfo *)p;
#ifdef VK_NV_push_constant_bank
        else if (p->sType == VK_STRUCTURE_TYPE_PUSH_CONSTANT_BANK_INFO_NV)
            bank = ((const VkPushConstantBankInfoNV *)p)->bank;
#endif
        else {
            cb->record_result = VK_ERROR_EXTENSION_NOT_PRESENT;
            return;
        }
    }
    /* Nonzero banks remain native state. The current typed RT system ABI
     * transports bank zero only, and must not alias an explicit RT bank. ALL
     * also names unrelated stages; updating another bank cannot change the
     * existing bank-zero RT snapshots. RT shaders using banks fail compilation. */
    if (bank && info->stageFlags != VK_SHADER_STAGE_ALL && (info->stageFlags & VKRT_RTP_SHADER_STAGES)) {
        cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }

    VkPipelineLayout layout = info->layout;
    PFN_vkDestroyPipelineLayout destroy = NULL;
    if (!layout) {
        if (!inline_layout) {
            cb->record_result = VK_ERROR_INITIALIZATION_FAILED;
            return;
        }
        VkPipelineLayoutCreateInfo create = *inline_layout;
        create.pNext = NULL;
        PFN_vkCreatePipelineLayout make = (PFN_vkCreatePipelineLayout)vkrt_rtp_proc("vkCreatePipelineLayout");
        destroy = (PFN_vkDestroyPipelineLayout)vkrt_rtp_proc("vkDestroyPipelineLayout");
        if (!make || !destroy) {
            cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
            return;
        }
        VkResult result = make(dev->device, &create, NULL, &layout);
        if (result != VK_SUCCESS) {
            cb->record_result = result;
            return;
        }
    }

    if (!bank) {
        CmdPushConstants(cmd, layout, info->stageFlags, info->offset, info->size, info->pValues);
    } else {
        /* Retain the compatible private layout even when the application used
         * an inline layout. Its public temporary is never recorded downstream. */
        vkrt_rtp_push(cb, layout, 0, 0, 0, NULL);
        VkPipelineLayout native = vkrt_rtp_native_layout(dev, layout);
        struct vkrt_saved_push *saved = malloc(sizeof(*saved) + info->size);
        if (!saved)
            cb->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        else if (cb->record_result != VK_SUCCESS)
            free(saved);
        else {
            saved->next = NULL;
            saved->layout = native;
            saved->stages = info->stageFlags;
            saved->offset = info->offset;
            saved->size = info->size;
            saved->bank = bank;
            memcpy(saved->bytes, info->pValues, info->size);
            if (cb->push_tail)
                cb->push_tail->next = saved;
            else
                cb->push_head = saved;
            cb->push_tail = saved;
            /* Replay uses the same normalized bank operation. */
#ifdef VK_NV_push_constant_bank
            VkPushConstantBankInfoNV extension = {VK_STRUCTURE_TYPE_PUSH_CONSTANT_BANK_INFO_NV, NULL, bank};
            VkPushConstantsInfo local = *info;
            local.layout = native;
            local.pNext = &extension;
            dev->vtable.CmdPushConstants2(cmd, &local);
#endif
        }
    }
    if (destroy) destroy(dev->device, layout, NULL);
}

PFN_vkVoidFunction vkrt_command_proc(const char *name)
{
#define ENTRY(fn) if (strcmp(name, "vk" #fn) == 0) return (PFN_vkVoidFunction)fn
   ENTRY(AllocateCommandBuffers); ENTRY(FreeCommandBuffers); ENTRY(DestroyCommandPool);
   ENTRY(ResetCommandPool); ENTRY(BeginCommandBuffer); ENTRY(ResetCommandBuffer);
   ENTRY(EndCommandBuffer);
   ENTRY(CmdBindPipeline);
   ENTRY(CmdBindShadersEXT);
   ENTRY(CmdPushConstants);
   ENTRY(CmdPushConstants2);
#undef ENTRY
   if (!strcmp(name, "vkCmdPushConstants2KHR")) return (PFN_vkVoidFunction)CmdPushConstants2;
   return NULL;
}
