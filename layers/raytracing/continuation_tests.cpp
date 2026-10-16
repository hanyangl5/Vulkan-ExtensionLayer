// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "continuations.h"
#include "compiler.h"
#include "runtime.h"
#include "continuation_runtime.h"
#include "runtime_test_shader.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv_cross/spirv.hpp>
#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace vel::rt;
namespace {
void Require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
const char *kHeader = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %launch %acc %payload
OpName %carry "carry"
OpName %next "next"
OpName %chosen "chosen"
OpName %arg "arg"
OpName %discarded "discarded"
OpName %res "res"
OpName %main "main"
OpName %helper "helper"
OpDecorate %launch BuiltIn LaunchIdKHR
OpDecorate %acc DescriptorSet 0
OpDecorate %acc Binding 0
OpDecorate %payload Location 0
%void = OpTypeVoid
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v3f = OpTypeVector %float 3
%v3u = OpTypeVector %uint 3
%input = OpTypePointer Input %v3u
%as = OpTypeAccelerationStructureKHR
%asptr = OpTypePointer UniformConstant %as
%pptr = OpTypePointer RayPayloadKHR %uint
%launch = OpVariable %input Input
%acc = OpVariable %asptr UniformConstant
%payload = OpVariable %pptr RayPayloadKHR
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%mask = OpConstant %uint 255
%f0 = OpConstant %float 0
%f1 = OpConstant %float 1
%origin = OpConstantComposite %v3f %f0 %f0 %f0
%dir = OpConstantComposite %v3f %f0 %f0 %f1
%fn = OpTypeFunction %void
%helperfn = OpTypeFunction %uint %uint
)spv";
const char *kTrace = R"spv(
%scene = OpLoad %as %acc
OpTraceRayKHR %scene %zero %mask %zero %zero %zero %origin %f0 %dir %f1 %payload
)spv";

struct Fixture {
    std::vector<uint32_t> words;
    ContinuationAnalysis analysis;
    std::map<std::string, uint32_t> names;
    explicit Fixture(const std::string &body, const std::string &header = kHeader) {
        spvtools::SpirvTools t(SPV_ENV_VULKAN_1_2);
        std::string error;
        t.SetMessageConsumer(
            [&](spv_message_level_t, const char *, const spv_position_t &, const char *message) { error += message; });
        std::string filtered;
        std::istringstream lines(header);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find("OpName ") == 0) {
                auto end = line.find(' ', 7);
                auto id = line.substr(7, end - 7);
                if (body.find(id + " =") == std::string::npos) continue;
            }
            filtered += line + "\n";
        }
        bool assembled = t.Assemble(filtered + body, &words);
        Require(assembled, error.c_str());
        analysis = AnalyzeContinuations(words, "main", spv::ExecutionModelRayGenerationKHR);
        for (size_t p = 5; p < words.size(); p += words[p] >> 16) {
            if ((words[p] & 0xffff) == spv::OpName) names[reinterpret_cast<const char *>(&words[p + 2])] = words[p + 1];
        }
    }
    const FunctionContinuations &Function(const char *name) const {
        auto id = names.at(name);
        for (const auto &f : analysis.functions) {
            if (f.function == id) return f;
        }
        throw std::runtime_error("Function absent from analysis");
    }
    bool Live(const Suspension &s, const char *name) const {
        return std::find(s.live_values.begin(), s.live_values.end(), names.at(name)) != s.live_values.end();
    }
};

void StraightLine() {
    Fixture f(std::string(R"spv(
%main = OpFunction %void None %fn
%entry = OpLabel
%lid = OpLoad %v3u %launch
%carry = OpCompositeExtract %uint %lid 0
%discarded = OpIAdd %uint %carry %one
)spv") + kTrace +
              R"spv(
%res = OpIAdd %uint %carry %one
OpStore %payload %res
OpReturn
OpFunctionEnd
)spv");
    const auto &a = f.Function("main");
    Require(a.suspensions.size() == 1, "TraceRay continuation missing");
    Require(f.Live(a.suspensions[0], "carry"), "Value used after TraceRay not preserved");
    Require(!f.Live(a.suspensions[0], "discarded"), "Dead SSA value retained");
    Require(!f.Live(a.suspensions[0], "res"), "Value defined after TraceRay retained");
    Require(a.suspensions[0].live_values.size() == 1, "Constants/globals spilled");
}

