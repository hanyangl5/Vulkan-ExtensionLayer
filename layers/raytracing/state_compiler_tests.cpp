// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "state_compiler.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv_cross/spirv.hpp>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using namespace vel::rt;
void Require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) { Require(result == VK_SUCCESS, "Vulkan result " + std::to_string(result)); }
#include "frame_test_gpu.h"

// This is source SPIR-V, not a hand-written compute/state test. The compiler
// must transform its actual CFG, helper call, Phi edges, arithmetic and memory
// operations. The CPU oracle below implements the algorithm independently.
const char *kSource = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint IntersectionKHR %main "main" %launch %config %output
OpName %local "local"
OpName %shadow "shadow"
OpName %untouched "untouched"
OpName %accepted "first_accepted"
OpDecorate %launch BuiltIn LaunchIdKHR
OpDecorate %launch UserSemantic "launch%123\"%456"
OpDecorate %config_type Block
OpMemberDecorate %config_type 0 Offset 0
OpDecorate %out_array ArrayStride 4
OpDecorate %out_type Block
OpMemberDecorate %out_type 0 Offset 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 1
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%f32 = OpTypeFloat 32
%v3u = OpTypeVector %u32 3
%input = OpTypePointer Input %v3u
%config_type = OpTypeStruct %u32
%config_ptr = OpTypePointer PushConstant %config_type
%config_word = OpTypePointer PushConstant %u32
%out_array = OpTypeRuntimeArray %u32
%out_type = OpTypeStruct %out_array
%out_ptr = OpTypePointer StorageBuffer %out_type
%out_word = OpTypePointer StorageBuffer %u32
%launch = OpVariable %input Input
%config = OpVariable %config_ptr PushConstant
%output = OpVariable %out_ptr StorageBuffer
%c0 = OpConstant %u32 0
%c1 = OpConstant %u32 1
%c2 = OpConstant %u32 2
%c3 = OpConstant %u32 3
%c5 = OpConstant %u32 5
%c7 = OpConstant %u32 7
%c11 = OpConstant %u32 11
%c13 = OpConstant %u32 13
%c17 = OpConstant %u32 17
%c23 = OpConstant %u32 23
%c31 = OpConstant %u32 31
%c64 = OpConstant %u32 64
%half = OpConstant %f32 0.5
%quarter = OpConstant %f32 1.25
%true = OpConstantTrue %bool
%array = OpTypeArray %u32 %c3
%payload = OpTypeStruct %v3u %array %bool %u32
%local_ptr = OpTypePointer Function %payload
%local_word = OpTypePointer Function %u32
%vec = OpConstantComposite %v3u %c17 %c23 %c31
%arr = OpConstantComposite %array %c5 %c7 %c11
%initial = OpConstantComposite %payload %vec %arr %true %c0
%entry_type = OpTypeFunction %void
%helper_type = OpTypeFunction %bool %f32 %u32
%main = OpFunction %void None %entry_type
%entry = OpLabel
%local = OpVariable %local_ptr Function
%shadow = OpVariable %local_ptr Function
%untouched = OpVariable %local_word Function
%launch_id = OpLoad %v3u %launch
%id = OpCompositeExtract %u32 %launch_id 0
%seed_ptr = OpAccessChain %out_word %output %c0 %id
%external_seed = OpLoad %u32 %seed_ptr
%index = OpUMod %u32 %id %c3
%alias = OpAccessChain %local_word %local %c1 %index
%limit_ptr = OpAccessChain %config_word %config %c0
%base_limit = OpLoad %u32 %limit_ptr
%limit = OpIAdd %u32 %base_limit %index
OpStore %local %initial
OpBranch %loop
%loop = OpLabel
%i = OpPhi %u32 %c0 %entry %next %continue
%sum = OpPhi %u32 %c0 %entry %next_sum %continue
%flip_a = OpPhi %u32 %c2 %entry %flip_b %continue
%flip_b = OpPhi %u32 %c3 %entry %flip_a %continue
%in_range = OpULessThan %bool %i %limit
OpLoopMerge %exit %continue None
OpBranchConditional %in_range %body %exit
%body = OpLabel
%snapshot = OpLoad %payload %local
%fi = OpConvertUToF %f32 %i
%distance = OpFAdd %f32 %fi %half
%kind = OpIAdd %u32 %i %c1
%accepted = OpReportIntersectionKHR %bool %distance %kind
OpStore %shadow %snapshot
%old = OpLoad %u32 %alias
%plus_i = OpIAdd %u32 %old %i
%bonus = OpSelect %u32 %accepted %c5 %c1
%updated = OpIAdd %u32 %plus_i %bonus
OpStore %alias %updated
OpSelectionMerge %choice None
OpBranchConditional %accepted %yes %no
%yes = OpLabel
%good = OpIAdd %u32 %sum %c3
OpBranch %choice
%no = OpLabel
%bad = OpIAdd %u32 %sum %c7
OpBranch %choice
%choice = OpLabel
%chosen = OpPhi %u32 %good %yes %bad %no
%low = OpBitwiseAnd %u32 %i %c1
%odd = OpIEqual %bool %low %c1
OpSelectionMerge %second_merge None
OpBranchConditional %odd %second %skip
%second = OpLabel
%distance2_raw = OpFAdd %f32 %fi %quarter
%negative_distance = OpFNegate %f32 %distance2_raw
%distance2 = OpExtInst %f32 %glsl FAbs %negative_distance
%kind2 = OpIAdd %u32 %i %c64
%accepted2 = OpFunctionCall %bool %helper %distance2 %kind2
%second_bonus = OpSelect %u32 %accepted2 %c11 %c13
OpBranch %second_merge
%skip = OpLabel
OpBranch %second_merge
%second_merge = OpLabel
%extra = OpPhi %u32 %second_bonus %second %c0 %skip
%late_first_bonus = OpSelect %u32 %accepted %c2 %c3
%with_value = OpIAdd %u32 %chosen %updated
%with_flip = OpIAdd %u32 %with_value %flip_a
%with_second = OpIAdd %u32 %with_flip %extra
%next_sum = OpIAdd %u32 %with_second %late_first_bonus
OpBranch %continue
%continue = OpLabel
%next = OpIAdd %u32 %i %c1
OpBranch %loop
%exit = OpLabel
%mode = OpUMod %u32 %id %c3
OpSelectionMerge %final None
OpSwitch %mode %case2 0 %case0 1 %case1
%case0 = OpLabel
%r0 = OpIAdd %u32 %sum %c17
OpBranch %final
%case1 = OpLabel
%r1 = OpIAdd %u32 %sum %c23
OpBranch %final
%case2 = OpLabel
%r2 = OpIAdd %u32 %sum %c31
OpBranch %final
%final = OpLabel
%final_sum = OpPhi %u32 %r0 %case0 %r1 %case1 %r2 %case2
%seed_added = OpIAdd %u32 %final_sum %external_seed
%result_ptr = OpAccessChain %local_word %local %c3
OpStore %result_ptr %seed_added
%external_ptr = OpAccessChain %out_word %output %c0 %id
OpStore %external_ptr %seed_added
OpReturn
OpFunctionEnd
%helper = OpFunction %bool None %helper_type
%argument_distance = OpFunctionParameter %f32
%argument_kind = OpFunctionParameter %u32
%helper_entry = OpLabel
%reported = OpReportIntersectionKHR %bool %argument_distance %argument_kind
OpReturnValue %reported
OpFunctionEnd
)spv";

