// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_shader_compose.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/linker.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace vel::omm {
namespace {
constexpr char kSymbol[] = "__vel_omm_scalar_classifier_v1";
constexpr char kDescriptorSymbol[] = "__vel_omm_descriptor_fetch_v1";
constexpr char kIntegerSymbol[] = "__vel_omm_integer_resolve_v1";
struct Instruction {
    spv::Op op;
    std::vector<uint32_t> words;
    uint32_t result = 0;
};
struct Module {
    std::vector<Instruction> instructions;
    std::unordered_map<uint32_t, size_t> definitions;
    uint32_t addressing = 0, memory = 0;
    explicit Module(const std::vector<uint32_t>& source) {
        for (size_t offset = 5; offset < source.size();) {
            const uint32_t count = source[offset] >> 16;
            if (!count || count > source.size() - offset) throw std::length_error("Malformed SPIR-V instruction");
            instructions.push_back({spv::Op(source[offset] & 0xffffu),
                                    std::vector<uint32_t>(source.begin() + offset, source.begin() + offset + count)});
            auto& instruction = instructions.back();
            bool has_result = false, has_type = false;
            spv::HasResultAndType(instruction.op, &has_result, &has_type);
            if (has_result) {
                const size_t index = has_type ? 2 : 1;
                if (index >= count) throw std::length_error("Truncated SPIR-V result");
                instruction.result = instruction.words[index];
                definitions.emplace(instruction.result, instructions.size() - 1);
            }
            if (instruction.op == spv::OpMemoryModel) {
                addressing = instruction.words.at(1);
                memory = instruction.words.at(2);
            }
            offset += count;
        }
    }
    const Instruction& get(uint32_t id) const { return instructions.at(definitions.at(id)); }
};

std::vector<uint32_t> make(spv::Op op, const std::vector<uint32_t>& operands) {
    std::vector<uint32_t> result{(uint32_t(operands.size() + 1) << 16) | uint32_t(op)};
    result.insert(result.end(), operands.begin(), operands.end());
    return result;
}
void emit(std::vector<uint32_t>& destination, const std::vector<uint32_t>& words) {
    destination.insert(destination.end(), words.begin(), words.end());
}
std::vector<uint32_t> export_decoration(uint32_t id, std::string_view symbol = kSymbol) {
    std::vector<uint32_t> operands{id, uint32_t(spv::DecorationLinkageAttributes)};
    const size_t begin = operands.size();
    operands.resize(begin + (symbol.size() + 4) / 4, 0);
    std::memcpy(operands.data() + begin, symbol.data(), symbol.size());
    operands.push_back(uint32_t(spv::LinkageTypeExport));
    return make(spv::OpDecorate, operands);
}
bool scalar(const Module& module, uint32_t id, spv::Op type, uint32_t width = 0) {
    const auto& definition = module.get(id);
    if (definition.op != type) return false;
    if (type == spv::OpTypeBool) return definition.words.size() == 2;
    if (type == spv::OpTypeInt) return definition.words.size() == 4 && definition.words[2] == width && definition.words[3] == 0;
    return definition.words.size() == 3 && definition.words[2] == width;
}
bool vector(const Module& module, uint32_t id, spv::Op element, uint32_t count = 2) {
    const auto& definition = module.get(id);
    return definition.op == spv::OpTypeVector && definition.words.size() == 4 && definition.words[3] == count &&
           scalar(module, definition.words[2], element, 32);
}

std::vector<uint32_t> library_with_thunk(const std::vector<uint32_t>& library, const Module& parsed, uint32_t classifier_function,
                                         uint32_t memory_model) {
    const auto& function = parsed.get(classifier_function);
    if (function.op != spv::OpFunction || !scalar(parsed, function.words[1], spv::OpTypeInt, 32))
        throw std::invalid_argument("Classifier must return uint32");
    const auto& signature = parsed.get(function.words.at(4));
    if (signature.op != spv::OpTypeFunction || signature.words.size() != 10)
        throw std::invalid_argument("Classifier requires seven parameters");
    std::array<uint32_t, 7> pointers{}, values{};
    for (size_t i = 0; i < values.size(); ++i) {
        pointers[i] = signature.words[3 + i];
        const auto& pointer = parsed.get(pointers[i]);
        if (pointer.op != spv::OpTypePointer || pointer.words.size() != 4 || pointer.words[2] != spv::StorageClassFunction)
            throw std::invalid_argument("Classifier parameters must be Function pointers");
        values[i] = pointer.words[3];
        const bool valid = i == 0   ? vector(parsed, values[i], spv::OpTypeInt)
                           : i == 4 ? vector(parsed, values[i], spv::OpTypeFloat)
                           : i == 5 ? scalar(parsed, values[i], spv::OpTypeBool)
                                    : scalar(parsed, values[i], spv::OpTypeInt, 32);
        if (!valid) throw std::invalid_argument("Classifier parameter type mismatch");
    }
    uint32_t bound = library.at(3);
    auto id = [&]() {
        if (bound >= 0x3fffff) throw std::length_error("SPIR-V ID limit");
        return bound++;
    };
    const uint32_t thunk_type = id(), thunk = id(), block = id(), returned = id();
    std::array<uint32_t, 7> parameters{}, locals{};
    for (auto& parameter : parameters) parameter = id();
    for (auto& local : locals) local = id();
    std::vector<uint32_t> function_type{thunk_type, function.words[1]};
    function_type.insert(function_type.end(), values.begin(), values.end());
    std::vector<uint32_t> result(library.begin(), library.begin() + 5);
    bool capability_inserted = false, decoration_inserted = false, type_inserted = false;
    for (const auto& instruction : parsed.instructions) {
        if (!capability_inserted && instruction.op != spv::OpCapability) {
            emit(result, make(spv::OpCapability, {spv::CapabilityLinkage}));
            capability_inserted = true;
        }
        // The helper introduces no resource/global state. Its ordinary unused
        // dummy main function can remain after removing entry/mode declarations.
        if (instruction.op == spv::OpEntryPoint || instruction.op == spv::OpExecutionMode ||
            instruction.op == spv::OpExecutionModeId)
            continue;
        // glslang can emit this constant even when the helper never reads it.
        // Leaving the BuiltIn decoration would override the APPLICATION's local
        // workgroup size, despite removing the helper's entry and modes.
        if (instruction.op == spv::OpDecorate && instruction.words.size() == 4 && instruction.words[2] == spv::DecorationBuiltIn &&
            instruction.words[3] == spv::BuiltInWorkgroupSize)
            continue;
        if (!decoration_inserted && instruction.op >= spv::OpTypeVoid && instruction.op <= spv::OpTypeForwardPointer) {
            emit(result, export_decoration(thunk));
            decoration_inserted = true;
        }
        if (!type_inserted && instruction.op == spv::OpFunction) {
            emit(result, make(spv::OpTypeFunction, function_type));
            type_inserted = true;
        }
        if (instruction.op == spv::OpMemoryModel)
            emit(result, make(spv::OpMemoryModel, {spv::AddressingModelPhysicalStorageBuffer64, memory_model}));
        else
            emit(result, instruction.words);
    }
    if (!decoration_inserted || !type_inserted) throw std::invalid_argument("Missing classifier module declarations");
    emit(result, make(spv::OpFunction, {function.words[1], thunk, spv::FunctionControlMaskNone, thunk_type}));
    for (size_t i = 0; i < values.size(); ++i) emit(result, make(spv::OpFunctionParameter, {values[i], parameters[i]}));
    emit(result, make(spv::OpLabel, {block}));
    for (size_t i = 0; i < values.size(); ++i)
        emit(result, make(spv::OpVariable, {pointers[i], locals[i], spv::StorageClassFunction}));
    for (size_t i = 0; i < values.size(); ++i) emit(result, make(spv::OpStore, {locals[i], parameters[i]}));
    std::vector<uint32_t> call{function.words[1], returned, classifier_function};
    call.insert(call.end(), locals.begin(), locals.end());
    emit(result, make(spv::OpFunctionCall, call));
    emit(result, make(spv::OpReturnValue, {returned}));
    emit(result, make(spv::OpFunctionEnd, {}));
    result[3] = bound;
    return result;
}

std::vector<uint32_t> append_provider(const std::vector<uint32_t>& library, uint32_t target, bool descriptor, uint64_t context) {
    if (!target) return library;
    const Module module(library);
    const auto& function = module.get(target);
    if (function.op != spv::OpFunction || !vector(module, function.words.at(1), spv::OpTypeInt, descriptor ? 4 : 2))
        throw std::invalid_argument("Provider return type mismatch");
    const auto& signature = module.get(function.words.at(4));
    const size_t count = descriptor ? 4 : 2;
    if (signature.op != spv::OpTypeFunction || signature.words.size() != count + 3)
        throw std::invalid_argument("Provider parameter count mismatch");
    std::vector<uint32_t> pointers(count), values(count);
    for (size_t i = 0; i < count; ++i) {
        pointers[i] = signature.words[3 + i];
        const auto& pointer = module.get(pointers[i]);
        if (pointer.op != spv::OpTypePointer || pointer.words.size() != 4 || pointer.words[2] != spv::StorageClassFunction)
            throw std::invalid_argument("Provider parameters must be Function pointers");
        values[i] = pointer.words[3];
        const bool valid = i == 0           ? scalar(module, values[i], spv::OpTypeInt, 64)
                           : i == count - 1 ? vector(module, values[i], spv::OpTypeInt)
                                            : scalar(module, values[i], spv::OpTypeInt, 32);
        if (!valid) throw std::invalid_argument("Provider parameter type mismatch");
    }
    uint32_t bound = library[3];
    auto id = [&]() {
        if (bound >= 0x3fffff) throw std::length_error("SPIR-V ID limit");
        return bound++;
    };
    const uint32_t type = id(), thunk = id(), block = id(), returned = id(), context_value = id();
    std::vector<uint32_t> parameters(count - 1), locals(count);
    for (auto& parameter : parameters) parameter = id();
    for (auto& local : locals) local = id();
    std::vector<uint32_t> signature_values{type, function.words[1]};
    signature_values.insert(signature_values.end(), values.begin() + 1, values.end());
    std::vector<uint32_t> result(library.begin(), library.begin() + 5);
    bool decorated = false, declared = false;
    for (const auto& instruction : module.instructions) {
        if (!decorated && instruction.op >= spv::OpTypeVoid && instruction.op <= spv::OpTypeForwardPointer) {
            emit(result, export_decoration(thunk, descriptor ? kDescriptorSymbol : kIntegerSymbol));
            decorated = true;
        }
        if (!declared && instruction.op == spv::OpFunction) {
            emit(result, make(spv::OpConstant, {values[0], context_value, uint32_t(context), uint32_t(context >> 32)}));
            emit(result, make(spv::OpTypeFunction, signature_values));
            declared = true;
        }
        emit(result, instruction.words);
    }
    if (!decorated || !declared) throw std::invalid_argument("Missing provider module declarations");
    emit(result, make(spv::OpFunction, {function.words[1], thunk, spv::FunctionControlMaskNone, type}));
    for (size_t i = 0; i < parameters.size(); ++i) emit(result, make(spv::OpFunctionParameter, {values[i + 1], parameters[i]}));
    emit(result, make(spv::OpLabel, {block}));
    for (size_t i = 0; i < count; ++i) emit(result, make(spv::OpVariable, {pointers[i], locals[i], spv::StorageClassFunction}));
    emit(result, make(spv::OpStore, {locals[0], context_value}));
    for (size_t i = 0; i < parameters.size(); ++i) emit(result, make(spv::OpStore, {locals[i + 1], parameters[i]}));
    std::vector<uint32_t> call{function.words[1], returned, target};
    call.insert(call.end(), locals.begin(), locals.end());
    emit(result, make(spv::OpFunctionCall, call));
    emit(result, make(spv::OpReturnValue, {returned}));
    emit(result, make(spv::OpFunctionEnd, {}));
    result[3] = bound;
    return result;
}
}  // namespace

