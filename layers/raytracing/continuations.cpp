// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "continuations.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv_cross/spirv.hpp>
#include <algorithm>
#include <cstring>
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
}  // namespace

ContinuationAnalysis AnalyzeContinuations(const std::vector<uint32_t> &spirv, const std::string &entry, uint32_t execution_model) {
    std::string error;
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    validator.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &where, const char *message) {
        error += std::to_string(where.index) + ": " + message + "\n";
    });
    if (!validator.Validate(spirv)) throw std::runtime_error("Invalid continuation input: " + error);
    Module m;
    auto context = spvContextCreate(SPV_ENV_VULKAN_1_3);
    spv_diagnostic diagnostic = nullptr;
    auto status = spvBinaryParse(context, &m, spirv.data(), spirv.size(), nullptr, ParseInstruction, &diagnostic);
    if (status != SPV_SUCCESS && diagnostic) error = diagnostic->error;
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    if (status != SPV_SUCCESS) throw std::runtime_error("Cannot parse continuation input: " + error);
    uint32_t entry_id = 0;
    for (const auto &i : m.instructions) {
        if (i.op == spv::OpEntryPoint && i.words.at(1) == execution_model) {
            const char *name = reinterpret_cast<const char *>(i.words.data() + 3);
            if (entry == name) entry_id = i.words.at(2);
        }
    }
    if (!entry_id) throw std::runtime_error("Continuation entry point not found: " + entry);
    BuildFunctions(m);
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
}  // namespace vel::rt
