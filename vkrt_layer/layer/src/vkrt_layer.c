/* SPDX-License-Identifier: MIT
 *
 * vkrt_layer.c - loader-layer entry points for the VKRT software ray query
 * layer.
 *
 * Implements milestone M0 of
 * vkrt_layer/rayquery_software_rt_layer_design.md:
 *   - vkNegotiateLoaderLayerInterfaceVersion
 *   - instance/device dispatch walking VK_LAYER_LINK_INFO
 *   - shader-module interception (detect + fail-fast on RT-pipeline ops)
 *   - acceleration-structure API virtualisation
 *
 * Deployment: bootstrap mode. The ICD keeps its real VK_KHR_ray_query;
 * this layer sits above it and replaces the AS build and ray-query
 * traversal with the software path so results can be diffed against the
 * native implementation (the oracle). Virtual-extension mode (strip the
 * extension names, advertise our own) is milestone M6.
 */

#include "vkrt_layer.h"

struct vkrt_global vkrt_global;

/* ================================================================== */
/* Chain info helpers                                                  */
/* ================================================================== */

static VkLayerInstanceCreateInfo *
vkrt_get_instance_chain_info(const VkInstanceCreateInfo *pCreateInfo)
{
   vk_foreach_struct_const (item, pCreateInfo->pNext) {
      if (item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
          ((const VkLayerInstanceCreateInfo *)item)->function ==
             VK_LAYER_LINK_INFO)
         return (VkLayerInstanceCreateInfo *)item;
   }
   VKRT_LOG_ERR("instance chain info not found");
   return NULL;
}

static VkLayerDeviceCreateInfo *
vkrt_get_device_chain_info(const VkDeviceCreateInfo *pCreateInfo)
{
   vk_foreach_struct_const (item, pCreateInfo->pNext) {
      if (item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
          ((const VkLayerDeviceCreateInfo *)item)->function == VK_LAYER_LINK_INFO)
         return (VkLayerDeviceCreateInfo *)item;
   }
   VKRT_LOG_ERR("device chain info not found");
   return NULL;
}

/* ================================================================== */
/* Instance                                                            */
/* ================================================================== */

