// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_command_state.h"
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace vel::omm {
void CommandState::begin() noexcept { reset(); }
void CommandState::reset() noexcept {
    binding_ = ComputeBinding::None;
    pipeline_ = VK_NULL_HANDLE;
    shader_ = VK_NULL_HANDLE;
    pushes_.clear();
    error_ = VK_SUCCESS;
}
void CommandState::fail(VkResult error) noexcept {
    if (error_ == VK_SUCCESS && error != VK_SUCCESS) error_ = error;
}
void CommandState::bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline) noexcept {
    if (error_ != VK_SUCCESS || bind_point != VK_PIPELINE_BIND_POINT_COMPUTE) return;
    binding_ = ComputeBinding::Pipeline;
    pipeline_ = pipeline;
    shader_ = VK_NULL_HANDLE;
}
void CommandState::bind_shaders(uint32_t stage_count, const VkShaderStageFlagBits* stages, const VkShaderEXT* shaders) noexcept {
    if (error_ != VK_SUCCESS) return;
    for (uint32_t i = 0; i < stage_count; ++i) {
        if (stages[i] != VK_SHADER_STAGE_COMPUTE_BIT) continue;
        // Even NULL compute shader binding disturbs an earlier compute pipeline.
        binding_ = ComputeBinding::Shader;
        shader_ = shaders ? shaders[i] : VK_NULL_HANDLE;
        pipeline_ = VK_NULL_HANDLE;
    }
}
VkResult CommandState::push(PushLayoutRef layout, VkShaderStageFlags stage_flags, uint32_t offset, uint32_t size,
                            const void* values) noexcept {
    if (error_ != VK_SUCCESS) return error_;
    if (!layout || layout->native_layout == VK_NULL_HANDLE || !values || !size || !stage_flags || offset % 4 || size % 4 ||
        offset > std::numeric_limits<uint32_t>::max() - size) {
        fail(VK_ERROR_INITIALIZATION_FAILED);
        return error_;
    }
    try {
        Push push{std::move(layout), stage_flags, offset, std::vector<uint8_t>(size)};
        std::memcpy(push.bytes.data(), values, size);
        // Push's vector/shared_ptr moves are noexcept: failed growth leaves all
        // previously recorded calls and layout ownership unchanged.
        pushes_.push_back(std::move(push));
    } catch (const std::bad_alloc&) {
        fail(VK_ERROR_OUT_OF_HOST_MEMORY);
    } catch (const std::length_error&) {
        fail(VK_ERROR_OUT_OF_HOST_MEMORY);
    }
    return error_;
}
VkResult CommandState::restore(VkCommandBuffer command_buffer, const NativeComputeDispatch& dispatch) noexcept {
    if (error_ != VK_SUCCESS) return error_;
    // Validate the complete replay dispatch before emitting any native command.
    if ((binding_ == ComputeBinding::Pipeline && !dispatch.bind_pipeline) ||
        (binding_ == ComputeBinding::Shader && !dispatch.bind_shaders) || (!pushes_.empty() && !dispatch.push_constants)) {
        fail(VK_ERROR_INITIALIZATION_FAILED);
        return error_;
    }
    if (binding_ == ComputeBinding::Pipeline)
        dispatch.bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    else if (binding_ == ComputeBinding::Shader) {
        const VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
        dispatch.bind_shaders(command_buffer, 1, &stage, &shader_);
    }
    for (const auto& push : pushes_)
        dispatch.push_constants(command_buffer, push.layout->native_layout, push.stages, push.offset,
                                static_cast<uint32_t>(push.bytes.size()), push.bytes.data());
    return VK_SUCCESS;
}
}  // namespace vel::omm
