// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "typed_pipeline.h"
#include "compiler.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using namespace vel::rt;
using Words = std::vector<uint32_t>;
void Require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) { Require(result == VK_SUCCESS, "Vulkan result " + std::to_string(result)); }
#include "frame_test_gpu.h"

Words Assemble(const std::string &source) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string errors;
    tools.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
        errors += std::string(message) + "\n";
    });
    Words result;
    Require(tools.Assemble(source, &result), "Source assembly: " + errors);
    Require(tools.Validate(result), "Source validation: " + errors);
    return result;
}

const char *kPrefix = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability Int16
OpCapability Int64
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
)spv";
const char *kTypes = R"spv(
%void = OpTypeVoid
%bool = OpTypeBool
%u16 = OpTypeInt 16 0
%u32 = OpTypeInt 32 0
%u64 = OpTypeInt 64 0
%f32 = OpTypeFloat 32
%v2u = OpTypeVector %u32 2
%v3u = OpTypeVector %u32 3
%v3f = OpTypeVector %f32 3
%payload = OpTypeStruct %u32 %bool %u64 %u16 %v3f
%c0 = OpConstant %u32 0
%c1 = OpConstant %u32 1
%c2 = OpConstant %u32 2
%c3 = OpConstant %u32 3
%c4 = OpConstant %u32 4
%c5 = OpConstant %u32 5
%c6 = OpConstant %u32 6
%c7 = OpConstant %u32 7
%c8 = OpConstant %u32 8
%c9 = OpConstant %u32 9
%c10 = OpConstant %u32 10
%c11 = OpConstant %u32 11
%c12 = OpConstant %u32 12
%c14 = OpConstant %u32 14
%c16 = OpConstant %u32 16
%h2 = OpConstant %u16 2
%h3 = OpConstant %u16 3
%delta64 = OpConstant %u64 4294967299
%f0 = OpConstant %f32 0
%f1 = OpConstant %f32 1
%f2 = OpConstant %f32 2
%f3 = OpConstant %f32 3
%fhalf = OpConstant %f32 0.5
%vector = OpConstantComposite %v3f %f1 %f2 %f3
%vector_delta = OpConstantComposite %v3f %fhalf %f1 %f2
%fn = OpTypeFunction %void
%input_ptr = OpTypePointer Input %v3u
%launch = OpVariable %input_ptr Input
)spv";

