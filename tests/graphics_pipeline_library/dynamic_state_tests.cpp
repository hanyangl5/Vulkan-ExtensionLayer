// Copyright 2026 Xiaomi. SPDX-License-Identifier: Apache-2.0
// Dynamic-state routing and cache serialization without a Vulkan device.
#include "test_helpers.h"

int main() {
    // Independent feature-membership list generated from the SDK registry, including aliases.
    const VkDynamicState advertised_states[] = {
#include "extended_dynamic_state_cases.inc"
    };
    for (auto state : advertised_states)
        Require(ClassifyDynamicState(state) != GplSubset::kUnknown, "An advertised dynamic state is unclassified");
    Require(ClassifyDynamicState(VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR) == GplSubset::kUnknown,
            "Ray tracing state must not be treated as graphics pipeline state");
    Require(ClassifyDynamicState(VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT) == GplSubset::kFragmentShaderAndOutput,
            "Multisampling must belong to both applicable fragment libraries");
    Require(ClassifyDynamicState(VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR) == GplSubset::kPreRasterAndFragmentShader,
            "Fragment shading rate must belong to both shader libraries");

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.renderPass = CastFromUint64<VkRenderPass>(0x1000);
    VkDynamicState states[] = {VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT, VK_DYNAMIC_STATE_SAMPLE_MASK_EXT,
                               VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT, VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 4, states};
    ci.pDynamicState = &dyn;
    auto traditional = Context(ci);
    Require(traditional.fragment_shader_dynamic.size() == 4 && traditional.fragment_output_dynamic.size() == 4,
            "Shared multisample states were not copied to both traditional-render-pass libraries");
    ci.renderPass = VK_NULL_HANDLE;
    auto rendering = Context(ci);
    Require(rendering.fragment_shader_dynamic.size() == 4 && rendering.fragment_output_dynamic.size() == 4,
            "Both fragment libraries must preserve multisample state for implicit per-sample shader execution");

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_64_BIT;
    ms.pSampleMask = reinterpret_cast<const VkSampleMask*>(uintptr_t{1});
    ci.pMultisampleState = &ms;
    auto dynamic_mask_key = Key(ci, GplSubset::kFragmentOutput);  // must not dereference the poison pointer
    ms.pSampleMask = reinterpret_cast<const VkSampleMask*>(uintptr_t{2});
    Require(dynamic_mask_key == Key(ci, GplSubset::kFragmentOutput), "An ignored sample mask affected the key");
    VkSampleMask masks[] = {0xffffffffu, 0x12345678u};
    ms.pSampleMask = masks;
    dyn.dynamicStateCount = 1;  // Dynamic sample count, static mask: the static count still sizes the mask.
    auto static_mask_key = Key(ci, GplSubset::kFragmentOutput);
    masks[1] ^= 1;
    Require(static_mask_key != Key(ci, GplSubset::kFragmentOutput), "A static mask's second word was lost");

    VkDynamicState blend_states[] = {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                                     VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
    ci.renderPass = CastFromUint64<VkRenderPass>(0x1000);
    dyn.dynamicStateCount = 3;
    dyn.pDynamicStates = blend_states;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 65535;
    cb.pAttachments = reinterpret_cast<const VkPipelineColorBlendAttachmentState*>(uintptr_t{1});
    ci.pColorBlendState = &cb;
    ci.pMultisampleState = nullptr;
    auto blend_key = Key(ci, GplSubset::kFragmentOutput);
    cb.pAttachments = nullptr;
    Require(blend_key == Key(ci, GplSubset::kFragmentOutput), "An ignored color attachment array affected the key");

    VkDynamicState viewport_states[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = viewport_states;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 65535;
    vp.pViewports = reinterpret_cast<const VkViewport*>(uintptr_t{1});
    vp.pScissors = reinterpret_cast<const VkRect2D*>(uintptr_t{1});
    ci.pViewportState = &vp;
    auto viewport_key = Key(ci, GplSubset::kPreRaster);
    vp.pViewports = nullptr;
    vp.pScissors = nullptr;
    Require(viewport_key == Key(ci, GplSubset::kPreRaster), "Ignored viewport/scissor arrays affected the key");

    VkDynamicState vertex_state = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    dyn.dynamicStateCount = 1;
    dyn.pDynamicStates = &vertex_state;
    ci.pVertexInputState = reinterpret_cast<const VkPipelineVertexInputStateCreateInfo*>(uintptr_t{1});
    Key(ci, GplSubset::kVertexInput);  // The entire static struct is ignored.
    ci.pVertexInputState = nullptr;
    ci.pViewportState = nullptr;
    ci.pColorBlendState = nullptr;
    ci.pDynamicState = nullptr;

    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.renderPass = VK_NULL_HANDLE;
    VkPipelineRenderingCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr, 1, 1, &format, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    ci.pNext = &info;
    info.colorAttachmentCount = 0;
    cb.attachmentCount = 1;
    cb.pAttachments = reinterpret_cast<const VkPipelineColorBlendAttachmentState*>(uintptr_t{1});
    ci.pColorBlendState = &cb;
    auto no_color_key = Key(ci, GplSubset::kFragmentOutput);
    ci.pColorBlendState = nullptr;
    Require(no_color_key == Key(ci, GplSubset::kFragmentOutput) && FragmentOutputColorBlendState(ci) == nullptr,
            "Depth/stencil-only dynamic rendering must ignore color blend state and its attachment array");
    info.colorAttachmentCount = 1;
    Require(ConvertBlocker(ci) == nullptr, "Base dynamic rendering must be eligible for conversion");
    auto pre = Key(ci, GplSubset::kPreRaster), fs = Key(ci, GplSubset::kFragmentShader), fo = Key(ci, GplSubset::kFragmentOutput);
    format = VK_FORMAT_R16G16B16A16_SFLOAT;
    Require(pre == Key(ci, GplSubset::kPreRaster) && fs == Key(ci, GplSubset::kFragmentShader),
            "Attachment formats must not prevent sharing the dynamic-rendering shader libraries");
    Require(fo != Key(ci, GplSubset::kFragmentOutput), "Attachment formats must distinguish output libraries");
    fo = Key(ci, GplSubset::kFragmentOutput);
    info.viewMask = 3;
    Require(pre != Key(ci, GplSubset::kPreRaster) && fs != Key(ci, GplSubset::kFragmentShader),
            "View masks must distinguish both shader libraries");
    Require(fo == Key(ci, GplSubset::kFragmentOutput), "View mask must not prevent output-library sharing");
    auto chain = Context(ci);
    Require(chain.pre_raster_extensions.head != chain.fragment_shader_extensions.head &&
                chain.link_extensions.head != chain.pre_raster_extensions.head && info.pNext == nullptr,
            "Library chains must have independent owned headers without modifying the application chain");
    DeviceData affected;
    affected.multiview_instance_input_workaround = true;
    VkVertexInputBindingDescription binding{0, 16, VK_VERTEX_INPUT_RATE_INSTANCE};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    ci.pVertexInputState = &vi;
    Require(ConvertBlocker(ci, &affected) != nullptr, "Affected multiview instance input must use the native path");
    info.viewMask = 0;
    Require(ConvertBlocker(ci, &affected) == nullptr, "Ordinary instance input must remain eligible");
    info.viewMask = 3;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    Require(ConvertBlocker(ci, &affected) == nullptr, "Multiview vertex-rate input must remain eligible");
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    affected.multiview_instance_input_workaround = false;
    Require(ConvertBlocker(ci, &affected) == nullptr, "Unlisted devices/drivers must not inherit the workaround");
    affected.multiview_instance_input_workaround = true;
    ci.renderPass = CastFromUint64<VkRenderPass>(0x1000);
    affected.SetRenderPassViewMasks(ci.renderPass, {0, 3});
    Require(ConvertBlocker(ci, &affected) == nullptr, "A single-view traditional subpass must remain eligible");
    ci.subpass = 1;
    Require(ConvertBlocker(ci, &affected) != nullptr, "Traditional render-pass multiview must use the same workaround");
    affected.ForgetRenderPass(ci.renderPass);
    Require(affected.RenderPassViewMask(ci.renderPass, 1) == 0, "Destroyed render-pass view masks must not survive handle reuse");
    std::printf(
        "Dynamic state membership, shared ownership, ignored arrays, and rendering cache keys passed (%zu registry entries).\n",
        sizeof(advertised_states) / sizeof(advertised_states[0]));
    return 0;
}
