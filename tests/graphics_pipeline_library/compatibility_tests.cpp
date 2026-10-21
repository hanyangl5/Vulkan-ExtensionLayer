// Copyright 2026 Xiaomi. SPDX-License-Identifier: Apache-2.0
// API-level failure semantics and cache identity, independent of any Vulkan driver.
#include "test_helpers.h"
#include <thread>

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
static VKAPI_ATTR void VKAPI_CALL IgnoreDestroy(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}

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
    explicit Fixture(size_t library_capacity = LibraryCache::kDefaultCapacity) : device(library_capacity) {
        calls.clear();
        native_results.clear();
        native_index = 0;
        fail_library_once = false;
        device.device = handle;
        device.tier = Tier::kPureGpl;
        device.caps.gpl_independent_interp = true;
        device.vtable.CreateGraphicsPipelines = NextCreate;
        device.vtable.DestroyPipeline = IgnoreDestroy;
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

namespace split_state_regression {
static uint32_t binding;
static float width;
static bool expect_dynamic;
static VkSampleCountFlagBits samples;

static VKAPI_ATTR VkResult VKAPI_CALL Observe(VkDevice device, VkPipelineCache cache, uint32_t count,
                                              const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* allocator,
                                              VkPipeline* output) {
    auto gpl = reinterpret_cast<const VkGraphicsPipelineLibraryCreateInfoEXT*>(
        Node(ci->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT));
    if (gpl && gpl->flags == (VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT |
                              VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT)) {
        Require(ci->pInputAssemblyState && ci->pVertexInputState && ci->pRasterizationState,
                "A combined VI/PR library must own input and raster state together");
        Require(
            ci->pVertexInputState->pVertexBindingDescriptions[0].binding == binding && ci->pRasterizationState->lineWidth == width,
            "Combined libraries must use the input layout and original static line width");
        if (expect_dynamic)
            for (auto state : {VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_LINE_WIDTH})
                Require(HasDynamicState(*ci, state), "A combined library lost one subset's dynamic-state declaration");
    }
    if (gpl && gpl->flags == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT)
        Require(ci->pMultisampleState && ci->pMultisampleState->rasterizationSamples == samples,
                "FS must retain multisample state even with sampleShadingEnable false");
    return NextCreate(device, cache, count, ci, allocator, output);
}

static void CombinedLibraries() {
    Fixture f;
    f.ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    Require(!CombineVertexInputWithPreRaster(f.ci, f.device), "Other drivers should retain separate VI and PR libraries");
    f.device.combine_line_pre_raster = true;
    Require(CombineVertexInputWithPreRaster(f.ci, f.device), "The line-width workaround must combine VI/PR on affected devices");
    f.rs.rasterizerDiscardEnable = VK_TRUE;
    Require(!CombineVertexInputWithPreRaster(f.ci, f.device), "Discarded rasterization does not need the line workaround");
    f.rs.rasterizerDiscardEnable = VK_FALSE;
    f.stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    Require(!CombineVertexInputWithPreRaster(f.ci, f.device), "Mesh pipelines must not acquire vertex-input state");
    f.stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;

    VkVertexInputBindingDescription desc{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    f.vi.vertexBindingDescriptionCount = f.vi.vertexAttributeDescriptionCount = 1;
    f.vi.pVertexBindingDescriptions = &desc;
    f.vi.pVertexAttributeDescriptions = &attr;
    binding = 0;
    width = f.rs.lineWidth = 5.0f;
    samples = f.ms.rasterizationSamples;
    expect_dynamic = false;
    f.device.vtable.CreateGraphicsPipelines = Observe;
    VkPipeline output;
    Require(
        f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == 4 && calls.back().linked_libraries == 3 && native_index == 0,
        "Line pipelines must still convert using one combined library, two fragment libraries and one link");
    auto count = calls.size();
    binding = desc.binding = attr.binding = 1;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == count + 2,
            "A changed vertex layout must rebuild combined VI/PR without rebuilding fragment libraries");
    count = calls.size();
    width = f.rs.lineWidth = 7.0f;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == count + 2,
            "Static line widths must distinguish combined libraries");

    VkDynamicState states[] = {VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, states};
    f.ci.pDynamicState = &dynamic;
    expect_dynamic = true;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS, "A combined library must support dynamic line width and restart");
    count = calls.size();
    f.rs.lineWidth = 0.0f;
    f.ia.primitiveRestartEnable = VK_TRUE;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == count + 1,
            "Ignored dynamic VI/PR fields must reuse the combined library");
    f.ci.flags = VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT;
    Require(LibraryFlagsForSubset(f.ci, GplSubset::kVertexInputAndPreRaster) & VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT,
            "Combined libraries must retain pre-raster flags");
}

