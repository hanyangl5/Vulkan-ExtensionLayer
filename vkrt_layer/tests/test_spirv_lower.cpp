/* SPDX-License-Identifier: MIT
 *
 * test_spirv_lower.cpp - drives vkrt_spirv_lower over a real ray-query
 * shader and asserts the rewrite actually happened.
 *
 *   usage: test_spirv_lower <user.spv> <library.spv> <out.spv> [--require-sbt-offset]
 *
 * Checks:
 *   1. the input really had OpRayQuery* / OpTypeRayQueryKHR /
 *      OpTypeAccelerationStructureKHR (otherwise the test is vacuous)
 *   2. the output has none of those
 *   3. the output has OpFunctionCall to the sw_* traversal
 *   4. SBT-offset getters in the input become calls to the matching helper
 *   5. the output validates inside vkrt_spirv_lower (SPIRV-Tools is required)
 *   6. scalar/array/integer/helper/SSA AS operands reach q.as_address;
 *      runtime arrays retain direct and grouped nonuniform decoration
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <spirv-tools/libspirv.hpp>

#include "vkrt_spirv_lower.h"

#define SPV_OP(w)  ((uint16_t)((w) & 0xFFFFu))
#define SPV_LEN(w) ((uint16_t)((w) >> 16))

static std::vector<uint32_t>
read_spirv(const char *path)
{
   FILE *f = std::fopen(path, "rb");
   if (!f) {
      std::fprintf(stderr, "cannot open %s\n", path);
      std::exit(2);
   }
   std::fseek(f, 0, SEEK_END);
   long sz = std::ftell(f);
   std::fseek(f, 0, SEEK_SET);
   std::vector<uint32_t> w((size_t)sz / 4);
   if (std::fread(w.data(), 4, w.size(), f) != w.size()) {
      std::fprintf(stderr, "short read on %s\n", path);
      std::exit(2);
   }
   std::fclose(f);
   return w;
}

static void
write_spirv(const char *path, const uint32_t *w, size_t n)
{
   FILE *f = std::fopen(path, "wb");
   if (!f) {
      std::fprintf(stderr, "cannot write %s\n", path);
      std::exit(2);
   }
   std::fwrite(w, 4, n, f);
   std::fclose(f);
}

struct Counts {
   int op_ray_query = 0;
   int type_ray_query = 0;
   int type_as = 0;
   int op_function_call = 0;
   int op_trace_ray = 0;
   int op_sbt_offset = 0;
   int rt_requirements = 0;
};

static Counts
classify(const std::vector<uint32_t> &w)
{
   Counts c;
   for (size_t i = 5; i < w.size();) {
      uint16_t op = SPV_OP(w[i]);
      uint16_t len = SPV_LEN(w[i]);
      if (len == 0 || i + len > w.size())
         break;
      if (op == 4472) c.type_ray_query++;
      else if (op == 5341) c.type_as++;
      else if (op == 57) c.op_function_call++;
      else if (op == 4445 || op == 4446) c.op_trace_ray++;
      else if ((op >= 4473 && op <= 4479) || op == 5340 ||
               (op >= 6016 && op <= 6032))
         c.op_ray_query++;
      if (op == 6021) c.op_sbt_offset++;
      if (op == 17 && len == 2 && (w[i + 1] == 4472 || w[i + 1] == 4479)) c.rt_requirements++;
      if (op == 10 && len >= 2) {
          const char *extension = reinterpret_cast<const char *>(&w[i + 1]);
          if (!std::strcmp(extension, "SPV_KHR_ray_query") || !std::strcmp(extension, "SPV_KHR_ray_tracing")) c.rt_requirements++;
      }
      i += len;
   }
   return c;
}

/* Does the module call a function whose OpName starts with `prefix`? */
static int count_calls_function_named(const std::vector<uint32_t> &w, const char *prefix) {
    /* Collect ids of OpFunctionCall targets. */
    std::vector<uint32_t> targets;
    for (size_t i = 5; i < w.size();) {
        uint16_t op = SPV_OP(w[i]);
        uint16_t len = SPV_LEN(w[i]);
        if (len == 0 || i + len > w.size()) break;
        if (op == 57 && len >= 4) /* OpFunctionCall %rty %res %fn ... */
            targets.push_back(w[i + 3]);
        i += len;
    }

    /* Map OpName id -> name. */
    size_t plen = std::strlen(prefix);
    int count = 0;
    for (size_t i = 5; i < w.size();) {
        uint16_t op = SPV_OP(w[i]);
        uint16_t len = SPV_LEN(w[i]);
        if (len == 0 || i + len > w.size()) break;
        if (op == 5 && len >= 3) { /* OpName %target "str" */
            uint32_t id = w[i + 1];
            std::string s;
            for (uint16_t k = 2; k < len; k++) {
                char buf[5];
                std::memcpy(buf, &w[i + k], 4);
                for (int b = 0; b < 4; b++) {
                    if (buf[b] == '\0') break;
                    s.push_back(buf[b]);
                }
            }
            for (uint32_t t : targets) {
                if (t == id && s.compare(0, plen, prefix) == 0) count++;
            }
        }
        i += len;
    }
    return count;
}

