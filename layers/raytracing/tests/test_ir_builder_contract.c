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
struct ploc_capture {
    struct bvh_ir_build_layout layout;
    uint64_t output, scratch;
    uint32_t calls, ploc_calls, sort_groups, sort_calls;
};
static void emit_ploc(void *user, uint32_t stage, const void *data, uint32_t size, uint32_t groups) {
    struct ploc_capture *c = user;
    assert(groups);
    if (stage == BVH_IR_STAGE_INIT) {
        assert(c->calls == 0 && size == 24);
    } else if (stage == BVH_IR_STAGE_SORT) {
        const struct morton_sort_args *a = data;
        assert(size == 48 && a->pass == c->sort_calls++);
        c->sort_groups = groups;
    } else if (stage == BVH_IR_STAGE_PLOC) {
        const struct ploc_args *a = data;
        uint32_t passes = 0;
        for (uint32_t n = c->sort_groups - 1; n; n >>= 1) ++passes;
        assert(size == 48 && groups == 1 && c->sort_groups);
        assert(a->bvh == c->output + BVH_IR_NODE_OFFSET && a->header == c->output);
        assert(a->ids_0 == c->scratch + ((passes & 1) ? c->layout.ids_even : c->layout.ids_odd));
        assert(a->ids_1 == c->scratch + ((passes & 1) ? c->layout.ids_odd : c->layout.ids_even));
        assert(a->prefix_scan_partitions == c->scratch + c->layout.node_info);
        assert(a->internal_node_offset == c->layout.nodes - BVH_IR_NODE_OFFSET);
        ++c->ploc_calls;
    } else {
        assert(stage == BVH_IR_STAGE_LEAF); /* Neither LBVH topology stage may leak into PLOC. */
    }
    ++c->calls;
}
static void ploc_contract(void) {
    VkPhysicalDeviceLimits limits = {0};
    limits.maxComputeWorkGroupInvocations = limits.maxComputeWorkGroupSize[0] = 1024;
    limits.maxComputeSharedMemorySize = BVH_IR_PLOC_SHARED_BYTES;
    VkPhysicalDeviceSubgroupProperties subgroup = {0};
    subgroup.supportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
    subgroup.supportedOperations =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    for (uint32_t size = 32; size <= 128; size *= 2) {
        subgroup.subgroupSize = size;
        assert(bvh_ir_ploc_eligible(&limits, &subgroup));
    }
    subgroup.subgroupSize = 32;
    limits.maxComputeWorkGroupInvocations = 128;
    assert(!bvh_ir_ploc_eligible(&limits, &subgroup));
    limits.maxComputeWorkGroupInvocations = 1024;
    limits.maxComputeSharedMemorySize--;
    assert(!bvh_ir_ploc_eligible(&limits, &subgroup));
    limits.maxComputeSharedMemorySize++;
    subgroup.supportedOperations &= ~VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    assert(bvh_ir_ploc_eligible(&limits, &subgroup)); /* Local scan does not use subgroup arithmetic. */
    subgroup.supportedOperations &= ~VK_SUBGROUP_FEATURE_BALLOT_BIT;
    assert(!bvh_ir_ploc_eligible(&limits, &subgroup));
    subgroup.supportedOperations |= VK_SUBGROUP_FEATURE_BALLOT_BIT;
    subgroup.subgroupSize = 16;
    assert(bvh_ir_ploc_eligible(&limits, &subgroup));
    subgroup.subgroupSize = 24;
    assert(!bvh_ir_ploc_eligible(&limits, &subgroup));
    VkAccelerationStructureGeometryKHR geometry = {0};
    geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometry.geometry.aabbs.stride = 24;
    VkAccelerationStructureBuildGeometryInfoKHR info = {0};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    assert(bvh_ir_choose_backend(&info, 1025, true, false) == BVH_IR_BACKEND_PLOC);
    assert(bvh_ir_choose_backend(&info, 1025, false, false) == BVH_IR_BACKEND_LBVH);
    assert(bvh_ir_choose_backend(&info, 4, true, false) == BVH_IR_BACKEND_LBVH);
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    assert(bvh_ir_choose_backend(&info, 1025, true, false) == BVH_IR_BACKEND_LBVH);
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    assert(bvh_ir_choose_backend(&info, 1025, true, false) == BVH_IR_BACKEND_LBVH);
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    assert(bvh_ir_choose_backend(&info, 1025, true, false) == BVH_IR_BACKEND_PLOC);
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = 0;
    const uint32_t capacities[] = {0, 1, 2, 3, 4, 5, 1024, 1025, 65537, 1u << 20};
    for (uint32_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); ++i) {
        VkAccelerationStructureBuildRangeInfoKHR range = {0};
        range.primitiveCount = capacities[i];
        struct ploc_capture c = {0};
        c.layout = bvh_ir_build_layout(&info, NULL, &range);
        c.output = UINT64_C(0x100000000);
        c.scratch = UINT64_C(0x2000000000);
        uint64_t partitions = ((uint64_t)range.primitiveCount + 1023) / 1024;
        assert(c.layout.prefix_scan_partitions + partitions * 8 <= c.layout.counts);
        assert(bvh_ir_record_build_backend(&info, &range, c.output, c.scratch, 0, emit_ploc, &c, BVH_IR_BACKEND_PLOC) ==
               VK_SUCCESS);
        assert(c.ploc_calls == 1 && c.calls == (range.primitiveCount ? 3u : 2u) + c.sort_calls);
    }
    VkAccelerationStructureBuildRangeInfoKHR range = {0};
    range.primitiveCount = BVH_IR_MAX_LEAVES;
    struct ploc_capture c = {0};
    assert(!bvh_ir_ploc_count_eligible(range.primitiveCount));
    assert(bvh_ir_choose_backend(&info, range.primitiveCount, true, false) == BVH_IR_BACKEND_LBVH);
    assert(bvh_ir_record_build_backend(&info, &range, UINT64_C(0x100000000), UINT64_C(0x2000000000), 0, emit_ploc, &c,
                                       BVH_IR_BACKEND_PLOC) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
           c.calls == 0);
}
struct hploc_capture {
    struct bvh_ir_build_layout layout;
    uint64_t output, scratch;
    uint32_t calls, sort_groups, init_ranges, hploc_calls, sort_calls;
};
static void emit_hploc(void *user, uint32_t stage, const void *data, uint32_t size, uint32_t groups) {
    struct hploc_capture *c = user;
    assert(groups);
    if (stage == BVH_IR_STAGE_INIT) {
        assert(c->calls == 0 && size == 24);
    } else if (stage == BVH_IR_STAGE_SORT) {
        const struct morton_sort_args *a = data;
        assert(size == 48 && a->pass == c->sort_calls++);
        c->sort_groups = groups;
    } else if (stage == BVH_IR_STAGE_HPLOC_INIT) {
        const struct hploc_init_args *a = data;
        const uint64_t count = c->layout.leaf_count > 1 ? c->layout.leaf_count - 1 : 1;
        assert(c->sort_groups && !c->init_ranges && !c->hploc_calls);
        assert(size == 16 && a->range_count == count && groups == (count + 63) / 64);
        assert(a->ranges == c->scratch + c->layout.node_info);
        assert(a->ranges + count * 4 <= c->scratch + c->layout.counts);
        ++c->init_ranges;
    } else if (stage == BVH_IR_STAGE_HPLOC) {
        const struct hploc_args *a = data;
        uint32_t passes = 0;
        for (uint32_t n = c->sort_groups - 1; n; n >>= 1) ++passes;
        uint32_t expected_groups = (uint32_t)((c->layout.leaf_count + 31) / 32);
        assert(c->init_ranges == 1 && !c->hploc_calls && size == 40);
        assert(groups == (expected_groups ? expected_groups : 1));
        assert(a->bvh == c->output + BVH_IR_NODE_OFFSET && a->header == c->output);
        assert(a->ids == c->scratch + ((passes & 1) ? c->layout.ids_even : c->layout.ids_odd));
        assert(a->ranges == c->scratch + c->layout.hploc_ranges);
        assert(a->internal_node_base == c->layout.nodes - BVH_IR_NODE_OFFSET);
        ++c->hploc_calls;
    } else {
        assert(stage == BVH_IR_STAGE_LEAF); /* HPLOC owns both topology and bounds. */
    }
    ++c->calls;
}
static void hploc_contract(void) {
    VkPhysicalDeviceLimits limits = {0};
    limits.maxComputeWorkGroupInvocations = limits.maxComputeWorkGroupSize[0] = 32;
    limits.maxComputeSharedMemorySize = BVH_IR_HPLOC_SHARED_BYTES;
    assert(bvh_ir_hploc_eligible(&limits) && !bvh_ir_hploc_eligible(NULL));
    limits.maxComputeWorkGroupSize[0] = 31;
    assert(!bvh_ir_hploc_eligible(&limits));
    limits.maxComputeWorkGroupSize[0] = 32;
    limits.maxComputeWorkGroupInvocations = 31;
    assert(!bvh_ir_hploc_eligible(&limits));
    limits.maxComputeWorkGroupInvocations = 32;
    limits.maxComputeSharedMemorySize--;
    assert(!bvh_ir_hploc_eligible(&limits));
    VkAccelerationStructureGeometryKHR geometry = {0};
    geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometry.geometry.aabbs.stride = 24;
    VkAccelerationStructureBuildGeometryInfoKHR info = {0};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    assert(bvh_ir_choose_backend(&info, 1024, true, true) == BVH_IR_BACKEND_PLOC);
    assert(bvh_ir_choose_backend(&info, 1025, true, true) == BVH_IR_BACKEND_HPLOC);
    assert(bvh_ir_choose_backend(&info, 1025, false, true) == BVH_IR_BACKEND_HPLOC);
    assert(bvh_ir_choose_backend(&info, 4, true, true) == BVH_IR_BACKEND_LBVH);
    assert(bvh_ir_choose_backend(&info, 33, false, true) == BVH_IR_BACKEND_LBVH);
    assert(bvh_ir_choose_backend(&info, BVH_IR_MAX_LEAVES, true, true) == BVH_IR_BACKEND_HPLOC);
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    assert(bvh_ir_choose_backend(&info, 2048, true, true) == BVH_IR_BACKEND_LBVH);
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    assert(bvh_ir_choose_backend(&info, 2048, true, true) == BVH_IR_BACKEND_LBVH);
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    assert(bvh_ir_choose_backend(&info, 2048, true, true) == BVH_IR_BACKEND_HPLOC);
    const uint32_t capacities[] = {0, 1, 2, 5, 31, 32, 33, 255, 256, 257, 1024, 1025, 65537, 1u << 20, BVH_IR_MAX_LEAVES};
    for (uint32_t width = 0; width < 2; ++width) {
        info.flags = width ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR : 0;
        for (uint32_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); ++i) {
            VkAccelerationStructureBuildRangeInfoKHR range = {0};
            range.primitiveCount = capacities[i];
            struct hploc_capture c = {0};
            c.output = UINT64_C(0x100000000);
            c.scratch = UINT64_C(0x2000000000);
            c.layout = bvh_ir_build_layout(&info, NULL, &range);
            const struct bvh_ir_build_layout queried = bvh_ir_build_layout(&info, &range.primitiveCount, NULL);
            assert(c.layout.valid && queried.valid && c.layout.as_size == queried.as_size &&
                   c.layout.scratch_size == queried.scratch_size && c.layout.ids_odd == queried.ids_odd &&
                   c.layout.node_info == queried.node_info && c.layout.hploc_ranges == queried.hploc_ranges);
            assert(c.layout.key_stride == (width ? 12 : 8));
            uint64_t allocated = range.primitiveCount ? range.primitiveCount : 1;
            assert(c.layout.ids_even + allocated * c.layout.key_stride <= c.layout.ids_odd);
            assert(c.layout.ids_odd + allocated * c.layout.key_stride <= c.layout.node_info);
            assert(c.layout.prefix_scan_partitions == c.layout.hploc_ranges);
            assert(bvh_ir_record_build_backend(&info, &range, c.output, c.scratch, 0, emit_hploc, &c, BVH_IR_BACKEND_HPLOC) ==
                   VK_SUCCESS);
            assert(c.init_ranges == 1 && c.hploc_calls == 1 && c.calls == (range.primitiveCount ? 4u : 3u) + c.sort_calls);
            c.calls = 0;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            assert(bvh_ir_record_build_backend(&info, &range, c.output, c.scratch, 0, emit_hploc, &c, BVH_IR_BACKEND_HPLOC) !=
                       VK_SUCCESS &&
                   !c.calls);
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            assert(bvh_ir_record_build_backend(&info, &range, UINT64_MAX - 128, c.scratch, 0, emit_hploc, &c,
                                               BVH_IR_BACKEND_HPLOC) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                   !c.calls);
        }
    }
    info.flags = 0;
    uint32_t leaves = 32;
    struct bvh_ir_build_layout narrow = bvh_ir_build_layout(&info, &leaves, NULL);
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    struct bvh_ir_build_layout wide = bvh_ir_build_layout(&info, &leaves, NULL);
    assert(narrow.ids_odd == 256 && wide.ids_odd == 512);
    assert(narrow.node_info == 512 && wide.node_info == 1024);
    assert(narrow.as_size == wide.as_size && wide.scratch_size == narrow.scratch_size + 512);
}
int main(void) {
    ploc_contract();
    hploc_contract();
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
        assert(l.nodes == 128 + (n + 1) * 68);  // no 8-byte padding between leaves and boxes
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
       assert(large.calls == 26); /* One initial local sort plus 21 global merge passes. */
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
