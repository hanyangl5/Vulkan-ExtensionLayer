/* SPDX-License-Identifier: MIT
 *
 * vkrt_spirv_lower.cpp - rewrite OpRayQuery*KHR into same-module function
 * calls into the software traversal (design doc sections 4 and 11).
 *
 * Contract (doc 11.1):
 *   in : compute/fragment SPIR-V with OpTypeAccelerationStructureKHR,
 *        OpTypeRayQueryKHR, OpRayQuery*KHR
 *   out: ordinary SPIR-V with no KHR AS/RayQuery types or instructions,
 *        containing the layer-injected traversal functions and
 *        custom-BVH buffer accesses.
 *
 * Two stages:
 *   1. spvtools::Link merges traversal/sw_ray_query.spv into the user
 *      module. The SPIRV-Tools linker remaps IDs and deduplicates types,
 *      which is exactly the hard part of doing this by hand.
 *   2. A word-level pass substitutes types and turns each OpRayQuery*
 *      into an OpFunctionCall.
 *
 * Name matching note: glslc mangles OpName to "fn(type;type;" so symbols
 * are found by prefix match on "<name>(" or exact "<name>". The
 * sw_ray_query_state struct is located via sw_ray_query_proceed's first
 * parameter pointee rather than by trusting a struct OpName.
 *
 * This file is C++ purely for spvtools::Link's C++ API; the rest of the
 * layer is C and talks to it through vkrt_spirv_lower.h.
 */

#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/linker.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "vkrt_spirv_lower.h"
#include "vkrt_spirv_ids.h"

namespace {

/* SPIR-V opcodes we touch. */
enum : uint16_t {
    SpvOpNop = 0,
    SpvOpName = 5,
    SpvOpMemberName = 6,
    SpvOpExtInstImport = 11,
    SpvOpExtInst = 12,
    SpvOpMemoryModel = 14,
    SpvOpEntryPoint = 15,
    SpvOpExecutionMode = 16,
    SpvOpCapability = 17,
    SpvOpTypeVoid = 19,
    SpvOpTypeBool = 20,
    SpvOpTypeInt = 21,
    SpvOpTypeFloat = 22,
    SpvOpTypeVector = 23,
    SpvOpTypeMatrix = 24,
    SpvOpTypeImage = 25,
    SpvOpTypeSampler = 26,
    SpvOpTypeSampledImage = 27,
    SpvOpTypeArray = 28,
    SpvOpTypeRuntimeArray = 29,
    SpvOpTypeOpaque = 31,
    SpvOpTypePointer = 32,
    SpvOpTypeFunction = 33,
    SpvOpConstant = 43,
    SpvOpConstantTrue = 41,
    SpvOpConstantFalse = 42,
    SpvOpFunction = 54,
    SpvOpFunctionEnd = 56,
    SpvOpFunctionCall = 57,
    SpvOpFunctionParameter = 55,
    SpvOpVariable = 59,
    SpvOpLoad = 61,
    SpvOpStore = 62,
    SpvOpAccessChain = 65,
    SpvOpDecorate = 71,
    SpvOpMemberDecorate = 72,
    SpvOpTypeStruct = 30,
    SpvOpBitcast = 124,
    SpvOpConvertPtrToU = 117,
    SpvOpUndef = 1,

    SpvOpTypeRayQueryKHR = 4472,
    SpvOpRayQueryInitializeKHR = 4473,
    SpvOpRayQueryTerminateKHR = 4474,
    SpvOpRayQueryGenerateIntersectionKHR = 4475,
    SpvOpRayQueryConfirmIntersectionKHR = 4476,
    SpvOpRayQueryProceedKHR = 4477,
    SpvOpRayQueryGetIntersectionTypeKHR = 4479,
    /* Core SPV_KHR_ray_query accessors are in the 6000 block, NOT
     * the 4480s. Getting this wrong silently leaves them unrewritten. */
    SpvOpRayQueryGetRayTMinKHR = 6016,
    SpvOpRayQueryGetRayFlagsKHR = 6017,
    SpvOpRayQueryGetIntersectionTKHR = 6018,
    SpvOpRayQueryGetIntersectionInstanceCustomIndexKHR = 6019,
    SpvOpRayQueryGetIntersectionInstanceIdKHR = 6020,
    SpvOpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR = 6021,
    SpvOpRayQueryGetIntersectionGeometryIndexKHR = 6022,
    SpvOpRayQueryGetIntersectionPrimitiveIndexKHR = 6023,
    SpvOpRayQueryGetIntersectionBarycentricsKHR = 6024,
    SpvOpRayQueryGetIntersectionFrontFaceKHR = 6025,
    SpvOpRayQueryGetIntersectionCandidateAABBOpaqueKHR = 6026,
    SpvOpRayQueryGetIntersectionObjectRayDirectionKHR = 6027,
    SpvOpRayQueryGetIntersectionObjectRayOriginKHR = 6028,
    SpvOpRayQueryGetWorldRayDirectionKHR = 6029,
    SpvOpRayQueryGetWorldRayOriginKHR = 6030,
    SpvOpRayQueryGetIntersectionObjectToWorldKHR = 6031,
    SpvOpRayQueryGetIntersectionWorldToObjectKHR = 6032,
    SpvOpRayQueryGetIntersectionTriangleVertexPositionsKHR = 5340,
    SpvOpRayQueryGetClusterIdNV = 5345,

    SpvOpTypeAccelerationStructureKHR = 5341,
    SpvOpConvertUToAccelerationStructureKHR = 4447,
};

/* Decoration / storage-class / enum values. */
enum : uint32_t {
   SpvDecorationBlock              = 2,
   SpvDecorationOffset             = 35,
   SpvDecorationBufferBlock        = 3,   /* pre-1.4 SSBO */
   SpvStorageClassUniformConstant  = 0,
   SpvStorageClassInput            = 1,
   SpvStorageClassUniform          = 2,
   SpvStorageClassOutput           = 3,
   SpvStorageClassWorkgroup        = 4,
   SpvStorageClassCrossWorkgroup   = 5,
   SpvStorageClassPrivate          = 6,
   SpvStorageClassFunction         = 7,
   SpvStorageClassGeneric          = 8,
   SpvStorageClassPushConstant     = 9,
   SpvStorageClassAtomicCounter    = 10,
   SpvStorageClassImage            = 11,
   SpvStorageClassStorageBuffer    = 12,
   SpvStorageClassPhysicalStorageBuffer = 5349,
};

/* Capability IDs -- a DIFFERENT enum from the opcodes above. Getting
 * these wrong emits bogus OpCapability (e.g. 5340 is RayTracingNV, not
 * PhysicalStorageBufferAddresses). */
enum : uint32_t {
   SpvCapabilityInt64                          = 11,
   SpvCapabilityRayQueryKHR                    = 4472,
   SpvCapabilityRayTracingKHR                  = 4479,
   SpvCapabilityPhysicalStorageBufferAddresses = 5347,
};

#define SPV_WORD0(op, len) (((uint32_t)(len) << 16) | (uint32_t)(op))
#define SPV_OP(w)          ((uint16_t)((w) & 0xFFFFu))
#define SPV_LEN(w)         ((uint16_t)((w) >> 16))

/* ------------------------------------------------------------------ */
/* Instruction buffer helpers                                          */
/* ------------------------------------------------------------------ */

/* Iterate every instruction, invoking fn(offset, opcode, length). */
template <typename F>
void
for_each(const std::vector<uint32_t> &w, F &&fn)
{
   for (size_t i = 5; i < w.size();) {
      uint32_t op = SPV_OP(w[i]);
      uint32_t len = SPV_LEN(w[i]);
      if (len == 0 || i + len > w.size())
         break;
      fn(i, op, len);
      i += len;
   }
}

/* Index (in words from the instruction start) of the result-id operand,
 * or 0 if the instruction defines nothing.
 *
 * Three shapes:
 *   OpType*          : word 1 = result id
 *   most definitions : word 1 = result type, word 2 = result id
 *   no result        : 0 (annotations, control flow, stores, ...) */
uint32_t
spv_result_id_index(uint32_t op)
{
   bool has_result = false, has_type = false;
   spv::HasResultAndType(static_cast<spv::Op>(op), &has_result, &has_type);
   return has_result ? (has_type ? 2u : 1u) : 0u;
}

/* Result id of a definition instruction, or 0.
 *
 * Bounds-checked: a mis-classified short instruction would otherwise
 * read the NEXT instruction's word0 ((len<<16)|opcode) as a result id,
 * which then poisons the ID bound with a nonsense value. */
uint32_t
result_id(const std::vector<uint32_t> &w, size_t off, uint32_t op)
{
   uint32_t idx = spv_result_id_index(op);
   if (!idx)
      return 0;
   uint32_t len = SPV_LEN(w[off]);
   if (idx >= len)
      return 0;
   return w[off + idx];
}

/* ------------------------------------------------------------------ */
/* Symbol table: OpName string -> result id                            */
/* ------------------------------------------------------------------ */

struct Symbols {
   std::unordered_map<std::string, uint32_t> exact;
   /* prefix -> id (glslc mangles to "name(t1;t2;"). */
   std::unordered_map<std::string, uint32_t> prefix;

