// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Included inside state_compiler_tests.cpp's anonymous namespace.
static void PhysicalArrayAliases(const std::vector<uint32_t> &spirv) {
    std::map<uint32_t, size_t> types;
    std::map<uint32_t, unsigned> variable_aliases;
    for (size_t p = 5; p < spirv.size(); p += spirv[p] >> 16) {
        const auto op = spirv[p] & 65535u;
        if (op == spv::OpTypePointer || op == spv::OpTypeArray || op == spv::OpTypeRuntimeArray) types[spirv[p + 1]] = p;
        if (op == spv::OpDecorate) {
            const auto decoration = spirv[p + 2];
            if (decoration == spv::DecorationAliasedPointer || decoration == spv::DecorationRestrictPointer)
                ++variable_aliases[spirv[p + 1]];
        }
    }
    auto physical_array = [&](uint32_t type) {
        for (;;) {
            const auto found = types.find(type);
            if (found == types.end()) return false;
            const auto p = found->second;
            if ((spirv[p] & 65535u) == spv::OpTypePointer) return spirv[p + 2] == spv::StorageClassPhysicalStorageBuffer;
            type = spirv[p + 2];
        }
    };
    unsigned variables = 0, parameters = 0;
    for (size_t p = 5; p < spirv.size(); p += spirv[p] >> 16) {
        const auto op = spirv[p] & 65535u;
        if (op == spv::OpVariable) {
            const auto type = types.at(spirv[p + 1]);
            if (physical_array(spirv[type + 3])) {
                ++variables;
                Require(variable_aliases[spirv[p + 2]] == 1,
                        "Native physical pointer array variable lost its conservative alias decoration");
            }
        } else if (op == spv::OpFunctionParameter && physical_array(spirv[p + 1])) {
            ++parameters;
        }
    }
    Require(variables >= 2 && parameters >= 1, "Physical pointer array regression did not exercise both bridge directions");
}

static void PhysicalMatrixAccesses(const std::vector<uint32_t> &spirv) {
    std::map<uint32_t, size_t> types, definitions;
    std::map<uint32_t, uint32_t> constants, pointer_types;
    std::set<uint32_t> restricted;
    for (size_t p = 5; p < spirv.size(); p += spirv[p] >> 16) {
        const auto op = spirv[p] & 65535u;
        if (op >= spv::OpTypeVoid && op < spv::OpTypeForwardPointer) types[spirv[p + 1]] = p;
        if (op == spv::OpConstant) constants[spirv[p + 2]] = spirv[p + 3];
    }
    auto matrix_array = [&](uint32_t type) {
        while ((spirv[types.at(type)] & 65535u) == spv::OpTypeArray || (spirv[types.at(type)] & 65535u) == spv::OpTypeRuntimeArray)
            type = spirv[types.at(type) + 2];
        return (spirv[types.at(type)] & 65535u) == spv::OpTypeMatrix;
    };
    for (size_t p = 5; p < spirv.size(); p += spirv[p] >> 16) {
        const auto op = spirv[p] & 65535u;
        if (op != spv::OpConvertUToPtr && op != spv::OpAccessChain && op != spv::OpInBoundsAccessChain &&
            op != spv::OpPtrAccessChain && op != spv::OpInBoundsPtrAccessChain)
            continue;
        const auto type = types.at(spirv[p + 1]);
        if (spirv[type + 2] != spv::StorageClassPhysicalStorageBuffer) continue;
        const auto result = spirv[p + 2];
        pointer_types[result] = spirv[p + 1];
        definitions[result] = p;
        bool matrix = matrix_array(spirv[type + 3]);
        if (op != spv::OpConvertUToPtr) {
            const auto base = spirv[p + 3];
            matrix |= restricted.count(base) != 0;
            auto part = spirv[types.at(pointer_types.at(base)) + 3];
            const auto first = (op == spv::OpPtrAccessChain || op == spv::OpInBoundsPtrAccessChain) ? 5u : 4u;
            for (size_t n = first; n < (spirv[p] >> 16); ++n) {
                const auto declaration = types.at(part);
                const auto kind = spirv[declaration] & 65535u;
                matrix |= kind == spv::OpTypeMatrix;
                if (kind == spv::OpTypeStruct)
                    part = spirv[declaration + 2 + constants.at(spirv[p + n])];
                else
                    part = spirv[declaration + 2];
            }
        }
        if (matrix) {
            restricted.insert(result);
            Require(op != spv::OpConvertUToPtr, "Restricted matrix pointer was reconstructed from an integer address");
            auto base = spirv[p + 3];
            while (definitions.count(base)) {
                const auto definition = definitions.at(base);
                const auto kind = spirv[definition] & 65535u;
                if (kind == spv::OpConvertUToPtr) break;
                base = spirv[definition + 3];
            }
            const auto pointee = spirv[types.at(pointer_types.at(base)) + 3];
            Require((spirv[types.at(pointee)] & 65535u) == spv::OpTypeStruct,
                    "Physical matrix access chain lost its structure root");
        }
    }
    unsigned loads = 0, stores = 0;
    for (size_t p = 5; p < spirv.size(); p += spirv[p] >> 16) {
        const auto op = spirv[p] & 65535u;
        if (op == spv::OpConvertPtrToU)
            Require(!restricted.count(spirv[p + 3]), "Restricted matrix/row/element pointer was converted to integer");
        if (op == spv::OpLoad && restricted.count(spirv[p + 3])) ++loads;
        if (op == spv::OpStore && restricted.count(spirv[p + 1])) ++stores;
    }
    Require(loads >= 3 && stores >= 3, "Physical matrix regression missed whole-matrix, column and element accesses");
}

