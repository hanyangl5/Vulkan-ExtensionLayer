/* SPDX-License-Identifier: MIT */
#ifndef BVH_IR_BUILDER_H
#define BVH_IR_BUILDER_H
#include "bvh_ir_build_layout.h"
#include "bvh_vertex_format.h"
#include <stdbool.h>
#include <string.h>

enum bvh_ir_stage {
    BVH_IR_STAGE_INIT,
    BVH_IR_STAGE_LEAF,
    BVH_IR_STAGE_SORT,
    BVH_IR_STAGE_TREE,
    BVH_IR_STAGE_BOUNDS,
    BVH_IR_STAGE_PLOC,
    BVH_IR_STAGE_HPLOC_INIT,
    BVH_IR_STAGE_HPLOC,
    BVH_IR_STAGE_COUNT
};

enum bvh_ir_backend { BVH_IR_BACKEND_LBVH, BVH_IR_BACKEND_PLOC, BVH_IR_BACKEND_HPLOC };

/* The public HPLOC port uses a fixed logical workgroup, not native subgroup
 * collectives. Its conservative LDS bound includes padded vec3 AABBs. This is
 * below the existing 128-invocation baseline and adds no subgroup features. */
#define BVH_IR_HPLOC_SHARED_BYTES 2048u
static inline bool bvh_ir_hploc_eligible(const VkPhysicalDeviceLimits *limits) {
    return limits && limits->maxComputeWorkGroupInvocations >= HPLOC_WORKGROUP_SIZE &&
           limits->maxComputeWorkGroupSize[0] >= HPLOC_WORKGROUP_SIZE &&
           limits->maxComputeSharedMemorySize >= BVH_IR_HPLOC_SHARED_BYTES;
}

/* Pinned Mesa 837ab0e PLOC uses a 1024-lane workgroup. The public adapter's
 * local compaction scan follows logical workgroup order, independent of the
 * implementation's subgroup lane layout. Only the baseline's compute subgroup
 * support is required; no arithmetic/full-subgroup feature is needed. The LDS
 * bound permits padded vec3 struct elements. */
