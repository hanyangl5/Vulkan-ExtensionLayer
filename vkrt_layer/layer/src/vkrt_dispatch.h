/* SPDX-License-Identifier: MIT
 *
 * vkrt_dispatch.h - hand-rolled dispatch tables for the VKRT layer.
 *
 * Mirrors the DECLARE_HOOK / INIT_HOOK pattern used by Mesa's
 * anti-lag-layer, but self-contained (no idep_vulkan_util).
 */

#ifndef VKRT_DISPATCH_H
#define VKRT_DISPATCH_H

#include "vkrt_common.h"

/* ------------------------------------------------------------------ */
/* Instance / physical-device table                                    */
/* ------------------------------------------------------------------ */

struct vkrt_instance_dispatch {
   PFN_vkGetInstanceProcAddr               GetInstanceProcAddr;
   PFN_vkDestroyInstance                   DestroyInstance;
   PFN_vkEnumeratePhysicalDevices          EnumeratePhysicalDevices;
   PFN_vkGetPhysicalDeviceProperties       GetPhysicalDeviceProperties;
   PFN_vkGetPhysicalDeviceProperties2      GetPhysicalDeviceProperties2;
   PFN_vkGetPhysicalDeviceFeatures2        GetPhysicalDeviceFeatures2;
   PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
   PFN_vkCreateDevice                      CreateDevice;
};

/* ------------------------------------------------------------------ */
/* Device table                                                        */
/* ------------------------------------------------------------------ */

struct vkrt_device_dispatch {
   PFN_vkGetDeviceProcAddr                  GetDeviceProcAddr;
   PFN_vkDestroyDevice                      DestroyDevice;
   PFN_vkGetDeviceQueue                     GetDeviceQueue;
   PFN_vkGetDeviceQueue2                    GetDeviceQueue2;
   PFN_vkQueueSubmit                        QueueSubmit;
   PFN_vkQueueSubmit2                       QueueSubmit2;

   /* Pass-through / usage-augmentation */
   PFN_vkCreateBuffer                       CreateBuffer;
   PFN_vkAllocateMemory                     AllocateMemory;
   PFN_vkCreatePipelineLayout               CreatePipelineLayout;
   PFN_vkCreateShaderModule                 CreateShaderModule;
   PFN_vkUpdateDescriptorSets               UpdateDescriptorSets;
   PFN_vkCreateDescriptorSetLayout          CreateDescriptorSetLayout;
   PFN_vkCreateDescriptorPool              CreateDescriptorPool;
   PFN_vkCreateGraphicsPipelines            CreateGraphicsPipelines;
   PFN_vkCreateComputePipelines             CreateComputePipelines;

   /* Command buffer */
   PFN_vkAllocateCommandBuffers             AllocateCommandBuffers;
   PFN_vkFreeCommandBuffers                 FreeCommandBuffers;
   PFN_vkResetCommandBuffer                 ResetCommandBuffer;
   PFN_vkResetCommandPool                   ResetCommandPool;
   PFN_vkDestroyCommandPool                 DestroyCommandPool;
   PFN_vkBeginCommandBuffer                 BeginCommandBuffer;
   PFN_vkEndCommandBuffer                   EndCommandBuffer;
   PFN_vkCmdBindPipeline                    CmdBindPipeline;
   PFN_vkCmdBindDescriptorSets              CmdBindDescriptorSets;
   PFN_vkCmdPushConstants                   CmdPushConstants;
   PFN_vkCmdPipelineBarrier                 CmdPipelineBarrier;
   PFN_vkCmdPipelineBarrier2                CmdPipelineBarrier2;
   PFN_vkCmdBindVertexBuffers               CmdBindVertexBuffers;
   PFN_vkCmdBindIndexBuffer                 CmdBindIndexBuffer;
   PFN_vkCmdSetViewport                     CmdSetViewport;
   PFN_vkCmdSetScissor                      CmdSetScissor;
   PFN_vkCmdDispatch                        CmdDispatch;
   PFN_vkCmdDraw                            CmdDraw;
   PFN_vkCmdBeginRenderPass                 CmdBeginRenderPass;
   PFN_vkCmdBeginRenderPass2                CmdBeginRenderPass2;
   PFN_vkCmdEndRenderPass                   CmdEndRenderPass;
   PFN_vkCmdEndRenderPass2                  CmdEndRenderPass2;
   PFN_vkCmdSetBlendConstants               CmdSetBlendConstants;
   PFN_vkCmdSetDepthBias                    CmdSetDepthBias;
   PFN_vkCmdSetDepthBounds                  CmdSetDepthBounds;
   PFN_vkCmdSetStencilCompareMask           CmdSetStencilCompareMask;
   PFN_vkCmdSetStencilWriteMask             CmdSetStencilWriteMask;
   PFN_vkCmdSetStencilReference             CmdSetStencilReference;
   PFN_vkCmdSetLineWidth                    CmdSetLineWidth;
   PFN_vkCmdSetDepthTestEnable              CmdSetDepthTestEnable;
   PFN_vkCmdSetDepthWriteEnable             CmdSetDepthWriteEnable;
   PFN_vkCmdSetDepthCompareOp               CmdSetDepthCompareOp;
   PFN_vkCmdSetStencilTestEnable            CmdSetStencilTestEnable;
   PFN_vkCmdSetStencilOp                    CmdSetStencilOp;
   PFN_vkCmdSetCullMode                     CmdSetCullMode;
   PFN_vkCmdSetFrontFace                    CmdSetFrontFace;
   PFN_vkCmdSetPrimitiveTopology            CmdSetPrimitiveTopology;
   PFN_vkCmdSetViewportWithCount            CmdSetViewportWithCount;
   PFN_vkCmdSetScissorWithCount             CmdSetScissorWithCount;
   PFN_vkCmdSetDepthBiasEnable              CmdSetDepthBiasEnable;
   PFN_vkCmdSetPrimitiveRestartEnable       CmdSetPrimitiveRestartEnable;
   PFN_vkCmdSetRasterizerDiscardEnable      CmdSetRasterizerDiscardEnable;

