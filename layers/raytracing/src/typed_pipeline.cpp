// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "typed_pipeline.h"
#include "vkrt_spirv_support.h"
#include "spirv_environment.h"
#include "spirv_memory_model.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/linker.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
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
    // Scalar layout is an explicit native prerequisite of the typed backend.
    // In particular, physical shader-record pointers retain source byte offsets.
    spvtools::ValidatorOptions validation;
    validation.SetScalarBlockLayout(true);
    bool valid = tools.Validate(words.data(), words.size(), validation);
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
    const auto environment = VulkanEnvironmentForSPIRV(source.size() > 1 ? source[1] : 0);
    Validate(source, environment);
    spvtools::Optimizer flatten(environment);
    flatten.RegisterPass(spvtools::CreateFlattenDecorationPass());
    Words normalized;
    spvtools::ValidatorOptions validation;
    validation.SetScalarBlockLayout(true);
    spvtools::OptimizerOptions optimizer_options;
    optimizer_options.set_validator_options(validation);
    Require(flatten.Run(source.data(), source.size(), &normalized, optimizer_options),
            "specialization decoration normalization failed");
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
    spvtools::Optimizer optimizer(environment);
    std::string error;
    optimizer.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
        error += std::string(message) + "\n";
    });
    optimizer.RegisterPass(spvtools::CreateSetSpecConstantDefaultValuePass(values));
    optimizer.RegisterPass(spvtools::CreateFreezeSpecConstantValuePass());
    optimizer.RegisterPass(spvtools::CreateFoldSpecConstantOpAndCompositePass());
    Words result;
    bool specialized = optimizer.Run(normalized.data(), normalized.size(), &result, optimizer_options);
    Require(specialized, "specialization failed: " + error);
    return result;
}
Words ImportStages(const Words &scheduler, const std::vector<TypedContinuationProgram> &stages) {
    Validate(scheduler, VulkanEnvironmentForSPIRV(scheduler.size() > 1 ? scheduler[1] : 0));
    std::map<std::string, uint32_t> targets;
    std::map<uint32_t, uint32_t> function_types;
    std::map<uint32_t, Words> signatures;
    std::map<uint32_t, uint32_t> address_pointers;
    std::set<uint32_t> void_types, address_types;
    bool physical_capability = false;
    std::set<std::string> requested;
    for (const auto &stage : stages) {
        Require(stage.transport == TypedFrameTransport::PhysicalAddress && !stage.step_export_name.empty(),
                "stage does not expose a physical frame library");
        Require(requested.insert(stage.step_export_name).second, "duplicate stage export");
    }
    for (size_t p = 5; p < scheduler.size(); p += scheduler[p] >> 16) {
        auto op = spv::Op(scheduler[p] & 0xffff);
        uint32_t length = scheduler[p] >> 16;
        if (op == spv::OpCapability && scheduler[p + 1] == spv::CapabilityPhysicalStorageBufferAddresses)
            physical_capability = true;
        if (op == spv::OpName) {
            auto name = String(scheduler, p + 2, p + length);
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
            Require((scheduler[p + 2] == spv::MemoryModelGLSL450 || scheduler[p + 2] == spv::MemoryModelVulkan) &&
                        (scheduler[p + 1] == spv::AddressingModelLogical ||
                         scheduler[p + 1] == spv::AddressingModelPhysicalStorageBuffer64),
                    "scheduler memory model is incompatible with physical typed stages");
            Emit(result, spv::OpMemoryModel, {spv::AddressingModelPhysicalStorageBuffer64, scheduler[p + 2]});
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
    Validate(result, UniversalEnvironmentForSPIRV(result.at(1)));
    return result;
}

bool UsesVulkanMemoryModel(const Words &module) {
    for (size_t p = 5; p < module.size(); p += module[p] >> 16)
        if ((module[p] & 65535) == spv::OpMemoryModel) return module[p + 2] == spv::MemoryModelVulkan;
    return false;
}
Words UpgradeTypedMemoryModel(const Words &module) {
    if (UsesVulkanMemoryModel(module)) return module;
    Words shell = module;
    uint32_t addressing = UINT32_MAX;
    for (size_t p = 5; p < shell.size(); p += shell[p] >> 16) {
        if ((shell[p] & 65535) != spv::OpMemoryModel) continue;
        Require(shell[p + 2] == spv::MemoryModelGLSL450, "incompatible typed memory model");
        addressing = shell[p + 1];
        shell[p + 1] = spv::AddressingModelLogical;
    }
    Require(addressing == spv::AddressingModelLogical || addressing == spv::AddressingModelPhysicalStorageBuffer64,
            "unsupported typed addressing model");
    // The generic upgrader narrows existing Device atomic/barrier scopes to
    // QueueFamily. Preserve the application's wider scope using temporary
    // ShaderCall scope IDs, restored to Device after the pass. Ordinary data
    // constants are never rewritten. The backend enables native Device scope.
    RewriteTypedMemoryScopes(shell, spv::ScopeDevice, spv::ScopeShaderCallKHR);
    // SPIRV-Tools' memory-semantics pass is gated on Logical addressing. Give
    // it that addressing shell without changing any pointer types/instructions,
    // then restore the actual addressing before validating or exposing output.
    // Apply this only AFTER typed lowering: RT payload pointer aggregates now
    // contain numeric identities, so a coherent pointee cannot accidentally
    // mark the payload itself as a NonPrivatePointer access.
    spvtools::Optimizer upgrade(UniversalEnvironmentForSPIRV(module.at(1)));
    std::string errors;
    upgrade.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
        errors += std::string(message) + "\n";
    });
    upgrade.RegisterPass(spvtools::CreateUpgradeMemoryModelPass());
    spvtools::OptimizerOptions options;
    options.set_run_validator(false);  // The temporary addressing shell is internal to this pass.
    Words result;
    Require(upgrade.Run(shell.data(), shell.size(), &result, options), "memory model upgrade failed: " + errors);
    for (size_t p = 5; p < result.size(); p += result[p] >> 16)
        if ((result[p] & 65535) == spv::OpMemoryModel) result[p + 1] = addressing;
    Require(UsesVulkanMemoryModel(result), "memory model upgrade did not run");
    LowerTypedShaderCallScopes(result);
    Validate(result, UniversalEnvironmentForSPIRV(result.at(1)));
    return result;
}
}  // namespace

