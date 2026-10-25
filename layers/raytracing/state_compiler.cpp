// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "state_compiler.h"
#include "frame_emitter.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv_cross/spirv.hpp>
#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace vel::rt {
namespace {
struct Instruction {
    uint32_t offset, type, result;
    spv::Op op;
    std::vector<uint32_t> words, uses;
    std::string text;
    bool in_function = false;
};
struct Block {
    uint32_t label;
    std::vector<const Instruction *> body;
};
struct Parsed {
    std::vector<Instruction> instructions;
    uint32_t offset = 5;
};
spv_result_t ParseInstruction(void *user, const spv_parsed_instruction_t *p) {
    auto &m = *static_cast<Parsed *>(user);
    Instruction i{m.offset, p->type_id, p->result_id, static_cast<spv::Op>(p->opcode), {}, {}, {}};
    i.words.assign(p->words, p->words + p->num_words);
    for (uint32_t n = 0; n < p->num_operands; ++n) {
        const auto &o = p->operands[n];
        if (o.type == SPV_OPERAND_TYPE_ID || o.type == SPV_OPERAND_TYPE_SCOPE_ID || o.type == SPV_OPERAND_TYPE_MEMORY_SEMANTICS_ID)
            i.uses.insert(i.uses.end(), p->words + o.offset, p->words + o.offset + o.num_words);
    }
    m.offset += p->num_words;
    m.instructions.push_back(std::move(i));
    return SPV_SUCCESS;
}
[[noreturn]] void Unsupported(const std::string &reason) {
    throw std::runtime_error("Typed continuation state compiler: " + reason);
}
std::string Rewrite(const std::string &text, const std::function<std::string(uint32_t)> &id) {
    std::string result;
    for (size_t n = 0; n < text.size();) {
        if (text[n] == '"') {
            // Literal bytes are not IDs. Preserve quoted and escaped strings
            // exactly, including names/semantics containing percent digits.
            result += text[n++];
            while (n < text.size()) {
                char c = text[n++];
                result += c;
                if (c == '\\' && n < text.size())
                    result += text[n++];
                else if (c == '"')
                    break;
            }
            continue;
        }
        if (text[n] == '%' && n + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[n + 1]))) {
            auto end = n + 1;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            result += id(static_cast<uint32_t>(std::stoul(text.substr(n + 1, end - n - 1))));
            n = end;
        } else
            result += text[n++];
    }
    return result;
}
class Compiler {
  public:
    Compiler(std::vector<uint32_t> source, std::string entry, uint32_t model, uint32_t set, uint32_t binding)
        : entry(std::move(entry)), model(model), set(set), binding(binding) {
        output.source = std::move(source);
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        std::string assembly;
        if (!tools.Disassemble(output.source, &assembly, SPV_BINARY_TO_TEXT_OPTION_NO_HEADER))
            Unsupported("source disassembly failed");
        auto context = spvContextCreate(SPV_ENV_VULKAN_1_3);
        auto status =
            spvBinaryParse(context, &parsed, output.source.data(), output.source.size(), nullptr, ParseInstruction, nullptr);
        spvContextDestroy(context);
        if (status != SPV_SUCCESS) Unsupported("source parse failed");
        std::istringstream lines(assembly);
        std::string line;
        size_t n = 0;
        while (std::getline(lines, line)) {
            if (line.find_first_not_of(" \t\r") == std::string::npos || line.find_first_not_of(" \t") == line.find(';')) continue;
            if (n >= parsed.instructions.size()) Unsupported("disassembly/instruction mismatch");
            parsed.instructions[n++].text = line;
        }
        if (n != parsed.instructions.size()) Unsupported("disassembly/instruction count mismatch");
        bool inside = false;
        uint32_t functions = 0;
        for (auto &i : parsed.instructions) {
            if (i.op == spv::OpFunction) {
                inside = true;
                ++functions;
            }
            i.in_function = inside;
            if (i.result) definitions[i.result] = &i;
            if (i.op == spv::OpLabel && inside)
                blocks.push_back({i.result, {}});
            else if (inside && !blocks.empty() && i.op != spv::OpFunctionEnd)
                blocks.back().body.push_back(&i);
            if (i.op == spv::OpFunctionEnd) inside = false;
        }
        if (functions != 1 || blocks.empty()) Unsupported("reachable helper could not be inlined");
        for (const auto &i : parsed.instructions) {
            if (i.op == spv::OpMemoryModel &&
                (i.words.at(1) != spv::AddressingModelLogical || i.words.at(2) != spv::MemoryModelGLSL450))
                Unsupported("source memory model requires explicit frame/scheduler support; only Logical GLSL450 is implemented");
        }
        for (const auto &i : parsed.instructions) {
            if (!i.in_function) continue;
            auto opcode = std::string(spvOpcodeString(i.op));
            if (opcode.find("Group") == 0 || opcode.find("Subgroup") == 0 || (i.op >= spv::OpDPdx && i.op <= spv::OpFwidthCoarse))
                Unsupported("subgroup/derivative execution semantics require scheduler support");
            if (i.type && Def(i.type).op == spv::OpTypePointer && i.op != spv::OpVariable && i.op != spv::OpAccessChain &&
                i.op != spv::OpInBoundsAccessChain && i.op != spv::OpCopyObject)
                Unsupported("unsupported pointer provenance Op" + std::string(spvOpcodeString(i.op)) + " for ID " +
                            std::to_string(i.result));
            NeedType(i.type);
            for (auto id : i.uses) NeedGlobal(id);
        }
        for (const auto &i : parsed.instructions) {
            if (i.op == spv::OpExecutionMode || i.op == spv::OpExecutionModeId) {
                for (auto id : i.uses)
                    if (Def(id).op != spv::OpFunction) NeedGlobal(id);
            }
            if (i.op == spv::OpDecorate && i.words.at(2) == spv::DecorationVolatile && Def(i.words.at(1)).in_function)
                Unsupported("volatile Function storage requires volatile frame codecs");
        }
        std::map<uint32_t, uint32_t> resource_sets, resource_bindings;
        for (const auto &i : parsed.instructions) {
            if (i.op != spv::OpDecorate || !globals.count(i.words.at(1))) continue;
            if (i.words.at(2) == spv::DecorationDescriptorSet) resource_sets[i.words.at(1)] = i.words.at(3);
            if (i.words.at(2) == spv::DecorationBinding) resource_bindings[i.words.at(1)] = i.words.at(3);
        }
        for (const auto &[resource, resource_set] : resource_sets) {
            auto source_binding = resource_bindings.find(resource);
            if (source_binding != resource_bindings.end() && resource_set == set && source_binding->second == binding)
                Unsupported("frame descriptor binding collides with a live source resource");
        }
        std::vector<uint32_t> types(value_types.begin(), value_types.end());
        const auto preview = PlanContinuationFrames(output.source, this->entry, model, types);
        for (const auto &f : preview.functions) {
            if (f.function != preview.analysis.entry) continue;
            const auto bounds = PlanTypedContinuationFrameBounds(f.size);
            output.frame_stride_words = bounds.stride_words;
            output.max_invocations = bounds.max_invocations;
        }
        code = EmitContinuationFrameCodecs(output.source, this->entry, model, set, binding, "sc_cf_", types);
        // Reuse the component's existing word pointer declaration rather than
        // emit an illegal duplicate non-aggregate SPIR-V pointer type.
        pointer_types[std::to_string(spv::StorageClassStorageBuffer) + " " + code.word_type] = code.word_pointer_type;
        output.frames = code.plan;
        for (const auto &c : code.types) codecs[c.type] = c;
        for (const auto &f : code.plan.functions) {
            if (f.function == code.plan.analysis.entry) frame = &f;
        }
        if (!frame || frame->suspensions.empty()) Unsupported("entry must contain a supported suspension");
        for (const auto &slot : frame->slots) slots[slot.value] = &slot;
        for (const auto &site : code.suspensions) sites[site.word_offset] = &site;
        output.control_word = static_cast<uint32_t>((frame->size + 3) / 4);
        for (const auto &[id, c] : codecs) {
            const auto &d = Def(id);
            if (d.op == spv::OpTypeInt && d.words.at(2) == 32 && d.words.at(3) == 0) word_codec = &c;
        }
        if (!word_codec) Unsupported("missing uint32 request/control codec");
        BuildStates();
    }

