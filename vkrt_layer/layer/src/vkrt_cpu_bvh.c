/* SPDX-License-Identifier: MIT
 *
 * vkrt_cpu_bvh.c - test-only CPU construction inputs for shared BVH4.
 *
 * Recursive median-split BVH2. Deliberately simple and obviously
 * correct. Its temporary Mesa IR is consumed by the common adapter and
 * reference encoder; only the shared BVH4 container is returned to fixtures.
 *
 * Primitive IDs: vk_ir_triangle_node.triangle_id is written with the
 * ORIGINAL triangle index, so the app-visible primitive id needs no
 * side-array lookup at trace time (doc section 9's remap requirement is
 * satisfied at build time instead).
 */

#include "vkrt_cpu_bvh.h"
#include "../bvh/bvh_ir_contract.h"

/* Mesa construction tags, never returned as runtime node IDs. */
#define VKRT_CPU_HANDLE_INVALID UINT32_MAX
#define VKRT_CPU_NODE_TRIANGLE 0u
#define VKRT_CPU_NODE_INTERNAL 1u
#define VKRT_CPU_NODE_INSTANCE 2u
/* Consumes the temporary allocation, including on conversion failure. */
int vkrt_cpu_encode_mesa_bvh4(struct vkrt_cpu_bvh *temporary, uint32_t geometry_id, uint32_t geometry_flags, int top_level);

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Scalar-layout sizes, matching vk_bvh_defines.h compiled with `scalar`. */
#define SZ_IR_NODE       24u   /* vk_aabb                                      */
#define SZ_BOX_NODE      40u   /* + children[2] + bvh_offset + flags           */
#define SZ_TRIANGLE_NODE 68u   /* + coords[3][3] + triangle_id + geom_and_flags */
#define SZ_AABB_NODE     32u   /* + primitive_id + geom_and_flags              */
#define SZ_INSTANCE_NODE 96u   /* + base_ptr + custom/mask + sbt/flags + mat3x4
                                * + instance_id + root_flags                   */

static void
put_u32(uint8_t *p, uint32_t off, uint32_t v)
{
   memcpy(p + off, &v, 4);
}

static void
put_u64(uint8_t *p, uint32_t off, uint64_t v)
{
   memcpy(p + off, &v, 8);
}

static void
put_f32(uint8_t *p, uint32_t off, float v)
{
   memcpy(p + off, &v, 4);
}

static void
put_aabb(uint8_t *p, uint32_t off, const float b[6])
{
   for (int i = 0; i < 6; i++)
      put_f32(p, off + (uint32_t)i * 4, b[i]);
}

static void
aabb_empty(float b[6])
{
   b[0] = b[1] = b[2] =  INFINITY;
   b[3] = b[4] = b[5] = -INFINITY;
}

static void
aabb_expand(float b[6], const float v[3])
{
   for (int i = 0; i < 3; i++) {
      if (v[i] < b[i])     b[i]     = v[i];
      if (v[i] > b[i + 3]) b[i + 3] = v[i];
   }
}

static void
aabb_merge(float b[6], const float a[6], const float c[6])
{
   for (int i = 0; i < 3; i++) {
      b[i]     = a[i]     < c[i]     ? a[i]     : c[i];
      b[i + 3] = a[i + 3] > c[i + 3] ? a[i + 3] : c[i + 3];
   }
}

/* ------------------------------------------------------------------ */
/* Growable node buffer                                                */
/* ------------------------------------------------------------------ */

struct build_buf {
   uint8_t *data;
   size_t   size;
   size_t   cap;
};

static int
buf_reserve(struct build_buf *b, size_t need)
{
   if (b->size + need <= b->cap)
      return 0;
   size_t ncap = b->cap ? b->cap : 4096;
   while (ncap < b->size + need)
      ncap *= 2;
   uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
   if (!nd)
      return -1;
   b->data = nd;
   b->cap  = ncap;
   return 0;
}

static uint32_t
buf_alloc(struct build_buf *b, size_t sz)
{
   if (buf_reserve(b, sz) != 0)
      return VKRT_CPU_HANDLE_INVALID;
   uint32_t off = (uint32_t)b->size;
   memset(b->data + off, 0, sz);
   b->size += sz;
   return off;
}

