// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Independent valid-IR graph and publication tests for the query-state preparation pass.
#include "../omm_query_rewrite.h"
#include <spirv-tools/libspirv.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using vel::omm::QueryRewriteStatus;
using vel::omm::QueryStateModule;
namespace {
unsigned failures = 0, checks = 0;
void expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}
struct Instruction {
    spv::Op op;
    std::vector<uint32_t> w;
    uint32_t result = 0;
};
struct Graph {
    std::vector<Instruction> instructions;
    std::map<uint32_t, Instruction> definitions;
    explicit Graph(const std::vector<uint32_t>& words) {
        for (size_t offset = 5; offset < words.size();) {
            size_t count = words[offset] >> 16;
            Instruction value{spv::Op(words[offset] & 65535), {words.begin() + offset, words.begin() + offset + count}};
            bool result = false, type = false;
            spv::HasResultAndType(value.op, &result, &type);
            if (result) {
                value.result = value.w[type ? 2 : 1];
                definitions[value.result] = value;
            }
            instructions.push_back(value);
            offset += count;
        }
    }
    uint32_t underlying(uint32_t id) const {
        auto found = definitions.find(id);
        if (found != definitions.end() && found->second.op == spv::OpBitcast) return underlying(found->second.w[3]);
        return id;
    }
    bool constant(uint32_t id, uint32_t& result) const {
        auto found = definitions.find(id);
        if (found == definitions.end()) return false;
        const auto& x = found->second;
        if (x.op == spv::OpConstant && x.w.size() == 4) {
            result = x.w[3];
            return true;
        }
        if (x.op == spv::OpBitcast) return constant(x.w[3], result);
        uint32_t left, right;
        if ((x.op == spv::OpBitwiseAnd || x.op == spv::OpBitwiseOr) && constant(x.w[3], left) && constant(x.w[4], right)) {
            result = x.op == spv::OpBitwiseAnd ? left & right : left | right;
            return true;
        }
        return false;
    }
};
std::string base(const std::string& globals, const std::string& body, const std::string& helpers = {},
                 bool variablePointers = false, bool signedOnly = false, bool fragment = false) {
    return "OpCapability Shader\nOpCapability RayQueryKHR\n" +
           std::string(variablePointers ? "OpCapability VariablePointers\n" : "") + "OpExtension \"SPV_KHR_ray_query\"\n" +
           std::string(variablePointers ? "OpExtension \"SPV_KHR_variable_pointers\"\n" : "") +
           "OpMemoryModel Logical GLSL450\nOpEntryPoint " + std::string(fragment ? "Fragment" : "GLCompute") +
           " %main \"main\" %scene " + globals.substr(0, globals.find('\n')) + "\n" +
           (fragment ? "OpExecutionMode %main OriginUpperLeft\n" : "OpExecutionMode %main LocalSize 1 1 1\n") +
           "OpDecorate %scene DescriptorSet 0\nOpDecorate %scene Binding 0\n"
           "%void = OpTypeVoid\n%bool = OpTypeBool\n%u = OpTypeInt 32 " +
           std::string(signedOnly ? "1\n" : "0\n%i = OpTypeInt 32 1\n") +
           "%f = OpTypeFloat 32\n%v3 = OpTypeVector %f 3\n%true = OpConstantTrue %bool\n"
           "%u0 = OpConstant %u 0\n%u1 = OpConstant %u 1\n%u2 = OpConstant %u 2\n%u64 = OpConstant %u 64\n"
           "%u128 = OpConstant %u 128\n%u255 = OpConstant %u 255\n%f0 = OpConstant %f 0\n%f1 = OpConstant %f 1\n"
           "%origin = OpConstantComposite %v3 %f0 %f0 %f0\n%direction = OpConstantComposite %v3 %f0 %f0 %f1\n"
           "%query = OpTypeRayQueryKHR\n%pFQ = OpTypePointer Function %query\n%pPQ = OpTypePointer Private %query\n"
           "%array = OpTypeArray %query %u2\n%nested = OpTypeArray %array %u2\n"
           "%pFA = OpTypePointer Function %array\n%pPA = OpTypePointer Private %array\n"
           "%pFN = OpTypePointer Function %nested\n%pPN = OpTypePointer Private %nested\n"
           "%as = OpTypeAccelerationStructureKHR\n%pAS = OpTypePointer UniformConstant %as\n"
           "%scene = OpVariable %pAS UniformConstant\n%fn = OpTypeFunction %void\n" +
           globals.substr(globals.find('\n') + 1) + helpers + "%main = OpFunction %void None %fn\n%entry = OpLabel\n" + body +
           "OpReturn\nOpFunctionEnd\n";
}
std::string initialize(const char* query, const char* flags) {
    return std::string("OpRayQueryInitializeKHR ") + query + " %loadedAS " + flags + " %u255 %origin %f0 %direction %f1\n";
}
bool equal(const QueryStateModule& a, const QueryStateModule& b) {
    if (a.words != b.words || a.initializations != b.initializations || a.flag_getters != b.flag_getters ||
        a.state.size() != b.state.size())
        return false;
    for (size_t i = 0; i < a.state.size(); ++i)
        if (a.state[i].query_pointer != b.state[i].query_pointer || a.state[i].flags_pointer != b.state[i].flags_pointer)
            return false;
    return true;
}
void graph_invariants(const Graph& source, const QueryStateModule& module, bool expose, const std::string& name) {
    Graph output(module.words);
    std::map<uint32_t, uint32_t> shadows;
    std::set<uint32_t> unique;
    for (const auto& binding : module.state) {
        expect(shadows.emplace(binding.query_pointer, binding.flags_pointer).second,
               name + ": one binding per source query pointer");
        expect(unique.insert(binding.flags_pointer).second, name + ": query pointers have independent SSA flag pointers");
        expect(binding.query_pointer != binding.flags_pointer, name + ": original query pointer retained");
    }
    // Verify the appended parallel ABI independently from the production parser.
    std::map<uint32_t, std::vector<size_t>> queryParameters;
    uint32_t function = 0;
    size_t parameter = 0;
    for (const auto& x : source.instructions) {
        if (x.op == spv::OpFunction) {
            function = x.result;
            parameter = 0;
        } else if (x.op == spv::OpFunctionParameter) {
            if (shadows.count(x.result)) queryParameters[function].push_back(parameter);
            ++parameter;
        }
    }
    for (const auto& x : source.instructions) {
        if (x.op != spv::OpFunctionCall) continue;
        const auto target = output.definitions.find(x.result);
        expect(target != output.definitions.end() && target->second.op == spv::OpFunctionCall, name + ": original call retained");
        if (target == output.definitions.end()) continue;
        const auto& y = target->second;
        const auto& positions = queryParameters[x.w[3]];
        bool correct = y.w.size() == x.w.size() + positions.size() && std::equal(x.w.begin() + 1, x.w.end(), y.w.begin() + 1);
        for (size_t i = 0; correct && i < positions.size(); ++i) correct = y.w[x.w.size() + i] == shadows[x.w[4 + positions[i]]];
        expect(correct, name + ": call appends shadow arguments in source parameter order");
    }
    unsigned initializations = 0, getters = 0;
    std::vector<Instruction> nativeInitializations;
    for (const auto& x : output.instructions)
        if (x.op == spv::OpRayQueryInitializeKHR) nativeInitializations.push_back(x);
    for (const auto& x : source.instructions) {
        if (x.op == spv::OpRayQueryInitializeKHR) {
            expect(initializations < nativeInitializations.size(), name + ": preserve native initialize count");
            if (initializations >= nativeInitializations.size()) continue;
            const auto& native = nativeInitializations[initializations++];
            expect(native.w[1] == x.w[1] && native.w[2] == x.w[2], name + ": keep original native query and AS operands");
            bool store = false;
            for (const auto& y : output.instructions)
                store |= y.op == spv::OpStore && y.w[1] == shadows[x.w[1]] && output.underlying(y.w[2]) == x.w[3];
            expect(store, name + ": logical initialization bits stored in matching shadow");
            if (!expose)
                expect(native.w[3] == x.w[3], name + ": capture mode leaves native ray flags untouched");
            else {
                uint32_t inputValue = 0, outputValue = 0;
                if (source.constant(x.w[3], inputValue))
                    expect(output.constant(native.w[3], outputValue) &&
                               outputValue == ((inputValue & ~(1u | 2u | 64u | 128u | 1024u)) | 2u),
                           name + ": exposed native flags have exact candidate mask");
            }
        } else if (x.op == spv::OpRayQueryGetRayFlagsKHR) {
            ++getters;
            auto result = output.definitions.find(output.underlying(x.result));
            expect(result != output.definitions.end() && result->second.op == spv::OpLoad && result->second.w[3] == shadows[x.w[3]],
                   name + ": flag getter reads the correct shadow, allowing signed bitcast");
        }
        auto shadow = shadows.find(x.result);
        if (shadow == shadows.end()) continue;
        const auto found = output.definitions.find(shadow->second);
        expect(found != output.definitions.end(), name + ": shadow pointer is defined");
        if (found == output.definitions.end()) continue;
        const auto& y = found->second;
        if (x.op == spv::OpCopyObject || x.op == spv::OpAccessChain || x.op == spv::OpInBoundsAccessChain)
            expect(y.op == x.op && y.w[3] == shadows[x.w[3]] && std::equal(x.w.begin() + 4, x.w.end(), y.w.begin() + 4),
                   name + ": copy/access shadow follows original provenance");
    }
    expect(module.initializations == initializations && module.flag_getters == getters, name + ": honest operation counts");
}
void run(const std::string& name, const std::string& assembly, unsigned expectedInitializations, unsigned expectedGetters,
         bool invalidInput = false) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string diagnostic;
    tools.SetMessageConsumer(
        [&](spv_message_level_t, const char*, const spv_position_t&, const char* message) { diagnostic = message; });
    std::vector<uint32_t> input;
    if (!tools.Assemble(assembly, &input)) {
        expect(false, name + ": test input assembly failed: " + diagnostic);
        return;
    }
    const bool inputValid = tools.Validate(input);
    if (invalidInput) {
        expect(!inputValid, name + ": independently reject prohibited Vulkan pointer use");
        QueryStateModule sentinel{{0xfeedbeefu}, {{11, 12}}, 99, 88};
        for (bool expose : {false, true}) {
            auto output = sentinel;
            expect(vel::omm::prepare_query_state(input, expose, &output, &diagnostic) == QueryRewriteStatus::InvalidModule &&
                       equal(output, sentinel),
                   name + ": invalid Vulkan pointer graph is rejected atomically");
        }
        std::printf("Rejected invalid Vulkan IR graph: %s\n", name.c_str());
        return;
    }
    if (!inputValid) {
        expect(false, name + ": independent input must validate: " + diagnostic);
        return;
    }
    const auto saved = input;
    Graph graph(input);
    for (bool expose : {false, true}) {
        const QueryStateModule sentinel{{0xfeedbeefu}, {{11, 12}}, 99, 88};
        QueryStateModule output = sentinel;
        const auto status = vel::omm::prepare_query_state(input, expose, &output, &diagnostic);
        expect(status == QueryRewriteStatus::Success, name + ": valid graph supported: " + diagnostic);
        expect(input == saved, name + ": input immutable");
        if (status != QueryRewriteStatus::Success) {
            expect(equal(output, sentinel), name + ": valid unsupported graph leaves output atomic");
            continue;
        }
        expect(tools.Validate(output.words), name + ": independent output validator: " + diagnostic);
        expect(output.initializations == expectedInitializations && output.flag_getters == expectedGetters,
               name + ": expected source operation inventory");
        graph_invariants(graph, output, expose, name);
    }
    std::printf("Checked valid IR graph: %s\n", name.c_str());
}
}  // namespace
int main() {
    const auto load = "%loadedAS = OpLoad %as %scene\n";
    run("Function scalar, reinitialize and unused CopyObject",
        base("\n", "%q = OpVariable %pFQ Function\n" + std::string(load) + initialize("%q", "%u1") +
                       "%alias = OpCopyObject %pFQ %q\n%g0 = OpRayQueryGetRayFlagsKHR %u %q\n" + initialize("%q", "%u128") +
                       "%g1 = OpRayQueryGetRayFlagsKHR %u %q\n"),
        2, 2);
    run("CopyObject query operand forbidden by Vulkan",
        base("\n", "%q = OpVariable %pFQ Function\n" + std::string(load) + initialize("%q", "%u1") +
                       "%alias = OpCopyObject %pFQ %q\n%g = OpRayQueryGetRayFlagsKHR %u %alias\n"),
        1, 1, true);
    run("Private scalar and unused CopyObject in Fragment",
        base("%q\n%q = OpVariable %pPQ Private\n",
             std::string(load) + initialize("%q", "%u64") + "%alias = OpCopyObject %pPQ %q\n%g0 = OpRayQueryGetRayFlagsKHR %u %q\n",
             {}, false, false, true),
        1, 1);
    for (bool privateStorage : {false, true}) {
        const auto structure = std::string("%structure = OpTypeStruct %query %u\n%pStructure = OpTypePointer ") +
                               (privateStorage ? "Private" : "Function") + " %structure\n";
        const auto variable = std::string("%object = OpVariable %pStructure ") + (privateStorage ? "Private\n" : "Function\n");
        const auto globals = privateStorage ? "%object\n" + structure + variable : "\n" + structure;
        const auto body = (privateStorage ? std::string() : variable) + load + "%member = OpAccessChain " +
                          (privateStorage ? "%pPQ" : "%pFQ") + " %object %u0\n" + initialize("%member", "%u1") +
                          "%memberFlags = OpRayQueryGetRayFlagsKHR %u %member\n";
        run(privateStorage ? "Private struct query member" : "Function struct query member", base(globals, body), 1, 1);
    }
    for (bool privateStorage : {false, true}) {
        const auto storage = privateStorage ? "Private" : "Function";
        const auto types = std::string("%inner = OpTypeStruct %query %v3\n%outer = OpTypeStruct %u %inner %array\n") +
                           "%outerArray = OpTypeArray %outer %u2\n%pOuterArray = OpTypePointer " + storage + " %outerArray\n";
        const auto variable = std::string("%mixed = OpVariable %pOuterArray ") + storage + "\n";
        const auto globals = privateStorage ? "%mixed\n" + types + variable : "\n" + types;
        const auto pointer = privateStorage ? "%pPQ" : "%pFQ";
        const auto body = (privateStorage ? std::string() : variable) + load + "%nestedMember = OpAccessChain " + pointer +
                          " %mixed %u1 %u1 %u0\n"
                          "%arrayMember = OpAccessChain " +
                          pointer + " %mixed %u0 %u2 %u1\n" + initialize("%nestedMember", "%u1") +
                          initialize("%arrayMember", "%u128") +
                          "%nestedFlags = OpRayQueryGetRayFlagsKHR %u %nestedMember\n"
                          "%arrayFlags = OpRayQueryGetRayFlagsKHR %u %arrayMember\n";
        run(privateStorage ? "Private arrays of mixed nested structs" : "Function arrays of mixed nested structs",
            base(globals, body), 2, 2);
    }
    for (bool privateStorage : {false, true}) {
        const std::string pointer = privateStorage ? "%pPN" : "%pFN", scalar = privateStorage ? "%pPQ" : "%pFQ";
        const std::string declaration = "%queries = OpVariable " + pointer + (privateStorage ? " Private\n" : " Function\n");
        auto globals = privateStorage ? "%queries\n" + declaration : "\n";
        auto body = (privateStorage ? std::string() : declaration) + load + "%left = OpAccessChain " + scalar +
                    " %queries %u0 %u1\n" + "%right = OpInBoundsAccessChain " + scalar + " %queries %u1 %u0\n" +
                    initialize("%left", "%u1") + initialize("%right", "%u128") +
                    "%a = OpRayQueryGetRayFlagsKHR %u %left\n%b = OpRayQueryGetRayFlagsKHR %u %right\n";
        run(privateStorage ? "Private nested fixed arrays" : "Function nested fixed arrays", base(globals, body), 2, 2);
    }
    // Vulkan forbids VariablePointers Select/Phi in Function/Private storage,
    // while RayQuery cannot be stored in StorageBuffer/Workgroup. These are
    // negative source-validation tests, never advertised as legal query graphs.
    for (bool privateStorage : {false, true}) {
        const std::string pointer = privateStorage ? "%pPQ" : "%pFQ";
        const std::string declaration = "%q0 = OpVariable " + pointer + (privateStorage ? " Private\n" : " Function\n") +
                                        "%q1 = OpVariable " + pointer + (privateStorage ? " Private\n" : " Function\n");
        auto globals = privateStorage ? "%q0 %q1\n" + declaration : "\n";
        auto body = (privateStorage ? std::string() : declaration) + load + initialize("%q0", "%u1") + initialize("%q1", "%u128") +
                    "%selected = OpSelect " + pointer +
                    " %true %q0 %q1\n%g0 = OpRayQueryGetRayFlagsKHR %u %q0\n"
                    "OpSelectionMerge %merge None\nOpBranchConditional %true %left %right\n%left = OpLabel\nOpBranch %merge\n"
                    "%right = OpLabel\nOpBranch %merge\n%merge = OpLabel\n%joined = OpPhi " +
                    pointer + " %q1 %left %q0 %right\n%g1 = OpRayQueryGetRayFlagsKHR %u %q1\n";
        run(privateStorage ? "Private pointer Select/Phi forbidden by Vulkan" : "Function pointer Select/Phi forbidden by Vulkan",
            base(globals, body, {}, true), 2, 2, true);
    }
    run("Function loop Phi forbidden by Vulkan",
        base(
            "\n",
            "%q = OpVariable %pFQ Function\n" + std::string(load) + initialize("%q", "%u1") +
                "OpBranch %loop\n%loop = OpLabel\n%loopQuery = OpPhi %pFQ %q %entry %nextQuery %continue\n"
                "%g = OpRayQueryGetRayFlagsKHR %u %q\nOpLoopMerge %exit %continue None\nOpBranchConditional %true %exit %continue\n"
                "%continue = OpLabel\n%nextQuery = OpCopyObject %pFQ %loopQuery\nOpBranch %loop\n%exit = OpLabel\n",
            {}, true),
        1, 1, true);
    const auto helpers =
        std::string("%helperType = OpTypeFunction %void %pFQ %u\n") +
        "%helper0 = OpFunction %void None %helperType\n%parameter0 = OpFunctionParameter %pFQ\n%value0 = OpFunctionParameter %u\n"
        "%helperEntry0 = OpLabel\n%helperAS = OpLoad %as %scene\n"
        "OpRayQueryInitializeKHR %parameter0 %helperAS %value0 %u255 %origin %f0 %direction %f1\n"
        "%helperFlags0 = OpRayQueryGetRayFlagsKHR %u %parameter0\nOpReturn\nOpFunctionEnd\n"
        "%helper1 = OpFunction %void None %helperType\n%parameter1 = OpFunctionParameter %pFQ\n%value1 = OpFunctionParameter %u\n"
        "%helperEntry1 = OpLabel\n%callNested = OpFunctionCall %void %helper0 %parameter1 %value1\n"
        "%helperFlags1 = OpRayQueryGetRayFlagsKHR %u %parameter1\nOpReturn\nOpFunctionEnd\n";
    run("Shared helper function type and mixed parameters",
        base("\n",
             "%q = OpVariable %pFQ Function\n"
             "%first = OpFunctionCall %void %helper0 %q %u1\n%second = OpFunctionCall %void %helper1 %q %u128\n"
             "%final = OpRayQueryGetRayFlagsKHR %u %q\n",
             helpers),
        1, 3);
    const auto pairHelper =
        std::string("%pairType = OpTypeFunction %void %pFQ %u %pFQ\n") +
        "%pairHelper = OpFunction %void None %pairType\n%leftParameter = OpFunctionParameter %pFQ\n"
        "%middleParameter = OpFunctionParameter %u\n%rightParameter = OpFunctionParameter %pFQ\n"
        "%pairEntry = OpLabel\n%pairAS = OpLoad %as %scene\n"
        "OpRayQueryInitializeKHR %leftParameter %pairAS %middleParameter %u255 %origin %f0 %direction %f1\n"
        "OpRayQueryInitializeKHR %rightParameter %pairAS %u128 %u255 %origin %f0 %direction %f1\n"
        "%leftFlags = OpRayQueryGetRayFlagsKHR %u %leftParameter\n%rightFlags = OpRayQueryGetRayFlagsKHR %u %rightParameter\n"
        "OpReturn\nOpFunctionEnd\n";
    run("Two-query helper with interleaved scalar argument",
        base("\n",
             "%q0 = OpVariable %pFQ Function\n%q1 = OpVariable %pFQ Function\n"
             "%callPair = OpFunctionCall %void %pairHelper %q0 %u1 %q1\n"
             "%pairResult0 = OpRayQueryGetRayFlagsKHR %u %q0\n%pairResult1 = OpRayQueryGetRayFlagsKHR %u %q1\n",
             pairHelper),
        2, 4);
    const auto arrayHelper = std::string("%arrayHelperType = OpTypeFunction %void %pFA %u\n") +
                             "%arrayHelper = OpFunction %void None %arrayHelperType\n%arrayParameter = OpFunctionParameter %pFA\n"
                             "%arrayFlagParameter = OpFunctionParameter %u\n%arrayEntry = OpLabel\n%arrayAS = OpLoad %as %scene\n"
                             "%arrayElement = OpAccessChain %pFQ %arrayParameter %u1\n"
                             "OpRayQueryInitializeKHR %arrayElement %arrayAS %arrayFlagParameter %u255 %origin %f0 %direction %f1\n"
                             "%arrayFlags = OpRayQueryGetRayFlagsKHR %u %arrayElement\nOpReturn\nOpFunctionEnd\n";
    run("Whole fixed array through helper",
        base("\n",
             "%queries = OpVariable %pFA Function\n"
             "%call = OpFunctionCall %void %arrayHelper %queries %u64\n%element = OpAccessChain %pFQ %queries %u1\n"
             "%result = OpRayQueryGetRayFlagsKHR %u %element\n",
             arrayHelper),
        1, 2);
    for (const char* stage : {"Vertex", "RayGenerationKHR"}) {
        auto assembly = base("%q\n%q = OpVariable %pPQ Private\n",
                             std::string(load) + initialize("%q", "%u1") + "%get = OpRayQueryGetRayFlagsKHR %u %q\n");
        const std::string entry = "OpEntryPoint GLCompute";
        assembly.replace(assembly.find(entry), entry.size(), std::string("OpEntryPoint ") + stage);
        const std::string execution = "OpExecutionMode %main LocalSize 1 1 1\n";
        assembly.erase(assembly.find(execution), execution.size());
        if (std::string(stage) == "RayGenerationKHR") {
            assembly.insert(assembly.find("OpExtension"), "OpCapability RayTracingKHR\n");
            assembly.insert(assembly.find("OpMemoryModel"), "OpExtension \"SPV_KHR_ray_tracing\"\n");
        }
        run(std::string(stage) + " private entry-point interface", assembly, 1, 1);
    }
    run("Signed32 flags and getters with no unsigned input type",
        base("\n",
             "%q = OpVariable %pFQ Function\n" + std::string(load) + initialize("%q", "%u128") +
                 "%g = OpRayQueryGetRayFlagsKHR %u %q\n",
             {}, false, true),
        1, 1);
    run("Signed getters with existing unsigned flag type",
        base("\n", "%q = OpVariable %pFQ Function\n" + std::string(load) + initialize("%q", "%u128") +
                       "%g = OpRayQueryGetRayFlagsKHR %i %q\n"),
        1, 1);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::vector<uint32_t> ordinary;
    expect(tools.Assemble("OpCapability Shader\nOpMemoryModel Logical GLSL450\nOpEntryPoint GLCompute %main \"main\"\n"
                          "OpExecutionMode %main LocalSize 1 1 1\n%void = OpTypeVoid\n%fn = OpTypeFunction %void\n"
                          "%main = OpFunction %void None %fn\n%label = OpLabel\nOpReturn\nOpFunctionEnd\n",
                          &ordinary) &&
               tools.Validate(ordinary),
           "ordinary module independently valid");
    QueryStateModule sentinel{{0xfeedbeefu, 0x12345678u}, {{11, 12}, {21, 22}}, 99, 88};
    for (bool expose : {false, true}) {
        auto output = sentinel;
        std::string diagnostic = "previous";
        expect(vel::omm::prepare_query_state(ordinary, expose, &output, &diagnostic) == QueryRewriteStatus::Success &&
                   output.words == ordinary && output.state.empty() && output.initializations == 0 && output.flag_getters == 0 &&
                   diagnostic.empty(),
               "ordinary module byte identity and cleared prior metadata");
        std::vector<std::vector<uint32_t>> malformed = {{}, {0x07230203u}, {0x07230203u, 0x10500u, 0u, 1u, 0u}, ordinary};
        malformed.back()[0] = 0;
        for (const auto& invalid : malformed) {
            output = sentinel;
            diagnostic = "previous";
            expect(vel::omm::prepare_query_state(invalid, expose, &output, &diagnostic) == QueryRewriteStatus::InvalidModule &&
                       equal(output, sentinel) && !diagnostic.empty(),
                   "invalid module preserves entire output atomically");
        }
        expect(vel::omm::prepare_query_state(ordinary, expose, nullptr) == QueryRewriteStatus::InvalidModule,
               "null output rejected");
    }
    std::printf("Independent query graph suite: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
