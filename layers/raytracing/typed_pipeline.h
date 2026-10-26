// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "state_compiler.h"
#include <vulkan/vulkan.h>

namespace vel::rt {
// Specialization is applied before typed analysis, including default values.
// Every stage exports void step(uint64 frameBDA); the scheduler supplies the
// immutable system prefix, continuation state and typed invocation backing.
TypedContinuationProgram CompileTypedPipelineStage(const std::vector<uint32_t> &source, const std::string &entry,
                                                   VkShaderStageFlagBits stage, const VkSpecializationInfo *specialization,
                                                   const std::string &export_name);

// Link a compute scheduler with its typed stage libraries. Scheduler prototypes
// are compiled as empty void functions with one uint64 argument and matching
// export names; they become real SPIR-V imports before linking. No application
// source is converted to GLSL, and no native RT instruction reaches the output.
// The final result is validated for Vulkan, with complete resource interfaces.
std::vector<uint32_t> LinkTypedPipelineProgram(const std::vector<uint32_t> &scheduler,
                                               const std::vector<TypedContinuationProgram> &stages);
}  // namespace vel::rt