/* ------------------------------------------------------------------ */
/* Triangle reference intersect (the oracle)                           */
/* ------------------------------------------------------------------ */

int
vkrt_cpu_ref_intersect(const float v0[3], const float v1[3],
                       const float v2[3],
                       const float origin[3], const float dir[3],
                       float tmin, float tmax,
                       float *out_t, float *out_u, float *out_v,
                       int *out_front)
{
   /* Same Woop / watertight construction the traversal uses, so the two
    * agree bit-for-bit on edge cases (that is the point of the oracle). */
   float d[3] = { dir[0], dir[1], dir[2] };
   int kz = 0;
   if (fabsf(d[1]) > fabsf(d[0])) kz = 1;
   if (fabsf(d[2]) > fabsf(d[kz])) kz = 2;
   int kx = kz + 1; if (kx == 3) kx = 0;
   int ky = kx + 1; if (ky == 3) ky = 0;
   if (d[kz] < 0.0f) { int t = kx; kx = ky; ky = t; }

   float dz = 1.0f / d[kz];
   float dx = d[kx] * dz;
   float dy = d[ky] * dz;

   float p0[3], p1[3], p2[3];
   for (int i = 0; i < 3; i++) {
      p0[i] = v0[i] - origin[i];
      p1[i] = v1[i] - origin[i];
      p2[i] = v2[i] - origin[i];
   }
   float p0x = p0[kx] - dx * p0[kz], p0y = p0[ky] - dy * p0[kz];
   float p1x = p1[kx] - dx * p1[kz], p1y = p1[ky] - dy * p1[kz];
   float p2x = p2[kx] - dx * p2[kz], p2y = p2[ky] - dy * p2[kz];

   float e0 = p2x * p1y - p2y * p1x;
   float e1 = p0x * p2y - p0y * p2x;
   float e2 = p1x * p0y - p1y * p0x;

   /* Vulkan default: counter-clockwise is front. */
   int is_ccw = (e0 <= 0.0f) && (e1 <= 0.0f) && (e2 <= 0.0f);
   if (out_front)
      *out_front = !is_ccw;

   int outside = ((e0 < 0.0f) || (e1 < 0.0f) || (e2 < 0.0f)) &&
                 ((e0 > 0.0f) || (e1 > 0.0f) || (e2 > 0.0f));
   if (outside)
      return 0;

   float det = e0 + e1 + e2;
   if (det == 0.0f)
      return 0;

   float p0z = dz * p0[kz], p1z = dz * p1[kz], p2z = dz * p2[kz];
   float t_scaled = e0 * p0z + e1 * p1z + e2 * p2z;

   int in_range = (det > 0.0f)
                     ? (t_scaled >= tmin * det && t_scaled <= tmax * det)
                     : (t_scaled <= tmin * det && t_scaled >= tmax * det);
   if (!in_range)
      return 0;

   float inv_det = 1.0f / det;
   if (inv_det == 0.0f)
      return 0;

   if (out_t) *out_t = t_scaled * inv_det;
   if (out_u) *out_u = e1 * inv_det;
   if (out_v) *out_v = e2 * inv_det;
   return 1;
}

/* ------------------------------------------------------------------ */
/* BLAS                                                                */
/* ------------------------------------------------------------------ */

struct tri_ref {
   float    centroid[3];
   float    bounds[6];
   float    v[3][3];
   uint32_t original_index;
};

static uint32_t
emit_triangle_leaf(struct build_buf *b, const struct tri_ref *t,
                   uint32_t geometry_id, uint32_t geometry_flags)
{
   uint32_t off = BVH_IR_NODE_OFFSET + t->original_index * SZ_TRIANGLE_NODE;
   if (off == VKRT_CPU_HANDLE_INVALID)
      return off;

   put_aabb(b->data, off, t->bounds);
   for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
         put_f32(b->data, off + 24u + ((uint32_t)i * 3u + (uint32_t)j) * 4u,
                 t->v[i][j]);
   /* triangle_id carries the ORIGINAL index -- see the file comment. */
   put_u32(b->data, off + 60u, t->original_index);
   put_u32(b->data, off + 64u, (geometry_id & 0x00FFFFFFu) | ((geometry_flags & VKRT_CPU_GEOMETRY_OPAQUE) ? 0x80000000u : 0));

   return (off - BVH_IR_NODE_OFFSET) | VKRT_CPU_NODE_TRIANGLE;
}

