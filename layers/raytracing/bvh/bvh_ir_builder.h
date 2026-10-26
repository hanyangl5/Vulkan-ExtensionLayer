/* SPDX-License-Identifier: MIT */
#ifndef BVH_IR_BUILDER_H
#define BVH_IR_BUILDER_H
#include "bvh_ir_build_layout.h"
#include <stdbool.h>
#include <string.h>

enum bvh_ir_stage {
   BVH_IR_STAGE_INIT,
   BVH_IR_STAGE_LEAF,
   BVH_IR_STAGE_SORT,
   BVH_IR_STAGE_TREE,
   BVH_IR_STAGE_BOUNDS,
   BVH_IR_STAGE_COUNT
};

/* emit must bind the corresponding common shader, push these bytes, dispatch
 * groups x 1 x 1, and make its writes visible before the next emit call.
 * output_ir is independent of the final hardware AS. Both consumers use this
 * exact builder and schema; no backend or old-format detection occurs here.
 * For TLAS, blas_ir_addresses names a per-instance array of common BLAS IR
 * addresses. Zero explicitly binds each instance AS reference as its IR address.
 * Each instance base_ptr retains its consumer's VkAS device address. */
typedef void (*bvh_ir_emit_fn)(void *user, uint32_t stage, const void *args, uint32_t size, uint32_t groups);

static inline bool bvh_ir_supported_geometry(const VkAccelerationStructureGeometryKHR *g) {
   if (g->pNext)
      return false;
   if (g->geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR)
      return !g->geometry.instances.pNext;
   if (g->geometryType == VK_GEOMETRY_TYPE_AABBS_KHR)
      return !g->geometry.aabbs.pNext && g->geometry.aabbs.stride <= UINT32_MAX;
   if (g->geometryType != VK_GEOMETRY_TYPE_TRIANGLES_KHR)
      return false;
   const VkAccelerationStructureGeometryTrianglesDataKHR *t = &g->geometry.triangles;
   if (t->pNext || t->vertexStride > UINT32_MAX ||
       (t->indexType != VK_INDEX_TYPE_NONE_KHR && t->indexType != VK_INDEX_TYPE_UINT16 &&
        t->indexType != VK_INDEX_TYPE_UINT32 && t->indexType != VK_INDEX_TYPE_UINT8_EXT))
      return false;
   switch (t->vertexFormat) {
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16B16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return true;
   default:
      return false;
   }
}