static void FragmentState() {
    Fixture f;
    f.ms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    f.ms.sampleShadingEnable = VK_FALSE;
    f.device.vtable.CreateGraphicsPipelines = Observe;
    samples = VK_SAMPLE_COUNT_4_BIT;
    VkPipeline output;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && native_index == 0,
            "Per-sample input-attachment shaders must remain convertible without explicit sample shading");
    auto count = calls.size();
    samples = f.ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == count + 3,
            "Changing sample count must rebuild both fragment libraries while preserving pre-raster state");

    auto ctx = Context(f.ci, false, true);
    const auto* indices = reinterpret_cast<const VkRenderingInputAttachmentIndexInfo*>(
        Node(ctx.fragment_shader_extensions.head, VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO));
    Require(indices && indices->colorAttachmentCount == 1 && indices->pColorAttachmentInputIndices[0] == 0 &&
                !indices->pDepthInputAttachmentIndex && !indices->pStencilInputAttachmentIndex && f.rendering.pNext == nullptr,
            "Implicit local-read color indices must become an owned identity map without changing the application chain");
    ctx = Context(f.ci);
    Require(!Node(ctx.fragment_shader_extensions.head, VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO),
            "Applications without the local-read feature must not receive an input-index structure");
    uint32_t explicit_index = VK_ATTACHMENT_UNUSED;
    VkRenderingInputAttachmentIndexInfo explicit_info{VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO, f.ci.pNext, 1,
                                                      &explicit_index};
    f.ci.pNext = &explicit_info;
    ctx = Context(f.ci, false, true);
    indices = reinterpret_cast<const VkRenderingInputAttachmentIndexInfo*>(
        Node(ctx.fragment_shader_extensions.head, VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO));
    Require(indices && indices->pColorAttachmentInputIndices == &explicit_index,
            "Application-provided input mappings must not be replaced by the identity mapping");
}
}  // namespace split_state_regression

