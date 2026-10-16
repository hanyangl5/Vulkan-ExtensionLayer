// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "compiler.h"
#include "continuations.h"
#include "runtime.h"
#include "bvh_shader.h"
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <mutex>
#include <regex>
#include <spirv_cross/spirv_glsl.hpp>
#include <sstream>
#include <stdexcept>

namespace vel::rt {
std::vector<uint32_t> CompileGLSL(const std::string &source, VkShaderStageFlagBits stage) {
    static std::once_flag init;
    std::call_once(init, [] { glslang::InitializeProcess(); });
    EShLanguage lang;
    switch (stage) {
        case VK_SHADER_STAGE_COMPUTE_BIT:
            lang = EShLangCompute;
            break;
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            lang = EShLangRayGen;
            break;
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            lang = EShLangClosestHit;
            break;
        case VK_SHADER_STAGE_MISS_BIT_KHR:
            lang = EShLangMiss;
            break;
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            lang = EShLangAnyHit;
            break;
        case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
            lang = EShLangIntersect;
            break;
        case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
            lang = EShLangCallable;
            break;
        default:
            throw std::runtime_error("Unsupported input shader stage");
    }
    glslang::TShader shader(lang);
    const char *text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 460);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 460, false, messages)) throw std::runtime_error(shader.getInfoLog());
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) throw std::runtime_error(program.getInfoLog());
    std::vector<uint32_t> out;
    glslang::SpvOptions opts;
    opts.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), out, &opts);
    return out;
}

// Older SPIRV-Cross releases know the KHR enumerant but do not give this
// builtin a GLSL name. Lower it by SPIR-V decoration, independent of debug names
// and of the emitted fallback spelling (e.g. gl_BuiltIn_5352).
class RTCompilerGLSL : public spirv_cross::CompilerGLSL {
  public:
    using spirv_cross::CompilerGLSL::CompilerGLSL;

  protected:
    std::string builtin_to_glsl(spv::BuiltIn builtin, spv::StorageClass storage) override {
        if (builtin == spv::BuiltInRayGeometryIndexKHR) return "int(vel_geometry)";
        return spirv_cross::CompilerGLSL::builtin_to_glsl(builtin, storage);
    }
};

static void Replace(std::string &s, const std::string &name, const std::string &value) {
    s = std::regex_replace(s, std::regex("\\b" + name + "\\b"), value);
}

