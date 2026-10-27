// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_as_provenance.h"

#include <spirv-tools/libspirv.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>
#include <map>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace vel::omm {
namespace {
struct Instruction {
    spv::Op op;
    std::vector<uint32_t> words, uses;
    uint32_t type = 0, result = 0, function = 0, block = 0;
};
struct Path {
    uint32_t set = 0, binding = 0, index = 0;
    bool aggregate = false;
};
struct Function {
    uint32_t type = 0, return_type = 0, return_metadata = 0;
    std::vector<uint32_t> parameters, locals;
    bool modified = false;
};
struct Pass {
    const std::vector<uint32_t>& source;
    const AsProvenanceOptions& options;
    std::vector<Instruction> instructions;
    std::unordered_map<uint32_t, size_t> definitions;
    std::unordered_map<uint32_t, Function> functions;
    std::unordered_map<uint32_t, Path> paths;
    std::unordered_map<uint32_t, uint32_t> metadata, return_slots;
    mutable std::unordered_map<uint32_t, bool> as_type_cache;
    std::unordered_map<uint32_t, std::map<uint32_t, uint32_t>> decorations;
    std::map<std::vector<uint32_t>, uint32_t> interned;
    std::map<uint32_t, uint32_t> constants;
    std::vector<std::vector<uint32_t>> declarations;
    AsProvenanceModule output;
    std::string error;
    uint32_t bound, uint_type = 0, pair_type = 0, fetch_type = 0, wide_result_type = 0, out_pointer_type = 0;
    uint32_t zero = 0, zero_pair = 0;
    bool needs_integer = false, needs_descriptor = false;