namespace {
bool TypeAnnotation(uint32_t op) {
    return op == spv::OpName || op == spv::OpMemberName || op == spv::OpDecorate || op == spv::OpMemberDecorate ||
           op == spv::OpDecorateId || op == spv::OpDecorateString || op == spv::OpMemberDecorateString;
}

Words RemoveUnusedTypes(const Words &source) {
    struct Types {
        std::map<uint32_t, std::set<uint32_t>> dependencies;
        std::set<uint32_t> live;
    } types;
    auto collect = [](void *user, const spv_parsed_instruction_t *instruction) {
        auto &data = *static_cast<Types *>(user);
        if (TypeAnnotation(instruction->opcode)) return SPV_SUCCESS;
        const bool type = std::strncmp(spvOpcodeString(instruction->opcode), "Type", 4) == 0;
        // A forward pointer and its eventual pointer definition share one ID.
        const auto id = instruction->opcode == spv::OpTypeForwardPointer ? instruction->words[1] : instruction->result_id;
        auto &uses = type ? data.dependencies[id] : data.live;
        for (uint32_t n = 0; n < instruction->num_operands; ++n) {
            const auto &operand = instruction->operands[n];
            if (operand.type == SPV_OPERAND_TYPE_ID || operand.type == SPV_OPERAND_TYPE_TYPE_ID)
                uses.insert(instruction->words + operand.offset, instruction->words + operand.offset + operand.num_words);
        }
        return SPV_SUCCESS;
    };
    auto context = spvContextCreate(UniversalEnvironmentForSPIRV(source.at(1)));
    const auto parsed = spvBinaryParse(context, &types, source.data(), source.size(), nullptr, collect, nullptr);
    spvContextDestroy(context);
    Require(parsed == SPV_SUCCESS, "cannot inspect selected entry type dependencies");
    std::vector<uint32_t> pending(types.live.begin(), types.live.end());
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto found = types.dependencies.find(id);
        if (found == types.dependencies.end()) continue;
        for (auto dependency : found->second)
            if (types.live.insert(dependency).second) pending.push_back(dependency);
    }
    Words result(source.begin(), source.begin() + 5);
    for (size_t p = 5; p < source.size(); p += source[p] >> 16) {
        const auto op = source[p] & 65535;
        if ((std::strncmp(spvOpcodeString(op), "Type", 4) == 0 || TypeAnnotation(op)) && types.dependencies.count(source[p + 1]) &&
            !types.live.count(source[p + 1]))
            continue;
        result.insert(result.end(), source.begin() + p, source.begin() + p + (source[p] >> 16));
    }
    return result;
}

