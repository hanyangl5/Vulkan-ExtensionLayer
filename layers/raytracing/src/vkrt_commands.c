/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_rtp.h"
#include "vkrt_rt_capabilities.h"
#include "vkrt_query_pool.h"
#include "vkrt_execution_replay.h"

static PFN_vkVoidFunction replay_resolve(void *user, const char *name) {
    struct vkrt_cmd_buffer_data *cb = user;
    return vkrt_device_direct_proc(cb->device_data, name);
}
static VkResult replay_capture_layout(void *user, VkPipelineLayout source, VkPipelineLayout *stable) {
    struct vkrt_cmd_buffer_data *cb = user;
    if (!source) {
        *stable = VK_NULL_HANDLE;
        return VK_SUCCESS;
    }
    return vkrt_rtp_replay_layout_capture(cb, source, stable);
}
static VkResult replay_emit_layout(void *user, const char *name, VkPipelineLayout identity, VkPipelineLayout *forward) {
    struct vkrt_cmd_buffer_data *cb = user;
    *forward = identity;
    if (!identity) return VK_SUCCESS;
    /* The translating hooks require the immutable original definition. Native
     * consumers require a genuine retained ICD layout. Aliases keep their name. */
    if (!strcmp(name, "vkCmdBindDescriptorSets") || !strcmp(name, "vkCmdBindDescriptorSets2") ||
        !strcmp(name, "vkCmdBindDescriptorSets2KHR") || !strcmp(name, "vkCmdPushConstants") ||
        !strcmp(name, "vkCmdPushConstants2") || !strcmp(name, "vkCmdPushConstants2KHR"))
        return VK_SUCCESS;
    return vkrt_rtp_replay_layout_native(cb, identity, forward);
}
struct vkrt_replay_context *vkrt_replay_lookup_context(VkCommandBuffer command) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (!cb) return NULL;
    /* Host adapters may construct command state directly. Production initializes
     * the same callbacks at allocation; never obtain a recorder recursively. */
    cb->replay.resolve = replay_resolve;
    cb->replay.user = cb;
    cb->replay.normalize_pipeline_layout = replay_capture_layout;
    cb->replay.emit_pipeline_layout = replay_emit_layout;
    return &cb->replay;
}

