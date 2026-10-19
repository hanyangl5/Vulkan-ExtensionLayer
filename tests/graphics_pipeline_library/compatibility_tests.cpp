// Copyright 2026 Xiaomi. SPDX-License-Identifier: Apache-2.0
// API-level failure semantics and cache identity, independent of any Vulkan driver.
#include "test_helpers.h"

template <typename T>
static T Handle(uintptr_t value) {
    return CastFromUint64<T>(value);
}

static const VkBaseInStructure* Node(const void* chain, VkStructureType type) {
    for (auto p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext)
        if (p->sType == type) return p;
    return nullptr;
}

struct ObservedCall {
    uint32_t count, subset, linked_libraries;
    VkPipelineCreateFlags2 flags;
    bool flags2;
};
static std::vector<ObservedCall> calls;
static std::vector<VkResult> native_results;
static size_t native_index;
static bool fail_library_once;
static uintptr_t next_pipeline = 0x10000;

static VKAPI_ATTR VkResult VKAPI_CALL NextCreate(VkDevice, VkPipelineCache, uint32_t count, const VkGraphicsPipelineCreateInfo* ci,
                                                 const VkAllocationCallbacks*, VkPipeline* output) {
    auto gpl = reinterpret_cast<const VkGraphicsPipelineLibraryCreateInfoEXT*>(
        Node(ci->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT));
    auto link = reinterpret_cast<const VkPipelineLibraryCreateInfoKHR*>(
        Node(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR));
    calls.push_back(
        {count, gpl ? gpl->flags : 0, link ? link->libraryCount : 0, EffectivePipelineFlags(*ci), PipelineFlags2(*ci) != nullptr});
    if (gpl || link)
        Require(!Node(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO),
                "Internal calls must not overwrite application feedback");
    if (gpl && gpl->flags == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT && !ci->renderPass)
        Require(ci->pDepthStencilState,
                "A standalone dynamic-rendering FS library must provide disabled DS state when the monolithic pipeline omitted it");
    VkResult result = VK_SUCCESS;
    if (gpl && fail_library_once) {
        fail_library_once = false;
        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (!gpl && !link) {
        if (native_index < native_results.size()) result = native_results[native_index];
        ++native_index;
        if (auto info = reinterpret_cast<const VkPipelineCreationFeedbackCreateInfo*>(
                Node(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO))) {
            *info->pPipelineCreationFeedback = {
                VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT | VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT, 123};
            for (uint32_t i = 0; i < info->pipelineStageCreationFeedbackCount; ++i)
                info->pPipelineStageCreationFeedbacks[i] = {0, 0};
        }
    }
    for (uint32_t i = 0; i < count; ++i) output[i] = result == VK_SUCCESS ? Handle<VkPipeline>(next_pipeline++) : VK_NULL_HANDLE;
    return result;
}

struct Fixture {
    void* dispatch = this;
    DeviceData device;
    VkDevice handle = reinterpret_cast<VkDevice>(&dispatch);
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr, 0, 1, &format};
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                                  VK_SHADER_STAGE_VERTEX_BIT, Handle<VkShaderModule>(0x2000), "main"},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                                  VK_SHADER_STAGE_FRAGMENT_BIT, Handle<VkShaderModule>(0x3000), "main"}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport{0, 0, 16, 16, 0, 1};
    VkRect2D scissor{{0, 0}, {16, 16}};
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &viewport, 1, &scissor};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachment{};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    Fixture() {
        calls.clear();
        native_results.clear();
        native_index = 0;
        fail_library_once = false;
        device.device = handle;
        device.tier = Tier::kPureGpl;
        device.caps.gpl_independent_interp = true;
        device.vtable.CreateGraphicsPipelines = NextCreate;
        device_data_map.insert_or_assign(dispatch, &device);
        rs.lineWidth = 1;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        attachment.colorWriteMask = 15;
        cb.attachmentCount = 1;
        cb.pAttachments = &attachment;
        ci.pNext = &rendering;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cb;
        ci.layout = Handle<VkPipelineLayout>(0x4000);
        ci.basePipelineIndex = -1;
    }
    ~Fixture() { device_data_map.erase(dispatch); }
    VkResult Create(uint32_t count, const VkGraphicsPipelineCreateInfo* infos, VkPipeline* output) {
        return CreateGraphicsPipelines(handle, VK_NULL_HANDLE, count, infos, nullptr, output);
    }
};