static bool calls_function_named(const std::vector<uint32_t> &w, const char *prefix) {
    return count_calls_function_named(w, prefix) != 0;
}

/* GLSL emits opcode 6021 with an unsigned result. Also exercise the legal
 * signed SPIR-V result while preserving the fixture's unsigned consumers. */
static bool check_signed_sbt_variant(const std::vector<uint32_t> &user, const std::vector<uint32_t> &lib) {
    std::vector<uint32_t> variant(user.begin(), user.begin() + 5);
    uint32_t signed_type = 0;
    for (size_t i = 5; i < user.size(); i += SPV_LEN(user[i]))
        if (SPV_OP(user[i]) == 21 && SPV_LEN(user[i]) == 4 && user[i + 2] == 32 && user[i + 3] == 1) signed_type = user[i + 1];
    if (!signed_type) {
        signed_type = variant[3]++;
    }
    bool inserted_type = false;
    for (size_t i = 5; i < user.size();) {
        uint32_t len = SPV_LEN(user[i]);
        if (!inserted_type && SPV_OP(user[i]) == 54) {
            bool already_defined = false;
            for (size_t j = 5; j < user.size(); j += SPV_LEN(user[j]))
                already_defined |= SPV_OP(user[j]) == 21 && user[j + 1] == signed_type;
            if (!already_defined) variant.insert(variant.end(), {(4u << 16) | 21u, signed_type, 32u, 1u});
            inserted_type = true;
        }
        if (SPV_OP(user[i]) == 6021) {
            uint32_t signed_value = variant[3]++;
            variant.insert(variant.end(), {user[i], signed_type, signed_value, user[i + 3], user[i + 4], (4u << 16) | 124u,
                                           user[i + 1], user[i + 2], signed_value});
        } else {
            variant.insert(variant.end(), user.begin() + i, user.begin() + i + len);
        }
        i += len;
    }
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_2);
    if (!validator.Validate(variant)) {
        std::fprintf(stderr, "FAIL: signed SBT-offset regression fixture is invalid\n");
        return false;
    }
    uint32_t *out = nullptr;
    size_t count = 0;
    char error[1024];
    int result = vkrt_spirv_lower(variant.data(), variant.size(), lib.data(), lib.size(), &out, &count, error, sizeof(error));
    vkrt_spirv_lower_free(out);
    if (result) std::fprintf(stderr, "FAIL: signed SBT-offset lowering: %s\n", error);
    return result == 0;
}

/* The external CTS flow/image and query-wrapper cases exposed two independent
 * lowering defects. Retain a small validated compile regression for CI; the
 * real CTS executions remain the execution/behavior evidence. */
