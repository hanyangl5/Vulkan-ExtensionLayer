/* SPDX-License-Identifier: MIT
 *
 * vkrt_spirv_support.h - C interface to the C++ SPIR-V inspection helpers.
 *
 * The layer owns no Ray Query lowering path: modules that reach the public
 * shader hooks are either forwarded unchanged (after classification) or
 * rejected. What remains here is the native-entry selection used when a
 * transported binary is turned back into an ordinary module, and the source
 * validators used by the shader-object binary paths.
 */

#ifndef VKRT_SPIRV_SUPPORT_H
#define VKRT_SPIRV_SUPPORT_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Release a buffer returned by vkrt_spirv_select_native_entry(). */
void vkrt_spirv_free(uint32_t *words);

/* Select an ordinary entry from a mixed RT/native source before classifying or
 * forwarding it. Keep its interface, execution modes and specialization
 * constants. On success return a malloc buffer released with
 * vkrt_spirv_free(). Failure clears both outputs; allocation failures return
 * OUT_OF_HOST_MEMORY, and unsupported or invalid source returns
 * FEATURE_NOT_PRESENT. */
VkResult vkrt_spirv_select_native_entry(const uint32_t *words, size_t word_count, const char *name, VkShaderStageFlagBits stage,
                                        uint32_t **out_words, size_t *out_word_count);

/* Validate a transported source module before turning opaque binary input
 * into native SPIR-V. Layout checks remain native feature-dependent. */
int vkrt_spirv_validate_source(const uint32_t *words, size_t word_count);

/* Validate the module after applying specialization and freezing constants.
 * The input module and specialization bytes remain unchanged. Unsupported or
 * invalid transported input returns INCOMPATIBLE_SHADER_BINARY_EXT; allocation
 * failures return OUT_OF_HOST_MEMORY. Uses the same layout feature union as
 * vkrt_spirv_validate_source(), not the destination logical device profile.
 * Undefined arithmetic left unresolved by the SDK is preserved: success does
 * not certify every specialized value or device-dependent shader limit. */
VkResult vkrt_spirv_validate_specialized_source(const uint32_t *words, size_t word_count,
                                                const VkSpecializationInfo *specialization);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VKRT_SPIRV_SUPPORT_H */
