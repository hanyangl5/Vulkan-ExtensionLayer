// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_query_rewrite.h"

#include <spirv-tools/libspirv.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace vel::omm {
namespace {
struct Instruction {
    spv::Op op;
    std::vector<uint32_t> words;
    uint32_t type = 0;
    uint32_t result = 0;
    uint32_t offset = 0;
};
struct Function {
    uint32_t type = 0;
    std::vector<uint32_t> parameters;
    std::vector<size_t> query_parameters;
};
struct Rewrite {
    const std::vector<uint32_t>& source;
    bool expose;
    const QueryConsumerOptions* consumer = nullptr;
    std::vector<Instruction> instructions;
    std::unordered_map<uint32_t, Instruction> definitions;
    std::unordered_map<uint32_t, Function> functions;
    std::unordered_map<uint32_t, uint32_t> types;
    std::unordered_map<uint32_t, uint32_t> flags;
    std::vector<std::vector<uint32_t>> declarations;
    std::vector<std::vector<uint32_t>> globals;
    std::map<std::vector<uint32_t>, uint32_t> interned;
    uint32_t bound;
    uint32_t uint_type = 0;
    uint32_t mask = 0;
    uint32_t no_opaque = 0;
    uint32_t bool_type = 0, float_type = 0, metadata_type = 0, bary_type = 0, state_type = 0;
    uint32_t zero = 0, one = 0, two = 0, false_value = 0, true_value = 0, zero_bary = 0, default_metadata = 0;
    std::unordered_map<uint32_t, uint32_t> metadata;
    std::unordered_map<uint32_t, std::vector<uint32_t>> loop_merges;
    std::unordered_map<uint32_t, uint32_t> final_blocks;
    std::unordered_set<uint32_t> hoisted_merges;
    QueryStateModule module;
    std::string error;

