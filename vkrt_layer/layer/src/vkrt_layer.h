/* SPDX-License-Identifier: MIT
 *
 * vkrt_layer.h - per-object layer state for the VKRT software ray-query layer.
 */

#ifndef VKRT_LAYER_H
#define VKRT_LAYER_H

#include "vkrt_common.h"
#include "vkrt_dispatch.h"

/* ------------------------------------------------------------------ */
/* Virtual acceleration structure (design doc section 7)               */
/* ------------------------------------------------------------------ */

struct vkrt_virtual_as {
   VkDevice                       owner;
   VkAccelerationStructureTypeKHR type;      /* BLAS / TLAS */
   VkBuffer                       backing_buffer; /* holds our BVHIR      */
   VkDeviceSize                   offset;
   VkDeviceSize                   size;
   VkDeviceAddress                header_address; /* &vk_ir_header        */
   VkDeviceAddress                node_address;
   VkDeviceAddress                leaf_address;

   uint32_t                       build_generation;

   /* Build inputs, captured at vkCmdBuildAccelerationStructuresKHR time
    * so the compute builder can read them by device address. */
   uint32_t                       geometry_count;
   uint32_t                       primitive_count;
   uint32_t                       instance_count;
};

/* ------------------------------------------------------------------ */
/* Instance / device layer data                                        */
/* ------------------------------------------------------------------ */

struct vkrt_instance_data {
   struct vkrt_instance_dispatch vtable;
   VkInstance                    instance;
   uint32_t                      api_version;
};

struct vkrt_device_data {
   struct vkrt_device_dispatch vtable;
   VkDevice                    device;
   VkPhysicalDevice            physical_device;
   struct vkrt_instance_data  *instance_data;

   /* Original application request, before emulated names are stripped. This
    * is not evidence that a native RT feature was enabled downstream. */
   bool app_requested_ray_query;
   VkPipelineLayout            build_pipeline_layout;
   VkPipeline                  build_pipelines[5];
   uint32_t                    max_build_dispatch_x;
   struct vkrt_cmd_buffer_data *command_buffers;
   vkrt_mutex_t                command_buffers_lock;
};

struct vkrt_saved_push {
   struct vkrt_saved_push *next;
   VkPipelineLayout layout;
   VkShaderStageFlags stages;
   uint32_t offset, size;
   uint8_t bytes[];
};

struct vkrt_cmd_buffer_data {
   VkCommandBuffer             cmd;
   struct vkrt_device_data    *device_data;
   VkCommandPool               pool;
   struct vkrt_cmd_buffer_data *next;
   struct vkrt_saved_push      *push_head, *push_tail;
   VkResult                    record_result;

   /* Saved application state so our builder binds can be undone. */
   VkPipeline                  bound_compute_pipeline;
};

/* ------------------------------------------------------------------ */
/* Shader-module metadata produced by the SPIR-V rewrite               */
/* ------------------------------------------------------------------ */

struct vkrt_shader_module_data {
   VkShaderModule              module;
   struct vkrt_device_data    *device_data;
   bool                        uses_ray_query;
   bool                        uses_ray_tracing_pipeline;
   /* Rewritten SPIR-V storage (heap; freed on destroy). */
   uint32_t                   *rewritten;
   size_t                      rewritten_words;
};

/* ------------------------------------------------------------------ */
/* Shared globals                                                      */
/* ------------------------------------------------------------------ */

extern struct vkrt_global vkrt_global;

/* vkrt_translate.c: host-side translations, also exercised without an ICD. */
VKAPI_ATTR VkResult VKAPI_CALL vkrt_CreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator, VkBuffer *buffer);
VKAPI_ATTR void VKAPI_CALL vkrt_UpdateDescriptorSets(VkDevice device,
   uint32_t writeCount, const VkWriteDescriptorSet *writes,
   uint32_t copyCount, const VkCopyDescriptorSet *copies);
VKAPI_ATTR VkResult VKAPI_CALL vkrt_AllocateMemory(VkDevice device,
   const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, VkDeviceMemory *memory);

/* vkrt_spirv.c */
VkResult vkrt_rewrite_shader_module(struct vkrt_device_data *dev,
                                    const VkShaderModuleCreateInfo *ci,
                                    VkShaderModuleCreateInfo *out_ci,
                                    bool *uses_ray_query,
                                    bool *uses_ray_tracing_pipeline);

/* vkrt_virtual_as.c */
VkResult vkrt_create_virtual_as(struct vkrt_device_data *dev,
                                const VkAccelerationStructureCreateInfoKHR *ci,
                                VkAccelerationStructureKHR *out_as);
void     vkrt_destroy_virtual_as(struct vkrt_device_data *dev,
                                 VkAccelerationStructureKHR as);
void     vkrt_as_build_sizes(struct vkrt_device_data *dev,
                             const VkAccelerationStructureBuildGeometryInfoKHR *pBuildInfo,
                             const uint32_t *pMaxPrimitiveCounts,
                             VkAccelerationStructureBuildSizesInfoKHR *pSizeInfo);
VkDeviceAddress vkrt_as_device_address(struct vkrt_device_data *dev,
                                       VkAccelerationStructureKHR as);

/* vkrt_build.c */
VkResult vkrt_create_device_with_features(struct vkrt_instance_data *inst,
   VkPhysicalDevice physical, PFN_vkCreateDevice create,
   const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *device);
PFN_vkVoidFunction vkrt_command_proc(const char *name);
void vkrt_commands_finish(struct vkrt_device_data *dev);
VkResult vkrt_build_init(struct vkrt_device_data *dev);
void vkrt_build_finish(struct vkrt_device_data *dev);
void vkrt_cmd_build_as(struct vkrt_device_data *dev,
                       VkCommandBuffer cmd,
                       uint32_t info_count,
                       const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
                       const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos);

/* vkrt_sync.c */
PFN_vkVoidFunction vkrt_sync_proc(const char *name);
/* vkrt_discovery.c: mask incompatible native RT options without exposing new
 * base support before the complete capability/implementation gate passes. */
PFN_vkVoidFunction vkrt_discovery_proc(const char *name);

#endif /* VKRT_LAYER_H */
