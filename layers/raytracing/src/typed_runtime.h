// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "typed_pipeline.h"
#include "typed_runtime_abi.h"
#include <array>

namespace vel::rt {
struct TypedRuntimeBuildOptions {
    uint32_t max_ray_recursion_depth = 1;
    uint64_t pipeline_identity = 1;
    // A private compute layout supplies the immutable context through 8 push
    // bytes. The default specialization route remains available to components.
    bool context_via_push_constants = false;
    // DISABLE_OPTIMIZATION omits optional runtime performance passes. Required
    // source specialization, continuation lowering and stage linking remain.
    bool optimize = true;
};
struct TypedRuntimeProgram {
    std::vector<uint32_t> spirv;
    std::vector<std::array<uint8_t, VEL_RT_SBT_HANDLE_BYTES>> group_handles;
    uint64_t pipeline_identity = 0;
    // Arena sizing includes scheduler state, typed source storage and trace
    // traversal state. Runtime allocation checks the actual context capacity.
    // Applications can supply larger arenas; callable recursion has no finite
    // static bound and fails with STACK_OVERFLOW if the chosen arena is full.
    uint64_t recommended_arena_bytes = 0;
    uint64_t max_shader_frame_bytes = 0;
    uint32_t max_ray_recursion_depth = 0;
    uint32_t max_hit_attribute_bytes = 0;
    uint32_t private_frame_words = 0;
    std::vector<uint64_t> stage_stack_bytes;
};
struct TypedRuntimeArenaPlan {
    uint64_t default_arena_bytes = 0;
    uint64_t trace_node_bytes = 0;
    std::vector<uint64_t> stage_stack_bytes;
};
// Pure metadata planning, including runtime call/query state. Stage stack
// charges can be returned by GetRayTracingShaderGroupStackSizeKHR. The default
// follows the Vulkan Ray Tracing Pipeline Stack equation, including 2*callable.
TypedRuntimeArenaPlan PlanTypedRuntimeArena(const std::vector<TypedContinuationProgram> &stages,
                                            const std::vector<VkRayTracingShaderGroupCreateInfoKHR> &groups,
                                            uint32_t max_ray_recursion_depth);
uint64_t PlanTypedRuntimeArenaBytes(const std::vector<TypedContinuationProgram> &stages,
                                    const std::vector<VkRayTracingShaderGroupCreateInfoKHR> &groups,
                                    uint32_t max_ray_recursion_depth);
// Stages must be homogeneous PrivateArray (production) or BDA (component)
// libraries from CompileTypedPipelineStage, with unique
// exports. The result is one ordinary Vulkan compute program. Its private
// uint32 specialization IDs0/1 contain the immutable context BDA by default.
// context_via_push_constants instead uses an independent 8-byte compute push
// range, allowing one native pipeline to execute different recorded contexts.
// No application descriptor/push slot is reserved. SBT records use the returned
// 32-byte opaque handles; user shader-record data immediately follows the handle.
// PrivateArray keeps mutable scheduler/source state in native invocation-private
// storage, permitting command-buffer simultaneous reuse. Its byte budget is
// planned from actual stage frames, rather than a fixed implementation cap.
// Context/SBT/per-stage push snapshot table and AS registry remain alive until
// completion; BDA component mode additionally requires exclusive mutable pools.
TypedRuntimeProgram BuildTypedRuntimeProgram(const std::vector<TypedContinuationProgram> &stages,
                                             const std::vector<VkRayTracingShaderGroupCreateInfoKHR> &groups,
                                             const TypedRuntimeBuildOptions &options = {});
}  // namespace vel::rt
