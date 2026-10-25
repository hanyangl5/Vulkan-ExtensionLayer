// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "state_compiler.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv_cross/spirv.hpp>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace vel::rt;
void Require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) { Require(result == VK_SUCCESS, "Vulkan result " + std::to_string(result)); }
#include "frame_test_gpu.h"

const char *kPrefix = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability Int16
OpCapability Int64
OpCapability Float64
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
)spv";
const char *kTypes = R"spv(
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%u16 = OpTypeInt 16 0
%u64 = OpTypeInt 64 0
%f32 = OpTypeFloat 32
%f64 = OpTypeFloat 64
%v2f = OpTypeVector %f32 2
%v3f = OpTypeVector %f32 3
%v3u = OpTypeVector %u32 3
%input_ptr = OpTypePointer Input %v3u
%c0 = OpConstant %u32 0
%c1 = OpConstant %u32 1
%c2 = OpConstant %u32 2
%c3 = OpConstant %u32 3
%c4 = OpConstant %u32 4
%c5 = OpConstant %u32 5
%c10 = OpConstant %u32 10
%c16 = OpConstant %u32 16
%c17 = OpConstant %u32 17
%c20 = OpConstant %u32 20
%c100 = OpConstant %u32 100
%c511 = OpConstant %u32 511
%c65536 = OpConstant %u32 65536
%h1 = OpConstant %u16 1
%h2 = OpConstant %u16 2
%h3 = OpConstant %u16 3
%h4 = OpConstant %u16 4
%h5 = OpConstant %u16 5
%h6 = OpConstant %u16 6
%h7 = OpConstant %u16 7
%h9 = OpConstant %u16 9
%h10 = OpConstant %u16 10
%h20 = OpConstant %u16 20
%h30 = OpConstant %u16 30
%h40 = OpConstant %u16 40
%h50 = OpConstant %u16 50
%h60 = OpConstant %u16 60
%true = OpConstantTrue %bool
%false = OpConstantFalse %bool
%f0 = OpConstant %f32 0
%f1 = OpConstant %f32 1
%f2 = OpConstant %f32 2
%f3 = OpConstant %f32 3
%f4 = OpConstant %f32 4
%f6 = OpConstant %f32 6
%f7 = OpConstant %f32 7
%f8 = OpConstant %f32 8
%fhalf = OpConstant %f32 0.5
%fquarter = OpConstant %f32 0.25
%fneg2 = OpConstant %f32 -2
%f100 = OpConstant %f32 100
%d4 = OpConstant %f64 4.5
%d9 = OpConstant %f64 9.5
%d2 = OpConstant %f64 2
%array = OpTypeArray %u16 %c3
%payload_type = OpTypeStruct %bool %v3f %f64 %array %u32
%vec123 = OpConstantComposite %v3f %f1 %f2 %f3
%vec678 = OpConstantComposite %v3f %f6 %f7 %f8
%vec246 = OpConstantComposite %v3f %f2 %f4 %f6
%direction = OpConstantComposite %v3f %f0 %f0 %f1
%arr123 = OpConstantComposite %array %h10 %h20 %h30
%arr456 = OpConstantComposite %array %h40 %h50 %h60
%initial_payload = OpConstantComposite %payload_type %true %vec123 %d4 %arr123 %c0
%initial_callable = OpConstantComposite %payload_type %false %vec678 %d9 %arr456 %c100
%fn_void = OpTypeFunction %void
)spv";
std::string Caller() {
    return std::string(kPrefix) + R"spv(
OpEntryPoint RayGenerationKHR %main "main" %launch %config %scene %ray_data %call_data
OpName %ray_data "ray_data"
OpName %call_data "call_data"
OpName %actual_as "actual_as"
OpName %shadow "shadow"
OpName %result "result"
OpDecorate %launch BuiltIn LaunchIdKHR
OpDecorate %config_type Block
OpMemberDecorate %config_type 0 Offset 0
OpDecorate %scene DescriptorSet 0
OpDecorate %scene Binding 1
OpDecorate %ray_data Location 7
OpDecorate %call_data Location 13
)spv" + kTypes +
           R"spv(
