// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bvh4_reference.h"
#include "bvh4_mesa_gpu_abi.h"
#include <cstddef>
#include <stdexcept>

namespace vel::rt::bvh4 {
// Direct GPU encoder for the default Mesa scalar IR in the existing VKRT v2
// envelope. The host knows only input capacity/type, not GPU active counts or
// topology. Plan output for capacity-1 nodes (zero for capacity <= 1), capacity
// leaves of the given kind, and vel_rt_mesa_mapping_words(capacity) words.
// Header offsets/total_size are initialized from that capacity plan. Source,
// mapping, side tables, status and output must not alias. Physical AS roots and
// source/mapping/flags/status are 4-byte aligned; the BLAS table is 8-aligned.
// Region offsets retain the planner's preferred 128-byte node alignment.
// Public Vulkan AS values are separately 256-aligned and require resolution.
//
// Clear status to zero, then dispatch phases 0,1,2; alternate phases 3 and 4
// for vel_rt_mesa_jump_rounds(capacity) total passes; finish with phases 5,6.
// Every phase/pass is separated by a compute write -> read/write barrier.
// Each phase dispatches
// ceil(max(capacity,1)/64) workgroups of 64 invocations, chunked at multiples
// of 64 invocations via first.
// The output counts, root and records are produced entirely on GPU. A nonzero
// status also atomically sets FLAG_INVALID in the runtime header, so consumers
// can reject failed output without retaining a pointer to construction scratch.
//
// geometry_flags has geometry_count uint32_t entries with complete base flags.
// For TLAS, runtime_blas has capacity uint64_t entries indexed by source instance
// slot, explicitly translating each live instance to the shared runtime BLAS.
// Alternatively, options=IDENTITY_BLAS and runtime_blas=0 explicitly declares
// that source instance addresses already name final runtime BLASes.
// IDENTITY_BLAS|PUBLIC_BLAS instead declares public AS values: specialize
// constant 6 with the AS-address registry context, resolve physical BLAS roots,
// and retain those public values for canonical serialization relocation.
// The encoder never mistakes an old source-IR address for a runtime address.
// Production RQ/RTP share this encoder. It retains even-depth input internals
// and expands their odd-depth children into two to four output children.
// Parallel pointer jumping derives depth parity without polling or a fixed
// tree-depth limit. Mesa construction IDs remain 32-bit byte offsets; output
// and workspace math is 64-bit. This is a two-level collapse, not SAH selection.
// Active bounds/vertices must be finite. Transform inversion currently uses
// scaled FP32 arithmetic and rejects singular/unrepresentable results; numerical
// qualification of ill-conditioned legal transforms remains a separate gate.
using MesaGpuEncodeArgs = VelRtMesaGpuEncodeArgs;
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
inline uint64_t MesaGpuMappingWords(uint32_t capacity) { return vel_rt_mesa_mapping_words(capacity); }
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