    Rewrite(const std::vector<uint32_t>& input, bool prepare, const QueryConsumerOptions* options = nullptr)
        : source(input), expose(prepare), consumer(options), bound(input[3]) {}
    uint32_t id() {
        if (bound >= 0x3fffff) throw std::length_error("SPIR-V ID limit");
        return bound++;
    }
    static std::vector<uint32_t> make(spv::Op op, std::initializer_list<uint32_t> operands) {
        std::vector<uint32_t> words{(uint32_t(operands.size() + 1) << 16) | uint32_t(op)};
        words.insert(words.end(), operands);
        return words;
    }
    static void emit(std::vector<uint32_t>& destination, const std::vector<uint32_t>& words) {
        destination.insert(destination.end(), words.begin(), words.end());
    }
    uint32_t declaration(spv::Op op, const std::vector<uint32_t>& operands) {
        std::vector<uint32_t> key{uint32_t(op)};
        key.insert(key.end(), operands.begin(), operands.end());
        auto found = interned.find(key);
        if (found != interned.end()) return found->second;
        const uint32_t result = id();
        std::vector<uint32_t> words{(uint32_t(operands.size() + 2) << 16) | uint32_t(op), result};
        words.insert(words.end(), operands.begin(), operands.end());
        declarations.push_back(words);
        definitions.emplace(result, Instruction{op, words, 0, result});
        interned.emplace(std::move(key), result);
        return result;
    }
    uint32_t constant(uint32_t value) {
        for (const auto& instruction : instructions)
            if (instruction.op == spv::OpConstant && instruction.type == uint_type && instruction.words.size() == 4 &&
                instruction.words[3] == value)
                return instruction.result;
        const uint32_t result = id();
        declarations.push_back(make(spv::OpConstant, {uint_type, result, value}));
        return result;
    }
    bool query_type(uint32_t type) const {
        std::vector<uint32_t> pending{type};
        std::unordered_set<uint32_t> visited;
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (!visited.insert(current).second) continue;
            const auto found = definitions.find(current);
            if (found == definitions.end()) continue;
            const auto& instruction = found->second;
            if (instruction.op == spv::OpTypeRayQueryKHR) return true;
            if (instruction.op == spv::OpTypeArray || instruction.op == spv::OpTypeRuntimeArray)
                pending.push_back(instruction.words[2]);
            else if (instruction.op == spv::OpTypePointer)
                pending.push_back(instruction.words[3]);
            else if (instruction.op == spv::OpTypeStruct)
                pending.insert(pending.end(), instruction.words.begin() + 2, instruction.words.end());
        }
        return false;
    }
    uint32_t shadow_type(uint32_t type) {
        auto found = types.find(type);
        if (found != types.end()) return found->second;
        const auto instruction = definitions.at(type);
        uint32_t result = 0;
        switch (instruction.op) {
            case spv::OpTypeRayQueryKHR:
                result = consumer ? state_type : uint_type;
                break;
            case spv::OpTypeArray:
                result = declaration(spv::OpTypeArray, {shadow_type(instruction.words[2]), instruction.words[3]});
                break;
            case spv::OpTypeStruct: {
                std::vector<uint32_t> members;
                for (size_t i = 2; i < instruction.words.size(); ++i) {
                    // Keep member indices stable even where no query state is needed.
                    const auto member = instruction.words[i];
                    const auto state = query_type(member) ? shadow_type(member) : uint_type;
                    if (!state) return 0;
                    members.push_back(state);
                }
                result = declaration(spv::OpTypeStruct, members);
                break;
            }
            case spv::OpTypePointer:
                if (instruction.words[2] != spv::StorageClassPrivate && instruction.words[2] != spv::StorageClassFunction) return 0;
                result = declaration(spv::OpTypePointer, {instruction.words[2], shadow_type(instruction.words[3])});
                break;
            default:
                return 0;
        }
        if (result) types.emplace(type, result);
        return result;
    }
    bool parse() {
        uint32_t current = 0;
        for (size_t offset = 5; offset < source.size();) {
            const uint32_t count = source[offset] >> 16;
            Instruction instruction{spv::Op(source[offset] & 0xffffu), {source.begin() + offset, source.begin() + offset + count}};
            instruction.offset = static_cast<uint32_t>(offset);
            bool has_result = false, has_type = false;
            spv::HasResultAndType(instruction.op, &has_result, &has_type);
            if (has_type) instruction.type = instruction.words[1];
            if (has_result) {
                instruction.result = instruction.words[has_type ? 2 : 1];
                definitions.emplace(instruction.result, instruction);
            }
            if (instruction.op == spv::OpTypeInt && instruction.words[2] == 32 && instruction.words[3] == 0)
                uint_type = instruction.result;
            if (has_result && !has_type && instruction.op >= spv::OpTypeVoid && instruction.op <= spv::OpTypeForwardPointer) {
                std::vector<uint32_t> key{uint32_t(instruction.op)};
                key.insert(key.end(), instruction.words.begin() + 2, instruction.words.end());
                interned.emplace(std::move(key), instruction.result);
            }
            if (instruction.op == spv::OpFunction) {
                current = instruction.result;
                functions[current].type = instruction.words[4];
            } else if (instruction.op == spv::OpFunctionParameter) {
                functions[current].parameters.push_back(instruction.result);
            } else if (instruction.op == spv::OpFunctionEnd) {
                current = 0;
            }
            instructions.push_back(std::move(instruction));
            offset += count;
        }
        if (!uint_type) uint_type = declaration(spv::OpTypeInt, {32, 0});
        if (consumer) {
            bool_type = declaration(spv::OpTypeBool, {});
            float_type = declaration(spv::OpTypeFloat, {32});
            metadata_type = declaration(spv::OpTypeVector, {uint_type, 2});
            bary_type = declaration(spv::OpTypeVector, {float_type, 2});
            state_type = id();
            const auto state_declaration = make(spv::OpTypeStruct, {state_type, uint_type, uint_type, metadata_type});
            declarations.push_back(state_declaration);
            definitions.emplace(state_type, Instruction{spv::OpTypeStruct, state_declaration, 0, state_type});
            declaration(spv::OpTypePointer, {spv::StorageClassFunction, uint_type});
            declaration(spv::OpTypePointer, {spv::StorageClassPrivate, uint_type});
            module.typed_state_type = state_type;
        }
        for (auto& pair : functions) {
            auto& function = pair.second;
            const auto& signature = definitions.at(function.type).words;
            if (query_type(signature[2])) {
                error = "query pointer return requires a parallel return ABI";
                return false;
            }
            std::vector<uint32_t> expanded(signature.begin() + 2, signature.end());
            for (size_t i = 0; i < function.parameters.size(); ++i) {
                const auto& parameter = definitions.at(function.parameters[i]);
                if (query_type(parameter.type)) {
                    const auto shadow = shadow_type(parameter.type);
                    if (!shadow) {
                        error = "unsupported query parameter storage";
                        return false;
                    }
                    function.query_parameters.push_back(i);
                    expanded.push_back(shadow);
                }
            }
            if (!function.query_parameters.empty()) function.type = declaration(spv::OpTypeFunction, expanded);
        }
        for (const auto& instruction : instructions) {
            if (!instruction.type || !query_type(instruction.type)) continue;
            switch (instruction.op) {
                case spv::OpVariable:
                case spv::OpFunctionParameter:
                case spv::OpAccessChain:
                case spv::OpInBoundsAccessChain:
                case spv::OpCopyObject:
                    if (!shadow_type(instruction.type)) {
                        error = "unsupported query pointer type";
                        return false;
                    }
                    flags.emplace(instruction.result, id());
                    break;
                default:
                    error = "unsupported query pointer producer";
                    return false;
            }
        }
        for (const auto& instruction : instructions)
            if (std::string(spv::OpToString(instruction.op)).find("OpRayQuery") == 0) {
                const size_t operand = instruction.result ? 3 : 1;
                if (operand >= instruction.words.size() || !flags.count(instruction.words[operand])) {
                    error = "ray query operation has unsupported pointer provenance";
                    return false;
                }
            }
        if (expose) {
            mask = constant(~(1u | 2u | 64u | 128u | 1024u));
            no_opaque = constant(2);
        }
        if (consumer && !flags.empty() && !prepare_consumer()) return false;
        return true;
    }
    bool prepare_consumer() {
        bool ext_declaration = false, native_trace = false;
        for (const auto& instruction : instructions) {
            ext_declaration |=
                instruction.op == spv::OpCapability && instruction.words[1] == spv::CapabilityRayTracingOpacityMicromapEXT;
            if (instruction.op == spv::OpExtension &&
                std::string(reinterpret_cast<const char*>(instruction.words.data() + 1)) == "SPV_KHR_opacity_micromap") {
                error = "KHR opacity micromap declarations require their separate consumer contract";
                return false;
            }
            const std::string name = spv::OpToString(instruction.op);
            native_trace |= name.find("OpTraceRay") == 0 || name.find("OpHitObjectTrace") == 0;
            if ((instruction.op == spv::OpExecutionMode || instruction.op == spv::OpExecutionModeId) &&
                instruction.words[2] == spv::ExecutionModeOpacityMicromapIdKHR) {
                error = "KHR opacity micromap execution mode requires its separate consumer contract";
                return false;
            }
        }
        if (ext_declaration && native_trace) {
            error = "mixed native pipeline micromap consumers require RTP flag translation before removing EXT declarations";
            return false;
        }
        const auto callback = functions.find(consumer->classify_function);
        if (callback == functions.end()) {
            error = "classification function is missing";
            return false;
        }
        const auto& signature = definitions.at(callback->second.type).words;
        const std::vector<uint32_t> expected{uint_type, metadata_type, uint_type, uint_type,
                                             uint_type, bary_type,     bool_type, uint_type};
        if (std::vector<uint32_t>(signature.begin() + 2, signature.end()) != expected) {
            error = "classification function requires scalar uint(uvec2,uint,uint,uint,vec2,bool,uint) ABI";
            return false;
        }
        std::unordered_map<uint32_t, std::vector<const Instruction*>> bodies;
        uint32_t current = 0, block = 0;
        std::unordered_set<uint32_t> proceeding_blocks, initialized_offsets;
        for (const auto& instruction : instructions) {
            if (instruction.op == spv::OpFunction) current = instruction.result;
            if (current) bodies[current].push_back(&instruction);
            if (instruction.op == spv::OpLabel) block = instruction.result;
            if (instruction.op == spv::OpLoopMerge) loop_merges[block] = instruction.words;
            if (instruction.op == spv::OpRayQueryProceedKHR) proceeding_blocks.insert(block);
            if (instruction.op == spv::OpRayQueryInitializeKHR) initialized_offsets.insert(instruction.offset);
            if (instruction.op == spv::OpFunctionEnd) current = 0;
        }
        for (auto it = loop_merges.begin(); it != loop_merges.end();) {
            if (!proceeding_blocks.count(it->first))
                it = loop_merges.erase(it);
            else
                ++it;
        }
        std::vector<uint32_t> pending{consumer->classify_function};
        std::unordered_set<uint32_t> visited;
        while (!pending.empty()) {
            const uint32_t function = pending.back();
            pending.pop_back();
            if (!visited.insert(function).second) continue;
            for (const auto* instruction : bodies[function]) {
                if (std::string(spv::OpToString(instruction->op)).find("OpRayQuery") == 0) {
                    error = "classification callback must not execute ray queries";
                    return false;
                }
                if (instruction->op == spv::OpFunctionCall) pending.push_back(instruction->words[3]);
            }
        }
        for (const auto& binding : consumer->initialization_metadata) {
            const auto definition = definitions.find(binding.metadata_value);
            if (!initialized_offsets.count(binding.initialize_word_offset) || definition == definitions.end() ||
                definition->second.type != metadata_type ||
                !metadata.emplace(binding.initialize_word_offset, binding.metadata_value).second) {
                error = "initialization metadata requires a unique source Initialize offset and existing uvec2 SSA value";
                return false;
            }
        }
        for (const auto offset : initialized_offsets)
            if (!metadata.count(offset) && !consumer->use_constant_metadata) {
                error = "initialization metadata provenance is missing";
                return false;
            }
        zero = constant(0);
        one = constant(1);
        two = constant(2);
        false_value = id();
        true_value = id();
        declarations.push_back(make(spv::OpConstantFalse, {bool_type, false_value}));
        declarations.push_back(make(spv::OpConstantTrue, {bool_type, true_value}));
        const uint32_t float_zero = id();
        declarations.push_back(make(spv::OpConstant, {float_type, float_zero, 0}));
        zero_bary = id();
        declarations.push_back(make(spv::OpConstantComposite, {bary_type, zero_bary, float_zero, float_zero}));
        if (consumer->use_constant_metadata) {
            const uint32_t low = constant(consumer->constant_metadata_low), high = constant(consumer->constant_metadata_high);
            default_metadata = id();
            declarations.push_back(make(spv::OpConstantComposite, {metadata_type, default_metadata, low, high}));
        }
        return true;
    }
    uint32_t state_member(uint32_t query, uint32_t member) {
        const uint32_t native_pointer_type = definitions.at(query).type;
        const uint32_t storage = definitions.at(native_pointer_type).words[2];
        const auto pointer_type = interned.at({uint32_t(spv::OpTypePointer), storage, uint_type});
        const uint32_t pointer = id();
        emit(module.words, make(spv::OpAccessChain, {pointer_type, pointer, shadow(query), member}));
        return pointer;
    }
    void proceed(const Instruction& instruction, uint32_t original_block) {
        const uint32_t query = instruction.words[3];
        const uint32_t header = id(), condition = id(), body = id(), triangle_block = id(), bary_merge = id();
        const uint32_t after_ignore = id(), confirm = id(), visible = id(), continuation = id(), merge = id();
        if (loop_merges.count(original_block) && hoisted_merges.insert(original_block).second)
            emit(module.words, loop_merges.at(original_block));
        emit(module.words, make(spv::OpBranch, {header}));
        emit(module.words, make(spv::OpLabel, {header}));
        emit(module.words, make(spv::OpLoopMerge, {merge, continuation, spv::LoopControlMaskNone}));
        emit(module.words, make(spv::OpBranch, {condition}));
        emit(module.words, make(spv::OpLabel, {condition}));
        const uint32_t native_result = id();
        emit(module.words, make(spv::OpRayQueryProceedKHR, {bool_type, native_result, query}));
        emit(module.words, make(spv::OpBranchConditional, {native_result, body, merge}));
        emit(module.words, make(spv::OpLabel, {body}));
        const uint32_t kind = id(), triangle = id(), instance = id(), geometry = id(), primitive = id();
        emit(module.words, make(spv::OpRayQueryGetIntersectionTypeKHR, {uint_type, kind, query, zero}));
        emit(module.words, make(spv::OpIEqual, {bool_type, triangle, kind, zero}));
        emit(module.words, make(spv::OpRayQueryGetIntersectionInstanceIdKHR, {uint_type, instance, query, zero}));
        emit(module.words, make(spv::OpRayQueryGetIntersectionGeometryIndexKHR, {uint_type, geometry, query, zero}));
        emit(module.words, make(spv::OpRayQueryGetIntersectionPrimitiveIndexKHR, {uint_type, primitive, query, zero}));
        emit(module.words, make(spv::OpSelectionMerge, {bary_merge, spv::SelectionControlMaskNone}));
        emit(module.words, make(spv::OpBranchConditional, {triangle, triangle_block, bary_merge}));
        emit(module.words, make(spv::OpLabel, {triangle_block}));
        const uint32_t barycentrics = id();
        emit(module.words, make(spv::OpRayQueryGetIntersectionBarycentricsKHR, {bary_type, barycentrics, query, zero}));
        emit(module.words, make(spv::OpBranch, {bary_merge}));
        emit(module.words, make(spv::OpLabel, {bary_merge}));
        const uint32_t bary = id(), loaded_state = id(), logical_flags = id(), scene = id(), opacity = id(), ignored = id();
        emit(module.words, make(spv::OpPhi, {bary_type, bary, zero_bary, body, barycentrics, triangle_block}));
        emit(module.words, make(spv::OpLoad, {state_type, loaded_state, shadow(query)}));
        emit(module.words, make(spv::OpCompositeExtract, {uint_type, logical_flags, loaded_state, 0}));
        emit(module.words, make(spv::OpCompositeExtract, {metadata_type, scene, loaded_state, 2}));
        emit(module.words, make(spv::OpFunctionCall, {uint_type, opacity, consumer->classify_function, scene, instance, geometry,
                                                      primitive, bary, triangle, logical_flags}));
        emit(module.words, make(spv::OpIEqual, {bool_type, ignored, opacity, zero}));
        emit(module.words, make(spv::OpSelectionMerge, {after_ignore, spv::SelectionControlMaskNone}));
        emit(module.words, make(spv::OpBranchConditional, {ignored, continuation, after_ignore}));
        emit(module.words, make(spv::OpLabel, {after_ignore}));
        const uint32_t opaque = id(), opaque_triangle = id();
        emit(module.words, make(spv::OpIEqual, {bool_type, opaque, opacity, one}));
        emit(module.words, make(spv::OpLogicalAnd, {bool_type, opaque_triangle, triangle, opaque}));
        emit(module.words, make(spv::OpSelectionMerge, {visible, spv::SelectionControlMaskNone}));
        emit(module.words, make(spv::OpBranchConditional, {opaque_triangle, confirm, visible}));
        emit(module.words, make(spv::OpLabel, {confirm}));
        emit(module.words, make(spv::OpRayQueryConfirmIntersectionKHR, {query}));
        emit(module.words, make(spv::OpBranch, {continuation}));
        emit(module.words, make(spv::OpLabel, {visible}));
        const uint32_t candidate_opaque = id();
        emit(module.words, make(spv::OpSelect, {uint_type, candidate_opaque, opaque, one, zero}));
        emit(module.words, make(spv::OpStore, {state_member(query, one), candidate_opaque}));
        emit(module.words, make(spv::OpBranch, {merge}));
        emit(module.words, make(spv::OpLabel, {continuation}));
        emit(module.words, make(spv::OpBranch, {header}));
        emit(module.words, make(spv::OpLabel, {merge}));
        emit(module.words, make(spv::OpPhi, {bool_type, instruction.result, false_value, condition, true_value, visible}));
        final_blocks[original_block] = merge;
        ++module.proceeds;
    }
    uint32_t shadow(uint32_t query) {
        auto found = flags.find(query);
        if (found == flags.end()) {
            error = "query pointer provenance is unavailable";
            return 0;
        }
        return found->second;
    }
    bool transform() {
        module.words.assign(source.begin(), source.begin() + 5);
        bool in_function = false, declarations_written = false;
        uint32_t current = 0, original_block = 0;
        std::vector<uint32_t> pending_parameters;
        // Private state declarations must follow all newly synthesized types.
        for (const auto& instruction : instructions)
            if (instruction.op == spv::OpVariable && flags.count(instruction.result) &&
                instruction.words[3] == spv::StorageClassPrivate)
                globals.push_back(
                    make(spv::OpVariable, {types.at(instruction.type), shadow(instruction.result), spv::StorageClassPrivate}));
        for (const auto& instruction : instructions) {
            auto words = instruction.words;
            if (consumer && instruction.op == spv::OpCapability &&
                instruction.words[1] == spv::CapabilityRayTracingOpacityMicromapEXT)
                continue;
            if (consumer && instruction.op == spv::OpExtension &&
                std::string(reinterpret_cast<const char*>(instruction.words.data() + 1)) == "SPV_EXT_opacity_micromap")
                continue;
            if (instruction.op == spv::OpLabel) original_block = instruction.result;
            if (instruction.op == spv::OpFunction) {
                if (!declarations_written) {
                    for (const auto& declaration : declarations) emit(module.words, declaration);
                    for (const auto& global : globals) emit(module.words, global);
                    declarations_written = true;
                }
                current = instruction.result;
                words[4] = functions.at(current).type;
                in_function = true;
            }
            if (instruction.op == spv::OpEntryPoint && source[1] >= 0x10400) {
                for (const auto& global : globals) words.push_back(global[2]);
                words[0] = (uint32_t(words.size()) << 16) | uint32_t(instruction.op);
            }
            if (instruction.op == spv::OpFunctionParameter && flags.count(instruction.result)) {
                pending_parameters.push_back(instruction.result);
            } else if (instruction.op == spv::OpLabel && !pending_parameters.empty()) {
                for (auto parameter : pending_parameters) {
                    const auto& definition = definitions.at(parameter);
                    emit(module.words, make(spv::OpFunctionParameter, {types.at(definition.type), shadow(parameter)}));
                }
                pending_parameters.clear();
            }
            if (instruction.op == spv::OpFunctionCall) {
                const auto& function = functions.at(words[3]);
                for (auto index : function.query_parameters) {
                    const auto value = shadow(words[4 + index]);
                    if (!value) return false;
                    words.push_back(value);
                }
                words[0] = (uint32_t(words.size()) << 16) | uint32_t(instruction.op);
            }
            if (instruction.op == spv::OpRayQueryInitializeKHR) {
                const auto state = shadow(words[1]);
                if (!state) return false;
                auto logical_flags = words[3];
                if (definitions.at(logical_flags).type != uint_type) {
                    const auto cast = id();
                    emit(module.words, make(spv::OpBitcast, {uint_type, cast, logical_flags}));
                    logical_flags = cast;
                }
                if (consumer) {
                    const auto context = metadata.count(instruction.offset) ? metadata.at(instruction.offset) : default_metadata;
                    const auto initialized = id();
                    emit(module.words, make(spv::OpCompositeConstruct, {state_type, initialized, logical_flags, zero, context}));
                    emit(module.words, make(spv::OpStore, {state, initialized}));
                } else {
                    emit(module.words, make(spv::OpStore, {state, logical_flags}));
                }
                if (expose) {
                    const auto masked = id(), forced = id();
                    emit(module.words, make(spv::OpBitwiseAnd, {uint_type, masked, logical_flags, mask}));
                    emit(module.words, make(spv::OpBitwiseOr, {uint_type, forced, masked, no_opaque}));
                    words[3] = forced;
                }
                ++module.initializations;
            } else if (instruction.op == spv::OpRayQueryGetRayFlagsKHR) {
                const auto state = consumer ? state_member(words[3], zero) : shadow(words[3]);
                if (!state) return false;
                if (instruction.type == uint_type) {
                    words = make(spv::OpLoad, {instruction.type, instruction.result, state});
                } else {
                    const auto loaded = id();
                    emit(module.words, make(spv::OpLoad, {uint_type, loaded, state}));
                    words = make(spv::OpBitcast, {instruction.type, instruction.result, loaded});
                }
                ++module.flag_getters;
            } else if (consumer && instruction.op == spv::OpRayQueryGetIntersectionCandidateAABBOpaqueKHR) {
                const auto state = state_member(words[3], one), loaded = id();
                emit(module.words, make(spv::OpLoad, {uint_type, loaded, state}));
                words = make(spv::OpINotEqual, {instruction.type, instruction.result, loaded, zero});
                ++module.aabb_getters;
            } else if (consumer && instruction.op == spv::OpRayQueryProceedKHR) {
                proceed(instruction, original_block);
                continue;
            } else if (consumer && instruction.op == spv::OpLoopMerge && hoisted_merges.count(original_block)) {
                continue;
            }
            emit(module.words, words);
            if (flags.count(instruction.result) && instruction.op != spv::OpFunctionParameter) {
                auto parallel = instruction.words;
                parallel[1] = types.at(instruction.type);
                parallel[2] = shadow(instruction.result);
                switch (instruction.op) {
                    case spv::OpVariable:
                        if (!in_function) continue;
                        // Opaque query initialization is not a scalar flag initializer.
                        if (parallel.size() != 4) {
                            error = "query variable initializer is unsupported";
                            return false;
                        }
                        break;
                    case spv::OpAccessChain:
                    case spv::OpInBoundsAccessChain:
                    case spv::OpCopyObject:
                        parallel[3] = shadow(parallel[3]);
                        break;
                    default:
                        error = "unsupported query pointer instruction";
                        return false;
                }
                if (!error.empty()) return false;
                emit(module.words, parallel);
            }
            if (instruction.op == spv::OpFunctionEnd) in_function = false;
        }
        if (consumer) {
            for (size_t offset = 5; offset < module.words.size();) {
                const uint32_t count = module.words[offset] >> 16;
                if (spv::Op(module.words[offset] & 0xffffu) == spv::OpPhi)
                    for (size_t parent = offset + 4; parent < offset + count; parent += 2) {
                        const auto replacement = final_blocks.find(module.words[parent]);
                        if (replacement != final_blocks.end()) module.words[parent] = replacement->second;
                    }
                offset += count;
            }
        }
        module.words[3] = bound;
        for (const auto& pair : flags)
            module.state.push_back(consumer ? QueryStateBinding{pair.first, 0, pair.second}
                                            : QueryStateBinding{pair.first, pair.second});
        std::sort(module.state.begin(), module.state.end(),
                  [](const auto& a, const auto& b) { return a.query_pointer < b.query_pointer; });
        return true;
    }
};
}  // namespace