    Pass(const std::vector<uint32_t>& words, const AsProvenanceOptions& value)
        : source(words), options(value), bound(words.at(3)) {}
    uint32_t id() {
        if (bound >= 0x3fffff) throw std::length_error("SPIR-V ID limit");
        return bound++;
    }
    static std::vector<uint32_t> make(spv::Op op, const std::vector<uint32_t>& operands) {
        std::vector<uint32_t> result{(uint32_t(operands.size() + 1) << 16) | uint32_t(op)};
        result.insert(result.end(), operands.begin(), operands.end());
        return result;
    }
    static void emit(std::vector<uint32_t>& destination, const std::vector<uint32_t>& words) {
        destination.insert(destination.end(), words.begin(), words.end());
    }
    void emit(spv::Op op, const std::vector<uint32_t>& operands) { emit(output.words, make(op, operands)); }
    const Instruction& get(uint32_t value) const { return instructions.at(definitions.at(value)); }
    uint32_t declaration(spv::Op op, const std::vector<uint32_t>& operands) {
        std::vector<uint32_t> key{uint32_t(op)};
        key.insert(key.end(), operands.begin(), operands.end());
        const auto found = interned.find(key);
        if (found != interned.end()) return found->second;
        const uint32_t result = id();
        std::vector<uint32_t> value{result};
        value.insert(value.end(), operands.begin(), operands.end());
        declarations.push_back(make(op, value));
        interned.emplace(std::move(key), result);
        return result;
    }
    uint32_t constant(uint32_t value) {
        const auto found = constants.find(value);
        if (found != constants.end()) return found->second;
        const uint32_t result = id();
        declarations.push_back(make(spv::OpConstant, {uint_type, result, value}));
        constants.emplace(value, result);
        return result;
    }
    uint32_t pair_constant(uint32_t low, uint32_t high) {
        const auto l = constant(low), h = constant(high), result = id();
        declarations.push_back(make(spv::OpConstantComposite, {pair_type, result, l, h}));
        return result;
    }
    bool scalar_as(uint32_t type) const { return get(type).op == spv::OpTypeAccelerationStructureKHR; }
    bool contains_as(uint32_t type) const {
        const auto cached = as_type_cache.find(type);
        if (cached != as_type_cache.end()) return cached->second;
        std::vector<uint32_t> pending{type};
        std::unordered_set<uint32_t> visited;
        while (!pending.empty()) {
            const uint32_t current = pending.back();
            pending.pop_back();
            if (!visited.insert(current).second) continue;
            const auto& t = get(current);
            if (t.op == spv::OpTypeAccelerationStructureKHR) {
                as_type_cache.emplace(type, true);
                return true;
            }
            if (t.op == spv::OpTypeArray || t.op == spv::OpTypeRuntimeArray) pending.push_back(t.words.at(2));
            if (t.op == spv::OpTypePointer) pending.push_back(t.words.at(3));
            if (t.op == spv::OpTypeStruct) pending.insert(pending.end(), t.words.begin() + 2, t.words.end());
        }
        as_type_cache.emplace(type, false);
        return false;
    }
    bool descriptor_pointer(uint32_t type) const {
        const auto& p = get(type);
        if (p.op != spv::OpTypePointer || p.words.at(2) != spv::StorageClassUniformConstant) return false;
        uint32_t pointee = p.words.at(3);
        while (get(pointee).op == spv::OpTypeArray || get(pointee).op == spv::OpTypeRuntimeArray)
            pointee = get(pointee).words.at(2);
        return scalar_as(pointee);
    }
    bool uint_scalar(uint32_t type) const {
        const auto& t = get(type);
        return t.op == spv::OpTypeInt && t.words.size() == 4 && t.words[2] == 32 && t.words[3] == 0;
    }
    bool uint_vector(uint32_t type, uint32_t count) const {
        const auto& t = get(type);
        return t.op == spv::OpTypeVector && t.words.size() == 4 && t.words[3] == count && uint_scalar(t.words[2]);
    }
    bool fail(const char* message) {
        error = message;
        return false;
    }
    bool parse() {
        spvtools::Context context(SPV_ENV_VULKAN_1_2);
        const auto parsed = spvBinaryParse(
            context.CContext(), this, source.data(), source.size(), nullptr,
            [](void* user, const spv_parsed_instruction_t* instruction) -> spv_result_t {
                auto& pass = *static_cast<Pass*>(user);
                Instruction item{spv::Op(instruction->opcode),
                                 {instruction->words, instruction->words + instruction->num_words},
                                 {},
                                 instruction->type_id,
                                 instruction->result_id};
                for (uint16_t i = 0; i < instruction->num_operands; ++i)
                    if (instruction->operands[i].type == SPV_OPERAND_TYPE_ID)
                        item.uses.push_back(instruction->words[instruction->operands[i].offset]);
                if (item.result) pass.definitions.emplace(item.result, pass.instructions.size());
                pass.instructions.push_back(std::move(item));
                return SPV_SUCCESS;
            },
            nullptr);
        if (parsed != SPV_SUCCESS) return fail("Unable to parse source SPIR-V");
        uint32_t current = 0, block = 0;
        for (auto& item : instructions) {
            if (item.op == spv::OpFunction) {
                current = item.result;
                block = 0;
                auto& function = functions[current];
                function.type = item.words.at(4);
                function.return_type = item.type;
            }
            if (item.op == spv::OpLabel) block = item.result;
            item.function = current;
            item.block = block;
            if (item.op == spv::OpFunctionParameter) functions.at(current).parameters.push_back(item.result);
            if (item.op == spv::OpFunctionEnd) current = block = 0;
            if (item.op == spv::OpDecorate && item.words.size() == 4 &&
                (item.words[2] == spv::DecorationDescriptorSet || item.words[2] == spv::DecorationBinding))
                decorations[item.words[1]][item.words[2]] = item.words[3];
            if (item.result && !item.type && item.op >= spv::OpTypeVoid && item.op <= spv::OpTypeForwardPointer) {
                std::vector<uint32_t> key{uint32_t(item.op)};
                key.insert(key.end(), item.words.begin() + 2, item.words.end());
                interned.emplace(std::move(key), item.result);
            }
            if (item.op == spv::OpLoad && scalar_as(item.type)) needs_descriptor = true;
            if (item.op == spv::OpConvertUToAccelerationStructureKHR) needs_integer = true;
        }
        for (const auto& item : instructions)
            if (item.op == spv::OpGroupDecorate) {
                const auto group = decorations.find(item.words.at(1));
                if (group != decorations.end())
                    for (size_t i = 2; i < item.words.size(); ++i)
                        for (const auto& decoration : group->second) decorations[item.words[i]].insert(decoration);
            }
        return true;
    }
    bool provider(uint32_t function, bool descriptor) {
        if (!function || !definitions.count(function) || get(function).op != spv::OpFunction)
            return fail(descriptor ? "Descriptor AS load requires a descriptor fetch provider"
                                   : "Integer AS conversion requires an integer resolver provider");
        const auto& signature = get(get(function).words.at(4));
        if (signature.op != spv::OpTypeFunction) return fail("AS provider has no function type");
        const auto& s = signature.words;
        if (descriptor) {
            if (s.size() != 6 || !uint_vector(s[2], 4) || !uint_scalar(s[3]) || !uint_scalar(s[4]) || !uint_vector(s[5], 2))
                return fail("Descriptor provider must be scalar uvec4 fetch(uint,uint,uvec2)");
            uint_type = s[3];
            pair_type = s[5];
            fetch_type = s[2];
        } else {
            if (s.size() != 4 || !uint_vector(s[2], 2) || s[2] != s[3])
                return fail("Integer provider must be scalar uvec2 resolve(uvec2)");
            if (pair_type && pair_type != s[2]) return fail("AS providers must share their uvec2 type");
            pair_type = s[2];
            if (!uint_type) uint_type = get(pair_type).words.at(2);
        }
        return true;
    }
    bool prepare() {
        if (needs_descriptor && !provider(options.descriptor_fetch_function, true)) return false;
        if (needs_integer && !provider(options.integer_resolver_function, false)) return false;
        if (!uint_type) uint_type = declaration(spv::OpTypeInt, {32, 0});
        if (!pair_type) pair_type = declaration(spv::OpTypeVector, {uint_type, 2});
        zero = constant(0);
        zero_pair = pair_constant(0, 0);
        wide_result_type = declaration(spv::OpTypeStruct, {uint_type, uint_type});
        out_pointer_type = declaration(spv::OpTypePointer, {spv::StorageClassFunction, pair_type});
        for (const auto& item : instructions) {
            if (!item.type || !contains_as(item.type)) continue;
            if (item.op == spv::OpFunction) continue;
            if (item.function) functions.at(item.function).modified = true;
            if (scalar_as(item.type)) {
                switch (item.op) {
                    case spv::OpLoad:
                        if (item.words.size() != 4 && !(item.words.size() == 5 && item.words[4] == 0))
                            return fail("AS descriptor load memory operands are unsupported");
                        if (!descriptor_pointer(get(item.words[3]).type))
                            return fail("AS load must originate in a UniformConstant descriptor");
                        break;
                    case spv::OpConvertUToAccelerationStructureKHR:
                    case spv::OpCopyObject:
                    case spv::OpPhi:
                    case spv::OpFunctionParameter:
                    case spv::OpFunctionCall:
                        break;
                    default:
                        return fail("Unknown or composite acceleration-structure producer");
                }
                metadata.emplace(item.result, id());
            } else if (descriptor_pointer(item.type)) {
                Path path;
                path.aggregate = !scalar_as(get(item.type).words.at(3));
                if (item.op == spv::OpVariable) {
                    if (item.function || item.words.size() != 4) return fail("Invalid descriptor AS variable");
                    const auto found = decorations.find(item.result);
                    if (found == decorations.end() || !found->second.count(spv::DecorationDescriptorSet) ||
                        !found->second.count(spv::DecorationBinding))
                        return fail("AS descriptor variable needs DescriptorSet and Binding decorations");
                    path.set = constant(found->second.at(spv::DecorationDescriptorSet));
                    path.binding = constant(found->second.at(spv::DecorationBinding));
                    path.index = zero_pair;
                } else if (item.op == spv::OpFunctionParameter || item.op == spv::OpAccessChain ||
                           item.op == spv::OpInBoundsAccessChain || item.op == spv::OpCopyObject) {
                    path.set = id();
                    path.binding = id();
                    path.index = id();
                } else {
                    return fail("Unknown descriptor AS pointer producer");
                }
                paths.emplace(item.result, path);
            } else {
                return fail("Composite acceleration-structure values or unsupported AS storage");
            }
        }
        // Descriptor array loads have stricter source-language use rules than
        // singleton AS values. Follow array origin through pointer parameters.
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& item : instructions) {
                auto result = paths.find(item.result);
                if (result != paths.end() && !result->second.aggregate &&
                    (item.op == spv::OpAccessChain || item.op == spv::OpInBoundsAccessChain || item.op == spv::OpCopyObject) &&
                    paths.at(item.words.at(3)).aggregate)
                    result->second.aggregate = changed = true;
                if (item.op != spv::OpFunctionCall) continue;
                const auto& parameters = functions.at(item.words.at(3)).parameters;
                for (size_t i = 0; i < parameters.size(); ++i) {
                    auto parameter = paths.find(parameters[i]);
                    if (parameter != paths.end() && !parameter->second.aggregate && paths.at(item.words.at(4 + i)).aggregate)
                        parameter->second.aggregate = changed = true;
                }
            }
        }
        for (const auto& item : instructions) {
            if (!item.function) continue;
            for (const uint32_t use : item.uses) {
                const auto value = metadata.find(use);
                if (value == metadata.end()) continue;
                const bool ray =
                    item.op == spv::OpRayQueryInitializeKHR || item.op == spv::OpTraceRayKHR || item.op == spv::OpTraceRayMotionNV;
                const auto& producer = get(use);
                if (producer.op == spv::OpLoad && paths.at(producer.words.at(3)).aggregate &&
                    (!ray || item.block != producer.block))
                    return fail("Composite-derived AS values require same-block ray-instruction uses");
                if (!ray && item.op != spv::OpCopyObject && item.op != spv::OpPhi && item.op != spv::OpFunctionCall &&
                    item.op != spv::OpReturnValue)
                    return fail("Unsupported acceleration-structure consumer");
            }
        }
        for (auto& entry : functions) {
            auto& function = entry.second;
            if (contains_as(function.return_type) && !scalar_as(function.return_type))
                return fail("Composite AS and descriptor pointer returns are unsupported");
            const auto& signature = get(function.type).words;
            std::vector<uint32_t> expanded(signature.begin() + 2, signature.end());
            bool extended = false;
            for (const uint32_t parameter : function.parameters) {
                if (metadata.count(parameter)) {
                    expanded.push_back(pair_type);
                    extended = true;
                } else if (paths.count(parameter)) {
                    expanded.insert(expanded.end(), {uint_type, uint_type, pair_type});
                    extended = true;
                }
            }
            if (scalar_as(function.return_type)) {
                function.return_metadata = id();
                expanded.push_back(out_pointer_type);
                extended = true;
            }
            if (extended) function.type = declaration(spv::OpTypeFunction, expanded);
        }
        for (const auto& item : instructions)
            if (item.op == spv::OpFunctionCall && metadata.count(item.result)) {
                const auto slot = id();
                return_slots.emplace(item.result, slot);
                functions.at(item.function).locals.push_back(slot);
            }
        return true;
    }
    // All flattened descriptor arithmetic is unsigned 64-bit, represented as
    // uvec2. This does not impose Int64 support on descriptor-only applications.
    uint32_t integer_pair(uint32_t value) {
        const auto type = get(value).type;
        if (type == pair_type) return value;
        const auto& t = get(type);
        if (t.op == spv::OpTypeVector && t.words.at(3) == 2 && get(t.words.at(2)).op == spv::OpTypeInt &&
            get(t.words.at(2)).words.at(2) == 32) {
            const auto result = id();
            emit(spv::OpBitcast, {pair_type, result, value});
            return result;
        }
        if (t.op != spv::OpTypeInt || t.words.at(2) > 64 || (t.words[2] > 32 && t.words[2] != 64)) {
            fail("Descriptor index/address must be a supported integer or a two-component 32-bit integer address");
            return 0;
        }
        if (t.words[2] == 64) {
            const auto result = id();
            emit(spv::OpBitcast, {pair_type, result, value});
            return result;
        }
        uint32_t low = value;
        if (t.words[2] < 32) {
            low = id();
            emit(t.words[3] ? spv::OpSConvert : spv::OpUConvert, {uint_type, low, value});
        } else if (type != uint_type) {
            low = id();
            emit(spv::OpBitcast, {uint_type, low, value});
        }
        uint32_t high = zero;
        if (t.words[3]) {
            high = id();
            emit(spv::OpShiftRightArithmetic, {uint_type, high, low, constant(31)});
        }
        const auto result = id();
        emit(spv::OpCompositeConstruct, {pair_type, result, low, high});
        return result;
    }
    uint32_t extract(uint32_t pair, uint32_t component, uint32_t type = 0) {
        const auto result = id();
        emit(spv::OpCompositeExtract, {type ? type : uint_type, result, pair, component});
        return result;
    }
    uint32_t multiply(uint32_t a, uint32_t b) {
        const auto al = extract(a, 0), ah = extract(a, 1), bl = extract(b, 0), bh = extract(b, 1);
        const auto product = id();
        emit(spv::OpUMulExtended, {wide_result_type, product, al, bl});
        const auto low = extract(product, 0), carry = extract(product, 1), cross0 = id(), cross1 = id(), sum = id(), high = id();
        emit(spv::OpIMul, {uint_type, cross0, al, bh});
        emit(spv::OpIMul, {uint_type, cross1, ah, bl});
        emit(spv::OpIAdd, {uint_type, sum, carry, cross0});
        emit(spv::OpIAdd, {uint_type, high, sum, cross1});
        const auto result = id();
        emit(spv::OpCompositeConstruct, {pair_type, result, low, high});
        return result;
    }
    void add(uint32_t a, uint32_t b, uint32_t result) {
        const auto al = extract(a, 0), ah = extract(a, 1), bl = extract(b, 0), bh = extract(b, 1), sum = id();
        emit(spv::OpIAddCarry, {wide_result_type, sum, al, bl});
        const auto low = extract(sum, 0), carry = extract(sum, 1), upper = id(), high = id();
        emit(spv::OpIAdd, {uint_type, upper, ah, bh});
        emit(spv::OpIAdd, {uint_type, high, upper, carry});
        emit(spv::OpCompositeConstruct, {pair_type, result, low, high});
    }
    uint32_t stride(uint32_t type) {
        const auto& t = get(type);
        if (t.op == spv::OpTypeAccelerationStructureKHR) return pair_constant(1, 0);
        if (t.op != spv::OpTypeArray) {
            fail("Nested runtime descriptor array has no finite flattening stride");
            return 0;
        }
        const auto length = integer_pair(t.words.at(3));
        if (!length) return 0;
        const auto inner = stride(t.words.at(2));
        return inner ? multiply(length, inner) : 0;
    }
    bool access_path(const Instruction& item) {
        const auto base = item.words.at(3);
        const auto& input = paths.at(base);
        const auto& result = paths.at(item.result);
        emit(spv::OpCopyObject, {uint_type, result.set, input.set});
        emit(spv::OpCopyObject, {uint_type, result.binding, input.binding});
        uint32_t offset = input.index;
        uint32_t type = get(get(base).type).words.at(3);
        for (size_t index = 4; index < item.words.size(); ++index) {
            const auto& container = get(type);
            if (container.op != spv::OpTypeArray && container.op != spv::OpTypeRuntimeArray)
                return fail("AS descriptor access chain is not a sequence of array dimensions");
            type = container.words.at(2);
            const auto index_pair = integer_pair(item.words[index]);
            if (!index_pair) return false;
            const auto scale = stride(type);
            if (!scale) return false;
            const auto term = multiply(index_pair, scale);
            const auto next = index + 1 == item.words.size() ? result.index : id();
            add(offset, term, next);
            offset = next;
        }
        if (item.words.size() == 4) emit(spv::OpCopyObject, {pair_type, result.index, input.index});
        return true;
    }
    bool transform() {
        output.words.assign(source.begin(), source.begin() + 5);
        // Declarations are finalized during emission (array strides can use
        // specialization constants), then inserted before the first function.
        size_t declaration_offset = 0;
        uint32_t function = 0;
        bool parameters_emitted = false, first_label = false;
        for (const auto& item : instructions) {
            auto words = item.words;
            if (item.op == spv::OpFunction) {
                if (!declaration_offset) declaration_offset = output.words.size();
                function = item.result;
                parameters_emitted = false;
                first_label = true;
                words[4] = functions.at(function).type;
                // A parallel return now stores through an out parameter, and
                // provenance providers may read layer-owned memory. Original
                // Const/Pure promises cannot describe these added operations.
                if (functions.at(function).modified)
                    words[3] &= ~(uint32_t(spv::FunctionControlConstMask) | uint32_t(spv::FunctionControlPureMask));
            }
            if (function && !parameters_emitted && item.op != spv::OpFunction && item.op != spv::OpFunctionParameter &&
                item.op != spv::OpLine && item.op != spv::OpNoLine) {
                const auto& f = functions.at(function);
                for (const uint32_t parameter : f.parameters) {
                    if (metadata.count(parameter)) {
                        emit(spv::OpFunctionParameter, {pair_type, metadata.at(parameter)});
                    } else if (paths.count(parameter)) {
                        const auto& path = paths.at(parameter);
                        emit(spv::OpFunctionParameter, {uint_type, path.set});
                        emit(spv::OpFunctionParameter, {uint_type, path.binding});
                        emit(spv::OpFunctionParameter, {pair_type, path.index});
                    }
                }
                if (f.return_metadata) emit(spv::OpFunctionParameter, {out_pointer_type, f.return_metadata});
                parameters_emitted = true;
            }
            if (item.op == spv::OpFunctionCall) {
                const auto& callee = functions.at(words.at(3));
                for (size_t i = 0; i < callee.parameters.size(); ++i) {
                    const auto argument = item.words.at(4 + i);
                    if (metadata.count(callee.parameters[i])) {
                        words.push_back(metadata.at(argument));
                    } else if (paths.count(callee.parameters[i])) {
                        const auto& path = paths.at(argument);
                        words.insert(words.end(), {path.set, path.binding, path.index});
                    }
                }
                if (callee.return_metadata) words.push_back(return_slots.at(item.result));
                words[0] = uint32_t(words.size() << 16) | uint32_t(item.op);
            }
            if (item.op == spv::OpReturnValue && functions.at(function).return_metadata)
                emit(spv::OpStore, {functions.at(function).return_metadata, metadata.at(words.at(1))});
            if (item.op == spv::OpRayQueryInitializeKHR) {
                const auto found = metadata.find(words.at(2));
                if (found == metadata.end()) return fail("Ray query initialization has unknown AS provenance");
                output.initialization_metadata.push_back({uint32_t(output.words.size()), found->second});
            }
            if (item.op == spv::OpLoad && metadata.count(item.result)) {
                const auto& path = paths.at(words.at(3));
                const auto fetched = id(), address = id();
                emit(spv::OpFunctionCall,
                     {fetch_type, fetched, options.descriptor_fetch_function, path.set, path.binding, path.index});
                emit(spv::OpVectorShuffle, {pair_type, address, fetched, fetched, 0, 1});
                emit(spv::OpVectorShuffle, {pair_type, metadata.at(item.result), fetched, fetched, 2, 3});
                emit(spv::OpConvertUToAccelerationStructureKHR, {item.type, item.result, address});
                ++output.descriptor_loads;
                continue;
            }
            emit(output.words, words);
            if (item.op == spv::OpLabel && first_label) {
                for (const uint32_t local : functions.at(function).locals)
                    emit(spv::OpVariable, {out_pointer_type, local, spv::StorageClassFunction});
                first_label = false;
            }
            if (metadata.count(item.result)) {
                const auto result = metadata.at(item.result);
                switch (item.op) {
                    case spv::OpFunctionParameter:
                        break;
                    case spv::OpConvertUToAccelerationStructureKHR: {
                        const auto address = integer_pair(item.words.at(3));
                        if (!address) return false;
                        emit(spv::OpFunctionCall, {pair_type, result, options.integer_resolver_function, address});
                        ++output.integer_conversions;
                        break;
                    }
                    case spv::OpCopyObject:
                        emit(spv::OpCopyObject, {pair_type, result, metadata.at(item.words.at(3))});
                        break;
                    case spv::OpPhi: {
                        auto phi = item.words;
                        phi[1] = pair_type;
                        phi[2] = result;
                        for (size_t i = 3; i < phi.size(); i += 2) phi[i] = metadata.at(phi[i]);
                        emit(output.words, phi);
                        break;
                    }
                    case spv::OpFunctionCall:
                        emit(spv::OpLoad, {pair_type, result, return_slots.at(item.result)});
                        break;
                    default:
                        return fail("Unimplemented scalar AS producer");
                }
            }
            if (paths.count(item.result)) {
                const auto& path = paths.at(item.result);
                if (item.op == spv::OpAccessChain || item.op == spv::OpInBoundsAccessChain) {
                    if (!access_path(item)) return false;
                } else if (item.op == spv::OpCopyObject) {
                    const auto& original = paths.at(item.words.at(3));
                    emit(spv::OpCopyObject, {uint_type, path.set, original.set});
                    emit(spv::OpCopyObject, {uint_type, path.binding, original.binding});
                    emit(spv::OpCopyObject, {pair_type, path.index, original.index});
                }
            }
            if (item.op == spv::OpFunctionEnd) function = 0;
        }
        size_t inserted = 0;
        for (const auto& declaration : declarations) inserted += declaration.size();
        std::vector<uint32_t> complete;
        complete.reserve(output.words.size() + inserted);
        if (!declaration_offset) declaration_offset = output.words.size();
        complete.insert(complete.end(), output.words.begin(), output.words.begin() + declaration_offset);
        for (const auto& declaration : declarations) emit(complete, declaration);
        complete.insert(complete.end(), output.words.begin() + declaration_offset, output.words.end());
        for (auto& binding : output.initialization_metadata) binding.initialize_word_offset += uint32_t(inserted);
        complete[3] = bound;
        output.words = std::move(complete);
        return true;
    }
};
}  // namespace

