/* SPDX-License-Identifier: MIT */
#include "vkrt_spirv_ids.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <cstdio>

static unsigned failures;
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

/* Numeric IDs intentionally collide with constants, bindings, offsets,
 * local sizes, switch literals, and the encoded string "X" (word 88).
 * Duplicate uint types model the lowerer's intermediate module. */
static const char *source = R"(
OpCapability Shader
OpCapability Int64
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %100 "X" %80
OpExecutionMode %100 LocalSize 24 1 1
OpName %88 "X"
OpDecorate %30 Block
OpMemberDecorate %30 0 Offset 24
OpDecorate %80 DescriptorSet 0
OpDecorate %80 Binding 24
%1 = OpTypeVoid
%2 = OpTypeFunction %1
%23 = OpTypeInt 32 0
%24 = OpTypeInt 32 0
%25 = OpConstant %24 24
%26 = OpTypeInt 64 0
%27 = OpConstant %26 103079215128
%28 = OpConstant %23 0
%30 = OpTypeStruct %24
%31 = OpTypePointer StorageBuffer %30
%32 = OpTypePointer StorageBuffer %24
%80 = OpVariable %31 StorageBuffer
%88 = OpConstant %23 2
%89 = OpConstant %23 72
%90 = OpConstant %23 2
%91 = OpConstant %23 72
%100 = OpFunction %1 None %2
%101 = OpLabel
OpMemoryBarrier %88 %89
OpSelectionMerge %104 None
OpSwitch %25 %104 24 %102 88 %103
%102 = OpLabel
OpBranch %104
%103 = OpLabel
OpBranch %104
%104 = OpLabel
%105 = OpAccessChain %32 %80 %28
OpStore %105 %25
OpReturn
OpFunctionEnd
)";

int main()
{
   spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
   tools.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
      std::fprintf(stderr, "%s\n", message);
   });
   std::vector<uint32_t> words;
   if (!tools.Assemble(source, &words, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS)) return 1;
   std::string error;
   REQUIRE(vkrt_spv::remap_id_uses(&words, {{88, 90}, {89, 91}}, &error));
   REQUIRE(vkrt_spv::dedupe_types(&words, &error));
   if (!error.empty()) std::fprintf(stderr, "%s\n", error.c_str());
   REQUIRE(tools.Validate(words));
   unsigned checked = 0;
   for (size_t i = 5; i < words.size();) {
      const uint32_t *w = words.data() + i;
      switch (static_cast<spv::Op>(w[0] & 0xffff)) {
      case spv::OpTypeInt:
         REQUIRE(w[1] != 24); break;
      case spv::OpConstant:
         if (w[2] == 25) { REQUIRE(w[1] == 23 && w[3] == 24); ++checked; }
         if (w[2] == 27) { REQUIRE(w[3] == 24 && w[4] == 24); ++checked; }
         if (w[2] == 88) ++checked; // result-ID definition is not renamed
         break;
      case spv::OpTypeStruct:
         REQUIRE(w[2] == 23); ++checked; break;
      case spv::OpName:
         REQUIRE(w[1] == 90 && w[2] == 88); ++checked; break;
      case spv::OpEntryPoint:
         REQUIRE(w[3] == 88 && w[4] == 80); ++checked; break;
      case spv::OpExecutionMode:
         REQUIRE(w[3] == 24); ++checked; break;
      case spv::OpMemberDecorate:
         REQUIRE(w[4] == 24); ++checked; break;
      case spv::OpDecorate:
         if (w[2] == static_cast<uint32_t>(spv::DecorationBinding)) { REQUIRE(w[3] == 24); ++checked; }
         break;
      case spv::OpSwitch:
         REQUIRE(w[3] == 24 && w[5] == 88 && w[4] == 102 && w[6] == 103); ++checked; break;
      case spv::OpMemoryBarrier:
         REQUIRE(w[1] == 90 && w[2] == 91); ++checked; break;
      default: break;
      }
      i += w[0] >> 16;
   }
   REQUIRE(checked == 11);
   // A parser failure must not leave edits applied to an incomplete module.
   words.push_back(0);
   auto original = words;
   REQUIRE(!vkrt_spv::remap_id_uses(&words, {{90, 88}}, &error));
   REQUIRE(words == original);
   std::vector<uint32_t> truncated(4, 0);
   REQUIRE(!vkrt_spv::dedupe_types(&truncated, &error));
   REQUIRE(truncated == std::vector<uint32_t>(4, 0));
   std::printf("SPIR-V operand remapping: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
