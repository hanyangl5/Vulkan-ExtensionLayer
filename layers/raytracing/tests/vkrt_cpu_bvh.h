/* SPDX-License-Identifier: MIT
 *
 * CPU fixture builder. Returns the shared BVH4 runtime representation from
 * bvh4_abi.h, using the common reference adapter/encoder. Mesa IR is an
 * internal construction temporary in this test-only library.
 */

#ifndef VKRT_CPU_BVH_H
#define VKRT_CPU_BVH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "bvh4_abi.h"
/* Original Vulkan geometry flags, preserved in the shared runtime leaves. */
#define VKRT_CPU_GEOMETRY_OPAQUE 1u
#define VKRT_CPU_GEOMETRY_NO_DUPLICATE_ANY_HIT 2u

struct vkrt_cpu_geom {
   const float    *vertices;      /* 3 floats per vertex, tightly packed */
   uint32_t        vertex_count;
   const uint32_t *indices;       /* 3 per triangle; NULL => non-indexed */
   uint32_t        triangle_count;
   uint32_t geometry_flags;       /* original VkGeometryFlagsKHR bits */
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