std::vector<uint32_t> Source(const std::string &text = kSource) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string error;
    tools.SetMessageConsumer(
        [&](spv_message_level_t, const char *, const spv_position_t &, const char *message) { error += message; });
    std::vector<uint32_t> binary;
    bool success = tools.Assemble(text, &binary);
    Require(success, "Source assembly: " + error);
    success = tools.Validate(binary);
    Require(success, "Source validation: " + error);
    return binary;
}
void Replace(std::string &source, const std::string &before, const std::string &after) {
    auto offset = source.find(before);
    Require(offset != std::string::npos, "Regression source fragment missing");
    source.replace(offset, before.size(), after);
}
void BoundariesAndSourcePreservation() {
    auto bounds = PlanTypedContinuationFrameBounds(UINT32_MAX - 31u);
    Require(bounds.stride_words == UINT32_MAX / 4 && bounds.max_invocations == 4, "Near-4GiB frame arithmetic changed");
    for (auto size : {uint64_t(UINT32_MAX) - 30, uint64_t(UINT32_MAX), UINT64_MAX}) {
        bool rejected = false;
        try {
            PlanTypedContinuationFrameBounds(size);
        } catch (const std::exception &error) {
            rejected = std::string(error.what()).find("32-bit byte indexing") != std::string::npos;
        }
        Require(rejected, "Frame/control byte offset overflow accepted");
    }
    std::string grouped(kSource);
    Replace(grouped, "OpDecorate %output DescriptorSet 0\nOpDecorate %output Binding 1",
            "%resource_decorations = OpDecorationGroup\nOpDecorate %resource_decorations DescriptorSet 0\n"
            "OpDecorate %resource_decorations Binding 1\nOpGroupDecorate %resource_decorations %output");
    CompileTypedContinuationSteps(Source(grouped), "main", spv::ExecutionModelIntersectionKHR);
    std::string switch64(kSource);
    Replace(switch64, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability Int64");
    Replace(switch64, "%f32 = OpTypeFloat 32", "%f32 = OpTypeFloat 32\n%u64 = OpTypeInt 64 0");
    Replace(switch64, "%mode = OpUMod %u32 %id %c3", "%mode32 = OpUMod %u32 %id %c3\n%mode = OpUConvert %u64 %mode32");
    Replace(switch64, "OpSwitch %mode %case2 0 %case0 1 %case1",
            "OpSwitch %mode %case2 4294967296 %case0 18446744073709551615 %case1");
    CompileTypedContinuationSteps(Source(switch64), "main", spv::ExecutionModelIntersectionKHR);
    std::string pointer_copy(kSource);
    Replace(pointer_copy, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability ShaderNonUniform");
    Replace(pointer_copy, "OpDecorate %launch BuiltIn", "OpDecorate %seed_ptr NonUniform\nOpDecorate %launch BuiltIn");
    Replace(pointer_copy, "%seed_ptr = OpAccessChain %out_word %output %c0 %id",
            "%seed_base = OpAccessChain %out_word %output %c0 %id\n%seed_ptr = OpCopyObject %out_word %seed_base");
    auto copied = CompileTypedContinuationSteps(Source(pointer_copy), "main", spv::ExecutionModelIntersectionKHR);
    std::set<uint32_t> decorated, pointer_copies;
    for (size_t p = 5; p < copied.spirv.size(); p += copied.spirv[p] >> 16) {
        auto opcode = copied.spirv[p] & 0xffff;
        if (opcode == spv::OpDecorate && copied.spirv[p + 2] == spv::DecorationNonUniform) decorated.insert(copied.spirv[p + 1]);
        if (opcode == spv::OpCopyObject) pointer_copies.insert(copied.spirv[p + 2]);
    }
    Require(std::any_of(pointer_copies.begin(), pointer_copies.end(), [&](uint32_t id) { return decorated.count(id) != 0; }),
            "Copied resource pointer lost its sole NonUniform decoration");

    std::string modes(kSource);
    Replace(modes, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability SignedZeroInfNanPreserve");
    Replace(modes, "OpExtension \"SPV_KHR_ray_tracing\"",
            "OpExtension \"SPV_KHR_ray_tracing\"\nOpExtension \"SPV_KHR_float_controls\"");
    Replace(modes, "OpName %local",
            "OpExecutionMode %main SignedZeroInfNanPreserve 32\nOpDecorate %distance NoContraction\nOpName %local");
    // Decorations belong after names, so insert NoContraction in that section.
    Replace(modes, "OpDecorate %distance NoContraction\n", "");
    Replace(modes, "OpDecorate %launch BuiltIn", "OpDecorate %distance NoContraction\nOpDecorate %launch BuiltIn");
    auto controls = CompileTypedContinuationSteps(Source(modes), "main", spv::ExecutionModelIntersectionKHR);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string disassembly;
    Require(tools.Disassemble(controls.spirv, &disassembly), "FP-control disassembly failed");
    Require(disassembly.find("SignedZeroInfNanPreserve 32") != std::string::npos &&
                disassembly.find("NoContraction") != std::string::npos,
            "Source FP execution mode/operation decoration was discarded");

    std::string multiple(kSource);
    Replace(multiple, "OpName %local", "OpEntryPoint RayGenerationKHR %other \"other\" %other_private\nOpName %local");
    Replace(multiple, "%launch = OpVariable",
            "%private_type = OpTypePointer Private %u32\n%other_private = OpVariable %private_type Private\n%launch = OpVariable");
    multiple +=
        "\n%other = OpFunction %void None %entry_type\n%other_label = OpLabel\n%unreachable_private_read = OpLoad %u32 "
        "%other_private\nOpReturn\nOpFunctionEnd\n";
    auto selected = CompileTypedContinuationSteps(Source(multiple), "main", spv::ExecutionModelIntersectionKHR);
    uint32_t entry_count = 0;
    for (size_t p = 5; p < selected.source.size(); p += selected.source[p] >> 16)
        if ((selected.source[p] & 0xffff) == spv::OpEntryPoint) ++entry_count;
    Require(entry_count == 1, "Unselected source entry point survived selection");

    auto reject = [&](const std::string &source, const char *reason) {
        try {
            CompileTypedContinuationSteps(Source(source), "main", spv::ExecutionModelIntersectionKHR);
        } catch (const std::exception &error) {
            Require(std::string(error.what()).find(reason) != std::string::npos,
                    "Wrong explicit rejection: " + std::string(error.what()));
            return;
        }
        throw std::runtime_error("Unsupported source accepted");
    };
    std::string collision(kSource);
    Replace(collision, "OpDecorate %output Binding 1", "OpDecorate %output Binding 0");
    reject(collision, "frame descriptor binding collides");
    std::string pointer(kSource);
    Replace(pointer, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability VariablePointersStorageBuffer");
    Replace(pointer, "%seed_ptr = OpAccessChain %out_word %output %c0 %id",
            "%seed_base = OpAccessChain %out_word %output %c0 %id\n%seed_ptr = OpSelect %out_word %true %seed_base %seed_base");
    reject(pointer, "pointer provenance");
    std::string memory_model(kSource);
    Replace(memory_model, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability VulkanMemoryModel");
    Replace(memory_model, "OpExtension \"SPV_KHR_ray_tracing\"",
            "OpExtension \"SPV_KHR_ray_tracing\"\nOpExtension \"SPV_KHR_vulkan_memory_model\"");
    Replace(memory_model, "OpMemoryModel Logical GLSL450", "OpMemoryModel Logical Vulkan");
    reject(memory_model, "source memory model requires explicit frame/scheduler support");
}
struct Event {
    float distance;
    uint32_t kind;
};
struct Oracle {
    std::vector<Event> events;
    uint32_t array[3]{5, 7, 11}, shadow[3]{5, 7, 11}, result = 0;
    Oracle(uint32_t id, uint32_t base_limit) {
        uint32_t a = 2, b = 3;
        auto index = id % 3;
        for (uint32_t i = 0; i < base_limit + index; ++i) {
            events.push_back({float(i) + 0.5f, i + 1});
            std::copy(std::begin(array), std::end(array), std::begin(shadow));
            bool first = (id + i + 1) % 3 != 0;
            array[index] += i + (first ? 5 : 1);
            result += (first ? 3 : 7) + array[index] + a;
            result += first ? 2 : 3;
            if (i & 1) {
                events.push_back({float(i) + 1.25f, i + 64});
                result += (id + i + 64) % 3 != 0 ? 11 : 13;
            }
            std::swap(a, b);
        }
        result += index == 0 ? 17 : index == 1 ? 23 : 31;
        result += id + 100;
    }
};
void VerifyPayload(const uint32_t *value, const uint32_t *array, uint32_t result) {
    Require(value[0] == 17 && value[1] == 23 && value[2] == 31, "Aggregate vector changed");
    Require(std::equal(array, array + 3, value + 3), "Aggregate/local alias state mismatch");
    Require(value[6] == 1 && value[7] == result, "Aggregate bool/result mismatch");
}
}  // namespace

int main() {
    try {
        BoundariesAndSourcePreservation();
        auto program = CompileTypedContinuationSteps(Source(), "main", spv::ExecutionModelIntersectionKHR);
        std::string text;
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        Require(tools.Disassemble(program.spirv, &text), "Generated module disassembly failed");
        Require(text.find("launch%123\\\"%456") != std::string::npos, "SPIR-V literal percent IDs/escaped quote were rewritten");
        Require(program.state_count > 12, "Source CFG was not split into block/resume states");
        const auto &frame = program.frames.functions.at(0);
        Require(frame.suspensions.size() == 2, "Reachable helper report was not lowered");
        std::map<std::string, uint32_t> names;
        for (size_t p = 5; p < program.source.size(); p += program.source[p] >> 16)
            if ((program.source[p] & 0xffff) == spv::OpName)
                names[reinterpret_cast<const char *>(&program.source[p + 2])] = program.source[p + 1];
        const auto first_result = names.at("first_accepted");
        Require(std::find(frame.suspensions[0].saved_values.begin(), frame.suspensions[0].saved_values.end(), first_result) ==
                        frame.suspensions[0].saved_values.end() &&
                    std::find(frame.suspensions[1].saved_values.begin(), frame.suspensions[1].saved_values.end(), first_result) !=
                        frame.suspensions[1].saved_values.end(),
                "Suspension bool was saved before creation or not preserved at a later pause");
        auto slot = [&](const char *name) -> const ContinuationFrameSlot & {
            for (const auto &s : frame.slots)
                if (s.value == names.at(name)) return s;
            throw std::runtime_error("Missing source variable backing");
        };
        constexpr uint32_t count = 32, base_limit = 7;
        std::vector<Oracle> expected;
        for (uint32_t i = 0; i < count; ++i) expected.emplace_back(i, base_limit);
        GPU gpu;
        auto frame_bytes = program.frame_stride_words * count * 4;
        auto output_offset = (frame_bytes + 255u) & ~255u;
        if (!gpu.Init(program.spirv, output_offset + count * 4, output_offset, false, frame_bytes)) {
            std::cout << "Generated source CFG SPIR-V validated; Vulkan execution unavailable\n";
            return 77;
        }
        for (uint32_t i = 0; i < count; ++i) std::fill_n(gpu.mapped + i * program.frame_stride_words + program.control_word, 7, 0);
        for (uint32_t i = 0; i < count; ++i) gpu.mapped[output_offset / 4 + i] = i + 100;
        std::vector<uint32_t> event_index(count, 0);
        gpu.Dispatch(base_limit, 0, count);
        // A second dispatch without completing the first request must not
        // speculate through the report, mutate source locals, or expose bool.
        std::vector<uint32_t> waiting(gpu.mapped, gpu.mapped + output_offset / 4 + count);
        gpu.Dispatch(base_limit, 0, count);
        Require(std::equal(waiting.begin(), waiting.end(), gpu.mapped), "Source resumed before suspension result was ready");
        // The source SSBO load has already occurred. Its typed SSA value must
        // survive pauses; rematerializing that load would observe this poison.
        for (uint32_t i = 0; i < count; ++i) gpu.mapped[output_offset / 4 + i] = 0xdeadbeef;
        bool finished = false;
        for (uint32_t round = 0; round < 128; ++round) {
            finished = true;
            for (uint32_t id = 0; id < count; ++id) {
                auto control = gpu.mapped + id * program.frame_stride_words + program.control_word;
                if (control[0] == UINT32_MAX) continue;
                finished = false;
                Require(control[1] == 2 && control[2] == 0, "Invalid generated request state");
                Require(event_index[id] < expected[id].events.size(), "Source emitted excess suspension");
                const auto &event = expected[id].events[event_index[id]++];
                float distance;
                std::memcpy(&distance, control + 4, 4);
                Require(distance == event.distance && control[5] == event.kind, "Source ALU/request arguments changed");
                Require(control[6] == frame.suspensions[0].word_offset || control[6] == frame.suspensions[1].word_offset,
                        "Request lost source-site identity");
                control[3] = (id + event.kind) % 3 != 0;
                control[2] = 1;
            }
            if (finished) break;
            gpu.Dispatch(base_limit, 0, count);
        }
        Require(finished, "Generated source state machine did not terminate");
        for (uint32_t id = 0; id < count; ++id) {
            auto words = gpu.mapped + id * program.frame_stride_words;
            Require(event_index[id] == expected[id].events.size(), "Source skipped a suspension");
            VerifyPayload(words + slot("local").offset / 4, expected[id].array, expected[id].result);
            VerifyPayload(words + slot("shadow").offset / 4, expected[id].shadow, 0);
            Require(gpu.mapped[output_offset / 4 + id] == expected[id].result, "Source SSBO binding/typed store changed");
            Require(words[slot("untouched").offset / 4] == 0xcdcdcdcd, "Source uninitialized local was speculatively copied");
        }
        std::vector<uint32_t> complete(gpu.mapped, gpu.mapped + output_offset / 4 + count);
        gpu.Dispatch(base_limit + 1000, 0, count + 5);
        Require(std::equal(complete.begin(), complete.end(), gpu.mapped), "Source terminal state executed again");
        // An undersized frame descriptor must also skip invocation zero before
        // touching its PC, local storage, or ordinary source output resource.
        VkDescriptorBufferInfo tiny{gpu.buffer, 0, (program.frame_stride_words - 1) * 4};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = gpu.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &tiny;
        vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
        std::fill_n(gpu.mapped + program.control_word, 7, 0);
        complete.assign(gpu.mapped, gpu.mapped + output_offset / 4 + count);
        gpu.Dispatch(base_limit, 0, count + 5);
        Require(std::equal(complete.begin(), complete.end(), gpu.mapped),
                "Undersized frame descriptor bypassed kernel bounds guard");
        std::cout << "Compiled source CFG validated and executed: 32 invocations, dynamic loops/branches, parallel Phi swaps, "
                     "inlined suspending helper, typed aggregates, persistent aliases, repeated real suspend/resume and late bool "
                     "results\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