static QueryRewriteStatus rewrite_module(const std::vector<uint32_t>& source, bool expose_candidates,
                                         const QueryConsumerOptions* consumer, QueryStateModule* output, std::string* diagnostic) {
    if (!output) return QueryRewriteStatus::InvalidModule;
    try {
        spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_2);
        std::string message;
        validator.SetMessageConsumer(
            [&](spv_message_level_t, const char*, const spv_position_t&, const char* text) { message = text; });
        if (!validator.Validate(source)) {
            if (diagnostic) *diagnostic = message;
            return QueryRewriteStatus::InvalidModule;
        }
        Rewrite rewrite(source, expose_candidates, consumer);
        if (!rewrite.parse()) {
            if (diagnostic) *diagnostic = rewrite.error;
            return QueryRewriteStatus::Unsupported;
        }
        if (rewrite.flags.empty()) {
            QueryStateModule unchanged;
            unchanged.words = source;
            *output = std::move(unchanged);
            if (diagnostic) diagnostic->clear();
            return QueryRewriteStatus::Success;
        }
        if (!rewrite.transform()) {
            if (diagnostic) *diagnostic = rewrite.error;
            return QueryRewriteStatus::Unsupported;
        }
        if (!validator.Validate(rewrite.module.words)) {
            if (diagnostic) *diagnostic = message;
            return QueryRewriteStatus::Unsupported;
        }
        *output = std::move(rewrite.module);
        if (diagnostic) diagnostic->clear();
        return QueryRewriteStatus::Success;
    } catch (const std::bad_alloc&) {
        return QueryRewriteStatus::OutOfMemory;
    } catch (const std::length_error&) {
        return QueryRewriteStatus::Unsupported;
    }
}
QueryRewriteStatus prepare_query_state(const std::vector<uint32_t>& source, bool expose_candidates, QueryStateModule* output,
                                       std::string* diagnostic) {
    return rewrite_module(source, expose_candidates, nullptr, output, diagnostic);
}
QueryRewriteStatus rewrite_query_consumers(const std::vector<uint32_t>& source, const QueryConsumerOptions& options,
                                           QueryStateModule* output, std::string* diagnostic) {
    return rewrite_module(source, true, &options, output, diagnostic);
}
}  // namespace vel::omm