Words Caller() {
    return Assemble(std::string(kPrefix) + R"spv(
OpEntryPoint RayGenerationKHR %main "main" %launch %app %data
OpName %wide_spec "wide_spec"
OpName %small_spec "small_spec"
OpName %flag_spec "flag_spec"
OpName %snapshot "snapshot"
OpDecorate %launch BuiltIn LaunchIdKHR
OpDecorate %app DescriptorSet 0
OpDecorate %app Binding 0
OpDecorate %array ArrayStride 4
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %data Location 7
OpDecorate %wide_spec SpecId 13
OpDecorate %small_spec SpecId 12
OpDecorate %flag_spec SpecId 11
)spv" + kTypes + R"spv(
%flag_spec = OpSpecConstantTrue %bool
%small_spec = OpSpecConstant %u16 19
%wide_spec = OpSpecConstant %u64 4294967303
%initial = OpSpecConstantComposite %payload %c5 %flag_spec %wide_spec %small_spec %vector
%array = OpTypeRuntimeArray %u32
%block = OpTypeStruct %array
%block_ptr = OpTypePointer StorageBuffer %block
%word_ptr = OpTypePointer StorageBuffer %u32
%data_ptr = OpTypePointer CallableDataKHR %payload
%half_ptr = OpTypePointer CallableDataKHR %u16
%app = OpVariable %block_ptr StorageBuffer
%data = OpVariable %data_ptr CallableDataKHR
%main = OpFunction %void None %fn
%entry = OpLabel
%launch_value = OpLoad %v3u %launch
%id = OpCompositeExtract %u32 %launch_value 0
%alias = OpAccessChain %half_ptr %data %c3
OpStore %data %initial
OpBranch %loop
%loop = OpLabel
%i = OpPhi %u32 %c0 %entry %next %continue
%sum = OpPhi %u32 %c0 %entry %new_sum %continue
%condition = OpULessThan %bool %i %c3
OpLoopMerge %exit %continue None
OpBranchConditional %condition %body %exit
%body = OpLabel
%snapshot = OpLoad %payload %data
%snapshot_base = OpIMul %u32 %id %c16
%counter_index = OpIAdd %u32 %snapshot_base %c14
%counter_pointer = OpAccessChain %word_ptr %app %c0 %counter_index
%counter = OpLoad %u32 %counter_pointer
%counter_next = OpIAdd %u32 %counter %c1
OpStore %counter_pointer %counter_next
%index_base = OpIMul %u32 %id %c3
%index = OpIAdd %u32 %index_base %i
OpExecuteCallableKHR %index %data
%half = OpLoad %u16 %alias
%half_plus = OpIAdd %u16 %half %h2
OpStore %alias %half_plus
%old_value = OpCompositeExtract %u32 %snapshot 0
%old_wide = OpCompositeExtract %u64 %snapshot 2
%old_small = OpCompositeExtract %u16 %snapshot 3
%old_flag = OpCompositeExtract %bool %snapshot 1
%old_wide_words = OpBitcast %v2u %old_wide
%old_lo = OpCompositeExtract %u32 %old_wide_words 0
%old_hi = OpCompositeExtract %u32 %old_wide_words 1
%old_small_word = OpUConvert %u32 %old_small
%old_flag_word = OpSelect %u32 %old_flag %c1 %c0
%snapshot_index9 = OpIAdd %u32 %snapshot_base %c9
%snapshot_ptr9 = OpAccessChain %word_ptr %app %c0 %snapshot_index9
OpStore %snapshot_ptr9 %old_lo
%snapshot_index10 = OpIAdd %u32 %snapshot_base %c10
%snapshot_ptr10 = OpAccessChain %word_ptr %app %c0 %snapshot_index10
OpStore %snapshot_ptr10 %old_hi
%snapshot_index11 = OpIAdd %u32 %snapshot_base %c11
%snapshot_ptr11 = OpAccessChain %word_ptr %app %c0 %snapshot_index11
OpStore %snapshot_ptr11 %old_small_word
%snapshot_index12 = OpIAdd %u32 %snapshot_base %c12
%snapshot_ptr12 = OpAccessChain %word_ptr %app %c0 %snapshot_index12
OpStore %snapshot_ptr12 %old_flag_word
%new_sum = OpIAdd %u32 %sum %old_value
OpBranch %continue
%continue = OpLabel
%next = OpIAdd %u32 %i %c1
OpBranch %loop
%exit = OpLabel
%final = OpLoad %payload %data
%value = OpCompositeExtract %u32 %final 0
%flag = OpCompositeExtract %bool %final 1
%wide = OpCompositeExtract %u64 %final 2
%small = OpCompositeExtract %u16 %final 3
%vec = OpCompositeExtract %v3f %final 4
%flag_word = OpSelect %u32 %flag %c1 %c0
%wide_words = OpBitcast %v2u %wide
%lo = OpCompositeExtract %u32 %wide_words 0
%hi = OpCompositeExtract %u32 %wide_words 1
%small_word = OpUConvert %u32 %small
%vx = OpCompositeExtract %f32 %vec 0
%vy = OpCompositeExtract %f32 %vec 1
%vz = OpCompositeExtract %f32 %vec 2
%wx = OpBitcast %u32 %vx
%wy = OpBitcast %u32 %vy
%wz = OpBitcast %u32 %vz
%base = OpIMul %u32 %id %c16
%p0 = OpAccessChain %word_ptr %app %c0 %base
OpStore %p0 %value
%a1 = OpIAdd %u32 %base %c1
%p1 = OpAccessChain %word_ptr %app %c0 %a1
OpStore %p1 %flag_word
%a2 = OpIAdd %u32 %base %c2
%p2 = OpAccessChain %word_ptr %app %c0 %a2
OpStore %p2 %lo
%a3 = OpIAdd %u32 %base %c3
%p3 = OpAccessChain %word_ptr %app %c0 %a3
OpStore %p3 %hi
%a4 = OpIAdd %u32 %base %c4
%p4 = OpAccessChain %word_ptr %app %c0 %a4
OpStore %p4 %small_word
%a5 = OpIAdd %u32 %base %c5
%p5 = OpAccessChain %word_ptr %app %c0 %a5
OpStore %p5 %wx
%a6 = OpIAdd %u32 %base %c6
%p6 = OpAccessChain %word_ptr %app %c0 %a6
OpStore %p6 %wy
%a7 = OpIAdd %u32 %base %c7
%p7 = OpAccessChain %word_ptr %app %c0 %a7
OpStore %p7 %wz
%a8 = OpIAdd %u32 %base %c8
%p8 = OpAccessChain %word_ptr %app %c0 %a8
OpStore %p8 %sum
OpReturn
OpFunctionEnd
)spv");
}