static void PhysicalSourceSemantics() {
    const char *assembly = R"spv(
OpCapability Shader
OpCapability Int64
OpCapability RayTracingKHR
OpCapability PhysicalStorageBufferAddresses
OpExtension "SPV_KHR_ray_tracing"
OpExtension "SPV_KHR_physical_storage_buffer"
OpMemoryModel PhysicalStorageBuffer64 GLSL450
OpEntryPoint IntersectionKHR %main "main" %out %private %aggregate
OpDecorate %words ArrayStride 4
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %out DescriptorSet 0
OpDecorate %out Binding 0
OpDecorate %local AliasedPointer
OpDecorate %physical ArrayStride 4
%void = OpTypeVoid
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%ulong = OpTypeInt 64 0
%float = OpTypeFloat 32
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%distance = OpConstant %float 1
%address = OpConstant %ulong 1234605616436508552
%words = OpTypeRuntimeArray %uint
%block = OpTypeStruct %words
%ssbo = OpTypePointer StorageBuffer %block
%ssbo_word = OpTypePointer StorageBuffer %uint
%physical = OpTypePointer PhysicalStorageBuffer %uint
%physical_array = OpTypeArray %physical %three
%holder = OpTypeStruct %physical_array %physical
%local_type = OpTypePointer Function %physical
%private_type = OpTypePointer Private %physical
%aggregate_type = OpTypePointer Private %holder
%entry = OpTypeFunction %void
%out = OpVariable %ssbo StorageBuffer
%private = OpVariable %private_type Private
%aggregate = OpVariable %aggregate_type Private
%main = OpFunction %void None %entry
%begin = OpLabel
%local = OpVariable %local_type Function
%condition_address = OpAccessChain %ssbo_word %out %zero %zero
%condition_value = OpLoad %uint %condition_address
%condition = OpIEqual %bool %condition_value %zero
%first = OpConvertUToPtr %physical %address
%second_address = OpIAdd %ulong %address %address
%second = OpConvertUToPtr %physical %second_address
OpSelectionMerge %join None
OpBranchConditional %condition %yes %no
%yes = OpLabel
OpBranch %join
%no = OpLabel
OpBranch %join
%join = OpLabel
%phi = OpPhi %physical %first %yes %second %no
%selected = OpSelect %physical %condition %phi %second
%advanced = OpPtrAccessChain %physical %selected %one
%array = OpCompositeConstruct %physical_array %first %second %selected
%whole = OpCompositeConstruct %holder %array %phi
OpStore %aggregate %whole
OpStore %local %selected
OpCopyMemory %private %local
%report = OpReportIntersectionKHR %bool %distance %one
%loaded_pointer = OpLoad %physical %local
%private_pointer = OpLoad %physical %private
%restored = OpLoad %holder %aggregate
%extracted = OpCompositeExtract %physical %whole 0 2
%read = OpLoad %uint %selected Volatile|Aligned|Nontemporal 4
OpStore %loaded_pointer %read Volatile|Aligned|Nontemporal 4
OpStore %private_pointer %read Aligned 4
OpStore %extracted %read Aligned 4
OpStore %advanced %read Aligned 4
OpReturn
OpFunctionEnd
)spv";
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1_SPIRV_1_4);
    std::string diagnostic;
    validator.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
        diagnostic += std::string(message) + "\n";
    });
    std::vector<uint32_t> source;
    Require(validator.Assemble(assembly, &source), "Physical source assembly failed: " + diagnostic);
    const bool valid = validator.Validate(source);
    Require(valid, "Physical source validation failed: " + diagnostic);
    auto program = CompileTypedPipelineStage(source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "physical_ssa");
    Require(program.frames.options.physical_pointer_addresses, "Physical source addresses were not represented explicitly");
    spvtools::SpirvTools universal(SPV_ENV_UNIVERSAL_1_4);
    Require(universal.Validate(program.spirv), "Physical source stage validation failed");
    std::set<uint32_t> physical_types;
    unsigned ptr_to_u = 0, u_to_ptr = 0;
    for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
        const auto op = program.spirv[p] & 65535u;
        if (op == spv::OpTypePointer && program.spirv[p + 2] == spv::StorageClassPhysicalStorageBuffer)
            physical_types.insert(program.spirv[p + 1]);
        if (op == spv::OpConvertPtrToU) ++ptr_to_u;
        if (op == spv::OpConvertUToPtr) ++u_to_ptr;
        if (op == spv::OpPhi || op == spv::OpSelect)
            Require(!physical_types.count(program.spirv[p + 1]),
                    "Native physical pointer merge escaped numeric identity transport");
    }
    Require(ptr_to_u >= 2 && u_to_ptr >= 4, "Physical source addresses were rematerialized instead of captured/reconstructed");
    for (const auto &layout : program.frames.types) {
        bool physical = false;
        for (size_t p = 5; p < source.size(); p += source[p] >> 16)
            physical |= (source[p] & 65535u) == spv::OpTypePointer && source[p + 1] == layout.type &&
                        source[p + 2] == spv::StorageClassPhysicalStorageBuffer;
        if (physical) Require(layout.size == 8 && layout.alignment == 8, "Physical pointer frame layout lost 64-bit address size");
    }
    const char *glsl = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430, buffer_reference_align=4) buffer Data { uint words[]; };
