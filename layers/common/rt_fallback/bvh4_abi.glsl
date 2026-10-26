// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#ifndef VEL_RT_FALLBACK_BVH4_ABI_GLSL
#define VEL_RT_FALLBACK_BVH4_ABI_GLSL

// Shader view of bvh4_abi.h. All region offsets and address arithmetic are
// 64-bit. Packed scalar word loads avoid std430 vec3-array stride differences.
#define VEL_RT_BVH4_MAGIC 0x344c4556u
#define VEL_RT_BVH4_ABI_VERSION 2u
#define VEL_RT_BVH4_INVALID_ID 0xffffffffu
#define VEL_RT_BVH4_INDEX_MASK 0x3fffffffu
#define VEL_RT_BVH4_FLAG_INVALID 0x80000000u
#define VEL_RT_BVH4_INTERNAL 0u
#define VEL_RT_BVH4_TRIANGLE 1u
#define VEL_RT_BVH4_AABB 2u
#define VEL_RT_BVH4_INSTANCE 3u

#define VEL_RT_BVH4_HEADER_SIZE 128u
#define VEL_RT_BVH4_NODE_SIZE 128u
#define VEL_RT_BVH4_TRIANGLE_SIZE 48u
#define VEL_RT_BVH4_AABB_SIZE 48u
#define VEL_RT_BVH4_INSTANCE_SIZE 128u
#define VEL_RT_BVH4_HEADER_ROOT 16u
#define VEL_RT_BVH4_HEADER_NODE_COUNT 20u
#define VEL_RT_BVH4_HEADER_TRIANGLE_COUNT 24u
#define VEL_RT_BVH4_HEADER_AABB_COUNT 28u
#define VEL_RT_BVH4_HEADER_INSTANCE_COUNT 32u
#define VEL_RT_BVH4_HEADER_NODE_OFFSET 48u
#define VEL_RT_BVH4_HEADER_TRIANGLE_OFFSET 56u
#define VEL_RT_BVH4_HEADER_AABB_OFFSET 64u
#define VEL_RT_BVH4_HEADER_INSTANCE_OFFSET 72u
#define VEL_RT_BVH4_HEADER_PARENT_OFFSET 80u
#define VEL_RT_BVH4_HEADER_TOTAL_SIZE 88u
#define VEL_RT_BVH4_HEADER_BOUNDS_MIN 96u
#define VEL_RT_BVH4_HEADER_BOUNDS_MAX 108u
#define VEL_RT_BVH4_NODE_BOUNDS_MIN 0u
#define VEL_RT_BVH4_NODE_BOUNDS_MAX 48u
#define VEL_RT_BVH4_NODE_CHILDREN 96u
#define VEL_RT_BVH4_NODE_CHILD_COUNT 112u
#define VEL_RT_BVH4_NODE_PARENT 116u
#define VEL_RT_BVH4_INSTANCE_OTW 16u
#define VEL_RT_BVH4_INSTANCE_WTO 64u
#define VEL_RT_BVH4_INSTANCE_PUBLIC_ADDRESS 120u

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer VelRtBvh4Word { uint value; };

uint vel_rt_bvh4_u32(uint64_t base, uint64_t byte_offset) { return VelRtBvh4Word(base + byte_offset).value; }
float vel_rt_bvh4_f32(uint64_t base, uint64_t byte_offset) { return uintBitsToFloat(vel_rt_bvh4_u32(base, byte_offset)); }
uint64_t vel_rt_bvh4_u64(uint64_t base, uint byte_offset) {
    return uint64_t(vel_rt_bvh4_u32(base, uint64_t(byte_offset))) |
           (uint64_t(vel_rt_bvh4_u32(base, uint64_t(byte_offset) + 4ul)) << 32);
}
uint vel_rt_bvh4_kind(uint id) { return id >> 30; }
uint vel_rt_bvh4_index(uint id) { return id & VEL_RT_BVH4_INDEX_MASK; }
uint64_t vel_rt_bvh4_node(uint64_t base, uint id) {
    return base + vel_rt_bvh4_u64(base, VEL_RT_BVH4_HEADER_NODE_OFFSET) +
           uint64_t(vel_rt_bvh4_index(id)) * uint64_t(VEL_RT_BVH4_NODE_SIZE);
}
uint64_t vel_rt_bvh4_leaf(uint64_t base, uint id) {
    uint kind = vel_rt_bvh4_kind(id);
    uint offset = kind == VEL_RT_BVH4_TRIANGLE ? VEL_RT_BVH4_HEADER_TRIANGLE_OFFSET :
                  kind == VEL_RT_BVH4_AABB ? VEL_RT_BVH4_HEADER_AABB_OFFSET : VEL_RT_BVH4_HEADER_INSTANCE_OFFSET;
    uint stride = kind == VEL_RT_BVH4_INSTANCE ? VEL_RT_BVH4_INSTANCE_SIZE : VEL_RT_BVH4_TRIANGLE_SIZE;
    return base + vel_rt_bvh4_u64(base, offset) + uint64_t(vel_rt_bvh4_index(id)) * uint64_t(stride);
}
uint vel_rt_bvh4_parent(uint64_t base, uint id) {
    uint kind = vel_rt_bvh4_kind(id);
    if (kind == VEL_RT_BVH4_INTERNAL)
        return vel_rt_bvh4_u32(vel_rt_bvh4_node(base, id), uint64_t(VEL_RT_BVH4_NODE_PARENT));
    uint64_t index = uint64_t(vel_rt_bvh4_index(id));
    if (kind > VEL_RT_BVH4_TRIANGLE)
        index += uint64_t(vel_rt_bvh4_u32(base, uint64_t(VEL_RT_BVH4_HEADER_TRIANGLE_COUNT)));
    if (kind > VEL_RT_BVH4_AABB)
        index += uint64_t(vel_rt_bvh4_u32(base, uint64_t(VEL_RT_BVH4_HEADER_AABB_COUNT)));
    return vel_rt_bvh4_u32(base, vel_rt_bvh4_u64(base, VEL_RT_BVH4_HEADER_PARENT_OFFSET) + index * 4ul);
}

#endif
