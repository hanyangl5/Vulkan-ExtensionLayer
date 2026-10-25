/* SPDX-License-Identifier: MIT
 *
 * sw_ray_query.glsl - software ray-query traversal over Mesa BVHIR (vk_ir_*).
 *
 * Ported from the Arm Avalon DDK's rayquery_sw_implementation.h
 * (compiler/cmpbe/src/builtingen/), which is a same-invocation, resumable
 * software traversal exactly matching the model in
 * vkrt_layer/rayquery_software_rt_layer_design.md sections 1/3/11.
 *
 * What is kept verbatim from Avalon (format-independent):
 *   - the Woop / watertight ray-triangle test with the top-left fill rule
 *     (NOT Moller-Trumbore) and pre-division scaled-space t comparison
 *   - the candidate / committed state machine with suspend+resume:
 *       opaque hit            -> auto-commit, may kill the ray
 *       non-opaque tri / AABB -> candidate + suspend (Proceed returns true)
 *   - ray-flag handling and the opacity precedence chain
 *
 * What is replaced (Avalon is Mali BVH6 + quantized bounds):
 *   - RayBoxNode  -> vk_ir_box_node   (BVH2, full float AABBs)
 *   - RayTriangleData 128-byte batches -> vk_ir_triangle_node
 *   - ASHeader    -> vk_ir_header
 *   - all Arm vendor intrinsics dropped (pure ALU + buffer loads)
 *
 * Opacity micromaps are stubbed out (v1): a triangle's opacity comes from
 * its geometry flags alone.
 */

#ifndef SW_RAY_QUERY_GLSL
#define SW_RAY_QUERY_GLSL

/* Required by vk_bvh_defines.h's buffer_reference address arithmetic
 * (OFFSET / INDEX). Declared here so every consumer of this library
 * gets it; #extension is legal after #version in an include. */
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types : require

/* vk_bvh_defines.h switches on VULKAN to emit its GLSL buffer_reference
 * form rather than the C struct form. Guard so a caller that already
 * defined it does not trip glslc's "macro redefined" error. */
#ifndef VULKAN
#define VULKAN
#define SW_RAY_QUERY_DEFINED_VULKAN
#endif
#include "vk_bvh_defines.h"

/* ------------------------------------------------------------------ */
/* Ray flag bits (gl_RayFlags*EXT values from the SPIR-V ray-query spec) */
/* ------------------------------------------------------------------ */

#define SW_RAY_FLAG_OPAQUE                       0x00000001u
#define SW_RAY_FLAG_NO_OPAQUE                    0x00000002u
#define SW_RAY_FLAG_TERMINATE_ON_FIRST_HIT       0x00000004u
#define SW_RAY_FLAG_SKIP_CLOSEST_HIT             0x00000008u
#define SW_RAY_FLAG_CULL_BACK_FACING             0x00000010u
#define SW_RAY_FLAG_CULL_FRONT_FACING            0x00000020u
#define SW_RAY_FLAG_CULL_OPAQUE                  0x00000040u
#define SW_RAY_FLAG_CULL_NO_OPAQUE               0x00000080u
#define SW_RAY_FLAG_SKIP_TRIANGLES               0x00000100u
#define SW_RAY_FLAG_SKIP_AABB                    0x00000200u

/* VkGeometryInstanceFlagBitsKHR, as folded into the combined flag word
 * (matches Avalon's INSTANCE_FLAGS_* placement). */
#define SW_INSTANCE_FACE_CULLING_DISABLE         0x00010000u
#define SW_INSTANCE_FRONT_CCW                    0x00020000u
#define SW_INSTANCE_FORCE_OPAQUE                 0x00040000u
#define SW_INSTANCE_FORCE_NO_OPAQUE              0x00080000u

/* Per-triangle / per-geometry opacity bit (VK_GEOMETRY_OPAQUE). */
#define SW_TRI_OPAQUE                            (1u << 31)

/* Intersection type values.
 *
 * GLSL exposes TWO overlapping enum groups that share the 0/1/2 value
 * space -- which one a number means depends on the `committed` flag the
 * caller passed to rayQueryGetIntersectionTypeEXT:
 *
 *   candidate: Triangle = 0, AABB = 1
 *   committed: None = 0, Triangle = 1, Generated = 2
 *
 * We keep the two spaces separate internally (the "no candidate
 * outstanding" sentinel has to be distinguishable from "candidate is a
 * triangle") and translate in the sw_get_intersection_type getter.
 */
#define SW_CAND_NONE                             0xFFu
#define SW_CAND_TRIANGLE                         0u
#define SW_CAND_AABB                             1u

#define SW_COMM_NONE                             0u
#define SW_COMM_TRIANGLE                         1u
#define SW_COMM_GENERATED                        2u

/* gl_HitKindEXT values. */
#define SW_HIT_KIND_NONE                         0u
#define SW_HIT_KIND_FRONT_FACING_TRIANGLE        0xFEu
#define SW_HIT_KIND_BACK_FACING_TRIANGLE         0xFFu

/* Opacity classes used by Avalon's omm_compute_opaqueness contract. */
#define SW_FULLY_TRANSPARENT                     0u
#define SW_FULLY_OPAQUE                          1u
#define SW_UNKNOWN_TRANSPARENT                   2u
#define SW_UNKNOWN_OPAQUE                        3u

/* Stack depth. Avalon sized this for BVH6 depth 102; BVH2 is shallower
 * for the same leaf count but let us keep a comfortable margin. */
/* Both TLAS siblings and the current BLAS share this stack. Each radix
 * tree uses a 32-bit Morton key plus a <29-bit index tie-breaker, so two
 * trees need up to 2 * (32 + 29) + 2 entries. */
