/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_chain.h"

static VkBufferUsageFlags2KHR buffer_usage(VkBufferUsageFlags2KHR usage) {
    /* The software builder and traversal access AS storage through ordinary
     * storage buffers/BDA. Native AS usages are invalid on a no-RT ICD. */
    usage &= ~((VkBufferUsageFlags2KHR)VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR);
    return usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkrt_CreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkBufferCreateInfo local = *info;
    local.usage = (VkBufferUsageFlags)buffer_usage(info->usage);
    const VkBaseInStructure *existing = info->pNext;
    while (existing && existing->sType != VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR) existing = existing->pNext;
    if (!existing) return dev->vtable.CreateBuffer(device, &local, allocator, buffer);

    /* Flags2 overrides VkBufferCreateInfo::usage. Clone its prefix and retain
     * the opaque suffix without mutating or truncating the caller's chain. */
    VkBufferUsageFlags2CreateInfoKHR usage2 = *(const VkBufferUsageFlags2CreateInfoKHR *)existing;
    usage2.usage = buffer_usage(usage2.usage);
    VkBaseOutStructure *head = NULL, **tail = &head;
    VkResult result = VK_SUCCESS;
    for (const VkBaseInStructure *p = info->pNext; p != existing; p = p->pNext) {
        size_t size = vkrt_chain_size(p->sType);
        if (!size) {
            result = VK_ERROR_EXTENSION_NOT_PRESENT;
            goto done;
        }
        VkBaseOutStructure *copy = malloc(size);
        if (!copy) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto done;
        }
        memcpy(copy, p, size);
        copy->pNext = NULL;
        *tail = copy;
        tail = &copy->pNext;
    }
    *tail = (VkBaseOutStructure *)&usage2;
    local.pNext = head;
    result = dev->vtable.CreateBuffer(device, &local, allocator, buffer);
done:
    while (head && head != (VkBaseOutStructure *)&usage2) {
        VkBaseOutStructure *next = head->pNext;
        free(head);
        head = next;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkrt_UpdateDescriptorSets(VkDevice device,
   uint32_t writeCount, const VkWriteDescriptorSet *writes,
   uint32_t copyCount, const VkCopyDescriptorSet *copies)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
   if (!dev) return;
   size_t buffer_count = 0;
   for (uint32_t i = 0; i < writeCount; ++i) {
      if (writes[i].descriptorType != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) continue;
      if (writes[i].descriptorCount > SIZE_MAX / sizeof(VkDescriptorBufferInfo) - buffer_count) {
         VKRT_LOG_ERR("UpdateDescriptorSets: descriptor array too large");
         return;
      }
      buffer_count += writes[i].descriptorCount;
   }
   if (!buffer_count) {
      dev->vtable.UpdateDescriptorSets(device, writeCount, writes, copyCount, copies);
      return;
   }
   VkWriteDescriptorSet *local = calloc(writeCount, sizeof(*local));
   VkDescriptorBufferInfo *buffers = calloc(buffer_count, sizeof(*buffers));
   if (!local || !buffers) {
      VKRT_LOG_ERR("UpdateDescriptorSets: out of memory");
      goto done;
   }
   size_t cursor = 0;
   for (uint32_t i = 0; i < writeCount; ++i) {
      local[i] = writes[i];
      if (writes[i].descriptorType != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) continue;
      const VkWriteDescriptorSetAccelerationStructureKHR *as_info = NULL;
      for (const VkBaseInStructure *p = writes[i].pNext; p; p = p->pNext)
         if (p->sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR)
            as_info = (const void *)p;
      if (!as_info || as_info->accelerationStructureCount != writes[i].descriptorCount ||
          (writes[i].descriptorCount && !as_info->pAccelerationStructures)) {
         VKRT_LOG_ERR("UpdateDescriptorSets: invalid AS descriptor array");
         goto done;
      }
      local[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      local[i].pNext = NULL;
      local[i].pBufferInfo = buffers + cursor;
      local[i].pImageInfo = NULL;
      local[i].pTexelBufferView = NULL;
      for (uint32_t j = 0; j < writes[i].descriptorCount; ++j) {
         VkAccelerationStructureKHR handle = as_info->pAccelerationStructures[j];
         if (handle == VK_NULL_HANDLE) {
            /* nullDescriptor, when enabled by the app, is null for SSBOs too. */
            buffers[cursor++] = (VkDescriptorBufferInfo){VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
            continue;
         }
         struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, handle);
         if (!as || as->owner != device) {
            VKRT_LOG_ERR("UpdateDescriptorSets: unknown or foreign AS handle");
            goto done;
         }
         if (!as->address_cell_buffer) {
             VKRT_LOG_ERR("UpdateDescriptorSets: AS address cell is unavailable");
             goto done;
         }
         /* Bind the immutable AS value cell without querying an unbound
          * backing buffer or writing storage that a shader may be reading. */
         buffers[cursor++] = (VkDescriptorBufferInfo){as->address_cell_buffer, 0, sizeof(VkDeviceAddress)};
      }
   }
   /* Keep each write's binding, array element and count. Each now owns a
    * disjoint contiguous slice; mixed ordinary writes and copies pass through. */
   dev->vtable.UpdateDescriptorSets(device, writeCount, local, copyCount, copies);
done:
   free(local);
   free(buffers);
}

VKAPI_ATTR VkResult VKAPI_CALL vkrt_AllocateMemory(VkDevice device,
   const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
   struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   const VkBaseInStructure *existing = info->pNext;
   while (existing && existing->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO)
      existing = existing->pNext;
   VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
   if (existing) {
      flags = *(const VkMemoryAllocateFlagsInfo *)existing;
      if (flags.flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)
         return dev->vtable.AllocateMemory(device, info, allocator, memory);
   } else flags.pNext = info->pNext;
   flags.flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

   /* Clone only the prefix leading to an existing flags node. The suffix is
    * borrowed unchanged. Never mutate the caller's chain or add a duplicate
    * sType; retain deviceMask, other flag bits and all unrelated payloads. */
   VkBaseOutStructure *head = NULL, **tail = &head;
   VkResult result = VK_SUCCESS;
   if (existing) {
      for (const VkBaseInStructure *p = info->pNext; p != existing; p = p->pNext) {
         size_t size = vkrt_chain_size(p->sType);
         if (!size) { result = VK_ERROR_EXTENSION_NOT_PRESENT; goto done; }
         VkBaseOutStructure *copy = malloc(size);
         if (!copy) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
         memcpy(copy, p, size);
         copy->pNext = NULL;
         *tail = copy;
         tail = &copy->pNext;
      }
   }
   *tail = (VkBaseOutStructure *)&flags;
   VkMemoryAllocateInfo local = *info;
   local.pNext = head;
   result = dev->vtable.AllocateMemory(device, &local, allocator, memory);
done:
   while (head && head != (VkBaseOutStructure *)&flags) {
      VkBaseOutStructure *next = head->pNext;
      free(head);
      head = next;
   }
   return result;
}