struct Item { Data a; Data b; uint number; uint pad; };
layout(buffer_reference, std430, buffer_reference_align=8) buffer Bundle { Item items[3]; };
layout(push_constant) uniform Config { uint64_t address; uint index; } config;
Item preserved[3];
void main() {
    Bundle source = Bundle(config.address);
    Item locals[3] = source.items;
    preserved = locals;
    reportIntersectionEXT(1.0, 1);
    source.items = preserved;
    locals[config.index % 3].a.words[config.index] += locals[config.index % 3].b.words[config.index];
}
)glsl";
    const auto array_source = CompileGLSL(glsl, VK_SHADER_STAGE_INTERSECTION_BIT_KHR, GLSLTarget::Vulkan11Spirv14);
    Require(validator.Validate(array_source), "Physical array bridge source failed validation");
    const auto arrays =
        CompileTypedPipelineStage(array_source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "physical_arrays");
    Require(universal.Validate(arrays.spirv), "Physical pointer array memory bridge failed validation");
    const auto caller_source = CompileGLSL("#version 460\n#extension GL_EXT_ray_tracing : require\nvoid main(){}\n",
                                           VK_SHADER_STAGE_RAYGEN_BIT_KHR, GLSLTarget::Vulkan11Spirv14);
    const auto caller =
        CompileTypedPipelineStage(caller_source, "main", VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr, "physical_caller");
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups{
        {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0,
         VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, nullptr},
        {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
         VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
         VK_SHADER_UNUSED_KHR, 1, nullptr}};
    TypedRuntimeBuildOptions options;
    options.optimize = false;
    const auto runtime = BuildTypedRuntimeProgram({caller, arrays}, groups, options);
    Require(validator.Validate(runtime.spirv), "Physical aggregate bridge final Vulkan validation failed");
    const auto recursive_source = CompileGLSL(R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430, buffer_reference_align=8) buffer Node;