Words Callee() {
    return Assemble(std::string(kPrefix) + R"spv(
OpEntryPoint CallableKHR %main "main" %launch %incoming
OpDecorate %launch BuiltIn LaunchIdKHR
OpDecorate %incoming Location 17
)spv" + kTypes + R"spv(
%incoming_ptr = OpTypePointer IncomingCallableDataKHR %payload
%incoming = OpVariable %incoming_ptr IncomingCallableDataKHR
%main = OpFunction %void None %fn
%entry = OpLabel
%lv = OpLoad %v3u %launch
%id = OpCompositeExtract %u32 %lv 0
%old = OpLoad %payload %incoming
%value = OpCompositeExtract %u32 %old 0
%flag = OpCompositeExtract %bool %old 1
%wide = OpCompositeExtract %u64 %old 2
%small = OpCompositeExtract %u16 %old 3
%vec = OpCompositeExtract %v3f %old 4
%delta = OpIAdd %u32 %id %c10
%new_value = OpIAdd %u32 %value %delta
%new_flag = OpLogicalNot %bool %flag
%new_wide = OpIAdd %u64 %wide %delta64
%new_small = OpIAdd %u16 %small %h3
%new_vec = OpFAdd %v3f %vec %vector_delta
%result = OpCompositeConstruct %payload %new_value %new_flag %new_wide %new_small %new_vec
OpStore %incoming %result
OpReturn
OpFunctionEnd
)spv");
}

Words StripNames(const Words &source) {
    Words result(source.begin(), source.begin() + 5);
    for (size_t p = 5; p < source.size(); p += source[p] >> 16) {
        auto op = spv::Op(source[p] & 0xffff);
        if (op != spv::OpName && op != spv::OpMemberName)
            result.insert(result.end(), source.begin() + p, source.begin() + p + (source[p] >> 16));
    }
    return result;
}

Words WithMode(const Words &source, uint32_t mode, uint32_t capability) {
    Words result(source.begin(), source.begin() + 5);
    bool inserted = false;
    for (size_t p = 5; p < source.size(); p += source[p] >> 16) {
        auto op = spv::Op(source[p] & 0xffff);
        if (!inserted && op != spv::OpCapability) {
            result.insert(result.end(), {(2u << 16) | spv::OpCapability, capability});
            inserted = true;
        }
        result.insert(result.end(), source.begin() + p, source.begin() + p + (source[p] >> 16));
        if (op == spv::OpEntryPoint) result.insert(result.end(), {(4u << 16) | spv::OpExecutionMode, source[p + 2], mode, 32});
    }
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    Require(tools.Validate(result), "FP-mode regression source is invalid");
    return result;
}