static VKAPI_ATTR VkResult VKAPI_CALL
vkrt_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkInstance *pInstance)
{
   VkLayerInstanceCreateInfo *chain_info =
      vkrt_get_instance_chain_info(pCreateInfo);
   if (!chain_info || !chain_info->u.pLayerInfo)
      return VK_ERROR_INITIALIZATION_FAILED;

   PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
      chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;

   PFN_vkCreateInstance fpCreateInstance =
      (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!fpCreateInstance)
      return VK_ERROR_INITIALIZATION_FAILED;

   /* Advance the chain for the next layer down. */
   chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

   VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
   if (result != VK_SUCCESS)
      return result;

   struct vkrt_instance_data *data =
      (struct vkrt_instance_data *)calloc(1, sizeof(*data));
   if (!data) {
      PFN_vkDestroyInstance fpDestroyInstance =
         (PFN_vkDestroyInstance)fpGetInstanceProcAddr(*pInstance,
                                                      "vkDestroyInstance");
      if (fpDestroyInstance)
         fpDestroyInstance(*pInstance, pAllocator);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   vkrt_instance_dispatch_load(&data->vtable, fpGetInstanceProcAddr, *pInstance);
   data->instance = *pInstance;
   data->api_version = pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion ?
      pCreateInfo->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;

   vkrt_map_object(VKRT_HKEY(*pInstance), data);

   /* vkrt_CreateDevice is handed a VkPhysicalDevice, not the instance, so
    * every physical device has to resolve back to this instance data. */
   {
      uint32_t n = 0;
      if (data->vtable.EnumeratePhysicalDevices &&
          data->vtable.EnumeratePhysicalDevices(*pInstance, &n, NULL) == VK_SUCCESS &&
          n > 0) {
         VkPhysicalDevice *pds =
            (VkPhysicalDevice *)calloc(n, sizeof(*pds));
         if (pds) {
            data->vtable.EnumeratePhysicalDevices(*pInstance, &n, pds);
            for (uint32_t i = 0; i < n; i++)
               vkrt_map_object(VKRT_HKEY(pds[i]), data);
            free(pds);
         }
      }
   }

   VKRT_LOG_DBG("CreateInstance ok (%p)", (void *)*pInstance);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
vkrt_DestroyInstance(VkInstance instance,
                     const VkAllocationCallbacks *pAllocator)
{
   struct vkrt_instance_data *data =
      VKRT_FIND(struct vkrt_instance_data, instance);
   if (!data)
      return;

   data->vtable.DestroyInstance(instance, pAllocator);
   vkrt_unmap_object(VKRT_HKEY(instance));
   free(data);
}

/* ================================================================== */
/* Device                                                              */
/* ================================================================== */

static VKAPI_ATTR VkResult VKAPI_CALL
vkrt_CreateDevice(VkPhysicalDevice physicalDevice,
                  const VkDeviceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkDevice *pDevice)
{
   struct vkrt_instance_data *inst_data =
      VKRT_FIND(struct vkrt_instance_data, physicalDevice);
   if (!inst_data) {
      /* The framework may hand us a physical device we did not map if the
       * app created the instance without this layer in the chain. Fail
       * loudly rather than silently dropping the interception. */
      VKRT_LOG_ERR("CreateDevice: no instance data for physical device %p",
                   (void *)physicalDevice);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   VkLayerDeviceCreateInfo *chain_info =
      vkrt_get_device_chain_info(pCreateInfo);
   if (!chain_info || !chain_info->u.pLayerInfo)
      return VK_ERROR_INITIALIZATION_FAILED;

   PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
      chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr =
      chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   if (!fpGetInstanceProcAddr || !fpGetDeviceProcAddr)
      return VK_ERROR_INITIALIZATION_FAILED;

   PFN_vkCreateDevice fpCreateDevice =
      (PFN_vkCreateDevice)fpGetInstanceProcAddr(NULL, "vkCreateDevice");
   if (!fpCreateDevice) {
      /* Fall back to the instance table -- some loaders only resolve it
       * through the instance. */
      fpCreateDevice = inst_data->vtable.CreateDevice;
   }
   if (!fpCreateDevice)
      return VK_ERROR_INITIALIZATION_FAILED;

   chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

   VkResult result = vkrt_create_device_with_features(inst_data, physicalDevice,
      fpCreateDevice, pCreateInfo, pAllocator, pDevice);
   if (result != VK_SUCCESS)
      return result;

   struct vkrt_device_data *data =
      (struct vkrt_device_data *)calloc(1, sizeof(*data));
   if (!data) {
      PFN_vkDestroyDevice fpDestroyDevice =
         (PFN_vkDestroyDevice)fpGetDeviceProcAddr(*pDevice, "vkDestroyDevice");
      if (fpDestroyDevice)
         fpDestroyDevice(*pDevice, pAllocator);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   vkrt_device_dispatch_load(&data->vtable, fpGetDeviceProcAddr, *pDevice);
   data->device           = *pDevice;
   data->physical_device  = physicalDevice;
   data->instance_data    = inst_data;
   vkrt_mutex_init(&data->command_buffers_lock);
   result = vkrt_build_init(data);
   if (result != VK_SUCCESS) {
      VKRT_LOG_ERR("GPU BVH pipeline initialization failed: %d", result);
      vkrt_build_finish(data);
      data->vtable.DestroyDevice(*pDevice, pAllocator);
      free(data);
      return result;
   }

   /* Bootstrap mode: record whether the ICD actually has ray query so the
    * oracle comparison knows what it is diffing against. */
   data->icd_has_ray_query = false;
   if (pCreateInfo->ppEnabledExtensionNames) {
      for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
         if (strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                    VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) {
            data->icd_has_ray_query = true;
            break;
         }
      }
   }

   vkrt_map_object(VKRT_HKEY(*pDevice), data);
   /* Physical devices map to the instance data so CreateDevice can find it;
    * now also key the physical device to our device data for the AS calls
    * that take VkPhysicalDevice. Keep both. */
   VKRT_LOG_DBG("CreateDevice ok (%p) icd_has_ray_query=%d", (void *)*pDevice,
                (int)data->icd_has_ray_query);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
vkrt_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data)
      return;

   vkrt_commands_finish(data);
   vkrt_build_finish(data);
   data->vtable.DestroyDevice(device, pAllocator);
   /* Remove device and queue aliases before releasing their shared owner. */
   vkrt_mutex_lock(&vkrt_global.objects_lock);
   for (size_t i = 0; i < vkrt_global.objects.capacity;) {
      if (vkrt_global.objects.entries[i].value == data) {
         vkrt_map_remove(&vkrt_global.objects, vkrt_global.objects.entries[i].key);
         i = 0; /* deletion can shift a wrapped probe cluster */
      } else ++i;
   }
   vkrt_mutex_unlock(&vkrt_global.objects_lock);
   free(data);
}

/* ================================================================== */
/* Shader module interception                                          */
/* ================================================================== */

static VKAPI_ATTR VkResult VKAPI_CALL
vkrt_CreateShaderModule(VkDevice device,
                        const VkShaderModuleCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkShaderModule *pShaderModule)
{
   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkShaderModuleCreateInfo local_ci = *pCreateInfo;
   bool uses_rq = false, uses_rtp = false;

   VkResult result = vkrt_rewrite_shader_module(data, pCreateInfo, &local_ci,
                                                &uses_rq, &uses_rtp);
   if (result != VK_SUCCESS) {
      /* Fail-fast on RT-pipeline ops / unrepresentable input (doc 11.1). */
      return result;
   }

   if (uses_rtp) {
      VKRT_LOG_ERR(
         "CreateShaderModule: VK_KHR_ray_tracing_pipeline ops are not "
         "supported by the software ray-query layer (doc section 2)");
      free((void *)local_ci.pCode);
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   result = data->vtable.CreateShaderModule(device, &local_ci, pAllocator,
                                            pShaderModule);

   if (local_ci.pCode != pCreateInfo->pCode)
      free((void *)local_ci.pCode);

   if (result != VK_SUCCESS)
      return result;

   struct vkrt_shader_module_data *sm =
      (struct vkrt_shader_module_data *)calloc(1, sizeof(*sm));
   if (sm) {
      sm->module        = *pShaderModule;
      sm->device_data   = data;
      sm->uses_ray_query = uses_rq;
      vkrt_map_object(VKRT_HKEY(*pShaderModule), sm);
   }

   if (uses_rq)
      VKRT_LOG_DBG("CreateShaderModule %p: ray query lowered",
                   (void *)*pShaderModule);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
vkrt_DestroyShaderModule(VkDevice device, VkShaderModule shaderModule,
                         const VkAllocationCallbacks *pAllocator)
{
   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data)
      return;

   struct vkrt_shader_module_data *sm =
      (struct vkrt_shader_module_data *)vkrt_unmap_object(
         VKRT_HKEY(shaderModule));
   if (sm) {
      free(sm->rewritten);
      free(sm);
   }
   data->vtable.DestroyShaderModule(device, shaderModule, pAllocator);
}

/* ================================================================== */
/* Descriptor rewrite (doc section 8)                                  */
/*                                                                     */
/* An AS binding becomes a storage buffer, keeping the app's            */
/* (set, binding, arrayElement) numbering exactly. The bound buffer is  */
/* a tiny cell holding the BVHIR root address -- see vkrt_as_addr_buffer.*/
/* ================================================================== */

static VKAPI_ATTR VkResult VKAPI_CALL
vkrt_CreateDescriptorSetLayout(VkDevice device,
                               const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                               const VkAllocationCallbacks *pAllocator,
                               VkDescriptorSetLayout *pSetLayout)
{
   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data)
      return VK_ERROR_INITIALIZATION_FAILED;

   bool rewrite = false;
   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      if (pCreateInfo->pBindings[i].descriptorType ==
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
         rewrite = true;
         break;
      }
   }
   if (!rewrite)
      return data->vtable.CreateDescriptorSetLayout(device, pCreateInfo,
                                                    pAllocator, pSetLayout);

   /* Copy and swap the descriptor type in place so every downstream
    * object (pipeline layout, descriptor set, update) agrees. */
   VkDescriptorSetLayoutBinding *b =
      (VkDescriptorSetLayoutBinding *)calloc(pCreateInfo->bindingCount,
                                             sizeof(*b));
   if (!b)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      b[i] = pCreateInfo->pBindings[i];
      if (b[i].descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)
         b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   }

   VkDescriptorSetLayoutCreateInfo local = *pCreateInfo;
   local.pBindings = b;
   VkResult r = data->vtable.CreateDescriptorSetLayout(device, &local,
                                                       pAllocator, pSetLayout);
   free(b);
   return r;
}