layout(buffer_reference, std430, buffer_reference_align=8) buffer Node { Node next; uint value; };
layout(push_constant) uniform Config { uint64_t address; } config;
Node preserved;
void main() {
    Node root = Node(config.address);
    Node saved = root.next;
    preserved = saved;
    reportIntersectionEXT(1.0, 1);
    preserved.value += root.value;
    root.next = saved.next;
}
)glsl",
                                              VK_SHADER_STAGE_INTERSECTION_BIT_KHR, GLSLTarget::Vulkan11Spirv14);
    Require(validator.Validate(recursive_source), "Recursive physical pointer source validation failed");
    const auto recursive =
        CompileTypedPipelineStage(recursive_source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "physical_recursive");
    const auto recursive_runtime = BuildTypedRuntimeProgram({caller, recursive}, groups, options);
    Require(validator.Validate(recursive_runtime.spirv), "Recursive physical pointer final Vulkan validation failed");

    // Native pointer arrays keep conservative alias information on Function
    // storage. Aliased cannot decorate their array-by-value parameters because
    // it requires pointer-typed targets. Use eight-byte Data elements so glslang's
    // generated whole-array memory accesses also have the required alignment.
    const auto direct_source = CompileGLSL(R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430, buffer_reference_align=8) buffer Data { uvec2 words[]; };
layout(buffer_reference, std430, buffer_reference_align=8) buffer List { Data pointers[3]; };
layout(push_constant) uniform Config { uint64_t address; uint index; } config;
Data preserved[3];
void main() {
    List source = List(config.address);
    Data locals[3] = source.pointers;
    preserved = locals;
    reportIntersectionEXT(1.0, 1);
    source.pointers = preserved;
    locals[config.index % 3].words[config.index].x += preserved[(config.index + 1) % 3].words[config.index].y;
}
)glsl",
                                           VK_SHADER_STAGE_INTERSECTION_BIT_KHR, GLSLTarget::Vulkan11Spirv14);
    Require(validator.Validate(direct_source), "Direct physical pointer array source validation failed: " + diagnostic);
    const auto direct =
        CompileTypedPipelineStage(direct_source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "physical_direct_array");
    Require(universal.Validate(direct.spirv), "Direct physical pointer array stage validation failed");
    PhysicalArrayAliases(direct.spirv);
    const auto direct_runtime = BuildTypedRuntimeProgram({caller, direct}, groups, options);
    Require(validator.Validate(direct_runtime.spirv), "Direct physical pointer array runtime validation failed: " + diagnostic);
    PhysicalArrayAliases(direct_runtime.spirv);

    const auto mixed_source = CompileGLSL(R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430, buffer_reference_align=8) buffer Node;
