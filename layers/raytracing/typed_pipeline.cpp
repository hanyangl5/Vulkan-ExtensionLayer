// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "typed_pipeline.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/linker.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace vel::rt {
namespace {
using Words = std::vector<uint32_t>;
void Require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error("Typed pipeline: " + message);
}
uint32_t Model(VkShaderStageFlagBits stage) {
    switch (stage) {
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            return spv::ExecutionModelRayGenerationKHR;
        case VK_SHADER_STAGE_MISS_BIT_KHR:
            return spv::ExecutionModelMissKHR;
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            return spv::ExecutionModelClosestHitKHR;
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            return spv::ExecutionModelAnyHitKHR;
        case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
            return spv::ExecutionModelIntersectionKHR;
        case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
            return spv::ExecutionModelCallableKHR;
        default:
            throw std::runtime_error("Typed pipeline: invalid RT shader stage");
    }
}
void Validate(const Words &words, spv_target_env environment) {
    spvtools::SpirvTools tools(environment);
    std::string error;
    tools.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
        error += std::string(message) + "\n";
    });
    bool valid = tools.Validate(words);
    Require(valid, error);
}
std::string String(const Words &words, size_t begin, size_t end) {
    const char *bytes = reinterpret_cast<const char *>(words.data() + begin);
    size_t count = (end - begin) * 4;
    const char *zero = static_cast<const char *>(std::memchr(bytes, 0, count));
    Require(zero != nullptr, "unterminated SPIR-V literal");
    return {bytes, zero};
}
Words Literal(const std::string &name) {
    Words result((name.size() + 4) / 4, 0);
    std::memcpy(result.data(), name.data(), name.size());
    return result;
}
void Emit(Words &output, spv::Op op, std::initializer_list<uint32_t> arguments) {
    output.push_back((uint32_t(arguments.size() + 1) << 16) | op);
    output.insert(output.end(), arguments);
}
Words Specialize(const Words &source, const VkSpecializationInfo *info) {
    Validate(source, SPV_ENV_VULKAN_1_3);
    spvtools::Optimizer flatten(SPV_ENV_VULKAN_1_3);
    flatten.RegisterPass(spvtools::CreateFlattenDecorationPass());
    Words normalized;
    Require(flatten.Run(source.data(), source.size(), &normalized), "specialization decoration normalization failed");
    std::map<uint32_t, uint32_t> widths, constants, spec_ids;
    for (size_t p = 5; p < normalized.size(); p += normalized[p] >> 16) {
        auto op = spv::Op(normalized[p] & 0xffff);
        if (op == spv::OpTypeBool)
            widths[normalized[p + 1]] = 4;
        else if (op == spv::OpTypeInt || op == spv::OpTypeFloat)
            widths[normalized[p + 1]] = normalized[p + 2] / 8;
        else if (op == spv::OpSpecConstant || op == spv::OpSpecConstantTrue || op == spv::OpSpecConstantFalse)
            constants[normalized[p + 2]] = normalized[p + 1];
        else if (op == spv::OpDecorate && normalized[p + 2] == spv::DecorationSpecId)
            spec_ids[normalized[p + 3]] = normalized[p + 1];
    }
    std::unordered_map<uint32_t, Words> values;
    if (info) {
        Require(!info->mapEntryCount || info->pMapEntries, "missing specialization map");
        Require(!info->dataSize || info->pData, "missing specialization data");
        for (uint32_t i = 0; i < info->mapEntryCount; ++i) {
            const auto &entry = info->pMapEntries[i];
            Require(entry.offset <= info->dataSize && entry.size <= info->dataSize - entry.offset,
                    "specialization data range overflow");
            auto scalar = spec_ids.find(entry.constantID);
            // Vulkan ignores map entries whose ID is not present in the module.
            if (scalar == spec_ids.end()) continue;
            Require(entry.size == widths.at(constants.at(scalar->second)) && entry.size && entry.size <= 8,
                    "specialization value size does not match its scalar type");
            Words value((entry.size + 3) / 4, 0);
            if (entry.size) std::memcpy(value.data(), static_cast<const uint8_t *>(info->pData) + entry.offset, entry.size);
            Require(values.emplace(entry.constantID, std::move(value)).second, "duplicate specialization ID");
        }
    }
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
    std::string error;
    optimizer.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
        error += std::string(message) + "\n";
    });
    optimizer.RegisterPass(spvtools::CreateSetSpecConstantDefaultValuePass(values));
    optimizer.RegisterPass(spvtools::CreateFreezeSpecConstantValuePass());
    optimizer.RegisterPass(spvtools::CreateFoldSpecConstantOpAndCompositePass());
    Words result;
    bool specialized = optimizer.Run(normalized.data(), normalized.size(), &result);
    Require(specialized, "specialization failed: " + error);
    return result;
}
Words ImportStages(const Words &scheduler, const std::vector<TypedContinuationProgram> &stages) {
    Validate(scheduler, SPV_ENV_VULKAN_1_3);
    std::map<std::string, uint32_t> targets;
    std::map<uint32_t, uint32_t> function_types;
    std::map<uint32_t, Words> signatures;
    std::map<uint32_t, uint32_t> address_pointers;
    std::set<uint32_t> void_types, address_types;
    bool physical_capability = false;
    uint32_t private_variable = 0, private_words = 0;
    std::set<std::string> requested;
    for (const auto &stage : stages) {
        Require((stage.transport == TypedFrameTransport::PhysicalAddress || stage.transport == TypedFrameTransport::PrivateArray) &&
                    !stage.step_export_name.empty(),
                "stage does not expose a physical frame library");
        Require(requested.insert(stage.step_export_name).second, "duplicate stage export");
        if (stage.transport == TypedFrameTransport::PrivateArray) {
            Require(stage.private_frame_words && (!private_words || private_words == stage.private_frame_words),
                    "private stage libraries disagree on invocation stack capacity");
            private_words = stage.private_frame_words;
        }
    }
    for (size_t p = 5; p < scheduler.size(); p += scheduler[p] >> 16) {
        auto op = spv::Op(scheduler[p] & 0xffff);
        uint32_t length = scheduler[p] >> 16;
        if (op == spv::OpCapability && scheduler[p + 1] == spv::CapabilityPhysicalStorageBufferAddresses)
            physical_capability = true;
        if (op == spv::OpName) {
            auto name = String(scheduler, p + 2, p + length);
            if (name == "vel_rt_private_frames") {
                Require(!private_variable, "ambiguous scheduler private frame variable");
                private_variable = scheduler[p + 1];
            }
            auto end = name.find('(');
            auto stem = name.substr(0, end);
            if (requested.count(stem)) Require(targets.emplace(stem, scheduler[p + 1]).second, "ambiguous scheduler stub");
        } else if (op == spv::OpFunction)
            function_types[scheduler[p + 2]] = scheduler[p + 4];
        else if (op == spv::OpTypeFunction)
            signatures[scheduler[p + 1]] = {scheduler.begin() + p + 2, scheduler.begin() + p + length};
        else if (op == spv::OpTypeVoid)
            void_types.insert(scheduler[p + 1]);
        else if (op == spv::OpTypeInt && scheduler[p + 2] == 64 && !scheduler[p + 3])
            address_types.insert(scheduler[p + 1]);
        else if (op == spv::OpTypePointer && scheduler[p + 2] == spv::StorageClassFunction)
            address_pointers[scheduler[p + 1]] = scheduler[p + 3];
    }
    Require(targets.size() == requested.size(), "scheduler is missing a required typed stage stub");
    Words annotations, declarations, imports;
    if (private_words) {
        Require(private_variable, "scheduler is missing per-invocation private frame storage");
        auto name = Literal("vel_rt_private_frames");
        annotations.push_back((uint32_t(name.size() + 4) << 16) | spv::OpDecorate);
        annotations.push_back(private_variable);
        annotations.push_back(spv::DecorationLinkageAttributes);
        annotations.insert(annotations.end(), name.begin(), name.end());
        annotations.push_back(spv::LinkageTypeExport);
    }
    struct Adapter {
        uint32_t import, result_type, address_type;
        bool reference;
    };
    std::map<uint32_t, Adapter> adapters;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> import_signatures;
    for (const auto &[id, signature] : signatures)
        if (signature.size() == 2 && void_types.count(signature[0]) && address_types.count(signature[1]))
            import_signatures.emplace(std::make_pair(signature[0], signature[1]), id);
    uint32_t bound = scheduler[3];
    for (const auto &[name, function] : targets) {
        const auto &signature = signatures.at(function_types.at(function));
        Require(signature.size() == 2 && void_types.count(signature[0]), "scheduler stage stub must be void(uint64)");
        bool reference = address_pointers.count(signature[1]) != 0;
        uint32_t address_type = reference ? address_pointers.at(signature[1]) : signature[1];
        Require(address_types.count(address_type), "scheduler stage stub argument must be uint64");
        uint32_t import = bound++, parameter = bound++, function_type;
        auto signature_key = std::make_pair(signature[0], address_type);
        auto found_signature = import_signatures.find(signature_key);
        if (found_signature != import_signatures.end())
            function_type = found_signature->second;
        else {
            function_type = bound++;
            import_signatures.emplace(signature_key, function_type);
            Emit(declarations, spv::OpTypeFunction, {function_type, signature[0], address_type});
        }
        adapters.emplace(function, Adapter{import, signature[0], address_type, reference});
        Emit(imports, spv::OpFunction, {signature[0], import, spv::FunctionControlMaskNone, function_type});
        Emit(imports, spv::OpFunctionParameter, {address_type, parameter});
        Emit(imports, spv::OpFunctionEnd, {});
        auto literal = Literal(name);
        annotations.push_back((uint32_t(literal.size() + 4) << 16) | spv::OpDecorate);
        annotations.push_back(import);
        annotations.push_back(spv::DecorationLinkageAttributes);
        annotations.insert(annotations.end(), literal.begin(), literal.end());
        annotations.push_back(spv::LinkageTypeImport);
    }
    Words result(scheduler.begin(), scheduler.begin() + 5);
    Emit(result, spv::OpCapability, {spv::CapabilityLinkage});
    if (!physical_capability) Emit(result, spv::OpCapability, {spv::CapabilityPhysicalStorageBufferAddresses});
    bool skipping = false, decorated = false, declared = false;
    const Adapter *adapter = nullptr;
    uint32_t argument = 0, labels = 0;
    for (size_t p = 5; p < scheduler.size(); p += scheduler[p] >> 16) {
        auto op = spv::Op(scheduler[p] & 0xffff);
        uint32_t length = scheduler[p] >> 16;
        if (op == spv::OpMemoryModel) {
            Require(
                scheduler[p + 2] == spv::MemoryModelGLSL450 && (scheduler[p + 1] == spv::AddressingModelLogical ||
                                                                scheduler[p + 1] == spv::AddressingModelPhysicalStorageBuffer64),
                "scheduler memory model is incompatible with physical typed stages");
            Emit(result, spv::OpMemoryModel, {spv::AddressingModelPhysicalStorageBuffer64, spv::MemoryModelGLSL450});
            continue;
        }
        if (!decorated && op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer) {
            result.insert(result.end(), annotations.begin(), annotations.end());
            decorated = true;
        }
        if (!declared && op == spv::OpFunction) {
            result.insert(result.end(), declarations.begin(), declarations.end());
            result.insert(result.end(), imports.begin(), imports.end());
            declared = true;
        }
        if (op == spv::OpFunction) {
            auto found = adapters.find(scheduler[p + 2]);
            adapter = found == adapters.end() ? nullptr : &found->second;
            skipping = adapter != nullptr;
            argument = 0;
            labels = 0;
        }
        if (skipping && op == spv::OpFunctionParameter) {
            Require(!argument, "scheduler stage stub has too many parameters");
            argument = scheduler[p + 2];
        }
        if (skipping && op == spv::OpLabel) {
            Require(++labels == 1 && argument, "scheduler stage stub is not empty");
            result.insert(result.end(), scheduler.begin() + p, scheduler.begin() + p + length);
            uint32_t value = argument;
            if (adapter->reference) {
                value = bound++;
                Emit(result, spv::OpLoad, {adapter->address_type, value, argument});
            }
            Emit(result, spv::OpFunctionCall, {adapter->result_type, bound++, adapter->import, value});
            Emit(result, spv::OpReturn, {});
        } else if (skipping && op != spv::OpFunction && op != spv::OpFunctionParameter && op != spv::OpFunctionEnd) {
            Require(op == spv::OpReturn || op == spv::OpLine || op == spv::OpNoLine, "scheduler stage stub is not empty");
        } else
            result.insert(result.end(), scheduler.begin() + p, scheduler.begin() + p + length);
        if (op == spv::OpFunctionEnd) {
            if (skipping) Require(labels == 1, "scheduler stage stub has no body");
            skipping = false;
        }
    }
    result[3] = bound;
    Validate(result, SPV_ENV_UNIVERSAL_1_5);
    return result;
}
}  // namespace

