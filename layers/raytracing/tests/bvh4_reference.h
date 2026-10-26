// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bvh4_abi.h"
#include <array>
#include <cstdint>
#include <vector>

namespace vel::rt::bvh4 {

struct Bounds {
    std::array<float, 3> min, max;
};

// Compact, generic binary tree input. Internal bounds must conservatively
// contain their children. Leaf indices are dense separately for each kind.
// This adapter is intentionally independent of Mesa's private host runtime.
struct BuildNode {
    Bounds bounds;
    uint32_t left = VEL_RT_BVH4_INVALID_ID;
    uint32_t right = VEL_RT_BVH4_INVALID_ID;
    uint32_t leaf_id = VEL_RT_BVH4_INVALID_ID;
};

struct Counts {
    uint64_t triangles = 0, aabbs = 0, instances = 0;
};

struct Layout {
    uint64_t nodes = 0, triangles = 0, aabbs = 0, instances = 0;
    uint64_t leaf_parents = 0, size = 0;
};

struct Encoded {
    uint32_t root_id = VEL_RT_BVH4_INVALID_ID;
    std::vector<VelRtBvh4Node> nodes;
    std::vector<uint32_t> leaf_parents;
    Counts counts;
};

// Throws invalid_argument for malformed inputs and overflow_error for values
// outside the ABI or 64-bit arithmetic. Planning does not allocate AS storage.
uint32_t MakeId(uint32_t kind, uint64_t index);
Layout PlanLayout(uint64_t node_count, Counts counts);
Encoded Encode(const std::vector<BuildNode>& tree, uint32_t root);

}  // namespace vel::rt::bvh4