static bool check_function_query_image(const std::vector<uint32_t> &lib) {
    const char *source = R"spv(
OpCapability Shader
OpCapability RayQueryKHR
OpExtension "SPV_KHR_ray_query"
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %scene %target
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %scene DescriptorSet 0
OpDecorate %scene Binding 0
OpDecorate %target DescriptorSet 0
OpDecorate %target Binding 1
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v3 = OpTypeVector %float 3
%v3u = OpTypeVector %uint 3
%v4u = OpTypeVector %uint 4
%zero = OpConstant %uint 0
%mask = OpConstant %uint 255
%f0 = OpConstant %float 0
%f1 = OpConstant %float 1
%origin = OpConstantComposite %v3 %f0 %f0 %f0
%direction = OpConstantComposite %v3 %f0 %f0 %f1
%coord = OpConstantComposite %v3u %zero %zero %zero
%texel = OpConstantComposite %v4u %zero %zero %zero %zero
%as = OpTypeAccelerationStructureKHR
%asptr = OpTypePointer UniformConstant %as
%scene = OpVariable %asptr UniformConstant
%image = OpTypeImage %uint 3D 0 0 0 2 R32ui
%imageptr = OpTypePointer UniformConstant %image
%target = OpVariable %imageptr UniformConstant
%query = OpTypeRayQueryKHR
%queryptr = OpTypePointer Function %query
%fn = OpTypeFunction %void
%helperfn = OpTypeFunction %void %queryptr
%helper = OpFunction %void None %helperfn
%arg = OpFunctionParameter %queryptr
%helperentry = OpLabel
%handle = OpLoad %as %scene
OpRayQueryInitializeKHR %arg %handle %zero %mask %origin %f0 %direction %f1
OpReturn
OpFunctionEnd
%main = OpFunction %void None %fn
%entry = OpLabel
%q = OpVariable %queryptr Function
%called = OpFunctionCall %void %helper %q
%destination = OpLoad %image %target
OpImageWrite %destination %coord %texel
OpReturn
OpFunctionEnd
)spv";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::vector<uint32_t> words;
    if (!tools.Assemble(source, &words) || !tools.Validate(words)) {
        std::fprintf(stderr, "FAIL: Function-query/storage-image regression fixture invalid\n");
        return false;
    }
    uint32_t *out = nullptr;
    size_t count = 0;
    char error[1024];
    const int result = vkrt_spirv_lower(words.data(), words.size(), lib.data(), lib.size(), &out, &count, error, sizeof(error));
    vkrt_spirv_lower_free(out);
    if (result) std::fprintf(stderr, "FAIL: Function-query/storage-image lowering: %s\n", error);
    return result == 0;
}

static std::string spirv_string(const std::vector<uint32_t> &words, size_t begin, size_t end) {
    const char *text = reinterpret_cast<const char *>(words.data() + begin);
    size_t limit = (end - begin) * sizeof(uint32_t), length = 0;
    while (length < limit && text[length]) ++length;
    return std::string(text, length);
}

/* Compile checks also inspect the dataflow into q.as_address. Valid output
 * alone would miss silently replacing every AS operand with one descriptor. */
