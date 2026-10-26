/* SPDX-License-Identifier: MIT
 *
 * vkrt_spirv_lower.h - C interface to the C++ SPIR-V ray-query lowerer.
 *
 * The implementation lives in vkrt_spirv_lower.cpp because it uses
 * spvtools::Link (C++ API). Everything else in the layer is C.
 */

#ifndef VKRT_SPIRV_LOWER_H
#define VKRT_SPIRV_LOWER_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rewrite OpRayQuery*KHR in `user_words` into OpFunctionCall into the
 * software traversal supplied by `lib_words` (traversal/sw_ray_query.spv).
 *
 * On success returns 0 and hands back a malloc'd buffer the caller must
 * release with vkrt_spirv_lower_free(). On failure returns non-zero and
 * writes a message into errbuf (when provided).
 */
int
vkrt_spirv_lower(const uint32_t *user_words, size_t user_word_count,
                 const uint32_t *lib_words, size_t lib_word_count,
                 uint32_t **out_words, size_t *out_word_count,
                 char *errbuf, size_t errbuf_len);

/* Device-owned immutable AS resolver context is embedded as an ordinary
 * constant, consuming no application descriptor or push-constant slot.
 * The context-free entry above is for compile tests (unresolved AS -> null). */
int vkrt_spirv_lower_with_context(const uint32_t *user_words, size_t user_word_count, const uint32_t *lib_words,
                                  size_t lib_word_count, uint64_t as_context, uint32_t **out_words, size_t *out_word_count,
                                  char *errbuf, size_t errbuf_len);

void
vkrt_spirv_lower_free(uint32_t *words);

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

#endif /* VKRT_SPIRV_LOWER_H */
