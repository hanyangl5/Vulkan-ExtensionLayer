// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#ifndef VEL_RT_BVH4_MESA_GPU_ABI_H
#define VEL_RT_BVH4_MESA_GPU_ABI_H
#include <stdint.h>

// Explicit identity relocation: source instance base_ptr already names a
// runtime BVH4 BLAS. This never detects or accepts a legacy runtime format.
#define VEL_RT_MESA_GPU_IDENTITY_BLAS UINT32_C(1)
typedef struct VelRtMesaGpuEncodeArgs {
    uint64_t source, source_size, output_as, mapping;
    uint64_t geometry_flags, runtime_blas, status, first;
    uint32_t capacity, geometry_type, geometry_count, options;
} VelRtMesaGpuEncodeArgs;
#endif
