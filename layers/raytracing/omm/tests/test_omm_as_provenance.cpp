// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "../omm_as_provenance.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <string>
#include <unordered_map>

namespace allocation_fault {
bool enabled = false;
int budget = 0;
}  // namespace allocation_fault
void* operator new(size_t size) {
    if (allocation_fault::enabled) {
        if (allocation_fault::budget == 0) throw std::bad_alloc();
        --allocation_fault::budget;
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }

using namespace vel::omm;
namespace {
uint32_t checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}
std::vector<uint32_t> assemble(const std::string& source, bool validate = true) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string diagnostic;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* text) { diagnostic = text; });
    std::vector<uint32_t> words;
    const bool assembled = tools.Assemble(source, &words, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS);
    require(assembled, "Assemble: " + diagnostic);
    if (validate) {
        const bool valid = tools.Validate(words);
        require(valid, "Validate fixture: " + diagnostic);
    }
    return words;
}
std::string prefix(bool arrays = false, bool int64 = false, bool runtime = false) {
    std::string text = "OpCapability Shader\nOpCapability RayQueryKHR\n";
    if (int64) text += "OpCapability Int64\n";
    if (runtime) text += "OpCapability RuntimeDescriptorArray\nOpExtension \"SPV_EXT_descriptor_indexing\"\n";
    text +=
        "OpExtension \"SPV_KHR_ray_query\"\nOpMemoryModel Logical GLSL450\n"
        "OpEntryPoint GLCompute %main \"main\" %scene %counter";
    if (arrays) text += " %scenes";
    text +=
        "\nOpExecutionMode %main LocalSize 1 1 1\n"
        "OpDecorate %scene DescriptorSet 7\nOpDecorate %scene Binding 11\n";
    if (arrays) text += "OpDecorate %scenes DescriptorSet 13\nOpDecorate %scenes Binding 17\n";
    text +=
        "%void = OpTypeVoid\n%bool = OpTypeBool\n%u = OpTypeInt 32 0\n%f = OpTypeFloat 32\n"
        "%v2u = OpTypeVector %u 2\n%v4u = OpTypeVector %u 4\n%v2f = OpTypeVector %f 2\n%v3f = OpTypeVector %f 3\n"
        "%u0 = OpConstant %u 0\n%u1 = OpConstant %u 1\n%u2 = OpConstant %u 2\n%u3 = OpConstant %u 3\n"
        "%addr0 = OpConstantComposite %v2u %u1 %u2\n%addr1 = OpConstantComposite %v2u %u2 %u3\n"
        "%true = OpConstantTrue %bool\n%f0 = OpConstant %f 0\n%f1 = OpConstant %f 1\n"
        "%v0 = OpConstantComposite %v3f %f0 %f0 %f0\n%dir = OpConstantComposite %v3f %f0 %f0 %f1\n"
        "%as = OpTypeAccelerationStructureKHR\n%pAS = OpTypePointer UniformConstant %as\n"
        "%scene = OpVariable %pAS UniformConstant\n%qtype = OpTypeRayQueryKHR\n%pQ = OpTypePointer Function %qtype\n"
        "%pPrivateU = OpTypePointer Private %u\n%counter = OpVariable %pPrivateU Private %u1\n"
        "%fn = OpTypeFunction %void\n%fnAS = OpTypeFunction %as %as\n%fnUseAS = OpTypeFunction %void %as\n"
        "%fnPtr = OpTypeFunction %void %pAS\n%fnFetch = OpTypeFunction %v4u %u %u %v2u\n"
        "%fnResolve = OpTypeFunction %v2u %v2u\n%fnClassify = OpTypeFunction %u %v2u %u %u %u %v2f %bool %u\n";
    if (arrays) {
        text += runtime ? "%array3 = OpTypeRuntimeArray %as\n" : "%array3 = OpTypeArray %as %u3\n";
        text +=
            "%pArray3 = OpTypePointer UniformConstant %array3\n%scenes = OpVariable %pArray3 UniformConstant\n"
            "%fnArrayPtr = OpTypeFunction %void %pArray3\n";
    }
    if (int64) text += "%u64 = OpTypeInt 64 0\n%address64 = OpConstant %u64 8589934593\n";
    // These fixture providers expose their arguments in the result, making the
    // path arithmetic independently executable below. Production providers read
    // the owned shadow records, rather than using fixture scene constants.
    text +=
        "%1000 = OpFunction %v4u None %fnFetch\n%set = OpFunctionParameter %u\n%binding = OpFunctionParameter %u\n"
        "%index = OpFunctionParameter %v2u\n%fetchEntry = OpLabel\n%low = OpCompositeExtract %u %index 0\n"
        "%high = OpCompositeExtract %u %index 1\n%record = OpCompositeConstruct %v4u %set %binding %low %high\n"
        "OpReturnValue %record\nOpFunctionEnd\n"
        "%1001 = OpFunction %v2u None %fnResolve\n%address = OpFunctionParameter %v2u\n%resolveEntry = OpLabel\n"
        "OpReturnValue %address\nOpFunctionEnd\n"
        "%1003 = OpFunction %u None %fnClassify\n%c0 = OpFunctionParameter %v2u\n%c1 = OpFunctionParameter %u\n"
        "%c2 = OpFunctionParameter %u\n%c3 = OpFunctionParameter %u\n%c4 = OpFunctionParameter %v2f\n"
        "%c5 = OpFunctionParameter %bool\n%c6 = OpFunctionParameter %u\n%classifyEntry = OpLabel\n"
        "OpReturnValue %u2\nOpFunctionEnd\n";
    return text;
}
const char* begin_main = "%main = OpFunction %void None %fn\n%entry = OpLabel\n%q = OpVariable %pQ Function\n";
const char* initialize = "OpRayQueryInitializeKHR %q %picked %u0 %u1 %v0 %f0 %dir %f1\n";
const char* end_main = "OpReturn\nOpFunctionEnd\n";
AsProvenanceModule transform(const std::vector<uint32_t>& source, uint32_t initializations = 1) {
    AsProvenanceModule result;
    std::string diagnostic;
    const auto status = propagate_as_provenance(source, {1001, 1000}, &result, &diagnostic);
    require(status == AsProvenanceStatus::Success, "Provenance transform: " + diagnostic);
    require(result.initialization_metadata.size() == initializations, "Every initialize has provenance");
    for (const auto& binding : result.initialization_metadata) {
        require(binding.initialize_word_offset < result.words.size(), "Binding offset is within output words");
        require(spv::Op(result.words[binding.initialize_word_offset] & 0xffffu) == spv::OpRayQueryInitializeKHR,
                "Binding offset points to OUTPUT initialize");
        require(binding.metadata_value && binding.metadata_value < result.words[3], "Binding uses output SSA ID");
    }
    QueryConsumerOptions options;
    options.classify_function = 1003;
    options.initialization_metadata = result.initialization_metadata;
    QueryStateModule consumers;
    const auto consumed = rewrite_query_consumers(result.words, options, &consumers, &diagnostic);
    require(consumed == QueryRewriteStatus::Success, "Provenance feeds the real query consumer pass: " + diagnostic);
    require(consumers.initializations == initializations, "Consumer rewrites every initialization");
    return result;
}
uint32_t count(const std::vector<uint32_t>& words, spv::Op opcode, uint32_t called_function = 0) {
    uint32_t total = 0;
    for (size_t offset = 5; offset < words.size(); offset += words[offset] >> 16)
        if (spv::Op(words[offset] & 0xffffu) == opcode && (!called_function || words[offset + 3] == called_function)) ++total;
    return total;
}