namespace identifier_regression {
static VkPipelineCache expected_cache;
static bool require_compile_control;
static bool fail_identifier_once;

static VKAPI_ATTR VkResult VKAPI_CALL Observe(VkDevice device, VkPipelineCache cache, uint32_t count,
                                              const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* allocator,
                                              VkPipeline* output) {
    Require(cache == expected_cache, "All GPL builds, links and native fallbacks must receive the application pipeline cache");
    if (require_compile_control)
        Require(EffectivePipelineFlags(*ci) & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT,
                "A speculative request must never trigger unguarded compilation in an internal creation call");
    if (Node(ci->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT)) {
        for (uint32_t i = 0; i < ci->stageCount; ++i) {
            const auto* identifier = StageIdentifier(ci->pStages[i]);
            if (identifier && identifier->identifierSize && fail_identifier_once) {
                fail_identifier_once = false;
                *output = VK_NULL_HANDLE;
                return VK_PIPELINE_COMPILE_REQUIRED;
            }
        }
    }
    return NextCreate(device, cache, count, ci, allocator, output);
}

static void Run() {
    Fixture f;
    expected_cache = Handle<VkPipelineCache>(0x9000);
    require_compile_control = false;
    fail_identifier_once = false;
    f.device.vtable.CreateGraphicsPipelines = Observe;
    VkPipeline output;
    auto create = [&] { return CreateGraphicsPipelines(f.handle, expected_cache, 1, &f.ci, nullptr, &output); };
    Require(create() == VK_SUCCESS && native_index == 0, "Module-backed cache priming should use GPL");
    auto ctx = Context(f.ci);
    const auto module_key = MakePreRasterKey(ctx, f.device);
    f.ci.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    ctx = Context(f.ci);
    Require(module_key == MakePreRasterKey(ctx, f.device), "Compile control must not fragment library cache identity");
    require_compile_control = true;
    auto count = calls.size();
    Require(create() == VK_SUCCESS && calls.size() == count + 1 && native_index == 0,
            "A warmed module-backed speculative request should reuse its libraries and link with compile control");

    uint8_t bytes[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
    VkPipelineShaderStageModuleIdentifierCreateInfoEXT ids[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT, nullptr, 4, bytes[0]},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT, nullptr, 4, bytes[1]}};
    for (uint32_t i = 0; i < 2; ++i) {
        f.stages[i].module = VK_NULL_HANDLE;
        f.stages[i].pNext = &ids[i];
    }
    Require(!ConvertBlocker(f.ci, &f.device), "Identifier-only stages must remain eligible for GPL");
    ctx = Context(f.ci);
    const auto identifier_key = MakePreRasterKey(ctx, f.device);
    Require(module_key != identifier_key,
            "Identifier-built libraries must not be reused by module-backed links without required compile control");
    count = calls.size();
    Require(create() == VK_SUCCESS && calls.size() == count + 3 && native_index == 0,
            "Identifier requests must build GPL shader libraries and link instead of switching to monolithic creation");
    uint8_t copy[4];
    std::memcpy(copy, bytes[0], sizeof(copy));
    ids[0].pIdentifier = copy;
    count = calls.size();
    Require(create() == VK_SUCCESS && calls.size() == count + 1, "Identifier addresses must not distinguish libraries");
    ++copy[0];
    ctx = Context(f.ci);
    Require(identifier_key != MakePreRasterKey(ctx, f.device), "Different identifier bytes must distinguish shader libraries");
    fail_identifier_once = true;
    native_results = {VK_PIPELINE_COMPILE_REQUIRED};
    Require(create() == VK_PIPELINE_COMPILE_REQUIRED && !output && native_index == 1,
            "A cold identifier miss must preserve compile-required and a null output through native fallback");
    Require(create() == VK_SUCCESS && output && native_index == 1,
            "A transient compile-required miss must not poison later attempts after the driver cache is primed");

    require_compile_control = false;
    f.ci.flags = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        ids[i].identifierSize = 0;
        ids[i].pIdentifier = reinterpret_cast<const uint8_t*>(uintptr_t{1});
        f.stages[i].module = Handle<VkShaderModule>(i == 0 ? 0x2000 : 0x3000);
    }
    ctx = Context(f.ci);
    Require(module_key == MakePreRasterKey(ctx, f.device), "Zero-size identifiers must be ignored without reading their data");
    count = calls.size();
    Require(create() == VK_SUCCESS && calls.size() == count + 1,
            "Module-backed links must continue reusing their original non-identifier libraries");
}
}  // namespace identifier_regression