Program Link(const std::vector<Stage> &stages, const std::vector<VkRayTracingShaderGroupCreateInfoKHR> &groups,
             uint32_t internal_set) {
    if (stages.empty() || groups.empty()) throw std::runtime_error("Empty RT pipeline");
    std::string source = "#version 460\n#define VEL_SET " + std::to_string(internal_set) + "\n";
    source += kMemory;
    source += kTraversal;
    uint32_t raygens = 0;
    for (uint32_t si = 0; si < stages.size(); si++) {
        const auto &stage = stages[si];
        spv::ExecutionModel model;
        switch (stage.stage) {
            case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
                model = spv::ExecutionModelRayGenerationKHR;
                raygens++;
                break;
            case VK_SHADER_STAGE_MISS_BIT_KHR:
                model = spv::ExecutionModelMissKHR;
                break;
            case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
                model = spv::ExecutionModelClosestHitKHR;
                break;
            default:
                throw std::runtime_error("Unsupported RT stage; only raygen/miss/closest-hit are implemented");
        }
        auto continuations = AnalyzeContinuations(stage.spirv, stage.entry, static_cast<uint32_t>(model));
        if (stage.stage != VK_SHADER_STAGE_RAYGEN_BIT_KHR) {
            for (const auto &function : continuations.functions) {
                if (function.can_suspend)
                    throw std::runtime_error("Hit/miss continuation execution is not yet connected to the state-machine backend");
            }
        }
        RTCompilerGLSL c(stage.spirv);
        c.set_entry_point(stage.entry, model);
        // Prefix SPIR-V object/type/function names before GLSL emission. Local
        // scopes remain independent.
        for (uint32_t id = 1; id < stage.spirv.at(3); id++) {
            auto name = c.get_name(id);
            c.set_name(id, "vel_s" + std::to_string(si) + "_" + (name.empty() ? "id" + std::to_string(id) : name));
        }
        auto options = c.get_common_options();
        options.version = 460;
        options.vulkan_semantics = true;
        options.es = false;
        c.set_common_options(options);
        std::string glsl = c.compile();
        if (glsl.find("shaderRecordEXT") != std::string::npos || glsl.find("callableData") != std::string::npos ||
            glsl.find("rayQuery") != std::string::npos || glsl.find("nonuniformEXT") != std::string::npos)
            throw std::runtime_error(
                "Shader record data, callables, ray queries and "
                "nonuniform indexing are not implemented");
        glsl = std::regex_replace(glsl, std::regex("#version[^\\n]*\\n"), "");
        glsl = std::regex_replace(glsl, std::regex("#extension GL_EXT_ray_tracing[^\\n]*\\n"), "");
        // A single vec4 payload at location zero is the first executable compiler
        // subset.
        std::regex payload(R"((layout\s*\(location\s*=\s*([0-9]+)\)\s*)?rayPayload(In)?EXT\s+(\w+)\s+(\w+)\s*;)");
        std::smatch m;
        while (std::regex_search(glsl, m, payload)) {
            if ((!m[2].str().empty() && m[2] != "0") || m[4] != "vec4")
                throw std::runtime_error("Only location-zero vec4 ray payload is implemented");
            std::string name = m[5];
            glsl.erase(m.position(), m.length());
            Replace(glsl, name, "vel_payload");
        }
        std::regex hitattr(R"(hitAttributeEXT\s+(\w+)\s+(\w+)\s*;)");
        while (std::regex_search(glsl, m, hitattr)) {
            if (m[1] != "vec2") throw std::runtime_error("Only vec2 triangle attributes are implemented");
            std::string name = m[2];
            glsl.erase(m.position(), m.length());
            Replace(glsl, name, "vel_bary");
        }
        std::regex as(R"((layout\s*\([^\n]*\))\s*uniform\s+accelerationStructureEXT\s+(\w+)\s*;)");
        while (std::regex_search(glsl, m, as)) {
            std::string layout = m[1], name = m[2];
            glsl.erase(m.position(), m.length());
            Replace(glsl, name, name + ".vel_address");
            glsl = layout + " readonly buffer " + name + "_block { uint64_t vel_address; } " + name + ";\n" + glsl;
        }
        const std::pair<const char *, const char *> builtins[] = {{"gl_LaunchIDEXT", "gl_GlobalInvocationID"},
                                                                  {"gl_LaunchSizeEXT", "vel_d.dims.xyz"},
                                                                  {"gl_WorldRayOriginEXT", "vel_worldOrigin"},
                                                                  {"gl_WorldRayDirectionEXT", "vel_worldDirection"},
                                                                  {"gl_ObjectRayOriginEXT", "vel_objectOrigin"},
                                                                  {"gl_ObjectRayDirectionEXT", "vel_objectDirection"},
                                                                  {"gl_RayTminEXT", "vel_tmin"},
                                                                  {"gl_RayTmaxEXT", "vel_tmax"},
                                                                  {"gl_PrimitiveID", "int(vel_primitive)"},
                                                                  {"gl_InstanceID", "int(vel_instance)"},
                                                                  {"gl_InstanceCustomIndexEXT", "int(vel_custom)"},
                                                                  {"gl_GeometryIndexEXT", "int(vel_geometry)"},
                                                                  {"gl_HitKindEXT", "vel_hitKind"},
                                                                  {"gl_IncomingRayFlagsEXT", "vel_flags"},
                                                                  {"traceRayEXT", "vel_trace"},
                                                                  {"gl_RayFlagsOpaqueEXT", "1u"},
                                                                  {"gl_RayFlagsTerminateOnFirstHitEXT", "4u"},
                                                                  {"gl_RayFlagsSkipClosestHitShaderEXT", "8u"}};
        for (auto &[name, value] : builtins) Replace(glsl, name, value);
        if (std::regex_search(glsl, std::regex(R"(\bgl_\w*(EXT|NV)\b)")))
            throw std::runtime_error("Unsupported ray tracing builtin");
        glsl = std::regex_replace(glsl, std::regex(R"(void\s+main\s*\(\s*\))"), "void vel_stage" + std::to_string(si) + "()");
        source += "\n" + glsl;
    }
    if (raygens != 1) throw std::runtime_error("Exactly one raygen stage is required");
    auto dispatch = [&](const char *name, VkShaderStageFlagBits stage) {
        source += std::string("\nvoid ") + name + "(uint g) { switch(g) {\n";
        for (uint32_t gi = 0; gi < groups.size(); gi++) {
            const auto &g = groups[gi];
            if (g.anyHitShader != VK_SHADER_UNUSED_KHR || g.intersectionShader != VK_SHADER_UNUSED_KHR)
                throw std::runtime_error("Any-hit and intersection groups are not implemented");
            uint32_t index = stage == VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR ? g.closestHitShader : g.generalShader;
            if (index != VK_SHADER_UNUSED_KHR) {
                if (index >= stages.size()) throw std::runtime_error("Out-of-range group stage index");
                if (stages[index].stage == stage)
                    source += "case " + std::to_string(gi) + "u: vel_stage" + std::to_string(index) + "(); break;\n";
            }
        }
        source += "default: break; } }\n";
    };
    dispatch("vel_hit", VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
    dispatch("vel_miss", VK_SHADER_STAGE_MISS_BIT_KHR);
    dispatch("vel_raygen", VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    source +=
        "void main() { "
        "if(any(greaterThanEqual(gl_GlobalInvocationID,vel_d.dims.xyz))) return; "
        "vel_record=vel_d.raygen; vel_raygen(vu(vel_record,0)); }\n";
    return {CompileGLSL(source, VK_SHADER_STAGE_COMPUTE_BIT), source};
}

std::vector<uint32_t> BuildShader(BuildPass pass) {
    auto prefix = std::string("#version 460\n#define VEL_BUILD_PASS ") + std::to_string(static_cast<uint32_t>(pass)) + "\n";
    return CompileGLSL(prefix + kMemory + kBVHBuilder, VK_SHADER_STAGE_COMPUTE_BIT);
}
}  // namespace vel::rt
