// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "bvh4_abi.h"
#include <stddef.h>
_Static_assert(sizeof(VelRtBvh4Header) == 128, "header wire size");
_Static_assert(sizeof(VelRtBvh4Node) == 128, "node wire size");
_Static_assert(offsetof(VelRtBvh4Node, child_id) == 96, "child wire offset");
_Static_assert(sizeof(VelRtBvh4Triangle) == 48, "triangle wire size");
_Static_assert(sizeof(VelRtBvh4Aabb) == 48, "AABB wire size");
_Static_assert(sizeof(VelRtBvh4Instance) == 128, "instance wire size");
