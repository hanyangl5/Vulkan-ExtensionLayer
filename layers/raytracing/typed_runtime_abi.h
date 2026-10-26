// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
/* C/GLSL-compatible offsets in the immutable per-vkCmdTraceRays context.
 * All addresses, sizes, counts and SBT strides are little-endian uint64.
 * BDA component mode uses independent pool arenas. Production PrivateArray
 * mode ignores pool/bytes; arenaBytes must equal the linked Private budget.
 * LaunchBase selects the first flattened ray in the application's launch. */
#define VEL_RT_RUNTIME_MAGIC 0x52545056u
#define VEL_RT_RUNTIME_VERSION 1u
#define VEL_RT_RUNTIME_CONTEXT_BYTES 208u
#define VEL_RT_RUN_MAGIC 0u
#define VEL_RT_RUN_VERSION 4u
#define VEL_RT_RUN_POOL_ADDRESS 8u
#define VEL_RT_RUN_POOL_BYTES 16u
#define VEL_RT_RUN_ARENA_BYTES 24u
#define VEL_RT_RUN_INVOCATION_COUNT 32u
#define VEL_RT_RUN_WIDTH 40u
#define VEL_RT_RUN_HEIGHT 44u
#define VEL_RT_RUN_DEPTH 48u
#define VEL_RT_RUN_LAUNCH_BASE 56u
#define VEL_RT_RUN_AS_REGISTRY 64u
/* BDA of uint64 push snapshot addresses, indexed by compiled stage index.
 * Different RT stages may legally have different bytes at overlapping offsets. */
#define VEL_RT_RUN_PUSH_TABLE 72u
#define VEL_RT_RUN_RAYGEN_ADDRESS 80u
#define VEL_RT_RUN_RAYGEN_STRIDE 88u
#define VEL_RT_RUN_RAYGEN_SIZE 96u
#define VEL_RT_RUN_MISS_ADDRESS 104u
#define VEL_RT_RUN_MISS_STRIDE 112u
#define VEL_RT_RUN_MISS_SIZE 120u
#define VEL_RT_RUN_HIT_ADDRESS 128u
#define VEL_RT_RUN_HIT_STRIDE 136u
#define VEL_RT_RUN_HIT_SIZE 144u
#define VEL_RT_RUN_CALLABLE_ADDRESS 152u
#define VEL_RT_RUN_CALLABLE_STRIDE 160u
#define VEL_RT_RUN_CALLABLE_SIZE 168u
#define VEL_RT_RUN_PIPELINE_ID 176u
/* Optional uint32 status per local invocation; zero disables publication.
 * Atomic exchange publishes status, so simultaneous replay has no data race;
 * the final value belongs to one of the overlapping invocations. */
#define VEL_RT_RUN_STATUS_ADDRESS 184u
#define VEL_RT_RUN_STATUS_COUNT 192u
#define VEL_RT_SBT_HANDLE_BYTES 32u
#define VEL_RT_SBT_HANDLE_MAGIC 0x53425456u
#define VEL_RT_STATUS_SUCCESS 0u
#define VEL_RT_STATUS_CONTEXT 1u
#define VEL_RT_STATUS_STACK_OVERFLOW 2u
#define VEL_RT_STATUS_SBT 3u
#define VEL_RT_STATUS_PAYLOAD_TYPE 4u
#define VEL_RT_STATUS_RECURSION 5u
#define VEL_RT_STATUS_REQUEST 6u
#define VEL_RT_STATUS_ATTRIBUTE_TYPE 7u
