/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_as_registry.h"
#include "vkrt_build_layout.h"
#include "bvh_ir_builder.h"
#include "vkrt_init.spv.h"
#include "leaf_bvh4.spv.h"
#include "morton_sort.spv.h"
#include "lbvh_main.spv.h"
#include "lbvh_generate_ir.spv.h"
#include "vkrt_bvh4_prepare.spv.h"
#include "bvh4_mesa_encode.spv.h"

enum { INIT, LEAF, SORT, TREE, BOUNDS, PREPARE, ENCODE_0, ENCODE_1, ENCODE_2 };

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
   const struct {
       const uint32_t *code;
       size_t size;
   } shaders[] = {
#define SHADER(name) {vkrt_spv_##name, sizeof(vkrt_spv_##name)}
       SHADER(vkrt_init),        SHADER(leaf_bvh4),         SHADER(morton_sort),      SHADER(lbvh_main),
       SHADER(lbvh_generate_ir), SHADER(vkrt_bvh4_prepare), SHADER(bvh4_mesa_encode), SHADER(bvh4_mesa_encode),
       SHADER(bvh4_mesa_encode)
#undef SHADER
   };
   /* 128 threads * 2 keys = 256 leaves per sorting workgroup. */
   struct {
       uint32_t values[6];
       uint64_t context;
   } values = {{64, VKRT_BOUNDS_OFFSET, 0, 0, 128, 2}, vkrt_as_registry_address(dev)};
   VkSpecializationMapEntry entries[7];
   for (uint32_t i = 0; i < 6; ++i)
      entries[i] = (VkSpecializationMapEntry){i, i * 4, 4};
   entries[6] = (VkSpecializationMapEntry){6, 24, 8};
   VkSpecializationInfo spec = {7, entries, sizeof(values), &values};
   for (uint32_t i = 0; i < VKRT_ARRAY_SIZE(shaders); ++i) {
       struct {
           uint32_t phase, padding;
           uint64_t context;
       } encode_values = {i >= ENCODE_0 ? i - ENCODE_0 : 0, 0, vkrt_as_registry_address(dev)};
       const VkSpecializationMapEntry encode_entries[] = {{0, 0, 4}, {6, 8, 8}};
       VkSpecializationInfo encode_spec = {2, encode_entries, sizeof(encode_values), &encode_values};
       VkShaderModuleCreateInfo smci = {
           .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = shaders[i].size, .pCode = shaders[i].code};
       VkShaderModule sm;
       r = dev->vtable.CreateShaderModule(dev->device, &smci, NULL, &sm);
       if (r != VK_SUCCESS) return r;
       VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                   .module = sm,
                                                   .pName = "main",
                                                   .pSpecializationInfo = i >= ENCODE_0 ? &encode_spec : &spec},
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
      bool update = info->mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
      struct vkrt_virtual_as *source = update ? VKRT_FIND(struct vkrt_virtual_as, info->srcAccelerationStructure) : NULL;
      if ((!update && info->mode != VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR) || !as || as->owner != dev->device || !ranges ||
          !ranges[i] || !info->scratchData.deviceAddress ||
          (update &&
           (!source || source->owner != dev->device || !(info->flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) ||
            (source->type != VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR && source->type != info->type) ||
            (as->type != VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR && as->type != info->type)))) {
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
      if (!l.valid || l.as_size > as->size || l.ir.as_size >= UINT32_MAX || l.leaf_count >= (1u << 29) ||
          (l.leaf_count + 63) / 64 > dev->max_build_dispatch_x) {
          VKRT_LOG_ERR("BVH4 output too small or Mesa construction exceeds its current ID capacity");
          cb->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
          break;
      }
      VkDeviceAddress address = vkrt_as_backing_address(dev, info->dstAccelerationStructure);
      VkDeviceAddress scratch = info->scratchData.deviceAddress;
      if (!address) { cb->record_result = VK_ERROR_INITIALIZATION_FAILED; break; }
      if (address > UINT64_MAX - l.as_size || scratch > UINT64_MAX - l.scratch_size) {
          cb->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
          break;
      }
      uint32_t geometry_type = info->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR ? 2u : 0u;
      if (info->geometryCount) geometry_type = (info->pGeometries ? &info->pGeometries[0] : info->ppGeometries[0])->geometryType;
      struct vkrt_bvh4_prepare_args prepare = {address,
                                               scratch + l.status,
                                               scratch + l.geometry_flags,
                                               l.nodes,
                                               l.triangles,
                                               l.aabbs,
                                               l.instances,
                                               l.parents,
                                               l.as_size,
                                               info->type,
                                               0,
                                               0,
                                               0};
      dispatch(dev, cmd, PREPARE, &prepare, sizeof(prepare), 1);
      prepare.mode = 1;
      for (uint32_t g = 0; g < info->geometryCount; ++g) {
          const VkAccelerationStructureGeometryKHR *geometry = info->pGeometries ? &info->pGeometries[g] : info->ppGeometries[g];
          prepare.geometry = g;
          prepare.flags = geometry->flags;
          dispatch(dev, cmd, PREPARE, &prepare, sizeof(prepare), 1);
      }
      struct vkrt_build_emit_context context = {dev, cmd};
      /* UPDATE supplies the complete geometry description. Rebuild the shared
       * representation on the GPU at the destination's unchanged physical
       * header base. No source AS bytes are written for an out-of-place update,
       * and existing TLAS references remain valid for an in-place BLAS update.
       * Topology need not be preserved by Vulkan. This is not a refit path. */
      VkAccelerationStructureBuildGeometryInfoKHR build = *info;
      build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      VkResult result = bvh_ir_record_build(&build, ranges[i], scratch + l.source, scratch, 0, vkrt_build_emit, &context);
      if (result != VK_SUCCESS) { cb->record_result = result; break; }
      VelRtMesaGpuEncodeArgs encode = {scratch + l.source,
                                       l.ir.as_size,
                                       address,
                                       scratch + l.mapping,
                                       scratch + l.geometry_flags,
                                       0,
                                       scratch + l.status,
                                       0,
                                       (uint32_t)l.leaf_count,
                                       geometry_type,
                                       info->geometryCount,
                                       VEL_RT_MESA_GPU_IDENTITY_BLAS | VEL_RT_MESA_GPU_PUBLIC_BLAS};
      for (uint32_t phase = ENCODE_0; phase <= ENCODE_2; ++phase) {
          uint64_t invocations = l.leaf_count ? l.leaf_count : 1;
          uint64_t chunk = (uint64_t)dev->max_build_dispatch_x * 64;
          for (encode.first = 0; encode.first < invocations; encode.first += chunk) {
              uint64_t remaining = invocations - encode.first;
              uint32_t groups = (uint32_t)((remaining < chunk ? remaining : chunk) + 63) / 64;
              dispatch(dev, cmd, phase, &encode, sizeof(encode), groups);
          }
      }
   }
   /* No descriptors or graphics state are touched by the BDA-only passes. */
   vkrt_replay_compute_binding(cb);
   vkrt_replay_push_constants(cb);
}