namespace content_identity_regression {
static uintptr_t next_object = 0x1000000;
static uintptr_t recycled_object;
static VkResult object_result;

template <typename Info, typename Object>
static VKAPI_ATTR VkResult VKAPI_CALL CreateObject(VkDevice, const Info*, const VkAllocationCallbacks*, Object* output) {
    if (object_result == VK_SUCCESS) {
        *output = Handle<Object>(recycled_object ? recycled_object : next_object++);
        recycled_object = 0;
    }
    return object_result;
}

template <typename Object>
static VKAPI_ATTR void VKAPI_CALL DestroyObject(VkDevice, Object, const VkAllocationCallbacks*) {}

static void Run() {
    Fixture f;
    recycled_object = 0;
    object_result = VK_SUCCESS;
    auto& v = f.device.vtable;
    v.CreateShaderModule = CreateObject<VkShaderModuleCreateInfo, VkShaderModule>;
    v.DestroyShaderModule = DestroyObject<VkShaderModule>;
    v.CreateSampler = CreateObject<VkSamplerCreateInfo, VkSampler>;
    v.DestroySampler = DestroyObject<VkSampler>;
    v.CreateDescriptorSetLayout = CreateObject<VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout>;
    v.DestroyDescriptorSetLayout = DestroyObject<VkDescriptorSetLayout>;
    v.CreatePipelineLayout = CreateObject<VkPipelineLayoutCreateInfo, VkPipelineLayout>;
    v.DestroyPipelineLayout = DestroyObject<VkPipelineLayout>;
    v.CreateRenderPass = CreateObject<VkRenderPassCreateInfo, VkRenderPass>;
    v.CreateRenderPass2 = CreateObject<VkRenderPassCreateInfo2, VkRenderPass>;
    v.DestroyRenderPass = DestroyObject<VkRenderPass>;
    for (const char* name : {"vkCreateShaderModule", "vkCreateSampler", "vkDestroySampler", "vkCreateDescriptorSetLayout",
                             "vkDestroyDescriptorSetLayout", "vkCreatePipelineLayout"})
        Require(FindIntercept(name) && GetDeviceProcAddr(f.handle, name) == FindIntercept(name),
                "Content tracking must be reachable through both instance and device dispatch");

    uint32_t words[] = {0x07230203, 0x10000, 0, 10, 0};
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(words), words};
    VkShaderModule shaders[2];
    for (auto& module : shaders)
        Require(CreateShaderModule(f.handle, &shader, nullptr, &module) == VK_SUCCESS, "Shader creation wrapper failed");
    Require(shaders[0] != shaders[1] && f.device.ShaderModuleIdentity(shaders[0]) == f.device.ShaderModuleIdentity(shaders[1]),
            "Equal SPIR-V in different objects must have equal shader identities");
    f.stages[0].module = shaders[0];
    VkPipeline output;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == 5, "Initial shader libraries were not built");
    f.stages[0].module = shaders[1];
    auto count = calls.size();
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == count + 1,
            "An equal shader object must reuse GPL libraries and only link a new executable");
    const auto original_identity = f.device.ShaderModuleIdentity(shaders[0]);
    DestroyShaderModule(f.handle, shaders[0], nullptr);
    recycled_object = CastToUint64(shaders[0]);
    ++words[3];
    Require(CreateShaderModule(f.handle, &shader, nullptr, &shaders[0]) == VK_SUCCESS &&
                f.device.ShaderModuleIdentity(shaders[0]) != original_identity,
            "A recycled shader handle with changed code must not alias the destroyed shader");
    --words[3];
    f.stages[0].module = VK_NULL_HANDLE;
    f.stages[0].pNext = &shader;
    count = calls.size();
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == count + 1,
            "Inline SPIR-V and an equal module-backed shader must reuse the same libraries");
    f.stages[0].module = shaders[1];
    f.stages[0].pNext = nullptr;
    object_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    Require(CreateShaderModule(f.handle, &shader, nullptr, &shaders[1]) == object_result &&
                f.device.ShaderModuleIdentity(shaders[1]) == original_identity,
            "Failed creation must not alter a live object's content identity");
    object_result = VK_SUCCESS;
    VkBaseInStructure unknown{VK_STRUCTURE_TYPE_MAX_ENUM, nullptr};
    shader.pNext = &unknown;
    VkShaderModule opaque[2];
    for (auto& module : opaque) CreateShaderModule(f.handle, &shader, nullptr, &module);
    Require(f.device.ShaderModuleIdentity(opaque[0]) != f.device.ShaderModuleIdentity(opaque[1]),
            "Unknown shader extensions must keep distinct lifetime identities");
    shader.pNext = nullptr;

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.maxLod = 1.0f;
    VkSampler samplers[3];
    for (uint32_t i = 0; i < 2; ++i) CreateSampler(f.handle, &sampler_info, nullptr, &samplers[i]);
    VkDescriptorSetLayoutBinding bindings[] = {{4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &samplers[0]},
                                               {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorBindingFlags binding_flags[] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, nullptr, 2,
                                                      binding_flags};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags, 0, 2, bindings};
    VkDescriptorSetLayout sets[3];
    CreateDescriptorSetLayout(f.handle, &set_info, nullptr, &sets[0]);
    bindings[0].pImmutableSamplers = &samplers[1];
    std::swap(bindings[0], bindings[1]);
    std::swap(binding_flags[0], binding_flags[1]);
    CreateDescriptorSetLayout(f.handle, &set_info, nullptr, &sets[1]);
    VkPushConstantRange ranges[] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 16}, {VK_SHADER_STAGE_FRAGMENT_BIT, 16, 16}};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &sets[0], 2, ranges};
    VkPipelineLayout layouts[3];
    CreatePipelineLayout(f.handle, &layout_info, nullptr, &layouts[0]);
    layout_info.pSetLayouts = &sets[1];
    std::swap(ranges[0], ranges[1]);
    CreatePipelineLayout(f.handle, &layout_info, nullptr, &layouts[1]);
    Require(f.device.PipelineLayoutIdentity(layouts[0]) == f.device.PipelineLayoutIdentity(layouts[1]),
            "Equal bindings, flags, immutable samplers and push constants must match independently of array order and handles");
    f.ci.layout = layouts[0];
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS, "Pipeline with tracked layout failed");
    f.ci.layout = layouts[1];
    count = calls.size();
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == count + 1,
            "Equivalent pipeline-layout objects must reuse both shader libraries");
    DestroyDescriptorSetLayout(f.handle, sets[0], nullptr);
    DestroySampler(f.handle, samplers[0], nullptr);
    DestroyPipelineLayout(f.handle, layouts[0], nullptr);
    recycled_object = CastToUint64(layouts[0]);
    CreatePipelineLayout(f.handle, &layout_info, nullptr, &layouts[0]);
    Require(f.device.PipelineLayoutIdentity(layouts[0]) == f.device.PipelineLayoutIdentity(layouts[1]),
            "Recreating identical layout content must preserve identity after the original dependencies are destroyed");
    sampler_info.minFilter = VK_FILTER_NEAREST;
    CreateSampler(f.handle, &sampler_info, nullptr, &samplers[2]);
    bindings[1].pImmutableSamplers = &samplers[2];
    CreateDescriptorSetLayout(f.handle, &set_info, nullptr, &sets[2]);
    layout_info.pSetLayouts = &sets[2];
    CreatePipelineLayout(f.handle, &layout_info, nullptr, &layouts[2]);
    Require(f.device.PipelineLayoutIdentity(layouts[2]) != f.device.PipelineLayoutIdentity(layouts[1]),
            "Changed immutable sampler content must distinguish pipeline layouts");
    layout_info.pSetLayouts = &sets[1];
    ranges[0].size += 4;
    VkPipelineLayout changed;
    CreatePipelineLayout(f.handle, &layout_info, nullptr, &changed);
    Require(f.device.PipelineLayoutIdentity(changed) != f.device.PipelineLayoutIdentity(layouts[1]),
            "Changed push constants must distinguish layouts");
    ranges[0].size -= 4;
    layout_info.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
    CreatePipelineLayout(f.handle, &layout_info, nullptr, &changed);
    Require(f.device.PipelineLayoutIdentity(changed) != f.device.PipelineLayoutIdentity(layouts[1]),
            "Independent-set and other layout flags must not be lost");
    layout_info.pNext = &unknown;
    VkPipelineLayout unknown_layouts[2];
    for (auto& layout : unknown_layouts) CreatePipelineLayout(f.handle, &layout_info, nullptr, &layout);
    Require(f.device.PipelineLayoutIdentity(unknown_layouts[0]) != f.device.PipelineLayoutIdentity(unknown_layouts[1]),
            "Unknown layout extensions must keep distinct lifetime identities");

    VkAttachmentDescription attachment{0,
                                       VK_FORMAT_R8G8B8A8_UNORM,
                                       VK_SAMPLE_COUNT_1_BIT,
                                       VK_ATTACHMENT_LOAD_OP_CLEAR,
                                       VK_ATTACHMENT_STORE_OP_STORE,
                                       VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                       VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                       VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkRenderPassCreateInfo render_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &attachment, 1, &subpass};
    VkRenderPass passes[3];
    for (uint32_t i = 0; i < 2; ++i) CreateRenderPass(f.handle, &render_info, nullptr, &passes[i]);
    f.ci.pNext = nullptr;
    f.ci.renderPass = passes[0];
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS, "Legacy pipeline with a tracked render pass failed");
    f.ci.renderPass = passes[1];
    count = calls.size();
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == count + 1,
            "Equal render-pass objects must reuse shader and fragment-output libraries");
    DestroyRenderPass(f.handle, passes[0], nullptr);
    recycled_object = CastToUint64(passes[0]);
    attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    CreateRenderPass(f.handle, &render_info, nullptr, &passes[0]);
    Require(f.device.RenderPassIdentity(passes[0]) != f.device.RenderPassIdentity(passes[1]),
            "Recycled render-pass handles with different formats must not reuse stale libraries");
    render_info.pNext = &unknown;
    for (uint32_t i = 0; i < 2; ++i) CreateRenderPass(f.handle, &render_info, nullptr, &passes[i]);
    Require(f.device.RenderPassIdentity(passes[0]) != f.device.RenderPassIdentity(passes[1]),
            "Unknown render-pass extensions must keep separate identities");
    VkAttachmentDescription2 attachment2{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    attachment2.format = attachment.format;
    attachment2.samples = VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentReference2 ref2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription2 subpass2{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    subpass2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass2.colorAttachmentCount = 1;
    subpass2.pColorAttachments = &ref2;
    VkRenderPassCreateInfo2 render2{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, nullptr, 0, 1, &attachment2, 1, &subpass2};
    for (uint32_t i = 0; i < 2; ++i) CreateRenderPass2(f.handle, &render2, nullptr, &passes[i]);
    Require(f.device.RenderPassIdentity(passes[0]) == f.device.RenderPassIdentity(passes[1]),
            "Equal render-pass-2 definitions must share content identity");
    ref2.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    CreateRenderPass2(f.handle, &render2, nullptr, &passes[2]);
    Require(f.device.RenderPassIdentity(passes[0]) != f.device.RenderPassIdentity(passes[2]),
            "Nested render-pass-2 attachment-reference state must distinguish definitions");
    VkSubpassDescriptionDepthStencilResolve resolve{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    resolve.pDepthStencilResolveAttachment = reinterpret_cast<const VkAttachmentReference2*>(uintptr_t{1});
    subpass2.pNext = &resolve;
    for (uint32_t i = 0; i < 2; ++i) CreateRenderPass2(f.handle, &render2, nullptr, &passes[i]);
    Require(f.device.RenderPassIdentity(passes[0]) == f.device.RenderPassIdentity(passes[1]),
            "Disabled depth/stencil resolve must not inspect an ignored resolve attachment pointer");
}
}  // namespace content_identity_regression

namespace multisample_identity_regression {
static VKAPI_ATTR VkResult VKAPI_CALL Observe(VkDevice device, VkPipelineCache cache, uint32_t count,
                                              const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* allocator,
                                              VkPipeline* output) {
    if (Node(ci->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT) && ci->pMultisampleState) {
        const auto& ms = *ci->pMultisampleState;
        Require(ms.rasterizationSamples == VK_SAMPLE_COUNT_1_BIT && !ms.pSampleMask && !ms.alphaToCoverageEnable &&
                    !ms.alphaToOneEnable && ms.minSampleShading == 0.0f,
                "Cached FS/FO libraries must receive identical canonical values for ignored multisample fields");
    }
    return NextCreate(device, cache, count, ci, allocator, output);
}

static void Run() {
    Fixture f;
    f.device.vtable.CreateGraphicsPipelines = Observe;
    VkDynamicState states[] = {VK_DYNAMIC_STATE_SAMPLE_MASK_EXT, VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT,
                               VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT, VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 4, states};
    f.ci.pDynamicState = &dynamic;
    f.ms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    f.ms.pSampleMask = reinterpret_cast<const VkSampleMask*>(uintptr_t{1});
    f.ms.alphaToCoverageEnable = f.ms.alphaToOneEnable = VK_TRUE;
    f.ms.minSampleShading = 0.75f;
    VkPipeline output;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS, "Initial dynamic multisample pipeline failed");
    f.ms.rasterizationSamples = VK_SAMPLE_COUNT_64_BIT;
    f.ms.pSampleMask = nullptr;
    f.ms.alphaToCoverageEnable = f.ms.alphaToOneEnable = VK_FALSE;
    f.ms.minSampleShading = 0.25f;
    f.attachment.blendEnable = VK_TRUE;
    const auto count = calls.size();
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && calls.size() == count + 2,
            "A reused FS library and newly built FO library must agree despite different ignored application values");
    f.ci.pDynamicState = nullptr;
    f.ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const auto null_mask = Key(f.ci, GplSubset::kFragmentShader);
    VkSampleMask all_samples = ~VkSampleMask{0};
    f.ms.pSampleMask = &all_samples;
    Require(null_mask != Key(f.ci, GplSubset::kFragmentShader),
            "A NULL mask must not collide with an explicit all-ones mask in the shared FS/FO key");
}
}  // namespace multisample_identity_regression

