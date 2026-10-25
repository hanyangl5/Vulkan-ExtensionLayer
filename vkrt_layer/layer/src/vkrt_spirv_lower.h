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

void
vkrt_spirv_lower_free(uint32_t *words);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VKRT_SPIRV_LOWER_H */
