/* SPDX-License-Identifier: MIT
 * Software ray-query traversal over the shared, versioned VEL BVH4 ABI.
 * The format-independent watertight triangle test and opacity/state-machine
 * rules retain their Avalon provenance. No Mesa private runtime layout is used.
 */

#ifndef SW_RAY_QUERY_GLSL
#define SW_RAY_QUERY_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types : require

#include "bvh4_abi.glsl"

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
#define SW_TRI_OPAQUE                            1u

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

#define SW_HANDLE_INVALID VEL_RT_BVH4_INVALID_ID

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

struct sw_ray_query_state {
   uint64_t as_address; /* Actual AS value, independent for every query. */
   sw_ray  ray;
   float   tmin;
   float   tclosest;      /* starts at tMax, shrinks on commit */
   uint    ray_flags;     /* SW_RAY_FLAG_* | SW_INSTANCE_* */
   uint    cull_mask;     /* visibility mask; 0 also means "terminated" */

   uint    intersection_type;          /* candidate kind */
   uint    committed_intersection_type;

   sw_hit  candidate;
   sw_hit  committed;

   /* Constant-space depth-first parent walks. Each cursor is the next
    * unconsumed node; TLAS siblings and BLAS candidates resume independently.
    * Parent links replace a depth-limited stack, so no child is dropped. */
   uint    tlas_next;
   uint    blas_next;
   uint    current_instance; /* Tagged BVH4 instance ID, also saved in hits. */
   bool    blas_active;

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

/* All scalar loads use an explicitly 64-bit address calculation. */
uint blas_u32(uint64_t base, uint word) {
   return vel_rt_bvh4_u32(base, uint64_t(word) * 4ul);
}
float blas_f32(uint64_t base, uint word) {
   return vel_rt_bvh4_f32(base, uint64_t(word) * 4ul);
}
uint64_t blas_u64(uint64_t base, uint word) {
   return uint64_t(blas_u32(base, word)) | (uint64_t(blas_u32(base, word + 1u)) << 32);
}

uint
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

/* Root identity and bounds are checked before following any runtime ID. */
uint sw_bvh4_root(uint64_t base, uint type, sw_ray ray, float tmin, float tmax) {
   if (base == 0ul || blas_u32(base, 0u) != VEL_RT_BVH4_MAGIC ||
       blas_u32(base, 1u) != VEL_RT_BVH4_ABI_VERSION || blas_u32(base, 2u) != type ||
       (blas_u32(base, 3u) & VEL_RT_BVH4_FLAG_INVALID) != 0u)
      return SW_HANDLE_INVALID;
   uint root = blas_u32(base, VEL_RT_BVH4_HEADER_ROOT / 4u);
   if (root == SW_HANDLE_INVALID ||
       !sw_ray_box_test(ray,
          vec3(blas_f32(base, 24u), blas_f32(base, 25u), blas_f32(base, 26u)),
          vec3(blas_f32(base, 27u), blas_f32(base, 28u), blas_f32(base, 29u)), tmin, tmax))
      return SW_HANDLE_INVALID;
   return root;
}

bool sw_bvh4_child_hit(uint64_t node, uint slot, sw_ray ray, float tmin, float tmax) {
   uint word = slot * 3u;
   return sw_ray_box_test(ray,
      vec3(blas_f32(node, word), blas_f32(node, word + 1u), blas_f32(node, word + 2u)),
      vec3(blas_f32(node, 12u + word), blas_f32(node, 13u + word), blas_f32(node, 14u + word)), tmin, tmax);
}

uint sw_bvh4_first(uint64_t base, uint id, sw_ray ray, float tmin, float tmax) {
   uint64_t node = vel_rt_bvh4_node(base, id);
   uint count = blas_u32(node, VEL_RT_BVH4_NODE_CHILD_COUNT / 4u);
   if (count > 4u) return SW_HANDLE_INVALID;
   for (uint slot = 0u; slot < count; ++slot) {
      uint child = blas_u32(node, VEL_RT_BVH4_NODE_CHILDREN / 4u + slot);
      if (child != SW_HANDLE_INVALID && sw_bvh4_child_hit(node, slot, ray, tmin, tmax)) return child;
   }
   return SW_HANDLE_INVALID;
}

/* After a completed subtree, test later siblings and ascend until one hits.
 * In a valid tree every ascent reaches a distinct internal ancestor, so the
 * node_count bound cannot truncate a valid walk at any depth. */
uint sw_bvh4_after(uint64_t base, uint id, sw_ray ray, float tmin, float tmax) {
   uint node_count = blas_u32(base, VEL_RT_BVH4_HEADER_NODE_COUNT / 4u);
   for (uint ascents = 0u; ascents <= node_count; ++ascents) {
      uint parent = vel_rt_bvh4_parent(base, id);
      if (parent == SW_HANDLE_INVALID) return SW_HANDLE_INVALID;
      if (vel_rt_bvh4_kind(parent) != VEL_RT_BVH4_INTERNAL || vel_rt_bvh4_index(parent) >= node_count)
         return SW_HANDLE_INVALID;
      uint64_t node = vel_rt_bvh4_node(base, parent);
      uint count = blas_u32(node, VEL_RT_BVH4_NODE_CHILD_COUNT / 4u);
      if (count > 4u) return SW_HANDLE_INVALID;
      bool later = false;
      for (uint slot = 0u; slot < count; ++slot) {
         uint child = blas_u32(node, VEL_RT_BVH4_NODE_CHILDREN / 4u + slot);
         if (later && child != SW_HANDLE_INVALID && sw_bvh4_child_hit(node, slot, ray, tmin, tmax)) return child;
         if (child == id) later = true;
      }
      if (!later) return SW_HANDLE_INVALID;
      id = parent;
   }
   return SW_HANDLE_INVALID;
}

/* A previous Proceed may have selected this node before Confirm shortened
 * tclosest. Recheck the saved cursor's bounds using the current ray interval. */
bool sw_bvh4_cursor_hit(uint64_t base, uint id, sw_ray ray, float tmin, float tmax) {
   uint parent = vel_rt_bvh4_parent(base, id);
   if (parent == SW_HANDLE_INVALID)
      return sw_ray_box_test(ray,
         vec3(blas_f32(base, 24u), blas_f32(base, 25u), blas_f32(base, 26u)),
         vec3(blas_f32(base, 27u), blas_f32(base, 28u), blas_f32(base, 29u)), tmin, tmax);
   uint64_t node = vel_rt_bvh4_node(base, parent);
   uint count = blas_u32(node, VEL_RT_BVH4_NODE_CHILD_COUNT / 4u);
   if (count > 4u) return false;
   for (uint slot = 0u; slot < count; ++slot)
      if (blas_u32(node, VEL_RT_BVH4_NODE_CHILDREN / 4u + slot) == id)
         return sw_bvh4_child_hit(node, slot, ray, tmin, tmax);
   return false;
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
   uint64_t base = vel_rt_bvh4_leaf(blas_base, leaf_handle);
   vec3 v0 = vec3(blas_f32(base, 0u), blas_f32(base, 1u), blas_f32(base, 2u));
   vec3 v1 = vec3(blas_f32(base, 3u), blas_f32(base, 4u), blas_f32(base, 5u));
   vec3 v2 = vec3(blas_f32(base, 6u), blas_f32(base, 7u), blas_f32(base, 8u));
   uint primitive_id = blas_u32(base, 9u);
   uint geometry_id = blas_u32(base, 10u);
   uint geometry_flags = blas_u32(base, 11u);

   float dist = 0.0;
   vec2  uv   = vec2(0.0);
   bool  opaque = false;
   bool  front  = true;

   if (!sw_ray_tri_test(v0, v1, v2, geometry_flags,
                        obj_ray, flags, q.tmin, q.tclosest,
                        dist, uv, opaque, front))
      return false;

   uint cls = sw_compute_opaqueness(opaque);
   if (cls == SW_FULLY_OPAQUE) {
      /* Opaque hits commit immediately and are not suspendable. */
      q.intersection_type          = SW_CAND_TRIANGLE;
      q.committed_intersection_type = SW_COMM_TRIANGLE;
      q.committed.instance_id           = instance_id;
      q.committed.instance_node         = q.current_instance;
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
      q.candidate.instance_node         = q.current_instance;
      q.candidate.instance_custom_index = instance_custom_index;
      q.candidate.geometry_index        = geometry_id;
      q.candidate.primitive_index       = primitive_id;
      q.candidate.barycentrics          = uv;
      q.candidate.distance              = dist;
      q.candidate.front_face            = front;
      q.candidate.opaque                = false;
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
   uint64_t base = vel_rt_bvh4_leaf(blas_base, leaf_handle);
   uint prim_id = blas_u32(base, 6u);
   uint geometry_id = blas_u32(base, 7u);
   uint geometry_flags = blas_u32(base, 8u);

   /* Procedural geometry uses the same ray > instance > geometry opacity
    * precedence as triangles, including CandidateAABBOpaque and culling. */
   bool opaque = sw_triangle_is_opaque(geometry_flags, flags);
   uint flag = opaque ? SW_RAY_FLAG_CULL_OPAQUE : SW_RAY_FLAG_CULL_NO_OPAQUE;
   if ((q.ray_flags & flag) != 0u) {
      return false;
   }

   /* AABB candidates carry no t until the app calls GenerateIntersection. */
   q.intersection_type = SW_CAND_AABB;
   q.candidate.instance_id           = instance_id;
   q.candidate.instance_node         = q.current_instance;
   q.candidate.instance_custom_index = instance_custom_index;
   q.candidate.geometry_index        = geometry_id;
   q.candidate.primitive_index       = prim_id;
   q.candidate.distance              = 0.0;
   q.candidate.opaque                = opaque;
   return true;
}

/* Instance matrices are original row-major 3x4 transforms. Both directions
 * are carried by the ABI, avoiding an inverse in the traversal hot path. */
bool sw_make_object_ray(uint64_t base, uint instance, sw_ray world, out sw_ray obj) {
   uint64_t record = vel_rt_bvh4_leaf(base, instance);
   uint m = VEL_RT_BVH4_INSTANCE_WTO / 4u;
   vec3 x = vec3(blas_f32(record, m), blas_f32(record, m + 1u), blas_f32(record, m + 2u));
   vec3 y = vec3(blas_f32(record, m + 4u), blas_f32(record, m + 5u), blas_f32(record, m + 6u));
   vec3 z = vec3(blas_f32(record, m + 8u), blas_f32(record, m + 9u), blas_f32(record, m + 10u));
   vec3 translation = vec3(blas_f32(record, m + 3u), blas_f32(record, m + 7u), blas_f32(record, m + 11u));
   obj = sw_setup_ray(vec3(dot(x, world.origin), dot(y, world.origin), dot(z, world.origin)) + translation,
                      vec3(dot(x, world.direction), dot(y, world.direction), dot(z, world.direction)));
   return true;
}

bool sw_bottom_traversal(inout sw_ray_query_state q) {
   uint64_t instance = vel_rt_bvh4_leaf(q.as_address, q.current_instance);
   uint custom_and_mask = blas_u32(instance, 2u);
   if (((custom_and_mask >> 24) & q.cull_mask) == 0u) return false;
   uint combined_flags = q.ray_flags | ((blas_u32(instance, 3u) >> 8) & 0x00ff0000u);
   uint instance_id = blas_u32(instance, 28u);
   uint custom_index = custom_and_mask & 0x00ffffffu;
   uint64_t base = blas_u64(instance, 0u);
   sw_ray ray;
   sw_make_object_ray(q.as_address, q.current_instance, q.ray, ray);
   while (q.blas_next != SW_HANDLE_INVALID) {
      uint handle = q.blas_next;
      if (!sw_bvh4_cursor_hit(base, handle, ray, q.tmin, q.tclosest)) {
         q.blas_next = sw_bvh4_after(base, handle, ray, q.tmin, q.tclosest);
         continue;
      }
      uint kind = vel_rt_bvh4_kind(handle);
      if (kind == VEL_RT_BVH4_INTERNAL) {
         uint child = sw_bvh4_first(base, handle, ray, q.tmin, q.tclosest);
         q.blas_next = child == SW_HANDLE_INVALID ? sw_bvh4_after(base, handle, ray, q.tmin, q.tclosest) : child;
         continue;
      }
      /* Consume this leaf before suspending. Resume can never return the same
       * primitive again, even when the application rejects a candidate. */
      q.blas_next = sw_bvh4_after(base, handle, ray, q.tmin, q.tclosest);
      bool candidate = false;
      if (kind == VEL_RT_BVH4_TRIANGLE && (q.ray_flags & SW_RAY_FLAG_SKIP_TRIANGLES) == 0u)
         candidate = sw_test_triangle_leaf(q, handle, combined_flags, instance_id, custom_index, ray, base);
      else if (kind == VEL_RT_BVH4_AABB && (q.ray_flags & SW_RAY_FLAG_SKIP_AABB) == 0u)
         candidate = sw_test_aabb_leaf(q, handle, combined_flags, instance_id, custom_index, base);
      if (candidate) return true;
      if (q.cull_mask == 0u) return false;
   }
   return false;
}

bool sw_ray_query_proceed(inout sw_ray_query_state q) {
   if (q.cull_mask == 0u) return false;
   q.intersection_type = SW_CAND_NONE;
   for (;;) {
      if (q.blas_active) {
         if (sw_bottom_traversal(q)) return true;
         q.blas_active = false;
         if (q.cull_mask == 0u) return false;
      }
      if (q.tlas_next == SW_HANDLE_INVALID) return false;
      uint handle = q.tlas_next;
      if (!sw_bvh4_cursor_hit(q.as_address, handle, q.ray, q.tmin, q.tclosest)) {
         q.tlas_next = sw_bvh4_after(q.as_address, handle, q.ray, q.tmin, q.tclosest);
         continue;
      }
      uint kind = vel_rt_bvh4_kind(handle);
      if (kind == VEL_RT_BVH4_INTERNAL) {
         uint child = sw_bvh4_first(q.as_address, handle, q.ray, q.tmin, q.tclosest);
         q.tlas_next = child == SW_HANDLE_INVALID ? sw_bvh4_after(q.as_address, handle, q.ray, q.tmin, q.tclosest) : child;
         continue;
      }
      q.tlas_next = sw_bvh4_after(q.as_address, handle, q.ray, q.tmin, q.tclosest);
      if (kind != VEL_RT_BVH4_INSTANCE) continue;
      q.current_instance = handle;
      uint64_t instance = vel_rt_bvh4_leaf(q.as_address, handle);
      if (((blas_u32(instance, 2u) >> 24) & q.cull_mask) == 0u) continue;
      sw_ray ray;
      sw_make_object_ray(q.as_address, handle, q.ray, ray);
      q.blas_next = sw_bvh4_root(blas_u64(instance, 0u), 1u, ray, q.tmin, q.tclosest);
      q.blas_active = q.blas_next != SW_HANDLE_INVALID;
   }
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

   q.tlas_next = sw_bvh4_root(q.as_address, 0u, q.ray, q.tmin, q.tclosest);
   q.blas_next = SW_HANDLE_INVALID;
   q.current_instance = SW_HANDLE_INVALID;
   q.blas_active = false;
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
   return blas_u32(vel_rt_bvh4_leaf(q.as_address, node), 3u) & 0x00ffffffu;
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
   sw_make_object_ray(q.as_address, node, q.ray, obj);
   return obj.origin;
}
vec3 sw_get_object_ray_direction(inout sw_ray_query_state q)
{
   uint node = q.want_committed != 0u ? q.committed.instance_node : q.candidate.instance_node;
   sw_ray obj;
   sw_make_object_ray(q.as_address, node, q.ray, obj);
   return obj.direction;
}
mat4x3 sw_instance_matrix(uint64_t base, uint instance, uint byte_offset) {
   uint64_t record = vel_rt_bvh4_leaf(base, instance);
   uint w = byte_offset / 4u;
   return mat4x3(vec3(blas_f32(record, w), blas_f32(record, w + 4u), blas_f32(record, w + 8u)),
      vec3(blas_f32(record, w + 1u), blas_f32(record, w + 5u), blas_f32(record, w + 9u)),
      vec3(blas_f32(record, w + 2u), blas_f32(record, w + 6u), blas_f32(record, w + 10u)),
      vec3(blas_f32(record, w + 3u), blas_f32(record, w + 7u), blas_f32(record, w + 11u)));
}
mat4x3 sw_get_object_to_world(inout sw_ray_query_state q) {
   uint instance = q.want_committed != 0u ? q.committed.instance_node : q.candidate.instance_node;
   return sw_instance_matrix(q.as_address, instance, VEL_RT_BVH4_INSTANCE_OTW);
}
mat4x3 sw_get_world_to_object(inout sw_ray_query_state q) {
   uint instance = q.want_committed != 0u ? q.committed.instance_node : q.candidate.instance_node;
   return sw_instance_matrix(q.as_address, instance, VEL_RT_BVH4_INSTANCE_WTO);
}
vec3
sw_get_world_ray_direction(inout sw_ray_query_state q) { return q.ray.direction; }
float
sw_get_ray_tmin(inout sw_ray_query_state q)            { return q.tmin; }
uint
sw_get_ray_flags(inout sw_ray_query_state q)           { return q.ray_flags; }

#endif /* SW_RAY_QUERY_GLSL */