static uint32_t
build_blas_node(struct build_buf *b, struct tri_ref *tris, uint32_t *idx,
                uint32_t count, uint32_t geometry_id, uint32_t geometry_flags)
{
   if (count == 0)
      return VKRT_CPU_HANDLE_INVALID;

   if (count == 1)
      return emit_triangle_leaf(b, &tris[idx[0]], geometry_id, geometry_flags);

   /* Median split on the longest centroid axis. */
   float lo[3] = { INFINITY,  INFINITY,  INFINITY };
   float hi[3] = { -INFINITY, -INFINITY, -INFINITY };
   for (uint32_t i = 0; i < count; i++) {
      for (int k = 0; k < 3; k++) {
         float c = tris[idx[i]].centroid[k];
         if (c < lo[k]) lo[k] = c;
         if (c > hi[k]) hi[k] = c;
      }
   }
   int axis = 0;
   if (hi[1] - lo[1] > hi[axis] - lo[axis]) axis = 1;
   if (hi[2] - lo[2] > hi[axis] - lo[axis]) axis = 2;

   /* Partition idx around the median centroid on that axis. */
   uint32_t mid = count / 2;
   for (uint32_t i = 0; i < count; i++) {
      for (uint32_t j = i + 1; j < count; j++) {
         if (tris[idx[j]].centroid[axis] < tris[idx[i]].centroid[axis]) {
            uint32_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
         }
      }
   }
   (void)mid;

   uint32_t left  = build_blas_node(b, tris, idx, count / 2,
                                   geometry_id, geometry_flags);
   uint32_t right = build_blas_node(b, tris, idx + count / 2,
                                   count - count / 2,
                                   geometry_id, geometry_flags);
   if (left == VKRT_CPU_HANDLE_INVALID || right == VKRT_CPU_HANDLE_INVALID)
      return VKRT_CPU_HANDLE_INVALID;

   /* Union of the two children's bounds, read back from their nodes. */
   float bounds[6], lb[6], rb[6];
   memcpy(lb, b->data + BVH_IR_NODE_OFFSET + (left & ~3u), 24);
   memcpy(rb, b->data + BVH_IR_NODE_OFFSET + (right & ~3u), 24);
   aabb_merge(bounds, lb, rb);

   uint32_t off = buf_alloc(b, SZ_BOX_NODE);
   if (off == VKRT_CPU_HANDLE_INVALID)
      return VKRT_CPU_HANDLE_INVALID;
   put_aabb(b->data, off, bounds);
   put_u32(b->data, off + 24u, left);
   put_u32(b->data, off + 28u, right);
   put_u32(b->data, off + 32u, UINT32_MAX);            /* bvh_offset */
   put_u32(b->data, off + 36u, 0);            /* flags      */
   return (off - BVH_IR_NODE_OFFSET) | VKRT_CPU_NODE_INTERNAL;
}

static void
write_header(struct build_buf *b, uint32_t root, uint32_t capacity,
             uint32_t active, uint32_t type)
{
   float bounds[6];
   memcpy(bounds, b->data + BVH_IR_NODE_OFFSET + (root & ~3u), sizeof(bounds));
   for (uint32_t i = 0; i < 6; ++i) {
      uint32_t bits;
      memcpy(&bits, &bounds[i], 4);
      if (bits & 0x80000000u) bits = 0x80000000u - bits;
      put_u32(b->data, i * 4, bits);
   }
   put_u32(b->data, 24, active);
   put_u32(b->data, 32, BVH_IR_INTERNAL_COUNT(active));
   put_u32(b->data, 36, 1);
   put_u32(b->data, 40, 1);
   put_u32(b->data, BVH_IR_MAGIC_OFFSET, BVH_IR_MAGIC);
   put_u32(b->data, BVH_IR_VERSION_OFFSET, BVH_IR_VERSION);
   put_u32(b->data, BVH_IR_LEAF_CAPACITY_OFFSET, capacity);
   put_u32(b->data, BVH_IR_GEOMETRY_TYPE_OFFSET, type);
}