static bool check_as_value_stores(const std::vector<uint32_t> &words, const std::vector<std::string> &expected) {
    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, uint32_t> constants, value_types, pointer_pointees;
    uint32_t state_type = 0, address_member = 0;
    for (size_t i = 5; i < words.size(); i += SPV_LEN(words[i])) {
        uint32_t op = SPV_OP(words[i]), len = SPV_LEN(words[i]);
        if (op == 5) names[words[i + 1]] = spirv_string(words, i + 2, i + len);
        if (op == 6 && spirv_string(words, i + 3, i + len) == "as_address") {
            state_type = words[i + 1];
            address_member = words[i + 2];
        }
        if (op == 32) pointer_pointees[words[i + 1]] = words[i + 3];
        if (op == 43 && len == 4) constants[words[i + 2]] = words[i + 3];
        if (op == 55 || op == 59 || op == 65) value_types[words[i + 2]] = words[i + 1];
    }
    std::unordered_set<uint32_t> address_pointers, stored;
    for (size_t i = 5; i < words.size(); i += SPV_LEN(words[i])) {
        if (SPV_OP(words[i]) != 65 || SPV_LEN(words[i]) != 5) continue;
        uint32_t base_type = value_types[words[i + 3]];
        auto index = constants.find(words[i + 4]);
        if (state_type && pointer_pointees[base_type] == state_type && index != constants.end() && index->second == address_member)
            address_pointers.insert(words[i + 2]);
    }
    for (size_t i = 5; i < words.size(); i += SPV_LEN(words[i]))
        if (SPV_OP(words[i]) == 62 && address_pointers.count(words[i + 1])) stored.insert(words[i + 2]);
    bool valid = true;
    for (const std::string &name : expected) {
        bool found = false;
        for (const auto &entry : names) found |= entry.second == name && stored.count(entry.first);
        if (!found) {
            std::fprintf(stderr, "FAIL: AS value %s was not stored into q.as_address\n", name.c_str());
            valid = false;
        }
    }
    return valid;
}

static bool lower_assembly(const char *label, const char *source, const std::vector<uint32_t> &lib,
                           std::vector<uint32_t> *lowered) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    tools.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
        std::fprintf(stderr, "fixture diagnostic: %s\n", message);
    });
    std::vector<uint32_t> user;
    if (!tools.Assemble(source, &user) || !tools.Validate(user)) {
        std::fprintf(stderr, "FAIL: %s regression fixture invalid\n", label);
        return false;
    }
    uint32_t *out = nullptr;
    size_t count = 0;
    char error[1024];
    int result = vkrt_spirv_lower(user.data(), user.size(), lib.data(), lib.size(), &out, &count, error, sizeof(error));
    if (!result) lowered->assign(out, out + count);
    vkrt_spirv_lower_free(out);
    if (result) std::fprintf(stderr, "FAIL: %s lowering: %s\n", label, error);
    return result == 0;
}

