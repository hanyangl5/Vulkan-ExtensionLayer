/* SPDX-License-Identifier: MIT */
#include "vkrt_spirv_ids.h"
#include <spirv-tools/libspirv.h>
#include <spirv/unified1/spirv.hpp>
#include <unordered_set>
#include <utility>

namespace vkrt_spv {
bool remap_id_uses(std::vector<uint32_t> *words,
   const std::unordered_map<uint32_t, uint32_t> &substitutions, std::string *error)
{
   struct Parse {
      const std::unordered_map<uint32_t, uint32_t> &sub;
      size_t offset = 5;
      std::vector<std::pair<size_t, uint32_t>> edits;
   } state{substitutions, 5, {}};
   auto instruction = [](void *user, const spv_parsed_instruction_t *inst) -> spv_result_t {
      Parse &p = *static_cast<Parse *>(user);
      for (uint16_t i = 0; i < inst->num_operands; ++i) {
         const spv_parsed_operand_t &operand = inst->operands[i];
         /* The parser expands optional, variadic and paired operands into
          * concrete types. Result IDs are definitions, not uses. In particular,
          * OpSwitch case literals, OpConstant bits, decoration values, strings,
          * storage classes and execution-mode literals must stay untouched. */
         if (operand.type != SPV_OPERAND_TYPE_ID && operand.type != SPV_OPERAND_TYPE_TYPE_ID &&
             operand.type != SPV_OPERAND_TYPE_MEMORY_SEMANTICS_ID && operand.type != SPV_OPERAND_TYPE_SCOPE_ID)
            continue;
         auto replacement = p.sub.find(inst->words[operand.offset]);
         if (replacement != p.sub.end())
            p.edits.emplace_back(p.offset + operand.offset, replacement->second);
      }
      p.offset += inst->num_words;
      return SPV_SUCCESS;
   };
   spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_2);
   if (!context) { if (error) *error = "could not create SPIR-V parser"; return false; }
   spv_diagnostic diagnostic = nullptr;
   spv_result_t result = spvBinaryParse(context, &state, words->data(), words->size(), nullptr, instruction, &diagnostic);
   if (result != SPV_SUCCESS && error)
      *error = diagnostic && diagnostic->error ? diagnostic->error : "SPIR-V operand parsing failed";
   spvDiagnosticDestroy(diagnostic);
   spvContextDestroy(context);
   if (result != SPV_SUCCESS) return false;
   /* Parse the original module in full first; changing type IDs while parsing
    * could change how following typed literals are decoded. Failure is atomic. */
   for (const auto &edit : state.edits) (*words)[edit.first] = edit.second;
   return true;
}

static bool dedupe_types_once(std::vector<uint32_t> *words, std::string *error) {
    if (words->size() < 5) {
        if (error) *error = "truncated SPIR-V header during type deduplication";
        return false;
    }
    std::unordered_map<std::string, uint32_t> seen;
    std::unordered_map<uint32_t, uint32_t> substitutions;
    std::unordered_set<size_t> dropped;
    std::unordered_set<uint32_t> decorated;
    for (size_t i = 5; i < words->size();) {
        const uint32_t length = (*words)[i] >> 16;
        if (!length || length > words->size() - i) break;
        const auto op = static_cast<spv::Op>((*words)[i] & 0xffff);
        if ((op == spv::OpDecorate || op == spv::OpDecorateId || op == spv::OpDecorateString) && length >= 3)
            decorated.insert((*words)[i + 1]);
        if (op == spv::OpGroupDecorate)
            for (uint32_t k = 2; k < length; ++k) decorated.insert((*words)[i + k]);
        i += length;
    }
    for (size_t i = 5; i < words->size();) {
        uint32_t length = (*words)[i] >> 16;
        auto op = static_cast<spv::Op>((*words)[i] & 0xffff);
        if (!length || length > words->size() - i) {
            if (error) *error = "malformed SPIR-V instruction during type deduplication";
            return false;
        }
        /* These types have their result ID at word 1. Forward-pointer
         * declarations have no result ID and must not be deduplicated this way. */
        bool type = op == spv::OpTypeVoid || op == spv::OpTypeBool || op == spv::OpTypeInt || op == spv::OpTypeFloat ||
                    op == spv::OpTypeVector || op == spv::OpTypeMatrix || op == spv::OpTypePointer || op == spv::OpTypeFunction;
        // Arrays are aggregate types: equal element/length operands do not imply
        // equal layout (ArrayStride can differ). Likewise, do not merge decorated
        // pointer/scalar types by instruction bytes alone.
        if (type && length >= 2 && !decorated.count((*words)[i + 1])) {
            std::string signature = std::to_string((*words)[i]) + ":";
            for (uint32_t k = 2; k < length; ++k) signature += std::to_string((*words)[i + k]) + ",";
            uint32_t id = (*words)[i + 1];
            auto found = seen.emplace(signature, id);
            if (!found.second && found.first->second != id) {
                substitutions[id] = found.first->second;
                dropped.insert(i);
            }
        }
        i += length;
    }
    if (dropped.empty()) return true;
    if (!remap_id_uses(words, substitutions, error)) return false;
    std::vector<uint32_t> out(words->begin(), words->begin() + 5);
    for (size_t i = 5; i < words->size();) {
        uint32_t length = (*words)[i] >> 16;
        if (!dropped.count(i)) out.insert(out.end(), words->begin() + i, words->begin() + i + length);
        i += length;
    }
    words->swap(out);
    return true;
}

bool dedupe_types(std::vector<uint32_t> *words, std::string *error) {
    // A pointer merge can make function signatures identical only after its
    // ID uses have been remapped. Iterate to a fixed point, always deleting at
    // least one declaration per changing iteration. Keep failure atomic.
    auto candidate = *words;
    size_t previous;
    do {
        previous = candidate.size();
        if (!dedupe_types_once(&candidate, error)) return false;
    } while (candidate.size() < previous);
    words->swap(candidate);
    return true;
}
}