%as_type = OpTypeAccelerationStructureKHR
%as_ptr = OpTypePointer UniformConstant %as_type
%ray_ptr = OpTypePointer RayPayloadKHR %payload_type
%call_ptr = OpTypePointer CallableDataKHR %payload_type
%ray_half_ptr = OpTypePointer RayPayloadKHR %u16
%call_half_ptr = OpTypePointer CallableDataKHR %u16
%local_payload_ptr = OpTypePointer Function %payload_type
%local_word = OpTypePointer Function %u32
%config_type = OpTypeStruct %u32
%config_ptr = OpTypePointer PushConstant %config_type
%config_word = OpTypePointer PushConstant %u32
%trace_fn = OpTypeFunction %void %as_type %u32 %u32
%call_fn = OpTypeFunction %void %u32
%launch = OpVariable %input_ptr Input
%config = OpVariable %config_ptr PushConstant
%scene = OpVariable %as_ptr UniformConstant
%ray_data = OpVariable %ray_ptr RayPayloadKHR
%call_data = OpVariable %call_ptr CallableDataKHR
%main = OpFunction %void None %fn_void
%entry = OpLabel
%shadow = OpVariable %local_payload_ptr Function
%result = OpVariable %local_word Function
%launch_value = OpLoad %v3u %launch
%id = OpCompositeExtract %u32 %launch_value 0
%index = OpUMod %u32 %id %c3
%index64 = OpUConvert %u64 %index
%index16 = OpUConvert %u16 %index
%ray_alias = OpAccessChain %ray_half_ptr %ray_data %c3 %index64
%call_alias = OpAccessChain %call_half_ptr %call_data %c3 %index16
%actual_as = OpLoad %as_type %scene
%initial_p = OpCompositeInsert %payload_type %id %initial_payload 4
%call_summary = OpIAdd %u32 %id %c100
%initial_c = OpCompositeInsert %payload_type %call_summary %initial_callable 4
OpStore %ray_data %initial_p
OpStore %call_data %initial_c
%limit_ptr = OpAccessChain %config_word %config %c0
%base_limit = OpLoad %u32 %limit_ptr
%limit = OpIAdd %u32 %base_limit %index
OpBranch %loop
%loop = OpLabel
%i = OpPhi %u32 %c0 %entry %next %continue
%sum = OpPhi %u32 %c0 %entry %next_sum %continue
%condition = OpULessThan %bool %i %limit
OpLoopMerge %exit %continue None
OpBranchConditional %condition %body %exit
%body = OpLabel
%snapshot = OpLoad %payload_type %ray_data
%trace_call = OpFunctionCall %void %trace_outer %actual_as %id %i
OpStore %shadow %snapshot
%r = OpLoad %u16 %ray_alias
%r_plus = OpIAdd %u16 %r %h7
OpStore %ray_alias %r_plus
%two_i = OpIMul %u32 %i %c2
%sbt_base = OpIAdd %u32 %id %c5
%sbt_index = OpIAdd %u32 %sbt_base %two_i
%call = OpFunctionCall %void %call_helper %sbt_index
%c = OpLoad %u16 %call_alias
%c_plus = OpIAdd %u16 %c %h9
OpStore %call_alias %c_plus
%r32 = OpUConvert %u32 %r_plus
%c32 = OpUConvert %u32 %c_plus
%old_summary = OpCompositeExtract %u32 %snapshot 4
%sum1 = OpIAdd %u32 %sum %r32
%sum2 = OpIAdd %u32 %sum1 %c32
%next_sum = OpIAdd %u32 %sum2 %old_summary
OpBranch %continue
%continue = OpLabel
%next = OpIAdd %u32 %i %c1
OpBranch %loop
%exit = OpLabel
OpStore %result %sum
OpReturn
OpFunctionEnd
%trace_outer = OpFunction %void None %trace_fn
%outer_as = OpFunctionParameter %as_type
%outer_id = OpFunctionParameter %u32
%outer_i = OpFunctionParameter %u32
%outer_entry = OpLabel
%inner_call = OpFunctionCall %void %trace_inner %outer_as %outer_id %outer_i
OpReturn
OpFunctionEnd
%trace_inner = OpFunction %void None %trace_fn
%inner_as = OpFunctionParameter %as_type
%inner_id = OpFunctionParameter %u32
%inner_i = OpFunctionParameter %u32
%inner_entry = OpLabel
%x = OpConvertUToF %f32 %inner_id
%y = OpConvertUToF %f32 %inner_i
%origin = OpCompositeConstruct %v3f %x %y %fneg2
%flags = OpBitwiseAnd %u32 %inner_id %c1
%offset = OpIAdd %u32 %inner_id %c16
%miss = OpIAdd %u32 %inner_id %c65536
%maximum = OpFAdd %f32 %y %f100
OpTraceRayKHR %inner_as %flags %c511 %offset %c17 %miss %origin %fquarter %direction %maximum %ray_data
OpReturn
OpFunctionEnd
%call_helper = OpFunction %void None %call_fn
%call_index = OpFunctionParameter %u32
%call_entry = OpLabel
OpExecuteCallableKHR %call_index %call_data
OpReturn
OpFunctionEnd
)spv";
}

