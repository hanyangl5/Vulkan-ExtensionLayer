// SPDX-License-Identifier: MIT
#include "typed_pipeline.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <cstdio>
#include <cstring>

extern "C" int vkrt_mixed_public_shader(const uint32_t *, size_t, const char *);
extern "C" int vkrt_mixed_native_words(const uint32_t *words, size_t count) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    if (!tools.Validate(words, count)) return 0;
    unsigned entries = 0;
    for (size_t p = 5; p < count; p += words[p] >> 16) {
        const auto op = words[p] & 65535;
        if (op == spv::OpCapability && words[p + 1] == spv::CapabilityRayTracingKHR) return 0;
        if (op == spv::OpEntryPoint) {
            ++entries;
            if (words[p + 1] != spv::ExecutionModelGLCompute || std::strcmp((const char *)(words + p + 3), "main")) return 0;
        }
    }
    return entries == 1;
}
int main() {
    const char *text = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %rt "main"
OpEntryPoint GLCompute %compute "main"
OpExecutionMode %compute LocalSize 1 1 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%rt = OpFunction %void None %fn
%r = OpLabel
OpReturn
OpFunctionEnd
%compute = OpFunction %void None %fn
%c = OpLabel
OpReturn
OpFunctionEnd
)spv";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::vector<uint32_t> words;
    if (!tools.Assemble(text, &words) || !tools.Validate(words)) return 2;
    auto original = words;
    auto selected = vel::rt::SelectNativeShaderEntry(words, "main", VK_SHADER_STAGE_COMPUTE_BIT);
    if (!vkrt_mixed_native_words(selected.data(), selected.size())) return 3;
    int control = vkrt_mixed_public_shader(selected.data(), selected.size(), "explicit-selector-control");
    int actual = vkrt_mixed_public_shader(words.data(), words.size(), "mixed-public-source");
    if (words != original) return 5;
    puts("Real shader-object selector/classifier/rewriter/cache roundtrip checked with valid mixed SPIR-V");
    return control ? 4 : actual;
}
