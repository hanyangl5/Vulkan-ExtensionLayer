// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "glsl_compiler.h"
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <mutex>
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

}  // namespace vel::rt
