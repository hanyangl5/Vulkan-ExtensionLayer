/* SPDX-License-Identifier: MIT
 *
 * test_spirv_lower.cpp - drives vkrt_spirv_lower over a real ray-query
 * shader and asserts the rewrite actually happened.
 *
 *   usage: test_spirv_lower <user.spv> <library.spv> <out.spv>
 *
 * Checks:
 *   1. the input really had OpRayQuery* / OpTypeRayQueryKHR /
 *      OpTypeAccelerationStructureKHR (otherwise the test is vacuous)
 *   2. the output has none of those
 *   3. the output has OpFunctionCall to the sw_* traversal
 *   4. the output still validates as SPIR-V (when spirv-val is around)
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
      i += len;
   }
   return c;
}

/* Does the module call a function whose OpName starts with `prefix`? */
static bool
calls_function_named(const std::vector<uint32_t> &w, const char *prefix)
{
   /* Collect ids of OpFunctionCall targets. */
   std::vector<uint32_t> targets;
   for (size_t i = 5; i < w.size();) {
      uint16_t op = SPV_OP(w[i]);
      uint16_t len = SPV_LEN(w[i]);
      if (len == 0 || i + len > w.size())
         break;
      if (op == 57 && len >= 4) /* OpFunctionCall %rty %res %fn ... */
         targets.push_back(w[i + 3]);
      i += len;
   }

   /* Map OpName id -> name. */
   size_t plen = std::strlen(prefix);
   for (size_t i = 5; i < w.size();) {
      uint16_t op = SPV_OP(w[i]);
      uint16_t len = SPV_LEN(w[i]);
      if (len == 0 || i + len > w.size())
         break;
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
            if (t == id && s.compare(0, plen, prefix) == 0)
               return true;
         }
      }
      i += len;
   }
   return false;
}

int
main(int argc, char **argv)
{
   if (argc != 4) {
      std::fprintf(stderr,
                   "usage: %s <user.spv> <library.spv> <out.spv>\n", argv[0]);
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

   write_spirv(argv[3], out, out_n);
   vkrt_spirv_lower_free(out);

   if (failures) {
      std::printf("\n%d FAILURE(S)\n", failures);
      return 1;
   }
   std::printf("\nPASS\n");
   return 0;
}
