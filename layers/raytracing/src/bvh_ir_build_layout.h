/* SPDX-License-Identifier: MIT */
#ifndef BVH_IR_BUILD_LAYOUT_H
#define BVH_IR_BUILD_LAYOUT_H
#include "bvh_ir_contract.h"
#include "vk_bvh_defines.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
#define BVH_IR_STATIC_ASSERT static_assert
#else
#define BVH_IR_STATIC_ASSERT _Static_assert
#endif

/* Internal construction ABI; records use scalar GLSL layout. */
BVH_IR_STATIC_ASSERT(sizeof(struct vk_ir_box_node) == 40, "box ABI");
BVH_IR_STATIC_ASSERT(sizeof(struct vk_ir_triangle_node) == 68, "triangle ABI");
BVH_IR_STATIC_ASSERT(sizeof(struct vk_ir_instance_node) == 96, "instance ABI");
BVH_IR_STATIC_ASSERT(offsetof(struct vk_ir_header, dst_leaf_node_offset) == 80,
                     "Mesa encoder leaf offset ABI");
BVH_IR_STATIC_ASSERT(offsetof(struct leaf_args, primitive_count) == 80, "leaf push ABI");
BVH_IR_STATIC_ASSERT(sizeof(struct leaf_args) == 96, "wide leaf push ABI");
BVH_IR_STATIC_ASSERT(sizeof(struct ploc_args) == 48, "wide PLOC push ABI");
BVH_IR_STATIC_ASSERT(sizeof(struct vk_prefix_scan_partition) == 8, "PLOC partition ABI");
BVH_IR_STATIC_ASSERT(offsetof(struct vk_ir_header, sync_data) == 44, "Mesa phase sentinel ABI");
BVH_IR_STATIC_ASSERT(offsetof(struct vk_ir_header, driver_internal) + 5 * sizeof(uint32_t) == BVH_IR_PRODUCER_OFFSET,
                     "Mesa producer_id ABI");

static inline VkDeviceSize bvh_ir_align_size(VkDeviceSize v, VkDeviceSize a) {
    if (v > UINT64_MAX - (a - 1)) return UINT64_MAX;
    return (v + a - 1) & ~(a - 1);
}
static inline uint32_t bvh_ir_leaf_stride(VkGeometryTypeKHR type) {
   return type == VK_GEOMETRY_TYPE_TRIANGLES_KHR ? 68 : type == VK_GEOMETRY_TYPE_AABBS_KHR ? 32 : 96;
}
struct bvh_ir_build_layout {
   VkDeviceSize nodes, as_size;
   VkDeviceSize ids_even, ids_odd, node_info, prefix_scan_partitions, counts, scratch_size;
   VkDeviceSize leaf_count;
   bool valid;
};
static inline struct bvh_ir_build_layout
bvh_ir_build_layout(const VkAccelerationStructureBuildGeometryInfoKHR *info, const uint32_t *counts,
                    const VkAccelerationStructureBuildRangeInfoKHR *ranges) {
   struct bvh_ir_build_layout l = {0};
   VkDeviceSize leaves = 0, n = 0;
   if (!info || (info->geometryCount && ((!counts && !ranges) || (!info->pGeometries && !info->ppGeometries)))) goto invalid;
   for (uint32_t i = 0; i < info->geometryCount; ++i) {
      const VkAccelerationStructureGeometryKHR *g =
          info->pGeometries ? &info->pGeometries[i] : info->ppGeometries[i];
      if (!g) goto invalid;
      uint32_t n = counts ? counts[i] : ranges[i].primitiveCount;
      VkDeviceSize stride = bvh_ir_leaf_stride(g->geometryType);
      if (n > UINT64_MAX - l.leaf_count || n > (UINT64_MAX - leaves) / stride) goto invalid;
      l.leaf_count += n;
      leaves += (VkDeviceSize)n * stride;
   }
   n = l.leaf_count ? l.leaf_count : 1;
   if (leaves > UINT64_MAX - BVH_IR_NODE_OFFSET) goto invalid;
   l.nodes = BVH_IR_NODE_OFFSET + leaves;
   if ((n > 1 ? n - 1 : 1) > (UINT64_MAX - l.nodes) / 40) goto invalid;
   l.as_size = bvh_ir_align_size(l.nodes + (n > 1 ? n - 1 : 1) * 40, 256);
   if (l.as_size == UINT64_MAX || n > UINT64_MAX / 16) goto invalid;
   l.ids_even = 0;
   l.ids_odd = bvh_ir_align_size(n * 8, 256);
   if (l.ids_odd == UINT64_MAX || n * 8 > UINT64_MAX - l.ids_odd) goto invalid;
   l.node_info = bvh_ir_align_size(l.ids_odd + n * 8, 256);
   if (l.node_info == UINT64_MAX || n * 16 > UINT64_MAX - l.node_info) goto invalid;
   /* Sort has finished before either internal builder runs. The LBVH node
    * info allocation (16 bytes/leaf) also covers PLOC's 8-byte partition per
    * 1024 leaves, including the empty build's single allocated slot. */
   l.prefix_scan_partitions = l.node_info;
   l.counts = bvh_ir_align_size(l.node_info + n * 16, 256);
   if (l.counts > UINT64_MAX - 256) goto invalid;
   l.scratch_size = l.counts + 256;
   l.valid = true;
   return l;
invalid:
    l.as_size = l.scratch_size = UINT64_MAX;
    return l;
}
#undef BVH_IR_STATIC_ASSERT
#endif