static void FlagsAndExtensions() {
    Fixture f;
    auto legacy_key = Key(f.ci, GplSubset::kPreRaster);
    VkPipelineCreateFlags2CreateInfo flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO, f.ci.pNext, 0};
    f.ci.pNext = &flags;
    f.ci.flags = ~0u;
    Require(!ConvertBlocker(f.ci) && legacy_key == Key(f.ci, GplSubset::kPreRaster),
            "Flags2 must replace legacy flags without fragmenting equivalent cache keys");
    flags.flags = uint64_t{1} << 63;
    Require(ConvertBlocker(f.ci), "Unknown high flags2 bits must remain native");
    flags.flags = VK_PIPELINE_CREATE_2_ENABLE_LEGACY_DITHERING_BIT_EXT;
    VkPipeline pipeline;
    Require(f.Create(1, &f.ci, &pipeline) == VK_SUCCESS, "Known 64-bit output flag should convert");
    for (const auto& c : calls) {
        Require(c.flags2, "Flags2 carrier was lost on an internal call");
        if (c.subset)
            Require(bool(c.flags & flags.flags) == bool(c.subset & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT),
                    "High output flag reached the wrong subset");
        else
            Require(c.flags == flags.flags, "Final link lost the high flags2 bit");
    }

    uint32_t locations[2] = {1, 0}, indices[2] = {4, 5}, depth = 6;
    VkRenderingAttachmentLocationInfo loc{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO, f.ci.pNext, 2, locations};
    VkRenderingInputAttachmentIndexInfo idx{
        VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO, &loc, 2, indices, &depth, nullptr};
    f.ci.flags = 0;
    flags.flags = 0;
    f.ci.pNext = &idx;
    auto fs = Key(f.ci, GplSubset::kFragmentShader), fo = Key(f.ci, GplSubset::kFragmentOutput);
    locations[0] = 0;
    Require(fs == Key(f.ci, GplSubset::kFragmentShader) && fo != Key(f.ci, GplSubset::kFragmentOutput),
            "Location mapping must affect FO only");
    locations[0] = 0;
    locations[1] = 1;
    auto identity_key = Key(f.ci, GplSubset::kFragmentOutput);
    loc.pColorAttachmentLocations = nullptr;
    Require(identity_key == Key(f.ci, GplSubset::kFragmentOutput), "NULL attachment locations must be a legal identity map");
    auto location_chain = Context(f.ci);
    auto copied_locations = reinterpret_cast<const VkRenderingAttachmentLocationInfo*>(
        Node(location_chain.fragment_output_extensions.head, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO));
    Require(copied_locations && copied_locations->pColorAttachmentLocations &&
                copied_locations->pColorAttachmentLocations[1] == 1 && !loc.pColorAttachmentLocations,
            "Identity-map materialization must be owned and leave the original pointer unchanged");
    fo = Key(f.ci, GplSubset::kFragmentOutput);
    indices[0] = 3;
    Require(fs != Key(f.ci, GplSubset::kFragmentShader) && fo == Key(f.ci, GplSubset::kFragmentOutput),
            "Input indices must affect FS only");
    f.ci.renderPass = Handle<VkRenderPass>(0x5000);
    idx.pColorAttachmentInputIndices = reinterpret_cast<const uint32_t*>(uintptr_t{1});
    idx.pDepthInputAttachmentIndex = reinterpret_cast<const uint32_t*>(uintptr_t{1});
    loc.pColorAttachmentLocations = reinterpret_cast<const uint32_t*>(uintptr_t{1});
    Key(f.ci, GplSubset::kFragmentShader);
    Key(f.ci, GplSubset::kFragmentOutput);

    f.ci.renderPass = VK_NULL_HANDLE;
    f.ci.pNext = &f.rendering;
    VkPipelineRobustnessCreateInfo robustness{VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO, f.ci.pNext};
    f.ci.pNext = &robustness;
    auto vi = Key(f.ci, GplSubset::kVertexInput);
    fs = Key(f.ci, GplSubset::kFragmentShader);
    robustness.vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED;
    Require(vi != Key(f.ci, GplSubset::kVertexInput) && fs != Key(f.ci, GplSubset::kFragmentShader),
            "Pipeline robustness must distinguish applicable libraries");
    auto chains = Context(f.ci);
    Require(chains.vertex_input_extensions.head != chains.fragment_shader_extensions.head && robustness.pNext == &f.rendering,
            "Robustness chains must be independently owned without rewriting the application");
    VkPipelineRobustnessCreateInfo stage_robustness{VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO};
    f.stages[1].pNext = &stage_robustness;
    auto pre = Key(f.ci, GplSubset::kPreRaster);
    fs = Key(f.ci, GplSubset::kFragmentShader);
    stage_robustness.storageBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED;
    Require(fs != Key(f.ci, GplSubset::kFragmentShader) && pre == Key(f.ci, GplSubset::kPreRaster),
            "A fragment-stage robustness override must distinguish FS without affecting PR");
    f.stages[1].pNext = nullptr;

    VkSampleCountFlagBits samples[2] = {VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_4_BIT};
    VkAttachmentSampleCountInfoAMD sample_info{VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD, f.ci.pNext, 2, samples,
                                               VK_SAMPLE_COUNT_4_BIT};
    f.ci.pNext = &sample_info;
    fo = Key(f.ci, GplSubset::kFragmentOutput);
    pre = Key(f.ci, GplSubset::kPreRaster);
    samples[1] = VK_SAMPLE_COUNT_2_BIT;
    Require(fo != Key(f.ci, GplSubset::kFragmentOutput) && pre == Key(f.ci, GplSubset::kPreRaster),
            "Mixed attachment sample-count contents must distinguish FO without affecting PR");

    f.ci.pNext = &f.rendering;
    f.ci.flags = VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
                 VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT | VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT;
    Require(LibraryFlagsForSubset(f.ci, GplSubset::kVertexInput) == VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
            "VI flag ownership wrong");
    Require(LibraryFlagsForSubset(f.ci, GplSubset::kPreRaster) ==
                (VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT | VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT),
            "PR flag ownership wrong");
    Require(
        LibraryFlagsForSubset(f.ci, GplSubset::kFragmentShader) ==
            (VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT | VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
             VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT),
        "FS flag ownership wrong");
    Require(LibraryFlagsForSubset(f.ci, GplSubset::kFragmentOutput) ==
                (VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT | VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT),
            "FO flag ownership wrong");
}