// Small independent executor for the integer/control-flow subset used in these
// fixtures. It models opaque AS values as uninterpreted pairs, never BVH bytes.
// It executes application calls/stores and the pass's 64-bit pair arithmetic.
// Thus side effects and flattened paths are checked as behavior, not just text.
struct Executor {
    using Value = std::vector<uint32_t>;
    struct Inst {
        spv::Op op;
        std::vector<uint32_t> words;
        uint32_t offset;
    };
    std::vector<Inst> instructions;
    std::unordered_map<uint32_t, size_t> functions, labels;
    std::unordered_map<uint32_t, Value> globals, memory;
    std::map<uint32_t, uint32_t> call_counts;
    std::map<uint32_t, uint32_t> metadata_offsets;
    std::vector<Value> scenes, roots, fetch_indices;
    uint32_t counter = 0, entry = 0;
    explicit Executor(const std::vector<uint32_t>& words, const std::vector<QueryInitializationMetadata>& bindings = {}) {
        for (const auto& binding : bindings) metadata_offsets.emplace(binding.initialize_word_offset, binding.metadata_value);
        bool function = false;
        for (size_t offset = 5; offset < words.size(); offset += words[offset] >> 16) {
            const uint32_t length = words[offset] >> 16;
            Inst inst{
                spv::Op(words[offset] & 0xffffu), {words.begin() + offset, words.begin() + offset + length}, uint32_t(offset)};
            const auto& w = inst.words;
            if (inst.op == spv::OpEntryPoint) entry = w[2];
            if (inst.op == spv::OpFunction) {
                functions.emplace(w[2], instructions.size());
                function = true;
            }
            if (inst.op == spv::OpLabel) labels.emplace(w[1], instructions.size());
            if (!function) {
                if (inst.op == spv::OpConstant || inst.op == spv::OpSpecConstant) globals[w[2]] = Value(w.begin() + 3, w.end());
                if (inst.op == spv::OpConstantTrue) globals[w[2]] = {1};
                if (inst.op == spv::OpConstantFalse) globals[w[2]] = {0};
                if (inst.op == spv::OpConstantComposite) {
                    auto& value = globals[w[2]];
                    for (size_t i = 3; i < w.size(); ++i)
                        value.insert(value.end(), globals.at(w[i]).begin(), globals.at(w[i]).end());
                }
                if (inst.op == spv::OpVariable) {
                    globals[w[2]] = {w[2]};
                    if (w.size() == 5) {
                        memory[w[2]] = globals.at(w[4]);
                        counter = w[2];
                    }
                }
            }
            if (inst.op == spv::OpFunctionEnd) function = false;
            instructions.push_back(std::move(inst));
        }
    }
    Value run(uint32_t function, const std::vector<Value>& arguments = {}) {
        ++call_counts[function];
        if (function == 1000) fetch_indices.push_back(arguments.at(2));
        auto values = globals;
        size_t parameter = 0;
        uint32_t block = 0, predecessor = 0;
        for (size_t pc = functions.at(function) + 1, steps = 0; steps < 100000; ++pc, ++steps) {
            const auto& inst = instructions.at(pc);
            const auto& w = inst.words;
            const auto v = [&](size_t operand) -> const Value& { return values.at(w.at(operand)); };
            switch (inst.op) {
                case spv::OpFunctionParameter:
                    values[w[2]] = arguments.at(parameter++);
                    break;
                case spv::OpLabel:
                    block = w[1];
                    break;
                case spv::OpVariable:
                    values[w[2]] = {w[2]};
                    memory[w[2]] = w.size() == 5 ? values.at(w[4]) : Value{0};
                    break;
                case spv::OpLoad:
                    values[w[2]] = memory.at(v(3).at(0));
                    break;
                case spv::OpStore:
                    memory[v(1).at(0)] = v(2);
                    break;
                case spv::OpAccessChain:
                case spv::OpInBoundsAccessChain:
                case spv::OpCopyObject:
                case spv::OpBitcast:
                case spv::OpSConvert:
                case spv::OpUConvert:
                case spv::OpConvertUToAccelerationStructureKHR:
                    values[w[2]] = v(3);
                    break;
                case spv::OpCompositeConstruct: {
                    Value value;
                    for (size_t i = 3; i < w.size(); ++i) value.insert(value.end(), v(i).begin(), v(i).end());
                    values[w[2]] = std::move(value);
                    break;
                }
                case spv::OpCompositeExtract:
                    values[w[2]] = {v(3).at(w[4])};
                    break;
                case spv::OpVectorShuffle: {
                    Value source = v(3), value;
                    source.insert(source.end(), v(4).begin(), v(4).end());
                    for (size_t i = 5; i < w.size(); ++i) value.push_back(source.at(w[i]));
                    values[w[2]] = std::move(value);
                    break;
                }
                case spv::OpIAdd:
                case spv::OpIMul: {
                    Value value;
                    for (size_t i = 0; i < v(3).size(); ++i)
                        value.push_back(inst.op == spv::OpIAdd ? v(3)[i] + v(4)[i] : v(3)[i] * v(4)[i]);
                    values[w[2]] = std::move(value);
                    break;
                }
                case spv::OpIAddCarry:
                case spv::OpUMulExtended: {
                    const uint64_t a = v(3).at(0), b = v(4).at(0);
                    const auto value = inst.op == spv::OpIAddCarry ? a + b : a * b;
                    values[w[2]] = {uint32_t(value), uint32_t(value >> 32)};
                    break;
                }
                case spv::OpSelect:
                    values[w[2]] = v(3).at(0) ? v(4) : v(5);
                    break;
                case spv::OpShiftRightArithmetic:
                    values[w[2]] = {uint32_t(int32_t(v(3).at(0)) >> v(4).at(0))};
                    break;
                case spv::OpPhi:
                    for (size_t i = 3; i < w.size(); i += 2)
                        if (w[i + 1] == predecessor) values[w[2]] = values.at(w[i]);
                    require(values.count(w[2]) != 0, "Executor selects a phi predecessor");
                    break;
                case spv::OpFunctionCall: {
                    std::vector<Value> args;
                    for (size_t i = 4; i < w.size(); ++i) args.push_back(v(i));
                    values[w[2]] = run(w[3], args);
                    break;
                }
                case spv::OpBranch:
                    predecessor = block;
                    pc = labels.at(w[1]) - 1;
                    break;
                case spv::OpBranchConditional:
                    predecessor = block;
                    pc = labels.at(v(1).at(0) ? w[2] : w[3]) - 1;
                    break;
                case spv::OpRayQueryInitializeKHR:
                    scenes.push_back(v(2));
                    if (metadata_offsets.count(inst.offset)) roots.push_back(values.at(metadata_offsets.at(inst.offset)));
                    break;
                case spv::OpReturn:
                    return {};
                case spv::OpReturnValue:
                    return v(1);
                case spv::OpSelectionMerge:
                case spv::OpLine:
                case spv::OpNoLine:
                    break;
                default:
                    require(false, "Executor supports every fixture instruction: " + std::to_string(uint32_t(inst.op)));
            }
        }
        require(false, "Executor terminates");
        return {};
    }
    void run() { run(entry); }
};

