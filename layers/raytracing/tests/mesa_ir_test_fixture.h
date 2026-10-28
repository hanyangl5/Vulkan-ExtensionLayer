// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Raw-byte construction fixtures, shared by the CPU adapter and GPU encoder
// tests. Offsets below are independently pinned to Mesa's scalar record layout;
// the adapter test also checks them against the production declarations when
// Vulkan headers are available. This is test data, not another build algorithm.
#include "mesa_ir_adapter.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace vel::rt::bvh4::mesa_test {
inline void U32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    for (size_t byte = 0; byte < 4; ++byte) data.at(offset + byte) = uint8_t(value >> (byte * 8));
}
inline void U64(std::vector<uint8_t>& data, size_t offset, uint64_t value) {
    U32(data, offset, uint32_t(value));
    U32(data, offset + 4, uint32_t(value >> 32));
}
inline void F32(std::vector<uint8_t>& data, size_t offset, float value, bool emulated = false) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    if (emulated && (bits & 0x80000000u)) bits = 0x80000000u - bits;
    U32(data, offset, bits);
}
inline void Box(std::vector<uint8_t>& data, size_t offset, const Bounds& bounds, bool emulated = false) {
    for (size_t axis = 0; axis < 3; ++axis) {
        F32(data, offset + axis * 4, bounds.min[axis], emulated);
        F32(data, offset + 12 + axis * 4, bounds.max[axis], emulated);
    }
}

struct Fixture {
    uint32_t capacity, active, type, stride, internal_count;
    std::vector<uint8_t> bytes;
    Fixture(uint32_t capacity_value, uint32_t active_value, uint32_t type_value)
        : capacity(capacity_value),
          active(active_value),
          type(type_value),
          stride(type == 0   ? 68u
                 : type == 1 ? 32u
                             : 96u),
          internal_count(active > 1 ? active - 1 : 1),
          bytes(128 + size_t(capacity) * stride + size_t(internal_count) * 40, 0xcd) {
        std::fill_n(bytes.begin(), 128, uint8_t(0));
        Box(bytes, 0, {{-10000, -10000, -10000}, {10000, 10000, 10000}}, true);
        U32(bytes, 24, active);
        U32(bytes, 32, internal_count);
        U32(bytes, 36, 1);
        U32(bytes, 40, 1);
        // These driver/encoder scratch fields must never be mistaken for roots.
        U32(bytes, 76, 0xabcdef00u);
        U32(bytes, 80, 0xabcdef04u);
        U32(bytes, 92, 0xfedcba98u);
        U32(bytes, 108, 0x58424856u);
        U32(bytes, 112, 2);
        U32(bytes, 116, capacity);
        U32(bytes, 120, type);
    }
    ByteSpan Span() const { return {bytes.data(), bytes.size()}; }
    size_t Leaf(uint32_t slot) const { return 128 + size_t(slot) * stride; }
    size_t Internal(uint32_t index) const { return 128 + size_t(capacity) * stride + size_t(index) * 40; }
    uint32_t LeafId(uint32_t slot) const { return slot * stride | (type == 0 ? 0u : type == 1 ? 3u : 2u); }
    uint32_t InternalId(uint32_t index) const { return (capacity * stride + index * 40) | 1u; }
    void InternalBox(uint32_t index, uint32_t left, uint32_t right, Bounds bounds) {
        const size_t offset = Internal(index);
        Box(bytes, offset, bounds);
        U32(bytes, offset + 24, left);
        U32(bytes, offset + 28, right);
        U32(bytes, offset + 32, 0xffffffffu);
        U32(bytes, offset + 36, 0);
    }
    void Triangle(uint32_t slot, float x, uint32_t primitive, uint32_t geometry) {
        const size_t offset = Leaf(slot);
        Box(bytes, offset, {{x, 0, 0}, {x + 1, 1, 1}});
        const float vertices[9] = {x, 0, 0, x + 1, 0, 0, x, 1, 1};
        for (size_t i = 0; i < 9; ++i) F32(bytes, offset + 24 + i * 4, vertices[i]);
        U32(bytes, offset + 60, primitive);
        U32(bytes, offset + 64, geometry);
    }
    void Aabb(uint32_t slot, Bounds bounds, uint32_t primitive, uint32_t geometry) {
        const size_t offset = Leaf(slot);
        Box(bytes, offset, bounds);
        U32(bytes, offset + 24, primitive);
        U32(bytes, offset + 28, geometry);
    }
    void Instance(uint32_t slot, uint64_t address, uint32_t instance, const float* transform) {
        const size_t offset = Leaf(slot);
        Box(bytes, offset, {{-8, -8, -8}, {8, 8, 8}});
        U64(bytes, offset + 24, address);
        U32(bytes, offset + 32, 0xab123456u);
        U32(bytes, offset + 36, 0x05876543u);
        for (size_t i = 0; i < 12; ++i) F32(bytes, offset + 40 + i * 4, transform[i]);
        U32(bytes, offset + 88, instance);
        U32(bytes, offset + 92, 0xfdu);  // Mesa optimization flags, not Vulkan flags.
    }
    void Empty() {
        const float infinity = std::numeric_limits<float>::infinity();
        const Bounds empty{{infinity, infinity, infinity}, {-infinity, -infinity, -infinity}};
        Box(bytes, 0, empty, true);
        InternalBox(0, 0xffffffffu, 0xffffffffu, empty);
    }
};

inline Fixture SparseTriangles() {
    Fixture fixture(5, 3, 0);
    fixture.Triangle(0, -3, 27, 0x80000001u);
    fixture.Triangle(2, 0, 4, 2);
    fixture.Triangle(4, 3, 91, 0x80000001u);
    fixture.InternalBox(0, fixture.LeafId(2), fixture.LeafId(0), {{-3, 0, 0}, {1, 1, 1}});
    fixture.InternalBox(1, fixture.InternalId(0), fixture.LeafId(4), {{-3, 0, 0}, {4, 1, 1}});
    return fixture;
}
inline std::vector<uint32_t> SparseTriangleFlags() { return {0, 3, 2}; }
}  // namespace vel::rt::bvh4::mesa_test