Words SelectShaderEntry(const Words &source, const std::string &entry, VkShaderStageFlagBits stage) {
    const auto environment = VulkanEnvironmentForSPIRV(source.size() > 1 ? source[1] : 0);
    Validate(source, environment);
    auto matches = [&](uint32_t model) {
        switch (stage) {
            case VK_SHADER_STAGE_VERTEX_BIT:
                return model == spv::ExecutionModelVertex;
            case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
                return model == spv::ExecutionModelTessellationControl;
            case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
                return model == spv::ExecutionModelTessellationEvaluation;
            case VK_SHADER_STAGE_GEOMETRY_BIT:
                return model == spv::ExecutionModelGeometry;
            case VK_SHADER_STAGE_FRAGMENT_BIT:
                return model == spv::ExecutionModelFragment;
            case VK_SHADER_STAGE_COMPUTE_BIT:
                return model == spv::ExecutionModelGLCompute;
            case VK_SHADER_STAGE_TASK_BIT_EXT:
                return model == spv::ExecutionModelTaskEXT || model == spv::ExecutionModelTaskNV;
            case VK_SHADER_STAGE_MESH_BIT_EXT:
                return model == spv::ExecutionModelMeshEXT || model == spv::ExecutionModelMeshNV;
            case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            case VK_SHADER_STAGE_MISS_BIT_KHR:
            case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
            case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
                return model == Model(stage);
            default:
                return false;
        }
    };
    uint32_t selected = 0;
    for (size_t p = 5; p < source.size(); p += source[p] >> 16)
        if ((source[p] & 65535) == spv::OpEntryPoint && matches(source[p + 1]) &&
            String(source, p + 3, p + (source[p] >> 16)) == entry) {
            Require(!selected, "ambiguous native shader entry point");
            selected = source[p + 2];
        }
    Require(selected != 0, "native shader entry point is absent");
    Words module(source.begin(), source.begin() + 5);
    for (size_t p = 5; p < source.size(); p += source[p] >> 16) {
        auto op = source[p] & 65535;
        if (op == spv::OpEntryPoint && source[p + 2] != selected) continue;
        if ((op == spv::OpExecutionMode || op == spv::OpExecutionModeId) && source[p + 1] != selected) continue;
        module.insert(module.end(), source.begin() + p, source.begin() + p + (source[p] >> 16));
    }
    const bool volatile_access = HasVolatileAccess(module);
    spvtools::Optimizer cleanup(environment);
    if (volatile_access) cleanup.RegisterPass(spvtools::CreateFlattenDecorationPass());
    cleanup.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
    if (!volatile_access) cleanup.RegisterPass(spvtools::CreateAggressiveDCEPass(true));
    cleanup.RegisterPass(spvtools::CreateDeadVariableEliminationPass());
    cleanup.RegisterPass(spvtools::CreateEliminateDeadConstantPass());
    if (!volatile_access) cleanup.RegisterPass(spvtools::CreateTrimCapabilitiesPass());
    spvtools::ValidatorOptions validation;
    validation.SetScalarBlockLayout(true);
    spvtools::OptimizerOptions options;
    options.set_validator_options(validation);
    Words cleaned;
    Require(cleanup.Run(module.data(), module.size(), &cleaned, options), "native entry cleanup failed");
    if (volatile_access) {
        // Unselected RT entries can leave AS/payload types after safe cleanup.
        // Remove only unused declarations; executable volatile reads stay put.
        cleaned = RemoveUnusedTypes(cleaned);
        spvtools::Optimizer trim(environment);
        trim.RegisterPass(spvtools::CreateTrimCapabilitiesPass());
        Words trimmed;
        Require(trim.Run(cleaned.data(), cleaned.size(), &trimmed, options), "selected entry capability cleanup failed");
        return trimmed;
    }
    return cleaned;
}

}  // namespace

