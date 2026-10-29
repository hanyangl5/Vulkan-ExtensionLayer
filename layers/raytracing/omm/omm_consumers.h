// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "omm_descriptors.h"

namespace vel::omm {
struct ConsumerBackend;
struct ConsumerPipeline;
struct ConsumerLayout;
struct ConsumerBindingState {
    VkPipeline logical_pipeline = VK_NULL_HANDLE;
    // Last pipeline actually recorded by the layer, updated by the recorder
    // only after a successful native bind (not by consumer_pipeline).
    VkPipeline native_pipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;
    std::vector<std::shared_ptr<const ConsumerLayout>> set_layouts;
    // Native variants and immutable contexts remain alive throughout executable
    // CB lifetime. Reset only after a successful native CB begin/reset.
    std::vector<std::shared_ptr<void>> retained;
    void reset() {
        logical_pipeline = VK_NULL_HANDLE;
        native_pipeline = VK_NULL_HANDLE;
        sets.clear();
        set_layouts.clear();
        retained.clear();
    }
};
struct ConsumerBackendCreateInfo {
    VkDevice device{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    DescriptorBackend* descriptors{};
    const VkAllocationCallbacks* allocator{};
    const uint32_t* library_code{};
    size_t library_code_size{};
};
VkResult consumer_backend_create(const ConsumerBackendCreateInfo&, ConsumerBackend**);
void consumer_backend_destroy(ConsumerBackend*);
VkResult consumer_track_set_layout(ConsumerBackend*, VkDescriptorSetLayout, const VkDescriptorSetLayoutCreateInfo*);
void consumer_untrack_set_layout(ConsumerBackend*, VkDescriptorSetLayout);
VkResult consumer_track_layout(ConsumerBackend*, VkPipelineLayout, const VkPipelineLayoutCreateInfo*);
void consumer_untrack_layout(ConsumerBackend*, VkPipelineLayout);
VkResult consumer_track_render_pass(ConsumerBackend*, VkRenderPass, const VkRenderPassCreateInfo*);
VkResult consumer_track_render_pass2(ConsumerBackend*, VkRenderPass, const VkRenderPassCreateInfo2*);
void consumer_untrack_render_pass(ConsumerBackend*, VkRenderPass);
VkResult consumer_create_shader(ConsumerBackend*, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
void consumer_destroy_shader(ConsumerBackend*, VkShaderModule, const VkAllocationCallbacks*);
// Allocation-free conservative source inspection, safe inside a public hook.
bool consumer_shader_uses_query(ConsumerBackend*, const VkPipelineShaderStageCreateInfo&) noexcept;
VkResult consumer_create_compute(ConsumerBackend*, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*,
                                 const VkAllocationCallbacks*, VkPipeline*);
VkResult consumer_create_graphics(ConsumerBackend*, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*,
                                  const VkAllocationCallbacks*, VkPipeline*);
void consumer_destroy_pipeline(ConsumerBackend*, VkPipeline, const VkAllocationCallbacks*);
VkResult consumer_bind_sets(ConsumerBackend*, ConsumerBindingState&, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*);
// Returns the native pipeline to bind for the current immutable bound-set
// context. No process-global current binding and no application resource slot.
VkResult consumer_pipeline(ConsumerBackend*, ConsumerBindingState&, VkPipeline logical_pipeline, VkPipeline* native_pipeline);
}  // namespace vel::omm