static bool check_as_operand_values(const std::vector<uint32_t> &lib) {
    const char *source = R"spv(
OpCapability Shader
OpCapability Int64
OpCapability RayQueryKHR
OpExtension "SPV_KHR_ray_query"
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %scene %scenes %pc
OpExecutionMode %main LocalSize 1 1 1
OpName %scalar "regression_scalar"
OpName %arrayval "regression_array"
OpName %root64 "regression_u64"
OpName %root2 "regression_uvec2"
OpName %returned "regression_helper_value"
OpName %joined "regression_phi"
OpName %loaded "regression_helper_pointer"
OpName %element "regression_element_pointer"
OpDecorate %scene DescriptorSet 2
OpDecorate %scene Binding 7
OpDecorate %scenes DescriptorSet 3
OpDecorate %scenes Binding 11
OpDecorate %PC Block
OpMemberDecorate %PC 0 Offset 0
OpMemberDecorate %PC 1 Offset 8
OpMemberDecorate %PC 2 Offset 16
%void = OpTypeVoid
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%ulong = OpTypeInt 64 0
%v2u = OpTypeVector %uint 2
%float = OpTypeFloat 32
%v3 = OpTypeVector %float 3
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%mask = OpConstant %uint 255
%f0 = OpConstant %float 0
%f1 = OpConstant %float 1
%origin = OpConstantComposite %v3 %f0 %f0 %f0
%direction = OpConstantComposite %v3 %f0 %f0 %f1
%PC = OpTypeStruct %uint %ulong %v2u
%pcptr = OpTypePointer PushConstant %PC
%pcuint = OpTypePointer PushConstant %uint
%pculong = OpTypePointer PushConstant %ulong
%pcv2u = OpTypePointer PushConstant %v2u
%pc = OpVariable %pcptr PushConstant
%as = OpTypeAccelerationStructureKHR
%asptr = OpTypePointer UniformConstant %as
%asarray = OpTypeArray %as %three
%asarrayptr = OpTypePointer UniformConstant %asarray
%scene = OpVariable %asptr UniformConstant
%scenes = OpVariable %asarrayptr UniformConstant
%query = OpTypeRayQueryKHR
%queryptr = OpTypePointer Function %query
%fn = OpTypeFunction %void
%valuefn = OpTypeFunction %as %as
%pointerfn = OpTypeFunction %as %asptr
%valuehelper = OpFunction %as None %valuefn
%valuearg = OpFunctionParameter %as
%valueentry = OpLabel
OpReturnValue %valuearg
OpFunctionEnd
%pointerhelper = OpFunction %as None %pointerfn
%pointerarg = OpFunctionParameter %asptr
%pointerentry = OpLabel
%pointervalue = OpLoad %as %pointerarg
OpReturnValue %pointervalue
OpFunctionEnd
%main = OpFunction %void None %fn
%entry = OpLabel
%q = OpVariable %queryptr Function
%idxptr = OpAccessChain %pcuint %pc %zero
%idx = OpLoad %uint %idxptr
%root64ptr = OpAccessChain %pculong %pc %one
%address64 = OpLoad %ulong %root64ptr
%root2ptr = OpAccessChain %pcv2u %pc %two
%address2 = OpLoad %v2u %root2ptr
%scalar = OpLoad %as %scene
OpRayQueryInitializeKHR %q %scalar %zero %mask %origin %f0 %direction %f1
%selected = OpAccessChain %asptr %scenes %idx
%arrayval = OpLoad %as %selected
OpRayQueryInitializeKHR %q %arrayval %zero %mask %origin %f0 %direction %f1
%root64 = OpConvertUToAccelerationStructureKHR %as %address64
OpRayQueryInitializeKHR %q %root64 %zero %mask %origin %f0 %direction %f1
%root2 = OpConvertUToAccelerationStructureKHR %as %address2
OpRayQueryInitializeKHR %q %root2 %zero %mask %origin %f0 %direction %f1
%returned = OpFunctionCall %as %valuehelper %arrayval
OpRayQueryInitializeKHR %q %returned %zero %mask %origin %f0 %direction %f1
%loaded = OpFunctionCall %as %pointerhelper %scene
OpRayQueryInitializeKHR %q %loaded %zero %mask %origin %f0 %direction %f1
%element = OpFunctionCall %as %pointerhelper %selected
OpRayQueryInitializeKHR %q %element %zero %mask %origin %f0 %direction %f1
%condition = OpIEqual %bool %idx %zero
OpSelectionMerge %merge None
OpBranchConditional %condition %left %right
%left = OpLabel
OpBranch %merge
%right = OpLabel
OpBranch %merge
%merge = OpLabel
%joined = OpPhi %as %root64 %left %root2 %right
OpRayQueryInitializeKHR %q %joined %zero %mask %origin %f0 %direction %f1
OpReturn
OpFunctionEnd
)spv";
    std::vector<uint32_t> lowered;
    return lower_assembly("AS operand values", source, lib, &lowered) &&
           check_as_value_stores(
               lowered, {"regression_scalar", "regression_array", "regression_u64", "regression_uvec2", "regression_helper_value",
                         "regression_phi", "regression_helper_pointer", "regression_element_pointer"});
}

