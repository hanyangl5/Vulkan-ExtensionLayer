// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "frame_emitter.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using namespace vel::rt;
void Require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) { Require(result == VK_SUCCESS, "Vulkan result " + std::to_string(result)); }
std::vector<uint32_t> Assemble(const std::string &source) {
    std::string error;
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    tools.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &p, const char *message) {
        error += std::to_string(p.index) + ": " + message + "\n";
    });
    std::vector<uint32_t> binary;
    auto assembled = tools.Assemble(source, &binary);
    Require(assembled, "Assembly failed: " + error);
    auto validated = tools.Validate(binary);
    Require(validated, "Validation failed: " + error);
    return binary;
}
void ValidateComponent(const ContinuationFrameCodecs &code) {
    Assemble(code.capabilities + "OpMemoryModel Logical GLSL450\nOpEntryPoint GLCompute %probe \"probe\" " + code.memory +
             "\nOpExecutionMode %probe LocalSize 1 1 1\n" + code.annotations + code.declarations + "%probe_sig = OpTypeFunction " +
             code.void_type + "\n" + code.functions + "%probe = OpFunction " + code.void_type +
             " None %probe_sig\n%probe_entry = OpLabel\nOpReturn\nOpFunctionEnd\n");
}
void RejectionsAndEmptySpills() {
    auto array_fixture = [](uint64_t count) {
        return Assemble(std::string(R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability Int64
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint IntersectionKHR %main "main"
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%u64 = OpTypeInt 64 0
%f32 = OpTypeFloat 32
%kind = OpConstant %u32 0
%distance = OpConstant %f32 1
%count = OpConstant %u64 )spv") +
                        std::to_string(count) + R"spv(
%array = OpTypeArray %u32 %count
%ptr = OpTypePointer Function %array
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%uninitialized = OpVariable %ptr Function
%result = OpReportIntersectionKHR %bool %distance %kind
OpReturn
OpFunctionEnd
)spv");
    };
    const auto small = array_fixture(3);
    const auto code = EmitContinuationFrameCodecs(small, "main", spv::ExecutionModelIntersectionKHR, 2, 7, "probe_");
    Require(code.suspensions.size() == 1 && code.suspensions[0].saved_values.empty() &&
                code.suspensions[0].restored_type == code.void_type,
            "Local-only suspension speculatively copied Function storage");
    ValidateComponent(code);
    auto rejected = [&](const std::vector<uint32_t> &input, const std::string &prefix, const std::string &diagnostic) {
        try {
            EmitContinuationFrameCodecs(input, "main", spv::ExecutionModelIntersectionKHR, 0, 0, prefix);
        } catch (const std::exception &error) {
            Require(std::string(error.what()).find(diagnostic) != std::string::npos,
                    "Wrong codec rejection: " + std::string(error.what()));
            return;
        }
        throw std::runtime_error("Frame emitter accepted unsupported input");
    };
    rejected(small, "bad prefix", "ID prefix");
    rejected(array_fixture(4097), "probe_", "4096 elements");
    rejected(array_fixture(1073741825ULL), "probe_", "smaller than 4 GiB");
}