static void reset_state(struct vkrt_cmd_buffer_data *cb)
{
    vkrt_replay_journal_reset(cb->replay.journal);
    VkResult query_result = vkrt_query_command_reset(cb);
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
   cb->record_result = query_result;
   cb->native_active_queries = 0;
   cb->usage_flags = 0;
   cb->has_virtual_queries = false;
   cb->has_rtp_execution = false;
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
   vkrt_replay_journal_destroy(cb->replay.journal);
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
      cb->level = info->level;
      if (dev->executions) {
          const VkAllocationCallbacks *allocator = dev->queue_families_have_allocator ? &dev->queue_families_allocator : NULL;
          if (!allocator && dev->instance_data && dev->instance_data->has_allocator) allocator = &dev->instance_data->allocator;
          cb->replay.journal = vkrt_replay_journal_create(allocator, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
          if (!cb->replay.journal) {
              free(cb);
              break;
          }
      }
      cb->next = dev->command_buffers; dev->command_buffers = cb;
      vkrt_map_object(VKRT_HKEY(cb->cmd), cb);
      vkrt_replay_lookup_context(cb->cmd);
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

static VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *info,
                                                        const VkAllocationCallbacks *allocator, VkCommandPool *pool) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = dev->vtable.CreateCommandPool(device, info, allocator, pool);
    if (result != VK_SUCCESS) return result;
    result = vkrt_execution_track_command_pool(dev, *pool, info->queueFamilyIndex, info->flags);
    if (result != VK_SUCCESS) {
        /* No record was acquired. Do not forget a replacement pool whose
         * native handle is recycled while this rollback destroys the old one. */
        dev->vtable.DestroyCommandPool(device, *pool, allocator);
        *pool = VK_NULL_HANDLE;
    }
    return result;
}
static VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *allocator) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    vkrt_mutex_lock(&dev->command_buffers_lock);
    struct vkrt_cmd_buffer_data *cb = dev->command_buffers;
    while (cb) {
        struct vkrt_cmd_buffer_data *next = cb->next;
        if (cb->pool == pool) remove_cb(dev, cb->cmd);
        cb = next;
    }
    vkrt_mutex_unlock(&dev->command_buffers_lock);
    /* The native call can release this handle for reuse by another thread.
     * Remove its old metadata before a new pool can acquire the same value. */
    vkrt_execution_forget_command_pool(dev, pool);
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
   if (r == VK_SUCCESS) {
       reset_state(cb);
       cb->usage_flags = info->flags;
       /* A future replay submit must inspect this status. Ordinary recording
        * keeps the native API result when a schema cannot yet be captured. */
       if (cb->replay.journal) {
           VkCommandBufferBeginInfo captured = *info;
           /* This pointer is ignored for primaries and need not identify valid
            * memory. Only the native call receives the original ignored bits. */
           if (cb->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) captured.pInheritanceInfo = NULL;
           vkrt_replay_journal_begin(cb->replay.journal, &captured);
       }
   }
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
   if (vkrt_execution_command_requires_replay(cb) && cb->record_result == VK_SUCCESS) {
       if (cb->replay.journal)
           cb->record_result = vkrt_replay_journal_status(cb->replay.journal);
       else if (cb->device_data->executions || cb->has_rtp_execution)
           cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
   }
   return cb->record_result != VK_SUCCESS ? cb->record_result : r;
}
static VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer command, uint32_t count, const VkCommandBuffer *children) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    for (uint32_t i = 0; i < count; ++i) {
        struct vkrt_cmd_buffer_data *child = VKRT_FIND(struct vkrt_cmd_buffer_data, children[i]);
        if (child && child->device_data == cb->device_data) cb->has_rtp_execution |= child->has_rtp_execution;
    }
    if (vkrt_query_record_children(cb, count, children)) cb->device_data->vtable.CmdExecuteCommands(command, count, children);
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
   layout = vkrt_rtp_command_native_layout(cb, layout);
   if (cb->record_result != VK_SUCCESS) return;
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
    VkPipelineLayout temporary_layout = VK_NULL_HANDLE;
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
        temporary_layout = layout;
        if (cb->replaying) {
            result = vkrt_rtp_replay_layout_capture(cb, temporary_layout, &layout);
            if (result != VK_SUCCESS) {
                destroy(dev->device, temporary_layout, NULL);
                cb->record_result = result;
                return;
            }
        }
    }

    if (!bank) {
        CmdPushConstants(cmd, layout, info->stageFlags, info->offset, info->size, info->pValues);
    } else {
        /* Retain the compatible private layout even when the application used
         * an inline layout. Its public temporary is never recorded downstream. */
        vkrt_rtp_push(cb, layout, 0, 0, 0, NULL);
        VkPipelineLayout native = vkrt_rtp_command_native_layout(cb, layout);
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
    if (destroy) destroy(dev->device, temporary_layout, NULL);
}

PFN_vkVoidFunction vkrt_command_proc(const char *name)
{
#define ENTRY(fn) if (strcmp(name, "vk" #fn) == 0) return (PFN_vkVoidFunction)fn
    ENTRY(AllocateCommandBuffers);
    ENTRY(FreeCommandBuffers);
    ENTRY(CreateCommandPool);
    ENTRY(DestroyCommandPool);
    ENTRY(ResetCommandPool);
    ENTRY(BeginCommandBuffer);
    ENTRY(ResetCommandBuffer);
    ENTRY(EndCommandBuffer);
    ENTRY(CmdExecuteCommands);
    ENTRY(CmdBindPipeline);
    ENTRY(CmdBindShadersEXT);
    ENTRY(CmdPushConstants);
    ENTRY(CmdPushConstants2);
#undef ENTRY
   if (!strcmp(name, "vkCmdPushConstants2KHR")) return (PFN_vkVoidFunction)CmdPushConstants2;
   return NULL;
}