/* The set layout's AS bindings were rewritten to STORAGE_BUFFER, so the
 * pool has to offer STORAGE_BUFFER slots -- leaving an AS pool size here
 * makes vkAllocateDescriptorSets fail with a layout/pool mismatch. */
static VKAPI_ATTR VkResult VKAPI_CALL
vkrt_CreateDescriptorPool(VkDevice device,
                          const VkDescriptorPoolCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkDescriptorPool *pDescriptorPool)
{
   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data)
      return VK_ERROR_INITIALIZATION_FAILED;

   bool rewrite = false;
   for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; i++) {
      if (pCreateInfo->pPoolSizes[i].type ==
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
         rewrite = true;
         break;
      }
   }
   if (!rewrite)
      return data->vtable.CreateDescriptorPool(device, pCreateInfo,
                                               pAllocator, pDescriptorPool);

   /* Fold every AS pool size into STORAGE_BUFFER, merging duplicates. */
   VkDescriptorPoolSize *sizes =
      (VkDescriptorPoolSize *)calloc(pCreateInfo->poolSizeCount,
                                     sizeof(*sizes));
   if (!sizes)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   uint32_t n = 0;
   uint32_t ssbo_extra = 0;
   for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; i++) {
      VkDescriptorPoolSize s = pCreateInfo->pPoolSizes[i];
      if (s.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
         ssbo_extra += s.descriptorCount;
         continue;
      }
      sizes[n++] = s;
   }
   if (ssbo_extra) {
      /* Reuse an existing SSBO row if there is one. */
      bool merged = false;
      for (uint32_t i = 0; i < n; i++) {
         if (sizes[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            sizes[i].descriptorCount += ssbo_extra;
            merged = true;
            break;
         }
      }
      if (!merged) {
         sizes[n].type  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
         sizes[n].descriptorCount = ssbo_extra;
         n++;
      }
   }

   VkDescriptorPoolCreateInfo local = *pCreateInfo;
   local.poolSizeCount = n;
   local.pPoolSizes    = sizes;
   VkResult r = data->vtable.CreateDescriptorPool(device, &local, pAllocator,
                                                  pDescriptorPool);
   free(sizes);
   return r;
}