static void ContinuousConversion() {
    Fixture f;
    constexpr uint32_t pipeline_count = 1024;
    for (uint32_t i = 0; i < pipeline_count; ++i) {
        // Distinct shader identities prevent PR/FS sharing and used to trip the distribution gate.
        f.stages[0].module = Handle<VkShaderModule>(0x100000 + i * 2);
        f.stages[1].module = Handle<VkShaderModule>(0x100001 + i * 2);
        VkPipeline output = VK_NULL_HANDLE;
        Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && output != VK_NULL_HANDLE && native_index == 0 &&
                    calls.back().linked_libraries == 4,
                "Eligible pipelines must continue converting even when shader libraries are not shared");
    }
    Require(f.device.converted_pipeline_count == pipeline_count && f.device.expensive_library_count == pipeline_count * 2,
            "Conversion statistics must continue beyond the former warmup threshold");
    const auto stats = f.device.cache.GetStats();
    Require(stats[1].built == 1 && stats[1].hits == pipeline_count - 1 && stats[8].built == 1 &&
                stats[8].hits == pipeline_count - 1 && stats[2].built == pipeline_count && stats[2].hits == 0 &&
                stats[4].built == pipeline_count && stats[4].hits == 0,
            "Subset counters must distinguish VI/FO reuse from unique PR/FS builds");
}

