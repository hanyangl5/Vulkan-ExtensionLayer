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
};
struct Function {
    uint32_t type = 0;
    std::vector<uint32_t> parameters;
    std::vector<size_t> query_parameters;
};
struct Rewrite {
    const std::vector<uint32_t>& source;
    bool expose;
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
    QueryStateModule module;
    std::string error;

    Rewrite(const std::vector<uint32_t>& input, bool prepare) : source(input), expose(prepare), bound(input[3]) {}
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
                result = uint_type;
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
        if (expose) {
            mask = constant(~(1u | 2u | 64u | 128u | 1024u));
            no_opaque = constant(2);
        }
        return true;
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
        uint32_t current = 0;
        std::vector<uint32_t> pending_parameters;
        // Private state declarations must follow all newly synthesized types.
        for (const auto& instruction : instructions)
            if (instruction.op == spv::OpVariable && flags.count(instruction.result) &&
                instruction.words[3] == spv::StorageClassPrivate)
                globals.push_back(
                    make(spv::OpVariable, {types.at(instruction.type), shadow(instruction.result), spv::StorageClassPrivate}));
        for (const auto& instruction : instructions) {
            auto words = instruction.words;
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
                emit(module.words, make(spv::OpStore, {state, logical_flags}));
                if (expose) {
                    const auto masked = id(), forced = id();
                    emit(module.words, make(spv::OpBitwiseAnd, {uint_type, masked, logical_flags, mask}));
                    emit(module.words, make(spv::OpBitwiseOr, {uint_type, forced, masked, no_opaque}));
                    words[3] = forced;
                }
                ++module.initializations;
            } else if (instruction.op == spv::OpRayQueryGetRayFlagsKHR) {
                const auto state = shadow(words[3]);
                if (!state) return false;
                if (instruction.type == uint_type) {
                    words = make(spv::OpLoad, {instruction.type, instruction.result, state});
                } else {
                    const auto loaded = id();
                    emit(module.words, make(spv::OpLoad, {uint_type, loaded, state}));
                    words = make(spv::OpBitcast, {instruction.type, instruction.result, loaded});
                }
                ++module.flag_getters;
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
        module.words[3] = bound;
        for (const auto& pair : flags) module.state.push_back({pair.first, pair.second});
        std::sort(module.state.begin(), module.state.end(),
                  [](const auto& a, const auto& b) { return a.query_pointer < b.query_pointer; });
        return true;
    }
};
}  // namespace

QueryRewriteStatus prepare_query_state(const std::vector<uint32_t>& source, bool expose_candidates, QueryStateModule* output,
                                       std::string* diagnostic) {
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
        Rewrite rewrite(source, expose_candidates);
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
}  // namespace vel::omm
