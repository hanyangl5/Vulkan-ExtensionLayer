/* SPDX-License-Identifier: MIT */
#ifndef VKRT_BUILD_DISPATCH_H
#define VKRT_BUILD_DISPATCH_H
#include <stdbool.h>
#include <stdint.h>

struct vkrt_build_dispatch_grid {
    uint32_t x, y, z;
};

/* The builder's logical workgroup IDs are flattened independently of native
 * dimensions. Extra groups in the last plane are harmless: each leaf/tree
 * invocation checks its logical bound; each sort pass guards its entire group.
 * Leave the output untouched if the reported device dimensions cannot cover it. */
static inline bool vkrt_build_dispatch_grid(uint32_t groups, uint32_t max_x, uint32_t max_y, uint32_t max_z,
                                            struct vkrt_build_dispatch_grid *out) {
    if (!groups || !max_x || !max_y || !max_z || !out) return false;
    struct vkrt_build_dispatch_grid grid;
    grid.x = groups < max_x ? groups : max_x;
    uint64_t rows = ((uint64_t)groups + grid.x - 1) / grid.x;
    grid.y = rows < max_y ? (uint32_t)rows : max_y;
    uint64_t planes = (rows + grid.y - 1) / grid.y;
    if (planes > max_z) return false;
    grid.z = (uint32_t)planes;
    *out = grid;
    return true;
}
#endif
