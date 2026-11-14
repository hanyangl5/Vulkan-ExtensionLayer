/* SPDX-License-Identifier: MIT
 *
 * vkrt_spirv_support.cpp - source validators and buffer ownership for the
 * C shader-object paths. C++ is used only because SPIRV-Tools exposes these
 * helpers through its C++ API.
 */

#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vkrt_spirv_support.h"

namespace {

#define SPV_OP(w) ((uint16_t)((w) & 0xFFFFu))
#define SPV_LEN(w) ((uint16_t)((w) >> 16))

/* Iterate every instruction, invoking fn(offset, opcode, length). */
template <typename F>
void for_each(const std::vector<uint32_t> &w, F &&fn) {
    for (size_t i = 5; i < w.size();) {
        uint32_t op = SPV_OP(w[i]);
        uint32_t len = SPV_LEN(w[i]);
        if (len == 0 || i + len > w.size()) break;
        fn(i, op, len);
        i += len;
    }
}

}  // namespace

void vkrt_spirv_free(uint32_t *words) { std::free(words); }

int vkrt_spirv_validate_source(const uint32_t *words, size_t word_count) {
    /* A binary can move across logical profiles and instance API versions.
     * Validate structure and module semantics with the newest supported
     * grammar, without applying the destination's optional block layout
     * feature rules to an otherwise compatible binary. */
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_4);
    spvtools::ValidatorOptions options;
    options.SetUniformBufferStandardLayout(true);
    options.SetScalarBlockLayout(true);
    options.SetWorkgroupScalarBlockLayout(true);
    options.SetAllowLocalSizeId(true);
    return validator.Validate(words, word_count, options) ? 1 : 0;
}