static bool check_as_runtime_nonuniform(const std::vector<uint32_t> &lib, bool grouped = false) {
    const char *source = R"spv(
OpCapability Shader
OpCapability RayQueryKHR
OpCapability ShaderNonUniformEXT
OpCapability RuntimeDescriptorArrayEXT
OpExtension "SPV_KHR_ray_query"
OpExtension "SPV_EXT_descriptor_indexing"
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %scenes %gid
OpExecutionMode %main LocalSize 1 1 1
OpName %root "regression_nonuniform"
OpDecorate %scenes DescriptorSet 1
OpDecorate %scenes Binding 13
OpDecorate %gid BuiltIn GlobalInvocationId
OpDecorate %selected NonUniformEXT
OpDecorate %root NonUniformEXT
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%v3u = OpTypeVector %uint 3
%gidptr = OpTypePointer Input %v3u
%gid = OpVariable %gidptr Input
%float = OpTypeFloat 32
%v3 = OpTypeVector %float 3
%zero = OpConstant %uint 0
%mask = OpConstant %uint 255
%f0 = OpConstant %float 0
%f1 = OpConstant %float 1
%origin = OpConstantComposite %v3 %f0 %f0 %f0
%direction = OpConstantComposite %v3 %f0 %f0 %f1
%as = OpTypeAccelerationStructureKHR
%asptr = OpTypePointer UniformConstant %as
%asarray = OpTypeRuntimeArray %as
%asarrayptr = OpTypePointer UniformConstant %asarray
%scenes = OpVariable %asarrayptr UniformConstant
%query = OpTypeRayQueryKHR
%queryptr = OpTypePointer Function %query
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%q = OpVariable %queryptr Function
%invocation = OpLoad %v3u %gid
%idx = OpCompositeExtract %uint %invocation 0
%selected = OpAccessChain %asptr %scenes %idx
%root = OpLoad %as %selected
OpRayQueryInitializeKHR %q %root %zero %mask %origin %f0 %direction %f1
OpReturn
OpFunctionEnd
)spv";
    std::string assembly(source);
    if (grouped) {
        const std::string decorations = "OpDecorate %selected NonUniformEXT\nOpDecorate %root NonUniformEXT";
        const std::string group =
            "%nonuniform_group = OpDecorationGroup\n"
            "OpDecorate %nonuniform_group NonUniformEXT\n"
            "OpGroupDecorate %nonuniform_group %selected %root";
        assembly.replace(assembly.find(decorations), decorations.size(), group);
    }
    std::vector<uint32_t> lowered;
    if (!lower_assembly(grouped ? "runtime/grouped nonuniform AS descriptor array" : "runtime/nonuniform AS descriptor array",
                        assembly.c_str(), lib, &lowered) ||
        !check_as_value_stores(lowered, {"regression_nonuniform"}))
        return false;
    std::unordered_set<uint32_t> nonuniform;
    std::unordered_map<uint32_t, uint32_t> value_pointer;
    uint32_t root = 0;
    bool capability = false;
    for (size_t i = 5; i < lowered.size(); i += SPV_LEN(lowered[i])) {
        uint32_t op = SPV_OP(lowered[i]), len = SPV_LEN(lowered[i]);
        if (op == 5 && spirv_string(lowered, i + 2, i + len) == "regression_nonuniform") root = lowered[i + 1];
        if (op == 17 && lowered[i + 1] == 5308) capability = true;
        if (op == 71 && len == 3 && lowered[i + 2] == 5300) nonuniform.insert(lowered[i + 1]);
        if (op == 61) value_pointer[lowered[i + 2]] = lowered[i + 3];
    }
    if (!capability || !root || !nonuniform.count(value_pointer[root])) {
        std::fprintf(stderr, "FAIL: nonuniform AS cell load lost storage-buffer capability/pointer decoration\n");
        return false;
    }
    return grouped || check_as_runtime_nonuniform(lib, true);
}