/* ================================================================== */
/* Buffer usage augmentation (doc section 8)                           */
/* ================================================================== */

static VKAPI_ATTR VkResult VKAPI_CALL
vkrt_CreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkBufferCreateInfo local_ci = *pCreateInfo;
   local_ci.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
   return data->vtable.CreateBuffer(device, &local_ci, pAllocator, pBuffer);
}


/* ================================================================== */
/* Acceleration structure virtualisation (doc section 7)               */
/* ================================================================== */

static VKAPI_ATTR VkResult VKAPI_CALL
vkrt_CreateAccelerationStructureKHR(
   VkDevice device,
   const VkAccelerationStructureCreateInfoKHR *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkAccelerationStructureKHR *pAccelerationStructure)
{
   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data)
      return VK_ERROR_INITIALIZATION_FAILED;
   return vkrt_create_virtual_as(data, pCreateInfo, pAccelerationStructure);
}

static VKAPI_ATTR void VKAPI_CALL
vkrt_DestroyAccelerationStructureKHR(
   VkDevice device, VkAccelerationStructureKHR accelerationStructure,
   const VkAllocationCallbacks *pAllocator)
{
   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data)
      return;
   vkrt_destroy_virtual_as(data, accelerationStructure);
}

static VKAPI_ATTR void VKAPI_CALL
vkrt_GetAccelerationStructureBuildSizesKHR(
   VkDevice device,
   VkAccelerationStructureBuildTypeKHR buildType,
   const VkAccelerationStructureBuildGeometryInfoKHR *pBuildInfo,
   const uint32_t *pMaxPrimitiveCounts,
   VkAccelerationStructureBuildSizesInfoKHR *pSizeInfo)
{
   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data)
      return;
   vkrt_as_build_sizes(data, pBuildInfo, pMaxPrimitiveCounts, pSizeInfo);
}

