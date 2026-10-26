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
#include <sstream>
#include <stdexcept>

namespace {
using namespace vel::rt;
void Require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) { Require(result == VK_SUCCESS, "Vulkan result " + std::to_string(result)); }
#include "frame_test_gpu.h"
struct Input {
    const char *name, *builtin, *type;
    uint32_t offset, count;
    bool integer;
};
const Input kInputs[] = {
    {"launch", "LaunchIdKHR", "v3u", VEL_RT_SYS_LAUNCH_ID, 3, true},
    {"size", "LaunchSizeKHR", "v3u", VEL_RT_SYS_LAUNCH_SIZE, 3, true},
    {"flags", "IncomingRayFlagsKHR", "u32", VEL_RT_SYS_RAY_FLAGS, 1, true},
    {"world_o", "WorldRayOriginKHR", "v3f", VEL_RT_SYS_WORLD_ORIGIN, 3, false},
    {"world_d", "WorldRayDirectionKHR", "v3f", VEL_RT_SYS_WORLD_DIRECTION, 3, false},
    {"object_o", "ObjectRayOriginKHR", "v3f", VEL_RT_SYS_OBJECT_ORIGIN, 3, false},
    {"object_d", "ObjectRayDirectionKHR", "v3f", VEL_RT_SYS_OBJECT_DIRECTION, 3, false},
    {"minimum", "RayTminKHR", "f32", VEL_RT_SYS_TMIN, 1, false},
    {"maximum", "RayTmaxKHR", "f32", VEL_RT_SYS_TMAX, 1, false},
    {"custom", "InstanceCustomIndexKHR", "i32", VEL_RT_SYS_INSTANCE_CUSTOM_INDEX, 1, true},
    {"instance", "InstanceId", "i32", VEL_RT_SYS_INSTANCE_ID, 1, true},
    {"geometry", "RayGeometryIndexKHR", "i32", VEL_RT_SYS_GEOMETRY_INDEX, 1, true},
    {"primitive", "PrimitiveId", "i32", VEL_RT_SYS_PRIMITIVE_ID, 1, true},
    {"hitkind", "HitKindKHR", "u32", VEL_RT_SYS_HIT_KIND, 1, true},
    {"otw", "ObjectToWorldKHR", "matrix", VEL_RT_SYS_OBJECT_TO_WORLD, 12, false},
    {"wto", "WorldToObjectKHR", "matrix", VEL_RT_SYS_WORLD_TO_OBJECT, 12, false},
    {"mask", "CullMaskKHR", "u32", VEL_RT_SYS_CULL_MASK, 1, true},
};
std::string Source() {
    std::ostringstream s;
    s << "OpCapability Shader\nOpCapability RayTracingKHR\nOpCapability RayCullMaskKHR\n"
      << "OpExtension \"SPV_KHR_ray_tracing\"\nOpExtension \"SPV_KHR_ray_cull_mask\"\nOpMemoryModel Logical GLSL450\n"
      << "OpEntryPoint ClosestHitKHR %main \"main\" %config %record %input %output %data";
    for (const auto &input : kInputs) s << " %" << input.name;
    s << "\n";
    for (const auto &input : kInputs) s << "OpDecorate %" << input.name << " BuiltIn " << input.builtin << "\n";
    s << R"spv(
OpDecorate %buffer Block
OpDecorate %words ArrayStride 4
OpMemberDecorate %buffer 0 Offset 0
OpDecorate %input DescriptorSet 0
OpDecorate %input Binding 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 1
OpDecorate %push_array ArrayStride 4
OpDecorate %push Block
OpMemberDecorate %push 0 Offset 0
OpDecorate %record_type Block
OpMemberDecorate %record_type 0 Offset 0
OpMemberDecorate %record_type 1 Offset 4
OpDecorate %data Location 19
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%i32 = OpTypeInt 32 1
%f32 = OpTypeFloat 32
%v3u = OpTypeVector %u32 3
%v3f = OpTypeVector %f32 3
%matrix = OpTypeMatrix %v3f 4
%c0 = OpConstant %u32 0
%c1 = OpConstant %u32 1
%c31 = OpConstant %u32 31
%c32 = OpConstant %u32 32
%f0 = OpConstant %f32 0
%words = OpTypeRuntimeArray %f32
%buffer = OpTypeStruct %words
%buffer_ptr = OpTypePointer StorageBuffer %buffer
%float_ptr = OpTypePointer StorageBuffer %f32
%push_array = OpTypeArray %u32 %c32
%push = OpTypeStruct %push_array
%push_ptr = OpTypePointer PushConstant %push
%push_word = OpTypePointer PushConstant %u32
%record_type = OpTypeStruct %u32 %f32
%record_ptr = OpTypePointer ShaderRecordBufferKHR %record_type
%record_word = OpTypePointer ShaderRecordBufferKHR %u32
%record_float = OpTypePointer ShaderRecordBufferKHR %f32
%call_ptr = OpTypePointer CallableDataKHR %u32
%fn = OpTypeFunction %void
%input_u32 = OpTypePointer Input %u32
%input_i32 = OpTypePointer Input %i32
%input_f32 = OpTypePointer Input %f32
%input_v3u = OpTypePointer Input %v3u
%input_v3f = OpTypePointer Input %v3f
%input_matrix = OpTypePointer Input %matrix
%input = OpVariable %buffer_ptr StorageBuffer
%output = OpVariable %buffer_ptr StorageBuffer
%config = OpVariable %push_ptr PushConstant
%record = OpVariable %record_ptr ShaderRecordBufferKHR
%data = OpVariable %call_ptr CallableDataKHR
)spv";
    for (const auto &input : kInputs) s << "%" << input.name << " = OpVariable %input_" << input.type << " Input\n";
    s << "%main = OpFunction %void None %fn\n%entry = OpLabel\n";
    std::string integer_sum = "%c0", float_sum = "%f0";
    uint32_t serial = 0;
    for (const auto &input : kInputs) {
        s << "%value_" << input.name << " = OpLoad %" << input.type << " %" << input.name << "\n";
        for (uint32_t n = 0; n < input.count; ++n) {
            auto value = "%value_" + std::string(input.name);
            if (input.count > 1) {
                auto part = "%part" + std::to_string(++serial);
                s << part << " = OpCompositeExtract %" << (input.integer ? "u32" : "f32") << " " << value;
                if (input.count == 12)
                    s << " " << n / 3 << " " << n % 3;
                else
                    s << " " << n;
                s << "\n";
                value = part;
            } else if (std::string(input.type) == "i32") {
                auto unsigned_value = "%bits" + std::to_string(++serial);
                s << unsigned_value << " = OpBitcast %u32 " << value << "\n";
                value = unsigned_value;
            }
            auto next = "%sum" + std::to_string(++serial);
            s << next << " = " << (input.integer ? "OpIAdd %u32 " : "OpFAdd %f32 ") << (input.integer ? integer_sum : float_sum)
              << " " << value << "\n";
            (input.integer ? integer_sum : float_sum) = next;
        }
    }
    s << R"spv(
