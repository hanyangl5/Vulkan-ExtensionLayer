/* SPDX-License-Identifier: MIT */
#ifndef BVH_IR_CONTRACT_H
#define BVH_IR_CONTRACT_H

/* Layer-owned construction container. Mesa's 108-byte scalar header/record
 * layout is retained. Metadata outside vk_ir_header identifies tagged byte
 * IDs (v2) or record-index IDs with 64-bit address decoding (v3). Neither is
 * the public runtime BVH4 or serialization ABI. */
#define BVH_IR_HEADER_SIZE 108u
#define BVH_IR_NODE_OFFSET 128u
#define BVH_IR_MAGIC_OFFSET 108u
#define BVH_IR_VERSION_OFFSET 112u
#define BVH_IR_LEAF_CAPACITY_OFFSET 116u
#define BVH_IR_GEOMETRY_TYPE_OFFSET 120u
#define BVH_IR_BUILD_FLAGS_OFFSET 124u
/* driver_internal[5] is named producer_id by this adapter. Mesa reserves these
 * fields for its consumer; neither the CPU adapter nor BVH4 encoder requires
 * them to be zero. This diagnostic is transient IR provenance, not a runtime
 * BVH4/serialization field or builder input flag. Common INIT writes zero;
 * an actual PLOC/HPLOC invocation writes its ID. driver_internal[4] records
 * the key width written by the executed sorting shader, rather than a host
 * selection flag. Both remain zero until their producing shaders run. */
#define BVH_IR_KEY_BITS_OFFSET 100u
#define BVH_IR_PRODUCER_OFFSET 104u
#define BVH_IR_PRODUCER_PLOC 0x504c4f43u
#define BVH_IR_PRODUCER_HPLOC 0x48504c43u
#define BVH_IR_MAGIC 0x58424856u
#define BVH_IR_VERSION 2u
/* v2 stores tagged byte offsets. v3 stores tagged record indices so address
 * arithmetic can cover large construction storage without widening child IDs.
 * Mesa scalar records and the final public BVH4/serialization ABI are unchanged. */
#define BVH_IR_INDEXED_VERSION 3u
#define BVH_IR_MAX_LEAVES 0x1fffffffu
#define BVH_IR_INVALID_ID 0xffffffffu
#define BVH_IR_BOX_SIZE 40u
#define BVH_IR_TRIANGLE_SIZE 68u
#define BVH_IR_AABB_SIZE 32u
#define BVH_IR_INSTANCE_SIZE 96u
/* The largest default sorter variant has 256 key64/id triples (12 bytes)
 * and four shared scalar indices. PLOC's larger LDS is admitted separately. */
#define BVH_IR_SORT_SHARED_BYTES (256u * 12u + 4u * 4u)
#define BVH_IR_INTERNAL_COUNT(active) (((active) > 1u) ? (active) - 1u : 1u)
#define BVH_IR_BOX_OFFSET(capacity, stride) ((capacity) * (stride))
#define BVH_IR_ROOT_ID(capacity, stride, count) \
   ((BVH_IR_BOX_OFFSET((capacity), (stride)) + ((count) - 1u) * BVH_IR_BOX_SIZE) | 1u)

/* BUILD baseline: no prepaired quad leaves and no ALWAYS_ACTIVE override.
 * Flags which only affect builder implementation do not change this container. */
#define BVH_IR_SUPPORTED_INPUT_FLAGS 0u
#endif