static void CacheReuseStatistics() {
    Fixture f;
    fail_library_once = true;
    VkPipeline output = VK_NULL_HANDLE;
    Require(f.Create(1, &f.ci, &output) == VK_SUCCESS && native_index == 1,
            "A failed VI build must fall back before the successful retry");
    for (uint32_t i = 0; i < 3; ++i) Require(f.Create(1, &f.ci, &output) == VK_SUCCESS, "Repeated conversion failed");
    const auto stats = f.device.cache.GetStats();
    for (uint32_t subset : {1u, 2u, 4u, 8u})
        Require(stats[subset].built == 1 && stats[subset].hits == 2 && stats[subset].in_flight == 0 &&
                    stats[subset].failures == (subset == 1 ? 1u : 0u),
                "Failed builds must not count as successful builds or hits; a successful retry can be reused");
}

namespace bounded_cache_regression {
static std::vector<VkPipeline> destroyed;
static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    Require(!allocator, "Cached libraries must be destroyed with their default allocator");
    destroyed.push_back(pipeline);
}
static VkPipeline Build(DeviceData&, const void* context) { return *static_cast<const VkPipeline*>(context); }

static void Run() {
    destroyed.clear();
    Fixture f;
    f.device.vtable.DestroyPipeline = Destroy;
    LibraryCache cache(2);
    VkPipeline handles[] = {Handle<VkPipeline>(0x100), Handle<VkPipeline>(0x200), Handle<VkPipeline>(0x300)};
    bool built;
    auto a = cache.GetOrBuild("a", f.device, &handles[0], Build, &built, GplSubset::kPreRaster);
    cache.GetOrBuild("b", f.device, &handles[1], Build, &built, GplSubset::kPreRaster);
    Require(cache.GetOrBuild("a", f.device, &handles[0], Build, &built, GplSubset::kPreRaster) == a && !built,
            "A cache hit must refresh recency and return the same lease");
    auto c = cache.GetOrBuild("c", f.device, &handles[2], Build, &built, GplSubset::kPreRaster);
    Require(cache.ResidentCount() == 2 && destroyed.size() == 1 && destroyed[0] == handles[1],
            "LRU overflow must destroy the unused oldest library");
    Require(cache.GetStats()[2].resident == 2 && cache.GetStats()[2].evicted == 1,
            "Subset residency must count retained libraries, not cumulative builds");
    cache.DestroyAll(f.device);
    Require(destroyed.size() == 1 && f.device.library_lifetime.live == 2 && cache.GetStats()[2].resident == 0,
            "Cache removal must preserve libraries leased by an in-progress link");
    a.reset();
    c.reset();
    Require(destroyed.size() == 3 && f.device.library_lifetime.live == 0 && cache.GetStats()[2].evicted == 1,
            "The final lease must destroy each library exactly once");

    destroyed.clear();
    Fixture linked(4);
    linked.device.vtable.DestroyPipeline = Destroy;
    VkPipeline first, second;
    Require(linked.Create(1, &linked.ci, &first) == VK_SUCCESS, "Initial executable creation failed");
    linked.ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    linked.stages[0].module = Handle<VkShaderModule>(0x5000);
    linked.stages[1].module = Handle<VkShaderModule>(0x6000);
    linked.attachment.blendEnable = VK_TRUE;
    Require(linked.Create(1, &linked.ci, &second) == VK_SUCCESS && linked.device.cache.ResidentCount() == 4 &&
                linked.device.library_lifetime.live == 8 && destroyed.empty(),
            "Eviction must preserve all libraries referenced by live executable pipelines, including pure GPL");
    DestroyPipeline(linked.handle, first, nullptr);
    Require(destroyed.size() == 5 && destroyed[0] == first && linked.device.library_lifetime.live == 4,
            "Evicted dependencies must be released after their executable, not before");
    DestroyPipeline(linked.handle, second, nullptr);
    linked.device.cache.DestroyAll(linked.device);
    Require(destroyed.size() == 10 && linked.device.library_lifetime.live == 0 && linked.device.pipeline_libraries.empty(),
            "Executable and cache cleanup must release all dependencies exactly once");
}

