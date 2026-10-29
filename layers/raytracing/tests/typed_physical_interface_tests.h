// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Included inside typed_runtime_tests.cpp's anonymous namespace.
static void PhysicalInterfaceMetadata() {
    const std::string prefix = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430, buffer_reference_align=8) buffer Node;
layout(buffer_reference, std430, buffer_reference_align=8) buffer Node { Node next; uint value; };
struct Packet { uint tag; Node root; Node refs[2]; uint endTag; };
struct Attribute { Node root; uint tag; uint pad; };
layout(push_constant) uniform Config { uint64_t address; uint index; } config;
)glsl";
    struct Case {
        VkShaderStageFlagBits stage;
        std::string source;
    };
    std::vector<Case> cases{{VK_SHADER_STAGE_RAYGEN_BIT_KHR, prefix + R"glsl(
layout(set=0,binding=0) uniform accelerationStructureEXT scene;
layout(location=0) rayPayloadEXT Packet payload;
layout(location=1) callableDataEXT Packet data;
void main() {
    payload.tag = 3u;
    payload.root = Node(config.address);
    payload.refs[0] = payload.root;
    payload.refs[1] = payload.root.next;
    payload.endTag = 11u;
    traceRayEXT(scene,0u,255u,0u,0u,0u,vec3(0),0.0,vec3(0,0,1),4.0,0);
    data = payload;
    executeCallableEXT(0u,1);
    payload = data;
    payload.root.value += payload.refs[config.index % 2].value;
}
)glsl"},
                            {VK_SHADER_STAGE_MISS_BIT_KHR, prefix + R"glsl(
layout(location=7) rayPayloadInEXT Packet payload;
void main() { payload.refs[0] = payload.root.next; payload.root = payload.refs[1]; payload.tag += 7u; }
)glsl"},
                            {VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, prefix + R"glsl(
layout(location=2) rayPayloadInEXT Packet payload;
layout(location=3) callableDataEXT Packet data;
hitAttributeEXT Attribute attributes;
void main() {
    payload.root = attributes.root;
    data = payload;
    executeCallableEXT(0u,3);
    payload = data;
    payload.endTag += attributes.tag;
}
)glsl"},
                            {VK_SHADER_STAGE_ANY_HIT_BIT_KHR, prefix + R"glsl(
layout(location=4) rayPayloadInEXT Packet payload;
hitAttributeEXT Attribute attributes;
void main() { payload.refs[1] = attributes.root; payload.tag += attributes.tag; }
)glsl"},
                            {VK_SHADER_STAGE_INTERSECTION_BIT_KHR, prefix + R"glsl(
hitAttributeEXT Attribute attributes;
void main() {
    attributes.root = Node(config.address);
    attributes.tag = 1u;
    attributes.pad = 19u;
    reportIntersectionEXT(2.0,33u);
    attributes.root = attributes.root.next;
    attributes.tag = 2u;
    reportIntersectionEXT(1.0,34u);
}
)glsl"},
                            {VK_SHADER_STAGE_CALLABLE_BIT_KHR, prefix + R"glsl(
layout(location=9) callableDataInEXT Packet data;
layout(location=6) callableDataEXT Packet nested;
Node preserved;
void main() {
    preserved = data.root;
    if (data.tag > 0u) {
        nested = data;
        nested.tag--;
        executeCallableEXT(0u,6);
        data = nested;
    }
    data.root = preserved.next;
    data.refs[config.index % 2] = preserved;
}
)glsl"}};
    // Equivalent recursive pointees may use different nominal type sharing.
    // One side reuses Node for both fields; this side has two equivalent nodes.
    cases.push_back({VK_SHADER_STAGE_MISS_BIT_KHR, R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430, buffer_reference_align=8) buffer Left;
layout(buffer_reference, std430, buffer_reference_align=8) buffer Right;
layout(buffer_reference, std430, buffer_reference_align=8) buffer Left { Left next; uint value; };
layout(buffer_reference, std430, buffer_reference_align=8) buffer Right { Right next; uint value; };
struct Packet { uint tag; Left root; Right refs[2]; uint endTag; };
layout(location=8) rayPayloadInEXT Packet payload;
void main() { payload.tag += payload.root.value + payload.refs[0].value; }
)glsl"});
    std::string numeric = prefix;
    const std::string pointer_packet = "struct Packet { uint tag; Node root; Node refs[2]; uint endTag; };";
    numeric.replace(numeric.find(pointer_packet), pointer_packet.size(),
                    "struct Packet { uint tag; uint64_t root; uint64_t refs[2]; uint endTag; };");
    cases.push_back({VK_SHADER_STAGE_MISS_BIT_KHR, numeric + R"glsl(
layout(location=8) rayPayloadInEXT Packet payload;
void main() { payload.root += payload.refs[0]; }
)glsl"});
    std::string different = prefix;
    const std::string pointer_node = "buffer Node { Node next; uint value; };";
    different.replace(different.find(pointer_node), pointer_node.size(), "buffer Node { Node next; float value; };");
    cases.push_back({VK_SHADER_STAGE_MISS_BIT_KHR, different + R"glsl(
layout(location=8) rayPayloadInEXT Packet payload;
void main() { payload.root.value += 1.0; }
)glsl"});
    cases.push_back({VK_SHADER_STAGE_CALLABLE_BIT_KHR, prefix + R"glsl(
layout(location=8) callableDataInEXT Node data;
void main() { data = data.next; }
)glsl"});
    cases.push_back({VK_SHADER_STAGE_CALLABLE_BIT_KHR, prefix + R"glsl(
layout(location=8) callableDataInEXT uint64_t data;
void main() { data += 1ul; }
)glsl"});
    std::vector<TypedContinuationProgram> stages;
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1_SPIRV_1_4);
    spvtools::ValidatorOptions validation;
    validation.SetScalarBlockLayout(true);
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto source = CompileGLSL(cases[i].source, cases[i].stage, GLSLTarget::Vulkan11Spirv14);
        Require(validator.Validate(source.data(), source.size(), validation), "Physical interface source validation failed");
        stages.push_back(
            CompileTypedPipelineStage(source, "main", cases[i].stage, nullptr, "physical_interface_" + std::to_string(i)));
    }
    auto group = [](VkRayTracingShaderGroupTypeKHR type, uint32_t general, uint32_t closest, uint32_t any, uint32_t intersection) {
        return VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                                    nullptr,
                                                    type,
                                                    general,
                                                    closest,
                                                    any,
                                                    intersection,
                                                    nullptr};
    };
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups{
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 1, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR, VK_SHADER_UNUSED_KHR, 2, 3, 4),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 5, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR)};
    for (uint32_t i = 6; i < stages.size(); ++i)
        groups.push_back(group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, i, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                               VK_SHADER_UNUSED_KHR));
    for (bool optimize : {false, true}) {
        TypedRuntimeBuildOptions options;
        options.optimize = optimize;
        const auto runtime = BuildTypedRuntimeProgram(stages, groups, options);
        Require(validator.Validate(runtime.spirv.data(), runtime.spirv.size(), validation),
                "Physical interface runtime validation failed");
        auto interface = [&](uint32_t stage, uint32_t storage) -> const TypedRuntimeInterface & {
            for (const auto &value : runtime.interfaces)
                if (value.stage == stage && value.storage_class == storage) return value;
            throw std::runtime_error("Physical interface is missing from the actual runtime copy plan");
        };
        const auto &payload = interface(0, spv::StorageClassRayPayloadKHR);
        Require(payload.size == 40 && payload.offset % 8 == 0, "Physical Packet frame ABI is not 40-byte/aligned8");
        for (const auto &[stage, storage] :
             std::vector<std::pair<uint32_t, uint32_t>>{{0, spv::StorageClassCallableDataKHR},
                                                        {1, spv::StorageClassIncomingRayPayloadKHR},
                                                        {2, spv::StorageClassIncomingRayPayloadKHR},
                                                        {2, spv::StorageClassCallableDataKHR},
                                                        {3, spv::StorageClassIncomingRayPayloadKHR},
                                                        {5, spv::StorageClassIncomingCallableDataKHR},
                                                        {5, spv::StorageClassCallableDataKHR},
                                                        {6, spv::StorageClassIncomingRayPayloadKHR}}) {
            const auto &value = interface(stage, storage);
            Require(value.size == payload.size && value.canonical_type == payload.canonical_type,
                    "Equivalent recursive physical interfaces have incompatible runtime copy metadata");
        }
        const auto &attribute = interface(4, spv::StorageClassHitAttributeKHR);
        Require(attribute.size == 16 && runtime.max_hit_attribute_bytes == 16, "Physical attribute ABI size changed");
        for (uint32_t stage : {2u, 3u})
            Require(interface(stage, spv::StorageClassHitAttributeKHR).canonical_type == attribute.canonical_type,
                    "Pending/committed physical attribute copies have mismatched types");
        Require(interface(7, spv::StorageClassIncomingRayPayloadKHR).size == payload.size &&
                    interface(7, spv::StorageClassIncomingRayPayloadKHR).canonical_type != payload.canonical_type,
                "Physical pointer interface was conflated with equally sized uint64 values");
        Require(interface(8, spv::StorageClassIncomingRayPayloadKHR).canonical_type != payload.canonical_type,
                "Different physical pointee type graphs were conflated");
        const auto &pointer = interface(9, spv::StorageClassIncomingCallableDataKHR);
        const auto &number = interface(10, spv::StorageClassIncomingCallableDataKHR);
        Require(pointer.size == 8 && number.size == 8 && pointer.canonical_type != number.canonical_type,
                "Direct physical pointer callable data lost its distinct eight-byte identity ABI");
    }
}
