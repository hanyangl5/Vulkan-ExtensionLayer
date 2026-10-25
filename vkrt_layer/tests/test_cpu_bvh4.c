/* SPDX-License-Identifier: MIT */
#include "vkrt_cpu_bvh.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(condition)                                            \
    do {                                                            \
        if (!(condition)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition); \
            ++failures;                                             \
        }                                                           \
    } while (0)

static VelRtBvh4Header header(const struct vkrt_cpu_bvh *bvh) {
    VelRtBvh4Header out;
    memcpy(&out, bvh->data, sizeof(out));
    CHECK(out.magic == VEL_RT_BVH4_MAGIC && out.version == VEL_RT_BVH4_ABI_VERSION && out.flags == 0);
    CHECK(out.total_size == bvh->size && bvh->size % 256 == 0);
    return out;
}

static void triangles(void) {
    /* Sparse source slots and an original ID differing from dense leaf IDs. */
    const float vertices[] = {5, 0, 0, 7, 0, 0, 6, 2, 0, NAN, 0, 0, 1, 0, 0, 0, 1, 0, -1, -1, 1, 1, -1, 1, 0, 1, 1};
    struct vkrt_cpu_geom geometry = {.vertices = vertices,
                                     .vertex_count = 9,
                                     .triangle_count = 3,
                                     .geometry_id = 2,
                                     .geometry_flags = VKRT_CPU_GEOMETRY_OPAQUE | VKRT_CPU_GEOMETRY_NO_DUPLICATE_ANY_HIT};
    struct vkrt_cpu_bvh bvh;
    CHECK(vkrt_cpu_build_blas(&geometry, &bvh) == 0);
    VelRtBvh4Header h = header(&bvh);
    CHECK(h.type == 1 && h.triangle_count == 2 && h.aabb_count == 0 && h.instance_count == 0);
    CHECK(bvh.primitive_count == 3 && bvh.leaf_count == 2 && bvh.node_count == h.node_count);
    CHECK((h.root_id >> VEL_RT_BVH4_KIND_SHIFT) == VEL_RT_BVH4_INTERNAL && h.node_count == 1);
    const VelRtBvh4Node *node = (const void *)(bvh.data + h.node_offset);
    CHECK(node->child_count == 2 && node->parent_id == VEL_RT_BVH4_INVALID_ID);
    const VelRtBvh4Triangle *leaves = (const void *)(bvh.data + h.triangle_offset);
    unsigned source_ids = 0;
    for (unsigned i = 0; i < h.triangle_count; ++i) {
        CHECK(leaves[i].geometry_id == 2 && leaves[i].geometry_flags == 3);
        CHECK(leaves[i].primitive_id == 0 || leaves[i].primitive_id == 2);
        source_ids |= 1u << leaves[i].primitive_id;
        uint32_t parent;
        memcpy(&parent, bvh.data + h.leaf_parent_offset + i * 4, 4);
        CHECK(parent == h.root_id);
    }
    CHECK(source_ids == 5);
    vkrt_cpu_bvh_free(&bvh);
    CHECK(bvh.data == NULL && bvh.size == 0);

    geometry.triangle_count = 1;
    CHECK(vkrt_cpu_build_blas(&geometry, &bvh) == 0);
    h = header(&bvh);
    CHECK(h.root_id == (VEL_RT_BVH4_TRIANGLE << VEL_RT_BVH4_KIND_SHIFT) && h.node_count == 0 && h.triangle_count == 1);
    vkrt_cpu_bvh_free(&bvh);
    geometry.triangle_count = 0;
    geometry.vertices = NULL;
    CHECK(vkrt_cpu_build_blas(&geometry, &bvh) == 0);
    h = header(&bvh);
    CHECK(h.root_id == VEL_RT_BVH4_INVALID_ID && h.node_count == 0 && h.triangle_count == 0);
    vkrt_cpu_bvh_free(&bvh);
}

static void instances(void) {
    const uint64_t address = UINT64_C(0x1234567800010010);
    struct vkrt_cpu_instance input[2] = {{.blas_base = address,
                                          .otw = {{2, 0, 0, 11}, {0, 3, 0, -7}, {0, 0, 4, 5}},
                                          .custom_instance_index = 0xabcdef,
                                          .mask = 0x81,
                                          .instance_id = 99,
                                          .instance_flags = 9,
                                          .sbt_record_offset = 0x654321},
                                         {.blas_base = address, .mask = 0}};
    struct vkrt_cpu_bvh bvh;
    CHECK(vkrt_cpu_build_tlas(input, 2, &bvh) == 0);
    VelRtBvh4Header h = header(&bvh);
    CHECK(h.type == 0 && h.instance_count == 1 && h.node_count == 0);
    CHECK(h.root_id == (VEL_RT_BVH4_INSTANCE << VEL_RT_BVH4_KIND_SHIFT));
    const VelRtBvh4Instance *leaf = (const void *)(bvh.data + h.instance_offset);
    CHECK(leaf->blas_address == address && (leaf->blas_address >> 32) == 0x12345678);
    CHECK(leaf->instance_id == 99 && leaf->custom_index_mask == 0x81abcdef && leaf->sbt_offset_flags == 0x09654321);
    CHECK(!memcmp(leaf->object_to_world, input[0].otw, sizeof(input[0].otw)));
    CHECK(fabsf(leaf->world_to_object[0] - 0.5f) < 1e-6f && fabsf(leaf->world_to_object[3] + 5.5f) < 1e-6f);
    CHECK(fabsf(leaf->world_to_object[5] - 1.f / 3) < 1e-6f && fabsf(leaf->world_to_object[7] - 7.f / 3) < 1e-6f);
    CHECK(fabsf(leaf->world_to_object[10] - 0.25f) < 1e-6f && fabsf(leaf->world_to_object[11] + 1.25f) < 1e-6f);
    vkrt_cpu_bvh_free(&bvh);
    CHECK(vkrt_cpu_build_tlas(NULL, 0, &bvh) == 0);
    h = header(&bvh);
    CHECK(h.root_id == VEL_RT_BVH4_INVALID_ID && h.instance_count == 0);
    vkrt_cpu_bvh_free(&bvh);
    /* The shared adapter rejects noninvertible active instance transforms. */
    memset(input[0].otw, 0, sizeof(input[0].otw));
    CHECK(vkrt_cpu_build_tlas(input, 1, &bvh) != 0 && bvh.data == NULL && bvh.size == 0);
}

int main(void) {
    triangles();
    instances();
    printf("CPU shared-BVH4 fixture checks: %u failures\n", failures);
    return failures != 0;
}
