// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <spirv-tools/libspirv.h>
#include <cstdint>

namespace vel::rt {
// The private backend's minimum is genuine SPIR-V 1.4, usable with Vulkan
// 1.1 + VK_KHR_spirv_1_4. Higher-version application modules retain their
// version and their corresponding validation rules throughout compilation.
inline spv_target_env VulkanEnvironmentForSPIRV(uint32_t version) {
    if (version <= 0x00010400u) return SPV_ENV_VULKAN_1_1_SPIRV_1_4;
    return version <= 0x00010500u ? SPV_ENV_VULKAN_1_2 : SPV_ENV_VULKAN_1_3;
}
inline spv_target_env UniversalEnvironmentForSPIRV(uint32_t version) {
    if (version <= 0x00010400u) return SPV_ENV_UNIVERSAL_1_4;
    return version <= 0x00010500u ? SPV_ENV_UNIVERSAL_1_5 : SPV_ENV_UNIVERSAL_1_6;
}
}  // namespace vel::rt
