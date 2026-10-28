/* SPDX-License-Identifier: MIT */
#ifndef BVH_IR_CONTRACT_H
#define BVH_IR_CONTRACT_H

/* Shared XM/VKRT container. Mesa's 108-byte record ABI is unchanged.
 * Metadata is outside vk_ir_header; node IDs are relative to byte 128.
 * No legacy layouts are accepted. This file must be identical in both projects. */
#define BVH_IR_HEADER_SIZE 108u
#define BVH_IR_NODE_OFFSET 128u
#define BVH_IR_MAGIC_OFFSET 108u
#define BVH_IR_VERSION_OFFSET 112u
#define BVH_IR_LEAF_CAPACITY_OFFSET 116u
#define BVH_IR_GEOMETRY_TYPE_OFFSET 120u
#define BVH_IR_BUILD_FLAGS_OFFSET 124u
#define BVH_IR_MAGIC 0x58424856u
#define BVH_IR_VERSION 2u
#define BVH_IR_INVALID_ID 0xffffffffu
#define BVH_IR_BOX_SIZE 40u
#define BVH_IR_TRIANGLE_SIZE 68u
#define BVH_IR_AABB_SIZE 32u
#define BVH_IR_INSTANCE_SIZE 96u
#define BVH_IR_INTERNAL_COUNT(active) (((active) > 1u) ? (active) - 1u : 1u)
#define BVH_IR_BOX_OFFSET(capacity, stride) ((capacity) * (stride))
#define BVH_IR_ROOT_ID(capacity, stride, count) \
   ((BVH_IR_BOX_OFFSET((capacity), (stride)) + ((count) - 1u) * BVH_IR_BOX_SIZE) | 1u)

/* BUILD baseline: no prepaired quad leaves and no ALWAYS_ACTIVE override.
 * Flags which only affect builder implementation do not change this container. */
#define BVH_IR_SUPPORTED_INPUT_FLAGS 0u
#endif