static bool check_workgroup_metadata(const std::vector<uint32_t> &lib) {
    const char *source = R"spv(
OpCapability Shader
OpCapability RayQueryKHR
OpExtension "SPV_KHR_ray_query"
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %scene
OpExecutionMode %main LocalSize 64 1 1
OpName %user_spec "regression_user_spec0"
OpDecorate %workgroup BuiltIn WorkgroupSize
OpDecorate %user_spec SpecId 0
OpDecorate %scene DescriptorSet 0
OpDecorate %scene Binding 0
%void = OpTypeVoid
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%v3u = OpTypeVector %uint 3
%float = OpTypeFloat 32
%v3 = OpTypeVector %float 3
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%sixty_four = OpConstant %uint 64
%mask = OpConstant %uint 255
%user_spec = OpSpecConstant %uint 7
%workgroup = OpConstantComposite %v3u %sixty_four %one %one
%f0 = OpConstant %float 0
%f1 = OpConstant %float 1
%origin = OpConstantComposite %v3 %f0 %f0 %f0
%direction = OpConstantComposite %v3 %f0 %f0 %f1
%as = OpTypeAccelerationStructureKHR
%asptr = OpTypePointer UniformConstant %as
%scene = OpVariable %asptr UniformConstant
%query = OpTypeRayQueryKHR
%queryptr = OpTypePointer Function %query
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%q = OpVariable %queryptr Function
%root = OpLoad %as %scene
OpRayQueryInitializeKHR %q %root %zero %mask %origin %f0 %direction %f1
%hit = OpRayQueryProceedKHR %bool %q
OpRayQueryConfirmIntersectionKHR %q
OpReturn
OpFunctionEnd
)spv";
    std::vector<uint32_t> lowered;
    if (!lower_assembly("application workgroup/specialization metadata", source, lib, &lowered)) return false;
    unsigned local_size_count = 0, workgroup_count = 0, spec_count = 0;
    uint32_t workgroup = 0, spec_target = 0, user_spec = 0;
    std::unordered_map<uint32_t, uint32_t> constants;
    std::unordered_map<uint32_t, std::vector<uint32_t>> composites;
    for (size_t i = 5; i < lowered.size(); i += SPV_LEN(lowered[i])) {
        uint32_t op = SPV_OP(lowered[i]), len = SPV_LEN(lowered[i]);
        if (op == 5 && spirv_string(lowered, i + 2, i + len) == "regression_user_spec0") user_spec = lowered[i + 1];
        if (op == 16 && len == 6 && lowered[i + 2] == 17) {
            if (lowered[i + 3] != 64 || lowered[i + 4] != 1 || lowered[i + 5] != 1) return false;
            ++local_size_count;
        }
        if (op == 71 && len == 4 && lowered[i + 2] == 11 && lowered[i + 3] == 25) {
            workgroup = lowered[i + 1];
            ++workgroup_count;
        }
        if (op == 71 && len == 4 && lowered[i + 2] == 1) {
            if (lowered[i + 3] != 0) return false;
            spec_target = lowered[i + 1];
            ++spec_count;
        }
        if (op == 43 && len == 4) constants[lowered[i + 2]] = lowered[i + 3];
        if (op == 44) composites[lowered[i + 2]] = {lowered.begin() + i + 3, lowered.begin() + i + len};
    }
    auto shape = composites.find(workgroup);
    bool correct_shape = shape != composites.end() && shape->second.size() == 3 && constants[shape->second[0]] == 64 &&
                         constants[shape->second[1]] == 1 && constants[shape->second[2]] == 1;
    if (local_size_count != 1 || workgroup_count != 1 || !correct_shape || spec_count != 1 || !user_spec ||
        spec_target != user_spec) {
        std::fprintf(stderr, "FAIL: traversal dummy entry-point metadata changed application workgroup/SpecId0 semantics\n");
        return false;
    }
    return true;
}