   /* Memory / resource */
   PFN_vkGetBufferDeviceAddress             GetBufferDeviceAddress;
   PFN_vkGetBufferMemoryRequirements        GetBufferMemoryRequirements;
   PFN_vkBindBufferMemory                   BindBufferMemory;

   /* Acceleration structure (virtualised in bootstrap mode these are
    * forwarded to the ICD only for object bookkeeping; the actual build
    * is replaced by our compute passes). */
   PFN_vkCreateAccelerationStructureKHR     CreateAccelerationStructureKHR;
   PFN_vkDestroyAccelerationStructureKHR    DestroyAccelerationStructureKHR;
   PFN_vkGetAccelerationStructureBuildSizesKHR GetAccelerationStructureBuildSizesKHR;
   PFN_vkGetAccelerationStructureDeviceAddressKHR GetAccelerationStructureDeviceAddressKHR;
   PFN_vkCmdBuildAccelerationStructuresKHR  CmdBuildAccelerationStructuresKHR;
   PFN_vkCreateRayTracingPipelinesKHR       CreateRayTracingPipelinesKHR;
   PFN_vkCmdTraceRaysKHR                    CmdTraceRaysKHR;

   /* For our internal compute pipelines (builder passes). */
   PFN_vkCreateComputePipelines             CreateComputePipelinesInternal;
   PFN_vkDestroyPipeline                    DestroyPipeline;
   PFN_vkDestroyShaderModule                DestroyShaderModule;
   PFN_vkDestroyPipelineLayout              DestroyPipelineLayout;
   PFN_vkDestroyDescriptorSetLayout         DestroyDescriptorSetLayout;
   PFN_vkFreeMemory                         FreeMemory;
   PFN_vkDestroyBuffer                      DestroyBuffer;
   PFN_vkCreateSampler                      CreateSampler;
};

/* Load every entry we might call. Missing pointers stay NULL and are
 * checked at use site -- the layer must not assume a 1.3 device. */