layout(buffer_reference, std430, buffer_reference_align=8) buffer Data { uvec2 words[]; };
struct Item { Data pointers[2][3]; Node next; };
layout(buffer_reference, std430, buffer_reference_align=8) buffer Node { Item items[2]; Node next; };
layout(push_constant) uniform Config { uint64_t address; uint index; } config;
Item preserved[2];
void main() {
    Node source = Node(config.address);
    Item locals[2] = source.items;
    preserved = locals;
    reportIntersectionEXT(1.0, 1);
    source.items = preserved;
    uint i = config.index;
    locals[i % 2].pointers[i % 2][i % 3].words[i].x += preserved[(i + 1) % 2].next.next.items[0].pointers[0][0].words[i].y;
}
)glsl",
                                          VK_SHADER_STAGE_INTERSECTION_BIT_KHR, GLSLTarget::Vulkan11Spirv14);
    Require(validator.Validate(mixed_source), "Mixed recursive pointer array source validation failed: " + diagnostic);
    const auto mixed =
        CompileTypedPipelineStage(mixed_source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "physical_mixed_array");
    Require(universal.Validate(mixed.spirv), "Mixed recursive pointer array stage validation failed");
    PhysicalArrayAliases(mixed.spirv);
    const auto mixed_runtime = BuildTypedRuntimeProgram({caller, mixed}, groups, options);
    Require(validator.Validate(mixed_runtime.spirv), "Mixed recursive pointer array runtime validation failed: " + diagnostic);
    PhysicalArrayAliases(mixed_runtime.spirv);

    auto matrix_source = CompileGLSL(R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430, buffer_reference_align=16) buffer Data {
    layout(row_major) mat2x3 rows[2];
    layout(column_major) mat3x2 columns;
};
layout(push_constant) uniform Config { uint64_t address; uint index; } config;
void main() {
    Data source = Data(config.address);
    uint i = config.index;
    mat2x3 matrix = source.rows[i % 2];
    vec3 column = source.rows[i % 2][i % 2];
    float element = source.columns[i % 3][i % 2];
    reportIntersectionEXT(1.0, 1);
    source.rows[(i + 1) % 2] = matrix * 2.0;
    source.rows[i % 2][i % 2] = column * 3.0;
    source.columns[i % 3][i % 2] = element * 4.0;
}
)glsl",
                                     VK_SHADER_STAGE_INTERSECTION_BIT_KHR, GLSLTarget::Vulkan11Spirv14);
    Require(validator.Validate(matrix_source), "Physical matrix source validation failed: " + diagnostic);
    // Make one matrix address genuinely live across suspension. GLSL normally
    // places each access chain immediately before its Load; moving Report to
    // that gap is legal because its operands are module constants. This also
    // exercises the planner's recipe path rather than only same-state rebuilds.
    std::set<uint32_t> matrix_types;
    size_t report = 0, first_matrix_load = 0;
    for (size_t p = 5; p < matrix_source.size(); p += matrix_source[p] >> 16) {
        const auto op = matrix_source[p] & 65535u;
        if (op == spv::OpTypeMatrix) matrix_types.insert(matrix_source[p + 1]);
        if (!first_matrix_load && op == spv::OpLoad && matrix_types.count(matrix_source[p + 1])) first_matrix_load = p;
        if (op == spv::OpReportIntersectionKHR) report = p;
    }
    Require(report > first_matrix_load && first_matrix_load, "Matrix source lacks the planned suspension/Load ordering");
    const auto report_words = matrix_source[report] >> 16;
    const std::vector<uint32_t> moved_report(matrix_source.begin() + report, matrix_source.begin() + report + report_words);
    matrix_source.erase(matrix_source.begin() + report, matrix_source.begin() + report + report_words);
    matrix_source.insert(matrix_source.begin() + first_matrix_load, moved_report.begin(), moved_report.end());
    Require(validator.Validate(matrix_source), "Matrix pointer live across suspension source validation failed: " + diagnostic);
    const auto matrices =
        CompileTypedPipelineStage(matrix_source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "physical_matrices");
    bool suspended_recipe = false;
    for (const auto &function : matrices.frames.functions)
        for (const auto &suspension : function.suspensions)
            for (const auto &recipe : suspension.rematerializations)
                suspended_recipe |= std::find(matrices.frames.options.physical_pointer_recipes.begin(),
                                              matrices.frames.options.physical_pointer_recipes.end(),
                                              recipe.value) != matrices.frames.options.physical_pointer_recipes.end();
    Require(universal.Validate(matrices.spirv), "Physical matrix stage validation failed");
    PhysicalMatrixAccesses(matrices.spirv);
    Require(suspended_recipe, "Matrix pointer was not preserved through a continuation recipe");
    const auto matrix_runtime = BuildTypedRuntimeProgram({caller, matrices}, groups, options);
    Require(validator.Validate(matrix_runtime.spirv), "Physical matrix final Vulkan validation failed: " + diagnostic);
    PhysicalMatrixAccesses(matrix_runtime.spirv);
}
