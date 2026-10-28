// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vel::rt {
std::vector<uint32_t> CompileGLSL(const std::string &source, VkShaderStageFlagBits stage);
}  // namespace vel::rt