static inline void
vkrt_instance_dispatch_load(struct vkrt_instance_dispatch *t,
                            PFN_vkGetInstanceProcAddr gipa,
                            VkInstance instance)
{
#define VKRT_LOAD_I(name)                                                      \
   t->name = (PFN_vk##name)gipa(instance, "vk" #name)
   VKRT_LOAD_I(GetInstanceProcAddr);
   VKRT_LOAD_I(DestroyInstance);
   VKRT_LOAD_I(EnumeratePhysicalDevices);
   VKRT_LOAD_I(GetPhysicalDeviceProperties);
   VKRT_LOAD_I(GetPhysicalDeviceProperties2);
   VKRT_LOAD_I(GetPhysicalDeviceFeatures2);
   if (!t->GetPhysicalDeviceFeatures2)
      t->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(instance, "vkGetPhysicalDeviceFeatures2KHR");
   VKRT_LOAD_I(GetPhysicalDeviceMemoryProperties);
   VKRT_LOAD_I(CreateDevice);
#undef VKRT_LOAD_I
}

static inline void
vkrt_device_dispatch_load(struct vkrt_device_dispatch *t,
                          PFN_vkGetDeviceProcAddr gdpa,
                          VkDevice device)
{
#define VKRT_LOAD_D(name) t->name = (PFN_vk##name)gdpa(device, "vk" #name)
   /* The buffer-device-address entry is exposed under both the core and
    * the KHR name, and only the KHR one is guaranteed to resolve on a
    * device enabled through VK_KHR_buffer_device_address. */
   t->GetBufferDeviceAddress =
      (PFN_vkGetBufferDeviceAddress)gdpa(device, "vkGetBufferDeviceAddressKHR");
   if (!t->GetBufferDeviceAddress)
      t->GetBufferDeviceAddress =
         (PFN_vkGetBufferDeviceAddress)gdpa(device, "vkGetBufferDeviceAddress");
   VKRT_LOAD_D(GetDeviceProcAddr);
   VKRT_LOAD_D(DestroyDevice);
   VKRT_LOAD_D(GetDeviceQueue);
   VKRT_LOAD_D(GetDeviceQueue2);
   VKRT_LOAD_D(QueueSubmit);
   VKRT_LOAD_D(QueueSubmit2);
   if (!t->QueueSubmit2) t->QueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(device, "vkQueueSubmit2KHR");
   VKRT_LOAD_D(CreateBuffer);
   VKRT_LOAD_D(AllocateMemory);
   VKRT_LOAD_D(CreatePipelineLayout);
   VKRT_LOAD_D(CreateShaderModule);
   VKRT_LOAD_D(UpdateDescriptorSets);
   VKRT_LOAD_D(CreateDescriptorSetLayout);
   VKRT_LOAD_D(CreateDescriptorPool);
   VKRT_LOAD_D(CreateGraphicsPipelines);
   VKRT_LOAD_D(CreateComputePipelines);
   VKRT_LOAD_D(AllocateCommandBuffers);
   VKRT_LOAD_D(FreeCommandBuffers);
   VKRT_LOAD_D(ResetCommandBuffer);
   VKRT_LOAD_D(ResetCommandPool);
   VKRT_LOAD_D(DestroyCommandPool);
   VKRT_LOAD_D(BeginCommandBuffer);
   VKRT_LOAD_D(EndCommandBuffer);
   VKRT_LOAD_D(CmdBindPipeline);
   VKRT_LOAD_D(CmdBindDescriptorSets);
   VKRT_LOAD_D(CmdPushConstants);
   VKRT_LOAD_D(CmdPipelineBarrier);
   VKRT_LOAD_D(CmdPipelineBarrier2);
   if (!t->CmdPipelineBarrier2) t->CmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)gdpa(device, "vkCmdPipelineBarrier2KHR");
   VKRT_LOAD_D(CmdBindVertexBuffers);
   VKRT_LOAD_D(CmdBindIndexBuffer);
   VKRT_LOAD_D(CmdSetViewport);
   VKRT_LOAD_D(CmdSetScissor);
   VKRT_LOAD_D(CmdDispatch);
   VKRT_LOAD_D(CmdDraw);
   VKRT_LOAD_D(CmdBeginRenderPass);
   VKRT_LOAD_D(CmdBeginRenderPass2);
   VKRT_LOAD_D(CmdEndRenderPass);
   VKRT_LOAD_D(CmdEndRenderPass2);
   VKRT_LOAD_D(CmdSetBlendConstants);
   VKRT_LOAD_D(CmdSetDepthBias);
   VKRT_LOAD_D(CmdSetDepthBounds);
   VKRT_LOAD_D(CmdSetStencilCompareMask);
   VKRT_LOAD_D(CmdSetStencilWriteMask);
   VKRT_LOAD_D(CmdSetStencilReference);
   VKRT_LOAD_D(CmdSetLineWidth);
   VKRT_LOAD_D(CmdSetDepthTestEnable);
   VKRT_LOAD_D(CmdSetDepthWriteEnable);
   VKRT_LOAD_D(CmdSetDepthCompareOp);
   VKRT_LOAD_D(CmdSetStencilTestEnable);
   VKRT_LOAD_D(CmdSetStencilOp);
   VKRT_LOAD_D(CmdSetCullMode);
   VKRT_LOAD_D(CmdSetFrontFace);
   VKRT_LOAD_D(CmdSetPrimitiveTopology);
   VKRT_LOAD_D(CmdSetViewportWithCount);
   VKRT_LOAD_D(CmdSetScissorWithCount);
   VKRT_LOAD_D(CmdSetDepthBiasEnable);
   VKRT_LOAD_D(CmdSetPrimitiveRestartEnable);
   VKRT_LOAD_D(CmdSetRasterizerDiscardEnable);
   /* GetBufferDeviceAddress is resolved above (KHR alias first); loading
    * it again under the core name would clobber the fallback with NULL. */
   VKRT_LOAD_D(GetBufferMemoryRequirements);
   VKRT_LOAD_D(BindBufferMemory);
   VKRT_LOAD_D(CreateAccelerationStructureKHR);
   VKRT_LOAD_D(DestroyAccelerationStructureKHR);
   VKRT_LOAD_D(GetAccelerationStructureBuildSizesKHR);
   VKRT_LOAD_D(GetAccelerationStructureDeviceAddressKHR);
   VKRT_LOAD_D(CmdBuildAccelerationStructuresKHR);
   VKRT_LOAD_D(CreateRayTracingPipelinesKHR);
   VKRT_LOAD_D(CmdTraceRaysKHR);
   VKRT_LOAD_D(DestroyPipeline);
   VKRT_LOAD_D(DestroyShaderModule);
   VKRT_LOAD_D(DestroyPipelineLayout);
   VKRT_LOAD_D(DestroyDescriptorSetLayout);
   VKRT_LOAD_D(FreeMemory);
   VKRT_LOAD_D(DestroyBuffer);
   VKRT_LOAD_D(CreateSampler);
#undef VKRT_LOAD_D
   t->CreateComputePipelinesInternal = t->CreateComputePipelines;
}

#endif /* VKRT_DISPATCH_H */
