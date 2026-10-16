// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vel::rt {
struct Stage {
    VkShaderStageFlagBits stage;
    std::vector<uint32_t> spirv;
    std::string entry = "main";
};
struct Program {
    std::vector<uint32_t> spirv;
    std::string glsl;
};
std::vector<uint32_t> CompileGLSL(const std::string &source, VkShaderStageFlagBits stage);
Program Link(const std::vector<Stage> &stages, const std::vector<VkRayTracingShaderGroupCreateInfoKHR> &groups,
             uint32_t internal_set);
std::vector<uint32_t> BuildShader();
// Header + complete binary tree + leaf data. Leaves are padded to a power of
// two.
uint64_t AccelerationStructureSize(uint32_t count, bool top_level);
}  // namespace vel::rt
