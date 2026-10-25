// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#ifndef VEL_RT_FALLBACK_BVH4_ABI_H
#define VEL_RT_FALLBACK_BVH4_ABI_H

#include <stdint.h>

// Layer-owned, experimental wire ABI. These are not AMD hardware node IDs.
#define VEL_RT_BVH4_MAGIC UINT32_C(0x344c4556)
#define VEL_RT_BVH4_ABI_VERSION UINT32_C(1)
#define VEL_RT_BVH4_FLAG_INVALID UINT32_C(0x80000000)
#define VEL_RT_BVH4_INVALID_ID UINT32_C(0xffffffff)
#define VEL_RT_BVH4_INDEX_MASK UINT32_C(0x3fffffff)
// Reserve the last index uniformly, including the instance-tagged invalid ID.
#define VEL_RT_BVH4_MAX_INDEX UINT32_C(0x3ffffffe)
#define VEL_RT_BVH4_MAX_COUNT UINT32_C(0x3fffffff)
#define VEL_RT_BVH4_KIND_SHIFT 30

typedef enum VelRtBvh4Kind {
    VEL_RT_BVH4_INTERNAL = 0,
    VEL_RT_BVH4_TRIANGLE = 1,
    VEL_RT_BVH4_AABB = 2,
    VEL_RT_BVH4_INSTANCE = 3
} VelRtBvh4Kind;

typedef struct VelRtBvh4Header {
    uint32_t magic, version, type, flags;
    uint32_t root_id, node_count, triangle_count, aabb_count;
    uint32_t instance_count, reserved0[3];
    uint64_t node_offset, triangle_offset, aabb_offset, instance_offset;
    uint64_t leaf_parent_offset, total_size;
    float bounds_min[3], bounds_max[3];
    uint32_t reserved1[2];
} VelRtBvh4Header;

// Bounds use packed scalar floats: shader consumers must use scalar layout or
// explicit word loads, not std430 arrays of vec3 (whose stride differs).
typedef struct VelRtBvh4Node {
    float bounds_min[4][3];
    float bounds_max[4][3];
    uint32_t child_id[4];
    uint32_t child_count;
    uint32_t parent_id;
    uint32_t reserved[2];
} VelRtBvh4Node;

typedef struct VelRtBvh4Triangle {
    float vertices[3][3];
    uint32_t primitive_id, geometry_id, geometry_flags;
} VelRtBvh4Triangle;

typedef struct VelRtBvh4Aabb {
    float bounds_min[3], bounds_max[3];
    uint32_t primitive_id, geometry_id, geometry_flags, reserved[3];
} VelRtBvh4Aabb;

typedef struct VelRtBvh4Instance {
    uint64_t blas_address;
    uint32_t custom_index_mask, sbt_offset_flags;
    float object_to_world[12], world_to_object[12];
    uint32_t instance_id, reserved[3];
} VelRtBvh4Instance;

// Leaf parent table order is triangles, AABBs, then instances. Entries and node
// parent_id are tagged INTERNAL IDs. A root leaf has INVALID_ID as its parent.

#endif