TypedContinuationProgram CompileTypedPipelineStage(const Words &source, const std::string &entry, VkShaderStageFlagBits stage,
                                                   const VkSpecializationInfo *specialization, const std::string &export_name) {
    TypedContinuationCompileOptions options;
    options.transport = TypedFrameTransport::PhysicalAddress;
    return CompileTypedPipelineStage(source, entry, stage, specialization, export_name, options);
}
TypedContinuationProgram CompileTypedPipelineStage(const Words &source, const std::string &entry, VkShaderStageFlagBits stage,
                                                   const VkSpecializationInfo *specialization, const std::string &export_name,
                                                   const TypedContinuationCompileOptions &configuration) {
    auto options = configuration;
    options.emit_compute_entry = false;
    options.step_export_name = export_name;
    return CompileTypedContinuationSteps(Specialize(source, specialization), entry, Model(stage), options);
}

Words LinkTypedPipelineProgram(const Words &scheduler, const std::vector<TypedContinuationProgram> &stages) {
    Require(!stages.empty(), "empty stage list");
    std::vector<Words> modules{ImportStages(scheduler, stages)};
    std::set<Words> required_modes;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> exclusive_modes;
    for (const auto &stage : stages) {
        Validate(stage.spirv, SPV_ENV_UNIVERSAL_1_5);
        modules.push_back(stage.spirv);
        for (const auto &mode : stage.required_execution_modes) {
            Words words{mode.mode};
            words.insert(words.end(), mode.operands.begin(), mode.operands.end());
            required_modes.insert(words);
            uint32_t family = 0;
            if (mode.mode == spv::ExecutionModeDenormPreserve || mode.mode == spv::ExecutionModeDenormFlushToZero) family = 1;
            if (mode.mode == spv::ExecutionModeRoundingModeRTE || mode.mode == spv::ExecutionModeRoundingModeRTZ) family = 2;
            if (family) {
                Require(mode.operands.size() == 1, "invalid floating-point execution mode");
                auto inserted = exclusive_modes.emplace(std::make_pair(family, mode.operands[0]), mode.mode);
                Require(inserted.second || inserted.first->second == mode.mode,
                        "incompatible per-stage floating-point execution modes require instruction-level lowering");
            }
        }
    }
    spvtools::Context context(SPV_ENV_UNIVERSAL_1_5);
    std::string error;
    context.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
        error += std::string(message) + "\n";
    });
    spvtools::LinkerOptions options;
    options.SetCreateLibrary(false);
    options.SetAllowPartialLinkage(false);
    Words result;
    auto linked = spvtools::Link(context, modules, &result, options);
    Require(linked == SPV_SUCCESS, "stage link failed: " + error);
    // SPIR-V1.4+ interfaces include every global used by called stage code.
    // After eliminating dead helpers, conservatively list all remaining globals.
    spvtools::Optimizer optimizer(SPV_ENV_UNIVERSAL_1_5);
    optimizer.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
    optimizer.RegisterPass(spvtools::CreateRemoveDuplicatesPass());
    Words cleaned;
    spvtools::OptimizerOptions optimizer_options;
    optimizer_options.set_run_validator(false);
    Require(optimizer.Run(result.data(), result.size(), &cleaned, optimizer_options), "linked stage cleanup failed");
    std::vector<uint32_t> globals;
    bool in_function = false;
    uint32_t entry_count = 0;
    for (size_t p = 5; p < cleaned.size(); p += cleaned[p] >> 16) {
        auto op = spv::Op(cleaned[p] & 0xffff);
        if (op == spv::OpFunction) in_function = true;
        if (op == spv::OpVariable && !in_function) globals.push_back(cleaned[p + 2]);
    }
    result.assign(cleaned.begin(), cleaned.begin() + 5);
    for (size_t p = 5; p < cleaned.size(); p += cleaned[p] >> 16) {
        uint32_t length = cleaned[p] >> 16;
        if (spv::Op(cleaned[p] & 0xffff) == spv::OpEntryPoint) {
            Require(++entry_count == 1, "scheduler must have one compute entry point");
            Require(cleaned[p + 1] == spv::ExecutionModelGLCompute, "scheduler entry is not compute");
            auto name = Literal(String(cleaned, p + 3, p + length));
            result.push_back((uint32_t(3 + name.size() + globals.size()) << 16) | spv::OpEntryPoint);
            result.push_back(cleaned[p + 1]);
            result.push_back(cleaned[p + 2]);
            result.insert(result.end(), name.begin(), name.end());
            result.insert(result.end(), globals.begin(), globals.end());
            for (const auto &mode : required_modes) {
                result.push_back((uint32_t(mode.size() + 2) << 16) | spv::OpExecutionMode);
                result.push_back(cleaned[p + 2]);
                result.insert(result.end(), mode.begin(), mode.end());
            }
        } else
            result.insert(result.end(), cleaned.begin() + p, cleaned.begin() + p + length);
    }
    Require(entry_count == 1, "scheduler has no compute entry point");
    Validate(result, SPV_ENV_VULKAN_1_3);
    return result;
}
}  // namespace vel::rt