void singleton_and_byvalue() {
    const auto source =
        assemble(prefix() +
                 "%1002 = OpFunction %void None %fnUseAS\n%arg = OpFunctionParameter %as\n%helperEntry = OpLabel\n"
                 "%qHelper = OpVariable %pQ Function\n%copy = OpCopyObject %as %arg\n"
                 "OpRayQueryInitializeKHR %qHelper %copy %u0 %u1 %v0 %f0 %dir %f1\nOpReturn\nOpFunctionEnd\n" +
                 begin_main + "%loaded = OpLoad %as %scene\n%call = OpFunctionCall %void %1002 %loaded\n" + end_main);
    const auto result = transform(source);
    require(result.descriptor_loads == 1 && result.integer_conversions == 0, "One original descriptor producer");
    require(count(result.words, spv::OpFunctionCall, 1000) == 1, "Exactly one coherent descriptor fetch");
    Executor execution(result.words, result.initialization_metadata);
    execution.run();
    require(execution.scenes == std::vector<Executor::Value>{{7, 11}}, "Native descriptor address comes from same fetch");
    require(execution.roots == std::vector<Executor::Value>{{0, 0}}, "Singleton metadata survives by-value helper/copy");
}
void integer_phi_and_select() {
    for (bool wide : {false, true}) {
        std::string body =
            "%a = OpConvertUToAccelerationStructureKHR %as %addr0\n"
            "%b = OpConvertUToAccelerationStructureKHR %as %addr1\n"
            "OpSelectionMerge %join None\nOpBranchConditional %true %left %right\n"
            "%left = OpLabel\nOpBranch %join\n%right = OpLabel\nOpBranch %join\n%join = OpLabel\n"
            "%picked = OpPhi %as %a %left %b %right\n";
        if (wide) body = "%picked = OpConvertUToAccelerationStructureKHR %as %address64\n";
        const auto source = assemble(prefix(false, wide) + begin_main + body + initialize + end_main);
        const auto result = transform(source);
        Executor execution(result.words, result.initialization_metadata);
        execution.run();
        require(execution.scenes == std::vector<Executor::Value>{{1, 2}}, "Original native AS selected");
        require(execution.roots == execution.scenes, "Integer address provenance follows phi and 64-bit bitcast");
    }
    const auto selected =
        assemble(prefix() + begin_main +
                 "%chosen = OpSelect %v2u %true %addr0 %addr1\n%picked = OpConvertUToAccelerationStructureKHR %as %chosen\n" +
                 initialize + end_main);
    const auto result = transform(selected);
    Executor execution(result.words, result.initialization_metadata);
    execution.run();
    require(execution.roots == std::vector<Executor::Value>{{1, 2}}, "Integer select occurs before opaque conversion");
}