    TypedContinuationProgram Run() {
        EmitGlobals();
        EmitStep();
        EmitKernel();
        std::string assembly = code.capabilities + source_caps.str() + extensions.str() + imports.str() +
                               "OpMemoryModel Logical GLSL450\nOpEntryPoint GLCompute %sc_main \"main\" " + code.memory;
        for (const auto &global : interfaces) assembly += " " + global;
        assembly += "\nOpExecutionMode %sc_main LocalSize 1 1 1\n" + execution_modes.str() + code.annotations + annotations.str() +
                    code.declarations + declarations.str() + code.functions + functions.str();
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        std::string error;
        tools.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &p, const char *message) {
            error += std::to_string(p.index) + ": " + message + "\n";
        });
        auto assembled = tools.Assemble(assembly, &output.spirv);
        if (!assembled) Unsupported("generated assembly failed: " + error);
        auto valid = tools.Validate(output.spirv);
        if (!valid) Unsupported("generated SPIR-V failed validation: " + error);
        output.state_count = static_cast<uint32_t>(states.size());
        return std::move(output);
    }

  private:
    struct State {
        const Block *block;
        size_t begin, end;
        uint32_t number;
        const Instruction *resume = nullptr;
    };
    TypedContinuationProgram output;
    Parsed parsed;
    std::string entry;
    uint32_t model, set, binding, serial = 0;
    ContinuationFrameCodecs code;
    const FunctionFrame *frame = nullptr;
    const ContinuationTypeCodec *word_codec = nullptr;
    std::vector<Block> blocks;
    std::vector<State> states;
    std::map<uint32_t, const Instruction *> definitions;
    std::set<uint32_t> globals, needed_types, value_types;
    std::map<uint32_t, ContinuationTypeCodec> codecs;
    std::map<uint32_t, const ContinuationFrameSlot *> slots;
    std::map<uint32_t, const ContinuationSuspensionCodec *> sites;
    std::map<uint32_t, uint32_t> first_state;
    std::map<uint32_t, std::string> cloned_types;
    std::map<std::string, std::string> pointer_types;
    std::map<uint32_t, std::string> constants;
    std::vector<std::string> interfaces;
    std::ostringstream source_caps, imports, extensions, execution_modes, annotations, declarations, functions;
    std::string gid, gid_type, group_count, pc_pointer;

    const Instruction &Def(uint32_t id) const {
        const auto found = definitions.find(id);
        if (found == definitions.end()) Unsupported("undefined source ID " + std::to_string(id));
        return *found->second;
    }
    std::string Fresh() { return "%sc_v" + std::to_string(++serial); }
    static std::string SourceID(uint32_t id) { return "%sc_s" + std::to_string(id); }
    static std::string Register(uint32_t id) { return "%sc_r" + std::to_string(id); }
    std::string Op(const std::string &text) {
        auto id = Fresh();
        functions << id << " = " << text << "\n";
        return id;
    }
    std::string Constant(uint32_t value) {
        auto known = constants.find(value);
        if (known != constants.end()) return known->second;
        auto name = "%sc_c" + std::to_string(value);
        declarations << name << " = OpConstant " << code.word_type << " " << value << "\n";
        constants[value] = name;
        return name;
    }
    bool Layoutable(uint32_t id) const {
        const auto &d = Def(id);
        if (d.op == spv::OpTypeBool || d.op == spv::OpTypeInt || d.op == spv::OpTypeFloat) return true;
        if (d.op == spv::OpTypeVector || d.op == spv::OpTypeMatrix || d.op == spv::OpTypeArray) return Layoutable(d.words.at(2));
        if (d.op == spv::OpTypeStruct) {
            for (size_t n = 2; n < d.words.size(); ++n)
                if (!Layoutable(d.words[n])) return false;
            return true;
        }
        return false;
    }
    void NeedType(uint32_t id) {
        if (!id || !needed_types.insert(id).second) return;
        const auto &d = Def(id);
        if (Layoutable(id)) value_types.insert(id);
        for (auto child : d.uses) {
            const auto &v = Def(child);
            if (v.op >= spv::OpTypeVoid && v.op <= spv::OpTypeForwardPointer)
                NeedType(child);
            else
                NeedGlobal(child);
        }
    }
    void NeedGlobal(uint32_t id) {
        const auto &d = Def(id);
        if (d.in_function || !globals.insert(id).second) return;
        NeedType(d.type);
        for (auto child : d.uses) NeedGlobal(child);
    }
    std::string PointerType(uint32_t storage, const std::string &pointee) {
        auto key = std::to_string(storage) + " " + pointee;
        auto found = pointer_types.find(key);
        if (found != pointer_types.end()) return found->second;
        auto id = Fresh();
        const char *name = storage == spv::StorageClassFunction        ? "Function"
                           : storage == spv::StorageClassInput         ? "Input"
                           : storage == spv::StorageClassPushConstant  ? "PushConstant"
                           : storage == spv::StorageClassStorageBuffer ? "StorageBuffer"
                                                                       : nullptr;
        if (!name) Unsupported("pointer storage class " + std::to_string(storage));
        declarations << id << " = OpTypePointer " << name << " " << pointee << "\n";
        pointer_types[key] = id;
        return id;
    }
    std::string Type(uint32_t id) {
        const auto &d = Def(id);
        if (d.op == spv::OpTypeVoid) return code.void_type;
        auto codec = codecs.find(id);
        if (codec != codecs.end()) return codec->second.spirv_type;
        auto found = cloned_types.find(id);
        if (found != cloned_types.end()) return found->second;
        if (d.op == spv::OpTypePointer) return PointerType(d.words.at(2), Type(d.words.at(3)));
        if (d.op != spv::OpTypeStruct && d.op != spv::OpTypeRuntimeArray)
            Unsupported("opaque or unsupported source type " + std::to_string(id));
        for (auto child : d.uses) Type(child);
        auto name = SourceID(id);
        declarations << Rewrite(d.text, [&](uint32_t child) { return child == id ? name : Type(child); }) << "\n";
        cloned_types[id] = name;
        return name;
    }
    std::string Global(uint32_t id) {
        const auto &d = Def(id);
        if (d.op >= spv::OpTypeVoid && d.op <= spv::OpTypeForwardPointer) return Type(id);
        return SourceID(id);
    }
    void EmitGlobals() {
        std::set<uint32_t> capabilities;
        for (const auto &i : parsed.instructions) {
            if (i.op == spv::OpExecutionMode || i.op == spv::OpExecutionModeId) {
                auto mode = i.words.at(2);
                if (!((mode >= spv::ExecutionModeDenormPreserve && mode <= spv::ExecutionModeRoundingModeRTZ) ||
                      mode == spv::ExecutionModeFPFastMathDefault))
                    Unsupported("source execution mode requires scheduler support");
                execution_modes << Rewrite(i.text, [&](uint32_t id) {
                    return id == code.plan.analysis.entry ? "%sc_main" : Global(id);
                }) << "\n";
            }
            if (i.op == spv::OpCapability && i.words.at(1) != spv::CapabilityShader &&
                i.text.find("OpCapability Ray") == std::string::npos) {
                // The codec already declares scalar capabilities required by
                // the cloned value types. Preserve additional resource caps.
                if (i.words.at(1) != spv::CapabilityInt8 && i.words.at(1) != spv::CapabilityInt16 &&
                    i.words.at(1) != spv::CapabilityInt64 && i.words.at(1) != spv::CapabilityFloat16 &&
                    i.words.at(1) != spv::CapabilityFloat64 && capabilities.insert(i.words.at(1)).second)
                    source_caps << i.text << "\n";
            }
            if (i.op == spv::OpExtension && i.text.find("ray_") == std::string::npos) extensions << i.text << "\n";
            if (i.op == spv::OpExtInstImport && globals.count(i.result)) imports << Rewrite(i.text, GlobalMapper()) << "\n";
        }
        for (auto id : needed_types) {
            if (Def(id).op != spv::OpTypeFunction) Type(id);
        }
        for (const auto &i : parsed.instructions) {
            if (!globals.count(i.result)) continue;
            if (i.op == spv::OpUndef || (i.op >= spv::OpConstantTrue && i.op <= spv::OpConstantNull))
                declarations << Rewrite(i.text, GlobalMapper()) << "\n";
            else if (i.op >= spv::OpSpecConstantTrue && i.op <= spv::OpSpecConstantOp)
                Unsupported("specialization must run before state lowering");
            else if (i.op == spv::OpVariable) {
                if (i.words.at(3) != spv::StorageClassInput && i.words.at(3) != spv::StorageClassPushConstant &&
                    i.words.at(3) != spv::StorageClassStorageBuffer)
                    Unsupported("module variable storage requires invocation backing");
                declarations << Rewrite(i.text, GlobalMapper()) << "\n";
                interfaces.push_back(SourceID(i.result));
            }
        }
        for (const auto &i : parsed.instructions) {
            if (i.op != spv::OpDecorate && i.op != spv::OpMemberDecorate) continue;
            auto target = i.words.at(1);
            if (!globals.count(target) && !needed_types.count(target)) continue;
            if (i.op == spv::OpDecorate && i.words.at(2) == spv::DecorationBuiltIn) {
                auto builtin = i.words.at(3);
                if (builtin == spv::BuiltInLaunchIdKHR) {
                    annotations << "OpDecorate " << Global(target) << " BuiltIn GlobalInvocationId\n";
                    gid = Global(target);
                    gid_type = Type(Def(Def(target).type).words.at(3));
                } else if (builtin == spv::BuiltInLaunchSizeKHR) {
                    annotations << "OpDecorate " << Global(target) << " BuiltIn NumWorkgroups\n";
                    group_count = Global(target);
                } else
                    Unsupported("RT builtin requires scheduler invocation backing");
            } else
                annotations << Rewrite(i.text, GlobalMapper()) << "\n";
        }
    }
    std::function<std::string(uint32_t)> GlobalMapper() {
        return [&](uint32_t id) { return Global(id); };
    }
    void BuildStates() {
        for (const auto &block : blocks) {
            first_state[block.label] = static_cast<uint32_t>(states.size());
            State current{&block, 0, block.body.size(), static_cast<uint32_t>(states.size()), nullptr};
            for (size_t n = 0; n < block.body.size(); ++n) {
                auto op = block.body[n]->op;
                if (op == spv::OpTraceRayKHR || op == spv::OpExecuteCallableKHR)
                    Unsupported("TraceRay/Callable request invocation ABI is not implemented");
                if (op != spv::OpReportIntersectionKHR) continue;
                current.end = n + 1;
                states.push_back(current);
                current = {&block, n + 1, block.body.size(), static_cast<uint32_t>(states.size()), block.body[n]};
            }
            states.push_back(current);
        }
    }
    bool IsPointer(uint32_t type) const { return type && Def(type).op == spv::OpTypePointer; }
    std::string Value(uint32_t id) {
        const auto &d = Def(id);
        if (!d.in_function) return Global(id);
        if (IsPointer(d.type)) return RealPointer(id);
        if (!d.result || !d.type || Def(d.type).op == spv::OpTypeVoid) Unsupported("non-value source operand");
        auto result = Op("OpLoad " + Type(d.type) + " " + Register(id));
        DecorateResult(id, result, true);
        return result;
    }
    void Save(uint32_t id, const std::string &value) { functions << "OpStore " << Register(id) << " " << value << "\n"; }
    uint32_t Storage(uint32_t pointer) const { return Def(Def(pointer).type).words.at(2); }
    std::string Offset(uint32_t pointer) {
        auto slot = slots.find(pointer);
        if (slot != slots.end() && slot->second->local_storage) return Constant(static_cast<uint32_t>(slot->second->offset));
        const auto &d = Def(pointer);
        if (d.op == spv::OpCopyObject) return Offset(d.words.at(3));
        if (d.op != spv::OpAccessChain && d.op != spv::OpInBoundsAccessChain)
            Unsupported("Function pointer provenance for ID " + std::to_string(pointer));
        auto result = Offset(d.words.at(3));
        uint32_t type = Def(Def(d.words.at(3)).type).words.at(3);
        for (size_t n = 4; n < d.words.size(); ++n) {
            const auto &t = Def(type);
            const auto &layout = Layout(type);
            std::string delta;
            if (t.op == spv::OpTypeStruct) {
                const auto &index = Def(d.words[n]);
                if (index.op != spv::OpConstant || index.words.size() != 4) Unsupported("nonconstant structure member");
                auto member = index.words.at(3);
                delta = Constant(static_cast<uint32_t>(layout.member_offsets.at(member)));
                type = t.words.at(member + 2);
            } else if (t.op == spv::OpTypeArray || t.op == spv::OpTypeVector || t.op == spv::OpTypeMatrix) {
                auto index = Value(d.words[n]);
                if (Type(Def(d.words[n]).type) != code.word_type) {
                    const auto width = Def(Def(d.words[n]).type).words.at(2);
                    index = Op(std::string(width == 32 ? "OpBitcast " : "OpUConvert ") + code.word_type + " " + index);
                }
                delta = Op("OpIMul " + code.word_type + " " + index + " " + Constant(static_cast<uint32_t>(layout.element_stride)));
                type = t.words.at(2);
            } else
                Unsupported("access-chain type cannot be relocated");
            result = Op("OpIAdd " + code.word_type + " " + result + " " + delta);
        }
        return result;
    }
    const ContinuationTypeLayout &Layout(uint32_t type) const {
        for (const auto &layout : code.plan.types)
            if (layout.type == type) return layout;
        Unsupported("missing persistent storage layout");
    }
    std::string RealPointer(uint32_t id) {
        const auto &d = Def(id);
        if (Storage(id) == spv::StorageClassFunction) Unsupported("Function pointer used outside typed load/store");
        if (!d.in_function && d.op == spv::OpVariable) return Global(id);
        if (d.op == spv::OpCopyObject) {
            auto source = RealPointer(d.words.at(3));
            auto result = Op("OpCopyObject " + Type(d.type) + " " + source);
            DecorateResult(id, result);
            return result;
        }
        if (d.op != spv::OpAccessChain && d.op != spv::OpInBoundsAccessChain)
            Unsupported("resource pointer provenance for ID " + std::to_string(id));
        auto result = Fresh();
        auto text = Rewrite(d.text, [&](uint32_t operand) {
            if (operand == d.result) return result;
            if (operand == d.type) return Type(operand);
            return Value(operand);
        });
        functions << text << "\n";
        DecorateResult(id, result);
        return result;
    }
    void DecorateResult(uint32_t source, const std::string &result, bool reloaded = false) {
        for (const auto &i : parsed.instructions) {
            if (i.op == spv::OpDecorate && i.words.at(1) == source &&
                (!reloaded || i.words.at(2) == spv::DecorationNonUniform || i.words.at(2) == spv::DecorationRelaxedPrecision))
                annotations << Rewrite(i.text, [&](uint32_t id) { return id == source ? result : Global(id); }) << "\n";
        }
    }
    std::string LoadWord(uint32_t relative) {
        return Op("OpFunctionCall " + code.word_type + " " + word_codec->load + " %sc_base " +
                  Constant((output.control_word + relative) * 4));
    }
    void StoreWord(uint32_t relative, const std::string &value) {
        Op("OpFunctionCall " + code.void_type + " " + word_codec->store + " %sc_base " +
           Constant((output.control_word + relative) * 4) + " " + value);
    }
    void Edge(uint32_t predecessor, uint32_t target) {
        auto found = std::find_if(blocks.begin(), blocks.end(), [&](const Block &b) { return b.label == target; });
        if (found == blocks.end()) Unsupported("missing branch target");
        std::vector<std::pair<uint32_t, std::string>> copies;
        for (auto i : found->body) {
            if (i->op != spv::OpPhi) continue;
            bool matched = false;
            for (size_t n = 3; n < i->words.size(); n += 2) {
                if (i->words.at(n + 1) == predecessor) {
                    copies.push_back({i->result, Value(i->words[n])});
                    matched = true;
                    break;
                }
            }
            if (!matched) Unsupported("missing Phi predecessor");
        }
        // Read every incoming operand before writing any destination. This is
        // required for loop-carried swaps and mutually dependent Phi nodes.
        for (const auto &[destination, value] : copies) Save(destination, value);
    }
    void Resume(const Instruction &i) {
        const auto &site = *sites.at(i.offset);
        auto restored = Op("OpFunctionCall " + site.restored_type + " " + site.restore + " %sc_base");
        for (size_t n = 0; n < site.saved_values.size(); ++n) {
            auto id = site.saved_values[n];
            Save(id, Op("OpCompositeExtract " + Type(Def(id).type) + " " + restored + " " + std::to_string(n)));
        }
        auto result = LoadWord(3);
        result = Op("OpINotEqual " + code.bool_type + " " + result + " " + Constant(0));
        Save(i.result, result);
        StoreWord(1, Constant(0));
        StoreWord(2, Constant(0));
    }
    void Suspend(const Instruction &i, uint32_t resume_state) {
        const auto &site = *sites.at(i.offset);
        std::string spill = "OpFunctionCall " + code.void_type + " " + site.spill + " %sc_base";
        for (auto value : site.saved_values) spill += " " + Value(value);
        Op(spill);
        auto distance = Value(i.words.at(3));
        if (Def(Def(i.words.at(3)).type).op != spv::OpTypeFloat || Def(Def(i.words.at(3)).type).words.at(2) != 32)
            Unsupported("ReportIntersection distance must be float32");
        auto bits = Op("OpBitcast " + code.word_type + " " + distance);
        StoreWord(4, bits);
        StoreWord(5, Value(i.words.at(4)));
        StoreWord(6, Constant(i.offset));
        StoreWord(0, Constant(resume_state));
        StoreWord(2, Constant(0));
        StoreWord(1, Constant(2));
        functions << "OpReturn\n";
    }
    void Generic(const Instruction &i) {
        if (!i.result || !i.type || IsPointer(i.type) || Def(i.type).op == spv::OpTypeVoid || i.op == spv::OpFunctionCall)
            Unsupported("unsupported source operation Op" + std::string(spvOpcodeString(i.op)) + " at word " +
                        std::to_string(i.offset));
        auto result = Fresh();
        std::map<uint32_t, std::string> loaded;
        for (auto id : i.uses)
            if (!loaded.count(id)) loaded[id] = Value(id);
        auto text = Rewrite(i.text, [&](uint32_t id) {
            if (id == i.result) return result;
            if (id == i.type) return Type(id);
            return loaded.at(id);
        });
        functions << text << "\n";
        DecorateResult(i.result, result);
        Save(i.result, result);
    }
    void Switch(const Instruction &i, uint32_t predecessor) {
        const auto &type = Def(Def(i.words.at(1)).type);
        const auto width = type.words.at(2);
        if (width != 32 && width != 64) Unsupported("OpSwitch selector width");
        const auto literal_words = width / 32;
        auto selector = Value(i.words.at(1));
        std::map<uint32_t, std::string> labels;
        labels[i.words.at(2)] = Fresh();
        for (size_t n = 3; n < i.words.size(); n += literal_words + 1) labels.try_emplace(i.words.at(n + literal_words), Fresh());
        auto merge = Fresh();
        functions << "OpSelectionMerge " << merge << " None\nOpSwitch " << selector << " " << labels.at(i.words.at(2));
        for (size_t n = 3; n < i.words.size(); n += literal_words + 1) {
            uint64_t literal = i.words[n];
            if (width == 64) literal |= uint64_t(i.words.at(n + 1)) << 32;
            std::string value = std::to_string(literal);
            if (type.words.at(3))
                value = width == 64 ? std::to_string(static_cast<int64_t>(literal)) : std::to_string(static_cast<int32_t>(literal));
            functions << " " << value << " " << labels.at(i.words.at(n + literal_words));
        }
        functions << "\n";
        for (const auto &[target, label] : labels) {
            functions << label << " = OpLabel\n";
            Edge(predecessor, target);
            functions << "OpStore " << pc_pointer << " " << Constant(first_state.at(target)) << "\nOpBranch " << merge << "\n";
        }
        functions << merge << " = OpLabel\nOpBranch %sc_dispatch_merge\n";
    }
    void Body(const State &state) {
        if (state.resume) Resume(*state.resume);
        for (size_t n = state.begin; n < state.end; ++n) {
            const auto &i = *state.block->body[n];
            if (i.op == spv::OpPhi || i.op == spv::OpSelectionMerge || i.op == spv::OpLoopMerge || i.op == spv::OpLine ||
                i.op == spv::OpNoLine)
                continue;
            if (i.op == spv::OpVariable) {
                if (i.words.size() > 4) {
                    const auto &slot = *slots.at(i.result);
                    Op("OpFunctionCall " + code.void_type + " " + codecs.at(slot.type).store + " %sc_base " +
                       Constant(static_cast<uint32_t>(slot.offset)) + " " + Value(i.words.at(4)));
                }
                continue;
            }
            if (i.op == spv::OpAccessChain || i.op == spv::OpInBoundsAccessChain ||
                (i.op == spv::OpCopyObject && IsPointer(i.type)))
                continue;
            if (i.op == spv::OpReportIntersectionKHR) {
                Suspend(i, state.number + 1);
                return;
            }
            if (i.op == spv::OpLoad && Storage(i.words.at(3)) == spv::StorageClassFunction) {
                if (i.words.size() != 4) Unsupported("persistent local load memory operands");
                auto address = Offset(i.words.at(3));
                Save(i.result, Op("OpFunctionCall " + Type(i.type) + " " + codecs.at(i.type).load + " %sc_base " + address));
            } else if (i.op == spv::OpStore) {
                if (Storage(i.words.at(1)) == spv::StorageClassFunction) {
                    if (i.words.size() != 3) Unsupported("persistent local store memory operands");
                    auto type = Def(Def(i.words.at(1)).type).words.at(3);
                    auto address = Offset(i.words.at(1));
                    Op("OpFunctionCall " + code.void_type + " " + codecs.at(type).store + " %sc_base " + address + " " +
                       Value(i.words.at(2)));
                } else {
                    auto text = Rewrite(i.text, [&](uint32_t id) { return Value(id); });
                    functions << text << "\n";
                }
            } else if (i.op == spv::OpBranch) {
                Edge(state.block->label, i.words.at(1));
                functions << "OpStore " << pc_pointer << " " << Constant(first_state.at(i.words.at(1)))
                          << "\nOpBranch %sc_dispatch_merge\n";
                return;
            } else if (i.op == spv::OpBranchConditional) {
                auto condition = Value(i.words.at(1));
                // Assign only the selected edge. A source value on the other
                // edge need not be initialized on this predecessor path.
                auto yes = Fresh(), no = Fresh(), merge = Fresh();
                functions << "OpSelectionMerge " << merge << " None\nOpBranchConditional " << condition << " " << yes << " " << no
                          << "\n";
                functions << yes << " = OpLabel\n";
                Edge(state.block->label, i.words.at(2));
                functions << "OpStore " << pc_pointer << " " << Constant(first_state.at(i.words.at(2))) << "\nOpBranch " << merge
                          << "\n";
                functions << no << " = OpLabel\n";
                Edge(state.block->label, i.words.at(3));
                functions << "OpStore " << pc_pointer << " " << Constant(first_state.at(i.words.at(3))) << "\nOpBranch " << merge
                          << "\n";
                functions << merge << " = OpLabel\nOpBranch %sc_dispatch_merge\n";
                return;
            } else if (i.op == spv::OpReturn) {
                StoreWord(0, Constant(UINT32_MAX));
                functions << "OpReturn\n";
                return;
            } else if (i.op == spv::OpSwitch) {
                Switch(i, state.block->label);
                return;
            } else if (i.op == spv::OpUnreachable) {
                functions << "OpUnreachable\n";
                return;
            } else
                Generic(i);
        }
        Unsupported("source state has no terminator");
    }
    void EmitStep() {
        declarations << "%sc_step_sig = OpTypeFunction " << code.void_type << " " << code.word_type << "\n";
        functions << "%sc_step = OpFunction " << code.void_type << " None %sc_step_sig\n%sc_base = OpFunctionParameter "
                  << code.word_type << "\n%sc_entry = OpLabel\n";
        pc_pointer = "%sc_pc";
        auto pc_type = PointerType(spv::StorageClassFunction, code.word_type);
        functions << pc_pointer << " = OpVariable " << pc_type << " Function\n";
        for (const auto &i : parsed.instructions) {
            if (!i.in_function || !i.result || !i.type || IsPointer(i.type) || Def(i.type).op == spv::OpTypeVoid) continue;
            auto pointer = PointerType(spv::StorageClassFunction, Type(i.type));
            functions << Register(i.result) << " = OpVariable " << pointer << " Function\n";
        }
        auto pending = LoadWord(1), ready = LoadWord(2);
        auto active = Op("OpINotEqual " + code.bool_type + " " + pending + " " + Constant(0));
        auto waiting = Op("OpIEqual " + code.bool_type + " " + ready + " " + Constant(0));
        auto blocked = Op("OpLogicalAnd " + code.bool_type + " " + active + " " + waiting);
        functions << "OpSelectionMerge %sc_start None\nOpBranchConditional " << blocked << " %sc_blocked %sc_start\n"
                  << "%sc_blocked = OpLabel\nOpReturn\n%sc_start = OpLabel\n";
        auto pc = LoadWord(0);
        functions << "OpStore " << pc_pointer << " " << pc << "\nOpBranch %sc_loop\n%sc_loop = OpLabel\n"
                  << "OpLoopMerge %sc_loop_exit %sc_continue None\nOpBranch %sc_dispatch\n%sc_dispatch = OpLabel\n";
        pc = Op("OpLoad " + code.word_type + " " + pc_pointer);
        functions << "OpSelectionMerge %sc_dispatch_merge None\nOpSwitch " << pc << " %sc_terminal";
        for (const auto &state : states) functions << " " << state.number << " %sc_state" << state.number;
        functions << "\n";
        for (const auto &state : states) {
            functions << "%sc_state" << state.number << " = OpLabel\n";
            Body(state);
        }
        functions << "%sc_terminal = OpLabel\nOpReturn\n%sc_dispatch_merge = OpLabel\nOpBranch %sc_continue\n"
                  << "%sc_continue = OpLabel\nOpBranch %sc_loop\n%sc_loop_exit = OpLabel\nOpUnreachable\nOpFunctionEnd\n";
    }
    void EmitKernel() {
        if (gid_type.empty()) {
            for (const auto &[id, codec] : codecs) {
                const auto &d = Def(id);
                if (d.op == spv::OpTypeVector && d.words.at(3) == 3 && Type(d.words.at(2)) == code.word_type)
                    gid_type = codec.spirv_type;
            }
            if (gid_type.empty()) {
                gid_type = "%sc_gid_type";
                declarations << gid_type << " = OpTypeVector " << code.word_type << " 3\n";
            }
        }
        auto pointer = PointerType(spv::StorageClassInput, gid_type);
        if (gid.empty()) {
            gid = "%sc_gid";
            declarations << gid << " = OpVariable " << pointer << " Input\n";
            annotations << "OpDecorate " << gid << " BuiltIn GlobalInvocationId\n";
            interfaces.push_back(gid);
        }
        if (group_count.empty()) {
            group_count = "%sc_groups";
            declarations << group_count << " = OpVariable " << pointer << " Input\n";
            annotations << "OpDecorate " << group_count << " BuiltIn NumWorkgroups\n";
            interfaces.push_back(group_count);
        }
        declarations << "%sc_main_sig = OpTypeFunction " << code.void_type << "\n";
        declarations << "%sc_product = OpTypeStruct " << code.word_type << " " << code.word_type << "\n";
        functions << "%sc_main = OpFunction " << code.void_type << " None %sc_main_sig\n%sc_main_entry = OpLabel\n";
        auto id = Op("OpLoad " + gid_type + " " + gid), groups = Op("OpLoad " + gid_type + " " + group_count);
        auto x = Op("OpCompositeExtract " + code.word_type + " " + id + " 0");
        auto y = Op("OpCompositeExtract " + code.word_type + " " + id + " 1");
        auto z = Op("OpCompositeExtract " + code.word_type + " " + id + " 2");
        auto width = Op("OpCompositeExtract " + code.word_type + " " + groups + " 0");
        auto height = Op("OpCompositeExtract " + code.word_type + " " + groups + " 1");
        auto product = Op("OpUMulExtended %sc_product " + height + " " + z);
        auto yz_low = Op("OpCompositeExtract " + code.word_type + " " + product + " 0");
        auto yz_high = Op("OpCompositeExtract " + code.word_type + " " + product + " 1");
        auto overflow = Op("OpINotEqual " + code.bool_type + " " + yz_high + " " + Constant(0));
        auto yz = Op("OpIAdd " + code.word_type + " " + y + " " + yz_low);
        auto addition_overflow = Op("OpULessThan " + code.bool_type + " " + yz + " " + yz_low);
        overflow = Op("OpLogicalOr " + code.bool_type + " " + overflow + " " + addition_overflow);
        product = Op("OpUMulExtended %sc_product " + width + " " + yz);
        auto index_low = Op("OpCompositeExtract " + code.word_type + " " + product + " 0");
        auto index_high = Op("OpCompositeExtract " + code.word_type + " " + product + " 1");
        auto multiply_overflow = Op("OpINotEqual " + code.bool_type + " " + index_high + " " + Constant(0));
        overflow = Op("OpLogicalOr " + code.bool_type + " " + overflow + " " + multiply_overflow);
        auto index = Op("OpIAdd " + code.word_type + " " + x + " " + index_low);
        addition_overflow = Op("OpULessThan " + code.bool_type + " " + index + " " + index_low);
        overflow = Op("OpLogicalOr " + code.bool_type + " " + overflow + " " + addition_overflow);
        auto capacity = Op("OpArrayLength " + code.word_type + " " + code.memory + " 0");
        capacity = Op("OpUDiv " + code.word_type + " " + capacity + " " + Constant(output.frame_stride_words));
        auto outside = Op("OpUGreaterThanEqual " + code.bool_type + " " + index + " " + capacity);
        overflow = Op("OpLogicalOr " + code.bool_type + " " + overflow + " " + outside);
        outside = Op("OpUGreaterThanEqual " + code.bool_type + " " + index + " " + Constant(output.max_invocations));
        overflow = Op("OpLogicalOr " + code.bool_type + " " + overflow + " " + outside);
        functions << "OpSelectionMerge %sc_kernel_run None\nOpBranchConditional " << overflow << " %sc_kernel_skip %sc_kernel_run\n"
                  << "%sc_kernel_skip = OpLabel\nOpReturn\n%sc_kernel_run = OpLabel\n";
        auto base = Op("OpIMul " + code.word_type + " " + index + " " + Constant(output.frame_stride_words));
        Op("OpFunctionCall " + code.void_type + " %sc_step " + base);
        functions << "OpReturn\nOpFunctionEnd\n";
    }
};
}  // namespace