static uint32_t
wrap_leaf_root(struct build_buf *b, uint32_t child)
{
   float bounds[6];
   aabb_empty(bounds);
   if (child != UINT32_MAX)
      memcpy(bounds, b->data + BVH_IR_NODE_OFFSET + (child & ~3u), sizeof(bounds));
   uint32_t off = buf_alloc(b, SZ_BOX_NODE);
   if (off == UINT32_MAX) return UINT32_MAX;
   put_aabb(b->data, off, bounds);
   put_u32(b->data, off + 24, child);
   put_u32(b->data, off + 28, UINT32_MAX);
   put_u32(b->data, off + 32, UINT32_MAX);
   return (off - BVH_IR_NODE_OFFSET) | VKRT_CPU_NODE_INTERNAL;
}

int
vkrt_cpu_build_blas(const struct vkrt_cpu_geom *geom, struct vkrt_cpu_bvh *out)
{
   if (!geom || !out || (geom->triangle_count && !geom->vertices)) return -1;
   memset(out, 0, sizeof(*out));
   uint32_t n = geom->triangle_count;
   struct tri_ref *tris = calloc(n ? n : 1, sizeof(*tris));
   uint32_t *idx = calloc(n ? n : 1, sizeof(*idx));
   struct build_buf nodes = {0};
   if (!tris || !idx) goto fail;
   uint32_t active = 0;
   for (uint32_t t = 0; t < geom->triangle_count; t++) {
      uint32_t i0, i1, i2;
      if (geom->indices) {
         i0 = geom->indices[t * 3 + 0];
         i1 = geom->indices[t * 3 + 1];
         i2 = geom->indices[t * 3 + 2];
      } else {
         i0 = t * 3 + 0; i1 = t * 3 + 1; i2 = t * 3 + 2;
      }
      const float *p0 = geom->vertices + i0 * 3;
      const float *p1 = geom->vertices + i1 * 3;
      const float *p2 = geom->vertices + i2 * 3;
      memcpy(tris[t].v[0], p0, 12);
      memcpy(tris[t].v[1], p1, 12);
      memcpy(tris[t].v[2], p2, 12);
      aabb_empty(tris[t].bounds);
      aabb_expand(tris[t].bounds, p0);
      aabb_expand(tris[t].bounds, p1);
      aabb_expand(tris[t].bounds, p2);
      for (int k = 0; k < 3; k++)
         tris[t].centroid[k] = (p0[k] + p1[k] + p2[k]) / 3.0f;
      tris[t].original_index = t;
      bool valid = true;
      for (int k = 0; k < 3; ++k)
         for (int j = 0; j < 3; ++j)
            if (isnan(tris[t].v[k][j])) valid = false;
      if (valid) idx[active++] = t;
   }


   size_t leaf_end = BVH_IR_NODE_OFFSET + (size_t)n * SZ_TRIANGLE_NODE;
   if (leaf_end > UINT32_MAX || buf_alloc(&nodes, leaf_end) == UINT32_MAX) goto fail;
   uint32_t root = active ? build_blas_node(&nodes, tris, idx, active,
                     geom->geometry_id, geom->geometry_flags) : UINT32_MAX;
   if (active && root == UINT32_MAX) goto fail;
   if (active <= 1) root = wrap_leaf_root(&nodes, root);
   if (root == UINT32_MAX) goto fail;
   write_header(&nodes, root, n, active, 0);
   out->data = nodes.data;
   out->size = nodes.size;
   out->primitive_count = n;
   out->leaf_count = active;
   out->node_count = BVH_IR_INTERNAL_COUNT(active);
   free(tris); free(idx);
   return vkrt_cpu_encode_mesa_bvh4(out, geom->geometry_id, geom->geometry_flags, 0);
fail:
   free(nodes.data); free(tris); free(idx);
   return -1;
}

/* ------------------------------------------------------------------ */
/* TLAS                                                                */
/* ------------------------------------------------------------------ */