Words SelectNativeShaderEntry(const Words &source, const std::string &entry, VkShaderStageFlagBits stage) {
    const auto cleaned = SelectShaderEntry(source, entry, stage);
    const auto environment = VulkanEnvironmentForSPIRV(source.size() > 1 ? source[1] : 0);
    Words module;
    // The generic trimmer does not cover every RT capability. Source validation
    // and reachable-code elimination precede this removal; validating again
    // proves that no retained declaration/operation requires the RT capability.
    module.assign(cleaned.begin(), cleaned.begin() + 5);
    for (size_t p = 5; p < cleaned.size(); p += cleaned[p] >> 16) {
        const auto op = cleaned[p] & 65535;
        if (op == spv::OpCapability && cleaned[p + 1] == spv::CapabilityRayTracingKHR) continue;
        if (op == spv::OpExtension && String(cleaned, p + 1, p + (cleaned[p] >> 16)) == "SPV_KHR_ray_tracing") continue;
        module.insert(module.end(), cleaned.begin() + p, cleaned.begin() + p + (cleaned[p] >> 16));
    }
    Validate(module, environment);
    return module;
}

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
    auto selected = Specialize(source, specialization);
    // Ray query is outside the delivered contract: the layer advertises neither
    // VK_KHR_ray_query nor a consuming-stage lowering path, so an RT stage that
    // still declares a query type is rejected instead of being half-compiled.
    for (size_t p = 5; p < selected.size(); p += selected[p] >> 16)
        Require((selected[p] & 65535) != spv::OpTypeRayQueryKHR, "ray query is not supported by this layer");
    return CompileTypedContinuationSteps(selected, entry, Model(stage), options);
}

TypedContinuationProgram RenameTypedPipelineStage(const TypedContinuationProgram &stage, const std::string &export_name) {
    Require(!export_name.empty() &&
                std::all_of(export_name.begin(), export_name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }),
            "invalid stage export name");
    Require(stage.transport == TypedFrameTransport::PhysicalAddress && !stage.step_export_name.empty(),
            "stage does not expose a physical frame library");
    const auto &source = stage.spirv;
    Require(source.size() >= 5, "missing compiled stage library");
    Words renamed(source.begin(), source.begin() + 5);
    renamed.reserve(source.size());
    uint32_t exports = 0;
    const auto name = Literal(export_name);
    Require(name.size() <= UINT16_MAX - 4, "stage export name exceeds the SPIR-V instruction limit");
    for (size_t p = 5; p < source.size();) {
        const auto length = source[p] >> 16, op = source[p] & 65535;
        Require(length != 0 && length <= source.size() - p, "invalid compiled stage instruction");
        if (op == spv::OpDecorate && length >= 3 && source[p + 2] == spv::DecorationLinkageAttributes) {
            Require(length >= 5 && source[p + length - 1] == spv::LinkageTypeExport &&
                        String(source, p + 3, p + length - 1) == stage.step_export_name,
                    "compiled stage has unexpected linkage");
            ++exports;
            renamed.insert(renamed.end(),
                           {(uint32_t(name.size() + 4) << 16) | spv::OpDecorate, source[p + 1], spv::DecorationLinkageAttributes});
            renamed.insert(renamed.end(), name.begin(), name.end());
            renamed.push_back(spv::LinkageTypeExport);
        } else
            renamed.insert(renamed.end(), source.begin() + p, source.begin() + p + length);
        p += length;
    }
    Require(exports == 1, "compiled stage must have one export");
    auto result = stage;
    result.spirv = std::move(renamed);
    result.step_export_name = export_name;
    return result;
}

