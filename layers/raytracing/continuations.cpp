// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "continuations.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv_cross/spirv.hpp>
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace vel::rt {
namespace {
using IDs = std::set<uint32_t>;
struct Instruction {
    spv::Op op;
    uint32_t offset, type, result;
    std::vector<uint32_t> words, uses;
};
struct Block {
    uint32_t label;
    std::vector<size_t> instructions;
    std::vector<uint32_t> successors;
    IDs definitions, uses, in, out;
    // Phi operands are uses on a predecessor edge, not in this block.
    std::map<uint32_t, IDs> phi_edges;
};
struct Function {
    uint32_t id;
    std::vector<Block> blocks;
    IDs local_values;
    std::set<uint32_t> callees;
    bool suspends = false;
};
struct Module {
    std::vector<Instruction> instructions;
    std::map<uint32_t, Function> functions;
    uint32_t word_offset = 5;
};

spv_result_t ParseInstruction(void *data, const spv_parsed_instruction_t *parsed) {
    auto &m = *static_cast<Module *>(data);
    Instruction i{static_cast<spv::Op>(parsed->opcode), m.word_offset, parsed->type_id, parsed->result_id, {}, {}};
    i.words.assign(parsed->words, parsed->words + parsed->num_words);
    for (uint32_t n = 0; n < parsed->num_operands; ++n) {
        const auto &operand = parsed->operands[n];
        // The grammar distinguishes literal integer words from IDs. Comparing
        // arbitrary instruction words to the module's ID bound is incorrect.
        if (operand.type == SPV_OPERAND_TYPE_ID || operand.type == SPV_OPERAND_TYPE_SCOPE_ID ||
            operand.type == SPV_OPERAND_TYPE_MEMORY_SEMANTICS_ID) {
            i.uses.insert(i.uses.end(), parsed->words + operand.offset, parsed->words + operand.offset + operand.num_words);
        }
    }
    m.word_offset += parsed->num_words;
    m.instructions.push_back(std::move(i));
    return SPV_SUCCESS;
}

bool IntrinsicSuspends(spv::Op op) {
    return op == spv::OpTraceRayKHR || op == spv::OpReportIntersectionKHR || op == spv::OpExecuteCallableKHR;
}
SuspensionKind Kind(spv::Op op) {
    if (op == spv::OpTraceRayKHR) return SuspensionKind::TraceRay;
    if (op == spv::OpReportIntersectionKHR) return SuspensionKind::ReportIntersection;
    if (op == spv::OpExecuteCallableKHR) return SuspensionKind::ExecuteCallable;
    return SuspensionKind::FunctionCall;
}
std::vector<uint32_t> Vector(const IDs &ids) { return {ids.begin(), ids.end()}; }

void BuildFunctions(Module &m) {
    Function *function = nullptr;
    Block *block = nullptr;
    for (size_t index = 0; index < m.instructions.size(); ++index) {
        const auto &i = m.instructions[index];
        if (i.op == spv::OpFunction) {
            auto &f = m.functions[i.result];
            f.id = i.result;
            function = &f;
        } else if (i.op == spv::OpFunctionEnd) {
            function = nullptr;
            block = nullptr;
        } else if (function) {
            if (i.op == spv::OpLabel) {
                function->blocks.push_back(Block{});
                block = &function->blocks.back();
                block->label = i.result;
            } else {
                if (i.result) function->local_values.insert(i.result);
                if (block) block->instructions.push_back(index);
                if (IntrinsicSuspends(i.op)) function->suspends = true;
                if (i.op == spv::OpFunctionCall) function->callees.insert(i.words.at(3));
            }
        }
    }
}

void BuildEdges(const Module &m, Function &function) {
    std::map<uint32_t, size_t> blocks;
    for (size_t n = 0; n < function.blocks.size(); ++n) blocks[function.blocks[n].label] = n;
    for (auto &block : function.blocks) {
        for (auto index : block.instructions) {
            const auto &i = m.instructions[index];
            if (i.op == spv::OpPhi) {
                for (size_t n = 3; n < i.words.size(); n += 2) {
                    auto value = i.words.at(n), predecessor = i.words.at(n + 1);
                    if (function.local_values.count(value)) block.phi_edges[predecessor].insert(value);
                }
            } else {
                for (auto use : i.uses) {
                    if (function.local_values.count(use) && !block.definitions.count(use)) block.uses.insert(use);
                }
            }
            if (i.result) block.definitions.insert(i.result);
            if (i.op == spv::OpBranch) block.successors.push_back(i.words.at(1));
            if (i.op == spv::OpBranchConditional) {
                block.successors.push_back(i.words.at(2));
                block.successors.push_back(i.words.at(3));
            }
            if (i.op == spv::OpSwitch) {
                // Literal cases may have one or two words. Label IDs, unlike
                // literals, were identified by the binary grammar above.
                for (auto id : i.uses) {
                    if (blocks.count(id)) block.successors.push_back(id);
                }
            }
        }
        std::sort(block.successors.begin(), block.successors.end());
        block.successors.erase(std::unique(block.successors.begin(), block.successors.end()), block.successors.end());
    }
}

void SolveLiveness(Function &function) {
    std::map<uint32_t, Block *> blocks;
    for (auto &b : function.blocks) blocks[b.label] = &b;
    // Least fixed point handles nested loops and back edges. At each successor
    // include only this predecessor's phi operands. In particular, a loop-carried
    // value defined before TraceRay may be used only by a phi on the back edge.
    bool changed;
    do {
        changed = false;
        for (auto it = function.blocks.rbegin(); it != function.blocks.rend(); ++it) {
            auto &b = *it;
            IDs out;
            for (auto label : b.successors) {
                const auto &next = *blocks.at(label);
                out.insert(next.in.begin(), next.in.end());
                auto phi = next.phi_edges.find(b.label);
                if (phi != next.phi_edges.end()) out.insert(phi->second.begin(), phi->second.end());
            }
            IDs in = b.uses;
            for (auto value : out) {
                if (!b.definitions.count(value)) in.insert(value);
            }
            if (b.out != out || b.in != in) changed = true;
            b.out = std::move(out);
            b.in = std::move(in);
        }
    } while (changed);
}

FunctionContinuations AnalyzeFunction(const Module &m, Function &function) {
    BuildEdges(m, function);
    SolveLiveness(function);
    FunctionContinuations result{function.id, function.suspends, {}, {}};
    for (const auto &block : function.blocks) {
        result.blocks.push_back({block.label, block.successors, Vector(block.in), Vector(block.out)});
        IDs live = block.out;
        for (auto it = block.instructions.rbegin(); it != block.instructions.rend(); ++it) {
            const auto &i = m.instructions[*it];
            if (i.result) live.erase(i.result);
            bool suspend = IntrinsicSuspends(i.op);
            if (i.op == spv::OpFunctionCall) suspend = m.functions.at(i.words.at(3)).suspends;
            if (suspend) result.suspensions.push_back({Kind(i.op), i.offset, block.label, i.result, Vector(live)});
            // Phi operands were accounted for on predecessor edges.
            if (i.op != spv::OpPhi) {
                for (auto use : i.uses) {
                    if (function.local_values.count(use)) live.insert(use);
                }
            }
        }
    }
    std::sort(result.suspensions.begin(), result.suspensions.end(),
              [](const Suspension &a, const Suspension &b) { return a.word_offset < b.word_offset; });
    return result;
}

Module ParseModule(const std::vector<uint32_t> &spirv) {
    std::string error;
    Module m;
    auto context = spvContextCreate(SPV_ENV_VULKAN_1_3);
    spv_diagnostic diagnostic = nullptr;
    auto status = spvBinaryParse(context, &m, spirv.data(), spirv.size(), nullptr, ParseInstruction, &diagnostic);
    if (status != SPV_SUCCESS && diagnostic) error = diagnostic->error;
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    if (status != SPV_SUCCESS) throw std::runtime_error("Cannot parse continuation input: " + error);
    BuildFunctions(m);
    return m;
}

uint64_t CheckedAdd(uint64_t a, uint64_t b) {
    if (b > std::numeric_limits<uint64_t>::max() - a) throw std::runtime_error("Continuation frame size overflow");
    return a + b;
}
uint64_t CheckedMultiply(uint64_t a, uint64_t b) {
    if (b && a > std::numeric_limits<uint64_t>::max() / b) throw std::runtime_error("Continuation frame size overflow");
    return a * b;
}
uint64_t Align(uint64_t value, uint32_t alignment) { return CheckedAdd(value, alignment - 1) & ~uint64_t(alignment - 1); }

class FramePlanner {
  public:
    explicit FramePlanner(const Module &module, ContinuationFrameOptions options) : m(module), options(options) {
        for (const auto &i : m.instructions) {
            if (i.result) definitions[i.result] = &i;
            if (i.op == spv::OpVariable && i.words.at(3) == spv::StorageClassPrivate) private_storage.insert(i.result);
        }
        for (const auto &[id, f] : m.functions) {
            (void)id;
            local_values.insert(f.local_values.begin(), f.local_values.end());
        }
    }

    FunctionFrame Plan(const FunctionContinuations &analysis) {
        FunctionFrame frame{analysis.function, options.prefix_bytes, 1, {}, {}};
        if (!analysis.can_suspend && !options.persistent_function_storage) return frame;
        RejectPrivateStorage(analysis.function);
        const auto &function = m.functions.at(analysis.function);
        // A pointer can be consumed by a suspending helper without being live
        // after the call. SSA liveness alone therefore cannot select variables
        // whose pointees must survive. Conservatively relocate every Function
        // variable for the whole invocation, maintaining one backing slot for
        // all aliases and preserving writes performed before/after suspension.
        for (auto value : function.local_values) {
            const auto &i = Definition(value);
            if (i.op == spv::OpVariable) {
                const auto &pointer = Definition(i.type);
                AddSlot(frame, value, pointer.words.at(3), true);
            }
        }
        for (const auto &site : analysis.suspensions) {
            SuspensionFrame suspension{site.word_offset, {}, {}};
            IDs prepared, visiting;
            for (const auto &slot : frame.slots) {
                if (slot.local_storage) suspension.saved_values.push_back(slot.value);
            }
            std::function<void(uint32_t)> preserve = [&](uint32_t value) {
                if (!local_values.count(value) || prepared.count(value)) return;
                if (!function.local_values.count(value)) throw std::runtime_error("Cross-function continuation value");
                if (!visiting.insert(value).second) throw std::runtime_error("Cyclic continuation rematerialization");
                const auto &i = Definition(value);
                const auto &type = Definition(i.type);
                if (!IsReference(type.op) ||
                    (options.acceleration_structure_addresses && type.op == spv::OpTypeAccelerationStructureKHR)) {
                    AddSlot(frame, value, i.type, false);
                    suspension.saved_values.push_back(value);
                } else if (i.op == spv::OpVariable) {
                    // The persistent backing slot already provides this root.
                } else {
                    bool supported = i.op == spv::OpCopyObject;
                    if (type.op == spv::OpTypePointer) {
                        supported |= i.op == spv::OpAccessChain || i.op == spv::OpInBoundsAccessChain;
                    } else {
                        supported |= i.op == spv::OpSampledImage || i.op == spv::OpImage;
                        if (i.op == spv::OpLoad) {
                            const auto &source_type = Definition(Definition(i.words.at(3)).type);
                            supported =
                                source_type.op == spv::OpTypePointer && source_type.words.at(2) == spv::StorageClassUniformConstant;
                        }
                    }
                    if (!supported) {
                        throw std::runtime_error("Unsupported continuation pointer/opaque provenance for ID " +
                                                 std::to_string(value) + " at word " + std::to_string(i.offset));
                    }
                    for (auto dependency : i.uses) preserve(dependency);
                    suspension.rematerializations.push_back({value, i.type, i.offset, i.uses});
                }
                visiting.erase(value);
                prepared.insert(value);
            };
            for (auto value : site.live_values) preserve(value);
            std::sort(suspension.saved_values.begin(), suspension.saved_values.end());
            suspension.saved_values.erase(std::unique(suspension.saved_values.begin(), suspension.saved_values.end()),
                                          suspension.saved_values.end());
            frame.suspensions.push_back(std::move(suspension));
        }
        frame.size = Align(frame.size, frame.alignment);
        return frame;
    }

    std::vector<ContinuationTypeLayout> Types() const {
        std::vector<ContinuationTypeLayout> result;
        for (const auto &[id, layout] : layouts) {
            (void)id;
            result.push_back(layout);
        }
        return result;
    }
    void IncludeValueTypes(const std::vector<uint32_t> &types) {
        for (auto type : types) Layout(type);
    }

  private:
    const Module &m;
    ContinuationFrameOptions options;
    std::map<uint32_t, const Instruction *> definitions;
    std::map<uint32_t, ContinuationTypeLayout> layouts;
    IDs local_values, laying_out, private_storage;

    void RejectPrivateStorage(uint32_t function) const {
        // Private variables are module-scope IDs but per-shader-invocation
        // mutable storage. They may be read only after a suspension (possibly
        // in a nonsuspending helper), so they never appear in local SSA
        // liveness. Reject their reachable use until invocation backing is
        // defined, rather than silently treating their storage as a constant.
        IDs visited;
        std::vector<uint32_t> pending{function};
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!visited.insert(id).second) continue;
            const auto &f = m.functions.at(id);
            for (const auto &block : f.blocks) {
                for (auto index : block.instructions) {
                    for (auto use : m.instructions[index].uses) {
                        if (private_storage.count(use))
                            throw std::runtime_error("Continuation module Private storage requires invocation backing for ID " +
                                                     std::to_string(use));
                    }
                }
            }
            pending.insert(pending.end(), f.callees.begin(), f.callees.end());
        }
    }

    const Instruction &Definition(uint32_t value) const { return *definitions.at(value); }
    static bool IsReference(spv::Op op) {
        return op == spv::OpTypePointer || op == spv::OpTypeImage || op == spv::OpTypeSampler || op == spv::OpTypeSampledImage ||
               op == spv::OpTypeAccelerationStructureKHR;
    }
    uint64_t ArrayLength(uint32_t id) const {
        const auto &constant = Definition(id);
        const auto &type = Definition(constant.type);
        // Specialization must run before concrete layout. A default value does
        // not define the frame size for all legal specializations.
        if (constant.op != spv::OpConstant || type.op != spv::OpTypeInt)
            throw std::runtime_error("Continuation array length requires specialization first");
        auto width = type.words.at(2);
        if (width != 32 && width != 64) throw std::runtime_error("Unsupported continuation array length width");
        uint64_t length = constant.words.at(3);
        if (width == 64) length |= uint64_t(constant.words.at(4)) << 32;
        if (!length || (type.words.at(3) && (length & (uint64_t(1) << (width - 1)))))
            throw std::runtime_error("Invalid continuation array length");
        return length;
    }
    ContinuationTypeLayout Layout(uint32_t id) {
        auto known = layouts.find(id);
        if (known != layouts.end()) return known->second;
        if (!laying_out.insert(id).second) throw std::runtime_error("Recursive continuation frame type");
        const auto &type = Definition(id);
        ContinuationTypeLayout result{id, 0, 1, 0, {}};
        if (type.op == spv::OpTypeAccelerationStructureKHR && options.acceleration_structure_addresses) {
            result.size = result.alignment = 8;
        } else if (type.op == spv::OpTypeBool) {
            result.size = result.alignment = 4;
        } else if (type.op == spv::OpTypeInt || type.op == spv::OpTypeFloat) {
            const auto width = type.words.at(2);
            if (width != 8 && width != 16 && width != 32 && width != 64)
                throw std::runtime_error("Unsupported continuation scalar width");
            result.size = result.alignment = width / 8;
        } else if (type.op == spv::OpTypeVector || type.op == spv::OpTypeMatrix || type.op == spv::OpTypeArray) {
            auto element = Layout(type.words.at(2));
            uint64_t count = type.op == spv::OpTypeArray ? ArrayLength(type.words.at(3)) : type.words.at(3);
            result.alignment = element.alignment;
            result.element_stride = Align(element.size, element.alignment);
            result.size = CheckedMultiply(result.element_stride, count);
        } else if (type.op == spv::OpTypeStruct) {
            for (size_t n = 2; n < type.words.size(); ++n) {
                auto member = Layout(type.words[n]);
                result.alignment = std::max(result.alignment, member.alignment);
                result.size = Align(result.size, member.alignment);
                result.member_offsets.push_back(result.size);
                result.size = CheckedAdd(result.size, member.size);
            }
            result.size = Align(result.size, result.alignment);
        } else {
            throw std::runtime_error("Unsupported continuation frame type ID " + std::to_string(id));
        }
        laying_out.erase(id);
        layouts[id] = result;
        return result;
    }
    void AddSlot(FunctionFrame &frame, uint32_t value, uint32_t type, bool storage) {
        for (const auto &slot : frame.slots) {
            if (slot.value == value) return;
        }
        auto layout = Layout(type);
        auto offset = Align(frame.size, layout.alignment);
        frame.slots.push_back({value, type, offset, layout.size, layout.alignment, storage});
        frame.size = CheckedAdd(offset, layout.size);
        frame.alignment = std::max(frame.alignment, layout.alignment);
    }
};
}  // namespace