int
vkrt_cpu_build_tlas(const struct vkrt_cpu_instance *instances,
                    uint32_t instance_count, struct vkrt_cpu_bvh *out)
{
   if (!out || (instance_count && !instances)) return -1;
   memset(out, 0, sizeof(*out));
   struct build_buf nodes = {0};
   size_t leaf_end = BVH_IR_NODE_OFFSET + (size_t)instance_count * SZ_INSTANCE_NODE;
   if (leaf_end > UINT32_MAX || buf_alloc(&nodes, leaf_end) == UINT32_MAX) return -1;
   uint32_t *leaves = calloc(instance_count ? instance_count : 1, sizeof(*leaves));
   if (!leaves) { free(nodes.data); return -1; }
   uint32_t active = 0;
   for (uint32_t i = 0; i < instance_count; i++) {
      const struct vkrt_cpu_instance *in = &instances[i];
      if (!in->blas_base || !(in->mask & 0xffu)) continue;
      uint32_t off = BVH_IR_NODE_OFFSET + i * SZ_INSTANCE_NODE;
      if (off == VKRT_CPU_HANDLE_INVALID) {
         free(leaves);
         free(nodes.data);
         return -1;
      }
      float big[6] = { -1e30f, -1e30f, -1e30f, 1e30f, 1e30f, 1e30f };
      put_aabb(nodes.data, off, big);
      put_u64(nodes.data, off + 24u, in->blas_base);
      put_u32(nodes.data, off + 32u,
              ((in->mask & 0xFFu) << 24) | (in->custom_instance_index & 0x00FFFFFFu));
      put_u32(nodes.data, off + 36u,
              ((in->instance_flags & 0xFFu) << 24) |
              (in->sbt_record_offset & 0x00FFFFFFu));
      for (int r = 0; r < 3; r++)
         for (int c = 0; c < 4; c++)
            put_f32(nodes.data, off + 40u + ((uint32_t)r * 4u + (uint32_t)c) * 4u,
                    in->otw[r][c]);
      put_u32(nodes.data, off + 88u, in->instance_id);
      put_u32(nodes.data, off + 92u, 0);   /* root_flags */

      leaves[active++] = (off - BVH_IR_NODE_OFFSET) | VKRT_CPU_NODE_INSTANCE;
   }

   uint32_t cur = active ? leaves[0] : UINT32_MAX;
   for (uint32_t i = 1; i < active; i++) {
      uint32_t off = buf_alloc(&nodes, SZ_BOX_NODE);
      if (off == VKRT_CPU_HANDLE_INVALID) {
         free(leaves);
         free(nodes.data);
         return -1;
      }
      float lb[6], rb[6], ub[6];
      memcpy(lb, nodes.data + BVH_IR_NODE_OFFSET + (cur & ~3u), 24);
      memcpy(rb, nodes.data + BVH_IR_NODE_OFFSET + (leaves[i] & ~3u), 24);
      aabb_merge(ub, lb, rb);
      put_aabb(nodes.data, off, ub);
      put_u32(nodes.data, off + 24u, cur);
      put_u32(nodes.data, off + 28u, leaves[i]);
      put_u32(nodes.data, off + 32u, UINT32_MAX);
      put_u32(nodes.data, off + 36u, 0);
      cur = (off - BVH_IR_NODE_OFFSET) | VKRT_CPU_NODE_INTERNAL;
   }

   if (active <= 1) cur = wrap_leaf_root(&nodes, cur);
   if (cur == UINT32_MAX) { free(leaves); free(nodes.data); return -1; }
   write_header(&nodes, cur, instance_count, active, 2);
   out->data = nodes.data;
   out->size = nodes.size;
   out->leaf_count = active;
   out->primitive_count = 0;
   out->node_count = BVH_IR_INTERNAL_COUNT(active);
   free(leaves);
   return vkrt_cpu_encode_mesa_bvh4(out, 0, 0, 1);
}

void
vkrt_cpu_bvh_free(struct vkrt_cpu_bvh *b)
{
   if (!b)
      return;
   free(b->data);
   memset(b, 0, sizeof(*b));
}