const TypedInvocationVariable &Invocation(const TypedContinuationProgram &program, uint32_t storage) {
    for (const auto &variable : program.invocation_variables)
        if (variable.storage_class == storage) return variable;
    throw std::runtime_error("Missing invocation backing");
}

uint64_t NamedConstant(const Words &source, const char *name) {
    uint32_t id = 0;
    for (size_t p = 5; p < source.size(); p += source[p] >> 16)
        if (spv::Op(source[p] & 0xffff) == spv::OpName && std::string(reinterpret_cast<const char *>(&source[p + 2])) == name)
            id = source[p + 1];
    Require(id != 0, std::string("Missing named constant ") + name);
    for (size_t p = 5; p < source.size(); p += source[p] >> 16) {
        auto op = spv::Op(source[p] & 0xffff);
        if ((op == spv::OpConstant || op == spv::OpConstantTrue || op == spv::OpConstantFalse) && source[p + 2] == id) {
            if (op == spv::OpConstantTrue) return 1;
            if (op == spv::OpConstantFalse) return 0;
            return source[p + 3] | ((source[p] >> 16) == 5 ? uint64_t(source[p + 4]) << 32 : 0);
        }
    }
    throw std::runtime_error(std::string("Specialization was not frozen: ") + name);
}

template <class F>
void Reject(F action, const char *reason) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception &) {
        rejected = true;
    }
    Require(rejected, std::string("Accepted invalid typed pipeline: ") + reason);
}

std::string Scheduler(const TypedContinuationProgram &caller, const TypedContinuationProgram &callee) {
    const auto &outgoing = Invocation(caller, spv::StorageClassCallableDataKHR);
    const auto &incoming = Invocation(callee, spv::StorageClassIncomingCallableDataKHR);
    Require(outgoing.size == incoming.size && outgoing.size % 4 == 0, "Typed payload ABI is incompatible");
    std::ostringstream text;
    text << R"glsl(#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(local_size_x=1) in;
layout(constant_id=0) const uint root_lo=0;
layout(constant_id=1) const uint root_hi=0;
layout(buffer_reference,std430,buffer_reference_align=4) buffer Words { uint w[]; };
void vel_rt_stage_0(uint64_t frame) {}
void vel_rt_stage_1(uint64_t frame) {}
uint64_t address(Words b,uint word) { return uint64_t(b.w[word]) | (uint64_t(b.w[word+1])<<32); }
void main() {
    Words root=Words(uint64_t(root_lo)|(uint64_t(root_hi)<<32));
    uint id=gl_GlobalInvocationID.x;
    uint count=root.w[6];
    if(id>=count) return;
)glsl";
    text << "const uint rc=" << caller.control_word << "u, cc=" << callee.control_word << "u;\n";
    text << "uint64_t ra=address(root,0u)+uint64_t(id)*" << caller.frame_stride_bytes + 16 << "ul;\n";
    text << "uint64_t ca=address(root,2u)+uint64_t(id)*" << callee.frame_stride_bytes + 16 << "ul;\n";
    text << "Words r=Words(ra), c=Words(ca), status=Words(address(root,4u)+uint64_t(id)*16ul);\n";
    text << "for(uint n=0;n<" << caller.frame_stride_words << "u;++n) r.w[n]=0u;\n";
    text << R"glsl(
    r.w[0]=id; r.w[3]=count; r.w[4]=1u; r.w[5]=1u;
    uint errors=0u;
    vel_rt_stage_0(ra);
    memoryBarrierBuffer();
    for(uint iteration=0;iteration<3u;++iteration) {
        if(r.w[rc+1u]!=3u) errors|=1u;
        if(r.w[rc+4u]!=id*3u+iteration) errors|=2u;
)glsl";
    text << "if(r.w[rc+20u]!=" << outgoing.offset << "u) errors|=4u;\n";
    text << "if(r.w[rc+21u]!=" << outgoing.size << "u || r.w[rc+22u]!=7u) errors|=8u;\n";
    text << "if(r.w[rc+23u]!=" << uint32_t(spv::StorageClassCallableDataKHR) << "u) errors|=16u;\n";
    text << R"glsl(
        uint pc=r.w[rc];
        // A pending stage called before Ready must perform no source work.
        vel_rt_stage_0(ra);
        memoryBarrierBuffer();
        if(r.w[rc]!=pc || r.w[rc+1u]!=3u || r.w[rc+2u]!=0u) errors|=32u;
)glsl";
    text << "for(uint n=0;n<" << callee.frame_stride_words << "u;++n) c.w[n]=0u;\n";
    text << "c.w[0]=id; c.w[3]=count; c.w[4]=1u; c.w[5]=1u;\n";
    text << "for(uint n=0;n<" << outgoing.size / 4 << "u;++n) c.w[" << incoming.offset / 4 << "u+n]=r.w[r.w[rc+20u]/4u+n];\n";
    text << R"glsl(
        memoryBarrierBuffer();
        vel_rt_stage_1(ca);
        memoryBarrierBuffer();
        if(c.w[cc]!=0xffffffffu || c.w[cc+1u]!=0u) errors|=64u;
)glsl";
    text << "for(uint n=0;n<" << outgoing.size / 4 << "u;++n) r.w[r.w[rc+20u]/4u+n]=c.w[" << incoming.offset / 4 << "u+n];\n";
    text << R"glsl(
        r.w[rc+2u]=1u;
        memoryBarrierBuffer();
        vel_rt_stage_0(ra);
        memoryBarrierBuffer();
    }
    if(r.w[rc]!=0xffffffffu || r.w[rc+1u]!=0u || r.w[rc+2u]!=0u) errors|=128u;
    status.w[0]=errors; status.w[1]=3u; status.w[2]=r.w[rc]; status.w[3]=r.w[rc+21u];
}
)glsl";
    return text.str();
}

