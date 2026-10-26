// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#ifndef VEL_OMM_OPACITY_H
#define VEL_OMM_OPACITY_H

#include <cstddef>
#include <cstdint>

namespace vel::omm {

enum class Status { Success, InvalidArgument, OutOfBounds, Overflow };
enum class Format : uint32_t { TwoState = 1, FourState = 2 };
enum class State : uint32_t { Transparent = 0, Opaque = 1, UnknownTransparent = 2, UnknownOpaque = 3 };
enum class Decision { Ignore, Opaque, NonOpaque };
enum class IndexType { Implicit, Uint16, Uint32 };

// Values match SPIR-V RayFlags and VkGeometryInstanceFlagBitsKHR. Other ray and
// instance bits are deliberately ignored here; primitive/facing/mask culling
// belongs to the adapter that supplies the intersection candidate.
constexpr uint32_t RayOpaque = 0x1;
constexpr uint32_t RayNoOpaque = 0x2;
constexpr uint32_t RayCullOpaque = 0x40;
constexpr uint32_t RayCullNoOpaque = 0x80;
constexpr uint32_t RayForceTwoState = 0x400;
constexpr uint32_t InstanceForceOpaque = 0x4;
constexpr uint32_t InstanceForceNoOpaque = 0x8;
constexpr uint32_t InstanceForceTwoState = 0x10;
constexpr uint32_t InstanceDisableMicromaps = 0x20;
constexpr uint32_t GeometryOpaque = 0x1;

struct TriangleIndex {
    bool special = false;
    State state = State::Transparent;  // Meaningful only for a special index.
    uint64_t triangle = 0;             // Meaningful only for an ordinary index.
};

struct Association {
    IndexType index_type = IndexType::Implicit;
    const uint8_t* indices = nullptr;
    size_t indices_size = 0;
    uint64_t index_stride = 0;
    uint32_t base_triangle = 0;
    uint64_t micromap_triangle_count = 0;
    bool has_micromap = false;  // Whether an actual backing micromap object exists.
};

struct OpacityInput {
    // Whether this geometry has an OMM association. A special-only association
    // with a null VkMicromapEXT is still active and sets this to true.
    bool has_micromap = false;
    State state = State::Transparent;
    uint32_t ray_flags = 0;
    uint32_t instance_flags = 0;
    uint32_t geometry_flags = 0;
};

// Output parameters are left unchanged on failure. No function allocates memory
// or accesses any native acceleration-structure representation.
Status encoded_data_size(Format format, uint32_t subdivision_level, uint64_t* bytes);
Status decode_state(Format format, uint32_t subdivision_level, const uint8_t* data, size_t data_size, uint64_t microtriangle_index,
                    State* state);

// This raw helper recognizes special values at their actual stored width before
// adding base_triangle. Width 8 is useful to format tools, but EXT's public AS
// association currently permits only UINT16, UINT32 and implicit indexing.
Status decode_index(uint32_t raw_index, uint32_t width, uint32_t base_triangle, TriangleIndex* result);
Status resolve_association(const Association& association, uint64_t geometry_triangle_index, TriangleIndex* result);

// Implements only opacity determination and opacity culling, including ordinary
// non-OMM geometry. An ignored micromap result cannot be overridden back to a hit.
Status resolve_opacity(const OpacityInput& input, Decision* decision);

// Vulkan EXT reference quantization and bird-curve mapping. Finite coordinates
// are clamped as in the specification, including outside-triangle edge values.
// The 32-bit result represents up to subdivision level 16. This is a utility
// limit, not a reported device subdivision capability.
Status barycentric_index(float u, float v, uint32_t subdivision_level, uint32_t* index);

}  // namespace vel::omm

#endif
