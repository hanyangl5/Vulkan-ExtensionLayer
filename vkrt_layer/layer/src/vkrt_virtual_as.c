/* SPDX-License-Identifier: MIT
 *
 * vkrt_virtual_as.c - VirtualAS registry (design doc section 7).
 *
 * A virtual AS is a layer-owned record. Its VkAccelerationStructureKHR
 * handle is a layer-allocated token that is never passed to the ICD; the
 * real storage is an ordinary VkBuffer holding Mesa BVHIR (vk_ir_*).
 */

#include "vkrt_layer.h"
#include "vkrt_build_layout.h"

/* Alignment we give every BVHIR header so both host and device agree. */
#include "bvh_ir_contract.h"
#define VKRT_BVH_HEADER_ALIGN BVH_IR_NODE_OFFSET


VkResult
vkrt_create_virtual_as(struct vkrt_device_data *dev,
                       const VkAccelerationStructureCreateInfoKHR *ci,
                       VkAccelerationStructureKHR *out_as)
{
   struct vkrt_virtual_as *as =
      (struct vkrt_virtual_as *)calloc(1, sizeof(*as));
   if (!as)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   as->type            = ci->type;
   as->owner           = dev->device;
   as->backing_buffer  = ci->buffer;
   as->offset          = ci->offset;
   as->size            = ci->size;
   as->build_generation = 0;

   /* The handle is our own token. We deliberately do NOT call the ICD's
    * vkCreateAccelerationStructureKHR: the object never exists down there.
    * The token is the pointer value itself, which keeps the map cheap and
    * makes use-after-destroy show up as a map miss rather than an ICD
    * crash. */
   VkAccelerationStructureKHR token =
      (VkAccelerationStructureKHR)(uintptr_t)as;

   /* Device address of the header is the backing buffer's address + offset.
    * Resolved lazily on first query once the buffer is bound; for now record
    * what we can. */
   as->header_address = 0;
   as->node_address   = 0;
   as->leaf_address   = 0;

   vkrt_map_object(VKRT_HKEY(token), as);

   *out_as = token;
   VKRT_LOG_DBG("create_virtual_as type=%d size=%llu token=%p",
                (int)ci->type, (unsigned long long)ci->size, (void *)as);
   return VK_SUCCESS;
}

void
vkrt_destroy_virtual_as(struct vkrt_device_data *dev,
                        VkAccelerationStructureKHR handle)
{
   struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, handle);
   if (!as) {
      VKRT_LOG_WARN("destroy_virtual_as: unknown handle %p", (void *)handle);
      return;
   }
   vkrt_unmap_object(VKRT_HKEY(handle));
   free(as);
}

void
vkrt_as_build_sizes(struct vkrt_device_data *dev,
                    const VkAccelerationStructureBuildGeometryInfoKHR *pBuildInfo,
                    const uint32_t *pMaxPrimitiveCounts,
                    VkAccelerationStructureBuildSizesInfoKHR *pSizeInfo)
{
   struct vkrt_build_layout l = vkrt_build_layout(pBuildInfo, pMaxPrimitiveCounts, NULL);
   pSizeInfo->accelerationStructureSize = l.as_size;
   pSizeInfo->buildScratchSize = l.scratch_size;
   pSizeInfo->updateScratchSize = 0; /* refit is explicitly unsupported */
}

VkDeviceAddress
vkrt_as_device_address(struct vkrt_device_data *dev,
                       VkAccelerationStructureKHR handle)
{
   struct vkrt_virtual_as *as = VKRT_FIND(struct vkrt_virtual_as, handle);
   if (!as) {
      VKRT_LOG_WARN("as_device_address: unknown handle %p", (void *)handle);
      return 0;
   }

   if (as->header_address != 0)
      return as->header_address;

   /* Resolve the backing buffer's device address on first query. */
   VKRT_LOG_DBG("as_device_address: GBDA=%p backing=%p",
                (void *)(uintptr_t)dev->vtable.GetBufferDeviceAddress,
                (void *)as->backing_buffer);
   if (!dev->vtable.GetBufferDeviceAddress || !as->backing_buffer) {
      VKRT_LOG_ERR("as_device_address: cannot resolve backing buffer address (GBDA=%p backing=%p)",
                   (void *)(uintptr_t)dev->vtable.GetBufferDeviceAddress,
                   (void *)as->backing_buffer);
      return 0;
   }

   VkBufferDeviceAddressInfo info = {
      .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = as->backing_buffer,
   };
   VkDeviceAddress base = dev->vtable.GetBufferDeviceAddress(dev->device, &info);

   as->header_address = base + as->offset;
   as->node_address   = as->header_address + VKRT_BVH_HEADER_ALIGN;
   as->leaf_address   = as->node_address; /* refined at build time */

   VKRT_LOG_DBG("as_device_address %p -> 0x%llx", (void *)handle,
                (unsigned long long)as->header_address);
   return as->header_address;
}
