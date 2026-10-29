/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT
 * CPU public-entry spies, included in test_rtp_bindings.cpp's namespace.
 */
using MixedWords = std::vector<uint32_t>;
std::map<VkShaderModule, MixedWords> selected_modules;
const VkAllocationCallbacks *selected_allocator = nullptr;
unsigned selected_creates = 0, selected_destroys = 0, selected_compute = 0, selected_graphics = 0;
unsigned selected_fail_create = 0;
bool selected_fail_batch = false;
unsigned selected_rewrites = 0;
MixedWords MixedEntriesSource() {
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %raygen "raygen" %payload
OpEntryPoint GLCompute %compute "ordinary" %buffer
OpEntryPoint Vertex %vertex "ordinary" %buffer %position
OpEntryPoint Fragment %fragment "ordinary" %buffer %color
OpExecutionMode %compute LocalSize 1 1 1
OpExecutionMode %fragment OriginUpperLeft
OpDecorate %payload Location 0
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 3
OpDecorate %position BuiltIn Position
OpDecorate %color Location 0
OpDecorate %special SpecId 19
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%vector = OpTypeVector %float 4
%block = OpTypeStruct %uint
%storage = OpTypePointer StorageBuffer %block
%word = OpTypePointer StorageBuffer %uint
%out = OpTypePointer Output %vector
%data = OpTypePointer RayPayloadKHR %uint
%buffer = OpVariable %storage StorageBuffer
%position = OpVariable %out Output
%color = OpVariable %out Output
%payload = OpVariable %data RayPayloadKHR
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%special = OpSpecConstant %uint 23
%fn = OpTypeFunction %void
%raygen = OpFunction %void None %fn
%r = OpLabel
OpStore %payload %one
OpReturn
OpFunctionEnd
%compute = OpFunction %void None %fn
%c = OpLabel
%cptr = OpAccessChain %word %buffer %zero
OpStore %cptr %special
OpReturn
OpFunctionEnd
%vertex = OpFunction %void None %fn
%v = OpLabel
%vptr = OpAccessChain %word %buffer %zero
%vword = OpLoad %uint %vptr
%vfloat = OpConvertUToF %float %vword
%vout = OpCompositeConstruct %vector %vfloat %fzero %fzero %fone
OpStore %position %vout
OpReturn
OpFunctionEnd
%fragment = OpFunction %void None %fn
%f = OpLabel
%fptr = OpAccessChain %word %buffer %zero
%fword = OpLoad %uint %fptr
%ffloat = OpConvertUToF %float %fword
%fout = OpCompositeConstruct %vector %ffloat %fzero %fzero %fone
OpStore %color %fout
OpReturn
OpFunctionEnd
)spv";
    MixedWords words;
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    CHECK(tools.Assemble(source, &words) && tools.Validate(words));
    return words;
}
void CheckSelectedModule(const MixedWords &words) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    CHECK(tools.Validate(words));
    unsigned entries = 0, bindings = 0, resources = 0;
    for (size_t p = 5; p < words.size(); p += words[p] >> 16) {
        const auto op = words[p] & 65535;
        if (op == spv::OpCapability) CHECK(words[p + 1] != spv::CapabilityRayTracingKHR);
        if (op == spv::OpEntryPoint) {
            ++entries;
            CHECK(words[p + 1] == spv::ExecutionModelGLCompute || words[p + 1] == spv::ExecutionModelVertex ||
                  words[p + 1] == spv::ExecutionModelFragment);
            CHECK(!std::strcmp(reinterpret_cast<const char *>(words.data() + p + 3), "ordinary"));
        }
        if (op == spv::OpTypePointer) CHECK(words[p + 2] != spv::StorageClassRayPayloadKHR);
        if (op == spv::OpDecorate && words[p + 2] == spv::DecorationBinding) {
            ++bindings;
            CHECK(words[p + 3] == 3);
        }
        if (op == spv::OpVariable && words[p + 3] == spv::StorageClassStorageBuffer) ++resources;
    }
    CHECK(entries == 1 && bindings == 1 && resources == 1);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSelectedShader(VkDevice d, const VkShaderModuleCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkShaderModule *out) {
    CHECK(d == device && allocator == selected_allocator);
    MixedWords words(info->pCode, info->pCode + info->codeSize / 4);
    CheckSelectedModule(words);
    *out = VK_NULL_HANDLE;
    if (++selected_creates == selected_fail_create) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = Handle<VkShaderModule>(next_native++);
    CHECK(selected_modules.emplace(*out, std::move(words)).second);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroySelectedShader(VkDevice d, VkShaderModule module, const VkAllocationCallbacks *allocator) {
    CHECK(d == device && allocator == selected_allocator && selected_modules.erase(module) == 1);
    ++selected_destroys;
}
void CheckSelectedStage(const VkPipelineShaderStageCreateInfo &stage) {
    CHECK(stage.pName && !std::strcmp(stage.pName, "ordinary") && selected_modules.count(stage.module));
    CHECK(stage.pSpecializationInfo && stage.pSpecializationInfo->mapEntryCount == 1 &&
          stage.pSpecializationInfo->pMapEntries[0].constantID == 19 &&
          *static_cast<const uint32_t *>(stage.pSpecializationInfo->pData) == 47);
    uint32_t model = UINT32_MAX;
    bool local_size = false, upper_left = false, specialized = false;
    const auto &words = selected_modules.at(stage.module);
    for (size_t p = 5; p < words.size(); p += words[p] >> 16) {
        const auto op = words[p] & 65535;
        if (op == spv::OpEntryPoint) model = words[p + 1];
        if (op == spv::OpExecutionMode) {
            local_size |= words[p + 2] == spv::ExecutionModeLocalSize;
            upper_left |= words[p + 2] == spv::ExecutionModeOriginUpperLeft;
        }
        specialized |= op == spv::OpSpecConstant;
    }
    if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT) CHECK(model == spv::ExecutionModelGLCompute && local_size && specialized);
    if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) CHECK(model == spv::ExecutionModelVertex && !upper_left && !local_size);
    if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) CHECK(model == spv::ExecutionModelFragment && upper_left && !local_size);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSelectedCompute(VkDevice d, VkPipelineCache, uint32_t count,
                                                     const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                     VkPipeline *out) {
    CHECK(d == device && a == selected_allocator && count == 2);
    ++selected_compute;
    for (uint32_t i = 0; i < count; ++i) {
        CheckSelectedStage(infos[i].stage);
        CHECK(layouts.count(infos[i].layout));
        out[i] = selected_fail_batch && i ? VK_NULL_HANDLE : Handle<VkPipeline>(9000 + i);
    }
    return selected_fail_batch ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSelectedGraphics(VkDevice d, VkPipelineCache, uint32_t count,
                                                      const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                      VkPipeline *out) {
    CHECK(d == device && a == selected_allocator && count == 1 && infos[0].stageCount == 2);
    ++selected_graphics;
    for (uint32_t i = 0; i < infos[0].stageCount; ++i) CheckSelectedStage(infos[0].pStages[i]);
    CHECK(layouts.count(infos[0].layout));
    *out = Handle<VkPipeline>(9002);
    return VK_SUCCESS;
}
void MixedEntryPipelines() {
    const auto create_shader = dev.vtable.CreateShaderModule;
    const auto destroy_shader = dev.vtable.DestroyShaderModule;
    const auto create_compute = dev.vtable.CreateComputePipelines;
    const auto create_graphics = dev.vtable.CreateGraphicsPipelines;
    dev.vtable.CreateShaderModule = CreateSelectedShader;
    dev.vtable.DestroyShaderModule = DestroySelectedShader;
    dev.vtable.CreateComputePipelines = CreateSelectedCompute;
    dev.vtable.CreateGraphicsPipelines = CreateSelectedGraphics;
    CallbackOwner callback_owner;
    auto callbacks = Callbacks(callback_owner);
    selected_allocator = &callbacks;
    auto source = MixedEntriesSource();
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = source.size() * 4;
    info.pCode = source.data();
    VkShaderModule module{};
    bool handled = false;
    CHECK(vkrt_rtp_create_shader(&dev, &info, nullptr, &module, &handled) == VK_SUCCESS && handled);
    VkDescriptorSetLayoutBinding binding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                         nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    const auto set = MakeDescriptorLayout(set_info);
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set;
    VkPipelineLayout layout{};
    auto make_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(vkrt_rtp_proc("vkCreatePipelineLayout"));
    auto drop_layout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(vkrt_rtp_proc("vkDestroyPipelineLayout"));
    CHECK(make_layout(device, &layout_info, nullptr, &layout) == VK_SUCCESS);
    uint32_t value = 47;
    VkSpecializationMapEntry map{19, 0, sizeof(value)};
    VkSpecializationInfo special{1, &map, sizeof(value), &value};
    VkComputePipelineCreateInfo compute[2]{};
    for (auto &pipeline : compute) {
        pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline.layout = layout;
        pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          nullptr,
                          0,
                          VK_SHADER_STAGE_COMPUTE_BIT,
                          module,
                          "ordinary",
                          &special};
    }
    const auto snapshot = Snapshot(compute);
    VkPipeline results[2]{};
    auto make_compute = reinterpret_cast<PFN_vkCreateComputePipelines>(vkrt_rtp_proc("vkCreateComputePipelines"));
    CHECK(make_compute(device, VK_NULL_HANDLE, 2, compute, &callbacks, results) == VK_SUCCESS);
    CHECK(selected_compute == 1 && results[0] && results[1] && selected_modules.empty() && selected_destroys == 2);
    selected_fail_batch = true;
    CHECK(make_compute(device, VK_NULL_HANDLE, 2, compute, &callbacks, results) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(selected_compute == 2 && results[0] && !results[1] && selected_modules.empty() && selected_destroys == 4);
    selected_fail_batch = false;
    selected_fail_create = selected_creates + 2;
    CHECK(make_compute(device, VK_NULL_HANDLE, 2, compute, &callbacks, results) == VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(selected_compute == 2 && !results[0] && !results[1] && selected_modules.empty() && selected_destroys == 5);
    selected_fail_create = 0;
    CHECK(!std::memcmp(&compute, snapshot.data(), sizeof(compute)));
    VkPipelineShaderStageCreateInfo stages[2]{compute[0].stage, compute[0].stage};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.rasterizerDiscardEnable = VK_TRUE;
    rasterization.lineWidth = 1;
    graphics.layout = layout;
    graphics.pVertexInputState = &vertex_input;
    graphics.pInputAssemblyState = &assembly;
    graphics.pRasterizationState = &rasterization;
    graphics.renderPass = Handle<VkRenderPass>(0x6d697865);
    graphics.stageCount = 2;
    graphics.pStages = stages;
    auto make_graphics = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(vkrt_rtp_proc("vkCreateGraphicsPipelines"));
    CHECK(make_graphics(device, VK_NULL_HANDLE, 1, &graphics, &callbacks, results) == VK_SUCCESS);
    CHECK(selected_graphics == 1 && results[0] && selected_modules.empty() && selected_destroys == 7);
    CHECK(selected_rewrites == selected_creates && source == MixedEntriesSource());
    drop_layout(device, layout, nullptr);
    DropDescriptorLayout(set);
    CHECK(vkrt_rtp_destroy_shader(&dev, module, nullptr));
    dev.vtable.CreateShaderModule = create_shader;
    dev.vtable.DestroyShaderModule = destroy_shader;
    dev.vtable.CreateComputePipelines = create_compute;
    dev.vtable.CreateGraphicsPipelines = create_graphics;
    selected_allocator = nullptr;
}
