// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "typed_pipeline.h"
#include "glsl_compiler.h"
#include <spirv/unified1/spirv.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
using namespace vel::rt;
void Require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
constexpr uint64_t kContext = 0x123456789abc0000ull;
TypedContinuationProgram Compile(const std::string &source, VkShaderStageFlagBits stage) {
    TypedContinuationCompileOptions options;
    options.transport = TypedFrameTransport::PhysicalAddress;
    options.ray_query_as_context = kContext;
    const auto start = std::chrono::steady_clock::now();
    auto program = CompileTypedPipelineStage(CompileGLSL(source, stage), "main", stage, nullptr, "vel_rt_stage_0", options);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "Stage " << stage << ": " << elapsed << " seconds, " << program.spirv.size() << " output words, "
              << program.frame_stride_bytes << " frame bytes" << std::endl;
    return program;
}
void Check(const TypedContinuationProgram &program, uint32_t binding) {
    Require(!program.spirv.empty() && program.frame_stride_bytes, "empty typed query program");
    Require(program.acceleration_structure_descriptors.size() == 1, "AS descriptor provenance was lost");
    const auto &descriptor = program.acceleration_structure_descriptors.front();
    Require(descriptor.descriptor_set == 0 && descriptor.binding == binding, "AS descriptor binding changed");
    bool variable = false, context = false;
    for (size_t p = 5; p < program.source.size(); p += program.source[p] >> 16) {
        const auto op = program.source[p] & 65535;
        Require(op != spv::OpTypeRayQueryKHR, "opaque query reached continuation planning");
        if (op == spv::OpVariable && program.source[p + 2] == descriptor.variable)
            variable = program.source[p + 3] == spv::StorageClassStorageBuffer;
        if (op == spv::OpConstant && (program.source[p] >> 16) == 5)
            context |= (uint64_t(program.source[p + 4]) << 32 | program.source[p + 3]) == kContext;
    }
    Require(variable && context, "query descriptor or device resolver identity changed");
    for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
        const auto op = program.spirv[p] & 65535;
        Require(op != spv::OpTypeRayQueryKHR && op != spv::OpTypeAccelerationStructureKHR && op != spv::OpTraceRayKHR,
                "RT/query operation leaked into compiled library");
    }
}
}  // namespace

int main(int argc, char **argv) {
    try {
        const std::string source = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_ray_query : require
layout(set=0,binding=0) uniform accelerationStructureEXT scene;
layout(set=0,binding=1,std430) buffer Results { uint result; } outputData;
void main() {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, 0, 255, vec3(0,0,1), 0, vec3(0,0,-1), 100);
    while (rayQueryProceedEXT(query)) rayQueryConfirmIntersectionEXT(query);
    outputData.result = rayQueryGetIntersectionTypeEXT(query, true);
})glsl";
        for (auto stage :
             {VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
              VK_SHADER_STAGE_ANY_HIT_BIT_KHR, VK_SHADER_STAGE_INTERSECTION_BIT_KHR, VK_SHADER_STAGE_CALLABLE_BIT_KHR}) {
            Check(Compile(source, stage), 0);
            std::cout << "Ray Query stage " << stage << " compiled with descriptor/context identity" << std::endl;
        }
        auto memory_source = source;
        memory_source.insert(memory_source.find("layout(set"),
                             "#extension GL_KHR_memory_scope_semantics : require\n#pragma use_vulkan_memory_model\n");
        memory_source.insert(memory_source.find("    rayQueryEXT query;"),
                             "    memoryBarrier(gl_ScopeShaderCallEXT, gl_StorageSemanticsBuffer, "
                             "gl_SemanticsAcquireRelease | gl_SemanticsMakeAvailable | gl_SemanticsMakeVisible);\n");
        const auto memory_program = Compile(memory_source, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
        Check(memory_program, 0);
        bool vulkan = false, barrier = false;
        for (size_t p = 5; p < memory_program.spirv.size(); p += memory_program.spirv[p] >> 16) {
            const auto op = memory_program.spirv[p] & 65535;
            if (op == spv::OpMemoryModel) vulkan = memory_program.spirv[p + 2] == spv::MemoryModelVulkan;
            barrier |= op == spv::OpMemoryBarrier;
        }
        Require(vulkan && barrier, "Vulkan source model/barrier was lost during query/continuation lowering");
        std::cout << "Vulkan memory model query stage compiled with source barrier" << std::endl;
        Require(argc == 2, "expected the public query suspension shader path");
        std::ifstream input(argv[1]);
        Require(bool(input), "cannot open query suspension shader");
        const std::string suspension{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto program = Compile(suspension, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
        Check(program, 1);
        Require(program.state_count > 1, "query fixture has no actual TraceRay suspension");
        bool trace = false;
        for (size_t p = 5; p < program.source.size(); p += program.source[p] >> 16)
            trace |= (program.source[p] & 65535) == spv::OpTraceRayKHR;
        Require(trace, "query lowering removed the source trace operation");
        std::cout << "Two dynamic query states survive typed planning across TraceRay; public GPU fixture checks values"
                  << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