static inline VkResult bvh_ir_record_build(const VkAccelerationStructureBuildGeometryInfoKHR *info,
                                           const VkAccelerationStructureBuildRangeInfoKHR *ranges,
                                           VkDeviceAddress address, VkDeviceAddress scratch,
                                           VkDeviceAddress blas_ir_addresses, bvh_ir_emit_fn emit,
                                           void *user) {
   if (!info || !ranges || !emit || !address || !scratch ||
       info->mode != VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR)
      return VK_ERROR_INITIALIZATION_FAILED;
   uint32_t type = info->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR ? 2u : 0u;
   for (uint32_t g = 0; g < info->geometryCount; ++g) {
      const VkAccelerationStructureGeometryKHR *geom =
          info->pGeometries ? &info->pGeometries[g] : info->ppGeometries[g];
      if (g == 0)
         type = geom->geometryType;
      if (geom->geometryType != type || !bvh_ir_supported_geometry(geom))
         return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   struct bvh_ir_build_layout l = bvh_ir_build_layout(info, NULL, ranges);
   if (l.as_size >= UINT32_MAX || l.leaf_count >= (1u << 29))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   const VkAccelerationStructureGeometryKHR *first_geom =
       info->geometryCount ? (info->pGeometries ? &info->pGeometries[0] : info->ppGeometries[0]) : NULL;
   uint32_t geometry_type = first_geom ? first_geom->geometryType
                                       : (info->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                                              ? VK_GEOMETRY_TYPE_INSTANCES_KHR
                                              : VK_GEOMETRY_TYPE_TRIANGLES_KHR);
   struct {
      uint64_t bvh, counts;
      uint32_t capacity, geometry_type;
   } init = {address, scratch + l.counts, (uint32_t)l.leaf_count, geometry_type};
   emit(user, BVH_IR_STAGE_INIT, &init, sizeof(init), 1);
   uint32_t leaf_offset = 0, first = 0;
   for (uint32_t g = 0; g < info->geometryCount; ++g) {
      const VkAccelerationStructureGeometryKHR *geom =
          info->pGeometries ? &info->pGeometries[g] : info->ppGeometries[g];
      const VkAccelerationStructureBuildRangeInfoKHR *range = &ranges[g];
      struct leaf_args args;
      memset(&args, 0, sizeof(args));
      args.bvh = address + BVH_IR_NODE_OFFSET;
      args.header = address;
      args.ids = scratch + l.ids_even;
      args.leaf_offset = leaf_offset;
      args.primitive_count = range->primitiveCount;
      args.blas_ir_addresses = blas_ir_addresses;
      vk_bvh_geometry_data *data = &args.geom_data;
      data->geometry_id = g | ((geom->flags & VK_GEOMETRY_OPAQUE_BIT_KHR) ? VK_GEOMETRY_OPAQUE : 0);
      data->geometry_type = geom->geometryType;
      data->first_id = first;
      if (geom->geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
         const VkAccelerationStructureGeometryTrianglesDataKHR *t = &geom->geometry.triangles;
         data->data = t->vertexData.deviceAddress + (VkDeviceSize)range->firstVertex * t->vertexStride;
         data->indices = t->indexData.deviceAddress;
         if (t->indexType == VK_INDEX_TYPE_NONE_KHR)
            data->data += range->primitiveOffset;
         else
            data->indices += range->primitiveOffset;
         data->transform =
             t->transformData.deviceAddress ? t->transformData.deviceAddress + range->transformOffset : 0;
         data->stride = (uint32_t)t->vertexStride;
         data->vertex_format = t->vertexFormat;
         data->index_format = t->indexType;
      } else if (geom->geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
         data->data = geom->geometry.aabbs.data.deviceAddress + range->primitiveOffset;
         data->stride = (uint32_t)geom->geometry.aabbs.stride;
      } else if (geom->geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
         data->data = geom->geometry.instances.data.deviceAddress + range->primitiveOffset;
         data->stride = geom->geometry.instances.arrayOfPointers ? 8 : 64;
      } else {
         return VK_ERROR_FEATURE_NOT_PRESENT;
      }
      if (range->primitiveCount)
         emit(user, BVH_IR_STAGE_LEAF, &args, sizeof(args), (range->primitiveCount + 63) / 64);
      first += range->primitiveCount;
      leaf_offset += range->primitiveCount * bvh_ir_leaf_stride(geom->geometryType);
   }

   uint32_t groups = (l.leaf_count + 255) / 256;
   if (!groups)
      groups = 1;
   struct morton_sort_args sort;
   memset(&sort, 0, sizeof(sort));
   sort.bvh = address + BVH_IR_NODE_OFFSET;
   sort.header = address;
   sort.ids_even = scratch + l.ids_even;
   sort.ids_odd = scratch + l.ids_odd;
   sort.counts = scratch + l.counts;
   sort.leaf_count = (uint32_t)l.leaf_count;
   emit(user, BVH_IR_STAGE_SORT, &sort, sizeof(sort), groups);
   uint32_t merge_passes = 0;
   for (uint32_t n = groups - 1; n; n >>= 1)
      ++merge_passes;
   VkDeviceAddress sorted = scratch + ((merge_passes & 1) ? l.ids_even : l.ids_odd);
   struct lbvh_main_args tree;
   memset(&tree, 0, sizeof(tree));
   tree.bvh = address + BVH_IR_NODE_OFFSET;
   tree.src_ids = sorted;
   tree.node_info = scratch + l.node_info;
   tree.header = address;
   tree.internal_node_base = (uint32_t)(l.nodes - BVH_IR_NODE_OFFSET);
   uint32_t internals = l.leaf_count > 1 ? l.leaf_count - 1 : 1;
   emit(user, BVH_IR_STAGE_TREE, &tree, sizeof(tree), (internals + 63) / 64);
   struct lbvh_generate_ir_args bounds;
   memset(&bounds, 0, sizeof(bounds));
   bounds.bvh = address + BVH_IR_NODE_OFFSET;
   bounds.node_info = scratch + l.node_info;
   bounds.header = address;
   bounds.internal_node_base = (uint32_t)(l.nodes - BVH_IR_NODE_OFFSET);
   emit(user, BVH_IR_STAGE_BOUNDS, &bounds, sizeof(bounds), (internals + 63) / 64);
   return VK_SUCCESS;
}
#endif