#define BVH_IR_PLOC_SHARED_BYTES (40u * 1024u)
static inline bool bvh_ir_ploc_eligible(const VkPhysicalDeviceLimits *limits, const VkPhysicalDeviceSubgroupProperties *subgroup) {
    if (!limits || !subgroup) return false;
    const VkSubgroupFeatureFlags required = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT;
    uint32_t size = subgroup->subgroupSize;
    return limits->maxComputeWorkGroupInvocations >= PLOC_WORKGROUP_SIZE &&
           limits->maxComputeWorkGroupSize[0] >= PLOC_WORKGROUP_SIZE &&
           limits->maxComputeSharedMemorySize >= BVH_IR_PLOC_SHARED_BYTES &&
           (subgroup->supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && (subgroup->supportedOperations & required) == required &&
           size >= 1 && size <= 128 && (size & (size - 1)) == 0 && PLOC_WORKGROUP_SIZE % size == 0;
}

/* Each merge removes at least one leaf. Even the pessimistic one-merge per
 * iteration bound must leave the uint32 task counters below their sentinel.
 * The upper bound includes both phases and padding in every iteration. */
static inline bool bvh_ir_ploc_count_eligible(uint32_t leaves) {
    uint64_t tasks = (uint64_t)leaves * ((uint64_t)leaves + 1) / PLOC_WORKGROUP_SIZE + (uint64_t)leaves * 2;
    return tasks < UINT32_MAX;
}

static inline enum bvh_ir_backend bvh_ir_choose_backend(const VkAccelerationStructureBuildGeometryInfoKHR *info, uint32_t leaves,
                                                        bool ploc_eligible, bool hploc_eligible) {
    if (leaves <= 4) return BVH_IR_BACKEND_LBVH;
    /* Preserve the pinned common builder's quality/fast-build policy. UPDATE
     * itself remains the independent topology-preserving runtime refit. */
    if (info->type != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR &&
        (info->flags &
         (VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR)))
        return BVH_IR_BACKEND_LBVH;
    /* Mesa's common host leaves HPLOC selection to a driver hook. This public
     * adapter keeps PLOC for small quality builds and selects the scalable
     * range-arrival HPLOC path above one PLOC workgroup's requested capacity.
     * Lack of either optional builder always retains a correct alternative. */
    if (leaves > PLOC_WORKGROUP_SIZE && hploc_eligible) return BVH_IR_BACKEND_HPLOC;
    return ploc_eligible && bvh_ir_ploc_count_eligible(leaves) ? BVH_IR_BACKEND_PLOC : BVH_IR_BACKEND_LBVH;
}

/* emit must bind the corresponding common shader, push these bytes, cover the
 * logical group count with a checked XYZ grid, and make its writes visible
 * before the next emit call. Shaders flatten the grid before range checking.
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
   return bvh_supported_vertex_format(t->vertexFormat);
}

static inline VkResult bvh_ir_record_build_backend(const VkAccelerationStructureBuildGeometryInfoKHR *info,
                                                   const VkAccelerationStructureBuildRangeInfoKHR *ranges, VkDeviceAddress address,
                                                   VkDeviceAddress scratch, VkDeviceAddress blas_ir_addresses, bvh_ir_emit_fn emit,
                                                   void *user, enum bvh_ir_backend backend) {
    if (!info || !ranges || !emit || !address || !scratch || info->mode != VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR ||
        (backend != BVH_IR_BACKEND_LBVH && backend != BVH_IR_BACKEND_PLOC && backend != BVH_IR_BACKEND_HPLOC))
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t type = info->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR ? 2u : 0u;
    for (uint32_t g = 0; g < info->geometryCount; ++g) {
        const VkAccelerationStructureGeometryKHR *geom = info->pGeometries ? &info->pGeometries[g] : info->ppGeometries[g];
        if (g == 0) type = geom->geometryType;
        if ((uint32_t)geom->geometryType != type || !bvh_ir_supported_geometry(geom)) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct bvh_ir_build_layout l = bvh_ir_build_layout(info, NULL, ranges);
    if (!l.valid || l.leaf_count > BVH_IR_MAX_LEAVES || address > UINT64_MAX - l.as_size || scratch > UINT64_MAX - l.scratch_size)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (backend == BVH_IR_BACKEND_PLOC && !bvh_ir_ploc_count_eligible((uint32_t)l.leaf_count)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const VkAccelerationStructureGeometryKHR *first_geom =
        info->geometryCount ? (info->pGeometries ? &info->pGeometries[0] : info->ppGeometries[0]) : NULL;
    uint32_t geometry_type = first_geom
                                 ? first_geom->geometryType
                                 : (info->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR ? VK_GEOMETRY_TYPE_INSTANCES_KHR
                                                                                               : VK_GEOMETRY_TYPE_TRIANGLES_KHR);
    struct {
        uint64_t bvh, counts;
        uint32_t capacity, geometry_type;
    } init = {address, scratch + l.counts, (uint32_t)l.leaf_count, geometry_type};
    emit(user, BVH_IR_STAGE_INIT, &init, sizeof(init), 1);
    uint64_t leaf_offset = 0;
    uint32_t first = 0;
    for (uint32_t g = 0; g < info->geometryCount; ++g) {
        const VkAccelerationStructureGeometryKHR *geom = info->pGeometries ? &info->pGeometries[g] : info->ppGeometries[g];
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
            data->transform = t->transformData.deviceAddress ? t->transformData.deviceAddress + range->transformOffset : 0;
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
        if (range->primitiveCount) emit(user, BVH_IR_STAGE_LEAF, &args, sizeof(args), (range->primitiveCount + 63) / 64);
        first += range->primitiveCount;
        leaf_offset += (uint64_t)range->primitiveCount * bvh_ir_leaf_stride(geom->geometryType);
    }

    uint32_t groups = (l.leaf_count + 255) / 256;
    if (!groups) groups = 1;
    struct morton_sort_args sort;
    memset(&sort, 0, sizeof(sort));
    sort.bvh = address + BVH_IR_NODE_OFFSET;
    sort.header = address;
    sort.ids_even = scratch + l.ids_even;
    sort.ids_odd = scratch + l.ids_odd;
    sort.counts = scratch + l.counts;
    sort.leaf_count = (uint32_t)l.leaf_count;
    uint32_t merge_passes = 0;
    for (uint32_t n = groups - 1; n; n >>= 1) ++merge_passes;
    /* Static GPU pass count follows requested capacity, never a CPU readback.
     * Every emit has a compute publication barrier. No workgroup waits for a
     * different workgroup to make progress inside the sort dispatch. */
    for (sort.pass = 0; sort.pass <= merge_passes; ++sort.pass) emit(user, BVH_IR_STAGE_SORT, &sort, sizeof(sort), groups);
    VkDeviceAddress sorted = scratch + ((merge_passes & 1) ? l.ids_even : l.ids_odd);
    if (backend == BVH_IR_BACKEND_HPLOC) {
        const uint64_t range_count = l.leaf_count > 1 ? l.leaf_count - 1 : 1;
        const struct hploc_init_args init_ranges = {scratch + l.hploc_ranges, range_count};
        emit(user, BVH_IR_STAGE_HPLOC_INIT, &init_ranges, sizeof(init_ranges), (uint32_t)((range_count + 63) / 64));
        const struct hploc_args hploc = {address, address + BVH_IR_NODE_OFFSET, sorted, scratch + l.hploc_ranges,
                                         l.nodes - BVH_IR_NODE_OFFSET};
        uint32_t groups = (uint32_t)((l.leaf_count + HPLOC_WORKGROUP_SIZE - 1) / HPLOC_WORKGROUP_SIZE);
        emit(user, BVH_IR_STAGE_HPLOC, &hploc, sizeof(hploc), groups ? groups : 1);
        return VK_SUCCESS;
    }
    if (backend == BVH_IR_BACKEND_PLOC) {
        struct ploc_args ploc;
        memset(&ploc, 0, sizeof(ploc));
        ploc.bvh = address + BVH_IR_NODE_OFFSET;
        ploc.header = address;
        ploc.prefix_scan_partitions = scratch + l.prefix_scan_partitions;
        ploc.ids_0 = sorted;
        ploc.ids_1 = scratch + ((merge_passes & 1) ? l.ids_odd : l.ids_even);
        ploc.internal_node_offset = l.nodes - BVH_IR_NODE_OFFSET;
        /* One persistent workgroup consumes all logical tasks. The pinned
         * global phase scheduler then never waits for an unscheduled workgroup;
         * public Vulkan does not promise cross-workgroup forward progress. */
        emit(user, BVH_IR_STAGE_PLOC, &ploc, sizeof(ploc), 1);
        return VK_SUCCESS;
    }
    struct lbvh_main_args tree;
    memset(&tree, 0, sizeof(tree));
    tree.bvh = address + BVH_IR_NODE_OFFSET;
    tree.src_ids = sorted;
    tree.node_info = scratch + l.node_info;
    tree.header = address;
    tree.internal_node_base = l.nodes - BVH_IR_NODE_OFFSET;
    uint32_t internals = l.leaf_count > 1 ? l.leaf_count - 1 : 1;
    emit(user, BVH_IR_STAGE_TREE, &tree, sizeof(tree), (internals + 63) / 64);
    struct lbvh_generate_ir_args bounds;
    memset(&bounds, 0, sizeof(bounds));
    bounds.bvh = address + BVH_IR_NODE_OFFSET;
    bounds.node_info = scratch + l.node_info;
    bounds.header = address;
    bounds.internal_node_base = l.nodes - BVH_IR_NODE_OFFSET;
    emit(user, BVH_IR_STAGE_BOUNDS, &bounds, sizeof(bounds), (internals + 63) / 64);
    return VK_SUCCESS;
}

/* Existing consumers retain the portable LBVH baseline unless they explicitly
 * qualify and select the additional production backend. */
static inline VkResult bvh_ir_record_build(const VkAccelerationStructureBuildGeometryInfoKHR *info,
                                           const VkAccelerationStructureBuildRangeInfoKHR *ranges, VkDeviceAddress address,
                                           VkDeviceAddress scratch, VkDeviceAddress blas_ir_addresses, bvh_ir_emit_fn emit,
                                           void *user) {
    return bvh_ir_record_build_backend(info, ranges, address, scratch, blas_ir_addresses, emit, user, BVH_IR_BACKEND_LBVH);
}
#endif
