/* SPDX-License-Identifier: MIT */
#ifndef VKRT_QUERIES_H
#define VKRT_QUERIES_H
#include <vulkan/vulkan.h>

/* GPU-only property computation. The validity word is not a host publication
 * primitive: native query/fence synchronization must precede host reads. */
struct vkrt_as_query_args {
    uint64_t source, source_capacity, output;
    uint32_t query_type, reserved;
};
struct vkrt_as_query_value {
    uint64_t value;
    uint32_t valid, reserved;
};

/* Format GPU-owned slots only after their producer dependency. WAIT does not
 * poll or wait inside the shader: future API integration must supply its
 * query execution dependency, and separately synchronize host publication.
 * The formatter's compute writes must target private scratch; public
 * CmdCopyQueryPoolResults output requires a final native transfer copy. */
struct vkrt_query_format_args {
    uint64_t input, input_capacity, output, output_capacity, stride;
    uint32_t first_query, count, flags, reserved;
};

#endif
