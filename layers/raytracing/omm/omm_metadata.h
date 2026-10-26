// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vel::omm {

constexpr uint32_t kMetadataVersion = 1;
constexpr uint32_t kMetadataRootMagic = 0x434d4d4f;  // LE "OMMC"
constexpr uint32_t kMetadataBodyMagic = 0x424d4d4f;  // LE "OMMB"
constexpr uint32_t kMetadataBlas = 1;
constexpr uint32_t kMetadataTlas = 2;
constexpr uint32_t kMetadataReady = 1;
constexpr uint32_t kMetadataAssociation = 1;
constexpr uint32_t kMetadataInstanceInactive = 0;
constexpr uint32_t kMetadataInstanceActive = 1;
constexpr uint32_t kMetadataTriangles = 0;
constexpr uint32_t kMetadataAabbs = 1;

// Runtime GPU ABI. Each BDA is an explicitly owned/readable device allocation,
// not an opaque native AS value. Relative offsets in a body are BVH independent.
// This pointer-bearing runtime layout is NOT a public serialization format.
struct MetadataRootCell {
    uint32_t magic, version, kind, flags;
    uint64_t body_address, body_capacity, generation;
    uint64_t reserved[3];
};
struct MetadataBodyHeader {
    uint32_t magic, version, kind, record_stride;
    uint64_t active_bytes, capacity, record_count, records_offset;
    uint32_t build_flags, reserved;
    uint64_t generation;
};
struct MetadataInstance {
    uint64_t blas_root_cell_address, native_blas_address;
    uint32_t original_flags, state;
    uint64_t reserved;
};
struct MetadataGeometry {
    uint32_t geometry_type, geometry_flags;
    uint64_t primitive_count, association_offset, association_bytes;
    uint64_t snapshot_offset, snapshot_capacity;
    uint32_t association_flags, reserved;
    uint64_t reserved64;
};
static_assert(sizeof(MetadataRootCell) == 64 && offsetof(MetadataRootCell, body_address) == 16 &&
              offsetof(MetadataRootCell, generation) == 32);
static_assert(sizeof(MetadataBodyHeader) == 64 && offsetof(MetadataBodyHeader, active_bytes) == 16 &&
              offsetof(MetadataBodyHeader, record_count) == 32 && offsetof(MetadataBodyHeader, build_flags) == 48);
static_assert(sizeof(MetadataInstance) == 32 && offsetof(MetadataInstance, original_flags) == 16);
static_assert(sizeof(MetadataGeometry) == 64 && offsetof(MetadataGeometry, primitive_count) == 8 &&
              offsetof(MetadataGeometry, association_offset) == 16 && offsetof(MetadataGeometry, snapshot_offset) == 32 &&
              offsetof(MetadataGeometry, association_flags) == 48);

enum class MetadataResult { Success, InvalidInput, OutOfBounds, Overflow, OutOfMemory, Incompatible };
struct MetadataGeometryPlan {
    uint32_t geometry_type, geometry_flags;
    uint64_t primitive_count;
    bool association;
    uint64_t snapshot_capacity;  // Zero for a special-only null-micromap association.
};
struct MetadataGeometryInput {
    MetadataGeometryPlan plan;
    const uint8_t* indices;
    size_t indices_size;
    uint64_t index_stride;
    uint32_t index_width, base_triangle;  // Width 0=implicit, 16/32=EXT.
    const uint8_t* snapshot;
    size_t snapshot_size;  // Known accessible source span; active size is in its GPU header.
};

// Internal producer job ABI: CPU-provided bounds/offsets, GPU-resident index and
// canonical micromap sources. Primitive ids start at zero in the built geometry;
// AS primitiveOffset/firstVertex do not offset this independent OMM index stream.
struct MetadataGeometrySource {
    MetadataGeometry geometry;
    uint64_t indices_address, indices_accessible_bytes, indices_offset, indices_bytes, index_stride;
    uint64_t snapshot_address, source_snapshot_capacity;
    uint32_t index_width, base_triangle;
};
struct MetadataBuildPush {
    uint64_t sources_address, output_address, output_capacity, status_address;
    uint64_t geometry_count, generation, geometry_base;
    uint32_t build_flags, phase;
};
static_assert(sizeof(MetadataGeometrySource) == 128 && offsetof(MetadataGeometrySource, indices_address) == 64 &&
              offsetof(MetadataGeometrySource, index_width) == 120);
static_assert(sizeof(MetadataBuildPush) == 64);

// Normalized association is one u64 per primitive. UINT64_MAX-state encodes the
// four specials. Ordinary base-adjusted indices retain their full 64-bit sum.
constexpr uint64_t metadata_special(uint32_t state) { return UINT64_MAX - state; }

// Checked allocation-free planner. Outputs remain unchanged on failure.
MetadataResult metadata_plan_blas(const MetadataGeometryPlan* plans, size_t count, MetadataGeometry* geometries,
                                  size_t geometry_capacity, uint64_t* body_bytes);
MetadataResult metadata_pack_blas(const MetadataGeometryInput* inputs, size_t count, uint32_t build_flags, uint64_t generation,
                                  std::vector<uint8_t>* output);
MetadataResult metadata_pack_tlas(const MetadataInstance* instances, size_t count, uint32_t build_flags, uint64_t generation,
                                  std::vector<uint8_t>* output);
MetadataResult metadata_validate_body(const uint8_t* body, size_t known_span, MetadataBodyHeader* header = nullptr);
MetadataResult metadata_clone_body(const uint8_t* body, size_t known_span, std::vector<uint8_t>* output);

}  // namespace vel::omm