void CheckNative(const Words &source) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    Require(tools.Validate(source), "Linked native module failed validation");
    std::map<uint32_t, uint32_t> bindings;
    std::map<uint32_t, uint32_t> specialization_ids;
    unsigned entries = 0;
    for (size_t p = 5; p < source.size(); p += source[p] >> 16) {
        auto op = spv::Op(source[p] & 0xffff);
        if (op == spv::OpCapability)
            Require(source[p + 1] != spv::CapabilityLinkage && source[p + 1] != spv::CapabilityRayTracingKHR &&
                        source[p + 1] != spv::CapabilityRayQueryKHR,
                    "Linkage/native RT capability leaked into compute module");
        Require(op != spv::OpTraceRayKHR && op != spv::OpExecuteCallableKHR && op != spv::OpReportIntersectionKHR &&
                    op != spv::OpTypeAccelerationStructureKHR && op != spv::OpConvertUToAccelerationStructureKHR,
                "Native RT instruction leaked into compute module");
        if (op == spv::OpEntryPoint) {
            ++entries;
            Require(source[p + 1] == spv::ExecutionModelGLCompute, "Non-compute native entry");
        }
        if (op == spv::OpExtension) {
            std::string extension(reinterpret_cast<const char *>(&source[p + 1]));
            Require(extension != "SPV_KHR_ray_tracing" && extension != "SPV_KHR_ray_query",
                    "Native RT extension leaked into compute");
        }
        if (op == spv::OpDecorate) {
            Require(source[p + 2] != spv::DecorationLinkageAttributes, "Unresolved stage linkage");
            if (source[p + 2] == spv::DecorationBinding) bindings[source[p + 1]] = source[p + 3];
            if (source[p + 2] == spv::DecorationSpecId) specialization_ids[source[p + 1]] = source[p + 3];
        }
    }
    Require(entries == 1, "Linked module must have only the scheduler compute entry");
    Require(bindings.size() == 1 && bindings.begin()->second == 0, "BDA transport consumed an extra descriptor binding");
    Require(specialization_ids.size() == 2, "Application specialization leaked into native scheduler");
    for (const auto &[id, value] : specialization_ids) {
        (void)id;
        Require(value <= 1, "Application specialization ID reached native scheduler");
    }
}