static void InlineShadersAndDiscard() {
    Fixture f;
    uint32_t code[] = {0x07230203, 0x10000, 0, 10, 0};  // serialization fixture, not submitted to a GPU
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(code), code};
    f.stages[0].module = VK_NULL_HANDLE;
    f.stages[0].pNext = &module;
    Require(!ConvertBlocker(f.ci), "Inline SPIR-V should be eligible");
    auto key = Key(f.ci, GplSubset::kPreRaster);
    code[3]++;
    Require(key != Key(f.ci, GplSubset::kPreRaster), "Different inline SPIR-V must not share a library");
    uint32_t copy[5];
    std::memcpy(copy, code, sizeof(code));
    key = Key(f.ci, GplSubset::kPreRaster);
    module.pCode = copy;
    Require(key == Key(f.ci, GplSubset::kPreRaster), "Inline-code addresses must not fragment the cache");
    auto ctx = Context(f.ci);
    auto layout_key = MakePreRasterKey(ctx, f.device);
    auto fragment_key = MakeFragmentShaderKey(ctx, f.device);
    f.device.ForgetPipelineLayout(f.ci.layout);
    Require(layout_key != MakePreRasterKey(ctx, f.device) && fragment_key != MakeFragmentShaderKey(ctx, f.device),
            "Recycled pipeline-layout handles must not reuse libraries from the old layout");
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, &module, 16};
    f.stages[0].pNext = &subgroup;
    key = Key(f.ci, GplSubset::kPreRaster);
    subgroup.requiredSubgroupSize = 32;
    Require(key != Key(f.ci, GplSubset::kPreRaster), "Required subgroup size must distinguish shader libraries");

    f.rs.rasterizerDiscardEnable = VK_TRUE;
    f.ci.pViewportState = reinterpret_cast<const VkPipelineViewportStateCreateInfo*>(uintptr_t{1});
    f.ci.pDepthStencilState = reinterpret_cast<const VkPipelineDepthStencilStateCreateInfo*>(uintptr_t{1});
    f.ci.pMultisampleState = reinterpret_cast<const VkPipelineMultisampleStateCreateInfo*>(uintptr_t{1});
    f.ci.pColorBlendState = reinterpret_cast<const VkPipelineColorBlendStateCreateInfo*>(uintptr_t{1});
    f.stages[1].pNext = reinterpret_cast<const void*>(uintptr_t{1});
    VkRenderingAttachmentLocationInfo ignored_locations{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO, f.ci.pNext, 4,
                                                        nullptr};
    f.ci.pNext = &ignored_locations;
    auto ignored_chain = Context(f.ci);
    Require(ignored_chain.fragment_output_extensions.identity_maps.empty(),
            "Static discard must not materialize ignored output arrays");
    Require(!ConvertBlocker(f.ci), "Discard must not inspect ignored fragment or viewport state");
    VkPipeline out;
    Require(f.Create(1, &f.ci, &out) == VK_SUCCESS, "Static discard conversion failed");
    Require(calls.size() == 3 && calls.back().linked_libraries == 2,
            "Primitive discard must create VI + PR and link exactly two libraries");
    for (const auto& c : calls)
        Require(!(c.subset & (VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT |
                              VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT)),
                "Discard created a fragment subset");
    calls.clear();
    f.stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    f.ci.pVertexInputState = reinterpret_cast<const VkPipelineVertexInputStateCreateInfo*>(uintptr_t{1});
    f.ci.pInputAssemblyState = reinterpret_cast<const VkPipelineInputAssemblyStateCreateInfo*>(uintptr_t{1});
    Require(!ConvertBlocker(f.ci), "Mesh shading must not inspect ignored vertex-input or assembly structs");
    Require(f.Create(1, &f.ci, &out) == VK_SUCCESS && calls.back().linked_libraries == 1, "Mesh discard must link PR alone");
}