void signed_and_narrow_descriptor_indices() {
    for (uint32_t width : {8u, 16u, 32u, 64u}) {
        auto text = prefix(true, width == 64);
        if (width < 32) text.insert(0, "OpCapability Int" + std::to_string(width) + "\n");
        text.insert(text.find("%1000 ="),
                    "%indexType = OpTypeInt " + std::to_string(width) + " 1\n%indexOne = OpConstant %indexType 1\n");
        const auto source =
            assemble(text + begin_main +
                     "%dynamic = OpCopyObject %indexType %indexOne\n%pointer = OpAccessChain %pAS %scenes %dynamic\n"
                     "%picked = OpLoad %as %pointer None\n" +
                     initialize + end_main);
        const auto result = transform(source);
        Executor execution(result.words, result.initialization_metadata);
        execution.run();
        require(execution.roots == std::vector<Executor::Value>{{1, 0}},
                "Signed descriptor index is normalized without Int64 requirement");
    }
}

void singleton_phi_and_return() {
    const auto source = assemble(prefix() +
                                 "%1002 = OpFunction %as Pure|Const %fnAS\n%arg = OpFunctionParameter %as\n%helperEntry = OpLabel\n"
                                 "OpReturnValue %arg\nOpFunctionEnd\n" +
                                 begin_main +
                                 "%a = OpLoad %as %scene\n%b = OpLoad %as %scene\n"
                                 "OpSelectionMerge %join None\nOpBranchConditional %true %left %right\n"
                                 "%left = OpLabel\nOpBranch %join\n%right = OpLabel\nOpBranch %join\n%join = OpLabel\n"
                                 "%phi = OpPhi %as %a %left %b %right\n%picked = OpFunctionCall %as %1002 %phi\n" +
                                 initialize + end_main);
    const auto result = transform(source);
    require(count(result.words, spv::OpPhi) == 2, "Singleton AS Phi has one parallel uvec2 Phi");
    for (size_t offset = 5; offset < result.words.size(); offset += result.words[offset] >> 16)
        if (spv::Op(result.words[offset] & 0xffffu) == spv::OpFunction && result.words[offset + 2] == 1002)
            require((result.words[offset + 3] & (spv::FunctionControlPureMask | spv::FunctionControlConstMask)) == 0,
                    "New metadata return stores invalidate original Pure/Const optimization promises");
    Executor execution(result.words, result.initialization_metadata);
    execution.run();
    require(execution.scenes == std::vector<Executor::Value>{{7, 11}} && execution.roots == std::vector<Executor::Value>{{0, 0}},
            "Singleton descriptor survives valid Phi and AS return");
}