%index = OpCompositeExtract %u32 %value_launch 0
%in_ptr = OpAccessChain %float_ptr %input %c0 %index
%input_value = OpLoad %f32 %in_ptr
%push_last = OpAccessChain %push_word %config %c0 %c31
%push_value = OpLoad %u32 %push_last
%rec_word_ptr = OpAccessChain %record_word %record %c0
%rec_float_ptr = OpAccessChain %record_float %record %c1
%rec_word = OpLoad %u32 %rec_word_ptr
%rec_float = OpLoad %f32 %rec_float_ptr
)spv";
    s << "%integer1 = OpIAdd %u32 " << integer_sum << " %push_value\n%integer2 = OpIAdd %u32 %integer1 %rec_word\n"
      << "%float1 = OpFAdd %f32 " << float_sum << " %input_value\n%float2 = OpFAdd %f32 %float1 %rec_float\n"
      << R"spv(
OpStore %data %integer2
OpExecuteCallableKHR %c0 %data
%completed = OpLoad %u32 %data
%old_float = OpConvertUToF %f32 %integer2
%new_float = OpConvertUToF %f32 %completed
%answer1 = OpFAdd %f32 %float2 %old_float
%answer2 = OpFAdd %f32 %answer1 %new_float
%out_ptr = OpAccessChain %float_ptr %output %c0 %index
OpStore %out_ptr %answer2
OpReturn
OpFunctionEnd
)spv";
    return s.str();
}
std::vector<uint32_t> Assemble(const std::string &source) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string error;
    tools.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &p, const char *m) {
        error += std::to_string(p.index) + ": " + m + "\n";
    });
    std::vector<uint32_t> binary;
    bool ok = tools.Assemble(source, &binary);
    Require(ok, "Source assembly: " + error);
    ok = tools.Validate(binary);
    Require(ok, "Source validation: " + error);
    return binary;
}
template <class T>
void Write(unsigned char *bytes, uint64_t offset, T value) {
    std::memcpy(bytes + offset, &value, sizeof(value));
}
template <class T>
T Read(const unsigned char *bytes, uint64_t offset) {
    T value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}
