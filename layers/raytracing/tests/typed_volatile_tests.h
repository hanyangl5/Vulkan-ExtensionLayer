// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Included in typed_pipeline_tests.cpp's anonymous namespace.
void VolatileOptimizationBoundaries() {
    for (uint32_t variant = 0; variant < 3; ++variant) {
        const std::string prefix = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %output
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 0
)spv";
        const std::string types = R"spv(
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%seven = OpConstant %uint 7
%block = OpTypeStruct %uint
%block_ptr = OpTypePointer StorageBuffer %block
%word_ptr = OpTypePointer StorageBuffer %uint
%local_ptr = OpTypePointer Function %uint
%output = OpVariable %block_ptr StorageBuffer
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%local = OpVariable %local_ptr Function
OpStore %local %seven
%address = OpAccessChain %word_ptr %output %zero
)spv";
        const auto text = prefix + (variant == 2 ? "OpDecorate %output Volatile\n" : "") + types + "%unused = OpLoad %uint " +
                          (variant == 1 ? "%local" : "%address") + (variant == 2 ? "\n" : " Volatile\n") +
                          "OpStore %address %seven\nOpReturn\nOpFunctionEnd\n";
        const auto source = Assemble(text);
        const auto stage = CompileTypedPipelineStage(source, "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr, "vel_rt_stage_0");
        VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        group.generalShader = 0;
        group.closestHitShader = group.anyHitShader = group.intersectionShader = VK_SHADER_UNUSED_KHR;
        const auto runtime = BuildTypedRuntimeProgram({stage}, {group});
        auto check = [&](const Words &words) {
            uint32_t loads = 0, decorations = 0, buffer_loads = 0;
            std::map<uint32_t, uint32_t> pointer_types, pointers;
            for (size_t p = 5; p < words.size(); p += words[p] >> 16) {
                const auto op = words[p] & 65535;
                if (op == spv::OpTypePointer) pointer_types[words[p + 1]] = words[p + 2];
                if ((op == spv::OpVariable || op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain ||
                     op == spv::OpCopyObject || op == spv::OpSelect || op == spv::OpPhi) &&
                    pointer_types.count(words[p + 1]))
                    pointers[words[p + 2]] = pointer_types.at(words[p + 1]);
                if (op == spv::OpLoad && pointers.count(words[p + 3]) &&
                    pointers.at(words[p + 3]) == spv::StorageClassStorageBuffer)
                    ++buffer_loads;
                if (op == spv::OpLoad && (words[p] >> 16) >= 5 && (words[p + 4] & spv::MemoryAccessVolatileMask)) ++loads;
                if (op == spv::OpDecorate && words[p + 2] == spv::DecorationVolatile) ++decorations;
            }
            Require(variant == 2 ? decorations == 1 && buffer_loads == 1 : loads == 1,
                    "Unused volatile access lost at a compiler boundary");
        };
        check(stage.source);
        check(runtime.spirv);
        CheckNative(runtime.spirv, 1);
        if (variant != 1) {
            auto ordinary = text;
            ordinary.replace(ordinary.find("RayGenerationKHR"), std::strlen("RayGenerationKHR"), "GLCompute");
            ordinary.insert(ordinary.find("OpDecorate"), "OpExecutionMode %main LocalSize 1 1 1\n");
            check(SelectNativeShaderEntry(Assemble(ordinary), "main", VK_SHADER_STAGE_COMPUTE_BIT));
            ordinary.insert(ordinary.find("OpExecutionMode"), "OpEntryPoint RayGenerationKHR %raygen \"raygen\" %scene %payload\n");
            ordinary.insert(ordinary.find("%void ="), R"spv(
OpDecorate %scene DescriptorSet 0
OpDecorate %scene Binding 1
OpDecorate %payload Location 0
)spv");
            ordinary.insert(ordinary.find("%main ="), R"spv(
%as_type = OpTypeAccelerationStructureKHR
%as_ptr = OpTypePointer UniformConstant %as_type
%scene = OpVariable %as_ptr UniformConstant
%payload_ptr = OpTypePointer RayPayloadKHR %uint
%payload = OpVariable %payload_ptr RayPayloadKHR
)spv");
            ordinary += R"spv(
%raygen = OpFunction %void None %fn
%raygen_entry = OpLabel
%as = OpLoad %as_type %scene
OpStore %payload %seven
OpReturn
OpFunctionEnd
)spv";
            const auto selected = SelectNativeShaderEntry(Assemble(ordinary), "main", VK_SHADER_STAGE_COMPUTE_BIT);
            check(selected);
            for (size_t p = 5; p < selected.size(); p += selected[p] >> 16) {
                const auto op = selected[p] & 65535;
                Require(op != spv::OpTypeAccelerationStructureKHR &&
                            !(op == spv::OpTypePointer && selected[p + 2] == spv::StorageClassRayPayloadKHR) &&
                            !(op == spv::OpCapability && selected[p + 1] == spv::CapabilityRayTracingKHR),
                        "Volatile native selection retained unselected RT declarations");
            }
        }
    }
}
