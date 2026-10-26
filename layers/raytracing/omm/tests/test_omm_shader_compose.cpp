// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "../omm_shader_compose.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <unordered_map>

namespace composition_fault {
bool enabled = false;
int budget = 0;
}  // namespace composition_fault
void* operator new(size_t size) {
    if (composition_fault::enabled) {
        if (composition_fault::budget == 0) throw std::bad_alloc();
        --composition_fault::budget;
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

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
std::vector<uint32_t> assemble(const std::string& text) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string diagnostic;
    tools.SetMessageConsumer(
        [&](spv_message_level_t, const char*, const spv_position_t&, const char* message) { diagnostic += message; });
    std::vector<uint32_t> result;
    require(tools.Assemble(text, &result, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS), "Assemble: " + diagnostic);
    require(tools.Validate(result), "Fixture validation: " + diagnostic);
    return result;
}
std::vector<uint32_t> source(bool physical = false, bool vulkan_memory = false, bool fragment = false, bool builtin = false) {
    std::string text = "OpCapability Shader\n";
    if (physical) text += "OpCapability PhysicalStorageBufferAddresses\n";
    if (vulkan_memory) text += "OpCapability VulkanMemoryModel\nOpExtension \"SPV_KHR_vulkan_memory_model\"\n";
    text += std::string("OpMemoryModel ") + (physical ? "PhysicalStorageBuffer64 " : "Logical ") +
            (vulkan_memory ? "Vulkan\n" : "GLSL450\n");
    text += fragment ? "OpEntryPoint Fragment %main \"main\"\nOpExecutionMode %main OriginUpperLeft\n"
                     : "OpEntryPoint GLCompute %main \"main\"\nOpExecutionMode %main LocalSize 4 2 1\n";
    // A debug name colliding with our internal export must never select the
    // application's main function as the classifier.
    text += "OpName %main \"__vel_omm_scalar_classifier_v1\"\n";
    if (builtin) text += "OpDecorate %workgroup BuiltIn WorkgroupSize\n";
    text += "%void = OpTypeVoid\n%fn = OpTypeFunction %void\n";
    if (builtin)
        text +=
            "%u = OpTypeInt 32 0\n%v3u = OpTypeVector %u 3\n%one = OpConstant %u 1\n"
            "%two = OpConstant %u 2\n%four = OpConstant %u 4\n"
            "%workgroup = OpConstantComposite %v3u %four %two %one\n";
    text += "%main = OpFunction %void None %fn\n%entry = OpLabel\nOpReturn\nOpFunctionEnd\n";
    return assemble(text);
}
std::vector<uint32_t> library(bool global = false, bool wrong_first_parameter = false, bool logical = false, bool builtin = false,
                              bool specialization = false) {
    std::string text = "OpCapability Shader\nOpCapability Int64\n";
    if (!logical) text += "OpCapability PhysicalStorageBufferAddresses\n";
    text += std::string("OpMemoryModel ") + (logical ? "Logical GLSL450\n" : "PhysicalStorageBuffer64 GLSL450\n");
    text += "OpEntryPoint GLCompute %main \"main\"\nOpExecutionMode %main LocalSize 1 1 1\n";
    if (builtin) text += "OpDecorate %workgroup BuiltIn WorkgroupSize\n";
    if (specialization) text += "OpDecorate %one SpecId 7\n";
    text +=
        "%void = OpTypeVoid\n%u = OpTypeInt 32 0\n%f = OpTypeFloat 32\n%b = OpTypeBool\n"
        "%v2u = OpTypeVector %u 2\n%v2f = OpTypeVector %f 2\n"
        "%pU = OpTypePointer Function %u\n%pB = OpTypePointer Function %b\n"
        "%pV2U = OpTypePointer Function %v2u\n%pV2F = OpTypePointer Function %v2f\n"
        "%fn = OpTypeFunction %void\n";
    text += specialization ? "%one = OpSpecConstant %u 1\n" : "%one = OpConstant %u 1\n";
    if (builtin) text += "%v3u = OpTypeVector %u 3\n%workgroup = OpConstantComposite %v3u %one %one %one\n";
    text += std::string("%fnC = OpTypeFunction %u ") + (wrong_first_parameter ? "%pV2F" : "%pV2U") + " %pU %pU %pU %pV2F %pB %pU\n";
    if (global) text += "%pPrivate = OpTypePointer Private %u\n%global = OpVariable %pPrivate Private\n";
    text += "%2000 = OpFunction %u None %fnC\n";
    text += std::string("%arg0 = OpFunctionParameter ") + (wrong_first_parameter ? "%pV2F\n" : "%pV2U\n");
    text +=
        "%arg1 = OpFunctionParameter %pU\n%arg2 = OpFunctionParameter %pU\n%arg3 = OpFunctionParameter %pU\n"
        "%arg4 = OpFunctionParameter %pV2F\n%arg5 = OpFunctionParameter %pB\n%arg6 = OpFunctionParameter %pU\n"
        "%body = OpLabel\nOpReturnValue %one\nOpFunctionEnd\n"
        "%main = OpFunction %void None %fn\n%entry = OpLabel\nOpReturn\nOpFunctionEnd\n";
    return assemble(text);
}
void valid_models() {
    const auto helper = library();
    auto old_application = source();
    old_application[1] = 0x00010000u;
    ComposedQueryShader upgraded;
    std::string diagnostic;
    require(compose_query_classifier(old_application, helper, 2000, &upgraded, &diagnostic) == ShaderCompositionStatus::Success,
            "SPIR-V 1.0 source with 1.5 helper: " + diagnostic);
    require(upgraded.words[1] == helper[1], "Linked version supports the newer physical-storage ABI");
    for (bool physical : {false, true})
        for (bool vulkan_memory : {false, true})
            for (bool fragment : {false, true}) {
                const auto application = source(physical, vulkan_memory, fragment);
                const auto original = application;
                ComposedQueryShader composed;
                std::string diagnostic;
                require(
                    compose_query_classifier(application, helper, 2000, &composed, &diagnostic) == ShaderCompositionStatus::Success,
                    "Composition: " + diagnostic);
                require(application == original, "Source bytes unchanged");
                require(composed.classifier_function != 0, "Classifier ID retained");
                spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
                require(tools.Validate(composed.words), "Final Vulkan validation");
                uint32_t entries = 0, modes = 0, globals = 0, locals = 0, wrapper_calls = 0;
                bool in_wrapper = false;
                for (size_t offset = 5; offset < composed.words.size(); offset += composed.words[offset] >> 16) {
                    const auto op = spv::Op(composed.words[offset] & 0xffffu);
                    if (op == spv::OpEntryPoint) {
                        ++entries;
                        require(
                            composed.words[offset + 1] == (fragment ? spv::ExecutionModelFragment : spv::ExecutionModelGLCompute),
                            "Original entry point stage retained");
                        require(composed.words[offset + 2] != composed.classifier_function, "Export/debug-name collision safe");
                    }
                    if (op == spv::OpExecutionMode) ++modes;
                    if (op == spv::OpMemoryModel)
                        require(
                            composed.words[offset + 1] == spv::AddressingModelPhysicalStorageBuffer64 &&
                                composed.words[offset + 2] == (vulkan_memory ? spv::MemoryModelVulkan : spv::MemoryModelGLSL450),
                            "Physical addressing and original memory semantics");
                    if (op == spv::OpCapability)
                        require(composed.words[offset + 1] != spv::CapabilityLinkage, "No Linkage capability");
                    if (op == spv::OpDecorate)
                        require(composed.words[offset + 2] != spv::DecorationLinkageAttributes, "No linkage decorations");
                    if (op == spv::OpFunction) in_wrapper = composed.words[offset + 2] == composed.classifier_function;
                    if (op == spv::OpVariable) {
                        if (composed.words[offset + 3] != spv::StorageClassFunction) ++globals;
                        if (in_wrapper) ++locals;
                    }
                    if (op == spv::OpFunctionCall && in_wrapper) ++wrapper_calls;
                }
                require(entries == 1 && modes == 1, "Dummy entry and execution modes removed");
                require(globals == 0 && locals == 7 && wrapper_calls == 1,
                        "Resource-free scalar thunk with memory-object arguments");
            }
}
void helper_workgroup_isolation() {
    for (bool application_builtin : {false, true}) {
        const auto application = source(false, false, false, application_builtin);
        const auto helper = library(false, false, false, true);
        ComposedQueryShader result;
        std::string diagnostic;
        require(compose_query_classifier(application, helper, 2000, &result, &diagnostic) == ShaderCompositionStatus::Success,
                "Helper WorkgroupSize composition: " + diagnostic);
        std::unordered_map<uint32_t, uint32_t> constants;
        std::unordered_map<uint32_t, std::vector<uint32_t>> composites;
        uint32_t workgroup = 0, decorations = 0;
        for (size_t offset = 5; offset < result.words.size(); offset += result.words[offset] >> 16) {
            const auto op = spv::Op(result.words[offset] & 0xffffu);
            if (op == spv::OpDecorate && result.words[offset + 2] == spv::DecorationBuiltIn &&
                result.words[offset + 3] == spv::BuiltInWorkgroupSize) {
                workgroup = result.words[offset + 1];
                ++decorations;
            }
            if (op == spv::OpConstant) constants[result.words[offset + 2]] = result.words[offset + 3];
            if (op == spv::OpConstantComposite) {
                const auto end = offset + (result.words[offset] >> 16);
                composites[result.words[offset + 2]] = {result.words.begin() + offset + 3, result.words.begin() + end};
            }
        }
        require(decorations == uint32_t(application_builtin), "Only original application's WorkgroupSize remains");
        if (application_builtin) {
            const auto& size = composites.at(workgroup);
            require(size.size() == 3 && constants.at(size[0]) == 4 && constants.at(size[1]) == 2 && constants.at(size[2]) == 1,
                    "Application WorkgroupSize not overridden by helper's 1x1x1");
        }
    }
}
void allocation_faults() {
    const auto application = source(), helper = library();
    for (int budget : {0, 1, 2, 4, 8, 32, 256}) {
        ComposedQueryShader output{{0x12345678u}, 77};
        std::string diagnostic;
        ShaderCompositionStatus status = ShaderCompositionStatus::Success;
        bool escaped = false;
        composition_fault::budget = budget;
        composition_fault::enabled = true;
        try {
            status = compose_query_classifier(application, helper, 2000, &output, &diagnostic);
        } catch (...) {
            escaped = true;
        }
        composition_fault::enabled = false;
        require(!escaped, "Sustained allocation failure does not escape");
        require(status == ShaderCompositionStatus::OutOfMemory, "Allocation failure returns OutOfMemory");
        require(output.words == std::vector<uint32_t>{0x12345678u} && output.classifier_function == 77,
                "Allocation failure leaves output unchanged");
    }
}
void negative_cases() {
    const auto application = source(), helper = library();
    auto reject = [&](const std::vector<uint32_t>& input, const std::vector<uint32_t>& lib, uint32_t function,
                      ShaderCompositionStatus expected, const char* label) {
        ComposedQueryShader output{{0x12345678u}, 77};
        std::string diagnostic;
        const auto result = compose_query_classifier(input, lib, function, &output, &diagnostic);
        require(result == expected, std::string(label) + ": " + diagnostic);
        require(output.words == std::vector<uint32_t>{0x12345678u} && output.classifier_function == 77,
                std::string(label) + " leaves output unchanged");
        require(!diagnostic.empty(), std::string(label) + " reports reason");
    };
    reject({}, helper, 2000, ShaderCompositionStatus::InvalidSource, "Malformed source");
    reject(application, {}, 2000, ShaderCompositionStatus::InvalidLibrary, "Malformed library");
    reject(application, helper, 0, ShaderCompositionStatus::InvalidLibrary, "Missing classifier");
    reject(application, library(true), 2000, ShaderCompositionStatus::InvalidLibrary, "Global helper state");
    reject(application, library(false, true), 2000, ShaderCompositionStatus::InvalidLibrary, "Wrong ABI type");
    reject(application, library(false, false, true), 2000, ShaderCompositionStatus::InvalidLibrary, "Wrong helper addressing");
    reject(application, library(false, false, false, false, true), 2000, ShaderCompositionStatus::InvalidLibrary,
           "Helper specialization namespace collision");
    require(compose_query_classifier(application, helper, 2000, nullptr) == ShaderCompositionStatus::Unsupported,
            "Null output rejected");
}
}  // namespace
int main() {
    valid_models();
    helper_workgroup_isolation();
    negative_cases();
    allocation_faults();
    std::printf("Shader classifier composition: %u checks, 0 failures\n", checks);
}