#define SW_RT_STACK_SIZE 128

#include "bvh_ir_contract.h"
#define SW_HANDLE_INVALID BVH_IR_INVALID_ID

/* ------------------------------------------------------------------ */
/* State (the in-shader rewrite of OpTypeRayQueryKHR)                  */
/* ------------------------------------------------------------------ */

struct sw_ray {
   vec3  origin;
   vec3  direction;
   vec3  inv_direction;
};

struct sw_hit {
   uint  instance_id;
   uint  instance_custom_index;
   uint  geometry_index;
   uint  primitive_index;
   vec2  barycentrics;
   float distance;
   bool  front_face;
   bool  opaque;
   uint  instance_node;
};

/* The TLAS is reached through the bound storage buffer, not through an
 * address. Only the TLAS -> BLAS hop needs a device address, and that one
 * comes out of the instance node (vk_ir_instance_node.base_ptr) -- i.e.
 * out of the BVH data itself, so nothing has to be delivered from the
 * host. This is what lets the layer drop its per-AS "address cell".
 *
 * The set/binding below is a placeholder: the SPIR-V rewrite moves this
 * variable's DescriptorSet/Binding decorations onto the app's original
 * acceleration-structure binding, so the numbering the app chose is
 * preserved exactly (design doc section 8). */
layout(set = 0, binding = 0, std430) readonly buffer VkrtTlas {
   uint words[];
} vkrt_tlas;

struct sw_ray_query_state {
   sw_ray  ray;
   float   tmin;
   float   tclosest;      /* starts at tMax, shrinks on commit */
   uint    ray_flags;     /* SW_RAY_FLAG_* | SW_INSTANCE_* */
   uint    cull_mask;     /* visibility mask; 0 also means "terminated" */

   uint    intersection_type;          /* candidate kind */
   uint    committed_intersection_type;

   sw_hit  candidate;
   sw_hit  committed;

   /* Suspend/resume. */
   bool    resume;
   uint    current_leaf;               /* packed handle of the leaf we are in */
   uint    next_prim;                  /* index within that leaf to resume at */
   /* A candidate can suspend in a BLAS leaf.  Keep its owning TLAS
    * instance separately: the shared stack then contains BLAS handles,
    * which must never be interpreted as TLAS nodes on resume. */
   uint    current_instance;
   bool    current_leaf_is_instance;   /* currently resuming a BLAS instance */

   /* Depth-first walk stack of packed node handles. */
   uint    stack[SW_RT_STACK_SIZE];
   uint    stack_size;

   /* BLAS-level walk shares the same array (Avalon's b_stack_level
    * starts at t_stack_level). */
   uint    blas_stack_base;

