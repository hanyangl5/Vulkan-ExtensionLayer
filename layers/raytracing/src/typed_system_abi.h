// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

/* C/GLSL-compatible byte offsets. All scalar words are little-endian uint32;
 * vectors are tightly packed; transforms are four columns of three float32
 * components (column stride 12). uint64 addresses occupy low/high words. */
#define VEL_RT_SYSTEM_BYTES 256u
#define VEL_RT_SYS_LAUNCH_ID 0u
#define VEL_RT_SYS_LAUNCH_SIZE 12u
#define VEL_RT_SYS_RAY_FLAGS 24u
#define VEL_RT_SYS_WORLD_ORIGIN 28u
#define VEL_RT_SYS_WORLD_DIRECTION 40u
#define VEL_RT_SYS_OBJECT_ORIGIN 52u
#define VEL_RT_SYS_OBJECT_DIRECTION 64u
#define VEL_RT_SYS_TMIN 76u
#define VEL_RT_SYS_TMAX 80u
#define VEL_RT_SYS_INSTANCE_CUSTOM_INDEX 84u
#define VEL_RT_SYS_INSTANCE_ID 88u
#define VEL_RT_SYS_GEOMETRY_INDEX 92u
#define VEL_RT_SYS_PRIMITIVE_ID 96u
#define VEL_RT_SYS_HIT_KIND 100u
#define VEL_RT_SYS_OBJECT_TO_WORLD 104u
#define VEL_RT_SYS_WORLD_TO_OBJECT 152u
#define VEL_RT_SYS_SHADER_RECORD_ADDRESS 200u
#define VEL_RT_SYS_CULL_MASK 208u
#define VEL_RT_SYS_APP_PUSH_ADDRESS 216u
#define VEL_RT_SYS_CONTEXT_ADDRESS 224u

/* Standalone BDA compute-wrapper context. Production stage libraries take a
 * frame address directly and do not read a global/private context root. */
#define VEL_RT_CONTEXT_BYTES 40u
#define VEL_RT_CTX_FRAMES_ADDRESS 0u
#define VEL_RT_CTX_FRAME_STRIDE 8u
#define VEL_RT_CTX_FRAME_COUNT 16u
#define VEL_RT_CTX_FRAME_OFFSET 24u
#define VEL_RT_CTX_FRAME_CAPACITY 32u