VkResult vkrt_spirv_validate_specialized_source(const uint32_t *words, size_t word_count,
                                                const VkSpecializationInfo *specialization) {
    constexpr VkResult incompatible = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    if (!words || word_count < 5 || word_count > SIZE_MAX / sizeof(uint32_t)) return incompatible;
    if (specialization &&
        ((specialization->mapEntryCount && !specialization->pMapEntries) || (specialization->dataSize && !specialization->pData)))
        return incompatible;
    try {
        std::unique_ptr<spv_context_t, decltype(&spvContextDestroy)> context(spvContextCreate(SPV_ENV_VULKAN_1_4),
                                                                             spvContextDestroy);
        spvtools::ValidatorOptions validation;
        if (!context || !static_cast<spv_validator_options>(validation)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        validation.SetUniformBufferStandardLayout(true);
        validation.SetScalarBlockLayout(true);
        validation.SetWorkgroupScalarBlockLayout(true);
        validation.SetAllowLocalSizeId(true);
        auto validate = [&](const uint32_t *data, size_t count) {
            spv_const_binary_t binary{data, count};
            const spv_result_t result = spvValidateWithOptions(context.get(), validation, &binary, nullptr);
            return result == SPV_SUCCESS               ? VK_SUCCESS
                   : result == SPV_ERROR_OUT_OF_MEMORY ? VK_ERROR_OUT_OF_HOST_MEMORY
                                                       : incompatible;
        };
        VkResult result = validate(words, word_count);
        if (result != VK_SUCCESS) return result;

        spvtools::OptimizerOptions options;
        if (!static_cast<spv_optimizer_options>(options)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        options.set_validator_options(validation);
        // Validate explicitly before and after the complete specializing transform.
        options.set_run_validator(false);
        spvtools::Optimizer flatten(SPV_ENV_VULKAN_1_4);
        flatten.RegisterPass(spvtools::CreateFlattenDecorationPass());
        std::vector<uint32_t> normalized;
        if (!flatten.Run(words, word_count, &normalized, options)) return incompatible;

        std::unordered_map<uint32_t, uint32_t> types;
        std::unordered_map<uint32_t, uint32_t> constants;
        std::unordered_map<uint32_t, std::vector<uint32_t>> ids;
        for_each(normalized, [&](size_t i, uint32_t op, uint32_t) {
            if (op == spv::OpTypeBool)
                types.emplace(normalized[i + 1], sizeof(VkBool32));
            else if (op == spv::OpTypeInt || op == spv::OpTypeFloat)
                types.emplace(normalized[i + 1], normalized[i + 2] / 8);
            else if (op == spv::OpSpecConstant || op == spv::OpSpecConstantTrue || op == spv::OpSpecConstantFalse)
                constants.emplace(normalized[i + 2], normalized[i + 1]);
            else if (op == spv::OpDecorate && normalized[i + 2] == spv::DecorationSpecId)
                ids[normalized[i + 3]].push_back(normalized[i + 1]);
        });

        std::unordered_set<uint32_t> supplied;
        std::unordered_map<uint32_t, std::vector<uint32_t>> values;
        if (specialization) {
            for (uint32_t i = 0; i < specialization->mapEntryCount; ++i) {
                const VkSpecializationMapEntry &entry = specialization->pMapEntries[i];
                // 00773 applies even to unused IDs and zero-byte map entries.
                if (entry.offset >= specialization->dataSize || entry.size > specialization->dataSize - entry.offset ||
                    !supplied.insert(entry.constantID).second)
                    return incompatible;
                const auto targets = ids.find(entry.constantID);
                if (targets == ids.end()) continue;  // 00776 only constrains declared IDs.
                for (uint32_t target : targets->second) {
                    const auto constant = constants.find(target);
                    if (constant == constants.end()) return incompatible;
                    const auto type = types.find(constant->second);
                    if (type == types.end() || !type->second || type->second > 8 || entry.size != type->second) return incompatible;
                }
                std::vector<uint32_t> value((entry.size + 3) / 4, 0);
                std::memcpy(value.data(), static_cast<const uint8_t *>(specialization->pData) + entry.offset, entry.size);
                // The bit-pattern pass normalizes booleans and sign/zero
                // extends sub-word literals according to each target's type.
                values.emplace(entry.constantID, std::move(value));
            }
        }

        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_4);
        optimizer.RegisterPass(spvtools::CreateSetSpecConstantDefaultValuePass(values));
        optimizer.RegisterPass(spvtools::CreateFreezeSpecConstantValuePass());
        optimizer.RegisterPass(spvtools::CreateFoldSpecConstantOpAndCompositePass());
        std::vector<uint32_t> specialized;
        if (!optimizer.Run(normalized.data(), normalized.size(), &specialized, options)) return incompatible;

        // SPIR-V 2.12 / OpSpecConstantOp: in the Shader environment the only
        // numeric floating specialization operations are FConvert and
        // QuantizeToF16. Float-to-integer conversions, float comparisons and
        // bitcasts require Kernel, forbidden by the Vulkan source validator.
        // Consequently floating values cannot affect integer/Boolean values,
        // including individual integer components of mixed composites. All
        // value-dependent Vulkan module-validity constraints (array sizes,
        // execution modes, scopes, memory semantics, indices and layouts) use
        // those integer/Boolean values; floating bit patterns impose none.
        //
        // The installed folder leaves FConvert/QuantizeToF16 unresolved. Use
        // typed zero representatives ONLY in this validation copy, then fold
        // composites/extracts again. This proves the exact constrained values
        // without approximating FP rounding in any native shader or packet.
        std::unordered_set<uint32_t> float_types, abstracted;
        for_each(specialized, [&](size_t i, uint32_t op, uint32_t len) {
            if (op == spv::OpTypeFloat || (op == spv::OpTypeVector && float_types.count(specialized[i + 2])))
                float_types.insert(specialized[i + 1]);
            else if (op == spv::OpSpecConstantOp && len >= 5 && float_types.count(specialized[i + 1]) &&
                     (specialized[i + 3] == spv::OpFConvert || specialized[i + 3] == spv::OpQuantizeToF16))
                abstracted.insert(specialized[i + 2]);
        });
        if (!abstracted.empty()) {
            std::vector<uint32_t> representative(specialized.begin(), specialized.begin() + 5);
            for_each(specialized, [&](size_t i, uint32_t op, uint32_t len) {
                if (op == spv::OpSpecConstantOp && abstracted.count(specialized[i + 2])) {
                    representative.insert(representative.end(), {(3u << 16) | static_cast<uint32_t>(spv::OpConstantNull),
                                                                 specialized[i + 1], specialized[i + 2]});
                } else if (!(op == spv::OpDecorate && abstracted.count(specialized[i + 1]) &&
                             (specialized[i + 2] == spv::DecorationFPRoundingMode ||
                              specialized[i + 2] == spv::DecorationFPFastMathMode))) {
                    representative.insert(representative.end(), specialized.begin() + i, specialized.begin() + i + len);
                }
            });
            spvtools::Optimizer fold(SPV_ENV_VULKAN_1_4);
            fold.RegisterPass(spvtools::CreateFoldSpecConstantOpAndCompositePass());
            if (!fold.Run(representative.data(), representative.size(), &specialized, options)) return incompatible;
        }
        // Preserve grammar-valid undefined arithmetic if the SDK leaves it
        // unresolved (e.g. an unused specialization divide by zero). Rejecting
        // every residual expression would shrink the legal source language.
        // Known values and FP-independent constraint values are checked here;
        // unresolved/undefined expressions retain the SDK validator's limits.
        return validate(specialized.data(), specialized.size());
    } catch (const std::bad_alloc &) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (...) {
        return incompatible;
    }
}
