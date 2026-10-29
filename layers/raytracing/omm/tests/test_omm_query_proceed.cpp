// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#include "../omm_query_rewrite.h"
#include "../omm_opacity.h"
#include "native_fixture.h"
#include <spirv-tools/libspirv.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace vel::omm;

namespace {
uint32_t checks = 0;
void require(bool value, const std::string& message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}
std::string base(const std::string& body, const std::string& helper = {}, const std::string& globals = {},
                 const std::string& interfaces = {}) {
    return "OpCapability Shader\nOpCapability RayQueryKHR\nOpExtension \"SPV_KHR_ray_query\"\n"
           "OpMemoryModel Logical GLSL450\nOpEntryPoint GLCompute %main \"main\" %scene " +
           interfaces +
           "\nOpExecutionMode %main LocalSize 1 1 1\n"
           "OpDecorate %scene DescriptorSet 0\nOpDecorate %scene Binding 0\n"
           "%void = OpTypeVoid\n%bool = OpTypeBool\n%u = OpTypeInt 32 0\n%f = OpTypeFloat 32\n"
           "%v2u = OpTypeVector %u 2\n%v2f = OpTypeVector %f 2\n%v3f = OpTypeVector %f 3\n"
           "%u0 = OpConstant %u 0\n%u1 = OpConstant %u 1\n%u2 = OpConstant %u 2\n%u255 = OpConstant %u 255\n"
           "%f0 = OpConstant %f 0\n%f1 = OpConstant %f 1\n%true = OpConstantTrue %bool\n"
           "%origin = OpConstantComposite %v3f %f0 %f0 %f0\n%direction = OpConstantComposite %v3f %f0 %f0 %f1\n"
           "%2001 = OpConstantComposite %v2u %u1 %u0\n%query = OpTypeRayQueryKHR\n"
           "%pFQ = OpTypePointer Function %query\n%pPQ = OpTypePointer Private %query\n"
           "%array = OpTypeArray %query %u2\n%pFA = OpTypePointer Function %array\n"
           "%pPA = OpTypePointer Private %array\n%as = OpTypeAccelerationStructureKHR\n"
           "%pAS = OpTypePointer UniformConstant %as\n%scene = OpVariable %pAS UniformConstant\n"
           "%fn = OpTypeFunction %void\n%fnClassify = OpTypeFunction %u %v2u %u %u %u %v2f %bool %u\n" +
           globals +
           "%2000 = OpFunction %u None %fnClassify\n%cm = OpFunctionParameter %v2u\n%ci = OpFunctionParameter %u\n"
           "%cg = OpFunctionParameter %u\n%cp = OpFunctionParameter %u\n%cb = OpFunctionParameter %v2f\n"
           "%ct = OpFunctionParameter %bool\n%cf = OpFunctionParameter %u\n%ce = OpLabel\n"
           "%decision = OpCompositeExtract %u %cm 0\nOpReturnValue %decision\nOpFunctionEnd\n" +
           helper + "%main = OpFunction %void None %fn\n%entry = OpLabel\n" + body + "\nOpFunctionEnd\n";
}
const char* initialize = "OpRayQueryInitializeKHR %q %loaded %u1 %u255 %origin %f0 %direction %f1\n";

QueryStateModule run(const std::string& name, const std::string& text, uint32_t initializes, uint32_t proceeds,
                     bool explicit_metadata = false, bool ir_only = false) {
    spvtools::SpirvTools tools(ir_only ? SPV_ENV_UNIVERSAL_1_5 : SPV_ENV_VULKAN_1_2);
    std::string diagnostic;
    tools.SetMessageConsumer(
        [&](spv_message_level_t, const char*, const spv_position_t&, const char* message) { diagnostic = message; });
    std::vector<uint32_t> input;
    require(tools.Assemble(text, &input, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS), name + " assemble: " + diagnostic);
    require(tools.Validate(input), name + " input validation: " + diagnostic);
    QueryConsumerOptions options;
    options.classify_function = 2000;
    options.use_constant_metadata = !explicit_metadata;
    options.constant_metadata_low = 1;
    if (explicit_metadata)
        for (uint32_t offset = 5; offset < input.size(); offset += input[offset] >> 16)
            if (spv::Op(input[offset] & 0xffffu) == spv::OpRayQueryInitializeKHR)
                options.initialization_metadata.push_back({offset, 2001});
    QueryStateModule output;
    if (ir_only) {
        // VUID-StandaloneSpirv-None-04667 prohibits a struct containing an
        // opaque ray query, including through an array. This is only a general
        // SPIR-V aggregate/aliasing proof, never a legal Vulkan shader or GPU
        // fixture. Some older linked SPIRV-Tools versions missed this rule.
        spvtools::SpirvTools vulkan(SPV_ENV_VULKAN_1_2);
        std::string vulkan_diagnostic;
        vulkan.SetMessageConsumer(
            [&](spv_message_level_t, const char*, const spv_position_t&, const char* message) { vulkan_diagnostic = message; });
        if (!vulkan.Validate(input)) {
            require(vulkan_diagnostic.find("opaque") != std::string::npos, name + " Vulkan rejects the opaque aggregate");
            output.words = {123, 456};
            require(rewrite_query_consumers(input, options, &output, &diagnostic) == QueryRewriteStatus::InvalidModule &&
                        output.words == std::vector<uint32_t>({123, 456}),
                    name + " prohibited Vulkan input rejected atomically");
            std::printf("PASS IR-only aggregate: %s (Vulkan input rejected; not GPU coverage)\n", name.c_str());
            return output;
        }
    }
    require(rewrite_query_consumers(input, options, &output, &diagnostic) == QueryRewriteStatus::Success,
            name + " transform: " + diagnostic);
    require(tools.Validate(output.words), name + " output validation: " + diagnostic);
    require(output.initializations == initializes && output.proceeds == proceeds && output.typed_state_type != 0,
            name + " operation inventory");
    for (const auto& binding : output.state)
        require(binding.state_pointer && !binding.flags_pointer, name + " typed state mapping");
    uint32_t native_proceeds = 0, scalar_calls = 0;
    for (size_t offset = 5; offset < output.words.size(); offset += output.words[offset] >> 16) {
        const auto op = spv::Op(output.words[offset] & 0xffffu);
        if (op == spv::OpRayQueryProceedKHR) ++native_proceeds;
        if (op == spv::OpFunctionCall && output.words[offset + 3] == 2000) {
            ++scalar_calls;
            require((output.words[offset] >> 16) == 11, name + " classification scalar ABI");
        }
    }
    require(native_proceeds == proceeds && scalar_calls == proceeds, name + " native query retained and classifier injected");
    if (ir_only)
        std::printf("PASS IR-only aggregate: %s (legacy validator; not legal Vulkan input or GPU coverage)\n", name.c_str());
    else
        std::printf("PASS automatic Proceed graph: %s\n", name.c_str());
    return output;
}
void rejection_tests() {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::vector<uint32_t> input;
    const auto text = base("%q = OpVariable %pFQ Function\n%loaded = OpLoad %as %scene\n" + std::string(initialize) +
                           "%p = OpRayQueryProceedKHR %bool %q\nOpReturn");
    require(tools.Assemble(text, &input, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS) && tools.Validate(input),
            "negative input");
    const auto saved_input = input;
    QueryStateModule output;
    output.words = {123, 456};
    output.initializations = 99;
    QueryConsumerOptions options;
    options.classify_function = 2000;
    std::string diagnostic;
    require(rewrite_query_consumers(input, options, &output, &diagnostic) == QueryRewriteStatus::Unsupported &&
                output.words == std::vector<uint32_t>({123, 456}) && output.initializations == 99,
            "missing metadata fails atomically");
    options.use_constant_metadata = true;
    options.classify_function = 2001;
    require(rewrite_query_consumers(input, options, &output, &diagnostic) == QueryRewriteStatus::Unsupported &&
                output.initializations == 99,
            "nonfunction classifier fails atomically");
    options.classify_function = 2000;
    uint32_t initialize_offset = 0;
    for (uint32_t offset = 5; offset < input.size(); offset += input[offset] >> 16)
        if (spv::Op(input[offset] & 0xffffu) == spv::OpRayQueryInitializeKHR) initialize_offset = offset;
    options.initialization_metadata = {{initialize_offset, 2000}};
    require(rewrite_query_consumers(input, options, &output, &diagnostic) == QueryRewriteStatus::Unsupported,
            "metadata value must be uvec2");
    options.initialization_metadata = {{initialize_offset, 2001}, {initialize_offset, 2001}};
    require(rewrite_query_consumers(input, options, &output, &diagnostic) == QueryRewriteStatus::Unsupported,
            "duplicate metadata binding rejected");
    options.initialization_metadata = {{initialize_offset + 1, 2001}};
    require(rewrite_query_consumers(input, options, &output, &diagnostic) == QueryRewriteStatus::Unsupported,
            "metadata offset must name initialize");
    options.initialization_metadata.clear();
    auto invalid = input;
    invalid[0] = 0;
    require(rewrite_query_consumers(invalid, options, &output, &diagnostic) == QueryRewriteStatus::InvalidModule &&
                output.words == std::vector<uint32_t>({123, 456}) && output.initializations == 99,
            "invalid module leaves all output unchanged");
    require(input == saved_input, "source immutable across failures");
    require(rewrite_query_consumers(input, options, nullptr) == QueryRewriteStatus::InvalidModule, "null output rejected");
    const auto late = base("%q = OpVariable %pFQ Function\n%loaded = OpLoad %as %scene\n" + std::string(initialize) +
                           "%2003 = OpCompositeConstruct %v2u %u1 %u0\n%p = OpRayQueryProceedKHR %bool %q\nOpReturn");
    require(tools.Assemble(late, &input, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS) && tools.Validate(input),
            "late metadata source valid");
    for (uint32_t offset = 5; offset < input.size(); offset += input[offset] >> 16)
        if (spv::Op(input[offset] & 0xffffu) == spv::OpRayQueryInitializeKHR) options.initialization_metadata = {{offset, 2003}};
    require(rewrite_query_consumers(input, options, &output, &diagnostic) == QueryRewriteStatus::Unsupported &&
                output.initializations == 99,
            "final validator rejects non-dominating metadata atomically");
    auto callback_query = base("%q = OpVariable %pFQ Function\n%loaded = OpLoad %as %scene\n" + std::string(initialize) +
                                   "%p = OpRayQueryProceedKHR %bool %q\nOpReturn",
                               {}, "%globalQ = OpVariable %pPQ Private\n", "%globalQ");
    const auto callback_return = callback_query.find("OpReturnValue %decision");
    callback_query.insert(callback_return, "%forbidden = OpRayQueryGetRayFlagsKHR %u %globalQ\n");
    require(tools.Assemble(callback_query, &input, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS) && tools.Validate(input),
            "callback query source valid");
    options.initialization_metadata.clear();
    require(rewrite_query_consumers(input, options, &output, &diagnostic) == QueryRewriteStatus::Unsupported &&
                output.initializations == 99,
            "classifier may not recursively consume query state");
}
std::string gpu_source() {
    const std::string extra =
        "%u3 = OpConstant %u 3\n%u4 = OpConstant %u 4\n%u5 = OpConstant %u 5\n%u6 = OpConstant %u 6\n"
        "%u7 = OpConstant %u 7\n%u8 = OpConstant %u 8\n%u9 = OpConstant %u 9\n%u10 = OpConstant %u 10\n"
        "%u11 = OpConstant %u 11\n%u16 = OpConstant %u 16\n%u30 = OpConstant %u 30\n%u31 = OpConstant %u 31\n"
        "%u32 = OpConstant %u 32\n%umax = OpConstant %u 4294967295\n%tag = OpConstant %u 1330466131\n"
        "%minusOneBits = OpConstant %u 3212836864\n%fquarter = OpConstant %f 0.25\n%f325 = OpConstant %f 3.25\n"
        "%fminus1 = OpConstant %f -1\n%f15 = OpConstant %f 1.5\n%f3 = OpConstant %f 3\n"
        "%triOrigin = OpConstantComposite %v3f %fquarter %fquarter %fminus1\n"
        "%boxOrigin = OpConstantComposite %v3f %f325 %fquarter %fminus1\n"
        "%outputArray = OpTypeArray %u %u32\n%outputBlock = OpTypeStruct %outputArray\n"
        "%pSB = OpTypePointer StorageBuffer %outputBlock\n%pSBu = OpTypePointer StorageBuffer %u\n"
        "%pFu = OpTypePointer Function %u\n%output = OpVariable %pSB StorageBuffer\n"
        "%workerType = OpTypeFunction %void %pFA %as %u %v2u %u %u %u\n";
    uint32_t next_store = 0;
    auto store = [&](const std::string& offset, const std::string& value) {
        const auto suffix = std::to_string(next_store++);
        return "%si" + suffix + " = OpIAdd %u %wbase " + offset + "\n%sp" + suffix + " = OpAccessChain %pSBu %output %u0 %si" +
               suffix + "\nOpStore %sp" + suffix + " " + value + "\n";
    };
    const std::string worker =
        "%worker = OpFunction %void None %workerType\n%wqueries = OpFunctionParameter %pFA\n"
        "%was = OpFunctionParameter %as\n%wflags = OpFunctionParameter %u\n%2002 = OpFunctionParameter %v2u\n"
        "%windex = OpFunctionParameter %u\n%wbase = OpFunctionParameter %u\n%wmode = OpFunctionParameter %u\n"
        "%we = OpLabel\n%visibleCount = OpVariable %pFu Function\n%aabbValue = OpVariable %pFu Function\n"
        "OpStore %visibleCount %u0\nOpStore %aabbValue %u9\n%wq = OpAccessChain %pFQ %wqueries %windex\n"
        "%boxMode = OpBitwiseAnd %u %wmode %u1\n%isBox = OpINotEqual %bool %boxMode %u0\n"
        "%rayOrigin = OpSelect %v3f %isBox %boxOrigin %triOrigin\n"
        "%shortMode = OpBitwiseAnd %u %wmode %u2\n%isShort = OpINotEqual %bool %shortMode %u0\n"
        "%rayMax = OpSelect %f %isShort %f15 %f3\n"
        "OpRayQueryInitializeKHR %wq %was %wflags %u255 %rayOrigin %f0 %direction %rayMax\n"
        "OpBranch %wloop\n%wloop = OpLabel\n%wproceed = OpRayQueryProceedKHR %bool %wq\n"
        "OpLoopMerge %wend %wcontinue None\nOpBranchConditional %wproceed %wbody %wend\n%wbody = OpLabel\n"
        "%previous = OpLoad %u %visibleCount\n%incremented = OpIAdd %u %previous %u1\nOpStore %visibleCount %incremented\n"
        "%candidateFlags = OpRayQueryGetRayFlagsKHR %u %wq\n" +
        store("%u11", "%candidateFlags") +
        "%candidateType = OpRayQueryGetIntersectionTypeKHR %u %wq %u0\n%isTri = OpIEqual %bool %candidateType %u0\n"
        "OpSelectionMerge %candidateEnd None\nOpBranchConditional %isTri %confirm %generate\n%confirm = OpLabel\n"
        "OpRayQueryConfirmIntersectionKHR %wq\nOpBranch %candidateEnd\n%generate = OpLabel\n"
        "%aabbOpaque = OpRayQueryGetIntersectionCandidateAABBOpaqueKHR %bool %wq\n"
        "%aabbInteger = OpSelect %u %aabbOpaque %u1 %u0\nOpStore %aabbValue %aabbInteger\n"
        "OpRayQueryGenerateIntersectionKHR %wq %f1\nOpBranch %candidateEnd\n%candidateEnd = OpLabel\n"
        "OpBranch %wcontinue\n%wcontinue = OpLabel\nOpBranch %wloop\n%wend = OpLabel\n"
        "%finalFlags = OpRayQueryGetRayFlagsKHR %u %wq\n%count = OpLoad %u %visibleCount\n"
        "%committed = OpRayQueryGetIntersectionTypeKHR %u %wq %u1\n%finalAabb = OpLoad %u %aabbValue\n" +
        store("%u5", "%finalFlags") + store("%u6", "%count") + store("%u7", "%committed") + store("%u8", "%umax") +
        store("%u9", "%minusOneBits") + store("%u10", "%finalAabb") +
        "%hasHit = OpINotEqual %bool %committed %u0\nOpSelectionMerge %afterHit None\n"
        "OpBranchConditional %hasHit %hit %afterHit\n%hit = OpLabel\n"
        "%instance = OpRayQueryGetIntersectionInstanceIdKHR %u %wq %u1\n"
        "%hitT = OpRayQueryGetIntersectionTKHR %f %wq %u1\n%hitBits = OpBitcast %u %hitT\n" +
        store("%u8", "%instance") + store("%u9", "%hitBits") + "OpBranch %afterHit\n%afterHit = OpLabel\nOpReturn\nOpFunctionEnd\n";
    const std::string main =
        "%queries = OpVariable %pFA Function\n%loaded = OpLoad %as %scene\n"
        "%fp = OpAccessChain %pSBu %output %u0 %u0\n%flags = OpLoad %u %fp\n"
        "%mp = OpAccessChain %pSBu %output %u0 %u1\n%packed = OpLoad %u %mp\n"
        "%tp = OpAccessChain %pSBu %output %u0 %u2\n%contextTag = OpLoad %u %tp\n"
        "%ip = OpAccessChain %pSBu %output %u0 %u3\n%seed = OpLoad %u %ip\n%index = OpBitwiseAnd %u %seed %u1\n"
        "%op = OpAccessChain %pSBu %output %u0 %u4\n%mode = OpLoad %u %op\n"
        "%context = OpCompositeConstruct %v2u %packed %contextTag\n"
        "%call0 = OpFunctionCall %void %worker %queries %loaded %flags %context %index %u0 %mode\n"
        "%other = OpBitwiseXor %u %index %u1\n%flags2 = OpBitwiseOr %u %flags %u8\n"
        "%call1 = OpFunctionCall %void %worker %queries %loaded %flags2 %context %other %u16 %mode\n"
        "%firstQ = OpAccessChain %pFQ %queries %index\n%secondQ = OpAccessChain %pFQ %queries %other\n"
        "%firstFlags = OpRayQueryGetRayFlagsKHR %u %firstQ\n%secondFlags = OpRayQueryGetRayFlagsKHR %u %secondQ\n"
        "%firstOut = OpAccessChain %pSBu %output %u0 %u30\nOpStore %firstOut %firstFlags\n"
        "%secondOut = OpAccessChain %pSBu %output %u0 %u31\nOpStore %secondOut %secondFlags\nOpReturn";
    auto text = base(main, worker, extra, "%output");
    const auto annotations = text.find("%void = OpTypeVoid");
    text.insert(annotations,
                "OpDecorate %outputArray ArrayStride 4\nOpMemberDecorate %outputBlock 0 Offset 0\n"
                "OpDecorate %outputBlock Block\nOpDecorate %output DescriptorSet 0\nOpDecorate %output Binding 1\n");
    const std::string old_callback = "%decision = OpCompositeExtract %u %cm 0\nOpReturnValue %decision";
    const std::string callback =
        "%decisions = OpCompositeExtract %u %cm 0\n%shift = OpIMul %u %ci %u2\n"
        "%shifted = OpShiftRightLogical %u %decisions %shift\n%decision = OpBitwiseAnd %u %shifted %u3\n"
        "%receivedTag = OpCompositeExtract %u %cm 1\n%correctTag = OpIEqual %bool %receivedTag %tag\n"
        "%answer = OpSelect %u %correctTag %decision %u0\nOpReturnValue %answer";
    text.replace(text.find(old_callback), old_callback.size(), callback);
    text.insert(0, "OpCapability RayTracingOpacityMicromapEXT\n");
    text.insert(text.find("OpMemoryModel"), "OpExtension \"SPV_EXT_opacity_micromap\"\n");
    return text;
}
int gpu_tests() {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string diagnostic;
    tools.SetMessageConsumer(
        [&](spv_message_level_t, const char*, const spv_position_t&, const char* message) { diagnostic = message; });
    std::vector<uint32_t> source;
    require(tools.Assemble(gpu_source(), &source, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS),
            "GPU source assemble: " + diagnostic);
    require(tools.Validate(source), "GPU source validation: " + diagnostic);
    QueryConsumerOptions options;
    options.classify_function = 2000;
    for (uint32_t offset = 5; offset < source.size(); offset += source[offset] >> 16)
        if (spv::Op(source[offset] & 0xffffu) == spv::OpRayQueryInitializeKHR)
            options.initialization_metadata.push_back({offset, 2002});
    QueryStateModule transformed;
    require(rewrite_query_consumers(source, options, &transformed, &diagnostic) == QueryRewriteStatus::Success,
            "GPU automatic consumer transform: " + diagnostic);
    require(tools.Validate(transformed.words) && transformed.proceeds == 1 && transformed.aabb_getters == 1 &&
                transformed.flag_getters == 4,
            "GPU transformed operations and validation: " + diagnostic);
    Fixture fixture;
    if (!fixture.open()) {
        std::puts("SKIP: native AS/RQ/BDA/shaderInt64 fixture prerequisites unavailable");
        return 77;
    }
    const auto input_usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    auto vertices = fixture.buffer(9 * sizeof(float), input_usage, true);
    const float triangle[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::memcpy(vertices.mapped, triangle, sizeof(triangle));
    VkAccelerationStructureGeometryKHR geometries[3]{};
    geometries[0] = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    geometries[0].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometries[0].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& triangles = geometries[0].geometry.triangles;
    triangles =
        init<VkAccelerationStructureGeometryTrianglesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertices.address;
    triangles.vertexStride = 12;
    triangles.maxVertex = 2;
    triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    auto triangle_as = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometries[0]);
    auto aabbs = fixture.buffer(sizeof(VkAabbPositionsKHR), input_usage, true);
    *static_cast<VkAabbPositionsKHR*>(aabbs.mapped) = {3, 0, 0, 4, 1, 0.5};
    geometries[1] = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    geometries[1].geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometries[1].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometries[1].geometry.aabbs =
        init<VkAccelerationStructureGeometryAabbsDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR);
    geometries[1].geometry.aabbs.data.deviceAddress = aabbs.address;
    geometries[1].geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
    auto box_as = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometries[1]);
    auto instances = fixture.buffer(3 * sizeof(VkAccelerationStructureInstanceKHR), input_usage, true);
    auto* values = static_cast<VkAccelerationStructureInstanceKHR*>(instances.mapped);
    for (uint32_t i = 0; i < 3; ++i) {
        values[i] = {};
        for (uint32_t axis = 0; axis < 3; ++axis) values[i].transform.matrix[axis][axis] = 1;
        values[i].transform.matrix[2][3] = i == 1 ? 1.f : 0.f;
        values[i].mask = 255;
        values[i].instanceCustomIndex = 77;
        values[i].accelerationStructureReference = i == 2 ? box_as.address : triangle_as.address;
    }
    geometries[2] = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    geometries[2].geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometries[2].geometry.instances =
        init<VkAccelerationStructureGeometryInstancesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
    geometries[2].geometry.instances.data.deviceAddress = instances.address;
    auto tlas = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geometries[2], 3);
    auto command = fixture.begin();
    Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                     VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const auto* triangle_range = &triangle_as.range;
    const auto* box_range = &box_as.range;
    fixture.buildAS(command, 1, &triangle_as.build, &triangle_range);
    fixture.buildAS(command, 1, &box_as.build, &box_range);
    Fixture::barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const auto* tlas_range = &tlas.range;
    fixture.buildAS(command, 1, &tlas.build, &tlas_range);
    Fixture::barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    fixture.submit(command);
    auto output = fixture.buffer(32 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    auto descriptor_info = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    descriptor_info.bindingCount = 2;
    descriptor_info.pBindings = bindings;
    VkDescriptorSetLayout descriptor_layout;
    CHECK(vkCreateDescriptorSetLayout(fixture.device, &descriptor_info, nullptr, &descriptor_layout));
    auto layout_info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_layout;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(fixture.device, &layout_info, nullptr, &layout));
    const VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    auto pool_info = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(fixture.device, &pool_info, nullptr, &pool));
    auto allocate = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_layout;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(fixture.device, &allocate, &set));
    auto as_write =
        init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &tlas.handle;
    VkDescriptorBufferInfo buffer_info{output.handle, 0, 32 * sizeof(uint32_t)};
    VkWriteDescriptorSet writes[] = {init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET),
                                     init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET)};
    writes[0].pNext = &as_write;
    writes[0].dstSet = set;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(fixture.device, 2, writes, 0, nullptr);
    auto module_info = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    module_info.codeSize = transformed.words.size() * 4;
    module_info.pCode = transformed.words.data();
    VkShaderModule module;
    CHECK(vkCreateShaderModule(fixture.device, &module_info, nullptr, &module));
    auto pipeline_info = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipeline_info.layout = layout;
    pipeline_info.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));
    uint32_t dispatches = 0;
    auto exercise = [&](uint32_t flags, uint32_t state, uint32_t instance_flags, uint32_t mode) {
        Decision decisions[3];
        require(resolve_opacity({state < 4, static_cast<State>(state), flags, instance_flags, 1}, &decisions[0]) == Status::Success,
                "near opacity CPU oracle");
        require(resolve_opacity({false, State::Opaque, flags, instance_flags, 1}, &decisions[1]) == Status::Success,
                "far opacity CPU oracle");
        decisions[2] = decisions[1];
        uint32_t packed = 0;
        for (uint32_t i = 0; i < 3; ++i) packed |= static_cast<uint32_t>(decisions[i]) << (2 * i);
        const uint32_t selected = mode & 1 ? 2 : (decisions[0] == Decision::Ignore && !(mode & 2) ? 1 : 0);
        const Decision decision = decisions[selected];
        const uint32_t hit = decision == Decision::Ignore ? 0 : (selected == 2 ? 2 : 1);
        const uint32_t visible = decision == Decision::NonOpaque || (selected == 2 && hit) ? 1 : 0;
        for (uint32_t seed = 0; seed < 2; ++seed) {
            auto* result = static_cast<uint32_t*>(output.mapped);
            std::fill(result, result + 32, UINT32_MAX);
            result[0] = flags;
            result[1] = packed;
            result[2] = 1330466131;
            result[3] = seed;
            result[4] = mode;
            command = fixture.begin();
            Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(command, 1, 1, 1);
            Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_ACCESS_HOST_READ_BIT);
            fixture.submit(command);
            for (uint32_t call = 0; call < 2; ++call) {
                const uint32_t offset = call * 16;
                float t;
                std::memcpy(&t, result + offset + 9, sizeof(t));
                const uint32_t logical = call ? flags | 8u : flags;
                const uint32_t aabb = selected == 2 && hit ? uint32_t(decision == Decision::Opaque) : 9;
                if (result[offset + 5] != logical || result[offset + 6] != visible || result[offset + 7] != hit ||
                    result[offset + 8] != (hit ? selected : UINT32_MAX) ||
                    std::fabs(t - (hit ? (selected == 1 ? 2.f : 1.f) : -1.f)) > 0.00001f || result[offset + 10] != aabb ||
                    result[offset + 11] != (visible ? logical : UINT32_MAX) || result[30] != flags || result[31] != (flags | 8u)) {
                    std::fprintf(stderr,
                                 "GPU flags=%u state=%u instance=%u mode=%u seed=%u call=%u got "
                                 "flags/visible/hit/id/aabb=%u/%u/%u/%u/%u t=%f\n",
                                 flags, state, instance_flags, mode, seed, call, result[offset + 5], result[offset + 6],
                                 result[offset + 7], result[offset + 8], result[offset + 10], t);
                    require(false, "automatic Proceed native GPU result");
                }
                ++checks;
            }
            ++dispatches;
        }
    };
    const uint32_t ray_flags[] = {0, 1, 2, 64, 128};
    const uint32_t instance_flags[] = {0, 4, 8, 16, 20, 24, 32, 36, 40, 48};
    for (uint32_t state = 0; state <= 4; ++state)
        for (auto ray : ray_flags)
            for (uint32_t force_two = 0; force_two < 2; ++force_two)
                for (auto instance : instance_flags) exercise(ray | (force_two ? 1024u : 0u), state, instance, 2);
    exercise(0, 0, 0, 0);
    exercise(1, 0, 0, 0);
    exercise(2, 0, 0, 0);
    exercise(4, 1, 0, 2);
    exercise(4, 2, 0, 2);
    for (auto ray : ray_flags) exercise(ray, 0, 0, 1);
    vkDestroyPipeline(fixture.device, pipeline, nullptr);
    vkDestroyShaderModule(fixture.device, module, nullptr);
    vkDestroyDescriptorPool(fixture.device, pool, nullptr);
    vkDestroyPipelineLayout(fixture.device, layout, nullptr);
    vkDestroyDescriptorSetLayout(fixture.device, descriptor_layout, nullptr);
    std::printf(
        "Automatic Proceed native GPU: %u dispatches, %u traces PASS (array elements/helper metadata/flags/AABB/first-hit)\n",
        dispatches, dispatches * 2);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string scalar = "%q = OpVariable %pFQ Function\n%loaded = OpLoad %as %scene\n";
    run("straight block and getters",
        base(scalar + initialize +
             "%p = OpRayQueryProceedKHR %bool %q\n"
             "%flags = OpRayQueryGetRayFlagsKHR %u %q\n"
             "%opaque = OpRayQueryGetIntersectionCandidateAABBOpaqueKHR %bool %q\nOpReturn"),
        1, 1, true);
    run("multiple proceeds and reinitialization",
        base(scalar + initialize + "%p = OpRayQueryProceedKHR %bool %q\n" + initialize +
             "%p2 = OpRayQueryProceedKHR %bool %q\nOpReturn"),
        2, 2);
    run("source while condition",
        base(scalar + initialize +
             "OpBranch %loop\n%loop = OpLabel\n"
             "OpLoopMerge %end %continue None\nOpBranch %condition\n%condition = OpLabel\n"
             "%p = OpRayQueryProceedKHR %bool %q\nOpBranchConditional %p %body %end\n"
             "%body = OpLabel\nOpRayQueryConfirmIntersectionKHR %q\nOpBranch %continue\n"
             "%continue = OpLabel\nOpBranch %loop\n%end = OpLabel\nOpReturn"),
        1, 1);
    run("Proceed in source loop header",
        base(scalar + initialize +
             "OpBranch %loop\n%loop = OpLabel\n"
             "%p = OpRayQueryProceedKHR %bool %q\n"
             "OpLoopMerge %end %continue None\nOpBranchConditional %p %body %end\n"
             "%body = OpLabel\nOpBranch %continue\n%continue = OpLabel\nOpBranch %loop\n"
             "%end = OpLabel\nOpReturn"),
        1, 1);
    run("loop Phi with Proceed in continue",
        base(scalar + initialize +
             "OpBranch %loop\n%loop = OpLabel\n"
             "%counter = OpPhi %u %u0 %entry %next %continue\n"
             "OpLoopMerge %end %continue None\nOpBranch %condition\n%condition = OpLabel\n"
             "%p = OpRayQueryProceedKHR %bool %q\nOpBranchConditional %p %body %end\n"
             "%body = OpLabel\nOpBranch %continue\n%continue = OpLabel\n"
             "%p2 = OpRayQueryProceedKHR %bool %q\n%next = OpIAdd %u %counter %u1\nOpBranch %loop\n"
             "%end = OpLabel\nOpReturn"),
        1, 2);
    run("source selection and successor Phi",
        base(scalar + initialize +
             "%p = OpRayQueryProceedKHR %bool %q\n"
             "OpSelectionMerge %join None\nOpBranchConditional %p %yes %join\n"
             "%yes = OpLabel\nOpBranch %join\n%join = OpLabel\n"
             "%value = OpPhi %u %u0 %entry %u1 %yes\nOpReturn"),
        1, 1);
    run("source switch successor Phi",
        base(scalar + initialize +
             "%p = OpRayQueryProceedKHR %bool %q\n"
             "OpSelectionMerge %join None\nOpSwitch %u0 %join 1 %yes\n"
             "%yes = OpLabel\nOpBranch %join\n%join = OpLabel\n"
             "%value = OpPhi %u %u0 %entry %u1 %yes\nOpReturn"),
        1, 1);
    run("Function array element",
        base("%queries = OpVariable %pFA Function\n%loaded = OpLoad %as %scene\n"
             "%q = OpAccessChain %pFQ %queries %u1\n" +
             std::string(initialize) + "%p = OpRayQueryProceedKHR %bool %q\nOpReturn"),
        1, 1);
    run("Function struct/array element",
        base("%queries = OpVariable %pFS Function\n%loaded = OpLoad %as %scene\n"
             "%q = OpAccessChain %pFQ %queries %u1 %u0\n" +
                 std::string(initialize) + "%p = OpRayQueryProceedKHR %bool %q\nOpReturn",
             {}, "%struct = OpTypeStruct %u %array\n%pFS = OpTypePointer Function %struct\n"),
        1, 1, false, true);
    run("Private array element",
        base("%loaded = OpLoad %as %scene\n%q = OpAccessChain %pPQ %queries %u1\n" + std::string(initialize) +
                 "%p = OpRayQueryProceedKHR %bool %q\nOpReturn",
             {}, "%queries = OpVariable %pPA Private\n", "%queries"),
        1, 1);
    const std::string helper =
        "%helper = OpFunction %void None %helperType\n%hq = OpFunctionParameter %pFQ\n"
        "%he = OpLabel\n%hproceed = OpRayQueryProceedKHR %bool %hq\nOpReturn\nOpFunctionEnd\n";
    run("query passed through existing helper",
        base(scalar + initialize + "%call = OpFunctionCall %void %helper %q\nOpReturn", helper,
             "%helperType = OpTypeFunction %void %pFQ\n"),
        1, 1);
    auto ext_source = base(scalar +
                               "OpRayQueryInitializeKHR %q %loaded %forceTwo %u255 %origin %f0 %direction %f1\n"
                               "%p = OpRayQueryProceedKHR %bool %q\nOpReturn",
                           {}, "%forceTwo = OpConstant %u 1024\n");
    ext_source.insert(0, "OpCapability RayTracingOpacityMicromapEXT\n");
    ext_source.insert(ext_source.find("OpMemoryModel"), "OpExtension \"SPV_EXT_opacity_micromap\"\n");
    auto ext_output = run("EXT declarations sanitized after flag translation", ext_source, 1, 1);
    bool ext_capability = false, ext_extension = false;
    for (size_t offset = 5; offset < ext_output.words.size(); offset += ext_output.words[offset] >> 16) {
        const auto op = spv::Op(ext_output.words[offset] & 0xffffu);
        ext_capability |= op == spv::OpCapability && ext_output.words[offset + 1] == spv::CapabilityRayTracingOpacityMicromapEXT;
        if (op == spv::OpExtension)
            ext_extension |=
                std::string(reinterpret_cast<const char*>(ext_output.words.data() + offset + 1)) == "SPV_EXT_opacity_micromap";
    }
    require(!ext_capability && !ext_extension, "EXT-only consumer declarations removed");
    rejection_tests();
    std::printf("Automatic typed query state/Proceed: %u checks PASS\n", checks);
    if (argc == 2 && std::string(argv[1]) == "--gpu") return gpu_tests();
    if (argc != 1) return 1;
    return 0;
}
