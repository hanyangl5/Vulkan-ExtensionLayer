// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#ifndef VEL_OMM_LOOKUP_GLSL
#define VEL_OMM_LOOKUP_GLSL

#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// This include consumes the canonical, BVH-independent storage ABI v1 from
// omm_storage.h. It introduces no descriptor or push-constant bindings. Callers
// must supply known readable backing allocations, never arbitrary AS addresses.
// Nonzero BDA validity cannot be established by shader arithmetic alone.
const uint OMM_LOOKUP_SUCCESS = 0u;
const uint OMM_LOOKUP_INVALID = 1u;
const uint OMM_LOOKUP_BOUNDS = 2u;
const uint OMM_LOOKUP_OVERFLOW = 3u;
const uint OMM_LOOKUP_INCOMPATIBLE = 4u;

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer OmmLookupWord { uint value; };

struct OmmStorageSpan {
    uint64_t address;
    uint64_t bytes;
};

struct OmmIndexSpan {
    uint64_t address;       // Known word-aligned readable backing allocation.
    uint64_t backingBytes;  // A multiple of four, including readable tail padding.
    uint64_t offset;        // Logical index data may start at a half-word offset.
    uint64_t bytes;         // Logical data extent, excluding backing padding.
    uint64_t stride;
    uint width;  // 0=implicit, 16/32=EXT index types; 8=format-tool helper.
    uint baseTriangle;
};

bool ommLookupSpanValid(uint64_t address, uint64_t bytes) {
    return address != 0ul && (address & 3ul) == 0ul && (bytes & 3ul) == 0ul && bytes <= ~0ul - address;
}

uint ommLookupRead32(uint64_t address, uint64_t offset) { return OmmLookupWord(address + offset).value; }

uint64_t ommLookupRead64(uint64_t address, uint64_t offset) {
    return uint64_t(ommLookupRead32(address, offset)) | (uint64_t(ommLookupRead32(address, offset + 4ul)) << 32);
}

uint ommLookupBarycentricIndex(vec2 bary, uint level, inout uint index) {
    // Same mathematical quantization/curve as the CPU utility; the specification
    // gives the reference map in appendices/VK_EXT_opacity_micromap.adoc.
    if (level > 16u || any(isnan(bary)) || any(isinf(bary))) return OMM_LOOKUP_INVALID;
    uint edge = 1u << level;
    vec2 scaled = clamp(bary, vec2(0.0), vec2(1.0)) * float(edge);
    uvec2 cell = uvec2(scaled);
    vec2 fraction = scaled - vec2(cell);
    cell = min(cell, uvec2(edge - 1u));
    uint sum = cell.x + cell.y;
    if (sum >= edge) cell.x -= sum - edge + 1u;
    uint w = ~(cell.x + cell.y);
    if (fraction.x + fraction.y >= 1.0 && sum < edge - 1u) --w;
    uint evenBits = ~(cell.x ^ w) & (edge - 1u);
    uint transitions = (cell.x ^ cell.y) & evenBits;
    uint parity = transitions;
    for (uint shift = 1u; shift < 16u; shift *= 2u) parity ^= parity >> shift;
    uint oddBits = ((parity ^ cell.x) & ~evenBits) | transitions;
    uint result = 0u;
    for (uint bit = 0u; bit < level; ++bit) {
        result |= ((evenBits >> bit) & 1u) << (2u * bit);
        result |= ((oddBits >> bit) & 1u) << (2u * bit + 1u);
    }
    index = result;
    return OMM_LOOKUP_SUCCESS;
}

uint ommLookupDecodeIndex(uint raw, uint width, uint baseTriangle, inout bool special, inout uint state, inout uint64_t triangle) {
    if (width != 8u && width != 16u && width != 32u) return OMM_LOOKUP_INVALID;
    uint maximum = 0xffffffffu >> (32u - width);
    if (raw > maximum) return OMM_LOOKUP_INVALID;
    special = raw >= maximum - 3u;
    if (special)
        state = maximum - raw;
    else
        triangle = uint64_t(raw) + uint64_t(baseTriangle);
    return OMM_LOOKUP_SUCCESS;
}

