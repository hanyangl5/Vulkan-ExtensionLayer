/* SPDX-License-Identifier: MIT */
#include "bvh_ir_builder.h"
#ifdef NDEBUG
#undef NDEBUG /* These checks must also run in the default Release CTest build. */
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct capture {
   unsigned calls, leaves;
   uint64_t output, scratch, refs;
   uint32_t capacity, stride;
};
struct large_capture {
    unsigned calls;
    uint32_t stride;
    uint64_t internal_base;
};
static void emit_large(void *user, uint32_t stage, const void *data, uint32_t size, uint32_t groups) {
    struct large_capture *c = user;
    assert(groups);
    if (stage == BVH_IR_STAGE_LEAF) {
        const struct leaf_args *a = data;
        assert(size == 96 && a->primitive_count == 0x1fffffffu && a->leaf_offset == 0);
    } else if (stage == BVH_IR_STAGE_TREE) {
        const struct lbvh_main_args *a = data;
        assert(a->internal_node_base == c->internal_base && a->internal_node_base > UINT32_MAX);
    } else if (stage == BVH_IR_STAGE_BOUNDS) {
        const struct lbvh_generate_ir_args *a = data;
        assert(a->internal_node_base == c->internal_base && a->internal_node_base > UINT32_MAX);
    }
    ++c->calls;
}
static void emit(void *user, uint32_t stage, const void *data, uint32_t size, uint32_t groups) {
   struct capture *c = user;
   assert(groups > 0);
   if (stage == BVH_IR_STAGE_INIT) {
      uint64_t addresses[2];
      uint32_t metadata[2];
      assert(c->calls == 0 && size == 24);
      memcpy(addresses, data, 16);
      memcpy(metadata, (const char *)data + 16, 8);
      assert(addresses[0] == c->output && metadata[0] == c->capacity);
   } else if (stage == BVH_IR_STAGE_LEAF) {
      const struct leaf_args *a = data;
      assert(size == sizeof(*a));
      assert(a->bvh == c->output + BVH_IR_NODE_OFFSET && a->header == c->output);
      assert(a->leaf_offset == c->leaves * c->stride);
      assert(a->geom_data.first_id == c->leaves);
      assert(a->blas_ir_addresses == c->refs);
      c->leaves += a->primitive_count;
   } else if (stage == BVH_IR_STAGE_SORT) {
      const struct morton_sort_args *a = data;
      assert(a->header == c->output && a->bvh == c->output + BVH_IR_NODE_OFFSET);
      assert(a->leaf_count == c->capacity && c->leaves == c->capacity);
   } else if (stage == BVH_IR_STAGE_TREE) {
      const struct lbvh_main_args *a = data;
      assert(a->bvh == c->output + BVH_IR_NODE_OFFSET);
      assert(a->internal_node_base == c->capacity * c->stride);
   } else {
      const struct lbvh_generate_ir_args *a = data;
      assert(stage == BVH_IR_STAGE_BOUNDS);
      assert(a->bvh == c->output + BVH_IR_NODE_OFFSET);
      assert(a->internal_node_base == c->capacity * c->stride);
   }
   c->calls++;
}
int main(void) {
   for (unsigned n = 0; n < 5; ++n) {
      VkAccelerationStructureGeometryKHR g[2] = {{0}, {0}};
      for (unsigned i = 0; i < 2; ++i) {
         g[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
         g[i].geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
         g[i].geometry.triangles.vertexStride = 12;
         g[i].geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
      }
      VkAccelerationStructureBuildGeometryInfoKHR info = {0};
      info.geometryCount = 2;
      info.pGeometries = g;
      info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
      VkAccelerationStructureBuildRangeInfoKHR ranges[2] = {{0}, {0}};
      ranges[0].primitiveCount = n;
      ranges[1].primitiveCount = 1;
      struct capture c = {0, 0, 0x100000, 0x200000, 0, n + 1, 68};
      struct bvh_ir_build_layout l = bvh_ir_build_layout(&info, NULL, ranges);
      assert(l.nodes == 128 + (n + 1) * 68); // no 8-byte padding between leaves and boxes
      assert(bvh_ir_record_build(&info, ranges, c.output, c.scratch, 0, emit, &c) == VK_SUCCESS);
      assert(c.calls == (n ? 6u : 5u));
      info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
      unsigned before = c.calls;
      assert(bvh_ir_record_build(&info, ranges, c.output, c.scratch, 0, emit, &c) != VK_SUCCESS);
      assert(c.calls == before);
   }
   VkAccelerationStructureGeometryKHR g = {0};
   g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
   VkAccelerationStructureBuildGeometryInfoKHR info = {0};
   info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
   info.geometryCount = 1;
   info.pGeometries = &g;
   VkAccelerationStructureBuildRangeInfoKHR range = {0};
   range.primitiveCount = 3;
   struct capture c = {0, 0, 0x100000, 0x200000, 0x300000, 3, 96};
   assert(bvh_ir_record_build(&info, &range, c.output, c.scratch, c.refs, emit, &c) == VK_SUCCESS);
   assert(c.calls == 5);
   for (uint32_t type = 0; type < 3; ++type) {
       memset(&g, 0, sizeof(g));
       g.geometryType = (VkGeometryTypeKHR)type;
       g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
       g.geometry.triangles.vertexStride = 12;
       g.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
       if (type == 1) g.geometry.aabbs.stride = 24;
       info.type = type == 2 ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
       range.primitiveCount = 0x1fffffffu;
       const uint32_t strides[] = {68, 32, 96};
       struct large_capture large = {0, strides[type], UINT64_C(0x1fffffff) * strides[type]};
       const struct bvh_ir_build_layout layout = bvh_ir_build_layout(&info, NULL, &range);
       assert(layout.valid && layout.as_size > UINT32_MAX && layout.scratch_size > UINT32_MAX);
       assert(bvh_ir_record_build(&info, &range, UINT64_C(0x100000000), UINT64_C(0x2000000000), 0, emit_large, &large) ==
              VK_SUCCESS);
       assert(large.calls == 5);
       unsigned before = large.calls;
       assert(bvh_ir_record_build(&info, &range, UINT64_MAX - 128, UINT64_C(0x2000000000), 0, emit_large, &large) ==
                  VK_ERROR_OUT_OF_DEVICE_MEMORY &&
              large.calls == before);
       range.primitiveCount++;
       assert(bvh_ir_record_build(&info, &range, UINT64_C(0x100000000), UINT64_C(0x2000000000), 0, emit_large, &large) ==
                  VK_ERROR_OUT_OF_DEVICE_MEMORY &&
              large.calls == before);
   }
   puts("PASS: backend-independent builder command stream and common layout");
   return 0;
}