void PhiBackEdge() {
    Fixture f(std::string(R"spv(
%main = OpFunction %void None %fn
%entry = OpLabel
OpBranch %loop
%loop = OpLabel
%chosen = OpPhi %uint %zero %entry %next %body
%next = OpIAdd %uint %chosen %one
%cond = OpULessThan %bool %chosen %mask
OpLoopMerge %exit %body None
OpBranchConditional %cond %body %exit
%body = OpLabel
)spv") + kTrace +
              R"spv(
OpBranch %loop
%exit = OpLabel
OpStore %payload %chosen
OpReturn
OpFunctionEnd
)spv");
    const auto &s = f.Function("main").suspensions.at(0);
    Require(f.Live(s, "next"), "Loop-carried phi incoming value lost across TraceRay");
    Require(!f.Live(s, "chosen"), "Phi result incorrectly live across its own back edge");
}

void PhiEdges() {
    Fixture f(std::string(R"spv(
%main = OpFunction %void None %fn
%entry = OpLabel
%lid = OpLoad %v3u %launch
%carry = OpCompositeExtract %uint %lid 0
%cond = OpULessThan %bool %carry %mask
OpSelectionMerge %merge None
OpBranchConditional %cond %left %right
%left = OpLabel
)spv") + kTrace +
              R"spv(
OpBranch %merge
%right = OpLabel
%discarded = OpIAdd %uint %carry %one
OpBranch %merge
%merge = OpLabel
%chosen = OpPhi %uint %carry %left %discarded %right
OpStore %payload %chosen
OpReturn
OpFunctionEnd
)spv");
    const auto &s = f.Function("main").suspensions.at(0);
    Require(f.Live(s, "carry"), "Phi incoming value missing on selected edge");
    Require(!f.Live(s, "discarded"), "Other predecessor's phi incoming value is spuriously live");
}

void TransitiveCalls() {
    Fixture f(std::string(R"spv(
%main = OpFunction %void None %fn
%entry = OpLabel
%lid = OpLoad %v3u %launch
%carry = OpCompositeExtract %uint %lid 0
%res = OpFunctionCall %uint %helper %carry
%next = OpIAdd %uint %res %carry
OpStore %payload %next
OpReturn
OpFunctionEnd
%helper = OpFunction %uint None %helperfn
%arg = OpFunctionParameter %uint
%he = OpLabel
)spv") + kTrace +
              R"spv(
OpReturnValue %arg
OpFunctionEnd
)spv");
    auto &caller = f.Function("main");
    Require(caller.can_suspend, "Suspension did not propagate through the call graph");
    Require(caller.suspensions.at(0).kind == SuspensionKind::FunctionCall, "Missing function-call continuation");
    Require(f.Live(caller.suspensions[0], "carry"), "Caller live value missing");
    Require(!f.Live(caller.suspensions[0], "res"), "Function return value spilled before being produced");
    Require(f.Live(f.Function("helper").suspensions.at(0), "arg"), "Function parameter not preserved");
    // Debug information is neither the IR nor the binding of suspension sites.
    spvtools::SpirvTools t(SPV_ENV_VULKAN_1_2);
    auto stripped = f.words;
    std::vector<uint32_t> out(stripped.begin(), stripped.begin() + 5);
    for (size_t p = 5; p < stripped.size(); p += stripped[p] >> 16) {
        if ((stripped[p] & 0xffff) != spv::OpName)
            out.insert(out.end(), stripped.begin() + p, stripped.begin() + p + (stripped[p] >> 16));
    }
    auto analysis = AnalyzeContinuations(out, "main", spv::ExecutionModelRayGenerationKHR);
    Require(analysis.functions.size() == f.analysis.functions.size(), "Stripping names changed call graph");
    for (size_t n = 0; n < analysis.functions.size(); ++n) {
        const auto &a = analysis.functions[n], &b = f.analysis.functions[n];
        Require(a.function == b.function && a.suspensions.size() == b.suspensions.size(), "Stripping names changed continuations");
        for (size_t k = 0; k < a.suspensions.size(); ++k)
            Require(a.suspensions[k].live_values == b.suspensions[k].live_values, "Stripping names changed liveness");
    }
}

