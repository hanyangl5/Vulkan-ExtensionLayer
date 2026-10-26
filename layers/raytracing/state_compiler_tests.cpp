// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "state_compiler.h"
#include "typed_pipeline.h"
#include "cts_raytmax_intersection_fixture.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
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
void LargeDescriptorResourceTypes() {
    // A full native storage-buffer budget is a descriptor declaration, not a
    // million-element value to spill into each invocation's continuation frame.
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability StorageBufferArrayDynamicIndexing
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %scenes %buffers %payload
OpDecorate %scenes DescriptorSet 31
OpDecorate %scenes Binding 0
OpDecorate %buffers DescriptorSet 31
OpDecorate %buffers Binding 1
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %payload Location 0
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%vec3 = OpTypeVector %float 3
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%mask = OpConstant %uint 255
%sixteen = OpConstant %uint 16
%last_scene = OpConstant %uint 15
%count = OpConstant %uint 1048544
%last = OpConstant %uint 1048543
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%origin = OpConstantComposite %vec3 %fzero %fzero %fzero
%direction = OpConstantComposite %vec3 %fzero %fzero %fone
%as = OpTypeAccelerationStructureKHR
%as_array = OpTypeArray %as %sixteen
%as_array_ptr = OpTypePointer UniformConstant %as_array
%as_ptr = OpTypePointer UniformConstant %as
%block = OpTypeStruct %uint
%blocks = OpTypeArray %block %count
%blocks_ptr = OpTypePointer StorageBuffer %blocks
%word_ptr = OpTypePointer StorageBuffer %uint
%payload_ptr = OpTypePointer RayPayloadKHR %uint
%scenes = OpVariable %as_array_ptr UniformConstant
%buffers = OpVariable %blocks_ptr StorageBuffer
%payload = OpVariable %payload_ptr RayPayloadKHR
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%scene_ptr = OpAccessChain %as_ptr %scenes %last_scene
%scene = OpLoad %as %scene_ptr
OpStore %payload %one
OpTraceRayKHR %scene %one %mask %zero %zero %zero %origin %fzero %direction %fone %payload
%word = OpAccessChain %word_ptr %buffers %last %zero
%result = OpLoad %uint %payload
OpStore %word %result
OpReturn
OpFunctionEnd
)spv";
    auto program = CompileTypedContinuationSteps(Source(source), "main", spv::ExecutionModelRayGenerationKHR);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    Require(tools.Validate(program.spirv), "Large resource declaration failed generated-module validation");
    Require(program.spirv.size() < 15000, "Resource descriptor array was expanded into per-element code");
    for (const auto &type : program.frames.types)
        Require(type.size < 4096, "Resource descriptor array was included in continuation value layouts");
    std::map<uint32_t, uint32_t> constants;
    bool full_array = false;
    for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
        const auto op = program.spirv[p] & 65535;
        if (op == spv::OpConstant && (program.spirv[p] >> 16) == 4) constants[program.spirv[p + 2]] = program.spirv[p + 3];
        if (op == spv::OpTypeArray) full_array |= constants[program.spirv[p + 3]] == 1048544;
    }
    Require(full_array && program.acceleration_structure_descriptors.size() == 1,
            "Full SSBO array or AS descriptor interface was discarded");
}
void ResourceValueTypes() {
    // SPIR-V1.4 permits a source Function pointer to the same explicitly laid
    // out structure used by a buffer. The newer emitted module must split
    // logical values from resource layouts without weakening those layouts.
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %buffer %callable
OpMemberDecorate %pair 0 Offset 0
OpMemberDecorate %pair 1 Offset 8
OpDecorate %array ArrayStride 16
OpMemberDecorate %block 0 Offset 0
OpMemberDecorate %block 1 Offset 16
OpDecorate %block Block
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 1
OpDecorate %callable Location 0
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%pair = OpTypeStruct %uint %float
%array = OpTypeArray %pair %two
%block = OpTypeStruct %pair %array
%buffer_ptr = OpTypePointer StorageBuffer %block
%pair_ptr = OpTypePointer StorageBuffer %pair
%array_ptr = OpTypePointer StorageBuffer %array
%local_ptr = OpTypePointer Function %pair
%callable_ptr = OpTypePointer CallableDataKHR %uint
%buffer = OpVariable %buffer_ptr StorageBuffer
%callable = OpVariable %callable_ptr CallableDataKHR
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%local = OpVariable %local_ptr Function
%p = OpAccessChain %pair_ptr %buffer %zero
%a = OpAccessChain %array_ptr %buffer %one
%before = OpLoad %pair %p Volatile
%before_array = OpLoad %array %a Volatile
OpExecuteCallableKHR %zero %callable
%after = OpLoad %pair %p Volatile
OpStore %local %before
OpStore %p %after Volatile
OpStore %a %before_array Volatile
OpReturn
OpFunctionEnd
)spv";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string error;
    tools.SetMessageConsumer(
        [&](spv_message_level_t, const char *, const spv_position_t &, const char *message) { error += message; });
    std::vector<uint32_t> words;
    Require(tools.Assemble(source, &words), "Resource source assembly: " + error);
    words[1] = 0x00010400;
    Require(tools.Validate(words), "Resource source validation: " + error);
    auto program = CompileTypedContinuationSteps(words, "main", spv::ExecutionModelRayGenerationKHR);
    Require(tools.Validate(program.spirv), "Resource value/layout split validation: " + error);
    uint32_t logical_copies = 0, volatile_loads = 0;
    for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
        const auto op = program.spirv[p] & 65535;
        logical_copies += op == spv::OpCopyLogical;
        volatile_loads +=
            op == spv::OpLoad && (program.spirv[p] >> 16) >= 5 && (program.spirv[p + 4] & spv::MemoryAccessVolatileMask);
    }
    Require(logical_copies >= 5 && volatile_loads == 3, "Aggregate conversion lost or duplicated a volatile source read");
}
void PrivateFrameBounds() {
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability Int64
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint IntersectionKHR %main "main" %large
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%u64 = OpTypeInt 64 0
%f32 = OpTypeFloat 32
%zero = OpConstant %u32 0
%wide_zero = OpConstant %u64 0
%one = OpConstant %f32 1
%count = OpConstant %u32 1073741824
%array = OpTypeArray %u64 %count
%array_ptr = OpTypePointer Private %array
%word_ptr = OpTypePointer Private %u64
%large = OpVariable %array_ptr Private
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%element = OpAccessChain %word_ptr %large %zero
%reported = OpReportIntersectionKHR %bool %one %zero
OpStore %element %wide_zero
OpReturn
OpFunctionEnd
)spv";
    bool rejected = false;
    try {
        CompileTypedContinuationSteps(Source(source), "main", spv::ExecutionModelIntersectionKHR);
    } catch (const std::exception &e) {
        rejected = std::string(e.what()).find("module-Private storage exceeds 32-bit") != std::string::npos;
    }
    Require(rejected, "Oversized module-Private array was emitted or its frame offset truncated");
}
void OpaqueResourceBoundaries() {
    const std::string image = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %image
OpDecorate %image DescriptorSet 0
OpDecorate %image Binding 1
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%i32 = OpTypeInt 32 1
%f32 = OpTypeFloat 32
%v3i = OpTypeVector %i32 3
%v4i = OpTypeVector %i32 4
%image_type = OpTypeImage %i32 3D 0 0 0 2 R32i
%image_pointer = OpTypePointer UniformConstant %image_type
%image = OpVariable %image_pointer UniformConstant
%fn = OpTypeFunction %void
%zero = OpConstant %i32 0
%one = OpConstant %i32 1
%uzero = OpConstant %u32 0
%fone = OpConstant %f32 1
%coord = OpConstantComposite %v3i %zero %zero %zero
%color = OpConstantComposite %v4i %one %zero %zero %one
%main = OpFunction %void None %fn
%entry = OpLabel
%loaded = OpLoad %image_type %image
OpImageWrite %loaded %coord %color SignExtend
OpReturn
OpFunctionEnd
)spv";
    auto check = [&](const std::string &source, uint32_t model) {
        auto program = CompileTypedContinuationSteps(Source(source), "main", model);
        uint32_t writes = 0, images = 0;
        for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
            auto op = program.spirv[p] & 65535;
            writes += op == spv::OpImageWrite;
            images += op == spv::OpTypeImage;
        }
        Require(writes == 1 && images == 1, "Storage image source operation/interface lost");
    };
    check(image, spv::ExecutionModelRayGenerationKHR);
    std::string late = image;
    Replace(late, "RayGenerationKHR %main", "IntersectionKHR %main");
    Replace(late, "%loaded = OpLoad", "%reported = OpReportIntersectionKHR %bool %fone %uzero\n%loaded = OpLoad");
    check(late, spv::ExecutionModelIntersectionKHR);
    std::string crossing = image;
    Replace(crossing, "OpImageWrite", "OpBranch %next\n%next = OpLabel\nOpImageWrite");
    check(crossing, spv::ExecutionModelRayGenerationKHR);

    std::string atomic = image;
    Replace(atomic, "%image_pointer = OpTypePointer", "%texel_pointer = OpTypePointer Image %i32\n%image_pointer = OpTypePointer");
    Replace(atomic, "%uzero = OpConstant",
            "%device = OpConstant %u32 1\n%image_acq_rel = OpConstant %u32 2056\n"
            "%image_release = OpConstant %u32 2052\n%uzero = OpConstant");
    Replace(atomic, "%loaded = OpLoad %image_type %image\nOpImageWrite %loaded %coord %color SignExtend",
            "%texel = OpImageTexelPointer %texel_pointer %image %coord %uzero\n"
            "%prior = OpAtomicSMin %i32 %texel %device %image_acq_rel %one\n"
            "OpAtomicStore %texel %device %image_release %prior\nOpMemoryBarrier %device %image_acq_rel");
    auto atomic_check = [&](const std::string &source, uint32_t model, uint32_t expected_texels = 1) {
        auto program = CompileTypedContinuationSteps(Source(source), "main", model);
        std::map<uint32_t, uint32_t> constants;
        uint32_t texels = 0, minima = 0, stores = 0, barriers = 0;
        for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
            const auto op = program.spirv[p] & 65535;
            if (op == spv::OpConstant && program.spirv[p] >> 16 == 4) constants[program.spirv[p + 2]] = program.spirv[p + 3];
            texels += op == spv::OpImageTexelPointer;
            if (op == spv::OpAtomicSMin) {
                ++minima;
                Require(constants.at(program.spirv[p + 4]) == 1 && constants.at(program.spirv[p + 5]) == 2056,
                        "Image atomic scope or acquire/release/image memory semantics changed");
            }
            if (op == spv::OpAtomicStore) {
                ++stores;
                Require(constants.at(program.spirv[p + 2]) == 1 && constants.at(program.spirv[p + 3]) == 2052,
                        "Image atomic store scope or release/image semantics changed");
            }
            if (op == spv::OpMemoryBarrier) {
                ++barriers;
                Require(constants.at(program.spirv[p + 1]) == 1 && constants.at(program.spirv[p + 2]) == 2056,
                        "Source memory barrier semantics changed");
            }
        }
        Require(texels == expected_texels && minima == 1 && stores == 1 && barriers == 1,
                "Image texel pointer/atomic side effects duplicated or lost");
        if (expected_texels == 2 && !program.frames.functions.at(0).suspensions.empty()) {
            uint32_t atomic_value = 0;
            for (size_t p = 5; p < program.source.size(); p += program.source[p] >> 16)
                if ((program.source[p] & 65535) == spv::OpAtomicSMin) atomic_value = program.source[p + 2];
            const auto &saved = program.frames.functions.at(0).suspensions.at(0).saved_values;
            Require(std::find(saved.begin(), saved.end(), atomic_value) != saved.end(),
                    "Atomic result lost across suspension or atomic side effect scheduled for replay");
        }
    };
    atomic_check(atomic, spv::ExecutionModelRayGenerationKHR);
    std::string late_atomic = atomic;
    Replace(late_atomic, "RayGenerationKHR %main", "IntersectionKHR %main");
    Replace(late_atomic, "%texel = OpImageTexelPointer",
            "%reported = OpReportIntersectionKHR %bool %fone %uzero\n%texel = OpImageTexelPointer");
    atomic_check(late_atomic, spv::ExecutionModelIntersectionKHR);
    std::string live_atomic = atomic;
    Replace(live_atomic, "RayGenerationKHR %main", "IntersectionKHR %main");
    Replace(live_atomic, "OpAtomicStore %texel",
            "%reported = OpReportIntersectionKHR %bool %fone %uzero\n"
            "%later_texel = OpImageTexelPointer %texel_pointer %image %coord %uzero\nOpAtomicStore %later_texel");
    atomic_check(live_atomic, spv::ExecutionModelIntersectionKHR, 2);
    std::string live_texel = atomic;
    Replace(live_texel, "RayGenerationKHR %main", "IntersectionKHR %main");
    Replace(live_texel, "OpAtomicStore %texel", "%reported = OpReportIntersectionKHR %bool %fone %uzero\nOpAtomicStore %texel");
    atomic_check(live_texel, spv::ExecutionModelIntersectionKHR, 2);
    // A selector is source data, not a rematerializable load. Both a texel
    // pointer and its copy must snapshot that data before its backing changes.
    std::string dynamic_texel = live_texel;
    Replace(dynamic_texel, "%fn = OpTypeFunction", "%local_pointer = OpTypePointer Function %i32\n%fn = OpTypeFunction");
    Replace(dynamic_texel, "%entry = OpLabel",
            "%entry = OpLabel\n%selector_storage = OpVariable %local_pointer Function %one\n"
            "%selector = OpLoad %i32 %selector_storage\n"
            "%dynamic_coord = OpCompositeConstruct %v3i %selector %selector %selector");
    Replace(dynamic_texel, "%image %coord %uzero", "%image %dynamic_coord %uzero");
    Replace(dynamic_texel, "%prior = OpAtomicSMin",
            "%saved_texel = OpCopyObject %texel_pointer %texel\nOpStore %selector_storage %zero\n%prior = OpAtomicSMin");
    Replace(dynamic_texel, "OpAtomicStore %texel", "OpAtomicStore %saved_texel");
    atomic_check(dynamic_texel, spv::ExecutionModelIntersectionKHR, 2);
    auto dynamic_program = CompileTypedContinuationSteps(Source(dynamic_texel), "main", spv::ExecutionModelIntersectionKHR);
    Require(dynamic_program.resource_snapshot_fields == 2 && dynamic_program.resource_snapshot_bytes == 24,
            "Dynamic texel coordinates/copy have no independent typed snapshots");
    std::string dynamic_descriptor = dynamic_texel;
    Replace(dynamic_descriptor, "OpCapability RayTracingKHR",
            "OpCapability RayTracingKHR\nOpCapability StorageImageArrayDynamicIndexing");
    Replace(dynamic_descriptor, "%image_pointer = OpTypePointer UniformConstant %image_type",
            "%descriptor_count = OpConstant %u32 2\n%image_array = OpTypeArray %image_type %descriptor_count\n"
            "%array_pointer = OpTypePointer UniformConstant %image_array\n"
            "%image_pointer = OpTypePointer UniformConstant %image_type");
    Replace(dynamic_descriptor, "%image = OpVariable %image_pointer", "%image = OpVariable %array_pointer");
    Replace(dynamic_descriptor, "%texel = OpImageTexelPointer",
            "%selected_image = OpAccessChain %image_pointer %image %selector\n%texel = OpImageTexelPointer");
    Replace(dynamic_descriptor, "%texel_pointer %image %dynamic_coord", "%texel_pointer %selected_image %dynamic_coord");
    atomic_check(dynamic_descriptor, spv::ExecutionModelIntersectionKHR, 2);
    auto selected_program = CompileTypedContinuationSteps(Source(dynamic_descriptor), "main", spv::ExecutionModelIntersectionKHR);
    Require(selected_program.resource_snapshot_fields == 5 && selected_program.resource_snapshot_bytes == 36,
            "Descriptor index was not captured independently through texel/copy provenance");
    auto dynamic_source = Source(dynamic_descriptor);
    auto physical_program =
        CompileTypedPipelineStage(dynamic_source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "snapshot_bda");
    TypedContinuationCompileOptions private_options;
    private_options.transport = TypedFrameTransport::PrivateArray;
    private_options.private_frame_words = physical_program.frame_stride_words;
    auto private_program = CompileTypedPipelineStage(dynamic_source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr,
                                                     "snapshot_private", private_options);
    Require(physical_program.resource_snapshot_fields == 5 && private_program.resource_snapshot_fields == 5 &&
                physical_program.frame_stride_bytes == private_program.frame_stride_bytes &&
                physical_program.control_byte_offset == private_program.control_byte_offset,
            "BDA and PrivateArray resource snapshots disagree on the checked stage-frame ABI");
    std::string crossing_pointer = atomic;
    Replace(crossing_pointer, "%prior = OpAtomicSMin", "OpBranch %next\n%next = OpLabel\n%prior = OpAtomicSMin");
    atomic_check(crossing_pointer, spv::ExecutionModelRayGenerationKHR, 2);
}
void ExactCtsImageAtomicStage() {
    spvtools::SpirvTools source_tools(SPV_ENV_VULKAN_1_3);
    std::vector<uint32_t> source;
    Require(source_tools.Assemble(kCtsRayTMaxIntersectionSource, &source, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS),
            "Exact CTS intersection source assembly failed");
    source[1] = 0x00010400;
    Require(source_tools.Validate(source), "Exact CTS SPIR-V1.4 intersection source failed validation");
    auto physical = CompileTypedPipelineStage(source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "cts_is_bda");
    TypedContinuationCompileOptions options;
    options.transport = TypedFrameTransport::PrivateArray;
    options.private_frame_words = static_cast<uint32_t>(physical.frame_stride_bytes / 4);
    auto private_array =
        CompileTypedPipelineStage(source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "cts_is_private", options);
    for (const auto *program : {&physical, &private_array}) {
        spvtools::SpirvTools library_tools(SPV_ENV_UNIVERSAL_1_5);
        Require(library_tools.Validate(program->spirv), "Exact CTS image atomic stage library failed validation");
        uint32_t pointers = 0, atomics = 0;
        for (size_t p = 5; p < program->spirv.size(); p += program->spirv[p] >> 16) {
            const auto op = program->spirv[p] & 65535;
            pointers += op == spv::OpImageTexelPointer;
            atomics += op == spv::OpAtomicSMin;
        }
        Require(pointers == 1 && atomics == 1 && program->frames.functions.at(0).suspensions.size() == 1,
                "Exact CTS image atomic/report source shape changed");
        Require(std::any_of(program->builtin_variables.begin(), program->builtin_variables.end(),
                            [](const auto &v) { return v.builtin == spv::BuiltInRayTmaxKHR; }),
                "Exact CTS RayTmax system backing was lost");
    }
}
void BoundariesAndSourcePreservation() {
    constexpr uint32_t tail = kTypedContinuationControlWords * 4u + 3u;
    auto bounds = PlanTypedContinuationFrameBounds(UINT32_MAX - tail);
    Require(bounds.stride_words == UINT32_MAX / 4 && bounds.max_invocations == 4, "Near-4GiB frame arithmetic changed");
    for (auto size : {uint64_t(UINT32_MAX) - tail + 1, uint64_t(UINT32_MAX), UINT64_MAX}) {
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
        LargeDescriptorResourceTypes();
        ResourceValueTypes();
        PrivateFrameBounds();
        OpaqueResourceBoundaries();
        ExactCtsImageAtomicStage();
        for (bool module_private : {false, true}) {
            std::string source = kSource;
            if (module_private) {
                Replace(source, "%launch %config %output", "%launch %config %output %local %untouched");
                Replace(source, "%entry_type = OpTypeFunction",
                        "%private_ptr = OpTypePointer Private %payload\n%private_word = OpTypePointer Private %u32\n"
                        "%local = OpVariable %private_ptr Private %initial\n%untouched = OpVariable %private_word Private\n"
                        "%entry_type = OpTypeFunction");
                Replace(source, "%local = OpVariable %local_ptr Function\n", "");
                Replace(source, "%untouched = OpVariable %local_word Function\n", "");
                Replace(source, "%alias = OpAccessChain %local_word", "%alias = OpAccessChain %private_word");
                Replace(source, "%result_ptr = OpAccessChain %local_word", "%result_ptr = OpAccessChain %private_word");
                Replace(source, "OpStore %local %initial\n", "");
                Replace(source, "OpStore %external_ptr %seed_added\n",
                        "OpStore %external_ptr %seed_added\n%uninitialized_value = OpLoad %u32 %untouched\n");
                for (const char *initializer : {"OpUndef", "OpConstantNull"}) {
                    std::string undefined = source;
                    Replace(undefined, "%local = OpVariable",
                            std::string("%private_initializer = ") + initializer + " %payload\n%local = OpVariable");
                    if (std::string(initializer) == "OpUndef")
                        Replace(undefined, "%launch_id = OpLoad", "OpStore %local %private_initializer\n%launch_id = OpLoad");
                    else
                        Replace(undefined, "%private_ptr Private %initial", "%private_ptr Private %private_initializer");
                    auto lowered = CompileTypedContinuationSteps(Source(undefined), "main", spv::ExecutionModelIntersectionKHR);
                    Require(lowered.private_variables.size() == 2,
                            "Private null initializer/undefined composite store backing lost");
                }
            }
            auto program = CompileTypedContinuationSteps(Source(source), "main", spv::ExecutionModelIntersectionKHR);
            Require(program.private_variables.size() == (module_private ? 2u : 0u), "Module-Private backing metadata missing");
            if (module_private) {
                auto physical = CompileTypedPipelineStage(Source(source), "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr,
                                                          "private_globals_bda");
                TypedContinuationCompileOptions private_options;
                private_options.transport = TypedFrameTransport::PrivateArray;
                private_options.private_frame_words = physical.frame_stride_words;
                auto native_private = CompileTypedPipelineStage(Source(source), "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR,
                                                                nullptr, "private_globals_private", private_options);
                Require(physical.private_variables.size() == 2 && native_private.private_variables.size() == 2 &&
                            physical.frame_stride_bytes == native_private.frame_stride_bytes &&
                            physical.private_variables[0].offset == native_private.private_variables[0].offset &&
                            physical.private_variables[1].offset == native_private.private_variables[1].offset,
                        "BDA/PrivateArray module-Private state has inconsistent checked frame backing");
            }
            std::string text;
            spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
            Require(tools.Disassemble(program.spirv, &text), "Generated module disassembly failed");
            Require(text.find("launch%123\\\"%456") != std::string::npos,
                    "SPIR-V literal percent IDs/escaped quote were rewritten");
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
                        std::find(frame.suspensions[1].saved_values.begin(), frame.suspensions[1].saved_values.end(),
                                  first_result) != frame.suspensions[1].saved_values.end(),
                    "Suspension bool was saved before creation or not preserved at a later pause");
            auto offset = [&](const char *name) -> uint64_t {
                for (const auto &s : frame.slots)
                    if (s.value == names.at(name)) return s.offset;
                for (const auto &s : program.private_variables)
                    if (s.variable == names.at(name)) return s.offset;
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
            std::vector<uint32_t> complete;
            // Reuse the same backing for a second logical activation. The initialized
            // aggregate must reset at entry, while all resumed states preserve it.
            for (uint32_t activation = 0; activation < 2; ++activation) {
                for (uint32_t i = 0; i < count; ++i)
                    std::fill_n(gpu.mapped + i * program.frame_stride_words + program.control_word, 7, 0);
                for (uint32_t i = 0; i < count; ++i) gpu.mapped[output_offset / 4 + i] = i + 100;
                std::vector<uint32_t> event_index(count, 0);
                gpu.Dispatch(base_limit, 0, count);
                // A second dispatch without completing the first request must not
                // speculate through the report, mutate source locals, or expose bool.
                std::vector<uint32_t> waiting(gpu.mapped, gpu.mapped + output_offset / 4 + count);
                gpu.Dispatch(base_limit, 0, count);
                Require(std::equal(waiting.begin(), waiting.end(), gpu.mapped),
                        "Source resumed before suspension result was ready");
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
                    VerifyPayload(words + offset("local") / 4, expected[id].array, expected[id].result);
                    VerifyPayload(words + offset("shadow") / 4, expected[id].shadow, 0);
                    Require(gpu.mapped[output_offset / 4 + id] == expected[id].result, "Source SSBO binding/typed store changed");
                    Require(words[offset("untouched") / 4] == 0xcdcdcdcd, "Source uninitialized local was speculatively copied");
                }
                complete.assign(gpu.mapped, gpu.mapped + output_offset / 4 + count);
                gpu.Dispatch(base_limit + 1000, 0, count + 5);
                Require(std::equal(complete.begin(), complete.end(), gpu.mapped), "Source terminal state executed again");
            }
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
            std::cout
                << "Compiled source CFG " << (module_private ? "module-Private" : "Function")
                << " storage validated and executed: 32 invocations, dynamic loops/branches, parallel Phi swaps, "
                   "inlined suspending helper, typed aggregates, persistent aliases, repeated real suspend/resume and late bool "
                   "results\n";
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