std::string Callee(bool callable) {
    auto storage = callable ? "IncomingCallableDataKHR" : "IncomingRayPayloadKHR";
    std::string source(kPrefix);
    source += callable ? "OpEntryPoint CallableKHR %main \"main\" %launch %incoming\n"
                       : "OpEntryPoint ClosestHitKHR %main \"main\" %launch %incoming %attribute\n";
    source += "OpDecorate %launch BuiltIn LaunchIdKHR\nOpDecorate %incoming Location " + std::string(callable ? "23\n" : "17\n");
    source += kTypes;
    source += "%incoming_ptr = OpTypePointer " + std::string(storage) + " %payload_type\n";
    if (!callable) source += "%attr_ptr = OpTypePointer HitAttributeKHR %v2f\n%attribute = OpVariable %attr_ptr HitAttributeKHR\n";
    source += "%launch = OpVariable %input_ptr Input\n%incoming = OpVariable %incoming_ptr " + std::string(storage) + R"spv(
%main = OpFunction %void None %fn_void
%entry = OpLabel
%launch_value = OpLoad %v3u %launch
%id = OpCompositeExtract %u32 %launch_value 0
%old = OpLoad %payload_type %incoming
%old_bool = OpCompositeExtract %bool %old 0
%old_vec = OpCompositeExtract %v3f %old 1
%old_double = OpCompositeExtract %f64 %old 2
%old_arr = OpCompositeExtract %array %old 3
%old_summary = OpCompositeExtract %u32 %old 4
%a0 = OpCompositeExtract %u16 %old_arr 0
%a1 = OpCompositeExtract %u16 %old_arr 1
%a2 = OpCompositeExtract %u16 %old_arr 2
%new_bool = OpLogicalNot %bool %old_bool
)spv";
    if (callable)
        source += R"spv(
%new_vec = OpFAdd %v3f %old_vec %vec246
%new_double = OpFMul %f64 %old_double %d2
%n0 = OpIAdd %u16 %a0 %h4
%n1 = OpIAdd %u16 %a1 %h5
%n2 = OpIAdd %u16 %a2 %h6
%delta = OpIAdd %u32 %id %c20
)spv";
    else
        source += R"spv(
%attr = OpLoad %v2f %attribute
%attr_x = OpCompositeExtract %f32 %attr 0
%attr_y = OpCompositeExtract %f32 %attr 1
%vec_delta = OpCompositeConstruct %v3f %attr_x %f1 %f2
%new_vec = OpFAdd %v3f %old_vec %vec_delta
%double_delta = OpFConvert %f64 %attr_y
%new_double = OpFAdd %f64 %old_double %double_delta
%n0 = OpIAdd %u16 %a0 %h1
%n1 = OpIAdd %u16 %a1 %h2
%n2 = OpIAdd %u16 %a2 %h3
%delta = OpIAdd %u32 %id %c10
)spv";
    source += R"spv(
%new_arr = OpCompositeConstruct %array %n0 %n1 %n2
%new_summary = OpIAdd %u32 %old_summary %delta
%new = OpCompositeConstruct %payload_type %new_bool %new_vec %new_double %new_arr %new_summary
OpStore %incoming %new
OpReturn
OpFunctionEnd
)spv";
    return source;
}