// A real RT entry with live aggregate SSA, full-lifetime local backing, a
// dynamic access-chain alias, and a bool produced only upon suspension return.
// It is analyzed, never executed; the component's compute proof follows below.
const char *kSource = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability Int8
OpCapability Int16
OpCapability Int64
OpCapability Float16
OpCapability Float64
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint IntersectionKHR %main "main"
OpName %payload "payload"
OpName %local "local"
OpName %untouched "untouched"
OpName %index "index"
OpName %iteration "iteration"
OpName %accepted "accepted"
OpName %alias "alias"
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%i8 = OpTypeInt 8 1
%u8 = OpTypeInt 8 0
%u16 = OpTypeInt 16 0
%i64 = OpTypeInt 64 1
%f16 = OpTypeFloat 16
%f32 = OpTypeFloat 32
%f64 = OpTypeFloat 64
%v3 = OpTypeVector %f32 3
%mat = OpTypeMatrix %v3 2
%c0 = OpConstant %u32 0
%c1 = OpConstant %u32 1
%c2 = OpConstant %u32 2
%c3 = OpConstant %u32 3
%c6 = OpConstant %u32 6
%c5 = OpConstant %u32 5
%c7 = OpConstant %u32 7
%c11 = OpConstant %u32 11
%array = OpTypeArray %u32 %c3
%narrow = OpTypeStruct %i8 %u8 %u16 %f16
%aggregate = OpTypeStruct %bool %v3 %f64 %narrow %i64 %mat %array
%ptr = OpTypePointer Function %aggregate
%uptr = OpTypePointer Function %u32
%true = OpConstantTrue %bool
%a = OpConstant %i8 -7
%b = OpConstant %u8 227
%c = OpConstant %u16 53001
%h = OpConstant %f16 1.5
%f = OpConstant %f32 2.5
%d = OpConstant %f64 -9.25
%wide = OpConstant %i64 -4294967303
%vec = OpConstantComposite %v3 %f %f %f
%matrix = OpConstantComposite %mat %vec %vec
%packed = OpConstantComposite %narrow %a %b %c %h
%array_value = OpConstantComposite %array %c5 %c7 %c11
%value = OpConstantComposite %aggregate %true %vec %d %packed %wide %matrix %array_value
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%local = OpVariable %ptr Function
%untouched = OpVariable %uptr Function
%payload = OpCopyObject %aggregate %value
%index = OpCopyObject %u32 %c1
%iteration = OpCopyObject %u32 %c0
OpStore %local %payload
%alias = OpAccessChain %uptr %local %c6 %index
%accepted = OpReportIntersectionKHR %bool %f %c1
%prior = OpLoad %u32 %alias
%adjusted = OpSelect %u32 %accepted %prior %iteration
OpStore %alias %adjusted
%read_payload = OpCompositeExtract %u32 %payload 6 0
OpStore %untouched %read_payload
OpReturn
OpFunctionEnd
)spv";

