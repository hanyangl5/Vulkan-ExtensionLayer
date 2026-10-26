// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vel::omm {

// This is an internal host reference representation, not Vulkan's public
// serialization header. GPU builds and the native adapter are separate work.
enum class StorageFormat : uint32_t { TwoState = 1, FourState = 2 };
enum class StorageResult { Success, InvalidInput, Overflow, OutOfMemory, Incompatible };

struct StorageUsage {
    uint64_t count;
    uint32_t subdivision_level;
    StorageFormat format;
};

struct StorageLayout {
    uint64_t triangle_count;
    uint64_t records_offset;
    uint64_t data_offset;
    uint64_t size;
};

// The input triangle wire record is the Vulkan VkMicromapTriangleEXT layout:
// little-endian dataOffset:u32, subdivisionLevel:u16, format:u16. The parser
// reads bytes, so unaligned host pointers do not become unaligned C++ accesses.
struct StorageBuildInput {
    const uint8_t* triangles;
    size_t triangles_size;
    uint64_t triangle_count;
    uint64_t triangle_stride;
    const uint8_t* data;
    size_t data_size;
};

struct StoredTriangle {
    StorageFormat format;
    uint32_t subdivision_level;
    uint64_t data_offset;
    uint64_t data_size;
};

constexpr uint32_t kStorageVersion = 1;
constexpr uint64_t kStorageHeaderSize = 64;
constexpr uint64_t kStorageRecordSize = 24;

// Exactly one usage form is accepted for a nonzero count. Planning is allocation
// free and permits sizes that cannot be allocated by the current host.
StorageResult storage_plan(uint32_t usage_count, const StorageUsage* usages, const StorageUsage* const* usage_pointers,
                           StorageLayout* layout);
StorageResult storage_data_size(StorageFormat format, uint32_t subdivision_level, uint64_t* size);

// On failure, output vectors and output layouts remain unchanged. Build output
// contains no references to input arrays and masks unused bits/padding to zero.
StorageResult storage_build(const StorageBuildInput& input, std::vector<uint8_t>* output);
StorageResult storage_validate(const uint8_t* bytes, size_t size, StorageLayout* layout = nullptr);
StorageResult storage_triangle(const uint8_t* bytes, size_t size, uint64_t triangle_index, StoredTriangle* triangle);
StorageResult storage_clone(const uint8_t* bytes, size_t size, std::vector<uint8_t>* output);

// The reference serialization is byte-identical to the canonical internal blob.
// It intentionally does not claim to implement vkCmdCopyMicromapToMemoryEXT.
StorageResult storage_serialize(const uint8_t* bytes, size_t size, std::vector<uint8_t>* output);
StorageResult storage_deserialize(const uint8_t* bytes, size_t size, std::vector<uint8_t>* output);

}  // namespace vel::omm
