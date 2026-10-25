/* SPDX-License-Identifier: MIT
 *
 * vkrt_cpu_bvh.h - CPU debug BVHIR builder (milestone M1).
 *
 * Emits the same on-GPU layout the traversal walks -- Mesa's BVHIR
 * (vk_ir_* from vk_bvh_defines.h) -- so tests can build a known-good
 * tree and diff the software traversal against a CPU reference before
 * the GPU LBVH builder (M2) exists.
 *
 * Buffer layout (must match sw_ray_query.glsl's SW_BVH_ROOT_OFFSET and
 * vkrt_virtual_as.c):
 *
 *   [0,108) Mesa vk_ir_header; [108,128) common layout descriptor;
 *   [128,128+capacity*stride) reserved leaf slots; then postorder boxes.
 * Node IDs are relative to byte 128. The root is the last actual box.
 */

#ifndef VKRT_CPU_BVH_H
#define VKRT_CPU_BVH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "../bvh/bvh_ir_contract.h"
#define VKRT_CPU_BVH_ROOT_OFFSET BVH_IR_NODE_OFFSET
#define VKRT_CPU_HANDLE_INVALID    0xFFFFFFFFu

/* vk_ir_* type tags (from vk_bvh_defines.h). */
#define VKRT_CPU_NODE_TRIANGLE  0u
#define VKRT_CPU_NODE_INTERNAL  1u
#define VKRT_CPU_NODE_INSTANCE  2u
#define VKRT_CPU_NODE_AABB      3u

/* VK_GEOMETRY_OPAQUE lives in bit 31 of geometry_id_and_flags. */
#define VKRT_CPU_GEOMETRY_OPAQUE (1u << 31)

struct vkrt_cpu_geom {
   const float    *vertices;      /* 3 floats per vertex, tightly packed */
   uint32_t        vertex_count;
   const uint32_t *indices;       /* 3 per triangle; NULL => non-indexed */
   uint32_t        triangle_count;
   uint32_t        geometry_flags;/* VKRT_CPU_GEOMETRY_OPAQUE or 0 */
   uint32_t        geometry_id;   /* reported back as geometry_index */
};

/* One TLAS entry, mirroring VkAccelerationStructureInstanceKHR but with
 * the object-to-world matrix already expanded to 3x4 row-major. */
struct vkrt_cpu_instance {
   uint64_t blas_base;            /* device address of the BLAS buffer   */
   float    otw[3][4];            /* object-to-world, row-major 3x4      */
   uint32_t custom_instance_index;/* low 24 bits                          */
   uint32_t mask;                 /* high 8 bits of custom_instance_and_mask */
   uint32_t instance_id;
   uint32_t instance_flags;       /* VkGeometryInstanceFlagBitsKHR        */
   uint32_t sbt_record_offset;
};

struct vkrt_cpu_bvh {
   uint8_t *data;
   size_t   size;
   uint32_t node_count;      /* internal box nodes                         */
   uint32_t leaf_count;
   uint32_t primitive_count;
};

/* Build a BLAS over the given triangles. Returns 0 on success. */
int  vkrt_cpu_build_blas(const struct vkrt_cpu_geom *geom,
                         struct vkrt_cpu_bvh *out);

/* Build a TLAS over the given instances. Returns 0 on success. */
int  vkrt_cpu_build_tlas(const struct vkrt_cpu_instance *instances,
                         uint32_t instance_count,
                         struct vkrt_cpu_bvh *out);

void vkrt_cpu_bvh_free(struct vkrt_cpu_bvh *b);

/* CPU reference intersect, for the differential tests. Mirrors the
 * traversal's committed-hit semantics. Returns 1 on hit and fills the
 * outs (which may be NULL). */
int vkrt_cpu_ref_intersect(const float v0[3], const float v1[3],
                           const float v2[3],
                           const float origin[3], const float dir[3],
                           float tmin, float tmax,
                           float *out_t, float *out_u, float *out_v,
                           int *out_front);

#ifdef __cplusplus
}
#endif

#endif /* VKRT_CPU_BVH_H */