struct Proof {
    ContinuationFrameCodecs code;
    std::map<std::string, uint32_t> names;
    std::map<uint32_t, ContinuationTypeCodec> codecs;
    std::map<std::string, ContinuationFrameSlot> slots;
    uint32_t words = 0;
    uint32_t control = 0;
    Proof() {
        auto input = Assemble(kSource);
        for (size_t p = 5; p < input.size(); p += input[p] >> 16) {
            if ((input[p] & 0xffff) == spv::OpName) names[reinterpret_cast<const char *>(&input[p + 2])] = input[p + 1];
        }
        code = EmitContinuationFrameCodecs(input, "main", spv::ExecutionModelIntersectionKHR, 0, 0);
        for (const auto &codec : code.types) codecs.emplace(codec.type, codec);
        const auto &frame = code.plan.functions.at(0);
        for (const auto &slot : frame.slots) {
            for (const auto &[name, id] : names) {
                if (id == slot.value) slots[name] = slot;
            }
        }
        Require(slots.count("payload") && slots.count("local") && slots.count("untouched") && slots.count("index") &&
                    slots.count("iteration"),
                "Planner omitted live values or full-lifetime local storage");
        Require(!slots.count("accepted"), "Suspension result was spilled before it exists");
        Require(slots.at("local").local_storage && slots.at("untouched").local_storage, "Local storage was made an SSA snapshot");
        Require(frame.suspensions.size() == 1 && frame.suspensions[0].rematerializations.size() == 1,
                "Dynamic local alias rematerialization missing");
        Require(frame.suspensions[0].rematerializations[0].value == names.at("alias"), "Wrong alias provenance");
        Require(code.suspensions.size() == 1 && code.suspensions[0].saved_values.size() == 3,
                "Suspension emitter copied local storage or omitted typed SSA");
        control = static_cast<uint32_t>((frame.size + 3) / 4);
        // PC, suspension result, checksum, failure mask, then an independently
        // observable copy that forces every restored aggregate component to be
        // consumed, including wide/narrow values that the loop does not use.
        words = control + 4 + static_cast<uint32_t>(slots.at("payload").size / 4);
    }
    const ContinuationTypeLayout &Layout(uint32_t type) const {
        for (const auto &layout : code.plan.types) {
            if (layout.type == type) return layout;
        }
        throw std::runtime_error("Missing layout");
    }
    std::string Program() const {
        const auto &aggregate = codecs.at(slots.at("payload").type);
        const auto &uint = codecs.at(slots.at("index").type);
        const auto &site = code.suspensions.at(0);
        auto field = [&](const std::string &name) {
            auto where = std::find(site.saved_values.begin(), site.saved_values.end(), names.at(name));
            Require(where != site.saved_values.end(), "Missing restored field " + name);
            return std::to_string(where - site.saved_values.begin());
        };
        std::string spill = "%spill = OpFunctionCall " + code.void_type + " " + site.spill + " %base";
        for (auto value : site.saved_values) {
            if (value == names.at("payload"))
                spill += " %aggregate_value";
            else if (value == names.at("index"))
                spill += " %index_value";
            else if (value == names.at("iteration"))
                spill += " %zero";
            else
                throw std::runtime_error("Unexpected suspension spill field");
        }
        // Find the constituent symbols through the typed aggregate's emitted
        // declaration, avoiding assumptions about binary type ID allocation.
        auto type_line = code.declarations.find(aggregate.spirv_type + " = OpTypeStruct ");
        std::istringstream member_line(code.declarations.substr(type_line));
        std::string ignore, bool_type, vec, dbl, narrow, wide, matrix, array;
        member_line >> ignore >> ignore >> ignore >> bool_type >> vec >> dbl >> narrow >> wide >> matrix >> array;
        auto narrow_line = code.declarations.find(narrow + " = OpTypeStruct ");
        std::istringstream narrow_members(code.declarations.substr(narrow_line));
        std::string i8, u8, u16, half;
        narrow_members >> ignore >> ignore >> ignore >> i8 >> u8 >> u16 >> half;
        auto vector_line = code.declarations.find(vec + " = OpTypeVector ");
        std::istringstream vector_members(code.declarations.substr(vector_line));
        std::string flt;
        vector_members >> ignore >> ignore >> ignore >> flt;
        const auto &layout = Layout(slots.at("payload").type);
        const auto array_offset = layout.member_offsets.at(6);
        std::ostringstream s;
        s << code.capabilities << "OpMemoryModel Logical GLSL450\n"
          << "OpEntryPoint GLCompute %main \"main\" " << code.memory << " %gid %push\n"
          << "OpExecutionMode %main LocalSize 1 1 1\n"
          << code.annotations << "OpDecorate %gid BuiltIn GlobalInvocationId\n"
          << "OpDecorate %push_type Block\nOpMemberDecorate %push_type 0 Offset 0\n"
          << "OpMemberDecorate %push_type 1 Offset 4\n"
          << code.declarations << "%gid_type = OpTypeVector " << uint.spirv_type << " 3\n"
          << "%gid_ptr = OpTypePointer Input %gid_type\n%gid = OpVariable %gid_ptr Input\n"
          << "%push_type = OpTypeStruct " << uint.spirv_type << " " << uint.spirv_type << "\n"
          << "%push_ptr = OpTypePointer PushConstant %push_type\n%push = OpVariable %push_ptr PushConstant\n"
          << "%push_word = OpTypePointer PushConstant " << uint.spirv_type << "\n"
          << "%main_sig = OpTypeFunction " << code.void_type << "\n";
        auto constant = [&](const std::string &name, uint64_t value) {
            s << "%" << name << " = OpConstant " << uint.spirv_type << " " << value << "\n";
        };
        constant("zero", 0);
        constant("one", 1);
        constant("two", 2);
        constant("three", 3);
        constant("four", 4);
        constant("five", 5);
        constant("seven", 7);
        constant("eleven", 11);
        constant("stride", words);
        constant("pc_offset", control * 4);
        constant("return_offset", (control + 1) * 4);
        constant("checksum_offset", (control + 2) * 4);
        constant("failure_offset", (control + 3) * 4);
        constant("copy_offset", (control + 4) * 4);
        constant("alias_offset", slots.at("local").offset + array_offset);
        constant("array_relative", array_offset);
        for (const auto &[name, slot] : slots) constant("slot_" + name, slot.offset);
        s << "%true = OpConstantTrue " << bool_type << "\n"
          << "%i8_value = OpConstant " << i8 << " -7\n%u8_value = OpConstant " << u8 << " 227\n"
          << "%u16_value = OpConstant " << u16 << " 53001\n%half_value = OpConstant " << half << " 1.5\n"
          << "%double_value = OpConstant " << dbl << " -9.25\n%wide_value = OpConstant " << wide << " -4294967303\n"
          << "%float_value = OpConstant " << flt << " 2.5\n"
          << "%vec_value = OpConstantComposite " << vec << " %float_value %float_value %float_value\n"
          << "%m1 = OpConstant " << flt << " 1.25\n%m2 = OpConstant " << flt << " 2.25\n"
          << "%m3 = OpConstant " << flt << " 3.25\n%m4 = OpConstant " << flt << " 4.25\n"
          << "%m5 = OpConstant " << flt << " 5.25\n%m6 = OpConstant " << flt << " 6.25\n"
          << "%column0 = OpConstantComposite " << vec << " %m1 %m2 %m3\n"
          << "%column1 = OpConstantComposite " << vec << " %m4 %m5 %m6\n"
          << "%matrix_value = OpConstantComposite " << matrix << " %column0 %column1\n"
          << "%narrow_value = OpConstantComposite " << narrow << " %i8_value %u8_value %u16_value %half_value\n"
          << "%array_value = OpConstantComposite " << array << " %five %seven %eleven\n"
          << "%aggregate_value = OpConstantComposite " << aggregate.spirv_type
          << " %true %vec_value %double_value %narrow_value %wide_value %matrix_value %array_value\n"
          << code.functions << "%main = OpFunction " << code.void_type << " None %main_sig\n%entry = OpLabel\n"
          << "%gid_value = OpLoad %gid_type %gid\n%id = OpCompositeExtract " << uint.spirv_type << " %gid_value 0\n"
          << "%base = OpIMul " << uint.spirv_type << " %id %stride\n"
          << "%phase_ptr = OpAccessChain %push_word %push %zero\n%phase = OpLoad " << uint.spirv_type << " %phase_ptr\n"
          << "%is_prepare = OpIEqual " << bool_type << " %phase %zero\n"
          << "OpSelectionMerge %done None\nOpBranchConditional %is_prepare %prepare %resume\n%prepare = OpLabel\n"
          << "%store_local = OpFunctionCall " << code.void_type << " " << aggregate.store << " %base %slot_local %aggregate_value\n"
          << "%index_value = OpUMod " << uint.spirv_type << " %id %three\n"
          << spill << "\n"
          << "%save_pc = OpFunctionCall " << code.void_type << " " << uint.store << " %base %pc_offset %one\n"
          << "OpBranch %done\n%resume = OpLabel\n"
          << "%pc = OpFunctionCall " << uint.spirv_type << " " << uint.load << " %base %pc_offset\n"
          << "%ready = OpIEqual " << bool_type << " %pc %one\n"
          << "OpSelectionMerge %resume_done None\nOpBranchConditional %ready %restore %resume_done\n%restore = OpLabel\n"
          << "%restored_values = OpFunctionCall " << site.restored_type << " " << site.restore << " %base\n"
          << "%restored = OpCompositeExtract " << aggregate.spirv_type << " %restored_values " << field("payload") << "\n"
          << "%copy_restored = OpFunctionCall " << code.void_type << " " << aggregate.store << " %base %copy_offset %restored\n"
          << "%restored_index = OpCompositeExtract " << uint.spirv_type << " %restored_values " << field("index") << "\n"
          << "%iteration = OpCompositeExtract " << uint.spirv_type << " %restored_values " << field("iteration") << "\n"
          << "%accepted = OpFunctionCall " << uint.spirv_type << " " << uint.load << " %base %return_offset\n"
          << "%limit_ptr = OpAccessChain %push_word %push %one\n%limit = OpLoad " << uint.spirv_type << " %limit_ptr\n"
          << "%index_delta = OpIMul " << uint.spirv_type << " %restored_index %four\n"
          << "%alias = OpIAdd " << uint.spirv_type << " %alias_offset %index_delta\n"
          << "%initial = OpFunctionCall " << uint.spirv_type << " " << uint.load << " %base %alias\n"
          << "OpBranch %loop\n%loop = OpLabel\n"
          << "%i = OpPhi " << uint.spirv_type << " %iteration %restore %next %body\n"
          << "%sum = OpPhi " << uint.spirv_type << " %zero %restore %sum_next %body\n"
          << "%continue = OpULessThan " << bool_type << " %i %limit\n"
          << "OpLoopMerge %loop_done %body None\nOpBranchConditional %continue %body %loop_done\n%body = OpLabel\n"
          << "%old = OpFunctionCall " << uint.spirv_type << " " << uint.load << " %base %alias\n"
          << "%increased = OpIAdd " << uint.spirv_type << " %old %one\n"
          << "%write_alias = OpFunctionCall " << code.void_type << " " << uint.store << " %base %alias %increased\n"
          << "%sum_next = OpIAdd " << uint.spirv_type << " %sum %increased\n"
          << "%next = OpIAdd " << uint.spirv_type << " %i %one\nOpBranch %loop\n%loop_done = OpLabel\n"
          << "%local_after = OpFunctionCall " << aggregate.spirv_type << " " << aggregate.load << " %base %slot_local\n"
          << "%loaded_array = OpCompositeExtract " << array << " %local_after 6\n"
          << "%expected = OpIAdd " << uint.spirv_type << " %initial %limit\n"
          << "%ssa_first = OpCompositeExtract " << uint.spirv_type << " %restored 6 0\n"
          << "%ssa_same = OpIEqual " << bool_type << " %ssa_first %five\n"
          << "%failure = OpSelect " << uint.spirv_type << " %ssa_same %zero %one\n"
          << "%with_return = OpIAdd " << uint.spirv_type << " %sum %accepted\n"
          << "%checksum = OpFunctionCall " << code.void_type << " " << uint.store << " %base %checksum_offset %with_return\n"
          << "%failures = OpFunctionCall " << code.void_type << " " << uint.store << " %base %failure_offset %failure\n"
          << "%save_progress = OpFunctionCall " << code.void_type << " " << uint.store << " %base %slot_iteration %i\n"
          << "%save_done = OpFunctionCall " << code.void_type << " " << uint.store << " %base %pc_offset %two\n"
          << "OpBranch %resume_done\n%resume_done = OpLabel\nOpBranch %done\n%done = OpLabel\nOpReturn\nOpFunctionEnd\n";
        return s.str();
    }
};