static VKAPI_ATTR VkDeviceAddress VKAPI_CALL
vkrt_GetAccelerationStructureDeviceAddressKHR(
   VkDevice device,
   const VkAccelerationStructureDeviceAddressInfoKHR *pInfo)
{
   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data)
      return 0;
   return vkrt_as_device_address(data, pInfo->accelerationStructure);
}

static VKAPI_ATTR void VKAPI_CALL
vkrt_CmdBuildAccelerationStructuresKHR(
   VkCommandBuffer commandBuffer, uint32_t infoCount,
   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
   const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos)
{
   struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, commandBuffer);
   struct vkrt_device_data *data = cb ? cb->device_data : NULL;

   if (!data) {
      VKRT_LOG_ERR("CmdBuildAccelerationStructuresKHR: no device data");
      return;
   }

   vkrt_cmd_build_as(data, commandBuffer, infoCount, pInfos,
                     ppBuildRangeInfos);
}

/* v1 explicitly rejects: indirect build, copy, compact, serialize,
 * deserialize, update. Fail-fast rather than forwarding (doc section 7).
 * Each stub keeps a full parameter list so it still matches its PFN_. */
#define VKRT_REJECT_CMD(name, ...)                                             \
   static VKAPI_ATTR void VKAPI_CALL vkrt_##name(__VA_ARGS__)                   \
   {                                                                           \
      VKRT_LOG_ERR(#name " is not supported by the software ray-query layer "  \
                         "(v1; see design doc section 2/7)");                  \
   }

VKRT_REJECT_CMD(CmdCopyAccelerationStructureKHR,
                VkCommandBuffer commandBuffer,
                const VkCopyAccelerationStructureInfoKHR *pInfo)
VKRT_REJECT_CMD(CmdCopyAccelerationStructureToMemoryKHR,
                VkCommandBuffer commandBuffer,
                const VkCopyAccelerationStructureToMemoryInfoKHR *pInfo)
VKRT_REJECT_CMD(CmdCopyMemoryToAccelerationStructureKHR,
                VkCommandBuffer commandBuffer,
                const VkCopyMemoryToAccelerationStructureInfoKHR *pInfo)
VKRT_REJECT_CMD(CmdWriteAccelerationStructuresPropertiesKHR,
                VkCommandBuffer commandBuffer,
                uint32_t accelerationStructureCount,
                const VkAccelerationStructureKHR *pAccelerationStructures,
                VkQueryType queryType, VkQueryPool queryPool, uint32_t firstQuery)