void independent_reinitializations() {
    const auto source =
        assemble(prefix() + begin_main + "%picked = OpConvertUToAccelerationStructureKHR %as %addr0\n" + initialize +
                 "%other = OpConvertUToAccelerationStructureKHR %as %addr1\n"
                 "OpRayQueryInitializeKHR %q %other %u0 %u1 %v0 %f0 %dir %f1\n" +
                 end_main);
    const auto result = transform(source, 2);
    require(result.initialization_metadata[0].metadata_value != result.initialization_metadata[1].metadata_value,
            "Reinitializations use independent AS SSA provenance");
    Executor execution(result.words, result.initialization_metadata);
    execution.run();
    require(execution.roots == std::vector<Executor::Value>{{1, 2}, {2, 3}}, "Different AS operands select different scene roots");
    require(execution.scenes == execution.roots, "Native address and provenance agree at each reinitialization");
}
void dynamic_pointer_helpers() {
    for (bool subarray : {false, true})
        for (bool runtime : {false, true}) {
            std::string helper =
                subarray
                    ? "%1002 = OpFunction %void None %fnArrayPtr\n%arg = OpFunctionParameter %pArray3\n%helperEntry = OpLabel\n"
                    : "%1002 = OpFunction %void None %fnPtr\n%arg = OpFunctionParameter %pAS\n%helperEntry = OpLabel\n";
            helper += "%qHelper = OpVariable %pQ Function\n";
            if (subarray) helper += "%element = OpAccessChain %pAS %arg %u2\n";
            helper += std::string("%loaded = OpLoad %as ") + (subarray ? "%element\n" : "%arg\n") +
                      "OpRayQueryInitializeKHR %qHelper %loaded %u0 %u1 %v0 %f0 %dir %f1\nOpReturn\nOpFunctionEnd\n";
            const std::string pointer = subarray ? "%pArray3 %scenes\n" : "%pAS %scenes %dynamic\n";
            const auto source = assemble(prefix(true, false, runtime) + helper + begin_main +
                                         "%dynamic = OpLoad %u %counter\n%pointer = OpAccessChain " + pointer +
                                         "%call = OpFunctionCall %void %1002 %pointer\n" + end_main);
            const auto result = transform(source);
            Executor execution(result.words, result.initialization_metadata);
            execution.run();
            const uint32_t expected = subarray ? 2 : 1;
            require(execution.fetch_indices == std::vector<Executor::Value>{{expected, 0}},
                    "Dynamic descriptor index follows pointer helper");
            require(execution.scenes == std::vector<Executor::Value>{{13, 17}}, "Descriptor retains set and binding");
            require(execution.roots == std::vector<Executor::Value>{{expected, 0}}, "Numeric pointer paths survive helper passage");
            // No extra UniformConstant pointer argument is introduced.
            uint32_t parameter_count = 0;
            bool in_helper = false;
            for (size_t offset = 5; offset < result.words.size(); offset += result.words[offset] >> 16) {
                const auto op = spv::Op(result.words[offset] & 0xffffu);
                if (op == spv::OpFunction) in_helper = result.words[offset + 2] == 1002;
                if (in_helper && op == spv::OpFunctionParameter) ++parameter_count;
            }
            require(parameter_count == 4, "Descriptor pointer helper appends only set,binding,index numeric parameters");
        }
}
void return_side_effects() {
    const auto source = assemble(prefix() +
                                 "%1002 = OpFunction %as None %fnAS\n%arg = OpFunctionParameter %as\n%helperEntry = OpLabel\n"
                                 "%old = OpLoad %u %counter\n%increment = OpIAdd %u %old %u1\nOpStore %counter %increment\n"
                                 "OpReturnValue %arg\nOpFunctionEnd\n" +
                                 begin_main +
                                 "%a = OpConvertUToAccelerationStructureKHR %as %addr0\n"
                                 "%first = OpFunctionCall %as %1002 %a\n%picked = OpFunctionCall %as %1002 %first\n" +
                                 initialize + end_main);
    const auto original = source;
    const auto result = transform(source);
    require(source == original, "Input source remains byte-identical");
    require(count(result.words, spv::OpFunctionCall, 1002) == 2, "Original side-effect helper calls are each emitted once");
    Executor before(source), after(result.words, result.initialization_metadata);
    before.run();
    after.run();
    require(before.call_counts[1002] == 2 && after.call_counts[1002] == 2, "Executed helper side-effect count is preserved");
    require(before.memory.at(before.counter) == Executor::Value{3} && after.memory.at(after.counter) == Executor::Value{3},
            "Application-visible counter is exactly initial 1 plus two calls");
    require(before.scenes == after.scenes && after.roots == before.scenes, "Returned native AS and metadata agree");
    uint32_t slot0 = 0, slot1 = 0;
    for (size_t offset = 5; offset < result.words.size(); offset += result.words[offset] >> 16)
        if (spv::Op(result.words[offset] & 0xffffu) == spv::OpFunctionCall && result.words[offset + 3] == 1002) {
            const auto slot = result.words[offset + (result.words[offset] >> 16) - 1];
            if (!slot0)
                slot0 = slot;
            else
                slot1 = slot;
        }
    require(slot0 && slot1 && slot0 != slot1, "Each return has an independent Function metadata memory object");
}
void negative_and_failure_cases() {
    const auto integer =
        assemble(prefix() + begin_main + "%picked = OpConvertUToAccelerationStructureKHR %as %addr0\n" + initialize + end_main);
    const auto descriptor = assemble(prefix() + begin_main + "%picked = OpLoad %as %scene\n" + initialize + end_main);
    auto reject = [&](const std::vector<uint32_t>& input, const AsProvenanceOptions& options, AsProvenanceStatus expected,
                      const char* label) {
        AsProvenanceModule output{{0x12345678u}, {{91, 92}}, 93, 94};
        std::string diagnostic;
        const auto status = propagate_as_provenance(input, options, &output, &diagnostic);
        require(status == expected, std::string(label) + ": " + diagnostic);
        require(output.words == std::vector<uint32_t>{0x12345678u} && output.initialization_metadata.size() == 1 &&
                    output.initialization_metadata[0].initialize_word_offset == 91 &&
                    output.initialization_metadata[0].metadata_value == 92 && output.integer_conversions == 93 &&
                    output.descriptor_loads == 94,
                std::string(label) + " leaves full output unchanged");
        require(!diagnostic.empty(), std::string(label) + " reports a reason");
    };
    reject({}, {}, AsProvenanceStatus::InvalidModule, "Malformed source");
    reject(integer, {}, AsProvenanceStatus::Unsupported, "Missing integer provider");
    reject(descriptor, {}, AsProvenanceStatus::Unsupported, "Missing descriptor provider");
    reject(integer, {1000, 1000}, AsProvenanceStatus::Unsupported, "Wrong integer provider signature");
    reject(descriptor, {1001, 1001}, AsProvenanceStatus::Unsupported, "Wrong descriptor provider signature");
    const auto unknown = assemble(prefix() + begin_main + "%picked = OpUndef %as\n" + initialize + end_main, false);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_2);
    reject(unknown, {1001, 1000}, validator.Validate(unknown) ? AsProvenanceStatus::Unsupported : AsProvenanceStatus::InvalidModule,
           "Unknown AS producer");
    // Some SDK validator versions miss the extension's array-origin rule. The
    // compiler must still reject composite-derived scalar Copy/Phi graphs.
    const auto bad_array = assemble(prefix(true) + begin_main +
                                        "%ptr = OpAccessChain %pAS %scenes %u0\n%loaded = OpLoad %as %ptr\n"
                                        "%picked = OpCopyObject %as %loaded\n" +
                                        initialize + end_main,
                                    false);
    reject(bad_array, {1001, 1000},
           validator.Validate(bad_array) ? AsProvenanceStatus::Unsupported : AsProvenanceStatus::InvalidModule,
           "Composite-derived AS cannot become scalar copy graph");
    const auto bad_select =
        assemble(prefix() + begin_main +
                     "%a = OpConvertUToAccelerationStructureKHR %as %addr0\n%b = OpConvertUToAccelerationStructureKHR %as %addr1\n"
                     "%picked = OpSelect %as %true %a %b\n" +
                     initialize + end_main,
                 false);
    reject(bad_select, {1001, 1000}, AsProvenanceStatus::InvalidModule, "Opaque AS Select is invalid SPIR-V");
    auto nested_prefix = prefix(true);
    const auto nested_type = nested_prefix.find("%pArray3 =");
    nested_prefix.insert(nested_type, "%nested = OpTypeArray %array3 %u2\n");
    nested_prefix.replace(nested_prefix.find("%pArray3 = OpTypePointer UniformConstant %array3"),
                          std::string("%pArray3 = OpTypePointer UniformConstant %array3").size(),
                          "%pArray3 = OpTypePointer UniformConstant %nested");
    const auto nested =
        assemble(nested_prefix + begin_main + "%pointer = OpAccessChain %pAS %scenes %u0 %u0\n%picked = OpLoad %as %pointer\n" +
                     initialize + end_main,
                 false);
    reject(nested, {1001, 1000}, AsProvenanceStatus::InvalidModule, "Nested opaque descriptor arrays violate Vulkan type rules");
    for (int budget : {0, 1, 2, 4, 8, 32}) {
        AsProvenanceModule output{{0x12345678u}, {{91, 92}}, 93, 94};
        std::string diagnostic;
        allocation_fault::budget = budget;
        allocation_fault::enabled = true;
        const auto status = propagate_as_provenance(integer, {1001, 1000}, &output, &diagnostic);
        allocation_fault::enabled = false;
        require(status == AsProvenanceStatus::OutOfMemory, "Sustained allocation failure returns OutOfMemory");
        require(output.words == std::vector<uint32_t>{0x12345678u} && output.integer_conversions == 93 &&
                    output.descriptor_loads == 94 && output.initialization_metadata[0].metadata_value == 92,
                "Allocation failure preserves output atomically");
    }
}
}  // namespace
int main() {
    singleton_and_byvalue();
    integer_phi_and_select();
    signed_and_narrow_descriptor_indices();
    singleton_phi_and_return();
    independent_reinitializations();
    dynamic_pointer_helpers();
    return_side_effects();
    negative_and_failure_cases();
    std::printf("OMM AS provenance: %u checks passed\n", checks);
}
