/* SPDX-License-Identifier: MIT */
#ifndef VKRT_CHAIN_H
#define VKRT_CHAIN_H
#include "vkrt_common.h"

static inline size_t vkrt_chain_size(VkStructureType type)
{
   switch (type) {
#include "vkrt_pnext_sizes.h"
   case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO: return sizeof(VkLayerDeviceCreateInfo);
   default: return 0;
   }
}
#endif
