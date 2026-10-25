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
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
