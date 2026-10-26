// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bvh4_reference.h"
#include <cstddef>
#include <functional>

namespace vel::rt::bvh4 {

struct ByteSpan {
    const void* data = nullptr;
    size_t size = 0;
};

enum class MesaGeometryType : uint32_t { Triangles = 0, Aabbs = 1, Instances = 2 };

// Mesa's scalar record ABI, as declared in layers/raytracing/bvh/vk_bvh_defines.h.
// Unlike the optional VKRT v2 envelope, Mesa itself takes header and node bases
// separately. Node IDs contain a 32-bit byte offset and a two-bit node kind.
// This adapter does not replace that construction-time capacity limit with an
// assertion that the final BVH4 representation has a 32-bit byte-offset limit.
struct MesaIrInput {
    ByteSpan header;
    ByteSpan nodes;
    uint32_t leaf_capacity = 0;
    MesaGeometryType geometry_type = MesaGeometryType::Triangles;
    uint32_t build_flags = 0;
};

using BlasAddressResolver = std::function<uint64_t(uint64_t)>;

struct MesaAdapted {
    // Compact generic input to Encode() or a GPU encoder. The empty tree has an
    // INVALID root; Mesa's empty/singleton wrapper box is intentionally removed.
    std::vector<BuildNode> tree;
    uint32_t root = VEL_RT_BVH4_INVALID_ID;
    std::vector<VelRtBvh4Triangle> triangles;
    std::vector<VelRtBvh4Aabb> aabbs;
    std::vector<VelRtBvh4Instance> instances;
    // Retain source slots independently of original app-visible primitive IDs,
    // which are not necessarily dense after filtering or across geometries.
    std::vector<uint32_t> triangle_source_slots, aabb_source_slots, instance_source_slots;
};

// Read only reachable leaves: inactive reserved slots may contain any bytes.
// The currently supported default BUILD record variant has build_flags == 0,
// no quad/ALWAYS_ACTIVE records, finite active bounds, and children-before-
// parents boxes. Malformed graphs, range/count/type/bounds inconsistencies and
// noninvertible instance transforms throw invalid_argument; ID capacity failures
// throw overflow_error. Caller owns valid backing storage for each byte span.
//
// geometry_flags is mandatory for each referenced BLAS geometry. Mesa's packed
// geometry field carries only opacity; the host table supplies the complete base
// Vulkan flags (OPAQUE=1, NO_DUPLICATE_ANY_HIT=2) without silently losing bit 2.
// Active TLAS instances require resolve_blas, even if an explicit identity mapping
// is intended: an old software-IR BLAS address must not accidentally become a
// shared-runtime BLAS address. The callback is not invoked for inactive slots.
MesaAdapted AdaptMesaIr(const MesaIrInput& input, const std::vector<uint32_t>& geometry_flags,
                        const BlasAddressResolver& resolve_blas = {});

// Convenience reader for the existing VKRT v2 envelope documented in
// layers/raytracing/docs/IR_CONTRACT_V2.md. It is not a Mesa or runtime BVH4 container ABI.
MesaAdapted AdaptMesaContainerV2(ByteSpan input, const std::vector<uint32_t>& geometry_flags,
                                 const BlasAddressResolver& resolve_blas = {});

}  // namespace vel::rt::bvh4