#include "frame_test_gpu.h"

template <typename T>
void Expect(const unsigned char *bytes, uint64_t offset, T expected, const char *message) {
    T actual;
    std::memcpy(&actual, bytes + offset, sizeof(T));
    Require(actual == expected, message);
}
void VerifyAggregate(const Proof &proof, const unsigned char *bytes, bool modified, uint32_t index, uint32_t limit) {
    const auto &layout = proof.Layout(proof.slots.at("payload").type);
    Require(layout.member_offsets == std::vector<uint64_t>({0, 4, 16, 24, 32, 40, 64}) && layout.size == 80,
            "Scalar-packed aggregate ABI changed");
    Expect<uint32_t>(bytes, 0, 1, "Bool not normalized");
    for (uint32_t i = 0; i < 3; ++i) Expect<float>(bytes, 4 + i * 4, 2.5f, "vec3 layout/roundtrip mismatch");
    Expect<double>(bytes, 16, -9.25, "double roundtrip mismatch");
    Expect<int8_t>(bytes, 24, -7, "signed byte roundtrip mismatch");
    Expect<uint8_t>(bytes, 25, 227, "neighbor byte overwritten");
    Expect<uint16_t>(bytes, 26, 53001, "uint16 roundtrip mismatch");
    Expect<uint16_t>(bytes, 28, 0x3e00, "float16 roundtrip mismatch");
    Expect<uint16_t>(bytes, 30, 0xcdcd, "Narrow store overwrote aggregate padding");
    Expect<int64_t>(bytes, 32, -4294967303LL, "int64 little-endian roundtrip mismatch");
    for (uint32_t i = 0; i < 6; ++i) Expect<float>(bytes, 40 + i * 4, 1.25f + i, "matrix layout/roundtrip mismatch");
    const uint32_t initial[] = {5, 7, 11};
    for (uint32_t i = 0; i < 3; ++i)
        Expect<uint32_t>(bytes, 64 + i * 4, initial[i] + (modified && i == index ? limit : 0),
                         "Persistent local alias update mismatch");
    Expect<uint32_t>(bytes, 76, 0xcdcdcdcd, "Aggregate tail padding overwritten");
}
}  // namespace