AsProvenanceStatus propagate_as_provenance(const std::vector<uint32_t>& source, const AsProvenanceOptions& options,
                                           AsProvenanceModule* output, std::string* diagnostic) {
    if (!output) return AsProvenanceStatus::InvalidModule;
    try {
        spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_2);
        std::string message;
        validator.SetMessageConsumer(
            [&](spv_message_level_t, const char*, const spv_position_t&, const char* text) { message = text; });
        if (!validator.Validate(source)) {
            if (diagnostic) *diagnostic = message;
            return AsProvenanceStatus::InvalidModule;
        }
        Pass pass(source, options);
        if (!pass.parse() || !pass.prepare() || !pass.transform()) {
            if (diagnostic) *diagnostic = pass.error;
            return AsProvenanceStatus::Unsupported;
        }
        if (!validator.Validate(pass.output.words)) {
            if (diagnostic) *diagnostic = message;
            return AsProvenanceStatus::Unsupported;
        }
        if (diagnostic) diagnostic->clear();
        *output = std::move(pass.output);
        return AsProvenanceStatus::Success;
    } catch (const std::bad_alloc&) {
        return AsProvenanceStatus::OutOfMemory;
    } catch (const std::exception&) {
        if (diagnostic) {
            try {
                *diagnostic = "Unsupported AS provenance graph or SPIR-V ID limit";
            } catch (const std::bad_alloc&) {
                return AsProvenanceStatus::OutOfMemory;
            }
        }
        return AsProvenanceStatus::Unsupported;
    }
}
}  // namespace vel::omm