int
main(int argc, char **argv)
{
    if (argc != 4 && !(argc == 5 && !std::strcmp(argv[4], "--require-sbt-offset"))) {
        std::fprintf(stderr, "usage: %s <user.spv> <library.spv> <out.spv> [--require-sbt-offset]\n", argv[0]);
        return 2;
    }

   std::vector<uint32_t> user = read_spirv(argv[1]);
   std::vector<uint32_t> lib  = read_spirv(argv[2]);
   if (user.size() < 5 || user[0] != 0x07230203u) {
      std::fprintf(stderr, "%s is not SPIR-V\n", argv[1]);
      return 2;
   }
   if (lib.size() < 5 || lib[0] != 0x07230203u) {
      std::fprintf(stderr, "%s is not SPIR-V\n", argv[2]);
      return 2;
   }

   Counts before = classify(user);
   std::printf("input : OpRayQuery*=%d OpTypeRayQueryKHR=%d "
               "OpTypeAccelerationStructureKHR=%d OpFunctionCall=%d\n",
               before.op_ray_query, before.type_ray_query, before.type_as,
               before.op_function_call);

   int failures = 0;
   if (!check_function_query_image(lib)) failures++;
   if (!check_as_operand_values(lib)) failures++;
   if (!check_as_runtime_nonuniform(lib)) failures++;
   if (!check_workgroup_metadata(lib)) failures++;
   if (argc == 5 && before.op_sbt_offset != 2) {
       std::printf("FAIL: getter fixture must contain both SBT-offset selectors (found %d)\n", before.op_sbt_offset);
       failures++;
   }
   if (before.op_ray_query == 0) {
      std::printf("FAIL: input has no OpRayQuery* -- test is vacuous\n");
      failures++;
   }
   if (before.type_ray_query == 0) {
      std::printf("FAIL: input has no OpTypeRayQueryKHR\n");
      failures++;
   }
   if (before.type_as == 0) {
      std::printf("FAIL: input has no OpTypeAccelerationStructureKHR\n");
      failures++;
   }

   uint32_t *out = nullptr;
   size_t out_n = 0;
   char err[1024];
   int rc = vkrt_spirv_lower(user.data(), user.size(),
                            lib.data(), lib.size(),
                            &out, &out_n, err, sizeof(err));
   if (rc != 0) {
      std::printf("FAIL: vkrt_spirv_lower returned %d: %s\n", rc, err);
      return 1;
   }

   std::vector<uint32_t> lowered(out, out + out_n);
   Counts after = classify(lowered);
   std::printf("output: OpRayQuery*=%d OpTypeRayQueryKHR=%d "
               "OpTypeAccelerationStructureKHR=%d OpFunctionCall=%d "
               "(%zu -> %zu words)\n",
               after.op_ray_query, after.type_ray_query, after.type_as,
               after.op_function_call, user.size(), out_n);

   if (after.op_ray_query != 0) {
      std::printf("FAIL: %d OpRayQuery* left in the output\n",
                  after.op_ray_query);
      failures++;
   }
   if (after.type_ray_query != 0) {
      std::printf("FAIL: %d OpTypeRayQueryKHR left in the output\n",
                  after.type_ray_query);
      failures++;
   }
   if (after.op_trace_ray != 0) {
      std::printf("FAIL: ray-tracing-pipeline ops in the output\n");
      failures++;
   }
   if (after.type_as != 0) {
      std::printf("FAIL: %d OpTypeAccelerationStructureKHR left in the output\n",
                  after.type_as);
      failures++;
   }
   if (after.rt_requirements != 0) {
       std::printf("FAIL: native RT capability/extension declarations remain in the output\n");
       failures++;
   }
   if (after.op_function_call == 0) {
      std::printf("FAIL: no OpFunctionCall in the output\n");
      failures++;
   }

   if (!calls_function_named(lowered, "sw_ray_query_proceed")) {
      std::printf("FAIL: output does not call sw_ray_query_proceed\n");
      failures++;
   }
   if (!calls_function_named(lowered, "sw_ray_query_initialize")) {
      std::printf("FAIL: output does not call sw_ray_query_initialize\n");
      failures++;
   }
   if (!calls_function_named(lowered, "sw_ray_query_confirm")) {
      std::printf("FAIL: output does not call sw_ray_query_confirm\n");
      failures++;
   }
   if (before.op_sbt_offset &&
       count_calls_function_named(lowered, "sw_get_intersection_instance_sbt_offset") < before.op_sbt_offset) {
       std::printf("FAIL: SBT-offset getters were not lowered to their helper\n");
       failures++;
   }
   if (before.op_sbt_offset && !check_signed_sbt_variant(user, lib)) failures++;

   write_spirv(argv[3], out, out_n);
   vkrt_spirv_lower_free(out);

   if (failures) {
      std::printf("\n%d FAILURE(S)\n", failures);
      return 1;
   }
   std::printf("\nPASS\n");
   return 0;
}