ContinuationAnalysis AnalyzeContinuations(const std::vector<uint32_t> &spirv, const std::string &entry, uint32_t execution_model) {
    std::string error;
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    validator.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &where, const char *message) {
        error += std::to_string(where.index) + ": " + message + "\n";
    });
    if (!validator.Validate(spirv)) throw std::runtime_error("Invalid continuation input: " + error);
    auto m = ParseModule(spirv);
    uint32_t entry_id = 0;
    for (const auto &i : m.instructions) {
        if (i.op == spv::OpEntryPoint && i.words.at(1) == execution_model) {
            const char *name = reinterpret_cast<const char *>(i.words.data() + 3);
            if (entry == name) entry_id = i.words.at(2);
        }
    }
    if (!entry_id) throw std::runtime_error("Continuation entry point not found: " + entry);
    IDs reachable{entry_id};
    std::vector<uint32_t> work{entry_id};
    while (!work.empty()) {
        auto id = work.back();
        work.pop_back();
        for (auto callee : m.functions.at(id).callees) {
            if (reachable.insert(callee).second) work.push_back(callee);
        }
    }
    // A helper containing a TraceRay (or calling another such helper) suspends
    // its caller too. This cannot be detected by searching entry-point text.
    bool changed;
    do {
        changed = false;
        for (auto id : reachable) {
            auto &f = m.functions.at(id);
            for (auto callee : f.callees) {
                if (!f.suspends && m.functions.at(callee).suspends) {
                    f.suspends = true;
                    changed = true;
                }
            }
        }
    } while (changed);
    ContinuationAnalysis result{entry_id, {}};
    for (auto id : reachable) result.functions.push_back(AnalyzeFunction(m, m.functions.at(id)));
    return result;
}

ContinuationFrames PlanContinuationFrames(const std::vector<uint32_t> &spirv, const std::string &entry, uint32_t execution_model,
                                          const std::vector<uint32_t> &additional_value_types, ContinuationFrameOptions options) {
    ContinuationFrames result;
    result.analysis = AnalyzeContinuations(spirv, entry, execution_model);
    auto module = ParseModule(spirv);
    result.options = options;
    FramePlanner planner(module, options);
    for (const auto &function : result.analysis.functions) result.functions.push_back(planner.Plan(function));
    planner.IncludeValueTypes(additional_value_types);
    result.types = planner.Types();
    return result;
}
}  // namespace vel::rt