uint ommLookupAssociation(OmmIndexSpan indices, uint64_t geometryTriangle, inout bool special, inout uint state,
                          inout uint64_t triangle) {
    if (indices.width == 0u) {
        if (geometryTriangle > 0xfffffffful) return OMM_LOOKUP_BOUNDS;
        // EXT special recognition follows source-index selection, including
        // implicit selection. KHR-specific validity rules belong to its adapter.
        return ommLookupDecodeIndex(uint(geometryTriangle), 32u, indices.baseTriangle, special, state, triangle);
    }
    if (indices.width != 8u && indices.width != 16u && indices.width != 32u) return OMM_LOOKUP_INVALID;
    uint64_t width = uint64_t(indices.width / 8u);
    if (indices.stride > 0xfffffffful || (indices.stride & (width - 1ul)) != 0ul) return OMM_LOOKUP_INVALID;
    if (!ommLookupSpanValid(indices.address, indices.backingBytes)) return OMM_LOOKUP_INVALID;
    if (indices.offset > indices.backingBytes || indices.bytes > indices.backingBytes - indices.offset) return OMM_LOOKUP_BOUNDS;
    // The stride is at most 32 bits. Splitting the product detects overflow
    // exactly without variable-width 64-bit division in the shader compiler.
    uint64_t low = (geometryTriangle & 0xfffffffful) * indices.stride;
    uint64_t high = (geometryTriangle >> 32u) * indices.stride;
    if (high > 0xfffffffful || low > ~0ul - (high << 32u)) return OMM_LOOKUP_OVERFLOW;
    uint64_t relative = low + (high << 32u);
    if (relative > indices.bytes || width > indices.bytes - relative) return OMM_LOOKUP_BOUNDS;
    uint64_t offset = indices.offset + relative;
    uint raw = 0u;
    for (uint byteIndex = 0u; byteIndex < uint(width); ++byteIndex) {
        uint64_t byteOffset = offset + uint64_t(byteIndex);
        uint word = ommLookupRead32(indices.address, byteOffset & ~3ul);
        raw |= ((word >> (uint(byteOffset & 3ul) * 8u)) & 255u) << (8u * byteIndex);
    }
    return ommLookupDecodeIndex(raw, indices.width, indices.baseTriangle, special, state, triangle);
}

uint ommLookupTriangle(OmmStorageSpan storage, uint64_t triangle, vec2 bary, inout uint state, inout uint microtriangle) {
    if (!ommLookupSpanValid(storage.address, storage.bytes)) return OMM_LOOKUP_INVALID;
    if (storage.bytes < 64ul || (storage.bytes & 15ul) != 0ul) return OMM_LOOKUP_BOUNDS;
    if (ommLookupRead32(storage.address, 0ul) != 0x4f4c4556u || ommLookupRead32(storage.address, 4ul) != 0x00004d4du ||
        ommLookupRead32(storage.address, 8ul) != 1u)
        return OMM_LOOKUP_INCOMPATIBLE;
    if (ommLookupRead32(storage.address, 12ul) != 64u || ommLookupRead64(storage.address, 16ul) != storage.bytes ||
        ommLookupRead64(storage.address, 32ul) != 64ul || ommLookupRead64(storage.address, 48ul) != 0ul ||
        ommLookupRead64(storage.address, 56ul) != 0ul)
        return OMM_LOOKUP_INVALID;
    uint64_t count = ommLookupRead64(storage.address, 24ul);
    if (count > (storage.bytes - 64ul) / 24ul) return OMM_LOOKUP_BOUNDS;
    uint64_t recordsEnd = 64ul + count * 24ul;
    if (recordsEnd > ~0ul - 15ul) return OMM_LOOKUP_OVERFLOW;
    uint64_t dataStart = (recordsEnd + 15ul) & ~15ul;
    if (dataStart > storage.bytes || ommLookupRead64(storage.address, 40ul) != dataStart) return OMM_LOOKUP_INVALID;
    if (triangle >= count) return OMM_LOOKUP_BOUNDS;
    uint64_t record = 64ul + triangle * 24ul;
    uint format = ommLookupRead32(storage.address, record);
    uint level = ommLookupRead32(storage.address, record + 4ul);
    if ((format != 1u && format != 2u) || level > 16u) return OMM_LOOKUP_INVALID;
    uint bits = format;
    uint64_t microtriangleCount = 1ul << (2u * level);
    uint64_t expectedBytes = (microtriangleCount * uint64_t(bits) + 7ul) / 8ul;
    uint64_t offset = ommLookupRead64(storage.address, record + 8ul);
    uint64_t bytes = ommLookupRead64(storage.address, record + 16ul);
    if (bytes != expectedBytes || offset < dataStart || (offset & 7ul) != 0ul) return OMM_LOOKUP_INVALID;
    if (offset > storage.bytes || bytes > storage.bytes - offset) return OMM_LOOKUP_BOUNDS;
    uint index = 0u;
    uint status = ommLookupBarycentricIndex(bary, level, index);
    if (status != OMM_LOOKUP_SUCCESS) return status;
    uint64_t byte = offset + uint64_t(index) / uint64_t(8u / bits);
    uint word = ommLookupRead32(storage.address, byte & ~3ul);
    uint byteValue = (word >> (uint(byte & 3ul) * 8u)) & 255u;
    state = (byteValue >> ((index % (8u / bits)) * bits)) & ((1u << bits) - 1u);
    microtriangle = index;
    return OMM_LOOKUP_SUCCESS;
}

uint ommLookupState(OmmStorageSpan storage, OmmIndexSpan indices, uint64_t geometryTriangle, vec2 bary, inout uint state,
                    inout uint microtriangle) {
    bool special = false;
    uint resolvedState = 0u;
    uint64_t triangle = 0ul;
    uint status = ommLookupAssociation(indices, geometryTriangle, special, resolvedState, triangle);
    if (status != OMM_LOOKUP_SUCCESS) return status;
    if (special) {
        state = resolvedState;
        microtriangle = 0u;
        return OMM_LOOKUP_SUCCESS;
    }
    return ommLookupTriangle(storage, triangle, bary, state, microtriangle);
}

#endif
