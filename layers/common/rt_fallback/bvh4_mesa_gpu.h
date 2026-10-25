// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bvh4_reference.h"
#include <cstddef>
#include <stdexcept>

namespace vel::rt::bvh4 {
// Direct GPU encoder for the default Mesa scalar IR in the existing VKRT v2
// envelope. The host knows only input capacity/type, not GPU active counts or
// topology. Plan output for capacity-1 nodes (zero for capacity <= 1), capacity
// leaves of the given kind, and mapping for capacity+max(capacity,2)-1 words.
// Header offsets/total_size are initialized from that capacity plan. Source,
// mapping, side tables, status and output must not alias. AS alignment is 128;
// source/mapping/flags/status are 4-byte aligned and the BLAS table is 8-aligned.
//
// Clear status to zero, then dispatch phases 0,1,2 (specialization constant 0),
// separated by compute write -> read/write barriers. Each phase dispatches
// ceil(max(capacity,1)/64) workgroups of 64 invocations, chunked at multiples
// of 64 invocations via first.
// The output counts, root and records are produced entirely on GPU. A nonzero
// status invalidates the output; consumers must gate on it before traversal.
//
// geometry_flags has geometry_count uint32_t entries with complete base flags.
// For TLAS, runtime_blas has capacity uint64_t entries indexed by source instance
// slot, explicitly translating each live instance to the shared runtime BLAS.
// The encoder never mistakes an old source-IR address for a runtime address.
// It retains binary arity; four-child collapse and production integration remain
// open. Mesa construction IDs remain 32-bit byte offsets; output math is 64-bit.
// Active bounds/vertices must be finite. Transform inversion currently uses
// scaled FP32 arithmetic and rejects singular/unrepresentable results; numerical
// qualification of ill-conditioned legal transforms remains a separate gate.
struct MesaGpuEncodeArgs {
    uint64_t source, source_size, output_as, mapping;
    uint64_t geometry_flags, runtime_blas, status, first;
    uint32_t capacity, geometry_type, geometry_count, reserved;
};
static_assert(sizeof(MesaGpuEncodeArgs) == 80 && offsetof(MesaGpuEncodeArgs, capacity) == 64);

enum MesaGpuEncodeError : uint32_t {
    MesaGpuMetadata = 1,
    MesaGpuReference = 2,
    MesaGpuDuplicate = 4,
    MesaGpuBounds = 8,
    MesaGpuGeometryFlags = 16,
    MesaGpuBlasAddress = 32,
    MesaGpuTransform = 64,
    MesaGpuCount = 128
};
inline uint64_t MesaGpuMappingWords(uint32_t capacity) { return uint64_t(capacity) + (capacity > 1 ? uint64_t(capacity) - 1 : 1); }
inline Layout PlanMesaGpuLayout(uint32_t capacity, uint32_t geometry_type) {
    if (geometry_type > 2) throw std::invalid_argument("Mesa GPU geometry type");
    Counts counts;
    if (geometry_type == 0)
        counts.triangles = capacity;
    else if (geometry_type == 1)
        counts.aabbs = capacity;
    else
        counts.instances = capacity;
    return PlanLayout(capacity > 1 ? uint64_t(capacity) - 1 : 0, counts);
}
}  // namespace vel::rt::bvh4