VKRT_REJECT_CMD(CmdBuildAccelerationStructuresIndirectKHR,
                VkCommandBuffer commandBuffer, uint32_t infoCount,
                const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
                const VkDeviceAddress *pIndirectDeviceAddresses,
                const uint32_t *pIndirectStrides,
                const uint32_t *const *ppMaxPrimitiveCounts)
VKRT_REJECT_CMD(CmdSerializeAccelerationStructuresKHR,
                VkCommandBuffer commandBuffer, uint32_t accelerationStructureCount,
                const VkAccelerationStructureKHR *pAccelerationStructures,
                const void *pInfos)
VKRT_REJECT_CMD(CmdDeserializeAccelerationStructuresKHR,
                VkCommandBuffer commandBuffer, uint32_t accelerationStructureCount,
                const VkAccelerationStructureKHR *pAccelerationStructures,
                const void *pInfos)

static VKAPI_ATTR VkResult VKAPI_CALL
vkrt_CreateRayTracingPipelinesKHR(VkDevice device,
                                  VkDeferredOperationKHR deferredOperation,
                                  VkPipelineCache pipelineCache,
                                  uint32_t createInfoCount,
                                  const VkRayTracingPipelineCreateInfoKHR *pCreateInfos,
                                  const VkAllocationCallbacks *pAllocator,
                                  VkPipeline *pPipelines)
{
   VKRT_LOG_ERR("vkCreateRayTracingPipelinesKHR is not supported: this layer "
                "implements VK_KHR_ray_query only, not "
                "VK_KHR_ray_tracing_pipeline (design doc section 2)");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VKAPI_ATTR void VKAPI_CALL
vkrt_CmdTraceRaysKHR(VkCommandBuffer commandBuffer,
                     const VkStridedDeviceAddressRegionKHR *pRaygenShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pMissShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pHitShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pCallableShaderBindingTable,
                     uint32_t width, uint32_t height, uint32_t depth)
{
   VKRT_LOG_ERR("vkCmdTraceRaysKHR is not supported: this layer implements "
                "VK_KHR_ray_query only (design doc section 2)");
}

/* ================================================================== */
/* Proc-addr tables                                                    */
/* ================================================================== */

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkrt_GetDeviceProcAddr(VkDevice device, const char *pName);

static void *
vkrt_find_device_fn(const char *name)
{
   PFN_vkVoidFunction command = vkrt_command_proc(name);
   if (command) return (void *)command;
   command = vkrt_sync_proc(name);
   if (command) return (void *)command;
   static const struct {
      const char *name;
      void       *ptr;
   } map[] = {
      {"vkGetDeviceProcAddr", (void *)vkrt_GetDeviceProcAddr},
      {"vkDestroyDevice", (void *)vkrt_DestroyDevice},
      {"vkCreateShaderModule", (void *)vkrt_CreateShaderModule},
      {"vkDestroyShaderModule", (void *)vkrt_DestroyShaderModule},
      {"vkCreateBuffer", (void *)vkrt_CreateBuffer},
      {"vkCreateDescriptorSetLayout", (void *)vkrt_CreateDescriptorSetLayout},
      {"vkCreateDescriptorPool", (void *)vkrt_CreateDescriptorPool},
      {"vkUpdateDescriptorSets", (void *)vkrt_UpdateDescriptorSets},
      {"vkAllocateMemory", (void *)vkrt_AllocateMemory},
      {"vkCreateAccelerationStructureKHR",
       (void *)vkrt_CreateAccelerationStructureKHR},
      {"vkDestroyAccelerationStructureKHR",
       (void *)vkrt_DestroyAccelerationStructureKHR},
      {"vkGetAccelerationStructureBuildSizesKHR",
       (void *)vkrt_GetAccelerationStructureBuildSizesKHR},
      {"vkGetAccelerationStructureDeviceAddressKHR",
       (void *)vkrt_GetAccelerationStructureDeviceAddressKHR},
      {"vkCmdBuildAccelerationStructuresKHR",
       (void *)vkrt_CmdBuildAccelerationStructuresKHR},
      {"vkCmdBuildAccelerationStructuresIndirectKHR",
       (void *)vkrt_CmdBuildAccelerationStructuresIndirectKHR},
      {"vkCmdCopyAccelerationStructureKHR",
       (void *)vkrt_CmdCopyAccelerationStructureKHR},
      {"vkCmdCopyAccelerationStructureToMemoryKHR",
       (void *)vkrt_CmdCopyAccelerationStructureToMemoryKHR},
      {"vkCmdCopyMemoryToAccelerationStructureKHR",
       (void *)vkrt_CmdCopyMemoryToAccelerationStructureKHR},
      {"vkCmdWriteAccelerationStructuresPropertiesKHR",
       (void *)vkrt_CmdWriteAccelerationStructuresPropertiesKHR},
      {"vkCmdSerializeAccelerationStructuresKHR",
       (void *)vkrt_CmdSerializeAccelerationStructuresKHR},
      {"vkCmdDeserializeAccelerationStructuresKHR",
       (void *)vkrt_CmdDeserializeAccelerationStructuresKHR},
      {"vkCreateRayTracingPipelinesKHR",
       (void *)vkrt_CreateRayTracingPipelinesKHR},
      {"vkCmdTraceRaysKHR", (void *)vkrt_CmdTraceRaysKHR},
   };

   for (size_t i = 0; i < VKRT_ARRAY_SIZE(map); i++) {
      if (strcmp(name, map[i].name) == 0)
         return map[i].ptr;
   }
   return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkrt_GetDeviceProcAddr(VkDevice device, const char *pName)
{
   void *ptr = vkrt_find_device_fn(pName);
   if (ptr)
      return (PFN_vkVoidFunction)ptr;

   if (device == VK_NULL_HANDLE)
      return NULL;

   struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
   if (!data || !data->vtable.GetDeviceProcAddr)
      return NULL;
   return data->vtable.GetDeviceProcAddr(device, pName);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkrt_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
   static const struct {
      const char *name;
      void       *ptr;
   } imap[] = {
      {"vkGetInstanceProcAddr", (void *)vkrt_GetInstanceProcAddr},
      {"vkCreateInstance", (void *)vkrt_CreateInstance},
      {"vkDestroyInstance", (void *)vkrt_DestroyInstance},
      {"vkCreateDevice", (void *)vkrt_CreateDevice},
      {"vkGetDeviceProcAddr", (void *)vkrt_GetDeviceProcAddr},
   };

   for (size_t i = 0; i < VKRT_ARRAY_SIZE(imap); i++) {
      if (strcmp(pName, imap[i].name) == 0)
         return (PFN_vkVoidFunction)imap[i].ptr;
   }

   void *dptr = vkrt_find_device_fn(pName);
   if (dptr)
      return (PFN_vkVoidFunction)dptr;

   if (instance == VK_NULL_HANDLE)
      return NULL;

   struct vkrt_instance_data *data =
      VKRT_FIND(struct vkrt_instance_data, instance);
   if (!data || !data->vtable.GetInstanceProcAddr)
      return NULL;
   return data->vtable.GetInstanceProcAddr(instance, pName);
}

/* ================================================================== */
/* Loader negotiation                                                  */
/* ================================================================== */

/* Declared by <vulkan/vk_layer.h> with VKAPI_ATTR (dllexport on
 * Windows). Do not repeat the attribute here. */
VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
   if (!pVersionStruct ||
       pVersionStruct->loaderLayerInterfaceVersion < 2)
      return VK_ERROR_INITIALIZATION_FAILED;

   pVersionStruct->loaderLayerInterfaceVersion = 2;
   pVersionStruct->pfnGetInstanceProcAddr      = vkrt_GetInstanceProcAddr;
   pVersionStruct->pfnGetDeviceProcAddr        = vkrt_GetDeviceProcAddr;
   return VK_SUCCESS;
}
