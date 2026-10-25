// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#ifndef VEL_RT_FALLBACK_AS_ADDRESS_GLSL
#define VEL_RT_FALLBACK_AS_ADDRESS_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// AS-only public addresses are opaque 256-byte-aligned values, not raw BDAs.
// The immutable 64-byte context header has eight uint32 fields followed by
// four uint64 fields: magic, version, entry_stride_log2, radix_bits, levels,
// address_shift, reserved[2]; root_page_bda, reserved[3]. All pages contain
// 256 eight-byte entries; six directory levels precede the physical-root leaf.
// The host must not modify/reclaim any entry read by an in-flight valid lookup.
// First-use publication precedes queue submission; CPU atomics alone provide
// no host/device synchronization. Two word loads are safe under that contract.
#define VEL_RT_AS_ADDRESS_MAGIC 0x52534156u
#define VEL_RT_AS_ADDRESS_VERSION 1u
#define VEL_RT_AS_ADDRESS_HEADER_SIZE 64u
#define VEL_RT_AS_ADDRESS_PAGE_ENTRIES 256u
#define VEL_RT_AS_ADDRESS_PAGE_SIZE 2048u

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer VelRtAsAddressWord { uint value; };

uint vel_rt_as_address_u32(uint64_t address) { return VelRtAsAddressWord(address).value; }
uint64_t vel_rt_as_address_u64(uint64_t address) {
    return uint64_t(vel_rt_as_address_u32(address)) | (uint64_t(vel_rt_as_address_u32(address + uint64_t(4))) << 32);
}

uint64_t vel_rt_resolve_as(uint64_t context, uint64_t public_value) {
    if (context == uint64_t(0) || (context & uint64_t(3)) != uint64_t(0) ||
        public_value == uint64_t(0) || (public_value & uint64_t(255)) != uint64_t(0))
        return uint64_t(0);
    if (vel_rt_as_address_u32(context) != VEL_RT_AS_ADDRESS_MAGIC ||
        vel_rt_as_address_u32(context + uint64_t(4)) != VEL_RT_AS_ADDRESS_VERSION ||
        vel_rt_as_address_u32(context + uint64_t(8)) != 3u ||
        vel_rt_as_address_u32(context + uint64_t(12)) != 8u ||
        vel_rt_as_address_u32(context + uint64_t(16)) != 7u ||
        vel_rt_as_address_u32(context + uint64_t(20)) != 8u)
        return uint64_t(0);

    uint64_t page = vel_rt_as_address_u64(context + uint64_t(32));
    uint64_t key = public_value >> 8;
    for (uint depth = 0u; depth < 7u; ++depth) {
        if (page == uint64_t(0) || (page & uint64_t(3)) != uint64_t(0)) return uint64_t(0);
        uint slot = uint((key >> ((6u - depth) * 8u)) & uint64_t(255));
        page = vel_rt_as_address_u64(page + uint64_t(slot) * uint64_t(8));
    }
    // The BVH4 decoder uses scalar word loads. Its physical header may be
    // merely 4/16-byte aligned even though the public address is 256-aligned.
    return (page & uint64_t(3)) == uint64_t(0) ? page : uint64_t(0);
}

#endif
