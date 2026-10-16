// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "bvh.h"
#include <algorithm>
#include <stdexcept>

namespace vel::rt {
uint32_t SortCapacity(uint32_t count) {
    if (count > kMaxASPrimitives) throw std::runtime_error("Acceleration structure primitive limit exceeded");
    uint32_t capacity = 1;
    while (capacity < count) capacity *= 2;
    return capacity;
}
uint64_t AccelerationStructureSize(uint32_t count, bool, uint32_t geometry_count) {
    SortCapacity(count);
    uint64_t nodes = count ? uint64_t(count) * 2 - 1 : 1;
    uint64_t words = kASHeaderWords + nodes * kASNodeWords + uint64_t(count) * kASRecordWords + geometry_count + count + nodes;
    return (words * 4 + 255) & ~uint64_t(255);
}
uint64_t BuildScratchSize(uint32_t count) {
    auto capacity = SortCapacity(count);
    // Scene AABB; per-workgroup reductions; unsorted records and bounds;
    // bitonic keys/indices; bottom-up fitting counters. None of these regions
    // alias while live. Vulkan scratch is a BDA, not a descriptor allocation.
    uint64_t words = 8 + uint64_t((count + 255) / 256) * 8 + uint64_t(count) * (kASRecordWords + 8) + uint64_t(capacity) * 2 +
                     (count ? count - 1 : 0);
    return (words * 4 + 255) & ~uint64_t(255);
}
}  // namespace vel::rt
