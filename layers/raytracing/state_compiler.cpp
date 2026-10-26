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
    Compiler(std::vector<uint32_t> source, std::string entry, uint32_t model, TypedContinuationCompileOptions config)
        : entry(std::move(entry)),
          model(model),
          set(config.frame_descriptor_set),
          binding(config.frame_binding),
          config(std::move(config)) {
        output.source = std::move(source);
        output.transport = this->config.transport;
        output.step_export_name = this->config.step_export_name;
        if (!this->config.emit_compute_entry && (!BDA() || this->config.step_export_name.empty()))
            Unsupported("stage libraries require BDA transport and a nonempty export name");
        if (!this->config.step_export_name.empty() &&
            !std::all_of(this->config.step_export_name.begin(), this->config.step_export_name.end(),
                         [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
            Unsupported("invalid stage export name");
        if (BDA() && this->config.context_root_low_spec_id == this->config.context_root_high_spec_id)
            Unsupported("BDA context root specialization IDs must differ");
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
        for (const auto &[id, d] : definitions) {
            if (d->op == spv::OpTypeInt && d->words.at(2) == 32 && d->words.at(3) == 0) {
                NeedType(id);
                break;
            }
        }
        for (const auto &i : parsed.instructions) {
            if (i.op == spv::OpEntryPoint) {
                // Selected-entry invocation interfaces are semantic state even
                // when no source instruction reads them (notably attributes).
                for (auto id : i.uses) {
                    const auto &d = Def(id);
                    if (d.op == spv::OpVariable &&
                        (IsInvocationStorage(d.words.at(3)) || (BDA() && d.words.at(3) == spv::StorageClassInput)))
                        NeedGlobal(id);
                }
            }
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
                i.op != spv::OpInBoundsAccessChain && i.op != spv::OpCopyObject && i.op != spv::OpImageTexelPointer)
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
            if (i.op == spv::OpDecorate && i.words.at(2) == spv::DecorationVolatile) {
                const auto &target = Def(i.words.at(1));
                if (target.in_function || (target.op == spv::OpVariable && IsInvocationStorage(target.words.at(3))))
                    Unsupported("volatile persistent storage requires volatile frame codecs");
            }
        }
        std::map<uint32_t, uint32_t> resource_sets, resource_bindings;
        for (const auto &i : parsed.instructions) {
            if (i.op != spv::OpDecorate || !globals.count(i.words.at(1))) continue;
            if (i.words.at(2) == spv::DecorationDescriptorSet) resource_sets[i.words.at(1)] = i.words.at(3);
            if (i.words.at(2) == spv::DecorationBinding) resource_bindings[i.words.at(1)] = i.words.at(3);
        }
        for (const auto &[resource, resource_set] : resource_sets) {
            auto source_binding = resource_bindings.find(resource);
            if (!BDA() && source_binding != resource_bindings.end() && resource_set == set && source_binding->second == binding)
                Unsupported("frame descriptor binding collides with a live source resource");
        }
        std::vector<uint32_t> types(value_types.begin(), value_types.end());
        ContinuationFrameOptions options{true, true};
        options.physical_frame_addresses = BDA();
        options.private_frame_words = config.private_frame_words;
        if (config.transport == TypedFrameTransport::PrivateArray) {
            if (!config.private_frame_words || config.private_frame_words > UINT32_MAX / 4u || config.emit_compute_entry)
                Unsupported("PrivateArray requires a library and a nonzero checked word budget");
            output.private_frame_words = config.private_frame_words;
        } else if (config.private_frame_words) {
            Unsupported("private word budget requires PrivateArray transport");
        }
        options.prefix_bytes = BDA() ? VEL_RT_SYSTEM_BYTES : 0;
        output.system_header_bytes = options.prefix_bytes;
        const auto preview = PlanContinuationFrames(output.source, this->entry, model, types, options);
        uint64_t persistent_size = 0;
        for (const auto &f : preview.functions) {
            if (f.function != preview.analysis.entry) continue;
            persistent_size = f.size;
        }
        PlanTypedContinuationFrameBounds(persistent_size);
        for (auto id : globals) {
            const auto &variable = Def(id);
            if (variable.op != spv::OpVariable) continue;
            if (IsInvocationStorage(variable.words.at(3))) {
                if (variable.words.size() > 4)
                    Unsupported("invocation variable initializers require explicit stage initialization");
                auto pointee = Def(variable.type).words.at(3);
                auto layout =
                    std::find_if(preview.types.begin(), preview.types.end(), [&](const auto &v) { return v.type == pointee; });
                if (layout == preview.types.end()) Unsupported("invocation storage has no concrete typed layout");
                persistent_size = (persistent_size + layout->alignment - 1) & ~uint64_t(layout->alignment - 1);
                uint32_t location = UINT32_MAX;
                for (const auto &decoration : parsed.instructions)
                    if (decoration.op == spv::OpDecorate && decoration.words.at(1) == id &&
                        decoration.words.at(2) == spv::DecorationLocation)
                        location = decoration.words.at(3);
                output.invocation_variables.push_back(
                    {id, pointee, variable.words.at(3), location, persistent_size, layout->size, layout->alignment});
                if (layout->size > UINT32_MAX || persistent_size > UINT32_MAX - layout->size)
                    Unsupported("invocation storage exceeds 32-bit byte indexing");
                persistent_size += layout->size;
            } else if (variable.words.at(3) == spv::StorageClassUniformConstant && IsASDescriptor(Def(variable.type).words.at(3))) {
                if (!resource_sets.count(id) || !resource_bindings.count(id)) Unsupported("AS descriptor is missing set/binding");
                output.acceleration_structure_descriptors.push_back({id, resource_sets.at(id), resource_bindings.at(id)});
            }
        }
        const auto bounds = PlanTypedContinuationFrameBounds(persistent_size);
        output.frame_stride_words = bounds.stride_words;
        output.max_invocations = bounds.max_invocations;
        output.control_word = static_cast<uint32_t>((persistent_size + 3) / 4);
        output.control_byte_offset = output.control_word * 4;
        output.frame_stride_bytes = uint64_t(output.frame_stride_words) * 4;
        if (config.transport == TypedFrameTransport::PrivateArray &&
            output.frame_stride_bytes > uint64_t(config.private_frame_words) * 4)
            Unsupported("source frame exceeds the PrivateArray word budget");
        output.max_invocations64 = BDA() ? UINT64_MAX / output.frame_stride_bytes : output.max_invocations;
        for (const auto &variable : output.invocation_variables) invocation_variables[variable.variable] = &variable;
        code = EmitContinuationFrameCodecs(output.source, this->entry, model, set, binding, "sc_cf_", types, options);
        // Reuse the component's existing word pointer declaration rather than
        // emit an illegal duplicate non-aggregate SPIR-V pointer type.
        pointer_types[std::to_string(config.transport == TypedFrameTransport::PrivateArray ? spv::StorageClassPrivate
                                     : BDA()                                               ? spv::StorageClassPhysicalStorageBuffer
                                                                                           : spv::StorageClassStorageBuffer) +
                      " " + code.word_type] = code.word_pointer_type;
        output.frames = code.plan;
        for (const auto &c : code.types) codecs[c.type] = c;
        for (const auto &f : code.plan.functions) {
            if (f.function == code.plan.analysis.entry) frame = &f;
        }
        if (!frame) Unsupported("entry frame missing");
        for (const auto &slot : frame->slots) slots[slot.value] = &slot;
        for (const auto &site : code.suspensions) sites[site.word_offset] = &site;
        for (const auto &[id, c] : codecs) {
            const auto &d = Def(id);
            if (d.op == spv::OpTypeInt && d.words.at(2) == 32 && d.words.at(3) == 0) word_codec = &c;
        }
        if (!word_codec) Unsupported("missing uint32 request/control codec");
        if (BDA()) CollectBuiltins();
        BuildStates();
    }

    TypedContinuationProgram Run() {
        EmitGlobals();
        EmitStep();
        if (config.emit_compute_entry)
            EmitKernel();
        else {
            EnsureCapability("Linkage");
            annotations << "OpDecorate %sc_step LinkageAttributes \"" << config.step_export_name << "\" Export\n";
        }
        std::string assembly = code.capabilities + source_caps.str() + extensions.str() + imports.str() +
                               (BDA() ? "OpMemoryModel PhysicalStorageBuffer64 GLSL450\n" : "OpMemoryModel Logical GLSL450\n");
        if (config.emit_compute_entry) {
            assembly += "OpEntryPoint GLCompute %sc_main \"main\" " + code.memory;
            for (const auto &global : interfaces) assembly += " " + global;
            assembly += "\nOpExecutionMode %sc_main LocalSize 1 1 1\n" + execution_modes.str();
        }
        assembly +=
            code.annotations + annotations.str() + code.declarations + declarations.str() + code.functions + functions.str();
        spvtools::SpirvTools tools(config.emit_compute_entry ? SPV_ENV_VULKAN_1_3 : SPV_ENV_UNIVERSAL_1_5);
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
    TypedContinuationCompileOptions config;
    ContinuationFrameCodecs code;
    const FunctionFrame *frame = nullptr;
    const ContinuationTypeCodec *word_codec = nullptr;
    std::vector<Block> blocks;
    std::vector<State> states;
    std::map<uint32_t, const Instruction *> definitions;
    std::set<uint32_t> globals, needed_types, value_types;
    std::map<uint32_t, ContinuationTypeCodec> codecs;
    std::map<uint32_t, const ContinuationFrameSlot *> slots;
    std::map<uint32_t, const TypedInvocationVariable *> invocation_variables;
    std::map<uint32_t, const TypedBuiltinVariable *> builtin_variables;
    std::map<uint32_t, const ContinuationSuspensionCodec *> sites;
    std::map<uint32_t, uint32_t> first_state;
    std::map<uint32_t, std::string> cloned_types;
    std::map<uint32_t, std::string> resource_types;
    std::map<std::string, std::string> pointer_types;
    std::map<uint32_t, std::string> descriptor_types;
    std::map<uint32_t, std::string> constants;
    std::map<uint32_t, std::string> ephemeral_values;
    std::map<uint32_t, std::string> ephemeral_pointers;
    std::set<uint64_t> wide_constants;
    std::vector<std::string> interfaces;
    std::ostringstream source_caps, imports, extensions, execution_modes, annotations, declarations, functions;
    std::string gid, gid_type, group_count, pc_pointer;
    bool BDA() const { return config.transport != TypedFrameTransport::StorageBuffer; }
    void CollectBuiltins() {
        for (auto id : globals) {
            const auto &variable = Def(id);
            if (variable.op != spv::OpVariable || variable.words.at(3) != spv::StorageClassInput) continue;
            uint32_t builtin = UINT32_MAX;
            for (const auto &i : parsed.instructions)
                if (i.op == spv::OpDecorate && i.words.at(1) == id && i.words.at(2) == spv::DecorationBuiltIn)
                    builtin = i.words.at(3);
            uint32_t offset = 0, size = 0;
            switch (builtin) {
                case spv::BuiltInLaunchIdKHR:
                    offset = VEL_RT_SYS_LAUNCH_ID;
                    size = 12;
                    break;
                case spv::BuiltInLaunchSizeKHR:
                    offset = VEL_RT_SYS_LAUNCH_SIZE;
                    size = 12;
                    break;
                case spv::BuiltInIncomingRayFlagsKHR:
                    offset = VEL_RT_SYS_RAY_FLAGS;
                    size = 4;
                    break;
                case spv::BuiltInWorldRayOriginKHR:
                    offset = VEL_RT_SYS_WORLD_ORIGIN;
                    size = 12;
                    break;
                case spv::BuiltInWorldRayDirectionKHR:
                    offset = VEL_RT_SYS_WORLD_DIRECTION;
                    size = 12;
                    break;
                case spv::BuiltInObjectRayOriginKHR:
                    offset = VEL_RT_SYS_OBJECT_ORIGIN;
                    size = 12;
                    break;
                case spv::BuiltInObjectRayDirectionKHR:
                    offset = VEL_RT_SYS_OBJECT_DIRECTION;
                    size = 12;
                    break;
                case spv::BuiltInRayTminKHR:
                    offset = VEL_RT_SYS_TMIN;
                    size = 4;
                    break;
                case spv::BuiltInRayTmaxKHR:
                    offset = VEL_RT_SYS_TMAX;
                    size = 4;
                    break;
                case spv::BuiltInInstanceCustomIndexKHR:
                    offset = VEL_RT_SYS_INSTANCE_CUSTOM_INDEX;
                    size = 4;
                    break;
                case spv::BuiltInInstanceId:
                    offset = VEL_RT_SYS_INSTANCE_ID;
                    size = 4;
                    break;
                case spv::BuiltInRayGeometryIndexKHR:
                    offset = VEL_RT_SYS_GEOMETRY_INDEX;
                    size = 4;
                    break;
                case spv::BuiltInPrimitiveId:
                    offset = VEL_RT_SYS_PRIMITIVE_ID;
                    size = 4;
                    break;
                case spv::BuiltInHitKindKHR:
                    offset = VEL_RT_SYS_HIT_KIND;
                    size = 4;
                    break;
                case spv::BuiltInObjectToWorldKHR:
                    offset = VEL_RT_SYS_OBJECT_TO_WORLD;
                    size = 48;
                    break;
                case spv::BuiltInWorldToObjectKHR:
                    offset = VEL_RT_SYS_WORLD_TO_OBJECT;
                    size = 48;
                    break;
                case spv::BuiltInCullMaskKHR:
                    offset = VEL_RT_SYS_CULL_MASK;
                    size = 4;
                    break;
                default:
                    Unsupported("input builtin requires additional system-frame ABI support: " + std::to_string(builtin));
            }
            auto type = Def(variable.type).words.at(3);
            if (Layout(type).size != size) Unsupported("builtin system-frame type size mismatch");
            output.builtin_variables.push_back({id, type, builtin, offset, size});
        }
        for (const auto &variable : output.builtin_variables) builtin_variables[variable.variable] = &variable;
    }
    uint32_t PointerRoot(uint32_t id) const {
        const auto &d = Def(id);
        if (d.op == spv::OpVariable) return id;
        if (d.op == spv::OpAccessChain || d.op == spv::OpInBoundsAccessChain || d.op == spv::OpCopyObject)
            return PointerRoot(d.words.at(3));
        Unsupported("unsupported pointer root provenance");
    }
    bool SystemPointer(uint32_t id) const {
        // Only Input pointers can name a system builtin. Native resource
        // pointers (notably ImageTexelPointer) have independent provenance.
        return BDA() && Storage(id) == spv::StorageClassInput && builtin_variables.count(PointerRoot(id)) != 0;
    }
    bool PhysicalResourcePointer(uint32_t id) const {
        auto storage = Storage(id);
        return BDA() && (storage == spv::StorageClassPushConstant || storage == spv::StorageClassShaderRecordBufferKHR);
    }
    static bool IsInvocationStorage(uint32_t storage) {
        return storage == spv::StorageClassRayPayloadKHR || storage == spv::StorageClassIncomingRayPayloadKHR ||
               storage == spv::StorageClassCallableDataKHR || storage == spv::StorageClassIncomingCallableDataKHR ||
               storage == spv::StorageClassHitAttributeKHR;
    }
    static bool IsPersistentStorage(uint32_t storage) {
        return storage == spv::StorageClassFunction || IsInvocationStorage(storage);
    }
    bool IsASDescriptor(uint32_t type) const {
        const auto &d = Def(type);
        if (d.op == spv::OpTypeAccelerationStructureKHR) return true;
        return (d.op == spv::OpTypeArray || d.op == spv::OpTypeRuntimeArray) && IsASDescriptor(d.words.at(2));
    }

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
    std::string WideConstant(uint64_t value) {
        auto key = "%sc_w" + std::to_string(value);
        if (wide_constants.insert(value).second) declarations << key << " = OpConstant " << code.base_type << " " << value << "\n";
        return key;
    }
    bool Layoutable(uint32_t id) const {
        const auto &d = Def(id);
        if (d.op == spv::OpTypeBool || d.op == spv::OpTypeInt || d.op == spv::OpTypeFloat ||
            d.op == spv::OpTypeAccelerationStructureKHR)
            return true;
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
            if ((v.op >= spv::OpTypeVoid && v.op <= spv::OpTypeForwardPointer) || v.op == spv::OpTypeAccelerationStructureKHR)
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
        if (IsInvocationStorage(storage)) storage = spv::StorageClassFunction;
        auto key = std::to_string(storage) + " " + pointee;
        auto found = pointer_types.find(key);
        if (found != pointer_types.end()) return found->second;
        auto id = Fresh();
        const char *name = storage == spv::StorageClassFunction                ? "Function"
                           : storage == spv::StorageClassInput                 ? "Input"
                           : storage == spv::StorageClassPushConstant          ? "PushConstant"
                           : storage == spv::StorageClassStorageBuffer         ? "StorageBuffer"
                           : storage == spv::StorageClassPrivate               ? "Private"
                           : storage == spv::StorageClassUniformConstant       ? "UniformConstant"
                           : storage == spv::StorageClassUniform               ? "Uniform"
                           : storage == spv::StorageClassImage                 ? "Image"
                           : storage == spv::StorageClassPhysicalStorageBuffer ? "PhysicalStorageBuffer"
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
        // Runtime arrays and their enclosing blocks have no logical frame
        // codec. Reuse their one resource declaration, including its Block
        // decoration, rather than emitting an invalid unadorned duplicate.
        if (d.op == spv::OpTypeStruct || d.op == spv::OpTypeArray || d.op == spv::OpTypeRuntimeArray) return ResourceType(id);
        auto found = cloned_types.find(id);
        if (found != cloned_types.end()) return found->second;
        if (d.op == spv::OpTypePointer) {
            if (d.words.at(2) == spv::StorageClassUniformConstant && IsASDescriptor(d.words.at(3)))
                return PointerType(spv::StorageClassStorageBuffer, DescriptorType(d.words.at(3)));
            if (BDA() &&
                (d.words.at(2) == spv::StorageClassPushConstant || d.words.at(2) == spv::StorageClassShaderRecordBufferKHR))
                return PointerType(spv::StorageClassPhysicalStorageBuffer, ResourceType(d.words.at(3)));
            const auto storage = d.words.at(2);
            const bool resource = storage == spv::StorageClassStorageBuffer || storage == spv::StorageClassUniform ||
                                  storage == spv::StorageClassPushConstant || storage == spv::StorageClassShaderRecordBufferKHR;
            return PointerType(storage, resource ? ResourceType(d.words.at(3)) : Type(d.words.at(3)));
        }
        if (d.op != spv::OpTypeStruct && d.op != spv::OpTypeRuntimeArray && d.op != spv::OpTypeArray && d.op != spv::OpTypeImage &&
            d.op != spv::OpTypeSampler && d.op != spv::OpTypeSampledImage)
            Unsupported("opaque or unsupported source type " + std::to_string(id));
        for (auto child : d.uses) {
            const auto &v = Def(child);
            if (v.op >= spv::OpTypeVoid && v.op <= spv::OpTypeForwardPointer) Type(child);
        }
        auto name = SourceID(id);
        declarations << Rewrite(d.text, [&](uint32_t child) { return child == id ? name : Global(child); }) << "\n";
        cloned_types[id] = name;
        return name;
    }
    // A source composite can be both an explicitly laid-out resource pointee
    // and an ordinary SSA/Function value. Their nominal types must differ:
    // Vulkan forbids Offset/ArrayStride/Block-decorated composites in Function
    // storage. Frame codecs deliberately own undecorated logical value types.
    std::string ResourceType(uint32_t id) {
        const auto &d = Def(id);
        if (d.op != spv::OpTypeStruct && d.op != spv::OpTypeArray && d.op != spv::OpTypeRuntimeArray) return Type(id);
        auto found = resource_types.find(id);
        if (found != resource_types.end()) return found->second;
        const auto name = "%sc_resource" + std::to_string(id);
        std::vector<std::string> children;
        if (d.op == spv::OpTypeStruct) {
            for (size_t member = 2; member < d.words.size(); ++member) children.push_back(ResourceType(d.words[member]));
            declarations << name << " = OpTypeStruct";
            for (const auto &child : children) declarations << " " << child;
        } else {
            const auto element = ResourceType(d.words.at(2));
            if (d.op == spv::OpTypeArray) {
                const auto &length = Def(d.words.at(3));
                if (length.op != spv::OpConstant || length.words.size() < 4)
                    Unsupported("resource array length requires specialization first");
                uint64_t count = length.words.at(3);
                if (length.words.size() > 4) count |= uint64_t(length.words.at(4)) << 32;
                if (!count || count > UINT32_MAX) Unsupported("resource array length exceeds uint32");
                auto codec = codecs.find(id);
                const auto size = codec != codecs.end() ? codec->second.array_length : Constant(static_cast<uint32_t>(count));
                if (size.empty()) Unsupported("missing logical array length ID");
                declarations << name << " = OpTypeArray " << element << " " << size;
            } else
                declarations << name << " = OpTypeRuntimeArray " << element;
        }
        declarations << "\n";
        resource_types[id] = name;
        return name;
    }
    void EnsureCapability(const std::string &capability) {
        auto line = "OpCapability " + capability + "\n";
        if (source_caps.str().find(line) == std::string::npos && code.capabilities.find(line) == std::string::npos)
            source_caps << line;
    }
    std::string DescriptorType(uint32_t type) {
        auto known = descriptor_types.find(type);
        if (known != descriptor_types.end()) return known->second;
        const auto &d = Def(type);
        auto result = "%sc_descriptor" + std::to_string(type);
        if (d.op == spv::OpTypeAccelerationStructureKHR) {
            declarations << result << " = OpTypeStruct " << Type(type) << "\n";
            annotations << "OpDecorate " << result << " Block\nOpMemberDecorate " << result << " 0 Offset 0\n";
        } else if (d.op == spv::OpTypeArray || d.op == spv::OpTypeRuntimeArray) {
            auto element = DescriptorType(d.words.at(2));
            if (d.op == spv::OpTypeArray) {
                const auto &length = Def(d.words.at(3));
                if (length.op != spv::OpConstant) Unsupported("AS descriptor array requires specialization first");
                uint64_t count = length.words.at(3);
                if (length.words.size() > 4) count |= uint64_t(length.words.at(4)) << 32;
                if (!count || count > UINT32_MAX) Unsupported("AS descriptor array length exceeds uint32");
                auto size = Constant(static_cast<uint32_t>(count));
                declarations << result << " = OpTypeArray " << element << " " << size << "\n";
            } else
                declarations << result << " = OpTypeRuntimeArray " << element << "\n";
            EnsureCapability("StorageBufferArrayDynamicIndexing");
            if (std::any_of(parsed.instructions.begin(), parsed.instructions.end(),
                            [](const auto &i) { return i.op == spv::OpDecorate && i.words.at(2) == spv::DecorationNonUniform; })) {
                EnsureCapability("ShaderNonUniform");
                EnsureCapability("StorageBufferArrayNonUniformIndexing");
            }
        } else
            Unsupported("unsupported AS descriptor pointee");
        descriptor_types[type] = result;
        return result;
    }
    std::string Global(uint32_t id) {
        const auto &d = Def(id);
        if ((d.op >= spv::OpTypeVoid && d.op <= spv::OpTypeForwardPointer) || d.op == spv::OpTypeAccelerationStructureKHR)
            return Type(id);
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
                if (!config.emit_compute_entry) {
                    if (i.op == spv::OpExecutionModeId)
                        Unsupported("stage-library execution-mode IDs require specialization/reconciliation");
                    output.required_execution_modes.push_back({mode, {i.words.begin() + 3, i.words.end()}});
                } else
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
                if (IsInvocationStorage(i.words.at(3))) continue;
                if (BDA() && i.words.at(3) == spv::StorageClassInput) continue;
                if (BDA() && i.words.at(3) == spv::StorageClassPushConstant) {
                    output.uses_app_push_constants = true;
                    continue;
                }
                if (BDA() && i.words.at(3) == spv::StorageClassShaderRecordBufferKHR) {
                    output.uses_shader_record = true;
                    continue;
                }
                if (i.words.at(3) == spv::StorageClassUniformConstant && IsASDescriptor(Def(i.type).words.at(3))) {
                    declarations << SourceID(i.result) << " = OpVariable " << Type(i.type) << " StorageBuffer\n";
                    interfaces.push_back(SourceID(i.result));
                    continue;
                }
                if (i.words.at(3) != spv::StorageClassInput && i.words.at(3) != spv::StorageClassPushConstant &&
                    i.words.at(3) != spv::StorageClassStorageBuffer && i.words.at(3) != spv::StorageClassUniformConstant &&
                    i.words.at(3) != spv::StorageClassUniform)
                    Unsupported("module variable storage requires invocation backing");
                declarations << Rewrite(i.text, GlobalMapper()) << "\n";
                interfaces.push_back(SourceID(i.result));
            }
        }
        for (const auto &i : parsed.instructions) {
            if (i.op != spv::OpDecorate && i.op != spv::OpMemberDecorate) continue;
            auto target = i.words.at(1);
            if (!globals.count(target) && !needed_types.count(target)) continue;
            if (invocation_variables.count(target)) continue;
            if (BDA() && globals.count(target) && Def(target).op == spv::OpVariable &&
                (Def(target).words.at(3) == spv::StorageClassInput || Def(target).words.at(3) == spv::StorageClassPushConstant ||
                 Def(target).words.at(3) == spv::StorageClassShaderRecordBufferKHR))
                continue;
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
                annotations << Rewrite(i.text, [&](uint32_t id) {
                    return id == target && (Def(id).op == spv::OpTypeStruct || Def(id).op == spv::OpTypeArray ||
                                            Def(id).op == spv::OpTypeRuntimeArray)
                               ? ResourceType(id)
                               : Global(id);
                }) << "\n";
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
                if (op != spv::OpReportIntersectionKHR && op != spv::OpTraceRayKHR && op != spv::OpExecuteCallableKHR) continue;
                current.end = n + 1;
                states.push_back(current);
                current = {&block, n + 1, block.body.size(), static_cast<uint32_t>(states.size()), block.body[n]};
            }
            states.push_back(current);
        }
    }
    bool IsPointer(uint32_t type) const { return type && Def(type).op == spv::OpTypePointer; }
    bool Ephemeral(uint32_t type) const {
        if (!type) return false;
        auto op = Def(type).op;
        return op == spv::OpTypeImage || op == spv::OpTypeSampler || op == spv::OpTypeSampledImage;
    }
    std::string Value(uint32_t id) {
        const auto &d = Def(id);
        if (IsPointer(d.type)) return RealPointer(id);
        if (!d.in_function) return Global(id);
        if (!d.result || !d.type || Def(d.type).op == spv::OpTypeVoid) Unsupported("non-value source operand");
        if (Ephemeral(d.type)) {
            auto found = ephemeral_values.find(id);
            if (found == ephemeral_values.end())
                Unsupported("opaque resource SSA crosses a source state boundary; explicit provenance rematerialization required");
            return found->second;
        }
        auto result = Op("OpLoad " + Type(d.type) + " " + Register(id));
        DecorateResult(id, result, true);
        return result;
    }
    void Save(uint32_t id, const std::string &value) {
        if (Ephemeral(Def(id).type))
            ephemeral_values[id] = value;
        else
            functions << "OpStore " << Register(id) << " " << value << "\n";
    }
    uint32_t Storage(uint32_t pointer) const { return Def(Def(pointer).type).words.at(2); }
    std::string Offset(uint32_t pointer) {
        auto system = builtin_variables.find(pointer);
        if (system != builtin_variables.end()) return Constant(system->second->byte_offset);
        auto invocation = invocation_variables.find(pointer);
        if (invocation != invocation_variables.end()) return Constant(static_cast<uint32_t>(invocation->second->offset));
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
        if (IsPersistentStorage(Storage(id)) || SystemPointer(id)) Unsupported("persistent pointer used outside typed load/store");
        if (Storage(id) == spv::StorageClassImage) {
            auto found = ephemeral_pointers.find(id);
            if (found != ephemeral_pointers.end()) return found->second;
            if (d.op == spv::OpCopyObject) return RealPointer(d.words.at(3));
            Unsupported("image texel pointer crosses a source state boundary; explicit provenance rematerialization required");
        }
        if (!d.in_function && d.op == spv::OpVariable && PhysicalResourcePointer(id)) {
            auto offset =
                Storage(id) == spv::StorageClassPushConstant ? VEL_RT_SYS_APP_PUSH_ADDRESS : VEL_RT_SYS_SHADER_RECORD_ADDRESS;
            auto address = LoadAddress("%sc_base", offset);
            return Op("OpConvertUToPtr " + Type(d.type) + " " + address);
        }
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
    std::string LoadAddress(const std::string &base, uint32_t offset) {
        auto low = Op("OpFunctionCall " + code.word_type + " " + word_codec->load + " " + base + " " + Constant(offset));
        auto high = Op("OpFunctionCall " + code.word_type + " " + word_codec->load + " " + base + " " + Constant(offset + 4));
        auto pair = Op("OpCompositeConstruct " + code.word_pair_type + " " + low + " " + high);
        return Op("OpBitcast " + code.base_type + " " + pair);
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
        if (i.op == spv::OpReportIntersectionKHR) {
            auto result = LoadWord(3);
            result = Op("OpINotEqual " + code.bool_type + " " + result + " " + Constant(0));
            Save(i.result, result);
        }
        StoreWord(1, Constant(0));
        StoreWord(2, Constant(0));
    }
    void DataReference(uint32_t variable) {
        const auto found = invocation_variables.find(variable);
        if (found == invocation_variables.end() || Def(variable).op != spv::OpVariable)
            Unsupported("RT request data operand must be an invocation OpVariable");
        const auto &data = *found->second;
        StoreWord(20, Constant(static_cast<uint32_t>(data.offset)));
        StoreWord(21, Constant(static_cast<uint32_t>(data.size)));
        StoreWord(22, Constant(data.location));
        StoreWord(23, Constant(data.storage_class));
        StoreWord(24, Constant(data.type));
        StoreWord(25, Constant(data.variable));
    }
    void FloatArgument(uint32_t word, uint32_t value) {
        const auto &type = Def(Def(value).type);
        if (type.op != spv::OpTypeFloat || type.words.at(2) != 32) Unsupported("RT float argument must be float32");
        auto number = Value(value);
        StoreWord(word, Op("OpBitcast " + code.word_type + " " + number));
    }
    void VectorArgument(uint32_t word, uint32_t value) {
        const auto &vector = Def(Def(value).type);
        if (vector.op != spv::OpTypeVector || vector.words.at(3) != 3) Unsupported("RT vector argument must have three components");
        const auto &scalar = Def(vector.words.at(2));
        if (scalar.op != spv::OpTypeFloat || scalar.words.at(2) != 32) Unsupported("RT vector argument must be float32");
        auto number = Value(value);
        for (uint32_t n = 0; n < 3; ++n) {
            auto component = Op("OpCompositeExtract " + Type(vector.words.at(2)) + " " + number + " " + std::to_string(n));
            StoreWord(word + n, Op("OpBitcast " + code.word_type + " " + component));
        }
    }
    std::string IntegerArgument(uint32_t value) {
        const auto &type = Def(Def(value).type);
        if (type.op != spv::OpTypeInt || type.words.at(2) != 32) Unsupported("RT integer argument must be 32-bit");
        auto result = Value(value);
        if (Type(Def(value).type) != code.word_type) result = Op("OpBitcast " + code.word_type + " " + result);
        return result;
    }
    void Suspend(const Instruction &i, uint32_t resume_state) {
        const auto &site = *sites.at(i.offset);
        std::string spill = "OpFunctionCall " + code.void_type + " " + site.spill + " %sc_base";
        for (auto value : site.saved_values) spill += " " + Value(value);
        Op(spill);
        for (uint32_t word = 20; word <= 25; ++word) StoreWord(word, Constant(0));
        uint32_t kind = 0;
        if (i.op == spv::OpReportIntersectionKHR) {
            kind = 2;
            FloatArgument(4, i.words.at(3));
            StoreWord(5, IntegerArgument(i.words.at(4)));
            for (const auto &data : output.invocation_variables)
                if (data.storage_class == spv::StorageClassHitAttributeKHR) DataReference(data.variable);
        } else if (i.op == spv::OpTraceRayKHR) {
            kind = 1;
            auto address = Value(i.words.at(1));
            auto pair = Op("OpBitcast " + code.word_pair_type + " " + address);
            StoreWord(4, Op("OpCompositeExtract " + code.word_type + " " + pair + " 0"));
            StoreWord(5, Op("OpCompositeExtract " + code.word_type + " " + pair + " 1"));
            for (uint32_t n = 2; n <= 6; ++n) StoreWord(n + 5, IntegerArgument(i.words.at(n)));
            VectorArgument(12, i.words.at(7));
            FloatArgument(15, i.words.at(8));
            VectorArgument(16, i.words.at(9));
            FloatArgument(19, i.words.at(10));
            DataReference(i.words.at(11));
        } else if (i.op == spv::OpExecuteCallableKHR) {
            kind = 3;
            StoreWord(4, IntegerArgument(i.words.at(1)));
            DataReference(i.words.at(2));
        } else
            Unsupported("unknown suspension request");
        StoreWord(6, Constant(i.offset));
        StoreWord(0, Constant(resume_state));
        StoreWord(2, Constant(0));
        StoreWord(1, Constant(kind));
        functions << "OpReturn\n";
    }
    void Generic(const Instruction &i) {
        if (!i.result || !i.type || IsPointer(i.type) || Def(i.type).op == spv::OpTypeVoid || i.op == spv::OpFunctionCall)
            Unsupported("unsupported source operation Op" + std::string(spvOpcodeString(i.op)) + " at word " +
                        std::to_string(i.offset));
        auto result = Fresh();
        if (i.op == spv::OpLoad && Def(i.type).op == spv::OpTypeAccelerationStructureKHR &&
            Storage(i.words.at(3)) == spv::StorageClassUniformConstant) {
            auto pointer = RealPointer(i.words.at(3));
            auto member_type = PointerType(spv::StorageClassStorageBuffer, Type(i.type));
            auto member = Op("OpAccessChain " + member_type + " " + pointer + " " + Constant(0));
            for (const auto &decoration : parsed.instructions) {
                if (decoration.op == spv::OpDecorate && decoration.words.at(2) == spv::DecorationNonUniform &&
                    (decoration.words.at(1) == i.result || decoration.words.at(1) == i.words.at(3))) {
                    annotations << "OpDecorate " << member << " NonUniform\n";
                    break;
                }
            }
            auto text = Rewrite(i.text, [&](uint32_t id) {
                if (id == i.result) return result;
                if (id == i.type) return Type(id);
                if (id == i.words.at(3)) return member;
                return Value(id);
            });
            functions << text << "\n";
            DecorateResult(i.result, result);
            Save(i.result, result);
            return;
        }
        std::map<uint32_t, std::string> loaded;
        for (auto id : i.uses)
            if (!loaded.count(id)) loaded[id] = Value(id);
        const auto result_type = i.op == spv::OpLoad ? ResourceType(i.type) : Type(i.type);
        auto text = Rewrite(i.text, [&](uint32_t id) {
            if (id == i.result) return result;
            if (id == i.type) return result_type;
            return loaded.at(id);
        });
        if (i.op == spv::OpLoad && PhysicalResourcePointer(i.words.at(3))) text = AlignedMemory(text, i, i.type, 5);
        if (i.op == spv::OpArrayLength && PhysicalResourcePointer(i.words.at(3)))
            Unsupported("physical shader-record runtime-array length requires a record-size ABI");
        if (i.op == spv::OpConvertUToAccelerationStructureKHR) {
            auto name = text.find("OpConvertUToAccelerationStructureKHR");
            if (name == std::string::npos) Unsupported("missing AS conversion opcode");
            text.replace(name, std::string("OpConvertUToAccelerationStructureKHR").size(), "OpBitcast");
        }
        functions << text << "\n";
        if (result_type != Type(i.type)) result = Op("OpCopyLogical " + Type(i.type) + " " + result);
        DecorateResult(i.result, result);
        Save(i.result, result);
    }
    uint32_t ScalarAlignment(uint32_t type) const {
        const auto &d = Def(type);
        if (d.op == spv::OpTypeInt || d.op == spv::OpTypeFloat) return d.words.at(2) / 8;
        if (d.op == spv::OpTypeBool) return 4;
        if (d.op == spv::OpTypeAccelerationStructureKHR) return 8;
        if (d.op == spv::OpTypeVector || d.op == spv::OpTypeMatrix || d.op == spv::OpTypeArray || d.op == spv::OpTypeRuntimeArray)
            return ScalarAlignment(d.words.at(2));
        uint32_t alignment = 1;
        if (d.op == spv::OpTypeStruct)
            for (size_t n = 2; n < d.words.size(); ++n) alignment = std::max(alignment, ScalarAlignment(d.words[n]));
        return alignment;
    }
    std::string AlignedMemory(const std::string &text, const Instruction &i, uint32_t type, size_t mask_token) const {
        if (text.find("Aligned") != std::string::npos) return text;
        std::istringstream stream(text);
        std::vector<std::string> tokens;
        std::string token;
        while (stream >> token) tokens.push_back(token);
        auto alignment = std::to_string(ScalarAlignment(type));
        if (tokens.size() <= mask_token) {
            tokens.push_back("Aligned");
            tokens.push_back(alignment);
        } else {
            tokens[mask_token] = tokens[mask_token] == "None" ? "Aligned" : tokens[mask_token] + "|Aligned";
            tokens.insert(tokens.begin() + mask_token + 1, alignment);
        }
        std::string result;
        for (const auto &part : tokens) result += (result.empty() ? "" : " ") + part;
        return result;
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
        ephemeral_values.clear();
        ephemeral_pointers.clear();
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
                (i.op == spv::OpCopyObject && IsPointer(i.type) && Def(i.type).words.at(2) != spv::StorageClassImage))
                continue;
            if (i.op == spv::OpImageTexelPointer || (i.op == spv::OpCopyObject && IsPointer(i.type))) {
                auto result = Fresh();
                functions << Rewrite(i.text, [&](uint32_t id) {
                    if (id == i.result) return result;
                    if (id == i.type) return Type(id);
                    return Value(id);
                }) << "\n";
                DecorateResult(i.result, result);
                ephemeral_pointers[i.result] = result;
                continue;
            }
            if (i.op == spv::OpIgnoreIntersectionKHR || i.op == spv::OpTerminateRayKHR) {
                StoreWord(1, Constant(i.op == spv::OpIgnoreIntersectionKHR ? 4u : 5u));
                StoreWord(2, Constant(0));
                StoreWord(0, Constant(UINT32_MAX));
                functions << "OpReturn\n";
                return;
            }
            if (i.op == spv::OpReportIntersectionKHR || i.op == spv::OpTraceRayKHR || i.op == spv::OpExecuteCallableKHR) {
                Suspend(i, state.number + 1);
                return;
            }
            if (i.op == spv::OpLoad && (IsPersistentStorage(Storage(i.words.at(3))) || SystemPointer(i.words.at(3)))) {
                if (i.words.size() != 4) Unsupported("persistent local load memory operands");
                auto address = Offset(i.words.at(3));
                Save(i.result, Op("OpFunctionCall " + Type(i.type) + " " + codecs.at(i.type).load + " %sc_base " + address));
            } else if (i.op == spv::OpStore) {
                if (IsPersistentStorage(Storage(i.words.at(1)))) {
                    if (i.words.size() != 3) Unsupported("persistent local store memory operands");
                    auto type = Def(Def(i.words.at(1)).type).words.at(3);
                    auto address = Offset(i.words.at(1));
                    Op("OpFunctionCall " + code.void_type + " " + codecs.at(type).store + " %sc_base " + address + " " +
                       Value(i.words.at(2)));
                } else {
                    const auto value_id = i.words.at(2);
                    const auto type = Def(value_id).type;
                    auto value = Value(value_id);
                    if (ResourceType(type) != Type(type)) value = Op("OpCopyLogical " + ResourceType(type) + " " + value);
                    auto text = Rewrite(i.text, [&](uint32_t id) { return id == value_id ? value : Value(id); });
                    if (PhysicalResourcePointer(i.words.at(1))) text = AlignedMemory(text, i, Def(i.words.at(2)).type, 3);
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
            } else if (i.op == spv::OpImageWrite || i.op == spv::OpAtomicStore || i.op == spv::OpAtomicFlagClear ||
                       i.op == spv::OpMemoryBarrier) {
                functions << Rewrite(i.text, [&](uint32_t id) { return Value(id); }) << "\n";
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
        functions << "%sc_step = OpFunction " << code.void_type << " None " << code.step_signature
                  << "\n%sc_base = OpFunctionParameter " << code.base_type << "\n%sc_entry = OpLabel\n";
        pc_pointer = "%sc_pc";
        auto pc_type = PointerType(spv::StorageClassFunction, code.word_type);
        functions << pc_pointer << " = OpVariable " << pc_type << " Function\n";
        for (const auto &i : parsed.instructions) {
            if (!i.in_function || !i.result || !i.type || IsPointer(i.type) || Ephemeral(i.type) ||
                Def(i.type).op == spv::OpTypeVoid)
                continue;
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
        if (BDA()) {
            EmitBDAKernel();
            return;
        }
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
    void EmitBDAKernel() {
        for (const auto &[id, codec] : codecs) {
            const auto &d = Def(id);
            if (d.op == spv::OpTypeVector && d.words.at(3) == 3 && Type(d.words.at(2)) == code.word_type)
                gid_type = codec.spirv_type;
        }
        if (gid_type.empty()) {
            gid_type = "%sc_gid_type";
            declarations << gid_type << " = OpTypeVector " << code.word_type << " 3\n";
        }
        auto pointer = PointerType(spv::StorageClassInput, gid_type);
        gid = "%sc_gid";
        group_count = "%sc_groups";
        declarations << gid << " = OpVariable " << pointer << " Input\n"
                     << group_count << " = OpVariable " << pointer << " Input\n"
                     << "%sc_ctx_low = OpSpecConstant " << code.word_type << " 0\n%sc_ctx_high = OpSpecConstant " << code.word_type
                     << " 0\n"
                     << "%sc_main_sig = OpTypeFunction " << code.void_type << "\n%sc_product64 = OpTypeStruct " << code.base_type
                     << " " << code.base_type << "\n";
        annotations << "OpDecorate " << gid << " BuiltIn GlobalInvocationId\nOpDecorate " << group_count
                    << " BuiltIn NumWorkgroups\n"
                    << "OpDecorate %sc_ctx_low SpecId " << config.context_root_low_spec_id << "\nOpDecorate %sc_ctx_high SpecId "
                    << config.context_root_high_spec_id << "\n";
        interfaces.push_back(gid);
        interfaces.push_back(group_count);
        functions << "%sc_main = OpFunction " << code.void_type << " None %sc_main_sig\n%sc_main_entry = OpLabel\n";
        auto pair = Op("OpCompositeConstruct " + code.word_pair_type + " %sc_ctx_low %sc_ctx_high");
        auto root = Op("OpBitcast " + code.base_type + " " + pair);
        auto is_null = Op("OpIEqual " + code.bool_type + " " + root + " " + WideConstant(0));
        auto alignment = Op("OpBitwiseAnd " + code.base_type + " " + root + " " + WideConstant(7));
        auto unaligned = Op("OpINotEqual " + code.bool_type + " " + alignment + " " + WideConstant(0));
        auto invalid = Op("OpLogicalOr " + code.bool_type + " " + is_null + " " + unaligned);
        functions << "OpSelectionMerge %sc_context_ready None\nOpBranchConditional " << invalid
                  << " %sc_no_context %sc_context_ready\n"
                  << "%sc_no_context = OpLabel\nOpReturn\n%sc_context_ready = OpLabel\n";
        auto frames = LoadAddress(root, VEL_RT_CTX_FRAMES_ADDRESS);
        auto stride = LoadAddress(root, VEL_RT_CTX_FRAME_STRIDE);
        auto count = LoadAddress(root, VEL_RT_CTX_FRAME_COUNT);
        auto offset = LoadAddress(root, VEL_RT_CTX_FRAME_OFFSET);
        auto capacity = LoadAddress(root, VEL_RT_CTX_FRAME_CAPACITY);
        auto driver_id = Op("OpLoad " + gid_type + " " + gid);
        auto groups = Op("OpLoad " + gid_type + " " + group_count);
        auto component = [&](const std::string &value, uint32_t n) {
            auto word = Op("OpCompositeExtract " + code.word_type + " " + value + " " + std::to_string(n));
            return Op("OpUConvert " + code.base_type + " " + word);
        };
        auto x = component(driver_id, 0), y = component(driver_id, 1), z = component(driver_id, 2);
        auto width = component(groups, 0), height = component(groups, 1);
        auto overflow = Op("OpULessThan " + code.bool_type + " " + stride + " " + WideConstant(output.frame_stride_bytes));
        auto add_flag = [&](const std::string &flag) {
            overflow = Op("OpLogicalOr " + code.bool_type + " " + overflow + " " + flag);
        };
        auto multiply = [&](const std::string &a, const std::string &b) {
            auto product = Op("OpUMulExtended %sc_product64 " + a + " " + b);
            auto low = Op("OpCompositeExtract " + code.base_type + " " + product + " 0");
            auto high = Op("OpCompositeExtract " + code.base_type + " " + product + " 1");
            add_flag(Op("OpINotEqual " + code.bool_type + " " + high + " " + WideConstant(0)));
            return low;
        };
        auto add = [&](const std::string &a, const std::string &b) {
            auto sum = Op("OpIAdd " + code.base_type + " " + a + " " + b);
            add_flag(Op("OpULessThan " + code.bool_type + " " + sum + " " + a));
            return sum;
        };
        auto yz = multiply(height, z);
        yz = add(y, yz);
        auto index = multiply(width, yz);
        index = add(x, index);
        add_flag(Op("OpUGreaterThanEqual " + code.bool_type + " " + index + " " + count));
        auto byte_index = multiply(index, stride);
        byte_index = add(offset, byte_index);
        auto end = add(byte_index, WideConstant(output.frame_stride_bytes));
        add_flag(Op("OpUGreaterThan " + code.bool_type + " " + end + " " + capacity));
        auto frame = add(frames, byte_index);
        auto frame_end = add(frame, WideConstant(output.frame_stride_bytes));
        (void)frame_end;
        auto frame_alignment = Op("OpBitwiseAnd " + code.base_type + " " + frame + " " + WideConstant(3));
        add_flag(Op("OpINotEqual " + code.bool_type + " " + frame_alignment + " " + WideConstant(0)));
        add_flag(Op("OpIEqual " + code.bool_type + " " + frames + " " + WideConstant(0)));
        functions << "OpSelectionMerge %sc_bda_run None\nOpBranchConditional " << overflow << " %sc_bda_skip %sc_bda_run\n"
                  << "%sc_bda_skip = OpLabel\nOpReturn\n%sc_bda_run = OpLabel\n";
        Op("OpFunctionCall " + code.void_type + " %sc_step " + frame);
        functions << "OpReturn\nOpFunctionEnd\n";
    }
};
}  // namespace

TypedContinuationFrameBounds PlanTypedContinuationFrameBounds(uint64_t payload_bytes) {
    if (payload_bytes > UINT32_MAX - kTypedContinuationControlWords * 4u)
        Unsupported("state frame/control exceeds 32-bit byte indexing");
    uint64_t stride = (payload_bytes + 3) / 4 + kTypedContinuationControlWords;
    if (stride * 4 > UINT32_MAX) Unsupported("state frame/control exceeds 32-bit byte indexing");
    auto words = static_cast<uint32_t>(stride);
    return {words, UINT32_MAX / words};
}

TypedContinuationProgram CompileTypedContinuationSteps(const std::vector<uint32_t> &source, const std::string &entry,
                                                       uint32_t execution_model, uint32_t frame_descriptor_set,
                                                       uint32_t frame_binding) {
    TypedContinuationCompileOptions options;
    options.frame_descriptor_set = frame_descriptor_set;
    options.frame_binding = frame_binding;
    return CompileTypedContinuationSteps(source, entry, execution_model, options);
}
TypedContinuationProgram CompileTypedContinuationSteps(const std::vector<uint32_t> &source, const std::string &entry,
                                                       uint32_t execution_model, const TypedContinuationCompileOptions &options) {
    // Validate before optimization; entry selection is validated again by the
    // continuation analysis after inlining. Inlining preserves source ALU/CFG
    // semantics and removes the need to pretend helper returns are RT returns.
    const auto selected_entry = AnalyzeContinuations(source, entry, execution_model).entry;
    std::vector<uint32_t> selected(source.begin(), source.begin() + 5);
    bool has_uint32 = false;
    for (size_t p = 5; p < source.size(); p += source[p] >> 16)
        if ((source[p] & 0xffff) == spv::OpTypeInt && source[p + 2] == 32 && source[p + 3] == 0) has_uint32 = true;
    bool inserted_control_type = has_uint32;
    for (size_t p = 5; p < source.size(); p += source[p] >> 16) {
        auto op = static_cast<spv::Op>(source[p] & 0xffff);
        if (op == spv::OpEntryPoint && source.at(p + 2) != selected_entry) continue;
        if ((op == spv::OpExecutionMode || op == spv::OpExecutionModeId) && source.at(p + 1) != selected_entry) continue;
        if (op == spv::OpFunction && !inserted_control_type) {
            auto id = selected[3]++;
            selected.insert(selected.end(), {(4u << 16) | spv::OpTypeInt, id, 32, 0});
            inserted_control_type = true;
        }
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
    return Compiler(std::move(inlined), entry, execution_model, options).Run();
}
}  // namespace vel::rt
