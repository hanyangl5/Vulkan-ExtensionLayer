// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "frame_emitter.h"

#include <spirv_cross/spirv.hpp>
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace vel::rt {
namespace {
struct Definition {
    spv::Op op;
    std::vector<uint32_t> words;
};
class Emitter {
  public:
    Emitter(const std::vector<uint32_t> &input, ContinuationFrames plan, std::string prefix) : prefix(std::move(prefix)) {
        output.plan = std::move(plan);
        if (this->prefix.empty() || !std::isalpha(static_cast<unsigned char>(this->prefix.front())) ||
            !std::all_of(this->prefix.begin(), this->prefix.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
            throw std::runtime_error("Invalid continuation frame emitter ID prefix");
        for (size_t p = 5; p < input.size(); p += input[p] >> 16) {
            auto op = static_cast<spv::Op>(input[p] & 0xffff);
            if ((op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer) || op == spv::OpConstant) {
                uint32_t id = op == spv::OpConstant ? input.at(p + 2) : input.at(p + 1);
                definitions.emplace(id, Definition{op, {input.begin() + p, input.begin() + p + (input[p] >> 16)}});
            }
        }
        for (const auto &layout : output.plan.types) layouts.emplace(layout.type, layout);
        for (const auto &frame : output.plan.functions) {
            // uint indexing gives an explicit, checked backend ABI limit. The
            // analysis planner itself still supports uint64-sized frames.
            if (frame.size > std::numeric_limits<uint32_t>::max() - 3u)
                throw std::runtime_error("Continuation frame emitter requires a frame smaller than 4 GiB");
        }
    }

    ContinuationFrameCodecs Emit(uint32_t set, uint32_t binding) {
        output.word_type = ID("u32");
        output.bool_type = ID("bool");
        output.void_type = ID("void");
        output.memory = ID("memory");
        caps.insert("Shader");
        types << ID("void") << " = OpTypeVoid\n"
              << ID("bool") << " = OpTypeBool\n"
              << ID("u32") << " = OpTypeInt 32 0\n"
              << ID("pair") << " = OpTypeVector " << ID("u32") << " 2\n";
        scalars["i32u"] = ID("u32");
        type_symbols[0] = ID("void");
        Constant(0);
        Constant(1);
        Constant(2);
        Constant(3);
        Constant(8);
        for (const auto &layout : output.plan.types) Type(layout.type);
        for (auto type : order) {
            auto symbol = Type(type);
            load_signatures[type] = Signature("load_sig" + std::to_string(type), symbol, {ID("u32"), ID("u32")});
            store_signatures[type] = Signature("store_sig" + std::to_string(type), ID("void"), {ID("u32"), ID("u32"), symbol});
        }
        for (auto type : order) {
            EmitLoad(type);
            EmitStore(type);
            output.types.push_back({type, Type(type), Load(type), Store(type)});
        }
        for (const auto &frame : output.plan.functions) {
            for (const auto &site : frame.suspensions) EmitSuspension(frame, site);
        }
        std::ostringstream storage;
        storage << ID("words") << " = OpTypeRuntimeArray " << ID("u32") << "\n"
                << ID("buffer") << " = OpTypeStruct " << ID("words") << "\n"
                << ID("buffer_ptr") << " = OpTypePointer StorageBuffer " << ID("buffer") << "\n"
                << ID("word_ptr") << " = OpTypePointer StorageBuffer " << ID("u32") << "\n"
                << output.memory << " = OpVariable " << ID("buffer_ptr") << " StorageBuffer\n";
        for (const auto &cap : caps) output.capabilities += "OpCapability " + cap + "\n";
        output.annotations = "OpDecorate " + ID("words") + " ArrayStride 4\nOpMemberDecorate " + ID("buffer") +
                             " 0 Offset 0\nOpDecorate " + ID("buffer") + " Block\nOpDecorate " + output.memory + " DescriptorSet " +
                             std::to_string(set) + "\nOpDecorate " + output.memory + " Binding " + std::to_string(binding) + "\n";
        output.declarations = types.str() + storage.str();
        output.functions = functions.str();
        return std::move(output);
    }

  private:
    std::string prefix;
    ContinuationFrameCodecs output;
    std::map<uint32_t, Definition> definitions;
    std::map<uint32_t, ContinuationTypeLayout> layouts;
    std::map<uint32_t, std::string> type_symbols;
    std::map<uint32_t, std::string> load_signatures, store_signatures;
    std::map<std::string, std::string> signatures;
    std::map<std::string, std::string> scalars;
    std::set<uint32_t> constant_values;
    std::set<std::string> caps;
    std::vector<uint32_t> order;
    std::ostringstream types, functions;
    uint32_t serial = 0;
    std::string ID(const std::string &suffix) const { return "%" + prefix + suffix; }
    std::string Fresh() { return ID("v" + std::to_string(++serial)); }
    std::string Constant(uint32_t value) {
        auto id = ID("c" + std::to_string(value));
        if (constant_values.insert(value).second) types << id << " = OpConstant " << ID("u32") << " " << value << "\n";
        return id;
    }
    std::string Load(uint32_t id) const { return ID("load" + std::to_string(id)); }
    std::string Store(uint32_t id) const { return ID("store" + std::to_string(id)); }
    std::string LoadSignature(uint32_t id) const { return load_signatures.at(id); }
    std::string StoreSignature(uint32_t id) const { return store_signatures.at(id); }
    std::string Signature(const std::string &name, const std::string &result, const std::vector<std::string> &parameters) {
        std::string key = result;
        for (const auto &parameter : parameters) key += " " + parameter;
        const auto existing = signatures.find(key);
        if (existing != signatures.end()) return existing->second;
        const auto id = ID(name);
        types << id << " = OpTypeFunction " << key << "\n";
        signatures[key] = id;
        return id;
    }
    std::string Unsigned(uint32_t width) {
        const auto key = "i" + std::to_string(width) + "u";
        auto found = scalars.find(key);
        if (found != scalars.end()) return found->second;
        if (width != 32) caps.insert("Int" + std::to_string(width));
        auto name = ID(key);
        types << name << " = OpTypeInt " << width << " 0\n";
        scalars[key] = name;
        return name;
    }
    std::string Type(uint32_t id) {
        auto known = type_symbols.find(id);
        if (known != type_symbols.end()) return known->second;
        const auto &d = definitions.at(id);
        std::string name = ID("t" + std::to_string(id));
        if (d.op == spv::OpTypeBool) {
            name = ID("bool");
        } else if (d.op == spv::OpTypeInt || d.op == spv::OpTypeFloat) {
            uint32_t width = d.words.at(2);
            if (d.op == spv::OpTypeFloat && width == 8)
                throw std::runtime_error("Continuation frame emitter does not support float8");
            auto key = (d.op == spv::OpTypeInt ? "i" : "f") + std::to_string(width);
            if (d.op == spv::OpTypeInt) key += d.words.at(3) ? "s" : "u";
            auto found = scalars.find(key);
            if (found != scalars.end()) {
                name = found->second;
            } else {
                if (d.op == spv::OpTypeInt && width != 32) caps.insert("Int" + std::to_string(width));
                if (d.op == spv::OpTypeFloat && width != 32) caps.insert("Float" + std::to_string(width));
                types << name << (d.op == spv::OpTypeInt ? " = OpTypeInt " : " = OpTypeFloat ") << width;
                if (d.op == spv::OpTypeInt) types << " " << d.words.at(3);
                types << "\n";
                scalars[key] = name;
            }
            // Narrow float/signed codecs bitcast through an unsigned scalar.
            if (width < 32) Unsigned(width);
        } else if (d.op == spv::OpTypeVector || d.op == spv::OpTypeMatrix || d.op == spv::OpTypeArray) {
            auto element = Type(d.words.at(2));
            uint32_t count = Count(id);
            // Reject code-size explosions explicitly; the final state compiler
            // can add dynamically indexed array codecs without changing ABI.
            if (count > 4096) throw std::runtime_error("Continuation aggregate codec exceeds 4096 elements");
            if (d.op == spv::OpTypeVector && element == ID("u32") && count == 2) {
                name = ID("pair");
            } else {
                auto count_operand = d.op == spv::OpTypeArray ? Constant(count) : std::to_string(count);
                types << name
                      << (d.op == spv::OpTypeVector   ? " = OpTypeVector "
                          : d.op == spv::OpTypeMatrix ? " = OpTypeMatrix "
                                                      : " = OpTypeArray ")
                      << element << " " << count_operand << "\n";
            }
        } else if (d.op == spv::OpTypeStruct) {
            std::vector<std::string> members;
            for (size_t n = 2; n < d.words.size(); ++n) members.push_back(Type(d.words[n]));
            types << name << " = OpTypeStruct";
            for (const auto &member : members) types << " " << member;
            types << "\n";
        } else {
            throw std::runtime_error("Unsupported continuation frame codec type");
        }
        type_symbols[id] = name;
        order.push_back(id);
        return name;
    }
    uint32_t Count(uint32_t type) const {
        const auto &d = definitions.at(type);
        if (d.op != spv::OpTypeArray) return d.words.at(3);
        const auto &length = definitions.at(d.words.at(3));
        uint64_t value = length.words.at(3);
        if (length.words.size() > 4) value |= uint64_t(length.words.at(4)) << 32;
        if (value > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Continuation array codec length exceeds uint32");
        return static_cast<uint32_t>(value);
    }
    std::vector<std::pair<uint32_t, uint32_t>> Elements(uint32_t type) const {
        const auto &d = definitions.at(type);
        const auto &layout = layouts.at(type);
        std::vector<std::pair<uint32_t, uint32_t>> result;
        if (d.op == spv::OpTypeStruct) {
            for (size_t n = 2; n < d.words.size(); ++n)
                result.push_back({d.words[n], static_cast<uint32_t>(layout.member_offsets[n - 2])});
        } else {
            for (uint32_t n = 0; n < Count(type); ++n)
                result.push_back({d.words.at(2), static_cast<uint32_t>(n * layout.element_stride)});
        }
        return result;
    }
    std::string Op(const std::string &operation) {
        auto result = Fresh();
        functions << result << " = " << operation << "\n";
        return result;
    }
    std::string Pointer(const std::string &base, const std::string &offset) {
        auto word = Op("OpShiftRightLogical " + ID("u32") + " " + offset + " " + Constant(2));
        word = Op("OpIAdd " + ID("u32") + " " + base + " " + word);
        return Op("OpAccessChain " + ID("word_ptr") + " " + output.memory + " " + Constant(0) + " " + word);
    }
    std::string Shift(const std::string &offset) {
        auto byte = Op("OpBitwiseAnd " + ID("u32") + " " + offset + " " + Constant(3));
        return Op("OpIMul " + ID("u32") + " " + byte + " " + Constant(8));
    }
    void EmitLoad(uint32_t type) {
        functions << Load(type) << " = OpFunction " << Type(type) << " None " << LoadSignature(type) << "\n";
        auto base = Op("OpFunctionParameter " + ID("u32"));
        auto offset = Op("OpFunctionParameter " + ID("u32"));
        Op("OpLabel");
        auto op = definitions.at(type).op;
        std::string result;
        if (op == spv::OpTypeBool || op == spv::OpTypeInt || op == spv::OpTypeFloat) {
            auto ptr = Pointer(base, offset);
            auto word = Op("OpLoad " + ID("u32") + " " + ptr);
            uint32_t width = op == spv::OpTypeBool ? 32 : definitions.at(type).words.at(2);
            if (op == spv::OpTypeBool) {
                result = Op("OpINotEqual " + ID("bool") + " " + word + " " + Constant(0));
            } else if (width == 64) {
                auto next = Op("OpIAdd " + ID("u32") + " " + offset + " " + Constant(4));
                auto hi = Op("OpLoad " + ID("u32") + " " + Pointer(base, next));
                auto pair = Op("OpCompositeConstruct " + ID("pair") + " " + word + " " + hi);
                result = Op("OpBitcast " + Type(type) + " " + pair);
            } else {
                auto bits = word;
                if (width < 32) {
                    bits = Op("OpBitFieldUExtract " + ID("u32") + " " + word + " " + Shift(offset) + " " + Constant(width));
                    bits = Op("OpUConvert " + Unsigned(width) + " " + bits);
                }
                result =
                    Type(type) == (width < 32 ? Unsigned(width) : ID("u32")) ? bits : Op("OpBitcast " + Type(type) + " " + bits);
            }
        } else {
            std::string construct = "OpCompositeConstruct " + Type(type);
            for (auto [child, delta] : Elements(type)) {
                auto address = Op("OpIAdd " + ID("u32") + " " + offset + " " + Constant(delta));
                construct += " " + Op("OpFunctionCall " + Type(child) + " " + Load(child) + " " + base + " " + address);
            }
            result = Op(construct);
        }
        functions << "OpReturnValue " << result << "\nOpFunctionEnd\n";
    }
    void EmitStore(uint32_t type) {
        functions << Store(type) << " = OpFunction " << ID("void") << " None " << StoreSignature(type) << "\n";
        auto base = Op("OpFunctionParameter " + ID("u32"));
        auto offset = Op("OpFunctionParameter " + ID("u32"));
        auto value = Op("OpFunctionParameter " + Type(type));
        Op("OpLabel");
        auto op = definitions.at(type).op;
        if (op == spv::OpTypeBool || op == spv::OpTypeInt || op == spv::OpTypeFloat) {
            auto ptr = Pointer(base, offset);
            uint32_t width = op == spv::OpTypeBool ? 32 : definitions.at(type).words.at(2);
            if (width == 64) {
                auto pair = Op("OpBitcast " + ID("pair") + " " + value);
                auto lo = Op("OpCompositeExtract " + ID("u32") + " " + pair + " 0");
                auto hi = Op("OpCompositeExtract " + ID("u32") + " " + pair + " 1");
                functions << "OpStore " << ptr << " " << lo << "\n";
                auto next = Op("OpIAdd " + ID("u32") + " " + offset + " " + Constant(4));
                auto next_ptr = Pointer(base, next);
                functions << "OpStore " << next_ptr << " " << hi << "\n";
            } else {
                auto bits = value;
                if (op == spv::OpTypeBool) {
                    bits = Op("OpSelect " + ID("u32") + " " + value + " " + Constant(1) + " " + Constant(0));
                } else {
                    if (Type(type) != (width < 32 ? Unsigned(width) : ID("u32")))
                        bits = Op("OpBitcast " + (width < 32 ? Unsigned(width) : ID("u32")) + " " + value);
                    if (width < 32) bits = Op("OpUConvert " + ID("u32") + " " + bits);
                }
                if (width < 32) {
                    auto previous = Op("OpLoad " + ID("u32") + " " + ptr);
                    bits = Op("OpBitFieldInsert " + ID("u32") + " " + previous + " " + bits + " " + Shift(offset) + " " +
                              Constant(width));
                }
                functions << "OpStore " << ptr << " " << bits << "\n";
            }
        } else {
            uint32_t index = 0;
            for (auto [child, delta] : Elements(type)) {
                auto part = Op("OpCompositeExtract " + Type(child) + " " + value + " " + std::to_string(index++));
                auto address = Op("OpIAdd " + ID("u32") + " " + offset + " " + Constant(delta));
                Op("OpFunctionCall " + ID("void") + " " + Store(child) + " " + base + " " + address + " " + part);
            }
        }
        functions << "OpReturn\nOpFunctionEnd\n";
    }
    void EmitSuspension(const FunctionFrame &frame, const SuspensionFrame &site) {
        const auto suffix = "site" + std::to_string(frame.function) + "_" + std::to_string(site.word_offset);
        ContinuationSuspensionCodec codec{frame.function,        site.word_offset,        {},
                                          ID(suffix + "_spill"), ID(suffix + "_restore"), ID(suffix + "_values")};
        std::vector<const ContinuationFrameSlot *> slots;
        for (auto value : site.saved_values) {
            auto slot = std::find_if(frame.slots.begin(), frame.slots.end(),
                                     [&](const ContinuationFrameSlot &s) { return s.value == value; });
            if (slot == frame.slots.end()) throw std::runtime_error("Continuation suspension slot missing");
            if (slot->local_storage) continue;
            codec.saved_values.push_back(value);
            slots.push_back(&*slot);
        }
        if (slots.empty()) {
            codec.restored_type = ID("void");
        } else {
            types << codec.restored_type << " = OpTypeStruct";
            for (auto slot : slots) types << " " << Type(slot->type);
            types << "\n";
        }
        std::vector<std::string> spill_parameters{ID("u32")};
        for (auto slot : slots) spill_parameters.push_back(Type(slot->type));
        const auto spill_signature = Signature(suffix + "_spill_sig", ID("void"), spill_parameters);
        const auto restore_signature = Signature(suffix + "_restore_sig", codec.restored_type, {ID("u32")});

        functions << codec.spill << " = OpFunction " << ID("void") << " None " << spill_signature << "\n";
        auto base = Op("OpFunctionParameter " + ID("u32"));
        std::vector<std::string> parameters;
        for (auto slot : slots) parameters.push_back(Op("OpFunctionParameter " + Type(slot->type)));
        Op("OpLabel");
        for (size_t n = 0; n < slots.size(); ++n) {
            auto slot = slots[n];
            Op("OpFunctionCall " + ID("void") + " " + Store(slot->type) + " " + base + " " +
               Constant(static_cast<uint32_t>(slot->offset)) + " " + parameters[n]);
        }
        functions << "OpReturn\nOpFunctionEnd\n";

        functions << codec.restore << " = OpFunction " << codec.restored_type << " None " << restore_signature << "\n";
        base = Op("OpFunctionParameter " + ID("u32"));
        Op("OpLabel");
        if (slots.empty()) {
            functions << "OpReturn\n";
        } else {
            std::string construct = "OpCompositeConstruct " + codec.restored_type;
            for (auto slot : slots) {
                construct += " " + Op("OpFunctionCall " + Type(slot->type) + " " + Load(slot->type) + " " + base + " " +
                                      Constant(static_cast<uint32_t>(slot->offset)));
            }
            auto value = Op(construct);
            functions << "OpReturnValue " << value << "\n";
        }
        functions << "OpFunctionEnd\n";
        output.suspensions.push_back(std::move(codec));
    }
};
}  // namespace

ContinuationFrameCodecs EmitContinuationFrameCodecs(const std::vector<uint32_t> &spirv, const std::string &entry,
                                                    uint32_t execution_model, uint32_t descriptor_set, uint32_t binding,
                                                    const std::string &prefix) {
    auto plan = PlanContinuationFrames(spirv, entry, execution_model);
    return Emitter(spirv, std::move(plan), prefix).Emit(descriptor_set, binding);
}
}  // namespace vel::rt