std::vector<uint32_t> Assemble(const std::string &source) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string error;
    tools.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &p, const char *m) {
        error += std::to_string(p.index) + ": " + m + "\n";
    });
    std::vector<uint32_t> binary;
    bool success = tools.Assemble(source, &binary);
    Require(success, "Source assembly: " + error);
    success = tools.Validate(binary);
    Require(success, "Source validation: " + error);
    return binary;
}
std::map<std::string, uint32_t> Names(const std::vector<uint32_t> &source) {
    std::map<std::string, uint32_t> result;
    for (size_t p = 5; p < source.size(); p += source[p] >> 16)
        if ((source[p] & 0xffff) == spv::OpName) result[reinterpret_cast<const char *>(&source[p + 2])] = source[p + 1];
    return result;
}
const TypedInvocationVariable &Invocation(const TypedContinuationProgram &p, uint32_t storage) {
    for (const auto &v : p.invocation_variables)
        if (v.storage_class == storage) return v;
    throw std::runtime_error("Missing invocation variable");
}
const ContinuationFrameSlot &Slot(const TypedContinuationProgram &p, const char *name) {
    auto id = Names(p.source).at(name);
    for (const auto &s : p.frames.functions.at(0).slots)
        if (s.value == id) return s;
    throw std::runtime_error("Missing function frame slot");
}
template <class T>
T Read(const unsigned char *bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}
template <class T>
void Write(unsigned char *bytes, size_t offset, T value) {
    std::memcpy(bytes + offset, &value, sizeof(value));
}
float Float(uint32_t word) {
    float value;
    std::memcpy(&value, &word, 4);
    return value;
}
struct Payload {
    bool flag;
    std::array<float, 3> vector;
    double wide;
    std::array<uint16_t, 3> array;
    uint32_t summary;
};
void Verify(const unsigned char *bytes, const Payload &expected) {
    Require(Read<uint32_t>(bytes, 0) == uint32_t(expected.flag), "Typed bool changed");
    for (uint32_t n = 0; n < 3; ++n) {
        Require(Read<float>(bytes, 4 + n * 4) == expected.vector[n], "Typed vec3 changed");
        Require(Read<uint16_t>(bytes, 24 + n * 2) == expected.array[n], "Packed typed array/local alias changed");
    }
    Require(Read<double>(bytes, 16) == expected.wide && Read<uint32_t>(bytes, 32) == expected.summary,
            "Typed wide/summary changed");
}
void Update(Payload &payload, uint32_t id, bool callable) {
    payload.flag = !payload.flag;
    for (uint32_t n = 0; n < 3; ++n) {
        payload.vector[n] += callable ? float((n + 1) * 2) : n == 0 ? 0.5f : float(n);
        payload.array[n] += uint16_t((callable ? 4 : 1) + n);
    }
    payload.wide = callable ? payload.wide * 2 : payload.wide + 1.5;
    payload.summary += id + (callable ? 20 : 10);
}
unsigned char *Frame(GPU &gpu, const TypedContinuationProgram &p, uint32_t id) {
    return reinterpret_cast<unsigned char *>(gpu.mapped + id * p.frame_stride_words);
}
uint32_t *Control(GPU &gpu, const TypedContinuationProgram &p, uint32_t id) {
    return reinterpret_cast<uint32_t *>(Frame(gpu, p, id)) + p.control_word;
}
void Initialize(GPU &gpu, const TypedContinuationProgram &p, uint32_t count, uint32_t pc = 0) {
    for (uint32_t id = 0; id < count; ++id) {
        auto control = Control(gpu, p, id);
        std::fill_n(control, kTypedContinuationControlWords, 0);
        control[0] = pc;
    }
}