ShaderCompositionStatus compose_query_classifier(const std::vector<uint32_t>& source,
                                                 const std::vector<uint32_t>& classifier_library,
                                                 uint32_t library_classifier_function, ComposedQueryShader* output,
                                                 std::string* diagnostic) {
    QueryLibraryOptions options;
    options.classifier_function = library_classifier_function;
    return compose_query_library(source, classifier_library, options, output, diagnostic);
}

ShaderCompositionStatus compose_query_library(const std::vector<uint32_t>& source, const std::vector<uint32_t>& classifier_library,
                                              const QueryLibraryOptions& exports, ComposedQueryShader* output,
                                              std::string* diagnostic) {
    auto fail = [&](ShaderCompositionStatus status, std::string_view reason) noexcept {
        if (diagnostic) {
            try {
                diagnostic->assign(reason.data(), reason.size());
            } catch (...) {
                diagnostic->clear();
            }
        }
        return status;
    };
    try {
        if (!output) return fail(ShaderCompositionStatus::Unsupported, "Missing composition output");
        std::string messages;
        auto consumer = [&](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
            messages += std::to_string(position.index) + ": " + message + "\n";
        };
        spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_2);
        validator.SetMessageConsumer(consumer);
        if (!validator.Validate(source)) return fail(ShaderCompositionStatus::InvalidSource, messages);
        messages.clear();
        if (!validator.Validate(classifier_library)) return fail(ShaderCompositionStatus::InvalidLibrary, messages);
        const Module application(source), helper(classifier_library);
        if ((application.addressing != spv::AddressingModelLogical &&
             application.addressing != spv::AddressingModelPhysicalStorageBuffer64) ||
            (application.memory != spv::MemoryModelGLSL450 && application.memory != spv::MemoryModelVulkan))
            return fail(ShaderCompositionStatus::Unsupported, "Unsupported application memory model");
        if (helper.addressing != spv::AddressingModelPhysicalStorageBuffer64 || helper.memory != spv::MemoryModelGLSL450)
            return fail(ShaderCompositionStatus::InvalidLibrary, "Classifier library must use physical-storage GLSL450");
        for (const auto& instruction : helper.instructions) {
            // Application specialization entries also apply to matching IDs in
            // the linked module. Internal helpers must not share that namespace.
            if (instruction.op == spv::OpDecorate && instruction.words.at(2) == spv::DecorationSpecId)
                return fail(ShaderCompositionStatus::InvalidLibrary, "Classifier library must not add specialization IDs");
            if (instruction.op == spv::OpVariable && instruction.words.at(3) != spv::StorageClassFunction)
                return fail(ShaderCompositionStatus::InvalidLibrary, "Classifier library must not add global resources/state");
            if (instruction.op == spv::OpEntryPoint && instruction.words.at(1) != spv::ExecutionModelGLCompute)
                return fail(ShaderCompositionStatus::InvalidLibrary, "Expected compute dummy entry point");
        }
        auto library = library_with_thunk(classifier_library, helper, exports.classifier_function, application.memory);
        library = append_provider(library, exports.descriptor_fetch_function, true, exports.context_address);
        library = append_provider(library, exports.integer_resolver_function, false, exports.context_address);
        auto prepared = source;
        for (size_t offset = 5; offset < prepared.size(); offset += prepared[offset] >> 16)
            if (spv::Op(prepared[offset] & 0xffffu) == spv::OpMemoryModel)
                prepared[offset + 1] = spv::AddressingModelPhysicalStorageBuffer64;
        spvtools::Context context(SPV_ENV_UNIVERSAL_1_5);
        context.SetMessageConsumer(consumer);
        spvtools::LinkerOptions options;
        options.SetCreateLibrary(true);
        options.SetVerifyIds(true);
        options.SetUseHighestVersion(true);
        std::vector<uint32_t> linked;
        messages.clear();
        if (spvtools::Link(context, {prepared, library}, &linked, options) != SPV_SUCCESS)
            return fail(ShaderCompositionStatus::LinkFailed, messages);
        ComposedQueryShader result;
        result.words.assign(linked.begin(), linked.begin() + 5);
        const Module parsed(linked);
        for (const auto& instruction : parsed.instructions) {
            if (instruction.op == spv::OpCapability && instruction.words.at(1) == spv::CapabilityLinkage) continue;
            if (instruction.op == spv::OpDecorate && instruction.words.at(2) == spv::DecorationLinkageAttributes) {
                const auto id = instruction.words.at(1);
                uint32_t* exported = nullptr;
                if (instruction.words == export_decoration(id)) exported = &result.classifier_function;
                if (instruction.words == export_decoration(id, kDescriptorSymbol)) exported = &result.descriptor_fetch_function;
                if (instruction.words == export_decoration(id, kIntegerSymbol)) exported = &result.integer_resolver_function;
                if (!exported || *exported) return fail(ShaderCompositionStatus::LinkFailed, "Unexpected linked classifier export");
                *exported = id;
                continue;
            }
            emit(result.words, instruction.words);
        }
        if (!result.classifier_function || (exports.descriptor_fetch_function && !result.descriptor_fetch_function) ||
            (exports.integer_resolver_function && !result.integer_resolver_function))
            return fail(ShaderCompositionStatus::LinkFailed, "Shader library export lost during link");
        messages.clear();
        if (!validator.Validate(result.words)) return fail(ShaderCompositionStatus::LinkFailed, messages);
        *output = std::move(result);
        if (diagnostic) diagnostic->clear();
        return ShaderCompositionStatus::Success;
    } catch (const std::bad_alloc&) {
        return fail(ShaderCompositionStatus::OutOfMemory, "Classifier composition allocation failed");
    } catch (const std::exception& exception) {
        return fail(ShaderCompositionStatus::InvalidLibrary, exception.what());
    }
}
}  // namespace vel::omm