static void BatchesAndFeedback() {
    Fixture f;
    VkGraphicsPipelineCreateInfo infos[3] = {f.ci, f.ci, f.ci};
    VkPipeline output[3];
    Require(f.Create(3, infos, output) == VK_SUCCESS && calls.size() == 7 && native_index == 0,
            "Independent batch must reuse four libraries and link three pipelines");
    VkPipelineCreationFeedback feedback{~0u, 0}, stages[2] = {{~0u, 1}, {~0u, 1}};
    VkPipelineCreationFeedbackCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, f.ci.pNext, &feedback, 2,
                                              stages};
    f.ci.pNext = &info;
    calls.clear();
    Require(f.Create(1, &f.ci, output) == VK_SUCCESS && calls.size() == 1,
            "Feedback addresses must not distinguish cached libraries");
    Require(feedback.flags == VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT && stages[0].flags == 0 && stages[1].flags == 0,
            "Conversion feedback must clear unsupported stage feedback and not fake a driver-cache hit");

    VkBaseInStructure unknown{VK_STRUCTURE_TYPE_PIPELINE_COMPILER_CONTROL_CREATE_INFO_AMD, nullptr};
    for (auto& ci : infos) ci.pNext = &unknown;
    infos[1].flags = VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT;
    calls.clear();
    native_index = 0;
    native_results = {VK_SUCCESS, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_SUCCESS};
    std::fill(output, output + 3, Handle<VkPipeline>(0xeeee));
    Require(
        f.Create(3, infos, output) == VK_ERROR_OUT_OF_DEVICE_MEMORY && native_index == 2 && output[0] && !output[1] && !output[2],
        "EARLY_RETURN must preserve earlier success and null skipped outputs");
    infos[1].flags = 0;
    calls.clear();
    native_index = 0;
    Require(f.Create(3, infos, output) == VK_ERROR_OUT_OF_DEVICE_MEMORY && native_index == 3 && output[2],
            "A failed ordinary entry must not suppress later independent entries");

    infos[0] = f.ci;
    infos[1].flags = VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT;
    calls.clear();
    native_index = 0;
    native_results = {VK_ERROR_OUT_OF_DEVICE_MEMORY};
    Require(f.Create(3, infos, output) == VK_ERROR_OUT_OF_DEVICE_MEMORY && output[0] && !output[1] && !output[2],
            "Mixed converted/native batches must preserve early-return behavior");

    infos[1].flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    infos[1].basePipelineIndex = 0;
    calls.clear();
    native_index = 0;
    native_results = {VK_SUCCESS};
    Require(f.Create(3, infos, output) == VK_SUCCESS && calls.size() == 1 && calls[0].count == 3,
            "Derivative dependencies must keep the entire native batch");
    infos[1].flags = 0;
    VkPipelineCreateFlags2CreateInfo flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO, nullptr,
                                           VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT};
    infos[2].pNext = &flags;
    calls.clear();
    native_index = 0;
    Require(f.Create(3, infos, output) == VK_SUCCESS && calls.size() == 1 && calls[0].count == 3,
            "Compile-required in flags2 must keep native batch semantics");
}

static void NativeFeedbackFallback() {
    Fixture f;
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedbackCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, f.ci.pNext, &feedback};
    f.ci.pNext = &info;
    fail_library_once = true;
    VkPipeline output;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && native_index == 1 && feedback.duration == 123 &&
                (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT),
            "Native fallback must retain the driver's original feedback");
}

static void ColorlessRenderPass() {
    Fixture f;
    f.ci.renderPass = Handle<VkRenderPass>(0x5000);
    f.device.SetRenderPassColorAttachments(f.ci.renderPass, {false, true});
    f.cb.attachmentCount = 32;
    f.cb.pAttachments = reinterpret_cast<const VkPipelineColorBlendAttachmentState*>(uintptr_t{1});
    auto ctx = Context(f.ci);
    MakeFragmentOutputKey(ctx, f.device);
    Require(!FragmentOutputColorBlendState(f.ci, &f.device), "A colorless subpass must ignore the color-blend attachment array");
    f.ci.subpass = 1;
    Require(FragmentOutputColorBlendState(f.ci, &f.device) == &f.cb, "Color subpasses must keep color-blend state");
    f.device.ForgetRenderPass(f.ci.renderPass);
    f.ci.subpass = 0;
    Require(FragmentOutputColorBlendState(f.ci, &f.device) == &f.cb,
            "Render-pass color metadata must not survive destroyed-handle reuse");
}