Words LinkTypedPipelineProgram(const Words &scheduler, const std::vector<TypedContinuationProgram> &stages) {
    Require(!stages.empty(), "empty stage list");
    std::vector<Words> modules{ImportStages(scheduler, stages)};
    std::set<Words> required_modes;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> exclusive_modes;
    uint32_t version = scheduler.at(1);
    for (const auto &stage : stages) {
        Validate(stage.spirv, UniversalEnvironmentForSPIRV(stage.spirv.size() > 1 ? stage.spirv[1] : 0));
        version = std::max(version, stage.spirv.at(1));
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
    // A linked module has one memory model. Upgrade GLSL450 peers and the
    // scheduler with the actual SPIRV-Tools semantics conversion, including
    // coherent/volatile accesses, barriers and atomic availability/visibility.
    if (std::any_of(modules.begin(), modules.end(), UsesVulkanMemoryModel))
        for (auto &module : modules) module = UpgradeTypedMemoryModel(module);
    const auto environment = UniversalEnvironmentForSPIRV(version);
    spvtools::Context context(environment);
    std::string error;
    context.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
        error += std::string(message) + "\n";
    });
    spvtools::LinkerOptions options;
    options.SetCreateLibrary(false);
    options.SetAllowPartialLinkage(false);
    options.SetUseHighestVersion(true);
    Words result;
    auto linked = spvtools::Link(context, modules, &result, options);
    Require(linked == SPV_SUCCESS, "stage link failed: " + error);
    // SPIR-V1.4+ interfaces include every global used by called stage code.
    // After eliminating dead helpers, conservatively list all remaining globals.
    spvtools::Optimizer optimizer(environment);
    optimizer.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
    optimizer.RegisterPass(spvtools::CreateRemoveDuplicatesPass());
    Words cleaned;
    spvtools::OptimizerOptions optimizer_options;
    optimizer_options.set_run_validator(false);
    Require(optimizer.Run(result.data(), result.size(), &cleaned, optimizer_options), "linked stage cleanup failed");
    // Linking and duplicate elimination can unify structurally equal aggregate
    // types. OpCopyLogical requires different type IDs; its value-preserving
    // conversion becomes OpCopyObject when both IDs now name the same type.
    struct Metadata {
        std::unordered_map<uint32_t, uint32_t> result_types;
        size_t offset = 5;
    } metadata;
    auto collect_type = [](void *user, const spv_parsed_instruction_t *instruction) {
        auto &data = *static_cast<Metadata *>(user);
        if (instruction->result_id && instruction->type_id) data.result_types.emplace(instruction->result_id, instruction->type_id);
        data.offset += instruction->num_words;
        return SPV_SUCCESS;
    };
    Require(spvBinaryParse(context.CContext(), &metadata, cleaned.data(), cleaned.size(), nullptr, collect_type, nullptr) ==
                SPV_SUCCESS,
            "linked type parse failed");
    for (size_t p = 5; p < cleaned.size(); p += cleaned[p] >> 16) {
        if (spv::Op(cleaned[p] & 0xffff) == spv::OpCopyLogical && metadata.result_types.at(cleaned[p + 3]) == cleaned[p + 1])
            cleaned[p] = (cleaned[p] & 0xffff0000u) | spv::OpCopyObject;
    }
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
    LowerTypedShaderCallScopes(result);
    Validate(result, VulkanEnvironmentForSPIRV(result.at(1)));
    return result;
}
}  // namespace vel::rt

extern "C" VkResult vkrt_spirv_select_native_entry(const uint32_t *words, size_t word_count, const char *name,
                                                   VkShaderStageFlagBits stage, uint32_t **out_words, size_t *out_word_count) {
    if (out_words) *out_words = nullptr;
    if (out_word_count) *out_word_count = 0;
    if (!out_words || !out_word_count || !words || !name || word_count < 5 || word_count > SIZE_MAX / sizeof(*words) ||
        word_count > static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(*words))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    try {
        const std::vector<uint32_t> source(words, words + word_count);
        const auto selected = vel::rt::SelectNativeShaderEntry(source, name, stage);
        if (selected.size() > SIZE_MAX / sizeof(*words)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        auto copy = static_cast<uint32_t *>(std::malloc(selected.size() * sizeof(*words)));
        if (!copy) return VK_ERROR_OUT_OF_HOST_MEMORY;
        std::memcpy(copy, selected.data(), selected.size() * sizeof(*words));
        *out_words = copy;
        *out_word_count = selected.size();
        return VK_SUCCESS;
    } catch (const std::bad_alloc &) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (...) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
}
