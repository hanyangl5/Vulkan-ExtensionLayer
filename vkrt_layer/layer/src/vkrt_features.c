/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_chain.h"

VkResult vkrt_create_device_with_features(struct vkrt_instance_data *inst,
   VkPhysicalDevice physical, PFN_vkCreateDevice create,
   const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *device)
{
   VkPhysicalDeviceBufferDeviceAddressFeatures bda = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
   VkPhysicalDeviceVulkanMemoryModelFeatures memory = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, .pNext = &bda};
   VkPhysicalDeviceScalarBlockLayoutFeatures scalar = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, .pNext = &memory};
   VkPhysicalDeviceShaderFloat16Int8Features int8 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, .pNext = &scalar};
   VkPhysicalDevice8BitStorageFeatures storage8 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, .pNext = &int8};
   VkPhysicalDevice16BitStorageFeatures storage16 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, .pNext = &storage8};
   VkPhysicalDeviceFeatures2 supported = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &storage16};
   if (!inst->vtable.GetPhysicalDeviceFeatures2)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   inst->vtable.GetPhysicalDeviceFeatures2(physical, &supported);
   if (!supported.features.shaderInt64 || !supported.features.shaderInt16 ||
       !storage16.storageBuffer16BitAccess || !storage8.storageBuffer8BitAccess ||
       !int8.shaderInt8 || !scalar.scalarBlockLayout || !bda.bufferDeviceAddress ||
       !memory.vulkanMemoryModel || !memory.vulkanMemoryModelDeviceScope) {
      VKRT_LOG_ERR("GPU BVH requires BDA, scalar layout, int8/16/64, buffer8/16 and Vulkan device-scope memory model");
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   VkBaseOutStructure *head = NULL, **tail = &head;
   VkResult result = VK_SUCCESS;
   for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
      size_t size = vkrt_chain_size(p->sType);
      if (!size) { result = VK_ERROR_EXTENSION_NOT_PRESENT; goto done; }
      VkBaseOutStructure *copy = malloc(size);
      if (!copy) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
      memcpy(copy, p, size);
      copy->pNext = NULL;
      *tail = copy;
      tail = &copy->pNext;
   }

   VkPhysicalDeviceFeatures core = ci->pEnabledFeatures ? *ci->pEnabledFeatures : (VkPhysicalDeviceFeatures){0};
   VkPhysicalDeviceFeatures *features = &core;
   bool has_features2 = false;
   bool has16 = false, has8 = false, has_int8 = false, has_scalar = false, has_memory = false, has_bda = false;
   for (VkBaseOutStructure *p = head; p; p = p->pNext) {
      switch (p->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
         features = &((VkPhysicalDeviceFeatures2 *)p)->features;
         has_features2 = true;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
         ((VkPhysicalDeviceVulkan11Features *)p)->storageBuffer16BitAccess = VK_TRUE;
         has16 = true; break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
         VkPhysicalDeviceVulkan12Features *f = (void *)p;
         f->storageBuffer8BitAccess = f->shaderInt8 = f->scalarBlockLayout = VK_TRUE;
         f->vulkanMemoryModel = f->vulkanMemoryModelDeviceScope = f->bufferDeviceAddress = VK_TRUE;
         has8 = has_int8 = has_scalar = has_memory = has_bda = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
         ((VkPhysicalDevice16BitStorageFeatures *)p)->storageBuffer16BitAccess = VK_TRUE;
         has16 = true; break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
         ((VkPhysicalDevice8BitStorageFeatures *)p)->storageBuffer8BitAccess = VK_TRUE;
         has8 = true; break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
         ((VkPhysicalDeviceShaderFloat16Int8Features *)p)->shaderInt8 = VK_TRUE;
         has_int8 = true; break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
         ((VkPhysicalDeviceScalarBlockLayoutFeatures *)p)->scalarBlockLayout = VK_TRUE;
         has_scalar = true; break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
         ((VkPhysicalDeviceVulkanMemoryModelFeatures *)p)->vulkanMemoryModel = VK_TRUE;
         ((VkPhysicalDeviceVulkanMemoryModelFeatures *)p)->vulkanMemoryModelDeviceScope = VK_TRUE;
         has_memory = true; break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
         ((VkPhysicalDeviceBufferDeviceAddressFeatures *)p)->bufferDeviceAddress = VK_TRUE;
         has_bda = true; break;
      default: break;
      }
   }
   features->shaderInt64 = features->shaderInt16 = VK_TRUE;
   /* Query results must not accidentally enable unrelated optional features. */
   storage16 = (VkPhysicalDevice16BitStorageFeatures){.sType = storage16.sType, .storageBuffer16BitAccess = VK_TRUE};
   storage8 = (VkPhysicalDevice8BitStorageFeatures){.sType = storage8.sType, .storageBuffer8BitAccess = VK_TRUE};
   int8 = (VkPhysicalDeviceShaderFloat16Int8Features){.sType = int8.sType, .shaderInt8 = VK_TRUE};
   scalar = (VkPhysicalDeviceScalarBlockLayoutFeatures){.sType = scalar.sType, .scalarBlockLayout = VK_TRUE};
   memory = (VkPhysicalDeviceVulkanMemoryModelFeatures){.sType = memory.sType, .vulkanMemoryModel = VK_TRUE, .vulkanMemoryModelDeviceScope = VK_TRUE};
   bda = (VkPhysicalDeviceBufferDeviceAddressFeatures){.sType = bda.sType, .bufferDeviceAddress = VK_TRUE};
   VkDeviceCreateInfo local = *ci;
   local.pNext = head;
   local.pEnabledFeatures = has_features2 ? NULL : &core;
#define PREPEND(node, present) if (!(present)) { node.pNext = (void *)local.pNext; local.pNext = &node; }
   PREPEND(storage16, has16);
   PREPEND(storage8, has8);
   PREPEND(int8, has_int8);
   PREPEND(scalar, has_scalar);
   PREPEND(memory, has_memory);
   PREPEND(bda, has_bda);
#undef PREPEND
   /* Extension aliases also cover applications that requested Vulkan 1.0/1.1. */
   const char *required[] = {VK_KHR_16BIT_STORAGE_EXTENSION_NAME, VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
      VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME,
      VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};
   const char **extensions = malloc((ci->enabledExtensionCount + VKRT_ARRAY_SIZE(required)) * sizeof(*extensions));
   if (!extensions) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
   memcpy(extensions, ci->ppEnabledExtensionNames, ci->enabledExtensionCount * sizeof(*extensions));
   VkPhysicalDeviceProperties properties;
   inst->vtable.GetPhysicalDeviceProperties(physical, &properties);
   for (size_t i = 0; i < VKRT_ARRAY_SIZE(required); ++i) {
      /* Do not require extension aliases when the application uses core
       * features: implementations may expose core 1.2 without those names. */
      uint32_t promoted = i == 0 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_2;
      if (inst->api_version >= promoted && properties.apiVersion >= promoted) continue;
      bool found = false;
      for (uint32_t j = 0; j < local.enabledExtensionCount; ++j)
         found |= strcmp(extensions[j], required[i]) == 0;
      if (!found) extensions[local.enabledExtensionCount++] = required[i];
   }
   local.ppEnabledExtensionNames = extensions;
   result = create(physical, &local, alloc, device);
   free(extensions);
done:
   while (head) { VkBaseOutStructure *next = head->pNext; free(head); head = next; }
   return result;
}
