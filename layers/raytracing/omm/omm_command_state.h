// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace vel::omm {
// The provider creates a zero-descriptor layout with every original push range
// unchanged. Its shared_ptr deleter owns the native handle and device lifetime.
// Retaining the token must not depend on the application's layout remaining live.
struct PushLayoutSnapshot {
    VkPipelineLayout native_layout = VK_NULL_HANDLE;
};
using PushLayoutRef = std::shared_ptr<const PushLayoutSnapshot>;

struct NativeComputeDispatch {
    PFN_vkCmdBindPipeline bind_pipeline = nullptr;
    PFN_vkCmdBindShadersEXT bind_shaders = nullptr;
    PFN_vkCmdPushConstants push_constants = nullptr;
};

// One instance per command buffer, externally synchronized with that command
// buffer. The adapter owns the registry and invokes begin/reset only after a
// successful native begin/reset; freeing a CB destroys its state object.
// This does not support inherited compute/push state from other command buffers.
class CommandState {
  public:
    void begin() noexcept;
    void reset() noexcept;
    void fail(VkResult error) noexcept;
    VkResult status() const noexcept { return error_; }
    void bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline) noexcept;
    void bind_shaders(uint32_t stage_count, const VkShaderStageFlagBits* stages, const VkShaderEXT* shaders) noexcept;
    VkResult push(PushLayoutRef layout, VkShaderStageFlags stage_flags, uint32_t offset, uint32_t size,
                  const void* values) noexcept;
    // No allocations or descriptor binding changes. Replays all stages in the
    // original push order, even when only compute internal work was injected.
    // OOM is sticky until begin/reset; restore emits nothing after any error.
    VkResult restore(VkCommandBuffer command_buffer, const NativeComputeDispatch& dispatch) noexcept;

  private:
    enum class ComputeBinding { None, Pipeline, Shader };
    struct Push {
        PushLayoutRef layout;
        VkShaderStageFlags stages;
        uint32_t offset;
        std::vector<uint8_t> bytes;
    };
    ComputeBinding binding_ = ComputeBinding::None;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkShaderEXT shader_ = VK_NULL_HANDLE;
    std::vector<Push> pushes_;
    VkResult error_ = VK_SUCCESS;
};
}  // namespace vel::omm
