/* SPDX-License-Identifier: MIT */
#ifndef VKRT_BUILD_LAYOUT_H
#define VKRT_BUILD_LAYOUT_H
#include "bvh_ir_build_layout.h"
#define VKRT_HEADER_SIZE BVH_IR_NODE_OFFSET
#define VKRT_BOUNDS_OFFSET 0u
#define vkrt_align_size bvh_ir_align_size
#define vkrt_leaf_stride bvh_ir_leaf_stride
#define vkrt_build_layout bvh_ir_build_layout
#endif
