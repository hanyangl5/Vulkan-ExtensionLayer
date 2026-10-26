// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "bvh4_reference.h"

namespace vel::rt::bvh4 {
// bvh4_encode.comp push constants. All source/map/node address multiplication
// is 64-bit. Caller provides valid BuildNodes, a distinct mapping buffer with
// count uint32_t entries, and a PlanLayout(internal_count, counts) allocation.
// Initialize its header/offsets/counts and node_count=0 before phase zero.
// Phase 0 maps IDs/initializes parents; phase 1 emits hierarchy/root bounds.
// Separate phases with a compute shader write -> read/write memory barrier.
// Dispatch ceil(count/64) groups, chunking with first at device dispatch limits.
// Chunk spans must be multiples of 64, except the final span. output_as must
// be 128-byte aligned; source and mapping must be at least 4-byte aligned.
// Input/mapping/output must not alias. Leaf records are populated separately.
struct GpuEncodeArgs {
    uint64_t source, mapping, output_as, first;
    uint32_t count, root;
};
static_assert(std::is_standard_layout_v<BuildNode>);
static_assert(sizeof(BuildNode) == 36 && offsetof(BuildNode, left) == 24 && offsetof(BuildNode, leaf_id) == 32);
static_assert(sizeof(GpuEncodeArgs) == 40 && offsetof(GpuEncodeArgs, count) == 32);
}  // namespace vel::rt::bvh4