namespace private_data_regression {

struct Observed {
    VkResult write_result = VK_SUCCESS;
    VkPipeline attempted_pipeline = VK_NULL_HANDLE;
    uint64_t record = 0;
    uint32_t writes = 0;
    std::vector<VkPipeline> destroyed;
    const VkAllocationCallbacks* destroy_allocator = nullptr;
    StateMask restored = 0;
    OverrideVals values;
} observed;

static VKAPI_ATTR VkResult VKAPI_CALL Set(VkDevice, VkObjectType, uint64_t pipeline, VkPrivateDataSlot, uint64_t record) {
    observed.attempted_pipeline = CastFromUint64<VkPipeline>(pipeline);
    ++observed.writes;
    if (observed.write_result == VK_SUCCESS) observed.record = record;
    return observed.write_result;
}
static VKAPI_ATTR void VKAPI_CALL Get(VkDevice, VkObjectType, uint64_t pipeline, VkPrivateDataSlot, uint64_t* record) {
    *record = pipeline == CastToUint64(observed.attempted_pipeline) ? observed.record : 0;
}
static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    observed.destroyed.push_back(pipeline);
    observed.destroy_allocator = allocator;
    if (pipeline == observed.attempted_pipeline) observed.record = 0;
}
static VKAPI_ATTR void VKAPI_CALL Bind(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
static VKAPI_ATTR void VKAPI_CALL Cull(VkCommandBuffer, VkCullModeFlags value) {
    observed.restored |= kCullMode;
    observed.values.cullMode = value;
}
static VKAPI_ATTR void VKAPI_CALL Front(VkCommandBuffer, VkFrontFace value) {
    observed.restored |= kFrontFace;
    observed.values.frontFace = value;
}
static VKAPI_ATTR void VKAPI_CALL DepthTest(VkCommandBuffer, VkBool32 value) {
    observed.restored |= kDepthTestEnable;
    observed.values.depthTestEnable = value;
}
static VKAPI_ATTR void VKAPI_CALL DepthWrite(VkCommandBuffer, VkBool32 value) {
    observed.restored |= kDepthWriteEnable;
    observed.values.depthWriteEnable = value;
}
static VKAPI_ATTR void VKAPI_CALL DepthCompare(VkCommandBuffer, VkCompareOp value) {
    observed.restored |= kDepthCompareOp;
    observed.values.depthCompareOp = value;
}
static void Initialize(Fixture& f) {
    observed = {};
    f.device.tier = Tier::kDynamize;
    f.device.caps.extended_dynamic_state = true;
    f.device.record_slot = Handle<VkPrivateDataSlot>(0x1234);
    f.device.vtable.SetPrivateData = Set;
    f.device.vtable.GetPrivateData = Get;
    f.device.vtable.DestroyPipeline = Destroy;
    f.device.vtable.CmdBindPipeline = Bind;
    f.device.vtable.CmdSetCullMode = Cull;
    f.device.vtable.CmdSetFrontFace = Front;
    f.device.vtable.CmdSetDepthTestEnable = DepthTest;
    f.device.vtable.CmdSetDepthWriteEnable = DepthWrite;
    f.device.vtable.CmdSetDepthCompareOp = DepthCompare;
}

static void Run() {
    for (auto native_result : {VK_SUCCESS, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        Fixture f;
        Initialize(f);
        observed.write_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        native_results = {native_result};
        VkAllocationCallbacks allocator{};
        VkPipeline output;
        Require(CreateGraphicsPipelines(f.handle, VK_NULL_HANDLE, 1, &f.ci, &allocator, &output) == native_result &&
                    native_index == 1 && observed.writes == 1,
                "A private-data write failure must fall back and return the native result");
        Require(observed.destroyed.size() == 1 && observed.destroyed[0] == observed.attempted_pipeline &&
                    observed.destroy_allocator == &allocator && output != observed.attempted_pipeline &&
                    f.device.converted_pipeline_count == 0,
                "A failed record attachment must destroy its executable with the original allocator and not count a conversion");
        Require(bool(output) == (native_result == VK_SUCCESS), "Native fallback must preserve output-handle semantics");
        if (output) {
            CmdBindPipeline(reinterpret_cast<VkCommandBuffer>(&f.dispatch), VK_PIPELINE_BIND_POINT_GRAPHICS, output);
            Require(observed.restored == 0, "A native fallback pipeline must not restore layer-promoted state");
            DestroyPipeline(f.handle, output, &allocator);
        }
    }

    Fixture f;
    Initialize(f);
    f.rs.cullMode = VK_CULL_MODE_BACK_BIT;
    f.rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER;
    f.ci.pDepthStencilState = &depth;
    VkPipeline output;
    Require(
        f.Create(1, &f.ci, &output) == VK_SUCCESS && native_index == 0 && observed.record && f.device.converted_pipeline_count == 1,
        "Successful record attachment must retain GPL conversion");
    CmdBindPipeline(reinterpret_cast<VkCommandBuffer>(&f.dispatch), VK_PIPELINE_BIND_POINT_GRAPHICS, output);
    Require(observed.restored == kPromotable && observed.values.cullMode == f.rs.cullMode &&
                observed.values.frontFace == f.rs.frontFace && observed.values.depthTestEnable == depth.depthTestEnable &&
                observed.values.depthWriteEnable == depth.depthWriteEnable &&
                observed.values.depthCompareOp == depth.depthCompareOp,
            "Binding a converted pipeline must restore all five original static values");
    DestroyPipeline(f.handle, output, nullptr);
    Require(observed.destroyed.size() == 1 && !observed.record, "Destroying a converted pipeline must release its record");
}

}  // namespace private_data_regression

namespace device_retry_regression {

struct State {
    VkLayerDeviceLink* lower_link = nullptr;
    const VkDeviceCreateInfo* original = nullptr;
    VkDevice loader_device = VK_NULL_HANDLE;
    VkDevice created_device = VK_NULL_HANDLE;
    VkResult retry_result = VK_SUCCESS;
    uint32_t attempts = 0;
    uint32_t destroys = 0;
};
static State* state;

static VKAPI_ATTR VkResult VKAPI_CALL Create(VkPhysicalDevice, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks*,
                                             VkDevice* device) {
    ++state->attempts;
    auto* chain = GetChainInfo(ci, VK_LAYER_LINK_INFO);
    Require(chain->u.pLayerInfo == state->lower_link, "Each device creation attempt must restore the lower layer link");
    Require(*device == state->loader_device, "Each device creation attempt must preserve the loader's output-slot contents");
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    if (state->attempts == 1) {
        Require(ci != state->original && ci->enabledExtensionCount == 2 &&
                    Node(ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT),
                "First device creation must use the augmented request");
        *device = VK_NULL_HANDLE;
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    Require(state->attempts == 2 && ci == state->original && ci->enabledExtensionCount == 0 &&
                !Node(ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT),
            "Retry must use the original feature chain and extension list");
    *device = state->retry_result == VK_SUCCESS ? state->created_device : VK_NULL_HANDLE;
    return state->retry_result;
}
static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice device, const VkAllocationCallbacks*) {
    Require(device == state->created_device, "Device destruction must reach the correct lower device");
    ++state->destroys;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Gipa(VkInstance, const char* name) {
    return std::strcmp(name, "vkCreateDevice") == 0 ? reinterpret_cast<PFN_vkVoidFunction>(Create) : nullptr;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Gdpa(VkDevice, const char* name) {
    return std::strcmp(name, "vkDestroyDevice") == 0 ? reinterpret_cast<PFN_vkVoidFunction>(Destroy) : nullptr;
}
static VKAPI_ATTR VkResult VKAPI_CALL Extensions(VkPhysicalDevice, const char*, uint32_t* count, VkExtensionProperties* props) {
    if (props) {
        std::strcpy(props[0].extensionName, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        std::strcpy(props[1].extensionName, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    }
    *count = 2;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL Features(VkPhysicalDevice, VkPhysicalDeviceFeatures2* features) {
    for (auto* node = reinterpret_cast<VkBaseOutStructure*>(features->pNext); node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT)
            reinterpret_cast<VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT*>(node)->graphicsPipelineLibrary = VK_TRUE;
}

static void Run() {
    for (auto retry_result : {VK_SUCCESS, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        InstanceData instance;
        instance.api_version = VK_API_VERSION_1_3;
        instance.vtable.EnumerateDeviceExtensionProperties = Extensions;
        instance.vtable.GetPhysicalDeviceFeatures2 = Features;
        void* physical_dispatch = &instance;
        instance_data_map.insert_or_assign(physical_dispatch, &instance);
        VkLayerDeviceLink lower{};
        VkLayerDeviceLink current{};
        current.pNext = &lower;
        current.pfnNextGetInstanceProcAddr = Gipa;
        current.pfnNextGetDeviceProcAddr = Gdpa;
        VkLayerDeviceCreateInfo chain{};
        chain.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
        chain.function = VK_LAYER_LINK_INFO;
        chain.u.pLayerInfo = &current;
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.pNext = &chain;
        State probe;
        void* device_dispatch = &probe;
        probe.lower_link = &lower;
        probe.original = &ci;
        probe.loader_device = reinterpret_cast<VkDevice>(&instance);
        probe.created_device = reinterpret_cast<VkDevice>(&device_dispatch);
        probe.retry_result = retry_result;
        state = &probe;
        VkDevice device = probe.loader_device;
        Require(CreateDevice(reinterpret_cast<VkPhysicalDevice>(&physical_dispatch), &ci, nullptr, &device) == retry_result &&
                    probe.attempts == 2,
                "Augmented creation failure must retry exactly once and return the original-path result");
        if (retry_result == VK_SUCCESS) {
            auto* data = GetDeviceData(device);
            Require(device == probe.created_device && data && data->tier == Tier::kPassthrough && !data->ConversionEnabled(),
                    "A successful original-path retry must register an inert device");
            DestroyDevice(device, nullptr);
            Require(probe.destroys == 1 && !device_data_map.contains(device_dispatch),
                    "Retried devices must still destroy and unregister correctly");
        } else {
            Require(!device_data_map.contains(device_dispatch), "Failed retries must not register a device");
        }
        instance_data_map.erase(physical_dispatch);
        state = nullptr;
    }
}

}  // namespace device_retry_regression

static void IndependentInterpolationFallback() {
    for (bool traditional : {false, true}) {
        Fixture f;
        f.device.caps.gpl_independent_interp = false;
        if (traditional) f.ci.renderPass = Handle<VkRenderPass>(0x5000);
        VkPipeline output;
        Require(ConvertBlocker(f.ci, &f.device) && f.Create(1, &f.ci, &output) == VK_SUCCESS && native_index == 1 &&
                    calls.size() == 1 && !calls[0].subset && !calls[0].linked_libraries,
                "Without independent interpolation, fragment pipelines must remain native for both rendering paths");
        f.stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
        Require(ConvertBlocker(f.ci, &f.device), "Mesh-to-fragment interfaces require the same interpolation fallback");
    }
    {
        Fixture f;
        f.device.caps.gpl_independent_interp = false;
        f.rs.rasterizerDiscardEnable = VK_TRUE;
        f.stages[1].pNext = reinterpret_cast<const void*>(uintptr_t{1});
        VkPipeline output;
        Require(!ConvertBlocker(f.ci, &f.device) && f.Create(1, &f.ci, &output) == VK_SUCCESS && native_index == 0 &&
                    calls.back().linked_libraries == 2,
                "Static discard must remain convertible without inspecting ignored fragment shaders");
        VkDynamicState discard = VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE;
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 1, &discard};
        f.ci.pDynamicState = &dynamic;
        f.stages[1].pNext = nullptr;
        Require(ConvertBlocker(f.ci, &f.device), "Dynamic discard still needs the interpolation fallback");
    }
    {
        Fixture f;
        f.device.caps.gpl_independent_interp = false;
        f.ci.stageCount = 1;
        Require(!ConvertBlocker(f.ci, &f.device), "Pipelines without a fragment shader have no interpolation interface to check");
    }
    {
        Fixture f;
        VkPipeline output;
        Require(!ConvertBlocker(f.ci, &f.device) && f.Create(1, &f.ci, &output) == VK_SUCCESS && native_index == 0 &&
                    calls.back().linked_libraries == 4,
                "Independent interpolation support must preserve ordinary GPL conversion");
    }
}

namespace dispatch_version_regression {
static std::vector<std::string> requested_names;
static VKAPI_ATTR void VKAPI_CALL Stub() {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Gdpa(VkDevice, const char* name) {
    // Return non-null even for core commands newer than the application's requested version.
    requested_names.emplace_back(name);
    return Stub;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Gipa(VkInstance, const char* name) {
    requested_names.emplace_back(name);
    return Stub;
}
static bool Requested(const char* name) {
    return std::find(requested_names.begin(), requested_names.end(), name) != requested_names.end();
}
static void Run() {
    for (auto version : {VK_API_VERSION_1_0, VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_API_VERSION_1_3}) {
        LayerDispatchDevice device;
        requested_names.clear();
        device.Initialize(VK_NULL_HANDLE, Gdpa, version);
        for (const char* name :
             {"vkCreatePrivateDataSlot", "vkDestroyPrivateDataSlot", "vkSetPrivateData", "vkGetPrivateData", "vkCmdSetCullMode",
              "vkCmdSetFrontFace", "vkCmdSetDepthTestEnable", "vkCmdSetDepthWriteEnable", "vkCmdSetDepthCompareOp"}) {
            auto alias = std::string(name) + "EXT";
            Require(
                Requested(name) == (version >= VK_API_VERSION_1_3) && Requested(alias.c_str()) == (version < VK_API_VERSION_1_3),
                "A command promoted in 1.3 must use the EXT name below 1.3 even when core returns non-null");
        }
        Require(Requested("vkCreateRenderPass2") == (version >= VK_API_VERSION_1_2) &&
                    Requested("vkCreateRenderPass2KHR") == (version < VK_API_VERSION_1_2),
                "RenderPass2 must use the KHR name below 1.2");

        LayerDispatchInstance instance;
        requested_names.clear();
        instance.Initialize(VK_NULL_HANDLE, Gipa, version);
        for (const char* name : {"vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceProperties2"}) {
            auto alias = std::string(name) + "KHR";
            Require(
                Requested(name) == (version >= VK_API_VERSION_1_1) && Requested(alias.c_str()) == (version < VK_API_VERSION_1_1),
                "Instance commands promoted in 1.1 must use KHR names below 1.1");
        }
    }
}
}  // namespace dispatch_version_regression

namespace cache_key_regression {
static void Run() {
    Fixture f;
    VkDynamicState states[] = {VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                               VK_DYNAMIC_STATE_LINE_RASTERIZATION_MODE_EXT, VK_DYNAMIC_STATE_LINE_STIPPLE_EXT};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 4, states};
    f.ci.pDynamicState = &dynamic;
    auto vi = Key(f.ci, GplSubset::kVertexInput);
    f.ia.primitiveRestartEnable = VK_TRUE;
    f.ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    Require(vi == Key(f.ci, GplSubset::kVertexInput), "Dynamic restart and topology within one class must reuse VI");
    f.ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    Require(vi != Key(f.ci, GplSubset::kVertexInput), "Different topology classes must remain distinct for restricted devices");
    f.ci.pDynamicState = nullptr;
    vi = Key(f.ci, GplSubset::kVertexInput);
    f.ia.primitiveRestartEnable = VK_FALSE;
    Require(vi != Key(f.ci, GplSubset::kVertexInput), "Static restart values must still distinguish VI");

    VkPipelineRasterizationLineStateCreateInfo line{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO};
    line.stippledLineEnable = VK_TRUE;
    line.lineStippleFactor = 1;
    f.rs.pNext = &line;
    auto pr = Key(f.ci, GplSubset::kPreRaster);
    const size_t padding =
        offsetof(VkPipelineRasterizationLineStateCreateInfo, lineStipplePattern) + sizeof(line.lineStipplePattern);
    std::memset(reinterpret_cast<unsigned char*>(&line) + padding, 0x7f, sizeof(line) - padding);
    Require(pr == Key(f.ci, GplSubset::kPreRaster), "Changing only structure padding must not split the PR cache");
    line.lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_BRESENHAM;
    Require(pr != Key(f.ci, GplSubset::kPreRaster), "Static line mode must distinguish PR");

    f.ci.pDynamicState = &dynamic;
    VkPipeline output;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS, "Initial dynamic line pipeline must convert");
    const auto first_calls = calls.size();
    const auto expensive = f.device.expensive_library_count.load();
    line.lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR;
    line.lineStippleFactor = 8;
    line.lineStipplePattern = 0x1234;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == first_calls + 1 && calls.back().linked_libraries == 4 &&
                f.device.expensive_library_count == expensive,
            "Changing ignored dynamic line values must only link, without building any new libraries");

    VkPipelineRasterizationConservativeStateCreateInfoEXT conservative{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT};
    f.rs.pNext = &conservative;
    VkDynamicState conservative_states[] = {VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT,
                                            VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT};
    dynamic.pDynamicStates = conservative_states;
    dynamic.dynamicStateCount = 2;
    pr = Key(f.ci, GplSubset::kPreRaster);
    conservative.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    conservative.extraPrimitiveOverestimationSize = 0.5f;
    Require(pr == Key(f.ci, GplSubset::kPreRaster), "Dynamic extension enum and float values must not split PR");
    f.ci.pDynamicState = nullptr;
    pr = Key(f.ci, GplSubset::kPreRaster);
    conservative.extraPrimitiveOverestimationSize = 0.75f;
    Require(pr != Key(f.ci, GplSubset::kPreRaster), "Static extension float values must still distinguish PR");

    f.rs.pNext = nullptr;
    VkPipelineFragmentShadingRateStateCreateInfoKHR rate{
        VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR, f.ci.pNext, {1, 1}};
    f.ci.pNext = &rate;
    VkDynamicState rate_state = VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;
    dynamic.dynamicStateCount = 1;
    dynamic.pDynamicStates = &rate_state;
    f.ci.pDynamicState = &dynamic;
    pr = Key(f.ci, GplSubset::kPreRaster);
    auto fs = Key(f.ci, GplSubset::kFragmentShader);
    rate.fragmentSize = {2, 2};
    rate.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
    Require(pr == Key(f.ci, GplSubset::kPreRaster) && fs == Key(f.ci, GplSubset::kFragmentShader),
            "Dynamic fragment shading rate must reuse both shader libraries");
    f.ci.pDynamicState = nullptr;
    pr = Key(f.ci, GplSubset::kPreRaster);
    fs = Key(f.ci, GplSubset::kFragmentShader);
    rate.fragmentSize = {1, 1};
    Require(pr != Key(f.ci, GplSubset::kPreRaster) && fs != Key(f.ci, GplSubset::kFragmentShader),
            "Static fragment shading rate must distinguish both shader libraries");
}
}  // namespace cache_key_regression

int main() {
    dispatch_version_regression::Run();
    cache_key_regression::Run();
    FlagsAndExtensions();
    InlineShadersAndDiscard();
    BatchesAndFeedback();
    NativeFeedbackFallback();
    ColorlessRenderPass();
    private_data_regression::Run();
    device_retry_regression::Run();
    IndependentInterpolationFallback();
    std::puts("Compatibility, private-data failure, loader retry and independent-interpolation regressions passed.");
}
