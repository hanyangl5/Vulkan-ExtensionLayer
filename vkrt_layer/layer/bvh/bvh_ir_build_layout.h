/* SPDX-License-Identifier: MIT */
#ifndef BVH_IR_BUILD_LAYOUT_H
#define BVH_IR_BUILD_LAYOUT_H
#include "bvh_ir_contract.h"
#include "vk_bvh_defines.h"
#include <stddef.h>

#ifdef __cplusplus
#define BVH_IR_STATIC_ASSERT static_assert
#else
#define BVH_IR_STATIC_ASSERT _Static_assert
#endif

/* Offsets are part of the traversal ABI; nodes use scalar GLSL layout. */
BVH_IR_STATIC_ASSERT(sizeof(struct vk_ir_box_node) == 40, "box ABI");
BVH_IR_STATIC_ASSERT(sizeof(struct vk_ir_triangle_node) == 68, "triangle ABI");
BVH_IR_STATIC_ASSERT(sizeof(struct vk_ir_instance_node) == 96, "instance ABI");
BVH_IR_STATIC_ASSERT(offsetof(struct vk_ir_header, dst_leaf_node_offset) == 80,
                     "Mesa encoder leaf offset ABI");
BVH_IR_STATIC_ASSERT(offsetof(struct leaf_args, primitive_count) == 76, "leaf push ABI");

static inline VkDeviceSize bvh_ir_align_size(VkDeviceSize v, VkDeviceSize a) {
   return (v + a - 1) & ~(a - 1);
}
static inline uint32_t bvh_ir_leaf_stride(VkGeometryTypeKHR type) {
   return type == VK_GEOMETRY_TYPE_TRIANGLES_KHR ? 68 : type == VK_GEOMETRY_TYPE_AABBS_KHR ? 32 : 96;
}
struct bvh_ir_build_layout {
   VkDeviceSize nodes, as_size;
   VkDeviceSize ids_even, ids_odd, node_info, counts, scratch_size;
   VkDeviceSize leaf_count;
};
static inline struct bvh_ir_build_layout
bvh_ir_build_layout(const VkAccelerationStructureBuildGeometryInfoKHR *info, const uint32_t *counts,
                    const VkAccelerationStructureBuildRangeInfoKHR *ranges) {
   struct bvh_ir_build_layout l = {0};
   VkDeviceSize leaves = 0;
   for (uint32_t i = 0; i < info->geometryCount; ++i) {
      const VkAccelerationStructureGeometryKHR *g =
          info->pGeometries ? &info->pGeometries[i] : info->ppGeometries[i];
      uint32_t n = counts ? counts[i] : ranges[i].primitiveCount;
      l.leaf_count += n;
      leaves += (VkDeviceSize)n * bvh_ir_leaf_stride(g->geometryType);
   }
   VkDeviceSize n = l.leaf_count ? l.leaf_count : 1;
   l.nodes = BVH_IR_NODE_OFFSET + leaves;
   l.as_size = bvh_ir_align_size(l.nodes + (n > 1 ? n - 1 : 1) * 40, 256);
   l.ids_even = 0;
   l.ids_odd = bvh_ir_align_size(n * 8, 256);
   l.node_info = bvh_ir_align_size(l.ids_odd + n * 8, 256);
   l.counts = bvh_ir_align_size(l.node_info + n * 16, 256);
   l.scratch_size = l.counts + 256;
   return l;
}
#undef BVH_IR_STATIC_ASSERT
#endif