void OtherRayOperations() {
    const char *intersection = R"glsl(
#version 460
#extension GL_EXT_ray_tracing : require
hitAttributeEXT vec2 attributes;
void main() {
    float t = gl_RayTminEXT + 1.0;
    attributes = vec2(0.1,0.2);
    bool accepted = reportIntersectionEXT(t,32u);
    attributes.y = accepted ? t : gl_RayTmaxEXT;
}
)glsl";
    auto words = CompileGLSL(intersection, VK_SHADER_STAGE_INTERSECTION_BIT_KHR);
    auto analysis = AnalyzeContinuations(words, "main", spv::ExecutionModelIntersectionKHR);
    Require(analysis.functions.size() == 1 && analysis.functions[0].suspensions.size() == 1,
            "ReportIntersection continuation missing");
    auto &site = analysis.functions[0].suspensions[0];
    Require(site.kind == SuspensionKind::ReportIntersection && site.result != 0, "ReportIntersection kind/result incorrect");
    Require(std::find(site.live_values.begin(), site.live_values.end(), site.result) == site.live_values.end(),
            "Intersection result incorrectly spilled before being produced");
    const char *callable = R"glsl(
#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) callableDataInEXT vec4 data;
layout(location=1) callableDataEXT vec4 child;
void main() { child=data; executeCallableEXT(0u,1); data=child; }
)glsl";
    words = CompileGLSL(callable, VK_SHADER_STAGE_CALLABLE_BIT_KHR);
    analysis = AnalyzeContinuations(words, "main", spv::ExecutionModelCallableKHR);
    Require(analysis.functions[0].suspensions.at(0).kind == SuspensionKind::ExecuteCallable,
            "ExecuteCallable continuation missing");
    // Recognizing callable in the IR does not imply execution support. The
    // pinned D3D12 fallback explicitly documents callable as unsupported.
    const char *anyhit = R"glsl(
#version 460
#extension GL_EXT_ray_tracing : require
void main() { if(gl_PrimitiveID==0) ignoreIntersectionEXT; else terminateRayEXT; }
)glsl";
    words = CompileGLSL(anyhit, VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
    analysis = AnalyzeContinuations(words, "main", spv::ExecutionModelAnyHitKHR);
    Require(analysis.functions[0].suspensions.empty(), "Terminal any-hit operations mistaken for suspension sites");
}

void ValidateInputAndEntry() {
    bool rejected = false;
    try {
        AnalyzeContinuations({0x07230203, 0x00010500, 0, 1, 0, 0}, "main", spv::ExecutionModelRayGenerationKHR);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    Require(rejected, "Invalid SPIR-V accepted");
    auto words =
        CompileGLSL("#version 460\n#extension GL_EXT_ray_tracing : require\nvoid main() {}", VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    auto a = AnalyzeContinuations(words, "main", spv::ExecutionModelRayGenerationKHR);
    Require(a.functions.size() == 1 && a.functions[0].suspensions.empty() && !a.functions[0].can_suspend,
            "Non-suspending shader incorrectly marked");
    for (auto pair :
         {std::pair<const char *, uint32_t>{"absent", spv::ExecutionModelRayGenerationKHR}, {"main", spv::ExecutionModelMissKHR}}) {
        rejected = false;
        try {
            AnalyzeContinuations(words, pair.first, pair.second);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        Require(rejected, "Missing/mismatched entry point accepted");
    }
}
}  // namespace
int main() {
    try {
        StraightLine();
        PhiBackEdge();
        PhiEdges();
        TransitiveCalls();
        ValidateInputAndEntry();
        OtherRayOperations();
        auto runtime = CompileGLSL(std::string("#version 460\n") + kMemory + kContinuationRuntime + kRuntimeTestShader,
                                   VK_SHADER_STAGE_COMPUTE_BIT);
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
        Require(tools.Validate(runtime), "GLSL continuation runtime produced invalid SPIR-V");
        for (uint32_t pass = 0; pass < static_cast<uint32_t>(BuildPass::Count); ++pass) {
            auto builder = BuildShader(static_cast<BuildPass>(pass));
            Require(tools.Validate(builder), "GPU BVH pass produced invalid SPIR-V");
        }
        std::cout << "SPIR-V continuation analysis passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
