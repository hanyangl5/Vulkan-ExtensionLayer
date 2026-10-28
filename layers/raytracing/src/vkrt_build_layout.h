/* SPDX-License-Identifier: MIT */
#ifndef VKRT_BUILD_LAYOUT_H
#define VKRT_BUILD_LAYOUT_H
#include "bvh_ir_build_layout.h"
#include "bvh4_abi.h"
#include "bvh4_mesa_gpu_abi.h"
#include <stdbool.h>

#define VKRT_BOUNDS_OFFSET 0u
struct vkrt_build_layout {
    struct bvh_ir_build_layout ir;
    VkDeviceSize nodes, triangles, aabbs, instances, parents, as_size;
    VkDeviceSize source, mapping, geometry_flags, status, scratch_size, leaf_count;
    bool valid;
};
static inline bool vkrt_layout_region(VkDeviceSize *cursor, VkDeviceSize count, VkDeviceSize stride, VkDeviceSize alignment,
                                      VkDeviceSize *offset) {
    if (*cursor > UINT64_MAX - (alignment - 1)) return false;
    VkDeviceSize start = (*cursor + alignment - 1) & ~(alignment - 1);
    if (count && stride > (UINT64_MAX - start) / count) return false;
    *offset = start;
    *cursor = start + count * stride;
    return true;
}
static inline struct vkrt_build_layout vkrt_build_layout(const VkAccelerationStructureBuildGeometryInfoKHR *info,
                                                         const uint32_t *counts,
                                                         const VkAccelerationStructureBuildRangeInfoKHR *ranges) {
    struct vkrt_build_layout l = {0};
    l.ir = bvh_ir_build_layout(info, counts, ranges);
    l.leaf_count = l.ir.leaf_count;
    uint32_t type = info->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR ? 2u : 0u;
    VkDeviceSize cursor = sizeof(VelRtBvh4Header), unused;
    VkDeviceSize internals = l.leaf_count > 1 ? l.leaf_count - 1 : 0;
    VkDeviceSize map_words = 0;
    if (l.leaf_count > VEL_RT_BVH4_MAX_COUNT) goto invalid;
    map_words = vel_rt_mesa_mapping_words((uint32_t)l.leaf_count);
    if (info->geometryCount) type = (info->pGeometries ? &info->pGeometries[0] : info->ppGeometries[0])->geometryType;
    if (type > 2u) goto invalid;
    if (!vkrt_layout_region(&cursor, internals, sizeof(VelRtBvh4Node), 128, &l.nodes) ||
        !vkrt_layout_region(&cursor, type == 0 ? l.leaf_count : 0, sizeof(VelRtBvh4Triangle), 16, &l.triangles) ||
        !vkrt_layout_region(&cursor, type == 1 ? l.leaf_count : 0, sizeof(VelRtBvh4Aabb), 16, &l.aabbs) ||
        !vkrt_layout_region(&cursor, type == 2 ? l.leaf_count : 0, sizeof(VelRtBvh4Instance), 16, &l.instances) ||
        !vkrt_layout_region(&cursor, l.leaf_count, 4, 16, &l.parents) || !vkrt_layout_region(&cursor, 0, 1, 256, &l.as_size))
        goto invalid;
    /* The source construction format and all encoder work maps are scratch,
     * not retained runtime AS storage. Every region is planned in uint64. */
    cursor = l.ir.scratch_size;
    if (!vkrt_layout_region(&cursor, l.ir.as_size, 1, 256, &l.source) ||
        !vkrt_layout_region(&cursor, map_words, 4, 256, &l.mapping) ||
        !vkrt_layout_region(&cursor, info->geometryCount, 4, 16, &l.geometry_flags) ||
        !vkrt_layout_region(&cursor, 1, 4, 16, &l.status) || !vkrt_layout_region(&cursor, 0, 1, 256, &unused))
        goto invalid;
    l.scratch_size = cursor;
    l.valid = true;
    return l;
invalid:
    l.as_size = l.scratch_size = UINT64_MAX;
    return l;
}
struct vkrt_bvh4_prepare_args {
    uint64_t output_as, status, geometry_flags;
    uint64_t nodes, triangles, aabbs, instances, parents, total_size;
    uint32_t type, mode, geometry, flags;
};
#ifdef __cplusplus
static_assert(sizeof(struct vkrt_bvh4_prepare_args) == 88, "BVH4 prepare push ABI");
static_assert(sizeof(VelRtMesaGpuEncodeArgs) == 80, "BVH4 encoder push ABI");
#else
_Static_assert(sizeof(struct vkrt_bvh4_prepare_args) == 88, "BVH4 prepare push ABI");
_Static_assert(sizeof(VelRtMesaGpuEncodeArgs) == 80, "BVH4 encoder push ABI");
#endif
#endif