int main() {
    try {
        RejectionsAndEmptySpills();
        Proof proof;
        auto binary = Assemble(proof.Program());
        Require(proof.code.capabilities.find("RayTracing") == std::string::npos, "Codec retained RT capability");
        Require(proof.code.functions.find("OpBitFieldInsert") != std::string::npos, "Packed scalar store emission missing");
        constexpr uint32_t count = 64;
        constexpr uint32_t limit = 257;  // Runtime push value, no static unroll.
        GPU gpu;
        if (!gpu.Init(binary, proof.words * count * 4)) {
            std::cout << "Typed codec SPIR-V validated; compute execution skipped: required Vulkan scalar features unavailable\n";
            return 77;
        }
        gpu.Dispatch(0, limit, count);
        for (uint32_t i = 0; i < count; ++i) {
            auto frame = gpu.mapped + i * proof.words;
            auto bytes = reinterpret_cast<unsigned char *>(frame);
            VerifyAggregate(proof, bytes + proof.slots.at("payload").offset, false, i % 3, limit);
            VerifyAggregate(proof, bytes + proof.slots.at("local").offset, false, i % 3, limit);
            Expect<uint32_t>(bytes, proof.slots.at("untouched").offset, 0xcdcdcdcd,
                             "Uninitialized local was read/spilled at suspension");
            Require(frame[proof.control] == 1 && frame[proof.control + 1] == 0xcdcdcdcd,
                    "Suspension return materialized too early");
            // The child operation resolves now, after the producer dispatch
            // has returned. It supplies a result independently of saved SSA.
            frame[proof.control + 1] = i % 2;
        }
        gpu.Dispatch(1, limit, count);
        for (uint32_t i = 0; i < count; ++i) {
            auto frame = gpu.mapped + i * proof.words;
            auto bytes = reinterpret_cast<unsigned char *>(frame);
            VerifyAggregate(proof, bytes + proof.slots.at("payload").offset, false, i % 3, limit);
            VerifyAggregate(proof, bytes + proof.slots.at("local").offset, true, i % 3, limit);
            VerifyAggregate(proof, bytes + (proof.control + 4) * 4, false, i % 3, limit);
            Expect<uint32_t>(bytes, proof.slots.at("untouched").offset, 0xcdcdcdcd, "Uninitialized local backing changed");
            Expect<uint32_t>(bytes, proof.slots.at("iteration").offset, limit, "Loop Phi state did not reach runtime limit");
            uint32_t initial[] = {5, 7, 11};
            auto expected = limit * initial[i % 3] + limit * (limit + 1) / 2 + i % 2;
            Require(frame[proof.control] == 2 && frame[proof.control + 2] == expected && frame[proof.control + 3] == 0,
                    "Restored SSA/control/return value checksum mismatch");
        }
        // Finished states must be stable on subsequent scheduler dispatches.
        std::vector<uint32_t> completed(gpu.mapped, gpu.mapped + proof.words * count);
        gpu.Dispatch(1, limit + 1000, count);
        Require(std::equal(completed.begin(), completed.end(), gpu.mapped), "Completed continuation ran again");
        std::cout << "Typed frame and suspension spill/restore SPIR-V validated and executed: 64 independent frames, "
                     "aggregate/narrow/matrix layout, persistent aliases, two-dispatch suspension, dynamic Phi loop, "
                     "late return, terminal state\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