TypedContinuationFrameBounds PlanTypedContinuationFrameBounds(uint64_t payload_bytes) {
    if (payload_bytes > UINT32_MAX - 28u) Unsupported("state frame/control exceeds 32-bit byte indexing");
    uint64_t stride = (payload_bytes + 3) / 4 + 7;
    if (stride * 4 > UINT32_MAX) Unsupported("state frame/control exceeds 32-bit byte indexing");
    auto words = static_cast<uint32_t>(stride);
    return {words, UINT32_MAX / words};
}

TypedContinuationProgram CompileTypedContinuationSteps(const std::vector<uint32_t> &source, const std::string &entry,
                                                       uint32_t execution_model, uint32_t frame_descriptor_set,
                                                       uint32_t frame_binding) {
    // Validate before optimization; entry selection is validated again by the
    // continuation analysis after inlining. Inlining preserves source ALU/CFG
    // semantics and removes the need to pretend helper returns are RT returns.
    const auto selected_entry = AnalyzeContinuations(source, entry, execution_model).entry;
    std::vector<uint32_t> selected(source.begin(), source.begin() + 5);
    for (size_t p = 5; p < source.size(); p += source[p] >> 16) {
        auto op = static_cast<spv::Op>(source[p] & 0xffff);
        if (op == spv::OpEntryPoint && source.at(p + 2) != selected_entry) continue;
        if ((op == spv::OpExecutionMode || op == spv::OpExecutionModeId) && source.at(p + 1) != selected_entry) continue;
        selected.insert(selected.end(), source.begin() + p, source.begin() + p + (source[p] >> 16));
    }
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
    std::string error;
    optimizer.SetMessageConsumer(
        [&](spv_message_level_t, const char *, const spv_position_t &, const char *message) { error += message; });
    optimizer.RegisterPass(spvtools::CreateInlineExhaustivePass());
    optimizer.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
    optimizer.RegisterPass(spvtools::CreateFlattenDecorationPass());
    std::vector<uint32_t> inlined;
    if (!optimizer.Run(selected.data(), selected.size(), &inlined)) Unsupported("helper inlining failed: " + error);
    return Compiler(std::move(inlined), entry, execution_model, frame_descriptor_set, frame_binding).Run();
}
}  // namespace vel::rt