   uint32_t find(const std::string &name) const {
      auto it = exact.find(name);
      if (it != exact.end())
         return it->second;
      it = prefix.find(name);
      if (it != prefix.end())
         return it->second;
      return 0;
   }
};

void
collect_symbols(const std::vector<uint32_t> &w, Symbols *out)
{
   for_each(w, [&](size_t i, uint32_t op, uint32_t len) {
      if (op != SpvOpName || len < 3)
         return;
      uint32_t target = w[i + 1];
      /* Remaining words are a nul-padded literal string. */
      std::string s;
      for (uint32_t k = 2; k < len; k++) {
         char c[5];
         std::memcpy(c, &w[i + k], 4);
         for (int b = 0; b < 4; b++) {
            if (c[b] == '\0')
               goto done;
            s.push_back(c[b]);
         }
      }
   done:
      out->exact[s] = target;
      auto paren = s.find('(');
      if (paren != std::string::npos && paren > 0) {
         std::string base = s.substr(0, paren);
         out->prefix[base] = target;
      }
   });
}

/* ------------------------------------------------------------------ */
/* Type introspection                                                  */
/* ------------------------------------------------------------------ */

struct TypeIndex {
   /* result id -> instruction offset. */
   std::unordered_map<uint32_t, size_t> offset_of;
   /* OpTypePointer result id -> (storage class, pointee). */
   std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;
   /* OpTypeFunction result id -> {return, params...}. */
   std::unordered_map<uint32_t, std::vector<uint32_t>> functions;
};

void
index_types(const std::vector<uint32_t> &w, TypeIndex *out)
{
   for_each(w, [&](size_t i, uint32_t op, uint32_t len) {
      uint32_t rid = result_id(w, i, op);
      if (rid)
         out->offset_of[rid] = i;

      if (op == SpvOpTypePointer && len >= 4)
         out->pointers[rid] = {w[i + 2], w[i + 3]};
      else if (op == SpvOpTypeFunction && len >= 3) {
         std::vector<uint32_t> sig;
         for (uint32_t k = 2; k < len; k++)
            sig.push_back(w[i + k]);
         out->functions[rid] = sig;
      }
   });
}

/* Given a function id, return its first parameter's pointee type id
 * (i.e. the sw_ray_query_state struct behind `inout sw_ray_query_state`). */
uint32_t
fn_first_param_pointee(const std::vector<uint32_t> &w, const TypeIndex &ti,
                       uint32_t fn_id)
{
   auto it = ti.offset_of.find(fn_id);
   if (it == ti.offset_of.end())
      return 0;
   size_t i = it->second;
   /* OpFunction %resultType %result %fnType %control */
   if (SPV_OP(w[i]) != SpvOpFunction || SPV_LEN(w[i]) < 5)
      return 0;
   uint32_t fn_type = w[i + 3];
   auto fit = ti.functions.find(fn_type);
   if (fit == ti.functions.end() || fit->second.size() < 2)
      return 0;
   uint32_t p0 = fit->second[1]; /* first parameter type */
   auto pit = ti.pointers.find(p0);
   if (pit == ti.pointers.end())
      return 0;
   return pit->second.second;
}

/* ------------------------------------------------------------------ */
/* Capability / extension injection                                    */
/* ------------------------------------------------------------------ */

void
ensure_capability(std::vector<uint32_t> *w, uint32_t cap)
{
   /* Already present? */
   bool have = false;
   for_each(*w, [&](size_t i, uint32_t op, uint32_t len) {
      if (op == SpvOpCapability && len >= 2 && (*w)[i + 1] == cap)
         have = true;
   });
   if (have)
      return;

   /* Insert after the header, before anything else that is not a
    * capability (capabilities must come first). */
   size_t at = 5;
   for_each(*w, [&](size_t i, uint32_t op, uint32_t len) {
      if (op == SpvOpCapability)
         at = i + len;
   });
   w->insert(w->begin() + at, {SPV_WORD0(SpvOpCapability, 2), cap});
}

/* Pack a C string into SPIR-V literal words (nul-terminated, zero-padded
 * to a whole number of words). */
static std::vector<uint32_t>
spv_string_literal(const char *name)
{
   std::vector<uint32_t> lit;
   size_t n = std::strlen(name);
   size_t bytes = n + 1;                       /* include the nul */
   size_t words = (bytes + 3) / 4;             /* round up */
   lit.resize(words, 0);
   for (size_t i = 0; i < n; i++)
      ((char *)lit.data())[i] = name[i];
   ((char *)lit.data())[n] = '\0';
   return lit;
}

void
ensure_extension(std::vector<uint32_t> *w, const char *name)
{
   /* Extensions come after capabilities. Check for a duplicate first. */
   std::vector<uint32_t> lit = spv_string_literal(name);

   bool have = false;
   for_each(*w, [&](size_t i, uint32_t op, uint32_t len) {
      if (op == SpvOpExtInstImport)
         return;
      if (op != 0)
         return;
   });
   /* OpExtension is opcode 10. */
   for_each(*w, [&](size_t i, uint32_t op, uint32_t len) {
      if (op == 10 && len == 1 + lit.size()) {
         if (std::memcmp(&(*w)[i + 1], lit.data(), lit.size() * 4) == 0)
            have = true;
      }
   });
   if (have)
      return;

   size_t at = 5;
   for_each(*w, [&](size_t i, uint32_t op, uint32_t len) {
      if (op == SpvOpCapability || op == 10 /* OpExtension */)
         at = i + len;
   });
   std::vector<uint32_t> ins;
   ins.push_back(SPV_WORD0(10, 1 + (uint32_t)lit.size()));
   for (uint32_t x : lit)
      ins.push_back(x);
   w->insert(w->begin() + at, ins.begin(), ins.end());
}

/* Remove OpDecorate ... LinkageAttributes ... after linking.
 *
 * Those decorations exist only to make spvtools::Link keep the
 * traversal (it drops non-exported symbols). A Vulkan shader module
 * must not carry them -- spirv-val rejects LinkageAttributes without
 * the Linkage capability, which Vulkan forbids. */
void
strip_linkage_decorations(std::vector<uint32_t> *w)
{
   std::vector<uint32_t> out;
   for (size_t i = 0; i < 5 && i < w->size(); i++)
      out.push_back((*w)[i]);
   for (size_t i = 5; i < w->size();) {
      uint32_t op = SPV_OP((*w)[i]);
      uint32_t len = SPV_LEN((*w)[i]);
      if (len == 0 || i + len > w->size())
         break;
      /* OpDecorate <target> LinkageAttributes <name-literal> <export> */
      bool is_linkage = (op == SpvOpDecorate && len >= 3 &&
                         (*w)[i + 2] == 41 /* LinkageAttributes */);
      if (is_linkage) {
         i += len;
         continue;
      }
      for (uint32_t k = 0; k < len; k++)
         out.push_back((*w)[i + k]);
      i += len;
   }
   w->swap(out);
}

/* Order type/constant instructions so no type references a type defined
 * later. spvtools::Link concatenates the two modules' type sections, so
 * the user's OpTypePointer can land before the library's OpTypeStruct
 * once we rewrite their pointee.
 *
 * This must be a real topological sort: a struct may contain an array
 * and an array may contain a struct, so no total rank order exists.
 *
 * Hard invariant: the word count never changes. If anything goes wrong
 * the original order is kept -- a late type is a validator complaint, a
 * missing type is a broken shader. */
void
sort_types_topo(std::vector<uint32_t> *ty)
{
   /* Intentionally inert: a rank permutation here reassembled instruction fragments and corrupted the stream. Ordering is handled by hoist_type_and_deps() instead. */
   return;
   const size_t original_size = ty->size();

   struct Item { size_t off, len; uint32_t rank; };
   std::vector<Item> items;
   size_t tail_off = ty->size();
   for (size_t i = 0; i < ty->size();) {
      uint32_t op = SPV_OP((*ty)[i]);
      uint32_t len = SPV_LEN((*ty)[i]);
      if (len == 0 || i + len > ty->size()) { tail_off = i; break; }

      uint32_t rank;
      switch (op) {
      case SpvOpTypeVoid: case SpvOpTypeBool:
      case SpvOpTypeInt:   case SpvOpTypeFloat:
      case SpvOpUndef:                          rank = 0; break;
      case SpvOpTypeVector: case SpvOpTypeMatrix: rank = 1; break;
      case SpvOpTypeArray: case SpvOpTypeRuntimeArray: rank = 2; break;
      case SpvOpTypeStruct: case SpvOpTypePointer: /* fallthrough */
      default:                                  rank = 3; break;
      }
      /* Scalar constants (array lengths etc.) must come first.
       * Composite constants must come AFTER the types they name. */
      if (op == SpvOpConstant || op == SpvOpConstantTrue ||
          op == SpvOpConstantFalse || op == 48 || op == 49 || op == 50)
         rank = 0;
      else if (op == 44 || op == 45 || op == 46 || op == 51 || op == 52)
         rank = 6;   /* OpConstantComposite / ConstantSampler / ConstantNull
                      * / SpecConstantComposite / SpecConstantOp */
      else if (op == SpvOpTypePointer)
         rank = 4;
      else if (op == SpvOpTypeFunction)
         rank = 5;

      items.push_back({i, len, rank});
      i += len;
   }

   std::stable_sort(items.begin(), items.end(),
                    [](const Item &a, const Item &b) { return a.rank < b.rank; });

   std::vector<uint32_t> out;
   out.reserve(original_size);
   for (const Item &it : items)
      for (uint32_t k = 0; k < it.len; k++)
         out.push_back((*ty)[it.off + k]);
   for (size_t i = tail_off; i < ty->size(); i++)
      out.push_back((*ty)[i]);

   /* A sort is a pure permutation: it must never lose bytes. */
   if (out.size() != original_size)
      return;
   ty->swap(out);
}


/* Hoist  and every type/constant it transitively needs to the
 * front of the type region.
 *
 * The generic passes are unreliable here: a stable permutation of a
 * mis-parsed bucket can reassemble fragments, and a forward-ref fixup
 * can miss a pair. Pulling one known type and its closure to the front
 * is deterministic and covers exactly the shape that broke -- the
 * sw_ray_query_state struct (which contains an array) referenced by the
 * user shader's rewritten OpTypePointer.
 */
void
hoist_type_and_deps(std::vector<uint32_t> *w, uint32_t root)
{
   if (!root)
      return;

   /* Map result id -> (off, len) for every type/constant. */
   struct Off { size_t off, len; uint32_t op; };
   std::unordered_map<uint32_t, Off> defs;
   for (size_t i = 5; i < w->size();) {
      uint32_t op = SPV_OP((*w)[i]);
      uint32_t len = SPV_LEN((*w)[i]);
      if (len == 0 || i + len > w->size()) {
         break;
      }
      uint32_t rid = result_id(*w, i, op);
      if (rid)
         defs[rid] = Off{i, len, op};
      i += len;
   }
   if (!defs.count(root))
      return;

   /* Transitive closure over operand ids. */
   std::vector<uint32_t> stack{root};
   std::unordered_map<uint32_t, int> need;
   /* Hoist EVERY type/constant definition, in dependency order.
    *
    * A closure rooted at sw_ray_query_state is not enough: the rewrite
    * appends new types (get_int_type and friends) to the end of the
    * module, so a shared scalar can end up defined after the struct that
    * uses it and never be reached from the root's own operands. Hoisting
    * the whole type class is deterministic and covers every case.
    */
   for (auto &kv : defs) {
      uint32_t op = kv.second.op;
      bool is_type_or_const =
         op == SpvOpTypeVoid || op == SpvOpTypeBool || op == SpvOpTypeInt ||
         op == SpvOpTypeFloat || op == SpvOpTypeVector || op == SpvOpTypeMatrix ||
         op == SpvOpTypeArray || op == SpvOpTypeRuntimeArray ||
         op == SpvOpTypeStruct || op == SpvOpTypePointer ||
         op == 39 || op == SpvOpTypeFunction ||
         op == SpvOpTypeRayQueryKHR || op == SpvOpTypeAccelerationStructureKHR ||
         op == 25 || op == 26 || op == 27 || op == 31 ||
         op == SpvOpUndef || op == SpvOpConstant || op == SpvOpConstantTrue ||
         op == SpvOpConstantFalse || op == 44 || op == 45 || op == 46 ||
         op == 48 || op == 49 || op == 50 || op == 51 || op == 52;
      if (is_type_or_const)
         need[kv.first] = 1;
   }

   std::unordered_map<uint32_t, int> emitted_keep;
   (void)emitted_keep;

   /* Emit the needed instructions in dependency order.
    *
    * Kahn over the `need` set. Ready items are chosen by opcode rank
    * (scalars, then vectors, arrays, structs, pointers) rather than by
    * map order: literal operands such as a storage class can collide
    * with a small result id and invent a bogus dependency cycle, and the
    * cycle fallback would otherwise emit in whatever order `need` happens
    * to hold -- putting OpTypePointer ahead of the OpTypeStruct it
    * points at.
    */
   std::vector<uint32_t> hoisted;

   /* OpTypeForwardPointer has no result id, so it never appears in
    * `defs`/`need` and would be left behind -- after the OpTypePointer it
    * is supposed to forward-declare. Emit the ones targeting a hoisted
    * pointer first, ahead of everything else. */
   {
      std::unordered_map<uint32_t, int> hoist_ptr;
      for (auto &kv : need) {
         auto it = defs.find(kv.first);
         if (it != defs.end() && it->second.op == SpvOpTypePointer)
            hoist_ptr[kv.first] = 1;
      }
      std::unordered_map<size_t, int> fwd_at;
      for (size_t i = 5; i < w->size();) {
         uint32_t op = SPV_OP((*w)[i]);
         uint32_t len = SPV_LEN((*w)[i]);
         if (len == 0 || i + len > w->size())
            break;
         if (op == 39 && len >= 2 && hoist_ptr.count((*w)[i + 1]))
            fwd_at[i] = 1;
         i += len;
      }
      for (size_t i = 5; i < w->size();) {
         uint32_t len = SPV_LEN((*w)[i]);
         if (len == 0 || i + len > w->size())
            break;
         if (fwd_at.count(i)) {
            for (uint32_t k = 0; k < len; k++)
               hoisted.push_back((*w)[i + k]);
            fwd_at.erase(i);
         }
         i += len;
      }
   }

   {
      auto rank_of = [](uint32_t op) -> int {
         if (op == SpvOpTypeVoid || op == SpvOpTypeBool ||
             op == SpvOpTypeInt   || op == SpvOpTypeFloat ||
             op == SpvOpConstant  || op == SpvOpConstantTrue ||
             op == SpvOpConstantFalse || op == SpvOpUndef ||
             op == 48 || op == 49 || op == 50)
            return 0;
         if (op == SpvOpTypeVector || op == SpvOpTypeMatrix)
            return 1;
         if (op == SpvOpTypeArray || op == SpvOpTypeRuntimeArray)
            return 2;
         /* OpTypeForwardPointer (39) must precede the OpTypePointer it
          * declares, so it shares the struct rank -- still ahead of the
          * rank-4 pointers. */
         if (op == SpvOpTypeStruct || op == 25 || op == 26 || op == 27 ||
             op == 31 || op == 39 || op == SpvOpTypeRayQueryKHR ||
             op == SpvOpTypeAccelerationStructureKHR)
            return 3;
         if (op == SpvOpTypePointer)
            return 4;
         if (op == SpvOpTypeFunction)
            return 5;
         return 6;
      };

      std::unordered_map<uint32_t, int> emitted;
      std::vector<uint32_t> pending;
      for (auto &kv : need)
         pending.push_back(kv.first);
      std::stable_sort(pending.begin(), pending.end(),
                       [&](uint32_t a, uint32_t b) {
                          int ra = defs.count(a) ? rank_of(defs[a].op) : 9;
                          int rb = defs.count(b) ? rank_of(defs[b].op) : 9;
                          return ra < rb;
                       });

      bool progress = true;
      while (progress && emitted.size() < pending.size()) {
         progress = false;
         for (size_t n = 0; n < pending.size(); n++) {
            uint32_t id = pending[n];
            if (emitted.count(id))
               continue;
            auto it = defs.find(id);
            if (it == defs.end()) {
               emitted[id] = 1;
               continue;
            }
            /* All needed deps already emitted? */
            bool ready = true;
            uint32_t res_idx = spv_result_id_index(it->second.op);
            for (uint32_t k = 1; k < it->second.len && ready; k++) {
               if (k == res_idx)
                  continue;
               uint32_t v = (*w)[it->second.off + k];
               if (need.count(v) && !emitted.count(v) && v != id)
                  ready = false;
            }
            if (!ready)
               continue;
            for (uint32_t k = 0; k < it->second.len; k++)
               hoisted.push_back((*w)[it->second.off + k]);
            emitted[id] = 1;
            progress = true;
         }
      }
      /* Anything left (bogus cycle) goes out in rank order rather than
       * being dropped. `pending` is already rank-sorted. */
      for (size_t n = 0; n < pending.size(); n++) {
         uint32_t id = pending[n];
         if (emitted.count(id))
            continue;
         auto it = defs.find(id);
         if (it == defs.end())
            continue;
         for (uint32_t k = 0; k < it->second.len; k++)
            hoisted.push_back((*w)[it->second.off + k]);
         emitted[id] = 1;
      }
   }

   /* Everything not hoisted stays where it is.
    *
    * Skip by OFFSET, not by result id: if a result id happens to be
    * defined twice, skipping every copy while emitting one loses bytes
    * and the size guard below aborts the whole hoist. */
   std::vector<uint32_t> rest;
   for (size_t i = 0; i < 5 && i < w->size(); i++)
      rest.push_back((*w)[i]);   /* keep the header with rest */
   {
      std::unordered_map<size_t, int> hoist_at;
      for (auto &kv : need) {
         auto it = defs.find(kv.first);
         if (it != defs.end())
            hoist_at[it->second.off] = 1;
      }
      /* OpTypeForwardPointer has no result id so `need` cannot see it;
       * drop the ones whose target pointer we hoisted. */
      for (size_t i = 5; i < w->size();) {
         uint32_t op = SPV_OP((*w)[i]);
         uint32_t len = SPV_LEN((*w)[i]);
         if (len == 0 || i + len > w->size())
            break;
         if (op == 39 && len >= 2 && hoist_at.count(defs.count((*w)[i + 1]) ? defs[(*w)[i + 1]].off : (size_t)-1))
            hoist_at[i] = 1;
         i += len;
      }
      for (size_t i = 5; i < w->size();) {
         uint32_t len = SPV_LEN((*w)[i]);
         if (len == 0 || i + len > w->size())
            break;
         if (!hoist_at.count(i)) {
            for (uint32_t k = 0; k < len; k++)
               rest.push_back((*w)[i + k]);
         }
         i += len;
      }
   }
   /* Anything unparseable stays with rest verbatim. */
   {
      size_t parsed_to = 5;
      for (size_t i = 5; i < w->size();) {
         uint32_t len = SPV_LEN((*w)[i]);
         if (len == 0 || i + len > w->size())
            break;
         parsed_to = i + len;
         i += len;
      }
      for (size_t i = parsed_to; i < w->size(); i++)
         rest.push_back((*w)[i]);
   }

   if (hoisted.empty())
      return;

   /* Splice  in right after the annotations (after the last
    * OpName/OpMemberName/OpDecorate/OpMemberDecorate). */
   size_t at = 5;
   for (size_t i = 5; i < rest.size();) {
      uint32_t op = SPV_OP(rest[i]);
      uint32_t len = SPV_LEN(rest[i]);
      if (len == 0 || i + len > rest.size())
         break;
      if (op == SpvOpDecorate || op == SpvOpMemberDecorate ||
          op == SpvOpName || op == SpvOpMemberName)
         at = i + len;
      else if (op == SpvOpCapability || op == 10 || op == SpvOpExtInstImport ||
               op == SpvOpMemoryModel || op == SpvOpEntryPoint ||
               op == SpvOpExecutionMode ||
               op == 2 || op == 3 || op == 4 || /* SourceContinued/Source/SourceExtension */
               op == 7 || op == 330)             /* OpString / OpModuleProcessed */
         at = i + len;
      else
         break;
      i += len;
   }
   rest.insert(rest.begin() + at, hoisted.begin(), hoisted.end());

   if (rest.size() != w->size()) {
      return;                  /* never lose bytes */
   }
   w->swap(rest);
}

/* Re-sort instructions into SPIR-V logical layout.
 *
 * Capabilities -> extensions -> entry points/modes -> annotations ->
 * types/constants -> functions. Our rewrite appends new types with
 * push_back (i.e. after functions), which is illegal; running this
 * before emitting fixes the layout no matter where they landed. */
void
normalize_layout(std::vector<uint32_t> *w)
{
   if (w->size() < 5)
      return;

   std::vector<uint32_t> cap, ext, entry, ann, ty, fn;
   size_t tail_off = w->size();
   for (size_t i = 5; i < w->size();) {
      uint32_t op = SPV_OP((*w)[i]);
      uint32_t len = SPV_LEN((*w)[i]);
      if (len == 0 || i + len > w->size()) {
         tail_off = i;
         break;
      }

      std::vector<uint32_t> *dst;
      if (op == SpvOpCapability)                       /* 17 */
         dst = &cap;
      else if (op == 10 || op == SpvOpExtInstImport)   /* OpExtension, OpExtInstImport */
         dst = &ext;
      else if (op == SpvOpMemoryModel ||               /* 14 */
               op == SpvOpEntryPoint ||                /* 15 */
               op == SpvOpExecutionMode ||             /* 16 */
               /* Debug section: SPIR-V puts these after entry points and
                * BEFORE annotations. Falling through to `fn` leaves
                * "OpSource is in an invalid layout section". */
               op == 2 || op == 3 || op == 4 ||        /* SourceContinued / Source / SourceExtension */
               op == 7)                                /* OpString        */
         dst = &entry;
      else if (op == SpvOpDecorate ||                  /* 71 */
               op == SpvOpMemberDecorate ||            /* 72 */
               op == SpvOpName ||                      /* 5  */
               op == SpvOpMemberName ||                /* 6  */
               op == 73 || op == 74 || op == 75)       /* DecorationGroup / GroupDecorate / GroupMemberDecorate */
         dst = &ann;
      else if (/* types */
               op == SpvOpTypeVoid || op == SpvOpTypeBool || op == SpvOpTypeInt || op == SpvOpTypeFloat || op == SpvOpTypeVector ||
               op == SpvOpTypeMatrix || op == SpvOpTypeImage || op == SpvOpTypeSampler || op == SpvOpTypeSampledImage ||
               op == SpvOpTypeOpaque || op == SpvOpTypeArray || op == SpvOpTypeRuntimeArray || op == SpvOpTypeStruct ||
               op == SpvOpTypePointer || op == 39 || /* OpTypeForwardPointer */
               op == SpvOpTypeFunction || op == SpvOpTypeRayQueryKHR || op == SpvOpTypeAccelerationStructureKHR ||
               /* constants and spec constants */
               op == SpvOpUndef ||                 /* 1  */
               op == SpvOpConstantTrue ||          /* 41 */
               op == SpvOpConstantFalse ||         /* 42 */
               op == SpvOpConstant ||              /* 43 */
               op == 44 || op == 45 || op == 46 || /* ConstantComposite / ConstantSampler / ConstantNull */
               op == 48 || op == 49 || op == 50 || /* SpecConstantTrue / False / SpecConstant */
               op == 51 || op == 52)               /* SpecConstantComposite / SpecConstantOp */
          dst = &ty;
      else
         dst = &fn;

      for (uint32_t k = 0; k < len; k++)
         dst->push_back((*w)[i + k]);
      i += len;
   }

   std::vector<uint32_t> out;
   for (size_t h = 0; h < 5 && h < w->size(); h++)
      out.push_back((*w)[h]);
   sort_types_topo(&ty);
   /* hoist_forward_defs(w); superseded by hoist_type_and_deps */
   for (auto *part : {&cap, &ext, &entry, &ann, &ty, &fn})
      out.insert(out.end(), part->begin(), part->end());
   /* Keep any unparseable tail verbatim instead of discarding it. */
   for (size_t i = tail_off; i < w->size(); i++)
      out.push_back((*w)[i]);
   w->swap(out);
}


/* Drop OpName / OpMemberName that target ids the module never defines.
 *
 * Stripping the traversal library's dummy entry point leaves debug names
 * for that function's locals behind. spirv-val then reports the whole
 * run of them as "forward referenced IDs have not been defined". */
void
strip_dangling_debug_names(std::vector<uint32_t> *w)
{
   std::unordered_map<uint32_t, int> defined;
   for (size_t i = 5; i < w->size();) {
      uint32_t op = SPV_OP((*w)[i]);
      uint32_t len = SPV_LEN((*w)[i]);
      if (len == 0 || i + len > w->size())
         break;
      uint32_t rid = result_id(*w, i, op);
      if (rid)
         defined[rid] = 1;
      i += len;
   }

   std::vector<uint32_t> out;
   for (size_t i = 0; i < 5 && i < w->size(); i++)
      out.push_back((*w)[i]);
   for (size_t i = 5; i < w->size();) {
      uint32_t op = SPV_OP((*w)[i]);
      uint32_t len = SPV_LEN((*w)[i]);
      if (len == 0 || i + len > w->size())
         break;
      bool dangling_name =
         (op == SpvOpName || op == SpvOpMemberName) && len >= 2 &&
         !defined.count((*w)[i + 1]);
      /* OpMemberName's target is at word 1 as well. */
      if (!dangling_name) {
         for (uint32_t k = 0; k < len; k++)
            out.push_back((*w)[i + k]);
      }
      i += len;
   }
   if (out.size() == w->size())
      w->swap(out);   /* only ever remove, never reorder */
   else if (out.size() < w->size())
      w->swap(out);
}

/* ------------------------------------------------------------------ */
/* Library prep                                                        */
/* ------------------------------------------------------------------ */

/* Mark every sw_* function as `LinkageAttributes "name" Export`.
 *
 * spvtools::Link with SetCreateLibrary(true) keeps only *exported*
 * symbols; glslc does not emit LinkageAttributes, so without this the
 * whole traversal body is silently dropped and the caller then cannot
 * find sw_ray_query_proceed.
 *
 * OpDecorate <target> LinkageAttributes <name-literal> <Export=0> */
void
export_named_functions(std::vector<uint32_t> *w)
{
   /* Map: function result id -> OpName string (the mangled glslc name). */
   std::unordered_map<uint32_t, std::string> names;
   for_each(*w, [&](size_t i, uint32_t op, uint32_t len) {
      if (op != SpvOpName || len < 3)
         return;
      uint32_t target = (*w)[i + 1];
      std::string s;
      for (uint32_t k = 2; k < len; k++) {
         char c[5];
         std::memcpy(c, &(*w)[i + k], 4);
         for (int b = 0; b < 4; b++) {
            if (c[b] == '\0')
               break;
            s.push_back(c[b]);
         }
      }
      names[target] = s;
   });

   /* Which of those are OpFunction definitions? */
   std::vector<uint32_t> fn_ids;
   for_each(*w, [&](size_t i, uint32_t op, uint32_t len) {
      if (op == SpvOpFunction && len >= 3)
         fn_ids.push_back((*w)[i + 2]);
   });

   std::vector<uint32_t> ann;
   for (uint32_t id : fn_ids) {
      auto it = names.find(id);
      if (it == names.end())
         continue;
      /* Only the public sw_* surface needs exporting. */
      if (it->second.compare(0, 3, "sw_") != 0)
         continue;

      std::vector<uint32_t> lit = spv_string_literal(it->second.c_str());
      ann.push_back(SPV_WORD0(SpvOpDecorate, 3 + (uint32_t)lit.size() + 1));
      ann.push_back(id);
      ann.push_back(41); /* LinkageAttributes */
      for (uint32_t x : lit)
         ann.push_back(x);
      ann.push_back(0); /* Export */
   }
   if (ann.empty())
      return;

   size_t at = 5;
   for_each(*w, [&](size_t i, uint32_t op, uint32_t len) {
      if (op == SpvOpDecorate || op == SpvOpMemberDecorate ||
          op == SpvOpName || op == SpvOpMemberName)
         at = i + len;
   });
   w->insert(w->begin() + at, ann.begin(), ann.end());
}

void
strip_entry_points(std::vector<uint32_t> *w, std::vector<uint32_t> *drop_ids)
{
   std::vector<uint32_t> entry_fns;
   for_each(*w, [&](size_t i, uint32_t op, uint32_t len) {
      if (op == SpvOpEntryPoint && len >= 3)
         entry_fns.push_back((*w)[i + 2]);
   });
   if (entry_fns.empty())
      return;

   for (uint32_t id : entry_fns)
      drop_ids->push_back(id);

   /* Remove OpEntryPoint / OpExecutionMode naming those functions, and
    * the OpFunction..OpFunctionEnd bodies. */
   std::vector<uint32_t> out;
   for (size_t i = 0; i < 5; i++)
      out.push_back((*w)[i]);

   for (size_t i = 5; i < w->size();) {
      uint32_t op = SPV_OP((*w)[i]);
      uint32_t len = SPV_LEN((*w)[i]);
      if (len == 0 || i + len > w->size())
         break;

      bool drop = false;
      if (op == SpvOpEntryPoint && len >= 3) {
         for (uint32_t id : entry_fns)
            if ((*w)[i + 2] == id) drop = true;
      } else if (op == SpvOpExecutionMode && len >= 2) {
         for (uint32_t id : entry_fns)
            if ((*w)[i + 1] == id) drop = true;
      } else if (op == SpvOpFunction && len >= 3) {
         uint32_t rid = (*w)[i + 2];
         for (uint32_t id : entry_fns)
            if (rid == id) drop = true;
      }

      if (drop) {
         /* If dropping a function, swallow to OpFunctionEnd. */
         if (op == SpvOpFunction) {
            i += len;
            while (i < w->size()) {
               uint32_t o2 = SPV_OP((*w)[i]);
               uint32_t l2 = SPV_LEN((*w)[i]);
               if (l2 == 0 || i + l2 > w->size())
                  break;
               i += l2;
               if (o2 == SpvOpFunctionEnd)
                  break;
            }
            continue;
         }
         i += len;
         continue;
      }

      for (uint32_t k = 0; k < len; k++)
         out.push_back((*w)[i + k]);
      i += len;
   }
   w->swap(out);
}

/* ------------------------------------------------------------------ */
/* The rewrite                                                         */
/* ------------------------------------------------------------------ */

/* Map from an OpRayQuery* opcode to the sw_* function that replaces it.
 * Values are the exact GLSL names in sw_ray_query.glsl (found by prefix). */
struct RqOp {
   uint16_t    opcode;
   const char *fn;
   bool        returns_value;
};

const RqOp kRqOps[] = {
    {SpvOpRayQueryInitializeKHR, "sw_ray_query_initialize", false},
    {SpvOpRayQueryProceedKHR, "sw_ray_query_proceed", true},
    {SpvOpRayQueryTerminateKHR, "sw_ray_query_terminate", false},
    {SpvOpRayQueryConfirmIntersectionKHR, "sw_ray_query_confirm", false},
    {SpvOpRayQueryGenerateIntersectionKHR, "sw_ray_query_generate_intersection", false},
    {SpvOpRayQueryGetIntersectionTypeKHR, "sw_get_intersection_type", true},
    {SpvOpRayQueryGetIntersectionTKHR, "sw_get_intersection_t", true},
    {SpvOpRayQueryGetIntersectionBarycentricsKHR, "sw_get_intersection_barycentrics", true},
    {SpvOpRayQueryGetIntersectionPrimitiveIndexKHR, "sw_get_intersection_primitive_index", true},
    {SpvOpRayQueryGetIntersectionGeometryIndexKHR, "sw_get_intersection_geometry_index", true},
    {SpvOpRayQueryGetIntersectionInstanceIdKHR, "sw_get_intersection_instance_id", true},
    {SpvOpRayQueryGetIntersectionInstanceCustomIndexKHR, "sw_get_intersection_instance_custom_index", true},
    {SpvOpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR, "sw_get_intersection_instance_sbt_offset", true},
    {SpvOpRayQueryGetIntersectionFrontFaceKHR, "sw_get_intersection_front_face", true},
    {SpvOpRayQueryGetIntersectionCandidateAABBOpaqueKHR, "sw_get_intersection_candidate_aabb_opaque", true},
    {SpvOpRayQueryGetWorldRayOriginKHR, "sw_get_world_ray_origin", true},
    {SpvOpRayQueryGetIntersectionObjectRayOriginKHR, "sw_get_object_ray_origin", true},
    {SpvOpRayQueryGetIntersectionObjectRayDirectionKHR, "sw_get_object_ray_direction", true},
    {SpvOpRayQueryGetIntersectionObjectToWorldKHR, "sw_get_object_to_world", true},
    {SpvOpRayQueryGetIntersectionWorldToObjectKHR, "sw_get_world_to_object", true},
    {SpvOpRayQueryGetWorldRayDirectionKHR, "sw_get_world_ray_direction", true},
    {SpvOpRayQueryGetRayTMinKHR, "sw_get_ray_tmin", true},
    {SpvOpRayQueryGetRayFlagsKHR, "sw_get_ray_flags", true},
};

struct IdGen {
   uint32_t next;
   uint32_t fresh() { return next++; }
};

/* These scan `merged` directly rather than trusting ti.offset_of: that
 * map goes stale as soon as anything is spliced in, and a stale lookup
 * makes us emit a DUPLICATE type. A duplicate result id is fatal later --
 * the hoist keeps one copy and drops the rest, so byte counts diverge. */

static bool
insn_is_type(const std::vector<uint32_t> &w, size_t i, uint32_t want_op,
             uint32_t want_bits, uint32_t want_signed)
{
   if (SPV_OP(w[i]) != want_op)
      return false;
   if (want_op == SpvOpTypeInt)
      return SPV_LEN(w[i]) == 4 && w[i + 2] == want_bits && w[i + 3] == want_signed;
   return true;
}

/* Find an existing result id for a type/constant, or 0. */
uint32_t
find_type(const std::vector<uint32_t> &w, uint32_t want_op,
          uint32_t want_bits, uint32_t want_signed)
{
   for (size_t i = 5; i < w.size();) {
      uint32_t len = SPV_LEN(w[i]);
      if (len == 0 || i + len > w.size())
         break;
      if (insn_is_type(w, i, want_op, want_bits, want_signed))
         return w[i + 1]; /* OpType* result id is at word 1 */
      i += len;
   }
   return 0;
}

uint32_t
get_int_type(std::vector<uint32_t> *w, TypeIndex *ti, IdGen *ids,
             uint32_t bits, uint32_t signedness)
{
   (void)ti;
   uint32_t id = find_type(*w, SpvOpTypeInt, bits, signedness);
   if (id)
      return id;
   id = ids->fresh();
   w->push_back(SPV_WORD0(SpvOpTypeInt, 4));
   w->push_back(id);
   w->push_back(bits);
   w->push_back(signedness);
   return id;
}

uint32_t
get_void_type(std::vector<uint32_t> *w, TypeIndex *ti, IdGen *ids)
{
   (void)ti;
   uint32_t id = find_type(*w, SpvOpTypeVoid, 0, 0);
   if (id)
      return id;
   id = ids->fresh();
   w->push_back(SPV_WORD0(SpvOpTypeVoid, 2));
   w->push_back(id);
   return id;
}

uint32_t
get_bool_type(std::vector<uint32_t> *w, TypeIndex *ti, IdGen *ids)
{
   (void)ti;
   uint32_t id = find_type(*w, SpvOpTypeBool, 0, 0);
   if (id)
      return id;
   id = ids->fresh();
   w->push_back(SPV_WORD0(SpvOpTypeBool, 2));
   w->push_back(id);
   return id;
}

uint32_t
get_ptr_type(std::vector<uint32_t> *w, TypeIndex *ti, IdGen *ids,
             uint32_t storage_class, uint32_t pointee)
{
   for (size_t i = 5; i < w->size();) {
      uint32_t op = SPV_OP((*w)[i]);
      uint32_t len = SPV_LEN((*w)[i]);
      if (len == 0 || i + len > w->size())
         break;
      if (op == SpvOpTypePointer && len == 4 &&
          (*w)[i + 2] == storage_class && (*w)[i + 3] == pointee)
         return (*w)[i + 1];
      i += len;
   }
   uint32_t id = ids->fresh();
   w->push_back(SPV_WORD0(SpvOpTypePointer, 4));
   w->push_back(id);
   w->push_back(storage_class);
   w->push_back(pointee);
   if (ti)
      ti->pointers[id] = {storage_class, pointee};
   return id;
}

uint32_t
get_const_u32(std::vector<uint32_t> *w, TypeIndex *ti, IdGen *ids,
              uint32_t u32ty, uint32_t value)
{
   (void)ti;
   for (size_t i = 5; i < w->size();) {
      uint32_t op = SPV_OP((*w)[i]);
      uint32_t len = SPV_LEN((*w)[i]);
      if (len == 0 || i + len > w->size())
         break;
      if (op == SpvOpConstant && len == 4 && (*w)[i + 1] == u32ty &&
          (*w)[i + 3] == value)
         return (*w)[i + 2];
      i += len;
   }
   uint32_t id = ids->fresh();
   w->push_back(SPV_WORD0(SpvOpConstant, 4));
   w->push_back(u32ty);
   w->push_back(id);
   w->push_back(value);
   return id;
}

} // namespace

