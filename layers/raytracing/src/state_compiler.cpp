// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "state_compiler.h"
#include "spirv_environment.h"
#include "spirv_memory_model.h"
#include "frame_emitter.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

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
        const auto environment = VulkanEnvironmentForSPIRV(output.source.size() > 1 ? output.source[1] : 0);
        spvtools::SpirvTools tools(environment);
        std::string assembly;
        if (!tools.Disassemble(output.source, &assembly, SPV_BINARY_TO_TEXT_OPTION_NO_HEADER))
            Unsupported("source disassembly failed");
        auto context = spvContextCreate(environment);
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
                NeedValueType(id);
                break;
            }
        }
        for (const auto &i : parsed.instructions) {
            if (i.op == spv::OpMemoryModel && i.words.at(1) == spv::AddressingModelPhysicalStorageBuffer64) physical_source = true;
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
            if (i.op == spv::OpMemoryModel) vulkan_source = i.words.at(2) == spv::MemoryModelVulkan;
            if (i.op == spv::OpMemoryModel &&
                ((i.words.at(1) != spv::AddressingModelLogical && i.words.at(1) != spv::AddressingModelPhysicalStorageBuffer64) ||
                 (i.words.at(2) != spv::MemoryModelGLSL450 && i.words.at(2) != spv::MemoryModelVulkan)))
                Unsupported("source addressing/memory model is not GLSL450 or Vulkan");
        }
        for (const auto &i : parsed.instructions) {
            if (!i.in_function) continue;
            auto opcode = std::string(spvOpcodeString(i.op));
            if (opcode.find("Group") == 0 || opcode.find("Subgroup") == 0 || (i.op >= spv::OpDPdx && i.op <= spv::OpFwidthCoarse))
                Unsupported("subgroup/derivative execution semantics require scheduler support");
            if (i.type && Def(i.type).op == spv::OpTypePointer && !PhysicalPointer(i.type) && i.op != spv::OpVariable &&
                i.op != spv::OpAccessChain && i.op != spv::OpInBoundsAccessChain && i.op != spv::OpCopyObject &&
                i.op != spv::OpImageTexelPointer &&
                !((i.op == spv::OpPhi || i.op == spv::OpSelect || i.op == spv::OpLoad || i.op == spv::OpCompositeExtract ||
                   i.op == spv::OpPtrAccessChain || i.op == spv::OpInBoundsPtrAccessChain) &&
                  Def(i.type).words.at(2) == spv::StorageClassStorageBuffer))
                Unsupported("unsupported pointer provenance Op" + std::string(spvOpcodeString(i.op)) + " for ID " +
                            std::to_string(i.result));
            NeedValueType(i.type);
            if (i.op == spv::OpVariable && IsPersistentStorage(i.words.at(3))) NeedValueType(Def(i.type).words.at(3));
            for (auto id : i.uses) NeedGlobal(id);
        }
        for (const auto &i : parsed.instructions) {
            if (i.op == spv::OpExecutionMode || i.op == spv::OpExecutionModeId) {
                for (auto id : i.uses)
                    if (Def(id).op != spv::OpFunction) NeedGlobal(id);
            }
            if (i.op == spv::OpDecorate && i.words.at(2) == spv::DecorationVolatile) {
                const auto &target = Def(i.words.at(1));
                if (target.in_function || (target.op == spv::OpVariable && IsPersistentStorage(target.words.at(3))))
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
        PlanPointerRoutes();
        std::vector<uint32_t> types(value_types.begin(), value_types.end());
        ContinuationFrameOptions options{true, true};
        options.persistent_private_storage = true;
        options.logical_pointer_snapshots = !pointer_plans.empty();
        options.logical_pointer_words = pointer_words;
        options.physical_pointer_addresses = true;
        for (const auto &i : parsed.instructions)
            if (i.result && MatrixPointer(i.result)) options.physical_pointer_recipes.push_back(i.result);
        options.physical_frame_addresses = BDA();
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
            } else if (variable.words.at(3) == spv::StorageClassPrivate) {
                const auto pointee = Def(variable.type).words.at(3);
                const auto layout =
                    std::find_if(preview.types.begin(), preview.types.end(), [&](const auto &v) { return v.type == pointee; });
                if (layout == preview.types.end()) Unsupported("module-Private storage has no concrete typed layout");
                persistent_size = (persistent_size + layout->alignment - 1) & ~uint64_t(layout->alignment - 1);
                if (layout->size > UINT32_MAX || persistent_size > UINT32_MAX - layout->size)
                    Unsupported("module-Private storage exceeds 32-bit byte indexing");
                output.private_variables.push_back({id, pointee, persistent_size, layout->size, layout->alignment});
                persistent_size += layout->size;
            } else if (variable.words.at(3) == spv::StorageClassUniformConstant && IsASDescriptor(Def(variable.type).words.at(3))) {
                if (!resource_sets.count(id) || !resource_bindings.count(id)) Unsupported("AS descriptor is missing set/binding");
                output.acceleration_structure_descriptors.push_back({id, resource_sets.at(id), resource_bindings.at(id)});
            }
        }
        PlanResourceRecipes(persistent_size, preview.types);
        PlanPointerSnapshots(persistent_size);
        const auto bounds = PlanTypedContinuationFrameBounds(persistent_size);
        output.frame_stride_words = bounds.stride_words;
        output.max_invocations = bounds.max_invocations;
        output.control_word = static_cast<uint32_t>((persistent_size + 3) / 4);
        output.control_byte_offset = output.control_word * 4;
        output.frame_stride_bytes = uint64_t(output.frame_stride_words) * 4;
        output.max_invocations64 = BDA() ? UINT64_MAX / output.frame_stride_bytes : output.max_invocations;
        for (const auto &variable : output.invocation_variables) invocation_variables[variable.variable] = &variable;
        for (const auto &variable : output.private_variables) private_variables[variable.variable] = &variable;
        code = EmitContinuationFrameCodecs(output.source, this->entry, model, set, binding, "sc_cf_", types, options);
        // Reuse the component's existing word pointer declaration rather than
        // emit an illegal duplicate non-aggregate SPIR-V pointer type.
        pointer_types[std::to_string(BDA() ? spv::StorageClassPhysicalStorageBuffer : spv::StorageClassStorageBuffer) + " " +
                      code.word_type] = code.word_pointer_type;
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
        if (source_caps.str().find("NonUniform") != std::string::npos) EnsureExtension("SPV_EXT_descriptor_indexing");
        std::string assembly = code.capabilities + source_caps.str() + code.extensions + extensions.str() + imports.str() +
                               ((BDA() || physical_source) ? "OpMemoryModel PhysicalStorageBuffer64 " : "OpMemoryModel Logical ") +
                               (vulkan_source ? "Vulkan\n" : "GLSL450\n");
        if (config.emit_compute_entry) {
            assembly += "OpEntryPoint GLCompute %sc_main \"main\" " + code.memory;
            for (const auto &global : interfaces) assembly += " " + global;
            assembly += "\nOpExecutionMode %sc_main LocalSize 1 1 1\n" + execution_modes.str();
        }
        assembly += code.annotations + annotations.str() + forward_types.str() + code.declarations + declarations.str() +
                    code.functions + bridge_functions.str() + qualified_functions.str() + functions.str();
        const auto version = std::max(output.source.at(1), 0x00010400u);
        spvtools::SpirvTools tools(config.emit_compute_entry ? VulkanEnvironmentForSPIRV(version)
                                                             : UniversalEnvironmentForSPIRV(version));
        std::string error;
        tools.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &p, const char *message) {
            error += std::to_string(p.index) + ": " + message + "\n";
        });
        auto assembled = tools.Assemble(assembly, &output.spirv);
        if (!assembled) Unsupported("generated assembly failed: " + error);
        if (config.emit_compute_entry) LowerTypedShaderCallScopes(output.spirv);
        spvtools::ValidatorOptions validation;
        validation.SetScalarBlockLayout(true);
        auto valid = tools.Validate(output.spirv.data(), output.spirv.size(), validation);
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
    // Opaque handles have no portable representation. A recipe contains only
    // a stable descriptor root and typed snapshots of its dynamic selectors.
    // Each result owns independent snapshots, including copies of its parents'
    // selectors: a loop can execute a parent definition again without changing
    // an already captured child handle or image texel pointer.
    struct RecipeNode {
        uint32_t source = 0, type = 0, offset = 0;
        std::vector<std::pair<uint32_t, std::shared_ptr<RecipeNode>>> operands;
    };
    struct RecipeCapture {
        uint32_t type, offset, value, inherited_offset;
    };
    struct ResourceRecipe {
        std::shared_ptr<RecipeNode> root;
        std::vector<RecipeCapture> captures;
    };
    // A route is a descriptor root plus an exact typed access path. Constant
    // selectors keep their module ID; dynamic selectors keep only their type.
    // Equal routes share a tag, while each SSA result owns its own selector
    // snapshots. Pointer Phi cycles therefore have a finite path fixed point.
    struct PointerIndex {
        uint32_t constant = 0, type = 0, pointer_type = 0;
        bool operator<(const PointerIndex &other) const {
            return std::tie(constant, type, pointer_type) < std::tie(other.constant, other.type, other.pointer_type);
        }
    };
    using PointerRoute = std::pair<uint32_t, std::vector<PointerIndex>>;
    struct PointerPlan {
        std::set<uint32_t> routes;
        uint32_t offset = 0, words = 1;
        bool non_uniform = false, conservative = false;
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
    std::map<uint32_t, std::map<std::string, std::string>> qualified_codecs;
    std::map<uint32_t, const ContinuationFrameSlot *> slots;
    std::map<uint32_t, const TypedInvocationVariable *> invocation_variables;
    std::map<uint32_t, const TypedPrivateVariable *> private_variables;
    std::map<uint32_t, const TypedBuiltinVariable *> builtin_variables;
    std::map<uint32_t, const ContinuationSuspensionCodec *> sites;
    std::map<uint32_t, uint32_t> first_state;
    std::map<uint32_t, std::string> cloned_types;
    std::map<uint32_t, std::string> resource_types;
    std::map<std::pair<uint32_t, bool>, std::string> bridges;
    std::map<std::string, std::string> pointer_types;
    std::map<uint32_t, std::string> descriptor_types;
    std::map<uint32_t, std::string> constants;
    std::map<uint32_t, std::string> ephemeral_values;
    std::map<uint32_t, std::string> ephemeral_pointers;
    std::map<uint32_t, ResourceRecipe> resource_recipes;
    std::map<PointerRoute, uint32_t> pointer_route_ids;
    std::vector<PointerRoute> pointer_routes;
    std::map<uint32_t, PointerPlan> pointer_plans;
    std::map<std::string, std::string> null_pointers;
    std::map<uint32_t, uint32_t> active_pointer_routes;
    uint32_t pointer_words = 0, pointer_arithmetic_type = 0;
    std::set<uint64_t> wide_constants;
    std::vector<std::string> interfaces;
    std::ostringstream source_caps, imports, extensions, execution_modes, annotations, declarations, functions, qualified_functions;
    std::ostringstream forward_types, bridge_functions;
    bool physical_source = false;
    bool vulkan_source = false;
    std::string gid, gid_type, group_count, pc_pointer;
    bool BDA() const { return config.transport == TypedFrameTransport::PhysicalAddress; }
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
        return storage == spv::StorageClassPhysicalStorageBuffer ||
               (BDA() && (storage == spv::StorageClassPushConstant || storage == spv::StorageClassShaderRecordBufferKHR));
    }
    static bool IsInvocationStorage(uint32_t storage) {
        return storage == spv::StorageClassRayPayloadKHR || storage == spv::StorageClassIncomingRayPayloadKHR ||
               storage == spv::StorageClassCallableDataKHR || storage == spv::StorageClassIncomingCallableDataKHR ||
               storage == spv::StorageClassHitAttributeKHR;
    }
    static bool IsPersistentStorage(uint32_t storage) {
        return storage == spv::StorageClassFunction || storage == spv::StorageClassPrivate || IsInvocationStorage(storage);
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
        if (d.op == spv::OpTypePointer && (d.words.at(2) == spv::StorageClassStorageBuffer || PhysicalPointer(id))) return true;
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
        // Scalar/vector declarations share the logical codec types. Aggregate
        // resource declarations have their own native layouts and can contain
        // millions of descriptors; merely reaching them through a pointer must
        // never request a logical aggregate codec or unroll their elements.
        if (d.op == spv::OpTypeBool || d.op == spv::OpTypeInt || d.op == spv::OpTypeFloat || d.op == spv::OpTypeVector ||
            d.op == spv::OpTypeMatrix || d.op == spv::OpTypeAccelerationStructureKHR)
            value_types.insert(id);
        for (auto child : d.uses) {
            const auto &v = Def(child);
            if ((v.op >= spv::OpTypeVoid && v.op <= spv::OpTypeForwardPointer) || v.op == spv::OpTypeAccelerationStructureKHR)
                NeedType(child);
            else
                NeedGlobal(child);
        }
    }
    void NeedValueType(uint32_t id) {
        if (!id) return;
        NeedType(id);
        // Concrete SSA values and persistent pointees need logical backing.
        // PlanContinuationFrames recursively includes their aggregate children.
        if (Layoutable(id)) value_types.insert(id);
    }
    void NeedGlobal(uint32_t id) {
        const auto &d = Def(id);
        if (d.in_function || !globals.insert(id).second) return;
        NeedType(d.type);
        if (d.op == spv::OpVariable && IsPersistentStorage(d.words.at(3)))
            NeedValueType(Def(d.type).words.at(3));
        else if (d.op == spv::OpUndef || (d.op >= spv::OpConstantTrue && d.op <= spv::OpConstantNull))
            NeedValueType(d.type);
        for (auto child : d.uses) NeedGlobal(child);
    }
    std::string PointerType(uint32_t storage, const std::string &pointee, uint32_t stride = 0) {
        if (IsInvocationStorage(storage)) storage = spv::StorageClassFunction;
        auto key = std::to_string(storage) + " " + pointee;
        if (stride) key += " stride=" + std::to_string(stride);
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
        if (stride) annotations << "OpDecorate " << id << " ArrayStride " << stride << "\n";
        pointer_types[key] = id;
        return id;
    }
    std::string Type(uint32_t id) {
        const auto &d = Def(id);
        if (d.op == spv::OpTypeVoid) return code.void_type;
        auto codec = codecs.find(id);
        if (codec != codecs.end() && d.op != spv::OpTypePointer) return codec->second.spirv_type;
        // Runtime arrays and their enclosing blocks have no logical frame
        // codec. Reuse their one resource declaration, including its Block
        // decoration, rather than emitting an invalid unadorned duplicate.
        if (d.op == spv::OpTypeStruct || d.op == spv::OpTypeArray || d.op == spv::OpTypeRuntimeArray) return ResourceType(id);
        auto found = cloned_types.find(id);
        if (found != cloned_types.end()) return found->second;
        if (d.op == spv::OpTypePointer) {
            if (PhysicalPointer(id)) {
                if (Def(d.words.at(3)).op != spv::OpTypeStruct) {
                    const auto name =
                        PointerType(spv::StorageClassPhysicalStorageBuffer, ResourceType(d.words.at(3)), PointerStride(id));
                    cloned_types[id] = name;
                    return name;
                }
                // A physical pointee may be recursive. Reserve its identity
                // before cloning layout types and declare the forward pointer.
                const auto name = Fresh();
                cloned_types[id] = name;
                forward_types << "OpTypeForwardPointer " << name << " PhysicalStorageBuffer\n";
                const auto pointee = ResourceType(d.words.at(3));
                declarations << name << " = OpTypePointer PhysicalStorageBuffer " << pointee << "\n";
                if (const auto stride = PointerStride(id))
                    annotations << "OpDecorate " << name << " ArrayStride " << stride << "\n";
                return name;
            }
            if (d.words.at(2) == spv::StorageClassUniformConstant && IsASDescriptor(d.words.at(3)))
                return PointerType(spv::StorageClassStorageBuffer, DescriptorType(d.words.at(3)));
            if (BDA() &&
                (d.words.at(2) == spv::StorageClassPushConstant || d.words.at(2) == spv::StorageClassShaderRecordBufferKHR))
                return PointerType(spv::StorageClassPhysicalStorageBuffer, ResourceType(d.words.at(3)));
            const auto storage = d.words.at(2);
            const bool resource = storage == spv::StorageClassStorageBuffer || storage == spv::StorageClassUniform ||
                                  storage == spv::StorageClassPushConstant || storage == spv::StorageClassShaderRecordBufferKHR;
            return PointerType(storage, resource ? ResourceType(d.words.at(3)) : ValueType(d.words.at(3)), PointerStride(id));
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
    std::string ValueType(uint32_t id) {
        const auto codec = codecs.find(id);
        return codec != codecs.end() ? codec->second.spirv_type : Type(id);
    }
    bool PhysicalPointer(uint32_t type) const {
        return type && Def(type).op == spv::OpTypePointer && Def(type).words.at(2) == spv::StorageClassPhysicalStorageBuffer;
    }
    bool ContainsPhysicalPointer(uint32_t type) const {
        if (PhysicalPointer(type)) return true;
        const auto &d = Def(type);
        if (d.op == spv::OpTypeArray || d.op == spv::OpTypeRuntimeArray) return ContainsPhysicalPointer(d.words.at(2));
        if (d.op == spv::OpTypeStruct)
            for (size_t n = 2; n < d.words.size(); ++n)
                if (ContainsPhysicalPointer(d.words[n])) return true;
        return false;
    }
    bool PhysicalPointerArray(uint32_t type) const {
        // Native pointer arrays retain conservative alias information across
        // the bridge; numeric identity arrays are not pointer memory objects.
        while (Def(type).op == spv::OpTypeArray || Def(type).op == spv::OpTypeRuntimeArray) type = Def(type).words.at(2);
        return PhysicalPointer(type);
    }
    bool MatrixPointer(uint32_t id) const {
        const auto &d = Def(id);
        if (!PhysicalPointer(d.type)) return false;
        auto matrix_array = [&](uint32_t type) {
            while (Def(type).op == spv::OpTypeArray || Def(type).op == spv::OpTypeRuntimeArray) type = Def(type).words.at(2);
            return Def(type).op == spv::OpTypeMatrix;
        };
        if (matrix_array(Def(d.type).words.at(3))) return true;
        const bool chain = d.op == spv::OpAccessChain || d.op == spv::OpInBoundsAccessChain;
        const bool ptr_chain = d.op == spv::OpPtrAccessChain || d.op == spv::OpInBoundsPtrAccessChain;
        if (!chain && !ptr_chain) return false;
        const auto base = d.words.at(3);
        if (MatrixPointer(base)) return true;
        auto type = Def(Def(base).type).words.at(3);
        for (size_t n = ptr_chain ? 5 : 4; n < d.words.size(); ++n) {
            const auto &part = Def(type);
            if (part.op == spv::OpTypeMatrix) return true;
            if (part.op == spv::OpTypeStruct)
                type = part.words.at(2 + Def(d.words[n]).words.at(3));
            else if (part.op == spv::OpTypeArray || part.op == spv::OpTypeRuntimeArray || part.op == spv::OpTypeVector)
                type = part.words.at(2);
            else
                break;
        }
        return matrix_array(type);
    }
    std::string BridgeFunction(uint32_t type, bool native) {
        const auto key = std::make_pair(type, native);
        auto known = bridges.find(key);
        if (known != bridges.end()) return known->second;
        const auto function = Fresh(), signature = Fresh();
        bridges[key] = function;
        const auto input = native ? ValueType(type) : ResourceType(type);
        const auto output = native ? ResourceType(type) : ValueType(type);
        declarations << signature << " = OpTypeFunction " << output << " " << input << "\n";
        std::ostringstream body;
        auto emit = [&](const std::string &operation) {
            auto result = Fresh();
            body << result << " = " << operation << "\n";
            return result;
        };
        body << function << " = OpFunction " << output << " None " << signature << "\n";
        const auto parameter = emit("OpFunctionParameter " + input);
        emit("OpLabel");
        auto convert = [&](uint32_t child, const std::string &value) {
            const auto from = native ? ValueType(child) : ResourceType(child);
            const auto to = native ? ResourceType(child) : ValueType(child);
            if (from == to) return value;
            if (PhysicalPointer(child))
                return emit(std::string(native ? "OpConvertUToPtr " : "OpConvertPtrToU ") + to + " " + value);
            if (!ContainsPhysicalPointer(child)) return emit("OpCopyLogical " + to + " " + value);
            const auto callee = BridgeFunction(child, native);
            return emit("OpFunctionCall " + to + " " + callee + " " + value);
        };
        const auto &d = Def(type);
        std::string result;
        if (d.op == spv::OpTypeStruct) {
            std::string construct = "OpCompositeConstruct " + output;
            for (size_t n = 2; n < d.words.size(); ++n) {
                const auto child = d.words[n];
                const auto from = native ? ValueType(child) : ResourceType(child);
                const auto part = emit("OpCompositeExtract " + from + " " + parameter + " " + std::to_string(n - 2));
                construct += " " + convert(child, part);
            }
            result = emit(construct);
        } else if (d.op == spv::OpTypeArray) {
            // Keep conversion bounded in module size for arbitrary fixed-size
            // arrays. Only the true source Load/Store carries memory operands;
            // these Function temporaries are compiler-private value shuttles.
            const auto child = d.words.at(2);
            const auto from = native ? ValueType(child) : ResourceType(child);
            const auto to = native ? ResourceType(child) : ValueType(child);
            const auto input_storage = emit("OpVariable " + PointerType(spv::StorageClassFunction, input) + " Function");
            const auto output_storage = emit("OpVariable " + PointerType(spv::StorageClassFunction, output) + " Function");
            if (PhysicalPointerArray(type))
                annotations << "OpDecorate " << (native ? output_storage : input_storage) << " AliasedPointer\n";
            body << "OpStore " << input_storage << " " << parameter << "\n";
            auto entry = Fresh(), loop = Fresh(), work = Fresh(), next = Fresh(), done = Fresh(), index = Fresh(),
                 increment = Fresh();
            const auto &length = Def(d.words.at(3));
            uint64_t count = length.words.at(3);
            if (length.words.size() > 4) count |= uint64_t(length.words.at(4)) << 32;
            if (count > UINT32_MAX) Unsupported("physical pointer aggregate conversion length exceeds uint32");
            body << "OpBranch " << entry << "\n"
                 << entry << " = OpLabel\nOpBranch " << loop << "\n"
                 << loop << " = OpLabel\n"
                 << index << " = OpPhi " << code.word_type << " " << Constant(0) << " " << entry << " " << increment << " " << next
                 << "\n";
            const auto active = emit("OpULessThan " + code.bool_type + " " + index + " " + Constant(static_cast<uint32_t>(count)));
            body << "OpLoopMerge " << done << " " << next << " None\nOpBranchConditional " << active << " " << work << " " << done
                 << "\n"
                 << work << " = OpLabel\n";
            const auto source =
                emit("OpAccessChain " + PointerType(spv::StorageClassFunction, from) + " " + input_storage + " " + index);
            const auto value = emit("OpLoad " + from + " " + source);
            const auto converted = convert(child, value);
            const auto target =
                emit("OpAccessChain " + PointerType(spv::StorageClassFunction, to) + " " + output_storage + " " + index);
            body << "OpStore " << target << " " << converted << "\nOpBranch " << next << "\n"
                 << next << " = OpLabel\n"
                 << increment << " = OpIAdd " << code.word_type << " " << index << " " << Constant(1) << "\nOpBranch " << loop
                 << "\n"
                 << done << " = OpLabel\n";
            result = emit("OpLoad " + output + " " + output_storage);
        } else
            Unsupported("physical pointer conversion requires a concrete aggregate");
        body << "OpReturnValue " << result << "\nOpFunctionEnd\n";
        bridge_functions << body.str();
        return function;
    }
    std::string Bridge(uint32_t type, const std::string &value, bool native) {
        const auto from = native ? ValueType(type) : ResourceType(type);
        const auto to = native ? ResourceType(type) : ValueType(type);
        if (from == to) return value;
        if (PhysicalPointer(type)) return Op(std::string(native ? "OpConvertUToPtr " : "OpConvertPtrToU ") + to + " " + value);
        if (!ContainsPhysicalPointer(type)) return Op("OpCopyLogical " + to + " " + value);
        const auto function = BridgeFunction(type, native);
        return Op("OpFunctionCall " + to + " " + function + " " + value);
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
        resource_types[id] = name;
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
        return name;
    }
    void EnsureCapability(const std::string &capability) {
        auto line = "OpCapability " + capability + "\n";
        if (source_caps.str().find(line) == std::string::npos && code.capabilities.find(line) == std::string::npos)
            source_caps << line;
    }
    void EnsureExtension(const std::string &extension) {
        const auto line = "OpExtension \"" + extension + "\"\n";
        if (extensions.str().find(line) == std::string::npos) extensions << line;
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
            if ((i.op == spv::OpUndef || i.op == spv::OpConstantNull) && IsPointer(i.type) &&
                (Def(i.type).words.at(2) == spv::StorageClassStorageBuffer || PhysicalPointer(i.type)))
                declarations << SourceID(i.result) << " = " << (i.op == spv::OpUndef ? "OpUndef " : "OpConstantNull ")
                             << ValueType(i.type) << "\n";
            else if (i.op == spv::OpUndef || (i.op >= spv::OpConstantTrue && i.op <= spv::OpConstantNull))
                declarations << Rewrite(i.text, [&](uint32_t id) { return id == i.type ? ValueType(id) : Global(id); }) << "\n";
            else if (i.op >= spv::OpSpecConstantTrue && i.op <= spv::OpSpecConstantOp)
                Unsupported("specialization must run before state lowering");
            else if (i.op == spv::OpVariable) {
                if (IsPersistentStorage(i.words.at(3))) continue;
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
            if (i.op == spv::OpDecorate && i.words.at(2) == spv::DecorationArrayStride && Def(target).op == spv::OpTypePointer)
                continue;  // Native pointer declaration owns its exact stride.
            // Function/Private objects live in numeric frame storage. A
            // physical-pointer AliasedPointer/RestrictPointer decoration must
            // not be attached to the uint64 identity instead. Reconstructed
            // accesses conservatively alias; actual native resource memory
            // retains its original layout and alias decorations below.
            if (invocation_variables.count(target) || private_variables.count(target)) continue;
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
    bool ContainsNativeOpaque(uint32_t type) const {
        if (Ephemeral(type)) return true;
        const auto &d = Def(type);
        return (d.op == spv::OpTypeArray || d.op == spv::OpTypeRuntimeArray) && ContainsNativeOpaque(d.words.at(2));
    }
    bool RecipeValue(uint32_t id) const {
        const auto &d = Def(id);
        if (MatrixPointer(id)) return true;
        if (Ephemeral(d.type)) return true;
        if (!IsPointer(d.type)) return false;
        const auto &p = Def(d.type);
        return p.words.at(2) == spv::StorageClassImage ||
               (p.words.at(2) == spv::StorageClassUniformConstant && ContainsNativeOpaque(p.words.at(3)));
    }
    uint32_t PointerRouteID(const PointerRoute &route) {
        auto found = pointer_route_ids.find(route);
        if (found != pointer_route_ids.end()) return found->second;
        if (pointer_routes.size() >= UINT32_MAX) Unsupported("logical pointer route tags exceed uint32");
        auto id = static_cast<uint32_t>(pointer_routes.size());
        pointer_routes.push_back(route);
        pointer_route_ids.emplace(route, id);
        return id;
    }
    uint32_t SelectorWords(uint32_t type) const {
        const auto &t = Def(type);
        if (t.op != spv::OpTypeInt || !t.words.at(2) || t.words.at(2) > 64)
            Unsupported("logical pointer selector is not an integer of at most 64 bits");
        return (t.words.at(2) + 31) / 32;
    }
    uint32_t RouteWords(uint32_t route) const {
        uint64_t words = 1;
        for (const auto &index : pointer_routes.at(route).second)
            if (!index.constant) words += SelectorWords(index.type);
        if (words > UINT32_MAX / 4) Unsupported("logical pointer selector snapshot exceeds frame indexing");
        return static_cast<uint32_t>(words);
    }
    uint32_t RouteArrayStride(const PointerRoute &route) const {
        if (!route.first) return 0;
        auto type = Def(Def(route.first).type).words.at(3);
        uint32_t stride = 0;
        for (const auto &index : route.second) {
            // Arithmetic stays in the same innermost array. A subsequent
            // ordinary index enters a new subobject and establishes its own
            // array origin, if any.
            if (index.pointer_type) continue;
            const auto &d = Def(type);
            stride = 0;
            if (d.op == spv::OpTypeStruct) {
                if (!index.constant || Def(index.constant).op != spv::OpConstant)
                    Unsupported("logical pointer structure member is not constant");
                type = d.words.at(Def(index.constant).words.at(3) + 2);
            } else if (d.op == spv::OpTypeArray || d.op == spv::OpTypeRuntimeArray) {
                stride = PointerStride(type);
                type = d.words.at(2);
            } else if (d.op == spv::OpTypeVector || d.op == spv::OpTypeMatrix)
                type = d.words.at(2);
            else
                Unsupported("logical pointer route indexes a scalar");
        }
        return stride;
    }
    bool RouteArrayStrideMismatch(const PointerRoute &route, uint32_t pointer_type) const {
        const auto stride = PointerStride(pointer_type), origin = RouteArrayStride(route);
        return stride && origin && stride != origin;
    }
    PointerRoute ExtendPointerRoute(uint32_t base, const Instruction &access) const {
        auto route = pointer_routes.at(base);
        if (!route.first || RouteArrayStrideMismatch(route, Def(access.words.at(3)).type)) return {0, {}};
        const bool arithmetic = access.op == spv::OpPtrAccessChain || access.op == spv::OpInBoundsPtrAccessChain;
        if (arithmetic) {
            const auto pointer_type = CanonicalPointerType(Def(access.words.at(3)).type);
            if (!route.second.empty() && route.second.back().pointer_type && route.second.back().pointer_type != pointer_type)
                Unsupported("logical pointer arithmetic changes stride without a proven matching array origin");
            if (route.second.empty() || route.second.back().pointer_type != pointer_type)
                route.second.push_back({0, pointer_arithmetic_type, pointer_type});
        }
        for (size_t n = arithmetic ? 5 : 4; n < access.words.size(); ++n) {
            const auto index = access.words[n];
            const auto &d = Def(index);
            route.second.push_back(d.in_function ? PointerIndex{0, d.type, 0} : PointerIndex{index, 0, 0});
        }
        if (RouteArrayStrideMismatch(route, access.type)) return {0, {}};
        return route;
    }
    uint32_t PointerStride(uint32_t type) const {
        for (const auto &i : parsed.instructions)
            if (i.op == spv::OpDecorate && i.words.at(1) == type && i.words.at(2) == spv::DecorationArrayStride)
                return i.words.at(3);
        return 0;
    }
    uint32_t CanonicalPointerType(uint32_t type) const {
        const auto &definition = Def(type);
        const auto stride = PointerStride(type);
        for (const auto &[id, candidate] : definitions)
            if (candidate->op == spv::OpTypePointer && candidate->words.at(2) == definition.words.at(2) &&
                candidate->words.at(3) == definition.words.at(3) && PointerStride(id) == stride)
                return id;
        return type;
    }
    void PlanPointerRoutes() {
        bool needed = false;
        for (const auto &i : parsed.instructions)
            needed |= i.type && IsPointer(i.type) && Def(i.type).words.at(2) == spv::StorageClassStorageBuffer;
        if (!needed) return;
        for (const auto &i : parsed.instructions) {
            if (!i.result || !IsPointer(i.type) || Def(i.type).words.at(2) != spv::StorageClassStorageBuffer) continue;
            const bool access = i.op == spv::OpAccessChain || i.op == spv::OpInBoundsAccessChain || i.op == spv::OpPtrAccessChain ||
                                i.op == spv::OpInBoundsPtrAccessChain;
            if (!access) continue;
            const auto base_type = Def(i.words.at(3)).type;
            // A pointer's ArrayStride is optional on an ordinary access
            // chain. Dropping it and subsequently restoring the real array
            // stride does not change the address or the arithmetic route.
            // Two different nonzero strides cannot both describe that same
            // array element (OpAccessChain and ArrayStride definitions), even
            // when a source validator fails to diagnose the mismatch.
            if (Def(base_type).words.at(3) == Def(i.type).words.at(3) && PointerStride(base_type) && PointerStride(i.type) &&
                PointerStride(base_type) != PointerStride(i.type))
                Unsupported("same-pointee ArrayStride changes require a matching array origin");
        }
        // Zero is the null identity even inside a null-initialized aggregate.
        PointerRouteID({0, {}});
        for (const auto &[id, type] : definitions)
            if (type->op == spv::OpTypeInt && type->words.at(2) == 64 && type->words.at(3) == 0) {
                pointer_arithmetic_type = id;
                break;
            }
        for (const auto &i : parsed.instructions)
            if (i.op == spv::OpPtrAccessChain || i.op == spv::OpInBoundsPtrAccessChain) {
                if (!pointer_arithmetic_type) Unsupported("pointer arithmetic has no private uint64 selector type");
                NeedValueType(pointer_arithmetic_type);
                break;
            }
        std::set<uint32_t> non_uniform;
        for (const auto &i : parsed.instructions)
            if (i.op == spv::OpDecorate && i.words.at(2) == spv::DecorationNonUniform) non_uniform.insert(i.words.at(1));
        for (const auto &i : parsed.instructions) {
            if (!i.result || !IsPointer(i.type) || Def(i.type).words.at(2) != spv::StorageClassStorageBuffer ||
                (!i.in_function && !globals.count(i.result)))
                continue;
            auto &plan = pointer_plans[i.result];
            NeedValueType(i.type);
            plan.non_uniform = non_uniform.count(i.result) != 0;
            if (i.op == spv::OpAccessChain || i.op == spv::OpInBoundsAccessChain || i.op == spv::OpPtrAccessChain ||
                i.op == spv::OpInBoundsPtrAccessChain)
                for (size_t n = 4; n < i.words.size(); ++n) plan.non_uniform |= non_uniform.count(i.words[n]) != 0;
            if (!i.in_function) {
                if (i.op != spv::OpVariable && i.op != spv::OpConstantNull && i.op != spv::OpUndef)
                    Unsupported("logical pointer root is not a descriptor or null");
                plan.routes.insert(PointerRouteID({i.op == spv::OpVariable ? i.result : 0u, {}}));
            } else if (i.op == spv::OpLoad || i.op == spv::OpCompositeExtract) {
                // A null aggregate initializer needs no separate pointer null
                // instruction. Memory/aggregate dataflow adds other identities
                // at the fixed point below; zero also represents poison paths.
                plan.routes.insert(0);
                plan.conservative = true;
            }
        }
        std::set<uint32_t> invalid_stride_results;
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto &i : parsed.instructions) {
                auto result = pointer_plans.find(i.result);
                if (result == pointer_plans.end() || !i.in_function) continue;
                auto inherit = [&](uint32_t source, bool extend) {
                    const auto found = pointer_plans.find(source);
                    if (found == pointer_plans.end()) Unsupported("logical pointer has no typed source route");
                    if (found->second.non_uniform && !result->second.non_uniform) {
                        result->second.non_uniform = true;
                        changed = true;
                    }
                    if (found->second.conservative && !result->second.conservative) {
                        result->second.conservative = true;
                        changed = true;
                    }
                    // A zero-index chain may refer back through Phi. Copy the
                    // set before insertion, so the fixed point never mutates
                    // the container currently being traversed.
                    const auto parents = found->second.routes;
                    for (auto route : parents) {
                        const auto extended = extend ? ExtendPointerRoute(route, i) : PointerRoute{};
                        if (extend && pointer_routes.at(route).first && !extended.first) {
                            // Pointer-valued loads conservatively join every
                            // same-typed stored identity. A restored stride
                            // rules out roots belonging to another array
                            // layout; those are not executable defined paths.
                            invalid_stride_results.insert(i.result);
                            continue;
                        }
                        const auto target = extend ? PointerRouteID(extended) : route;
                        changed |= result->second.routes.insert(target).second;
                    }
                };
                if (i.op == spv::OpAccessChain || i.op == spv::OpInBoundsAccessChain || i.op == spv::OpPtrAccessChain ||
                    i.op == spv::OpInBoundsPtrAccessChain)
                    inherit(i.words.at(3), true);
                else if (i.op == spv::OpCopyObject)
                    inherit(i.words.at(3), false);
                else if (i.op == spv::OpSelect) {
                    inherit(i.words.at(4), false);
                    inherit(i.words.at(5), false);
                } else if (i.op == spv::OpPhi) {
                    for (size_t n = 3; n < i.words.size(); n += 2) inherit(i.words[n], false);
                } else if (i.op == spv::OpLoad || i.op == spv::OpCompositeExtract) {
                    // Conservatively join all same-typed identities that can
                    // be stored in invocation-private memory/composites. The
                    // runtime tag and actual dynamic cell offset stay exact.
                    for (const auto &[source, plan] : pointer_plans) {
                        (void)plan;
                        if (CanonicalPointerType(Def(source).type) == CanonicalPointerType(i.type)) inherit(source, false);
                    }
                } else
                    Unsupported("logical pointer SSA operation requires an explicit identity encoding");
            }
        }
        for (auto &[id, plan] : pointer_plans) {
            const bool nonnull = std::any_of(plan.routes.begin(), plan.routes.end(),
                                             [&](uint32_t route) { return pointer_routes.at(route).first != 0; });
            if (invalid_stride_results.count(id) && !nonnull && !plan.conservative)
                Unsupported("logical pointer ArrayStride disagrees with its originating array layout");
            if (plan.routes.empty()) Unsupported("logical pointer Phi has no reachable descriptor root");
            for (auto route : plan.routes) plan.words = std::max(plan.words, RouteWords(route));
            pointer_words = std::max(pointer_words, plan.words);
        }
        for (auto &[id, plan] : pointer_plans) {
            (void)id;
            plan.words = pointer_words;
        }
    }
    void PlanPointerSnapshots(uint64_t &size) {
        const auto begin = size;
        for (auto &[id, plan] : pointer_plans) {
            if (!Def(id).in_function) continue;
            size = (size + 3) & ~uint64_t(3);
            const uint64_t bytes = uint64_t(plan.words) * 4;
            if (size > UINT32_MAX || bytes > UINT32_MAX - size)
                Unsupported("logical pointer snapshots exceed 32-bit frame indexing");
            plan.offset = static_cast<uint32_t>(size);
            size += bytes;
            ++output.pointer_snapshot_values;
        }
        output.pointer_snapshot_bytes = size - begin;
    }
    std::vector<std::string> ReadPointerSnapshot(uint32_t id) {
        const auto &plan = pointer_plans.at(id);
        std::vector<std::string> result;
        if (!Def(id).in_function) {
            result.push_back(Constant(*plan.routes.begin()));
            return result;
        }
        for (uint32_t n = 0; n < plan.words; ++n)
            result.push_back(
                Op("OpFunctionCall " + code.word_type + " " + word_codec->load + " %sc_base " + Constant(plan.offset + n * 4)));
        return result;
    }
    void WritePointerSnapshot(uint32_t id, const std::vector<std::string> &words) {
        const auto &plan = pointer_plans.at(id);
        for (uint32_t n = 0; n < plan.words; ++n)
            Op("OpFunctionCall " + code.void_type + " " + word_codec->store + " %sc_base " + Constant(plan.offset + n * 4) + " " +
               (n < words.size() ? words[n] : Constant(0)));
    }
    std::vector<std::string> EncodeSelector(uint32_t id) {
        auto value = Value(id);
        const auto type = Def(id).type;
        const auto width = Def(type).words.at(2);
        if (width == 64) {
            auto pair = Op("OpBitcast " + code.word_pair_type + " " + value);
            return {Op("OpCompositeExtract " + code.word_type + " " + pair + " 0"),
                    Op("OpCompositeExtract " + code.word_type + " " + pair + " 1")};
        }
        if (Type(type) != code.word_type)
            value = Op(std::string(width == 32 ? "OpBitcast " : "OpUConvert ") + code.word_type + " " + value);
        return {value};
    }
    std::string ArithmeticSelector(uint32_t id) {
        if (!pointer_arithmetic_type) Unsupported("pointer arithmetic requires the private uint64 selector type");
        auto value = Value(id);
        const auto width = Def(Def(id).type).words.at(2);
        // SPIR-V treats Element as signed regardless of its type's Signedness.
        return Op(std::string(width == 64 ? "OpBitcast " : "OpSConvert ") + ValueType(pointer_arithmetic_type) + " " + value);
    }
    std::vector<std::string> ArithmeticWords(const std::string &value) {
        const auto pair = Op("OpBitcast " + code.word_pair_type + " " + value);
        return {Op("OpCompositeExtract " + code.word_type + " " + pair + " 0"),
                Op("OpCompositeExtract " + code.word_type + " " + pair + " 1")};
    }
    std::string DecodeSelector(uint32_t type, const std::vector<std::string> &words, size_t &offset) {
        const auto width = Def(type).words.at(2);
        auto value = words.at(offset++);
        if (width == 64) {
            auto pair = Op("OpCompositeConstruct " + code.word_pair_type + " " + value + " " + words.at(offset++));
            return Op("OpBitcast " + Type(type) + " " + pair);
        }
        if (Type(type) != code.word_type)
            value = Op(std::string(width == 32 ? "OpBitcast " : "OpUConvert ") + Type(type) + " " + value);
        return value;
    }
    void CapturePointerSnapshot(const Instruction &i) {
        if (!pointer_plans.count(i.result)) return;
        if (i.op == spv::OpCopyObject) {
            WritePointerSnapshot(i.result, ReadPointerSnapshot(i.words.at(3)));
            return;
        }
        if (i.op == spv::OpSelect) {
            const auto condition = Value(i.words.at(3));
            const auto yes = ReadPointerSnapshot(i.words.at(4)), no = ReadPointerSnapshot(i.words.at(5));
            std::vector<std::string> result;
            for (uint32_t n = 0; n < pointer_plans.at(i.result).words; ++n)
                result.push_back(Op("OpSelect " + code.word_type + " " + condition + " " + (n < yes.size() ? yes[n] : Constant(0)) +
                                    " " + (n < no.size() ? no[n] : Constant(0))));
            WritePointerSnapshot(i.result, result);
            return;
        }
        const auto base = i.words.at(3);
        const auto parent = ReadPointerSnapshot(base);
        const bool arithmetic = i.op == spv::OpPtrAccessChain || i.op == spv::OpInBoundsPtrAccessChain;
        const auto element = arithmetic ? ArithmeticSelector(i.words.at(4)) : std::string();
        std::vector<std::string> appended;
        for (size_t n = arithmetic ? 5 : 4; n < i.words.size(); ++n)
            if (Def(i.words[n]).in_function) {
                const auto encoded = EncodeSelector(i.words[n]);
                appended.insert(appended.end(), encoded.begin(), encoded.end());
            }
        const auto &routes = pointer_plans.at(base).routes;
        const auto merge = Fresh();
        std::vector<std::pair<uint32_t, std::string>> labels;
        for (auto route : routes) labels.push_back({route, Fresh()});
        if (labels.size() > 1) {
            functions << "OpSelectionMerge " << merge << " None\nOpSwitch " << parent[0] << " " << labels[0].second;
            for (const auto &[route, label] : labels) functions << " " << route << " " << label;
            functions << "\n";
        }
        for (const auto &[route, label] : labels) {
            if (labels.size() > 1) functions << label << " = OpLabel\n";
            const auto extended = ExtendPointerRoute(route, i);
            if (!extended.first) {
                // An impossible conservative candidate, or undefined source
                // arithmetic on null, needs no native address reconstruction.
                WritePointerSnapshot(i.result, {Constant(0)});
                if (labels.size() > 1) functions << "OpBranch " << merge << "\n";
                continue;
            }
            std::vector<std::string> result{Constant(pointer_route_ids.at(extended))};
            result.insert(result.end(), parent.begin() + 1, parent.begin() + RouteWords(route));
            if (arithmetic) {
                const auto &path = pointer_routes.at(route).second;
                const auto canonical = CanonicalPointerType(Def(base).type);
                auto delta = element;
                if (!path.empty() && path.back().pointer_type == canonical) {
                    size_t offset = result.size() - 2;
                    const auto previous = DecodeSelector(pointer_arithmetic_type, result, offset);
                    delta = Op("OpIAdd " + ValueType(pointer_arithmetic_type) + " " + previous + " " + element);
                    result.resize(result.size() - 2);
                }
                const auto encoded = ArithmeticWords(delta);
                result.insert(result.end(), encoded.begin(), encoded.end());
            }
            result.insert(result.end(), appended.begin(), appended.end());
            WritePointerSnapshot(i.result, result);
            if (labels.size() > 1) functions << "OpBranch " << merge << "\n";
        }
        if (labels.size() > 1) functions << merge << " = OpLabel\n";
    }
    std::string RebuildPointerSnapshot(uint32_t id) {
        const auto &routes = pointer_plans.at(id).routes;
        const auto active = active_pointer_routes.find(id);
        if (active == active_pointer_routes.end() && routes.size() != 1)
            Unsupported("variable logical pointer must be consumed inside its selected route");
        const auto &route = pointer_routes.at(active == active_pointer_routes.end() ? *routes.begin() : active->second);
        if (!route.first) {
            const auto type = Type(Def(id).type);
            auto found = null_pointers.find(type);
            if (found == null_pointers.end()) {
                const auto value = Fresh();
                declarations << value << " = OpConstantNull " << type << "\n";
                found = null_pointers.emplace(type, value).first;
            }
            return found->second;
        }
        if (route.second.empty()) return Global(route.first);
        const auto words = ReadPointerSnapshot(id);
        auto pointer = Global(route.first);
        std::string indexes;
        size_t offset = 1;
        for (size_t n = 0; n < route.second.size(); ++n) {
            const auto &index = route.second[n];
            if (index.pointer_type) {
                if (!indexes.empty()) {
                    pointer = Op("OpAccessChain " + Type(index.pointer_type) + " " + pointer + indexes);
                    indexes.clear();
                }
                const auto element = DecodeSelector(index.type, words, offset);
                const auto result_type = n + 1 == route.second.size() ? Type(Def(id).type) : Type(index.pointer_type);
                pointer = Op("OpPtrAccessChain " + result_type + " " + pointer + " " + element);
            } else
                indexes += " " + (index.constant ? Global(index.constant) : DecodeSelector(index.type, words, offset));
        }
        if (!indexes.empty()) pointer = Op("OpAccessChain " + Type(Def(id).type) + " " + pointer + indexes);
        DecoratePointerSnapshot(id, pointer);
        return pointer;
    }
    bool RoutedPointerOperation(const Instruction &i) {
        const auto opcode_name = std::string(spvOpcodeString(i.op));
        const bool native = (i.op == spv::OpLoad && Storage(i.words.at(3)) == spv::StorageClassStorageBuffer) ||
                            (i.op == spv::OpStore && Storage(i.words.at(1)) == spv::StorageClassStorageBuffer) ||
                            i.op == spv::OpCopyMemory || i.op == spv::OpArrayLength || opcode_name.find("Atomic") == 0 ||
                            i.op == spv::OpPtrEqual || i.op == spv::OpPtrNotEqual || i.op == spv::OpPtrDiff;
        if (!native) return false;
        std::vector<uint32_t> pointers;
        bool needed = false;
        for (auto id : i.uses) {
            const auto found = pointer_plans.find(id);
            if (found == pointer_plans.end() || std::find(pointers.begin(), pointers.end(), id) != pointers.end()) continue;
            pointers.push_back(id);
            needed |= found->second.routes.size() != 1 || !pointer_routes.at(*found->second.routes.begin()).first;
        }
        if (!needed) return false;
        // Keep native pointers within a statically rooted case. Only the
        // selected operation executes, including volatile/atomic accesses.
        // The normal numeric Function registers merge results after the
        // switch; no native logical pointer Phi/Select is required downstream.
        std::function<void(size_t)> emit = [&](size_t index) {
            if (index == pointers.size()) {
                const auto opcode = std::string(spvOpcodeString(i.op));
                const bool memory =
                    i.op == spv::OpLoad || i.op == spv::OpStore || i.op == spv::OpCopyMemory || opcode.find("Atomic") == 0;
                bool null = false;
                for (const auto &[id, route] : active_pointer_routes) {
                    (void)id;
                    null |= !pointer_routes.at(route).first;
                }
                if (memory && null) {
                    // Dereferencing a logical null pointer is undefined. Do
                    // not issue an invalid native memory access on that path;
                    // all defined, selected non-null paths remain unchanged.
                    if (i.result) {
                        const auto value = Fresh();
                        declarations << value << " = OpUndef " << Type(i.type) << "\n";
                        Save(i.result, value);
                    }
                } else if (CopyPersistentMemory(i)) {
                } else if (i.op == spv::OpCopyMemory) {
                    functions << Rewrite(i.text, [&](uint32_t id) { return NativeValue(id); }) << "\n";
                } else if (i.op == spv::OpStore) {
                    const auto value_id = i.words.at(2);
                    const auto type = Def(value_id).type;
                    auto value = Value(value_id);
                    value = Bridge(type, value, true);
                    functions << Rewrite(i.text, [&](uint32_t id) { return id == value_id ? value : NativeValue(id); }) << "\n";
                } else if (i.op == spv::OpAtomicStore || i.op == spv::OpAtomicFlagClear)
                    functions << Rewrite(i.text, [&](uint32_t id) { return NativeValue(id); }) << "\n";
                else
                    Generic(i);
                return;
            }
            const auto id = pointers[index];
            const auto &routes = pointer_plans.at(id).routes;
            if (routes.size() == 1) {
                active_pointer_routes[id] = *routes.begin();
                emit(index + 1);
                active_pointer_routes.erase(id);
                return;
            }
            const auto words = ReadPointerSnapshot(id);
            const auto merge = Fresh();
            std::vector<std::pair<uint32_t, std::string>> labels;
            for (auto route : routes) labels.push_back({route, Fresh()});
            functions << "OpSelectionMerge " << merge << " None\nOpSwitch " << words[0] << " " << labels[0].second;
            for (const auto &[route, label] : labels) functions << " " << route << " " << label;
            functions << "\n";
            for (const auto &[route, label] : labels) {
                functions << label << " = OpLabel\n";
                active_pointer_routes[id] = route;
                emit(index + 1);
                functions << "OpBranch " << merge << "\n";
            }
            active_pointer_routes.erase(id);
            functions << merge << " = OpLabel\n";
        };
        emit(0);
        return true;
    }
    void DecoratePointerSnapshot(uint32_t id, const std::string &result) {
        DecorateResult(id, result, true);
        if (!pointer_plans.at(id).non_uniform) return;
        bool direct = false;
        for (const auto &i : parsed.instructions)
            direct |= i.op == spv::OpDecorate && i.words.at(1) == id && i.words.at(2) == spv::DecorationNonUniform;
        if (!direct) annotations << "OpDecorate " << result << " NonUniform\n";
    }
    void PlanResourceRecipes(uint64_t &size, const std::vector<ContinuationTypeLayout> &layouts) {
        const auto begin = size;
        auto allocate = [&](uint32_t type) {
            const auto layout = std::find_if(layouts.begin(), layouts.end(), [&](const auto &l) { return l.type == type; });
            if (layout == layouts.end()) Unsupported("resource provenance selector has no numeric typed layout");
            size = (size + layout->alignment - 1) & ~uint64_t(layout->alignment - 1);
            if (size > UINT32_MAX || layout->size > UINT32_MAX - size)
                Unsupported("resource provenance snapshots exceed 32-bit frame indexing");
            const auto offset = static_cast<uint32_t>(size);
            size += layout->size;
            ++output.resource_snapshot_fields;
            return offset;
        };
        std::set<uint32_t> pending;
        std::function<const ResourceRecipe &(uint32_t)> plan = [&](uint32_t id) -> const ResourceRecipe & {
            auto found = resource_recipes.find(id);
            if (found != resource_recipes.end()) return found->second;
            if (!pending.insert(id).second)
                Unsupported("cyclic opaque provenance requires an explicitly supported pointer representation");
            const auto &d = Def(id);
            ResourceRecipe recipe;
            auto root = std::make_shared<RecipeNode>();
            root->source = id;
            recipe.root = root;
            if (!d.in_function) {
                if (d.op != spv::OpVariable || Storage(id) != spv::StorageClassUniformConstant)
                    Unsupported("opaque resource provenance must originate in a traditional UniformConstant descriptor");
            } else {
                if (d.op != spv::OpAccessChain && d.op != spv::OpInBoundsAccessChain &&
                    !(MatrixPointer(id) && (d.op == spv::OpPtrAccessChain || d.op == spv::OpInBoundsPtrAccessChain)) &&
                    d.op != spv::OpCopyObject && d.op != spv::OpLoad && d.op != spv::OpSampledImage && d.op != spv::OpImage &&
                    d.op != spv::OpImageTexelPointer)
                    Unsupported("unsupported opaque resource provenance Op" + std::string(spvOpcodeString(d.op)));
                if (d.op == spv::OpLoad && (Storage(d.words.at(3)) != spv::StorageClassUniformConstant || d.words.size() != 4))
                    Unsupported(
                        "opaque resource loads require immutable traditional descriptor provenance without memory operands");
                std::function<std::shared_ptr<RecipeNode>(const std::shared_ptr<RecipeNode> &)> clone = [&](const auto &node) {
                    auto copy = std::make_shared<RecipeNode>();
                    copy->source = node->source;
                    copy->type = node->type;
                    if (node->type) {
                        copy->offset = allocate(node->type);
                        recipe.captures.push_back({node->type, copy->offset, 0, node->offset});
                    }
                    for (const auto &[operand, child] : node->operands) copy->operands.push_back({operand, clone(child)});
                    return copy;
                };
                for (auto operand : d.uses) {
                    if (RecipeValue(operand)) {
                        const auto &parent = plan(operand);
                        root->operands.push_back({operand, clone(parent.root)});
                    } else {
                        const auto &value = Def(operand);
                        auto child = std::make_shared<RecipeNode>();
                        child->source = operand;
                        if (value.in_function) {
                            if (!Layoutable(value.type)) Unsupported("resource provenance operand is not a numeric value");
                            child->type = value.type;
                            child->offset = allocate(value.type);
                            recipe.captures.push_back({value.type, child->offset, operand, 0});
                        }
                        root->operands.push_back({operand, child});
                    }
                }
            }
            pending.erase(id);
            return resource_recipes.emplace(id, std::move(recipe)).first->second;
        };
        for (const auto &i : parsed.instructions)
            if (i.in_function && i.result && RecipeValue(i.result)) plan(i.result);
        output.resource_snapshot_bytes = size - begin;
    }
    void CaptureRecipe(uint32_t id) {
        const auto found = resource_recipes.find(id);
        if (found == resource_recipes.end()) return;
        for (const auto &field : found->second.captures) {
            auto value = field.value ? Value(field.value)
                                     : Op("OpFunctionCall " + ValueType(field.type) + " " + codecs.at(field.type).load +
                                          " %sc_base " + Constant(field.inherited_offset));
            Op("OpFunctionCall " + code.void_type + " " + codecs.at(field.type).store + " %sc_base " + Constant(field.offset) +
               " " + value);
        }
    }
    std::string RebuildRecipe(const std::shared_ptr<RecipeNode> &node) {
        if (node->type) {
            auto value = Op("OpFunctionCall " + ValueType(node->type) + " " + codecs.at(node->type).load + " %sc_base " +
                            Constant(node->offset));
            if (PhysicalPointer(node->type)) {
                value = Op("OpConvertUToPtr " + Type(node->type) + " " + value);
                DecorateResult(node->source, value, true);
            }
            return value;
        }
        const auto &d = Def(node->source);
        if (!d.in_function) return Global(node->source);
        std::map<uint32_t, std::string> operands;
        for (const auto &[id, child] : node->operands) operands[id] = RebuildRecipe(child);
        const auto result = Fresh();
        functions << Rewrite(d.text, [&](uint32_t id) {
            if (id == d.result) return result;
            if (id == d.type) return Type(id);
            return operands.at(id);
        }) << "\n";
        DecorateResult(node->source, result, true);
        return result;
    }
    std::string Value(uint32_t id) {
        const auto &d = Def(id);
        if (PhysicalPointer(d.type)) {
            if (!d.in_function) return Global(id);
            auto result = Op("OpLoad " + ValueType(d.type) + " " + Register(id));
            DecorateResult(id, result, true);
            return result;
        }
        if (IsPointer(d.type)) {
            if (pointer_plans.count(id)) {
                const auto words = ReadPointerSnapshot(id);
                std::string construct = "OpCompositeConstruct " + ValueType(d.type);
                for (uint32_t n = 0; n < pointer_words; ++n) construct += " " + (n < words.size() ? words[n] : Constant(0));
                return Op(construct);
            }
            return RealPointer(id);
        }
        if (!d.in_function) return Global(id);
        if (!d.result || !d.type || Def(d.type).op == spv::OpTypeVoid) Unsupported("non-value source operand");
        if (Ephemeral(d.type)) {
            auto found = ephemeral_values.find(id);
            if (found != ephemeral_values.end()) return found->second;
            auto recipe = resource_recipes.find(id);
            if (recipe == resource_recipes.end()) Unsupported("opaque resource has no proven descriptor identity");
            return ephemeral_values[id] = RebuildRecipe(recipe->second.root);
        }
        auto result = Op("OpLoad " + ValueType(d.type) + " " + Register(id));
        DecorateResult(id, result, true);
        return result;
    }
    void Save(uint32_t id, const std::string &value) {
        if (pointer_plans.count(id)) {
            std::vector<std::string> words;
            for (uint32_t n = 0; n < pointer_words; ++n)
                words.push_back(Op("OpCompositeExtract " + code.word_type + " " + value + " " + std::to_string(n)));
            WritePointerSnapshot(id, words);
        } else if (Ephemeral(Def(id).type))
            ephemeral_values[id] = value;
        else
            functions << "OpStore " << Register(id) << " " << value << "\n";
    }
    uint32_t Storage(uint32_t pointer) const { return Def(Def(pointer).type).words.at(2); }
    std::string NativeValue(uint32_t id) {
        const auto &d = Def(id);
        if (MatrixPointer(id)) return RebuildRecipe(resource_recipes.at(id).root);
        if (PhysicalPointer(d.type)) {
            auto pointer = Op("OpConvertUToPtr " + Type(d.type) + " " + Value(id));
            DecorateResult(id, pointer, true);
            return pointer;
        }
        return IsPointer(d.type) && pointer_plans.count(id) ? RealPointer(id) : Value(id);
    }
    std::string Offset(uint32_t pointer) {
        auto system = builtin_variables.find(pointer);
        if (system != builtin_variables.end()) return Constant(system->second->byte_offset);
        auto invocation = invocation_variables.find(pointer);
        if (invocation != invocation_variables.end()) return Constant(static_cast<uint32_t>(invocation->second->offset));
        auto private_variable = private_variables.find(pointer);
        if (private_variable != private_variables.end()) return Constant(static_cast<uint32_t>(private_variable->second->offset));
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
        if (PhysicalPointer(d.type)) return NativeValue(id);
        if (IsPersistentStorage(Storage(id)) || SystemPointer(id)) Unsupported("persistent pointer used outside typed load/store");
        if (pointer_plans.count(id)) return RebuildPointerSnapshot(id);
        if (resource_recipes.count(id)) {
            auto found = ephemeral_pointers.find(id);
            if (found != ephemeral_pointers.end()) return found->second;
            return ephemeral_pointers[id] = RebuildRecipe(resource_recipes.at(id).root);
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
        std::vector<std::pair<uint32_t, std::vector<std::string>>> pointer_copies;
        for (auto i : found->body) {
            if (i->op != spv::OpPhi) continue;
            bool matched = false;
            for (size_t n = 3; n < i->words.size(); n += 2) {
                if (i->words.at(n + 1) == predecessor) {
                    if (pointer_plans.count(i->result))
                        pointer_copies.push_back({i->result, ReadPointerSnapshot(i->words[n])});
                    else
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
        for (const auto &[destination, value] : pointer_copies) WritePointerSnapshot(destination, value);
    }
    void Resume(const Instruction &i) {
        const auto &site = *sites.at(i.offset);
        auto restored = Op("OpFunctionCall " + site.restored_type + " " + site.restore + " %sc_base");
        for (size_t n = 0; n < site.saved_values.size(); ++n) {
            auto id = site.saved_values[n];
            Save(id, Op("OpCompositeExtract " + ValueType(Def(id).type) + " " + restored + " " + std::to_string(n)));
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
        if (!i.result || !i.type || (IsPointer(i.type) && !PhysicalPointer(i.type) && !pointer_plans.count(i.result)) ||
            Def(i.type).op == spv::OpTypeVoid || i.op == spv::OpFunctionCall)
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
        const auto opcode = std::string(spvOpcodeString(i.op));
        const bool address_value =
            PhysicalPointer(i.type) && (i.op == spv::OpSelect || i.op == spv::OpCopyObject || i.op == spv::OpCompositeExtract);
        const bool native = i.op == spv::OpLoad || i.op == spv::OpArrayLength || opcode.find("Atomic") == 0 ||
                            i.op == spv::OpPtrEqual || i.op == spv::OpPtrNotEqual || i.op == spv::OpPtrDiff ||
                            i.op == spv::OpAccessChain || i.op == spv::OpInBoundsAccessChain || i.op == spv::OpPtrAccessChain ||
                            i.op == spv::OpInBoundsPtrAccessChain || i.op == spv::OpConvertPtrToU || i.op == spv::OpBitcast;
        for (auto id : i.uses)
            if (!loaded.count(id)) loaded[id] = native ? NativeValue(id) : Value(id);
        const auto result_type = i.op == spv::OpLoad                         ? ResourceType(i.type)
                                 : PhysicalPointer(i.type) && !address_value ? Type(i.type)
                                                                             : ValueType(i.type);
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
        if (result_type != ValueType(i.type)) result = Bridge(i.type, result, false);
        DecorateResult(i.result, result);
        Save(i.result, result);
        CaptureRecipe(i.result);
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
    struct PersistentAccess {
        uint32_t flags = 0, alignment = 0;
    };
    PersistentAccess ParsePersistentAccess(const Instruction &i, size_t &cursor) const {
        if (cursor == i.words.size()) return {};
        PersistentAccess access;
        const auto mask = i.words.at(cursor++);
        const uint32_t supported = spv::MemoryAccessVolatileMask | spv::MemoryAccessAlignedMask | spv::MemoryAccessNontemporalMask;
        if (mask & ~supported) Unsupported("persistent memory operands require additional source memory-model support");
        access.flags = mask & (spv::MemoryAccessVolatileMask | spv::MemoryAccessNontemporalMask);
        if (mask & spv::MemoryAccessAlignedMask) {
            if (cursor == i.words.size()) Unsupported("persistent Aligned operand has no alignment");
            access.alignment = i.words.at(cursor++);
            if (!access.alignment || (access.alignment & (access.alignment - 1)))
                Unsupported("persistent Aligned operand is not a nonzero power of two");
        }
        // Aligned promises the source object's alignment. Relocation uses a
        // different, numeric identity/value representation; its codecs already
        // use the proven alignment of each actual backing word (BDA Aligned4).
        // Copying the source promise to every backing word would be incorrect.
        return access;
    }
    std::string QualifiedCodec(uint32_t type, bool store, PersistentAccess access) {
        const auto &codec = codecs.at(type);
        const auto &original = store ? codec.store : codec.load;
        if (!access.flags) return original;
        auto found = qualified_codecs.find(access.flags);
        if (found != qualified_codecs.end()) return found->second.at(original);

        // Specialize the existing typed codec graph, including aggregate and
        // pointer-identity children. Only real frame-word accesses get source
        // qualifiers; compiler-owned Function counters/temporaries do not.
        using Tokens = std::vector<std::string>;
        std::vector<Tokens> lines;
        std::set<std::string> names, frame_pointers;
        for (const auto &[id, value] : codecs) {
            (void)id;
            names.insert(value.load);
            names.insert(value.store);
        }
        std::istringstream input(code.functions);
        std::string line, function;
        while (std::getline(input, line)) {
            std::istringstream stream(line);
            Tokens tokens;
            for (std::string token; stream >> token;) tokens.push_back(std::move(token));
            if (tokens.size() > 2 && tokens[2] == "OpFunction") function = tokens[0];
            if (!names.count(function)) continue;
            if (tokens.size() > 3 && (tokens[2] == "OpAccessChain" || tokens[2] == "OpConvertUToPtr") &&
                tokens[3] == code.word_pointer_type)
                frame_pointers.insert(tokens[0]);
            lines.push_back(tokens);
            if (!tokens.empty() && tokens[0] == "OpFunctionEnd") function.clear();
        }
        std::map<std::string, std::string> replacements;
        for (const auto &tokens : lines)
            if (tokens.size() > 1 && tokens[1] == "=") replacements.emplace(tokens[0], Fresh());
        auto &specialized = qualified_codecs[access.flags];
        for (const auto &name : names) specialized[name] = replacements.at(name);
        std::string qualifiers;
        if (access.flags & spv::MemoryAccessVolatileMask) qualifiers = "Volatile";
        if (access.flags & spv::MemoryAccessNontemporalMask)
            qualifiers += (qualifiers.empty() ? "" : "|") + std::string("Nontemporal");
        for (auto tokens : lines) {
            size_t mask = SIZE_MAX;
            if (tokens.size() > 4 && tokens[2] == "OpLoad" && frame_pointers.count(tokens[4])) mask = 5;
            if (tokens.size() > 2 && tokens[0] == "OpStore" && frame_pointers.count(tokens[1])) mask = 3;
            if (mask != SIZE_MAX) {
                if (tokens.size() == mask)
                    tokens.push_back(qualifiers);
                else
                    tokens[mask] = qualifiers + (tokens[mask] == "None" ? "" : "|" + tokens[mask]);
            }
            for (auto &token : tokens) {
                const auto replacement = replacements.find(token);
                if (replacement != replacements.end()) token = replacement->second;
            }
            for (size_t n = 0; n < tokens.size(); ++n) qualified_functions << (n ? " " : "") << tokens[n];
            qualified_functions << "\n";
        }
        return specialized.at(original);
    }
    bool CopyPersistentMemory(const Instruction &i) {
        if (i.op != spv::OpCopyMemory) return false;
        const auto target = i.words.at(1), source = i.words.at(2);
        const bool persistent_target = IsPersistentStorage(Storage(target));
        const bool persistent_source = IsPersistentStorage(Storage(source)) || SystemPointer(source);
        if (!persistent_target && !persistent_source) return false;
        for (auto pointer : {target, source}) {
            const auto found = pointer_plans.find(pointer);
            if (found == pointer_plans.end() || active_pointer_routes.count(pointer)) continue;
            if (found->second.routes.size() != 1 || !pointer_routes.at(*found->second.routes.begin()).first)
                return false;  // The outer native-root switch must select this operation first.
        }
        // Copy the logical value, including any numeric pointer identities,
        // rather than copying the implementation's native pointer bytes. A
        // complete source value is read before the target is written, so
        // aliases and whole aggregates retain ordinary CopyMemory semantics.
        size_t cursor = 3;
        const auto target_access = ParsePersistentAccess(i, cursor);
        const auto source_access = cursor == i.words.size() ? target_access : ParsePersistentAccess(i, cursor);
        if (cursor != i.words.size()) Unsupported("extra persistent CopyMemory operands");
        const auto type = Def(Def(target).type).words.at(3);
        auto native_operands = [&](PersistentAccess access, uint32_t pointer) {
            std::string mask;
            if (access.flags & spv::MemoryAccessVolatileMask) mask = "Volatile";
            if (access.flags & spv::MemoryAccessNontemporalMask) mask += (mask.empty() ? "" : "|") + std::string("Nontemporal");
            auto alignment = access.alignment;
            if (PhysicalResourcePointer(pointer)) alignment = std::max(alignment, ScalarAlignment(type));
            if (alignment) mask += (mask.empty() ? "" : "|") + std::string("Aligned");
            return mask.empty() ? std::string() : " " + mask + (alignment ? " " + std::to_string(alignment) : "");
        };
        std::string value;
        if (persistent_source)
            value = Op("OpFunctionCall " + ValueType(type) + " " + QualifiedCodec(type, false, source_access) + " %sc_base " +
                       Offset(source));
        else {
            value = Op("OpLoad " + ResourceType(type) + " " + NativeValue(source) + native_operands(source_access, source));
            value = Bridge(type, value, false);
        }
        if (persistent_target)
            Op("OpFunctionCall " + code.void_type + " " + QualifiedCodec(type, true, target_access) + " %sc_base " +
               Offset(target) + " " + value);
        else {
            value = Bridge(type, value, true);
            const auto pointer = NativeValue(target);
            functions << "OpStore " << pointer << " " << value << native_operands(target_access, target) << "\n";
        }
        return true;
    }
    void Body(const State &state) {
        ephemeral_values.clear();
        ephemeral_pointers.clear();
        if (state.number == 0) {
            // The source entry block cannot be a branch target. It executes
            // exactly once in each logical activation, including a recursive
            // activation with a different frame. Resume states skip this code.
            for (const auto &variable : output.private_variables) {
                const auto &source = Def(variable.variable);
                if (source.words.size() > 4)
                    Op("OpFunctionCall " + code.void_type + " " + codecs.at(variable.type).store + " %sc_base " +
                       Constant(static_cast<uint32_t>(variable.offset)) + " " + Value(source.words.at(4)));
            }
        }
        if (state.resume) Resume(*state.resume);
        for (size_t n = state.begin; n < state.end; ++n) {
            const auto &i = *state.block->body[n];
            if (i.op == spv::OpPhi || i.op == spv::OpSelectionMerge || i.op == spv::OpLoopMerge || i.op == spv::OpLine ||
                i.op == spv::OpNoLine)
                continue;
            if (i.result && MatrixPointer(i.result)) {
                // Preserve the struct-root address and every selector at the
                // definition. Rebuild only at the actual matrix Load/Store;
                // the restricted pointer itself is never converted or spilled.
                CaptureRecipe(i.result);
                continue;
            }
            if (PhysicalPointer(i.type) && !(i.op == spv::OpLoad && IsPersistentStorage(Storage(i.words.at(3))))) {
                if (RoutedPointerOperation(i)) continue;
                Generic(i);
                continue;
            }
            if (i.op == spv::OpVariable) {
                if (i.words.size() > 4) {
                    const auto &slot = *slots.at(i.result);
                    Op("OpFunctionCall " + code.void_type + " " + codecs.at(slot.type).store + " %sc_base " +
                       Constant(static_cast<uint32_t>(slot.offset)) + " " + Value(i.words.at(4)));
                }
                continue;
            }
            if (i.op == spv::OpAccessChain || i.op == spv::OpInBoundsAccessChain || i.op == spv::OpPtrAccessChain ||
                i.op == spv::OpInBoundsPtrAccessChain ||
                (i.op == spv::OpCopyObject && IsPointer(i.type) && Def(i.type).words.at(2) != spv::StorageClassImage)) {
                CapturePointerSnapshot(i);
                CaptureRecipe(i.result);
                continue;
            }
            if (i.op == spv::OpSelect && pointer_plans.count(i.result)) {
                CapturePointerSnapshot(i);
                continue;
            }
            if (i.op == spv::OpImageTexelPointer || (i.op == spv::OpCopyObject && IsPointer(i.type))) {
                auto result = Fresh();
                functions << Rewrite(i.text, [&](uint32_t id) {
                    if (id == i.result) return result;
                    if (id == i.type) return Type(id);
                    return Value(id);
                }) << "\n";
                DecorateResult(i.result, result);
                ephemeral_pointers[i.result] = result;
                CaptureRecipe(i.result);
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
            if (CopyPersistentMemory(i) || RoutedPointerOperation(i)) continue;
            if (i.op == spv::OpCopyMemory) {
                functions << Rewrite(i.text, [&](uint32_t id) { return NativeValue(id); }) << "\n";
                continue;
            }
            if (i.op == spv::OpLoad && (IsPersistentStorage(Storage(i.words.at(3))) || SystemPointer(i.words.at(3)))) {
                size_t cursor = 4;
                const auto access = ParsePersistentAccess(i, cursor);
                if (cursor != i.words.size()) Unsupported("extra persistent load operands");
                auto address = Offset(i.words.at(3));
                Save(i.result, Op("OpFunctionCall " + ValueType(i.type) + " " + QualifiedCodec(i.type, false, access) +
                                  " %sc_base " + address));
            } else if (i.op == spv::OpStore) {
                if (IsPersistentStorage(Storage(i.words.at(1)))) {
                    size_t cursor = 3;
                    const auto access = ParsePersistentAccess(i, cursor);
                    if (cursor != i.words.size()) Unsupported("extra persistent store operands");
                    auto type = Def(Def(i.words.at(1)).type).words.at(3);
                    auto address = Offset(i.words.at(1));
                    Op("OpFunctionCall " + code.void_type + " " + QualifiedCodec(type, true, access) + " %sc_base " + address +
                       " " + Value(i.words.at(2)));
                } else {
                    const auto value_id = i.words.at(2);
                    const auto type = Def(value_id).type;
                    auto value = Value(value_id);
                    value = Bridge(type, value, true);
                    auto text = Rewrite(i.text, [&](uint32_t id) { return id == value_id ? value : NativeValue(id); });
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
                functions << Rewrite(i.text, [&](uint32_t id) { return NativeValue(id); }) << "\n";
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
            if (!i.in_function || !i.result || !i.type || (IsPointer(i.type) && !PhysicalPointer(i.type)) || Ephemeral(i.type) ||
                Def(i.type).op == spv::OpTypeVoid)
                continue;
            auto pointer = PointerType(spv::StorageClassFunction, ValueType(i.type));
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
    bool has_uint32 = false, has_uint64 = false, has_int64 = false, pointer_arithmetic = false;
    for (size_t p = 5; p < source.size(); p += source[p] >> 16) {
        const auto op = source[p] & 0xffff;
        if (op == spv::OpTypeInt && source[p + 2] == 32 && source[p + 3] == 0) has_uint32 = true;
        if (op == spv::OpTypeInt && source[p + 2] == 64 && source[p + 3] == 0) has_uint64 = true;
        if (op == spv::OpCapability && source[p + 1] == spv::CapabilityInt64) has_int64 = true;
        pointer_arithmetic |= op == spv::OpPtrAccessChain || op == spv::OpInBoundsPtrAccessChain;
    }
    if (pointer_arithmetic && !has_int64) selected.insert(selected.end(), {(2u << 16) | spv::OpCapability, spv::CapabilityInt64});
    bool inserted_control_type = has_uint32;
    bool inserted_arithmetic_type = has_uint64 || !pointer_arithmetic;
    for (size_t p = 5; p < source.size(); p += source[p] >> 16) {
        auto op = static_cast<spv::Op>(source[p] & 0xffff);
        if (op == spv::OpEntryPoint && source.at(p + 2) != selected_entry) continue;
        if ((op == spv::OpExecutionMode || op == spv::OpExecutionModeId) && source.at(p + 1) != selected_entry) continue;
        if (op == spv::OpFunction && !inserted_control_type) {
            auto id = selected[3]++;
            selected.insert(selected.end(), {(4u << 16) | spv::OpTypeInt, id, 32, 0});
            inserted_control_type = true;
        }
        if (op == spv::OpFunction && !inserted_arithmetic_type) {
            const auto id = selected[3]++;
            selected.insert(selected.end(), {(4u << 16) | spv::OpTypeInt, id, 64, 0});
            inserted_arithmetic_type = true;
        }
        selected.insert(selected.end(), source.begin() + p, source.begin() + p + (source[p] >> 16));
    }
    spvtools::Optimizer optimizer(VulkanEnvironmentForSPIRV(selected.at(1)));
    std::string error;
    optimizer.SetMessageConsumer(
        [&](spv_message_level_t, const char *, const spv_position_t &, const char *message) { error += message; });
    optimizer.RegisterPass(spvtools::CreateInlineExhaustivePass());
    optimizer.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
    optimizer.RegisterPass(spvtools::CreateFlattenDecorationPass());
    std::vector<uint32_t> inlined;
    spvtools::ValidatorOptions validation;
    validation.SetScalarBlockLayout(true);
    spvtools::OptimizerOptions optimization;
    optimization.set_validator_options(validation);
    if (!optimizer.Run(selected.data(), selected.size(), &inlined, optimization)) Unsupported("helper inlining failed: " + error);
    return Compiler(std::move(inlined), entry, execution_model, options).Run();
}
}  // namespace vel::rt
