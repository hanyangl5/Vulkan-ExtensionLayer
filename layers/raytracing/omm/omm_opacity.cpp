// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#include "omm_opacity.h"

#include <algorithm>
#include <cmath>

namespace vel::omm {

namespace {

uint32_t format_bits(Format format) {
    switch (format) {
        case Format::TwoState:
            return 1;
        case Format::FourState:
            return 2;
    }
    return 0;
}

}  // namespace

Status encoded_data_size(Format format, uint32_t subdivision_level, uint64_t* bytes) {
    const uint32_t bits = format_bits(format);
    if (!bytes || !bits || subdivision_level > 31) return Status::InvalidArgument;
    const uint64_t triangles = uint64_t{1} << (2 * subdivision_level);
    // Dividing the triangle count before multiplication avoids overflowing the
    // bit count for representations near the 64-bit limit.
    *bytes = triangles / 8 * bits + ((triangles % 8 * bits + 7) / 8);
    return Status::Success;
}

Status decode_state(Format format, uint32_t subdivision_level, const uint8_t* data, size_t data_size, uint64_t microtriangle_index,
                    State* state) {
    if (!state) return Status::InvalidArgument;
    uint64_t bytes;
    const Status status = encoded_data_size(format, subdivision_level, &bytes);
    if (status != Status::Success) return status;
    const uint64_t triangle_count = uint64_t{1} << (2 * subdivision_level);
    if (microtriangle_index >= triangle_count || bytes > data_size) return Status::OutOfBounds;
    if (!data) return Status::InvalidArgument;
    const uint32_t bits = format_bits(format);
    const uint64_t byte = microtriangle_index / (8 / bits);
    const uint32_t shift = static_cast<uint32_t>(microtriangle_index % (8 / bits)) * bits;
    *state = static_cast<State>((data[byte] >> shift) & ((1u << bits) - 1u));
    return Status::Success;
}

Status decode_index(uint32_t raw_index, uint32_t width, uint32_t base_triangle, TriangleIndex* result) {
    if (!result || (width != 8 && width != 16 && width != 32)) return Status::InvalidArgument;
    const uint32_t maximum = width == 32 ? UINT32_MAX : (1u << width) - 1u;
    if (raw_index > maximum) return Status::InvalidArgument;
    TriangleIndex decoded;
    if (raw_index >= maximum - 3) {
        decoded.special = true;
        decoded.state = static_cast<State>(maximum - raw_index);
    } else {
        decoded.triangle = static_cast<uint64_t>(raw_index) + base_triangle;
    }
    *result = decoded;
    return Status::Success;
}

Status resolve_association(const Association& association, uint64_t geometry_triangle_index, TriangleIndex* result) {
    if (!result) return Status::InvalidArgument;
    TriangleIndex decoded;
    if (association.index_type == IndexType::Implicit) {
        if (geometry_triangle_index > UINT32_MAX) return Status::OutOfBounds;
        // EXT applies special-value recognition after selecting the source
        // index, including when that source is the implicit geometry index.
        const Status status = decode_index(static_cast<uint32_t>(geometry_triangle_index), 32, association.base_triangle, &decoded);
        if (status != Status::Success) return status;
    } else {
        uint32_t width;
        switch (association.index_type) {
            case IndexType::Uint16:
                width = 2;
                break;
            case IndexType::Uint32:
                width = 4;
                break;
            default:
                return Status::InvalidArgument;
        }
        if (association.index_stride > UINT32_MAX || association.index_stride % width) return Status::InvalidArgument;
        if (association.index_stride && geometry_triangle_index > UINT64_MAX / association.index_stride) return Status::Overflow;
        const uint64_t offset = geometry_triangle_index * association.index_stride;
        if (offset > association.indices_size || width > association.indices_size - offset) return Status::OutOfBounds;
        if (!association.indices) return Status::InvalidArgument;
        uint32_t raw = 0;
        for (uint32_t i = 0; i < width; ++i) raw |= static_cast<uint32_t>(association.indices[offset + i]) << (8 * i);
        const Status status = decode_index(raw, 8 * width, association.base_triangle, &decoded);
        if (status != Status::Success) return status;
    }
    if (!decoded.special && (!association.has_micromap || decoded.triangle >= association.micromap_triangle_count))
        return Status::OutOfBounds;
    *result = decoded;
    return Status::Success;
}

Status resolve_opacity(const OpacityInput& input, Decision* decision) {
    if (!decision) return Status::InvalidArgument;
    const uint32_t mutually_exclusive = input.ray_flags & (RayOpaque | RayNoOpaque | RayCullOpaque | RayCullNoOpaque);
    if (mutually_exclusive && (mutually_exclusive & (mutually_exclusive - 1))) return Status::InvalidArgument;
    if ((input.instance_flags & (InstanceForceOpaque | InstanceForceNoOpaque)) == (InstanceForceOpaque | InstanceForceNoOpaque))
        return Status::InvalidArgument;
    bool opaque = (input.geometry_flags & GeometryOpaque) != 0;
    if (input.has_micromap && !(input.instance_flags & InstanceDisableMicromaps)) {
        if (static_cast<uint32_t>(input.state) > 3) return Status::InvalidArgument;
        const bool force_two = (input.ray_flags & RayForceTwoState) || (input.instance_flags & InstanceForceTwoState);
        if (input.state == State::Transparent || (force_two && input.state == State::UnknownTransparent)) {
            *decision = Decision::Ignore;
            return Status::Success;
        }
        opaque = input.state == State::Opaque || (force_two && input.state == State::UnknownOpaque);
    }
    if (input.instance_flags & InstanceForceOpaque) opaque = true;
    if (input.instance_flags & InstanceForceNoOpaque) opaque = false;
    if (input.ray_flags & RayOpaque) opaque = true;
    if (input.ray_flags & RayNoOpaque) opaque = false;
    if ((opaque && (input.ray_flags & RayCullOpaque)) || (!opaque && (input.ray_flags & RayCullNoOpaque))) {
        *decision = Decision::Ignore;
    } else {
        *decision = opaque ? Decision::Opaque : Decision::NonOpaque;
    }
    return Status::Success;
}

Status barycentric_index(float u, float v, uint32_t subdivision_level, uint32_t* index) {
    if (!index || subdivision_level > 16 || !std::isfinite(u) || !std::isfinite(v)) return Status::InvalidArgument;
    // Quantization follows the Vulkan EXT appendix's reference mapping. The
    // integer bird-curve address is expressed here with scalar bit interleaving.
    // https://github.com/KhronosGroup/Vulkan-Docs/blob/main/appendices/VK_EXT_opacity_micromap.adoc
    const uint32_t edge = 1u << subdivision_level;
    const float scaled_u = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(edge);
    const float scaled_v = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(edge);
    uint32_t iu = static_cast<uint32_t>(scaled_u);
    uint32_t iv = static_cast<uint32_t>(scaled_v);
    const float fraction = (scaled_u - static_cast<float>(iu)) + (scaled_v - static_cast<float>(iv));
    iu = std::min(iu, edge - 1);
    iv = std::min(iv, edge - 1);
    const uint32_t sum = iu + iv;
    if (sum >= edge) iu -= sum - edge + 1;
    uint32_t iw = ~(iu + iv);
    if (fraction >= 1.0f && sum < edge - 1) --iw;
    const uint32_t even_bits = ~(iu ^ iw) & (edge - 1);
    const uint32_t transitions = (iu ^ iv) & even_bits;
    uint32_t parity = transitions;
    for (uint32_t shift = 1; shift < 16; shift *= 2) parity ^= parity >> shift;
    const uint32_t odd_bits = ((parity ^ iu) & ~even_bits) | transitions;
    uint32_t result = 0;
    for (uint32_t bit = 0; bit < subdivision_level; ++bit) {
        result |= ((even_bits >> bit) & 1u) << (2 * bit);
        result |= ((odd_bits >> bit) & 1u) << (2 * bit + 1);
    }
    *index = result;
    return Status::Success;
}

}  // namespace vel::omm
