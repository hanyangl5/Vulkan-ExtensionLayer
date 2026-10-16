// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace vel::rt {
// Software-AS ABI v2. All offsets in the GPU header are in uint32 words,
// except total_size (bytes). Data records carry geometry/primitive IDs so that
// sorting never changes the shader-visible indices. Separate inverse-sort and
// parent arrays survive BUILD and are reused by UPDATE.
constexpr uint32_t kASVersion = 2;
constexpr uint32_t kASHeaderWords = 16;
constexpr uint32_t kASNodeWords = 8;
constexpr uint32_t kASRecordWords = 32;
constexpr uint32_t kMaxASPrimitives = 1u << 20;
enum class BuildPass : uint32_t {
    Init,
    Load,
    Reduce,
    Morton,
    PreSort,
    OuterSort,
    InnerSort,
    Reorder,
    Hierarchy,
    ClearFit,
    Fit,
    Copy,
    Count
};
struct BuildParameters {
    uint64_t dst, scratch, geometries, src;
    uint32_t count, capacity, geometry_count, top;
    uint32_t k, j, update, flags;
};
static_assert(sizeof(BuildParameters) == 64);
struct GeometryInput {
    uint64_t vertices, indices, transform, stride;
    uint32_t count, base, first, offset;
    uint32_t transform_offset, index_type, geometry_type, flags;
};
static_assert(sizeof(GeometryInput) == 64);
uint32_t SortCapacity(uint32_t count);
uint64_t BuildScratchSize(uint32_t count);
uint64_t AccelerationStructureSize(uint32_t count, bool top_level, uint32_t geometry_count = 1);
}  // namespace vel::rt