   /* Extra call arguments.
    *
    * glslang's ABI passes EVERY function parameter as a Function-class
    * pointer, including scalars. A call site this layer synthesises from
    * an OpRayQuery* instruction only has values, and a Function-class
    * temporary cannot be created at a global scope. So every sw_*
    * entry point takes a single `inout sw_ray_query_state` and reads any
    * further argument from here, which the SPIR-V rewrite stores first.
    */
   float   gen_thit;        /* GenerateIntersection(thit)          */
   uint    want_committed;  /* GetIntersection*(q, committed) flag, 0/1 */
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

uint
sw_ir_offset(uint id)
{
   return BVH_IR_NODE_OFFSET + (id & ~3u);
}

uint
sw_ir_type(uint id)
{
   return id & 3u;
}

/* ---- TLAS: indexed directly through the bound storage buffer ---- */

uint
tlas_u32(uint word)
{
   return vkrt_tlas.words[word];
}

float
tlas_f32(uint word)
{
   return uintBitsToFloat(vkrt_tlas.words[word]);
}

uint64_t
tlas_u64(uint word)
{
   uint lo = tlas_u32(word);
   uint hi = tlas_u32(word + 1u);
   return uint64_t(lo) | (uint64_t(hi) << 32);
}

/* ---- BLAS: reached through a device address ----------------------
 * The address is vk_ir_instance_node.base_ptr, read from the TLAS data,
 * so it needs no host-side plumbing. */
uint
blas_u32(uint64_t base, uint word)
{
   return DEREF(INDEX(uint32_t, base, word));
}

float
blas_f32(uint64_t base, uint word)
{
   return uintBitsToFloat(DEREF(INDEX(uint32_t, base, word)));
}

uint64_t
blas_u64(uint64_t base, uint word)
{
   uint lo = blas_u32(base, word);
   uint hi = blas_u32(base, word + 1u);
   return uint64_t(lo) | (uint64_t(hi) << 32);
}

uint32_t
sw_max_dimension(vec3 v)
{
   return (abs(v.x) > abs(v.y))
             ? ((abs(v.x) > abs(v.z)) ? 0u : 2u)
             : ((abs(v.y) > abs(v.z)) ? 1u : 2u);
}

sw_ray
sw_setup_ray(vec3 origin, vec3 direction)
{
   sw_ray r;
   r.origin        = origin;
   r.direction     = direction;
   r.inv_direction = 1.0 / direction;
   return r;
}

/* Lexicographic compare on float bits -- tie-break for the top-left rule
 * on exactly axis-aligned degeneracies (verbatim from Avalon). */
bool
sw_lexicographic_compare(vec3 v0, vec3 v1)
{
   uvec3 a = floatBitsToUint(v0);
   uvec3 b = floatBitsToUint(v1);
   if (a.z != b.z) return a.z > b.z;
   if (a.y != b.y) return a.y > b.y;
   return a.x > b.x;
}

uint sw_leaf_stride(uint geometry_type)
{
   return geometry_type == 0u ? BVH_IR_TRIANGLE_SIZE :
          geometry_type == 1u ? BVH_IR_AABB_SIZE : BVH_IR_INSTANCE_SIZE;
}

uint sw_blas_root(uint64_t base)
{
   if (blas_u32(base, BVH_IR_MAGIC_OFFSET / 4u) != BVH_IR_MAGIC ||
       blas_u32(base, BVH_IR_VERSION_OFFSET / 4u) != BVH_IR_VERSION ||
       blas_u32(base, BVH_IR_BUILD_FLAGS_OFFSET / 4u) != BVH_IR_SUPPORTED_INPUT_FLAGS ||
       blas_u32(base, 6u) == 0u || blas_u32(base, 8u) == 0u)
      return SW_HANDLE_INVALID;
   uint capacity = blas_u32(base, BVH_IR_LEAF_CAPACITY_OFFSET / 4u);
   uint type = blas_u32(base, BVH_IR_GEOMETRY_TYPE_OFFSET / 4u);
   return BVH_IR_ROOT_ID(capacity, sw_leaf_stride(type), blas_u32(base, 8u));
}

uint sw_tlas_root()
{
   if (tlas_u32(BVH_IR_MAGIC_OFFSET / 4u) != BVH_IR_MAGIC ||
       tlas_u32(BVH_IR_VERSION_OFFSET / 4u) != BVH_IR_VERSION ||
       tlas_u32(BVH_IR_BUILD_FLAGS_OFFSET / 4u) != BVH_IR_SUPPORTED_INPUT_FLAGS ||
       tlas_u32(6u) == 0u || tlas_u32(8u) == 0u)
      return SW_HANDLE_INVALID;
   uint capacity = tlas_u32(BVH_IR_LEAF_CAPACITY_OFFSET / 4u);
   uint type = tlas_u32(BVH_IR_GEOMETRY_TYPE_OFFSET / 4u);
   return BVH_IR_ROOT_ID(capacity, sw_leaf_stride(type), tlas_u32(8u));
}

/* ------------------------------------------------------------------ */
/* Ray / AABB  (slab test; bounds are full floats in vk_ir_*)           */
/* ------------------------------------------------------------------ */

bool
sw_ray_box_test(sw_ray ray, vec3 bmin, vec3 bmax, float tmin, float tmax)
{
   if (any(greaterThan(bmin, bmax))) return false;
   for (uint axis = 0; axis < 3; ++axis) {
      if (ray.direction[axis] == 0.0) {
         if (ray.origin[axis] < bmin[axis] || ray.origin[axis] > bmax[axis]) return false;
      } else {
         float a = (bmin[axis] - ray.origin[axis]) * ray.inv_direction[axis];
         float b = (bmax[axis] - ray.origin[axis]) * ray.inv_direction[axis];
         tmin = max(tmin, min(a, b));
         tmax = min(tmax, max(a, b));
         if (tmin > tmax) return false;
      }
   }
   return true;
}

/* ------------------------------------------------------------------ */
/* Top-left fill rule (verbatim from Avalon)                           */
/* ------------------------------------------------------------------ */

int
sw_top_left_rule_edge(vec3 v0, vec3 v1, float dx, float dy, float e)
{
   int is_top_left = 0;
   if (e == 0.0) {
      float ex = v1.x - v0.x;
      float ey = v1.y - v0.y;
      float x  = dx * ex;
      float y  = dy * ey;
      if (y < x) {
         is_top_left++;
      } else if (y == x) {
         if (sw_lexicographic_compare(v1, v0)) is_top_left++;
         else                                  is_top_left--;
      } else {
         is_top_left--;
      }
   }
   return is_top_left;
}

bool
sw_top_left_rule(vec3 dir, vec3 v0, vec3 v1, vec3 v2,
                 float e0, float e1, float e2)
{
   float dx = (dir.x == 0.0) ? 1.0 : dir.y;
   float dy = dir.x;
   int is_top_left  = sw_top_left_rule_edge(v1, v2, dx, dy, e0);
        is_top_left += sw_top_left_rule_edge(v2, v0, dx, dy, e1);
        is_top_left += sw_top_left_rule_edge(v0, v1, dx, dy, e2);
   return is_top_left > 0;
}

/* ------------------------------------------------------------------ */
/* Opacity (OMM stubbed to flags-only in v1)                           */
/* ------------------------------------------------------------------ */

bool
sw_triangle_is_opaque(uint geometry_flags, uint flags)
{
   bool tri_opaque       = (geometry_flags & SW_TRI_OPAQUE) != 0u;
   bool ray_opaque       = (flags & SW_RAY_FLAG_OPAQUE) != 0u;
   bool ray_no_opaque    = (flags & SW_RAY_FLAG_NO_OPAQUE) != 0u;
   bool inst_opaque      = (flags & SW_INSTANCE_FORCE_OPAQUE) != 0u;
   bool inst_no_opaque   = (flags & SW_INSTANCE_FORCE_NO_OPAQUE) != 0u;

   return ray_opaque ||
          (!ray_no_opaque &&
           (inst_opaque || (!inst_no_opaque && tri_opaque)));
}

/* v1 OMM stub: no micromaps, so the class is decided by the flags. */
uint
sw_compute_opaqueness(bool tri_is_opaque)
{
   return tri_is_opaque ? SW_FULLY_OPAQUE : SW_UNKNOWN_OPAQUE;
}

/* ------------------------------------------------------------------ */
/* Ray / triangle -- Woop watertight (ported from Avalon)              */
/* ------------------------------------------------------------------ */

bool
sw_ray_tri_test(vec3 v0, vec3 v1, vec3 v2,
                uint geometry_flags,
                sw_ray ray,
                uint flags,
                float tmin,
                float tclosest,
                out float distance,
                out vec2 uv,
                out bool opaque,
                out bool front_face)
{
   bool cull_opaque    = (flags & SW_RAY_FLAG_CULL_OPAQUE) != 0u;
   bool cull_no_opaque = (flags & SW_RAY_FLAG_CULL_NO_OPAQUE) != 0u;
   bool tri_is_opaque  = sw_triangle_is_opaque(geometry_flags, flags);
   if ((cull_opaque && tri_is_opaque) || (cull_no_opaque && !tri_is_opaque))
      return false;

   vec3 origin = ray.origin;
   vec3 dir    = ray.direction;

   /* Affine transform that maps the ray to the origin with direction
    * (0,0,1), so the triangle can be projected for a watertight top-left
    * fill rule. Largest |dir| component becomes z. */
   uint kz = sw_max_dimension(dir);
   uint kx = kz + 1u; if (kx == 3u) kx = 0u;
   uint ky = kx + 1u; if (ky == 3u) ky = 0u;

   /* Flip x/y when dir[kz] < 0 to preserve winding. */
   bool flipped = dir[kz] < 0.0;
   if (flipped) { uint tmp = kx; kx = ky; ky = tmp; }

   float dz = 1.0 / dir[kz];
   float dx = dir[kx] * dz;
   float dy = dir[ky] * dz;

   vec3 p0t = v0 - origin;
   vec3 p1t = v1 - origin;
   vec3 p2t = v2 - origin;

   float p0x = p0t[kx] - (dx * p0t[kz]);
   float p0y = p0t[ky] - (dy * p0t[kz]);
   float p1x = p1t[kx] - (dx * p1t[kz]);
   float p1y = p1t[ky] - (dy * p1t[kz]);
   float p2x = p2t[kx] - (dx * p2t[kz]);
   float p2y = p2t[ky] - (dy * p2t[kz]);

   /* Scaled barycentrics. */
   float e0 = (p2x * p1y) - (p2y * p1x);
   float e1 = (p0x * p2y) - (p0y * p2x);
   float e2 = (p1x * p0y) - (p1y * p0x);

   /* Winding cull. is_ccw here means all edge functions non-positive. */
   bool is_ccw = (e0 <= 0.0) && (e1 <= 0.0) && (e2 <= 0.0);
   bool cull_back_face  = (flags & SW_RAY_FLAG_CULL_BACK_FACING) != 0u;
   bool cull_front_face = (flags & SW_RAY_FLAG_CULL_FRONT_FACING) != 0u;
   bool no_face_culling = (flags & SW_INSTANCE_FACE_CULLING_DISABLE) != 0u;
   bool front_ccw       = (flags & SW_INSTANCE_FRONT_CCW) != 0u;
   if (front_ccw) is_ccw = !is_ccw;

   /* Front face is !is_ccw (CCW winding is front by Vulkan default). */
   front_face = !is_ccw;

   if (!no_face_culling &&
       ((cull_back_face && is_ccw) || (cull_front_face && !is_ccw)))
      return false;

   bool top_left_hit = true;
   if ((e0 == 0.0) || (e1 == 0.0) || (e2 == 0.0))
      top_left_hit = sw_top_left_rule(dir, v0, v1, v2, e0, e1, e2);

   bool outside_tri = ((e0 < 0.0) || (e1 < 0.0) || (e2 < 0.0)) &&
                      ((e0 > 0.0) || (e1 > 0.0) || (e2 > 0.0));

   if (!outside_tri && top_left_hit) {
      float det = e0 + e1 + e2;
      if (det != 0.0) {
         float p0z = dz * p0t[kz];
         float p1z = dz * p1t[kz];
         float p2z = dz * p2t[kz];

         /* Compare t in scaled space to avoid the division error. */
         float t_scaled = (e0 * p0z) + (e1 * p1z) + (e2 * p2z);

         bool in_range =
            (det > 0.0) ? (t_scaled >= (tmin * det) &&
                           t_scaled <= (tclosest * det))
                        : (t_scaled <= (tmin * det) &&
                           t_scaled >= (tclosest * det));
         if (in_range) {
            float inv_det = 1.0 / det;
            if (inv_det == 0.0) return false;
            distance  = t_scaled * inv_det;
            uv.x      = e1 * inv_det;
            uv.y      = e2 * inv_det;
            opaque    = tri_is_opaque;
            return true;
         }
      }
   }
   return false;
}

/* ------------------------------------------------------------------ */
/* Leaf walk -- triangles and AABBs, with suspend/resume                */
/* ------------------------------------------------------------------ */

bool
sw_test_triangle_leaf(inout sw_ray_query_state q,
                      uint leaf_handle,
                      uint flags,
                      uint instance_id,
                      uint instance_custom_index,
                      sw_ray obj_ray,
                      uint64_t blas_base)
{
   /* vk_ir_triangle_node = { vk_ir_node base; float coords[3][3];
    *                         uint triangle_id; uint geometry_id_and_flags }.
    * Read member-by-member: a `vk_ir_triangle_node` local would carry
    * ArrayStride (coords is an array) on a Function variable. */
   VOID_REF base = OFFSET(blas_base, sw_ir_offset(leaf_handle));
   /* coords[3][3] starts after vk_ir_node (6 floats). */
   vec3 v0 = vec3(blas_f32(base, 6u),  blas_f32(base, 7u),  blas_f32(base, 8u));
   vec3 v1 = vec3(blas_f32(base, 9u),  blas_f32(base, 10u), blas_f32(base, 11u));
   vec3 v2 = vec3(blas_f32(base, 12u), blas_f32(base, 13u), blas_f32(base, 14u));
   uint sorted_id  = blas_u32(base, 15u);
   uint geo_flags  = blas_u32(base, 16u);

   float dist = 0.0;
   vec2  uv   = vec2(0.0);
   bool  opaque = false;
   bool  front  = true;

   uint primitive_id = sorted_id;
   uint geometry_id  = geo_flags & 0x00FFFFFFu;

   if (!sw_ray_tri_test(v0, v1, v2, geo_flags,
                        obj_ray, flags, q.tmin, q.tclosest,
                        dist, uv, opaque, front))
      return false;

   uint cls = sw_compute_opaqueness(opaque);
   if (cls == SW_FULLY_OPAQUE) {
      /* Opaque hits commit immediately and are not suspendable. */
      q.intersection_type          = SW_CAND_TRIANGLE;
      q.committed_intersection_type = SW_COMM_TRIANGLE;
      q.committed.instance_id           = instance_id;
      q.committed.instance_node         = sw_ir_offset(q.current_instance);
      q.committed.instance_custom_index = instance_custom_index;
      q.committed.geometry_index        = geometry_id;
      q.committed.primitive_index       = primitive_id;
      q.committed.barycentrics          = uv;
      q.committed.distance              = dist;
      q.committed.front_face            = front;
      q.committed.opaque                = true;
      q.tclosest = dist;
      if ((q.ray_flags & SW_RAY_FLAG_TERMINATE_ON_FIRST_HIT) != 0u) {
         q.cull_mask = 0u;
      }
      return false; /* keep walking (or die via cull_mask) */
   }

   if (cls != SW_FULLY_TRANSPARENT) {
      /* Candidate: suspend and let the app Confirm / reject. */
      q.intersection_type = SW_CAND_TRIANGLE;
      q.candidate.instance_id           = instance_id;
      q.candidate.instance_node         = sw_ir_offset(q.current_instance);
      q.candidate.instance_custom_index = instance_custom_index;
      q.candidate.geometry_index        = geometry_id;
      q.candidate.primitive_index       = primitive_id;
      q.candidate.barycentrics          = uv;
      q.candidate.distance              = dist;
      q.candidate.front_face            = front;
      q.candidate.opaque                = false;
      q.next_prim = 1u; /* only one triangle per vk_ir_triangle_node */
      q.resume    = true;
      return true;
   }
   return false;
}

bool
sw_test_aabb_leaf(inout sw_ray_query_state q,
                  uint leaf_handle,
                  uint flags,
                  uint instance_id,
                  uint instance_custom_index,
                  uint64_t blas_base)
{
   /* vk_ir_aabb_node = { vk_ir_node base; uint primitive_id;
    *                     uint geometry_id_and_flags }. */
   VOID_REF base = OFFSET(blas_base, sw_ir_offset(leaf_handle));
   uint prim_id  = blas_u32(base, 6u);
   uint geo_flg  = blas_u32(base, 7u);

   /* Procedural geometry uses the same ray > instance > geometry opacity
    * precedence as triangles, including CandidateAABBOpaque and culling. */
   bool opaque = sw_triangle_is_opaque(geo_flg, flags);
   uint flag = opaque ? SW_RAY_FLAG_CULL_OPAQUE : SW_RAY_FLAG_CULL_NO_OPAQUE;
   if ((q.ray_flags & flag) != 0u) {
      q.next_prim = 1u;
      return false;
   }

   /* AABB candidates carry no t until the app calls GenerateIntersection. */
   q.intersection_type = SW_CAND_AABB;
   q.candidate.instance_id           = instance_id;
   q.candidate.instance_node         = sw_ir_offset(q.current_instance);
   q.candidate.instance_custom_index = instance_custom_index;
   q.candidate.geometry_index        = geo_flg & 0x00FFFFFFu;
   q.candidate.primitive_index       = prim_id;
   q.candidate.distance              = 0.0;
   q.candidate.opaque                = opaque;
   q.next_prim = 1u;
   q.resume    = true;
   return true;
}

/* ------------------------------------------------------------------ */
/* Instance transform                                                  */
/* ------------------------------------------------------------------ */

/* World-to-object ray. t is invariant under an affine transform of the
 * ray, so t computed in object space is already the world t.
 *
 * The matrix is read scalar-by-scalar: a `float m[12]` local or
 * parameter would carry ArrayStride, which the validator rejects on
 * Function storage class.
 */
/* Build the world-to-object ray for a TLAS instance node.
 * `node_word` is the node's offset in bytes divided by 4. */
bool
sw_make_object_ray(uint node_word, sw_ray world, out sw_ray obj)
{
   /* vk_ir_instance_node layout after base.aabb (24):
    *   u64 base_ptr (8) @24, u32 custom_instance_and_mask @32,
    *   u32 sbt_offset_and_flags @36, mat3x4 otw_matrix (48) @40.
    * The matrix starts at word 10 of the node. */
   uint m = node_word + 10u;
   float m0 = tlas_f32(m + 0u),  m1 = tlas_f32(m + 1u);
   float m2 = tlas_f32(m + 2u),  m3 = tlas_f32(m + 3u);
   float m4 = tlas_f32(m + 4u),  m5 = tlas_f32(m + 5u);
   float m6 = tlas_f32(m + 6u),  m7 = tlas_f32(m + 7u);
   float m8 = tlas_f32(m + 8u),  m9 = tlas_f32(m + 9u);
   float m10 = tlas_f32(m + 10u), m11 = tlas_f32(m + 11u);

   /* Invert the 3x3 in place. */
   float A =  (m5 * m10 - m6 * m9);
   float B = -(m4 * m10 - m6 * m8);
   float C =  (m4 * m9  - m5 * m8);
   float det = m0 * A + m1 * B + m2 * C;
   if (det == 0.0)
      return false;
   float id = 1.0 / det;

   float i0 = A * id,                 i1 = (m2 * m9 - m1 * m10) * id;
   float i2 = (m1 * m6 - m2 * m5) * id;
   float i3 = B * id,                 i4 = (m0 * m10 - m2 * m8) * id;
   float i5 = (m2 * m4 - m0 * m6) * id;
   float i6 = C * id,                 i7 = (m1 * m8 - m0 * m9) * id;
   float i8 = (m0 * m5 - m1 * m4) * id;

   vec3 o = world.origin - vec3(m3, m7, m11);
   vec3 oo = vec3(i0 * o.x + i1 * o.y + i2 * o.z,
                  i3 * o.x + i4 * o.y + i5 * o.z,
                  i6 * o.x + i7 * o.y + i8 * o.z);
   vec3 d = world.direction;
   vec3 od = vec3(i0 * d.x + i1 * d.y + i2 * d.z,
                  i3 * d.x + i4 * d.y + i5 * d.z,
                  i6 * d.x + i7 * d.y + i8 * d.z);
   obj = sw_setup_ray(oo, od);
   return true;
}

/* ------------------------------------------------------------------ */
/* Bottom-level traversal (Avalon's bottom_traversal_sw)               */
/* ------------------------------------------------------------------ */

bool
sw_bottom_traversal(inout sw_ray_query_state q, uint instance_node_off)
{
   /* The BLAS lives at its own base address (vk_ir_instance_node.base_ptr),
    * not at q.as. Every handle below is an offset from THAT base. */
   /* vk_ir_instance_node is read scalar-by-scalar; it holds a mat3x4 and
    * a local of that type would carry explicit layout. It lives in the
    * TLAS buffer, so these are TLAS reads. */
   uint iword = instance_node_off / 4u;
   uint custom_and_mask = tlas_u32(iword + 8u);   /* after aabb(24)+base_ptr(8) */
   uint sbt_and_flags_r = tlas_u32(iword + 9u);
   uint instance_id_r   = tlas_u32(iword + 22u);  /* after 48-byte matrix */
   uint root_flags_r    = tlas_u32(iword + 23u);

   /* Visibility mask is the top byte of custom_instance_and_mask. */
   uint inst_mask = (custom_and_mask >> 24) & 0xFFu;
   if ((inst_mask & q.cull_mask) == 0u)
      return false;

   sw_ray obj_ray;
   if (!sw_make_object_ray(iword, q.ray, obj_ray))
      return false;

   uint instance_id           = instance_id_r;
   uint instance_custom_index = custom_and_mask & 0x00FFFFFFu;
   uint sbt_and_flags         = sbt_and_flags_r;
   uint combined_flags        = q.ray_flags | ((sbt_and_flags >> 8) & 0x00FF0000u);

   /* base_ptr is the u64 at byte 24 (word 6) of the instance node. This
    * is a real VkDeviceAddress written in by the AS build, so it is what
    * hops us into the separate BLAS buffer. */
   uint64_t blas_base = tlas_u64(iword + 6u);
   uint blas_root = sw_blas_root(blas_base);

   if (!q.resume) {
      q.blas_stack_base = q.stack_size;
      if (q.stack_size < SW_RT_STACK_SIZE)
         q.stack[q.stack_size++] = blas_root;
   }

   while (q.stack_size > q.blas_stack_base) {
      uint handle = q.stack[q.stack_size - 1];
      if (handle == SW_HANDLE_INVALID) {
         q.stack_size--;
         continue;
      }

      if (q.resume && handle == q.current_leaf) {
         /* The application has consumed (and optionally confirmed) this
          * candidate.  It was the only primitive in this IR leaf, so pop
          * it before continuing with its siblings.  Re-testing it would
          * return the identical candidate forever. */
         q.resume = false;
         q.stack_size--;
         continue;
      }
      q.resume = false;

      VOID_REF nbase = OFFSET(blas_base, sw_ir_offset(handle));
      uint ntype = sw_ir_type(handle);
      if ((ntype == vk_ir_node_triangle && (q.ray_flags & SW_RAY_FLAG_SKIP_TRIANGLES) != 0u) ||
          (ntype == vk_ir_node_aabb && (q.ray_flags & SW_RAY_FLAG_SKIP_AABB) != 0u)) {
         q.stack_size--;
         continue;
      }

      /* Bounds test (skipped when resuming into the leaf). */
      if (!q.resume) {
         /* bounds come from nbase directly in sw_ray_box_test */
         if (!sw_ray_box_test(obj_ray,
                             vec3(blas_f32(nbase, 0u), blas_f32(nbase, 1u), blas_f32(nbase, 2u)),
                             vec3(blas_f32(nbase, 3u), blas_f32(nbase, 4u), blas_f32(nbase, 5u)),
                             q.tmin, q.tclosest)) {
            q.stack_size--;
            continue;
         }
      }

      if (ntype == vk_ir_node_internal) {
         /* Push both children; child0 is processed first. */
         /* vk_ir_box_node = { vk_ir_node base; uint children[2];
          *                    uint bvh_offset; uint flags }.
          * children[] is an array, so a local of this type would carry
          * ArrayStride on a Function variable -- read the two words. */
         q.stack_size--; /* pop this node */
         if (q.stack_size + 2 <= SW_RT_STACK_SIZE) {
            q.stack[q.stack_size++] = blas_u32(nbase, 7u);
            q.stack[q.stack_size++] = blas_u32(nbase, 6u);
         }
         continue;
      }

      /* Leaf. */
      q.current_leaf = handle;
      bool suspend;
      if (ntype == vk_ir_node_triangle) {
         suspend = sw_test_triangle_leaf(q, handle, combined_flags,
                                         instance_id, instance_custom_index,
                                         obj_ray, blas_base);
      } else if (ntype == vk_ir_node_aabb) {
         suspend = sw_test_aabb_leaf(q, handle, combined_flags,
                                     instance_id, instance_custom_index,
                                     blas_base);
      } else {
         q.next_prim = 1u;
         suspend = false;
      }

      if (suspend) return true;
      if (q.cull_mask == 0u) return false;

      q.resume = false;
      q.stack_size--;
   }
   return false;
}

/* ------------------------------------------------------------------ */
/* Top-level traversal + the public state machine                      */
/* ------------------------------------------------------------------ */

bool
sw_ray_query_proceed(inout sw_ray_query_state q)
{
   if (q.cull_mask == 0u)
      return false;

   bool continuing_tlas = false;

   /* A candidate returned from sw_bottom_traversal leaves BLAS nodes on
    * the shared stack.  Resume that walk directly; entering the TLAS loop
    * here would read those BLAS offsets from the TLAS buffer and silently
    * discard siblings (notably the nearer of two overlapping triangles). */
   if (q.resume && q.current_leaf_is_instance) {
      q.intersection_type = SW_CAND_NONE;
      if (sw_bottom_traversal(q, sw_ir_offset(q.current_instance)))
         return true;
      if (q.cull_mask == 0u)
         return false;
      q.resume = false;
      q.current_leaf_is_instance = false;
      /* The BLAS occupied stack slots above blas_stack_base.  Its walk
       * is now complete, but TLAS siblings below that boundary still need
       * to be processed. */
      if (q.stack_size == 0u)
         return false;
      continuing_tlas = true;
   }

   if (!q.resume && !continuing_tlas) {
      q.stack_size = 0u;
      q.stack[q.stack_size++] = sw_tlas_root();
   }

   q.intersection_type = SW_CAND_NONE;

   while (q.stack_size > 0) {
      uint handle = q.stack[q.stack_size - 1];
      if (handle == SW_HANDLE_INVALID) {
         q.stack_size--;
         q.resume = false;
         continue;
      }
      uint ntype  = sw_ir_type(handle);
      uint nword  = sw_ir_offset(handle) / 4u;

      bool resuming_leaf = q.resume && (handle == q.current_leaf) &&
                           (ntype != vk_ir_node_instance);
      if (!resuming_leaf) {
         /* bounds come from nbase directly in sw_ray_box_test */
         if (!sw_ray_box_test(q.ray,
                             vec3(tlas_f32(nword + 0u), tlas_f32(nword + 1u), tlas_f32(nword + 2u)),
                             vec3(tlas_f32(nword + 3u), tlas_f32(nword + 4u), tlas_f32(nword + 5u)),
                             q.tmin, q.tclosest)) {
            q.stack_size--;
            q.resume = false;
            continue;
         }
      }

      if (ntype == vk_ir_node_internal) {
         /* children[] is an array: read the two words rather than
          * materialise a vk_ir_box_node local. */
         q.stack_size--;
         if (q.stack_size + 2 <= SW_RT_STACK_SIZE) {
            q.stack[q.stack_size++] = tlas_u32(nword + 7u);
            q.stack[q.stack_size++] = tlas_u32(nword + 6u);
         }
         q.resume = false;
         continue;
      }

      if (ntype == vk_ir_node_instance) {
         /* TLAS leaf: descend into the BLAS. */
         q.resume = false;
         q.current_instance = handle;
         q.current_leaf_is_instance = true;
         q.stack_size--;

         if (sw_bottom_traversal(q, sw_ir_offset(handle)))
            return true;
         if (q.cull_mask == 0u) return false;
         q.resume = false;
         continue;
      }

      /* Unexpected leaf type at TLAS level (tri/aabb in a TLAS). */
      q.stack_size--;
      q.resume = false;
   }
   return false;
}

/* --- The sw_* surface that the SPIR-V rewrite targets --------------- */

/* Every argument arrives through q: the SPIR-V rewrite has already
 * stored them into q.as, q.ray_flags, q.cull_mask, q.ray.origin,
 * q.ray.direction, q.tmin and q.tclosest. See sw_ray_query_state for
 * why the signature is a single `inout q`. */
void
sw_ray_query_initialize(inout sw_ray_query_state q)
{
   q.ray.inv_direction = 1.0 / q.ray.direction;

   q.intersection_type          = SW_CAND_NONE;
   q.committed_intersection_type = SW_COMM_NONE;

   q.resume                = false;
   q.current_leaf          = 0u;
   q.current_instance      = 0u;
   q.current_leaf_is_instance = false;
   q.next_prim             = 0u;
   q.stack_size            = 0u;
   q.blas_stack_base       = 0u;
}

void
sw_ray_query_confirm(inout sw_ray_query_state q)
{
   if (q.intersection_type == SW_CAND_TRIANGLE &&
       q.candidate.distance <= q.tclosest) {
      q.committed_intersection_type = SW_COMM_TRIANGLE;
      q.committed = q.candidate;
      q.tclosest  = q.candidate.distance;
      if ((q.ray_flags & SW_RAY_FLAG_TERMINATE_ON_FIRST_HIT) != 0u)
         q.cull_mask = 0u;
   } else if (q.intersection_type == SW_CAND_AABB) {
      /* Confirming an AABB candidate without GenerateIntersection is a
       * spec error; ignore it rather than commit a bogus hit. */
   }
}

void
sw_ray_query_generate_intersection(inout sw_ray_query_state q)
{
   float thit = q.gen_thit;
   if (q.intersection_type == SW_CAND_AABB &&
       q.tmin <= thit && thit <= q.tclosest) {
      q.committed_intersection_type = SW_COMM_GENERATED;
      q.committed = q.candidate;
      q.committed.distance = thit;
      q.tclosest = thit;
      if ((q.ray_flags & SW_RAY_FLAG_TERMINATE_ON_FIRST_HIT) != 0u)
         q.cull_mask = 0u;
   }
}

void
sw_ray_query_terminate(inout sw_ray_query_state q)
{
   q.cull_mask = 0u;
}

uint
sw_get_intersection_type(inout sw_ray_query_state q)
{
   if (q.want_committed != 0u)
      return q.committed_intersection_type;   /* 0 none, 1 tri, 2 generated */
   /* Candidate space: 0 = triangle, 1 = AABB. "No outstanding candidate"
    * has no GLSL representation; report it as AABB so nothing matches a
    * triangle test (the only candidate kind the samples inspect). */
   return (q.intersection_type == SW_CAND_AABB) ? 1u : 0u;
}

float
sw_get_intersection_t(inout sw_ray_query_state q)
{
   return (q.want_committed != 0u) ? q.committed.distance : q.candidate.distance;
}

vec2
sw_get_intersection_barycentrics(inout sw_ray_query_state q)
{
   return (q.want_committed != 0u) ? q.committed.barycentrics : q.candidate.barycentrics;
}

int
sw_get_intersection_primitive_index(inout sw_ray_query_state q)
{
   return int((q.want_committed != 0u) ? q.committed.primitive_index
                             : q.candidate.primitive_index);
}

int
sw_get_intersection_geometry_index(inout sw_ray_query_state q)
{
   return int((q.want_committed != 0u) ? q.committed.geometry_index
                             : q.candidate.geometry_index);
}

int
sw_get_intersection_instance_id(inout sw_ray_query_state q)
{
   return int((q.want_committed != 0u) ? q.committed.instance_id : q.candidate.instance_id);
}

int
sw_get_intersection_instance_custom_index(inout sw_ray_query_state q)
{
   return int((q.want_committed != 0u) ? q.committed.instance_custom_index
                             : q.candidate.instance_custom_index);
}

uint
sw_get_intersection_instance_sbt_offset(inout sw_ray_query_state q)
{
   uint node = q.want_committed != 0u ? q.committed.instance_node : q.candidate.instance_node;
   /* vk_ir_instance_node.sbt_offset_and_flags is at byte 36. The upper
    * eight bits contain instance flags, not the SBT record offset. */
   return tlas_u32(node / 4u + 9u) & 0x00FFFFFFu;
}

bool
sw_get_intersection_front_face(inout sw_ray_query_state q)
{
   return (q.want_committed != 0u) ? q.committed.front_face : q.candidate.front_face;
}

bool
sw_get_intersection_candidate_aabb_opaque(inout sw_ray_query_state q)
{
   return q.candidate.opaque;
}

uint
sw_get_intersection_hit_kind(inout sw_ray_query_state q)
{
   bool committed = (q.want_committed != 0u);
   /* Avalon has no hit-kind; synthesise it from front-face (see the
    * layer's gap list). */
   uint t = committed ? q.committed_intersection_type : q.intersection_type;
   if ((!committed && t == SW_CAND_TRIANGLE) ||
       ( committed && t == SW_COMM_TRIANGLE)) {
      bool front = committed ? q.committed.front_face : q.candidate.front_face;
      return front ? SW_HIT_KIND_FRONT_FACING_TRIANGLE
                   : SW_HIT_KIND_BACK_FACING_TRIANGLE;
   }
   return SW_HIT_KIND_NONE;
}

vec3
sw_get_world_ray_origin(inout sw_ray_query_state q)    { return q.ray.origin; }
vec3 sw_get_object_ray_origin(inout sw_ray_query_state q)
{
   uint node = q.want_committed != 0u ? q.committed.instance_node : q.candidate.instance_node;
   sw_ray obj;
   sw_make_object_ray(node / 4u, q.ray, obj);
   return obj.origin;
}
vec3 sw_get_object_ray_direction(inout sw_ray_query_state q)
{
   uint node = q.want_committed != 0u ? q.committed.instance_node : q.candidate.instance_node;
   sw_ray obj;
   sw_make_object_ray(node / 4u, q.ray, obj);
   return obj.direction;
}
mat4x3 sw_get_object_to_world(inout sw_ray_query_state q)
{
   uint node = q.want_committed != 0u ? q.committed.instance_node : q.candidate.instance_node;
   uint w = node / 4u + 10u;
   return mat4x3(vec3(tlas_f32(w), tlas_f32(w+4u), tlas_f32(w+8u)),
      vec3(tlas_f32(w+1u), tlas_f32(w+5u), tlas_f32(w+9u)),
      vec3(tlas_f32(w+2u), tlas_f32(w+6u), tlas_f32(w+10u)),
      vec3(tlas_f32(w+3u), tlas_f32(w+7u), tlas_f32(w+11u)));
}
mat4x3 sw_get_world_to_object(inout sw_ray_query_state q)
{
   return mat4x3(inverse(mat4(sw_get_object_to_world(q))));
}
vec3
sw_get_world_ray_direction(inout sw_ray_query_state q) { return q.ray.direction; }
float
sw_get_ray_tmin(inout sw_ray_query_state q)            { return q.tmin; }
uint
sw_get_ray_flags(inout sw_ray_query_state q)           { return q.ray_flags; }

#endif /* SW_RAY_QUERY_GLSL */
