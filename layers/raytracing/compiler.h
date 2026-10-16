// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "bvh.h"
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
std::vector<uint32_t> BuildShader(BuildPass pass);
}  // namespace vel::rt