bool CrossStageExecution() {
    auto caller = CompileTypedContinuationSteps(Assemble(Caller()), "main", spv::ExecutionModelRayGenerationKHR);
    auto hit = CompileTypedContinuationSteps(Assemble(Callee(false)), "main", spv::ExecutionModelClosestHitKHR);
    auto callable = CompileTypedContinuationSteps(Assemble(Callee(true)), "main", spv::ExecutionModelCallableKHR);
    Require(caller.acceleration_structure_descriptors.size() == 1 && caller.acceleration_structure_descriptors[0].binding == 1,
            "AS descriptor value-cell remap lost its original binding");
    Require(caller.frames.options.acceleration_structure_addresses, "AS serialization was not an explicit backend policy");
    const auto &as_slot = Slot(caller, "actual_as");
    Require(as_slot.size == 8 && !as_slot.local_storage, "Live AS value was replayed instead of captured as uint64");
    const auto &ray = Invocation(caller, spv::StorageClassRayPayloadKHR);
    const auto &call = Invocation(caller, spv::StorageClassCallableDataKHR);
    const auto &incoming_hit = Invocation(hit, spv::StorageClassIncomingRayPayloadKHR);
    const auto &attribute = Invocation(hit, spv::StorageClassHitAttributeKHR);
    const auto &incoming_call = Invocation(callable, spv::StorageClassIncomingCallableDataKHR);
    Require(ray.location == 7 && call.location == 13 && incoming_hit.location == 17 && incoming_call.location == 23,
            "Nonzero/distinct outgoing and incoming locations were collapsed");
    Require(ray.size == 40 && call.size == 40 && incoming_hit.size == 40 && incoming_call.size == 40 && attribute.size == 8,
            "Typed invocation layout changed");
    constexpr uint32_t count = 8, iterations = 2;
    auto frame_bytes = caller.frame_stride_words * count * 4;
    auto cell_offset = (frame_bytes + 255u) & ~255u;
    GPU source_gpu, hit_gpu, call_gpu;
    if (!source_gpu.Init(caller.spirv, cell_offset + 8, cell_offset, true, frame_bytes) ||
        !hit_gpu.Init(hit.spirv, hit.frame_stride_words * count * 4) ||
        !call_gpu.Init(callable.spirv, callable.frame_stride_words * count * 4))
        return false;
    constexpr uint64_t address = 0x123456780000abcdULL, changed_address = 0xfedcba9811223344ULL;
    Write<uint64_t>(reinterpret_cast<unsigned char *>(source_gpu.mapped), cell_offset, address);
    Initialize(source_gpu, caller, count);
    source_gpu.Dispatch(iterations, 0, count);
    std::vector<uint32_t> blocked(source_gpu.mapped, source_gpu.mapped + cell_offset / 4 + 2);
    source_gpu.Dispatch(iterations, 0, count);
    Require(std::equal(blocked.begin(), blocked.end(), source_gpu.mapped), "Trace resumed before its callee completed");
    // A captured AS value is stable even if the descriptor cell changes after
    // that load. Future traces must consume saved SSA, not reload the cell.
    Write<uint64_t>(reinterpret_cast<unsigned char *>(source_gpu.mapped), cell_offset, changed_address);
    std::array<Payload, count> expected_ray, expected_call, expected_shadow;
    std::array<uint32_t, count> phase{}, checksum{};
    for (uint32_t id = 0; id < count; ++id) {
        expected_ray[id] = {true, {1, 2, 3}, 4.5, {10, 20, 30}, id};
        expected_call[id] = {false, {6, 7, 8}, 9.5, {40, 50, 60}, id + 100};
    }
    bool finished = false;
    uint32_t trace_count = 0, callable_count = 0;
    for (uint32_t round = 0; round < 128; ++round) {
        finished = true;
        Initialize(hit_gpu, hit, count, UINT32_MAX);
        Initialize(call_gpu, callable, count, UINT32_MAX);
        bool need_hit = false, need_call = false;
        std::array<uint32_t, count> requests{};
        for (uint32_t id = 0; id < count; ++id) {
            auto control = Control(source_gpu, caller, id);
            if (control[0] == UINT32_MAX) continue;
            finished = false;
            auto kind = control[1];
            Require(kind == 1 || kind == 3, "Unexpected source RT request");
            Require(kind == (phase[id] % 2 == 0 ? 1u : 3u) && control[2] == 0, "Wrong source request ordering");
            auto iteration = phase[id] / 2;
            const auto &data = kind == 1 ? ray : call;
            Require(control[20] == data.offset && control[21] == data.size && control[22] == data.location &&
                        control[23] == data.storage_class && control[24] == data.type && control[25] == data.variable,
                    "Request lost its exact typed invocation reference");
            auto payload = Frame(source_gpu, caller, id) + data.offset;
            if (kind == 1) {
                ++trace_count;
                uint64_t actual_address = uint64_t(control[4]) | (uint64_t(control[5]) << 32);
                Require(actual_address == address, "Trace AS SSA value reloaded/changed after suspension");
                Require(control[7] == (id & 1) && control[8] == 511 && control[9] == id + 16 && control[10] == 17 &&
                            control[11] == id + 65536,
                        "Trace flags/mask/SBT arguments changed");
                Require(Float(control[12]) == float(id) && Float(control[13]) == float(iteration) && Float(control[14]) == -2 &&
                            Float(control[15]) == 0.25f && Float(control[16]) == 0 && Float(control[17]) == 0 &&
                            Float(control[18]) == 1 && Float(control[19]) == float(iteration) + 100,
                        "Trace ray arguments changed");
                Verify(payload, expected_ray[id]);
                expected_shadow[id] = expected_ray[id];
                auto destination = Frame(hit_gpu, hit, id);
                std::memcpy(destination + incoming_hit.offset, payload, data.size);
                Write<float>(destination, attribute.offset, 0.5f);
                Write<float>(destination, attribute.offset + 4, 1.5f);
                Control(hit_gpu, hit, id)[0] = 0;
                need_hit = true;
            } else {
                ++callable_count;
                Require(control[4] == id + 5 + iteration * 2, "Callable SBT index changed");
                Verify(payload, expected_call[id]);
                auto destination = Frame(call_gpu, callable, id);
                std::memcpy(destination + incoming_call.offset, payload, data.size);
                Control(call_gpu, callable, id)[0] = 0;
                need_call = true;
            }
            requests[id] = kind;
        }
        if (finished) break;
        if (need_hit) hit_gpu.Dispatch(0, 0, count);
        if (need_call) call_gpu.Dispatch(0, 0, count);
        for (uint32_t id = 0; id < count; ++id) {
            auto kind = requests[id];
            if (!kind) continue;
            auto control = Control(source_gpu, caller, id);
            auto &expected = kind == 1 ? expected_ray[id] : expected_call[id];
            Update(expected, id, kind == 3);
            auto &child = kind == 1 ? hit : callable;
            auto &gpu = kind == 1 ? hit_gpu : call_gpu;
            const auto &incoming = kind == 1 ? incoming_hit : incoming_call;
            Require(Control(gpu, child, id)[0] == UINT32_MAX, "Non-suspending incoming stage did not complete");
            auto modified = Frame(gpu, child, id) + incoming.offset;
            Verify(modified, expected);
            std::memcpy(Frame(source_gpu, caller, id) + control[20], modified, control[21]);
            if (kind == 1)
                expected_ray[id].array[id % 3] += 7;
            else {
                expected_call[id].array[id % 3] += 9;
                checksum[id] += expected_ray[id].array[id % 3] + expected_call[id].array[id % 3] + expected_shadow[id].summary;
            }
            ++phase[id];
            control[2] = 1;
        }
        source_gpu.Dispatch(iterations, 0, count);
    }
    Require(finished, "Trace/callable source state machine did not terminate");
    const auto &shadow = Slot(caller, "shadow");
    const auto &result = Slot(caller, "result");
    for (uint32_t id = 0; id < count; ++id) {
        auto bytes = Frame(source_gpu, caller, id);
        Require(phase[id] == 2 * (iterations + id % 3), "Source skipped a Trace/Callable request");
        Verify(bytes + ray.offset, expected_ray[id]);
        Verify(bytes + call.offset, expected_call[id]);
        Verify(bytes + shadow.offset, expected_shadow[id]);
        Require(Read<uint32_t>(bytes, result.offset) == checksum[id], "32/64-bit typed alias/callee checksum mismatch");
    }
    Require(trace_count == 23 && callable_count == 23, "Unexpected cross-stage request totals");
    std::cout << "Real source RT requests executed: 23 TraceRay + 23 ExecuteCallable, nested helpers, captured AS cells, "
                 "typed outgoing/incoming storage at distinct locations, closest-hit attributes, late cross-stage modifications, "
                 "16/64-bit aliases\n";
    return true;
}
void Replace(std::string &source, const std::string &before, const std::string &after) {
    auto offset = source.find(before);
    Require(offset != std::string::npos, "Missing source regression fragment");
    source.replace(offset, before.size(), after);
}
bool AdditionalCompilation() {
    auto source = Caller();
    Replace(source, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability ShaderNonUniform");
    Replace(source, "OpDecorate %scene DescriptorSet", "OpDecorate %as_element NonUniform\nOpDecorate %scene DescriptorSet");
    Replace(source, "%as_ptr = OpTypePointer UniformConstant %as_type",
            "%as_ptr = OpTypePointer UniformConstant %as_type\n%as_array = OpTypeArray %as_type %c2\n%as_array_ptr = OpTypePointer "
            "UniformConstant %as_array");
    Replace(source, "%scene = OpVariable %as_ptr UniformConstant", "%scene = OpVariable %as_array_ptr UniformConstant");
    Replace(source, "%actual_as = OpLoad %as_type %scene",
            "%as_index = OpUMod %u32 %id %c2\n%as_element = OpAccessChain %as_ptr %scene %as_index\n%actual_as = OpLoad %as_type "
            "%as_element");
    auto array = CompileTypedContinuationSteps(Assemble(source), "main", spv::ExecutionModelRayGenerationKHR);
    Require(array.acceleration_structure_descriptors.size() == 1, "AS descriptor array was not retained as one binding");
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string text;
    Require(tools.Disassemble(array.spirv, &text), "AS-array disassembly failed");
    Require(
        text.find("StorageBufferArrayNonUniformIndexing") != std::string::npos && text.find("RayTracingKHR") == std::string::npos,
        "AS descriptor array retained native capability or lost translated nonuniform capability");

    source = Caller();
    Replace(source, "%launch = OpVariable", "%address = OpConstant %u64 1311768464867765197\n%launch = OpVariable");
    Replace(source, "%actual_as = OpLoad %as_type %scene", "%actual_as = OpConvertUToAccelerationStructureKHR %as_type %address");
    auto converted = CompileTypedContinuationSteps(Assemble(source), "main", spv::ExecutionModelRayGenerationKHR);
    Require(converted.acceleration_structure_descriptors.empty(), "Unused native descriptor acquired an unnecessary cell ABI");
    Require(tools.Disassemble(converted.spirv, &text), "Converted AS disassembly failed");
    Require(text.find("OpConvertUToAccelerationStructureKHR") == std::string::npos,
            "AS conversion retained a native opaque operation");
    auto captured = [&](const TypedContinuationProgram &program, uint64_t address, uint32_t expected_mask = 511) {
        GPU gpu;
        if (!gpu.Init(program.spirv, program.frame_stride_words * 4)) return false;
        Initialize(gpu, program, 1);
        gpu.Dispatch(2, 0, 1);
        uint32_t traces = 0;
        for (uint32_t request = 0; request < 8; ++request) {
            auto control = Control(gpu, program, 0);
            if (control[0] == UINT32_MAX) break;
            Require(control[1] == 1 || control[1] == 3, "Converted AS emitted an unexpected request");
            if (control[1] == 1) {
                ++traces;
                auto actual = uint64_t(control[4]) | (uint64_t(control[5]) << 32);
                Require(actual == address && control[7] == 0 && control[8] == expected_mask && control[9] == 16 &&
                            control[10] == 17 && control[11] == 65536,
                        "Converted AS or signed32 Trace argument bit transport changed");
            }
            control[2] = 1;
            gpu.Dispatch(2, 0, 1);
        }
        Require(Control(gpu, program, 0)[0] == UINT32_MAX && traces == 2,
                "Converted AS was not preserved across repeated suspensions");
        return true;
    };
    constexpr uint64_t address = 1311768464867765197ULL;
    if (!captured(converted, address)) return false;
    Replace(source, "%address = OpConstant %u64 1311768464867765197",
            "%v2u = OpTypeVector %u32 2\n%address_low = OpConstant %u32 43981\n%address_high = OpConstant %u32 305419896\n"
            "%address = OpConstantComposite %v2u %address_low %address_high");
    auto vector_address = CompileTypedContinuationSteps(Assemble(source), "main", spv::ExecutionModelRayGenerationKHR);
    if (!captured(vector_address, address)) return false;
    Replace(source, "%u32 = OpTypeInt 32 0", "%u32 = OpTypeInt 32 0\n%i32 = OpTypeInt 32 1");
    Replace(source, "%address = OpConstantComposite", "%negative_mask = OpConstant %i32 -1\n%address = OpConstantComposite");
    Replace(source, "OpTraceRayKHR %inner_as %flags %c511 %offset %c17 %miss",
            "%signed_flags = OpBitcast %i32 %flags\n%signed_mask = OpCopyObject %i32 %negative_mask\n%signed_offset = OpBitcast "
            "%i32 %offset\n"
            "%signed_stride = OpBitcast %i32 %c17\n%signed_miss = OpBitcast %i32 %miss\n"
            "OpTraceRayKHR %inner_as %signed_flags %signed_mask %signed_offset %signed_stride %signed_miss");
    auto signed_arguments = CompileTypedContinuationSteps(Assemble(source), "main", spv::ExecutionModelRayGenerationKHR);
    if (!captured(signed_arguments, address, UINT32_MAX)) return false;

    auto unused = Callee(false);
    unused.resize(unused.find("%main = OpFunction"));
    unused += "%main = OpFunction %void None %fn_void\n%entry = OpLabel\nOpReturn\nOpFunctionEnd\n";
    auto interfaces = CompileTypedContinuationSteps(Assemble(unused), "main", spv::ExecutionModelClosestHitKHR);
    Require(interfaces.invocation_variables.size() == 2 && Invocation(interfaces, spv::StorageClassHitAttributeKHR).size == 8 &&
                Invocation(interfaces, spv::StorageClassIncomingRayPayloadKHR).size == 40,
            "Unused selected-entry invocation interfaces were dropped");
    return true;
}

bool IntersectionAttributes() {
    auto source = std::string(kPrefix) + R"spv(
OpEntryPoint IntersectionKHR %main "main" %attribute %launch
OpName %result "result"
OpName %untouched "untouched"
OpDecorate %launch BuiltIn LaunchIdKHR
)spv" + kTypes + R"spv(
%attribute_ptr = OpTypePointer HitAttributeKHR %v2f
%attribute_float_ptr = OpTypePointer HitAttributeKHR %f32
%local_float_ptr = OpTypePointer Function %f32
%local_word_ptr = OpTypePointer Function %u32
%attribute = OpVariable %attribute_ptr HitAttributeKHR
%launch = OpVariable %input_ptr Input
%initial = OpConstantComposite %v2f %fquarter %fhalf
%main = OpFunction %void None %fn_void
%entry = OpLabel
%result = OpVariable %local_float_ptr Function
%untouched = OpVariable %local_word_ptr Function
%launch_value = OpLoad %v3u %launch
%id = OpCompositeExtract %u32 %launch_value 0
%index64 = OpUConvert %u64 %c1
%alias = OpAccessChain %attribute_float_ptr %attribute %index64
OpStore %attribute %initial
%fi = OpConvertUToF %f32 %id
%distance = OpFAdd %f32 %fi %fquarter
%kind = OpIAdd %u32 %id %c17
%accepted = OpReportIntersectionKHR %bool %distance %kind
%prior = OpLoad %f32 %alias
%delta = OpSelect %f32 %accepted %f1 %f2
%updated = OpFAdd %f32 %prior %delta
OpStore %alias %updated
OpStore %result %updated
OpReturn
OpFunctionEnd
)spv";
    auto program = CompileTypedContinuationSteps(Assemble(source), "main", spv::ExecutionModelIntersectionKHR);
    const auto &attribute = Invocation(program, spv::StorageClassHitAttributeKHR);
    constexpr uint32_t count = 4;
    GPU gpu;
    if (!gpu.Init(program.spirv, program.frame_stride_words * count * 4)) return false;
    Initialize(gpu, program, count);
    gpu.Dispatch(0, 0, count);
    std::vector<uint32_t> blocked(gpu.mapped, gpu.mapped + program.frame_stride_words * count);
    gpu.Dispatch(0, 0, count);
    Require(std::equal(blocked.begin(), blocked.end(), gpu.mapped), "Attribute producer resumed before Report result");
    for (uint32_t id = 0; id < count; ++id) {
        auto control = Control(gpu, program, id);
        Require(control[1] == 2 && Float(control[4]) == float(id) + 0.25f && control[5] == id + 17, "Report arguments changed");
        Require(control[20] == attribute.offset && control[21] == 8 && control[22] == UINT32_MAX &&
                    control[23] == spv::StorageClassHitAttributeKHR && control[25] == attribute.variable,
                "Report did not expose the exact hit-attribute backing");
        auto bytes = Frame(gpu, program, id);
        Require(Read<float>(bytes, attribute.offset) == 0.25f && Read<float>(bytes, attribute.offset + 4) == 0.5f,
                "Intersection attribute stores did not precede Report");
        control[3] = id & 1;
        control[2] = 1;
    }
    gpu.Dispatch(0, 0, count);
    for (uint32_t id = 0; id < count; ++id) {
        auto bytes = Frame(gpu, program, id);
        auto expected = id & 1 ? 1.5f : 2.5f;
        Require(Control(gpu, program, id)[0] == UINT32_MAX && Read<float>(bytes, attribute.offset + 4) == expected &&
                    Read<float>(bytes, Slot(program, "result").offset) == expected,
                "Late Report bool/attribute alias was lost");
        Require(Read<uint32_t>(bytes, Slot(program, "untouched").offset) == 0xcdcdcdcd,
                "Unused local was read at attribute suspension");
    }
    return true;
}