struct ConcurrentBuild {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool finish = false;
    std::atomic<uint32_t> started{0};
    std::atomic<uint32_t> builds{0};
};
static VkPipeline ConcurrentBuilder(DeviceData&, const void* context) {
    auto& state = *const_cast<ConcurrentBuild*>(static_cast<const ConcurrentBuild*>(context));
    ++state.builds;
    std::unique_lock<std::mutex> lock(state.mutex);
    state.entered = true;
    state.cv.notify_all();
    state.cv.wait(lock, [&] { return state.finish; });
    return Handle<VkPipeline>(0x7000);
}
static void Concurrent() {
    Fixture f;
    ConcurrentBuild state;
    LibraryCache cache(1);
    std::array<LibraryCache::Handle, 6> leases;
    std::array<std::thread, 6> threads;
    for (size_t i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread([&, i] {
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                ++state.started;
            }
            state.cv.notify_all();
            bool built;
            leases[i] = cache.GetOrBuild("same", f.device, &state, ConcurrentBuilder, &built, GplSubset::kFragmentShader);
        });
    }
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.cv.wait(lock, [&] { return state.entered && state.started == threads.size(); });
        state.finish = true;
    }
    state.cv.notify_all();
    for (auto& thread : threads) thread.join();
    for (const auto& lease : leases) Require(lease == leases[0], "Concurrent requests must share one library generation");
    const auto stats = cache.GetStats()[4];
    Require(state.builds == 1 && stats.built == 1 && stats.hits == 5 && stats.in_flight == 0,
            "Single-flight builds and cache counters must remain correct under concurrency");
    cache.DestroyAll(f.device);
    Require(f.device.library_lifetime.live == 1, "Concurrent leases must survive removal of cache ownership");
    leases = {};
    Require(f.device.library_lifetime.live == 0, "Concurrent leases must release the library after their last user");
}
}  // namespace bounded_cache_regression

int main() {
    bounded_cache_regression::Run();
    bounded_cache_regression::Concurrent();
    CacheReuseStatistics();
    multisample_identity_regression::Run();
    content_identity_regression::Run();
    identifier_regression::Run();
    ContinuousConversion();
    split_state_regression::CombinedLibraries();
    split_state_regression::FragmentState();
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