uint32_t UValue(const Input &input, uint32_t id, uint32_t n) {
    if (input.offset == VEL_RT_SYS_LAUNCH_ID) return n == 0 ? id : n == 1 ? 11 + id : 21 + id;
    if (input.offset == VEL_RT_SYS_LAUNCH_SIZE) return n == 0 ? 17 : n == 1 ? 19 : 23;
    if (input.offset == VEL_RT_SYS_RAY_FLAGS) return 1u << (id % 3);
    if (input.offset == VEL_RT_SYS_INSTANCE_CUSTOM_INDEX) return 41 + id;
    if (input.offset == VEL_RT_SYS_INSTANCE_ID) return 51 + id;
    if (input.offset == VEL_RT_SYS_GEOMETRY_INDEX) return 61 + id;
    if (input.offset == VEL_RT_SYS_PRIMITIVE_ID) return 71 + id;
    if (input.offset == VEL_RT_SYS_HIT_KIND) return id & 1 ? 255 : 254;
    return 160 + id;
}
float FValue(const Input &input, uint32_t id, uint32_t n) {
    if (input.offset == VEL_RT_SYS_WORLD_ORIGIN) return float(id) + float(n + 1) * 1.25f;
    if (input.offset == VEL_RT_SYS_WORLD_DIRECTION) return n == 0 ? 0.25f : n == 1 ? 0.5f : 1.0f;
    if (input.offset == VEL_RT_SYS_OBJECT_ORIGIN) return -float(id) - float(n + 1);
    if (input.offset == VEL_RT_SYS_OBJECT_DIRECTION) return n == 0 ? 2.0f : n == 1 ? 0.125f : 1.5f;
    if (input.offset == VEL_RT_SYS_TMIN) return 0.5f + float(id) / 8;
    if (input.offset == VEL_RT_SYS_TMAX) return 100.0f + id;
    if (input.offset == VEL_RT_SYS_OBJECT_TO_WORLD) return 10.0f + id + float(n) / 4;
    return -5.0f - id - float(n) / 8;
}
bool Execute() {
    auto source = Assemble(Source());
    TypedContinuationCompileOptions options;
    options.transport = TypedFrameTransport::PhysicalAddress;
    auto program = CompileTypedContinuationSteps(source, "main", spv::ExecutionModelClosestHitKHR, options);
    Require(program.system_header_bytes == 256 && program.builtin_variables.size() == std::size(kInputs) &&
                program.uses_app_push_constants && program.uses_shader_record,
            "BDA/system metadata missing");
    for (const auto &slot : program.frames.functions[0].slots)
        Require(slot.offset >= VEL_RT_SYSTEM_BYTES, "SSA/local offset excludes system prefix");
    for (const auto &slot : program.invocation_variables)
        Require(slot.offset >= VEL_RT_SYSTEM_BYTES, "Invocation offset excludes system prefix");
    options.emit_compute_entry = false;
    options.step_export_name = "vel_rt_bda_probe";
    auto library = CompileTypedContinuationSteps(source, "main", spv::ExecutionModelClosestHitKHR, options);
    Require(library.spirv.at(1) == 0x00010500u, "Stage library is not SPIR-V1.5");
    spvtools::SpirvTools inspect(SPV_ENV_UNIVERSAL_1_5);
    std::string text;
    Require(inspect.Disassemble(library.spirv, &text), "Stage library disassembly failed");
    Require(text.find("LinkageAttributes \"vel_rt_bda_probe\" Export") != std::string::npos &&
                text.find("OpEntryPoint") == std::string::npos && text.find("OpSpecConstant") == std::string::npos,
            "Stage export contains a private entry/root instead of direct frameBDA ABI");
    Require(text.find("Binding 0") != std::string::npos && text.find("Binding 1") != std::string::npos,
            "Application bindings were changed");
    uint32_t bindings = 0;
    for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16)
        if ((program.spirv[p] & 0xffff) == spv::OpDecorate && program.spirv[p + 2] == spv::DecorationBinding) ++bindings;
    Require(bindings == 2, "BDA mode silently reserved an extra frame descriptor");
    constexpr uint32_t count = 8, context_offset = 64, input_offset = 128, output_offset = 256, frame_offset = 512;
    auto frame_end = frame_offset + program.frame_stride_bytes * count;
    auto record_offset = (frame_end + 255) & ~uint64_t(255);
    auto push_offset = (record_offset + count * 16 + 255) & ~uint64_t(255);
    auto total = push_offset + count * 128 + 128;
    GPU gpu;
    if (!gpu.Init(program.spirv, uint32_t(total), output_offset, false, count * 4, context_offset)) return false;
    VkDescriptorBufferInfo input_info{gpu.buffer, input_offset, count * 4};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = gpu.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &input_info;
    vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
    auto bytes = reinterpret_cast<unsigned char *>(gpu.mapped);
    Write<uint64_t>(bytes, context_offset + VEL_RT_CTX_FRAMES_ADDRESS, gpu.address);
    Write<uint64_t>(bytes, context_offset + VEL_RT_CTX_FRAME_STRIDE, program.frame_stride_bytes);
    Write<uint64_t>(bytes, context_offset + VEL_RT_CTX_FRAME_COUNT, uint64_t(1) << 32);
    Write<uint64_t>(bytes, context_offset + VEL_RT_CTX_FRAME_OFFSET, frame_offset);
    Write<uint64_t>(bytes, context_offset + VEL_RT_CTX_FRAME_CAPACITY, frame_end);
    std::array<uint32_t, count> integers{};
    std::array<float, count> floats{};
    for (uint32_t id = 0; id < count; ++id) {
        auto frame = bytes + frame_offset + id * program.frame_stride_bytes;
        for (const auto &input : kInputs)
            for (uint32_t n = 0; n < input.count; ++n) {
                if (input.integer) {
                    auto value = UValue(input, id, n);
                    Write<uint32_t>(frame, input.offset + n * 4, value);
                    integers[id] += value;
                } else {
                    auto value = FValue(input, id, n);
                    Write<float>(frame, input.offset + n * 4, value);
                    floats[id] += value;
                }
            }
        Write<uint64_t>(frame, VEL_RT_SYS_SHADER_RECORD_ADDRESS, gpu.address + record_offset + id * 16);
        Write<uint64_t>(frame, VEL_RT_SYS_APP_PUSH_ADDRESS, gpu.address + push_offset + id * 128);
        Write<uint64_t>(frame, VEL_RT_SYS_CONTEXT_ADDRESS, gpu.address + context_offset);
        Write<uint32_t>(bytes, record_offset + id * 16, 200 + id);
        Write<float>(bytes, record_offset + id * 16 + 4, 0.25f + id);
        Write<uint32_t>(bytes, push_offset + id * 128 + 124, 1000 + id);
        Write<float>(bytes, input_offset + id * 4, 5.0f + id);
        integers[id] += 200 + id + 1000 + id;
        floats[id] += 5.0f + id;
        floats[id] += 0.25f + id;
        std::fill_n(reinterpret_cast<uint32_t *>(frame) + program.control_word, kTypedContinuationControlWords, 0);
    }
    gpu.Dispatch(0, 0, count + 3);
    for (uint32_t id = 0; id < count; ++id) {
        auto frame = bytes + frame_offset + id * program.frame_stride_bytes;
        auto control = reinterpret_cast<uint32_t *>(frame) + program.control_word;
        Require(control[1] == 3 && control[2] == 0 && control[20] >= 256 && control[21] == 4,
                "BDA source did not issue a typed callable request");
        Require(Read<uint32_t>(frame, control[20]) == integers[id], "System builtin/record/push transport changed source value");
    }
    std::vector<uint32_t> blocked(gpu.mapped, gpu.mapped + total / 4);
    gpu.Dispatch(0, 0, count + 3);
    Require(std::equal(blocked.begin(), blocked.end(), gpu.mapped), "BDA step resumed before callable completion");
    for (uint32_t id = 0; id < count; ++id) {
        auto frame = bytes + frame_offset + id * program.frame_stride_bytes;
        auto control = reinterpret_cast<uint32_t *>(frame) + program.control_word;
        Write<uint32_t>(frame, control[20], integers[id] + 1000);
        control[2] = 1;
    }
    gpu.Dispatch(0, 0, count + 3);
    for (uint32_t id = 0; id < count; ++id) {
        auto frame = bytes + frame_offset + id * program.frame_stride_bytes;
        Require(reinterpret_cast<uint32_t *>(frame)[program.control_word] == UINT32_MAX, "BDA stage did not terminate");
        auto expected = floats[id] + float(integers[id]);
        expected += float(integers[id] + 1000);
        Require(Read<float>(bytes, output_offset + id * 4) == expected,
                "BDA source scalar/matrix builtin snapshot result mismatch");
    }
    // A stride above4GiB must skip invocation1 by its real uint64 extent,
    // rather than wrap and alias the second small frame in this allocation.
    Write<uint64_t>(bytes, context_offset + VEL_RT_CTX_FRAME_STRIDE, (uint64_t(1) << 32) + program.frame_stride_bytes);
    auto frame0 = bytes + frame_offset;
    std::fill_n(reinterpret_cast<uint32_t *>(frame0) + program.control_word, kTypedContinuationControlWords, 0);
    std::vector<unsigned char> second(bytes + frame_offset + program.frame_stride_bytes, bytes + frame_end);
    gpu.Dispatch(0, 0, 2);
    Require(reinterpret_cast<uint32_t *>(frame0)[program.control_word + 1] == 3, "64-bit stride incorrectly rejected invocation0");
    Require(std::equal(second.begin(), second.end(), bytes + frame_offset + program.frame_stride_bytes),
            "BDA stride truncated above4GiB");
    Write<uint64_t>(bytes, context_offset + VEL_RT_CTX_FRAME_OFFSET, UINT64_MAX - 15);
    Write<uint64_t>(bytes, context_offset + VEL_RT_CTX_FRAME_CAPACITY, UINT64_MAX);
    blocked.assign(gpu.mapped, gpu.mapped + total / 4);
    gpu.Dispatch(0, 0, 2);
    Require(std::equal(blocked.begin(), blocked.end(), gpu.mapped), "BDA frame arithmetic overflow touched memory");
    std::cout << "BDA source/system ABI executed:8 frames, nonzero512-byte pool offset, all17 RT builtin variables/transforms, "
                 "shader records,128-byte source push snapshots, occupied app bindings0/1, late completion, uint64 counts/strides, "
                 "exported SPIR-V1.5 stage\n";
    return true;
}
bool AnyHitDisposition() {
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint AnyHitKHR %main "main" %launch %payload
OpDecorate %launch BuiltIn LaunchIdKHR
OpDecorate %payload Location 7
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%v3u = OpTypeVector %u32 3
%input = OpTypePointer Input %v3u
%incoming = OpTypePointer IncomingRayPayloadKHR %u32
%launch = OpVariable %input Input
%payload = OpVariable %incoming IncomingRayPayloadKHR
%zero = OpConstant %u32 0
%one = OpConstant %u32 1
%ten = OpConstant %u32 10
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%launch_value = OpLoad %v3u %launch
%id = OpCompositeExtract %u32 %launch_value 0
%updated = OpIAdd %u32 %id %ten
OpStore %payload %updated
%bit = OpBitwiseAnd %u32 %id %one
%reject = OpIEqual %bool %bit %zero
OpSelectionMerge %unused None
OpBranchConditional %reject %ignore %terminate
%ignore = OpLabel
OpIgnoreIntersectionKHR
%terminate = OpLabel
OpTerminateRayKHR
%unused = OpLabel
OpUnreachable
OpFunctionEnd
)spv";
    TypedContinuationCompileOptions options;
    options.transport = TypedFrameTransport::PhysicalAddress;
    auto program = CompileTypedContinuationSteps(Assemble(source), "main", spv::ExecutionModelAnyHitKHR, options);
    const auto &payload = program.invocation_variables.at(0);
    constexpr uint32_t count = 2, frame_offset = 256;
    auto bytes_count = frame_offset + program.frame_stride_bytes * count;
    GPU gpu;
    if (!gpu.Init(program.spirv, uint32_t(bytes_count), VK_WHOLE_SIZE, false, VK_WHOLE_SIZE, 0)) return false;
    auto bytes = reinterpret_cast<unsigned char *>(gpu.mapped);
    Write<uint64_t>(bytes, VEL_RT_CTX_FRAMES_ADDRESS, gpu.address);
    Write<uint64_t>(bytes, VEL_RT_CTX_FRAME_STRIDE, program.frame_stride_bytes);
    Write<uint64_t>(bytes, VEL_RT_CTX_FRAME_COUNT, count);
    Write<uint64_t>(bytes, VEL_RT_CTX_FRAME_OFFSET, frame_offset);
    Write<uint64_t>(bytes, VEL_RT_CTX_FRAME_CAPACITY, bytes_count);
    for (uint32_t id = 0; id < count; ++id) {
        auto frame = bytes + frame_offset + id * program.frame_stride_bytes;
        Write<uint32_t>(frame, VEL_RT_SYS_LAUNCH_ID, id);
        std::fill_n(reinterpret_cast<uint32_t *>(frame) + program.control_word, kTypedContinuationControlWords, 0);
    }
    gpu.Dispatch(0, 0, count);
    for (uint32_t id = 0; id < count; ++id) {
        auto frame = bytes + frame_offset + id * program.frame_stride_bytes;
        auto control = reinterpret_cast<uint32_t *>(frame) + program.control_word;
        Require(control[0] == UINT32_MAX && control[1] == (id == 0 ? 4u : 5u) && control[2] == 0,
                "Any-hit terminal disposition was resumable or lost");
        Require(Read<uint32_t>(frame, payload.offset) == id + 10, "Any-hit payload write lost before terminal disposition");
    }
    std::vector<uint32_t> complete(gpu.mapped, gpu.mapped + bytes_count / 4);
    gpu.Dispatch(0, 0, count);
    Require(std::equal(complete.begin(), complete.end(), gpu.mapped), "Terminated any-hit stage ran again");
    std::cout << "Any-hit source IgnoreIntersection/TerminateRay emitted terminal dispositions4/5 with payload writes preserved\n";
    return true;
}
bool EmptyLiveSuspension() {
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %data
OpDecorate %data Location 3
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%pointer = OpTypePointer CallableDataKHR %uint
%data = OpVariable %pointer CallableDataKHR
%zero = OpConstant %uint 0
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
OpStore %data %zero
OpExecuteCallableKHR %zero %data
OpReturn
OpFunctionEnd
)spv";
    auto binary = Assemble(source);
    auto descriptor = CompileTypedContinuationSteps(binary, "main", spv::ExecutionModelRayGenerationKHR);
    Require(descriptor.frames.functions[0].suspensions[0].saved_values.empty(), "Empty-live regression retained SSA");
    TypedContinuationCompileOptions options;
    options.transport = TypedFrameTransport::PhysicalAddress;
    auto program = CompileTypedContinuationSteps(binary, "main", spv::ExecutionModelRayGenerationKHR, options);
    options.emit_compute_entry = false;
    options.step_export_name = "vel_rt_empty_live";
    CompileTypedContinuationSteps(binary, "main", spv::ExecutionModelRayGenerationKHR, options);
    constexpr uint32_t frame_offset = 256;
    auto bytes_count = frame_offset + program.frame_stride_bytes;
    GPU gpu;
    if (!gpu.Init(program.spirv, uint32_t(bytes_count), VK_WHOLE_SIZE, false, VK_WHOLE_SIZE, 0)) return false;
    auto bytes = reinterpret_cast<unsigned char *>(gpu.mapped);
    Write<uint64_t>(bytes, VEL_RT_CTX_FRAMES_ADDRESS, gpu.address);
    Write<uint64_t>(bytes, VEL_RT_CTX_FRAME_STRIDE, program.frame_stride_bytes);
    Write<uint64_t>(bytes, VEL_RT_CTX_FRAME_COUNT, 1);
    Write<uint64_t>(bytes, VEL_RT_CTX_FRAME_OFFSET, frame_offset);
    Write<uint64_t>(bytes, VEL_RT_CTX_FRAME_CAPACITY, bytes_count);
    auto frame = bytes + frame_offset;
    auto control = reinterpret_cast<uint32_t *>(frame) + program.control_word;
    std::fill_n(control, kTypedContinuationControlWords, 0);
    gpu.Dispatch(0, 0, 1);
    Require(control[1] == 3 && control[21] == 4 && Read<uint32_t>(frame, control[20]) == 0, "Empty-live source did not suspend");
    Write<uint32_t>(frame, control[20], 123);
    control[2] = 1;
    gpu.Dispatch(0, 0, 1);
    Require(control[0] == UINT32_MAX && Read<uint32_t>(frame, program.invocation_variables[0].offset) == 123,
            "Empty-live resume lost completed callable data");
    return true;
}
}  // namespace
int main() {
    try {
        if (!Execute() || !AnyHitDisposition() || !EmptyLiveSuspension()) {
            std::cout << "BDA source validated; device address execution unavailable\n";
            return 77;
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
