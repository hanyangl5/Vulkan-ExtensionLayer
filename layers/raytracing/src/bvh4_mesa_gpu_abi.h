// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#ifndef VEL_RT_BVH4_MESA_GPU_ABI_H
#define VEL_RT_BVH4_MESA_GPU_ABI_H
#include <stdint.h>

// Explicit identity relocation: source instance base_ptr already names a
// runtime BVH4 BLAS. This never detects or accepts a legacy runtime format.
#define VEL_RT_MESA_GPU_IDENTITY_BLAS UINT32_C(1)
/* Requires IDENTITY_BLAS; source values are public AS handles resolved through
 * specialization constant 6, then retained at instance byte 120. */
#define VEL_RT_MESA_GPU_PUBLIC_BLAS UINT32_C(2)
/* Reuse the encoder pipelines for topology-preserving runtime refit. */
#define VEL_RT_MESA_GPU_REFIT UINT32_C(0x100)
#define VEL_RT_MESA_GPU_ENCODE_PHASE_COUNT 7u

/* Leaf remap followed by input parents, two pointer-jump banks, and dense
 * retained-node IDs. All workspace sizes are byte-addressed in uint64. */
static inline uint64_t vel_rt_mesa_mapping_words(uint32_t capacity) {
    uint64_t internals = capacity > 1 ? (uint64_t)capacity - 1 : 1;
    return (uint64_t)capacity + 4 * internals;
}

/* Pointer jumping starts at a single parent edge. Round up to an even number
 * of doublings so the final result is always in bank A. No depth cap or GPU
 * count readback is needed: a valid tree has fewer edges than its capacity. */
static inline uint32_t vel_rt_mesa_jump_rounds(uint32_t capacity) {
    uint32_t rounds = 0;
    for (uint32_t value = capacity ? capacity - 1 : 0; value; value >>= 1) ++rounds;
    return (rounds + 1u) & ~1u;
}

static inline uint32_t vel_rt_mesa_encode_passes(uint32_t capacity) { return 5u + vel_rt_mesa_jump_rounds(capacity); }
static inline uint32_t vel_rt_mesa_encode_phase(uint32_t capacity, uint32_t pass) {
    if (pass < 3u) return pass;
    uint32_t rounds = vel_rt_mesa_jump_rounds(capacity);
    return pass < 3u + rounds ? 3u + ((pass - 3u) & 1u) : pass - rounds + 2u;
}
typedef struct VelRtMesaGpuEncodeArgs {
    uint64_t source, source_size, output_as, mapping;
    uint64_t geometry_flags, runtime_blas, status, first;
    uint32_t capacity, geometry_type, geometry_count, options;
} VelRtMesaGpuEncodeArgs;

/* Phase 0 copies source_as to output_as when distinct and initializes one
 * ready counter per retained internal. Phase 1 updates original leaves from
 * source_ir and propagates bounds with release/acquire child completion.
 * geometry_prefix contains geometry_count original primitive prefix offsets;
 * no dense-leaf-order assumption is made. Both phases use the encoder's
 * existing dispatch/chunk ABI and require a barrier between them. */
typedef struct VelRtBvh4RefitArgs {
    uint64_t source_as, source_ir, output_as, geometry_prefix;
    uint64_t geometry_flags, ready_counts, status, first;
    uint32_t capacity, geometry_type, geometry_count, options;
} VelRtBvh4RefitArgs;
#endif