bool PlainVec3Callable() {
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint CallableKHR %main "main" %incoming
OpDecorate %incoming Location 31
%void = OpTypeVoid
%float = OpTypeFloat 32
%vec3 = OpTypeVector %float 3
%pointer = OpTypePointer IncomingCallableDataKHR %vec3
%incoming = OpVariable %pointer IncomingCallableDataKHR
%one = OpConstant %float 1
%two = OpConstant %float 2
%three = OpConstant %float 3
%delta = OpConstantComposite %vec3 %one %two %three
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%value = OpLoad %vec3 %incoming
%result = OpFAdd %vec3 %value %delta
OpStore %incoming %result
OpReturn
OpFunctionEnd
)spv";
    auto program = CompileTypedContinuationSteps(Assemble(source), "main", spv::ExecutionModelCallableKHR);
    const auto &data = Invocation(program, spv::StorageClassIncomingCallableDataKHR);
    Require(data.location == 31 && data.size == 12 && program.frames.functions[0].suspensions.empty(),
            "Plain vec3 incoming ABI changed");
    GPU gpu;
    if (!gpu.Init(program.spirv, program.frame_stride_words * 4, VK_WHOLE_SIZE, false)) return false;
    Initialize(gpu, program, 1);
    auto bytes = Frame(gpu, program, 0);
    for (uint32_t n = 0; n < 3; ++n) Write<float>(bytes, data.offset + n * 4, float(n + 5));
    gpu.Dispatch(0, 0, 1);
    Require(Control(gpu, program, 0)[0] == UINT32_MAX, "Non-suspending vec3 shader did not complete");
    for (uint32_t n = 0; n < 3; ++n)
        Require(Read<float>(bytes, data.offset + n * 4) == float(n * 2 + 6), "Vec3 input/callee modification changed");
    return true;
}
}  // namespace

int main() {
    try {
        if (!AdditionalCompilation() || !CrossStageExecution() || !IntersectionAttributes() || !PlainVec3Callable()) {
            std::cout << "Typed RT request SPIR-V validated; Vulkan execution unavailable\n";
            return 77;
        }
        std::cout
            << "Intersection hit-attribute Report backing and non-suspending vec3 callable (no source uint32 type) executed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