struct SpecData {
    VkBool32 flag;
    uint16_t small;
    uint64_t wide;
};
const SpecData provided = {VK_FALSE, 65530, UINT64_C(0xfffffff012345678)};
const std::array<VkSpecializationMapEntry, 3> spec_map = {{{11, offsetof(SpecData, flag), sizeof(provided.flag)},
                                                           {12, offsetof(SpecData, small), sizeof(provided.small)},
                                                           {13, offsetof(SpecData, wide), sizeof(provided.wide)}}};
const VkSpecializationInfo specialization = {uint32_t(spec_map.size()), spec_map.data(), sizeof(provided), &provided};

void CPUChecks(const TypedContinuationProgram &caller, const TypedContinuationProgram &callee) {
    Require(NamedConstant(caller.source, "flag_spec") == 1 && NamedConstant(caller.source, "small_spec") == 19 &&
                NamedConstant(caller.source, "wide_spec") == UINT64_C(0x100000007),
            "Default bool/u16/u64 specialization changed");
    auto specialized =
        CompileTypedPipelineStage(Caller(), "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, &specialization, "vel_rt_stage_0");
    Require(NamedConstant(specialized.source, "flag_spec") == 0 &&
                NamedConstant(specialized.source, "small_spec") == provided.small &&
                NamedConstant(specialized.source, "wide_spec") == provided.wide,
            "Provided bool/u16/u64 specialization changed");
    auto stripped =
        CompileTypedPipelineStage(StripNames(Caller()), "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr, "vel_rt_stage_0");
    Require(stripped.frame_stride_bytes == caller.frame_stride_bytes && stripped.state_count == caller.state_count &&
                Invocation(stripped, spv::StorageClassCallableDataKHR).size ==
                    Invocation(caller, spv::StorageClassCallableDataKHR).size,
            "Application debug names altered the typed ABI");
    auto scheduler = CompileGLSL(Scheduler(caller, callee), VK_SHADER_STAGE_COMPUTE_BIT);
    CheckNative(LinkTypedPipelineProgram(scheduler, {caller, callee}));
    CheckNative(LinkTypedPipelineProgram(scheduler, {stripped, callee}));
    auto preserve = CompileTypedPipelineStage(WithMode(Caller(), spv::ExecutionModeDenormPreserve, spv::CapabilityDenormPreserve),
                                              "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr, "vel_rt_stage_0");
    auto flush =
        CompileTypedPipelineStage(WithMode(Callee(), spv::ExecutionModeDenormFlushToZero, spv::CapabilityDenormFlushToZero), "main",
                                  VK_SHADER_STAGE_CALLABLE_BIT_KHR, nullptr, "vel_rt_stage_1");
    Reject([&] { LinkTypedPipelineProgram(scheduler, {preserve, flush}); }, "conflicting stage floating-point semantics");
    auto bad_map = spec_map;
    bad_map[1].size = 4;
    VkSpecializationInfo bad_specialization = specialization;
    bad_specialization.pMapEntries = bad_map.data();
    Reject(
        [&] { CompileTypedPipelineStage(Caller(), "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, &bad_specialization, "vel_rt_stage_0"); },
        "u16 specialization has wrong byte size");
    // An ID absent from the module is ignored, even when its byte count would
    // not fit a scalar that happens to exist in the application module.
    std::array<uint32_t, 3> ignored_data = {0, 1, 2};
    VkSpecializationMapEntry ignored_map{123456, 0, sizeof(ignored_data)};
    VkSpecializationInfo ignored{1, &ignored_map, sizeof(ignored_data), ignored_data.data()};
    auto ignored_program = CompileTypedPipelineStage(Caller(), "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, &ignored, "vel_rt_stage_0");
    Require(NamedConstant(ignored_program.source, "small_spec") == 19, "Unused specialization ID altered a stage");
    Reject([&] { LinkTypedPipelineProgram(scheduler, {caller, caller}); }, "duplicate exports");
    Reject(
        [&] {
            LinkTypedPipelineProgram(
                CompileGLSL("#version 460\nlayout(local_size_x=1)in;void main(){}", VK_SHADER_STAGE_COMPUTE_BIT), {caller});
        },
        "missing scheduler stub");
    Reject(
        [&] {
            LinkTypedPipelineProgram(CompileGLSL(R"glsl(#version 460
layout(local_size_x=1)in;
void vel_rt_stage_0(uint frame){}
void main(){vel_rt_stage_0(gl_GlobalInvocationID.x);}
)glsl",
                                                 VK_SHADER_STAGE_COMPUTE_BIT),
                                     {caller});
        },
        "wrong stub parameter");
    Reject(
        [&] {
            LinkTypedPipelineProgram(CompileGLSL(R"glsl(#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(local_size_x=1)in;
uint vel_rt_stage_0(uint64_t frame){return uint(frame);}
void main(){uint x=vel_rt_stage_0(uint64_t(gl_GlobalInvocationID.x));}
)glsl",
                                                 VK_SHADER_STAGE_COMPUTE_BIT),
                                     {caller});
        },
        "wrong stub return");
    Reject(
        [&] {
            LinkTypedPipelineProgram(CompileGLSL(R"glsl(#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(local_size_x=1)in;
void vel_rt_stage_0(uint64_t frame){frame+=1ul;}
void main(){vel_rt_stage_0(uint64_t(gl_GlobalInvocationID.x));}
)glsl",
                                                 VK_SHADER_STAGE_COMPUTE_BIT),
                                     {caller});
        },
        "nonempty scheduler stub");
    std::cout << "Typed stage specialization/name stripping, stub checks and final native module validated\n";
}

template <class T>
void Write(uint32_t *mapped, uint64_t offset, T value) {
    std::memcpy(reinterpret_cast<uint8_t *>(mapped) + offset, &value, sizeof(value));
}
uint64_t Align(uint64_t value) { return (value + 255) & ~UINT64_C(255); }
uint32_t FloatBits(float value) {
    uint32_t out;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

bool GPUExecution(const VkSpecializationInfo *info, bool strip_names) {
    constexpr uint32_t count = 7;
    auto source = strip_names ? StripNames(Caller()) : Caller();
    auto caller = CompileTypedPipelineStage(source, "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, info, "vel_rt_stage_0");
    auto callee =
        CompileTypedPipelineStage(StripNames(Callee()), "main", VK_SHADER_STAGE_CALLABLE_BIT_KHR, nullptr, "vel_rt_stage_1");
    auto linked = LinkTypedPipelineProgram(CompileGLSL(Scheduler(caller, callee), VK_SHADER_STAGE_COMPUTE_BIT), {caller, callee});
    CheckNative(linked);
    uint64_t caller_offset = 256, caller_stride = caller.frame_stride_bytes + 16;
    uint64_t callee_offset = Align(caller_offset + count * caller_stride), callee_stride = callee.frame_stride_bytes + 16;
    uint64_t status_offset = Align(callee_offset + count * callee_stride);
    uint64_t output_offset = Align(status_offset + count * 16), total_bytes = output_offset + count * 64 + 256;
    GPU gpu;
    if (!gpu.Init(linked, uint32_t(total_bytes), VK_WHOLE_SIZE, true, VK_WHOLE_SIZE, 0)) return false;
    Write(gpu.mapped, 0, gpu.address + caller_offset);
    Write(gpu.mapped, 8, gpu.address + callee_offset);
    Write(gpu.mapped, 16, gpu.address + status_offset);
    Write(gpu.mapped, 24, count);
    // Application set0/binding0 is entirely occupied by the source SSBO.
    // The scheduler and both frames use only BDA, with no hidden descriptor.
    VkDescriptorBufferInfo output{gpu.buffer, output_offset, count * 64};
    VkWriteDescriptorSet descriptor{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    descriptor.dstSet = gpu.set;
    descriptor.dstBinding = 0;
    descriptor.descriptorCount = 1;
    descriptor.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptor.pBufferInfo = &output;
    vkUpdateDescriptorSets(gpu.device, 1, &descriptor, 0, nullptr);
    std::memset(reinterpret_cast<uint8_t *>(gpu.mapped) + output_offset, 0, count * 64);
    // Exactly one dispatch: every request, frame copy, callee call and resume
    // happens on the GPU. The host reads only after the whole scheduler ends.
    gpu.Dispatch(0, 0, count + 3);
    const SpecData expected = info ? provided : SpecData{VK_TRUE, 19, UINT64_C(0x100000007)};
    for (uint32_t id = 0; id < count; ++id) {
        auto status = gpu.mapped + status_offset / 4 + id * 4;
        Require(status[0] == 0 && status[1] == 3 && status[2] == UINT32_MAX,
                "GPU scheduler request/return errors=" + std::to_string(status[0]));
        auto row = gpu.mapped + output_offset / 4 + id * 16;
        Require(row[0] == 35 + 3 * id && row[1] == !expected.flag, "Late callable scalar/bool writeback changed");
        uint64_t wide = expected.wide + UINT64_C(3) * UINT64_C(0x100000003);
        Require(row[2] == uint32_t(wide) && row[3] == uint32_t(wide >> 32), "Late u64 callable writeback changed");
        Require(row[4] == uint16_t(expected.small + 15), "Persistent caller payload alias lost across stages");
        Require(row[5] == FloatBits(2.5f) && row[6] == FloatBits(5.f) && row[7] == FloatBits(9.f), "Typed vec3 writeback changed");
        Require(row[8] == 45 + 3 * id, "Saved SSA snapshot reloaded late payload data");
        uint64_t snapshot = expected.wide + UINT64_C(2) * UINT64_C(0x100000003);
        Require(row[9] == uint32_t(snapshot) && row[10] == uint32_t(snapshot >> 32) && row[11] == uint16_t(expected.small + 10) &&
                    row[12] == uint32_t(expected.flag),
                "Aggregate SSA snapshot changed after callee writeback");
        Require(row[14] == 3, "Source SSBO side effect executed more than once per suspension");
        for (uint32_t n = 0; n < 4; ++n) {
            Require(gpu.mapped[(caller_offset + id * caller_stride + caller.frame_stride_bytes) / 4 + n] == 0xcdcdcdcd,
                    "RGEN frame guard overwritten");
            Require(gpu.mapped[(callee_offset + id * callee_stride + callee.frame_stride_bytes) / 4 + n] == 0xcdcdcdcd,
                    "Callable frame guard overwritten");
        }
    }
    std::cout << "One-dispatch GPU-linked RGEN/Callable: " << count * 3
              << " calls, specialization=" << (info ? "provided" : "default")
              << ", names=" << (strip_names ? "stripped" : "present") << " PASS\n";
    return true;
}
}  // namespace

int main() {
    try {
        auto caller = CompileTypedPipelineStage(Caller(), "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr, "vel_rt_stage_0");
        auto callee = CompileTypedPipelineStage(Callee(), "main", VK_SHADER_STAGE_CALLABLE_BIT_KHR, nullptr, "vel_rt_stage_1");
        CPUChecks(caller, callee);
        if (!GPUExecution(nullptr, false) || !GPUExecution(&specialization, true)) return 77;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