/* ================================================================== */
/* C entry point                                                       */
/* ================================================================== */

int
vkrt_spirv_lower(const uint32_t *user_words, size_t user_word_count,
                 const uint32_t *lib_words, size_t lib_word_count,
                 uint32_t **out_words, size_t *out_word_count,
                 char *errbuf, size_t errbuf_len)
{
   auto fail = [&](const char *msg) -> int {
      if (errbuf && errbuf_len)
         std::snprintf(errbuf, errbuf_len, "%s", msg);
      std::fprintf(stderr, "VKRT SPIR-V lower: %s\n", msg);
      return -1;
   };

   if (out_words)
      *out_words = nullptr;
   if (out_word_count)
      *out_word_count = 0;

   /* ---- 1. Prep the library: drop its dummy entry point ------------ */
   std::vector<uint32_t> lib(lib_words, lib_words + lib_word_count);
   if (lib.size() < 5)
      return fail("traversal library is not valid SPIR-V");
   {
      std::vector<uint32_t> dropped;
      strip_entry_points(&lib, &dropped);
      export_named_functions(&lib);
      std::string error;
      if (!vkrt_spv::dedupe_types(&lib, &error)) return fail(error.c_str());
      strip_dangling_debug_names(&lib);
   }

   /* ---- 1b. Normalise the addressing model -------------------------
    * The traversal uses GLSL buffer_reference (device addresses), which
    * forces PhysicalStorageBuffer64. A Logical-addressed user shader is
    * a strict subset -- PhysicalStorageBuffer64 only *adds* the ability
    * to cast pointers to integers -- so retargeting the user module is
    * safe and is what lets spvtools::Link accept both (it rejects
    * mismatched addressing models).
    */
   std::vector<uint32_t> user(user_words, user_words + user_word_count);
   {
      /* OpMemoryModel <addressing> <memory>; addressing 0=Logical,
       * 1=Physical32, 2=Physical64... actually:
       *   0 Logical, 1 Physical32, 2 Physical64, 5348 PhysicalStorageBuffer64
       * (PhysicalStorageBuffer64 is the one buffer_reference needs.) */
      const uint32_t kPhysicalStorageBuffer64 = 5348;
      bool touched = false;
      for_each(user, [&](size_t i, uint32_t op, uint32_t len) {
         if (op == SpvOpMemoryModel && len == 3 && !touched) {
            if (user[i + 1] != kPhysicalStorageBuffer64) {
               user[i + 1] = kPhysicalStorageBuffer64;
               touched = true;
            }
         }
      });
      if (touched) {
         ensure_capability(&user, SpvCapabilityPhysicalStorageBufferAddresses);
         ensure_extension(&user, "SPV_KHR_physical_storage_buffer");
      }
   }

   /* ---- 2. Link ---------------------------------------------------- */
   spvtools::Context ctx(SPV_ENV_VULKAN_1_2);
   std::string link_diag;
   ctx.SetMessageConsumer(
      [&](spv_message_level_t, const char *, const spv_position_t &,
          const char *message) {
         if (!link_diag.empty())
            link_diag += " | ";
         link_diag += (message ? message : "(no message)");
      });

   spvtools::LinkerOptions lopts;
   lopts.SetCreateLibrary(true);
   lopts.SetAllowPartialLinkage(true);
   lopts.SetVerifyIds(true);
   lopts.SetUseHighestVersion(true);

   std::vector<uint32_t> merged;
   std::vector<std::vector<uint32_t>> inputs = {
      user,
      lib,
   };
   spv_result_t lres = spvtools::Link(ctx, inputs, &merged, lopts);
   if (lres != SPV_SUCCESS) {
      std::string msg = "spvtools::Link failed: " +
                        (link_diag.empty() ? "(no diagnostic)" : link_diag);
      return fail(msg.c_str());
   }
   strip_linkage_decorations(&merged);

   /* ---- 3. Index the merged module --------------------------------- */
   Symbols sym;
   collect_symbols(merged, &sym);

   TypeIndex ti;
   index_types(merged, &ti);

   /* Locate the state struct. glslang emits an exact OpName for the
    * struct ("sw_ray_query_state") but mangles function names, so prefer
    * the struct name and fall back to digging it out of
    * sw_ray_query_proceed's first parameter. */
   uint32_t proceed_id = sym.find("sw_ray_query_proceed");
   if (!proceed_id)
      return fail("sw_ray_query_proceed not found after link");
   uint32_t state_ty = sym.exact.count("sw_ray_query_state")
                          ? sym.exact["sw_ray_query_state"]
                          : fn_first_param_pointee(merged, ti, proceed_id);
   if (!state_ty)
      return fail("could not derive sw_ray_query_state type from proceed()");

   /* Collect the sw_* function ids we will call. */
   std::unordered_map<uint32_t, uint32_t> rqop_to_fn; /* RqOp index -> fn id */
   std::unordered_map<uint16_t, uint32_t> op_to_fn;
   for (const RqOp &r : kRqOps) {
      uint32_t id = sym.find(r.fn);
      if (!id) {
         std::fprintf(stderr, "VKRT: traversal fn %s missing (op %u)\n",
                      r.fn, (unsigned)r.opcode);
         continue;
      }
      op_to_fn[r.opcode] = id;
   }

   /* Also map the AS type substitution. */
   uint32_t as_type_id = 0;
   for (auto &kv : ti.offset_of) {
      if (SPV_OP(merged[kv.second]) == SpvOpTypeAccelerationStructureKHR) {
         as_type_id = kv.first;
         break;
      }
   }

   /* Remap every OpTypeRayQueryKHR id to state_ty BEFORE the main
    * rewrite pass drops the definition -- doing it afterwards leaves
    * dangling ids (%_ptr_Private_<rq> no longer resolves). */
   {
      std::vector<uint32_t> rq_ids;
      for_each(merged, [&](size_t i, uint32_t op, uint32_t len) {
         if (op == SpvOpTypeRayQueryKHR && len >= 2)
            rq_ids.push_back(merged[i + 1]);
      });
      if (!rq_ids.empty()) {
         std::unordered_map<uint32_t, uint32_t> sub;
         for (uint32_t id : rq_ids)
            sub[id] = state_ty;

         std::string error;
         if (!vkrt_spv::remap_id_uses(&merged, sub, &error)) return fail(error.c_str());

         std::vector<uint32_t> tmp;
         for (size_t i = 0; i < 5 && i < merged.size(); i++)
            tmp.push_back(merged[i]);
         for (size_t i = 5; i < merged.size();) {
            uint32_t op = SPV_OP(merged[i]);
            uint32_t len = SPV_LEN(merged[i]);
            if (len == 0 || i + len > merged.size())
               break;
            if (op == SpvOpTypeRayQueryKHR) {
               i += len;
               continue;
            }
            tmp.insert(tmp.end(), merged.begin() + i, merged.begin() + i + len);
            i += len;
         }
         merged.swap(tmp);
         /* The swap invalidated every offset in ti. Re-index before the
          * get_* helpers below start indexing merged through it -- stale
          * offsets read out of bounds and write garbage result ids. */
         ti.offset_of.clear();
         ti.pointers.clear();
         ti.functions.clear();
         index_types(merged, &ti);
      }
   }

   /* Member indices by (type id, field name). The rewrite stores extra
    * OpRayQuery* operands into sw_ray_query_state fields before each
    * call -- every sw_* entry point takes only `inout q`, because
    * glslang's ABI passes all parameters as Function-class pointers
    * that a synthesised call site cannot materialise. */
   std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>> member_idx;
   {
      std::unordered_map<uint32_t, std::string> type_name;
      for_each(merged, [&](size_t i, uint32_t op, uint32_t len) {
         if (op == SpvOpName && len >= 3) {
            std::string s2;
            for (uint32_t k = 2; k < len; k++) {
               char c[5];
               std::memcpy(c, &merged[i + k], 4);
               for (int b = 0; b < 4; b++) {
                  if (c[b] == '\0') break;
                  s2.push_back(c[b]);
               }
            }
            type_name[merged[i + 1]] = s2;
         } else if (op == SpvOpMemberName && len >= 4) {
            std::string s2;
            for (uint32_t k = 3; k < len; k++) {
               char c[5];
               std::memcpy(c, &merged[i + k], 4);
               for (int b = 0; b < 4; b++) {
                  if (c[b] == '\0') break;
                  s2.push_back(c[b]);
               }
            }
            member_idx[merged[i + 1]][s2] = merged[i + 2];
         }
      });
      /* sw_ray's member indices are needed for q.ray.origin/.direction. */
      for (auto &kv : type_name) {
         if (kv.second == "sw_ray")
            member_idx[kv.first]; /* ensure it exists */
      }
   }

   /* ---- 4. Rewrite -------------------------------------------------- */
   /* We rebuild into a fresh word vector so we can expand instructions.
    *
    * Start allocating from the header's ID bound: by definition every
    * valid id is < bound, so fresh() can never collide with an existing
    * type (an earlier version collided with sw_ray_query_state and the
    * duplicate OpTypeStruct silently ate the real definition). */
   IdGen ids;
   ids.next = (merged.size() >= 4 && merged[3] >= 1) ? merged[3] : 1;
   for (auto &kv : ti.offset_of) {
      /* SPIR-V ids are < 2^22. Anything larger came from a mis-read
       * literal and must not steer allocation. */
      if (kv.first < (1u << 22) && kv.first >= ids.next)
         ids.next = kv.first + 1;
   }

   uint32_t u32_ty = get_int_type(&merged, &ti, &ids, 32, 0);
   uint32_t u64_ty = get_int_type(&merged, &ti, &ids, 64, 0);
   uint32_t void_ty = get_void_type(&merged, &ti, &ids);
   uint32_t bool_ty = get_bool_type(&merged, &ti, &ids);
   (void)get_const_u32;   /* zero_c is only needed once an AS address is stored */

   /* Coerce `val` to `want_ty` so a store into a struct field type-checks.
    * OpRayQuery* operands are whatever glslang constant-folded (often
    * %int where the field is %uint, or %bool where it is %uint). */
   auto emit_convert = [&](std::vector<uint32_t> *outv, uint32_t val,
                           uint32_t from_ty, uint32_t want_ty) -> uint32_t {
      if (val == 0 || from_ty == 0 || from_ty == want_ty)
         return val;
      /* bool -> uint */
      if (want_ty == u32_ty && from_ty == bool_ty) {
         uint32_t r = ids.fresh();
         uint32_t one = get_const_u32(&merged, &ti, &ids, u32_ty, 1);
         uint32_t zero32 = get_const_u32(&merged, &ti, &ids, u32_ty, 0);
         /* OpSelect <rty> <res> <cond> <a> <b> is 6 words. Emitting 7
          * silently truncates every later instruction in the stream. */
         outv->push_back(SPV_WORD0(169, 6));   /* OpSelect */
         outv->push_back(u32_ty);
         outv->push_back(r);
         outv->push_back(val);
         outv->push_back(one);
         outv->push_back(zero32);
         return r;
      }
      /* same-width scalar: bitcast covers int <-> uint <-> float */
      uint32_t r = ids.fresh();
      outv->push_back(SPV_WORD0(SpvOpBitcast, 4));
      outv->push_back(want_ty);
      outv->push_back(r);
      outv->push_back(val);
      return r;
   };

   /* Per-function OpRayQuery* call count. Each call needs a
    * Function-class temporary to pass as `inout q`: glslang types that
    * parameter as a FUNCTION pointer while a file-scope rayQueryEXT is a
    * PRIVATE global, and OpFunctionCall only accepts a memory object
    * declaration (so neither a bitcast nor a computed pointer works). */
   std::unordered_map<uint32_t, uint32_t> fn_rq_count;
   {
      uint32_t cur_fn = 0;
      for_each(merged, [&](size_t i, uint32_t op, uint32_t len) {
         if (op == SpvOpFunction && len >= 3)
            cur_fn = merged[i + 2];
         else if (op == SpvOpFunctionEnd)
            cur_fn = 0;
         else if (cur_fn && op_to_fn.count((uint16_t)op))
            fn_rq_count[cur_fn]++;
      });
   }

   /* Helper: store `val` into q.<member chain>. `want_ty` is the struct
    * field's type -- the AccessChain result type must match it. */
   /* Wrap `q_orig` (a Private global) in a Function-class temporary the
    * call can legally accept: load the state, store it into the temp,
    * call with the temp, then store the mutated state back. */
   auto emit_call_q = [&](std::vector<uint32_t> *outv, uint32_t temp,
                          uint32_t q_orig) {
      uint32_t v1 = ids.fresh();
      outv->push_back(SPV_WORD0(SpvOpLoad, 4));
      outv->push_back(state_ty);
      outv->push_back(v1);
      outv->push_back(q_orig);
      outv->push_back(SPV_WORD0(SpvOpStore, 3));
      outv->push_back(temp);
      outv->push_back(v1);
   };
   auto emit_call_q_back = [&](std::vector<uint32_t> *outv, uint32_t temp,
                               uint32_t q_orig) {
      uint32_t v2 = ids.fresh();
      outv->push_back(SPV_WORD0(SpvOpLoad, 4));
      outv->push_back(state_ty);
      outv->push_back(v2);
      outv->push_back(temp);
      outv->push_back(SPV_WORD0(SpvOpStore, 3));
      outv->push_back(q_orig);
      outv->push_back(v2);
   };

   /* A query may be a Private global or Function storage passed through a
    * helper. Preserve its actual pointer storage class when addressing fields. */
   std::unordered_map<uint32_t, uint32_t> val_ty;
   for_each(merged, [&](size_t i, uint32_t op, uint32_t len) {
       if (spv_result_id_index(op) == 2 && len >= 3) val_ty[merged[i + 2]] = merged[i + 1];
   });
   std::string rewrite_error;
   auto emit_store = [&](std::vector<uint32_t> *outv, uint32_t qvar, const std::vector<uint32_t> &chain, uint32_t val,
                         uint32_t source_ty, uint32_t want_ty) {
       uint32_t coerced = emit_convert(outv, val, source_ty, want_ty);
       auto value = val_ty.find(qvar);
       auto pointer = value == val_ty.end() ? ti.pointers.end() : ti.pointers.find(value->second);
       if (pointer == ti.pointers.end() ||
           (pointer->second.first != SpvStorageClassPrivate && pointer->second.first != SpvStorageClassFunction)) {
           rewrite_error = "ray-query operand has no Private/Function pointer type";
           return;
       }
       uint32_t ptr_ty = get_ptr_type(&merged, &ti, &ids, pointer->second.first, want_ty);
       uint32_t gep = ids.fresh();
       outv->push_back(SPV_WORD0(SpvOpAccessChain, 4 + (uint32_t)chain.size()));
       outv->push_back(ptr_ty);
       outv->push_back(gep);
       outv->push_back(qvar);
       for (uint32_t idx : chain) /* OpAccessChain indices are constant IDS, not literals. */
           outv->push_back(get_const_u32(&merged, &ti, &ids, u32_ty, idx));
       outv->push_back(SPV_WORD0(SpvOpStore, 3));
       outv->push_back(gep);
       outv->push_back(coerced);
   };

   /* Pointer to a Private uint64, used when writing q.as. Private is
    * per-invocation in Vulkan. */
   (void)get_ptr_type(&merged, &ti, &ids,
                      SpvStorageClassPrivate, u64_ty);

   /* ---- AS descriptor rewrite ---------------------------------------
    * Doc section 8: an AS binding becomes a storage buffer, keeping the
    * app's (set, binding) numbering. The bound buffer IS the BVHIR
    * backing buffer, so the shader wants its *device address* rather
    * than its contents -- sw_ray_query_initialize takes a uint64_t root.
    *
    *   OpTypePointer UniformConstant %asType
    *     -> OpTypePointer StorageBuffer %asBlock   (Block struct)
    *   OpVariable %var %asPtr UniformConstant
    *     -> OpVariable %var %asPtr StorageBuffer
    *   OpLoad %asType %var
    *     -> %phys = OpBitcast %physPtr %var
    *        %addr = OpConvertPtrToU %u64 %phys
    *
    * OpConvertPtrToU requires a PhysicalStorageBuffer-class pointer,
    * hence the OpBitcast hop. A StorageBuffer variable must point at a
    * Block-decorated struct, hence the dummy one-member block.
    * ---------------------------------------------------------------- */
   uint32_t as_struct = 0;      /* Block { u64 } -- contents never read   */
   uint32_t as_struct_ptr = 0;  /* kept so the Block type is materialised */
   uint32_t as_ptr_type_id = 0; /* the AS OpTypePointer (dropped)       */
   uint32_t as_var_id = 0;      /* the AS OpVariable (dropped)         */
   uint32_t as_set = 0, as_binding = 0;
   bool have_as_binding = false;
   if (as_type_id) {
      as_struct = ids.fresh();
      merged.push_back(SPV_WORD0(SpvOpTypeStruct, 3));
      merged.push_back(as_struct);
      merged.push_back(u64_ty);
      ti.offset_of[as_struct] = merged.size() - 3;

      as_struct_ptr = get_ptr_type(&merged, &ti, &ids,
                                   SpvStorageClassStorageBuffer, as_struct);
      (void)as_struct_ptr;

      /* Block + Member Offset 0. Annotations must precede functions. */
      std::vector<uint32_t> ann;
      ann.push_back(SPV_WORD0(SpvOpDecorate, 3));
      ann.push_back(as_struct);
      ann.push_back(SpvDecorationBlock);
      ann.push_back(SPV_WORD0(SpvOpMemberDecorate, 5));
      ann.push_back(as_struct);
      ann.push_back(0);
      ann.push_back(SpvDecorationOffset);
      ann.push_back(0);
      size_t at = 5;
      for_each(merged, [&](size_t i, uint32_t op, uint32_t len) {
         if (op == SpvOpDecorate || op == SpvOpMemberDecorate ||
             op == SpvOpName || op == SpvOpMemberName)
            at = i + len;
      });
      merged.insert(merged.begin() + at, ann.begin(), ann.end());
      /* ti offsets are stale after the insert -- as_type_id is still
       * valid as an id, which is all the rewrite pass uses. */
   }

   /* Ensure Int64 + PhysicalStorageBufferAddresses are declared (the
    * traversal uses uint64_t buffer references). */
   ensure_capability(&merged, SpvCapabilityInt64);
   ensure_capability(&merged, SpvCapabilityPhysicalStorageBufferAddresses);
   ensure_extension(&merged, "SPV_KHR_physical_storage_buffer");

   /* Every OpRayQuery* and OpTypeRayQueryKHR is gone at this point, so
   * the RayQueryKHR capability is no longer required. Dropping it is
   * what lets the module run on an ICD with no ray-query support (the
   * whole point of the layer). */
   {
      std::vector<uint32_t> out_caps;
      /* Keep the 5-word SPIR-V header! Rebuilding from offset 5 and
       * swapping without re-prepending it destroys the module (magic
       * number gone -> the whole rewrite emits a 5-word stub). */
      for (size_t h = 0; h < 5 && h < merged.size(); h++)
         out_caps.push_back(merged[h]);
      size_t i = 5;
      while (i < merged.size()) {
         uint32_t op = SPV_OP(merged[i]);
         uint32_t len = SPV_LEN(merged[i]);
         if (len == 0 || i + len > merged.size())
            break;
         if (op == SpvOpCapability && len == 2 &&
             (merged[i + 1] == SpvCapabilityRayQueryKHR ||
              merged[i + 1] == SpvCapabilityRayTracingKHR)) {
            i += len;
            continue;
         }
         for (uint32_t k = 0; k < len; k++)
            out_caps.push_back(merged[i + k]);
         i += len;
      }
      merged.swap(out_caps);
   }
   /* Walk and rewrite into `out`. */
   std::vector<uint32_t> out;
   out.push_back(merged[0]);
   out.push_back(merged[1]);
   out.push_back(merged[2]);
   out.push_back(merged[3]);
   out.push_back(merged[4]);

   /* Pending function bodies' "insert after this point" is just linear
    * append; SPIR-V allows the type/const sections to grow. */

   /* One Function-class temporary per ray-query call in each function.
    * They are emitted just after that function's first OpLabel, which is
    * the only place SPIR-V allows a Function OpVariable. */
   uint32_t cur_fn = 0;
   uint32_t cur_fn_next_temp = 0;
   std::unordered_map<uint32_t, uint32_t> fn_temp_base;
   std::unordered_map<uint32_t, uint32_t> fn_temps_emitted;
   uint32_t state_fn_ptr = get_ptr_type(&merged, &ti, &ids,
                                        SpvStorageClassFunction, state_ty);

   for (size_t i = 5; i < merged.size();) {
      uint32_t op = SPV_OP(merged[i]);
      uint32_t len = SPV_LEN(merged[i]);
      if (len == 0 || i + len > merged.size())
         break;

      /* --- OpTypeRayQueryKHR: dropped; its id is remapped to the
      *     sw_ray_query_state struct by the substitution below ------- */
      /* OpModuleProcessed is linker bookkeeping. The validator treats it
       * as the end of the debug section, so any OpName after it is
       * rejected as "in an invalid layout section". */
      if (op == 330) {
         i += len;
         continue;
      }

      if (op == SpvOpTypeRayQueryKHR) {
         i += len;
         continue;
      }

      /* --- OpTypeAccelerationStructureKHR -> OpTypeInt %same-id 64 ---
      * Reusing the SAME result id means every OpTypePointer / OpLoad
      * that referenced it keeps working -- no dangling ids. The value
      * is the BVHIR root device address. -------------------------------- */
      if (op == SpvOpTypeAccelerationStructureKHR && len == 2) {
         out.push_back(SPV_WORD0(SpvOpTypeInt, 4));
         out.push_back(merged[i + 1]); /* keep the result id */
         out.push_back(64);
         out.push_back(0); /* unsigned */
         i += len;
         continue;
      }

      /* --- drop the AS pointer type and variable --------------------
       * The TLAS is provided by the library's `vkrt_tlas` storage
       * buffer, which the pass below points at the app's original
       * (set, binding). The app's own AS variable is therefore unused. */
      if (op == SpvOpTypePointer && as_type_id && len == 4 &&
          merged[i + 3] == as_type_id) {
         as_ptr_type_id = merged[i + 1];
         i += len;
         continue;
      }
      if (op == SpvOpVariable && as_ptr_type_id && len >= 4 &&
          merged[i + 1] == as_ptr_type_id) {
         as_var_id = merged[i + 2];
         i += len;
         continue;
      }

      /* --- OpTypePointer <Function> %state -> <Private> %state --------
       * glslang compiles a file-scope `rayQueryEXT q;` into a PRIVATE
       * global, while an `inout sw_ray_query_state` parameter becomes a
       * FUNCTION pointer. SPIR-V requires the call argument's pointer
       * type to match the parameter exactly, so normalise every
       * Function-class pointer to state over to Private -- both are just
       * "a place to keep the struct". */
      if (false && op == SpvOpTypePointer && state_ty && len == 4 &&
          merged[i + 3] == state_ty &&
          merged[i + 2] == SpvStorageClassFunction) {
         uint32_t pid = merged[i + 1];
         out.push_back(SPV_WORD0(SpvOpTypePointer, 4));
         out.push_back(pid);
         out.push_back(SpvStorageClassPrivate);
         out.push_back(merged[i + 3]);   /* keep the original pointee */
         i += len;
         continue;
      }

      /* --- OpTypePointer ... %as -> StorageBuffer %asBlock ------------
      * UniformConstant has no Vulkan descriptor that reads as a plain
      * uint64, so the binding becomes a StorageBuffer. The variable's
      * bound range IS the BVHIR backing buffer; we only take its
      * address, never its contents. ------------------------------------ */
      if (op == SpvOpTypePointer && as_type_id && len == 4 &&
          merged[i + 3] == as_type_id) {
         uint32_t pid = merged[i + 1];
         uint32_t sc  = merged[i + 2];
         uint32_t out_sc = (sc == SpvStorageClassUniformConstant)
                              ? SpvStorageClassStorageBuffer : sc;
         if (sc == SpvStorageClassUniformConstant)
            as_ptr_type_id = pid;
         out.push_back(SPV_WORD0(SpvOpTypePointer, 4));
         out.push_back(pid);
         out.push_back(out_sc);
         out.push_back(as_struct);
         i += len;
         continue;
      }

      /* --- OpVariable of the AS pointer type ------------------------
       * OpVariable <result type> <result id> <storage class> is 4 words:
       * the storage class is word 3, not word 4. Using word 4 left the
       * operand as UniformConstant while the (retargeted) result type
       * said StorageBuffer. */
      if (op == SpvOpVariable && as_ptr_type_id && len >= 4 &&
          merged[i + 1] == as_ptr_type_id) {
         out.push_back(SPV_WORD0(SpvOpVariable, len));
         for (uint32_t k = 1; k < len; k++)
            out.push_back(merged[i + k]);
         out[out.size() - len + 3] = SpvStorageClassStorageBuffer;
         i += len;
         continue;
      }

      /* --- OpLoad of the AS object: drop it ------------------------
       * The traversal reads the TLAS through the bound storage buffer,
       * so no address has to be materialised and the load is dead. */
      if (op == SpvOpLoad && as_type_id && len == 4 &&
          merged[i + 1] == as_type_id) {
         i += len;
         continue;
      }

      /* --- OpRayQuery*KHR -> OpFunctionCall ------------------------- */
      auto rqit = op_to_fn.find((uint16_t)op);
      if (rqit != op_to_fn.end()) {
         uint32_t fn = rqit->second;
         bool returns = false;
         for (const RqOp &r : kRqOps)
            if (r.opcode == op) { returns = r.returns_value; break; }

         /* Every sw_* entry point takes a single `inout q`; the extra
          * OpRayQuery* operands are stored into q's fields first. */
         auto m_of = [&](const char *f) -> uint32_t {
            auto it = member_idx.find(state_ty);
            if (it == member_idx.end()) return 0u;
            auto jt = it->second.find(f);
            return jt == it->second.end() ? 0u : jt->second;
         };

         if (op == SpvOpRayQueryInitializeKHR && len >= 9) {
            /* OpRayQueryInitializeKHR %rq %as %flags %mask %origin %tmin %dir %tmax
             * %as is dropped: the TLAS is reached through the bound
             * storage buffer, so there is no address to record. */
            uint32_t rq_var = merged[i + 1];
            emit_store(&out, rq_var, {m_of("ray_flags")},
                       merged[i + 3], val_ty.count(merged[i + 3]) ? val_ty[merged[i + 3]] : u32_ty, u32_ty);
            emit_store(&out, rq_var, {m_of("cull_mask")},
                       merged[i + 4], val_ty.count(merged[i + 4]) ? val_ty[merged[i + 4]] : u32_ty, u32_ty);
            /* sw_ray is { origin, direction, inv_direction }. */
            emit_store(&out, rq_var, {m_of("ray"), 0u},
                       merged[i + 5], val_ty.count(merged[i + 5]) ? val_ty[merged[i + 5]] : u32_ty, val_ty.count(merged[i + 5]) ? val_ty[merged[i + 5]] : u32_ty);
            emit_store(&out, rq_var, {m_of("tmin")},
                       merged[i + 6], val_ty.count(merged[i + 6]) ? val_ty[merged[i + 6]] : u32_ty, val_ty.count(merged[i + 6]) ? val_ty[merged[i + 6]] : u32_ty);
            emit_store(&out, rq_var, {m_of("ray"), 1u},
                       merged[i + 7], val_ty.count(merged[i + 7]) ? val_ty[merged[i + 7]] : u32_ty, val_ty.count(merged[i + 7]) ? val_ty[merged[i + 7]] : u32_ty);
            emit_store(&out, rq_var, {m_of("tclosest")},
                       merged[i + 8], val_ty.count(merged[i + 8]) ? val_ty[merged[i + 8]] : u32_ty, val_ty.count(merged[i + 8]) ? val_ty[merged[i + 8]] : u32_ty);
         } else if (op == SpvOpRayQueryGenerateIntersectionKHR && len >= 3) {
            /* OpRayQueryGenerateIntersectionKHR %rq %thit */
            emit_store(&out, merged[i + 1], {m_of("gen_thit")},
                       merged[i + 2],
                       val_ty.count(merged[i + 2]) ? val_ty[merged[i + 2]] : u32_ty,
                       val_ty.count(merged[i + 2]) ? val_ty[merged[i + 2]] : u32_ty);
         } else if (len >= 5 &&
                    (op == SpvOpRayQueryGetIntersectionTypeKHR || op == SpvOpRayQueryGetIntersectionTKHR ||
                     op == SpvOpRayQueryGetIntersectionBarycentricsKHR || op == SpvOpRayQueryGetIntersectionPrimitiveIndexKHR ||
                     op == SpvOpRayQueryGetIntersectionGeometryIndexKHR || op == SpvOpRayQueryGetIntersectionInstanceIdKHR ||
                     op == SpvOpRayQueryGetIntersectionInstanceCustomIndexKHR ||
                     op == SpvOpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR ||
                     op == SpvOpRayQueryGetIntersectionObjectRayOriginKHR ||
                     op == SpvOpRayQueryGetIntersectionObjectRayDirectionKHR ||
                     op == SpvOpRayQueryGetIntersectionObjectToWorldKHR || op == SpvOpRayQueryGetIntersectionWorldToObjectKHR ||
                     op == SpvOpRayQueryGetIntersectionFrontFaceKHR)) {
             /* OpX %rty %res %rq %committed  ->  store committed into q */
             emit_store(&out, merged[i + 3], {m_of("want_committed")}, merged[i + 4],
                        val_ty.count(merged[i + 4]) ? val_ty[merged[i + 4]] : bool_ty, u32_ty);
         }

         if (returns) {
            /* OpX %resultType %result %rq <extra...> */
            if (len < 4) {
               i += len;
               continue;
            }
            uint32_t rty = merged[i + 1];
            uint32_t res = merged[i + 2];
            /* SPIR-V permits either signedness for this 32-bit result;
             * the GLSL helper has the unsigned return type. */
            uint32_t call_type = op == SpvOpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR ? val_ty.at(fn) : rty;
            uint32_t call_result = call_type == rty ? res : ids.fresh();
            uint32_t q_orig = merged[i + 3];
            uint32_t q_arg = q_orig;
            if (cur_fn && fn_temp_base.count(cur_fn)) {
               q_arg = fn_temp_base[cur_fn] + cur_fn_next_temp++;
               emit_call_q(&out, q_arg, q_orig);
            }
            out.push_back(SPV_WORD0(SpvOpFunctionCall, 5));
            out.push_back(call_type);
            out.push_back(call_result);
            out.push_back(fn);
            out.push_back(q_arg);   /* %rq */
            if (call_type != rty) {
                out.push_back(SPV_WORD0(SpvOpBitcast, 4));
                out.push_back(rty);
                out.push_back(res);
                out.push_back(call_result);
            }
            if (q_arg != q_orig)
               emit_call_q_back(&out, q_arg, q_orig);
         } else {
            /* OpX %rq <extra...>  (no result). Only the ray-query object
             * is passed; everything else is already in q. The call is
             * therefore always 5 words: word0, void, res, fn, %rq. */
            uint32_t q_orig = merged[i + 1];
            uint32_t q_arg = q_orig;
            if (cur_fn && fn_temp_base.count(cur_fn)) {
               q_arg = fn_temp_base[cur_fn] + cur_fn_next_temp++;
               emit_call_q(&out, q_arg, q_orig);
            }
            out.push_back(SPV_WORD0(SpvOpFunctionCall, 5));
            out.push_back(void_ty);
            out.push_back(ids.fresh()); /* unused result id for void call */
            out.push_back(fn);
            out.push_back(q_arg);   /* %rq */
            if (q_arg != q_orig)
               emit_call_q_back(&out, q_arg, q_orig);
         }
         i += len;
         continue;
      }

      /* --- track the enclosing function and mint its temporaries ----- */
      if (op == SpvOpFunction && len >= 3) {
         cur_fn = merged[i + 2];
         cur_fn_next_temp = 0;
      } else if (op == SpvOpFunctionEnd) {
         cur_fn = 0;
      } else if (op == 248 /* OpLabel */ && cur_fn &&
                 fn_rq_count.count(cur_fn) && !fn_temps_emitted.count(cur_fn)) {
         out.push_back(SPV_WORD0(248, 2));
         out.push_back(merged[i + 1]);
         uint32_t n = fn_rq_count[cur_fn];
         fn_temp_base[cur_fn] = ids.fresh();
         /* Consume the whole block up front: fresh() hands out one id at
          * a time, so allocating n of them as base+k without advancing
          * next makes every later fresh() collide. */
         for (uint32_t k = 1; k < n; k++)
            (void)ids.fresh();
         for (uint32_t k = 0; k < n; k++) {
            uint32_t tid = fn_temp_base[cur_fn] + k;
            out.push_back(SPV_WORD0(SpvOpVariable, 4));
            out.push_back(state_fn_ptr);
            out.push_back(tid);
            out.push_back(SpvStorageClassFunction);
         }
         fn_temps_emitted[cur_fn] = 1;
         i += len;
         continue;
      }

      /* --- default: copy through ------------------------------------ */
      for (uint32_t k = 0; k < len; k++)
         out.push_back(merged[i + k]);
      i += len;
   }

   if (!rewrite_error.empty()) return fail(rewrite_error.c_str());
   /* (OpTypeRayQueryKHR ids were remapped before the main pass.) */

   /* Hand vkrt_tlas the (set, binding) the app used for its
    * acceleration structure, and drop the app's AS variable from the
    * entry-point interface (it was removed above). */
   {
      /* Collect the app's AS descriptor decorations. */
      for_each(merged, [&](size_t i, uint32_t op, uint32_t len) {
         if (op != SpvOpDecorate || len < 4)
            return;
         if (as_var_id && merged[i + 1] == as_var_id) {
            if (merged[i + 2] == 33 /* DescriptorSet */) {
               as_set = merged[i + 3];
               have_as_binding = true;
            } else if (merged[i + 2] == 34 /* Binding */) {
               as_binding = merged[i + 3];
               have_as_binding = true;
            }
         }
      });

      uint32_t tlas_id = 0;
      {
         std::unordered_map<uint32_t, std::string> names;
         for_each(merged, [&](size_t i, uint32_t op, uint32_t len) {
            if (op != SpvOpName || len < 3)
               return;
            std::string s2;
            for (uint32_t k = 2; k < len; k++) {
               char c[5];
               std::memcpy(c, &merged[i + k], 4);
               for (int b = 0; b < 4; b++) {
                  if (c[b] == '\0') break;
                  s2.push_back(c[b]);
               }
            }
            if (s2 == "vkrt_tlas")
               tlas_id = merged[i + 1];
         });
      }

      std::vector<uint32_t> out2;
      for (size_t i = 0; i < 5 && i < out.size(); i++)
         out2.push_back(out[i]);
      for (size_t i = 5; i < out.size();) {
         uint32_t op = SPV_OP(out[i]);
         uint32_t len = SPV_LEN(out[i]);
         if (len == 0 || i + len > out.size())
            break;

         /* Rewrite vkrt_tlas's DescriptorSet/Binding to the app's. */
         if (tlas_id && have_as_binding && op == SpvOpDecorate && len >= 4 &&
             out[i + 1] == tlas_id) {
            if (out[i + 2] == 33) {
               out2.push_back(SPV_WORD0(SpvOpDecorate, 4));
               out2.push_back(tlas_id);
               out2.push_back(33);
               out2.push_back(as_set);
            } else if (out[i + 2] == 34) {
               out2.push_back(SPV_WORD0(SpvOpDecorate, 4));
               out2.push_back(tlas_id);
               out2.push_back(34);
               out2.push_back(as_binding);
            } else {
               for (uint32_t k = 0; k < len; k++)
                  out2.push_back(out[i + k]);
            }
            i += len;
            continue;
         }

         /* Drop the AS variable's own decorations. */
         if (as_var_id && op == SpvOpDecorate && len >= 2 &&
             out[i + 1] == as_var_id) {
            i += len;
            continue;
         }

         /* Replace the AS variable with vkrt_tlas in OpEntryPoint's
          * interface: the app's AS variable is gone and the traversal's
          * storage buffer must be listed instead. */
         if (op == SpvOpEntryPoint && len > 3) {
            std::vector<uint32_t> keep;
            keep.push_back(out[i + 0]);
            keep.push_back(out[i + 1]);
            keep.push_back(out[i + 2]);
            bool listed = false;
            /* Entry-point names are literal strings, not interface IDs. */
            uint32_t interface_start = 3;
            for (; interface_start < len; ++interface_start) {
               uint32_t word = out[i + interface_start];
               keep.push_back(word);
               if (!(word & 0xffu) || !(word & 0xff00u) ||
                   !(word & 0xff0000u) || !(word & 0xff000000u)) {
                  ++interface_start;
                  break;
               }
            }
            for (uint32_t k = interface_start; k < len; k++) {
               if (out[i + k] == as_var_id) {
                  if (tlas_id)
                     keep.push_back(tlas_id);
                  listed = true;
               } else if (out[i + k] == tlas_id) {
                  listed = true;
                  keep.push_back(out[i + k]);
               } else {
                  keep.push_back(out[i + k]);
               }
            }
            if (tlas_id && !listed)
               keep.push_back(tlas_id);
            keep[0] = SPV_WORD0(SpvOpEntryPoint, (uint32_t)keep.size());
            for (uint32_t k = 0; k < keep.size(); k++)
               out2.push_back(keep[k]);
            i += len;
            continue;
         }

         for (uint32_t k = 0; k < len; k++)
            out2.push_back(out[i + k]);
         i += len;
      }
      out.swap(out2);
   }

   /* ---- 5. Emit ---------------------------------------------------- */
   /* Hoist BEFORE the layout pass: at this point the module is still in
    * glslc's logical order (annotations -> types -> functions), so the
    * splice lands at the head of the type region. Running it after
    * normalize_layout would insert types into the middle of a function.
    * normalize_layout then keeps them first within the type bucket. */
   hoist_type_and_deps(&out, state_ty);
   normalize_layout(&out);
   std::string dedupe_error;
   if (!vkrt_spv::dedupe_types(&out, &dedupe_error)) return fail(dedupe_error.c_str());
   strip_dangling_debug_names(&out);

   /* Word 3 of the header is the ID bound: every result id in the module
    * must be < bound. The rewrite allocates fresh ids (ids.fresh()) so
    * the original bound is stale. Recompute it from what the module
    * actually defines rather than trusting ids.next -- a single bogus
    * result id would otherwise push the bound past the 2^22-1 limit. */
   if (out.size() >= 4) {
      uint32_t max_id = out[3];
      for (size_t i = 5; i < out.size();) {
         uint32_t op = SPV_OP(out[i]);
         uint32_t len = SPV_LEN(out[i]);
         if (len == 0 || i + len > out.size())
            break;
         uint32_t rid = result_id(out, i, op);
         if (rid > max_id && rid < (1u << 22))
            max_id = rid;
         i += len;
      }
      /* Also cover ids we allocated but that a pass may have dropped. */
      if (ids.next > max_id && ids.next < (1u << 22))
         max_id = ids.next;
      out[3] = max_id + 1;
   }

   /* Never hand an invalid rewrite to the driver. Scalar layout is enabled
    * on the device by the layer and is required by our packed BVH nodes. */
   spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_2);
   std::string validation_diag;
   validator.SetMessageConsumer([&](spv_message_level_t, const char *,
                                    const spv_position_t &, const char *message) {
      if (!validation_diag.empty()) validation_diag += " | ";
      validation_diag += message ? message : "(no diagnostic)";
   });
   spvtools::ValidatorOptions validation_options;
   validation_options.SetScalarBlockLayout(true);
   if (!validator.Validate(out.data(), out.size(), validation_options))
      return fail(("rewritten SPIR-V validation failed: " + validation_diag).c_str());

   size_t n = out.size();
   uint32_t *buf = (uint32_t *)std::malloc(n * 4);
   if (!buf)
      return fail("out of memory emitting rewritten SPIR-V");
   std::memcpy(buf, out.data(), n * 4);
   *out_words = buf;
   *out_word_count = n;
   return 0;
}

void
vkrt_spirv_lower_free(uint32_t *words)
{
   std::free(words);
}
