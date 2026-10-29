// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Included in typed_pipeline_tests.cpp's anonymous namespace.
Words VulkanSource(const Words &source) {
    spvtools::Optimizer upgrade(SPV_ENV_VULKAN_1_3);
    upgrade.RegisterPass(spvtools::CreateUpgradeMemoryModelPass());
    Words result;
    Require(upgrade.Run(source.data(), source.size(), &result), "source Vulkan memory-model upgrade failed");
    return result;
}
struct MemoryFacts {
    bool vulkan = false, device_capability = false;
    uint32_t available = 0, visible = 0, nonprivate = 0;
    uint32_t texel_available = 0, texel_visible = 0, nonprivate_texel = 0;
    std::map<uint32_t, uint32_t> constants;
    std::vector<std::pair<uint32_t, uint32_t>> atomic_scope_semantics;
    std::vector<uint32_t> barrier_semantics;
    std::vector<uint32_t> barrier_scopes;
    std::vector<uint32_t> scopes;
};
MemoryFacts InspectMemory(const Words &words) {
    MemoryFacts f;
    for (size_t p = 5; p < words.size(); p += words[p] >> 16) {
        const auto op = words[p] & 65535;
        if (op == spv::OpConstant && (words[p] >> 16) == 4) f.constants[words[p + 2]] = words[p + 3];
        if (op == spv::OpCapability && words[p + 1] == spv::CapabilityVulkanMemoryModelDeviceScope) f.device_capability = true;
        if (op == spv::OpMemoryModel) f.vulkan = words[p + 2] == spv::MemoryModelVulkan;
        if (op == spv::OpLoad || op == spv::OpStore) {
            const auto mask = p + (op == spv::OpLoad ? 4 : 3);
            if (mask < p + (words[p] >> 16)) {
                f.available += (words[mask] & spv::MemoryAccessMakePointerAvailableMask) != 0;
                f.visible += (words[mask] & spv::MemoryAccessMakePointerVisibleMask) != 0;
                f.nonprivate += (words[mask] & spv::MemoryAccessNonPrivatePointerMask) != 0;
            }
        }
        if (op == spv::OpImageRead || op == spv::OpImageWrite) {
            const auto mask = p + (op == spv::OpImageRead ? 5 : 4);
            if (mask < p + (words[p] >> 16)) {
                f.texel_available += (words[mask] & spv::ImageOperandsMakeTexelAvailableMask) != 0;
                f.texel_visible += (words[mask] & spv::ImageOperandsMakeTexelVisibleMask) != 0;
                f.nonprivate_texel += (words[mask] & spv::ImageOperandsNonPrivateTexelMask) != 0;
            }
        }
        if (op == spv::OpAtomicLoad || op == spv::OpAtomicIAdd)
            f.atomic_scope_semantics.emplace_back(f.constants.at(words[p + 4]), f.constants.at(words[p + 5]));
        if (op == spv::OpAtomicStore)
            f.atomic_scope_semantics.emplace_back(f.constants.at(words[p + 2]), f.constants.at(words[p + 3]));
        if (op == spv::OpMemoryBarrier) {
            f.barrier_semantics.push_back(f.constants.at(words[p + 2]));
            f.barrier_scopes.push_back(f.constants.at(words[p + 1]));
        }
    }
    struct ScopeData {
        size_t offset = 5;
        MemoryFacts *facts;
    } data{5, &f};
    auto collect = [](void *user, const spv_parsed_instruction_t *i) {
        auto &d = *static_cast<ScopeData *>(user);
        for (uint32_t n = 0; n < i->num_operands; ++n)
            if (i->operands[n].type == SPV_OPERAND_TYPE_SCOPE_ID)
                d.facts->scopes.push_back(d.facts->constants.at(i->words[i->operands[n].offset]));
        d.offset += i->num_words;
        return SPV_SUCCESS;
    };
    auto context = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
    auto status = spvBinaryParse(context, &data, words.data(), words.size(), nullptr, collect, nullptr);
    spvContextDestroy(context);
    Require(status == SPV_SUCCESS, "memory test could not parse scope operands");
    return f;
}
void VulkanMemoryModelChecks(const TypedContinuationProgram &caller, const TypedContinuationProgram &callee) {
    auto source = VulkanSource(WithShaderCallMemoryBarriers(Caller()));
    const auto stage = CompileTypedPipelineStage(source, "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr, "vel_rt_stage_0");
    auto scheduler = CompileGLSL(Scheduler(caller, callee), VK_SHADER_STAGE_COMPUTE_BIT, GLSLTarget::Vulkan11Spirv14);
    const auto linked = LinkTypedPipelineProgram(scheduler, {stage, callee});
    const auto facts = InspectMemory(linked), original = InspectMemory(source);
    Require(facts.vulkan && facts.device_capability, "mixed typed module lost Vulkan memory model/device scope capability");
    Require(!original.barrier_semantics.empty(), "source test lacks barriers");
    for (const auto semantics : original.barrier_semantics)
        Require(std::count(facts.barrier_semantics.begin(), facts.barrier_semantics.end(), semantics) ==
                    std::count(original.barrier_semantics.begin(), original.barrier_semantics.end(), semantics),
                "source barrier semantics were dropped or changed");
    Require(std::find(facts.scopes.begin(), facts.scopes.end(), spv::ScopeShaderCallKHR) == facts.scopes.end(),
            "source ShaderCall scope leaked into compute");
    Require(std::find(facts.scopes.begin(), facts.scopes.end(), spv::ScopeDevice) != facts.scopes.end(),
            "source ShaderCall scope lost its device-domain memory operations");
    Require(
        std::all_of(facts.barrier_scopes.begin(), facts.barrier_scopes.end(), [](auto scope) { return scope == spv::ScopeDevice; }),
        "mixed GLSL450 peer/scheduler Device barriers were narrowed to QueueFamily");
    CheckNative(linked);

    const auto native_source = CompileGLSL(R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_KHR_memory_scope_semantics : require
#pragma use_vulkan_memory_model
layout(set=0,binding=0,std430) coherent buffer Values { uint value[]; } data;
layout(set=0,binding=1,r32ui) coherent uniform uimage2D pixels;
layout(location=0) callableDataEXT uint callData;
void main() {
    uint index=gl_LaunchIDEXT.x;
    imageStore(pixels,ivec2(index,0),uvec4(index+23u));
    data.value[index*4u]=index+11u;
    atomicStore(data.value[index*4u+1u],index+17u,gl_ScopeDevice,gl_StorageSemanticsBuffer,
                gl_SemanticsRelease|gl_SemanticsMakeAvailable);
    memoryBarrier(gl_ScopeShaderCallEXT,gl_StorageSemanticsBuffer,
                  gl_SemanticsAcquireRelease|gl_SemanticsMakeAvailable|gl_SemanticsMakeVisible);
    callData=atomicLoad(data.value[index*4u+1u],gl_ScopeShaderCallEXT,gl_StorageSemanticsBuffer,
                        gl_SemanticsAcquire|gl_SemanticsMakeVisible);
    callData+=atomicAdd(data.value[index*4u+3u],19u,gl_ScopeQueueFamily,gl_StorageSemanticsBuffer,
                       gl_SemanticsAcquireRelease|gl_SemanticsMakeAvailable|gl_SemanticsMakeVisible);
    callData+=imageLoad(pixels,ivec2(index,0)).x;
    executeCallableEXT(0,0);
    data.value[index*4u+2u]=callData+data.value[index*4u];
}
)glsl",
                                           VK_SHADER_STAGE_RAYGEN_BIT_KHR, GLSLTarget::Vulkan11Spirv14);
    // A scalar call interface is sufficient for this native memory-operation
    // preservation check; no private frame byte offsets are used as an oracle.
    const auto native_callee = CompileGLSL(R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) callableDataInEXT uint callData;
void main(){callData+=29u;}
)glsl",
                                           VK_SHADER_STAGE_CALLABLE_BIT_KHR, GLSLTarget::Vulkan11Spirv14);
    auto a = CompileTypedPipelineStage(native_source, "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr, "vel_rt_stage_0");
    auto b = CompileTypedPipelineStage(native_callee, "main", VK_SHADER_STAGE_CALLABLE_BIT_KHR, nullptr, "vel_rt_stage_1");
    auto native_linked = LinkTypedPipelineProgram(CompileGLSL(Scheduler(a, b), VK_SHADER_STAGE_COMPUTE_BIT), {a, b});
    auto n = InspectMemory(native_linked), s = InspectMemory(native_source);
    Require(n.vulkan && n.device_capability && n.available >= s.available && n.visible >= s.visible &&
                n.nonprivate >= s.nonprivate && s.available && s.visible,
            "native non-private availability/visibility operands were not preserved");
    Require(n.texel_available == s.texel_available && n.texel_visible == s.texel_visible &&
                n.nonprivate_texel == s.nonprivate_texel && s.texel_available && s.texel_visible,
            "native image availability/visibility operands were not preserved");
    Require(n.atomic_scope_semantics.size() == s.atomic_scope_semantics.size() && !n.atomic_scope_semantics.empty(),
            "native atomic operations were dropped");
    for (size_t i = 0; i < s.atomic_scope_semantics.size(); ++i) {
        const auto scope =
            s.atomic_scope_semantics[i].first == spv::ScopeShaderCallKHR ? spv::ScopeDevice : s.atomic_scope_semantics[i].first;
        Require(
            n.atomic_scope_semantics[i].first == scope && n.atomic_scope_semantics[i].second == s.atomic_scope_semantics[i].second,
            "native atomic scope/semantics changed beyond ShaderCall domain lowering");
    }
    CheckNative(native_linked, 2);
    std::cout << "Vulkan memory model mixed peers, native memory/atomic operands and ShaderCall scopes validated\n";
}
