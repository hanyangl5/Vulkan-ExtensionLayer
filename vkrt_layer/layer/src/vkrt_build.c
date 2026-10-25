/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_build_layout.h"
#include "bvh_ir_builder.h"
#include "vkrt_init.spv.h"
#include "leaf.spv.h"
#include "morton_sort.spv.h"
#include "lbvh_main.spv.h"
#include "lbvh_generate_ir.spv.h"

enum { INIT, LEAF, SORT, TREE, BOUNDS };

VkResult vkrt_build_init(struct vkrt_device_data *dev)
{
   VkPhysicalDeviceProperties properties;
   dev->instance_data->vtable.GetPhysicalDeviceProperties(dev->physical_device, &properties);
   dev->max_build_dispatch_x = properties.limits.maxComputeWorkGroupCount[0];
   VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
   VkPipelineLayoutCreateInfo layout = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
   VkResult r = dev->vtable.CreatePipelineLayout(dev->device, &layout, NULL, &dev->build_pipeline_layout);
   if (r != VK_SUCCESS) return r;
   const struct { const uint32_t *code; size_t size; } shaders[] = {
#define SHADER(name) {vkrt_spv_##name, sizeof(vkrt_spv_##name)}
      SHADER(vkrt_init), SHADER(leaf), SHADER(morton_sort), SHADER(lbvh_main), SHADER(lbvh_generate_ir)
#undef SHADER
   };
   /* 128 threads * 2 keys = 256 leaves per sorting workgroup. */
   uint32_t values[] = {64, VKRT_BOUNDS_OFFSET, 0, 0, 128, 2};
   VkSpecializationMapEntry entries[6];
   for (uint32_t i = 0; i < 6; ++i)
      entries[i] = (VkSpecializationMapEntry){i, i * 4, 4};
   VkSpecializationInfo spec = {6, entries, sizeof(values), values};
   for (uint32_t i = 0; i < VKRT_ARRAY_SIZE(shaders); ++i) {
      VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = shaders[i].size, .pCode = shaders[i].code};
      VkShaderModule sm;
      r = dev->vtable.CreateShaderModule(dev->device, &smci, NULL, &sm);
      if (r != VK_SUCCESS) return r;
      VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main", .pSpecializationInfo = &spec},
         .layout = dev->build_pipeline_layout};
      r = dev->vtable.CreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &ci, NULL, &dev->build_pipelines[i]);
      dev->vtable.DestroyShaderModule(dev->device, sm, NULL);
      if (r != VK_SUCCESS) return r;
   }
   return VK_SUCCESS;
}

void vkrt_build_finish(struct vkrt_device_data *dev)
{
   for (uint32_t i = 0; i < VKRT_ARRAY_SIZE(dev->build_pipelines); ++i)
      if (dev->build_pipelines[i]) dev->vtable.DestroyPipeline(dev->device, dev->build_pipelines[i], NULL);
   if (dev->build_pipeline_layout)
      dev->vtable.DestroyPipelineLayout(dev->device, dev->build_pipeline_layout, NULL);
}

static void build_barrier(struct vkrt_device_data *dev, VkCommandBuffer cmd)
{
   VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
   dev->vtable.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}

static void dispatch(struct vkrt_device_data *dev, VkCommandBuffer cmd,
   uint32_t pass, const void *args, uint32_t size, uint32_t groups)
{
   dev->vtable.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->build_pipelines[pass]);
   dev->vtable.CmdPushConstants(cmd, dev->build_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, args);
   dev->vtable.CmdDispatch(cmd, groups, 1, 1);
   build_barrier(dev, cmd);
}

struct vkrt_build_emit_context {
   struct vkrt_device_data *dev;
   VkCommandBuffer cmd;
};

static void vkrt_build_emit(void *user, uint32_t pass, const void *args,
                            uint32_t size, uint32_t groups)
{
   struct vkrt_build_emit_context *ctx = user;
   dispatch(ctx->dev, ctx->cmd, pass, args, size, groups);
}

void vkrt_cmd_build_as(struct vkrt_device_data *dev, VkCommandBuffer cmd, uint32_t count,
   const VkAccelerationStructureBuildGeometryInfoKHR *infos,
   const VkAccelerationStructureBuildRangeInfoKHR *const *ranges)
{
   struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, cmd);
   if (!cb || cb->record_result != VK_SUCCESS) return;
   build_barrier(dev, cmd);
   for (uint32_t i = 0; i < count; ++i) {
      const VkAccelerationStructureBuildGeometryInfoKHR *info = &infos[i];
      struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, info->dstAccelerationStructure);
      if (info->mode != VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR || !as ||
          as->owner != dev->device || !ranges ||
          !ranges[i] || !info->scratchData.deviceAddress) {
         VKRT_LOG_ERR("invalid or unsupported acceleration structure build");
         cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
         break;
      }
      struct vkrt_build_layout l = vkrt_build_layout(info, NULL, ranges[i]);
      for (uint32_t g = 0; g < info->geometryCount; ++g) {
         const VkAccelerationStructureGeometryKHR *geom = info->pGeometries ? &info->pGeometries[g] : info->ppGeometries[g];
         const VkAccelerationStructureGeometryKHR *first_geom = info->pGeometries ?
            &info->pGeometries[0] : info->ppGeometries[0];
         if (!bvh_ir_supported_geometry(geom) || geom->geometryType != first_geom->geometryType)
            cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
      }
      if (cb->record_result != VK_SUCCESS) {
         VKRT_LOG_ERR("unsupported geometry extension, format, index type or stride");
         break;
      }
      if (l.as_size > as->size || l.as_size >= UINT32_MAX || l.leaf_count >= (1u << 29) ||
          (l.leaf_count + 63) / 64 > dev->max_build_dispatch_x) {
         VKRT_LOG_ERR("BVH output too small or exceeds 32-bit node addressing");
         cb->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         break;
      }
      VkDeviceAddress address = vkrt_as_device_address(dev, info->dstAccelerationStructure);
      VkDeviceAddress scratch = info->scratchData.deviceAddress;
      if (!address) { cb->record_result = VK_ERROR_INITIALIZATION_FAILED; break; }
      struct vkrt_build_emit_context context = {dev, cmd};
      VkResult result = bvh_ir_record_build(info, ranges[i], address, scratch, 0,
                                            vkrt_build_emit, &context);
      if (result != VK_SUCCESS) { cb->record_result = result; break; }
   }
   /* No descriptors or graphics state are touched by the BDA-only passes. */
   if (cb->bound_compute_pipeline)
      dev->vtable.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cb->bound_compute_pipeline);
   for (struct vkrt_saved_push *p = cb->push_head; p; p = p->next)
      dev->vtable.CmdPushConstants(cmd, p->layout, p->stages, p->offset, p->size, p->bytes);
}
